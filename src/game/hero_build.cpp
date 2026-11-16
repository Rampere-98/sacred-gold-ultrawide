#include "game/hero_build.h"
#include "game/act.h"
#include "game/combat_learn.h"
#include "game/combat_state.h"
#include "game/gear.h"
#include "game/tactics.h"
#include "game/mind.h"
#include "config/tactics.h"
#include "game/modkit_menu.h"
#include "input/inject.h"
#include "log.h"
#include "sacred/object.h"
#include "sacred/ui.h"
#include "sig.h"

#include <imgui.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <cwchar>
#include <cstdio>
#include <format>
#include <vector>

using namespace Sacred;

namespace
{
    struct Settings
    {
        bool attributes = true;
        bool skills = true;
        bool learn = true;
        bool bar = true;
        bool swap = true;       // [Build] SwapBar: arts swapped on a full bar for the situation (docs/Artes-de-la-barra.md)
        bool presets = false;   // [Build] ArtPresets: each weapon slot keeps its own art bar
    };
    Settings g_settings;

    // stats = creature + 0x3A8 (seen live, 2026-11-09): +8 class; +0x1C u8[6] points put in each attribute, +0x23
    // attribute points to spend; +0x24 u8[8] skill id per slot, +0x2C u8[8] its base level; +0x42 u16 skill points to
    // spend; +0x44 u16 new skill slots to fill; +0x56 u16 the hero's level.
    constexpr size_t kStats = 0x3A8;

    uintptr_t g_spendAttr = 0, g_addSkillLevel = 0, g_learnable = 0, g_learnSkill = 0, g_setSlot = 0;
    const Sig::Entry kSigs[] = {
        {"spendAttr", "64 A1 00 00 00 00 6A FF 68 ?? ?? ?? ?? 50 64 89 25 00 00 00 00 83 EC 7C 53 56 57 8B ?? ?? ?? ?? 00 00 83 FF 07",
            0, Sig::Take::Match, &g_spendAttr},
        {"addSkillLevel", "6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 ?? ?? ?? ?? 00 00 8B ?? ?? ?? ?? 00 00 53 56 57 8B F9",
            0, Sig::Take::Match, &g_addSkillLevel},
        {"learnableSkills", "83 EC 10 56 8B F1 8B 0D ?? ?? ?? ??", 0, Sig::Take::Match, &g_learnable},
        {"learnSkill", "6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 ?? ?? ?? ?? 00 00 53 56 57 8B F9 33 DB",
            0, Sig::Take::Match, &g_learnSkill},
        // The taskbar's art slot setter, as a drop onto a slot (ENG 005D94C0): ecx = the player's data, (slot, kind,
        // w2, const u32 data[4]); it also tells the server.
        {"setTaskbarSlot", "6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 83 EC 6C 8B 54 24 7C",
            0, Sig::Take::Match, &g_setSlot},
    };
    bool g_ready = false;

    using SpendAttr = void(__fastcall*)(void* stats, void*, uint32_t attr);
    using AddSkillLevel = void(__fastcall*)(void* stats, void*, uint32_t slot, int16_t delta, bool pay);
    using Learnable = int(__fastcall*)(void* stats, void*, uint8_t* out);
    using LearnSkill = void(__fastcall*)(void* stats, void*, uint8_t id);
    using SetSlot = void(__fastcall*)(void* player, void*, uint32_t slot, uint32_t kind, uint32_t w2, const uint32_t* data);

    // The players' data (32 of 0x1180 bytes at [ENG 00AD4448], the hero's by his id's low word): the art slots (keys
    // 6-0) as 20-byte entries at +0xC90, two per slot (the second for the alternate bar, +0x1162 bit 1): u16 kind (2
    // spell, 3 weapon art, 4 combo), u16, u32 data[4] (the art, a combo's 0x200 + index).
    constexpr uintptr_t kPlayers = 0xAD4448;
    constexpr size_t kPlayerSize = 0x1180, kArtEntries = 0xC90, kArtEntrySize = 20, kAltBar = 0x1162;
    // The taskbar's slot count shown (more come with the levels, at most 5).
    constexpr size_t kTaskbarSlots = 0x15C;

    // The learned arts (stats +0x102 begin, +0x106 end; 22 bytes each: u32 kind 1 spell / 2 weapon art, u16 id, u16
    // level) and the combos (stats +0x10E: 4 records of 38 bytes, u16 id 0x200 + i, u32 arts[4]).
    struct Candidate
    {
        uint16_t kind = 0;
        uint32_t data = 0;
        int worth = 0;
    };

