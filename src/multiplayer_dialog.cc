// Synchronized dialogue and barter (host-authoritative).
//
// The host owns every dialogue script and barter table. While a dialogue is
// open, the host's blocking gameDialogProcessUI loop is pumped by
// MpDialogHostPump() every frame: the network and world keep running behind
// the dialogue, participants vote on options (unanimous -> instant, strict
// majority -> 15s timer), the transcript streams to every connected player's
// combat log, and barter runs as an integrated host-authoritative session.
//
// Clients never run dialogue scripts. They receive node state and vote
// through reliable packets and render their own opaque modal (the
// multiplayer-menu technique, per AGENTS.md).
#include "multiplayer_dialog.h"

#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <string>

#include "combat.h"
#include "color.h"
#include "critter.h"
#include "debug.h"
#include "display_monitor.h"
#include "fps_limiter.h"
#include "game.h"
#include "game_dialog.h"
#include "input.h"
#include "inventory.h"
#include "item.h"
#include "kb.h"
#include "memory.h"
#include "message.h"
#include "mouse.h"
#include "multiplayer.h"
#include "multiplayer_combat.h"
#include "multiplayer_profile.h"
#include "multiplayer_vote.h"
#include "multiplayer_log.h"
#include "object.h"
#include "perk.h"
#include "proto.h"
#include "proto_types.h"
#include "random.h"
#include "reaction.h"
#include "skill.h"
#include "stat.h"
#include "stat_defs.h"
#include "svga.h"
#include "text_font.h"
#include "text_object.h"
#include "tile.h"
#include "time.h"
#include "window_manager.h"

namespace fallout {

// ---------------------------------------------------------------------------
// Session model
// ---------------------------------------------------------------------------

struct MpDialogOption {
    int16_t msgListId;
    int16_t msgId;
    int8_t reaction;
    int16_t proc; // script procedure index (host-side, join re-injection)
    char text[NET_DIALOG_OPTION_TEXT_MAX + 1];
};

struct MpDialogVoter {
    int8_t selected; // -1 = none
    bool suspended;  // bartering
};

struct MpBarterSession {
    bool active;
    uint8_t netId;
    Object* offerTable;
    Object* requestTable;
};

struct MpDialogSession {
    bool active;
    uint32_t sessionId;
    uint16_t nodeSeq;
    Object* speaker;
    uint32_t speakerNetId;
    uint8_t initiatorNetId; // 0 = none (scripted)
    uint8_t participants[NET_MAX_PLAYERS]; // join order
    int participantCount;
    MpDialogVoter voters[NET_MAX_PLAYERS]; // indexed by netId - 1
    char replyText[NET_DIALOG_REPLY_MAX + 1];
    int optionCount;
    MpDialogOption options[NET_DIALOG_MAX_OPTIONS];
    uint32_t lastNodeHash;
    int8_t resolvedOption;
    bool timerActive;
    uint32_t timerStart;
    uint16_t timerMs;
    int8_t majorityOption;
    char transcript[NET_DIALOG_TRANSCRIPT_REPLAY][NET_DIALOG_TRANSCRIPT_MAX + 1];
    uint32_t transcriptEmitted;
    bool hostParticipant;
    bool hostUiHidden;
    bool exitRequested;
    int hostVoteWindow;
    MpBarterSession barter[NET_MAX_PLAYERS]; // indexed by netId - 1
    bool hostBarterUiOpen;
    bool barterDirty; // host's own trade screen needs a re-render
    bool hostJoinNeedsModal; // host joined mid-session: enter the modal
    bool joinParkedScript; // the talk script is parked at dialog_go by the join
    bool voteUiDirty; // option texts need a re-render (chooser names)
    uint8_t pendingInitiator; // consumed at first node
};

static MpDialogSession gMpDialog = {};

// Builds the current host node's option texts with the chooser names appended
// (defined below; used by the join modal and the host pump re-render).
static void mpDialogHostBuildNodeTexts(const char** texts, int* reactions, int* procs);

// Synchronized dialogue flavor: float `text` over `obj` (host display) and
// relay it to every client through the float channel. The dialogue floats use
// an explicit style (NET_FLOAT_DIALOG_STYLE) so host and client render the
// identical text — NPC lines in light yellow over the speaker, player choices
// in white over the choosing avatar. The full line stays in the combat-log
// transcript; the float is a capped echo (120 chars).
//
// The NPC and the choosing player stand adjacent, so their floats would
// overlap: every new float pushes every tracked owner's live floats one
// float-height up (textObjectsShiftVertically), so the newest always sits at
// the bottom and older lines stack above. Dialogue floats never replace each
// other (textObjectAddNoReplace) — every line lives until its natural fade.
static Object* gMpDialogFloatStack[MP_DIALOG_FLOAT_STACK_MAX];
static int gMpDialogFloatStackCount = 0;

static void mpDialogFloat(Object* obj, const char* text, int font, int color, int outline)
{
    if (obj == nullptr || text == nullptr || text[0] == '\0' || !gMpActive) {
        return;
    }
    char capped[121];
    snprintf(capped, sizeof(capped), "%.120s", text);

    // Stack first, add last: every tracked owner's live floats climb by a
    // UNIFORM amount (textObjectsComputeStackShift) BEFORE the new line
    // exists, so the new line lands at the bottom slot and is never caught in
    // its own shift, and the old floats keep their relative spacing (per-owner
    // shifts would break it — adjacent owners anchor to different tiles).
    const int floatHeight = textObjectMeasure(capped, font, outline);
    if (floatHeight > 0) {
        const int dy = textObjectsComputeStackShift(obj->tile, floatHeight);
        if (dy > 0) {
            for (int i = 0; i < gMpDialogFloatStackCount; i++) {
                textObjectsShiftVertically(gMpDialogFloatStack[i], -dy);
            }
        }
    }
    for (int i = 0; i < gMpDialogFloatStackCount; i++) {
        if (gMpDialogFloatStack[i] == obj) {
            gMpDialogFloatStackCount--;
            for (int j = i; j < gMpDialogFloatStackCount; j++) {
                gMpDialogFloatStack[j] = gMpDialogFloatStack[j + 1];
            }
            break;
        }
    }
    if (gMpDialogFloatStackCount < MP_DIALOG_FLOAT_STACK_MAX) {
        gMpDialogFloatStack[gMpDialogFloatStackCount++] = obj;
    }

    Rect rect;
    if (textObjectAddNoReplace(obj, const_cast<char*>(capped), font, color, outline, &rect) != 0) {
        return; // nothing was created — no stacking to do
    }

    tileWindowRefreshRect(&rect, obj->elevation);
    if (gMpIsHost) {
        uint32_t netId = MpGetObjNetId(obj);
        if (netId != 0) {
            NetFloatMessagePayload payload;
            memset(&payload, 0, sizeof(payload));
            payload.netId = netId;
            payload.type = NET_FLOAT_DIALOG_STYLE;
            payload.font = font;
            payload.color = color;
            payload.outline = outline;
            strncpy(payload.text, capped, sizeof(payload.text) - 1);
            NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE, NET_PKT_FLOAT_MESSAGE,
                &payload, sizeof(payload));
            MpLog(MP_LOG_DIALOG, "float relayed netId=%u font=%d color=%d outline=%d text='%.48s'",
                netId, font, color, outline, capped);
        }
    }
}

struct MpDialogClientState {
    bool sessionActive;
    uint32_t sessionId;
    uint32_t speakerObjNetId;
    uint16_t nodeSeq;
    uint8_t initiatorNetId;
    uint8_t participants[NET_MAX_PLAYERS];
    int participantCount;
    int8_t selections[NET_MAX_PLAYERS];
    bool suspended[NET_MAX_PLAYERS];
    char replyText[NET_DIALOG_REPLY_MAX + 1];
    int optionCount;
    MpDialogOption options[NET_DIALOG_MAX_OPTIONS];
    int8_t resolvedOption;
    bool timerActive;
    uint32_t timerEndTick;
    // chunk accumulation
    uint32_t chunkSession;
    uint16_t chunkNodeSeq;
    uint16_t chunkCount;
    std::string chunkBuffer;
    // modal
    int window;
    bool uiPending;
    bool modalOpen;
    int pageIndex; // options page
    // barter
    bool barterActive;
    NetBarterItem npcItems[NET_BARTER_MAX_ITEMS];
    int npcItemCount;
    NetBarterItem offers[NET_BARTER_MAX_ITEMS];
    int offerCount;
    NetBarterItem requests[NET_BARTER_MAX_ITEMS];
    int requestCount;
    uint8_t lastOp;
    uint8_t lastOk;
    uint8_t lastMsgId;
    int16_t barterMod; // host-computed barter modifier (incl. reaction)
    // vanilla-style dialogue screen
    int32_t headFid;
    int8_t reaction;
    bool nodeApplied;  // at least one node rendered into the vanilla windows
    char audioName[64]; // lip-sync speech base name for this node (v3 bodies)
    // vanilla trade screen (barter)
    bool tradeOpen;
    bool barterDirty;  // trade screen needs a re-render from fresh state
    Object* offerMirror;   // local mirror of the host-authoritative offer table
    Object* requestMirror; // local mirror of the host-authoritative request table
};

static MpDialogClientState gMpDialogClient = {};

static constexpr int MP_DIALOG_WINDOW_W = 620;
static constexpr int MP_DIALOG_WINDOW_H = 400;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool mpDialogPlayerConnected(uint8_t netId)
{
    if (netId < 1 || netId > NET_MAX_PLAYERS) {
        return false;
    }
    return gMpSession.players[netId - 1].isConnected;
}

static Object* mpDialogPlayerAvatar(uint8_t netId)
{
    if (netId < 1 || netId > NET_MAX_PLAYERS) {
        return nullptr;
    }
    return gMpSession.players[netId - 1].obj;
}

Object* MpDialogPendingInitiatorAvatar()
{
    if (!gMpActive || !gMpIsHost || gMpDialog.pendingInitiator == 0) {
        return nullptr;
    }
    return mpDialogPlayerAvatar(gMpDialog.pendingInitiator);
}

Object* MpDialogInitiatorAvatar()
{
    if (!gMpActive || !gMpIsHost || !gMpDialog.active || gMpDialog.initiatorNetId == 0) {
        return nullptr;
    }
    return mpDialogPlayerAvatar(gMpDialog.initiatorNetId);
}

static bool mpDialogIsParticipant(uint8_t netId)
{
    for (int i = 0; i < gMpDialog.participantCount; i++) {
        if (gMpDialog.participants[i] == netId) {
            return true;
        }
    }
    return false;
}

// FNV-1a over the node content.
static uint32_t mpDialogNodeHash(const MpDialogNodeData* node)
{
    uint32_t hash = 2166136261u;
    auto mix = [&hash](const void* data, size_t size) {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; i++) {
            hash ^= bytes[i];
            hash *= 16777619u;
        }
    };
    mix(node->replyText, strlen(node->replyText) + 1);
    mix(&node->optionCount, sizeof(node->optionCount));
    for (int i = 0; i < node->optionCount && i < NET_DIALOG_MAX_OPTIONS; i++) {
        mix(&node->optionMsgListIds[i], sizeof(int));
        mix(&node->optionMsgIds[i], sizeof(int));
        mix(&node->optionReactions[i], sizeof(int));
        mix(node->optionTexts[i], strlen(node->optionTexts[i]) + 1);
    }
    return hash;
}

// Host never sends packets to itself (its own peer is null).
static bool mpDialogCanSendTo(uint8_t netId)
{
    return netId >= 1 && netId <= NET_MAX_PLAYERS
        && netId != gMpSession.localNetId
        && mpDialogPlayerConnected(netId);
}

// ---------------------------------------------------------------------------
// Transcript
// ---------------------------------------------------------------------------

static void mpDialogEmitTranscript(uint8_t speakerNetId, const char* text)
{
    if (!gMpDialog.active) {
        return;
    }

    char line[NET_DIALOG_TRANSCRIPT_MAX + 1];
    strncpy(line, text, NET_DIALOG_TRANSCRIPT_MAX);
    line[NET_DIALOG_TRANSCRIPT_MAX] = '\0';

    uint32_t seq = ++gMpDialog.transcriptEmitted;
    int slot = (seq - 1) % NET_DIALOG_TRANSCRIPT_REPLAY;
    strncpy(gMpDialog.transcript[slot], line, NET_DIALOG_TRANSCRIPT_MAX);
    gMpDialog.transcript[slot][NET_DIALOG_TRANSCRIPT_MAX] = '\0';

    NetDialogTranscriptPayload p;
    memset(&p, 0, sizeof(p));
    p.sessionId = gMpDialog.sessionId;
    p.seq = (uint16_t)seq;
    p.speakerNetId = speakerNetId;
    strncpy(p.text, line, NET_DIALOG_TRANSCRIPT_MAX);

    NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE, NET_PKT_DIALOG_TRANSCRIPT, &p, sizeof(p));
    displayMonitorAddMessage(line);
    MpLog(MP_LOG_DIALOG, "transcript session=%u seq=%u speaker=%u text=%s", gMpDialog.sessionId, seq, speakerNetId, line);
}

// Strips the vanilla "1. " / bullet prefix from an option entry.
static void mpDialogStripOptionPrefix(const char* in, char* out, size_t outSize)
{
    const char* src = in;
    if (src[0] == '\x95') {
        src += 1;
        while (*src == ' ') {
            src++;
        }
    } else if (src[0] >= '1' && src[0] <= '9') {
        while (*src >= '0' && *src <= '9') {
            src++;
        }
        if (*src == '.') {
            src++;
        }
        while (*src == ' ') {
            src++;
        }
    }
    strncpy(out, src, outSize - 1);
    out[outSize - 1] = '\0';
}

static void mpDialogEmitNpcLine()
{
    char line[NET_DIALOG_TRANSCRIPT_MAX + 1];
    if (gMpDialog.replyText[0] == '\0') {
        return;
    }
    const char* name = gMpDialog.speaker != nullptr ? critterGetName(gMpDialog.speaker) : "NPC";
    snprintf(line, sizeof(line), "%s: %s", name, gMpDialog.replyText);
    mpDialogEmitTranscript(0, line);
}

// The name of the current dialogue initiator — whoever holds the role at the
// moment (it hands off when the initiator leaves or disconnects). Returns
// nullptr when there is no initiator.
// Resolve a participant's display name for dialogue output. The player slots
// carry the names exchanged during the handshake, but the client's OWN slot
// is never filled locally (only the other players are broadcast to it) — so
// always check the slot and fall back to the local avatar's name, then "?".
// The dialogue must always show the correct name regardless of which side
// renders it.
static const char* mpDialogParticipantName(uint8_t netId)
{
    if (netId >= 1 && netId <= NET_MAX_PLAYERS) {
        const char* name = gMpSession.players[netId - 1].name;
        if (name != nullptr && name[0] != '\0') {
            return name;
        }
        if (netId == gMpSession.localNetId && gDude != nullptr) {
            return critterGetName(gDude);
        }
    }
    return "?";
}

static void mpDialogEmitResolvedLine(int optionIndex)
{
    char line[NET_DIALOG_TRANSCRIPT_MAX + 1];
    char text[NET_DIALOG_OPTION_TEXT_MAX + 1];
    mpDialogStripOptionPrefix(gMpDialog.options[optionIndex].text, text, sizeof(text));

    // Supporters: connected, non-suspended participants who picked this option.
    char names[NET_DIALOG_TRANSCRIPT_MAX] = {};
    int firstName = 0;
    for (int i = 0; i < gMpDialog.participantCount; i++) {
        uint8_t netId = gMpDialog.participants[i];
        if (!mpDialogPlayerConnected(netId) || gMpDialog.voters[netId - 1].suspended) {
            continue;
        }
        if (gMpDialog.voters[netId - 1].selected != optionIndex) {
            continue;
        }
        const char* name = mpDialogParticipantName(netId);
        if (firstName == 0) {
            strncpy(names, name, sizeof(names) - 1);
            firstName = netId;
        } else {
            strncat(names, " + ", sizeof(names) - strlen(names) - 1);
            strncat(names, name, sizeof(names) - strlen(names) - 1);
        }
    }
    if (firstName == 0) {
        strncpy(names, "?", sizeof(names) - 1);
    }
    snprintf(line, sizeof(line), "%s: %s", names, text);
    mpDialogEmitTranscript((uint8_t)firstName, line);
}

// ---------------------------------------------------------------------------
// Vote
// ---------------------------------------------------------------------------

