#pragma once

#include <utility>
#include <vector>

// The route net (the player, 2026-11-11: «las rutas ya deberían estar precalculadas antes que el agente piense en
// usarlas»): towns, services, entrances, portals and way points, with the walks between them worked out away from the
// game over the eight copies' shared map (tools/red_rutas.py red -> steamapps\common\SacredBild-red.txt). Live, only
// the short stretches are searched (the hero to a node near, a node near to the goal, MapMemory::routeWithin in small
// boxes) and the net between them, a few hundred nodes: no big search on the game's time.
namespace RouteNet
{
    enum class Trip
    {
        None,       // no net, or no node reached near one of the ends: the old ways
        Working,    // being worked out on a low thread: ask again
        Ready,      // `out` holds the tiles to walk, from about the hero to the goal, a point every few tiles
    };
    Trip trip(int layer, int fromX, int fromY, int toX, int toY, std::vector<std::pair<int, int>>& out);
}
