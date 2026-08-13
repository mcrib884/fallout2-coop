#ifndef FALLOUT_NET_H_
#define FALLOUT_NET_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "game_movie.h"

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
    NET_PKT_PLAYER_STATUS = 33,   // DEPRECATED -> NET_PKT_PLAYER_EVENT (DOWNED/REVIVED)
    NET_PKT_GAME_OVER = 34,       // DEPRECATED -> NET_PKT_PLAYER_EVENT (GAME_OVER)
    NET_PKT_DEBUG_CMD = 35,       // DEPRECATED -> NET_PKT_PLAYER_CMD (HEAL)
    NET_PKT_COMBAT_ATTACK_RESULT = 36, // DEPRECATED -> NET_PKT_PLAYER_EVENT (ATTACK_RESULT)
    NET_PKT_PLAYER_CMD = 37,      // client -> host: generic player command (runtime fields)
    NET_PKT_PLAYER_EVENT = 38,    // host -> clients: generic player event (reliable)
    NET_PKT_MODEL_REQUEST = 39,   // receiver -> sender: profile model payload missing
    // --- Synchronized dialogue (host-authoritative) ---
    NET_PKT_DIALOG_BEGIN = 40,    // host -> all clients: dialogue session opened
    NET_PKT_DIALOG_STATE = 41,    // host -> participants: current node (chunked)
    NET_PKT_DIALOG_CHOICE = 42,   // client -> host: participant selected an option
    NET_PKT_DIALOG_VOTE = 43,     // host -> participants: full vote state
    NET_PKT_DIALOG_TRANSCRIPT = 44, // host -> all clients: combat-log line
    NET_PKT_DIALOG_JOIN = 45,     // host -> clients: participant joined
    NET_PKT_DIALOG_LEAVE = 46,    // client -> host: leave request; host -> clients: left
    NET_PKT_DIALOG_END = 47,      // host -> clients: session ended
    // --- Synchronized barter (within a dialogue session) ---
    NET_PKT_BARTER_CMD = 48,      // client -> host: barter op (start/move/end/commit)
    NET_PKT_BARTER_STATE = 49,    // host -> barterer: authoritative tables + last op result
    NET_PKT_LOOT_CMD = 50,        // client -> host: loot/steal op (move/takeall/end)
    NET_PKT_LOOT_STATE = 51,      // host -> looter: authoritative loot state (open/move/takeall/end)
    NET_PKT_COMBAT_MOVE_RESULT = 52, // host -> acting client: authoritative resolved move tile
    NET_PKT_FLOAT_MESSAGE = 53,   // host -> clients: script float_msg relay (combat chatter)
    NET_PKT_GVAR_SNAPSHOT = 54,   // host -> joining client: full gvar table (quest state)
    NET_PKT_GVAR_CHANGE = 55,     // host -> clients: one live gvar write
    NET_PKT_MAP_ELEVATION = 56,   // host -> clients: shared elevation change (same-map transition)
    NET_PKT_ITEM_REMOVE = 57,     // host -> client: script removed an item from the player's inventory
    NET_PKT_GAME_TIME = 58,       // host -> clients: authoritative game clock (client keeps a mirror)
    NET_PKT_TO_HIT_QUERY = 59,    // client -> host: called-shot probabilities for a target (host-owned rolls)
    NET_PKT_TO_HIT_RESULT = 60,   // host -> client: the 8 called-shot probabilities computed host-side
    NET_PKT_CHEAT_POLICY = 61,    // host -> clients: session cheat policy (client cheats enabled?)
    NET_PKT_CHAT_MESSAGE = 62,    // client -> host -> other clients: user chat relay
    NET_PKT_WORLDMAP_ENTER = 63,  // host -> clients: host entered the worldmap (mode: 0=world, 1=town)
    NET_PKT_WORLDMAP_STATE = 64,  // host -> clients: authoritative worldmap state (party pos, walking, car)
    NET_PKT_WORLDMAP_EXIT = 65,   // host -> clients: host left the worldmap (no map load follows)
    NET_PKT_WALK_INTERRUPTED = 66, // host -> client: the avatar's walk was stopped short of its target (trap/script interrupt)
    NET_PKT_WORLDMAP_DISCOVERY = 67, // host -> clients: discovered worldmap subtiles/areas (full snapshot or incremental)
    NET_PKT_ADDICTION_CHANGE = 68,  // client -> host: one per-player drug addiction write
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
    // FNV-1a hash of the session password (NetPasswordHash); 0 = no password.
    // The raw password never crosses the wire or lands in logs.
    uint32_t passwordHash;
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

