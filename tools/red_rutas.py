"""The agents' shared map and route net, worked out away from the game (no big search live: 2026-11-11, eight copies
at 100 % CPU).

  comun  SacredA1..A8's SacredBild-map2.txt -> SacredBild-mapa-comun.bin: per tile what routes need (walkable, wall,
         shut gate, shut by the tile test alone), the union of what every copy saw. The game reads it beside its own map
         (MapMemory, read only); each copy keeps writing its own map2.txt, so none overwrites another's.
  red    the shared map + towns (Pueblos.tsv), services (Servicios.tsv), the copies' landmarks (entrances, portals,
         merchants, storage) and way points -> SacredBild-red.txt: nodes and the ways between them, each with its
         points every few tiles, walked over known walkable tiles only.
  consulta  a trip as the agent makes it live (nearest nodes, the net between them), to check a net before installing.

Writes where --salida says; putting the files beside the copies is installing (the coordinator's).
"""

import argparse
import heapq
import os
import struct
import sys

import numpy as np

# The step classes, as MapMemory::routeWithin costs them (src/game/map_memory.cpp).
UNKNOWN, WALK, WALL, GATE, SCAN_SHUT = 0, 1, 2, 3, 4
CHUNK = 32
MAGIC = b"SBMC"
VERSION = 1

# Landmark kinds (MapMemory::Mark).
MARK_NAMES = {0: "mercader", 1: "entrada", 2: "portal", 3: "almacen"}


def kind_walkable(kind):
    k = kind & 0xF
    return (k == 1) | (k == 6) | (k == 8)


def read_map(path):
    """A copy's map2.txt: (squares as an int32 array [layer, tx, ty, visits, blocked, scan, kind], landmarks)."""
    with open(path, "rb") as f:
        data = f.read()
    lines = data.split(b"\n")
    squares, landmarks = [], []
    for line in lines:
        if not line:
            continue
        c = line[0]
        if c == 76:  # 'L'
            p = line.split()
            if len(p) == 5:
                landmarks.append((int(p[1]), int(p[2]), int(p[3]), int(p[4])))
        elif 48 <= c <= 57 or c == 45:
            squares.append(line)
    del lines, data
    tokens = np.array(b" ".join(squares).split(), dtype="S8")
    del squares
    n = len(tokens) // 10
    t = tokens[: n * 10].reshape(n, 10)
    cols = [0, 1, 2, 3, 4, 7, 8]
    out = np.empty((n, len(cols)), dtype=np.int32)
    for i, c in enumerate(cols):
        out[:, i] = t[:, c].astype(np.int32)
    return out, landmarks


def merge(paths):
    """The copies' squares joined: per (layer, tx, ty) the step class. Returns (keys int64 sorted, classes uint8),
    the landmarks of all, and per copy its count."""
    keys_all, cls_all, landmarks, counts = [], [], set(), []
    for p in paths:
        sq, lm = read_map(p)
        counts.append((os.path.basename(p), len(sq)))
        landmarks.update(lm)
        layer, tx, ty, visits, blocked, scan, kind = (sq[:, i] for i in range(7))
        cls = np.zeros(len(sq), dtype=np.uint8)
        # Same order as routeWithin: walked or walkable by the tile test first, then blocked again and again, a shut
        # gate, the game's tile kind, the tile test alone.
        walk = (visits > 0) | (scan == 1)
        wall = ~walk & (blocked >= 2)
        rest = ~walk & ~wall
        gate = rest & ((kind & 0xF) == 7)
        bad = rest & ~gate & (kind != 0) & ~kind_walkable(kind)
        scan_shut = rest & ~gate & ~bad & (scan == 2)
        cls[walk] = WALK
        cls[wall | bad] = WALL
        cls[gate] = GATE
        cls[scan_shut] = SCAN_SHUT
        keep = cls != UNKNOWN
        k = (layer[keep].astype(np.int64) << 32) | ((tx[keep].astype(np.int64) & 0xFFFF) << 16) | (ty[keep].astype(np.int64) & 0xFFFF)
        keys_all.append(k)
        cls_all.append(cls[keep])
        print(f"  {os.path.basename(p)}: {len(sq)} squares, {keep.sum()} with a class", file=sys.stderr)
    keys = np.concatenate(keys_all)
    cls = np.concatenate(cls_all)
    # Priority across copies: walked by one beats what another thought (as within one copy: visits first); then a
    # wall over a gate over the tile test alone.
    rank = np.array([0, 4, 3, 2, 1], dtype=np.uint8)[cls]
    order = np.lexsort((-rank.astype(np.int16), keys))
    keys, cls = keys[order], cls[order]
    first = np.ones(len(keys), dtype=bool)
    first[1:] = keys[1:] != keys[:-1]
    return keys[first], cls[first], sorted(landmarks), counts


