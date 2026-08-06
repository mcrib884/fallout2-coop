#include "multiplayer_vote.h"

#include <stdio.h>
#include <string.h>

#include "animation.h"
#include "color.h"
#include "combat.h"
#include "debug.h"
#include "input.h"
#include "kb.h"
#include "map.h"
#include "multiplayer.h"
#include "multiplayer_combat.h"
#include "object.h"
#include "svga.h"
#include "text_font.h"
#include "window_manager.h"
#include "window_manager_private.h"

namespace fallout {

VoteSession gVoteSession = {};

// Window dimensions and button codes for the vote modals.
constexpr int MP_VOTE_BTN_YES = 600;
constexpr int MP_VOTE_BTN_NO = 601;

// Both vote modals (initiator and voter) use the same geometry as the
// multiplayer menu windows.
constexpr int MP_VOTE_WINDOW_W = 280;
constexpr int MP_VOTE_WINDOW_H = 140;

extern void MpTick(); // defined in multiplayer.cc; used by the voter modal loop
                     // to pump packets while blocking on user input.

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void MpVoteInit()
{
    memset(&gVoteSession, 0, sizeof(gVoteSession));
    gVoteSession.state = VOTE_STATE_NONE;
    gVoteSession.voteWindow = -1;
}

void MpVoteShutdown()
{
    MpVoteHideUI();
    MpVoteInit();
}

void MpVoteReset()
{
    MpVoteHideUI();
    MpVoteInit();
}

// ---------------------------------------------------------------------------
// Host side
// ---------------------------------------------------------------------------

int MpVoteStart(const MapTransition* transition, uint8_t initiatorNetId)
{
    if (!gMpIsHost) {
        return -1;
    }
    if (gVoteSession.state == VOTE_STATE_ACTIVE) {
        debugFilePrint("MP: vote start ignored (active) initiator=%u", initiatorNetId);
        return -1;
    }
    // Co-op: combat and map votes are mutually exclusive. A map change mid-
    // combat would tear down the combat list under the sequence.
    if (isInCombat() || MpCombatIsActive()) {
        debugFilePrint("MP: vote start ignored (in combat) initiator=%u", initiatorNetId);
        return -1;
    }

    MpVoteHideUI();
    memset(&gVoteSession, 0, sizeof(gVoteSession));
    gVoteSession.state = VOTE_STATE_ACTIVE;
    gVoteSession.initiatorNetId = initiatorNetId;
    if (transition != nullptr) {
        gVoteSession.transition = *transition;
    }
    gVoteSession.startTime = getTicks();
    gVoteSession.timeoutMs = VOTE_TIMEOUT_MS;
    gVoteSession.totalPlayers = (uint8_t)gMpSession.numPlayers;
    gVoteSession.voteWindow = -1;
    gVoteSession.isInitiator = (initiatorNetId == gMpSession.localNetId);
    debugFilePrint("MP: vote start initiator=%u targetMap=%d tile=%d elev=%d players=%u local=%u",
        initiatorNetId, gVoteSession.transition.map, gVoteSession.transition.tile,
        gVoteSession.transition.elevation, gVoteSession.totalPlayers, gMpSession.localNetId);

    if (initiatorNetId >= 1 && initiatorNetId <= NET_MAX_PLAYERS) {
        MultiplayerPlayer* initiator = &gMpSession.players[initiatorNetId - 1];
        if (initiator->isConnected && initiator->obj != nullptr
            && initiator->hasSafePosition) {
            gVoteSession.lastSafeTile = initiator->lastSafeTile;
            gVoteSession.lastSafeElevation = initiator->lastSafeElevation;
            gVoteSession.lastSafeRotation = initiator->lastSafeRotation;
        } else if (initiator->isConnected && initiator->obj != nullptr) {
            gVoteSession.lastSafeTile = initiator->obj->tile;
            gVoteSession.lastSafeElevation = initiator->obj->elevation;
            gVoteSession.lastSafeRotation = initiator->obj->rotation;
        }
    }

    // Initiator auto-votes yes.
    if (initiatorNetId >= 1 && initiatorNetId <= NET_MAX_PLAYERS) {
        gVoteSession.votes[initiatorNetId - 1] = 1;
        gVoteSession.yesCount = 1;
    }

    gMpSession.initiatorFrozen = true;

    // Broadcast the vote-start to all clients. Host's own local view of the
    // vote UI is shown on the next tick (see MpVoteMaybeShowUI).
    NetVoteStartPayload p;
    p.initiatorNetId = initiatorNetId;
    p.totalPlayers = gVoteSession.totalPlayers;
    p.targetMap = gVoteSession.transition.map;
    p.targetTile = gVoteSession.transition.tile;
    p.targetElevation = gVoteSession.transition.elevation;
    p.targetRotation = gVoteSession.transition.rotation;
    p.timeoutMs = VOTE_TIMEOUT_MS;
    NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE, NET_PKT_VOTE_START, &p, sizeof(p));
    // Clients need the initial tally too (initiator auto-yes).
    MpVoteBroadcastTally();

