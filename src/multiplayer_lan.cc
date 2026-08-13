#include "multiplayer_lan.h"
#include "multiplayer_log.h"

#include <stdio.h>
#include <string.h>

#include "animation.h"
#include "color.h"
#include "debug.h"
#include "game.h"
#include "game_mouse.h"
#include "input.h"
#include "kb.h"
#include "map.h"
#include "mouse.h"
#include "multiplayer.h"
#include "multiplayer_menu.h"
#include "net.h"
#include "scripts.h"
#include "svga.h"
#include "text_font.h"
#include "window_manager.h"
#include "window_manager_private.h"

namespace fallout {

namespace {

// Button event codes (above any KEY_* constants, same convention as the
// multiplayer menus).
constexpr int LAN_BTN_SCAN = 520;
constexpr int LAN_BTN_JOIN = 521;
constexpr int LAN_BTN_CLOSE = 522;

// Host list rows (right of the buttons, below the title). The panel is a
// NARROW side window (kLanBrowserWidth from the header) — the caller places
// it beside the multiplayer dialog / F11 menu.
constexpr int kListX = 10;
constexpr int kListY = 95;
constexpr int kRowHeight = 24;
constexpr int kMaxVisibleRows = 7;

// The world keeps simulating behind the browser (same pump the debug menus
// use): animations, script requests, transitions, network and camera drag.
void lanPumpGameBehindModal()
{
    tickersExecute();
    scriptsHandleRequests();
    mapHandleTransition();
    MpTick();
    MpDrawPlayerIndicators();
    gameMouseCameraDragTick();
}

// Draw the host list rows into the window; the selected row is highlighted.
void lanDrawHostRows(int win, const NetLanHostInfo* hosts, int count, int selected)
{
    for (int index = 0; index < kMaxVisibleRows; index++) {
        int rowY = kListY + index * kRowHeight;
        if (index >= count) {
            windowFill(win, kListX, rowY, kLanBrowserWidth - kListX - 10, kRowHeight, COLOR_BLACK);
            continue;
        }
        const NetLanHostInfo* host = &hosts[index];
        char line[96];
        if (host->versionHash != NetGetVersionHash()) {
            snprintf(line, sizeof(line), "%s %s:%u %u/%u <other build>",
                host->name, host->address, host->port,
                host->currentPlayers, host->maxPlayers);
        } else {
            snprintf(line, sizeof(line), "%s %s:%u %u/%u%s",
                host->name, host->address, host->port,
                host->currentPlayers, host->maxPlayers,
                host->passwordRequired ? " [pass]" : "");
        }
        // Truncate to the panel's width so long names do not bleed over the
        // border (the window buffer is only 240 wide).
        while (line[0] != '\0' && fontGetStringWidth(line) > kLanBrowserWidth - kListX - 16) {
            line[strlen(line) - 1] = '\0';
        }
        if (index == selected) {
            // Border-only highlight: a full-row fill would wash out the row
            // text, so the selection is a plain rectangle outline around the
            // row, leaving the text untouched.
            windowFill(win, kListX, rowY, kLanBrowserWidth - kListX - 10, kRowHeight, COLOR_BLACK);
            windowDrawText(win, line, 0, kListX + 3, rowY + (kRowHeight - fontGetLineHeight()) / 2, COLOR_WHITE);
            windowDrawRect(win, kListX, rowY, kListX + kLanBrowserWidth - kListX - 11, rowY + kRowHeight - 1, COLOR_LIGHT_GREY);
        } else {
            windowFill(win, kListX, rowY, kLanBrowserWidth - kListX - 10, kRowHeight, COLOR_BLACK);
            windowDrawText(win, line, 0, kListX + 3, rowY + (kRowHeight - fontGetLineHeight()) / 2, COLOR_WHITE);
        }
    }
}

} // namespace

int MpLanBrowserShow(int x, int y)
{
    MpLog(MP_LOG_UI, "lan browser show begin x=%d y=%d", x, y);
    bool cursorWasHidden = cursorIsHidden();
    if (cursorWasHidden) {
        mouseShowCursor();
    }

    if (!NetLanBrowserStart()) {
        if (cursorWasHidden) {
            mouseHideCursor();
        }
        return 0;
    }

    int winX = x;
    int winY = y;
    if (winX < 0) {
        winX = 0;
    }
    if (winY < 0) {
        winY = 0;
    }

    int win = windowCreate(winX, winY, kLanBrowserWidth, kLanBrowserHeight, COLOR_BLACK, WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    if (win == -1) {
        win_timed_msg("Failed to open dialog", COLOR_RED);
        NetLanBrowserStop();
        if (cursorWasHidden) {
            mouseHideCursor();
        }
        return 0;
    }
    windowDrawBorder(win);

    const char* title = "LAN BROWSER";
    int titleX = (kLanBrowserWidth - fontGetStringWidth(title)) / 2;
    windowDrawText(win, title, 0, titleX, 6, COLOR_WHITE);

    _win_register_text_button(win, 20, 20, -1, -1, -1, LAN_BTN_SCAN, "Scan", 0);
    _win_register_text_button(win, 20, 45, -1, -1, -1, LAN_BTN_JOIN, "Join", 0);
    _win_register_text_button(win, 20, 70, -1, -1, -1, LAN_BTN_CLOSE, "Close", 0);

    windowRefresh(win);

    NetLanHostInfo hosts[NET_LAN_MAX_HOSTS];
    int hostCount = 0;
    // Last-seen tick per host. A poll only sees the replies queued in the
    // socket buffer at that instant — the probe burst — so replacing the
    // list every frame would wipe hosts back to "Scanning..." a few frames
    // after each scan. Keep the list persistent and drop hosts only when
    // they stop answering (grace above the 2s scan cadence).
    uint32_t hostLastSeen[NET_LAN_MAX_HOSTS] = {0};
    int selected = -1;
    uint32_t lastScanTick = 0;
    bool prevLeftButton = false;

    int rc = 0;
    bool keepGoing = true;
    while (keepGoing) {
        sharedFpsLimiter.mark();

        // Rescan on open and every ~2s so new hosts appear without a click.
        uint32_t now = getTicks();
        if (lastScanTick == 0 || getTicksSince(lastScanTick) >= 2000) {
            lastScanTick = now;
            NetLanBrowserScan();
        }

        // Drain fresh replies and merge them into the persistent list.
        int previousHostCount = hostCount;
        NetLanHostInfo fresh[NET_LAN_MAX_HOSTS];
        int freshCount = NetLanBrowserPoll(fresh, NET_LAN_MAX_HOSTS);
        bool rowsChanged = freshCount > 0;
        for (int index = 0; index < freshCount; index++) {
            int slot = -1;
            for (int existing = 0; existing < hostCount; existing++) {
                if (hosts[existing].port == fresh[index].port
                    && strcmp(hosts[existing].address, fresh[index].address) == 0) {
                    slot = existing;
                    break;
                }
            }
            if (slot == -1) {
                if (hostCount >= NET_LAN_MAX_HOSTS) {
                    continue;
                }
                slot = hostCount++;
            }
            hosts[slot] = fresh[index];
            hostLastSeen[slot] = now;
        }

        // Drop hosts that stopped answering (scan cadence is 2s; keep a
        // 2.5s grace so the list survives the quiet gap between scans).
        int out = 0;
        for (int index = 0; index < hostCount; index++) {
            if (getTicksSince(hostLastSeen[index]) < 2500) {
                if (out != index) {
                    hosts[out] = hosts[index];
                    hostLastSeen[out] = hostLastSeen[index];
                }
                out++;
            } else {
                MpLog(MP_LOG_UI, "lan host dropped '%s' %s:%u (no reply for 2.5s)",
                    hosts[index].name, hosts[index].address, hosts[index].port);
            }
        }
        hostCount = out;
        if (hostCount != previousHostCount) {
            rowsChanged = true;
        }
        if (selected >= hostCount) {
            selected = hostCount - 1;
        }
        if (rowsChanged) {
            lanDrawHostRows(win, hosts, hostCount, selected);
            windowRefresh(win);
        }

        // Row selection by mouse click (raw state: the modal consumes the
        // normal input event stream, so poll the button like the camera drag).
        int mouseX;
        int mouseY;
        int mouseButtons;
        _mouse_get_raw_state(&mouseX, &mouseY, &mouseButtons);
        bool leftButton = (mouseButtons & MOUSE_STATE_LEFT_BUTTON_DOWN) != 0;
        if (leftButton && !prevLeftButton) {
            int localX = mouseX - winX;
            int localY = mouseY - winY;
            if (localX >= kListX && localY >= kListY) {
                int row = (localY - kListY) / kRowHeight;
                if (row >= 0 && row < hostCount) {
                    selected = row;
                    lanDrawHostRows(win, hosts, hostCount, selected);
                    windowRefresh(win);
                }
            }
        }
        prevLeftButton = leftButton;

        int keyCode = inputGetInput();
        switch (keyCode) {
        case KEY_ESCAPE:
            rc = 0;
            keepGoing = false;
            break;
        case LAN_BTN_CLOSE:
            rc = 0;
            keepGoing = false;
            break;
        case LAN_BTN_SCAN:
            lastScanTick = getTicks();
            NetLanBrowserScan();
            break;
        case LAN_BTN_JOIN:
        case KEY_RETURN:
            if (selected >= 0 && selected < hostCount) {
                keepGoing = false;
                rc = -1; // join pending below
            }
            break;
        case KEY_ARROW_UP:
            if (hostCount > 0) {
                selected = (selected <= 0) ? hostCount - 1 : selected - 1;
                lanDrawHostRows(win, hosts, hostCount, selected);
                windowRefresh(win);
            }
            break;
        case KEY_ARROW_DOWN:
            if (hostCount > 0) {
                selected = (selected + 1 >= hostCount) ? 0 : selected + 1;
                lanDrawHostRows(win, hosts, hostCount, selected);
                windowRefresh(win);
            }
            break;
        default:
            break;
        }

        // Status line under the list.
        char status[96];
        if (hostCount == 0) {
            strncpy(status, "Scanning for hosts...", sizeof(status) - 1);
            status[sizeof(status) - 1] = '\0';
        } else {
            snprintf(status, sizeof(status), "%d host(s) - Join (Enter)", hostCount);
        }
        windowFill(win, kListX, kListY + kMaxVisibleRows * kRowHeight,
            kLanBrowserWidth - kListX - 10, 16, COLOR_BLACK);
        windowDrawText(win, status, 0, kListX + 3,
            kListY + kMaxVisibleRows * kRowHeight + 1, COLOR_WHITE);
        windowRefresh(win);

        lanPumpGameBehindModal();
        windowRefreshAll(&_scr_size);
        renderPresent();
        sharedFpsLimiter.throttle();
    }

    windowDestroy(win);

    if (rc == -1 && selected >= 0 && selected < hostCount) {
        const NetLanHostInfo* host = &hosts[selected];
        MpLog(MP_LOG_UI, "lan browser join '%s' %s:%u",
            host->name, host->address, host->port);
        char password[64] = "";
        if (host->passwordRequired) {
            _win_get_str_masked(password, 63, "Password (optional)", winX + 40, winY + 120);
        }
        // Same join path as address joining: in-game joins back the session
        // with the hidden co-op save, main-menu joins ask for a character or
        // save first. Connecting directly would join without a backed save.
        rc = MpJoinInitiate(host->address, host->port, password) != 0 ? 1 : 0;
    }

    NetLanBrowserStop();
    if (cursorWasHidden) {
        mouseHideCursor();
    }
    MpLog(MP_LOG_UI, "lan browser show done rc=%d", rc);
    return rc;
}

} // namespace fallout