// Host -> clients: a passed vote resolved a transition to another elevation
// of the CURRENT map (intra-map link). The map stays loaded; the session
// switches elevation together and every player's critter snaps to the
// destination tile.
typedef struct NetMapElevationPayload {
    int32_t tile;
    int32_t elevation;
    int32_t rotation;
} NetMapElevationPayload;

// Script-driven inventory removal relay (host -> owning client): a script
// removed an item from a player avatar's inventory (e.g. the temple warrior
// strip). Carries the proto pid + quantity so the client can drop the same
// item from its local dude inventory.
typedef struct NetItemRemovePayload {
int32_t pid;
int32_t quantity;
} NetItemRemovePayload;

// Authoritative game clock relay (host -> clients). The client advances its
// own mirror between syncs so time-gated scripts (cutscenes, quest gates,
// timed encounters) evaluate against a live value instead of the frozen
// join-time clock.
typedef struct NetGameTimePayload {
int32_t time;
} NetGameTimePayload;

typedef struct NetWelcomePayload {
    uint8_t assignedNetId;
    uint32_t objNetId;
    NetMapSyncPayload map;
    // Movie-seen array (gameMovieGetSeenFlags layout): scripts decide cutscenes
    // from it, so the client must start with the host's state or map-enter
    // scripts skip their movies.
    uint8_t moviesSeen[MOVIE_COUNT];
    // Host's configured session player cap (the "Players: N/X" display).
    uint16_t maxPlayers;
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
    // Client uses an inventory item on a map object (crosshair click or the
    // "use item on" inventory picker). targetNetId = the target object's
    // netId, tile = the item's pid, skill = entry mode (0 = crosshair click,
    // 1 = inventory picker; the host uses it to pick the combat AP cost).
    NET_PLAYER_ACTION_USE_ITEM_ON = 12,
    // Client arms an explosive from its own inventory (sets the fuse). The
    // arm mutated the pid locally (DYNAMITE_I -> DYNAMITE_II etc.), so the
    // host must arm its avatar's copy with the same fuse. targetNetId = the
    // item's pid (the INACTIVE one, so the host can find it in the avatar's
    // inventory), tile = the fuse seconds the client selected.
    NET_PLAYER_ACTION_USE_ITEM = 13,
    NET_PLAYER_ACTION_LOOK = 14, // hover look-at (mirrors vanilla look_at feedback)
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
    // Movie-seen array (see NetWelcomePayload.moviesSeen): re-synced on every
    // transition so host/client cutscene decisions stay identical.
    uint8_t moviesSeen[MOVIE_COUNT];
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

// A profile transfer is a concatenation of independent sections. Only the
// sections whose persistent content changed since the last transfer to this
// peer are shipped; the volatile runtime fields (transform, hp/ap, combat
// state, object flags) live in NO section — they are owned exclusively by
// the per-tick state channel, which structurally ends the "forgot to zero a
// volatile field in the change-detection hash" bug class.
typedef struct NetProfileSectionInfo {
    uint8_t id;        // ProfileSectionId
    uint8_t reserved;
    uint16_t reserved2;
    uint32_t byteSize; // model payloads can exceed 64KB
} NetProfileSectionInfo;

typedef struct NetPlayerProfileBeginPayload {
    uint32_t streamId;
    uint8_t netId;
    uint8_t modelIncluded; // 1 = the MODEL section carries the model payload
    uint16_t schemaVersion;
    uint32_t generation;
    uint32_t objNetId;
    uint32_t totalBytes;
    uint32_t chunkCount;
    uint32_t contentHash; // hash of the serialized body (exactly the shipped sections)
    uint16_t sectionCount;
    NetProfileSectionInfo sections[12];
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
    uint32_t modelHash; // model payload received with this profile (0 = none)
} NetPlayerProfileAckPayload;

// Receiver -> sender: the profile's model payload was skipped (assumed
// known) but this peer does not have it — re-send the profile WITH the model.
typedef struct NetModelRequestPayload {
    uint8_t netId;
    uint32_t modelHash;
} NetModelRequestPayload;

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
    uint8_t flags; // bit 0 = DUDE_STATE_SNEAKING (avatar proto flag)
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
    int32_t scriptIndex; // host's script list index; fixes scripted names/examine on the client
} NetMapFullSyncObjectPayload;