static void mpDialogBroadcastVote()
{
    if (!gMpDialog.active) {
        return;
    }

    char buffer[512];
    uint8_t* cursor = reinterpret_cast<uint8_t*>(buffer);
    NetDialogVotePayload* p = reinterpret_cast<NetDialogVotePayload*>(cursor);
    p->sessionId = gMpDialog.sessionId;
    p->nodeSeq = gMpDialog.nodeSeq;
    p->initiatorNetId = gMpDialog.initiatorNetId;
    p->resolvedOption = gMpDialog.resolvedOption;
    p->timerActive = gMpDialog.timerActive ? 1 : 0;
    p->timerMs = 0;
    if (gMpDialog.timerActive) {
        uint32_t remaining = gMpDialog.timerStart + gMpDialog.timerMs - getTicks();
        p->timerMs = remaining > 0 ? (uint16_t)remaining : 0;
    }
    p->participantCount = (uint8_t)gMpDialog.participantCount;
    cursor += sizeof(NetDialogVotePayload);

    for (int i = 0; i < gMpDialog.participantCount; i++) {
        uint8_t netId = gMpDialog.participants[i];
        NetDialogVoteEntry* entry = reinterpret_cast<NetDialogVoteEntry*>(cursor);
        entry->netId = netId;
        entry->optionIndex = (uint8_t)(gMpDialog.voters[netId - 1].selected & 0xFF);
        entry->flags = gMpDialog.voters[netId - 1].suspended ? 1 : 0;
        cursor += sizeof(NetDialogVoteEntry);
    }

    size_t length = (size_t)(cursor - reinterpret_cast<uint8_t*>(buffer));
    for (int i = 0; i < gMpDialog.participantCount; i++) {
        uint8_t netId = gMpDialog.participants[i];
        if (!mpDialogCanSendTo(netId)) {
            continue;
        }
        NetSendPacket(gMpSession.players[netId - 1].peer, NET_CHANNEL_RELIABLE, NET_PKT_DIALOG_VOTE, buffer, length);
    }
}

static void mpDialogResolve(int optionIndex)
{
    if (!gMpDialog.active || optionIndex < 0 || optionIndex >= gMpDialog.optionCount) {
        return;
    }

    mpDialogEmitResolvedLine(optionIndex);

    gMpDialog.timerActive = false;
    gMpDialog.majorityOption = -1;
    for (int i = 0; i < gMpDialog.participantCount; i++) {
        gMpDialog.voters[gMpDialog.participants[i] - 1].selected = -1;
    }
    gMpDialog.resolvedOption = (int8_t)optionIndex;

    MpLog(MP_LOG_DIALOG, "resolve session=%u node=%u option=%d", gMpDialog.sessionId, gMpDialog.nodeSeq, optionIndex);
    mpDialogBroadcastVote();
}

// Recomputes the vote after any participant/selection/suspension change.
static void mpDialogRecalcVote()
{
    if (!gMpDialog.active) {
        return;
    }

    // The option texts (chooser names) changed with the vote state; the
    // host's modal re-renders them on the next pump.
    gMpDialog.voteUiDirty = true;

    int counts[NET_DIALOG_MAX_OPTIONS] = {};
    int activeVoters = 0;
    for (int i = 0; i < gMpDialog.participantCount; i++) {
        uint8_t netId = gMpDialog.participants[i];
        if (!mpDialogPlayerConnected(netId) || gMpDialog.voters[netId - 1].suspended) {
            continue;
        }
        activeVoters++;
        if (gMpDialog.voters[netId - 1].selected >= 0
            && gMpDialog.voters[netId - 1].selected < gMpDialog.optionCount) {
            counts[gMpDialog.voters[netId - 1].selected]++;
        }
    }

    if (activeVoters <= 0) {
        // Nobody can vote. Wait (the leave path ends the session when empty).
        if (gMpDialog.timerActive) {
            gMpDialog.timerActive = false;
            gMpDialog.majorityOption = -1;
            MpLog(MP_LOG_DIALOG, "vote timer cancelled (no active voters) session=%u", gMpDialog.sessionId);
        }
        mpDialogBroadcastVote();
        return;
    }

    // Unanimous?
    int unanimousOption = -1;
    bool unanimous = true;
    for (int o = 0; o < gMpDialog.optionCount; o++) {
        if (counts[o] == activeVoters) {
            unanimousOption = o;
        } else if (counts[o] > 0) {
            unanimous = false;
        }
    }
    if (unanimous && unanimousOption >= 0) {
        mpDialogResolve(unanimousOption);
        return;
    }

    // Strict majority (> half)? Timer is tied to that option.
    int majorityOption = -1;
    for (int o = 0; o < gMpDialog.optionCount; o++) {
        if (counts[o] > activeVoters / 2) {
            if (majorityOption == -1) {
                majorityOption = o;
            } else {
                majorityOption = -2; // two options above half is impossible; defensive
            }
        }
    }

    if (majorityOption >= 0) {
        if (!gMpDialog.timerActive || gMpDialog.majorityOption != majorityOption) {
            gMpDialog.timerActive = true;
            gMpDialog.majorityOption = (int8_t)majorityOption;
            gMpDialog.timerStart = getTicks();
            gMpDialog.timerMs = NET_DIALOG_VOTE_TIMER_MS;
            MpLog(MP_LOG_DIALOG, "majority session=%u node=%u option=%d timer=%dms",
                gMpDialog.sessionId, gMpDialog.nodeSeq, majorityOption, NET_DIALOG_VOTE_TIMER_MS);
        }
    } else {
        if (gMpDialog.timerActive) {
            gMpDialog.timerActive = false;
            gMpDialog.majorityOption = -1;
            MpLog(MP_LOG_DIALOG, "vote timer cancelled (majority lost) session=%u", gMpDialog.sessionId);
        }
    }

    mpDialogBroadcastVote();
}

// ---------------------------------------------------------------------------
// Host: session lifecycle
// ---------------------------------------------------------------------------

static void mpDialogHostTeardownBarter(uint8_t netId)
{
    MpBarterSession* b = &gMpDialog.barter[netId - 1];
    if (!b->active) {
        return;
    }
    MpLog(MP_LOG_DIALOG, "MPBARTER teardown session=%u netId=%u", gMpDialog.sessionId, netId);
    if (b->offerTable != nullptr) {
        itemMoveAll(b->offerTable, mpDialogPlayerAvatar(netId));
        objectDestroy(b->offerTable, nullptr);
    }
    if (b->requestTable != nullptr) {
        itemMoveAll(b->requestTable, gMpDialog.speaker);
        objectDestroy(b->requestTable, nullptr);
    }
    b->offerTable = nullptr;
    b->requestTable = nullptr;
    b->active = false;
}

static void mpDialogHostTeardownAllBarter()
{
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        if (gMpDialog.barter[i].active) {
            mpDialogHostTeardownBarter(i + 1);
        }
    }
}

static void mpDialogHostCloseVoteOverlay()
{
    if (gMpDialog.hostVoteWindow != -1) {
        windowDestroy(gMpDialog.hostVoteWindow);
        gMpDialog.hostVoteWindow = -1;
    }
}

static void mpDialogHostClearSession()
{
    mpDialogHostCloseVoteOverlay();
    mpDialogHostTeardownAllBarter();
    // The vanilla dialogue state needs its full teardown when the script is
    // parked at dialog_go: the director case (host not a participant) and the
    // join-modal case (the host joined mid-session and the script never
    // resumed). Both leave the reply program's stack elevated — a fresh
    // program reset is required or the next talk on the same script crashes.
    bool directorParked = gMpDialog.active && (!gMpDialog.hostParticipant || gMpDialog.joinParkedScript);
    memset(&gMpDialog, 0, sizeof(gMpDialog));
    gMpDialog.hostVoteWindow = -1;
    if (directorParked) {
        // The vanilla dialogue state is parked (windows alive or destroyed by
        // the join-modal exit, _gdialog_state INACTIVE). Run the full vanilla
        // teardown now. It is guarded internally and its MpDialogHostEnd hook
        // is a no-op once the session is already cleared here.
        MpLog(MP_LOG_DIALOG, "director finish (parked vanilla state teardown)");
        MpDialogDirectorFinishDialogue();
    }
}

static void mpDialogHostAbort(uint8_t reason)
{
    if (!gMpDialog.active) {
        return;
    }

    MpLogAlways(MP_LOG_DIALOG, "abort session=%u reason=%u", gMpDialog.sessionId, reason);

    NetDialogEndPayload p;
    p.sessionId = gMpDialog.sessionId;
    p.reason = reason;
    NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE, NET_PKT_DIALOG_END, &p, sizeof(p));

    mpDialogHostClearSession();
    // Must survive the memset: the modal's ShouldExit check reads it right
    // after the pump returns.
    gMpDialog.exitRequested = true;
}

void MpDialogHostEnd()
{
    // Normal script end (end_dialogue -> _gdialogExitFromScript).
    gMpDialog.pendingInitiator = 0;
    if (!gMpDialog.active) {
        return;
    }

    MpLog(MP_LOG_DIALOG, "end session=%u normal", gMpDialog.sessionId);
    NetDialogEndPayload p;
    p.sessionId = gMpDialog.sessionId;
    p.reason = NET_DIALOG_END_NORMAL;
    NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE, NET_PKT_DIALOG_END, &p, sizeof(p));

    mpDialogHostClearSession();
}

void MpDialogHostAbortCombat()
{
    if (!gMpActive || !gMpIsHost || !gMpDialog.active) {
        return;
    }
    // Combat is taking over the main loop: the director tick can no longer
    // run (the blocking combat pump owns it), so the session must be closed
    // here or every client's dialogue modal hangs forever on the dead node,
    // deferring all combat packets.
    mpDialogHostAbort(NET_DIALOG_END_COMBAT);
}

void MpDialogSetPendingInitiator(uint8_t netId)
{
    gMpDialog.pendingInitiator = netId;
    MpLog(MP_LOG_DIALOG, "pending initiator=%u", netId);
}

void MpDialogClearPendingInitiator()
{
    gMpDialog.pendingInitiator = 0;
}

bool MpDialogHostActive()
{
    return gMpActive && gMpIsHost && gMpDialog.active;
}

bool MpDialogHostIsParticipant()
{
    return gMpActive && gMpIsHost && gMpDialog.active && gMpDialog.hostParticipant;
}

int MpDialogMySelection()
{
    if (!gMpActive) {
        return -1;
    }
    if (gMpIsHost) {
        if (!gMpDialog.active) {
            return -1;
        }
        return gMpDialog.voters[gMpSession.localNetId - 1].selected;
    }
    if (!gMpDialogClient.sessionActive) {
        return -1;
    }
    return gMpDialogClient.selections[gMpSession.localNetId - 1];
}

bool MpDialogAllowWorldTick()
{
    return gMpActive && gMpIsHost && gMpDialog.active;
}

bool MpDialogDirectorMode()
{
    if (!gMpActive || !gMpIsHost) {
        return false;
    }
    if (gMpDialog.active) {
        return !gMpDialog.hostParticipant;
    }
    // About to start: a pending initiator that is not the host means a client
    // started this dialogue — the host will be a director.
    return gMpDialog.pendingInitiator != 0
        && gMpDialog.pendingInitiator != gMpSession.localNetId;
}

uint32_t MpDialogHostNodeSeq()
{
    return gMpDialog.nodeSeq;
}

uint32_t MpDialogHostSessionId()
{
    return gMpDialog.sessionId;
}

// The host joined an active session mid-dialogue: create the vanilla
// dialogue screen and enter the blocking modal. The talk script is parked
// (director return) — the modal just overlays the session and pumps MpTick
// itself via MpDialogHostPump. Returns when the host leaves the vote or the
// session ends; the director flow resumes either way.
static void mpDialogHostEnterModal()
{
    MpLog(MP_LOG_DIALOG, "host join enters modal session=%u node=%u", gMpDialog.sessionId, gMpDialog.nodeSeq);

    // The talk script stays parked at dialog_go while this modal runs; the
    // session teardown must reset the parked program when it ends.
    gMpDialog.joinParkedScript = true;

    if (gameDialogGetWindow() == -1) {
        if (_gdCreateHeadWindow() == -1) {
            MpLogAlways(MP_LOG_DIALOG, "host join modal setup failed (head window)");
            return;
        }
    }
    // The director capture consumed the parked script's option entries:
    // inject the session node so the modal's first frame shows the options,
    // and re-render the head portrait in the camera window. The script
    // procedures ride along so resolved choices still run their reply procs.
    const char* optionTexts[NET_DIALOG_MAX_OPTIONS];
    int reactions[NET_DIALOG_MAX_OPTIONS];
    int procs[NET_DIALOG_MAX_OPTIONS];
    mpDialogHostBuildNodeTexts(optionTexts, reactions, procs);
    gameDialogCoopHostJoinShowNode(gMpDialog.replyText, reactions, optionTexts, gMpDialog.optionCount, procs);
    if (gMpDialog.speaker != nullptr) {
        _tile_scroll_to(gMpDialog.speaker->tile, 2);
    }

    gameDialogProcessUI();

    // The modal ended: either the session ended (the vanilla teardown already
    // destroyed everything) or the host left the vote (ESC/0) — back to the
    // director: destroy the vanilla windows so the host plays the world
    // again. The session itself continues with the remaining participants.
    if (gameDialogGetWindow() != -1) {
        _gdDestroyHeadWindow();
    }
    MpLog(MP_LOG_DIALOG, "host join modal closed session=%u active=%d", gMpDialog.sessionId, gMpDialog.active ? 1 : 0);
}

void MpDialogHostDirectorTick()
{
    if (!gMpActive || !gMpIsHost || !gMpDialog.active) {
        return;
    }

    // The host joined an active session: enter the blocking vanilla modal
    // right now (the script is parked; the modal drives itself).
    if (gMpDialog.hostJoinNeedsModal) {
        gMpDialog.hostJoinNeedsModal = false;
        mpDialogHostEnterModal();
        // The modal may have ended the session (terminal option, all-left,
        // combat/abort): broadcast the END now — the participant check below
        // must not swallow the pending close, or the clients hang forever on
        // the dead node.
        if (gMpDialog.exitRequested) {
            MpDialogHostEnd();
        }
        return;
    }

    if (gMpDialog.hostParticipant) {
        return;
    }

    // NOTE: no proximity auto-join here. This conversation only starts when
    // someone talks to the NPC (the scripted forced dialogue is talk-triggered
    // via the NPC's talk procedure), so joining must be by click as well —
    // symmetric with the client. The join fires from actionTalk and from the
    // client NET_PLAYER_ACTION_TALK routing only.

    // Interruptions first (mirrors the modal pump; MpCombatTick runs right
    // after this in MpTick and may start combat).
    if (MpCombatIsActive() || gMpCombat.pendingStart) {
        mpDialogHostAbort(NET_DIALOG_END_COMBAT);
        return;
    }
    if (gMpDialog.speaker != nullptr && critterIsDead(gMpDialog.speaker)) {
        mpDialogHostAbort(NET_DIALOG_END_NPC_DEAD);
        return;
    }
    if (gVoteSession.state == VOTE_STATE_PASSED) {
        mpDialogHostAbort(NET_DIALOG_END_MAP_CHANGE);
        return;
    }

    // Majority timer expiry.
    if (gMpDialog.timerActive && gMpDialog.majorityOption >= 0) {
        if (getTicksSince(gMpDialog.timerStart) >= gMpDialog.timerMs) {
            MpLogAlways(MP_LOG_DIALOG, "timer expired session=%u node=%u option=%d",
                gMpDialog.sessionId, gMpDialog.nodeSeq, gMpDialog.majorityOption);
            mpDialogResolve(gMpDialog.majorityOption);
        }
    }

    // Pending resolution -> run the director choice path (no window work;
    // the vanilla dialogue state is parked).
    if (gMpDialog.resolvedOption != -1) {
        int optionIndex = gMpDialog.resolvedOption;
        gMpDialog.resolvedOption = -1;
        if (optionIndex >= 0 && optionIndex < gMpDialog.optionCount) {
            if (MpDialogDirectorProcessChoice(optionIndex) == -1) {
                MpDialogHostEnd();
            }
        } else {
            MpDialogHostEnd();
        }
    }

    if (!gMpDialog.active) {
        return;
    }

    // A leave/abort already asked for the session to close (e.g. all
    // participants left): end it through the normal path so the parked
    // vanilla dialogue state gets its teardown.
    if (gMpDialog.exitRequested) {
        MpDialogHostEnd();
    }
}

static void mpDialogHostSetParticipantUi()
{
    bool wantHidden = gMpDialog.active && !gMpDialog.hostParticipant;
    if (wantHidden == gMpDialog.hostUiHidden) {
        return;
    }

    int windows[4];
    windows[0] = gameDialogGetWindow();
    windows[1] = gameDialogGetBackgroundWindow();
    windows[2] = gameDialogGetReplyWindow();
    windows[3] = gameDialogGetOptionsWindow();

    for (int i = 0; i < 4; i++) {
        if (windows[i] == -1) {
            continue;
        }
        if (wantHidden) {
            windowHide(windows[i]);
        } else {
            windowShow(windows[i]);
            windowRefresh(windows[i]);
        }
    }
    gMpDialog.hostUiHidden = wantHidden;
    MpLog(MP_LOG_DIALOG, "host participant UI hidden=%d", wantHidden ? 1 : 0);
}

