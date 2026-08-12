#include "multiplayer_debug.h"

#include <algorithm>
#include <cctype>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "animation.h"
#include "art.h"
#include "color.h"
#include "combat.h"
#include "content_config.h"
#include "critter.h"
#include "debug.h"
#include "display_monitor.h"
#include "game.h"
#include "input.h"
#include "interface.h"
#include "item.h"
#include "kb.h"
#include "mouse.h"
#include "multiplayer.h"
#include "multiplayer_perf.h"
#include "multiplayer_profile.h"
#include "multiplayer_log.h"
#include "net.h"
#include "object.h"
#include "perk.h"
#include "platform_compat.h"
#include "proto.h"
#include "skill.h"
#include "sfall_script_hooks.h"
#include "stat.h"
#include "trait.h"
#include "svga.h"
#include "text_font.h"
#include "tile.h"
#include "window_manager.h"
#include "window_manager_private.h"

namespace fallout {

namespace {

// Button event codes. Picked above any KEY_* constants so they don't collide
// with real keyboard scans dispatched by inputGetInput().
constexpr int DBG_BTN_MONEY_1000 = 600;
constexpr int DBG_BTN_MONEY_100 = 601;
constexpr int DBG_BTN_HEAL_FULL = 602;
constexpr int DBG_BTN_HP_50 = 603;
constexpr int DBG_BTN_XP_1000 = 604;
constexpr int DBG_BTN_XP_5000 = 605;
constexpr int DBG_BTN_SP_10 = 606;
constexpr int DBG_BTN_LEVEL_1 = 607;
constexpr int DBG_BTN_SKILLS = 608;
constexpr int DBG_BTN_STATS = 609;
constexpr int DBG_BTN_PERKS = 610;
constexpr int DBG_BTN_CLOSE = 611;
constexpr int DBG_BTN_AP_REFILL = 612;
constexpr int DBG_BTN_FULL_RESTORE = 613;
constexpr int DBG_BTN_STIMPAK = 614;
constexpr int DBG_BTN_SUPER_STIMPAK = 615;
constexpr int DBG_BTN_RADAWAY = 616;
constexpr int DBG_BTN_AMMO = 617;
constexpr int DBG_BTN_MAX_LEVEL = 618;
constexpr int DBG_BTN_ITEMS = 619;
constexpr int DBG_BTN_CHEATS = 620;
constexpr int DBG_BTN_PREV = 700;
constexpr int DBG_BTN_NEXT = 701;
constexpr int DBG_BTN_DEC = 702;
constexpr int DBG_BTN_INC = 703;
constexpr int DBG_BTN_BACK = 706;
constexpr int DBG_BTN_CHEAT_GOD = 730;
constexpr int DBG_BTN_CHEAT_AP = 731;
constexpr int DBG_BTN_CHEAT_AMMO = 732;
constexpr int DBG_BTN_CHEAT_CARRY = 733;
constexpr int DBG_BTN_CHEAT_SKILLS = 734;
constexpr int DBG_BTN_CHEAT_ENCOUNTERS = 735;
constexpr int DBG_BTN_PERF_METER = 736;
constexpr int DBG_BTN_CHEAT_BACK = 737;
constexpr int DBG_BTN_CHEAT_INSTA_KILL = 738;
constexpr int DBG_BTN_CLIENT_CHEATS = 739;
constexpr int DBG_BTN_CHEAT_HOSTILE = 740;
constexpr int DBG_BTN_ITEM_ROW_BASE = 900;
constexpr int DBG_BTN_ITEM_PREV = 942;
constexpr int DBG_BTN_ITEM_NEXT = 943;
constexpr int DBG_BTN_ITEM_QTY = 944;
constexpr int DBG_BTN_ITEM_BACK = 945;
constexpr int DBG_BTN_SKIN_RESTORE = 741;
constexpr int DBG_BTN_SKIN_BACK = 742;
constexpr int DBG_BTN_SKIN_CAT_PREV = 743;
constexpr int DBG_BTN_SKIN_CAT_NEXT = 744;
constexpr int DBG_BTN_SKIN_MODEL_PREV = 745;
constexpr int DBG_BTN_SKIN_MODEL_NEXT = 746;
constexpr int DBG_BTN_SKIN = 747;
constexpr int DBG_BTN_NAME = 748;
constexpr int DBG_BTN_COLOR = 749;
constexpr int DBG_BTN_COLOR_BASE = 830;
constexpr int DBG_BTN_SKIN_CAT_BASE = 750;
constexpr int DBG_BTN_SKIN_MODEL_BASE = 770;
constexpr int DBG_BTN_PERK_ROW_BASE = 780; // + 0..41 = rows of the current perk page
constexpr int DBG_BTN_PERK_PREV = 850;
constexpr int DBG_BTN_PERK_NEXT = 851;
constexpr int DBG_BTN_PERK_DEC = 852;
constexpr int DBG_BTN_PERK_BACK = 853;
constexpr int DBG_BTN_TRAITS = 621;
constexpr int DBG_BTN_TRAIT_ROW_BASE = 860; // + 0..15 = the trait list
constexpr int DBG_BTN_TRAIT_BACK = 861;

constexpr int kDbgWindowWidth = 460;
constexpr int kDbgWindowHeight = 305;
constexpr int kDbgCheatWindowWidth = 380;
constexpr int kDbgCheatWindowHeight = 220;

uint32_t gDbgCheatFlags = 0;
bool gDbgCheatFlagsDirty = false;
uint32_t gDbgCheatLastSyncTick = 0;

// Session cheat policy: when false (the default) only the host may use the
// co-op cheats. Host flips it in the F11 co-op settings menu; every change
// is broadcast to the clients (NET_PKT_CHEAT_POLICY), which gate their
// menus and local effects on it.
bool gDbgClientCheatsEnabled = false;

// Centered position for a window of the given (width, height).
void dbgCenteredPos(int width, int height, int* outX, int* outY)
{
    *outX = (screenGetWidth() - width) / 2;
    if (*outX < 0) {
        *outX = 0;
    }
    *outY = (screenGetHeight() - height) / 2 - 30; // Bias up a bit
    if (*outY < 0) {
        *outY = 0;
    }
}

// Heal the local dude. On the host this runs the same authoritative apply a
// client heal uses (MpDebugApplyHeal) — it also revives a downed host. On a
// co-op client the HP is host-authoritative: the request goes to the host,
// which applies it to the avatar and lets the regular state/status broadcast
// carry the result back; the local apply is display-only convergence.
void dbgHeal(int amount)
{
    if (gDude == nullptr) {
        return;
    }
    if (gMpActive && gMpIsHost) {
        MpDebugApplyHeal(gDude, amount);
        return;
    }
    if (gMpActive && gMpIsClient) {
        MpDebugSendHeal(amount);
    }
    int maxHp = critterGetStat(gDude, STAT_MAXIMUM_HIT_POINTS);
    int hp = critterGetHitPoints(gDude);
    int delta = amount <= 0 ? maxHp - hp : amount;
    if (delta > 0) {
        critterAdjustHitPoints(gDude, delta);
        interfaceRenderHitPoints(true);
    }
}

// Refill the local dude's action points to maximum. Combat AP is
// host-authoritative in co-op: a client relays the request, the host applies
// it to the avatar, and the per-tick state channel carries the result back;
// the local apply is display-only convergence (same pattern as dbgHeal).
void dbgRefillAp()
{
    if (gDude == nullptr) {
        return;
    }
    if (gMpActive && gMpIsHost) {
        MpDebugApplyApRefill(gDude);
        return;
    }
    if (gMpActive && gMpIsClient) {
        MpDebugSendApRefill();
    }
    int maxAp = critterGetStat(gDude, STAT_MAXIMUM_ACTION_POINTS);
    gDude->data.critter.combat.ap = maxAp;
    MpLog(MP_LOG_UI, "ap refill local ap=%d", maxAp);
}

// Gives [count] copies of item proto [pid] to the local dude. On a co-op
// client the items ride the regular 1s profile capture up to the host's
// avatar mirror, so host-validated use (combat attacks, item actions) keeps
// working — same trust model as the money buttons.
void dbgGiveItem(int pid, int count)
{
    if (gDude == nullptr || pid < 0 || count <= 0) {
        return;
    }
    Object* item = nullptr;
    if (objectCreateWithPid(&item, pid) != 0 || item == nullptr) {
        MpLogAlways(MP_LOG_UI, "give item failed pid=0x%X", pid);
        return;
    }
    itemAdd(gDude, item, count);
    MpLog(MP_LOG_UI, "give item pid=0x%X count=%d", pid, count);
    interfaceUpdateItems(false, INTERFACE_ITEM_ACTION_DEFAULT, INTERFACE_ITEM_ACTION_DEFAULT);
}

// Ammo refill: fill the wielded weapon to capacity and drop a few spare
// clips of its caliber into the inventory. Melee/unarmed (no ammo type)
// does nothing.
void dbgRefillAmmo()
{
    if (gDude == nullptr) {
        return;
    }
    Hand hand = interfaceGetCurrentHand();
    Object* weapon = hand == HAND_RIGHT ? critterGetItem2(gDude) : critterGetItem1(gDude);
    if (weapon == nullptr || itemGetType(weapon) != ITEM_TYPE_WEAPON) {
        return;
    }
    int ammoTypePid = weaponGetAmmoTypePid(weapon);
    if (ammoTypePid == -1) {
        return;
    }
    int capacity = ammoGetCapacity(weapon);
    if (ammoGetQuantity(weapon) < capacity) {
        ammoSetQuantity(weapon, capacity);
    }
    dbgGiveItem(ammoTypePid, 3);
    MpLog(MP_LOG_UI, "ammo refill weapon pid=0x%X ammo=0x%X capacity=%d",
        weapon->pid, ammoTypePid, capacity);
}

void dbgRefillWeaponAmmo(Object* critter)
{
    if (critter == nullptr) {
        return;
    }

    Object* weapons[] = { critterGetItem1(critter), critterGetItem2(critter) };
    for (Object* weapon : weapons) {
        if (weapon == nullptr || itemGetType(weapon) != ITEM_TYPE_WEAPON) {
            continue;
        }
        int ammoTypePid = weaponGetAmmoTypePid(weapon);
        int capacity = ammoGetCapacity(weapon);
        if (ammoTypePid != -1 && capacity > 0 && ammoGetQuantity(weapon) < capacity) {
            ammoSetQuantity(weapon, capacity);
        }
    }
}

void dbgToggleCheat(uint32_t flag)
{
    // Client gate: when the host disabled client cheats, the client's
    // toggles are inert (its flags were cleared by the policy packet).
    if (gMpActive && gMpIsClient && !gDbgClientCheatsEnabled) {
        MpLogAlways(MP_LOG_UI, "cheat toggle blocked (disabled by host) flag=0x%X", flag);
        displayMonitorAddMessage("Cheats are disabled by the host.");
        return;
    }
    gDbgCheatFlags ^= flag;
    gDbgCheatFlagsDirty = true;
    if (gMpActive && gMpIsHost) {
        gMpSession.players[0].debugCheatFlags = gDbgCheatFlags;
    }
    MpLog(MP_LOG_UI, "cheat %s flags=0x%X",
        (gDbgCheatFlags & flag) != 0 ? "enabled" : "disabled", gDbgCheatFlags);
}

bool dbgCheatOn(uint32_t flag)
{
    return (gDbgCheatFlags & flag) != 0;
}

const char* dbgOnOff(uint32_t flag)
{
    return dbgCheatOn(flag) ? "ON" : "off";
}

uint32_t dbgCheatFlagsFor(const Object* critter)
{
    if (critter == nullptr) {
        return 0;
    }
    if (critter == gDude) {
        return gDbgCheatFlags;
    }
    if (gMpActive && gMpIsHost) {
        for (int index = 0; index < NET_MAX_PLAYERS; index++) {
            const MultiplayerPlayer* player = &gMpSession.players[index];
            if (player->isConnected && player->obj == critter) {
                return player->debugCheatFlags;
            }
        }
    }
    return 0;
}

void dbgApplyCheats(Object* critter, uint32_t flags)
{
    if (critter == nullptr) {
        return;
    }

    bool hpChanged = false;
    bool apChanged = false;
    if ((flags & MP_DEBUG_CHEAT_GOD_MODE) != 0) {
        int maxHp = critterGetStat(critter, STAT_MAXIMUM_HIT_POINTS);
        if (critter->data.critter.hp < maxHp) {
            critter->data.critter.hp = maxHp;
            hpChanged = true;
        }
    }
    if ((flags & MP_DEBUG_CHEAT_INFINITE_AP) != 0) {
        int maxAp = critterGetStat(critter, STAT_MAXIMUM_ACTION_POINTS);
        if (critter->data.critter.combat.ap != maxAp) {
            critter->data.critter.combat.ap = maxAp;
            apChanged = true;
        }
    }
    if ((flags & MP_DEBUG_CHEAT_INFINITE_AMMO) != 0) {
        dbgRefillWeaponAmmo(critter);
    }
    if (hpChanged) {
        MpLog(MP_LOG_UI, "god mode restored netId=%u hp=%d",
            MpGetObjNetId(critter), critter->data.critter.hp);
        if (critter == gDude) {
            interfaceRenderHitPoints(true);
        }
    }
    if (apChanged && critter == gDude) {
        interfaceRenderActionPoints(critter->data.critter.combat.ap, _combat_free_move);
    }
}

void dbgCheatModal()
{
    // Client gate: the whole cheats area is host-policy controlled — a
    // disabled client cannot even open the toggles.
    if (gMpActive && gMpIsClient && !gDbgClientCheatsEnabled) {
        MpLogAlways(MP_LOG_UI, "cheat options blocked (disabled by host)");
        displayMonitorAddMessage("Cheats are disabled by the host.");
        return;
    }
    int winX;
    int winY;
    dbgCenteredPos(kDbgCheatWindowWidth, kDbgCheatWindowHeight, &winX, &winY);

    int win = windowCreate(winX, winY, kDbgCheatWindowWidth, kDbgCheatWindowHeight,
        COLOR_BLACK, WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    if (win == -1) {
        MpLogAlways(MP_LOG_UI, "cheat submenu window create failed");
        return;
    }
    windowDrawBorder(win);

    const char* title = "CHEAT TOGGLES";
    int titleX = (kDbgCheatWindowWidth - fontGetStringWidth(title)) / 2;
    windowDrawText(win, title, 0, titleX, 6, COLOR_WHITE);
    _win_register_text_button(win, 30, 30, -1, -1, -1, DBG_BTN_CHEAT_GOD, "God Mode", 0);
    _win_register_text_button(win, 140, 30, -1, -1, -1, DBG_BTN_CHEAT_AP, "Infinite AP", 0);
    _win_register_text_button(win, 250, 30, -1, -1, -1, DBG_BTN_CHEAT_AMMO, "Infinite Ammo", 0);
    _win_register_text_button(win, 30, 55, -1, -1, -1, DBG_BTN_CHEAT_CARRY, "Unlimited Carry", 0);
    _win_register_text_button(win, 140, 55, -1, -1, -1, DBG_BTN_CHEAT_SKILLS, "Always Succeed", 0);
    _win_register_text_button(win, 250, 55, -1, -1, -1, DBG_BTN_CHEAT_ENCOUNTERS, "No Encounters", 0);
    _win_register_text_button(win, 30, 80, -1, -1, -1, DBG_BTN_CHEAT_INSTA_KILL, "Insta Kill", 0);
    _win_register_text_button(win, 140, 80, -1, -1, -1, DBG_BTN_CHEAT_HOSTILE, "Kill Hostile", 0);
    _win_register_text_button(win, 250, 80, -1, -1, -1, DBG_BTN_CHEAT_BACK, "Back", 0);
    windowRefresh(win);

    bool keepGoing = true;
    while (keepGoing) {
        sharedFpsLimiter.mark();
        windowFill(win, 8, 132, kDbgCheatWindowWidth - 16, 56, COLOR_BLACK);
        char status1[128];
        char status2[128];
        char status3[128];
        snprintf(status1, sizeof(status1), "God: %s   AP: %s   Ammo: %s   Carry: %s",
            dbgOnOff(MP_DEBUG_CHEAT_GOD_MODE),
            dbgOnOff(MP_DEBUG_CHEAT_INFINITE_AP),
            dbgOnOff(MP_DEBUG_CHEAT_INFINITE_AMMO),
            dbgOnOff(MP_DEBUG_CHEAT_UNLIMITED_CARRY));
        snprintf(status2, sizeof(status2), "Skills: %s   Enc: %s   InstaKill: %s",
            dbgOnOff(MP_DEBUG_CHEAT_ALWAYS_SUCCEED),
            dbgOnOff(MP_DEBUG_CHEAT_NO_RANDOM_ENCOUNTERS),
            dbgOnOff(MP_DEBUG_CHEAT_INSTA_KILL));
        snprintf(status3, sizeof(status3), "Hostile: %s",
            dbgOnOff(MP_DEBUG_CHEAT_KILL_HOSTILE));
        windowDrawText(win, status1, 0,
            (kDbgCheatWindowWidth - fontGetStringWidth(status1)) / 2, 135, COLOR_WHITE);
        windowDrawText(win, status2, 0,
            (kDbgCheatWindowWidth - fontGetStringWidth(status2)) / 2, 151, COLOR_WHITE);
        windowDrawText(win, status3, 0,
            (kDbgCheatWindowWidth - fontGetStringWidth(status3)) / 2, 167, COLOR_WHITE);
        if (gMpActive && gMpIsClient && !gDbgClientCheatsEnabled) {
            const char* blocked = "CHEATS DISABLED BY HOST";
            windowDrawText(win, blocked, 0,
                (kDbgCheatWindowWidth - fontGetStringWidth(blocked)) / 2, 183, COLOR_WHITE);
        }
        windowRefresh(win);
        renderPresent();
        MpTick();
        mouseShowCursor();

        int keyCode = inputGetInput();
        switch (keyCode) {
        case KEY_ESCAPE:
            MpLog(MP_LOG_UI, "modal close via ESC menu='cheat-toggles'");
        case DBG_BTN_CHEAT_BACK:
            keepGoing = false;
            break;
        case DBG_BTN_CHEAT_GOD:
            dbgToggleCheat(MP_DEBUG_CHEAT_GOD_MODE);
            break;
        case DBG_BTN_CHEAT_AP:
            dbgToggleCheat(MP_DEBUG_CHEAT_INFINITE_AP);
            break;
        case DBG_BTN_CHEAT_AMMO:
            dbgToggleCheat(MP_DEBUG_CHEAT_INFINITE_AMMO);
            break;
        case DBG_BTN_CHEAT_CARRY:
            dbgToggleCheat(MP_DEBUG_CHEAT_UNLIMITED_CARRY);
            break;
        case DBG_BTN_CHEAT_SKILLS:
            dbgToggleCheat(MP_DEBUG_CHEAT_ALWAYS_SUCCEED);
            break;
        case DBG_BTN_CHEAT_ENCOUNTERS:
            dbgToggleCheat(MP_DEBUG_CHEAT_NO_RANDOM_ENCOUNTERS);
            break;
        case DBG_BTN_CHEAT_INSTA_KILL:
            dbgToggleCheat(MP_DEBUG_CHEAT_INSTA_KILL);
            break;
        case DBG_BTN_CHEAT_HOSTILE:
            dbgToggleCheat(MP_DEBUG_CHEAT_KILL_HOSTILE);
            break;
        default:
            break;
        }
        sharedFpsLimiter.throttle();
    }

    windowDestroy(win);
}

// Level the local dude to the cap (99) through the vanilla level-up path
// (add exactly the XP needed for each next level), so perks and skill
// points are granted normally.
void dbgMaxLevel()
{
    if (gDude == nullptr) {
        return;
    }
    int level = pcGetStat(PC_STAT_LEVEL);
    int guard = 0;
    while (level < PC_LEVEL_MAX && guard++ < 200) {
        int xpToNext = pcGetExperienceForNextLevel() - pcGetStat(PC_STAT_EXPERIENCE);
        if (xpToNext <= 0) {
            break;
        }
        pcAddExperience(xpToNext, nullptr);
        level = pcGetStat(PC_STAT_LEVEL);
    }
    MpLog(MP_LOG_UI, "max level done level=%d", level);
}

// ---------------------------------------------------------------------------
// editor submenus (skills / stats / perks)
// ---------------------------------------------------------------------------

struct SubmenuCallbacks {
    const char* title;
    int count;
    int incAmount; // value delta per INC/DEC click
    const char* (*name)(int);
    int (*value)(int);
    void (*modify)(int, int);
};

const char* dbgSkillName(int index)
{
    return skillGetName(static_cast<Skill>(index));
}

int dbgSkillValue(int index)
{
    return gDude != nullptr ? skillGetValue(gDude, static_cast<Skill>(index)) : 0;
}

void dbgSkillModify(int index, int delta)
{
    if (gDude == nullptr) {
        return;
    }
    Skill skill = static_cast<Skill>(index);
    if (delta > 0) {
        for (int i = 0; i < delta; i++) {
            skillAddForce(gDude, skill);
        }
    } else {
        for (int i = 0; i < -delta; i++) {
            skillSubForce(gDude, skill);
        }
    }
}

const char* dbgStatName(int index)
{
    return statGetName(static_cast<Stat>(index));
}

int dbgStatValue(int index)
{
    // The base stat — the same number the character editor shows. The
    // effective value (with trait/perk modifiers) would make the +/- edits
    // land on the wrong base.
    return gDude != nullptr ? critterGetBaseStat(gDude, static_cast<Stat>(index)) : 0;
}

void dbgStatModify(int index, int delta)
{
    if (gDude == nullptr) {
        return;
    }
    Stat stat = static_cast<Stat>(index);
    int before = critterGetBaseStat(gDude, stat);
    MpLog(MP_LOG_UI, "stat modify index=%d delta=%d stat=%s before=%d",
        index, delta, statGetName(stat), before);
    // The vanilla setter stores (target - traitModifier) into the raw base
    // (critterSetBaseStat subtracts the trait modifier so that
    // getWithTraitModifier == target). The menu displays the raw value, so
    // the target must be computed from the trait-MODIFIED base — exactly the
    // critterIncBaseStat/critterDecBaseStat semantics. Without this, a
    // Gifted character (+1 all SPECIAL) stores raw+delta-1: increases never
    // visibly change and decreases jump by two.
    int next = std::clamp(
        critterGetBaseStatWithTraitModifier(gDude, stat) + delta, 1, 10);
    critterSetBaseStat(gDude, stat, next);
    critterUpdateDerivedStats(gDude);
    interfaceRenderHitPoints(true);
}

const char* dbgPerkName(int index)
{
    return perkGetName(static_cast<Perk>(index));
}

int dbgPerkValue(int index)
{
    return gDude != nullptr ? perkGetRank(gDude, static_cast<Perk>(index)) : 0;
}

void dbgPerkModify(int index, int delta)
{
    if (gDude == nullptr) {
        return;
    }
    Perk perk = static_cast<Perk>(index);
    if (delta > 0) {
        for (int i = 0; i < delta; i++) {
            perkAddForce(gDude, perk);
        }
    } else {
        for (int i = 0; i < -delta; i++) {
            perkRemove(gDude, perk);
        }
    }
}

// Curated item list for the Items... submenu. Pids from the PROTO_ID_*
// constants (verified item protos); display names are static on purpose.
struct DebugItemEntry {
    int pid;
    const char* name;
};

const DebugItemEntry kDbgItems[] = {
    { PROTO_ID_STIMPAK, "Stimpak" },
    { PROTO_ID_SUPER_STIMPAK, "Super Stimpak" },
    { PROTO_ID_FIRST_AID_KIT, "First Aid Kit" },
    { PROTO_ID_DOCTORS_BAG, "Doctor's Bag" },
    { PROTO_ID_HEALING_POWDER, "Healing Powder" },
    { PROTO_ID_RADAWAY, "RadAway" },
    { PROTO_ID_MENTATS, "Mentats" },
    { PROTO_ID_BUFF_OUT, "Buffout" },
    { PROTO_ID_PSYCHO, "Psycho" },
    { PROTO_ID_JET, "Jet" },
    { PROTO_ID_NUKA_COLA, "Nuka Cola" },
    { PROTO_ID_BEER, "Beer" },
    { PROTO_ID_BOOZE, "Booze" },
    { PROTO_ID_MONEY, "Money" },
    { PROTO_ID_SMALL_ENERGY_CELL, "Small Energy Cell" },
    { PROTO_ID_MICRO_FUSION_CELL, "Micro Fusion Cell" },
    { PROTO_ID_DYNAMITE_I, "Dynamite" },
    { PROTO_ID_PLASTIC_EXPLOSIVES_I, "Plastic Explosives" },
    { PROTO_ID_MOLOTOV_COCKTAIL, "Molotov Cocktail" },
    { PROTO_ID_FLARE, "Flare" },
};

const char* dbgItemName(int index)
{
    return kDbgItems[index].name;
}

// Quantity of this item currently carried by the local dude.
int dbgItemValue(int index)
{
    if (gDude == nullptr) {
        return 0;
    }
    int count = 0;
    for (int i = 0; i < gDude->data.inventory.length; i++) {
        auto* invItem = &gDude->data.inventory.items[i];
        if (invItem->item != nullptr && invItem->item->pid == kDbgItems[index].pid) {
            count += invItem->quantity;
        }
    }
    return count;
}

void dbgItemModify(int index, int delta)
{
    if (gDude == nullptr) {
        return;
    }
    if (delta > 0) {
        dbgGiveItem(kDbgItems[index].pid, delta);
        return;
    }
    if (delta >= 0) {
        return;
    }
    // Remove up to -delta copies. Equipped gear is left alone — the removal
    // hook reasons differ per slot (hand/armor) and a wrong reason would
    // desync the sprite (see the weapon-strip fix).
    int remaining = -delta;
    for (int i = 0; i < gDude->data.inventory.length && remaining > 0; i++) {
        auto* invItem = &gDude->data.inventory.items[i];
        if (invItem->item == nullptr
            || invItem->item->pid != kDbgItems[index].pid
            || (invItem->item->flags & OBJECT_EQUIPPED) != 0) {
            continue;
        }
        int removeQty = std::min(remaining, invItem->quantity);
        itemRemoveWithReason(gDude, invItem->item, removeQty,
            RemoveInventoryObjectHookReason::ItemRemoved);
        remaining -= removeQty;
    }
    MpLog(MP_LOG_UI, "item removed pid=0x%X count=%d", kDbgItems[index].pid, -delta - remaining);
}

// Runs the modal loop for an editor submenu. The current entry line is
// cleared and redrawn every frame — the centered x shifts with the string
// width, so the text's own background fill covers a different rect each time
// and would leave ghost tails without an explicit full-line clear.
int dbgSubmenuModal(int win, const SubmenuCallbacks* cb, int current)
{
    char buf[96];
    int rc = -1;
    while (rc == -1) {
        sharedFpsLimiter.mark();

        // Full-line clear must cover the whole glyph cell (the text at y=30
        // with the 16px default font spans to ~46) — a shorter rect leaves
        // the old glyphs' bottom pixels behind as dots.
        windowFill(win, 8, 24, kDbgWindowWidth - 16, 24, COLOR_BLACK);
        int value = cb->value(current);
        snprintf(buf, sizeof(buf), "%s  %d", cb->name(current), value);
        int x = (kDbgWindowWidth - fontGetStringWidth(buf)) / 2;
        windowDrawText(win, buf, 0, x, 30, COLOR_WHITE);
        // The info rect's own dirty marking does not reliably reach the blit;
        // only a full-window refresh does (a click reveals the line because it
        // triggers one). Force it every frame so the line shows immediately.
        windowRefresh(win);

        // Show THIS frame before waiting for input. With the blit after
        // inputGetInput the visible frame is always the previous iteration's
        // (static menus hide the lag; a live info line falls one action
        // behind — clicking Next shows the old stat until the next click).
        renderPresent();

        // Keep the session alive behind the modal: the profile sync, deferred
        // drains and host detect all run from MpTick, which the main loop
        // cannot reach while this modal blocks it. Same pattern as the vote
        // modal. No-ops when not in a session.
        MpTick();
        mouseShowCursor();
        int keyCode = inputGetInput();
        switch (keyCode) {
        case KEY_ESCAPE:
            MpLog(MP_LOG_UI, "modal close via ESC menu='submenu-cycler'");
            rc = 0;
            break;
        case DBG_BTN_PREV:
        case DBG_BTN_NEXT:
        case DBG_BTN_DEC:
        case DBG_BTN_INC:
        case DBG_BTN_BACK:
            rc = keyCode;
            break;
        default:
            break;
        }

        sharedFpsLimiter.throttle();
    }
    return rc;
}

// Opens a cycling editor submenu (skills / stats / perks).
void dbgSubmenuShow(const SubmenuCallbacks* cb)
{
    constexpr int kWidth = kDbgWindowWidth;
    constexpr int kHeight = 185;
    int winX, winY;
    dbgCenteredPos(kWidth, kHeight, &winX, &winY);

    int win = windowCreate(winX, winY, kWidth, kHeight, COLOR_BLACK, WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    if (win == -1) {
        return;
    }
    windowDrawBorder(win);

    int titleX = (kWidth - fontGetStringWidth(cb->title)) / 2;
    windowDrawText(win, cb->title, 0, titleX, 6, COLOR_WHITE);

    char incLabel[16];
    char decLabel[16];
    snprintf(incLabel, sizeof(incLabel), "+%d", cb->incAmount);
    snprintf(decLabel, sizeof(decLabel), "-%d", cb->incAmount);

    _win_register_text_button(win, 30, 55, -1, -1, -1, DBG_BTN_PREV, "Prev", 0);
    _win_register_text_button(win, 170, 55, -1, -1, -1, DBG_BTN_NEXT, "Next", 0);
    _win_register_text_button(win, 30, 85, -1, -1, -1, DBG_BTN_DEC, decLabel, 0);
    _win_register_text_button(win, 170, 85, -1, -1, -1, DBG_BTN_INC, incLabel, 0);
    _win_register_text_button(win, 30, 115, -1, -1, -1, DBG_BTN_BACK, "Back", 0);
    windowRefresh(win);

    int current = 0;
    for (;;) {
        int choice = dbgSubmenuModal(win, cb, current);
        MpLog(MP_LOG_UI, "submenu %s choice=%d current=%d name='%s' value=%d",
            cb->title, choice, current, cb->name(current), cb->value(current));
        if (choice == 0 || choice == DBG_BTN_BACK) {
            break;
        }
        switch (choice) {
        case DBG_BTN_PREV:
            current = (current + cb->count - 1) % cb->count;
            break;
        case DBG_BTN_NEXT:
            current = (current + 1) % cb->count;
            break;
        case DBG_BTN_DEC:
            cb->modify(current, -cb->incAmount);
            break;
        case DBG_BTN_INC:
            cb->modify(current, cb->incAmount);
            break;
        }
    }

    windowDestroy(win);
}

// Perk editor as a paged list: 3 columns x 14 rows = 42 perks per page;
// PERK_COUNT (121) fits in 3 pages. The old single-item cycler needed up
// to 120 clicks to reach the last perk.
static void dbgPerkListShow()
{
    if (gDude == nullptr) {
        return;
    }
    constexpr int kPerkCols = 3;
    constexpr int kPerkRows = 14;
    constexpr int kPerkPageSize = kPerkCols * kPerkRows;
    constexpr int kPerkPages = (PERK_COUNT + kPerkPageSize - 1) / kPerkPageSize;
    constexpr int kPerkWidth = 640;
    constexpr int kPerkHeight = 460;

    int page = 0;
    int selected = 0;
    int win = -1;

    auto perkIndex = [&](int slot) {
        return page * kPerkPageSize + slot;
    };

    auto rebuild = [&]() {
        if (win != -1) {
            windowDestroy(win);
            win = -1;
        }
        int winX, winY;
        dbgCenteredPos(kPerkWidth, kPerkHeight, &winX, &winY);
        win = windowCreate(winX, winY, kPerkWidth, kPerkHeight, COLOR_BLACK,
            WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
        if (win == -1) {
            return;
        }
        windowDrawBorder(win);
        const char* title = "PERKS";
        int titleX = (kPerkWidth - fontGetStringWidth(title)) / 2;
        windowDrawText(win, title, 0, titleX, 6, COLOR_WHITE);

        char status[128];
        const char* selName = perkGetName(static_cast<Perk>(selected));
        int selRank = perkGetRank(gDude, static_cast<Perk>(selected));
        snprintf(status, sizeof(status), "Page %d/%d   Selected: %s [%d]",
            page + 1, kPerkPages,
            selName != nullptr ? selName : "?", selRank);
        windowDrawText(win, status, 0, 30, 28, COLOR_WHITE);

        for (int slot = 0; slot < kPerkPageSize; slot++) {
            int perk = perkIndex(slot);
            if (perk >= PERK_COUNT) {
                break;
            }
            const char* name = perkGetName(static_cast<Perk>(perk));
            if (name == nullptr) {
                continue;
            }
            int rank = perkGetRank(gDude, static_cast<Perk>(perk));
            char label[40];
            if (rank > 0) {
                // 24 chars keeps the widest labels inside their column slot.
                snprintf(label, sizeof(label), "%.24s [%d]", name, rank);
            } else {
                snprintf(label, sizeof(label), "%.24s", name);
            }
            int col = slot % kPerkCols;
            int row = slot / kPerkCols;
            _win_register_text_button(win, 30 + col * 210, 62 + row * 24,
                -1, -1, -1, DBG_BTN_PERK_ROW_BASE + slot, label, 0);
        }
        _win_register_text_button(win, 30, 410, -1, -1, -1, DBG_BTN_PERK_PREV, "Prev", 0);
        _win_register_text_button(win, 170, 410, -1, -1, -1, DBG_BTN_PERK_NEXT, "Next", 0);
        _win_register_text_button(win, 330, 410, -1, -1, -1, DBG_BTN_PERK_DEC, "Remove", 0);
        _win_register_text_button(win, 530, 410, -1, -1, -1, DBG_BTN_PERK_BACK, "Back", 0);
        windowRefresh(win);
    };

    rebuild();
    if (win == -1) {
        return;
    }

    bool keepGoing = true;
    while (keepGoing) {
        sharedFpsLimiter.mark();
        // Keep the session alive behind the modal (same pattern as every
        // other debug modal).
        MpTick();
        mouseShowCursor();
        int keyCode = inputGetInput();
        switch (keyCode) {
        case KEY_ESCAPE:
            MpLog(MP_LOG_UI, "modal close via ESC menu='perks'");
        case DBG_BTN_PERK_BACK:
            keepGoing = false;
            break;
        case DBG_BTN_PERK_PREV:
            page = (page + kPerkPages - 1) % kPerkPages;
            selected = perkIndex(0);
            rebuild();
            break;
        case DBG_BTN_PERK_NEXT:
            page = (page + 1) % kPerkPages;
            selected = perkIndex(0);
            rebuild();
            break;
        case DBG_BTN_PERK_DEC:
            perkRemove(gDude, static_cast<Perk>(selected));
            MpLog(MP_LOG_UI, "perk remove index=%d name='%s'",
                selected, perkGetName(static_cast<Perk>(selected)));
            rebuild();
            break;
        default:
            if (keyCode >= DBG_BTN_PERK_ROW_BASE
                && keyCode < DBG_BTN_PERK_ROW_BASE + kPerkPageSize) {
                int perk = perkIndex(keyCode - DBG_BTN_PERK_ROW_BASE);
                if (perk >= 0 && perk < PERK_COUNT) {
                    selected = perk;
                    perkAddForce(gDude, static_cast<Perk>(perk));
                    MpLog(MP_LOG_UI, "perk add index=%d name='%s'",
                        perk, perkGetName(static_cast<Perk>(perk)));
                    rebuild();
                }
            }
            break;
        }
        renderPresent();
        sharedFpsLimiter.throttle();
    }

    if (win != -1) {
        windowDestroy(win);
    }
}

// Trait editor: all 16 traits in one 2-column list (Bloody Mess lives here —
// it is a trait, not a perk). Click toggles the trait in/out of the two
// selected slots (slot 1 replaced when both are full).
static void dbgTraitListShow()
{
    if (gDude == nullptr) {
        return;
    }
    constexpr int kTraitCols = 2;
    constexpr int kTraitRows = 8;
    constexpr int kTraitWidth = kDbgWindowWidth;
    constexpr int kTraitHeight = 305;

    int winX, winY;
    dbgCenteredPos(kTraitWidth, kTraitHeight, &winX, &winY);
    int win = windowCreate(winX, winY, kTraitWidth, kTraitHeight, COLOR_BLACK,
        WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    if (win == -1) {
        return;
    }
    windowDrawBorder(win);
    const char* title = "TRAITS";
    int titleX = (kTraitWidth - fontGetStringWidth(title)) / 2;
    windowDrawText(win, title, 0, titleX, 6, COLOR_WHITE);

    auto drawStatus = [&]() {
        Trait selected[2];
        traitsGetSelected(&selected[0], &selected[1]);
        char status[128];
        if (selected[0] != TRAIT_INVALID && selected[1] != TRAIT_INVALID) {
            snprintf(status, sizeof(status), "Selected: %s, %s",
                traitGetName(selected[0]), traitGetName(selected[1]));
        } else if (selected[0] != TRAIT_INVALID) {
            snprintf(status, sizeof(status), "Selected: %s",
                traitGetName(selected[0]));
        } else {
            snprintf(status, sizeof(status), "Selected: none");
        }
        windowFill(win, 8, 24, kTraitWidth - 16, 20, COLOR_BLACK);
        windowDrawText(win, status, 0, 30, 28, COLOR_WHITE);
    };

    auto rebuild = [&]() {
        // Re-registering buttons in place never clears the surface: the old
        // ' [1]'/' [2]' glyphs beyond a shorter label's width stayed in the
        // window buffer (the unselect ghost). Clear the whole surface and
        // redraw the border before re-registering.
        windowFill(win, 0, 0, kTraitWidth, kTraitHeight, COLOR_BLACK);
        windowDrawBorder(win);
        for (int index = 0; index < TRAIT_COUNT; index++) {
            Trait trait = static_cast<Trait>(index);
            const char* name = traitGetName(trait);
            if (name == nullptr) {
                continue;
            }
            Trait selected[2];
            traitsGetSelected(&selected[0], &selected[1]);
            char label[40];
            if (selected[0] == trait) {
                snprintf(label, sizeof(label), "%.24s [1]", name);
            } else if (selected[1] == trait) {
                snprintf(label, sizeof(label), "%.24s [2]", name);
            } else {
                snprintf(label, sizeof(label), "%.24s", name);
            }
            int col = index % kTraitCols;
            int row = index / kTraitCols;
            _win_register_text_button(win, 30 + col * 210, 62 + row * 24,
                -1, -1, -1, DBG_BTN_TRAIT_ROW_BASE + index, label, 0);
        }
        _win_register_text_button(win, 330, 260, -1, -1, -1, DBG_BTN_TRAIT_BACK, "Back", 0);
        drawStatus();
        windowRefresh(win);
    };

    rebuild();

    bool keepGoing = true;
    while (keepGoing) {
        sharedFpsLimiter.mark();
        MpTick();
        mouseShowCursor();
        int keyCode = inputGetInput();
        switch (keyCode) {
        case KEY_ESCAPE:
            MpLog(MP_LOG_UI, "modal close via ESC menu='traits'");
        case DBG_BTN_TRAIT_BACK:
            keepGoing = false;
            break;
        default:
            if (keyCode >= DBG_BTN_TRAIT_ROW_BASE
                && keyCode < DBG_BTN_TRAIT_ROW_BASE + TRAIT_COUNT) {
                Trait trait = static_cast<Trait>(keyCode - DBG_BTN_TRAIT_ROW_BASE);
                Trait selected[2];
                traitsGetSelected(&selected[0], &selected[1]);
                if (selected[0] == trait) {
                    selected[0] = selected[1];
                    selected[1] = TRAIT_INVALID;
                } else if (selected[1] == trait) {
                    selected[1] = TRAIT_INVALID;
                } else if (selected[0] == TRAIT_INVALID) {
                    selected[0] = trait;
                } else {
                    selected[1] = trait;
                }
                traitsSetSelected(selected[0], selected[1]);
                MpLog(MP_LOG_UI, "traits set %d %d (%s %s)",
                    (int)selected[0], (int)selected[1],
                    traitGetName(selected[0]) != nullptr ? traitGetName(selected[0]) : "none",
                    traitGetName(selected[1]) != nullptr ? traitGetName(selected[1]) : "none");
                rebuild();
            }
            break;
        }
        renderPresent();
        sharedFpsLimiter.throttle();
    }

    windowDestroy(win);
}

// Kill Hostile: one-shot action — executes once and self-clears. Kills every
// living critter on the map that is hostile to the local player's team (the
// same predicate the end-combat check uses), excluding the co-op players.
static int dbgKillHostiles()
{
    int executed = 0;
    Object* probe = objectFindFirst();
    while (probe != nullptr) {
        if (objectTypeFromFid(probe->fid) == OBJ_TYPE_CRITTER
            && probe != gDude
            && !MpIsCoopPlayerCritter(probe)
            && critterGetStat(probe, STAT_CURRENT_HIT_POINTS) > 0
            && (probe->data.critter.combat.results & DAM_DEAD) == 0
            && probe->data.critter.combat.team != gDude->data.critter.combat.team) {
            critterKill(probe, ANIM_INVALID, true);
            executed++;
        }
        probe = objectFindNext();
    }
    MpLog(MP_LOG_UI, "kill hostile executed=%d", executed);
    return executed;
}

// Item browser: every item proto in paged form; a row click hands the item
// to the local dude in the current quantity. Stays open for repeated grabs.
static void dbgItemBrowserShow()
{
    // One-time enumeration: FO2 item protos are contiguous from 0x1000001;
    // stop after a run of 16 misses.
    static std::vector<int> sItemPids;
    if (sItemPids.empty()) {
        int miss = 0;
        for (int n = 1; n <= 4096 && miss < 16; n++) {
            Proto* proto = nullptr;
            if (protoGetProto(0x1000000 + n, &proto) == 0) {
                sItemPids.push_back(0x1000000 + n);
                miss = 0;
            } else {
                miss++;
            }
        }
        MpLog(MP_LOG_UI, "item browser enumerated %zu pids", sItemPids.size());
    }
    if (sItemPids.empty()) {
        win_timed_msg("No item protos found", COLOR_RED);
        return;
    }

    constexpr int kWindowWidth = 640;
    constexpr int kWindowHeight = 460;
    constexpr int kCols = 3;
    constexpr int kRows = 14;
    constexpr int kPageSize = kCols * kRows;
    const int pageCount = (int)((sItemPids.size() + kPageSize - 1) / kPageSize);

    int qty = 1;
    int page = 0;
    int selected = -1;

    auto rebuild = [&]() {
        int winX;
        int winY;
        dbgCenteredPos(kWindowWidth, kWindowHeight, &winX, &winY);
        int win = windowCreate(winX, winY, kWindowWidth, kWindowHeight,
            COLOR_BLACK, WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
        if (win == -1) {
            MpLogAlways(MP_LOG_UI, "item browser window create failed w=%d h=%d", kWindowWidth, kWindowHeight);
            return -1;
        }
        windowDrawBorder(win);
        const char* title = "ITEMS";
        windowDrawText(win, title, 0,
            (kWindowWidth - fontGetStringWidth(title)) / 2, 6, COLOR_WHITE);

        char status[96];
        snprintf(status, sizeof(status), "Page %d/%d   Qty: %d",
            page + 1, pageCount, qty);
        windowFill(win, 8, 24, kWindowWidth - 16, 24, COLOR_BLACK);
        windowDrawText(win, status, 0,
            (kWindowWidth - fontGetStringWidth(status)) / 2, 28, COLOR_WHITE);

        int start = page * kPageSize;
        for (int slot = 0; slot < kPageSize; slot++) {
            int index = start + slot;
            if (index >= (int)sItemPids.size()) {
                break;
            }
            int pid = sItemPids[index];
            char* name = protoGetName(pid);
            char label[40];
            if (name == nullptr || name[0] == '\0') {
                snprintf(label, sizeof(label), "pid 0x%X", pid);
            } else {
                snprintf(label, sizeof(label), "%.22s", name);
            }
            int col = slot % kCols;
            int row = slot / kCols;
            _win_register_text_button(win, 30 + col * 210, 62 + row * 24, -1, -1, -1,
                DBG_BTN_ITEM_ROW_BASE + slot, label, 0);
        }
        _win_register_text_button(win, 30, 410, -1, -1, -1, DBG_BTN_ITEM_PREV, "Prev", 0);
        _win_register_text_button(win, 170, 410, -1, -1, -1, DBG_BTN_ITEM_NEXT, "Next", 0);
        _win_register_text_button(win, 330, 410, -1, -1, -1, DBG_BTN_ITEM_QTY, "Qty", 0);
        _win_register_text_button(win, 530, 410, -1, -1, -1, DBG_BTN_ITEM_BACK, "Back", 0);
        windowRefresh(win);
        return win;
    };

    int win = rebuild();
    if (win == -1) {
        return;
    }

    bool keepGoing = true;
    while (keepGoing) {
        sharedFpsLimiter.mark();
        MpTick();
        mouseShowCursor();
        int keyCode = inputGetInput();
        switch (keyCode) {
        case KEY_ESCAPE:
            MpLog(MP_LOG_UI, "modal close via ESC menu='items'");
        case DBG_BTN_ITEM_BACK:
            keepGoing = false;
            break;
        case DBG_BTN_ITEM_PREV:
            page = (page + pageCount - 1) % pageCount;
            selected = -1;
            windowDestroy(win);
            win = rebuild();
            break;
        case DBG_BTN_ITEM_NEXT:
            page = (page + 1) % pageCount;
            selected = -1;
            windowDestroy(win);
            win = rebuild();
            break;
        case DBG_BTN_ITEM_QTY:
            qty = (qty == 1) ? 10 : (qty == 10) ? 100 : 1;
            windowDestroy(win);
            win = rebuild();
            break;
        default:
            if (keyCode >= DBG_BTN_ITEM_ROW_BASE && keyCode < DBG_BTN_ITEM_ROW_BASE + kPageSize) {
                int slot = keyCode - DBG_BTN_ITEM_ROW_BASE;
                int index = page * kPageSize + slot;
                if (index < (int)sItemPids.size()) {
                    selected = index;
                    dbgGiveItem(sItemPids[index], qty);
                    // Keep the window; just refresh the status beat.
                    char status[96];
                    snprintf(status, sizeof(status), "Page %d/%d   Qty: %d   Gave %d x%s",
                        page + 1, pageCount, qty, qty,
                        protoGetName(sItemPids[index]) != nullptr
                            ? protoGetName(sItemPids[index]) : "");
                    windowFill(win, 8, 24, kWindowWidth - 16, 24, COLOR_BLACK);
                    windowDrawText(win, status, 0,
                        (kWindowWidth - fontGetStringWidth(status)) / 2, 28, COLOR_WHITE);
                    windowRefresh(win);
                }
            }
            break;
        }
        renderPresent();
        sharedFpsLimiter.throttle();
    }

    windowDestroy(win);
}

static const char* kPlayerPaletteNames[8] = {
    "Cyan", "Red", "Green", "Blue", "Yellow", "Pink", "Orange", "Gold",
};

// Rename the local player from the F11 CO-OP SETTINGS menu. The name rides
// the profile's IDENTITY wire to every machine and persists via the COOP
// save handler. Printable KEY_* constants ARE the ASCII characters (kb.h), so
// the keycode->char mapping is the printable range; BACKSPACE deletes,
// RETURN commits (empty = revert to the default name), ESC cancels.
static void dbgNameEditorShow()
{
    int winX;
    int winY;
    dbgCenteredPos(320, 120, &winX, &winY);
    int win = windowCreate(winX, winY, 320, 120, COLOR_BLACK,
        WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    if (win == -1) {
        return;
    }
    windowDrawBorder(win);
    const char* title = "PLAYER NAME";
    windowDrawText(win, title, 0, (320 - fontGetStringWidth(title)) / 2, 6, COLOR_WHITE);

    char nameBuf[MP_PROFILE_NAME_LENGTH];
    const char* current = MpDebugLocalPlayerNameOverride();
    strncpy(nameBuf, current != nullptr ? current : "", MP_PROFILE_NAME_LENGTH - 1);
    nameBuf[MP_PROFILE_NAME_LENGTH - 1] = '\0';
    windowRefresh(win);

    bool keepGoing = true;
    while (keepGoing) {
        sharedFpsLimiter.mark();
        MpTick();
        mouseShowCursor();
        windowFill(win, 8, 40, 304, 20, COLOR_BLACK);
        windowDrawText(win, nameBuf, 0, 16, 42, COLOR_WHITE);
        windowRefresh(win);
        renderPresent();
        int keyCode = inputGetInput();
        if (keyCode >= 32 && keyCode <= 126) {
            size_t len = strlen(nameBuf);
            if (len < MP_PROFILE_NAME_LENGTH - 1) {
                nameBuf[len] = (char)keyCode;
                nameBuf[len + 1] = '\0';
            }
        } else if (keyCode == KEY_BACKSPACE) {
            size_t len = strlen(nameBuf);
            if (len > 0) {
                nameBuf[len - 1] = '\0';
            }
        } else if (keyCode == KEY_RETURN) {
            if (nameBuf[0] != '\0') {
                MpDebugSetLocalPlayerName(nameBuf);
                MpLog(MP_LOG_UI, "player name set to '%s'", nameBuf);
            } else {
                MpDebugSetLocalPlayerName(nullptr);
                MpLog(MP_LOG_UI, "player name override cleared");
            }
            keepGoing = false;
        } else if (keyCode == KEY_ESCAPE) {
            MpLog(MP_LOG_UI, "modal close via ESC menu='name-editor'");
            keepGoing = false;
        }
        sharedFpsLimiter.throttle();
    }
    windowDestroy(win);
}

// Pick the local player's highlight/label/card color. The color rides the
// profile's IDENTITY wire (playerColor) and persists via the COOP save
// handler; the ring, floating name label, and combat card borders all use it.
static void dbgColorPickerShow()
{
    constexpr int kWindowWidth = 340;
    constexpr int kWindowHeight = 250;
    constexpr int DBG_BTN_R_DEC = DBG_BTN_COLOR_BASE + 10;
    constexpr int DBG_BTN_R_INC = DBG_BTN_COLOR_BASE + 11;
    constexpr int DBG_BTN_G_DEC = DBG_BTN_COLOR_BASE + 12;
    constexpr int DBG_BTN_G_INC = DBG_BTN_COLOR_BASE + 13;
    constexpr int DBG_BTN_B_DEC = DBG_BTN_COLOR_BASE + 14;
    constexpr int DBG_BTN_B_INC = DBG_BTN_COLOR_BASE + 15;
    constexpr int DBG_BTN_CLOSE_X = DBG_BTN_COLOR_BASE + 16;

    int winX;
    int winY;
    dbgCenteredPos(kWindowWidth, kWindowHeight, &winX, &winY);
    int win = windowCreate(winX, winY, kWindowWidth, kWindowHeight, COLOR_BLACK,
        WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    if (win == -1) {
        return;
    }
    windowDrawBorder(win);
    const char* title = "PLAYER COLOR";
    windowDrawText(win, title, 0, (kWindowWidth - fontGetStringWidth(title)) / 2, 6, COLOR_WHITE);

    // Top-right close [X] button
    _win_register_text_button(win, kWindowWidth - 26, 4, -1, -1, -1, DBG_BTN_CLOSE_X, "X", 0);

    // Static slider labels
    windowDrawText(win, "R:", 0, 20, 92, COLOR_LIGHT_RED);
    windowDrawText(win, "G:", 0, 20, 132, COLOR_LIGHT_GREEN_3);
    windowDrawText(win, "B:", 0, 20, 172, COLOR_PALE_BLUE);

    // Red Slider buttons
    _win_register_text_button(win, 282, 90, -1, -1, -1, DBG_BTN_R_DEC, "<", 0);
    _win_register_text_button(win, 305, 90, -1, -1, -1, DBG_BTN_R_INC, ">", 0);

    // Green Slider buttons
    _win_register_text_button(win, 282, 130, -1, -1, -1, DBG_BTN_G_DEC, "<", 0);
    _win_register_text_button(win, 305, 130, -1, -1, -1, DBG_BTN_G_INC, ">", 0);

    // Blue Slider buttons
    _win_register_text_button(win, 282, 170, -1, -1, -1, DBG_BTN_B_DEC, "<", 0);
    _win_register_text_button(win, 305, 170, -1, -1, -1, DBG_BTN_B_INC, ">", 0);

    // Action buttons at bottom
    _win_register_text_button(win, 50, 210, -1, -1, -1, DBG_BTN_COLOR_BASE + 8, "Apply", 0);
    _win_register_text_button(win, 200, 210, -1, -1, -1, DBG_BTN_COLOR_BASE + 9, "Cancel", 0);

    int rVal = 0;
    int gVal = 255;
    int bVal = 0;

    int pal = MpPlayerColorFor(gDude);
    if (pal >= 0 && pal < 256 && _cmap != nullptr) {
        // The palette stores 6-bit channels; scale to the slider's 0-255.
        // (Color2RGB returns 15-bit fields, not 24-bit RGB — the old
        // initialization produced garbage slider values from a chosen color.)
        rVal = (_cmap[3 * pal + 0] * 255) / 63;
        gVal = (_cmap[3 * pal + 1] * 255) / 63;
        bVal = (_cmap[3 * pal + 2] * 255) / 63;
    }

    const int trackX = 75;
    const int trackW = 200;
    const int trackH = 14;

    windowRefresh(win);

    bool keepGoing = true;
    while (keepGoing) {
        sharedFpsLimiter.mark();
        MpTick();
        mouseShowCursor();

        // 1. Calculate live palette color from R, G, B
        int rgb24 = ((rVal & 0xFF) << 16) | ((gVal & 0xFF) << 8) | (bVal & 0xFF);
        int previewColor = _colorTable[rgb555(rgb24)];

        // 2. Redraw Live Preview Box (y = 28..76)
        windowFill(win, 20, 28, 300, 48, COLOR_BLACK);
        windowDrawRect(win, 20, 28, 20 + 300 - 1, 28 + 48 - 1, previewColor);

        const char* nameStr = MpDebugLocalPlayerNameOverride();
        if (nameStr == nullptr || nameStr[0] == '\0') {
            nameStr = critterGetName(gDude);
        }
        if (nameStr == nullptr || nameStr[0] == '\0') {
            nameStr = "PLAYER";
        }

        char labelBuf[64];
        snprintf(labelBuf, sizeof(labelBuf), "PREVIEW: %s", nameStr);
        int textW = fontGetStringWidth(labelBuf);
        int textX = (kWindowWidth - textW) / 2;

        windowDrawText(win, labelBuf, 0, textX, 34, previewColor);

        char rgbBuf[64];
        snprintf(rgbBuf, sizeof(rgbBuf), "R: %3d  G: %3d  B: %3d  (#%02X%02X%02X)",
            rVal, gVal, bVal, rVal, gVal, bVal);
        int rgbW = fontGetStringWidth(rgbBuf);
        windowDrawText(win, rgbBuf, 0, (kWindowWidth - rgbW) / 2, 56, COLOR_LIGHT_GREY);

        // 3. Update R Value & Slider Track (y = 92)
        windowFill(win, 42, 92, 28, 14, COLOR_BLACK);
        char rTxt[8];
        snprintf(rTxt, sizeof(rTxt), "%3d", rVal);
        windowDrawText(win, rTxt, 0, 42, 92, COLOR_WHITE);

        windowFill(win, trackX, 92, trackW, trackH, COLOR_BLACK);
        windowDrawRect(win, trackX, 92, trackX + trackW - 1, 92 + trackH - 1, COLOR_DARK_GREY);
        int rThumbX = trackX + (rVal * (trackW - 16) / 255);
        windowFill(win, rThumbX, 93, 16, 12, COLOR_LIGHT_RED);
        windowDrawRect(win, rThumbX, 93, rThumbX + 15, 93 + 11, COLOR_WHITE);

        // 4. Update G Value & Slider Track (y = 132)
        windowFill(win, 42, 132, 28, 14, COLOR_BLACK);
        char gTxt[8];
        snprintf(gTxt, sizeof(gTxt), "%3d", gVal);
        windowDrawText(win, gTxt, 0, 42, 132, COLOR_WHITE);

        windowFill(win, trackX, 132, trackW, trackH, COLOR_BLACK);
        windowDrawRect(win, trackX, 132, trackX + trackW - 1, 132 + trackH - 1, COLOR_DARK_GREY);
        int gThumbX = trackX + (gVal * (trackW - 16) / 255);
        windowFill(win, gThumbX, 133, 16, 12, COLOR_LIGHT_GREEN_3);
        windowDrawRect(win, gThumbX, 133, gThumbX + 15, 133 + 11, COLOR_WHITE);

        // 5. Update B Value & Slider Track (y = 172)
        windowFill(win, 42, 172, 28, 14, COLOR_BLACK);
        char bTxt[8];
        snprintf(bTxt, sizeof(bTxt), "%3d", bVal);
        windowDrawText(win, bTxt, 0, 42, 172, COLOR_WHITE);

        windowFill(win, trackX, 172, trackW, trackH, COLOR_BLACK);
        windowDrawRect(win, trackX, 172, trackX + trackW - 1, 172 + trackH - 1, COLOR_DARK_GREY);
        int bThumbX = trackX + (bVal * (trackW - 16) / 255);
        windowFill(win, bThumbX, 173, 16, 12, COLOR_PALE_BLUE);
        windowDrawRect(win, bThumbX, 173, bThumbX + 15, 173 + 11, COLOR_WHITE);

        windowRefresh(win);
        renderPresent();

        // 6. Direct Mouse Dragging / Clicking on RGB Tracks
        int mouseX, mouseY;
        mouseGetPosition(&mouseX, &mouseY);
        if ((mouse_get_last_buttons() & 1) != 0) {
            int localX = mouseX - (winX + trackX);
            int localY = mouseY - winY;
            if (localX >= 0 && localX <= trackW) {
                int newVal = std::clamp(localX * 255 / trackW, 0, 255);
                if (localY >= 86 && localY <= 110) {
                    rVal = newVal;
                } else if (localY >= 126 && localY <= 150) {
                    gVal = newVal;
                } else if (localY >= 166 && localY <= 190) {
                    bVal = newVal;
                }
            }
        }

        int keyCode = inputGetInput();
        if (keyCode == DBG_BTN_R_DEC) {
            rVal = std::clamp(rVal - 15, 0, 255);
        } else if (keyCode == DBG_BTN_R_INC) {
            rVal = std::clamp(rVal + 15, 0, 255);
        } else if (keyCode == DBG_BTN_G_DEC) {
            gVal = std::clamp(gVal - 15, 0, 255);
        } else if (keyCode == DBG_BTN_G_INC) {
            gVal = std::clamp(gVal + 15, 0, 255);
        } else if (keyCode == DBG_BTN_B_DEC) {
            bVal = std::clamp(bVal - 15, 0, 255);
        } else if (keyCode == DBG_BTN_B_INC) {
            bVal = std::clamp(bVal + 15, 0, 255);
        } else if (keyCode == DBG_BTN_COLOR_BASE + 8 || keyCode == KEY_RETURN) {
            int finalRgb = ((rVal & 0xFF) << 16) | ((gVal & 0xFF) << 8) | (bVal & 0xFF);
            int finalPal = _colorTable[rgb555(finalRgb)];
            MpDebugSetLocalPlayerColor(finalPal);
            MpLog(MP_LOG_UI, "RGB player color applied pal=%d (R=%d G=%d B=%d)",
                finalPal, rVal, gVal, bVal);
            if (_cmap != nullptr) {
                // What the chosen palette entry actually renders as — proves
                // whether the nearest-match mapping is faithful.
                MpLog(MP_LOG_UI, "pal %d renders RGB=(%d,%d,%d)",
                    finalPal,
                    (_cmap[3 * finalPal] * 255) / 63,
                    (_cmap[3 * finalPal + 1] * 255) / 63,
                    (_cmap[3 * finalPal + 2] * 255) / 63);
            }
            keepGoing = false;
        } else if (keyCode == KEY_ESCAPE || keyCode == 500
            || keyCode == DBG_BTN_COLOR_BASE + 9 || keyCode == DBG_BTN_CLOSE_X) {
            MpLog(MP_LOG_UI, "modal close via ESC/Close menu='color-picker'");
            keepGoing = false;
        }
        sharedFpsLimiter.throttle();
    }
    windowDestroy(win);
}

} // namespace

void MpDebugToggleClientCheats()
{
    if (!gMpActive || !gMpIsHost) {
        return;
    }
    gDbgClientCheatsEnabled = !gDbgClientCheatsEnabled;
    if (!gDbgClientCheatsEnabled) {
        // Revoke every connected client's cheat flags host-side; their local
        // copies are cleared by the policy packet below.
        for (int index = 1; index < NET_MAX_PLAYERS; index++) {
            MultiplayerPlayer* player = &gMpSession.players[index];
            if (player->isConnected && !player->isLocal) {
                player->debugCheatFlags = 0;
            }
        }
    }
    MpLog(MP_LOG_UI, "client cheats %s",
        gDbgClientCheatsEnabled ? "enabled" : "disabled");
    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        MultiplayerPlayer* player = &gMpSession.players[index];
        if (player->isConnected && !player->isLocal && player->peer != nullptr) {
            MpDebugSendCheatPolicyTo(player->peer);
        }
    }
}

void MpDebugSetClientCheatsEnabled(bool enabled)
{
    gDbgClientCheatsEnabled = enabled;
    if (!enabled) {
        // The host revoked client cheats: clear the local flags so the menu
        // reads OFF and no local effect keeps running.
        gDbgCheatFlags = 0;
        gDbgCheatFlagsDirty = false;
    }
    MpLog(MP_LOG_UI, "client cheats policy %s", enabled ? "enabled" : "disabled");
}

bool MpDebugClientCheatsEnabled()
{
    return gDbgClientCheatsEnabled;
}

void MpDebugSendCheatPolicyTo(ENetPeer* peer)
{
    if (peer == nullptr) {
        return;
    }
    NetCheatPolicyPayload payload;
    payload.clientCheatsEnabled = gDbgClientCheatsEnabled ? 1 : 0;
    NetSendPacket(peer, NET_CHANNEL_RELIABLE, NET_PKT_CHEAT_POLICY,
        &payload, sizeof(payload));
}

uint32_t MpDebugCheatFlagsFor(const Object* critter)
{
    return dbgCheatFlagsFor(critter);
}

bool MpDebugCheatEnabled(const Object* critter, uint32_t flag)
{
    return (dbgCheatFlagsFor(critter) & flag) != 0;
}

void MpDebugCheatsTick()
{
    if (gMpActive && gMpIsClient && gMpSession.hostPeer != nullptr) {
        uint32_t now = getTicks();
        if (gDbgCheatFlagsDirty || getTicksSince(gDbgCheatLastSyncTick) >= 1000) {
            NetPlayerCmdPayload payload;
            payload.opcode = NET_PLAYER_CMD_CHEAT_FLAGS;
            payload.arg1 = (int32_t)gDbgCheatFlags;
            payload.arg2 = 0;
            if (NetSendPacket(gMpSession.hostPeer, NET_CHANNEL_RELIABLE,
                    NET_PKT_PLAYER_CMD, &payload, sizeof(payload))) {
                gDbgCheatFlagsDirty = false;
                gDbgCheatLastSyncTick = now;
                // debugFilePrint("MPDBG: cheat flags sent flags=0x%X", gDbgCheatFlags);
            }
        }
    }

    if (gMpActive && gMpIsHost) {
        // Kill Hostile is one-shot: execute and clear the local flag before
        // it is mirrored into players[0] below (the cleared copy then rides
        // the next flag push instead of re-triggering).
        if ((gDbgCheatFlags & MP_DEBUG_CHEAT_KILL_HOSTILE) != 0) {
            dbgKillHostiles();
            gDbgCheatFlags &= ~MP_DEBUG_CHEAT_KILL_HOSTILE;
            gDbgCheatFlagsDirty = true;
        }
        gMpSession.players[0].debugCheatFlags = gDbgCheatFlags;
        for (int index = 0; index < NET_MAX_PLAYERS; index++) {
            MultiplayerPlayer* player = &gMpSession.players[index];
            if (player->isConnected && player->obj != nullptr) {
                // Client cheats apply only when the host enabled them; the
                // host's own flags are never gated.
                uint32_t flags = player->isLocal || gDbgClientCheatsEnabled
                    ? player->debugCheatFlags : 0;
                // A remote Kill Hostile request fires once and clears.
                if ((flags & MP_DEBUG_CHEAT_KILL_HOSTILE) != 0) {
                    dbgKillHostiles();
                    flags &= ~MP_DEBUG_CHEAT_KILL_HOSTILE;
                    if (!player->isLocal) {
                        player->debugCheatFlags = flags;
                    }
                }
                dbgApplyCheats(player->obj, flags);
            }
        }
    } else if (gMpActive && gMpIsClient) {
        // The KILL bit rode the send above; the host executes and clears its
        // copy. Clear the local bit so it never re-sends.
        if ((gDbgCheatFlags & MP_DEBUG_CHEAT_KILL_HOSTILE) != 0) {
            gDbgCheatFlags &= ~MP_DEBUG_CHEAT_KILL_HOSTILE;
            gDbgCheatFlagsDirty = true;
        }
        if (gDbgClientCheatsEnabled) {
            dbgApplyCheats(gDude, gDbgCheatFlags);
        }
    } else {
        if ((gDbgCheatFlags & MP_DEBUG_CHEAT_KILL_HOSTILE) != 0) {
            dbgKillHostiles();
            gDbgCheatFlags &= ~MP_DEBUG_CHEAT_KILL_HOSTILE;
        }
        dbgApplyCheats(gDude, gDbgCheatFlags);
    }
}

// === Cheats menu (the old F11 debug editor — everything except the co-op
// settings now lives here, opened via the Cheats button in the settings
// menu) ===
static void dbgCheatsMenuShow()
{
    MpLog(MP_LOG_UI, "cheats menu begin");
    // Client gate: the whole cheats area (money/heal/xp editors included) is
    // host-policy controlled — a disabled client gets no access at all.
    if (gMpActive && gMpIsClient && !gDbgClientCheatsEnabled) {
        MpLogAlways(MP_LOG_UI, "cheats menu blocked (disabled by host)");
        displayMonitorAddMessage("Cheats are disabled by the host.");
        return;
    }
    if (gDude == nullptr) {
        return;
    }
    bool cursorWasHidden = cursorIsHidden();
    if (cursorWasHidden) {
        mouseShowCursor();
    }

    int winX, winY;
    dbgCenteredPos(kDbgWindowWidth, kDbgWindowHeight, &winX, &winY);

    int win = windowCreate(winX, winY, kDbgWindowWidth, kDbgWindowHeight, COLOR_BLACK, WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    if (win == -1) {
        if (cursorWasHidden) {
            mouseHideCursor();
        }
        return;
    }
    windowDrawBorder(win);

    const char* title = "CHEATS MENU";
    int titleX = (kDbgWindowWidth - fontGetStringWidth(title)) / 2;
    windowDrawText(win, title, 0, titleX, 6, COLOR_WHITE);

    _win_register_text_button(win, 30, 30, -1, -1, -1, DBG_BTN_MONEY_1000, "Money +1000", 0);
    _win_register_text_button(win, 170, 30, -1, -1, -1, DBG_BTN_MONEY_100, "Money +100", 0);
    _win_register_text_button(win, 30, 55, -1, -1, -1, DBG_BTN_HEAL_FULL, "Heal Full", 0);
    _win_register_text_button(win, 170, 55, -1, -1, -1, DBG_BTN_HP_50, "HP +50", 0);
    _win_register_text_button(win, 30, 80, -1, -1, -1, DBG_BTN_XP_1000, "XP +1000", 0);
    _win_register_text_button(win, 170, 80, -1, -1, -1, DBG_BTN_XP_5000, "XP +5000", 0);
    _win_register_text_button(win, 30, 105, -1, -1, -1, DBG_BTN_SP_10, "Skill Pts +10", 0);
    _win_register_text_button(win, 170, 105, -1, -1, -1, DBG_BTN_LEVEL_1, "Level +1", 0);
    _win_register_text_button(win, 30, 130, -1, -1, -1, DBG_BTN_AP_REFILL, "AP Refill", 0);
    _win_register_text_button(win, 170, 130, -1, -1, -1, DBG_BTN_FULL_RESTORE, "Full Restore", 0);
    _win_register_text_button(win, 30, 155, -1, -1, -1, DBG_BTN_STIMPAK, "Stimpak +5", 0);
    _win_register_text_button(win, 170, 155, -1, -1, -1, DBG_BTN_SUPER_STIMPAK, "Super Stimpak", 0);
    _win_register_text_button(win, 30, 180, -1, -1, -1, DBG_BTN_RADAWAY, "RadAway +5", 0);
    _win_register_text_button(win, 170, 180, -1, -1, -1, DBG_BTN_AMMO, "Ammo Refill", 0);
    _win_register_text_button(win, 30, 205, -1, -1, -1, DBG_BTN_MAX_LEVEL, "Max Level", 0);
    _win_register_text_button(win, 170, 205, -1, -1, -1, DBG_BTN_ITEMS, "Items...", 0);
    _win_register_text_button(win, 30, 230, -1, -1, -1, DBG_BTN_SKILLS, "Skills...", 0);
    _win_register_text_button(win, 170, 230, -1, -1, -1, DBG_BTN_STATS, "Stats...", 0);
    _win_register_text_button(win, 30, 255, -1, -1, -1, DBG_BTN_PERKS, "Perks...", 0);
    _win_register_text_button(win, 170, 255, -1, -1, -1, DBG_BTN_CHEATS, "Cheat Options...", 0);
    _win_register_text_button(win, 30, 280, -1, -1, -1, DBG_BTN_CLOSE, "Close", 0);
    _win_register_text_button(win, 170, 280, -1, -1, -1, DBG_BTN_TRAITS, "Traits...", 0);
    windowRefresh(win);

    SubmenuCallbacks skillsCb {
        "SKILLS", SKILL_COUNT, 10, dbgSkillName, dbgSkillValue, dbgSkillModify,
    };
    SubmenuCallbacks statsCb {
        "STATS", 7, 1, dbgStatName, dbgStatValue, dbgStatModify,
    };
    SubmenuCallbacks itemsCb {
        "ITEMS", (int)(sizeof(kDbgItems) / sizeof(kDbgItems[0])), 5,
        dbgItemName, dbgItemValue, dbgItemModify,
    };

    bool keepGoing = true;
    while (keepGoing) {
        int rc = -1;
        while (rc == -1) {
            sharedFpsLimiter.mark();

            // Keep the session alive behind the modal: the profile sync,
            // deferred drains and host detect all run from MpTick, which the
            // main loop cannot reach while this modal blocks it. Same pattern
            // as the vote modal. No-ops when not in a session.
            MpTick();
            mouseShowCursor();
            int keyCode = inputGetInput();
            switch (keyCode) {
            case KEY_ESCAPE:
                MpLog(MP_LOG_UI, "modal close via ESC menu='cheats-menu'");
                rc = 0;
                break;
            case DBG_BTN_MONEY_1000:
            case DBG_BTN_MONEY_100:
            case DBG_BTN_HEAL_FULL:
            case DBG_BTN_HP_50:
            case DBG_BTN_XP_1000:
            case DBG_BTN_XP_5000:
            case DBG_BTN_SP_10:
            case DBG_BTN_LEVEL_1:
            case DBG_BTN_AP_REFILL:
            case DBG_BTN_FULL_RESTORE:
            case DBG_BTN_STIMPAK:
            case DBG_BTN_SUPER_STIMPAK:
            case DBG_BTN_RADAWAY:
            case DBG_BTN_AMMO:
            case DBG_BTN_MAX_LEVEL:
            case DBG_BTN_SKILLS:
            case DBG_BTN_STATS:
            case DBG_BTN_PERKS:
            case DBG_BTN_ITEMS:
            case DBG_BTN_TRAITS:
            case DBG_BTN_CHEATS:
            case DBG_BTN_CLOSE:
                rc = keyCode;
                break;
            default:
                break;
            }

            renderPresent();
            sharedFpsLimiter.throttle();
        }

        // Individual-action gate (defense in depth): even if this menu is
        // somehow open, every action is inert for a disabled client. Close
        // and ESC always pass so the window can always be exited.
        if (rc != 0 && rc != DBG_BTN_CLOSE && gMpActive && gMpIsClient
            && !gDbgClientCheatsEnabled) {
            MpLogAlways(MP_LOG_UI, "cheats action blocked (disabled by host) rc=%d", rc);
            displayMonitorAddMessage("Cheats are disabled by the host.");
            continue;
        }

        switch (rc) {
        case 0:
        case DBG_BTN_CLOSE:
            keepGoing = false;
            break;
        case DBG_BTN_MONEY_1000:
            itemCapsAdjust(gDude, 1000);
            break;
        case DBG_BTN_MONEY_100:
            itemCapsAdjust(gDude, 100);
            break;
        case DBG_BTN_HEAL_FULL:
            dbgHeal(0);
            break;
        case DBG_BTN_HP_50:
            dbgHeal(50);
            break;
        case DBG_BTN_XP_1000:
            pcAddExperience(1000, nullptr);
            break;
        case DBG_BTN_XP_5000:
            pcAddExperience(5000, nullptr);
            break;
        case DBG_BTN_SP_10:
            pcSetStat(PC_STAT_UNSPENT_SKILL_POINTS, pcGetStat(PC_STAT_UNSPENT_SKILL_POINTS) + 10);
            break;
        case DBG_BTN_LEVEL_1: {
            int xpToNext = pcGetExperienceForNextLevel() - pcGetStat(PC_STAT_EXPERIENCE);
            if (xpToNext > 0) {
                pcAddExperience(xpToNext, nullptr);
            }
            break;
        }
        case DBG_BTN_AP_REFILL:
            dbgRefillAp();
            break;
        case DBG_BTN_FULL_RESTORE:
            dbgHeal(0);
            dbgRefillAp();
            break;
        case DBG_BTN_STIMPAK:
            dbgGiveItem(PROTO_ID_STIMPAK, 5);
            break;
        case DBG_BTN_SUPER_STIMPAK:
            dbgGiveItem(PROTO_ID_SUPER_STIMPAK, 5);
            break;
        case DBG_BTN_RADAWAY:
            dbgGiveItem(PROTO_ID_RADAWAY, 5);
            break;
        case DBG_BTN_AMMO:
            dbgRefillAmmo();
            break;
        case DBG_BTN_MAX_LEVEL:
            dbgMaxLevel();
            break;
        case DBG_BTN_SKILLS:
            dbgSubmenuShow(&skillsCb);
            break;
        case DBG_BTN_STATS:
            dbgSubmenuShow(&statsCb);
            break;
        case DBG_BTN_PERKS:
            dbgPerkListShow();
            break;
        case DBG_BTN_ITEMS:
            dbgItemBrowserShow();
            break;
        case DBG_BTN_TRAITS:
            dbgTraitListShow();
            break;
        case DBG_BTN_CHEATS:
            dbgCheatModal();
            break;
        }
    }

    windowDestroy(win);
    if (cursorWasHidden) {
        mouseHideCursor();
    }
    MpLog(MP_LOG_UI, "cheats menu end");
}

// === MpDebugMenuShow ===
// Forward: the skin status helper is defined with the skin-picker block below
// but drawn from the settings menu.
static void dbgSkinStatusText(char* buf, size_t size);

void MpDebugMenuShow()
{
    MpLog(MP_LOG_UI, "menu show begin");
    if (gDude == nullptr) {
        return;
    }
    bool cursorWasHidden = cursorIsHidden();
    if (cursorWasHidden) {
        mouseShowCursor();
    }

    int winX, winY;
    dbgCenteredPos(kDbgWindowWidth, kDbgWindowHeight, &winX, &winY);

    int win = windowCreate(winX, winY, kDbgWindowWidth, kDbgWindowHeight, COLOR_BLACK, WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    if (win == -1) {
        if (cursorWasHidden) {
            mouseHideCursor();
        }
        return;
    }
    windowDrawBorder(win);

    const char* title = "CO-OP SETTINGS";
    int titleX = (kDbgWindowWidth - fontGetStringWidth(title)) / 2;
    windowDrawText(win, title, 0, titleX, 6, COLOR_WHITE);

    _win_register_text_button(win, 30, 30, -1, -1, -1, DBG_BTN_CHEATS, "Cheats...", 0);
    _win_register_text_button(win, 30, 55, -1, -1, -1, DBG_BTN_CLOSE, "Close", 0);

    // --- Settings column (right side): the perf meter is a per-machine
    // render toggle available to everyone (singleplayer included); the
    // co-op host policy toggles sit below it ---
    char perfStatus[64];
    snprintf(perfStatus, sizeof(perfStatus), "Perf: %s", MpPerfIsEnabled() ? "ON" : "OFF");
    _win_register_text_button(win, 330, 30, -1, -1, -1, DBG_BTN_PERF_METER, "Perf Meter", 0);
    windowDrawText(win, perfStatus, 0, 330, 58, COLOR_WHITE);
    char clientCheatsStatus[64];
    if (gMpActive) {
        if (gMpIsHost) {
            // Host: toggle button; the status line below carries the state
            // (buttons keep a static label, so the state is drawn here and
            // redrawn after each toggle).
            _win_register_text_button(win, 330, 88, -1, -1, -1, DBG_BTN_CLIENT_CHEATS, "Client Cheats", 0);
            snprintf(clientCheatsStatus, sizeof(clientCheatsStatus), "Clients: %s",
                gDbgClientCheatsEnabled ? "ON" : "OFF");
            windowDrawText(win, clientCheatsStatus, 0, 330, 116, COLOR_WHITE);
        } else {
            // Client: informational only — the host controls this setting.
            windowDrawText(win, "Client Cheats:", 0, 330, 92, COLOR_WHITE);
            snprintf(clientCheatsStatus, sizeof(clientCheatsStatus), "%s",
                gDbgClientCheatsEnabled ? "enabled" : "disabled by host");
            windowDrawText(win, clientCheatsStatus, 0, 330, 110, COLOR_WHITE);
        }
    }
    // Skin picker: per-machine appearance override, available to everyone
    // (singleplayer included — it is the local dude's look either way).
    char skinStatus[64];
    dbgSkinStatusText(skinStatus, sizeof(skinStatus));
    _win_register_text_button(win, 330, 146, -1, -1, -1, DBG_BTN_SKIN, "Skin...", 0);
    windowDrawText(win, skinStatus, 0, 330, 174, COLOR_WHITE);
    // Player identity: rename and recolor. Both ride the profile (IDENTITY
    // wire) to every machine and persist via the COOP save handler.
    char nameStatus[64];
    snprintf(nameStatus, sizeof(nameStatus), "Name: %.20s",
        MpDebugLocalPlayerNameOverride() != nullptr ? MpDebugLocalPlayerNameOverride() : "");
    _win_register_text_button(win, 330, 204, -1, -1, -1, DBG_BTN_NAME, "Name...", 0);
    windowDrawText(win, nameStatus, 0, 330, 232, COLOR_WHITE);
    char colorStatus[64];
    int curColor = MpDebugLocalPlayerColor();
    snprintf(colorStatus, sizeof(colorStatus), "Color: %s",
        curColor >= 0 && curColor < 8 ? kPlayerPaletteNames[curColor] : "default");
    _win_register_text_button(win, 330, 262, -1, -1, -1, DBG_BTN_COLOR, "Color...", 0);
    windowDrawText(win, colorStatus, 0, 330, 290, COLOR_WHITE);
    windowRefresh(win);

    bool keepGoing = true;
    while (keepGoing) {
        int rc = -1;
        while (rc == -1) {
            sharedFpsLimiter.mark();

            // Keep the session alive behind the modal: the profile sync,
            // deferred drains and host detect all run from MpTick, which the
            // main loop cannot reach while this modal blocks it. Same pattern
            // as the vote modal. No-ops when not in a session.
            MpTick();
            mouseShowCursor();
            int keyCode = inputGetInput();
            switch (keyCode) {
            case KEY_ESCAPE:
                MpLog(MP_LOG_UI, "modal close via ESC menu='f11-settings'");
                rc = 0;
                break;
            case DBG_BTN_CHEATS:
            case DBG_BTN_SKIN:
            case DBG_BTN_NAME:
            case DBG_BTN_COLOR:
            case DBG_BTN_CLOSE:
                rc = keyCode;
                break;
            case DBG_BTN_PERF_METER:
                MpPerfSetEnabled(!MpPerfIsEnabled());
                // Redraw the toggle status line (the button label is static).
                snprintf(perfStatus, sizeof(perfStatus), "Perf: %s",
                    MpPerfIsEnabled() ? "ON" : "OFF");
                windowFill(win, 330, 56, 130, 20, COLOR_BLACK);
                windowDrawText(win, perfStatus, 0, 330, 58, COLOR_WHITE);
                windowRefresh(win);
                break;
            case DBG_BTN_CLIENT_CHEATS:
                MpDebugToggleClientCheats();
                // Redraw the toggle status line (the button label is static).
                snprintf(clientCheatsStatus, sizeof(clientCheatsStatus), "Clients: %s",
                    gDbgClientCheatsEnabled ? "ON" : "OFF");
                windowFill(win, 330, 114, 130, 20, COLOR_BLACK);
                windowDrawText(win, clientCheatsStatus, 0, 330, 116, COLOR_WHITE);
                windowRefresh(win);
                break;
            default:
                break;
            }

            renderPresent();
            sharedFpsLimiter.throttle();
        }

        switch (rc) {
        case 0:
        case DBG_BTN_CLOSE:
            keepGoing = false;
            break;
        case DBG_BTN_CHEATS:
            if (gMpActive && gMpIsClient && !gDbgClientCheatsEnabled) {
                MpLogAlways(MP_LOG_UI, "cheats menu blocked (disabled by host)");
                displayMonitorAddMessage("Cheats are disabled by the host.");
            } else {
                dbgCheatsMenuShow();
            }
            break;
        case DBG_BTN_SKIN:
            MpDebugModelPickerShow();
            // Redraw the skin status line after the picker returned.
            dbgSkinStatusText(skinStatus, sizeof(skinStatus));
            windowFill(win, 330, 172, 130, 20, COLOR_BLACK);
            windowDrawText(win, skinStatus, 0, 330, 174, COLOR_WHITE);
            windowRefresh(win);
            break;
        case DBG_BTN_NAME:
            dbgNameEditorShow();
            snprintf(nameStatus, sizeof(nameStatus), "Name: %.20s",
                MpDebugLocalPlayerNameOverride() != nullptr ? MpDebugLocalPlayerNameOverride() : "");
            windowFill(win, 330, 230, 130, 20, COLOR_BLACK);
            windowDrawText(win, nameStatus, 0, 330, 232, COLOR_WHITE);
            windowRefresh(win);
            break;
        case DBG_BTN_COLOR:
            dbgColorPickerShow();
            // Diagnostic: the menu window handle must still be valid after
            // the picker's create/destroy (seen: windowRefresh AV in the
            // post-picker redraw on the host).
            MpLog(MP_LOG_UI, "color picker returned win=%d valid=%d buf=%p",
                win, windowGetWindow(win) != nullptr ? 1 : 0,
                windowGetWindow(win) != nullptr ? (void*)windowGetWindow(win)->buffer : nullptr);
            curColor = MpDebugLocalPlayerColor();
            snprintf(colorStatus, sizeof(colorStatus), "Color: %s",
                curColor >= 0 && curColor < 8 ? kPlayerPaletteNames[curColor] : "default");
            // NOTE: the window is kDbgWindowHeight=305 tall, so a 20px fill
            // from y=288 writes rows 288..307 — 3 rows (1,380 bytes) past
            // the buffer. That overrun corrupted the heap on every color
            // apply and crashed the host in a later windowFill (AV in
            // memset). Height 16 covers the status text at y=290 and stays
            // inside the buffer.
            windowFill(win, 330, 288, 130, 16, COLOR_BLACK);
            windowDrawText(win, colorStatus, 0, 330, 290, COLOR_WHITE);
            windowRefresh(win);
            break;
        }
    }

    windowDestroy(win);
    if (cursorWasHidden) {
        mouseHideCursor();
    }
    MpLog(MP_LOG_UI, "menu show end");
}

// ---------------------------------------------------------------------------
// Skin picker: a per-machine appearance override. The choice is written into
// the dude's proto fid so the periodic profile sync (change-detected on the
// model NAME) propagates it to every machine; vanilla models resolve by name
// on the receivers. The override wins over the armor appearance on the
// picker's own machine (inventoryComputeCritterFid consults it), and the
// state-stream FID carries the picked look to everyone else.
// ---------------------------------------------------------------------------

// -1 = no override (the config default / armor decides), otherwise the picked
// critter model index.
static int gDbgSkinOverrideModel = -1;

int MpDebugSkinOverrideModel()
{
    return gDbgSkinOverrideModel;
}

// Fills the "Skin: <model>" status text with the dude's current base model
// name (the proto FID model, i.e. what the picker would restore to).
static const char* dbgCurrentModelName()
{
    if (gDude != nullptr) {
        Proto* proto = nullptr;
        if (protoGetProto(gDude->pid, &proto) == 0 && proto != nullptr) {
            const char* name = artGetCritterModelName(proto->critter.fid & 0xFFF);
            if (name != nullptr && name[0] != '\0') {
                return name;
            }
        }
    }
    return "?";
}

static void dbgSkinStatusText(char* buf, size_t size)
{
    snprintf(buf, size, "Skin: %.12s", dbgCurrentModelName());
}

void MpDebugApplyModel(int modelIndex)
{
    if (gDude == nullptr || modelIndex < 0 || modelIndex >= artGetCritterModelCount()) {
        return;
    }
    Proto* proto = nullptr;
    if (protoGetProto(gDude->pid, &proto) == -1 || proto == nullptr) {
        MpLogAlways(MP_LOG_UI, "skin apply protoGetProto failed pid=0x%X", gDude->pid);
        return;
    }
    gDbgSkinOverrideModel = modelIndex;
    int preFid = gDude->fid;
    // The profile capture reads the proto FID model — writing the pick here
    // makes the periodic profile sync detect and propagate it.
    proto->critter.fid = (proto->critter.fid & ~0xFFF) | (modelIndex & 0xFFF);
    int fid = buildFid(OBJ_TYPE_CRITTER, modelIndex, ANIM_STAND,
        weaponAnimationFromFid(gDude->fid), gDude->rotation + 1);
    Rect rect;
    objectSetFid(gDude, fid, &rect);
    tileWindowRefreshRect(&rect, gDude->elevation);
    const char* name = artGetCritterModelName(modelIndex);
    MpLog(MP_LOG_UI, "skin applied index=%d name='%.12s' preFid=0x%X newFid=0x%X busy=%d",
        modelIndex, name != nullptr ? name : "?", preFid, fid,
        animationIsBusy(gDude) != 0 ? 1 : 0);
    // Push the model change immediately — the periodic profile sync runs on
    // a 1s cadence, and the pick should reach the other player right away.
    MpProfileForceSync();
}

void MpDebugRestoreSkin()
{
    if (gDude == nullptr) {
        return;
    }
    Proto* proto = nullptr;
    if (protoGetProto(gDude->pid, &proto) == -1 || proto == nullptr) {
        return;
    }
    gDbgSkinOverrideModel = -1;
    // The config's per-gender default is the vanilla look (the fork's ce.dat
    // ships it; the code fallback is the jumpsuit per gender).
    bool female = critterGetStat(gDude, STAT_GENDER) == GENDER_FEMALE;
    const char* defaultName = female ? "hfjmps" : "hmjmps";
    configGetString(&gContentConfig, CONTENT_CONFIG_START_SECTION,
        female ? "model_female_default" : "model_male_default",
        const_cast<char**>(&defaultName), defaultName);
    int modelId = -1;
    if (defaultName != nullptr && defaultName[0] != '\0') {
        modelId = artListIndex(OBJ_TYPE_CRITTER, defaultName);
    }
    if (modelId < 0) {
        MpLogAlways(MP_LOG_UI, "skin restore default model not found name='%s'",
            defaultName != nullptr ? defaultName : "?");
        return;
    }
    proto->critter.fid = (proto->critter.fid & ~0xFFF) | (modelId & 0xFFF);
    // Armor decides the look again after a restore (vanilla behavior).
    int model = modelId;
    Object* armor = critterGetArmor(gDude);
    if (armor != nullptr) {
        Proto* armorProto = nullptr;
        if (protoGetProto(armor->pid, &armorProto) == 0 && armorProto != nullptr) {
            int armorFid = female
                ? armorProto->item.data.armor.femaleFid
                : armorProto->item.data.armor.maleFid;
            if (armorFid != -1) {
                model = armorFid;
            }
        }
    }
    int fid = buildFid(OBJ_TYPE_CRITTER, model, ANIM_STAND,
        weaponAnimationFromFid(gDude->fid), gDude->rotation + 1);
    Rect rect;
    objectSetFid(gDude, fid, &rect);
    tileWindowRefreshRect(&rect, gDude->elevation);
    MpLog(MP_LOG_UI, "skin restored model=%d override cleared", model);
}


// ---------------------------------------------------------------------------
// Player identity overrides: the local name + highlight color. Both ride the
// profile IDENTITY section to the other machines and persist through the COOP
// save-section handler (loadsave.cc); the color drives the per-player ring,
// the floating name label and the combat card borders.
// ---------------------------------------------------------------------------
static char gMpLocalPlayerName[MP_PROFILE_NAME_LENGTH] = {};
static int gMpLocalPlayerColor = -1;

const char* MpDebugLocalPlayerNameOverride()
{
    return gMpLocalPlayerName[0] != '\0' ? gMpLocalPlayerName : nullptr;
}

int MpDebugLocalPlayerColor()
{
    return gMpLocalPlayerColor;
}

void MpDebugSetLocalPlayerName(const char* name)
{
    if (name == nullptr || name[0] == '\0') {
        gMpLocalPlayerName[0] = '\0';
        MpLog(MP_LOG_UI, "local player name cleared");
        return;
    }
    strncpy(gMpLocalPlayerName, name, MP_PROFILE_NAME_LENGTH - 1);
    gMpLocalPlayerName[MP_PROFILE_NAME_LENGTH - 1] = '\0';
    // The player slot name drives the floating tag and the combat cards —
    // refresh it immediately so the local game shows the new name without
    // waiting for a profile round trip.
    if (gMpActive && gMpSession.localNetId > 0
        && gMpSession.localNetId <= NET_MAX_PLAYERS) {
        MultiplayerPlayer* local = &gMpSession.players[gMpSession.localNetId - 1];
        if (local->isConnected) {
            strncpy(local->name, gMpLocalPlayerName, NET_PEER_NAME_LENGTH - 1);
            local->name[NET_PEER_NAME_LENGTH - 1] = '\0';
        }
        // The character screen (F2) reads critterGetName(gDude), which
        // resolves through the local runtime profile FIRST. The host never
        // echoes a rename back to its owner (skipOwner), so without this the
        // client's own runtime profile would keep the old name forever and
        // the F2 screen would never update.
        MpPlayerRuntime* localRuntime = MpProfileGetRuntime(gMpSession.localNetId);
        if (localRuntime != nullptr) {
            strncpy(localRuntime->profile.name, gMpLocalPlayerName,
                MP_PROFILE_NAME_LENGTH - 1);
            localRuntime->profile.name[MP_PROFILE_NAME_LENGTH - 1] = '\0';
        }
    }
    MpLog(MP_LOG_UI, "local player name set '%s'", gMpLocalPlayerName);
}

void MpDebugSetLocalPlayerColor(int colorIndex)
{
    gMpLocalPlayerColor = colorIndex;
    MpLog(MP_LOG_UI, "local player color set %d", colorIndex);
}

int MpPlayerColorFor(const Object* obj)
{
    if (!gMpActive || obj == nullptr) {
        return -1;
    }
    int colorIndex = -1;
    int playerNetId = 0;
    if (obj == gDude) {
        colorIndex = gMpLocalPlayerColor;
        playerNetId = gMpSession.localNetId;
    }
    uint32_t objNetId = MpGetObjNetId(const_cast<Object*>(obj));
    if (colorIndex < 0) {
        for (int index = 0; index < NET_MAX_PLAYERS; index++) {
            const MultiplayerPlayer* p = &gMpSession.players[index];
            if (p->isConnected) {
                if (p->obj == obj || (objNetId != 0 && p->objNetId == objNetId)) {
                    playerNetId = p->netId;
                    MpPlayerRuntime* runtime = MpProfileGetRuntime(p->netId);
                    if (runtime != nullptr) {
                        colorIndex = runtime->profile.playerColor;
                    }
                    break;
                }
            }
        }
    }
    if (colorIndex < 0) {
        for (int netId = 1; netId <= NET_MAX_PLAYERS; netId++) {
            MpPlayerRuntime* runtime = MpProfileGetRuntime(netId);
            if (runtime != nullptr && runtime->object == obj) {
                playerNetId = netId;
                colorIndex = runtime->profile.playerColor;
                break;
            }
        }
    }
    // Default fallback when playerColor is unset (-1): assign distinct per-player preset
    if (colorIndex < 0 && playerNetId >= 1 && playerNetId <= NET_MAX_PLAYERS) {
        colorIndex = (playerNetId - 1) % 8;
    }
    // Evaluate legacy preset indices (0..7) to macro palette colors.
    if (colorIndex >= 0 && colorIndex < 8) {
        switch (colorIndex) {
        case 0: return COLOR_CYAN;
        case 1: return COLOR_RED;
        case 2: return COLOR_LIGHT_GREEN_3;
        case 3: return COLOR_BLUE;
        case 4: return COLOR_LIGHT_YELLOW;
        case 5: return COLOR_LIGHT_PINK;
        case 6: return COLOR_LIGHT_ORANGE;
        case 7: return COLOR_LIGHT_GOLD;
        default: break;
        }
    }
    // Direct palette color index (0..255) from the RGB slider.
    if (colorIndex >= 0 && colorIndex < 256) {
        return colorIndex;
    }
    return -1;
}

void MpDebugModelPickerShow()
{
    constexpr int kWidth = 640;
    constexpr int kHeight = 440;
    constexpr int kCatRows = 9;
    constexpr int kModelRows = 12;

    // Flatten the runtime critter art list, dropping reserved/empty slots,
    // then sort by name so the two-letter prefix groups fall together.
    struct SkinModel { int index; char name[13]; };
    std::vector<SkinModel> models;
    {
        int count = artGetCritterModelCount();
        for (int i = 0; i < count; i++) {
            const char* name = artGetCritterModelName(i);
            if (name == nullptr || name[0] == '\0') {
                continue;
            }
            SkinModel model;
            strncpy(model.name, name, 12);
            model.name[12] = '\0';
            if (compat_stricmp(model.name, "reserv") == 0) {
                continue;
            }
            model.index = i;
            models.push_back(model);
        }
        std::stable_sort(models.begin(), models.end(),
            [](const SkinModel& a, const SkinModel& b) {
                return compat_stricmp(a.name, b.name) < 0;
            });
    }

    // Category runs: equal two-letter prefixes.
    struct SkinCategory { char prefix[3]; int start; int count; };
    std::vector<SkinCategory> categories;
    for (size_t i = 0; i < models.size();) {
        char prefix[3] = {
            (char)tolower((unsigned char)models[i].name[0]),
            (char)tolower((unsigned char)models[i].name[1]),
            '\0',
        };
        size_t end = i + 1;
        while (end < models.size()) {
            char nextPrefix[3] = {
                (char)tolower((unsigned char)models[end].name[0]),
                (char)tolower((unsigned char)models[end].name[1]),
                '\0',
            };
            if (strncmp(nextPrefix, prefix, 2) != 0) {
                break;
            }
            end++;
        }
        SkinCategory category;
        memcpy(category.prefix, prefix, sizeof(category.prefix));
        category.start = (int)i;
        category.count = (int)(end - i);
        categories.push_back(category);
        i = end;
    }
    if (categories.empty()) {
        MpLog(MP_LOG_UI, "skin picker no categories (art list empty)");
        return;
    }

    auto skinCategoryLabel = [](const char* prefix) -> const char* {
        struct Entry {
            const char* prefix;
            const char* label;
        };
        static const Entry kEntries[] = {
            { "hm", "Human Male" },
            { "hf", "Human Female" },
            { "ha", "Humans (armor)" },
            { "ma", "Creatures & Mutants" },
            { "na", "Ghouls & Oddities" },
            { "nf", "NPC Female" },
            { "nm", "NPC Male" },
        };
        for (const Entry& entry : kEntries) {
            if (compat_stricmp(prefix, entry.prefix) == 0) {
                return entry.label;
            }
        }
        return nullptr;
    };

    // Open on the category that holds the dude's current base model.
    int catSel = 0;
    {
        Proto* proto = nullptr;
        if (gDude != nullptr && protoGetProto(gDude->pid, &proto) == 0 && proto != nullptr) {
            const char* current = artGetCritterModelName(proto->critter.fid & 0xFFF);
            if (current != nullptr && current[0] != '\0') {
                for (size_t i = 0; i < models.size(); i++) {
                    if (strncmp(models[i].name, current, 12) == 0) {
                        for (size_t c = 0; c < categories.size(); c++) {
                            if ((int)i >= categories[c].start
                                && (int)i < categories[c].start + categories[c].count) {
                                catSel = (int)c;
                                break;
                            }
                        }
                        break;
                    }
                }
            }
        }
    }

    int catPage = 0;
    int modelPage = 0;
    int win = -1;

    auto rebuild = [&]() {
        if (win != -1) {
            windowDestroy(win);
        }
        int winX, winY;
        dbgCenteredPos(kWidth, kHeight, &winX, &winY);
        win = windowCreate(winX, winY, kWidth, kHeight, COLOR_BLACK,
            WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
        if (win == -1) {
            return;
        }
        windowDrawBorder(win);

        const char* title = "SKIN PICKER";
        windowDrawText(win, title, 0, (kWidth - fontGetStringWidth(title)) / 2, 6, COLOR_WHITE);
        char status[64];
        snprintf(status, sizeof(status), "Current: %.12s", dbgCurrentModelName());
        windowDrawText(win, status, 0, 30, 28, COLOR_WHITE);
        windowDrawText(win, "Category", 0, 30, 48, COLOR_WHITE);
        windowDrawText(win, "Models", 0, 250, 48, COLOR_WHITE);

        int catStart = catPage * kCatRows;
        int catShown = std::min(kCatRows, (int)categories.size() - catStart);
        for (int i = 0; i < catShown; i++) {
            int categoryIndex = catStart + i;
            const char* label = skinCategoryLabel(categories[categoryIndex].prefix);
            char fallback[32];
            if (label == nullptr) {
                snprintf(fallback, sizeof(fallback), "Other: %s", categories[categoryIndex].prefix);
                label = fallback;
            }
            _win_register_text_button(win, 30, 62 + i * 26, -1, -1, -1,
                DBG_BTN_SKIN_CAT_BASE + i, label, 0);
        }

        const SkinCategory& category = categories[catSel];
        int modelStart = modelPage * kModelRows;
        int modelShown = std::min(kModelRows, category.count - modelStart);
        for (int i = 0; i < modelShown; i++) {
            const SkinModel& model = models[category.start + modelStart + i];
            _win_register_text_button(win, 250, 62 + i * 26, -1, -1, -1,
                DBG_BTN_SKIN_MODEL_BASE + i, model.name, 0);
        }

        _win_register_text_button(win, 30, 300, -1, -1, -1, DBG_BTN_SKIN_CAT_PREV, "Cat Prev", 0);
        _win_register_text_button(win, 170, 300, -1, -1, -1, DBG_BTN_SKIN_CAT_NEXT, "Cat Next", 0);
        _win_register_text_button(win, 250, 380, -1, -1, -1, DBG_BTN_SKIN_MODEL_PREV, "Prev", 0);
        _win_register_text_button(win, 370, 380, -1, -1, -1, DBG_BTN_SKIN_MODEL_NEXT, "Next", 0);
        _win_register_text_button(win, 30, 410, -1, -1, -1, DBG_BTN_SKIN_RESTORE, "Restore Vanilla", 0);
        _win_register_text_button(win, 250, 410, -1, -1, -1, DBG_BTN_SKIN_BACK, "Back", 0);
        windowRefresh(win);
    };

    rebuild();

    bool keepGoing = true;
    while (keepGoing) {
        sharedFpsLimiter.mark();

        // Keep the session alive behind the modal (profile sync runs from
        // MpTick, so a picked skin propagates even while the picker is open).
        MpTick();
        mouseShowCursor();

        int keyCode = inputGetInput();
        switch (keyCode) {
        case KEY_ESCAPE:
            MpLog(MP_LOG_UI, "modal close via ESC menu='skin-picker'");
        case DBG_BTN_SKIN_BACK:
            keepGoing = false;
            break;
        case DBG_BTN_SKIN_RESTORE:
            MpDebugRestoreSkin();
            rebuild();
            break;
        case DBG_BTN_SKIN_CAT_PREV:
            if (catPage > 0) {
                catPage--;
                rebuild();
            }
            break;
        case DBG_BTN_SKIN_CAT_NEXT:
            if ((catPage + 1) * kCatRows < (int)categories.size()) {
                catPage++;
                rebuild();
            }
            break;
        case DBG_BTN_SKIN_MODEL_PREV:
            if (modelPage > 0) {
                modelPage--;
                rebuild();
            }
            break;
        case DBG_BTN_SKIN_MODEL_NEXT:
            if ((modelPage + 1) * kModelRows < categories[catSel].count) {
                modelPage++;
                rebuild();
            }
            break;
        default:
            if (keyCode >= DBG_BTN_SKIN_CAT_BASE
                && keyCode < DBG_BTN_SKIN_CAT_BASE + kCatRows) {
                int categoryIndex = catPage * kCatRows + (keyCode - DBG_BTN_SKIN_CAT_BASE);
                if (categoryIndex >= 0 && categoryIndex < (int)categories.size()
                    && categoryIndex != catSel) {
                    catSel = categoryIndex;
                    modelPage = 0;
                    rebuild();
                }
            } else if (keyCode >= DBG_BTN_SKIN_MODEL_BASE
                && keyCode < DBG_BTN_SKIN_MODEL_BASE + kModelRows) {
                const SkinCategory& category = categories[catSel];
                int modelIndex = modelPage * kModelRows + (keyCode - DBG_BTN_SKIN_MODEL_BASE);
                if (modelIndex >= 0 && modelIndex < category.count) {
                    MpDebugApplyModel(models[category.start + modelIndex].index);
                    // Redraw the current-model line.
                    char status[64];
                    snprintf(status, sizeof(status), "Current: %.12s",
                        models[category.start + modelIndex].name);
                    windowFill(win, 8, 24, kWidth - 16, 24, COLOR_BLACK);
                    windowDrawText(win, status, 0, 30, 28, COLOR_WHITE);
                    windowRefresh(win);
                }
            }
            break;
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    windowDestroy(win);
}

// Client: heal request for the local avatar (value <= 0 = full). Rides the
// generic player-command route — the host applies it to the avatar and the
// state channel carries the result back.
void MpDebugSendHeal(int value)
{
    if (!gMpIsClient || !gMpActive || gMpSession.hostPeer == nullptr) {
        return;
    }
    NetPlayerCmdPayload payload;
    payload.opcode = NET_PLAYER_CMD_HEAL;
    payload.arg1 = value;
    payload.arg2 = 0;
    NetSendPacket(gMpSession.hostPeer, NET_CHANNEL_RELIABLE,
        NET_PKT_PLAYER_CMD, &payload, sizeof(payload));
    MpLog(MP_LOG_COMBAT, "heal sent value=%d", value);
}

// Client: AP refill request for the local avatar. Rides the generic
// player-command route — the host applies it to the avatar and the state
// channel carries the result back.
void MpDebugSendApRefill()
{
    if (!gMpIsClient || !gMpActive || gMpSession.hostPeer == nullptr) {
        return;
    }
    NetPlayerCmdPayload payload;
    payload.opcode = NET_PLAYER_CMD_AP_REFILL;
    payload.arg1 = 0;
    payload.arg2 = 0;
    NetSendPacket(gMpSession.hostPeer, NET_CHANNEL_RELIABLE,
        NET_PKT_PLAYER_CMD, &payload, sizeof(payload));
    MpLog(MP_LOG_COMBAT, "ap refill sent");
}

} // namespace fallout