// Per-player debug menu commands (client -> host). The menu edits the
// player's own sheet, which rides the profile channel; only the volatile
// runtime fields (current HP) need the host's authority.
typedef struct NetDebugCmdPayload {
    uint8_t action; // NetDebugAction
    int32_t value;  // 0 = full
} NetDebugCmdPayload;

enum NetDebugAction {
    NET_DEBUG_ACTION_HEAL = 1,
};

// Generic client -> host player command. Every client-initiated runtime
// change (heal, inventory AP cost, ...) rides this single route — the host
// validates and applies it to the sender's avatar. The per-tick state
// channel then carries the result back. (DEPRECATED: NET_PKT_DEBUG_CMD)
typedef struct NetPlayerCmdPayload {
    uint8_t opcode; // NetPlayerCmdOpcode
    int32_t arg1;
    uint32_t arg2;
} NetPlayerCmdPayload;

enum NetPlayerCmdOpcode {
    NET_PLAYER_CMD_HEAL = 1,         // arg1: 0 = full, else amount
    NET_PLAYER_CMD_INVENTORY_AP = 2, // arg1: AP cost to deduct
    NET_PLAYER_CMD_AP_REFILL = 3,    // refill the avatar's action points
    NET_PLAYER_CMD_CHEAT_FLAGS = 4,  // arg1: MpDebugCheatFlags bitmask
};

// Called-shot probability query: the client's aiming window must show the
// host's numbers (the host owns every combat roll), so the client asks for
// them instead of computing with its own settings and script hooks.
typedef struct NetToHitQueryPayload {
    uint32_t targetNetId;
    uint8_t hitMode;
} NetToHitQueryPayload;

// Host reply: probs[0..3] are the left-column locations, probs[4..7] the
// right-column ones, in the same order the called-shot window draws them.
typedef struct NetToHitResultPayload {
    uint32_t targetNetId;
    uint8_t hitMode;
    int16_t probs[8];
} NetToHitResultPayload;

// Host -> clients: the session's cheat policy. When clientCheatsEnabled is 0
// only the host may use the co-op cheats; clients are told so at join and on
// every change (their menus and local effects are gated).
typedef struct NetCheatPolicyPayload {
    uint8_t clientCheatsEnabled;
} NetCheatPolicyPayload;

// Generic host -> client player event (reliable). Discrete player lifecycle
// and combat outcomes ride this single route. (DEPRECATED: NET_PKT_PLAYER_STATUS,
// NET_PKT_GAME_OVER, NET_PKT_COMBAT_ATTACK_RESULT)
typedef struct NetPlayerEventPayload {
    uint8_t opcode; // NetPlayerEventOpcode
    uint8_t netId;  // player the event concerns
    int32_t arg1;
    int32_t arg2;
    int32_t arg3;
    uint32_t arg4;
} NetPlayerEventPayload;

