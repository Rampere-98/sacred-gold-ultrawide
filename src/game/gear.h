#pragma once

#include <windows.h>

// The hero's gear, as a player tends it ([Gear], ModKit menu): out of a fight it puts on what in the backpack is
// better than what it wears (each equipment slot, by the item's weapon damage or armor, its value deciding the rest),
// and at a merchant it sells what it will not wear (worse than what it has for every slot it fits, or not for its
// class) and buys what would be better, keeping a share of its gold. All through the game's own functions (equip,
// the backpack, the price and the gold, which keeps its anti-cheat copies right), as the inventory and shop windows
// do. Quest items, and items that fit no equipment slot (potions, runes, gems...), are never sold.
namespace Gear
{
    // Every frame of the auto attack, while the game world runs; `calm`: no fight on and nothing being collected.
    void onFrame(DWORD now, bool calm);

    // Merchant trips ([Gear] Trips): the backpack is full, the agent should take the hero to a merchant.
    bool wantsShop();
    // The auto attack wants healing potions before more fights (no fight to take, under half the class's stock): a
    // merchant trip is wanted for them until `until` (tick), as with 2 left.
    void askPotions(unsigned long until);
    // Runes of other classes in the backpack, and whether to take them to a combo master: three or more and gold for
    // the swap (three of them make one of the hero's own class).
    int foreignRunes();
    bool wantsRuneSwap();
    bool wantsCombo();
    bool wantsSocket();
    bool wantsHorse();
    // A horse's worth, to compare two (Manolito, block H2b, seen live): its level (u16 +0x3FE, 1..25) first, its
    // speed (the stats block +0x3A8, u16 +0x40, 140..220) to break ties. 0 for none.
    inline int horseWorth(const void* creature)
    {
        if (!creature)
        {
            return 0;
        }
        const auto* c = static_cast<const unsigned char*>(creature);
        return *reinterpret_cast<const unsigned short*>(c + 0x3FE) * 1000 + *reinterpret_cast<const unsigned short*>(c + 0x3A8 + 0x40);
    }
    // A stable near offers a horse clearly better than the hero's (two levels or more): a trip to buy it.
    void setHorseUpgrade(bool better);         // no horse yet: one from a stable        // something to socket at a smith         // a combo to learn at a combo master

    // Healing potions in the backpack at the last look (-1 not counted yet).
    int healingLeft();
    // The backpack has no room left (whatever the trips setting): the agent leaves items lying.
    bool packFull();
    // The agent opened a shop for the trip: sold to, upgrades put on (whatever the menu says), then done.
    void tripOpened();
    // The trip's shop has nothing more (once): the agent closes it.
    bool tripDone();
    // True once after a trip that left the backpack full: that merchant did not deal in what the hero carries.
    bool tripLeftFull();
    bool shopOpen();
    // The stable's window open with no horse wanted: closed after 4 s (or at once with `now`); true when it closed it.
    bool closeStrayHorseWindow(bool now = false);
    // A combo master's window open (its runes' page shows the inventory beside it: not to be closed meanwhile).
    bool craftsmanOpen();

    // An item's grade as the game colours it (Manolito, AGENT_ADDRESSES «Color = rareza»: the nibble info+0x06 & 0xF,
    // <=4 white, 5..0xB blue, 0xC..0xE yellow, 0xF gold/unique; a set piece green): 0 white, 1 blue, 2 yellow,
    // 3 gold, 4 set. -1: no equipment (potions, runes, gold...) or not read.
    // equipment: the caller knows it is (by its class), so the grade is read without the damage/armour test (an
    // amulet has neither).
    int grade(uint32_t id, bool equipment = false);

    // A new enemy taken: the weapon slot that deals it the most (each slot's weapons by kind, the hero's multipliers,
    // the enemy's resistances), switched to as the keys 1-5 do when it beats the active one by 15 %. True if switched.
    // Call with the hero at rest (no art under way).
    bool chooseWeaponFor(uint32_t targetId);
    // The storage chest (the player, 2026-11-16: «el almacén [...] deberá contener equipamiento con resistencia a la
    // zona donde van [...] no dejes que el agente guarde armas»): pieces worth keeping for other zones to put in, or
    // one to take out for where the hero is going. The auto attack goes to a storage chest seen near for it.
    bool wantsStorage();
    // The agent used storage chest `id` (its window about to open): Gear works in it and closes it.
    void storageUsed(uint32_t id);
    // The agent's visit to the storage under way (its window open or about to): the inventory left open meanwhile.
    bool storageOpen();
    // Weapon slot `slot`'s weapons, for the arts that need a kind of weapon: bit 0 read, bit 1 a ranged weapon in it,
    // bit 2 a two-handed one, bit 3 any weapon. 0: not known yet (the weapon slots not ready).
    int slotWeapons(int slot);
    // The weapon slots the hero's level opens (1-5); 0 not known.
    int weaponSlotsOpen();
    // Whether art `art` works with a slot's weapons (slotWeapons): the Wood Elf's arrows (1004-1008) only with a bow
    // or crossbow, the executors skip them otherwise without a word; Recoil and Vehemence (1083, 1084) only with a
    // two-handed weapon (Manolito, block BG, static: moveInfo+0x74 bits 1 and 2). The others with any.
    inline bool artFitsWeapons(uint32_t art, int weapons)
    {
        if (art >= 1004 && art <= 1008)
        {
            return (weapons & 2) != 0;
        }
        if (art == 1083 || art == 1084)
        {
            return (weapons & 4) != 0;
        }
        return true;
    }
    // The active weapon slot's expected blow against creature `targetId` (its resistances and the chance to hit it
    // counted); -1 if unknown.
    float expectedHit(uint32_t targetId);
}
