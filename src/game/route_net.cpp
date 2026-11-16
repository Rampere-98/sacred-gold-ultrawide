#include "game/route_net.h"
#include "game/map_memory.h"
#include "log.h"

#include <windows.h>
#include <algorithm>
#include <climits>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace
{
    struct Node
    {
        int layer, x, y;
        std::string kind, name;
    };
    struct Way
    {
        int to, cost;               // cost in tenths of a tile, as the script worked it out
        int points;                 // index into g_points
        bool reversed;              // its points walked from the end
    };
    std::vector<Node> g_nodes;
    std::vector<std::vector<Way>> g_ways;
    std::vector<std::vector<std::pair<int, int>>> g_points;
    bool g_loaded = false;          // under g_lock, as all here

    std::mutex g_lock;

    // A trip worked out, kept for its goal while the hero stays on it.
    struct Kept
    {
        int layer = -1, toX = 0, toY = 0, fromX = 0, fromY = 0;
        DWORD at = 0;
        bool ok = false;
        std::vector<std::pair<int, int>> points;  // the hero's start .. the goal
        size_t progress = 0;                      // the point nearest the hero last asked
        int tiles = 0;                            // the walk's length by the net
    };
    Kept g_kept;
    bool g_busy = false;
    // Lengths asked for (walkTiles: the portals' choice of walking or jumping), kept apart from the trip walked.
    std::deque<Kept> g_lengths;

    constexpr int kReach = 120;     // nodes this near an end (tiles, the larger axis) are tried for it
    constexpr int kTry = 8, kKeep = 4;
    constexpr int kStretchMargin = 24;
    constexpr int kOffTrip = 40;    // the hero this far from every point ahead: the trip is worked out again

    std::wstring netPath()
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring p = exe;
        p = p.substr(0, p.find_last_of(L"\\/"));
        return p.substr(0, p.find_last_of(L"\\/") + 1) + L"SacredBild-red.txt";
    }

    int cheb(int ax, int ay, int bx, int by)
    {
        return std::max(std::abs(ax - bx), std::abs(ay - by));
    }

    // Under g_lock.
    void load()
    {
        g_loaded = true;
        const DWORD t0 = GetTickCount();
        FILE* f = _wfopen(netPath().c_str(), L"rb");
        if (!f)
        {
            LOG("RouteNet: no net beside the copies (SacredBild-red.txt): far walks the old ways");
            return;
        }
        std::string line;
        char buf[4096];
        size_t ways = 0;
        while (std::fgets(buf, sizeof(buf), f))
        {
            line = buf;
            // A way's line can be longer than the buffer: read on to its end.
            while (!line.empty() && line.back() != '\n' && std::fgets(buf, sizeof(buf), f))
            {
                line += buf;
            }
            const char* p = line.c_str();
            if (p[0] == 'N')
            {
                int id = 0, layer = 0, x = 0, y = 0, used = 0;
                char kind[32] = {}, name[96] = {};
                if (std::sscanf(p, "N %d %d %d %d %31s %n", &id, &layer, &x, &y, kind, &used) >= 5 &&
                    id == static_cast<int>(g_nodes.size()))
                {
                    std::sscanf(p + used, "%95s", name);
                    g_nodes.push_back({layer, x, y, kind, name});
                }
            }
            else if (p[0] == 'W')
            {
                char* end = nullptr;
                const long a = std::strtol(p + 1, &end, 10);
                const long b = std::strtol(end, &end, 10);
                const long cost = std::strtol(end, &end, 10);
                if (a < 0 || b < 0 || a >= static_cast<long>(g_nodes.size()) || b >= static_cast<long>(g_nodes.size()) || cost <= 0)
                {
                    continue;
                }
                std::vector<std::pair<int, int>> pts;
                for (;;)
                {
                    char* e2 = nullptr;
                    const long x = std::strtol(end, &e2, 10);
                    if (e2 == end || *e2 != ',')
                    {
                        break;
                    }
                    char* e3 = nullptr;
                    const long y = std::strtol(e2 + 1, &e3, 10);
                    if (e3 == e2 + 1)
                    {
                        break;
                    }
                    pts.push_back({static_cast<int>(x), static_cast<int>(y)});
                    end = e3;
                }
                if (pts.size() < 2)
                {
                    continue;
                }
                g_ways.resize(g_nodes.size());
                const int idx = static_cast<int>(g_points.size());
                g_points.push_back(std::move(pts));
                g_ways[a].push_back({static_cast<int>(b), static_cast<int>(cost), idx, false});
                g_ways[b].push_back({static_cast<int>(a), static_cast<int>(cost), idx, true});
                ++ways;
            }
        }
        std::fclose(f);
        g_ways.resize(g_nodes.size());
        LOG("RouteNet: net of {} nodes and {} ways read in {} ms", g_nodes.size(), ways, GetTickCount() - t0);
    }

    // The walk's length in tenths of a tile along route tiles.
    int lengthOf(const std::vector<std::pair<int, int>>& cells)
    {
        int c = 0;
        for (size_t i = 1; i < cells.size(); ++i)
        {
            c += (cells[i].first != cells[i - 1].first && cells[i].second != cells[i - 1].second) ? 14 : 10;
        }
        return c;
    }

    struct End
    {
        int node, cost;
        std::vector<std::pair<int, int>> cells;   // the hero to the node, or the node to the goal
    };

    // The nodes near (x, y) on `layer` a route over the agent's map reaches (towards them, or from them to (x, y)):
    // the nearest first, at most kKeep. Not under g_lock (routeWithin takes the map's).
    std::vector<End> ends(const std::vector<std::pair<int, int>>& around, int layer, int x, int y, bool toNode, int& tried)
    {
        std::vector<End> out;
        tried = 0;
        for (const auto& [node, d] : around)
        {
            if (static_cast<int>(out.size()) >= kKeep || tried >= kTry)
            {
                break;
            }
            ++tried;
            const Node& n = g_nodes[node];
            std::vector<std::pair<int, int>> cells;
            const bool ok = toNode ? MapMemory::routeWithin(layer, x, y, n.x, n.y, kStretchMargin, cells)
                                   : MapMemory::routeWithin(layer, n.x, n.y, x, y, kStretchMargin, cells);
            if (ok)
            {
                out.push_back({node, lengthOf(cells), std::move(cells)});
            }
        }
        return out;
    }

    std::vector<std::pair<int, int>> nearNodes(int layer, int x, int y)
    {
        std::vector<std::pair<int, int>> around;
        for (int i = 0; i < static_cast<int>(g_nodes.size()); ++i)
        {
            const Node& n = g_nodes[i];
            const int d = cheb(n.x, n.y, x, y);
            if (n.layer == layer && d <= kReach)
            {
                around.push_back({i, d});
            }
        }
        std::sort(around.begin(), around.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        return around;
    }

    void addThinned(std::vector<std::pair<int, int>>& out, const std::vector<std::pair<int, int>>& cells, size_t every)
    {
        for (size_t i = 0; i < cells.size(); i += every)
        {
            out.push_back(cells[i]);
        }
        if (!cells.empty() && (cells.size() - 1) % every)
        {
            out.push_back(cells.back());
        }
    }

    // On the low thread: the trip from (fromX, fromY) to (toX, toY).
    Kept work(int layer, int fromX, int fromY, int toX, int toY)
    {
        const DWORD t0 = GetTickCount();
        Kept k;
        k.layer = layer;
        k.fromX = fromX;
        k.fromY = fromY;
        k.toX = toX;
        k.toY = toY;
        // A goal one cannot stand on (a quest giver against a house's wall, the baron at 1676,2352 in a block no route
        // enters): the stretches end on the known walkable tile nearest it, the walk's last steps left to the grid.
        int goalX = toX, goalY = toY;
        if (MapMemory::nearestWalkable(layer, toX, toY, 16, goalX, goalY) && (goalX != toX || goalY != toY))
        {
            LOG("RouteNet: goal {},{} not walkable: the net's trip ends at {},{}", toX, toY, goalX, goalY);
        }
        std::vector<std::pair<int, int>> nearStart, nearGoal;
        {
            const std::lock_guard lock(g_lock);
            if (!g_loaded)
            {
                load();
            }
            nearStart = nearNodes(layer, fromX, fromY);
            nearGoal = nearNodes(layer, goalX, goalY);
        }
        // g_nodes and g_ways are only written by load(), done above: read without the lock from here.
        int triedA = 0, triedB = 0;
        std::vector<End> a = ends(nearStart, layer, fromX, fromY, true, triedA);
        std::vector<End> b = a.empty() ? std::vector<End>{} : ends(nearGoal, layer, goalX, goalY, false, triedB);
        if (a.empty() || b.empty())
        {
            LOG("RouteNet: trip {},{} to {},{} on layer {}: no node reached near the {} ({} within {} tiles, {} tried), {} ms",
                fromX, fromY, toX, toY, layer, a.empty() ? "hero" : "goal", (a.empty() ? nearStart : nearGoal).size(),
                kReach, a.empty() ? triedA : triedB, GetTickCount() - t0);
            k.at = GetTickCount();
            return k;
        }
        // Dijkstra over the nodes, from the ones near the hero (their stretch's cost) to the ones near the goal.
        const size_t n = g_nodes.size();
        std::vector<int> dist(n, INT32_MAX), prev(n, -1), prevWay(n, -1);
        std::vector<int> goalCost(n, -1), goalEnd(n, -1);
        for (size_t i = 0; i < b.size(); ++i)
        {
            goalCost[b[i].node] = b[i].cost;
            goalEnd[b[i].node] = static_cast<int>(i);
        }
        using Entry = std::pair<int, int>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
        std::vector<int> startEnd(n, -1);
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (a[i].cost < dist[a[i].node])
            {
                dist[a[i].node] = a[i].cost;
                startEnd[a[i].node] = static_cast<int>(i);
                open.push({a[i].cost, a[i].node});
            }
        }
        int best = INT32_MAX, bestNode = -1;
        while (!open.empty())
        {
            const auto [c, v] = open.top();
            open.pop();
            if (c > dist[v])
            {
                continue;
            }
            if (c >= best)
            {
                break;
            }
            if (goalCost[v] >= 0 && c + goalCost[v] < best)
            {
                best = c + goalCost[v];
                bestNode = v;
            }
            for (size_t wi = 0; wi < g_ways[v].size(); ++wi)
            {
                const Way& w = g_ways[v][wi];
                if (c + w.cost < dist[w.to])
                {
                    dist[w.to] = c + w.cost;
                    prev[w.to] = v;
                    prevWay[w.to] = static_cast<int>(wi);
                    open.push({c + w.cost, w.to});
                }
            }
        }
        if (bestNode < 0)
        {
            LOG("RouteNet: trip {},{} to {},{} on layer {}: the net has no way between the nodes near either end, {} ms",
                fromX, fromY, toX, toY, layer, GetTickCount() - t0);
            k.at = GetTickCount();
            return k;
        }
        std::vector<int> chain{bestNode};
        while (prev[chain.back()] >= 0)
        {
            chain.push_back(prev[chain.back()]);
        }
        std::reverse(chain.begin(), chain.end());
        addThinned(k.points, a[startEnd[chain.front()]].cells, 6);
        std::string names;
        for (size_t i = 0; i < chain.size(); ++i)
        {
            const Node& nd = g_nodes[chain[i]];
            if (nd.kind != "paso" && names.size() < 160)
            {
                names += (names.empty() ? "" : " > ") + nd.name;
            }
            if (i + 1 < chain.size())
            {
                const Way& w = g_ways[chain[i]][prevWay[chain[i + 1]]];
                const auto& pts = g_points[w.points];
                if (w.reversed)
                {
                    k.points.insert(k.points.end(), pts.rbegin() + 1, pts.rend());
                }
                else
                {
                    k.points.insert(k.points.end(), pts.begin() + 1, pts.end());
                }
            }
        }
        addThinned(k.points, b[goalEnd[chain.back()]].cells, 6);
        if (goalX != toX || goalY != toY)
        {
            k.points.push_back({toX, toY});
        }
        k.ok = true;
        k.tiles = best / 10;
        k.at = GetTickCount();
        LOG("RouteNet: trip {},{} to {},{} on layer {}: {} tiles by the net ({} as the crow flies), {} nodes{}{}, {} points, "
            "worked out in {} ms", fromX, fromY, toX, toY, layer, best / 10, cheb(fromX, fromY, toX, toY), chain.size(),
            names.empty() ? "" : ": ", names, k.points.size(), k.at - t0);
        return k;
    }
}

