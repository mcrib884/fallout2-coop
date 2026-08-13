#include "multiplayer_menu.h"
#include "multiplayer_log.h"

#include <stdio.h>
#include <string.h>

#include "color.h"
#include "debug.h"
#include "game.h"
#include "input.h"
#include "kb.h"
#include "loadsave.h"
#include "mouse.h"
#include "multiplayer.h"
#include "multiplayer_lan.h"
#include "net.h"
#include "svga.h"
#include "text_font.h"
#include "window_manager.h"
#include "window_manager_private.h"

namespace fallout {

namespace {

// Button event codes. Picked above any KEY_* constants so they don't collide
// with real keyboard scans dispatched by inputGetInput().
constexpr int MP_BTN_HOST = 500;
constexpr int MP_BTN_JOIN = 501;
constexpr int MP_BTN_CANCEL = 502;
constexpr int MP_BTN_NEW_GAME = 503;
constexpr int MP_BTN_LOAD_SAVE = 504;
constexpr int MP_BTN_CREATE_CHARACTER = 505;
constexpr int MP_BTN_LOAD_CHARACTER = 506;
constexpr int MP_BTN_LAN_BROWSER = 507;

// Centered position for a window of the given (width, height).
void mpCenteredPos(int width, int height, int* outX, int* outY)
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

// Local input loop for a modal window. Returns the chosen button code
// (MP_BTN_*) or 0 when KEY_ESCAPE was pressed. Other key codes are ignored.
int mpRunModalLoop()
{
    int rc = -1;
    while (rc == -1) {
        sharedFpsLimiter.mark();

        int keyCode = inputGetInput();

        switch (keyCode) {
        case KEY_ESCAPE:
            rc = 0;
            break;
        case MP_BTN_HOST:
        case MP_BTN_JOIN:
        case MP_BTN_CANCEL:
        case MP_BTN_NEW_GAME:
        case MP_BTN_LOAD_SAVE:
        case MP_BTN_CREATE_CHARACTER:
        case MP_BTN_LOAD_CHARACTER:
        case MP_BTN_LAN_BROWSER:
            rc = keyCode;
            break;
        default:
            break;
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }
    MpLog(MP_LOG_UI, "modal choice=%d", rc);
    return rc;
}

} // namespace

// === MpMenuShow ===
int MpMenuShow()
{
    MpLog(MP_LOG_UI, "show begin gameLoaded=%d", gGameLoaded ? 1 : 0);
    bool cursorWasHidden = cursorIsHidden();
    if (cursorWasHidden) {
        mouseShowCursor();
    }

    constexpr int kWindowWidth = 280;
    constexpr int kWindowHeight = 140;
    int winX, winY;
    mpCenteredPos(kWindowWidth, kWindowHeight, &winX, &winY);

    int rc = 0;
    bool keepGoing = true;
    while (keepGoing) {
        int win = windowCreate(winX, winY, kWindowWidth, kWindowHeight, COLOR_BLACK, WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
        if (win == -1) {
            win_timed_msg("Failed to open dialog", COLOR_RED);
            if (cursorWasHidden) {
                mouseHideCursor();
            }
            return 0;
        }
        windowDrawBorder(win);

        const char* title = "MULTIPLAYER";
        int titleX = (kWindowWidth - fontGetStringWidth(title)) / 2;
        windowDrawText(win, title, 0, titleX, 6, COLOR_WHITE);

        _win_register_text_button(win, 40, 30, -1, -1, -1, MP_BTN_HOST, "Host", 0);
        _win_register_text_button(win, 40, 55, -1, -1, -1, MP_BTN_JOIN, "Join", 0);
        _win_register_text_button(win, 40, 80, -1, -1, -1, MP_BTN_CANCEL, "Cancel", 0);
        _win_register_text_button(win, 40, 105, -1, -1, -1, MP_BTN_LAN_BROWSER, "LAN Browser", 0);

        windowRefresh(win);

        int choice = mpRunModalLoop();

        if (choice == MP_BTN_LAN_BROWSER) {
            // The browser is a SIDE panel: keep this dialog on screen next to
            // it (it is created after, on top, so it blocks these buttons for
            // the duration — _win_check_all_buttons stops at the first modal).
            int lanX = screenGetWidth() - kLanBrowserWidth - 8;
            if (lanX < 0) {
                lanX = 0;
            }
            if (MpLanBrowserShow(lanX, winY) != 0) {
                rc = 1;
                keepGoing = false;
                windowDestroy(win);
                break;
            }
            // Browser cancelled: rebuild the dialog.
            windowDestroy(win);
            continue;
        }

        windowDestroy(win);

        switch (choice) {
        case MP_BTN_HOST: {
            if (gGameLoaded) {
                if (MpHostCurrentGame() != 0) {
                    win_timed_msg("Could not start co-op hosting", COLOR_RED);
                    break;
                }
                rc = 1;
                keepGoing = false;
                break;
            }

            int hostRc = MpHostFlowShow();
            if (hostRc == 1) {
                gMpPendingHostStartAfterLoad = 1;
                rc = 1;
                keepGoing = false;
            } else if (hostRc == 2) {
                gMpPendingHostStartAfterLoad = 2;
                rc = 1;
                keepGoing = false;
            }
            break;
        }
        case MP_BTN_JOIN: {
            int joinRc = MpJoinFlowShow();
            if (joinRc == 1) {
                rc = 1;
                keepGoing = false;
            }
            break;
        }
        case MP_BTN_CANCEL:
        case 0:
            rc = 0;
            keepGoing = false;
            break;
        default:
            rc = 0;
            keepGoing = false;
            break;
        }
    }

    if (cursorWasHidden) {
        mouseHideCursor();
    }
    return rc;
}

// === MpHostFlowShow ===
int MpHostFlowShow()
{
    // Main-menu hosting uses the default host options; the F11 CO-OP
    // SETTINGS menu sets its own values explicitly right before hosting.
    // Reset here so a previous F11 configuration never leaks into a later
    // main-menu host.
    gMpHostPort = NET_DEFAULT_PORT;
    gMpHostMaxPlayers = NET_MAX_PLAYERS;
    gMpHostPasswordHash = 0;
    MpLog(MP_LOG_UI, "host flow defaults restored port=%u maxPlayers=%d",
        gMpHostPort, gMpHostMaxPlayers);

    constexpr int kWindowWidth = 280;
    constexpr int kWindowHeight = 140;
    int winX, winY;
    mpCenteredPos(kWindowWidth, kWindowHeight, &winX, &winY);

    int win = windowCreate(winX, winY, kWindowWidth, kWindowHeight, COLOR_BLACK, WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    if (win == -1) {
        return 0;
    }
    windowDrawBorder(win);

    const char* title = "HOST CO-OP";
    int titleX = (kWindowWidth - fontGetStringWidth(title)) / 2;
    windowDrawText(win, title, 0, titleX, 6, COLOR_WHITE);

    _win_register_text_button(win, 40, 30, -1, -1, -1, MP_BTN_NEW_GAME, "New Game", 0);
    _win_register_text_button(win, 40, 55, -1, -1, -1, MP_BTN_LOAD_SAVE, "Load Save", 0);
    _win_register_text_button(win, 40, 80, -1, -1, -1, MP_BTN_CANCEL, "Cancel", 0);

    windowRefresh(win);
    int choice = mpRunModalLoop();
    windowDestroy(win);

    switch (choice) {
    case MP_BTN_NEW_GAME:
        return 1;
    case MP_BTN_LOAD_SAVE:
        return 2;
    default:
        return 0;
    }
}

// === MpJoinInitiate ===
// Shared join path used by BOTH the address join (MpJoinFlowShow) and the
// LAN browser (MpLanBrowserShow) so a session always starts from a save or a
// fresh character, never from whatever game state happens to be loaded.
//
// Returns 1 when a join was started (either connected directly or queued via
// the pending-client globals), 0 on cancel or failure.
int MpJoinInitiate(const char* address, uint16_t port, const char* password)
{
    // Joining from an already-loaded game: per the co-op framework, never
    // use the in-memory character directly — back the session with a save
    // first. The current game is saved into the reserved hidden co-op slot
    // and reloaded from it, so the session always builds from a save and the
    // player's own slots are never touched.
    if (gGameLoaded) {
        // The session's base save is the hidden co-op slot; client saves
        // during the session land there too.
        gMpSessionSlot = lsgGetCoopSaveSlot();
        int saveRc = lsgQuickSaveGameCoop();
        MpLog(MP_LOG_UI, "in-game join coop save rc=%d sessionSlot=%d", saveRc, gMpSessionSlot);
        if (saveRc != 1) {
            win_timed_msg("Could not back up your game before joining", COLOR_RED);
            return 0;
        }
        if (lsgLoadGameCoop() != 0) {
            win_timed_msg("Could not reload your backed-up game", COLOR_RED);
            return 0;
        }
        return MpClientConnect(address, port, password) == 0 ? 1 : 0;
    }

    constexpr int kWindowWidth = 320;
    constexpr int kWindowHeight = 150;
    int winX, winY;
    mpCenteredPos(kWindowWidth, kWindowHeight, &winX, &winY);

    int win = windowCreate(winX, winY, kWindowWidth, kWindowHeight,
        COLOR_BLACK, WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    if (win == -1) {
        return 0;
    }

    windowDrawBorder(win);
    const char* title = "JOIN CO-OP CHARACTER";
    int titleX = (kWindowWidth - fontGetStringWidth(title)) / 2;
    windowDrawText(win, title, 0, titleX, 6, COLOR_WHITE);
    _win_register_text_button(win, 40, 30, -1, -1, -1,
        MP_BTN_CREATE_CHARACTER, "Create New Character", 0);
    _win_register_text_button(win, 40, 55, -1, -1, -1,
        MP_BTN_LOAD_CHARACTER, "Load Character", 0);
    _win_register_text_button(win, 40, 80, -1, -1, -1,
        MP_BTN_CANCEL, "Cancel", 0);
    windowRefresh(win);

    int choice = mpRunModalLoop();
    windowDestroy(win);
    if (choice != MP_BTN_CREATE_CHARACTER && choice != MP_BTN_LOAD_CHARACTER) {
        return 0;
    }

    strncpy(gMpPendingClientAddress, address, sizeof(gMpPendingClientAddress) - 1);
    gMpPendingClientAddress[sizeof(gMpPendingClientAddress) - 1] = '\0';
    gMpPendingClientPort = port;
    strncpy(gMpPendingClientPassword, password, sizeof(gMpPendingClientPassword) - 1);
    gMpPendingClientPassword[sizeof(gMpPendingClientPassword) - 1] = '\0';
    gMpPendingClientStartAfterLoad = choice == MP_BTN_CREATE_CHARACTER ? 1 : 2;

    return 1;
}

// === MpJoinFlowShow ===
int MpJoinFlowShow()
{
    char ipBuffer[64];
    strncpy(ipBuffer, "127.0.0.1", sizeof(ipBuffer) - 1);
    ipBuffer[sizeof(ipBuffer) - 1] = '\0';

    int ipX = (screenGetWidth() - 240) / 2;
    int ipY = (screenGetHeight() - 100) / 2;
    if (ipX < 0) {
        ipX = 0;
    }
    if (ipY < 0) {
        ipY = 0;
    }

    // _win_get_str blocks until Enter (returns 0) or ESC (returns non-zero).
    if (_win_get_str(ipBuffer, 46, "Enter Host IP", ipX, ipY) != 0) {
        return 0;
    }

    // Target port (defaults to the standard co-op port).
    int port = gMpPendingClientPort > 0 ? gMpPendingClientPort : NET_DEFAULT_PORT;
    if (win_get_num_i(&port, 1, 65535, false, "Host Port", ipX, ipY) == -1) {
        return 0;
    }

    // Optional session password — masked entry, hashed for the handshake.
    char passwordBuffer[64] = "";
    if (_win_get_str_masked(passwordBuffer, 32, "Password (optional)", ipX, ipY) != 0) {
        return 0;
    }

    return MpJoinInitiate(ipBuffer, (uint16_t)port, passwordBuffer);
}

} // namespace fallout
