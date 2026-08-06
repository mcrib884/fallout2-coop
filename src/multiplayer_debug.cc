#include "multiplayer_debug.h"

#include <algorithm>
#include <stdio.h>

#include "color.h"
#include "critter.h"
#include "debug.h"
#include "game.h"
#include "input.h"
#include "interface.h"
#include "item.h"
#include "kb.h"
#include "mouse.h"
#include "multiplayer.h"
#include "net.h"
#include "perk.h"
#include "skill.h"
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
constexpr int DBG_BTN_PREV = 700;
constexpr int DBG_BTN_NEXT = 701;
constexpr int DBG_BTN_DEC = 702;
constexpr int DBG_BTN_INC = 703;
constexpr int DBG_BTN_JUMP_DEC = 704;
constexpr int DBG_BTN_JUMP_INC = 705;
constexpr int DBG_BTN_BACK = 706;

constexpr int kDbgWindowWidth = 320;
constexpr int kDbgWindowHeight = 185;

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

// Heal the local dude. On a co-op client the HP is host-authoritative: the
// request goes to the host, which applies it to the avatar and lets the
// regular state/status broadcast carry the result back. The local apply is
// only for instant feedback — the echo converges within a round trip.
void dbgHeal(int amount)
{
    if (gDude == nullptr) {
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

// ---------------------------------------------------------------------------
// editor submenus (skills / stats / perks)
// ---------------------------------------------------------------------------

struct SubmenuCallbacks {
    const char* title;
    int count;
    int jump;      // list jump step (0 = hide jump buttons)
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
    return gDude != nullptr ? critterGetStat(gDude, static_cast<Stat>(index)) : 0;
}

void dbgStatModify(int index, int delta)
{
    if (gDude == nullptr) {
        return;
    }
    Stat stat = static_cast<Stat>(index);
    int next = std::clamp(critterGetStat(gDude, stat) + delta, 1, 10);
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

// Runs the modal loop for an editor submenu. The current entry line is drawn
// every frame (plain windowDrawText — no DRAW_TEXT_FLAG_NO_BG — so the fill
// erases the previous frame's glyphs). Returns the pressed button code or 0
// on KEY_ESCAPE.
int dbgSubmenuModal(int win, const SubmenuCallbacks* cb, int current)
{
    char buf[96];
    int rc = -1;
    while (rc == -1) {
        sharedFpsLimiter.mark();

        int value = cb->value(current);
        snprintf(buf, sizeof(buf), "%s  %d", cb->name(current), value);
        int x = (kDbgWindowWidth - fontGetStringWidth(buf)) / 2;
        windowDrawText(win, buf, 0, x, 30, COLOR_WHITE);

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
        case DBG_BTN_JUMP_DEC:
        case DBG_BTN_JUMP_INC:
        case DBG_BTN_BACK:
            rc = keyCode;
            break;
        default:
            break;
        }

        renderPresent();
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
    if (cb->jump > 0) {
        _win_register_text_button(win, 30, 115, -1, -1, -1, DBG_BTN_JUMP_DEC, "-10", 0);
        _win_register_text_button(win, 170, 115, -1, -1, -1, DBG_BTN_JUMP_INC, "+10", 0);
    }
    _win_register_text_button(win, 30, 145, -1, -1, -1, DBG_BTN_BACK, "Back", 0);
    windowRefresh(win);

    int current = 0;
    for (;;) {
        int choice = dbgSubmenuModal(win, cb, current);
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
        case DBG_BTN_JUMP_DEC:
            current = (current + cb->count - cb->jump) % cb->count;
            break;
        case DBG_BTN_JUMP_INC:
            current = (current + cb->jump) % cb->count;
            break;
        }
    }

    windowDestroy(win);
}

} // namespace

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
    _win_register_text_button(win, 30, 130, -1, -1, -1, DBG_BTN_SKILLS, "Skills...", 0);
    _win_register_text_button(win, 140, 130, -1, -1, -1, DBG_BTN_STATS, "Stats...", 0);
    _win_register_text_button(win, 250, 130, -1, -1, -1, DBG_BTN_PERKS, "Perks...", 0);
    _win_register_text_button(win, 30, 160, -1, -1, -1, DBG_BTN_CLOSE, "Close", 0);
    windowRefresh(win);

    SubmenuCallbacks skillsCb {
        "SKILLS", SKILL_COUNT, 5, 10, dbgSkillName, dbgSkillValue, dbgSkillModify,
    };
    SubmenuCallbacks statsCb {
        "STATS", 7, 0, 1, dbgStatName, dbgStatValue, dbgStatModify,
    };
    SubmenuCallbacks perksCb {
        "PERKS", PERK_COUNT, 10, 1, dbgPerkName, dbgPerkValue, dbgPerkModify,
    };

    bool keepGoing = true;
    while (keepGoing) {
        int rc = -1;
        while (rc == -1) {
            sharedFpsLimiter.mark();

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
            case DBG_BTN_MONEY_1000:
            case DBG_BTN_MONEY_100:
            case DBG_BTN_HEAL_FULL:
            case DBG_BTN_HP_50:
            case DBG_BTN_XP_1000:
            case DBG_BTN_XP_5000:
            case DBG_BTN_SP_10:
            case DBG_BTN_LEVEL_1:
            case DBG_BTN_SKILLS:
            case DBG_BTN_STATS:
            case DBG_BTN_PERKS:
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
        case DBG_BTN_SKILLS:
            dbgSubmenuShow(&skillsCb);
            break;
        case DBG_BTN_STATS:
            dbgSubmenuShow(&statsCb);
            break;
        case DBG_BTN_PERKS:
            dbgSubmenuShow(&perksCb);
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

} // namespace fallout