    // If host is alone they pass immediately. Resolved before the UI is
    // armed so no modal ever blocks for a vote that is already over.
    if (gVoteSession.totalPlayers <= 1 && gVoteSession.yesCount == gVoteSession.totalPlayers) {
        MpVoteResolve();
        return 0;
    }
    // The host is a participant too: when someone else initiated, the host
    // gets the voter modal so he can cast his vote.
    gVoteSession.uiPending = true;
    return 0;
}

void MpVoteCastVote(uint8_t voterNetId, uint8_t vote)
{
    if (!gMpIsHost || gVoteSession.state != VOTE_STATE_ACTIVE) {
        return;
    }
    if (voterNetId < 1 || voterNetId > NET_MAX_PLAYERS) {
        return;
    }
    if (vote != 0 && vote != 1) {
        return;
    }
    uint8_t oldVote = gVoteSession.votes[voterNetId - 1];
    if (oldVote != 0) {
        // Only the initiator may change their auto-cast YES to NO (vetoing
        // their own proposal). Everyone else votes once.
        if (voterNetId != gVoteSession.initiatorNetId) {
            debugFilePrint("MP: vote cast ignored (already voted) voter=%u vote=%d", voterNetId, vote);
            return;
        }
        if (oldVote == 1) {
            gVoteSession.yesCount--;
        } else {
            gVoteSession.noCount--;
        }
    }
    debugFilePrint("MP: vote cast voter=%u vote=%d (flip=%d)",
        voterNetId, vote, oldVote != 0 ? 1 : 0);
    // vote payload: 0 = no, 1 = yes — matches our internal mapping (0 pending,
    // 1 yes, 2 no). Translate.
    uint8_t internal = vote ? 1u : 2u;
    gVoteSession.votes[voterNetId - 1] = internal;
    if (internal == 1) {
        gVoteSession.yesCount++;
    } else {
        gVoteSession.noCount++;
    }

    // Clients display the live tally; broadcast it before any resolve so the
    // final number is already on their screens when the result arrives.
    MpVoteBroadcastTally();

    // Any no kills it.
    if (gVoteSession.noCount > 0) {
        MpVoteResolve();
        return;
    }
    // Unanimous yes.
    if (gVoteSession.yesCount == gVoteSession.totalPlayers) {
        MpVoteResolve();
    }
}

void MpVoteBroadcastTally()
{
    if (!gMpIsHost || gVoteSession.state != VOTE_STATE_ACTIVE) {
        return;
    }
    NetVoteTallyPayload t;
    t.yesCount = (uint8_t)gVoteSession.yesCount;
    t.noCount = (uint8_t)gVoteSession.noCount;
    t.totalCount = gVoteSession.totalPlayers;
    NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE, NET_PKT_VOTE_TALLY, &t, sizeof(t));
}

void MpVoteOnVoteTally(const NetVoteTallyPayload* payload)
{
    if (!gMpIsClient || payload == nullptr) {
        return;
    }
    gVoteSession.yesCount = payload->yesCount;
    gVoteSession.noCount = payload->noCount;
    if (payload->totalCount != 0) {
        gVoteSession.totalPlayers = payload->totalCount;
    }
    debugFilePrint("MP: vote tally received yes=%u no=%u total=%u",
        payload->yesCount, payload->noCount, payload->totalCount);
    // The next MpTick's MpVoteUpdateUI redraws the modal with the new numbers.
}

