#include "game/whereabouts.h"
#include "game/click_walk.h"
#include "log.h"

#include <imgui.h>
#include <windows.h>
#include <algorithm>
#include <cstdlib>
#include <unordered_set>
#include <vector>

namespace
{
    constexpr int kSide = 2 * Whereabouts::kRadius + 1;
    Whereabouts::Area g_area;
    std::vector<uint8_t> g_reached;     // kSide*kSide, 1 reached
    DWORD g_fillAt = 0;

    // The same flood without the window, for an area that looked open only because the window ended (the stuck
    // solver, 2026-11-16, A5: the cave 5.2 is 97 tiles wide and the window 129; from x < 5350 or x > 5380 its far end
    // is cut off by the window and the fill says «open ground»: 43 of 43 verdicts inside the cave were open outside
    // that band, 0 inside it; the hero sat in the west chamber for 36 h with every shut-in rule asleep). Up to 6000
    // tiles; any unknown tile ends it as open (not known to be shut).
    bool closedBeyondWindow(int layer, int hx, int hy, int& total, int& minX, int& maxX, int& minY, int& maxY)
    {
        constexpr size_t kCap = 6000;
        const auto key = [](int x, int y) {
            return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(y);
        };
        std::unordered_set<uint64_t> seen;
        std::vector<std::pair<int, int>> queue;
        seen.insert(key(hx, hy));
        queue.emplace_back(hx, hy);
        minX = maxX = hx;
        minY = maxY = hy;
        static const int kDx[4] = {1, -1, 0, 0}, kDy[4] = {0, 0, 1, -1};
        for (size_t i = 0; i < queue.size(); ++i)
        {
            const auto [x, y] = queue[i];
            for (int d = 0; d < 4; ++d)
            {
                const int nx = x + kDx[d], ny = y + kDy[d];
                if (seen.count(key(nx, ny)))
                {
                    continue;
                }
                const int w = ClickWalk::walkable(layer, nx, ny);
                if (w < 0)
                {
                    return false;
                }
                if (!w)
                {
                    continue;
                }
                if (queue.size() >= kCap)
                {
                    return false;
                }
                seen.insert(key(nx, ny));
                queue.emplace_back(nx, ny);
                minX = std::min(minX, nx);
                maxX = std::max(maxX, nx);
                minY = std::min(minY, ny);
                maxY = std::max(maxY, ny);
            }
        }
        total = static_cast<int>(queue.size());
        return true;
    }