    // One art not yet on the bar into the first free slot the hero has: the combos first (several arts at once), then
    // the arts by their level. A slot the player filled stays as it is. True when something was placed.
    bool fillBar(cCreature* hero, uint8_t* st)
    {
        cUI_Manager* manager = cUI_Manager::instance();
        cUI_Taskbar2* taskbar = manager ? manager->taskbar() : nullptr;
        const uint8_t* players = *reinterpret_cast<uint8_t* const*>(kPlayers);
        const uint32_t index = hero->id & 0xFFFF;
        if (!taskbar || !players || index >= 0x20)
        {
            return false;
        }
        const int shown = std::clamp<int>(*reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(taskbar) +
            kTaskbarSlots), 0, cUI_Taskbar2::slotCount);
        uint8_t* player = const_cast<uint8_t*>(players) + index * kPlayerSize;
        const int alt = (player[kAltBar] & 2) ? 1 : 0;
        auto entry = [&](int slot) { return player + kArtEntries + static_cast<size_t>(slot * 2 + alt) * kArtEntrySize; };
        int freeSlot = -1;
        for (int slot = 0; slot < shown && freeSlot < 0; ++slot)
        {
            if (!*reinterpret_cast<const uint16_t*>(entry(slot)))
            {
                freeSlot = slot;
            }
        }
        if (freeSlot < 0)
        {
            return false;
        }
        auto onBar = [&](uint32_t data) {
            for (int slot = 0; slot < cUI_Taskbar2::slotCount; ++slot)
            {
                const uint8_t* e = entry(slot);
                if (*reinterpret_cast<const uint16_t*>(e) && *reinterpret_cast<const uint32_t*>(e + 4) == data)
                {
                    return true;
                }
            }
            return false;
        };
        Candidate best;
        const uint8_t* combos = *reinterpret_cast<uint8_t* const*>(st + 0x10E);
        for (int i = 0; combos && i < 4; ++i)
        {
            const uint8_t* r = combos + i * 38;
            const uint16_t id = *reinterpret_cast<const uint16_t*>(r);
            if (id >= 0x200 && id < 0x204 && *reinterpret_cast<const uint32_t*>(r + 2) && !onBar(id))
            {
                best = {4, id, 1000 - i};
                break;
            }
        }
        const uint8_t* begin = *reinterpret_cast<uint8_t* const*>(st + 0x102);
        const uint8_t* end = *reinterpret_cast<uint8_t* const*>(st + 0x106);
        const bool combo = best.kind == 4;
        for (const uint8_t* a = begin; !combo && begin && end > begin && a + 22 <= end; a += 22)
        {
            const uint32_t kind = *reinterpret_cast<const uint32_t*>(a);
            const uint16_t id = *reinterpret_cast<const uint16_t*>(a + 4);
            const int level = *reinterpret_cast<const uint16_t*>(a + 6);
            if ((kind == 1 || kind == 2) && id && !onBar(id) && level > best.worth)
            {
                best = {static_cast<uint16_t>(kind + 1), id, level};
            }
        }
        if (!best.kind)
        {
            return false;
        }
        const uint32_t data[4] = {best.data, 0, 0, 0};
        LOG("HeroBuild: {} {:x} onto taskbar slot {} ({} shown)", best.kind == 4 ? "combo" : best.kind == 2 ? "spell" : "weapon art",
            best.data, freeSlot + 1, shown);
        reinterpret_cast<SetSlot>(g_setSlot)(player, nullptr, static_cast<uint32_t>(freeSlot), best.kind, 0, data);
        return true;
    }

    // The bar for the situation (docs/Artes-de-la-barra.md; the player, 2026-11-14: «no me cambió las artes equipadas
    // en ningún momento, aun teniendo todas desbloqueadas»): fillBar only fills empty slots; this swaps one art at a
    // time for one worth more in what the hero meets now: a lone enemy at a time, groups, or a strong one (a boss, a
    // champion); on horseback only what the game lets be cast there. Out of fights only (onFrame's calm), one slot a
    // look, the situation steady 8 s, a slot left 60 s after a change, five changes a minute at most, the new one worth
    // a quarter more than the one it replaces. A slot set in [Tactics] (not Auto), or changed by hand (10 min), stays.
    enum class Sit
    {
        Single,
        Group,
        Strong,
    };
    const char* const kSitNames[] = {"one at a time", "groups", "a strong enemy"};
    // By the situation, what each use is worth: not known yet, heal, area, single, buff, off (Config::Tactics::Art).
    constexpr float kUseWorth[3][6] = {
        {0.7f, 0.8f, 0.4f, 1.0f, 0.6f, 0.0f},
        {0.7f, 0.8f, 1.2f, 0.5f, 0.6f, 0.0f},
        {0.7f, 1.0f, 0.3f, 1.2f, 1.0f, 0.0f},
    };
    constexpr DWORD kSitSteadyMs = 8000, kSlotRestMs = 60000, kPlayerHoldMs = 600000;
    constexpr int kSwapsPerMinute = 5;
    constexpr float kSwapMargin = 1.25f;
    struct BarState
    {
        Sit sit = Sit::Single, pending = Sit::Single;
        DWORD pendingSince = 0;
        uint32_t placed[cUI_Taskbar2::slotCount] = {};      // what this put there (0: not this)
        DWORD changedAt[cUI_Taskbar2::slotCount] = {};
        DWORD playerUntil[cUI_Taskbar2::slotCount] = {};    // changed by hand: left alone until then
        DWORD swaps[kSwapsPerMinute] = {};
        int nextSwap = 0;
    };
    BarState g_bar;

    float artWorth(const Tactics::ArtFact& f, Sit sit, bool mounted)
    {
        if ((mounted && !f.mounted) || f.use < 0 || f.use > 5)
        {
            return 0.0f;
        }
        float w = kUseWorth[static_cast<int>(sit)][f.use];
        // The class's panic art and opening buff stay in the bar in every situation (an escape is «off» as a blow,
        // worth 0 otherwise, and the panic would find it missing).
        // The Vampiress's turns (0x3FE into the vampire, 0x3FF back; ids unique across classes) too: without 0x3FF
        // she would stay a vampire, her runes refused.
        if (f.art && (f.art == Mind::now().panicArt || Mind::opens(Mind::now(), f.art) || f.art == 0x3FE ||
                      f.art == 0x3FF))
        {
            w = std::max(w, 1.5f);
        }
        w *= 1.0f + static_cast<float>(std::min(f.level, 30)) / 10.0f;
        if (f.slotKind == 4)
        {
            w *= 1.3f;      // a combo: several arts in one cast
        }
        if (f.rechargeMs)
        {
            w *= std::clamp(8000.0f / (static_cast<float>(f.rechargeMs) + 2000.0f), 0.3f, 1.5f);
        }
        if (f.use == 0 && f.tries >= 3)
        {
            w *= 0.5f;      // tried and still unclear
        }
        return w;
    }

    bool arrangeBar(cCreature* hero)
    {
        std::vector<Tactics::ArtFact> facts;
        if (!Tactics::artFacts(facts) || facts.empty())
        {
            return false;
        }
        cUI_Manager* manager = cUI_Manager::instance();
        cUI_Taskbar2* taskbar = manager ? manager->taskbar() : nullptr;
        const uint8_t* players = *reinterpret_cast<uint8_t* const*>(kPlayers);
        const uint32_t index = hero->id & 0xFFFF;
        if (!taskbar || !players || index >= 0x20)
        {
            return false;
        }
        const int shown = std::clamp<int>(*reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(taskbar) +
            kTaskbarSlots), 0, cUI_Taskbar2::slotCount);
        uint8_t* player = const_cast<uint8_t*>(players) + index * kPlayerSize;
        const int alt = (player[kAltBar] & 2) ? 1 : 0;
        auto entry = [&](int slot) { return player + kArtEntries + static_cast<size_t>(slot * 2 + alt) * kArtEntrySize; };
        const DWORD now = GetTickCount();
        const bool mounted = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(hero) + 0x1EC) != 0;

        // The situation: the last fights, and the enemies in sight now; steady 8 s before it counts.
        float foes = 0.0f, strong = 0.0f;
        CombatLearn::recentFights(foes, strong);
        const float around = std::max(foes, static_cast<float>(CombatLearn::forecast().foes));
        const Sit seen = strong >= 0.3f ? Sit::Strong : around >= 3.0f ? Sit::Group : Sit::Single;
        if (seen != g_bar.pending)
        {
            g_bar.pending = seen;
            g_bar.pendingSince = now;
        }
        if (g_bar.pending != g_bar.sit && now - g_bar.pendingSince >= kSitSteadyMs)
        {
            g_bar.sit = g_bar.pending;
            LOG("HeroBuild: the situation is now {} ({:.1f} enemies a fight, {:.0f}% with a strong one)",
                kSitNames[static_cast<int>(g_bar.sit)], foes, strong * 100.0f);
        }
        const Sit sit = g_bar.sit;

        std::vector<float> worth(facts.size());
        for (size_t i = 0; i < facts.size(); ++i)
        {
            worth[i] = artWorth(facts[i], sit, mounted);
        }
        // What the bar holds and what each is worth; a slot no longer holding what this put there was changed by hand.
        uint32_t held[cUI_Taskbar2::slotCount] = {};
        float heldWorth[cUI_Taskbar2::slotCount] = {};
        for (int slot = 0; slot < shown; ++slot)
        {
            const uint8_t* e = entry(slot);
            held[slot] = *reinterpret_cast<const uint16_t*>(e) ? *reinterpret_cast<const uint32_t*>(e + 4) : 0;
            if (g_bar.placed[slot] && held[slot] != g_bar.placed[slot])
            {
                g_bar.placed[slot] = 0;
                g_bar.playerUntil[slot] = now + kPlayerHoldMs;
                LOG("HeroBuild: art bar slot {} changed by hand: left alone 10 min", slot + 1);
            }
            for (size_t i = 0; i < facts.size(); ++i)
            {
                if (held[slot] && facts[i].art == held[slot])
                {
                    heldWorth[slot] = worth[i];
                }
            }
        }
        // The arts wanted: the best as many as the slots shown, each further one of a use already taken worth less (a
        // bar of five area arts recharges all at once), a second heal hardly anything.
        std::vector<size_t> wanted;
        int byUse[6] = {};
        for (int k = 0; k < shown; ++k)
        {
            size_t best = facts.size();
            float bestWorth = 0.0f;
            for (size_t i = 0; i < facts.size(); ++i)
            {
                if (std::find(wanted.begin(), wanted.end(), i) != wanted.end())
                {
                    continue;
                }
                const int use = std::clamp(facts[i].use, 0, 5);
                float w = worth[i];
                for (int n = 0; n < byUse[use]; ++n)
                {
                    w *= use == 1 ? 0.2f : 0.6f;
                }
                if (w > bestWorth)
                {
                    best = i;
                    bestWorth = w;
                }
            }
            if (best == facts.size())
            {
                break;
            }
            wanted.push_back(best);
            ++byUse[std::clamp(facts[best].use, 0, 5)];
        }
        auto onBar = [&](uint32_t art) {
            for (int slot = 0; slot < shown; ++slot)
            {
                if (held[slot] == art)
                {
                    return true;
                }
            }
            return false;
        };
        auto isWanted = [&](uint32_t art) {
            for (size_t i : wanted)
            {
                if (facts[i].art == art)
                {
                    return true;
                }
            }
            return false;
        };
        // The one wanted most that is not on the bar (wanted is in order), and the weakest slot it may take.
        size_t add = facts.size();
        for (size_t i : wanted)
        {
            if (!onBar(facts[i].art))
            {
                add = i;
                break;
            }
        }
        if (add == facts.size())
        {
            return false;
        }
        int out = -1;
        for (int slot = 0; slot < shown; ++slot)
        {
            if (Config::tactics.slots[slot] != Config::Tactics::Art::Auto ||
                (g_bar.playerUntil[slot] && static_cast<int>(now - g_bar.playerUntil[slot]) < 0) ||
                (g_bar.changedAt[slot] && now - g_bar.changedAt[slot] < kSlotRestMs) || (held[slot] && isWanted(held[slot])))
            {
                continue;
            }
            if (out < 0 || heldWorth[slot] < heldWorth[out])
            {
                out = slot;
            }
        }
        if (out < 0 || (heldWorth[out] > 0.0f && worth[add] < kSwapMargin * heldWorth[out]))
        {
            return false;
        }
        if (const DWORD oldest = g_bar.swaps[g_bar.nextSwap]; oldest && now - oldest < 60000)
        {
            return false;   // five changes in the last minute
        }
        if (!Act::allowed(Act::Kind::ArtBar, "swap an art on the bar for what the hero meets"))
        {
            return false;
        }
        const Tactics::ArtFact& f = facts[add];
        LOG("HeroBuild: art bar for {}{}: slot {} {:x} (worth {:.2f}) -> {} {:x} ({:.2f})", kSitNames[static_cast<int>(sit)],
            mounted ? ", mounted" : "", out + 1, held[out], heldWorth[out],
            f.slotKind == 4 ? "combo" : f.slotKind == 2 ? "spell" : "weapon art", f.art, worth[add]);
        const uint32_t data[4] = {f.art, 0, 0, 0};
        reinterpret_cast<SetSlot>(g_setSlot)(player, nullptr, static_cast<uint32_t>(out), f.slotKind, 0, data);
        g_bar.placed[out] = f.art;
        g_bar.changedAt[out] = now;
        g_bar.swaps[g_bar.nextSwap] = now;
        g_bar.nextSwap = (g_bar.nextSwap + 1) % kSwapsPerMinute;
        return true;
    }

    // The skill names (ENG 008EBA20: records of 0x44 bytes, u32 id and char[64]), for the plan below.
    const char* skillName(uint8_t id)
    {
        const auto* table = reinterpret_cast<const uint8_t*>(0x8EBA20);
        for (int i = 0; i < 64; ++i)
        {
            const uint8_t* r = table + i * 0x44;
            if (*reinterpret_cast<const uint32_t*>(r) == id)
            {
                return reinterpret_cast<const char*>(r + 4);
            }
        }
        return "";
    }

    bool named(uint8_t id, const char* part)
    {
        return std::strstr(skillName(id), part) != nullptr;
    }

    // The skills each class leans on, first the most (Manolito's table, docs/AGENT_ADDRESSES.md «Clases del héroe»;
    // ids are the game's SKILL_* numbers). 0 ends a list.
    // From the expert builds (docs/Builds.md §10.2, the player, 2026-11-13: «que el agente se comporte tal cual
    // recomiendan las guías»): constitution and armour early for every class, the battle mage on two schools only
    // (fire and earth), the seraphim's constitution at level 3 and her school at 6 (Builds.md §2).
    const uint8_t kSkillPlan[10][7] = {
        {0},
        {10, 1, 14, 12, 25, 2, 11},     // seraphim: constitution, heavenly magic, magic lore, meditation
        {2, 25, 10, 11, 8, 9, 4},       // gladiator: weapon lore, concentration, constitution, armour, agility, parry
        {14, 12, 15, 10, 17, 8, 9},     // battle mage: magic lore, meditation, fire, constitution, earth
        {2, 25, 6, 27, 10, 11, 26},     // dark elf: D1's universalist, dual wield, trap lore, constitution, armour, ballistics
        {7, 2, 10, 11, 25, 19, 8},      // wood elf: long range, weapon lore, constitution, armour
        {2, 20, 10, 25, 11, 28, 5},     // vampiress: weapon lore, vampirism, constitution
        {2, 20, 10, 25, 11, 28, 5},
        {2, 29, 31, 25, 11, 9, 10},     // dwarf: weapon technique, dwarven lore, armour, parry
        {2, 10, 25, 8, 11, 9, 32},      // daemon: the physical build
    };
    uint32_t g_class = 0;   // the hero's, set before the picks below

    // How much a skill is worth to the agent: its class's plan first, trade and riding last, the rest alike.
    float skillWeight(uint8_t id)
    {
        if (g_class < 10)
        {
            for (int i = 0; i < 7 && kSkillPlan[g_class][i]; ++i)
            {
                if (kSkillPlan[g_class][i] == id)
                {
                    return 3.0f - 0.25f * static_cast<float>(i);
                }
            }
        }
        if (named(id, "Handel") || named(id, "Reiten"))
        {
            return 0.25f;
        }
        if (named(id, "Konstitution") || named(id, "Konzentration") || named(id, "kunde") || named(id, "Kunde"))
        {
            return 1.5f;
        }
        return 1.0f;
    }

    // Attributes by the class (order of the character window, global.res 71321354..59: strength, endurance,
    // dexterity, physical regeneration, mental regeneration, charisma; no intelligence in Sacred): how much of each,
    // relative. Charisma only lowers prices and adds poison damage (GAME_GUIDE): the dark elf's poison only. Mental
    // regeneration recharges the arts. 1 seraphim, 2 gladiator, 3 battle mage, 4 dark elf, 5 wood elf, 6/7 vampiress,
    // 8 dwarf, 9 daemon.
    // Builds.md §10.1: physical regeneration 3-4 for the melee classes (shorter art recharge, more health).
    const int kAttrPlan[10][6] = {
        {2, 3, 2, 1, 1, 0},     // unknown
        {2, 2, 1, 3, 3, 0},     // seraphim
        {3, 2, 1, 3, 0, 0},     // gladiator
        {0, 2, 1, 1, 5, 0},     // battle mage
        {0, 0, 0, 1, 0, 0},     // dark elf: all to physical regeneration (D1: both timers, moves and traps, run on it)
        {0, 2, 3, 3, 0, 0},     // wood elf
        {4, 2, 1, 2, 0, 0},     // vampiress
        {4, 2, 1, 2, 0, 0},     // vampiress (other form)
        {3, 2, 1, 3, 0, 0},     // dwarf
        {3, 2, 1, 3, 0, 0},     // daemon
    };

    std::atomic<bool> g_working{false};

    void work(void*)
    {
        // Whatever way work() leaves, the next one may start.
        struct Done
        {
            ~Done() { g_working = false; }
        } done;
        cObjectManager* objects = cObjectManager::instance();
        cCreature* hero = objects ? objects->hero() : nullptr;
        if (!hero || hero->health <= 0)
        {
            return;
        }
        uint8_t* st = reinterpret_cast<uint8_t*>(hero) + kStats;
        const uint32_t cls = *reinterpret_cast<const uint32_t*>(st + 8);
        g_class = cls;
        const int level = *reinterpret_cast<const uint16_t*>(st + 0x56);
        // One point a look, as a player clicking "+".
        if (g_settings.attributes && st[0x23] > 0 && Act::allowed(Act::Kind::Attributes, "spend an attribute point"))
        {
            const int* plan = kAttrPlan[cls < 10 ? cls : 0];
            int best = -1;
            float bestShare = 0.0f;
            for (int i = 0; i < 6; ++i)
            {
                if (plan[i] <= 0)
                {
                    continue;
                }
                const float share = static_cast<float>(st[0x1C + i]) / static_cast<float>(plan[i]);
                if (best < 0 || share < bestShare)
                {
                    best = i;
                    bestShare = share;
                }
            }
            if (best >= 0)
            {
                LOG("HeroBuild: attribute point to attribute {} (had {}, {} left)", best, st[0x1C + best], st[0x23] - 1);
                reinterpret_cast<SpendAttr>(g_spendAttr)(st, nullptr, static_cast<uint32_t>(best));
            }
        }
        else if (g_settings.learn && *reinterpret_cast<const uint16_t*>(st + 0x44) > 0 &&
                 Act::allowed(Act::Kind::Skills, "learn a new skill"))
        {
            uint8_t offered[8] = {};
            const int count = std::clamp(reinterpret_cast<Learnable>(g_learnable)(st, nullptr, offered), 0, 8);
            int pick = -1;
            for (int i = 0; i < count; ++i)
            {
                if (offered[i] && (pick < 0 || skillWeight(offered[i]) > skillWeight(offered[pick])))
                {
                    pick = i;
                }
            }
            if (pick >= 0)
            {
                LOG("HeroBuild: new skill {} ({})", offered[pick], skillName(offered[pick]));
                reinterpret_cast<LearnSkill>(g_learnSkill)(st, nullptr, offered[pick]);
            }
        }
        else if (g_settings.bar && g_setSlot && Act::may(Act::Kind::ArtBar) && fillBar(hero, st))
        {
        }
        else if (g_settings.bar && g_settings.swap && !g_settings.presets && g_setSlot && Act::may(Act::Kind::ArtBar) &&
                 arrangeBar(hero))
        {
        }
        else if (g_settings.skills && *reinterpret_cast<const uint16_t*>(st + 0x42) > 0 &&
                 Act::allowed(Act::Kind::Skills, "spend a skill point"))
        {
            // The skill furthest behind for its worth, never above the hero's level (the window's own limit).
            int best = -1;
            float bestShare = 0.0f;
            for (int slot = 0; slot < 8; ++slot)
            {
                const uint8_t id = st[0x24 + slot];
                const int base = st[0x2C + slot];
                if (!id || base >= level || base >= 255)
                {
                    continue;
                }
                // Weapon Lore past 12 is worth little (attack speed tops at 220; Builds.md §0.3 and §10.3): its points
                // go to constitution and armour.
                const float weight = id == 2 && base >= 12 ? 0.5f : skillWeight(id);
                const float share = static_cast<float>(base) / weight;
                if (best < 0 || share < bestShare)
                {
                    best = slot;
                    bestShare = share;
                }
            }
            if (best >= 0)
            {
                LOG("HeroBuild: skill point to {} ({}) level {}", st[0x24 + best], skillName(st[0x24 + best]),
                    st[0x2C + best] + 1);
                reinterpret_cast<AddSkillLevel>(g_addSkillLevel)(st, nullptr, static_cast<uint32_t>(best), 1, true);
            }
        }
        g_working = false;
    }

    // Art presets by weapon slot (the player, 2026-11-16: «las ranuras de armas quiero que carguen presets [...] cada
    // ranura quiero que contenga un preset de artes [...] diversificar el combate y ampliar las tácticas»). Each weapon
    // slot (the player's data +0xD58; 1-5 by level, the same table as the art slots) remembers the art bar it last had;
    // a switch to another slot puts that slot's bar back through the taskbar's own setter, as a drop. A hero with n
    // weapon slots has n presets. Only learned arts and combos are put back; nothing while an art is being cast.
    constexpr size_t kActiveWeaponSlot = 0xD58;
    constexpr int kPresetSlots = 5;
    struct Preset
    {
        bool set = false;
        bool agent = false;         // composed by the agent (no bar the player made): «SlotNBy=agent» in the file
        int weapons = 0;            // an agent's preset: the slot's weapons it was composed for (Gear::slotWeapons)
        uint8_t entries[cUI_Taskbar2::slotCount][kArtEntrySize] = {};
    };
    struct PresetState
    {
        uint32_t heroId = 0, cls = 0;
        int lastSlot = -1;          // the weapon slot the bar belongs to (-1: not sampled since the load)
        int pendingSlot = -1;       // a switch waiting for the cast to end
        Preset presets[kPresetSlots];
        DWORD dirtyAt = 0;          // the file written 5 s after the last change
        DWORD restores[8] = {};
        int nextRestore = 0;
        DWORD composeAt = 0;        // the slots without a preset (or the agent's, their weapons changed) looked at
        bool toldNoCompose = false;
    };
    PresetState g_presets;
    std::atomic<bool> g_presetsWorking{false};
    // The presets' arts for Gear's weapon slot choice (another thread): a copy, written after each pass.
    struct PublishedPreset
    {
        bool set = false;
        uint32_t arts[cUI_Taskbar2::slotCount] = {};
    };
    std::mutex g_publishedLock;
    PublishedPreset g_published[kPresetSlots];
    static_assert(cUI_Taskbar2::slotCount == 5, "presetArts hands out 5 art slots");
    // The taskbar's art slot selection, as the keys 6-0 (Manolito, block BF: ENG 006E1A20, thiscall(taskbar, u16 slot,
    // u32 net), ret 8). After the bar is written the selected slot is selected again, or the engine's right click
    // (+0x58/+0x5A) keeps the art that was there before (the game's own UI does the same after a drop). Resolved on its
    // own: a miss turns off only this.
    uintptr_t g_selectArtSlot = 0;
    const Sig::Entry kPresetSigs[] = {
        {"selectArtSlot", "53 56 8B F1 57 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 74 2D 66 8B 88 FE 03 00 00 51 8D 88 A8 03 00 00",
            0, Sig::Take::Match, &g_selectArtSlot},
    };
    using SelectArtSlot = void(__fastcall*)(void* taskbar, void*, uint16_t slot, uint32_t net);
    constexpr size_t kSelectedArtSlot = 0xD5A;

    std::wstring presetsPath()
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring path = exe;
        return path.substr(0, path.find_last_of(L"\\/") + 1) + L"SacredBild-presets.ini";
    }

    // One file beside the game, a section per class (one hero of each class per folder, as the player and the copies
    // play): Slot1..Slot5 = the bar's 20-byte entries in hex.
    void loadPresets(uint32_t cls)
    {
        const std::wstring path = presetsPath(), section = L"Class" + std::to_wstring(cls);
        for (int w = 0; w < kPresetSlots; ++w)
        {
            Preset& p = g_presets.presets[w];
            p = Preset{};
            wchar_t buf[512] = {};
            GetPrivateProfileStringW(section.c_str(), (L"Slot" + std::to_wstring(w + 1)).c_str(), L"", buf, 512, path.c_str());
            if (std::wcslen(buf) != sizeof(p.entries) * 2)
            {
                continue;
            }
            uint8_t* out = &p.entries[0][0];
            for (size_t i = 0; i < sizeof(p.entries); ++i)
            {
                const wchar_t two[3] = {buf[i * 2], buf[i * 2 + 1], 0};
                out[i] = static_cast<uint8_t>(std::wcstoul(two, nullptr, 16));
            }
            p.set = true;
            GetPrivateProfileStringW(section.c_str(), (L"Slot" + std::to_wstring(w + 1) + L"By").c_str(), L"", buf, 512,
                                     path.c_str());
            p.agent = std::wcscmp(buf, L"agent") == 0;
            p.weapons = p.agent ? static_cast<int>(GetPrivateProfileIntW(section.c_str(),
                                      (L"Slot" + std::to_wstring(w + 1) + L"Weapons").c_str(), 0, path.c_str())) : 0;
        }
    }

    void savePresets()
    {
        const std::wstring path = presetsPath(), section = L"Class" + std::to_wstring(g_presets.cls);
        for (int w = 0; w < kPresetSlots; ++w)
        {
            const Preset& p = g_presets.presets[w];
            if (!p.set)
            {
                continue;
            }
            std::wstring hex;
            const uint8_t* in = &p.entries[0][0];
            for (size_t i = 0; i < sizeof(p.entries); ++i)
            {
                wchar_t two[3] = {};
                swprintf(two, 3, L"%02X", in[i]);
                hex += two;
            }
            WritePrivateProfileStringW(section.c_str(), (L"Slot" + std::to_wstring(w + 1)).c_str(), hex.c_str(), path.c_str());
            // The agent's mark; a bar the player reworked loses it (nullptr: the key removed).
            WritePrivateProfileStringW(section.c_str(), (L"Slot" + std::to_wstring(w + 1) + L"By").c_str(),
                                       p.agent ? L"agent" : nullptr, path.c_str());
            WritePrivateProfileStringW(section.c_str(), (L"Slot" + std::to_wstring(w + 1) + L"Weapons").c_str(),
                                       p.agent ? std::to_wstring(p.weapons).c_str() : nullptr, path.c_str());
        }
    }

    // A preset for each weapon slot that has none (the player, 2026-11-16: «diversificar el combate y ampliar las
    // tácticas»): a bar of learned arts only, with a leaning by slot (the first one without a preset leans to the area
    // arts, the next to the single blows, and so on), a heal and a lasting effect in each. Data only: the bar changes
    // when the hero takes that weapon slot, as with the player's own presets. A slot with a preset is never touched.
    // `artSlots`: the art slots shown; `active`: the weapon slot in use (its bar is its preset already). False while
    // the weapon slots are not known yet (looked at again later).
    bool composePresets(int artSlots, int active, const std::vector<Tactics::ArtFact>& facts)
    {
        // The weapon slots the level opens, and theirs weapons known (the reviewer, P2: not before, or the arrows and
        // Recoil were left out for good).
        const int weaponSlots = Gear::weaponSlotsOpen();
        if (weaponSlots <= 0 || Gear::slotWeapons(0) == 0)
        {
            return false;
        }
        using Use = Config::Tactics::Art;
        const auto of = [&](Use use) {
            std::vector<const Tactics::ArtFact*> list;
            for (const Tactics::ArtFact& f : facts)
            {
                if (f.use == static_cast<int>(use) && f.slotKind)
                {
                    list.push_back(&f);
                }
            }
            std::sort(list.begin(), list.end(), [](auto* a, auto* b) { return a->level > b->level; });
            return list;
        };
        const auto area = of(Use::Area), single = of(Use::Single), heal = of(Use::Heal), buff = of(Use::Buff),
                   unknown = of(Use::Auto);
        // Two leanings are only worth it with arts of both kinds.
        if (area.empty() || single.empty())
        {
            if (!g_presets.toldNoCompose)
            {
                g_presets.toldNoCompose = true;
                LOG("ArtPresets: no preset composed ({} area arts, {} single blows known)", area.size(), single.size());
            }
            return true;
        }
        const int open = artSlots;
        int lean = 0;
        for (int w = 0; w < weaponSlots && w < kPresetSlots; ++w)
        {
            Preset& p = g_presets.presets[w];
            const int weapons = Gear::slotWeapons(w);   // the arrows only with a bow, Recoil only two-handed
            const bool leaning = !p.set || p.agent;
            if (!leaning)
            {
                continue;
            }
            const bool group = lean++ % 2 == 0;
            // A slot without a weapon, the one in use, or an agent's preset still made for these weapons: as it is.
            if (!(weapons & 8) || w == active || (p.set && p.weapons == weapons))
            {
                continue;
            }
            std::vector<const Tactics::ArtFact*> pick;
            const auto add = [&](const std::vector<const Tactics::ArtFact*>& from, size_t most) {
                for (const Tactics::ArtFact* f : from)
                {
                    if (most == 0 || static_cast<int>(pick.size()) >= open)
                    {
                        return;
                    }
                    if (Gear::artFitsWeapons(f->art, weapons) && std::none_of(pick.begin(), pick.end(), [&](auto* q) { return q->art == f->art; }))
                    {
                        pick.push_back(f);
                        --most;
                    }
                }
            };
            add(group ? area : single, 2);
            add(heal, 1);
            add(buff, 1);
            add(group ? area : single, 5);
            add(group ? single : area, 5);
            add(unknown, 5);
            p = Preset{};
            std::string names;
            for (size_t slot = 0; slot < pick.size(); ++slot)
            {
                uint8_t* e = p.entries[slot];
                *reinterpret_cast<uint16_t*>(e) = pick[slot]->slotKind;
                *reinterpret_cast<uint32_t*>(e + 4) = pick[slot]->art;
                names += std::format("{:x} ", pick[slot]->art);
            }
            p.set = true;
            p.agent = true;
            p.weapons = weapons;
            g_presets.dirtyAt = GetTickCount() | 1;
            LOG("ArtPresets: weapon slot {} given a preset leaning to {} (weapons {:x}): {}", w + 1,
                group ? "groups (area arts)" : "single enemies (single blows)", weapons, names);
        }
        return true;
    }

    // The same art in an entry (its kind and id; the game's other fields left aside).
    bool sameArt(const uint8_t* a, const uint8_t* b)
    {
        return *reinterpret_cast<const uint16_t*>(a) == *reinterpret_cast<const uint16_t*>(b) &&
               (!*reinterpret_cast<const uint16_t*>(a) ||
                *reinterpret_cast<const uint32_t*>(a + 4) == *reinterpret_cast<const uint32_t*>(b + 4));
    }

    void publishPresets()
    {
        std::lock_guard<std::mutex> lock(g_publishedLock);
        for (int w = 0; w < kPresetSlots; ++w)
        {
            g_published[w].set = g_presets.presets[w].set;
            for (int slot = 0; slot < cUI_Taskbar2::slotCount; ++slot)
            {
                const uint8_t* e = g_presets.presets[w].entries[slot];
                g_published[w].arts[slot] = *reinterpret_cast<const uint16_t*>(e) ? *reinterpret_cast<const uint32_t*>(e + 4) : 0;
            }
        }
    }

    // On the engine's thread, as the taskbar's own drops.
    void presetsWork(void*)
    {
        struct Done
        {
            ~Done() { g_presetsWorking = false; }
        } done;
        cObjectManager* objects = cObjectManager::instance();
        cCreature* hero = objects ? objects->hero() : nullptr;
        cUI_Manager* manager = cUI_Manager::instance();
        cUI_Taskbar2* taskbar = manager ? manager->taskbar() : nullptr;
        uint8_t* players = *reinterpret_cast<uint8_t* const*>(kPlayers);
        const uint32_t index = hero ? hero->id & 0xFFFF : 0x20;
        if (!hero || hero->health <= 0 || !taskbar || !players || index >= 0x20)
        {
            return;
        }
        const DWORD now = GetTickCount();
        const uint32_t cls = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(hero) + kStats + 8);
        // Another hero (a load, a new game): its class's presets from the file, the bar only sampled at first.
        if (hero->id != g_presets.heroId || cls != g_presets.cls)
        {
            if (g_presets.dirtyAt)
            {
                savePresets();
            }
            g_presets = PresetState{};
            g_presets.heroId = hero->id;
            g_presets.cls = cls;
            loadPresets(cls);
            publishPresets();
        }
        uint8_t* player = players + index * kPlayerSize;
        const int shown = std::clamp<int>(*reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(taskbar) +
            kTaskbarSlots), 0, cUI_Taskbar2::slotCount);
        const int alt = (player[kAltBar] & 2) ? 1 : 0;
        auto entry = [&](int slot) { return player + kArtEntries + static_cast<size_t>(slot * 2 + alt) * kArtEntrySize; };
        const int weapon = *reinterpret_cast<const uint16_t*>(player + kActiveWeaponSlot);
        if (weapon < 0 || weapon >= kPresetSlots)
        {
            return;
        }
        auto snapshot = [&](int w) {
            Preset& p = g_presets.presets[w];
            bool changed = !p.set, otherArt = false;
            for (int slot = 0; slot < cUI_Taskbar2::slotCount; ++slot)
            {
                changed = changed || std::memcmp(p.entries[slot], entry(slot), kArtEntrySize) != 0;
                otherArt = otherArt || !sameArt(p.entries[slot], entry(slot));
                std::memcpy(p.entries[slot], entry(slot), kArtEntrySize);
            }
            if (otherArt && p.set)
            {
                p.agent = false;    // another art on the bar (the player's): no longer the agent's
            }
            p.set = true;
            if (changed)
            {
                g_presets.dirtyAt = now ? now : 1;
            }
        };
        if (g_presets.lastSlot < 0)
        {
            g_presets.lastSlot = weapon;
            if (!g_presets.presets[weapon].set)
            {
                snapshot(weapon);
            }
            std::string kept;
            for (int w = 0; w < kPresetSlots; ++w)
            {
                if (g_presets.presets[w].set)
                {
                    kept += std::to_string(w + 1);
                }
            }
            LOG("ArtPresets: weapon slot {} at the start; presets kept for slots {}", weapon + 1, kept.empty() ? "none" : kept);
            return;
        }
        if (weapon != g_presets.lastSlot || g_presets.pendingSlot >= 0)
        {
            // The bar so far is the slot left's (kept each pass below; not again while a switch waits).
            if (g_presets.pendingSlot < 0)
            {
                snapshot(g_presets.lastSlot);
            }
            const Preset& p = g_presets.presets[weapon];
            std::vector<Tactics::ArtFact> facts;
            const bool casting = static_cast<int>(CombatState::measuringUntil.load() - now) > 0;
            if (p.set && (casting || !Tactics::artFacts(facts)))
            {
                g_presets.pendingSlot = weapon;     // put back once the cast is over and the arts are known
                return;
            }
            g_presets.pendingSlot = -1;
            if (weapon == g_presets.lastSlot)
            {
                return;     // switched back before it was put back: the bar is still this slot's
            }
            if (p.set)
            {
                int put = 0, refused = 0, emptied = 0;
                for (int slot = 0; slot < shown; ++slot)
                {
                    const uint8_t* want = p.entries[slot];
                    if (std::memcmp(want, entry(slot), kArtEntrySize) == 0)
                    {
                        continue;
                    }
                    const uint16_t kind = *reinterpret_cast<const uint16_t*>(want);
                    const uint32_t art = *reinterpret_cast<const uint32_t*>(want + 4);
                    if (!kind)
                    {
                        // Empty in the preset: emptied as clearArtSlot (ENG 005D96C0) leaves it; the same memory in a
                        // game of one (Manolito, block BF; online it would send «put» instead of «take»).
                        const uint32_t none[4] = {};
                        reinterpret_cast<SetSlot>(g_setSlot)(player, nullptr, static_cast<uint32_t>(slot), 0, 0, none);
                        g_bar.placed[slot] = 0;
                        g_bar.playerUntil[slot] = now + kPlayerHoldMs;
                        ++emptied;
                        continue;
                    }
                    const bool learned = std::any_of(facts.begin(), facts.end(),
                                                     [&](const Tactics::ArtFact& f) { return f.art == art; });
                    if (!learned)
                    {
                        ++refused;
                        continue;
                    }
                    reinterpret_cast<SetSlot>(g_setSlot)(player, nullptr, static_cast<uint32_t>(slot), kind,
                                                         *reinterpret_cast<const uint16_t*>(want + 2),
                                                         reinterpret_cast<const uint32_t*>(want + 4));
                    // The bar is the player's preset now: the situation swaps leave it alone.
                    g_bar.placed[slot] = 0;
                    g_bar.playerUntil[slot] = now + kPlayerHoldMs;
                    ++put;
                }
                // The selected slot selected again, as the game does after a drop: the right click casts what the bar
                // now shows.
                bool reselected = false;
                if ((put || emptied) && g_selectArtSlot)
                {
                    const uint16_t selected = *reinterpret_cast<const uint16_t*>(player + kSelectedArtSlot);
                    if (selected < shown)
                    {
                        reinterpret_cast<SelectArtSlot>(g_selectArtSlot)(taskbar, nullptr, selected, 1);
                        reselected = true;
                    }
                }
                const DWORD oldest = g_presets.restores[g_presets.nextRestore];
                g_presets.restores[g_presets.nextRestore] = now ? now : 1;
                g_presets.nextRestore = (g_presets.nextRestore + 1) % 8;
                LOG("ArtPresets: weapon slot {} -> {}: {} arts put back, {} emptied, {} not learned, art slot {} {}{}",
                    g_presets.lastSlot + 1, weapon + 1, put, emptied, refused,
                    *reinterpret_cast<const uint16_t*>(player + kSelectedArtSlot) + 1,
                    reselected ? "selected again" : "not selected again",
                    oldest && now - oldest < 60000 ? " (8 switches within a minute)" : "");
            }
            else
            {
                LOG("ArtPresets: weapon slot {} -> {}: no preset yet, this bar kept for it", g_presets.lastSlot + 1, weapon + 1);
                snapshot(weapon);
            }
            g_presets.lastSlot = weapon;
            return;
        }
        // The same slot: whatever the bar holds now is its preset.
        snapshot(weapon);
        if (shown > 1 && (!g_presets.composeAt || now - g_presets.composeAt > 10000))
        {
            std::vector<Tactics::ArtFact> facts;
            if (Tactics::artFacts(facts))
            {
                composePresets(shown, weapon, facts);   // not ready yet: looked at again in 10 s
                g_presets.composeAt = now | 1;
            }
        }
        publishPresets();
        if (g_presets.dirtyAt && now - g_presets.dirtyAt > 5000)
        {
            savePresets();
            g_presets.dirtyAt = 0;
        }
    }

    std::wstring iniPath()
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring path = exe;
        return path.substr(0, path.find_last_of(L"\\/") + 1) + L"SacredBild.ini";
    }

    void drawMenu()
    {
        if (!g_ready)
        {
            ImGui::TextDisabled("No disponible en esta versión del juego.");
            return;
        }
        ImGui::Checkbox("Repartir puntos de atributo (según la clase)", &g_settings.attributes);
        ImGui::SetItemTooltip("Con el botón «+» del juego, uno a uno: resistencia siempre, y la fuerza, destreza o "
                              "inteligencia que pide la clase.");
        ImGui::Checkbox("Subir habilidades", &g_settings.skills);
        ImGui::SetItemTooltip("Los puntos de habilidad a la más atrasada (comercio y montar las últimas), nunca por "
                              "encima del nivel del héroe.");
        ImGui::Checkbox("Aprender habilidades nuevas", &g_settings.learn);
        ImGui::SetItemTooltip("Al abrirse un hueco (niveles 3, 6, 12, 20, 30 y 50), elige entre las que ofrece la clase.");
        ImGui::Checkbox("Poner artes y combos en la barra", &g_settings.bar);
        ImGui::SetItemTooltip("En los huecos libres de la barra (teclas 6-0) que ya tiene el héroe: primero los combos, "
                              "luego las artes de más nivel. Lo que pusiste tú no se toca.");
        ImGui::Checkbox("Presets de artes por ranura de armas", &g_settings.presets);
        ImGui::SetItemTooltip("Cada ranura de armas (teclas 1-5) recuerda la barra de artes que tenía: al cambiar de "
                              "ranura vuelve la suya. Tantos presets como ranuras tenga el héroe. Solo artes aprendidas; "
                              "con esto encendido, el cambio de artes según la situación queda apagado.");
        ImGui::Checkbox("Cambiar las artes según la situación", &g_settings.swap);
        ImGui::SetItemTooltip("Con la barra llena, fuera de combate y de una en una: artes de área para grupos, de un "
                              "blanco para enemigos sueltos o fuertes, solo las que se pueden usar a caballo al ir "
                              "montado. Respeta las ranuras fijadas en Tácticas y las que cambies tú (10 min).");
    }

    void saveMenu(const std::wstring& ini)
    {
        ModkitMenu::write(ini, L"Build", L"Attributes", g_settings.attributes);
        ModkitMenu::write(ini, L"Build", L"Skills", g_settings.skills);
        ModkitMenu::write(ini, L"Build", L"Learn", g_settings.learn);
        ModkitMenu::write(ini, L"Build", L"Bar", g_settings.bar);
        ModkitMenu::write(ini, L"Build", L"SwapBar", g_settings.swap);
        ModkitMenu::write(ini, L"Build", L"ArtPresets", g_settings.presets);
    }

    void install()
    {
        const std::wstring ini = iniPath();
        g_settings.attributes = GetPrivateProfileIntW(L"Build", L"Attributes", 1, ini.c_str()) != 0;
        g_settings.skills = GetPrivateProfileIntW(L"Build", L"Skills", 1, ini.c_str()) != 0;
        g_settings.learn = GetPrivateProfileIntW(L"Build", L"Learn", 1, ini.c_str()) != 0;
        g_settings.bar = GetPrivateProfileIntW(L"Build", L"Bar", 1, ini.c_str()) != 0;
        g_settings.swap = GetPrivateProfileIntW(L"Build", L"SwapBar", 1, ini.c_str()) != 0;
        g_settings.presets = GetPrivateProfileIntW(L"Build", L"ArtPresets", 0, ini.c_str()) != 0;
        g_ready = Sig::resolve(kSigs) == 0;
        if (Sig::resolve(kPresetSigs) != 0)
        {
            g_selectArtSlot = 0;
        }
        LOG("HeroBuild: art presets {} (selectArtSlot {:x})", g_settings.presets ? "on" : "off", g_selectArtSlot);
        LOG("HeroBuild: {} (attributes {}, skills {}, learn {})", g_ready ? "ready" : "not available (signatures)",
            g_settings.attributes, g_settings.skills, g_settings.learn);
        ModkitMenu::add({"Progreso del héroe", &drawMenu, &saveMenu});
    }
}

