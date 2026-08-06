#ifndef FALLOUT_NET_H_
#define FALLOUT_NET_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Forward-declare ENet opaque types at global scope so we don't have to
// include <enet/enet.h> from this public header. ENet itself declares
// these structs at global scope, so this is consistent.
struct _ENetHost;
struct _ENetPeer;

typedef struct _ENetHost ENetHost;
typedef struct _ENetPeer ENetPeer;

namespace fallout {

#define NET_MAX_PLAYERS 90
#define NET_DEFAULT_PORT 7777
#define NET_MAX_PACKET_SIZE 4096
#define NET_PEER_NAME_LENGTH 32
#define NET_MAP_NAME_LENGTH 16
#define NET_NUM_CHANNELS 2
#define NET_CHANNEL_RELIABLE 0
#define NET_CHANNEL_UNRELIABLE 1

enum NetPacketType {
    NET_PKT_HELLO = 1,
    NET_PKT_WELCOME = 2,
    NET_PKT_MAP_FULL_SYNC = 3,
    NET_PKT_PLAYER_INPUT = 4,
    NET_PKT_VOTE_START = 5,
    NET_PKT_VOTE_CAST = 6,
    NET_PKT_VOTE_RESULT = 7,
    NET_PKT_MAP_CHANGED = 8,
    NET_PKT_DISCONNECT = 9,
    NET_PKT_KICK = 10,
    NET_PKT_VOTE_START_REQUEST = 11,
    NET_PKT_PLAYER_JOINED = 12,
    NET_PKT_PLAYER_LEFT = 13,
    NET_PKT_OBJECT_REMOVED = 14,
    NET_PKT_MAP_TILE_SYNC = 15,
    NET_PKT_PLAYER_ACTION = 16,
    NET_PKT_PLAYER_PROFILE_BEGIN = 17,
    NET_PKT_PLAYER_PROFILE_CHUNK = 18,
    NET_PKT_PLAYER_PROFILE_END = 19,
    NET_PKT_PLAYER_PROFILE_ACK = 20,
    NET_PKT_PLAYER_PROFILE_REJECT = 21,
    NET_PKT_MAP_CHANGE_ABORT = 22,
    NET_PKT_VOTE_TALLY = 23,
    NET_PKT_COMBAT_START_REQUEST = 24,
    NET_PKT_COMBAT_STARTED = 25,
    NET_PKT_COMBAT_TURN_START = 26,
    NET_PKT_COMBAT_CMD = 27,
    NET_PKT_COMBAT_TURN_END = 28,
    NET_PKT_COMBAT_END_REQUEST = 29,
    NET_PKT_COMBAT_END_DENIED = 30,
    NET_PKT_COMBAT_ENDED = 31,
    NET_PKT_COMBAT_MESSAGE = 32,
    NET_PKT_PLAYER_STATUS = 33,   // host -> clients: player downed/revived
    NET_PKT_GAME_OVER = 34,       // host -> clients: all players downed
};

enum NetUnreliablePacketType {
    NET_PKT_PLAYER_STATE_UPDATE = 100,
    NET_PKT_OBJECT_STATE_UPDATE = 101,
};

// Combat sync — the client sends intents, the host resolves them against its
// authoritative copies and broadcasts outcomes. TURN_START carries the
// canonical AP the host granted so no client ever re-derives it locally.
typedef struct NetCombatTurnStartPayload {
    uint8_t netId; // player whose turn it is; 0 = no player (NPC turn)
    uint16_t ap;
    uint16_t maxAp;
    // Acting critter's object netId. Player turns (netId != 0) leave this 0;
    // NPC turns (netId == 0) carry the acting NPC so the client's mirror can
    // draw the vanilla acting-critter red outline.
    uint32_t targetNetId;
} NetCombatTurnStartPayload;

// Client-initiated combat: the client attacked an enemy outside combat, so
// the host starts a combat sequence that mirrors a vanilla scripted ambush
// (attacker first, defender second). targetNetId is the attacked object.
typedef struct NetCombatStartRequestPayload {
    uint32_t targetNetId; // 0 = no specific target
} NetCombatStartRequestPayload;

enum NetCombatCmdType {
    NET_COMBAT_CMD_MOVE = 1,
    NET_COMBAT_CMD_ATTACK = 2,
    // Client->host: the acting client spent AP on inventory (open cost,
    // item use). The host deducts it from its authoritative copy so the
    // next state sync confirms it instead of refunding the client.
    NET_COMBAT_CMD_INV_AP_COST = 3,
};

typedef struct NetCombatCmdPayload {
    uint8_t cmd;
    uint8_t reserved;     // bit0 = run (MOVE)
    uint16_t hitMode;     // ATTACK: hit mode
    uint16_t hitLocation; // ATTACK: hit location
    uint32_t targetNetId; // attack target (object netId on the host)
    int32_t tile;         // move destination
    int32_t elevation;
} NetCombatCmdPayload;

// Host -> clients: a player's downed state changed (co-op players are downed
// instead of killed; they revive when combat ends). Reliable. The visual
// (lying fid, hp, combat results) also flows through the player-state
// broadcast; this packet is the authoritative marker + canonical HP so the
// client can gate the vanilla death scene and render the body even if a
// state packet is lost.
typedef struct NetPlayerStatusPayload {
    uint8_t netId;  // the player whose state changed
    uint8_t downed; // 1 = downed, 0 = revived
    int32_t hp;     // canonical HP after the change (0 when downed, 5% of max when revived)
} NetPlayerStatusPayload;

// Host -> clients: every connected player is downed — the game is over.
// Both sides return to the main menu through the normal quit path.
typedef struct NetGameOverPayload {
    uint8_t reason; // reserved for future reasons
} NetGameOverPayload;

#pragma pack(push, 1)
typedef struct NetPacketHeader {
    uint8_t type;
    uint16_t length;
} NetPacketHeader;

typedef struct NetHelloPayload {
    uint32_t versionHash;
    char peerName[NET_PEER_NAME_LENGTH];
} NetHelloPayload;

typedef struct NetMapSyncPayload {
    int32_t mapId;
    char mapName[NET_MAP_NAME_LENGTH];
    int32_t enteringTile;
    int32_t enteringElevation;
    int32_t enteringRotation;
    int32_t centerTile;
    int32_t elevation;
    int32_t flags;
    int32_t darkness;
    int32_t ambientIntensity;
} NetMapSyncPayload;

typedef struct NetWelcomePayload {
    uint8_t assignedNetId;
    uint32_t objNetId;
    NetMapSyncPayload map;
} NetWelcomePayload;

typedef struct NetPlayerInputPayload {
    int32_t tile;
    int32_t elevation;
    uint8_t isRun;
} NetPlayerInputPayload;

enum NetPlayerActionType {
    NET_PLAYER_ACTION_WALK = 1,
    NET_PLAYER_ACTION_RUN = 2,
    NET_PLAYER_ACTION_INSPECT = 3,
    NET_PLAYER_ACTION_TALK = 4,
    NET_PLAYER_ACTION_TOUCH = 5,
    NET_PLAYER_ACTION_PICK_UP = 6,
    NET_PLAYER_ACTION_LOOT = 7,
    NET_PLAYER_ACTION_USE_SKILL = 8,
    NET_PLAYER_ACTION_PUSH = 9,
    NET_PLAYER_ACTION_ROTATE = 10,
    // Client drops an item from its own inventory. targetNetId carries the
    // item's pid; the host pulls the matching item from the avatar's
    // inventory and drops it on the ground (the ground object then streams
    // back through the regular object sync).
    NET_PLAYER_ACTION_DROP = 11,
};

typedef struct NetPlayerActionPayload {
    uint8_t action;
    uint8_t skill;
    uint16_t reserved;
    uint32_t targetNetId;
    int32_t tile;
    int32_t elevation;
} NetPlayerActionPayload;

typedef struct NetVoteStartRequestPayload {
    int32_t targetMap;
    int32_t targetTile;
    int32_t targetElevation;
    int32_t targetRotation;
} NetVoteStartRequestPayload;

typedef struct NetVoteStartPayload {
    uint8_t initiatorNetId;
    uint8_t totalPlayers;
    int32_t targetMap;
    int32_t targetTile;
    int32_t targetElevation;
    int32_t targetRotation;
    uint32_t timeoutMs;
} NetVoteStartPayload;

typedef struct NetVoteCastPayload {
    uint8_t voterNetId;
    uint8_t vote;
} NetVoteCastPayload;

typedef struct NetVoteTallyPayload {
    uint8_t yesCount;
    uint8_t noCount;
    uint8_t totalCount;
} NetVoteTallyPayload;

typedef struct NetVoteResultPayload {
    uint8_t passed;
    uint8_t yesCount;
    uint8_t totalCount;
} NetVoteResultPayload;

typedef struct NetMapChangedPayload {
    NetMapSyncPayload map;
} NetMapChangedPayload;

typedef struct NetPlayerJoinedPayload {
    uint8_t netId;
    uint32_t objNetId;
    char name[NET_PEER_NAME_LENGTH];
} NetPlayerJoinedPayload;

typedef struct NetPlayerLeftPayload {
    uint8_t netId;
    uint32_t objNetId;
} NetPlayerLeftPayload;

typedef struct NetPlayerProfileBeginPayload {
    uint32_t streamId;
    uint8_t netId;
    uint8_t reserved0;
    uint16_t schemaVersion;
    uint32_t generation;
    uint32_t objNetId;
    uint32_t totalBytes;
    uint32_t chunkCount;
    uint32_t contentHash;
} NetPlayerProfileBeginPayload;

typedef struct NetPlayerProfileChunkHeader {
    uint32_t streamId;
    uint8_t netId;
    uint8_t reserved0;
    uint16_t chunkIndex;
    uint16_t chunkCount;
    uint16_t dataLength;
} NetPlayerProfileChunkHeader;

typedef struct NetPlayerProfileEndPayload {
    uint32_t streamId;
    uint8_t netId;
    uint8_t accepted;
    uint16_t reserved0;
    uint32_t generation;
    uint32_t contentHash;
} NetPlayerProfileEndPayload;

typedef struct NetPlayerProfileAckPayload {
    uint32_t streamId;
    uint8_t netId;
    uint8_t accepted;
    uint16_t reason;
    uint32_t generation;
} NetPlayerProfileAckPayload;

typedef struct NetMapFullSyncChunkHeader {
    uint32_t syncId;
    uint16_t chunkIndex;
    uint16_t chunkCount;
    int32_t objectCount;
} NetMapFullSyncChunkHeader;

typedef struct NetMapTileSyncChunkHeader {
    uint32_t syncId;
    int32_t mapId;
    uint16_t chunkIndex;
    uint16_t chunkCount;
    uint8_t elevation;
    uint16_t tileOffset;
    uint16_t tileCount;
} NetMapTileSyncChunkHeader;

typedef struct NetPlayerStateUpdatePayload {
    uint8_t netId;
    uint32_t objNetId;
    int32_t tile;
    int32_t x, y;
    int32_t rotation;
    int32_t fid;
    int32_t frame;
    int32_t elevation;
    int32_t hp;
    int32_t ap;
    int32_t radiation;
    int32_t poison;
    int32_t combatResults;
} NetPlayerStateUpdatePayload;

typedef struct NetMapFullSyncObjectPayload {
    uint32_t netId;
    int32_t pid;
    int32_t tile;
    int32_t fid;
    int32_t frame;
    int32_t x, y;
    int32_t rotation;
    int32_t elevation;
    int32_t flags;
    int32_t hp;
    int32_t ap;
    int32_t radiation;
    int32_t poison;
    int32_t combatTeam;
    int32_t combatManeuver;
    int32_t combatResults;
} NetMapFullSyncObjectPayload;
#pragma pack(pop)

// --- Transport API ---

bool NetInit();
void NetShutdown();

ENetHost* NetHostCreate(uint16_t port, int maxPeers);
ENetHost* NetClientCreate();
ENetPeer* NetClientConnect(ENetHost* client, const char* address, uint16_t port);
void NetPeerDisconnect(ENetPeer* peer);
void NetHostDestroy(ENetHost* host);

// data must point to a NetPacketHeader followed by the payload.
bool NetPeerSend(ENetPeer* peer, int channel, const void* data, size_t dataLength);
bool NetHostBroadcast(ENetHost* host, int channel, const void* data, size_t dataLength);

// eventType: 1 = connect, 2 = disconnect, 3 = receive.
// For receive, data/dataLength describe the raw packet (header + payload).
typedef void (*NetEventCallback)(ENetPeer* peer, int eventType, const void* data, size_t dataLength, void* userData);
void NetHostService(ENetHost* host, NetEventCallback callback, void* userData);

uint32_t NetGetVersionHash();

// Helper that appends a NetPacketHeader in front of a payload and sends it
// on the given peer/channel. payload may be NULL if payloadLength == 0.
bool NetSendPacket(ENetPeer* peer, int channel, uint8_t packetType, const void* payload, size_t payloadLength);
bool NetBroadcastPacket(ENetHost* host, int channel, uint8_t packetType, const void* payload, size_t payloadLength);

} // namespace fallout

#endif /* FALLOUT_NET_H_ */
