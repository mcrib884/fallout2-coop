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
#include "multiplayer.h"
#include "multiplayer_combat.h"
#include "multiplayer_profile.h"
#include "multiplayer_vote.h"
#include "object.h"
#include "perk.h"
#include "proto.h"
#include "proto_types.h"
#include "random.h"
#include "skill.h"
#include "stat.h"
#include "stat_defs.h"
#include "svga.h"
#include "text_font.h"
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
    uint8_t pendingInitiator; // consumed at first node
};

static MpDialogSession gMpDialog = {};

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
    int barterFocus;   // 0 my, 1 offer, 2 request, 3 npc
    int barterSelected;
    int barterScroll[4];
};

static MpDialogClientState gMpDialogClient = {};

static constexpr int MP_DIALOG_WINDOW_W = 620;
static constexpr int MP_DIALOG_WINDOW_H = 400;
static constexpr int MP_BARTER_ITEMS_PER_PAGE = 10;

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
    debugFilePrint("MPDIALOG transcript session=%u seq=%u speaker=%u text=%s", gMpDialog.sessionId, seq, speakerNetId, line);
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
        const char* name = gMpSession.players[netId - 1].name;
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

    debugFilePrint("MPDIALOG resolve session=%u node=%u option=%d", gMpDialog.sessionId, gMpDialog.nodeSeq, optionIndex);
    mpDialogBroadcastVote();
}

// Recomputes the vote after any participant/selection/suspension change.
static void mpDialogRecalcVote()
{
    if (!gMpDialog.active) {
        return;
    }

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
            debugFilePrint("MPDIALOG vote timer cancelled (no active voters) session=%u", gMpDialog.sessionId);
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
            debugFilePrint("MPDIALOG majority session=%u node=%u option=%d timer=%dms",
                gMpDialog.sessionId, gMpDialog.nodeSeq, majorityOption, NET_DIALOG_VOTE_TIMER_MS);
        }
    } else {
        if (gMpDialog.timerActive) {
            gMpDialog.timerActive = false;
            gMpDialog.majorityOption = -1;
            debugFilePrint("MPDIALOG vote timer cancelled (majority lost) session=%u", gMpDialog.sessionId);
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
    debugFilePrint("MPBARTER teardown session=%u netId=%u", gMpDialog.sessionId, netId);
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
    memset(&gMpDialog, 0, sizeof(gMpDialog));
    gMpDialog.hostVoteWindow = -1;
}

static void mpDialogHostAbort(uint8_t reason)
{
    if (!gMpDialog.active) {
        return;
    }

    debugFilePrint("MPDIALOG abort session=%u reason=%u", gMpDialog.sessionId, reason);

    NetDialogEndPayload p;
    p.sessionId = gMpDialog.sessionId;
    p.reason = reason;
    NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE, NET_PKT_DIALOG_END, &p, sizeof(p));

    gMpDialog.exitRequested = true;
    mpDialogHostClearSession();
}

void MpDialogHostEnd()
{
    // Normal script end (end_dialogue -> _gdialogExitFromScript).
    gMpDialog.pendingInitiator = 0;
    if (!gMpDialog.active) {
        return;
    }

    debugFilePrint("MPDIALOG end session=%u normal", gMpDialog.sessionId);

    NetDialogEndPayload p;
    p.sessionId = gMpDialog.sessionId;
    p.reason = NET_DIALOG_END_NORMAL;
    NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE, NET_PKT_DIALOG_END, &p, sizeof(p));

    mpDialogHostClearSession();
}