static void mpDialogHostSendStateToPeer(uint8_t netId)
{
    if (!mpDialogCanSendTo(netId)) {
        return;
    }

    // Serialize the node body (version 3 adds the lip-sync audio name so the
    // client can play the voice and phonemes; version 2 carried head data).
    std::string body;
    body.push_back((char)3); // version
    int32_t headFid = gGameDialogHeadFid;
    int8_t reaction = (int8_t)gGameDialogReactionOrFidget;
    body.append(reinterpret_cast<const char*>(&headFid), sizeof(headFid));
    body.push_back((char)reaction);
    uint16_t replyLen = (uint16_t)strlen(gMpDialog.replyText);
    body.append(reinterpret_cast<const char*>(&replyLen), sizeof(replyLen));
    body.append(gMpDialog.replyText, replyLen);
    body.push_back((char)gMpDialog.optionCount);
    for (int i = 0; i < gMpDialog.optionCount; i++) {
        int16_t msgListId = gMpDialog.options[i].msgListId;
        int16_t msgId = gMpDialog.options[i].msgId;
        int8_t reaction = gMpDialog.options[i].reaction;
        uint16_t textLen = (uint16_t)strlen(gMpDialog.options[i].text);
        body.append(reinterpret_cast<const char*>(&msgListId), sizeof(msgListId));
        body.append(reinterpret_cast<const char*>(&msgId), sizeof(msgId));
        body.push_back((char)reaction);
        body.append(reinterpret_cast<const char*>(&textLen), sizeof(textLen));
        body.append(gMpDialog.options[i].text, textLen);
    }

    // Lip-sync audio base name (empty when the host isn't speaking). Read on
    // the host while the lips system is live; gLipsData.file_name holds the
    // base name lipsLoad normalized from the vanilla audio id.
    const char* audioName = gameDialogGetLipFileName();
    uint8_t audioLen = 0;
    if (audioName != nullptr && audioName[0] != '\0') {
        size_t n = strlen(audioName);
        audioLen = (uint8_t)(n < 63 ? n : 63);
    }
    body.push_back((char)audioLen);
    if (audioLen > 0) {
        body.append(audioName, audioLen);
    }

    uint16_t chunkCount = (uint16_t)((body.size() + NET_DIALOG_STATE_CHUNK_MAX - 1) / NET_DIALOG_STATE_CHUNK_MAX);
    if (chunkCount == 0) {
        chunkCount = 1;
    }

    char buffer[NET_MAX_PACKET_SIZE];
    for (uint16_t chunk = 0; chunk < chunkCount; chunk++) {
        NetDialogStateChunkHeader* header = reinterpret_cast<NetDialogStateChunkHeader*>(buffer);
        header->sessionId = gMpDialog.sessionId;
        header->nodeSeq = gMpDialog.nodeSeq;
        header->chunkIndex = chunk;
        header->chunkCount = chunkCount;
        size_t offset = (size_t)chunk * NET_DIALOG_STATE_CHUNK_MAX;
        size_t chunkSize = std::min<size_t>(NET_DIALOG_STATE_CHUNK_MAX, body.size() - offset);
        header->chunkSize = (uint16_t)chunkSize;
        memcpy(buffer + sizeof(NetDialogStateChunkHeader), body.data() + offset, chunkSize);
        NetSendPacket(gMpSession.players[netId - 1].peer, NET_CHANNEL_RELIABLE,
            NET_PKT_DIALOG_STATE, buffer, sizeof(NetDialogStateChunkHeader) + chunkSize);
    }
}

void MpDialogHostNodeReady(const MpDialogNodeData* node)
{
    if (!gMpActive || !gMpIsHost || node == nullptr) {
        return;
    }

    // Copy the node content (file-statics in game_dialog.cc are captured here).
    uint32_t hash = mpDialogNodeHash(node);
    if (gMpDialog.active && hash == gMpDialog.lastNodeHash) {
        MpLog(MP_LOG_DIALOG, "node dedup (same hash) session=%u node=%u",
            gMpDialog.sessionId, gMpDialog.nodeSeq); // same node (re-entrant _gdProcessUpdate); nothing new
        return;
    }

    bool firstNode = !gMpDialog.active;
    if (firstNode) {
        // The memset below must not eat the pending initiator: read it first,
        // then restore it after the session is zeroed.
        uint8_t pendingInitiator = gMpDialog.pendingInitiator;
        memset(&gMpDialog, 0, sizeof(gMpDialog));
        gMpDialog.hostVoteWindow = -1;
        gMpDialog.active = true;
        gMpDialog.sessionId = (uint32_t)((getTicks() ^ (uintptr_t)node) & 0x7FFFFFFF);
        if (gMpDialog.sessionId == 0) {
            gMpDialog.sessionId = 1;
        }
        gMpDialog.speaker = gGameDialogSpeaker;
        gMpDialog.speakerNetId = gMpDialog.speaker != nullptr ? MpGetObjNetId(gMpDialog.speaker) : 0;
        gMpDialog.initiatorNetId = pendingInitiator;
        gMpDialog.pendingInitiator = 0;

        if (gMpDialog.initiatorNetId != 0 && mpDialogPlayerConnected(gMpDialog.initiatorNetId)) {
            gMpDialog.participants[gMpDialog.participantCount++] = gMpDialog.initiatorNetId;
        } else {
            // Scripted conversation: every connected player participates.
            // Join order (netId ascending) picks the first initiator.
            gMpDialog.initiatorNetId = 0;
            for (int i = 0; i < NET_MAX_PLAYERS; i++) {
                if (gMpSession.players[i].isConnected && gMpSession.players[i].obj != nullptr) {
                    gMpDialog.participants[gMpDialog.participantCount++] = (uint8_t)(i + 1);
                }
            }
            if (gMpDialog.participantCount > 0) {
                gMpDialog.initiatorNetId = gMpDialog.participants[0];
            }
        }
        for (int i = 0; i < NET_MAX_PLAYERS; i++) {
            gMpDialog.voters[i].selected = -1;
        }
        gMpDialog.hostParticipant = mpDialogIsParticipant(gMpSession.localNetId);
        gMpDialog.hostUiHidden = false;

        MpLog(MP_LOG_DIALOG, "begin session=%u speaker=%u initiator=%u participants=%d",
            gMpDialog.sessionId, gMpDialog.speakerNetId, gMpDialog.initiatorNetId, gMpDialog.participantCount);

        // Broadcast BEGIN to every client (non-participants ignore it).
        char buffer[128];
        NetDialogBeginPayload* begin = reinterpret_cast<NetDialogBeginPayload*>(buffer);
        begin->sessionId = gMpDialog.sessionId;
        begin->speakerObjNetId = gMpDialog.speakerNetId;
        begin->initiatorNetId = gMpDialog.initiatorNetId;
        begin->participantCount = (uint8_t)gMpDialog.participantCount;
        for (int i = 0; i < gMpDialog.participantCount; i++) {
            begin->participants[i] = gMpDialog.participants[i];
        }
        NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE, NET_PKT_DIALOG_BEGIN,
            buffer, sizeof(NetDialogBeginPayload) + gMpDialog.participantCount);
    }

    gMpDialog.nodeSeq++;
    gMpDialog.lastNodeHash = hash;
    strncpy(gMpDialog.replyText, node->replyText, NET_DIALOG_REPLY_MAX);
    gMpDialog.replyText[NET_DIALOG_REPLY_MAX] = '\0';
    gMpDialog.optionCount = std::min(node->optionCount, NET_DIALOG_MAX_OPTIONS);
    for (int i = 0; i < gMpDialog.optionCount; i++) {
        MpDialogOption* out = &gMpDialog.options[i];
        out->msgListId = (int16_t)node->optionMsgListIds[i];
        out->msgId = (int16_t)node->optionMsgIds[i];
        out->reaction = (int8_t)node->optionReactions[i];
        out->proc = node->optionProcs != nullptr ? (int16_t)node->optionProcs[i] : 0;
        // Strip the vanilla "1. "/bullet prefix; every client re-numbers.
        mpDialogStripOptionPrefix(node->optionTexts[i], out->text, sizeof(out->text));
    }

    // New node: fresh vote.
    for (int i = 0; i < gMpDialog.participantCount; i++) {
        gMpDialog.voters[gMpDialog.participants[i] - 1].selected = -1;
    }
    gMpDialog.timerActive = false;
    gMpDialog.majorityOption = -1;
    gMpDialog.resolvedOption = -1;

    // NPC line goes to the combat log of every connected player.
    mpDialogEmitNpcLine();

    // Float the NPC line over the speaker (host display + client relay).
    if (gMpDialog.speaker != nullptr && gMpDialog.replyText[0] != '\0') {
        mpDialogFloat(gMpDialog.speaker, gMpDialog.replyText, 101, COLOR_LIGHT_YELLOW, COLOR_BLACK);
    }

    for (int i = 0; i < gMpDialog.participantCount; i++) {
        if (!mpDialogCanSendTo(gMpDialog.participants[i])) {
            continue;
        }
        mpDialogHostSendStateToPeer(gMpDialog.participants[i]);
    }
    mpDialogBroadcastVote();
    mpDialogHostSetParticipantUi();

    MpLog(MP_LOG_DIALOG, "node session=%u node=%u options=%d",
        gMpDialog.sessionId, gMpDialog.nodeSeq, gMpDialog.optionCount);
}

bool MpDialogHostTryJoin(Object* speaker, uint8_t netId)
{
    if (!gMpActive || !gMpIsHost || !gMpDialog.active) {
        return false;
    }

    if (speaker == gMpDialog.speaker) {
        if (mpDialogIsParticipant(netId)) {
            // Already in this dialogue; clicking again does nothing.
            MpLogAlways(MP_LOG_DIALOG, "join ignored session=%u netId=%u (already participant)", gMpDialog.sessionId, netId);
            return true;
        }
        if (!mpDialogPlayerConnected(netId)) {
            return true;
        }
        gMpDialog.participants[gMpDialog.participantCount++] = netId;
        gMpDialog.voters[netId - 1].selected = -1;

        MpLog(MP_LOG_DIALOG, "join session=%u netId=%u participants=%d", gMpDialog.sessionId, netId, gMpDialog.participantCount);

        // Tell the other participants AND the joiner (the joiner's client may
        // never have seen BEGIN — the JOIN itself opens the session there).
        NetDialogJoinPayload join;
        join.sessionId = gMpDialog.sessionId;
        join.netId = netId;
        for (int i = 0; i < gMpDialog.participantCount; i++) {
            uint8_t other = gMpDialog.participants[i];
            if (other == netId) {
                continue;
            }
            if (!mpDialogCanSendTo(other)) {
                continue;
            }
            NetSendPacket(gMpSession.players[other - 1].peer, NET_CHANNEL_RELIABLE, NET_PKT_DIALOG_JOIN, &join, sizeof(join));
        }
        if (mpDialogCanSendTo(netId)) {
            NetSendPacket(gMpSession.players[netId - 1].peer, NET_CHANNEL_RELIABLE, NET_PKT_DIALOG_JOIN, &join, sizeof(join));
        }

        // Replay the transcript, then the current node.
        for (uint32_t seq = 1; seq <= gMpDialog.transcriptEmitted; seq++) {
            int slot = (seq - 1) % NET_DIALOG_TRANSCRIPT_REPLAY;
            NetDialogTranscriptPayload t;
            memset(&t, 0, sizeof(t));
            t.sessionId = gMpDialog.sessionId;
            t.seq = (uint16_t)seq;
            t.speakerNetId = 0;
            strncpy(t.text, gMpDialog.transcript[slot], NET_DIALOG_TRANSCRIPT_MAX);
            if (mpDialogCanSendTo(netId)) {
                NetSendPacket(gMpSession.players[netId - 1].peer, NET_CHANNEL_RELIABLE, NET_PKT_DIALOG_TRANSCRIPT, &t, sizeof(t));
            }
        }
        mpDialogHostSendStateToPeer(netId);

        if (netId == gMpSession.localNetId) {
            gMpDialog.hostParticipant = true;
            gMpDialog.hostJoinNeedsModal = true;
            mpDialogHostSetParticipantUi();
        }
        mpDialogRecalcVote();
        return true;
    }

    // A different NPC while a session is running.
    if (mpDialogIsParticipant(netId)) {
        MpLogAlways(MP_LOG_DIALOG, "talk blocked session=%u netId=%u (one dialogue per player)", gMpDialog.sessionId, netId);
        return true;
    }
    return false;
}

void MpDialogHostHandleChoice(const void* data, size_t dataLength, void* peer)
{
    if (!gMpActive || !gMpIsHost || !gMpDialog.active) {
        return;
    }
    if (dataLength < sizeof(NetDialogChoicePayload)) {
        MpLogAlways(MP_LOG_DIALOG, "choice rejected (short payload) len=%u", (unsigned)dataLength);
        return;
    }
    const NetDialogChoicePayload* p = static_cast<const NetDialogChoicePayload*>(data);
    if (p->sessionId != gMpDialog.sessionId || p->nodeSeq != gMpDialog.nodeSeq) {
        MpLogAlways(MP_LOG_DIALOG, "choice rejected (stale) session=%u/%u node=%u/%u", p->sessionId, gMpDialog.sessionId, p->nodeSeq, gMpDialog.nodeSeq);
        return;
    }
    if (!mpDialogIsParticipant(p->netId)) {
        MpLogAlways(MP_LOG_DIALOG, "choice rejected (not participant) netId=%u", p->netId);
        return;
    }
    MultiplayerPlayer* player = &gMpSession.players[p->netId - 1];
    if (player->peer != peer) {
        MpLogAlways(MP_LOG_DIALOG, "choice rejected (peer mismatch) netId=%u", p->netId);
        return;
    }
    if (gMpDialog.voters[p->netId - 1].suspended) {
        MpLogAlways(MP_LOG_DIALOG, "choice rejected (bartering) netId=%u", p->netId);
        return;
    }
    if (gMpDialog.resolvedOption != -1) {
        return; // resolution already in flight
    }

    if (p->optionIndex == 0xFF) {
        gMpDialog.voters[p->netId - 1].selected = -1;
    } else if (p->optionIndex < gMpDialog.optionCount) {
        gMpDialog.voters[p->netId - 1].selected = (int8_t)p->optionIndex;
    } else {
        MpLogAlways(MP_LOG_DIALOG, "choice rejected (out of range) option=%u count=%d", p->optionIndex, gMpDialog.optionCount);
        return;
    }

    MpLog(MP_LOG_DIALOG, "vote session=%u node=%u netId=%u option=%d",
        gMpDialog.sessionId, gMpDialog.nodeSeq, p->netId, p->optionIndex);
    // Float the chosen option over the choosing player's avatar.
    if (p->optionIndex < gMpDialog.optionCount && player->obj != nullptr
        && gMpDialog.options[p->optionIndex].text[0] != '\0') {
        mpDialogFloat(player->obj, gMpDialog.options[p->optionIndex].text, 101, COLOR_WHITE, COLOR_BLACK);
    }
    mpDialogRecalcVote();
}

void MpDialogHostHandleLeave(const void* data, size_t dataLength, void* peer)
{
    if (!gMpActive || !gMpIsHost || !gMpDialog.active) {
        return;
    }
    if (dataLength < sizeof(NetDialogLeavePayload)) {
        return;
    }
    const NetDialogLeavePayload* p = static_cast<const NetDialogLeavePayload*>(data);
    if (p->sessionId != gMpDialog.sessionId || !mpDialogIsParticipant(p->netId)) {
        return;
    }
    MultiplayerPlayer* player = &gMpSession.players[p->netId - 1];
    if (player->peer != peer) {
        MpLogAlways(MP_LOG_DIALOG, "leave rejected (peer mismatch) netId=%u", p->netId);
        return;
    }

    MpLog(MP_LOG_DIALOG, "leave session=%u netId=%u", gMpDialog.sessionId, p->netId);

    if (gMpDialog.barter[p->netId - 1].active) {
        mpDialogHostTeardownBarter(p->netId);
    }

    // Remove from the participant list (keep order).
    for (int i = 0; i < gMpDialog.participantCount; i++) {
        if (gMpDialog.participants[i] == p->netId) {
            for (int j = i; j < gMpDialog.participantCount - 1; j++) {
                gMpDialog.participants[j] = gMpDialog.participants[j + 1];
            }
            gMpDialog.participantCount--;
            break;
        }
    }

    if (gMpDialog.initiatorNetId == p->netId) {
        // Initiator handoff: next in join order.
        gMpDialog.initiatorNetId = gMpDialog.participantCount > 0 ? gMpDialog.participants[0] : 0;
        MpLog(MP_LOG_DIALOG, "initiator handoff session=%u new=%u", gMpDialog.sessionId, gMpDialog.initiatorNetId);
    }

    if (p->netId == gMpSession.localNetId) {
        gMpDialog.hostParticipant = false;
        mpDialogHostSetParticipantUi();
    }

    if (gMpDialog.participantCount == 0) {
        MpLog(MP_LOG_DIALOG, "all left session=%u", gMpDialog.sessionId);
        NetDialogEndPayload end;
        end.sessionId = gMpDialog.sessionId;
        end.reason = NET_DIALOG_END_ALL_LEFT;
        NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE, NET_PKT_DIALOG_END, &end, sizeof(end));
        gMpDialog.exitRequested = true;
        mpDialogHostClearSession();
        return;
    }

    NetDialogLeavePayload leave;
    leave.sessionId = gMpDialog.sessionId;
    leave.netId = p->netId;
    leave.reason = 1;
    for (int i = 0; i < gMpDialog.participantCount; i++) {
        uint8_t other = gMpDialog.participants[i];
        if (!mpDialogCanSendTo(other)) {
            continue;
        }
        NetSendPacket(gMpSession.players[other - 1].peer, NET_CHANNEL_RELIABLE, NET_PKT_DIALOG_LEAVE, &leave, sizeof(leave));
    }
    mpDialogRecalcVote();
}

