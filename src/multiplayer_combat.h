#ifndef FALLOUT_MULTIPLAYER_COMBAT_H_
#define FALLOUT_MULTIPLAYER_COMBAT_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "combat.h"
#include "net.h"
#include "obj_types.h"

namespace fallout {

// Combat mirror state, kept on BOTH machines. The host is the only combat
// simulator; clients send intents and mirror the host's broadcasts.
typedef struct MpCombatState {
    bool inCombat;             // mirror active (both machines)
    bool pendingStart;         // host: COMBAT_START_REQUEST queued, start on next tick
    bool startRequestPending;  // client: COMBAT_START_REQUEST already sent, wait for STARTED
    bool turnStartPending;     // client: TURN_START queued, run the blocking turn from MpTick
    uint8_t whoseTurn;         // 0 = no player (NPC turn); else player netId
    uint8_t waitingForTurnEnd; // host: netId of the remote player being waited on (0 = none)
    bool turnEndPending;       // host: TURN_END arrived but queued cmds still need to drain first
    bool turnActive;           // the blocking turn loop is running on this machine
    int turnAp;                // canonical AP granted by the host (TURN_START)
    int turnMaxAp;
    bool endRequestPending;    // end-combat request queued (any machine)
    uint8_t endRequesterNetId; // netId of the player who requested combat end
    // Client: while a predicted attack is resolving, drop locally generated
    // combat messages — the client rolls its own dice, so those lines are
    // frequently wrong. The host's authoritative broadcast replaces them.
    bool suppressLocalMonitor;
} MpCombatState;

extern MpCombatState gMpCombat;

void MpCombatReset();
bool MpCombatIsActive();
// Co-op: is this critter a player's (gDude or a synced player runtime)?
bool MpCombatIsPlayerCritter(const Object* obj);
// Co-op: nearest live player critter on obj's elevation (host only). Used by
// the dude_obj opcode hook so vanilla enemy detection considers all players.
Object* MpCombatGetNearestPlayerTo(const Object* obj);
// Called from MpTick on both machines: mirror side + deferred work.
void MpCombatTick();

// host hooks (called from combat.cc)
void MpCombatOnStarted();      // after _combat_begin on the host: broadcast COMBAT_STARTED
void MpCombatOnEnded();        // at _combat exit on the host: broadcast COMBAT_ENDED
// Host: an NPC's turn began. Broadcasts TURN_START (netId=0, targetNetId=the
// acting NPC) so the client's mirror can draw the vanilla acting outline.
void MpCombatOnNpcTurnStarted(Object* npc);
// Blocking host-side wait for a remote player's turn. Returns -1 when combat
// is ending (EXIT_REQUESTED) so the caller can break the round loop.
int MpCombatHostRemoteTurn(Object* critter, uint8_t netId);

// packet callbacks (called from the net event handler; only queue/defer work)
// The host receives the requesting player's critter (the ambusher) plus the
// netId of the object they attacked; both feed the deferred CombatStartData.
void MpCombatOnStartRequest(Object* attacker, uint32_t targetNetId);       // client -> host
void MpCombatOnCmd(uint8_t netId, const NetCombatCmdPayload* payload); // client -> host
void MpCombatOnTurnEnd(uint8_t netId);                               // client -> host
void MpCombatOnEndRequest(uint8_t netId);                            // client -> host
void MpCombatOnStartedPacket();                                      // host -> client
void MpCombatOnTurnStart(const NetCombatTurnStartPayload* payload);  // host -> client
void MpCombatOnEndDenied();                                          // host -> client
void MpCombatOnEndedPacket();                                        // host -> client
void MpCombatOnMoveResult(const NetCombatMoveResultPayload* payload); // host -> acting client
void MpCombatOnFloatMessage(const NetFloatMessagePayload* payload);  // host -> clients

// Display-monitor sync: the host is the only combat simulator, so NPC turns,
// host-player actions and the authoritative outcomes of remote intents only
// exist on the host. The host mirrors every combat monitor line to its
// clients; the acting client suppresses its own predicted combat messages in
// favor of the host's broadcast version.
void MpCombatBroadcastMonitorMessage(const char* text);
// Host: temporarily suppress the display-monitor broadcast (the host's own
// first-person lines are routed locally only — see combat.cc mpCombatMonitorLine).
void MpCombatSetMonitorBroadcastSuppressed(bool suppress);
bool MpCombatMonitorBroadcastSuppressed();
void MpCombatOnMonitorMessage(const char* text, size_t textLength);
void MpCombatBeginLocalAttackPrediction();
void MpCombatEndLocalAttackPrediction();
bool MpCombatMonitorSuppressed();

// client
void MpCombatSendStartRequest(Object* defender); // defender = the attacked target (may be null)
void MpCombatSendEndRequest();
void MpCombatSendMoveIntent(int tile, int elevation, bool isRun);
void MpCombatSendAttackIntent(Object* target, HitMode hitMode, HitLocation hitLocation);
void MpCombatSendInventoryApCost(int cost);
// Blocking client turn: the co-op input loop with network pump. Returns -1
// when combat ended while the player was acting.
int MpCombatClientTurnLoop();
// Host loss / disconnect: restore the UI from the disabled mirror.
void MpCombatForceExit();

// helpers used by combat.cc / game.cc / interface.cc
// True when any REMOTE player (not gDude) has the Jinxed trait or perk — the
// curse poisons everyone's misses, exactly like the vanilla dude's would.
bool MpCombatAnyPlayerHasJinxed();
// Returns the player netId if the critter is a REMOTE player's critter on the
// host (0 otherwise). The host's round loop uses this to hand the turn over.
uint8_t MpCombatGetCritterPlayerNetId(Object* critter);
// Network pump + deferred-queue drain + state broadcasts. Safe inside the
// blocking combat loops; must NEVER run scripts/map transitions.
void MpCombatPump();

} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_COMBAT_H_ */