void MpDialogSetPendingInitiator(uint8_t netId)
{
    gMpDialog.pendingInitiator = netId;
    debugFilePrint("MPDIALOG pending initiator=%u", netId);
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

bool MpDialogAllowWorldTick()
{
    return gMpActive && gMpIsHost && gMpDialog.active;
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
    debugFilePrint("MPDIALOG host participant UI hidden=%d", wantHidden ? 1 : 0);
}

static void mpDialogHostSendStateToPeer(uint8_t netId)
{
    if (!mpDialogCanSendTo(netId)) {
        return;
    }

    // Serialize the node body.
    std::string body;
    body.push_back((char)1); // version
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
        return; // same node (re-entrant _gdProcessUpdate); nothing new
    }

    bool firstNode = !gMpDialog.active;
    if (firstNode) {
        memset(&gMpDialog, 0, sizeof(gMpDialog));
        gMpDialog.hostVoteWindow = -1;
        gMpDialog.active = true;
        gMpDialog.sessionId = (uint32_t)((getTicks() ^ (uintptr_t)node) & 0x7FFFFFFF);
        if (gMpDialog.sessionId == 0) {
            gMpDialog.sessionId = 1;
        }
        gMpDialog.speaker = gGameDialogSpeaker;
        gMpDialog.speakerNetId = gMpDialog.speaker != nullptr ? MpGetObjNetId(gMpDialog.speaker) : 0;
        gMpDialog.initiatorNetId = gMpDialog.pendingInitiator;
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

        debugFilePrint("MPDIALOG begin session=%u speaker=%u initiator=%u participants=%d",
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

    for (int i = 0; i < gMpDialog.participantCount; i++) {
        if (!mpDialogCanSendTo(gMpDialog.participants[i])) {
            continue;
        }
        mpDialogHostSendStateToPeer(gMpDialog.participants[i]);
    }
    mpDialogBroadcastVote();
    mpDialogHostSetParticipantUi();

    debugFilePrint("MPDIALOG node session=%u node=%u options=%d",
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
            debugFilePrint("MPDIALOG join ignored session=%u netId=%u (already participant)", gMpDialog.sessionId, netId);
            return true;
        }
        if (!mpDialogPlayerConnected(netId)) {
            return true;
        }
        gMpDialog.participants[gMpDialog.participantCount++] = netId;
        gMpDialog.voters[netId - 1].selected = -1;

        debugFilePrint("MPDIALOG join session=%u netId=%u participants=%d", gMpDialog.sessionId, netId, gMpDialog.participantCount);

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
            mpDialogHostSetParticipantUi();
        }
        mpDialogRecalcVote();
        return true;
    }

    // A different NPC while a session is running.
    if (mpDialogIsParticipant(netId)) {
        debugFilePrint("MPDIALOG talk blocked session=%u netId=%u (one dialogue per player)", gMpDialog.sessionId, netId);
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
        debugFilePrint("MPDIALOG choice rejected (short payload) len=%u", (unsigned)dataLength);
        return;
    }
    const NetDialogChoicePayload* p = static_cast<const NetDialogChoicePayload*>(data);
    if (p->sessionId != gMpDialog.sessionId || p->nodeSeq != gMpDialog.nodeSeq) {
        debugFilePrint("MPDIALOG choice rejected (stale) session=%u/%u node=%u/%u", p->sessionId, gMpDialog.sessionId, p->nodeSeq, gMpDialog.nodeSeq);
        return;
    }
    if (!mpDialogIsParticipant(p->netId)) {
        debugFilePrint("MPDIALOG choice rejected (not participant) netId=%u", p->netId);
        return;
    }
    MultiplayerPlayer* player = &gMpSession.players[p->netId - 1];
    if (player->peer != peer) {
        debugFilePrint("MPDIALOG choice rejected (peer mismatch) netId=%u", p->netId);
        return;
    }
    if (gMpDialog.voters[p->netId - 1].suspended) {
        debugFilePrint("MPDIALOG choice rejected (bartering) netId=%u", p->netId);
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
        debugFilePrint("MPDIALOG choice rejected (out of range) option=%u count=%d", p->optionIndex, gMpDialog.optionCount);
        return;
    }

    debugFilePrint("MPDIALOG vote session=%u node=%u netId=%u option=%d",
        gMpDialog.sessionId, gMpDialog.nodeSeq, p->netId, p->optionIndex);
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
        debugFilePrint("MPDIALOG leave rejected (peer mismatch) netId=%u", p->netId);
        return;
    }

    debugFilePrint("MPDIALOG leave session=%u netId=%u", gMpDialog.sessionId, p->netId);

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
        debugFilePrint("MPDIALOG initiator handoff session=%u new=%u", gMpDialog.sessionId, gMpDialog.initiatorNetId);
    }

    if (p->netId == gMpSession.localNetId) {
        gMpDialog.hostParticipant = false;
        mpDialogHostSetParticipantUi();
    }

    if (gMpDialog.participantCount == 0) {
        debugFilePrint("MPDIALOG all left session=%u", gMpDialog.sessionId);
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
        debugFilePrint("MPDIALOG vote rejected (out of range) option=%d count=%d", optionIndex, gMpDialog.optionCount);
        return;
    }
    gMpDialog.voters[netId - 1].selected = (int8_t)optionIndex;
    debugFilePrint("MPDIALOG vote session=%u node=%u netId=%u option=%d", gMpDialog.sessionId, gMpDialog.nodeSeq, netId, optionIndex);
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

    debugFilePrint("MPDIALOG leave (local) session=%u netId=%u", gMpDialog.sessionId, netId);

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
        debugFilePrint("MPDIALOG initiator handoff session=%u new=%u", gMpDialog.sessionId, gMpDialog.initiatorNetId);
    }

    gMpDialog.hostParticipant = false;
    mpDialogHostSetParticipantUi();

    if (gMpDialog.participantCount == 0) {
        debugFilePrint("MPDIALOG all left session=%u", gMpDialog.sessionId);
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

    debugFilePrint("MPDIALOG disconnect session=%u netId=%u", gMpDialog.sessionId, netId);

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
        debugFilePrint("MPDIALOG initiator handoff (disconnect) session=%u new=%u", gMpDialog.sessionId, gMpDialog.initiatorNetId);
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
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        if (gMpDialog.barter[i].active) {
            mpDialogSendBarterState(i + 1, 0, 0, 0);
        }
    }
}

static bool mpDialogBarterStart(uint8_t netId)
{
    MpBarterSession* b = &gMpDialog.barter[netId - 1];
    if (b->active) {
        mpDialogSendBarterState(netId, NET_BARTER_OP_START, 1, 0);
        return true;
    }
    if (!mpDialogNpcCanBarter()) {
        debugFilePrint("MPBARTER start rejected session=%u netId=%u (no barter flag)", gMpDialog.sessionId, netId);
        mpDialogSendBarterState(netId, NET_BARTER_OP_START, 0, 903);
        return false;
    }

    b->active = true;
    b->netId = netId;
    b->offerTable = mpDialogContainerCreate();
    b->requestTable = mpDialogContainerCreate();

    gMpDialog.voters[netId - 1].suspended = true;
    debugFilePrint("MPBARTER start session=%u netId=%u", gMpDialog.sessionId, netId);
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
    debugFilePrint("MPBARTER end session=%u netId=%u", gMpDialog.sessionId, netId);
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
                debugFilePrint("MPBARTER move rejected session=%u netId=%u pid=0x%X qty=%d (avatar lacks item)", gMpDialog.sessionId, netId, pid, qty);
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
                debugFilePrint("MPBARTER move rejected session=%u netId=%u pid=0x%X qty=%d (npc lacks item)", gMpDialog.sessionId, netId, pid, qty);
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

    debugFilePrint("MPBARTER move session=%u netId=%u target=%u pid=0x%X qty=%d", gMpDialog.sessionId, netId, target, pid, qty);
    mpDialogSendBarterState(netId, NET_BARTER_OP_MOVE, 1, 0);
    if (target == 2) {
        mpDialogBroadcastBarterStateToBarterers();
    }
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
    debugFilePrint("MPBARTER commit session=%u netId=%u rc=%d", gMpDialog.sessionId, netId, rc);
    mpDialogSendBarterState(netId, NET_BARTER_OP_COMMIT, rc == 0 ? 1 : 0, msgId);
    mpDialogBroadcastBarterStateToBarterers();
}