enum NetPlayerEventOpcode {
    NET_PLAYER_EVENT_DOWNED = 1,        // arg1: 1 downed / 0 revived; arg2: hp
    NET_PLAYER_EVENT_GAME_OVER = 2,     // arg1: reason
    NET_PLAYER_EVENT_ATTACK_RESULT = 3, // arg1: damage; arg2: attackerFlags; arg3: defenderFlags; arg4: targetNetId
    NET_PLAYER_EVENT_SKILL_USE = 4,     // arg1: skill message id; arg2: format arg (hp / limb msg id); arg3: target netId; arg4: 1 = play time-skip fade
    NET_PLAYER_EVENT_ATTACK_REJECTED = 5, // arg1: attacker authoritative tile; arg2: elevation; arg3: targetNetId; arg4: 0
};

// --- Synchronized dialogue ---

#define NET_DIALOG_MAX_OPTIONS 30
#define NET_DIALOG_REPLY_MAX 900
#define NET_DIALOG_OPTION_TEXT_MAX 384
#define NET_DIALOG_TRANSCRIPT_MAX 512
#define NET_DIALOG_TRANSCRIPT_REPLAY 64
#define NET_DIALOG_VOTE_TIMER_MS 15000
#define NET_DIALOG_STATE_CHUNK_MAX 2048
#define NET_BARTER_MAX_ITEMS 250

enum NetDialogEndReason {
    NET_DIALOG_END_NORMAL = 1,     // dialogue script ended normally (end_dialogue)
    NET_DIALOG_END_ALL_LEFT = 2,   // every participant left
    NET_DIALOG_END_COMBAT = 3,     // combat started (single-player end path)
    NET_DIALOG_END_MAP_CHANGE = 4, // map transition
    NET_DIALOG_END_NPC_DEAD = 5,   // speaker died or was killed
    NET_DIALOG_END_RESET = 6,      // no single-player equivalent — full reset
};

// Host -> all clients: a synchronized dialogue session opened. Participants
// are listed in join order (first = initiator unless scripted). For
// player-initiated dialogue only the participants (initiator + joiners) act;
// scripted conversations include every connected player.
typedef struct NetDialogBeginPayload {
    uint32_t sessionId;
    uint32_t speakerObjNetId; // NPC object netId
    uint8_t initiatorNetId;   // 0 = scripted (no initiator)
    uint8_t participantCount;
    uint8_t participants[NET_MAX_PLAYERS]; // join order
} NetDialogBeginPayload;

// Host -> participants: the current node, chunked. The body (concatenated
// across chunks) is: uint8 version; uint16 replyLen; reply[replyLen];
// uint8 optionCount; per option: int16 msgListId; int16 msgId; int8 reaction;
// uint16 textLen; text[textLen]. Text is the already-resolved display text.
typedef struct NetDialogStateChunkHeader {
    uint32_t sessionId;
    uint16_t nodeSeq;
    uint16_t chunkIndex;
    uint16_t chunkCount;
    uint16_t chunkSize;
} NetDialogStateChunkHeader;

// Client -> host: participant chose option (re-picking overwrites; 0xFF = clear).
typedef struct NetDialogChoicePayload {
    uint32_t sessionId;
    uint16_t nodeSeq;
    uint8_t netId;        // sender (validated against the peer on the host)
    uint8_t optionIndex;
} NetDialogChoicePayload;

typedef struct NetDialogVoteEntry {
    uint8_t netId;
    uint8_t optionIndex; // 0xFF = no selection yet
    uint8_t flags;       // bit0 = suspended (bartering)
} NetDialogVoteEntry;

// Host -> participants: full vote state after every change.
typedef struct NetDialogVotePayload {
    uint32_t sessionId;
    uint16_t nodeSeq;
    uint8_t initiatorNetId;
    int8_t resolvedOption; // -1 = none
    uint8_t timerActive;   // 1 = majority timer running
    uint16_t timerMs;      // remaining (0 when inactive)
    uint8_t participantCount;
    // followed by participantCount NetDialogVoteEntry entries
} NetDialogVotePayload;

// Host -> all clients: one transcript line for the display/combat monitor.
// speakerNetId 0 = NPC (host avatar); otherwise the speaking player. The
// text is the fully formatted line ("Aradesh: ..." / "A + B: ...").
typedef struct NetDialogTranscriptPayload {
    uint32_t sessionId;
    uint16_t seq;
    uint8_t speakerNetId; // 0 = NPC
    char text[NET_DIALOG_TRANSCRIPT_MAX];
} NetDialogTranscriptPayload;