// The host participant picked an option with its keyboard (vanilla keys are
// intercepted in gameDialogProcessUI).
void MpDialogHostLocalChoice(int optionIndex)
{
    if (!gMpActive || !gMpIsHost || !gMpDialog.active || !gMpDialog.hostParticipant) {
        return;
    }
    uint8_t netId = gMpSession.localNetId;
    if (!mpDialogIsParticipant(netId) || gMpDialog.voters[netId - 1].suspended) {
        return;
    }
    if (gMpDialog.resolvedOption != -1) {
        return;
    }
    if (optionIndex < 0 || optionIndex >= gMpDialog.optionCount) {
        MpLogAlways(MP_LOG_DIALOG, "vote rejected (out of range) option=%d count=%d", optionIndex, gMpDialog.optionCount);
        return;
    }
    gMpDialog.voters[netId - 1].selected = (int8_t)optionIndex;
    MpLog(MP_LOG_DIALOG, "vote session=%u node=%u netId=%u option=%d", gMpDialog.sessionId, gMpDialog.nodeSeq, netId, optionIndex);
    // Float the chosen option over the host participant's avatar.
    if (gDude != nullptr && gMpDialog.options[optionIndex].text[0] != '\0') {
        mpDialogFloat(gDude, gMpDialog.options[optionIndex].text, 101, COLOR_WHITE, COLOR_BLACK);
    }
    mpDialogRecalcVote();
}

// The host participant left via 0/ESC (vanilla keys are intercepted in
// gameDialogProcessUI).
void MpDialogHostLocalLeave()
{
    if (!gMpActive || !gMpIsHost || !gMpDialog.active) {
        return;
    }
    uint8_t netId = gMpSession.localNetId;
    if (!mpDialogIsParticipant(netId)) {
        return;
    }

    MpLog(MP_LOG_DIALOG, "leave (local) session=%u netId=%u", gMpDialog.sessionId, netId);

    if (gMpDialog.barter[netId - 1].active) {
        mpDialogHostTeardownBarter(netId);
    }

    for (int i = 0; i < gMpDialog.participantCount; i++) {
        if (gMpDialog.participants[i] == netId) {
            for (int j = i; j < gMpDialog.participantCount - 1; j++) {
                gMpDialog.participants[j] = gMpDialog.participants[j + 1];
            }
            gMpDialog.participantCount--;
            break;
        }
    }

    if (gMpDialog.initiatorNetId == netId) {
        gMpDialog.initiatorNetId = gMpDialog.participantCount > 0 ? gMpDialog.participants[0] : 0;
        MpLog(MP_LOG_DIALOG, "initiator handoff session=%u new=%u", gMpDialog.sessionId, gMpDialog.initiatorNetId);
    }

    gMpDialog.hostParticipant = false;
    mpDialogHostSetParticipantUi();

    if (gMpDialog.participantCount == 0) {
        MpLog(MP_LOG_DIALOG, "all left session=%u", gMpDialog.sessionId);
        NetDialogEndPayload end;
        end.sessionId = gMpDialog.sessionId;
        end.reason = NET_DIALOG_END_ALL_LEFT;
        NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE, NET_PKT_DIALOG_END, &end, sizeof(end));
        gMpDialog.exitRequested = true;
        mpDialogHostClearSession();
        return;
    }

    NetDialogLeavePayload leave;
    leave.sessionId = gMpDialog.sessionId;
    leave.netId = netId;
    leave.reason = 1;
    for (int i = 0; i < gMpDialog.participantCount; i++) {
        uint8_t other = gMpDialog.participants[i];
        if (!mpDialogCanSendTo(other)) {
            continue;
        }
        NetSendPacket(gMpSession.players[other - 1].peer, NET_CHANNEL_RELIABLE, NET_PKT_DIALOG_LEAVE, &leave, sizeof(leave));
    }
    mpDialogRecalcVote();
}

void MpDialogHostPlayerDisconnected(uint8_t netId)
{
    if (!gMpActive || !gMpIsHost || !gMpDialog.active) {
        return;
    }
    if (!mpDialogIsParticipant(netId)) {
        return;
    }

    MpLog(MP_LOG_DIALOG, "disconnect session=%u netId=%u", gMpDialog.sessionId, netId);

    if (gMpDialog.barter[netId - 1].active) {
        mpDialogHostTeardownBarter(netId);
    }

    for (int i = 0; i < gMpDialog.participantCount; i++) {
        if (gMpDialog.participants[i] == netId) {
            for (int j = i; j < gMpDialog.participantCount - 1; j++) {
                gMpDialog.participants[j] = gMpDialog.participants[j + 1];
            }
            gMpDialog.participantCount--;
            break;
        }
    }

    if (gMpDialog.initiatorNetId == netId) {
        gMpDialog.initiatorNetId = gMpDialog.participantCount > 0 ? gMpDialog.participants[0] : 0;
        MpLog(MP_LOG_DIALOG, "initiator handoff (disconnect) session=%u new=%u", gMpDialog.sessionId, gMpDialog.initiatorNetId);
    }

    if (netId == gMpSession.localNetId) {
        gMpDialog.hostParticipant = false;
        mpDialogHostSetParticipantUi();
    }

    if (gMpDialog.participantCount == 0) {
        NetDialogEndPayload end;
        end.sessionId = gMpDialog.sessionId;
        end.reason = NET_DIALOG_END_ALL_LEFT;
        NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE, NET_PKT_DIALOG_END, &end, sizeof(end));
        gMpDialog.exitRequested = true;
        mpDialogHostClearSession();
        return;
    }

    NetDialogLeavePayload leave;
    leave.sessionId = gMpDialog.sessionId;
    leave.netId = netId;
    leave.reason = 2;
    for (int i = 0; i < gMpDialog.participantCount; i++) {
        uint8_t other = gMpDialog.participants[i];
        if (!mpDialogCanSendTo(other)) {
            continue;
        }
        NetSendPacket(gMpSession.players[other - 1].peer, NET_CHANNEL_RELIABLE, NET_PKT_DIALOG_LEAVE, &leave, sizeof(leave));
    }
    mpDialogRecalcVote();
}

// ---------------------------------------------------------------------------
// Host: barter
// ---------------------------------------------------------------------------

static bool mpDialogNpcCanBarter()
{
    if (gMpDialog.speaker == nullptr) {
        return false;
    }
    Proto* proto;
    if (protoGetProto(gMpDialog.speaker->pid, &proto) == -1) {
        return false;
    }
    return (proto->critter.data.flags & CRITTER_BARTER) != 0;
}

static Object* mpDialogContainerCreate()
{
    Object* obj = nullptr;
    objectCreateWithFidPid(&obj, -1, PROTO_ID_JESSE_CONTAINER);
    if (obj != nullptr) {
        // Hidden + no-save like the vanilla barter tables (game_dialog.cc
        // L3472-3480): keeps them out of the object-state broadcast and the
        // world renderer entirely.
        obj->flags |= (OBJECT_HIDDEN | OBJECT_NO_SAVE);
    }
    return obj;
}

static bool mpDialogItemIsEquipped(Object* item)
{
    if (item == nullptr) {
        return false;
    }
    return (item->flags & (OBJECT_WORN | OBJECT_IN_RIGHT_HAND | OBJECT_IN_LEFT_HAND)) != 0;
}

static Object* mpDialogFindItemByPid(Object* container, uint32_t pid)
{
    if (container == nullptr) {
        return nullptr;
    }
    Inventory* inventory = &container->data.inventory;
    for (int i = 0; i < inventory->length; i++) {
        if (inventory->items[i].item->pid == (int)pid) {
            return inventory->items[i].item;
        }
    }
    return nullptr;
}

static int mpDialogCountNpcItems()
{
    if (gMpDialog.speaker == nullptr) {
        return 0;
    }
    int count = 0;
    Inventory* inventory = &gMpDialog.speaker->data.inventory;
    for (int i = 0; i < inventory->length; i++) {
        if (!mpDialogItemIsEquipped(inventory->items[i].item)) {
            count++;
        }
    }
    return std::min(count, NET_BARTER_MAX_ITEMS);
}

static int mpDialogCountContainerItems(Object* container)
{
    if (container == nullptr) {
        return 0;
    }
    return std::min(container->data.inventory.length, NET_BARTER_MAX_ITEMS);
}

// Host-computed barter modifier (dialogue modifier + NPC reaction modifier).
// Defined before the state builder, which ships it to the barterers.
static int mpDialogHostBarterModifier()
{
    int mod = gameDialogGetBarterModifier();
    if (gMpDialog.speaker != nullptr) {
        int translated = reactionTranslateValue(reactionGetValue(gMpDialog.speaker));
        if (translated == NPC_REACTION_BAD) {
            mod += 25;
        } else if (translated == NPC_REACTION_GOOD) {
            mod -= 15;
        }
    }
    return mod;
}

// Fills the item entry list of a container (equipped items excluded for NPCs).
static int mpDialogCollectItems(Object* container, bool excludeEquipped, NetBarterItem* out, int maxOut)
{
    if (container == nullptr) {
        return 0;
    }
    int count = 0;
    Inventory* inventory = &container->data.inventory;
    for (int i = 0; i < inventory->length && count < maxOut; i++) {
        Object* item = inventory->items[i].item;
        if (excludeEquipped && mpDialogItemIsEquipped(item)) {
            continue;
        }
        out[count].pid = (uint32_t)item->pid;
        out[count].qty = inventory->items[i].quantity;
        out[count].unitValue = itemGetCost(item);
        count++;
    }
    return count;
}

// Builds a BARTER_STATE payload for one barterer (into caller buffer).
// The payload must fit one packet: at most MP_BARTER_STATE_ENTRY_BUDGET
// entries total (npc + offer + request).
static size_t mpDialogBuildBarterState(uint8_t netId, uint8_t lastOp, uint8_t lastOk, uint8_t lastMsgId, char* buffer)
{
    constexpr int MP_BARTER_STATE_ENTRY_BUDGET = (NET_MAX_PACKET_SIZE - (int)sizeof(NetBarterStatePayload)) / (int)sizeof(NetBarterItem);

    NetBarterStatePayload* p = reinterpret_cast<NetBarterStatePayload*>(buffer);
    p->sessionId = gMpDialog.sessionId;
    p->netId = netId;
    p->lastOp = lastOp;
    p->lastOk = lastOk;
    p->lastMsgId = lastMsgId;
    p->barterMod = (int16_t)mpDialogHostBarterModifier();

    NetBarterItem* cursor = reinterpret_cast<NetBarterItem*>(buffer + sizeof(NetBarterStatePayload));
    int remaining = MP_BARTER_STATE_ENTRY_BUDGET;

    int npcBudget = std::min(NET_BARTER_MAX_ITEMS, remaining);
    p->npcItemCount = (uint16_t)mpDialogCollectItems(gMpDialog.speaker, true, cursor, npcBudget);
    if (p->npcItemCount > (uint16_t)npcBudget) {
        p->npcItemCount = (uint16_t)npcBudget;
    }
    cursor += p->npcItemCount;
    remaining -= p->npcItemCount;

    MpBarterSession* b = &gMpDialog.barter[netId - 1];
    int offerBudget = std::min(NET_BARTER_MAX_ITEMS, remaining);
    p->offerCount = (uint16_t)mpDialogCollectItems(b->offerTable, false, cursor, offerBudget);
    if (p->offerCount > (uint16_t)offerBudget) {
        p->offerCount = (uint16_t)offerBudget;
    }
    cursor += p->offerCount;
    remaining -= p->offerCount;

    int requestBudget = std::min(NET_BARTER_MAX_ITEMS, remaining);
    p->requestCount = (uint16_t)mpDialogCollectItems(b->requestTable, false, cursor, requestBudget);
    if (p->requestCount > (uint16_t)requestBudget) {
        p->requestCount = (uint16_t)requestBudget;
    }
    cursor += p->requestCount;

    return (size_t)(reinterpret_cast<uint8_t*>(cursor) - reinterpret_cast<uint8_t*>(buffer));
}

static void mpDialogSendBarterState(uint8_t netId, uint8_t lastOp, uint8_t lastOk, uint8_t lastMsgId)
{
    if (!mpDialogCanSendTo(netId)) {
        return; // host barterer reads its real tables directly
    }
    char buffer[NET_MAX_PACKET_SIZE];
    size_t length = mpDialogBuildBarterState(netId, lastOp, lastOk, lastMsgId, buffer);
    NetSendPacket(gMpSession.players[netId - 1].peer, NET_CHANNEL_RELIABLE, NET_PKT_BARTER_STATE, buffer, length);
}

// Broadcasts fresh state to every active barterer (NPC changed).
static void mpDialogBroadcastBarterStateToBarterers()
{
    int count = 0;
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        if (gMpDialog.barter[i].active) {
            mpDialogSendBarterState(i + 1, 0, 0, 0);
            count++;
        }
    }
    MpLog(MP_LOG_DIALOG, "MPBARTER broadcast barterers=%d", count);
}

static bool mpDialogBarterStart(uint8_t netId)
{
    MpBarterSession* b = &gMpDialog.barter[netId - 1];
    if (b->active) {
        mpDialogSendBarterState(netId, NET_BARTER_OP_START, 1, 0);
        return true;
    }
    if (!mpDialogNpcCanBarter()) {
        MpLogAlways(MP_LOG_DIALOG, "MPBARTER start rejected session=%u netId=%u (no barter flag)", gMpDialog.sessionId, netId);
        mpDialogSendBarterState(netId, NET_BARTER_OP_START, 0, 903);
        return false;
    }

    b->active = true;
    b->netId = netId;
    b->offerTable = mpDialogContainerCreate();
    b->requestTable = mpDialogContainerCreate();

    gMpDialog.voters[netId - 1].suspended = true;
    MpLog(MP_LOG_DIALOG, "MPBARTER start session=%u netId=%u", gMpDialog.sessionId, netId);
    mpDialogRecalcVote();
    mpDialogSendBarterState(netId, NET_BARTER_OP_START, 1, 0);
    return true;
}

static void mpDialogBarterEnd(uint8_t netId)
{
    MpBarterSession* b = &gMpDialog.barter[netId - 1];
    if (!b->active) {
        mpDialogSendBarterState(netId, NET_BARTER_OP_END, 1, 0);
        return;
    }
    mpDialogHostTeardownBarter(netId);
    gMpDialog.voters[netId - 1].suspended = false;
    MpLog(MP_LOG_DIALOG, "MPBARTER end session=%u netId=%u", gMpDialog.sessionId, netId);
    mpDialogRecalcVote();
    mpDialogSendBarterState(netId, NET_BARTER_OP_END, 1, 0);
}