void MpVoteCheckTimeout()
{
    if (!gMpIsHost || gVoteSession.state != VOTE_STATE_ACTIVE) {
        return;
    }
    if (getTicksSince(gVoteSession.startTime) >= gVoteSession.timeoutMs) {
        MpVoteResolve();
    }
}

// Starts the real map transition after a passed vote. The interceptor sees
// state == PASSED and lets the call through; state resets so future
// transitions trigger fresh votes.
static void mpVoteStartTransitionIfPassed()
{
    if (!gMpIsHost || gVoteSession.state != VOTE_STATE_PASSED) {
        return;
    }
    mapSetTransition(&gVoteSession.transition);
    gVoteSession.state = VOTE_STATE_NONE;
}

void MpVoteResolve()
{
    if (!gMpIsHost || gVoteSession.state != VOTE_STATE_ACTIVE) {
        return;
    }

    bool passed = (gVoteSession.noCount == 0) && (gVoteSession.yesCount == gVoteSession.totalPlayers);
    debugFilePrint("MP: vote resolve passed=%d yes=%u no=%u total=%u",
        passed ? 1 : 0, gVoteSession.yesCount, gVoteSession.noCount, gVoteSession.totalPlayers);

    NetVoteResultPayload result;
    result.passed = passed ? 1 : 0;
    result.yesCount = gVoteSession.yesCount;
    result.totalCount = gVoteSession.totalPlayers;
    NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE, NET_PKT_VOTE_RESULT, &result, sizeof(result));

    if (passed) {
        gVoteSession.state = VOTE_STATE_PASSED;
        gMpSession.initiatorFrozen = false;
        gVoteSession.resolveDisplayStart = getTicks();
        // Keep the modal up showing the final tally; the actual transition is
        // deferred until the passed-display beat elapses (mpRunVoteModal exits
        // on its own for the host) or, with no modal up, starts immediately.
        if (gVoteSession.voteWindow == -1) {
            mpVoteStartTransitionIfPassed();
        }
    } else {
        gVoteSession.state = VOTE_STATE_FAILED;
        gMpSession.initiatorFrozen = false;
        MpVoteHideUI();
        // Teleport the initiator's critter back to the last safe tile on the
        // host (host owns all positions). Local gDude-or-spawned-critter
        // absolute-object handling: we resolve via players[] slot. The safe
        // tile can itself be an exit grid; this is a state correction, not a
        // player choice, so the connect hook must not re-trigger a vote or a
        // transition while we place the critter.
        if (gVoteSession.initiatorNetId >= 1 && gVoteSession.initiatorNetId <= NET_MAX_PLAYERS) {
            MultiplayerPlayer* ip = &gMpSession.players[gVoteSession.initiatorNetId - 1];
            if (ip->isConnected && ip->obj != nullptr) {
                reg_anim_clear(ip->obj);
                int safeTile = hexGridTileIsValid(gVoteSession.lastSafeTile)
                    ? gVoteSession.lastSafeTile
                    : ip->obj->tile;
                int safeElevation = elevationIsValid(gVoteSession.lastSafeElevation)
                    ? gVoteSession.lastSafeElevation
                    : ip->obj->elevation;
                debugFilePrint("MP: vote failed teleport initiator=%u from=%d to=%d elev=%d",
                    gVoteSession.initiatorNetId, ip->obj->tile, safeTile, safeElevation);
                gMpSuppressExitGridCheck = true;
                objectSetLocation(ip->obj, safeTile, safeElevation, nullptr);
                objectSetRotation(ip->obj, gVoteSession.lastSafeRotation, nullptr);
                gMpSuppressExitGridCheck = false;
            }
        }
        gVoteSession.state = VOTE_STATE_NONE;
    }
}

void MpVoteCancel()
{
    if (!gMpIsHost || gVoteSession.state != VOTE_STATE_ACTIVE) {
        return;
    }
    debugFilePrint("MP: vote cancelled initiator=%u", gVoteSession.initiatorNetId);
    gVoteSession.state = VOTE_STATE_CANCELLED;
    gMpSession.initiatorFrozen = false;
    MpVoteHideUI();
    NetVoteResultPayload result;
    result.passed = 0;
    result.yesCount = gVoteSession.yesCount;
    result.totalCount = gVoteSession.totalPlayers;
    NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE, NET_PKT_VOTE_RESULT, &result, sizeof(result));
    gVoteSession.state = VOTE_STATE_NONE;
}

