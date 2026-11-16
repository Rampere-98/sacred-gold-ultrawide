#include "game/portals.h"
#include "game/teleports.h"

#include <atomic>
#include <cmath>
#include <windows.h>

namespace
{
    // The EE_USEPORTAL switch (ENG 0x6195F7, jump table 0x619B0C): portal id -> tile; ids 0..12 match the map's
    // teleporter landmarks (class 1) live (docs/AGENT_ADDRESSES.md «Mapa», «Red de portales», block AE).
    constexpr int kTiles[Portals::kCount][2] = {
        {3357, 2459}, {2642, 3057}, {1343, 2239}, {2383, 4160}, {1393, 2832}, {1860, 3496}, {3500, 3590},
        {2455, 2010}, {1395, 1508}, {3132, 2220}, {3291, 1397}, {4062, 2508}, {4625, 2489},
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

bool Portals::plan(int hx, int hy, int gx, int gy, uint32_t skip, Plan& out)
{
    out = Plan{};
    out.walk = tiles(hx, hy, gx, gy);
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

int Portals::unjoinedNear(int hx, int hy, double radius, uint32_t skip)
{
    const uint32_t mask = joined() | skip;
    int nearest = -1;
    double best = radius;
    for (int id = 0; id < kCount; ++id)
    {
        if (mask & (1u << id))
        {
            continue;
        }
        if (const double d = tiles(hx, hy, kTiles[id][0], kTiles[id][1]); d <= best)
        {
            best = d;
            nearest = id;
        }
    }
    return nearest;
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
