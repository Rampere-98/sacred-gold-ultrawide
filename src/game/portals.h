#pragma once
#include <cstdint>

// The game's portal network, used as a player does (the player, 2026-11-16: «todo lo que un jugador puede hacer está
// permitido, portales incluidos»; never a teleport by writing the position). A portal joins the hero's network when
// the hero steps onto it (the script's Teleporter(id) sets bit id of hero+0x578 and opens the portal box, Manolito
// block AE); in the box, only the rows of portals joined can be picked, and a pick sends the hero to that portal.
// Here: where the surface portals stand (the EE_USEPORTAL switch at ENG 0x6195F7, ids 0..12; 13 is the add-on's
// island, 14..27 the underworld's, which does not link with the surface), which ones the hero has joined, and
// whether a walk is shorter through two of them.
namespace Portals
{
    constexpr int kCount = 13;  // surface portals 0..12

    // The portal's trigger tile (stepped onto, it opens the box; the hero lands there too, within a tile), layer 0.
    void tile(int id, int& tx, int& ty);
    // The hero's joined portals (hero+0x578, bit id); 0 with no hero.
    uint32_t joined();

    // A walk from tile (hx, hy) to (gx, gy) on the surface compared with the way through portals: walk to the joined
    // portal `from`, pick `to` in its box, walk on from there. True when the portals save enough (the walk over
    // `kMinWalkTiles`, the way through them at most `kShare` of it and `kMinSavedTiles` shorter); lengths in tiles,
    // as the crow flies (both ways alike), with `kBoxTiles` for the stop at the box.
    struct Plan
    {
        int from = -1, to = -1;
        double walk = 0.0, through = 0.0;
    };
    constexpr double kMinWalkTiles = 150.0, kMinSavedTiles = 100.0, kShare = 0.7, kBoxTiles = 20.0;
    // `skip`: portals not to use now (bit = id; a portal whose box did not come up lately).
    // `walk`: the walk's length when known better than the straight line (the route net), else 0.
    bool plan(int hx, int hy, int gx, int gy, uint32_t skip, double walk, Plan& out);

    // The nearest portal not joined yet within `radius` tiles of (hx, hy), -1 if none.
    int unjoinedNear(int hx, int hy, double radius, uint32_t skip);

    // The trip under way, shared with the portal box's watcher (another thread: the box holds the game in its modal
    // loop, no frames meanwhile). `to` -1: only going to join `from`.
    void arm(int from, int to);
    void disarm();
    int armedFrom();
    int armedTo();
    // The watcher clicked the row of `to` (the tick, 0 not yet).
    void clicked(uint32_t at);
    uint32_t clickedAt();
}
