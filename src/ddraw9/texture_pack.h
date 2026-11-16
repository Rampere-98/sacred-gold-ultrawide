#pragma once
#include "ddraw9/d3d9_api.h"

#include <cstdint>

// [Textures]: dumping the game's textures as PNG and drawing replacements (e.g. upscaled ones) instead.
//
// A texture is identified by a 64-bit hash of its pixels (row by row, without pitch padding), size and format,
// computed when the game has changed it and the texture is next used. Replacements are SacredBild\textures\<hash>
// .png, listed once at startup; they are decoded on a worker thread and become Direct3D 9 textures on the render
// thread, shared by every surface with that content. Until one is ready the game's own texture is drawn.
namespace DDraw9::TexturePack
{
    bool dumping();
    bool replacing();

    // Hash of `rows` rows of `rowBytes` bytes each, `pitch` bytes apart, plus the texture's size and format.
    uint64_t hash(const uint8_t* bits, int pitch, uint32_t rowBytes, uint32_t rows, uint32_t width, uint32_t height,
        uint32_t format);

    // True if SacredBild\textures has a replacement for this hash (no file access: the folder was listed once).
    bool has(uint64_t hash);

    // The replacement texture for `hash`, or null while it is still being decoded (or failed). Render thread only.
    // `alpha`: the game's texture has an alpha channel (else the replacement is created opaque).
    d9::IDirect3DTexture9* texture(uint64_t hash, bool alpha);

    // Queues `argb` (width * height pixels, A8R8G8B8, top row first) to be written as dump\<hash>.png unless that
    // hash was written already. Takes the buffer over.
    void dump(uint64_t hash, uint32_t width, uint32_t height, const char* format, uint32_t* argb);
    // Whether `hash` still needs dumping (so the caller can skip converting the pixels).
    bool wantsDump(uint64_t hash);
}
