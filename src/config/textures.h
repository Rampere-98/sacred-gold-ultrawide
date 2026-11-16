#pragma once

// [Textures] in SacredBild.ini (read by config.cpp). Needs [DDraw] Backend=d3d9.
namespace Config
{
    struct Textures
    {
        // Write every distinct texture content the game uploads as SacredBild\textures\dump\<hash>.png (once per
        // hash; the hash covers the pixels, size and format), with a line per texture in dump\index.txt.
        bool dump = false;
        // Draw SacredBild\textures\<hash>.png instead of the game's texture with that hash. The replacement may be
        // larger (2x, 4x...): texture coordinates are normalized, so it covers the same area in finer detail.
        bool replace = false;
    };
    inline Textures textures;
}