static void mpDialogBarterMove(uint8_t netId, uint8_t target, uint32_t pid, int32_t qty)
{
    MpBarterSession* b = &gMpDialog.barter[netId - 1];
    if (!b->active) {
        mpDialogSendBarterState(netId, NET_BARTER_OP_MOVE, 0, 0);
        return;
    }

    Object* avatar = mpDialogPlayerAvatar(netId);
    if (target == 1) {
        // Player <-> offer table.
        if (qty > 0) {
            Object* item = mpDialogFindItemByPid(avatar, pid);
            if (item == nullptr || qty > itemGetQuantity(avatar, item)) {
                MpLogAlways(MP_LOG_DIALOG, "MPBARTER move rejected session=%u netId=%u pid=0x%X qty=%d (avatar lacks item)", gMpDialog.sessionId, netId, pid, qty);
                mpDialogSendBarterState(netId, NET_BARTER_OP_MOVE, 0, 0);
                return;
            }
            itemMoveForce(avatar, b->offerTable, item, qty);
        } else {
            Object* item = mpDialogFindItemByPid(b->offerTable, pid);
            if (item == nullptr || -qty > itemGetQuantity(b->offerTable, item)) {
                mpDialogSendBarterState(netId, NET_BARTER_OP_MOVE, 0, 0);
                return;
            }
            itemMoveForce(b->offerTable, avatar, item, -qty);
        }
    } else {
        // NPC <-> request table.
        if (qty > 0) {
            Object* item = mpDialogFindItemByPid(gMpDialog.speaker, pid);
            if (item == nullptr || mpDialogItemIsEquipped(item) || qty > itemGetQuantity(gMpDialog.speaker, item)) {
                MpLogAlways(MP_LOG_DIALOG, "MPBARTER move rejected session=%u netId=%u pid=0x%X qty=%d (npc lacks item)", gMpDialog.sessionId, netId, pid, qty);
                mpDialogSendBarterState(netId, NET_BARTER_OP_MOVE, 0, 0);
                return;
            }
            itemMoveForce(gMpDialog.speaker, b->requestTable, item, qty);
        } else {
            Object* item = mpDialogFindItemByPid(b->requestTable, pid);
            if (item == nullptr || -qty > itemGetQuantity(b->requestTable, item)) {
                mpDialogSendBarterState(netId, NET_BARTER_OP_MOVE, 0, 0);
                return;
            }
            itemMoveForce(b->requestTable, gMpDialog.speaker, item, -qty);
        }
    }

    MpLog(MP_LOG_DIALOG, "MPBARTER move session=%u netId=%u target=%u pid=0x%X qty=%d", gMpDialog.sessionId, netId, target, pid, qty);
    mpDialogSendBarterState(netId, NET_BARTER_OP_MOVE, 1, 0);
    if (target == 2) {
        mpDialogBroadcastBarterStateToBarterers();
    }
    gMpDialog.barterDirty = true;
}

static void mpDialogBarterCommit(uint8_t netId)
{
    MpBarterSession* b = &gMpDialog.barter[netId - 1];
    if (!b->active) {
        mpDialogSendBarterState(netId, NET_BARTER_OP_COMMIT, 0, 0);
        return;
    }

    Object* avatar = mpDialogPlayerAvatar(netId);
    if (avatar == nullptr || gMpDialog.speaker == nullptr) {
        mpDialogSendBarterState(netId, NET_BARTER_OP_COMMIT, 0, 0);
        return;
    }

    int rc = MpBarterAttemptTransaction(avatar, b->offerTable, gMpDialog.speaker, b->requestTable);
    uint8_t msgId = rc == 0 ? 27 : 28;
    MpLog(MP_LOG_DIALOG, "MPBARTER commit session=%u netId=%u rc=%d", gMpDialog.sessionId, netId, rc);
    mpDialogSendBarterState(netId, NET_BARTER_OP_COMMIT, rc == 0 ? 1 : 0, msgId);
    mpDialogBroadcastBarterStateToBarterers();
    gMpDialog.barterDirty = true;
}

// ---------------------------------------------------------------------------
// Vanilla trade loop hooks (called from inventory.cc barterProcessUI, gated
// on gMpActive). The co-op barter drives the REAL vanilla trade screen: the
// hooks pump the network, intercept M/T/ESC and slot clicks so they route
// through the host-authoritative session, and tear the session down when the
// loop exits.
// ---------------------------------------------------------------------------

static void mpDialogClientSendBarter(uint8_t op, uint8_t target, uint32_t pid, int32_t qty); // fwd

bool MpDialogBarterSessionOpen()
{
    if (!gMpActive) {
        return false;
    }
    if (gMpIsHost) {
        return gMpDialog.active && gMpDialog.barter[gMpSession.localNetId - 1].active;
    }
    return gMpDialogClient.barterActive;
}

bool MpDialogBarterLoopTick()
{
    if (!gMpActive) {
        return true;
    }

    if (gMpIsHost) {
        // Interruptions first: never pump when combat is about to start.
        if (MpCombatIsActive() || gMpCombat.pendingStart) {
            mpDialogHostAbort(NET_DIALOG_END_COMBAT);
            return false;
        }
        if (gMpDialog.speaker != nullptr && critterIsDead(gMpDialog.speaker)) {
            mpDialogHostAbort(NET_DIALOG_END_NPC_DEAD);
            return false;
        }
    }

    MpTick();

    if (gMpIsHost) {
        if (!gMpDialog.active || !gMpDialog.barter[gMpSession.localNetId - 1].active) {
            return false;
        }
        if (gMpDialog.barterDirty) {
            gMpDialog.barterDirty = false;
            MpBarterSession* b = &gMpDialog.barter[gMpSession.localNetId - 1];
            // Draw the session's own tables explicitly: another player's
            // commit can leave the canonical table globals pointing at their
            // (now empty) tables.
            mpBarterTradeRefreshWithTables(b->offerTable, b->requestTable);
        }
    } else {
        if (!gMpDialogClient.barterActive) {
            return false;
        }
        if (gMpDialogClient.barterDirty) {
            gMpDialogClient.barterDirty = false;
            MpLog(MP_LOG_DIALOG, "MPBARTER client tick refresh npc=%d offer=%d req=%d",
                gMpDialogClient.npcItemCount, gMpDialogClient.offerCount, gMpDialogClient.requestCount);
            mpBarterTradeRefreshWithTables(gMpDialogClient.offerMirror, gMpDialogClient.requestMirror);
        }
    }
    return true;
}

bool MpDialogBarterInterceptKey(int keyCode)
{
    if (!MpDialogBarterSessionOpen()) {
        return false;
    }
    if (keyCode == KEY_LOWERCASE_M) {
        if (gMpIsHost) {
            MpBarterSession* b = &gMpDialog.barter[gMpSession.localNetId - 1];
            // Vanilla parity: M on empty tables does nothing.
            if (mpDialogCountContainerItems(b->offerTable) > 0 || mpDialogCountContainerItems(b->requestTable) > 0) {
                mpDialogBarterCommit(gMpSession.localNetId);
            }
        } else {
            mpDialogClientSendBarter(NET_BARTER_OP_COMMIT, 0, 0, 0);
        }
        return true;
    }
    if (keyCode == KEY_LOWERCASE_T || keyCode == KEY_ESCAPE) {
        if (gMpIsHost) {
            mpDialogBarterEnd(gMpSession.localNetId);
        } else {
            mpDialogClientSendBarter(NET_BARTER_OP_END, 0, 0, 0);
            gMpDialogClient.barterActive = false;
        }
        return true;
    }
    return false;
}

bool MpDialogBarterMoveFromVanilla(uint32_t pid, int quantity, int target, bool back)
{
    if (quantity <= 0) {
        return true; // nothing to move — swallow
    }
    if (!MpDialogBarterSessionOpen()) {
        return false; // no active mp session — let the vanilla move run
    }
    if (gMpIsHost) {
        mpDialogBarterMove(gMpSession.localNetId, (uint8_t)target, pid, back ? -quantity : quantity);
    } else {
        mpDialogClientSendBarter(NET_BARTER_OP_MOVE, (uint8_t)target, pid, back ? -quantity : quantity);
    }
    return true;
}

void MpDialogBarterLoopEnded()
{
    if (!gMpActive) {
        return;
    }
    if (gMpIsHost) {
        // Idempotent: no-op when the session already ended via T/ESC/abort.
        mpDialogBarterEnd(gMpSession.localNetId);
    }
    // Client: the session end is host-driven (BARTER_STATE END clears
    // barterActive); nothing to do here.
}

void MpDialogHostHandleBarterCmd(const void* data, size_t dataLength, void* peer)
{
    if (!gMpActive || !gMpIsHost || !gMpDialog.active) {
        return;
    }
    if (dataLength < sizeof(NetBarterCmdPayload)) {
        MpLogAlways(MP_LOG_DIALOG, "MPBARTER cmd rejected (short payload) len=%u", (unsigned)dataLength);
        return;
    }
    const NetBarterCmdPayload* p = static_cast<const NetBarterCmdPayload*>(data);
    if (p->sessionId != gMpDialog.sessionId || !mpDialogIsParticipant(p->netId)) {
        return;
    }
    MultiplayerPlayer* player = &gMpSession.players[p->netId - 1];
    if (player->peer != peer) {
        MpLogAlways(MP_LOG_DIALOG, "MPBARTER cmd rejected (peer mismatch) netId=%u", p->netId);
        return;
    }

    switch (p->op) {
    case NET_BARTER_OP_START:
        mpDialogBarterStart(p->netId);
        break;
    case NET_BARTER_OP_MOVE:
        mpDialogBarterMove(p->netId, p->target, p->pid, p->qty);
        break;
    case NET_BARTER_OP_END:
        mpDialogBarterEnd(p->netId);
        break;
    case NET_BARTER_OP_COMMIT:
        mpDialogBarterCommit(p->netId);
        break;
    default:
        MpLogAlways(MP_LOG_DIALOG, "MPBARTER cmd rejected (unknown op) op=%u", p->op);
        break;
    }
}

void MpDialogHostRequestBarter()
{
    if (!gMpDialog.active || !gMpDialog.hostParticipant) {
        return;
    }
    if (!mpDialogNpcCanBarter()) {
        // Mirror the vanilla "will not barter" message.
        MessageListItem messageListItem;
        messageListItem.num = gGameDialogSpeakerIsPartyMember ? 913 : 903;
        if (messageListGetItem(&gProtoMessageList, &messageListItem)) {
            gameDialogRenderSupplementaryMessage(messageListItem.text);
        }
        return;
    }
    if (gMpDialog.barter[gMpSession.localNetId - 1].active) {
        return;
    }
    mpDialogBarterStart(gMpSession.localNetId);
    gMpDialog.hostBarterUiOpen = true;
}

// ---------------------------------------------------------------------------
// Host: vote overlay
// ---------------------------------------------------------------------------

static void mpDialogHostDrawOverlay()
{
    if (!gMpDialog.active || !gMpDialog.hostParticipant) {
        mpDialogHostCloseVoteOverlay();
        return;
    }

    constexpr int W = 240;
    constexpr int H = 150;
    if (gMpDialog.hostVoteWindow == -1) {
        int x = screenGetWidth() - W - 8;
        int y = 8;
        int win = windowCreate(x, y, W, H, COLOR_BLACK, WINDOW_MOVE_ON_TOP);
        if (win == -1) {
            return;
        }
        windowDrawBorder(win);
        gMpDialog.hostVoteWindow = win;
    }
    int win = gMpDialog.hostVoteWindow;

    windowFill(win, 1, 1, W - 2, H - 2, COLOR_BLACK);

    int y = 4;
    const char* title = "DIALOGUE";
    windowDrawText(win, title, 0, (W - fontGetStringWidth(title)) / 2, y, COLOR_WHITE);
    y += 14;

    char line[128];
    for (int i = 0; i < gMpDialog.participantCount && i < 6; i++) {
        uint8_t netId = gMpDialog.participants[i];
        const char* name = mpDialogParticipantName(netId);
        if (gMpDialog.voters[netId - 1].suspended) {
            snprintf(line, sizeof(line), "%s (barter)", name);
        } else {
            snprintf(line, sizeof(line), "%s", name);
        }
        if (netId == gMpDialog.initiatorNetId) {
            strncat(line, " *", sizeof(line) - strlen(line) - 1);
        }
        windowDrawText(win, line, 0, 8, y, COLOR_WHITE);
        y += 13;
    }

    if (gMpDialog.timerActive) {
        uint32_t remaining = gMpDialog.timerStart + gMpDialog.timerMs - getTicks();
        if (remaining > gMpDialog.timerMs) {
            remaining = 0;
        }
        snprintf(line, sizeof(line), "Majority in %d s", (int)(remaining / 1000) + 1);
        windowDrawText(win, line, 0, 8, y, COLOR_WHITE);
        y += 13;
    }
    windowDrawText(win, "[1-9] choose  [0] leave", 0, 8, H - 16, COLOR_WHITE);

    windowRefresh(win);
}

// ---------------------------------------------------------------------------
// Host: barter modal (vanilla trade screen)
// ---------------------------------------------------------------------------

// Blocking host barter modal (nested inside the dialogue pump). Drives the
// REAL vanilla trade loop (barterProcessUI) with the authoritative tables;
// the co-op hooks inside it pump the network, route M/T/ESC and slot clicks
// through the session, and tear the session down on exit.
static void mpDialogHostRunBarter()
{
    uint8_t local = gMpSession.localNetId;
    MpBarterSession* b = &gMpDialog.barter[local - 1];
    if (!b->active || gMpDialog.speaker == nullptr || b->offerTable == nullptr || b->requestTable == nullptr) {
        gMpDialog.hostBarterUiOpen = false;
        return;
    }

    ScopedGameMode gm(GameMode::kBarter);
    gameDialogCoopHideDialogue();

    // Vanilla parity: swap the dialogue window for the dedicated barter
    // window (barter.frm / trade.frm) — the trade panes are positioned for it.
    gameDialogCoopDestroyDialogueWindow();
    if (gameDialogCoopCreateBarterWindow() == -1) {
        gameDialogCoopRecreateDialogueWindow();
        gameDialogCoopShowDialogue();
        gMpDialog.hostBarterUiOpen = false;
        return;
    }

    // The vote overlay must not sit on top of the trade screen.
    if (gMpDialog.hostVoteWindow != -1) {
        windowHide(gMpDialog.hostVoteWindow);
    }

    MpLog(MP_LOG_DIALOG, "MPBARTER trade screen open (host) session=%u", gMpDialog.sessionId);

    // Base modifier only: the vanilla trade loop adds the speaker's reaction
    // modifier itself (vanilla parity). The full modifier (base + reaction)
    // is what the client receives in the state packet.
    barterProcessUI(gameDialogGetWindow(), gMpDialog.speaker, b->offerTable, b->requestTable,
        gameDialogGetBarterModifier());

    if (gMpDialog.hostVoteWindow != -1) {
        windowShow(gMpDialog.hostVoteWindow);
    }
    gameDialogCoopDestroyBarterWindow();
    gameDialogCoopRecreateDialogueWindow();
    gameDialogCoopShowDialogue();
    gMpDialog.hostBarterUiOpen = false;
    MpLog(MP_LOG_DIALOG, "MPBARTER trade screen closed (host) session=%u", gMpDialog.sessionId);
}

// ---------------------------------------------------------------------------
// Host: pump
// ---------------------------------------------------------------------------

extern int gameDialogChooseOption(int optionIndex); // game_dialog.cc

void MpDialogHostPump()
{
    if (!gMpActive || !gMpIsHost || !gMpDialog.active) {
        return;
    }

    // Interruptions first: never call MpTick when combat is about to start.
    if (MpCombatIsActive() || gMpCombat.pendingStart) {
        mpDialogHostAbort(NET_DIALOG_END_COMBAT);
        return;
    }
    if (gMpDialog.speaker != nullptr && critterIsDead(gMpDialog.speaker)) {
        mpDialogHostAbort(NET_DIALOG_END_NPC_DEAD);
        return;
    }
    if (gVoteSession.state == VOTE_STATE_PASSED) {
        mpDialogHostAbort(NET_DIALOG_END_MAP_CHANGE);
        return;
    }

    MpTick();

    if (!gMpDialog.active) {
        return;
    }

    // Host opened barter: run the nested modal.
    if (gMpDialog.hostBarterUiOpen) {
        mpDialogHostRunBarter();
        if (!gMpDialog.active) {
            return;
        }
    }

    // Majority timer expiry.
    if (gMpDialog.timerActive && gMpDialog.majorityOption >= 0) {
        if (getTicksSince(gMpDialog.timerStart) >= gMpDialog.timerMs) {
            MpLogAlways(MP_LOG_DIALOG, "timer expired session=%u node=%u option=%d", gMpDialog.sessionId, gMpDialog.nodeSeq, gMpDialog.majorityOption);
            mpDialogResolve(gMpDialog.majorityOption);
        }
    }

    // Pending resolution -> run the vanilla choice path.
    if (gMpDialog.resolvedOption != -1) {
        int optionIndex = gMpDialog.resolvedOption;
        gMpDialog.resolvedOption = -1;
        if (optionIndex >= 0 && optionIndex < gMpDialog.optionCount) {
            int rc = gameDialogChooseOption(optionIndex);
            MpLog(MP_LOG_DIALOG, "pump resolve session=%u option=%d rc=%d", gMpDialog.sessionId, optionIndex, rc);
            if (rc == -1) {
                gMpDialog.exitRequested = true;
            }
        } else {
            gMpDialog.exitRequested = true;
        }
    }

    if (!gMpDialog.active) {
        return;
    }

    // Vote state changed: re-render the option texts (chooser names) and the
    // local selection highlight.
    if (gMpDialog.voteUiDirty && gMpDialog.hostParticipant) {
        gMpDialog.voteUiDirty = false;
        const char* texts[NET_DIALOG_MAX_OPTIONS];
        int reactions[NET_DIALOG_MAX_OPTIONS];
        int procs[NET_DIALOG_MAX_OPTIONS];
        mpDialogHostBuildNodeTexts(texts, reactions, procs);
        gameDialogCoopApplyNode(gMpDialog.replyText, reactions, texts, gMpDialog.optionCount, procs);
    }

    mpDialogHostDrawOverlay();
}

