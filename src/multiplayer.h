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
#define MP_FULL_SYNC_MAX_OBJECTS_PER_PACKET 60
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
    bool hasLastState;
    int32_t lastSafeTile, lastSafeElevation, lastSafeRotation;
    bool hasSafePosition;
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

// lifecycle
int MpInit();
void MpShutdown();
void MpReset();

// host
int MpHostStart(int32_t mapId);
int MpHostCurrentGame();
int MpHostStop();

// client
int MpClientConnect(const char* address, uint16_t port);
int MpClientDisconnect();

// per-tick
void MpTick();
// Network pump (ENet service + packet dispatch) for the blocking combat loops.
void MpPumpNetwork();

// input (client -> host)
void MpSendPlayerAction(uint8_t action, uint32_t targetNetId, int32_t tile, int32_t elevation, uint8_t skill = 0xFF);

// broadcast (host -> clients)
void MpBroadcastPlayerStates();
void MpBroadcastObjectStates();
void MpBroadcastMapFullSync(ENetPeer* toPeer /* or NULL = all */);
void MpBroadcastMapChanged(int32_t mapId);
void MpBroadcastMapChangeAbort();
void MpResetObjectSyncBaseline();
void MpPrepareForMapChange();
void MpFinishHostMapChange();

// apply (client)
void MpApplyPlayerState(const NetPlayerStateUpdatePayload* payload);
// Snap the local dude to an authoritative tile (combat move resolutions that
// diverge from the optimistic walk). Stops the running walk first.
void MpApplyLocalDudeSnap(int tile, int elevation);
void MpApplyMapFullSync(const void* data, size_t dataLength);
void MpApplyMapTileSync(const void* data, size_t dataLength);
void MpApplyMapChanged(const NetMapSyncPayload* payload);
void MpApplyPlayerJoined(const NetPlayerJoinedPayload* payload);
void MpApplyPlayerLeft(const NetPlayerLeftPayload* payload);
void MpApplyObjectState(const NetMapFullSyncObjectPayload* payload);
void MpApplyObjectRemoved(uint32_t netId);

// netId mapping
uint32_t MpAllocObjNetId();
void MpRegisterObjNetId(Object* obj, uint32_t netId);
void MpAssignNetIdsToAllObjects();
Object* MpFindObjByNetId(uint32_t netId);
uint32_t MpGetObjNetId(Object* obj);
bool MpIsNetworkedCritter(Object* obj);
void MpClearNetIdMappings();

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
