#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "map.h"
#include "net.h"

namespace fallout {

typedef enum VoteState {
    VOTE_STATE_NONE = 0,
    VOTE_STATE_ACTIVE = 1,
    VOTE_STATE_PASSED = 2,
    VOTE_STATE_FAILED = 3,
    VOTE_STATE_CANCELLED = 4,
} VoteState;

#define VOTE_TIMEOUT_MS 30000
// After the last vote passes, the host keeps the modal up showing the final
// tally for this long before actually starting the map change.
#define VOTE_PASSED_DISPLAY_MS 1000

typedef struct VoteSession {
    VoteState state;
    uint8_t initiatorNetId;
    MapTransition transition;
    uint32_t startTime;
    uint32_t timeoutMs;
    uint32_t resolveDisplayStart; // set when the vote resolves (passed)
    uint8_t votes[NET_MAX_PLAYERS]; // 0 = pending, 1 = yes, 2 = no
    int yesCount;
    int noCount;
    uint8_t totalPlayers;
    int lastSafeTile;
    int lastSafeElevation;
    int lastSafeRotation;
    int voteWindow; // -1 when closed
    bool isInitiator; // local player initiated the vote
    bool uiPending;  // show the vote window on the next tick (never from
                     // inside the network callback: the modal pumps MpTick,
                     // which would re-enter NetHostService)
} VoteSession;

extern VoteSession gVoteSession;

void MpVoteInit();
void MpVoteShutdown();
void MpVoteReset();

// Host-side vote state machine.
int MpVoteStart(const MapTransition* transition, uint8_t initiatorNetId);
void MpVoteCastVote(uint8_t voterNetId, uint8_t vote);
void MpVoteCheckTimeout();
void MpVoteResolve();
void MpVoteCancel();

// Client-side vote receive handlers.
void MpVoteOnVoteStart(const NetVoteStartPayload* payload);
void MpVoteOnVoteResult(const NetVoteResultPayload* payload);
void MpVoteOnVoteTally(const NetVoteTallyPayload* payload);
void MpVoteSendCast(uint8_t vote);
void MpVoteBroadcastTally();

// Vote UI (host + client share the initiator modal; voters get a modal).
// Called from MpTick once per tick; blocks while the modal is up.
void MpVoteMaybeShowUI();
void MpVoteShowInitiatorUI();
void MpVoteShowVoterUI();
void MpVoteHideUI();
void MpVoteUpdateUI();

} // namespace fallout