RouteNet::Trip RouteNet::trip(int layer, int fromX, int fromY, int toX, int toY, std::vector<std::pair<int, int>>& out)
{
    const std::lock_guard lock(g_lock);
    if (g_loaded && g_nodes.empty())
    {
        return Trip::None;
    }
    Kept& k = g_kept;
    const DWORD now = GetTickCount();
    if (k.layer == layer && std::abs(k.toX - toX) <= 4 && std::abs(k.toY - toY) <= 4)
    {
        if (!k.ok)
        {
            // No trip for this goal: the old ways for two minutes, unless the hero has gone well away.
            if (now - k.at < 120000 && cheb(k.fromX, k.fromY, fromX, fromY) <= 24)
            {
                return Trip::None;
            }
        }
        else if (now - k.at < 600000)
        {
            // On the trip still: the point ahead nearest the hero, and the trip from there.
            size_t nearest = k.progress;
            int nearestD = INT32_MAX;
            // Only a window ahead (about 150 tiles): round a mountain the trip comes back near where it was, beyond
            // the rock, and the nearest point of all of it would skip the way round.
            const size_t windowEnd = std::min(k.points.size(), k.progress + 24);
            for (size_t i = k.progress; i < windowEnd; ++i)
            {
                const int d = cheb(k.points[i].first, k.points[i].second, fromX, fromY);
                if (d < nearestD)
                {
                    nearestD = d;
                    nearest = i;
                }
            }
            if (nearestD <= kOffTrip)
            {
                k.progress = nearest;
                out.assign(k.points.begin() + static_cast<std::ptrdiff_t>(nearest), k.points.end());
                out.insert(out.begin(), {fromX, fromY});
                return Trip::Ready;
            }
            static int s_off = 0;
            if (s_off++ < 40)
            {
                LOG("RouteNet: the hero at {},{} is {} tiles off the trip to {},{}: worked out again", fromX, fromY,
                    nearestD, toX, toY);
            }
        }
    }
    if (!g_busy)
    {
        g_busy = true;
        std::thread([=] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
            Kept r = work(layer, fromX, fromY, toX, toY);
            const std::lock_guard done(g_lock);
            g_kept = std::move(r);
            g_busy = false;
        }).detach();
    }
    return Trip::Working;
}