// ---------------------------------------------------------------------------
// Client side
// ---------------------------------------------------------------------------

void MpVoteOnVoteStart(const NetVoteStartPayload* payload)
{
    if (!gMpIsClient || payload == nullptr) {
        return;
    }
    MpVoteHideUI();
    memset(&gVoteSession, 0, sizeof(gVoteSession));
    gVoteSession.state = VOTE_STATE_ACTIVE;
    gVoteSession.initiatorNetId = payload->initiatorNetId;
    gVoteSession.transition.map = payload->targetMap;
    gVoteSession.transition.tile = payload->targetTile;
    gVoteSession.transition.elevation = payload->targetElevation;
    gVoteSession.transition.rotation = payload->targetRotation;
    gVoteSession.startTime = getTicks();
    gVoteSession.timeoutMs = payload->timeoutMs;
    gVoteSession.totalPlayers = payload->totalPlayers;
    gVoteSession.isInitiator = (payload->initiatorNetId == gMpSession.localNetId);
    gVoteSession.voteWindow = -1;
    gVoteSession.uiPending = true;
    // Mirror the host's initiator auto-vote so the local modal starts at the
    // right number (1/total) before the first tally packet arrives.
    gVoteSession.yesCount = 0;
    gVoteSession.noCount = 0;
    if (gVoteSession.isInitiator && gMpSession.localNetId >= 1
        && gMpSession.localNetId <= NET_MAX_PLAYERS) {
        gVoteSession.votes[gMpSession.localNetId - 1] = 1;
        gVoteSession.yesCount = 1;
    }
    debugFilePrint("MP: vote start received initiator=%u targetMap=%d local=%u isInitiator=%d",
        payload->initiatorNetId, payload->targetMap, gMpSession.localNetId,
        gVoteSession.isInitiator ? 1 : 0);
    if (gVoteSession.isInitiator) {
        gMpSession.initiatorFrozen = true;
        // Kill the local walk immediately: the host has already stopped the
        // critter on the exit grid and will now drive its position
        // authoritatively. Leaving the local prediction running would walk
        // the dude past the zone on the client's own screen.
        if (gDude != nullptr) {
            reg_anim_clear(gDude);
        }
    }
}

void MpVoteOnVoteResult(const NetVoteResultPayload* payload)
{
    if (!gMpIsClient || payload == nullptr) {
        return;
    }
    debugFilePrint("MP: vote result received passed=%d yes=%u total=%u",
        payload->passed ? 1 : 0, payload->yesCount, payload->totalCount);
    if (gVoteSession.isInitiator) {
        gMpSession.initiatorFrozen = false;
    }
    if (payload->passed) {
        // Keep the modal up showing the final tally. It closes when the host's
        // MAP_CHANGED actually arrives (MpApplyMapChanged hides the window),
        // so the player sees the updated number right up until the map change.
        gVoteSession.yesCount = payload->yesCount;
        gVoteSession.totalPlayers = payload->totalCount;
        gVoteSession.state = VOTE_STATE_PASSED;
    } else {
        MpVoteHideUI();
        // The host owns positions; clients will receive the corrective
        // state update for the initiator's critter via PLAYER_STATE_UPDATE.
        gVoteSession.state = VOTE_STATE_NONE;
    }
}

void MpVoteSendCast(uint8_t vote)
{
    if (!gMpIsClient || gMpSession.hostPeer == nullptr) {
        return;
    }
    NetVoteCastPayload p;
    p.voterNetId = gMpSession.localNetId;
    p.vote = vote ? 1 : 0;
    debugFilePrint("MP: vote cast sent vote=%d", p.vote);
    NetSendPacket(gMpSession.hostPeer, NET_CHANNEL_RELIABLE, NET_PKT_VOTE_CAST, &p, sizeof(p));
}

// ---------------------------------------------------------------------------
// Vote UI
// ---------------------------------------------------------------------------