def unpack(keys):
    layer = (keys >> 32).astype(np.int32)
    tx = ((keys >> 16) & 0xFFFF).astype(np.int16).astype(np.int32)
    ty = (keys & 0xFFFF).astype(np.int16).astype(np.int32)
    return layer, tx, ty


def write_common(path, keys, cls, copies):
    layer, tx, ty = unpack(keys)
    cx, cy = tx >> 5, ty >> 5
    ck = (layer.astype(np.int64) << 32) | ((cx.astype(np.int64) & 0xFFFF) << 16) | (cy.astype(np.int64) & 0xFFFF)
    uniq, inv = np.unique(ck, return_inverse=True)
    blocks = np.zeros((len(uniq), CHUNK * CHUNK), dtype=np.uint8)
    blocks[inv, ((ty & 31) << 5) | (tx & 31)] = cls
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(MAGIC + struct.pack("<III", VERSION, len(uniq), copies))
        for i, k in enumerate(uniq.tolist()):
            f.write(struct.pack("<BBhhH", (k >> 32) & 0xFF, 0, ((k >> 16) & 0xFFFF) - (0x10000 if (k >> 16) & 0x8000 else 0),
                                (k & 0xFFFF) - (0x10000 if k & 0x8000 else 0), 0))
            f.write(blocks[i].tobytes())
    os.replace(tmp, path)
    return len(uniq)