bool MpDialogHostShouldExit()
{
    // The modal closes when the session asked to end OR when the host stopped
    // being a participant (ESC/0 mid-session): the director flow resumes and
    // the session continues for the remaining participants.
    return gMpDialog.exitRequested
        || (gMpDialog.active && !gMpDialog.hostParticipant);
}

// ---------------------------------------------------------------------------
// Effective stats / perks / rewards (host, dialogue-gated)
// ---------------------------------------------------------------------------

static bool mpDialogRouteChecks(Object* critter)
{
    return gMpActive && gMpIsHost && gMpDialog.active && critter == gDude;
}

static int mpDialogMaxParticipantStat(int stat)
{
    int maxValue = -1;
    for (int i = 0; i < gMpDialog.participantCount; i++) {
        Object* avatar = mpDialogPlayerAvatar(gMpDialog.participants[i]);
        if (avatar != nullptr) {
            maxValue = std::max(maxValue, critterGetStat(avatar, (Stat)stat));
        }
    }
    return maxValue;
}

static int mpDialogMaxParticipantSkill(int skill)
{
    int maxValue = -1;
    for (int i = 0; i < gMpDialog.participantCount; i++) {
        Object* avatar = mpDialogPlayerAvatar(gMpDialog.participants[i]);
        if (avatar != nullptr) {
            maxValue = std::max(maxValue, skillGetValue(avatar, (Skill)skill));
        }
    }
    return maxValue;
}

int MpDialogRollStat(Object* critter, int stat, int modifier, int* howMuch)
{
    if (!mpDialogRouteChecks(critter)) {
        return statRoll(critter, (Stat)stat, modifier, howMuch);
    }
    int value = mpDialogMaxParticipantStat(stat);
    if (value == -1) {
        return statRoll(critter, (Stat)stat, modifier, howMuch);
    }
    value += modifier;
    int chance = randomBetween(PRIMARY_STAT_MIN, PRIMARY_STAT_MAX);
    if (howMuch != nullptr) {
        *howMuch = value - chance;
    }
    MpLog(MP_LOG_DIALOG, "stat check session=%u stat=%d value=%d roll=%d", gMpDialog.sessionId, stat, value, chance);
    return chance <= value ? ROLL_SUCCESS : ROLL_FAILURE;
}

int MpDialogRollSkill(Object* critter, int skill, int modifier, int* howMuch)
{
    if (!mpDialogRouteChecks(critter)) {
        return skillRoll(critter, (Skill)skill, modifier, howMuch);
    }
    int value = mpDialogMaxParticipantSkill(skill);
    if (value == -1) {
        return skillRoll(critter, (Skill)skill, modifier, howMuch);
    }
    int critChance = 0;
    for (int i = 0; i < gMpDialog.participantCount; i++) {
        Object* avatar = mpDialogPlayerAvatar(gMpDialog.participants[i]);
        if (avatar != nullptr && skillGetValue(avatar, (Skill)skill) == value) {
            critChance = critterGetStat(avatar, STAT_CRITICAL_CHANCE);
            break;
        }
    }
    int rc = randomRoll(value + modifier, critChance, howMuch);
    MpLog(MP_LOG_DIALOG, "skill check session=%u skill=%d value=%d rc=%d", gMpDialog.sessionId, skill, value, rc);
    return rc;
}

int MpDialogGetSkillValue(Object* critter, int skill)
{
    if (!mpDialogRouteChecks(critter)) {
        return skillGetValue(critter, (Skill)skill);
    }
    int value = mpDialogMaxParticipantSkill(skill);
    return value == -1 ? skillGetValue(critter, (Skill)skill) : value;
}

int MpDialogGetStat(Object* critter, int stat)
{
    if (!mpDialogRouteChecks(critter)) {
        return critterGetStat(critter, (Stat)stat);
    }
    int value = mpDialogMaxParticipantStat(stat);
    return value == -1 ? critterGetStat(critter, (Stat)stat) : value;
}

static Object* mpDialogInitiatorAvatarInternal()
{
    if (gMpDialog.initiatorNetId != 0) {
        return mpDialogPlayerAvatar(gMpDialog.initiatorNetId);
    }
    return nullptr;
}

int MpDialogGetPerkRank(Object* critter, int perk)
{
    if (!mpDialogRouteChecks(critter)) {
        return perkGetRank(critter, (Perk)perk);
    }
    // Perk-driven behavior follows the current initiator.
    Object* initiator = mpDialogInitiatorAvatarInternal();
    return initiator != nullptr ? perkGetRank(initiator, (Perk)perk) : perkGetRank(critter, (Perk)perk);
}

int MpDialogEmpathyRank()
{
    if (!gMpActive || !gMpIsHost || !gMpDialog.active) {
        return perkGetRank(gDude, PERK_EMPATHY);
    }
    Object* initiator = mpDialogInitiatorAvatarInternal();
    return initiator != nullptr ? perkGetRank(initiator, PERK_EMPATHY) : perkGetRank(gDude, PERK_EMPATHY);
}

int MpDialogGetIntelligence()
{
    if (!gMpActive || !gMpIsHost || !gMpDialog.active) {
        return critterGetStat(gDude, STAT_INTELLIGENCE);
    }
    int value = mpDialogMaxParticipantStat(STAT_INTELLIGENCE);
    return value == -1 ? critterGetStat(gDude, STAT_INTELLIGENCE) : value;
}

int MpDialogGrantExperience(int xp)
{
    if (!(gMpActive && gMpIsHost && gMpDialog.active)) {
        return 0;
    }

    // The host's own XP flows through the vanilla path (UI + level-up state);
    // remote players get the mirrored math on their avatar sheets.
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        if (!gMpSession.players[i].isConnected || gMpSession.players[i].isLocal) {
            continue;
        }
        Object* avatar = gMpSession.players[i].obj;
        if (avatar == nullptr) {
            continue;
        }

        int currentXp = MpProfileGetPcStat(avatar, PC_STAT_EXPERIENCE);
        int swiftLearner = perkGetRank(avatar, PERK_SWIFT_LEARNER);
        int newXp = currentXp + xp + swiftLearner * 5 * xp / 100;
        int level = MpProfileGetPcStat(avatar, PC_STAT_LEVEL);
        int maxHpBefore = critterGetStat(avatar, STAT_MAXIMUM_HIT_POINTS);

        while (level < 99) {
            int nextXp = pcGetExperienceForLevel(level + 1);
            if (nextXp == -1 || newXp < nextXp) {
                break;
            }
            level++;
            int endurance = critterGetBaseStatWithTraitModifier(avatar, STAT_ENDURANCE);
            int hpPerLevel = endurance / 2 + 2 + perkGetRank(avatar, PERK_LIFEGIVER) * 4;
            int bonusHp = critterGetBonusStat(avatar, STAT_MAXIMUM_HIT_POINTS);
            critterSetBonusStat(avatar, STAT_MAXIMUM_HIT_POINTS, bonusHp + hpPerLevel);
            int maxHpAfter = critterGetStat(avatar, STAT_MAXIMUM_HIT_POINTS);
            critterAdjustHitPoints(avatar, maxHpAfter - maxHpBefore);
            maxHpBefore = maxHpAfter;
            MpLog(MP_LOG_DIALOG, "xp level-up netId=%u level=%d hpBonus=%d", i + 1, level, hpPerLevel);
        }

        MpProfileSetPcStat(avatar, PC_STAT_EXPERIENCE, newXp);
        MpProfileSetPcStat(avatar, PC_STAT_LEVEL, level);
        MpLog(MP_LOG_DIALOG, "xp netId=%u xp=%d (+%d) level=%d", i + 1, newXp, xp, level);
    }
    return 0;
}

int MpDialogAdjustCaps(Object* target, int amount)
{
    if (!(gMpActive && gMpIsHost && gMpDialog.active) || target != gDude || amount <= 0) {
        return -1; // not routed; caller keeps the vanilla path
    }
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        if (!gMpSession.players[i].isConnected || gMpSession.players[i].isLocal) {
            continue;
        }
        Object* avatar = gMpSession.players[i].obj;
        if (avatar == nullptr) {
            continue;
        }
        itemCapsAdjust(avatar, amount);
        MpLog(MP_LOG_DIALOG, "caps netId=%u amount=%d", i + 1, amount);
    }
    return 0;
}

int MpDialogHeal(Object* critter, int amount)
{
    if (!(gMpActive && gMpIsHost && gMpDialog.active) || critter != gDude || amount <= 0) {
        return -1; // not routed; caller keeps the vanilla path
    }
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        if (!gMpSession.players[i].isConnected || gMpSession.players[i].isLocal) {
            continue;
        }
        Object* avatar = gMpSession.players[i].obj;
        if (avatar == nullptr) {
            continue;
        }
        critterAdjustHitPoints(avatar, amount);
        MpLog(MP_LOG_DIALOG, "heal netId=%u amount=%d", i + 1, amount);
    }
    return 0;
}

Object* MpDialogGetInitiatorAvatar()
{
    return mpDialogInitiatorAvatarInternal();
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

static void mpDialogClientReset()
{
    memset(&gMpDialogClient, 0, sizeof(gMpDialogClient));
    gMpDialogClient.window = -1;
}

// Clear a container (into a scratch box) and repopulate it from a state list.
static void mpDialogClientBarterClear(Object* container)
{
    if (container == nullptr || container->data.inventory.length == 0) {
        return;
    }
    Object* scratch = nullptr;
    if (objectCreateWithFidPid(&scratch, -1, PROTO_ID_JESSE_CONTAINER) == -1) {
        return;
    }
    itemMoveAll(container, scratch);
    objectDestroy(scratch, nullptr);
}

static void mpDialogClientBarterPopulate(Object* container, const NetBarterItem* items, int count)
{
    if (container == nullptr) {
        return;
    }
    mpDialogClientBarterClear(container);
    for (int i = 0; i < count; i++) {
        Object* item = nullptr;
        if (objectCreateWithPid(&item, (int)items[i].pid) == -1) {
            continue;
        }
        itemAdd(container, item, items[i].qty);
    }
}

static Object* mpDialogClientBarterMirrorCreate()
{
    Object* obj = nullptr;
    if (objectCreateWithFidPid(&obj, -1, PROTO_ID_JESSE_CONTAINER) == -1) {
        return nullptr;
    }
    obj->flags |= (OBJECT_HIDDEN | OBJECT_NO_SAVE);
    return obj;
}

// Rebuild the client's mirror tables from the authoritative BARTER_STATE.
static void mpDialogClientBarterRebuildMirrors()
{
    Object* mirror = gMpDialogClient.speakerObjNetId != 0 ? MpFindObjByNetId(gMpDialogClient.speakerObjNetId) : nullptr;
    if (mirror != nullptr) {
        mpDialogClientBarterPopulate(mirror, gMpDialogClient.npcItems, gMpDialogClient.npcItemCount);
    }
    mpDialogClientBarterPopulate(gMpDialogClient.offerMirror, gMpDialogClient.offers, gMpDialogClient.offerCount);
    mpDialogClientBarterPopulate(gMpDialogClient.requestMirror, gMpDialogClient.requests, gMpDialogClient.requestCount);
}

// ---------------------------------------------------------------------------
// Client-side inventory reconciliation.
//
// The client's local barter moves are routed through the host (the vanilla
// itemMoveForce is skipped), so the client's own gDude inventory NEVER
// changes during a trade. The host is authoritative: it moves the real items
// between the avatar, the offer table, the NPC and the request table, then
// ships the resulting state. These helpers replay the authoritative outcome
// onto the client's local inventory so nothing lingers or clones.
// ---------------------------------------------------------------------------

static void mpDialogClientRemoveFromDude(uint32_t pid, int qty)
{
    if (gDude == nullptr || qty <= 0) {
        return;
    }
    Object* scratch = nullptr;
    if (objectCreateWithFidPid(&scratch, -1, PROTO_ID_JESSE_CONTAINER) == -1) {
        return;
    }
    Object* item = mpDialogFindItemByPid(gDude, pid);
    if (item != nullptr) {
        int have = itemGetQuantity(gDude, item);
        if (qty > have) {
            qty = have;
        }
        if (qty > 0) {
            itemMoveForce(gDude, scratch, item, qty);
        }
    }
    objectDestroy(scratch, nullptr);
}

static void mpDialogClientAddToDude(uint32_t pid, int qty)
{
    if (gDude == nullptr || qty <= 0) {
        return;
    }
    Object* item = nullptr;
    if (objectCreateWithPid(&item, (int)pid) == -1) {
        return;
    }
    itemAdd(gDude, item, qty);
}

// A confirmed offer-table move changed the authoritative state: items added
// to the offer left the player's inventory; items removed from the offer
// returned to the player.
static void mpDialogClientReconcileOfferDelta(const NetBarterItem* oldOffers, int oldCount,
    const NetBarterItem* newOffers, int newCount)
{
    for (int i = 0; i < newCount; i++) {
        int oldQty = 0;
        for (int j = 0; j < oldCount; j++) {
            if (oldOffers[j].pid == newOffers[i].pid) {
                oldQty = oldOffers[j].qty;
                break;
            }
        }
        int delta = newOffers[i].qty - oldQty;
        if (delta > 0) {
            mpDialogClientRemoveFromDude(newOffers[i].pid, delta);
        } else if (delta < 0) {
            mpDialogClientAddToDude(newOffers[i].pid, -delta);
        }
    }
    for (int j = 0; j < oldCount; j++) {
        bool stillThere = false;
        for (int i = 0; i < newCount; i++) {
            if (newOffers[i].pid == oldOffers[j].pid) {
                stillThere = true;
                break;
            }
        }
        if (!stillThere) {
            mpDialogClientAddToDude(oldOffers[j].pid, oldOffers[j].qty);
        }
    }
}

// A successful commit moved the sold items (the pre-commit offer table) to
// the NPC and the bought items (the pre-commit request table) to the player.
// Replay both onto the local inventory.
static void mpDialogClientReconcileCommit(const NetBarterItem* sold, int soldCount,
    const NetBarterItem* bought, int boughtCount)
{
    for (int i = 0; i < soldCount; i++) {
        mpDialogClientRemoveFromDude(sold[i].pid, sold[i].qty);
    }
    for (int i = 0; i < boughtCount; i++) {
        mpDialogClientAddToDude(bought[i].pid, bought[i].qty);
    }
}

static void mpDialogClientCloseWindow()
{
    if (gMpDialogClient.tradeOpen) {
        // The vanilla trade loop is still driving input; it cannot be
        // interrupted from here — the session end path (host END packet or
        // local END) clears tradeOpen in mpDialogClientUpdateBarterUi.
        // Destroy the mirrors defensively so nothing leaks.
        gMpDialogClient.tradeOpen = false;
    }
    if (gMpDialogClient.offerMirror != nullptr) {
        objectDestroy(gMpDialogClient.offerMirror, nullptr);
        gMpDialogClient.offerMirror = nullptr;
    }
    if (gMpDialogClient.requestMirror != nullptr) {
        objectDestroy(gMpDialogClient.requestMirror, nullptr);
        gMpDialogClient.requestMirror = nullptr;
    }
    if (gMpDialogClient.window != -1) {
        windowDestroy(gMpDialogClient.window);
        gMpDialogClient.window = -1;
    }
    if (gameDialogCoopIsOpen()) {
        gameDialogCoopClose();
    }
    gMpDialogClient.modalOpen = false;
    gMpDialogClient.uiPending = false;
}

static bool mpDialogClientIsParticipant()
{
    for (int i = 0; i < gMpDialogClient.participantCount; i++) {
        if (gMpDialogClient.participants[i] == gMpSession.localNetId) {
            return true;
        }
    }
    return false;
}

// Append the chooser names to an option text: "Option text (Name1, Name2)".
// `selections` is indexed by netId - 1 (int8_t, -1 = none).
static void mpDialogFormatOptionWithChoosers(char* out, size_t outSize, const char* baseText,
    const uint8_t* participants, int participantCount, const int8_t* selections, int optionIndex)
{
    snprintf(out, outSize, "%s", baseText);
    char names[256] = "";
    size_t written = 0;
    for (int i = 0; i < participantCount; i++) {
        uint8_t netId = participants[i];
        if (netId < 1 || netId > NET_MAX_PLAYERS) {
            continue;
        }
        if (selections[netId - 1] != optionIndex) {
            continue;
        }
        const char* name = mpDialogParticipantName(netId);
        if (name[0] == '?') {
            continue;
        }
        size_t nameLen = strlen(name);
        if (written + nameLen + (written > 0 ? 2 : 0) >= sizeof(names)) {
            break;
        }
        if (written > 0) {
            names[written++] = ',';
            names[written++] = ' ';
        }
        memcpy(names + written, name, nameLen);
        written += nameLen;
        names[written] = '\0';
    }
    size_t used = strlen(out);
    size_t room = outSize - used - 1;
    if (written > 0 && room > written + 3) {
        out[used++] = ' ';
        out[used++] = '(';
        memcpy(out + used, names, written);
        used += written;
        out[used++] = ')';
        out[used] = '\0';
        room = outSize - used - 1;
    }

    // Always mark the local player's own selection with a visible "*" — the
    // option color highlight is nice to have, this is the guaranteed signal.
    bool localChose = gMpSession.localNetId >= 1
        && gMpSession.localNetId <= NET_MAX_PLAYERS
        && selections[gMpSession.localNetId - 1] == optionIndex;
    if (localChose && room >= 3) {
        out[used++] = ' ';
        out[used++] = '*';
        out[used] = '\0';
    }
}

// Build the current node's option texts with the chooser names appended, for
// rendering into the vanilla dialogue windows (host side).
static void mpDialogHostBuildNodeTexts(const char** texts, int* reactions, int* procs)
{
    static char formatted[NET_DIALOG_MAX_OPTIONS][NET_DIALOG_OPTION_TEXT_MAX + 64];
    int8_t selections[NET_MAX_PLAYERS];
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        selections[i] = gMpDialog.voters[i].selected;
    }
    for (int i = 0; i < gMpDialog.optionCount; i++) {
        mpDialogFormatOptionWithChoosers(formatted[i], sizeof(formatted[i]), gMpDialog.options[i].text,
            gMpDialog.participants, gMpDialog.participantCount, selections, i);
        texts[i] = formatted[i];
        reactions[i] = gMpDialog.options[i].reaction;
        procs[i] = gMpDialog.options[i].proc;
    }
}