    void fill(int layer, int hx, int hy)
    {
        g_reached.assign(kSide * kSide, 0);
        Whereabouts::Area a;
        a.valid = true;
        a.layer = layer;
        a.heroX = hx;
        a.heroY = hy;
        a.minX = a.maxX = hx;
        a.minY = a.maxY = hy;
        bool open = false;
        bool unknownSeen = false;   // a tile beyond the loaded sectors: the window cannot be widened to decide
        std::vector<int> queue;
        queue.reserve(kSide * kSide);
        const int start = Whereabouts::kRadius * kSide + Whereabouts::kRadius;
        g_reached[start] = 1;
        queue.push_back(start);
        static const int kDx[4] = {1, -1, 0, 0}, kDy[4] = {0, 0, 1, -1};
        for (size_t i = 0; i < queue.size(); ++i)
        {
            const int cx = queue[i] % kSide, cy = queue[i] / kSide;
            const int tx = hx + cx - Whereabouts::kRadius, ty = hy + cy - Whereabouts::kRadius;
            ++a.tiles;
            a.minX = std::min(a.minX, tx);
            a.maxX = std::max(a.maxX, tx);
            a.minY = std::min(a.minY, ty);
            a.maxY = std::max(a.maxY, ty);
            for (int d = 0; d < 4; ++d)
            {
                const int nx = cx + kDx[d], ny = cy + kDy[d];
                if (nx < 0 || ny < 0 || nx >= kSide || ny >= kSide)
                {
                    open = true;    // the fill ran to its edge: the way goes on further out
                    continue;
                }
                const int n = ny * kSide + nx;
                if (g_reached[n])
                {
                    continue;
                }
                const int w = ClickWalk::walkable(layer, hx + nx - Whereabouts::kRadius, hy + ny - Whereabouts::kRadius);
                if (w < 0)
                {
                    open = true;    // beyond the loaded sectors: not known to be shut
                    unknownSeen = true;
                    continue;
                }
                if (w)
                {
                    g_reached[n] = 1;
                    queue.push_back(n);
                }
            }
        }
        // Open only for the window's edge (not for unknown ground): the flood goes on past the window; if it ends
        // inside 6000 tiles the area is shut in whatever the hero's place in it.
        if (open && !unknownSeen && a.tiles > 1 && a.tiles < 4000)
        {
            int total = 0, loX = a.minX, hiX = a.maxX, loY = a.minY, hiY = a.maxY;
            if (closedBeyondWindow(layer, hx, hy, total, loX, hiX, loY, hiY))
            {
                open = false;
                a.tiles = total;
                a.minX = loX;
                a.maxX = hiX;
                a.minY = loY;
                a.maxY = hiY;
            }
        }
        // The hero's own tile not walkable by the test (standing on an object's edge): no verdict.
        a.shutIn = !open && a.tiles > 1;
        const bool changed = a.shutIn != g_area.shutIn || !g_area.valid || a.layer != g_area.layer ||
                             (a.tiles <= 1) != (g_area.tiles <= 1);
        g_area = a;
        if (changed)
        {
            if (a.tiles <= 1)
            {
                // Seen live (2026-11-09): inside the Vampiress's start house the test says no to the hero's own tile.
                LOG("Whereabouts: the tile test does not know the ground at tile {},{} (layer {}): no verdict", hx, hy,
                    layer);
            }
            else if (a.shutIn)
            {
                LOG("Whereabouts: shut in, {} tiles reachable (tiles {}..{} x {}..{}, layer {})", a.tiles, a.minX, a.maxX,
                    a.minY, a.maxY, a.layer);
            }
            else
            {
                LOG("Whereabouts: open ground around tile {},{} (layer {})", hx, hy, layer);
            }
        }
    }
}

void Whereabouts::update(int layer, int tileX, int tileY)
{
    const DWORD now = GetTickCount();
    const bool moved = !g_area.valid || layer != g_area.layer || tileX != g_area.heroX || tileY != g_area.heroY;
    if ((moved && now - g_fillAt > 1000) || now - g_fillAt > 5000)
    {
        g_fillAt = now;
        fill(layer, tileX, tileY);
    }
}

const Whereabouts::Area& Whereabouts::area()
{
    return g_area;
}

int Whereabouts::reachable(int x, int y)
{
    if (!g_area.valid)
    {
        return -1;
    }
    const int cx = x - g_area.heroX + kRadius, cy = y - g_area.heroY + kRadius;
    if (cx < 0 || cy < 0 || cx >= kSide || cy >= kSide)
    {
        return -1;
    }
    return g_reached[cy * kSide + cx] ? 1 : 0;
}

bool Whereabouts::nearestReachable(int x, int y, int& outX, int& outY)
{
    // Only the hero's own tile (the test blind here): no answer, not «go where you stand».
    if (!g_area.valid || g_area.tiles <= 1)
    {
        return false;
    }
    long best = -1;
    for (int cy = 0; cy < kSide; ++cy)
    {
        for (int cx = 0; cx < kSide; ++cx)
        {
            if (!g_reached[cy * kSide + cx])
            {
                continue;
            }
            const int tx = g_area.heroX + cx - kRadius, ty = g_area.heroY + cy - kRadius;
            const long d = static_cast<long>(tx - x) * (tx - x) + static_cast<long>(ty - y) * (ty - y);
            if (best < 0 || d < best)
            {
                best = d;
                outX = tx;
                outY = ty;
            }
        }
    }
    return best >= 0;
}

void Whereabouts::drawMenu()
{
    const Area& a = g_area;
    if (!a.valid)
    {
        ImGui::TextDisabled("Aún sin mirar alrededor.");
        return;
    }
    ImGui::Text("Casilla %d,%d, capa %d", a.heroX, a.heroY, a.layer);
    if (a.shutIn)
    {
        ImGui::Text("Recinto cerrado: %d casillas alcanzables (%d..%d x %d..%d)", a.tiles, a.minX, a.maxX, a.minY,
                    a.maxY);
    }
    else
    {
        ImGui::Text("Terreno abierto (%d casillas alcanzables a %d de radio)", a.tiles, kRadius);
    }
}