void MpDialogHostHandleBarterCmd(const void* data, size_t dataLength, void* peer)
{
    if (!gMpActive || !gMpIsHost || !gMpDialog.active) {
        return;
    }
    if (dataLength < sizeof(NetBarterCmdPayload)) {
        debugFilePrint("MPBARTER cmd rejected (short payload) len=%u", (unsigned)dataLength);
        return;
    }
    const NetBarterCmdPayload* p = static_cast<const NetBarterCmdPayload*>(data);
    if (p->sessionId != gMpDialog.sessionId || !mpDialogIsParticipant(p->netId)) {
        return;
    }
    MultiplayerPlayer* player = &gMpSession.players[p->netId - 1];
    if (player->peer != peer) {
        debugFilePrint("MPBARTER cmd rejected (peer mismatch) netId=%u", p->netId);
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
        debugFilePrint("MPBARTER cmd rejected (unknown op) op=%u", p->op);
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
        const char* name = gMpSession.players[netId - 1].name;
        int sel = gMpDialog.voters[netId - 1].selected;
        if (gMpDialog.voters[netId - 1].suspended) {
            snprintf(line, sizeof(line), "%s: barter", name);
        } else if (sel >= 0) {
            snprintf(line, sizeof(line), "%s: [%d]", name, sel + 1);
        } else {
            snprintf(line, sizeof(line), "%s: -", name);
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
// Host: barter modal (text UI, shared layout with the client modal)
// ---------------------------------------------------------------------------

static const char* mpDialogBarterPaneTitle(int focus)
{
    switch (focus) {
    case 0:
        return "MY ITEMS";
    case 1:
        return "OFFER";
    case 2:
        return "REQUEST";
    default:
        return "NPC ITEMS";
    }
}

static const int MP_BARTER_PANE_X[4] = { 8, 160, 312, 464 };
static const int MP_BARTER_PANE_W = 148;

// Host-side item accessors (real objects).
static int mpDialogHostPaneCount(int pane)
{
    switch (pane) {
    case 0: {
        Object* avatar = mpDialogPlayerAvatar(gMpSession.localNetId);
        return avatar != nullptr ? avatar->data.inventory.length : 0;
    }
    case 1:
        return mpDialogCountContainerItems(gMpDialog.barter[gMpSession.localNetId - 1].offerTable);
    case 2:
        return mpDialogCountContainerItems(gMpDialog.barter[gMpSession.localNetId - 1].requestTable);
    default:
        return mpDialogCountNpcItems();
    }
}

static void mpDialogHostPaneEntry(int pane, int index, char* out, size_t outSize)
{
    Object* avatar = mpDialogPlayerAvatar(gMpSession.localNetId);
    switch (pane) {
    case 0: {
        if (avatar == nullptr || index >= avatar->data.inventory.length) {
            out[0] = '\0';
            return;
        }
        InventoryItem* entry = &avatar->data.inventory.items[index];
        snprintf(out, outSize, "%s x%d", itemGetName(entry->item), entry->quantity);
        break;
    }
    case 1:
    case 2: {
        Object* table = pane == 1
            ? gMpDialog.barter[gMpSession.localNetId - 1].offerTable
            : gMpDialog.barter[gMpSession.localNetId - 1].requestTable;
        if (table == nullptr || index >= table->data.inventory.length) {
            out[0] = '\0';
            return;
        }
        InventoryItem* entry = &table->data.inventory.items[index];
        snprintf(out, outSize, "%s x%d", itemGetName(entry->item), entry->quantity);
        break;
    }
    default: {
        if (gMpDialog.speaker == nullptr || index >= gMpDialog.speaker->data.inventory.length) {
            out[0] = '\0';
            return;
        }
        InventoryItem* entry = &gMpDialog.speaker->data.inventory.items[index];
        if (mpDialogItemIsEquipped(entry->item)) {
            out[0] = '\0';
            return;
        }
        snprintf(out, outSize, "%s x%d", itemGetName(entry->item), entry->quantity);
        break;
    }
    }
}

static void mpDialogHostBarterMoveSelected(int focus, int selectedIndex)
{
    uint8_t local = gMpSession.localNetId;
    Object* avatar = mpDialogPlayerAvatar(local);
    if (avatar == nullptr) {
        return;
    }
    MpBarterSession* b = &gMpDialog.barter[local - 1];

    switch (focus) {
    case 0: { // MY -> OFFER
        if (selectedIndex >= avatar->data.inventory.length) {
            return;
        }
        Object* item = avatar->data.inventory.items[selectedIndex].item;
        int qty = avatar->data.inventory.items[selectedIndex].quantity;
        itemMoveForce(avatar, b->offerTable, item, qty);
        mpDialogSendBarterState(local, NET_BARTER_OP_MOVE, 1, 0);
        break;
    }
    case 1: { // OFFER -> MY
        if (b->offerTable == nullptr || selectedIndex >= b->offerTable->data.inventory.length) {
            return;
        }
        Object* item = b->offerTable->data.inventory.items[selectedIndex].item;
        int qty = b->offerTable->data.inventory.items[selectedIndex].quantity;
        itemMoveForce(b->offerTable, avatar, item, qty);
        mpDialogSendBarterState(local, NET_BARTER_OP_MOVE, 1, 0);
        break;
    }
    case 2: { // REQUEST -> NPC
        if (b->requestTable == nullptr || selectedIndex >= b->requestTable->data.inventory.length) {
            return;
        }
        Object* item = b->requestTable->data.inventory.items[selectedIndex].item;
        int qty = b->requestTable->data.inventory.items[selectedIndex].quantity;
        itemMoveForce(b->requestTable, gMpDialog.speaker, item, qty);
        mpDialogSendBarterState(local, NET_BARTER_OP_MOVE, 1, 0);
        mpDialogBroadcastBarterStateToBarterers();
        break;
    }
    default: { // NPC -> REQUEST
        if (gMpDialog.speaker == nullptr || selectedIndex >= gMpDialog.speaker->data.inventory.length) {
            return;
        }
        InventoryItem* entry = &gMpDialog.speaker->data.inventory.items[selectedIndex];
        if (mpDialogItemIsEquipped(entry->item)) {
            return;
        }
        itemMoveForce(gMpDialog.speaker, b->requestTable, entry->item, entry->quantity);
        mpDialogSendBarterState(local, NET_BARTER_OP_MOVE, 1, 0);
        mpDialogBroadcastBarterStateToBarterers();
        break;
    }
    }
}

// Blocking host barter modal (nested inside the dialogue pump).
static void mpDialogHostRunBarter()
{
    uint8_t local = gMpSession.localNetId;
    MpBarterSession* b = &gMpDialog.barter[local - 1];
    if (!b->active) {
        gMpDialog.hostBarterUiOpen = false;
        return;
    }

    constexpr int W = MP_DIALOG_WINDOW_W;
    constexpr int H = 300;
    int x = (screenGetWidth() - W) / 2;
    int y = (screenGetHeight() - H) / 2 - 30;
    int win = windowCreate(x, y, W, H, COLOR_BLACK, WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    if (win == -1) {
        gMpDialog.hostBarterUiOpen = false;
        return;
    }
    windowDrawBorder(win);

    int focus = 0;
    int selected = -1;
    int scroll[4] = { 0, 0, 0, 0 };

    bool running = true;
    while (running) {
        sharedFpsLimiter.mark();
        int keyCode = inputGetInput();

        if (keyCode == KEY_TAB) {
            focus = (focus + 1) % 4;
            selected = -1;
        } else if (keyCode == KEY_ESCAPE || keyCode == KEY_LOWERCASE_T) {
            running = false;
        } else if (keyCode == KEY_ARROW_UP || keyCode == KEY_PAGE_UP) {
            if (selected > 0) {
                selected--;
            } else if (scroll[focus] > 0) {
                scroll[focus]--;
            }
        } else if (keyCode == KEY_ARROW_DOWN || keyCode == KEY_PAGE_DOWN) {
            if (selected < MP_BARTER_ITEMS_PER_PAGE - 1) {
                int count = mpDialogHostPaneCount(focus);
                if (scroll[focus] + selected + 1 < count) {
                    selected++;
                }
            } else if (scroll[focus] + MP_BARTER_ITEMS_PER_PAGE < mpDialogHostPaneCount(focus)) {
                scroll[focus]++;
            }
        } else if (keyCode >= 49 && keyCode <= 57) {
            selected = keyCode - 49;
        } else if (keyCode == KEY_0) {
            selected = 9;
        } else if (keyCode == KEY_RETURN || keyCode == KEY_SPACE) {
            if (selected >= 0) {
                int absolute = scroll[focus] + selected;
                if (absolute < mpDialogHostPaneCount(focus)) {
                    mpDialogHostBarterMoveSelected(focus, absolute);
                }
            }
        } else if (keyCode == KEY_LOWERCASE_M) {
            if (mpDialogCountContainerItems(b->offerTable) > 0 || mpDialogCountContainerItems(b->requestTable) > 0) {
                int rc = MpBarterAttemptTransaction(mpDialogPlayerAvatar(local), b->offerTable, gMpDialog.speaker, b->requestTable);
                uint8_t msgId = rc == 0 ? 27 : 28;
                debugFilePrint("MPBARTER commit (host) session=%u rc=%d", gMpDialog.sessionId, rc);
                mpDialogSendBarterState(local, NET_BARTER_OP_COMMIT, rc == 0 ? 1 : 0, msgId);
                mpDialogBroadcastBarterStateToBarterers();
            }
        }

        // Draw.
        windowFill(win, 1, 1, W - 2, H - 2, COLOR_BLACK);
        int drawY = 6;
        char line[192];
        for (int pane = 0; pane < 4; pane++) {
            const char* title = mpDialogBarterPaneTitle(pane);
            windowDrawText(win, title, 0, MP_BARTER_PANE_X[pane], drawY, pane == focus ? COLOR_LIGHT_YELLOW : COLOR_WHITE);
        }
        drawY += 16;
        for (int pane = 0; pane < 4; pane++) {
            int lineY = drawY;
            int count = mpDialogHostPaneCount(pane);
            for (int i = 0; i < MP_BARTER_ITEMS_PER_PAGE; i++) {
                int index = scroll[pane] + i;
                if (index >= count) {
                    break;
                }
                mpDialogHostPaneEntry(pane, index, line, sizeof(line));
                char marker[2] = { ' ', '\0' };
                if (pane == focus && i == selected) {
                    marker[0] = '>';
                }
                snprintf(line + strlen(line), sizeof(line) - strlen(line), "%s", marker);
                windowDrawText(win, line, 0, MP_BARTER_PANE_X[pane], lineY, COLOR_WHITE);
                lineY += 12;
                if (lineY > H - 30) {
                    break;
                }
            }
        }
        snprintf(line, sizeof(line), "TAB focus  |  1-9/0 select  |  ENTER move  |  M trade  |  T/ESC end");
        windowDrawText(win, line, 0, (W - fontGetStringWidth(line)) / 2, H - 20, COLOR_WHITE);

        windowRefresh(win);
        renderPresent();
        sharedFpsLimiter.throttle();

        // Combat or an interruption may be pending: abort the whole dialogue
        // (and this barter) so MpTick never starts combat under the modal.
        if (MpCombatIsActive() || gMpCombat.pendingStart) {
            mpDialogHostAbort(NET_DIALOG_END_COMBAT);
            running = false;
            break;
        }

        // Keep the network alive and process other players' packets.
        MpTick();
        if (!gMpDialog.active) {
            running = false;
        }
    }

    windowDestroy(win);
    mpDialogBarterEnd(local);
    gMpDialog.hostBarterUiOpen = false;
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
            debugFilePrint("MPDIALOG timer expired session=%u node=%u option=%d", gMpDialog.sessionId, gMpDialog.nodeSeq, gMpDialog.majorityOption);
            mpDialogResolve(gMpDialog.majorityOption);
        }
    }

    // Pending resolution -> run the vanilla choice path.
    if (gMpDialog.resolvedOption != -1) {
        int optionIndex = gMpDialog.resolvedOption;
        gMpDialog.resolvedOption = -1;
        if (optionIndex >= 0 && optionIndex < gMpDialog.optionCount) {
            if (gameDialogChooseOption(optionIndex) == -1) {
                gMpDialog.exitRequested = true;
            }
        } else {
            gMpDialog.exitRequested = true;
        }
    }

    if (!gMpDialog.active) {
        return;
    }

    mpDialogHostDrawOverlay();
}

bool MpDialogHostShouldExit()
{
    return gMpDialog.exitRequested;
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
    debugFilePrint("MPDIALOG stat check session=%u stat=%d value=%d roll=%d", gMpDialog.sessionId, stat, value, chance);
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
    debugFilePrint("MPDIALOG skill check session=%u skill=%d value=%d rc=%d", gMpDialog.sessionId, skill, value, rc);
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
            debugFilePrint("MPDIALOG xp level-up netId=%u level=%d hpBonus=%d", i + 1, level, hpPerLevel);
        }

        MpProfileSetPcStat(avatar, PC_STAT_EXPERIENCE, newXp);
        MpProfileSetPcStat(avatar, PC_STAT_LEVEL, level);
        debugFilePrint("MPDIALOG xp netId=%u xp=%d (+%d) level=%d", i + 1, newXp, xp, level);
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
        debugFilePrint("MPDIALOG caps netId=%u amount=%d", i + 1, amount);
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
        debugFilePrint("MPDIALOG heal netId=%u amount=%d", i + 1, amount);
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

static void mpDialogClientCloseWindow()
{
    if (gMpDialogClient.window != -1) {
        windowDestroy(gMpDialogClient.window);
        gMpDialogClient.window = -1;
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
    if (!read8(&version) || version != 1) {
        debugFilePrint("MPDIALOG client state rejected (bad version)");
        return;
    }
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
    gMpDialogClient.resolvedOption = -1;
    gMpDialogClient.timerActive = false;
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        gMpDialogClient.selections[i] = -1;
    }
    debugFilePrint("MPDIALOG client node session=%u node=%u options=%d", gMpDialogClient.sessionId, gMpDialogClient.nodeSeq, gMpDialogClient.optionCount);
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
        debugFilePrint("MPDIALOG client begin session=%u participants=%d local=%u",
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
        debugFilePrint("MPDIALOG client vote session=%u node=%u resolved=%d timer=%d", p->sessionId, p->nodeSeq, p->resolvedOption, p->timerActive);
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
            debugFilePrint("MPDIALOG client join opens session=%u netId=%u", p->sessionId, p->netId);
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
        debugFilePrint("MPDIALOG client join session=%u netId=%u", p->sessionId, p->netId);
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
        debugFilePrint("MPDIALOG client leave session=%u netId=%u", p->sessionId, p->netId);
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
        debugFilePrint("MPDIALOG client end session=%u reason=%u", p->sessionId, p->reason);
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

        if (p->lastOp == NET_BARTER_OP_START && p->lastOk == 1) {
            gMpDialogClient.barterActive = true;
            debugFilePrint("MPDIALOG client barter open session=%u", p->sessionId);
        } else if (p->lastOp == NET_BARTER_OP_END) {
            gMpDialogClient.barterActive = false;
            gMpDialogClient.barterFocus = 0;
            gMpDialogClient.barterSelected = -1;
            debugFilePrint("MPDIALOG client barter closed session=%u", p->sessionId);
        }
        if (p->lastMsgId != 0) {
            const char* msg = mpDialogClientBarterMessage(p->lastMsgId);
            if (msg[0] != '\0') {
                displayMonitorAddMessage(msg);
            }
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

// Client-side pane item counts for the barter layout.
static int mpDialogClientPaneCount(int pane)
{
    switch (pane) {
    case 0:
        return gDude->data.inventory.length;
    case 1:
        return gMpDialogClient.offerCount;
    case 2:
        return gMpDialogClient.requestCount;
    default:
        return gMpDialogClient.npcItemCount;
    }
}

// Greedy word wrap into at most maxLines lines.
static int mpDialogWrapText(const char* in, int maxWidth, char* out, size_t outSize, int maxLines)
{
    char line[512];
    int lineCount = 0;
    size_t written = 0;
    const char* start = in;
    while (*start != '\0' && lineCount < maxLines) {
        // Find the longest prefix that fits.
        int len = 0;
        int lastSpace = -1;
        while (start[len] != '\0') {
            if (start[len] == ' ') {
                lastSpace = len;
            }
            char tmp[512];
            strncpy(tmp, start, len + 1);
            tmp[len + 1] = '\0';
            if (fontGetStringWidth(tmp) > maxWidth) {
                break;
            }
            len++;
        }
        if (len == 0) {
            len = 1;
        }
        int end = len;
        if (start[end] != '\0' && lastSpace > 0 && lastSpace < len) {
            end = lastSpace;
        }
        snprintf(line, sizeof(line), "%.*s", end, start);
        size_t lineLen = strlen(line);
        if (written + lineLen + 1 < outSize) {
            if (written > 0) {
                out[written++] = '\n';
            }
            memcpy(out + written, line, lineLen);
            written += lineLen;
        }
        start += end;
        while (*start == ' ') {
            start++;
        }
        lineCount++;
    }
    out[written] = '\0';
    return lineCount;
}

// Draws a pane of pid-based entries (client offer/request/npc lists).
static void mpDialogDrawPidPane(int win, int x, int y, const NetBarterItem* items, int count, int scroll, int focus, int selected)
{
    for (int i = 0; i < MP_BARTER_ITEMS_PER_PAGE; i++) {
        int index = scroll + i;
        if (index >= count) {
            break;
        }
        const char* name = protoGetName((int)items[index].pid);
        if (name == nullptr) {
            name = "?";
        }
        char line[128];
        snprintf(line, sizeof(line), "%s x%d", name, items[index].qty);
        if (focus == 2 || focus == 3) {
            snprintf(line + strlen(line), sizeof(line) - strlen(line), " (%d)", items[index].unitValue);
        }
        if (focus && i == selected) {
            snprintf(line + strlen(line), sizeof(line) - strlen(line), " >");
        }
        windowDrawText(win, line, 0, x, y + i * 12, COLOR_WHITE);
    }
}

static void mpDialogDrawClientWindow()
{
    int win = gMpDialogClient.window;
    if (win == -1) {
        return;
    }

    constexpr int W = MP_DIALOG_WINDOW_W;
    constexpr int H = MP_DIALOG_WINDOW_H;
    windowFill(win, 1, 1, W - 2, H - 2, COLOR_BLACK);

    char line[512];

    if (gMpDialogClient.barterActive) {
        // ---- Barter layout ----
        int drawY = 6;
        for (int pane = 0; pane < 4; pane++) {
            windowDrawText(win, mpDialogBarterPaneTitle(pane), 0, MP_BARTER_PANE_X[pane], drawY,
                pane == gMpDialogClient.barterFocus ? COLOR_LIGHT_YELLOW : COLOR_WHITE);
        }
        drawY += 16;

        // MY items (real client inventory).
        int lineY = drawY;
        Inventory* inventory = &gDude->data.inventory;
        int scroll = gMpDialogClient.barterScroll[0];
        for (int i = 0; i < MP_BARTER_ITEMS_PER_PAGE; i++) {
            int index = scroll + i;
            if (index >= inventory->length) {
                break;
            }
            InventoryItem* entry = &inventory->items[index];
            snprintf(line, sizeof(line), "%s x%d", itemGetName(entry->item), entry->quantity);
            if (gMpDialogClient.barterFocus == 0 && i == gMpDialogClient.barterSelected) {
                snprintf(line + strlen(line), sizeof(line) - strlen(line), " >");
            }
            windowDrawText(win, line, 0, MP_BARTER_PANE_X[0], lineY, COLOR_WHITE);
            lineY += 12;
        }

        mpDialogDrawPidPane(win, MP_BARTER_PANE_X[1], drawY, gMpDialogClient.offers, gMpDialogClient.offerCount,
            gMpDialogClient.barterScroll[1], 1, gMpDialogClient.barterSelected);
        mpDialogDrawPidPane(win, MP_BARTER_PANE_X[2], drawY, gMpDialogClient.requests, gMpDialogClient.requestCount,
            gMpDialogClient.barterScroll[2], 2, gMpDialogClient.barterSelected);
        mpDialogDrawPidPane(win, MP_BARTER_PANE_X[3], drawY, gMpDialogClient.npcItems, gMpDialogClient.npcItemCount,
            gMpDialogClient.barterScroll[3], 3, gMpDialogClient.barterSelected);

        // Totals.
        int offerValue = 0;
        int requestValue = 0;
        for (int i = 0; i < gMpDialogClient.offerCount; i++) {
            offerValue += gMpDialogClient.offers[i].unitValue * gMpDialogClient.offers[i].qty;
        }
        for (int i = 0; i < gMpDialogClient.requestCount; i++) {
            requestValue += gMpDialogClient.requests[i].unitValue * gMpDialogClient.requests[i].qty;
        }
        snprintf(line, sizeof(line), "Offer: %d caps    Request: %d caps", offerValue, requestValue);
        windowDrawText(win, line, 0, 8, H - 36, COLOR_WHITE);
        snprintf(line, sizeof(line), "TAB focus | 1-9/0 select | ENTER move | M trade | T/ESC end");
        windowDrawText(win, line, 0, (W - fontGetStringWidth(line)) / 2, H - 20, COLOR_WHITE);
    } else {
        // ---- Dialogue layout ----
        const char* speakerName = "NPC";
        Object* mirror = gMpDialogClient.speakerObjNetId != 0 ? MpFindObjByNetId(gMpDialogClient.speakerObjNetId) : nullptr;
        if (mirror != nullptr) {
            speakerName = critterGetName(mirror);
            if (speakerName == nullptr || speakerName[0] == '\0') {
                speakerName = "NPC";
            }
        }
        snprintf(line, sizeof(line), "DIALOGUE WITH %s", speakerName);
        windowDrawText(win, line, 0, (W - fontGetStringWidth(line)) / 2, 4, COLOR_WHITE);

        // Reply text (wrapped).
        char wrapped[512];
        int lines = mpDialogWrapText(gMpDialogClient.replyText, 360, wrapped, sizeof(wrapped), 6);
        int drawY = 22;
        char* lineStart = wrapped;
        for (int i = 0; i < lines; i++) {
            char* newline = strchr(lineStart, '\n');
            if (newline != nullptr) {
                *newline = '\0';
            }
            windowDrawText(win, lineStart, 0, 8, drawY, COLOR_WHITE);
            drawY += 14;
            if (newline != nullptr) {
                lineStart = newline + 1;
            } else {
                break;
            }
        }
        drawY += 8;

        // Options (paged, 9 per page).
        int pageSize = 9;
        int pageCount = (gMpDialogClient.optionCount + pageSize - 1) / pageSize;
        if (gMpDialogClient.pageIndex >= pageCount) {
            gMpDialogClient.pageIndex = 0;
        }
        for (int i = 0; i < pageSize; i++) {
            int index = gMpDialogClient.pageIndex * pageSize + i;
            if (index >= gMpDialogClient.optionCount) {
                break;
            }
            int8_t selection = gMpDialogClient.selections[gMpSession.localNetId - 1];
            char marker = ' ';
            if (selection == index) {
                marker = '*';
            }
            snprintf(line, sizeof(line), "%d. %s %c", i + 1, gMpDialogClient.options[index].text, marker);
            windowDrawText(win, line, 0, 8, drawY, COLOR_WHITE);
            drawY += 14;
        }
        if (pageCount > 1) {
            snprintf(line, sizeof(line), "[Page %d/%d - arrows]", gMpDialogClient.pageIndex + 1, pageCount);
            windowDrawText(win, line, 0, 8, drawY, COLOR_LIGHT_YELLOW);
            drawY += 14;
        }
        drawY += 4;

        // Participants + votes (right column).
        int colX = 420;
        windowDrawText(win, "PARTICIPANTS", 0, colX, 22, COLOR_WHITE);
        int colY = 36;
        for (int i = 0; i < gMpDialogClient.participantCount && colY < H - 40; i++) {
            uint8_t netId = gMpDialogClient.participants[i];
            if (netId < 1 || netId > NET_MAX_PLAYERS) {
                continue;
            }
            const char* name = gMpSession.players[netId - 1].name;
            if (gMpDialogClient.suspended[netId - 1]) {
                snprintf(line, sizeof(line), "%s: barter", name);
            } else if (gMpDialogClient.selections[netId - 1] >= 0) {
                snprintf(line, sizeof(line), "%s: [%d]", name, gMpDialogClient.selections[netId - 1] + 1);
            } else {
                snprintf(line, sizeof(line), "%s: -", name);
            }
            if (netId == gMpDialogClient.initiatorNetId) {
                strncat(line, " *", sizeof(line) - strlen(line) - 1);
            }
            windowDrawText(win, line, 0, colX, colY, COLOR_WHITE);
            colY += 13;
        }
        if (gMpDialogClient.timerActive) {
            uint32_t remaining = gMpDialogClient.timerEndTick > getTicks() ? gMpDialogClient.timerEndTick - getTicks() : 0;
            snprintf(line, sizeof(line), "Majority in %d s", (int)(remaining / 1000) + 1);
            windowDrawText(win, line, 0, colX, colY, COLOR_LIGHT_YELLOW);
        }

        snprintf(line, sizeof(line), "1-9 choose | 0/ESC leave | B barter | arrows page");
        windowDrawText(win, line, 0, (W - fontGetStringWidth(line)) / 2, H - 20, COLOR_WHITE);
    }

    windowRefresh(win);
}

static void mpDialogClientHandleBarterKey(int keyCode)
{
    bool sent = false;
    switch (keyCode) {
    case KEY_TAB:
        gMpDialogClient.barterFocus = (gMpDialogClient.barterFocus + 1) % 4;
        gMpDialogClient.barterSelected = -1;
        break;
    case KEY_ESCAPE:
    case KEY_LOWERCASE_T:
        mpDialogClientSendBarter(NET_BARTER_OP_END, 0, 0, 0);
        gMpDialogClient.barterActive = false;
        gMpDialogClient.barterFocus = 0;
        gMpDialogClient.barterSelected = -1;
        break;
    case KEY_ARROW_UP:
    case KEY_PAGE_UP:
        if (gMpDialogClient.barterSelected > 0) {
            gMpDialogClient.barterSelected--;
        } else if (gMpDialogClient.barterScroll[gMpDialogClient.barterFocus] > 0) {
            gMpDialogClient.barterScroll[gMpDialogClient.barterFocus]--;
        }
        break;
    case KEY_ARROW_DOWN:
    case KEY_PAGE_DOWN: {
        int count = mpDialogClientPaneCount(gMpDialogClient.barterFocus);
        if (gMpDialogClient.barterSelected < MP_BARTER_ITEMS_PER_PAGE - 1) {
            if (gMpDialogClient.barterScroll[gMpDialogClient.barterFocus] + gMpDialogClient.barterSelected + 1 < count) {
                gMpDialogClient.barterSelected++;
            }
        } else if (gMpDialogClient.barterScroll[gMpDialogClient.barterFocus] + MP_BARTER_ITEMS_PER_PAGE < count) {
            gMpDialogClient.barterScroll[gMpDialogClient.barterFocus]++;
        }
        break;
    }
    case KEY_RETURN:
    case KEY_SPACE:
        if (gMpDialogClient.barterSelected >= 0) {
            int absolute = gMpDialogClient.barterScroll[gMpDialogClient.barterFocus] + gMpDialogClient.barterSelected;
            int focus = gMpDialogClient.barterFocus;
            if (focus == 0) {
                if (absolute < gDude->data.inventory.length) {
                    uint32_t pid = (uint32_t)gDude->data.inventory.items[absolute].item->pid;
                    int qty = gDude->data.inventory.items[absolute].quantity;
                    mpDialogClientSendBarter(NET_BARTER_OP_MOVE, 1, pid, qty);
                    sent = true;
                }
            } else if (focus == 1) {
                if (absolute < gMpDialogClient.offerCount) {
                    mpDialogClientSendBarter(NET_BARTER_OP_MOVE, 1, gMpDialogClient.offers[absolute].pid, -gMpDialogClient.offers[absolute].qty);
                    sent = true;
                }
            } else if (focus == 2) {
                if (absolute < gMpDialogClient.requestCount) {
                    mpDialogClientSendBarter(NET_BARTER_OP_MOVE, 2, gMpDialogClient.requests[absolute].pid, -gMpDialogClient.requests[absolute].qty);
                    sent = true;
                }
            } else {
                if (absolute < gMpDialogClient.npcItemCount) {
                    mpDialogClientSendBarter(NET_BARTER_OP_MOVE, 2, gMpDialogClient.npcItems[absolute].pid, gMpDialogClient.npcItems[absolute].qty);
                    sent = true;
                }
            }
            if (sent) {
                // Pessimistic local echo: mark the move as pending by clearing
                // the selection; the authoritative state refresh follows.
                gMpDialogClient.barterSelected = -1;
            }
        }
        break;
    case KEY_LOWERCASE_M:
        mpDialogClientSendBarter(NET_BARTER_OP_COMMIT, 0, 0, 0);
        break;
    default:
        break;
    }
}

// Blocking client modal (vote-modal pattern). Runs from MpTick top level.
static void mpDialogClientRunModal()
{
    gMpDialogClient.modalOpen = true;
    gMpDialogClient.uiPending = false;
    gMpDialogClient.pageIndex = 0;
    gMpDialogClient.barterFocus = 0;
    gMpDialogClient.barterSelected = -1;
    gMpModalActive = true;

    constexpr int W = MP_DIALOG_WINDOW_W;
    constexpr int H = MP_DIALOG_WINDOW_H;
    int x = (screenGetWidth() - W) / 2;
    int y = (screenGetHeight() - H) / 2 - 30;
    int win = windowCreate(x, y, W, H, COLOR_BLACK, WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    if (win == -1) {
        gMpDialogClient.modalOpen = false;
        return;
    }
    windowDrawBorder(win);
    gMpDialogClient.window = win;

    debugFilePrint("MPDIALOG client modal open session=%u", gMpDialogClient.sessionId);

    while (gMpDialogClient.modalOpen) {
        sharedFpsLimiter.mark();
        int keyCode = inputGetInput();

        if (gMpDialogClient.barterActive) {
            mpDialogClientHandleBarterKey(keyCode);
        } else {
            if (keyCode >= 49 && keyCode <= 57) {
                int index = gMpDialogClient.pageIndex * 9 + (keyCode - 49);
                if (index < gMpDialogClient.optionCount) {
                    mpDialogClientSendChoice(index);
                }
            } else if (keyCode == KEY_0 || keyCode == KEY_ESCAPE) {
                mpDialogClientSendLeave();
                gMpDialogClient.modalOpen = false;
            } else if (keyCode == KEY_LOWERCASE_B) {
                mpDialogClientSendBarter(NET_BARTER_OP_START, 0, 0, 0);
            } else if (keyCode == KEY_ARROW_UP) {
                if (gMpDialogClient.pageIndex > 0) {
                    gMpDialogClient.pageIndex--;
                }
            } else if (keyCode == KEY_ARROW_DOWN) {
                int pageCount = (gMpDialogClient.optionCount + 8) / 9;
                if (gMpDialogClient.pageIndex + 1 < pageCount) {
                    gMpDialogClient.pageIndex++;
                }
            }
        }

        MpTick();

        if (!gMpDialogClient.sessionActive && !gMpDialogClient.barterActive) {
            break;
        }

        mpDialogDrawClientWindow();
        renderPresent();
        sharedFpsLimiter.throttle();
    }

    mpDialogClientCloseWindow();
    gMpModalActive = false;
    debugFilePrint("MPDIALOG client modal closed");
}

void MpDialogClientMaybeShowUI()
{
    if (!gMpActive || !gMpIsClient) {
        return;
    }
    if (!gMpDialogClient.uiPending || gMpDialogClient.modalOpen || gMpDialogClient.window != -1) {
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