bool HeroBuild::presetsOn()
{
    return g_ready && g_setSlot && g_settings.presets;
}

bool HeroBuild::presetArts(int weaponSlot, uint32_t out[5])
{
    if (weaponSlot < 0 || weaponSlot >= kPresetSlots)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_publishedLock);
    const PublishedPreset& p = g_published[weaponSlot];
    std::memcpy(out, p.arts, sizeof(p.arts));
    return p.set;
}

void HeroBuild::onFrame(DWORD now, bool calm)
{
    static std::once_flag once;
    std::call_once(once, install);
    // The presets every 100 ms, in a fight too: a switch of weapon slot brings its arts at once.
    static DWORD presetsAt = 0;
    if (g_ready && g_setSlot && g_settings.presets && !g_presetsWorking &&
        (!presetsAt || static_cast<int>(now - presetsAt) >= 0))
    {
        presetsAt = now + 100;
        g_presetsWorking = true;
        Inject::engineCall(&presetsWork, nullptr);
    }
    static DWORD nextAt = 0;
    if (!g_ready || !calm || (nextAt && static_cast<int>(now - nextAt) < 0) || g_working ||
        !(g_settings.attributes || g_settings.skills || g_settings.learn || g_settings.bar))
    {
        return;
    }
    nextAt = now + 2000;
    g_working = true;
    Inject::engineCall(&work, nullptr);
}