// Push the stored client node into the vanilla dialogue windows.
static void mpDialogClientApplyNodeToVanilla()
{
    if (gMpDialogClient.optionCount <= 0) {
        return;
    }
    const char* texts[NET_DIALOG_MAX_OPTIONS];
    int reactions[NET_DIALOG_MAX_OPTIONS];
    static char formatted[NET_DIALOG_MAX_OPTIONS][NET_DIALOG_OPTION_TEXT_MAX + 64];
    for (int i = 0; i < gMpDialogClient.optionCount; i++) {
        mpDialogFormatOptionWithChoosers(formatted[i], sizeof(formatted[i]), gMpDialogClient.options[i].text,
            gMpDialogClient.participants, gMpDialogClient.participantCount, gMpDialogClient.selections, i);
        texts[i] = formatted[i];
        reactions[i] = gMpDialogClient.options[i].reaction;
    }
    // The client never runs reply procs — pass nullptr.
    gameDialogCoopApplyNode(gMpDialogClient.replyText, reactions, texts, gMpDialogClient.optionCount, nullptr);
    // Play the node's voice + lip phonemes (once per node; the name is
    // consumed here so re-applies after barter don't restart the speech).
    if (gMpDialogClient.audioName[0] != '\0') {
        MpLog(MP_LOG_DIALOG, "client lips start file=%s", gMpDialogClient.audioName);
        gameDialogStartLips(gMpDialogClient.audioName);
        gMpDialogClient.audioName[0] = '\0';
    }
}

static void mpDialogClientApplyNodeBody(const std::string& body)
{
    size_t offset = 0;
    auto read8 = [&body, &offset](int* out) {
        if (offset + 1 > body.size()) {
            return false;
        }
        *out = (uint8_t)body[offset];
        offset += 1;
        return true;
    };
    auto read32 = [&body, &offset](int* out) {
        if (offset + 4 > body.size()) {
            return false;
        }
        int32_t v;
        memcpy(&v, body.data() + offset, sizeof(v));
        *out = v;
        offset += 4;
        return true;
    };
    auto read16 = [&body, &offset](int* out) {
        if (offset + 2 > body.size()) {
            return false;
        }
        uint16_t v;
        memcpy(&v, body.data() + offset, sizeof(v));
        *out = v;
        offset += 2;
        return true;
    };
    auto readStr = [&body, &offset](char* out, size_t outSize, int len) {
        if (offset + len > body.size() || (size_t)len >= outSize) {
            return false;
        }
        memcpy(out, body.data() + offset, len);
        out[len] = '\0';
        offset += len;
        return true;
    };

    int version;
    if (!read8(&version) || (version != 2 && version != 3)) {
        MpLogAlways(MP_LOG_DIALOG, "client state rejected (bad version %d)", version);
        return;
    }
    int headFid;
    if (!read32(&headFid)) {
        return;
    }
    int reaction;
    if (!read8(&reaction)) {
        return;
    }
    gMpDialogClient.headFid = headFid;
    gMpDialogClient.reaction = (int8_t)reaction;
    int replyLen;
    if (!read16(&replyLen) || !readStr(gMpDialogClient.replyText, sizeof(gMpDialogClient.replyText), replyLen)) {
        return;
    }
    int optionCount;
    if (!read8(&optionCount)) {
        return;
    }
    gMpDialogClient.optionCount = std::min(optionCount, NET_DIALOG_MAX_OPTIONS);
    for (int i = 0; i < gMpDialogClient.optionCount; i++) {
        int msgListId, msgId, reaction, textLen;
        if (!read16(&msgListId) || !read16(&msgId) || !read8(&reaction) || !read16(&textLen)) {
            return;
        }
        if (!readStr(gMpDialogClient.options[i].text, sizeof(gMpDialogClient.options[i].text), textLen)) {
            return;
        }
        gMpDialogClient.options[i].msgListId = (int16_t)msgListId;
        gMpDialogClient.options[i].msgId = (int16_t)msgId;
        gMpDialogClient.options[i].reaction = (int8_t)reaction;
    }
    // v3 bodies append the lip-sync audio base name (empty = host silent).
    gMpDialogClient.audioName[0] = '\0';
    if (version >= 3) {
        int audioLen;
        if (!read8(&audioLen) || audioLen < 0 || audioLen >= (int)sizeof(gMpDialogClient.audioName)) {
            MpLog(MP_LOG_DIALOG, "client node bad audio len=%d", audioLen);
            return;
        }
        if (audioLen > 0 && !readStr(gMpDialogClient.audioName, sizeof(gMpDialogClient.audioName), audioLen)) {
            return;
        }
    }
    gMpDialogClient.resolvedOption = -1;
    gMpDialogClient.timerActive = false;
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        gMpDialogClient.selections[i] = -1;
    }
    gMpDialogClient.nodeApplied = true;
    // Live-render into the vanilla windows when they are up and the player is
    // not bartering (during barter the screen is hidden; it is re-applied on
    // barter end).
    if (!gMpDialogClient.barterActive && gameDialogCoopIsOpen()) {
        mpDialogClientApplyNodeToVanilla();
    }
    MpLog(MP_LOG_DIALOG, "client node session=%u node=%u options=%d", gMpDialogClient.sessionId, gMpDialogClient.nodeSeq, gMpDialogClient.optionCount);
}

static void mpDialogClientApplyVote(const NetDialogVotePayload* p)
{
    gMpDialogClient.initiatorNetId = p->initiatorNetId;
    gMpDialogClient.resolvedOption = p->resolvedOption;
    gMpDialogClient.timerActive = p->timerActive != 0;
    gMpDialogClient.timerEndTick = gMpDialogClient.timerActive ? getTicks() + p->timerMs : 0;
}

static const char* mpDialogClientBarterMessage(uint8_t msgId)
{
    switch (msgId) {
    case 27:
        return "Ok, that's a good trade.";
    case 28:
        return "No, your offer is not good enough.";
    case 31:
        return "Sorry, you cannot carry that much.";
    case 32:
        return "Sorry, that's too much to carry.";
    case 903:
        return "This person will not barter with you.";
    default:
        return "";
    }
}

// ---- packet dispatch ----