// Host -> clients: participant joined an active session.
typedef struct NetDialogJoinPayload {
    uint32_t sessionId;
    uint8_t netId;
} NetDialogJoinPayload;

// Client -> host: leave request. Host -> clients: participant left.
typedef struct NetDialogLeavePayload {
    uint32_t sessionId;
    uint8_t netId;
    uint8_t reason; // 1 = player left (ESC), 2 = disconnected
} NetDialogLeavePayload;

// Host -> clients: session ended.
typedef struct NetDialogEndPayload {
    uint32_t sessionId;
    uint8_t reason; // NetDialogEndReason
} NetDialogEndPayload;

// --- Synchronized barter (within a dialogue session) ---

enum NetBarterOpcode {
    NET_BARTER_OP_START = 1,  // client -> host: open barter
    NET_BARTER_OP_MOVE = 2,   // client -> host: move item (qty signed; + into table, - back)
    NET_BARTER_OP_END = 3,    // client -> host: close barter
    NET_BARTER_OP_COMMIT = 4, // client -> host: attempt transaction
};

typedef struct NetBarterItem {
    uint32_t pid;
    int32_t qty;
    int32_t unitValue;
} NetBarterItem;

// Client -> host: barter operation.
typedef struct NetBarterCmdPayload {
    uint32_t sessionId;
    uint8_t op;       // NetBarterOpcode
    uint8_t netId;    // sender
    uint8_t target;   // MOVE: 1 = offer table, 2 = request table
    uint32_t pid;
    int32_t qty;      // signed
} NetBarterCmdPayload;

// Host -> barterer: authoritative tables + the result of the last op.
// Followed by npcItemCount + offerCount + requestCount NetBarterItem entries.
typedef struct NetBarterStatePayload {
    uint32_t sessionId;
    uint8_t netId;
    uint8_t lastOp;    // echoed NetBarterOpcode (0 = none)
    uint8_t lastOk;    // 1 = last op applied
    uint8_t lastMsgId; // vanilla barter message id (27/28/31/32/...), 0 = none
    int16_t barterMod; // host-computed barter modifier (incl. reaction)
    uint16_t npcItemCount;
    uint16_t offerCount;   // this barterer's offer as the host sees it
    uint16_t requestCount; // this barterer's request as the host sees it
} NetBarterStatePayload;

// --- Synchronized loot/steal (outside dialogue) ---
//
// The vanilla loot window on the client runs against a local mirror of the
// target (fid/pid/items supplied by the host). Every move is a command to the
// host, which applies it to the real objects (with the steal roll for steal
// sessions) and echoes the authoritative state back: the target's item list,
// the delta applied to the looter's own inventory, and any feedback message.

enum NetLootOpcode {
    NET_LOOT_OP_OPEN = 1,     // host -> client: open the loot/steal window
    NET_LOOT_OP_MOVE = 2,     // client -> host: move item (qty signed; + take, - plant)
    NET_LOOT_OP_TAKE_ALL = 3, // client -> host: take everything (weight-checked)
    NET_LOOT_OP_END = 4,      // both ways: session over
};

typedef struct NetLootItem {
    uint32_t pid;
    int32_t qty;
} NetLootItem;

#define NET_LOOT_MAX_ITEMS 120

// Client -> host: loot operation.
typedef struct NetLootCmdPayload {
    uint8_t op;          // NetLootOpcode
    uint8_t netId;       // sender
    uint8_t reserved[2];
    uint32_t targetNetId; // session target (echoed from OPEN)
    uint32_t pid;         // MOVE: item pid
    int32_t qty;          // MOVE: signed quantity (+ take from target, - plant)
} NetLootCmdPayload;

