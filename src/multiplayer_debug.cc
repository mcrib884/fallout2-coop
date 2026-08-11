#include "multiplayer_debug.h"

#include <algorithm>
#include <stdio.h>

#include "color.h"
#include "combat.h"
#include "critter.h"
#include "debug.h"
#include "game.h"
#include "input.h"
#include "interface.h"
#include "item.h"
#include "kb.h"
#include "mouse.h"
#include "multiplayer.h"
#include "multiplayer_perf.h"
#include "net.h"
#include "perk.h"
#include "proto.h"
#include "skill.h"
#include "sfall_script_hooks.h"
#include "stat.h"
#include "svga.h"
#include "text_font.h"
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
constexpr int DBG_BTN_CHEAT_PERF = 736;
constexpr int DBG_BTN_CHEAT_BACK = 737;
constexpr int DBG_BTN_CHEAT_INSTA_KILL = 738;

constexpr int kDbgWindowWidth = 320;
constexpr int kDbgWindowHeight = 305;
constexpr int kDbgCheatWindowWidth = 380;
constexpr int kDbgCheatWindowHeight = 220;

uint32_t gDbgCheatFlags = 0;
bool gDbgCheatFlagsDirty = false;
uint32_t gDbgCheatLastSyncTick = 0;

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
    debugFilePrint("MPDBG: ap refill local ap=%d", maxAp);
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
        debugFilePrint("MPDBG: give item failed pid=0x%X", pid);
        return;
    }
    itemAdd(gDude, item, count);
    debugFilePrint("MPDBG: give item pid=0x%X count=%d", pid, count);
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
    debugFilePrint("MPDBG: ammo refill weapon pid=0x%X ammo=0x%X capacity=%d",
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
    gDbgCheatFlags ^= flag;
    gDbgCheatFlagsDirty = true;
    if (gMpActive && gMpIsHost) {
        gMpSession.players[0].debugCheatFlags = gDbgCheatFlags;
    }
    debugFilePrint("MPDBG: cheat %s flags=0x%X",
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
        debugFilePrint("MPDBG: god mode restored netId=%u hp=%d",
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
    int winX;
    int winY;
    dbgCenteredPos(kDbgCheatWindowWidth, kDbgCheatWindowHeight, &winX, &winY);

    int win = windowCreate(winX, winY, kDbgCheatWindowWidth, kDbgCheatWindowHeight,
        COLOR_BLACK, WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    if (win == -1) {
        debugFilePrint("MPDBG: cheat submenu window create failed");
        return;
    }
    windowDrawBorder(win);

    const char* title = "CHEATS";
    int titleX = (kDbgCheatWindowWidth - fontGetStringWidth(title)) / 2;
    windowDrawText(win, title, 0, titleX, 6, COLOR_WHITE);
    _win_register_text_button(win, 30, 30, -1, -1, -1, DBG_BTN_CHEAT_GOD, "God Mode", 0);
    _win_register_text_button(win, 140, 30, -1, -1, -1, DBG_BTN_CHEAT_AP, "Infinite AP", 0);
    _win_register_text_button(win, 250, 30, -1, -1, -1, DBG_BTN_CHEAT_AMMO, "Infinite Ammo", 0);
    _win_register_text_button(win, 30, 55, -1, -1, -1, DBG_BTN_CHEAT_CARRY, "Unlimited Carry", 0);
    _win_register_text_button(win, 140, 55, -1, -1, -1, DBG_BTN_CHEAT_SKILLS, "Always Succeed", 0);
    _win_register_text_button(win, 250, 55, -1, -1, -1, DBG_BTN_CHEAT_ENCOUNTERS, "No Encounters", 0);
    _win_register_text_button(win, 30, 80, -1, -1, -1, DBG_BTN_CHEAT_PERF, "Perf Meter", 0);
    _win_register_text_button(win, 140, 80, -1, -1, -1, DBG_BTN_CHEAT_INSTA_KILL, "Insta Kill", 0);
    _win_register_text_button(win, 250, 80, -1, -1, -1, DBG_BTN_CHEAT_BACK, "Back", 0);
    windowRefresh(win);

    bool keepGoing = true;
    while (keepGoing) {
        sharedFpsLimiter.mark();
        windowFill(win, 8, 132, kDbgCheatWindowWidth - 16, 34, COLOR_BLACK);
        char status1[128];
        char status2[128];
        snprintf(status1, sizeof(status1), "God: %s   AP: %s   Ammo: %s   Carry: %s",
            dbgOnOff(MP_DEBUG_CHEAT_GOD_MODE),
            dbgOnOff(MP_DEBUG_CHEAT_INFINITE_AP),
            dbgOnOff(MP_DEBUG_CHEAT_INFINITE_AMMO),
            dbgOnOff(MP_DEBUG_CHEAT_UNLIMITED_CARRY));
        snprintf(status2, sizeof(status2), "Skills: %s   Enc: %s   InstaKill: %s",
            dbgOnOff(MP_DEBUG_CHEAT_ALWAYS_SUCCEED),
            dbgOnOff(MP_DEBUG_CHEAT_NO_RANDOM_ENCOUNTERS),
            dbgOnOff(MP_DEBUG_CHEAT_INSTA_KILL));
        windowDrawText(win, status1, 0,
            (kDbgCheatWindowWidth - fontGetStringWidth(status1)) / 2, 135, COLOR_WHITE);
        windowDrawText(win, status2, 0,
            (kDbgCheatWindowWidth - fontGetStringWidth(status2)) / 2, 151, COLOR_WHITE);
        windowRefresh(win);
        renderPresent();
        MpTick();

        int keyCode = inputGetInput();
        switch (keyCode) {
        case KEY_ESCAPE:
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
        case DBG_BTN_CHEAT_PERF:
            MpPerfSetEnabled(!MpPerfIsEnabled());
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
    debugFilePrint("MPDBG: max level done level=%d", level);
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
    debugFilePrint("MPDBG: stat modify index=%d delta=%d stat=%s before=%d",
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
    debugFilePrint("MPDBG: item removed pid=0x%X count=%d", kDbgItems[index].pid, -delta - remaining);
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
        int keyCode = inputGetInput();
        switch (keyCode) {
        case KEY_ESCAPE:
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
        debugFilePrint("MPDBG: submenu %s choice=%d current=%d name='%s' value=%d",
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

} // namespace

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
                debugFilePrint("MPDBG: cheat flags sent flags=0x%X", gDbgCheatFlags);
            }
        }
    }

    if (gMpActive && gMpIsHost) {
        gMpSession.players[0].debugCheatFlags = gDbgCheatFlags;
        for (int index = 0; index < NET_MAX_PLAYERS; index++) {
            MultiplayerPlayer* player = &gMpSession.players[index];
            if (player->isConnected && player->obj != nullptr) {
                dbgApplyCheats(player->obj, player->debugCheatFlags);
            }
        }
    } else {
        dbgApplyCheats(gDude, gDbgCheatFlags);
    }
}

// === MpDebugMenuShow ===
void MpDebugMenuShow()
{
    debugFilePrint("MPDBG: menu show begin");
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

    const char* title = "CO-OP DEBUG";
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
    _win_register_text_button(win, 170, 255, -1, -1, -1, DBG_BTN_CHEATS, "Cheats...", 0);
    _win_register_text_button(win, 30, 280, -1, -1, -1, DBG_BTN_CLOSE, "Close", 0);
    windowRefresh(win);

    SubmenuCallbacks skillsCb {
        "SKILLS", SKILL_COUNT, 10, dbgSkillName, dbgSkillValue, dbgSkillModify,
    };
    SubmenuCallbacks statsCb {
        "STATS", 7, 1, dbgStatName, dbgStatValue, dbgStatModify,
    };
    SubmenuCallbacks perksCb {
        "PERKS", PERK_COUNT, 1, dbgPerkName, dbgPerkValue, dbgPerkModify,
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
            int keyCode = inputGetInput();
            switch (keyCode) {
            case KEY_ESCAPE:
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
            dbgSubmenuShow(&perksCb);
            break;
        case DBG_BTN_ITEMS:
            dbgSubmenuShow(&itemsCb);
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
    debugFilePrint("MPDBG: menu show end");
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
    debugFilePrint("MP: heal sent value=%d", value);
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
    debugFilePrint("MP: ap refill sent");
}

} // namespace fallout
