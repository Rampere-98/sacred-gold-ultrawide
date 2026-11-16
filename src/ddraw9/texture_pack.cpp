#include "ddraw9/texture_pack.h"
#include "ddraw9/gpu.h"
#include "config/textures.h"
#include "log.h"

#include <SDL3/SDL.h>
#include <windows.h>

#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace DDraw9::TexturePack
{
    namespace
    {
        constexpr size_t kMaxQueuedDumps = 512;     // beyond: dropped and dumped again when next seen
        constexpr int kMaxReplacementSize = 4096;

        enum class State
        {
            Queued,
            Decoded,
            Ready,
            Failed,
        };

        struct Replacement
        {
            State state = State::Queued;
            int width = 0, height = 0;
            std::unique_ptr<uint32_t[]> pixels;     // decoded A8R8G8B8, until the texture is created
            d9::IDirect3DTexture9* texture = nullptr;
        };

        struct Job
        {
            uint64_t hash = 0;
            bool decode = false;                    // else dump
            uint32_t width = 0, height = 0;
            std::string format;
            std::unique_ptr<uint32_t[]> argb;
        };

        std::once_flag g_init;
        std::wstring g_root;                        // ...\SacredBild\ (the game folder's)
        std::wstring g_folder;                      // ...\SacredBild\textures\ (UTF-8 copy below for SDL)
        std::string g_folderUtf8;
        std::unordered_set<uint64_t> g_index;       // replacements on disk (read-only after init)

        std::mutex g_lock;                          // everything below
        std::condition_variable g_wake;
        std::deque<Job> g_jobs;
        size_t g_queuedDumps = 0;
        bool g_workerStarted = false;
        std::unordered_set<uint64_t> g_dumped;      // on disk or queued
        bool g_dumpFolderReady = false;
        uint32_t g_dumpedNew = 0, g_dumpedSeen = 0;
        std::unordered_map<uint64_t, std::unique_ptr<Replacement>> g_replacements;
        uint32_t g_created = 0, g_failed = 0;

        std::string utf8(const std::wstring& s)
        {
            if (s.empty())
            {
                return {};
            }
            const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
            std::string out(n, '\0');
            WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
            return out;
        }

        std::string hexName(uint64_t hash)
        {
            char name[24] = {};
            std::snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(hash));
            return name;
        }

        // "0123456789abcdef.png" -> hash.
        bool parseName(const wchar_t* name, uint64_t& out)
        {
            if (wcslen(name) != 20 || _wcsicmp(name + 16, L".png") != 0)
            {
                return false;
            }
            uint64_t v = 0;
            for (int i = 0; i < 16; ++i)
            {
                const wchar_t c = name[i];
                const int d = c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 :
                    c >= L'A' && c <= L'F' ? c - L'A' + 10 : -1;
                if (d < 0)
                {
                    return false;
                }
                v = (v << 4) | static_cast<uint64_t>(d);
            }
            out = v;
            return true;
        }

        // Calls `fn(hash)` for every <hash>.png in `folder`.
        template <typename Fn>
        void listPngs(const std::wstring& folder, Fn&& fn)
        {
            WIN32_FIND_DATAW found = {};
            HANDLE find = FindFirstFileW((folder + L"*.png").c_str(), &found);
            if (find == INVALID_HANDLE_VALUE)
            {
                return;
            }
            do
            {
                uint64_t hash = 0;
                if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && parseName(found.cFileName, hash))
                {
                    fn(hash);
                }
            } while (FindNextFileW(find, &found));
            FindClose(find);
        }

        void init()
        {
            std::call_once(g_init, [] {
                wchar_t path[MAX_PATH] = {};
                GetModuleFileNameW(nullptr, path, MAX_PATH);
                std::wstring dir = path;
                dir = dir.substr(0, dir.find_last_of(L"\\/"));
                g_root = dir + L"\\SacredBild\\";
                g_folder = g_root + L"textures\\";
                g_folderUtf8 = utf8(g_folder);
                if (Config::textures.replace)
                {
                    listPngs(g_folder, [](uint64_t hash) { g_index.insert(hash); });
                    LOG("Textures: {} replacements in SacredBild\\textures", g_index.size());
                }
                if (Config::textures.dump)
                {
                    LOG("Textures: dumping new textures to SacredBild\\textures\\dump");
                }
            });
        }

        void writeDump(Job& job)
        {
            const std::string name = hexName(job.hash);
            SDL_Surface* surface = SDL_CreateSurfaceFrom(static_cast<int>(job.width), static_cast<int>(job.height),
                SDL_PIXELFORMAT_ARGB8888, job.argb.get(), static_cast<int>(job.width * 4));
            const std::string file = g_folderUtf8 + "dump\\" + name + ".png";
            const bool ok = surface && SDL_SavePNG(surface, file.c_str());
            SDL_DestroySurface(surface);
            if (!ok)
            {
                static int failures = 0;
                if (++failures <= 4)
                {
                    LOG("Textures: writing dump\\{}.png failed ({})", name, SDL_GetError());
                }
                return;
            }
            if (FILE* index = _wfopen((g_folder + L"dump\\index.txt").c_str(), L"a"))
            {
                std::fprintf(index, "%s %ux%u %s\n", name.c_str(), job.width, job.height, job.format.c_str());
                std::fclose(index);
            }
        }

        void decode(Job& job)
        {
            const std::string file = g_folderUtf8 + hexName(job.hash) + ".png";
            SDL_Surface* loaded = SDL_LoadPNG(file.c_str());
            SDL_Surface* argb = loaded ? SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_ARGB8888) : nullptr;
            SDL_DestroySurface(loaded);
            std::unique_ptr<uint32_t[]> pixels;
            int w = 0, h = 0;
            if (argb && argb->w > 0 && argb->h > 0 && argb->w <= kMaxReplacementSize && argb->h <= kMaxReplacementSize)
            {
                w = argb->w;
                h = argb->h;
                pixels.reset(new (std::nothrow) uint32_t[size_t(w) * h]);
                if (pixels)
                {
                    for (int y = 0; y < h; ++y)
                    {
                        std::memcpy(pixels.get() + size_t(y) * w, static_cast<const uint8_t*>(argb->pixels) + size_t(y) * argb->pitch,
                            size_t(w) * 4);
                    }
                }
            }
            else
            {
                LOG("Textures: {}.png not loaded ({})", hexName(job.hash),
                    argb ? "larger than 4096 texels" : SDL_GetError());
            }
            SDL_DestroySurface(argb);
            std::scoped_lock lock(g_lock);
            Replacement& r = *g_replacements[job.hash];
            if (pixels)
            {
                r.width = w;
                r.height = h;
                r.pixels = std::move(pixels);
                r.state = State::Decoded;
            }
            else
            {
                r.state = State::Failed;
            }
        }

        void worker()
        {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            for (;;)
            {
                Job job;
                {
                    std::unique_lock lock(g_lock);
                    g_wake.wait(lock, [] { return !g_jobs.empty(); });
                    // Decoding first: it is what the screen waits for.
                    auto it = g_jobs.begin();
                    for (auto j = g_jobs.begin(); j != g_jobs.end(); ++j)
                    {
                        if (j->decode)
                        {
                            it = j;
                            break;
                        }
                    }
                    job = std::move(*it);
                    g_jobs.erase(it);
                    if (!job.decode)
                    {
                        --g_queuedDumps;
                    }
                }
                if (job.decode)
                {
                    decode(job);
                }
                else
                {
                    writeDump(job);
                }
            }
        }

        // With g_lock held.
        void push(Job&& job)
        {
            if (!g_workerStarted)
            {
                g_workerStarted = true;
                std::thread(worker).detach();
            }
            if (!job.decode)
            {
                ++g_queuedDumps;
            }
            g_jobs.push_back(std::move(job));
            g_wake.notify_one();
        }

        // With g_lock held: the dump folder exists and g_dumped lists what is already in it.
        void prepareDumpFolder()
        {
            if (g_dumpFolderReady)
            {
                return;
            }
            g_dumpFolderReady = true;
            CreateDirectoryW(g_root.c_str(), nullptr);
            CreateDirectoryW(g_folder.c_str(), nullptr);
            CreateDirectoryW((g_folder + L"dump").c_str(), nullptr);
            listPngs(g_folder + L"dump\\", [](uint64_t hash) { g_dumped.insert(hash); });
            LOG("Textures: {} textures already in dump", g_dumped.size());
        }

        uint64_t mix(uint64_t h)
        {
            h ^= h >> 33;
            h *= 0xff51afd7ed558ccdull;
            h ^= h >> 33;
            h *= 0xc4ceb9fe1a85ec53ull;
            h ^= h >> 33;
            return h;
        }
    }

    bool dumping()
    {
        return Config::textures.dump;
    }

    bool replacing()
    {
        return Config::textures.replace;
    }

    uint64_t hash(const uint8_t* bits, int pitch, uint32_t rowBytes, uint32_t rows, uint32_t width, uint32_t height,
        uint32_t format)
    {
        // Two independent 32-bit lanes (the DLL is 32-bit) folded into 64 bits at the end.
        uint32_t a = 0x811c9dc5u ^ width ^ (format << 16), b = 0x9e3779b9u ^ height ^ (format >> 16);
        for (uint32_t y = 0; y < rows; ++y)
        {
            const uint8_t* p = bits + size_t(y) * pitch;
            uint32_t i = 0;
            for (; i + 8 <= rowBytes; i += 8)
            {
                uint32_t w0, w1;
                std::memcpy(&w0, p + i, 4);
                std::memcpy(&w1, p + i + 4, 4);
                a = (a ^ w0) * 0x01000193u;
                a ^= a >> 15;
                b = (b ^ w1) * 0x85ebca6bu;
                b ^= b >> 13;
            }
            for (; i < rowBytes; ++i)
            {
                a = (a ^ p[i]) * 0x01000193u;
            }
            b = (b ^ y) * 0xc2b2ae35u;
        }
        return mix((uint64_t(a) << 32 | b) ^ (uint64_t(width) << 48) ^ (uint64_t(height) << 32) ^ format);
    }

    bool has(uint64_t hash)
    {
        init();
        return g_index.count(hash) != 0;
    }

    d9::IDirect3DTexture9* texture(uint64_t hash, bool alpha)
    {
        init();
        if (!g_index.count(hash))
        {
            return nullptr;
        }
        std::unique_ptr<uint32_t[]> pixels;
        int w = 0, h = 0;
        {
            std::scoped_lock lock(g_lock);
            auto& slot = g_replacements[hash];
            if (!slot)
            {
                slot = std::make_unique<Replacement>();
                Job job;
                job.hash = hash;
                job.decode = true;
                push(std::move(job));
                return nullptr;
            }
            if (slot->state != State::Decoded)
            {
                return slot->state == State::Ready ? slot->texture : nullptr;
            }
            pixels = std::move(slot->pixels);
            w = slot->width;
            h = slot->height;
            slot->state = State::Failed;    // until created below
        }
        d9::IDirect3DDevice9Ex* dev = Gpu::existingDevice();
        const d9::D3DFORMAT format = alpha ? d9::D3DFMT_A8R8G8B8 : d9::D3DFMT_X8R8G8B8;
        d9::IDirect3DTexture9* staging = nullptr;
        d9::IDirect3DTexture9* texture = nullptr;
        bool ok = dev && SUCCEEDED(dev->CreateTexture(w, h, 1, 0, format, d9::D3DPOOL_SYSTEMMEM, &staging, nullptr)) &&
            SUCCEEDED(dev->CreateTexture(w, h, 1, 0, format, d9::D3DPOOL_DEFAULT, &texture, nullptr));
        d9::D3DLOCKED_RECT locked = {};
        if (ok && SUCCEEDED(staging->LockRect(0, &locked, nullptr, 0)))
        {
            for (int y = 0; y < h; ++y)
            {
                std::memcpy(static_cast<uint8_t*>(locked.pBits) + size_t(y) * locked.Pitch, pixels.get() + size_t(y) * w,
                    size_t(w) * 4);
            }
            staging->UnlockRect(0);
            ok = SUCCEEDED(dev->UpdateTexture(staging, texture));
        }
        else
        {
            ok = false;
        }
        if (staging)
        {
            staging->Release();
        }
        if (!ok && texture)
        {
            texture->Release();
            texture = nullptr;
        }
        std::scoped_lock lock(g_lock);
        Replacement& r = *g_replacements[hash];
        r.texture = texture;
        r.state = texture ? State::Ready : State::Failed;
        if (texture)
        {
            ++g_created;
            if (g_created <= 8 || g_created % 100 == 0)
            {
                LOG("Textures: {} replacements in use ({}.png, {}x{})", g_created, hexName(hash), w, h);
            }
        }
        else if (++g_failed <= 8)
        {
            LOG("Textures: creating the {}x{} replacement {} failed", w, h, hexName(hash));
        }
        return texture;
    }

    bool wantsDump(uint64_t hash)
    {
        init();
        std::scoped_lock lock(g_lock);
        prepareDumpFolder();
        if (g_dumped.count(hash))
        {
            if (++g_dumpedSeen % 5000 == 0)
            {
                LOG("Textures: {} new textures dumped this session, {} uses of ones already dumped", g_dumpedNew, g_dumpedSeen);
            }
            return false;
        }
        return g_queuedDumps < kMaxQueuedDumps;
    }

    void dump(uint64_t hash, uint32_t width, uint32_t height, const char* format, uint32_t* argb)
    {
        Job job;
        job.hash = hash;
        job.width = width;
        job.height = height;
        job.format = format;
        job.argb.reset(argb);
        std::scoped_lock lock(g_lock);
        if (!g_dumped.insert(hash).second)
        {
            return;
        }
        ++g_dumpedNew;
        if (g_dumpedNew % 250 == 0)
        {
            LOG("Textures: {} new textures dumped this session, {} uses of ones already dumped", g_dumpedNew, g_dumpedSeen);
        }
        push(std::move(job));
    }
}
