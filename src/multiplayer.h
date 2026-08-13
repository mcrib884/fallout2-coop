#ifndef FALLOUT_MULTIPLAYER_H_
#define FALLOUT_MULTIPLAYER_H_

#include <stdint.h>
#include <stdbool.h>

#include "map.h"
#include "net.h"
#include "obj_types.h"

namespace fallout {

typedef enum MultiplayerState {
    MP_STATE_NONE = 0,
    MP_STATE_HOST_LOBBY = 1,
    MP_STATE_HOST_PLAYING = 2,
    MP_STATE_CLIENT_CONNECTING = 3,
    MP_STATE_CLIENT_SYNCING = 4,
    MP_STATE_CLIENT_PLAYING = 5,
} MultiplayerState;

#define MP_OBJ_ID_TABLE_SIZE 65536
#define MP_NETID_TO_OBJ_INITIAL_CAPACITY 4096
#define MP_OBJ_NETID_BASE 1000
// Full-sync object chunks must fit NET_MAX_PACKET_SIZE. Derived at compile
// time from the actual payload sizes so a struct growth can never silently
// break the sync again: the old fixed 60 objects/packet exceeded the 4096
// byte limit once NetMapFullSyncObjectPayload grew (script index), and the
// sender's size guard dropped the ENTIRE full sync, leaving the client
// black-screened in CLIENT_SYNCING forever.
#define MP_FULL_SYNC_MAX_OBJECTS_PER_PACKET \
    ((NET_MAX_PACKET_SIZE - (int)sizeof(NetPacketHeader) - (int)sizeof(NetMapFullSyncChunkHeader)) / (int)sizeof(NetMapFullSyncObjectPayload))
#define MP_MAP_TILE_VALUES_PER_PACKET 1000
// Max time a client may sit in CLIENT_SYNCING (initial join or map change)
// before giving up. Custom-model uploads can take a while on slow links.
#define MP_SYNC_TIMEOUT_MS 60000
// How often to force a full tile-window refresh while co-op is active, so
// remote critters appear even when nothing dirties their tiles.
#define MP_TILE_REFRESH_INTERVAL_MS 500
// Client-side host-death detection: the host broadcasts every tick in steady
// state, so this gap (12s — far longer than any legitimate silence) means the
// host process died. ENet's own peer timeout is ~30s.
#define MP_HOST_DEAD_TIMEOUT_MS 12000

typedef struct MultiplayerPlayer {
    uint8_t netId;
    char name[NET_PEER_NAME_LENGTH];
    Object* obj;            // host: the critter; client: NULL (use netIdToObj)
    uint32_t objNetId;      // network-stable object id of this player's critter
    ENetPeer* peer;         // host: the peer; client: NULL for self
    bool isLocal;
    bool isConnected;
    bool isHandshaken;
    bool profileReady;
    uint32_t profileGeneration;
    // host: highest profile generation already broadcast for this player
    // (echoes of the owner's own uploads and detect-path broadcasts both
    // advance it; a broadcast is only sent when the generation is newer).
    uint32_t lastProfileBroadcastGeneration;
    bool hasInitialState;
    // delta-sync bookkeeping (host)
    int32_t lastTile, lastX, lastY, lastRotation, lastFid, lastFrame, lastElevation;
    int32_t lastHp, lastAp, lastRadiation, lastPoison, lastCombatResults;
    uint8_t lastFlags;
    bool hasLastState;
    // Host-only: the tile the avatar's current registered walk is heading to
    // and whether a walk is in flight. When the walk ends and the avatar is
    // NOT on this tile, the walk was interrupted (trap plate, script stop)
    // and the owning client must be told to stop its optimistic walk.
    // memset-safe: zeroed = no walk in flight (tile 0 IS a valid tile, so the
    // bool is the guard, not the sentinel).
    bool walkInFlight;
    int32_t walkTargetTile;
    int32_t lastSafeTile, lastSafeElevation, lastSafeRotation;
    bool hasSafePosition;
    uint32_t debugCheatFlags;
    // Downed state (co-op: players don't die — they get downed and revive
    // when combat ends). downedOrigFid is the standing fid restored on
    // revive. Valid on the host for every player and on the client for the
    // local player only.
    bool downed;
    int32_t downedOrigFid;
} MultiplayerPlayer;

typedef struct MultiplayerSession {
    MultiplayerState state;
    ENetHost* enetHost;
    ENetPeer* hostPeer;          // client only

    uint8_t localNetId;
    MultiplayerPlayer players[NET_MAX_PLAYERS];
    int numPlayers;
    int32_t currentMapId;
    // Session player cap: the host's configured max (NetHostCreate peer
    // limit), shipped to clients in the WELCOME so both sides render
    // "Players: N/X". 0 before a session.
    uint16_t maxPlayers;

    // host: obj -> objNetId  (per-object map in multiplayer.cc, immune to
    // duplicate obj ids; cannot live here because mpZeroSession() memsets
    // this struct)
    uint32_t nextObjNetId;

    // client: objNetId -> Object* (dynamic, grows from MP_NETID_TO_OBJ_INITIAL_CAPACITY)
    Object** netIdToObj;
    int netIdToObjCapacity;
    int netIdToObjCount;

    bool initiatorFrozen;        // local input frozen during own vote
    bool clientDisconnectPending;
    bool clientDisconnectNotifyPeer;
    bool applyingNetworkState;
    uint32_t nextFullSyncId;
    uint32_t clientSyncId;
    uint16_t clientSyncExpectedChunks;
    uint16_t clientSyncNextChunk;
    uint32_t nextTileSyncId;
    uint32_t clientTileSyncId;
    uint16_t clientTileSyncExpectedChunks;
    uint16_t clientTileSyncNextChunk;
    NetMapSyncPayload clientMapMetadata;
    bool clientMapMetadataValid;
    uint32_t clientSyncStartTick;    // getTicks() when CLIENT_SYNCING was entered
    uint32_t lastHostPacketTime;     // getTicks() of the last host packet (client)
} MultiplayerSession;

extern MultiplayerSession gMpSession;
extern bool gMpIsHost;
extern bool gMpIsClient;
extern bool gMpActive;
// The save slot a co-op session was built from; every in-session save (host
// and client alike) silently lands there: the player's own SP slot when they
// loaded a save, the next empty slot for new-game sessions, the hidden co-op
// slot for in-game joins. Set at session start, -1 before.
extern int gMpSessionSlot;
// Set while the synchronized dialogue modal is open on a client: MpTick
// skips the deferred-packet drain so session-changing packets never apply
// under the modal (the host's DIALOG_END closes the modal first).
extern bool gMpModalActive;
// While set, the object system's exit-grid hook must not fire. Used around
// map-change respawns, where players are (re)placed on the new map and a
// spawn tile that happens to be an exit grid would re-trigger a vote.
extern bool gMpSuppressExitGridCheck;
extern int gMpPendingHostStartAfterLoad;
extern int gMpPendingClientStartAfterLoad;
extern char gMpPendingClientAddress[64];
// Join options captured by the join flow: the target port and the session
// password (the HELLO carries NetPasswordHash of it). Empty password = none.
extern int gMpPendingClientPort;
extern char gMpPendingClientPassword[64];
// Host options set by the F11 CO-OP SETTINGS menu before hosting (defaults:
// port 7777, cap NET_MAX_PLAYERS, no password). MpHostStart consumes them.
extern uint16_t gMpHostPort;
extern int gMpHostMaxPlayers;
extern uint32_t gMpHostPasswordHash;

// lifecycle
int MpInit();
void MpShutdown();
void MpReset();

// host
int MpHostStart(int32_t mapId);
int MpHostCurrentGame();
int MpHostStop();

// client
int MpClientConnect(const char* address, uint16_t port, const char* password = nullptr);
int MpClientDisconnect();

// per-tick
void MpTick();
// Network pump (ENet service + packet dispatch) for the blocking combat loops.
void MpPumpNetwork();

// A join-time rejection (wrong password, server full, version mismatch) sets a
// notice that the main menu shows for a few seconds; the menu loop takes it
// and clears it. The KICK arrives while the game is loaded, so win_timed_msg
// alone would die with the game unload before the menu appears.
const char* MpTakeKickNotice();

// input (client -> host)
void MpSendPlayerAction(uint8_t action, uint32_t targetNetId, int32_t tile, int32_t elevation, uint8_t skill = 0xFF);
// Teleport the local critter to the given player's critter (client -> host).
void MpSendTeleportTo(uint8_t targetNetId);
// Host-side teleport: move the critter of [requesterNetId] onto the critter
// of [targetNetId] and broadcast. Used by the F11 player list (host clicks
// its own entry) and by the NET_PKT_TELEPORT_TO handler.
void MpHostTeleportPlayer(uint8_t requesterNetId, uint8_t targetNetId);
// Per-player addictions: chem use sets the local gvar (shared quest relay is
// skipped for these) — a client reports its own write to the host, which
// keeps a per-player overlay; dialogue scripts for a client read the overlay
// through the initiator-aware accessors instead of the shared gvars.
void MpSendAddictionChange(int32_t gvar, int32_t value);
void MpSendAddictionSnapshot();
bool MpAddictionIsTracked(int32_t gvar);
int32_t MpAddictionGetForDialogue(int32_t gvar);
void MpAddictionSetForDialogue(int32_t gvar, int32_t value);
// True while the host is inside a remote player's action handler (see
// multiplayer.cc gMpRemoteActionNetId). Lets feedback relay and the elevator
// guard attribute script-side effects to a remote player's action.
bool MpRemoteActionActive();
// The netId of the remote player whose action is currently executing on the
// host (0 when none). The deferred walk/animation callbacks re-open the
// window via MpSetRemoteActionNetId when the script runs after the accepting
// packet handler exited.
uint32_t MpRemoteActionNetId();
void MpSetRemoteActionNetId(uint32_t netId);
// Per-frame edge indicators pointing at off-screen remote players. Called
// from the main loop after MpTick, before renderPresent.
void MpDrawPlayerIndicators();
void MpRenderPlayerLabels();

// Collects the objects (local dude + every connected remote avatar) at whose
// positions the roof/wall transparency circle must be punched. Single-player
// returns just the local dude. Fills outAnchors up to maxAnchors.
int MpGetCircleAnchors(Object** outAnchors, int maxAnchors);
// Returns the roof-transparency circle bounds for a connected player avatar.
// The bounds are larger than the critter sprite and must be included in
// movement invalidation when the avatar moves.
bool MpGetPlayerCircleRect(const Object* obj, Rect* rect);

// True for any critter that belongs to a connected player: the local dude or
// any remote avatar (both the host's avatar on a client and the clients'
// avatars on the host). Used to render the constant player overlay.
bool MpIsPlayerObject(const Object* obj);

// broadcast (host -> clients)
void MpBroadcastPlayerStates();
void MpBroadcastObjectStates();
// Called by MpCombatPump after it runs the host state broadcasts. MpTick
// checks this so a main-loop frame never sweeps the object state twice
// (inputGetInput -> MpCombatPump and then MpTick would otherwise both
// broadcast; the meter showed ~30ms each, i.e. the frame was ~60ms of
// duplicate work at ~15fps).
void MpNoteHostBroadcastTick();
void MpBroadcastMapFullSync(ENetPeer* toPeer /* or NULL = all */);
void MpBroadcastMapChanged(int32_t mapId);
void MpBroadcastMapChangeAbort();
void MpResetObjectSyncBaseline();
void MpPrepareForMapChange();
void MpFinishHostMapChange();

// apply (client)
void MpApplyPlayerState(const NetPlayerStateUpdatePayload* payload);
// A passed vote resolved a transition to another tile/elevation of the
// CURRENT map: snap the local dude and switch the shared elevation (no map
// reload). See multiplayer.cc mpApplySessionElevationChange.
void MpOnMapElevationChange(const NetMapElevationPayload* payload);
// A script removed an item from a player's inventory on the host (e.g. the
// temple warrior strip): drop the matching item from the local dude inventory.
// (client)
void MpOnItemRemove(const NetItemRemovePayload* payload);
// The host's authoritative game clock arrived: adopt it so time-gated
// scripts (cutscenes, quest gates, timed encounters) evaluate against a
// live value. (client)
void MpOnGameTime(const NetGameTimePayload* payload);
// A script removed an item from a player avatar's inventory on the host:
// relay the removal to the owning client so its LOCAL inventory matches.
// (host; called from the script removal opcodes)
void MpHostMirrorItemRemoval(Object* avatar, Object* item, int quantity);
// A script MOVED a player avatar's inventory into a container on the host
// (e.g. the temple warrior strip into the trunk via move_obj_inven_to_obj):
// mirror the same strip onto every other player avatar and relay the
// removals to each owning client. (host; called from opMoveObjectInventoryToObject)
void MpHostMirrorInventoryMove(Object* sourceAvatar, Object* destContainer,
    const int* pids, const int* qtys, int count);
// The client's current map-entrance snapshot (natural entering position from
// the map file, captured before the co-op metadata overwrites the header).
// Used by the save redirect so client saves never carry a session position.
void MpGetClientMapEnteringPosition(int* tile, int* elevation, int* rotation);
// Snap the local dude to an authoritative tile (combat move resolutions that
// diverge from the optimistic walk). Stops the running walk first.
void MpApplyLocalDudeSnap(int tile, int elevation);
// The host stopped the avatar's walk before it reached the clicked tile
// (pressure plate, script interrupt, knockback): stop the local optimistic
// walk and snap to the authoritative position. (client)
void MpOnWalkInterrupted(const NetWalkInterruptedPayload* payload);
// Derive the weapon slot of a critter's FID from the weapon in its hands
// (left preferred, right fallback). Run after any co-op inventory apply or
// savegame load — the vanilla inven_wield path never runs for synced gear.
void MpSyncCritterWeaponFid(Object* critter);
// Make the next periodic profile sync run immediately instead of waiting for
// its cadence (used by the skin picker so a model change propagates at once).
void MpProfileForceSync();
void MpApplyMapFullSync(const void* data, size_t dataLength);
void MpApplyMapTileSync(const void* data, size_t dataLength);
void MpApplyMapChanged(const NetMapSyncPayload* payload);
void MpApplyPlayerJoined(const NetPlayerJoinedPayload* payload);
void MpApplyPlayerLeft(const NetPlayerLeftPayload* payload);
void MpApplyObjectState(const NetMapFullSyncObjectPayload* payload);
// Co-op: immediately re-broadcast one object's authoritative state to every
// client (used by host-side single-field flips like script set_team).
void MpBroadcastObjectStateFor(Object* obj);
void MpApplyObjectRemoved(uint32_t netId);

// netId mapping
uint32_t MpAllocObjNetId();
void MpRegisterObjNetId(Object* obj, uint32_t netId);
void MpAssignNetIdsToAllObjects();
Object* MpFindObjByNetId(uint32_t netId);
uint32_t MpGetObjNetId(Object* obj);
bool MpIsNetworkedCritter(Object* obj);
void MpClearNetIdMappings();

// Called-shot probabilities (co-op): the host owns every combat roll, so a
// client's aiming window asks the host for the numbers instead of computing
// them with local settings/script hooks.
void MpToHitQuery(Object* target, int hitMode);
// Client modal loop: consume the latest host reply (returns false when none
// is pending). The caller must match targetNetId/hitMode against its window.
bool MpToHitResultTake(uint32_t* targetNetId, uint8_t* hitMode, int probs[8]);

// vote hook (called from map.cc)
int MpOnMapTransitionRequested(MapTransition* transition);
int MpOnNetworkedPlayerTransitionRequested(Object* obj, MapTransition* transition);

// downed state (co-op: players are downed instead of killed)
bool MpIsCoopPlayerCritter(const Object* critter);
bool MpPlayerIsDownedByNetId(uint8_t netId);
// Convert a would-be player death into the downed state (called from
// critterKill on both sides; the host decides the game-over check).
void MpPlayerDown(Object* critter);
// Host: revive every downed player with 5% of max HP (combat ended).
void MpCombatEndReviveDowned();
// Co-op combat outcome sync: host reads the last resolved attack, client
// replays the authoritative result's feedback (flinch, pain sound, blood).
void MpGetLastAttackResult(int* outDamage, int* outAttackerFlags, int* outDefenderFlags);
Object* MpGetLastAttackWeapon();
// Co-op: swap the script combat's start-data override out for the duration
// of a remote attack resolve (its min/max clamp would zero remote damage).
CombatStartData* MpCombatSwapStartData(CombatStartData* value);
void MpReplayLocalAttackResult(int damage, int attackerFlags, int defenderFlags);
// apply (client)
void MpApplyPlayerStatus(const NetPlayerStatusPayload* payload);
void MpApplyGameOver(const NetGameOverPayload* payload);
// Host: route a skill-use feedback (monitor message + optional time-skip
// fade) to the performing player's client. netId 0 = no-op.
void MpSendSkillUseFeedback(uint8_t netId, int messageId, int arg2, int arg3, int fade);

// Co-op: if the destination tile holds a live critter (not the mover),
// truncate the destination to the last free tile on the straight line
// toward it. Vanilla allows sprite stacking, and a stacked avatar breaks
// targeting for both players — player walks must never resolve onto an
// occupied tile. Returns the (possibly unchanged) destination.
int MpTruncateDestinationAtOccupant(Object* mover, int tile, int elevation);

} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_H_ */