void MpDialogOnClientPacket(uint8_t packetType, const void* data, size_t dataLength)
{
    switch (packetType) {
    case NET_PKT_DIALOG_BEGIN: {
        if (dataLength < sizeof(NetDialogBeginPayload)) {
            return;
        }
        const NetDialogBeginPayload* p = static_cast<const NetDialogBeginPayload*>(data);
        size_t expected = sizeof(NetDialogBeginPayload) + p->participantCount;
        if (dataLength < expected || p->participantCount > NET_MAX_PLAYERS) {
            return;
        }
        if (gMpDialogClient.sessionActive && gMpDialogClient.sessionId == p->sessionId) {
            return; // already in this session
        }
        mpDialogClientReset();
        gMpDialogClient.sessionActive = true;
        gMpDialogClient.sessionId = p->sessionId;
        gMpDialogClient.speakerObjNetId = p->speakerObjNetId;
        gMpDialogClient.initiatorNetId = p->initiatorNetId;
        gMpDialogClient.participantCount = p->participantCount;
        memcpy(gMpDialogClient.participants, p->participants, p->participantCount);
        if (mpDialogClientIsParticipant()) {
            gMpDialogClient.uiPending = true;
        }
        MpLog(MP_LOG_DIALOG, "client begin session=%u participants=%d local=%u",
            p->sessionId, p->participantCount, gMpSession.localNetId);
        break;
    }
    case NET_PKT_DIALOG_STATE: {
        if (dataLength < sizeof(NetDialogStateChunkHeader)) {
            return;
        }
        const NetDialogStateChunkHeader* h = static_cast<const NetDialogStateChunkHeader*>(data);
        size_t payloadLen = dataLength - sizeof(NetDialogStateChunkHeader);
        if (h->chunkSize != payloadLen || h->chunkCount == 0 || h->chunkIndex >= h->chunkCount) {
            return;
        }
        if (!gMpDialogClient.sessionActive || h->sessionId != gMpDialogClient.sessionId) {
            return;
        }
        if (h->nodeSeq < gMpDialogClient.nodeSeq) {
            return; // stale
        }
        if (h->nodeSeq != gMpDialogClient.chunkNodeSeq) {
            // New node: start a fresh accumulation.
            gMpDialogClient.chunkNodeSeq = h->nodeSeq;
            gMpDialogClient.chunkCount = h->chunkCount;
            gMpDialogClient.chunkBuffer.clear();
        }
        gMpDialogClient.chunkBuffer.append(
            static_cast<const char*>(data) + sizeof(NetDialogStateChunkHeader), payloadLen);
        if (h->chunkIndex + 1 == h->chunkCount) {
            gMpDialogClient.nodeSeq = h->nodeSeq;
            gMpDialogClient.chunkCount = 0;
            gMpDialogClient.chunkNodeSeq = 0;
            std::string body;
            body.swap(gMpDialogClient.chunkBuffer);
            mpDialogClientApplyNodeBody(body);
        }
        break;
    }
    case NET_PKT_DIALOG_VOTE: {
        if (dataLength < sizeof(NetDialogVotePayload)) {
            return;
        }
        const NetDialogVotePayload* p = static_cast<const NetDialogVotePayload*>(data);
        if (!gMpDialogClient.sessionActive || p->sessionId != gMpDialogClient.sessionId || p->nodeSeq != gMpDialogClient.nodeSeq) {
            return;
        }
        size_t expected = sizeof(NetDialogVotePayload) + (size_t)p->participantCount * sizeof(NetDialogVoteEntry);
        if (dataLength < expected || p->participantCount > NET_MAX_PLAYERS) {
            return;
        }
        mpDialogClientApplyVote(p);
        const NetDialogVoteEntry* entries =
            reinterpret_cast<const NetDialogVoteEntry*>(static_cast<const uint8_t*>(data) + sizeof(NetDialogVotePayload));
        for (int i = 0; i < p->participantCount; i++) {
            const NetDialogVoteEntry* e = &entries[i];
            if (e->netId < 1 || e->netId > NET_MAX_PLAYERS) {
                continue;
            }
            gMpDialogClient.selections[e->netId - 1] = (int8_t)e->optionIndex;
            gMpDialogClient.suspended[e->netId - 1] = (e->flags & 1) != 0;
        }
        MpLog(MP_LOG_DIALOG, "client vote session=%u node=%u resolved=%d timer=%d", p->sessionId, p->nodeSeq, p->resolvedOption, p->timerActive);
        // The chooser names and the local highlight changed: re-render the
        // option texts (no-op while bartering — the trade screen hides them).
        if (!gMpDialogClient.barterActive && gameDialogCoopIsOpen() && gMpDialogClient.nodeApplied) {
            mpDialogClientApplyNodeToVanilla();
        }
        break;
    }
    case NET_PKT_DIALOG_TRANSCRIPT: {
        if (dataLength < sizeof(NetDialogTranscriptPayload)) {
            return;
        }
        const NetDialogTranscriptPayload* p = static_cast<const NetDialogTranscriptPayload*>(data);
        if (!gMpDialogClient.sessionActive || p->sessionId != gMpDialogClient.sessionId) {
            return;
        }
        displayMonitorAddMessage(p->text);
        break;
    }
    case NET_PKT_DIALOG_JOIN: {
        if (dataLength < sizeof(NetDialogJoinPayload)) {
            return;
        }
        const NetDialogJoinPayload* p = static_cast<const NetDialogJoinPayload*>(data);
        if (!gMpDialogClient.sessionActive) {
            // Joined a session we never saw BEGIN for: the JOIN itself opens
            // it (the host sends the transcript replay and node state right
            // after, in order).
            mpDialogClientReset();
            gMpDialogClient.sessionActive = true;
            gMpDialogClient.sessionId = p->sessionId;
            gMpDialogClient.participantCount = 1;
            gMpDialogClient.participants[0] = p->netId;
            if (p->netId == gMpSession.localNetId) {
                gMpDialogClient.uiPending = true;
            }
            MpLog(MP_LOG_DIALOG, "client join opens session=%u netId=%u", p->sessionId, p->netId);
            break;
        }
        if (p->sessionId != gMpDialogClient.sessionId) {
            return;
        }
        for (int i = 0; i < gMpDialogClient.participantCount; i++) {
            if (gMpDialogClient.participants[i] == p->netId) {
                return;
            }
        }
        if (gMpDialogClient.participantCount < NET_MAX_PLAYERS) {
            gMpDialogClient.participants[gMpDialogClient.participantCount++] = p->netId;
        }
        if (p->netId == gMpSession.localNetId) {
            gMpDialogClient.uiPending = true;
        }
        MpLog(MP_LOG_DIALOG, "client join session=%u netId=%u", p->sessionId, p->netId);
        break;
    }
    case NET_PKT_DIALOG_LEAVE: {
        if (dataLength < sizeof(NetDialogLeavePayload)) {
            return;
        }
        const NetDialogLeavePayload* p = static_cast<const NetDialogLeavePayload*>(data);
        if (!gMpDialogClient.sessionActive || p->sessionId != gMpDialogClient.sessionId) {
            return;
        }
        for (int i = 0; i < gMpDialogClient.participantCount; i++) {
            if (gMpDialogClient.participants[i] == p->netId) {
                for (int j = i; j < gMpDialogClient.participantCount - 1; j++) {
                    gMpDialogClient.participants[j] = gMpDialogClient.participants[j + 1];
                }
                gMpDialogClient.participantCount--;
                break;
            }
        }
        MpLog(MP_LOG_DIALOG, "client leave session=%u netId=%u", p->sessionId, p->netId);
        break;
    }
    case NET_PKT_DIALOG_END: {
        if (dataLength < sizeof(NetDialogEndPayload)) {
            return;
        }
        const NetDialogEndPayload* p = static_cast<const NetDialogEndPayload*>(data);
        if (!gMpDialogClient.sessionActive || p->sessionId != gMpDialogClient.sessionId) {
            return;
        }
        MpLog(MP_LOG_DIALOG, "client end session=%u reason=%u", p->sessionId, p->reason);
        mpDialogClientCloseWindow();
        mpDialogClientReset();
        break;
    }
    case NET_PKT_BARTER_STATE: {
        if (dataLength < sizeof(NetBarterStatePayload)) {
            return;
        }
        const NetBarterStatePayload* p = static_cast<const NetBarterStatePayload*>(data);
        if (!gMpDialogClient.sessionActive || p->sessionId != gMpDialogClient.sessionId || p->netId != gMpSession.localNetId) {
            return;
        }
        const NetBarterItem* items =
            reinterpret_cast<const NetBarterItem*>(static_cast<const uint8_t*>(data) + sizeof(NetBarterStatePayload));
        size_t total = (size_t)p->npcItemCount + p->offerCount + p->requestCount;
        if (dataLength < sizeof(NetBarterStatePayload) + total * sizeof(NetBarterItem)
            || p->npcItemCount > NET_BARTER_MAX_ITEMS
            || p->offerCount > NET_BARTER_MAX_ITEMS
            || p->requestCount > NET_BARTER_MAX_ITEMS) {
            return;
        }
        // Snapshot the pre-state offer/request tables: a confirmed move or
        // commit needs them to replay the authoritative outcome onto the
        // local inventory (the local vanilla moves are routed through the
        // host and never touch gDude).
        NetBarterItem oldOffers[NET_BARTER_MAX_ITEMS];
        NetBarterItem oldRequests[NET_BARTER_MAX_ITEMS];
        int oldOfferCount = gMpDialogClient.offerCount;
        int oldRequestCount = gMpDialogClient.requestCount;
        memcpy(oldOffers, gMpDialogClient.offers, sizeof(oldOffers));
        memcpy(oldRequests, gMpDialogClient.requests, sizeof(oldRequests));

        gMpDialogClient.npcItemCount = std::min((int)p->npcItemCount, NET_BARTER_MAX_ITEMS);
        memcpy(gMpDialogClient.npcItems, items, gMpDialogClient.npcItemCount * sizeof(NetBarterItem));
        items += p->npcItemCount;
        gMpDialogClient.offerCount = std::min((int)p->offerCount, NET_BARTER_MAX_ITEMS);
        memcpy(gMpDialogClient.offers, items, gMpDialogClient.offerCount * sizeof(NetBarterItem));
        items += p->offerCount;
        gMpDialogClient.requestCount = std::min((int)p->requestCount, NET_BARTER_MAX_ITEMS);
        memcpy(gMpDialogClient.requests, items, gMpDialogClient.requestCount * sizeof(NetBarterItem));

        gMpDialogClient.lastOp = p->lastOp;
        gMpDialogClient.lastOk = p->lastOk;
        gMpDialogClient.lastMsgId = p->lastMsgId;
        gMpDialogClient.barterMod = p->barterMod;

        // Replay the authoritative outcome onto the local inventory.
        if (gMpDialogClient.tradeOpen && p->lastOk == 1) {
            if (p->lastOp == NET_BARTER_OP_COMMIT) {
                mpDialogClientReconcileCommit(oldOffers, oldOfferCount, oldRequests, oldRequestCount);
                MpLog(MP_LOG_DIALOG, "MPBARTER client commit reconcile sold=%d bought=%d", oldOfferCount, oldRequestCount);
            } else if (p->lastOp == NET_BARTER_OP_MOVE) {
                mpDialogClientReconcileOfferDelta(oldOffers, oldOfferCount,
                    gMpDialogClient.offers, gMpDialogClient.offerCount);
            }
        }

        if (p->lastOp == NET_BARTER_OP_START && p->lastOk == 1) {
            gMpDialogClient.barterActive = true;
            MpLog(MP_LOG_DIALOG, "client barter open session=%u", p->sessionId);
        } else if (p->lastOp == NET_BARTER_OP_END) {
            gMpDialogClient.barterActive = false;
            MpLog(MP_LOG_DIALOG, "client barter closed session=%u", p->sessionId);
        }
        if (p->lastMsgId != 0) {
            const char* msg = mpDialogClientBarterMessage(p->lastMsgId);
            if (msg[0] != '\0') {
                displayMonitorAddMessage(msg);
            }
        }

        // Live-rebuild the trade-screen mirrors when the vanilla trade
        // screen is open; the vanilla loop's per-frame tick re-renders.
        MpLog(MP_LOG_DIALOG, "MPBARTER client state op=%u ok=%u msg=%u npc=%u offer=%u req=%u tradeOpen=%d",
            p->lastOp, p->lastOk, p->lastMsgId, gMpDialogClient.npcItemCount,
            gMpDialogClient.offerCount, gMpDialogClient.requestCount, (int)gMpDialogClient.tradeOpen);
        if (gMpDialogClient.tradeOpen) {
            mpDialogClientBarterRebuildMirrors();
            gMpDialogClient.barterDirty = true;
            MpLog(MP_LOG_DIALOG, "MPBARTER client mirrors rebuilt (dirty)");
        }
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Client: dialogue modal
// ---------------------------------------------------------------------------

static void mpDialogClientSendChoice(int optionIndex)
{
    if (gMpSession.hostPeer == nullptr || !gMpDialogClient.sessionActive) {
        return;
    }
    NetDialogChoicePayload p;
    p.sessionId = gMpDialogClient.sessionId;
    p.nodeSeq = gMpDialogClient.nodeSeq;
    p.netId = gMpSession.localNetId;
    p.optionIndex = (uint8_t)optionIndex;
    NetSendPacket(gMpSession.hostPeer, NET_CHANNEL_RELIABLE, NET_PKT_DIALOG_CHOICE, &p, sizeof(p));
}

static void mpDialogClientSendLeave()
{
    if (gMpSession.hostPeer == nullptr || !gMpDialogClient.sessionActive) {
        return;
    }
    NetDialogLeavePayload p;
    p.sessionId = gMpDialogClient.sessionId;
    p.netId = gMpSession.localNetId;
    p.reason = 1;
    NetSendPacket(gMpSession.hostPeer, NET_CHANNEL_RELIABLE, NET_PKT_DIALOG_LEAVE, &p, sizeof(p));
}

static void mpDialogClientSendBarter(uint8_t op, uint8_t target, uint32_t pid, int32_t qty)
{
    if (gMpSession.hostPeer == nullptr || !gMpDialogClient.sessionActive) {
        return;
    }
    NetBarterCmdPayload p;
    memset(&p, 0, sizeof(p));
    p.sessionId = gMpDialogClient.sessionId;
    p.op = op;
    p.netId = gMpSession.localNetId;
    p.target = target;
    p.pid = pid;
    p.qty = qty;
    NetSendPacket(gMpSession.hostPeer, NET_CHANNEL_RELIABLE, NET_PKT_BARTER_CMD, &p, sizeof(p));
}

// Draws a pane of pid-based entries (client offer/request/npc lists).
static void mpDialogDrawClientWindow()
{
    // Barter: the vanilla trade screen renders itself (mirrors + refresh);
    // nothing to draw here. Dialogue: the participant panel.
    if (gMpDialogClient.barterActive) {
        return;
    }

    // ---- Participant panel (top-right, like the host's overlay). ----
    // The vanilla dialogue windows render the actual dialogue screen.
    int win = gMpDialogClient.window;
    if (win == -1) {
        return;
    }

    char line[512];

    constexpr int W = 240;
    constexpr int H = 150;
    windowFill(win, 1, 1, W - 2, H - 2, COLOR_BLACK);

    int y = 4;
    const char* title = "DIALOGUE";
    windowDrawText(win, title, 0, (W - fontGetStringWidth(title)) / 2, y, COLOR_WHITE);
    y += 14;

    for (int i = 0; i < gMpDialogClient.participantCount && i < 6; i++) {
        uint8_t netId = gMpDialogClient.participants[i];
        if (netId < 1 || netId > NET_MAX_PLAYERS) {
            continue;
        }
            const char* name = mpDialogParticipantName(netId);
            if (gMpDialogClient.suspended[netId - 1]) {
                snprintf(line, sizeof(line), "%s (barter)", name);
            } else {
                snprintf(line, sizeof(line), "%s", name);
            }
            if (netId == gMpDialogClient.initiatorNetId) {
                strncat(line, " *", sizeof(line) - strlen(line) - 1);
            }
            windowDrawText(win, line, 0, 8, y, COLOR_WHITE);
            y += 13;
    }

    if (gMpDialogClient.timerActive) {
        uint32_t remaining = gMpDialogClient.timerEndTick > getTicks() ? gMpDialogClient.timerEndTick - getTicks() : 0;
        snprintf(line, sizeof(line), "Majority in %d s", (int)(remaining / 1000) + 1);
        windowDrawText(win, line, 0, 8, y, COLOR_LIGHT_YELLOW);
        y += 13;
    }

    snprintf(line, sizeof(line), "1-9 choose | 0/ESC leave | B barter");
    windowDrawText(win, line, 0, 8, H - 16, COLOR_WHITE);

    windowRefresh(win);
}

// Open/close the vanilla trade screen around the vanilla dialogue screen.
// While open, the REAL vanilla trade loop (barterProcessUI) drives input;
// its co-op hooks pump the network, route M/T/ESC and slot clicks through
// the host, and rebuild the mirrors as authoritative state arrives.
static void mpDialogClientUpdateBarterUi()
{
    if (gMpDialogClient.barterActive && !gMpDialogClient.tradeOpen) {
        Object* mirror = gMpDialogClient.speakerObjNetId != 0 ? MpFindObjByNetId(gMpDialogClient.speakerObjNetId) : nullptr;
        if (mirror == nullptr) {
            MpLogAlways(MP_LOG_DIALOG, "client trade open abort: no speaker mirror");
            mpDialogClientSendBarter(NET_BARTER_OP_END, 0, 0, 0);
            gMpDialogClient.barterActive = false;
            return;
        }
        gMpDialogClient.offerMirror = mpDialogClientBarterMirrorCreate();
        gMpDialogClient.requestMirror = mpDialogClientBarterMirrorCreate();
        if (gMpDialogClient.offerMirror == nullptr || gMpDialogClient.requestMirror == nullptr) {
            if (gMpDialogClient.offerMirror != nullptr) {
                objectDestroy(gMpDialogClient.offerMirror, nullptr);
            }
            if (gMpDialogClient.requestMirror != nullptr) {
                objectDestroy(gMpDialogClient.requestMirror, nullptr);
            }
            gMpDialogClient.offerMirror = nullptr;
            gMpDialogClient.requestMirror = nullptr;
            mpDialogClientSendBarter(NET_BARTER_OP_END, 0, 0, 0);
            gMpDialogClient.barterActive = false;
            return;
        }
        mpDialogClientBarterRebuildMirrors();
        gameDialogCoopHideDialogue();
        // Vanilla parity: swap the dialogue window for the barter window.
        gameDialogCoopDestroyDialogueWindow();
        if (gameDialogCoopCreateBarterWindow() == -1) {
            gameDialogCoopRecreateDialogueWindow();
            gameDialogCoopShowDialogue();
            objectDestroy(gMpDialogClient.offerMirror, nullptr);
            objectDestroy(gMpDialogClient.requestMirror, nullptr);
            gMpDialogClient.offerMirror = nullptr;
            gMpDialogClient.requestMirror = nullptr;
            mpDialogClientSendBarter(NET_BARTER_OP_END, 0, 0, 0);
            gMpDialogClient.barterActive = false;
            MpLogAlways(MP_LOG_DIALOG, "client barter window create failed");
            return;
        }
        // The participant panel must not sit on top of the trade screen.
        if (gMpDialogClient.window != -1) {
            windowHide(gMpDialogClient.window);
        }
        gMpDialogClient.tradeOpen = true;
        MpLog(MP_LOG_DIALOG, "client trade screen open session=%u", gMpDialogClient.sessionId);

        // Blocking: the vanilla trade loop (co-op hooks inside). Returns when
        // the session ended (T/ESC, host END, abort).
        barterProcessUI(gameDialogGetWindow(), mirror, gMpDialogClient.offerMirror,
            gMpDialogClient.requestMirror, gMpDialogClient.barterMod);

        gMpDialogClient.tradeOpen = false;
        if (gMpDialogClient.offerMirror != nullptr) {
            objectDestroy(gMpDialogClient.offerMirror, nullptr);
            gMpDialogClient.offerMirror = nullptr;
        }
        if (gMpDialogClient.requestMirror != nullptr) {
            objectDestroy(gMpDialogClient.requestMirror, nullptr);
            gMpDialogClient.requestMirror = nullptr;
        }
        gameDialogCoopDestroyBarterWindow();
        gameDialogCoopRecreateDialogueWindow();
        gameDialogCoopShowDialogue();
        if (gMpDialogClient.window != -1) {
            windowShow(gMpDialogClient.window);
        }
        // The dialogue may have advanced while bartering: re-render the
        // current node into the restored vanilla windows.
        if (gMpDialogClient.nodeApplied) {
            mpDialogClientApplyNodeToVanilla();
        }
        MpLog(MP_LOG_DIALOG, "client trade screen closed session=%u", gMpDialogClient.sessionId);
    }
}

static void mpDialogClientRunModal()
{
    gMpDialogClient.modalOpen = true;
    gMpDialogClient.uiPending = false;
    gMpModalActive = true;

    // Open the vanilla-style dialogue screen (head + reply + options).
    Object* mirror = gMpDialogClient.speakerObjNetId != 0 ? MpFindObjByNetId(gMpDialogClient.speakerObjNetId) : nullptr;
    if (mirror == nullptr) {
        MpLogAlways(MP_LOG_DIALOG, "client modal abort: no speaker mirror netId=%u", gMpDialogClient.speakerObjNetId);
        gMpDialogClient.modalOpen = false;
        gMpModalActive = false;
        return;
    }
    gameDialogCoopOpen(gMpDialogClient.headFid, gMpDialogClient.reaction, mirror);
    if (gMpDialogClient.nodeApplied) {
        mpDialogClientApplyNodeToVanilla();
    }

    // Participant panel (top-right, matching the host overlay).
    constexpr int PW = 240;
    constexpr int PH = 150;
    int px = screenGetWidth() - PW - 8;
    int py = 8;
    int panel = windowCreate(px, py, PW, PH, COLOR_BLACK, WINDOW_MOVE_ON_TOP);
    if (panel == -1) {
        gameDialogCoopClose();
        gMpDialogClient.modalOpen = false;
        gMpModalActive = false;
        return;
    }
    windowDrawBorder(panel);
    gMpDialogClient.window = panel;

    MpLog(MP_LOG_DIALOG, "client modal open session=%u", gMpDialogClient.sessionId);

    while (gMpDialogClient.modalOpen) {
        sharedFpsLimiter.mark();
        int keyCode = inputGetInput();

        // While bartering, the vanilla trade loop (inside
        // mpDialogClientUpdateBarterUi) owns all input.
        if (!gMpDialogClient.barterActive) {
            if (keyCode >= 1200 && keyCode <= 1250) {
                // Vanilla option-button hover highlight.
                gameDialogCoopOptionHover(keyCode - 1200);
            } else if (keyCode >= 1300 && keyCode <= 1330) {
                gameDialogCoopOptionHoverExit(keyCode - 1300);
            } else if (keyCode >= 49 && keyCode <= 57) {
                int index = keyCode - 49;
                if (index < gMpDialogClient.optionCount) {
                    mpDialogClientSendChoice(index);
                }
            } else if (keyCode == KEY_0 || keyCode == KEY_ESCAPE) {
                mpDialogClientSendLeave();
                gMpDialogClient.modalOpen = false;
                break;
            } else if (keyCode == KEY_LOWERCASE_B) {
                MpDialogClientRequestBarter();
            } else if (keyCode == KEY_CTRL_Q || keyCode == KEY_CTRL_X || keyCode == KEY_F10) {
                showQuitConfirmationDialog();
            }
        }

        MpTick();

        if (!gMpDialogClient.sessionActive && !gMpDialogClient.barterActive) {
            break;
        }

        mpDialogClientUpdateBarterUi();
        mpDialogDrawClientWindow();
        renderPresent();
        sharedFpsLimiter.throttle();
    }

    mpDialogClientCloseWindow();
    gMpModalActive = false;
    MpLog(MP_LOG_DIALOG, "client modal closed");
}

bool MpDialogClientSessionActive()
{
    return gMpDialogClient.sessionActive;
}

void MpDialogClientRequestBarter()
{
    if (!gMpDialogClient.sessionActive || gMpDialogClient.barterActive) {
        return;
    }
    mpDialogClientSendBarter(NET_BARTER_OP_START, 0, 0, 0);
}

void MpDialogClientMaybeShowUI()
{
    if (!gMpActive || !gMpIsClient) {
        return;
    }
    if (!gMpDialogClient.uiPending || gMpDialogClient.modalOpen || gMpDialogClient.window != -1) {
        return;
    }
    // Wait for the first node: the head portrait and reply text come with the
    // first STATE packet, and the modal is built around them.
    if (!gMpDialogClient.nodeApplied) {
        return;
    }
    // Never stack on a vote modal (a vote window being up blocks this).
    if (gVoteSession.voteWindow != -1) {
        return;
    }
    if (isInCombat() || MpCombatIsActive()) {
        return;
    }
    mpDialogClientRunModal();
}

bool MpDialogAnyModalActive()
{
    return gMpDialogClient.modalOpen;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void MpDialogInit()
{
    memset(&gMpDialog, 0, sizeof(gMpDialog));
    gMpDialog.hostVoteWindow = -1;
    mpDialogClientReset();
}

void MpDialogShutdown()
{
    mpDialogClientCloseWindow();
    if (gMpDialog.hostVoteWindow != -1) {
        windowDestroy(gMpDialog.hostVoteWindow);
    }
    memset(&gMpDialog, 0, sizeof(gMpDialog));
    gMpDialog.hostVoteWindow = -1;
    mpDialogClientReset();
}

void MpDialogReset()
{
    MpDialogShutdown();
}

} // namespace fallout
