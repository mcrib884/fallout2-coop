#ifndef FALLOUT_MULTIPLAYER_DIALOG_H_
#define FALLOUT_MULTIPLAYER_DIALOG_H_

#include "net.h"
#include "obj_types.h"

// Max number of stacked dialogue floats (NPC lines + player choices). Each
// new float pushes the older ones one float-height up; the newest sits at
// the bottom, closest to the critter. Shared by the host (mpDialogFloat)
// and the client display (MpCombatOnFloatMessage) so both screens stack
// identically.
#define MP_DIALOG_FLOAT_STACK_MAX (8)

namespace fallout {

// Synchronized dialogue and barter (host-authoritative).
//
// The host owns every dialogue script and barter table; clients receive
// node state, vote on options, and see the shared NPC inventory through
// reliable packets. All script-side effects (rewards, checks, perks) are
// routed through this module while an mp dialogue is active.
//
// See multiplayer_dialog.cc for the full state machine.

// Node content captured from game_dialog.cc's file-statics. The host copies
// everything into its session; clients rebuild from the wire.
typedef struct MpDialogNodeData {
    const char* replyText;
    int optionCount;
    const int* optionMsgListIds;
    const int* optionMsgIds;
    const int* optionReactions;
    const char* const* optionTexts;
    const int* optionProcs; // script procedure per option (host join re-injection)
} MpDialogNodeData;

// --- Lifecycle (multiplayer.cc) ---

void MpDialogInit();
void MpDialogShutdown();
void MpDialogReset();

// --- Host-side hooks (game_dialog.cc / actions.cc / multiplayer.cc) ---

// Records which player initiated the next dialogue (consumed at the first
// node). 0 = scripted conversation (every connected player participates).
void MpDialogSetPendingInitiator(uint8_t netId);
void MpDialogClearPendingInitiator();

// True while the host has an active mp dialogue session (script paused at
// dialog_go). Used to gate world-tick processing and effect routing.
bool MpDialogHostActive();
// A remote/host player tried to talk to `speaker` while a session is already
// running on that NPC: join the session instead of re-entering the talk
// script. Returns true when the join happened (caller must skip actionTalk).
bool MpDialogHostTryJoin(Object* speaker, uint8_t netId);

// The current node was finalized (called after _gdProcessUpdate). Broadcasts
// session begin / node state / NPC transcript line to participants.
void MpDialogHostNodeReady(const MpDialogNodeData* node);

// The dialogue script ended normally (called from _gdialogExitFromScript).
void MpDialogHostEnd();

// Called every frame of the host's blocking dialogue loop: pumps the network
// and world, resolves votes, runs the host barter modal, and handles
// interruptions (combat, dead speaker, passed map vote).
void MpDialogHostPump();

// The dialogue loop must break now (interruption or resolved close).
bool MpDialogHostShouldExit();

// scripts.cc: allow critter/timed processing while an mp dialogue is open.
bool MpDialogAllowWorldTick();

// game_dialog.cc: host is a participant (has the vanilla UI + vote overlay).
bool MpDialogHostIsParticipant();

// True when the host is a DIRECTOR: a session is active (or about to start)
// that the host is not part of. The host never enters the blocking dialogue
// modal in this mode — the session is driven from MpTick instead, so the
// host keeps playing normally behind the client's dialogue.
bool MpDialogDirectorMode();

// Avatar of the player who initiated the pending dialogue (host-side).
// nullptr when no pending initiator, not host, or the player is missing.
Object* MpDialogPendingInitiatorAvatar();

// Avatar of the player who initiated the ACTIVE session (host-side). Unlike
// the pending variant this stays valid for the whole session, so scripts that
// resolve "dude_obj" mid-dialogue keep seeing the initiator even when another
// player walks closer. nullptr when no session, no initiator, or not host.
Object* MpDialogInitiatorAvatar();

// Current node sequence of the active host session (used by the director
// choice path to detect whether a reply proc built a new node).
uint32_t MpDialogHostNodeSeq();

// Current session id of the active host session (diagnostics).
uint32_t MpDialogHostSessionId();

// Per-frame driver for a director-host session. Called from MpTick (host
// branch) — never from the dialogue modal. Handles interruptions, the
// majority timer, and resolved-choice dispatch without any window work.
void MpDialogHostDirectorTick();

// game_dialog.cc: the host participant picked an option / left via keys.
void MpDialogHostLocalChoice(int optionIndex);
void MpDialogHostLocalLeave();

// game_dialog.cc: host pressed the barter button while in mp dialogue.
void MpDialogHostRequestBarter();

// The option index the local player currently has selected (host or client),
// or -1 when none/outside a session. Used to highlight the chosen option.
int MpDialogMySelection();

// --- Effective stat / perk routing (host, dialogue-gated) ---
// Requirement: dialogue checks use the highest stat/skill among the players
// currently in the dialogue; perk-driven behavior follows the current
// initiator. These helpers return the vanilla result when no mp dialogue is
// active.

int MpDialogRollStat(Object* critter, int stat, int modifier, int* howMuch);
int MpDialogRollSkill(Object* critter, int skill, int modifier, int* howMuch);
int MpDialogGetSkillValue(Object* critter, int skill);
int MpDialogGetStat(Object* critter, int stat);
int MpDialogGetPerkRank(Object* critter, int perk);
int MpDialogEmpathyRank();
int MpDialogGetIntelligence();

// --- Reward routing (host, dialogue-gated; interpreter_extra.cc) ---

// XP to every connected player (per-player swift-learner bonus, level-up
// math mirrors pcAddExperienceWithOptions on each avatar). Returns 0.
int MpDialogGrantExperience(int xp);
// Positive caps to every connected avatar. Returns 0 on success.
int MpDialogAdjustCaps(Object* target, int amount);
// Positive healing to every connected avatar. Returns 0 on success.
int MpDialogHeal(Object* critter, int amount);
// Avatar of the current initiator (nullptr when none). Item rewards target.
Object* MpDialogGetInitiatorAvatar();

// --- Client side (multiplayer.cc) ---

// Inline packet dispatch for the dialogue/barter packet types.
void MpDialogOnClientPacket(uint8_t packetType, const void* data, size_t dataLength);

// Opens the client dialogue modal on the next top-level MpTick.
void MpDialogClientMaybeShowUI();

// True while the client dialogue/barter modal is open (blocks vote UI and
// the deferred-packet drain inside MpTick).
bool MpDialogAnyModalActive();

// Client-side helpers for game_dialog.cc's vanilla dialogue screen.
bool MpDialogClientSessionActive();
// The client pressed the vanilla barter button: start an mp barter session.
void MpDialogClientRequestBarter();

// --- Vanilla barter loop hooks (inventory.cc barterProcessUI calls these,
// gated on gMpActive, when the co-op session drives the trade screen) ---

// True while the local player's mp barter session is open.
bool MpDialogBarterSessionOpen();
// Per-frame pump inside the vanilla trade loop: network + world, external
// table refresh, interruption checks. Returns false → the loop must break
// (session ended/aborted).
bool MpDialogBarterLoopTick();
// Key interception before the vanilla T/M handling. Returns true when the key
// was consumed by the mp session (host: authoritative commit/end; client:
// request packet). ESC is also consumed so the session ends cleanly.
bool MpDialogBarterInterceptKey(int keyCode);
// Called from the vanilla barter move helpers at the exact point where the
// item would transfer locally (after the vanilla drag/drop UX). Routes the
// transfer through the host-authoritative session. Returns true when handled
// (co-op), false when the caller must fall through to the vanilla move.
bool MpDialogBarterMoveFromVanilla(uint32_t pid, int quantity, int target, bool back);
// The vanilla trade loop exited — tear down the mp barter session (host:
// return items, un-suspend the voter, broadcast; client: no-op, the session
// end is host-driven).
void MpDialogBarterLoopEnded();

// Host-side packet handlers (multiplayer.cc dispatch).
void MpDialogHostHandleChoice(const void* data, size_t dataLength, void* peer);
void MpDialogHostHandleLeave(const void* data, size_t dataLength, void* peer);
void MpDialogHostHandleBarterCmd(const void* data, size_t dataLength, void* peer);
void MpDialogHostPlayerDisconnected(uint8_t netId);

} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_DIALOG_H_ */