// Same centering math as the multiplayer menu.
static void mpVoteCenteredPos(int width, int height, int* outX, int* outY)
{
    *outX = (screenGetWidth() - width) / 2;
    if (*outX < 0) {
        *outX = 0;
    }
    *outY = (screenGetHeight() - height) / 2 - 30;
    if (*outY < 0) {
        *outY = 0;
    }
}

// Blocking modal loop — the multiplayer menu technique. The window was
// created, populated and refreshed by the caller; this loop pumps input and
// the network (MpTick) until the vote resolves and the window is dismissed.
// Voters cast inside the loop and the modal stays up showing the live tally
// until the vote resolves (host: PASSED display beat elapses; client:
// MAP_CHANGED hides the window). The initiator's Cancel/ESC returns 0 so the
// caller can veto the proposal. Returns 1 for YES, 0 for NO/ESC (initiator
// only), -2 when the vote resolved externally.
static int mpRunVoteModal()
{
    int rc = -1;
    while (rc == -1) {
        sharedFpsLimiter.mark();
        int keyCode = inputGetInput();
        if (gVoteSession.state == VOTE_STATE_ACTIVE) {
            if (keyCode == MP_VOTE_BTN_YES) {
                if (gVoteSession.isInitiator) {
                    rc = 1; // initiator modal has no Yes button; harmless
                } else if (gMpIsHost) {
                    // Host voter: cast and keep the modal up showing the live
                    // tally until the vote resolves.
                    MpVoteCastVote(gMpSession.localNetId, 1);
                } else {
                    MpVoteSendCast(1);
                }
            } else if (keyCode == MP_VOTE_BTN_NO || keyCode == KEY_ESCAPE) {
                if (gVoteSession.isInitiator) {
                    rc = 0; // cancel the proposal (host cancels, client vetoes)
                } else if (gMpIsHost) {
                    MpVoteCastVote(gMpSession.localNetId, 0);
                } else {
                    MpVoteSendCast(0);
                }
            }
        }
        MpTick(); // pump packets; VOTE_RESULT may set state != ACTIVE and hide
                  // voteWindow via MpVoteHideUI concurrently.
        if (gVoteSession.state == VOTE_STATE_PASSED) {
            // Vote passed: keep showing the final tally. The host holds the
            // modal for the display beat, then starts the transition; the
            // client keeps it until MAP_CHANGED hides the window.
            if (gMpIsHost
                && getTicksSince(gVoteSession.resolveDisplayStart) >= VOTE_PASSED_DISPLAY_MS) {
                rc = -2;
            } else if (gVoteSession.voteWindow == -1) {
                rc = -2;
            }
        } else if (gVoteSession.state != VOTE_STATE_ACTIVE && gVoteSession.voteWindow == -1) {
            // Host timed out or other voters resolved it while we were
            // waiting — modal already dismissed.
            rc = -2;
        }
        renderPresent();
        sharedFpsLimiter.throttle();
    }
    return rc;
}

// Show the vote window on the next top-level MpTick. Never invoked from
// inside a NetHostService callback: the modal blocks and pumps MpTick, which
// would re-enter NetHostService.
void MpVoteMaybeShowUI()
{
    if (gVoteSession.state != VOTE_STATE_ACTIVE || gVoteSession.voteWindow != -1
        || !gVoteSession.uiPending) {
        return;
    }
    gVoteSession.uiPending = false;
    if (gVoteSession.isInitiator) {
        MpVoteShowInitiatorUI();
    } else {
        MpVoteShowVoterUI();
    }
}

