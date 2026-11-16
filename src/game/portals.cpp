#include "game/portals.h"
#include "game/teleports.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <windows.h>

namespace
{
    // Each portal's trigger: the script zone Wegpunkte_<zone> of one tile that calls Teleporter(id) when stepped onto
    // (Manolito, block BM, docs/Sacred/Portales.tsv). The EE_USEPORTAL switch (ENG 0x6195F7) lands the hero on the
    // same tile, but for 0 (lands on 3357,2459) and 12 (4625,2489).
    constexpr int kTiles[Portals::kCount][2] = {
        {3355, 2457}, {2642, 3057}, {1343, 2239}, {2383, 4160}, {1393, 2832}, {1860, 3496}, {3500, 3590},
        {2455, 2010}, {1395, 1508}, {3132, 2220}, {3291, 1397}, {4062, 2508}, {4625, 2488},
    };

    std::atomic<int> g_from{-1}, g_to{-1};
    std::atomic<uint32_t> g_clickedAt{0};

    double tiles(int ax, int ay, int bx, int by)
    {
        return std::hypot(static_cast<double>(ax - bx), static_cast<double>(ay - by));
    }
}

void Portals::tile(int id, int& tx, int& ty)
{
    tx = id >= 0 && id < kCount ? kTiles[id][0] : 0;
    ty = id >= 0 && id < kCount ? kTiles[id][1] : 0;
}

uint32_t Portals::joined()
{
    const uintptr_t hero = Teleports::hero();
    return hero ? *reinterpret_cast<const uint32_t*>(hero + 0x578) & ((1u << kCount) - 1) : 0;
}

bool Portals::plan(int hx, int hy, int gx, int gy, uint32_t skip, double walk, Plan& out)
{
    out = Plan{};
    out.walk = std::max(tiles(hx, hy, gx, gy), walk);
    if (out.walk < kMinWalkTiles)
    {
        return false;
    }
    const uint32_t mask = joined() & ~skip;
    double best = out.walk;
    for (int from = 0; from < kCount; ++from)
    {
        if (!(mask & (1u << from)))
        {
            continue;
        }
        const double toFrom = tiles(hx, hy, kTiles[from][0], kTiles[from][1]);
        for (int to = 0; to < kCount; ++to)
        {
            if (to == from || !(mask & (1u << to)))
            {
                continue;
            }
            const double way = toFrom + kBoxTiles + tiles(kTiles[to][0], kTiles[to][1], gx, gy);
            if (way < best)
            {
                best = way;
                out.from = from;
                out.to = to;
            }
        }
    }
    out.through = best;
    return out.from >= 0 && best <= out.walk * kShare && out.walk - best >= kMinSavedTiles;
}

int Portals::unjoinedOnTheWay(int hx, int hy, int gx, int gy, double maxExtra, uint32_t skip, double& extra)
{
    const uint32_t mask = joined() | skip;
    const double direct = tiles(hx, hy, gx, gy);
    int best = -1;
    extra = maxExtra;
    for (int id = 0; id < kCount; ++id)
    {
        if (mask & (1u << id))
        {
            continue;
        }
        const double more = tiles(hx, hy, kTiles[id][0], kTiles[id][1]) + tiles(kTiles[id][0], kTiles[id][1], gx, gy) - direct;
        if (more <= extra)
        {
            extra = more;
            best = id;
        }
    }
    return best;
}

void Portals::arm(int from, int to)
{
    g_clickedAt = 0;
    g_to = to;
    g_from = from;
}

void Portals::disarm()
{
    g_from = -1;
    g_to = -1;
    g_clickedAt = 0;
}

int Portals::armedFrom()
{
    return g_from;
}

int Portals::armedTo()
{
    return g_to;
}

void Portals::clicked(uint32_t at)
{
    g_clickedAt = at ? at : 1;
}

uint32_t Portals::clickedAt()
{
    return g_clickedAt;
}