// Host -> client: authoritative loot state. Followed by targetItemCount
// NetLootItem entries (target inventory snapshot) then dudeDeltaCount
// NetLootItem entries (signed deltas applied to the looter's inventory).
typedef struct NetLootStatePayload {
    uint8_t op;           // NetLootOpcode
    uint8_t netId;        // session owner
    uint8_t isSteal;      // steal-mode session (rolls apply)
    uint8_t lastOk;       // 1 = last op applied
    uint8_t targetItemCount;
    uint8_t dudeDeltaCount;
    uint16_t reserved;
    uint32_t targetNetId;
    uint32_t targetFid;   // OPEN: mirror display art
    uint32_t targetPid;   // OPEN: mirror proto
    char msgText[64];     // formatted feedback message (0 = none)
} NetLootStatePayload;

// Host -> acting client: the authoritative outcome of a combat move intent.
// The client walks its local dude optimistically; a resolution to a different
// tile than the clicked destination (rejected, or truncated at an occupied
// tile) snaps the local dude into place so the vanilla targeting UI reads the
// true geometry.
typedef struct NetCombatMoveResultPayload {
    uint8_t netId;        // acting player
    uint8_t reserved[3];
    uint32_t tile;        // authoritative destination tile
    int32_t elevation;
} NetCombatMoveResultPayload;

// Host -> clients: floating text over a critter, relayed so clients see the
// same text over the same critter (scripts and the engine AI only run
// host-side). Two style sources: the script float_msg path sets type to the
// resolved FloatingMessageType and leaves font/color/outline zeroed (the
// client derives them from type); the engine AI path (combatai.msg "weapon
// comments" like "My blade grows thirsty") sets type to NET_FLOAT_AI_STYLE
// and fills the explicit style, because the AI config's font/color/outline
// do not map to any FloatingMessageType. The synchronized dialogue floats
// (NPC lines and player choices) use NET_FLOAT_DIALOG_STYLE with the same
// explicit style contract.
#define NET_FLOAT_AI_STYLE (-3)     // matches no FloatingMessageType (WARNING=-2..)
#define NET_FLOAT_DIALOG_STYLE (-4) // dialogue float (explicit style below)
typedef struct NetFloatMessagePayload {
    uint32_t netId;       // critter the text floats over
    int32_t type;         // resolved FloatingMessageType or NET_FLOAT_AI/DIALOG_STYLE
    int32_t font;         // explicit style when type == NET_FLOAT_AI/DIALOG_STYLE
    int32_t color;        // explicit style when type == NET_FLOAT_AI/DIALOG_STYLE
    int32_t outline;      // explicit style when type == NET_FLOAT_AI/DIALOG_STYLE
    char text[160];       // the message text
} NetFloatMessagePayload;

// Chat relay: the client sends its message to the host, the host validates
// the sender (netId must match the peer) and forwards it to every other
// connected player. The sender shows its own line locally and never gets an
// echo. The text is NUL-terminated, at most MP_CHAT_MESSAGE_MAX_LENGTH
// characters. The receiver floats it above the sender's critter (chat
// messages are the "floating text" the spec asks for) and appends it to the
// chat window, which mirrors the combat log 1:1.
#define MP_CHAT_MESSAGE_MAX_LENGTH (120)
typedef struct NetChatMessagePayload {
    uint8_t senderNetId;   // validated against the peer on the host
    char text[MP_CHAT_MESSAGE_MAX_LENGTH + 1];
} NetChatMessagePayload;

// Quest state relay: the gvar table is the single quest persistence in FO2
// (script local vars of the dude do not survive vanilla saves at all), so the
// host's table IS the co-op quest state. The joiner receives the full table
// (FO2 ships 10000 gvars; the relay cap covers any modded table) and every
// subsequent write rides NET_PKT_GVAR_CHANGE. A full snapshot is one ~64KB
// reliable packet — well inside ENet's fragmentation limits.
#define NET_GVAR_MAX_VALUES (16384)
typedef struct NetGvarSnapshotPayload {
    uint32_t count;    // entries that follow (clamped to NET_GVAR_MAX_VALUES)
    int32_t values[NET_GVAR_MAX_VALUES];
} NetGvarSnapshotPayload;