void MpVoteShowInitiatorUI()
{
    MpVoteHideUI();
    int x, y;
    mpVoteCenteredPos(MP_VOTE_WINDOW_W, MP_VOTE_WINDOW_H, &x, &y);
    int win = windowCreate(x, y, MP_VOTE_WINDOW_W, MP_VOTE_WINDOW_H, COLOR_BLACK, WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    if (win == -1) {
        return;
    }
    windowDrawBorder(win);

    const char* title = "MAP CHANGE VOTE";
    int titleX = (MP_VOTE_WINDOW_W - fontGetStringWidth(title)) / 2;
    windowDrawText(win, title, 0, titleX, 6, COLOR_WHITE);
    windowDrawText(win, "You want to change map.", 0, 40, 28, COLOR_WHITE);

    _win_register_text_button(win, 40, 80, -1, -1, -1, MP_VOTE_BTN_NO, "Cancel", 0);
    gVoteSession.voteWindow = win;
    windowRefresh(win);

    // The initiator auto-voted YES; the modal stays up (network pumping via
    // MpTick inside the loop) until the vote resolves. ESC/Cancel vetoes the
    // proposal: the host cancels directly, a client initiator casts a NO that
    // the host applies as a vote flip.
    int choice = mpRunVoteModal();
    MpVoteHideUI();
    if (choice == 0) {
        if (gMpIsHost) {
            MpVoteCancel();
        } else {
            MpVoteSendCast(0);
        }
    }
    // Vote passed: the modal just finished its display beat — start the map
    // change now (host only; the client waits for MAP_CHANGED).
    mpVoteStartTransitionIfPassed();
}

void MpVoteShowVoterUI()
{
    MpVoteHideUI();
    int x, y;
    mpVoteCenteredPos(MP_VOTE_WINDOW_W, MP_VOTE_WINDOW_H, &x, &y);
    int win = windowCreate(x, y, MP_VOTE_WINDOW_W, MP_VOTE_WINDOW_H, COLOR_BLACK, WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    if (win == -1) {
        return;
    }
    windowDrawBorder(win);

    const char* title = "MAP CHANGE VOTE";
    int titleX = (MP_VOTE_WINDOW_W - fontGetStringWidth(title)) / 2;
    windowDrawText(win, title, 0, titleX, 6, COLOR_WHITE);
    windowDrawText(win, "Another player wants to change map.", 0, 40, 28, COLOR_WHITE);

    _win_register_text_button(win, 40, 55, -1, -1, -1, MP_VOTE_BTN_YES, "Yes", 0);
    _win_register_text_button(win, 40, 80, -1, -1, -1, MP_VOTE_BTN_NO, "No", 0);
    gVoteSession.voteWindow = win;
    windowRefresh(win);

    // Voter casts are handled inside the modal loop; it exits when the vote
    // resolves — host: PASSED display beat elapsed, client: MAP_CHANGED hid
    // the window. The live tally stays visible the whole time.
    mpRunVoteModal();
    MpVoteHideUI();
    // Vote passed: the modal just finished its display beat — start the map
    // change now (host only; the client waits for MAP_CHANGED).
    mpVoteStartTransitionIfPassed();
}

void MpVoteHideUI()
{
    if (gVoteSession.voteWindow != -1) {
        windowDestroy(gVoteSession.voteWindow);
        gVoteSession.voteWindow = -1;
    }
}

void MpVoteUpdateUI()
{
    if (gVoteSession.voteWindow == -1
        || (gVoteSession.state != VOTE_STATE_ACTIVE && gVoteSession.state != VOTE_STATE_PASSED)) {
        return;
    }
    uint32_t elapsed = getTicksSince(gVoteSession.startTime);
    uint32_t remainingMs = (gVoteSession.timeoutMs > elapsed)
        ? (gVoteSession.timeoutMs - elapsed)
        : 0;
    uint32_t remainingS = remainingMs / 1000;

    char buf[64];
    if (gVoteSession.state == VOTE_STATE_PASSED) {
        // Final tally, held until the map change actually starts.
        snprintf(buf, sizeof(buf), "Votes: %u/%u", (unsigned)gVoteSession.yesCount,
            (unsigned)gVoteSession.totalPlayers);
    } else {
        snprintf(buf, sizeof(buf), "Votes: %u/%u   %us", (unsigned)gVoteSession.yesCount,
            (unsigned)gVoteSession.totalPlayers, (unsigned)remainingS);
    }
    // Voters see the tally below their buttons; the initiator above its.
    int tallyY = gVoteSession.isInitiator ? 50 : 105;
    // No DRAW_TEXT_FLAG_NO_BG: the background fill erases the previous
    // frame's glyphs, so the changing countdown never ghosts.
    windowDrawText(gVoteSession.voteWindow, buf, 0, 40, tallyY, COLOR_WHITE);
    windowRefresh(gVoteSession.voteWindow);
}

} // namespace fallout