def read_common(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != MAGIC:
        raise SystemExit(f"{path}: not a shared map")
    version, count, copies = struct.unpack_from("<III", data, 4)
    pos = 16
    keys, cls = [], []
    sub = np.arange(CHUNK * CHUNK)
    for _ in range(count):
        layer, _, cx, cy, _ = struct.unpack_from("<BBhhH", data, pos)
        pos += 8
        b = np.frombuffer(data, dtype=np.uint8, count=CHUNK * CHUNK, offset=pos)
        pos += CHUNK * CHUNK
        nz = np.nonzero(b)[0]
        tx = cx * CHUNK + (sub[nz] & 31)
        ty = cy * CHUNK + (sub[nz] >> 5)
        keys.append((np.int64(layer) << 32) | ((tx.astype(np.int64) & 0xFFFF) << 16) | (ty.astype(np.int64) & 0xFFFF))
        cls.append(b[nz])
    keys = np.concatenate(keys)
    cls = np.concatenate(cls)
    order = np.argsort(keys)
    return keys[order], cls[order]


def cmd_comun(a):
    paths = [os.path.join(a.copias, f"SacredA{i}", "SacredBild-map2.txt") for i in range(1, 9)] if a.copias else a.mapas
    paths = [p for p in paths if os.path.exists(p)]
    keys, cls, landmarks, counts = merge(paths)
    layer, _, _ = unpack(keys)
    out = os.path.join(a.salida, "SacredBild-mapa-comun.bin")
    chunks = write_common(out, keys, cls, len(paths))
    print(f"shared map: {len(keys)} squares from {len(paths)} copies, {chunks} chunks of 1 KB ({os.path.getsize(out) // 1024} KB) -> {out}")
    for l in np.unique(layer)[:12]:
        sel = layer == l
        print(f"  layer {l}: {sel.sum()} squares, walk {np.sum(cls[sel] == WALK)}, wall {np.sum(cls[sel] == WALL)}")
    lm = os.path.join(a.salida, "SacredBild-hitos.txt")
    with open(lm, "w") as f:
        for kind, layer_, x, y in landmarks:
            f.write(f"L {kind} {layer_} {x} {y}\n")
    print(f"landmarks: {len(landmarks)} -> {lm}")


# ---------------------------------------------------------------------------------------------------------------
# The net.

STEP = {WALK: 10, GATE: 40}
DIAG = 14


class Grid:
    """One layer of the shared map as a dense array (only known walkable tiles walk)."""

    def __init__(self, keys, cls, layer):
        lay, tx, ty = unpack(keys)
        sel = lay == layer
        tx, ty, c = tx[sel], ty[sel], cls[sel]
        self.x0, self.y0 = int(tx.min()), int(ty.min())
        self.w, self.h = int(tx.max()) - self.x0 + 1, int(ty.max()) - self.y0 + 1
        self.cls = np.zeros((self.h, self.w), dtype=np.uint8)
        self.cls[ty - self.y0, tx - self.x0] = c
        self.step = np.zeros((self.h, self.w), dtype=np.int32)
        self.step[self.cls == WALK] = STEP[WALK]
        self.step[self.cls == GATE] = STEP[GATE]

    def inside(self, x, y):
        return self.x0 <= x < self.x0 + self.w and self.y0 <= y < self.y0 + self.h

    def walk(self, x, y):
        return self.inside(x, y) and self.step[y - self.y0, x - self.x0] > 0

    def snap(self, x, y, radius=12):
        """The nearest known walkable tile to (x, y) within `radius`, or None."""
        if self.walk(x, y):
            return x, y
        best = None
        for r in range(1, radius + 1):
            for dy in range(-r, r + 1):
                for dx in range(-r, r + 1):
                    if max(abs(dx), abs(dy)) == r and self.walk(x + dx, y + dy):
                        d = dx * dx + dy * dy
                        if best is None or d < best[0]:
                            best = (d, x + dx, y + dy)
            if best:
                return best[1], best[2]
        return None


def graph(grid):
    """The layer's known walkable tiles as a graph (8 neighbours, no corner cut; a step costs the tile entered), made
    once: scipy's Dijkstra over it, the grid too large for one in Python."""
    if getattr(grid, "csr", None) is not None:
        return grid.csr
    from scipy.sparse import csr_matrix
    step = grid.step
    ok = step > 0
    idx = np.full(step.shape, -1, dtype=np.int64)
    idx[ok] = np.arange(int(ok.sum()))
    grid.idx = idx
    ys, xs = np.nonzero(ok)
    grid.cell_x, grid.cell_y = xs + grid.x0, ys + grid.y0
    rows, cols, vals = [], [], []
    h, w = step.shape
    for dx, dy, m in [(1, 0, 10), (-1, 0, 10), (0, 1, 10), (0, -1, 10), (1, 1, DIAG), (1, -1, DIAG), (-1, 1, DIAG), (-1, -1, DIAG)]:
        nx, ny = xs + dx, ys + dy
        good = (nx >= 0) & (ny >= 0) & (nx < w) & (ny < h)
        a, b, c, d = xs[good], ys[good], nx[good], ny[good]
        good = ok[d, c]
        if dx and dy:
            good &= ok[b, c] & ok[d, a]
        a, b, c, d = a[good], b[good], c[good], d[good]
        rows.append(idx[b, a])
        cols.append(idx[d, c])
        vals.append(step[d, c].astype(np.float64) * m / 10)
    n = int(ok.sum())
    grid.csr = csr_matrix((np.concatenate(vals), (np.concatenate(rows), np.concatenate(cols))), shape=(n, n))
    return grid.csr


def dijkstra(grid, sx, sy, limit, targets):
    """From (sx, sy) over the grid, up to cost `limit`: for each target tile reached, (cost, path tiles)."""
    from scipy.sparse.csgraph import dijkstra as sp_dijkstra
    g = graph(grid)
    src = int(grid.idx[sy - grid.y0, sx - grid.x0])
    if src < 0:
        return {}
    dist, pred = sp_dijkstra(g, directed=True, indices=src, limit=limit, return_predecessors=True)
    out = {}
    for t, (tx, ty) in targets.items():
        if not grid.inside(tx, ty):
            continue
        v = int(grid.idx[ty - grid.y0, tx - grid.x0])
        if v < 0 or not np.isfinite(dist[v]):
            continue
        path = [v]
        while path[-1] != src:
            path.append(int(pred[path[-1]]))
        path.reverse()
        out[t] = (int(round(dist[v])), [(int(grid.cell_x[p]), int(grid.cell_y[p])) for p in path])
    return out


def thin(path, every):
    """Every `every`-th tile of a path, its ends kept."""
    pts = path[::every]
    if pts[-1] != path[-1]:
        pts.append(path[-1])
    return pts


def read_tsv(path):
    with open(path, encoding="utf-8") as f:
        head = f.readline().rstrip("\n").split("\t")
        return [dict(zip(head, line.rstrip("\n").split("\t"))) for line in f if line.strip()]


def cmd_red(a):
    keys, cls = read_common(a.comun)
    grid = Grid(keys, cls, 0)
    print(f"layer 0: {grid.w}x{grid.h} tiles from {grid.x0},{grid.y0}, {int((grid.step > 0).sum())} known walkable")
    raw = []  # (name, kind, x, y)
    if a.pueblos:
        for r in read_tsv(a.pueblos):
            raw.append((r["pueblo"], "pueblo", int(r["x_almacen"]), int(r["y_almacen"])))
    if a.servicios:
        for r in read_tsv(a.servicios):
            raw.append((r["servicio"] + ":" + r["nombre_o_posicion"], "servicio", int(r["x"]), int(r["y"])))
    if a.hitos:
        with open(a.hitos) as f:
            for line in f:
                p = line.split()
                if len(p) == 5 and p[0] == "L" and int(p[2]) == 0:
                    raw.append((MARK_NAMES.get(int(p[1]), "hito"), MARK_NAMES.get(int(p[1]), "hito"), int(p[3]), int(p[4])))
    # Way points: a known walkable tile every `paso` tiles, so any walked place has a node near.
    walk = grid.step > 0
    paso = a.paso
    for gy in range(grid.y0 + paso // 2, grid.y0 + grid.h, paso):
        for gx in range(grid.x0 + paso // 2, grid.x0 + grid.w, paso):
            ys, xs = slice(max(gy - paso // 2, grid.y0) - grid.y0, min(gy + paso // 2, grid.y0 + grid.h) - grid.y0), \
                     slice(max(gx - paso // 2, grid.x0) - grid.x0, min(gx + paso // 2, grid.x0 + grid.w) - grid.x0)
            if walk[ys, xs].sum() >= paso * paso // 8:
                raw.append(("paso", "paso", gx, gy))
    # Snapped to a known walkable tile; nodes within 6 tiles of one already kept are the same node (the first, so
    # towns and services keep their names).
    nodes = []
    unsnapped = []
    taken = {}
    for name, kind, x, y in raw:
        s = grid.snap(x, y, 16 if kind == "paso" else 24)
        if not s:
            unsnapped.append((name, x, y))
            continue
        cell = (s[0] // 6, s[1] // 6)
        if any(taken.get((cell[0] + i, cell[1] + j)) is not None and
               max(abs(nodes[taken[(cell[0] + i, cell[1] + j)]][2] - s[0]), abs(nodes[taken[(cell[0] + i, cell[1] + j)]][3] - s[1])) <= 6
               for i in (-1, 0, 1) for j in (-1, 0, 1)):
            continue
        taken[cell] = len(nodes)
        nodes.append((name, kind, s[0], s[1]))
    print(f"nodes: {len(nodes)} ({sum(1 for n in nodes if n[1] != 'paso')} named), {len(unsnapped)} with no known walkable tile near")
    # Ways: from each node to the others within `radio` tiles, along known walkable tiles; the k cheapest kept per
    # node (both ways count).
    pos = np.array([(n[2], n[3]) for n in nodes])
    edges = {}
    for i, (name, kind, x, y) in enumerate(nodes):
        d = np.max(np.abs(pos - (x, y)), axis=1)
        near = [j for j in np.argsort(d) if j != i and d[j] <= a.radio][: a.vecinos * 2]
        if not near:
            continue
        found = dijkstra(grid, x, y, a.radio * 18, {j: (nodes[j][2], nodes[j][3]) for j in near})
        best = sorted(found.items(), key=lambda kv: kv[1][0])[: a.vecinos]
        for j, (c, path) in best:
            k = (min(i, j), max(i, j))
            if k not in edges or edges[k][0] > c:
                pts = thin(path, a.cada)
                if i > j:
                    pts.reverse()
                edges[k] = (c, pts)
        if i % 200 == 0:
            print(f"  {i}/{len(nodes)} nodes, {len(edges)} ways", file=sys.stderr)
    # Ways that pass by another node's tile are kept: the live search costs them as they are.
    deg = np.zeros(len(nodes), dtype=int)
    for i, j in edges:
        deg[i] += 1
        deg[j] += 1
    out = os.path.join(a.salida, "SacredBild-red.txt")
    tmp = out + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write("# SacredBild route net (tools/red_rutas.py red): N id layer x y kind name; W a b cost x,y x,y ...\n")
        for i, (name, kind, x, y) in enumerate(nodes):
            f.write(f"N {i} 0 {x} {y} {kind} {name.replace(' ', '_')}\n")
        for (i, j), (c, pts) in sorted(edges.items()):
            f.write(f"W {i} {j} {c} " + " ".join(f"{x},{y}" for x, y in pts) + "\n")
    os.replace(tmp, out)
    # Connected parts: a net in pieces leaves trips between them to the old ways.
    parent = list(range(len(nodes)))

    def root(v):
        while parent[v] != v:
            parent[v] = parent[parent[v]]
            v = parent[v]
        return v

    for i, j in edges:
        parent[root(i)] = root(j)
    parts = {}
    for v in range(len(nodes)):
        parts.setdefault(root(v), []).append(v)
    sizes = sorted((len(p) for p in parts.values()), reverse=True)
    print(f"net: {len(nodes)} nodes, {len(edges)} ways, {int((deg == 0).sum())} nodes with none -> {out}")
    print(f"  connected parts: {len(parts)}, the largest {sizes[:6]}")
    named_alone = [nodes[v][0] for v in range(len(nodes)) if nodes[v][1] != "paso" and deg[v] == 0]
    if named_alone:
        print(f"  named nodes with no way: {', '.join(named_alone[:20])}")


# ---------------------------------------------------------------------------------------------------------------
# A trip as the game makes it (ClickWalk + RouteNet, src/game/route_net.cpp).

def read_net(path):
    nodes, adj = [], {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            p = line.split()
            if not p or p[0] == "#":
                continue
            if p[0] == "N":
                nodes.append((int(p[3]), int(p[4]), p[5], p[6] if len(p) > 6 else ""))
            elif p[0] == "W":
                i, j, c = int(p[1]), int(p[2]), int(p[3])
                pts = [tuple(map(int, q.split(","))) for q in p[4:]]
                adj.setdefault(i, []).append((j, c, pts))
                adj.setdefault(j, []).append((i, c, pts[::-1]))
    return nodes, adj


def trip(nodes, adj, grid, sx, sy, gx, gy, reach=120, k=6):
    """The live trip: up to k nodes within `reach` of each end (the nearer ones first) that a route over the shared map
    reaches (the live code tries them with routeWithin), the net between them (Dijkstra), the points to walk."""
    pos = np.array([(n[0], n[1]) for n in nodes])

    def ends(x, y):
        d = np.max(np.abs(pos - (x, y)), axis=1)
        cand = [int(j) for j in np.argsort(d)[: 4 * k] if d[j] <= reach]
        s = grid.snap(x, y, 12)
        if not s:
            return {}
        found = dijkstra(grid, s[0], s[1], reach * 22, {j: (nodes[j][0], nodes[j][1]) for j in cand})
        return {j: c for j, (c, _) in sorted(found.items(), key=lambda kv: kv[1][0])[:k]}

    a, b = ends(sx, sy), ends(gx, gy)
    if not a or not b:
        return None, a, b
    dist = {j: c for j, c in a.items()}
    prev = {}
    heap = [(c, j) for j, c in a.items()]
    heapq.heapify(heap)
    best = None
    while heap:
        c, v = heapq.heappop(heap)
        if c > dist.get(v, 1 << 60):
            continue
        if best and c >= best[0]:
            break
        if v in b and (not best or c + b[v] < best[0]):
            best = (c + b[v], v)
        for u, w, _ in adj.get(v, []):
            if c + w < dist.get(u, 1 << 60):
                dist[u] = c + w
                prev[u] = v
                heapq.heappush(heap, (c + w, u))
    if not best:
        return None, a, b
    chain = [best[1]]
    while chain[-1] in prev:
        chain.append(prev[chain[-1]])
    chain.reverse()
    pts = [(nodes[chain[0]][0], nodes[chain[0]][1])]
    for v, u in zip(chain, chain[1:]):
        for w_, c_, p_ in adj[v]:
            if w_ == u:
                pts.extend(p_[1:])
                break
    pts.append((gx, gy))
    return (best[0], chain, pts), a, b


def cmd_consulta(a):
    keys, cls = read_common(a.comun)
    grid = Grid(keys, cls, 0)
    nodes, adj = read_net(a.red)
    for q in a.viajes:
        s, g = q.split(":")
        sx, sy = map(int, s.split(","))
        gx, gy = map(int, g.split(","))
        r, ea, eb = trip(nodes, adj, grid, sx, sy, gx, gy)
        if not r:
            print(f"{sx},{sy} -> {gx},{gy}: no trip (start nodes {len(ea)}, goal nodes {len(eb)})")
            continue
        c, chain, pts = r
        print(f"{sx},{sy} -> {gx},{gy}: {c / 10:.0f} tiles by the net ({max(abs(gx - sx), abs(gy - sy))} straight), "
              f"{len(chain)} nodes: " + " > ".join(f"{nodes[v][3] or nodes[v][2]}@{nodes[v][0]},{nodes[v][1]}" for v in chain[:8])
              + (" ..." if len(chain) > 8 else ""))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("comun")
    c.add_argument("--copias", help="steamapps\\common: its SacredA1..A8")
    c.add_argument("--mapas", nargs="*", default=[], help="or the map2.txt files themselves")
    c.add_argument("--salida", required=True)
    r = sub.add_parser("red")
    r.add_argument("--comun", required=True)
    r.add_argument("--pueblos")
    r.add_argument("--servicios")
    r.add_argument("--hitos")
    r.add_argument("--paso", type=int, default=48, help="tiles between way points")
    r.add_argument("--radio", type=int, default=110, help="how far a way from a node reaches, tiles")
    r.add_argument("--vecinos", type=int, default=8)
    r.add_argument("--cada", type=int, default=8, help="a way's point every so many tiles")
    r.add_argument("--salida", required=True)
    q = sub.add_parser("consulta")
    q.add_argument("--comun", required=True)
    q.add_argument("--red", required=True)
    q.add_argument("viajes", nargs="+", help="x,y:x,y")
    a = ap.parse_args()
    {"comun": cmd_comun, "red": cmd_red, "consulta": cmd_consulta}[a.cmd](a)


if __name__ == "__main__":
    main()