// Host -> clients: a single gvar write (hooked at gameSetGlobalVar).
typedef struct NetGvarChangePayload {
    int32_t index;
    int32_t value;
} NetGvarChangePayload;

// Client -> host: one per-player drug addiction write. Addictions are NOT
// quest state — each player's addiction gvars live in a host-side overlay,
// and dialogue scripts for that player read the overlay (initiator-aware).
typedef struct NetAddictionChangePayload {
    int32_t gvar;
    int32_t value;
} NetAddictionChangePayload;

// Worldmap travel (co-op): the host drives the vanilla worldmap and clients
// mirror it read-only. ENTER carries the screen mode so the client renders
// the same loop the host is in (0 = world map, 1 = town map). STATE carries
// the authoritative party state: world position, walking target, current
// area, and car state — applied onto the client's WmGenData each update.
// EXIT closes the mirror when the host leaves without loading a map (a map
// load always follows MAP_CHANGED, which supersedes EXIT).
typedef struct NetWorldmapEnterPayload {
    uint8_t mode;        // 0 = wmWorldMapFunc(0), 1 = wmWorldMapFunc(1) (town map)
} NetWorldmapEnterPayload;

typedef struct NetWorldmapStatePayload {
    int32_t worldPosX;
    int32_t worldPosY;
    int32_t walkDestinationX;
    int32_t walkDestinationY;
    uint8_t isWalking;
    uint8_t isInCar;
    int32_t currentAreaId;
    int32_t carFuel;
    int32_t currentCarAreaId;
    // Random-encounter selection (wmRndEncounterPick ran on the host right
    // before the map load): the client mirror needs these ids to run
    // wmSetupRandomEncounter after its deferred map-enter and spawn its own
    // copy of the encounter critters. -1 = no encounter pending.
    int32_t encounterMapId;
    int32_t encounterTableId;
    int32_t encounterEntryId;
} NetWorldmapStatePayload;

// Host -> acting client: the avatar's walk ended before reaching its
// registered destination (pressure plate, script interrupt, knockback).
// The client's optimistic walk must stop and snap to the authoritative
// position instead of continuing to the clicked tile.
typedef struct NetWalkInterruptedPayload {
    int32_t tile;
    int32_t elevation;
} NetWalkInterruptedPayload;

// Worldmap discovery sync (host -> clients): the host's walking marks subtiles
// visited/known and areas visited; the client mirror must show the same fog
// and town list, and its own save must persist them. mode 1 = full snapshot
// (count = tile count, body = 2-bit packed subtile states, 11 bytes per tile,
// then visited/state pairs for each area), mode 2 = incremental (count =
// change count, body = count * 5 bytes: u16 tile, u8 subX, u8 subY, u8 state).
typedef struct NetWorldmapDiscoveryPayload {
    uint8_t mode;
    uint16_t count;
    uint16_t reserved;
} NetWorldmapDiscoveryPayload;
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
// FNV-1a 32-bit hash of a session password; returns 0 for null/empty input
// (the "no password" sentinel, so an empty string is never a valid secret).
uint32_t NetPasswordHash(const char* password);
// Host: the port the last NetHostCreate bound (0 before any host).
uint16_t NetGetBoundPort();
// Client: the address/port the last NetClientConnect resolved (address text
// as typed by the player, port as passed). Empty string before any connect.
const char* NetGetConnectedAddress();
uint16_t NetGetConnectedPort();
// Round-trip time in ms. Client (hostPeer set): ping to the host. Host
// (hostPeer null): average ping of all connected peers, 0 when idle.
uint32_t NetGetPingMs(ENetHost* host, ENetPeer* hostPeer);

// Helper that appends a NetPacketHeader in front of a payload and sends it
// on the given peer/channel. payload may be NULL if payloadLength == 0.
bool NetSendPacket(ENetPeer* peer, int channel, uint8_t packetType, const void* payload, size_t payloadLength);
bool NetBroadcastPacket(ENetHost* host, int channel, uint8_t packetType, const void* payload, size_t payloadLength);

} // namespace fallout

#endif /* FALLOUT_NET_H_ */