float RouteNet::walkTiles(int fromX, int fromY, int toX, int toY)
{
    const std::lock_guard lock(g_lock);
    if (g_loaded && g_nodes.empty())
    {
        return -2.0f;
    }
    const DWORD now = GetTickCount();
    const auto same = [&](const Kept& k) {
        return k.layer == 0 && std::abs(k.toX - toX) <= 4 && std::abs(k.toY - toY) <= 4 && cheb(k.fromX, k.fromY, fromX, fromY) <= 12;
    };
    for (const Kept& k : g_lengths)
    {
        if (same(k) && now - k.at < 300000)
        {
            return k.ok ? static_cast<float>(k.tiles) : -2.0f;
        }
    }
    // The trip walked now may be the one asked for.
    if (g_kept.ok && same(g_kept) && now - g_kept.at < 300000)
    {
        return static_cast<float>(g_kept.tiles);
    }
    if (!g_busy)
    {
        g_busy = true;
        std::thread([=] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
            Kept r = work(0, fromX, fromY, toX, toY);
            r.points.clear();   // only its length is kept
            r.points.shrink_to_fit();
            const std::lock_guard done(g_lock);
            g_lengths.push_front(std::move(r));
            if (g_lengths.size() > 24)
            {
                g_lengths.pop_back();
            }
            g_busy = false;
        }).detach();
    }
    return -1.0f;
}
