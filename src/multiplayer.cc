#include "multiplayer.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "animation.h"
#include "actions.h"
#include "art.h"
#include "combat.h"
#include "color.h"
#include "debug.h"
#include "game.h"
#include "geometry.h"
#include "input.h"
#include "interface.h"
#include "item.h"
#include "light.h"
#include "loadsave.h"
#include "map.h"
#include "multiplayer_vote.h"
#include "multiplayer_combat.h"
#include "multiplayer_profile.h"
#include "object.h"
#include "party_member.h"
#include "proto.h"
#include "proto_instance.h"
#include "scripts.h"
#include "settings.h"
#include "stat.h"
#include "tile.h"
#include "window_manager.h"
#include "window_manager_private.h"
#include "worldmap.h"

namespace fallout {

MultiplayerSession gMpSession = {};
bool gMpIsHost = false;
bool gMpIsClient = false;
bool gMpActive = false;
bool gMpSuppressExitGridCheck = false;
static uint32_t gMpLastTileRefreshTick = 0;
int gMpPendingHostStartAfterLoad = 0;
int gMpPendingClientStartAfterLoad = 0;
char gMpPendingClientAddress[64] = {};

struct MpHostObjectRecord {
    uint32_t netId;
    NetMapFullSyncObjectPayload state;
};

static std::vector<MpHostObjectRecord> gMpHostObjectRecords;
static std::vector<int32_t> gMpHostTileBaseline;
static bool gMpHostFirstTickPending = false;
static MpPlayerProfile gMpPendingClientProfile;
static bool gMpPendingClientProfileValid = false;
static uint32_t gMpNextProfileStreamId = 0;

// Client-side character-sheet change tracking. The client owns its sheet
// (XP from scripts/skills, level-up perks, skills spent, items looted); the
// unified sync pushes every change up through the profile channel so the
// host's avatar and the other clients stay in lockstep.
static uint32_t gMpLocalProfileSyncTick = 0;
static uint32_t gMpLastUploadedProfileHash = 0;
static char gMpLastUploadedModelName[13] = {};
static bool gMpLocalProfileSyncReady = false;

struct MpProfileReceiveState {
    bool active = false;
    uint32_t streamId = 0;
    uint8_t netId = 0;
    uint32_t generation = 0;
    uint32_t objNetId = 0;
    uint32_t expectedBytes = 0;
    uint32_t expectedChunks = 0;
    uint32_t expectedHash = 0;
    uint32_t receivedBytes = 0;
    uint16_t nextChunk = 0;
    std::vector<uint8_t> bytes;
};

static std::unordered_map<uint8_t, MpProfileReceiveState> gMpProfileReceives;

static void mpShowClientPlayer(Object* obj);
static void mpEnsureClientPlayersVisible();
static void mpDebugDumpWalls(const char* tag, int limit);
static void mpDebugSnapshotPreSyncWalls();
static void mpDebugReportMissingWalls();
static MultiplayerPlayer* mpPlayerFindByPeer(ENetPeer* peer);
static int mpFindPlayerSpawnTile(int preferredTile, int elevation);
static void mpBuildMapSyncPayload(NetMapSyncPayload* payload);
static bool mpSendProfile(ENetPeer* peer, uint8_t netId, uint32_t objNetId,
    const MpPlayerProfile& profile);
static void mpBroadcastProfileToClients(uint8_t netId, uint32_t objNetId,
    const MpPlayerProfile& profile);
static void mpApplyReceivedProfile(uint8_t netId, uint32_t objNetId,
    const MpPlayerProfile& profile);
static void mpClientTryFinishMapSync();
static void mpDebugDumpLightState(const char* tag);

// _obj_remove_all() (run by the map load) destroys every object in the head
// list — including gDude's inventory items, which live there with tile == -1.
// That leaves dangling item pointers in the dude's inventory and crashes the
// HUD weight display. Mark the dude's items OBJECT_NO_REMOVE for the duration
// of a map change so the removal walks skip them.
static void mpSetDudeInventoryProtected(bool protect)
{
    if (gDude == nullptr) {
        return;
    }
    Inventory* inv = &gDude->data.inventory;
    for (int index = 0; index < inv->length; index++) {
        Object* item = inv->items[index].item;
        if (item == nullptr) {
            continue;
        }
        if (protect) {
            item->flags |= OBJECT_NO_REMOVE;
        } else {
            item->flags &= ~OBJECT_NO_REMOVE;
        }
    }
}

// True if obj is an item carried by critter (hands, armor slot, or inventory).
// Player-held items are part of the local character state and must never be
// destroyed by map-sync cleanup, nor broadcast as world objects.
static bool mpObjectIsInCritterInventory(Object* obj, Object* critter)
{
    if (obj == nullptr || critter == nullptr) {
        return false;
    }
    if (obj == critterGetItem1(critter) || obj == critterGetItem2(critter)
        || obj == critterGetArmor(critter)) {
        return true;
    }
    const Inventory* inventory = &critter->data.inventory;
    for (int index = 0; index < inventory->length; index++) {
        if (inventory->items[index].item == obj) {
            return true;
        }
    }
    return false;
}

static bool mpObjectIsInAnyPlayerInventory(Object* obj)
{
    if (obj == nullptr) {
        return false;
    }
    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        MultiplayerPlayer* player = &gMpSession.players[index];
        if (player->isConnected && player->obj != nullptr
            && mpObjectIsInCritterInventory(obj, player->obj)) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void mpZeroSession()
{
    memset(&gMpSession, 0, sizeof(gMpSession));
    gMpSession.state = MP_STATE_NONE;
}

// Every received packet must contain exactly one valid header and payload.
// Keeping this check centralized prevents a malformed packet from being cast
// directly to one of the packed payload structs below.
static bool mpDecodePacket(const void* data, size_t dataLength, uint8_t* type, const void** payload, size_t* payloadLength)
{
    if (data == nullptr || type == nullptr || payload == nullptr || payloadLength == nullptr) {
        return false;
    }

    if (dataLength < sizeof(NetPacketHeader)) {
        return false;
    }

    NetPacketHeader header;
    memcpy(&header, data, sizeof(header));
    size_t actualPayloadLength = dataLength - sizeof(NetPacketHeader);
    if (header.length != actualPayloadLength
        || actualPayloadLength > NET_MAX_PACKET_SIZE - sizeof(NetPacketHeader)) {
        return false;
    }

    *type = header.type;
    *payload = (const char*)data + sizeof(NetPacketHeader);
    *payloadLength = actualPayloadLength;
    return true;
}

static uint32_t mpAllocProfileStreamId()
{
    uint32_t streamId = ++gMpNextProfileStreamId;
    if (streamId == 0) {
        streamId = ++gMpNextProfileStreamId;
    }
    return streamId;
}

static bool mpSendProfile(ENetPeer* peer, uint8_t netId, uint32_t objNetId,
    const MpPlayerProfile& profile)
{
    if (peer == nullptr || !MpProfileValidate(profile)) {
        debugFilePrint("MP: send profile invalid peer=%p netId=%u", (void*)peer, netId);
        return false;
    }

    std::vector<uint8_t> bytes;
    if (!MpProfileSerialize(profile, &bytes) || bytes.empty()
        || bytes.size() > MP_PROFILE_MAX_BYTES) {
        debugFilePrint("MP: send profile serialize failed netId=%u name='%s'", netId, profile.name);
        return false;
    }

    constexpr size_t kChunkDataSize = NET_MAX_PACKET_SIZE
        - sizeof(NetPacketHeader) - sizeof(NetPlayerProfileChunkHeader);
    uint32_t chunkCount = (uint32_t)((bytes.size() + kChunkDataSize - 1) / kChunkDataSize);
    if (chunkCount == 0 || chunkCount > UINT16_MAX) {
        debugFilePrint("MP: send profile chunk count invalid bytes=%zu chunks=%u", bytes.size(), chunkCount);
        return false;
    }

    uint32_t streamId = mpAllocProfileStreamId();
    debugFilePrint("MP: send profile begin netId=%u name='%s' gen=%u objNet=%u bytes=%zu chunks=%u stream=%u",
        netId, profile.name, profile.generation, objNetId, bytes.size(), chunkCount, streamId);
    NetPlayerProfileBeginPayload begin;
    memset(&begin, 0, sizeof(begin));
    begin.streamId = streamId;
    begin.netId = netId;
    begin.schemaVersion = profile.schemaVersion;
    begin.generation = profile.generation;
    begin.objNetId = objNetId;
    begin.totalBytes = (uint32_t)bytes.size();
    begin.chunkCount = chunkCount;
    begin.contentHash = MpProfileHash(profile);
    if (!NetSendPacket(peer, NET_CHANNEL_RELIABLE, NET_PKT_PLAYER_PROFILE_BEGIN,
            &begin, sizeof(begin))) {
        return false;
    }

    for (uint32_t chunkIndex = 0; chunkIndex < chunkCount; chunkIndex++) {
        size_t offset = (size_t)chunkIndex * kChunkDataSize;
        size_t remaining = bytes.size() - offset;
        uint16_t dataLength = (uint16_t)(remaining > kChunkDataSize ? kChunkDataSize : remaining);
        uint8_t packet[NET_MAX_PACKET_SIZE];
        NetPacketHeader packetHeader;
        packetHeader.type = NET_PKT_PLAYER_PROFILE_CHUNK;
        packetHeader.length = (uint16_t)(sizeof(NetPlayerProfileChunkHeader) + dataLength);
        NetPlayerProfileChunkHeader chunk;
        memset(&chunk, 0, sizeof(chunk));
        chunk.streamId = streamId;
        chunk.netId = netId;
        chunk.chunkIndex = (uint16_t)chunkIndex;
        chunk.chunkCount = (uint16_t)chunkCount;
        chunk.dataLength = dataLength;
        memcpy(packet, &packetHeader, sizeof(packetHeader));
        memcpy(packet + sizeof(packetHeader), &chunk, sizeof(chunk));
        memcpy(packet + sizeof(packetHeader) + sizeof(chunk), bytes.data() + offset, dataLength);
        if (!NetPeerSend(peer, NET_CHANNEL_RELIABLE, packet,
                sizeof(packetHeader) + sizeof(chunk) + dataLength)) {
            return false;
        }
    }

    NetPlayerProfileEndPayload end;
    memset(&end, 0, sizeof(end));
    end.streamId = streamId;
    end.netId = netId;
    end.generation = profile.generation;
    end.contentHash = begin.contentHash;
    return NetSendPacket(peer, NET_CHANNEL_RELIABLE, NET_PKT_PLAYER_PROFILE_END,
        &end, sizeof(end));
}

static void mpSendProfileReject(ENetPeer* peer, uint8_t netId, uint32_t streamId, uint16_t reason)
{
    if (peer == nullptr) return;
    NetPlayerProfileAckPayload ack;
    memset(&ack, 0, sizeof(ack));
    ack.streamId = streamId;
    ack.netId = netId;
    ack.accepted = 0;
    ack.reason = reason;
    NetSendPacket(peer, NET_CHANNEL_RELIABLE, NET_PKT_PLAYER_PROFILE_REJECT, &ack, sizeof(ack));
}

static void mpSendProfileAck(ENetPeer* peer, uint8_t netId, uint32_t streamId,
    uint32_t generation, bool accepted, uint16_t reason = 0)
{
    if (peer == nullptr) return;
    NetPlayerProfileAckPayload ack;
    memset(&ack, 0, sizeof(ack));
    ack.streamId = streamId;
    ack.netId = netId;
    ack.accepted = accepted ? 1 : 0;
    ack.reason = reason;
    ack.generation = generation;
    NetSendPacket(peer, NET_CHANNEL_RELIABLE, accepted
        ? NET_PKT_PLAYER_PROFILE_ACK
        : NET_PKT_PLAYER_PROFILE_REJECT, &ack, sizeof(ack));
}

static void mpApplyReceivedProfile(uint8_t netId, uint32_t objNetId,
    const MpPlayerProfile& profile)
{
    if (!gMpIsClient || netId == 0 || netId > NET_MAX_PLAYERS) {
        debugFilePrint("MP: apply received profile ignored netId=%u client=%d", netId, gMpIsClient);
        return;
    }

    MultiplayerPlayer* player = &gMpSession.players[netId - 1];
    if (player->profileReady && profile.generation < player->profileGeneration) {
        debugFilePrint("MP: apply received profile stale netId=%u gen=%u have=%u",
            netId, profile.generation, player->profileGeneration);
        return;
    }
    debugFilePrint("MP: apply received profile netId=%u name='%s' gen=%u objNet=%u",
        netId, profile.name, profile.generation, objNetId);
    player->netId = netId;
    player->isConnected = true;
    player->isHandshaken = true;
    player->profileReady = false;
    player->hasInitialState = false;
    player->profileGeneration = profile.generation;
    if (objNetId != 0) {
        player->objNetId = objNetId;
    }

    if (netId == gMpSession.localNetId) {
        if (!MpProfileBindLocal(netId, profile, gDude)) {
            debugFilePrint("MP: apply received profile local bind failed netId=%u", netId);
            return;
        }
        player->obj = gDude;
        // Host-accepted canonical profile: apply name/stats/skills/perks/
        // traits/inventory onto the local gDude as an authoritative
        // correction. Transform and combat runtime stay on the player-state
        // channel.
        //
        // Combat XP: the host grants it by bumping the avatar's proto
        // experience, which rides down in this profile. Apply the delta
        // through the vanilla level-up path (level, max HP, LEVEL UP
        // indicator, perk pick) — and never backwards: the local pc block
        // is authoritative for anything the host hasn't granted.
        int localXp = 0;
        Proto* localProto = nullptr;
        if (protoGetProto(gDude->pid, &localProto) == 0 && localProto != nullptr) {
            localXp = localProto->critter.data.experience;
        }
        if (profile.experience > localXp) {
            int gained = profile.experience - localXp;
            debugFilePrint("MP: apply received profile combat xp netId=%u delta=%d",
                netId, gained);
            pcAddExperience(gained);
        }
        MpProfileApplyLocal(profile, /*applyPcStats=*/false);
    } else {
        if (player->obj != nullptr) {
            if (gMpSession.netIdToObj != nullptr && player->objNetId < (uint32_t)gMpSession.netIdToObjCapacity) {
                gMpSession.netIdToObj[player->objNetId] = nullptr;
            }
            MpProfileDestroyRuntime(netId);
            player->obj = nullptr;
        }
        MpPlayerRuntime* runtime = MpProfileCreateRuntime(netId, profile,
            profile.tile, profile.elevation, profile.rotation);
        if (runtime == nullptr) {
            debugFilePrint("MP: apply received profile runtime create failed netId=%u", netId);
            return;
        }
        player->obj = runtime->object;
        if (player->objNetId != 0) {
            MpRegisterObjNetId(player->obj, player->objNetId);
        }
    }
    player->profileReady = true;
    player->hasInitialState = false;
    if (player->obj != nullptr) {
        mpShowClientPlayer(player->obj);
    }
    debugFilePrint("MP: apply received profile done netId=%u ready=%d", netId, player->profileReady);
    // A profile may be the last readiness condition (map/tile chunks and
    // states already complete); re-check so the client does not sit in
    // SYNCING forever.
    mpClientTryFinishMapSync();
}

static void mpHostAcceptProfile(MultiplayerPlayer* player, ENetPeer* peer,
    const MpPlayerProfile& profile, uint32_t streamId)
{
    if (!gMpIsHost || player == nullptr || peer == nullptr || !MpProfileValidate(profile)) {
        debugFilePrint("MP: host accept profile rejected netId=%u handshaken=%d validate=%d",
            player != nullptr ? player->netId : 0,
            player != nullptr ? player->isHandshaken : 0,
            MpProfileValidate(profile) ? 1 : 0);
        mpSendProfileReject(peer, player != nullptr ? player->netId : 0, streamId, 1);
        return;
    }

    // Mid-session profile update: the client's character sheet changed
    // (level-up perks, skills, items, XP from scripts, ...). Apply it onto
    // the existing avatar in place and echo it to every other client. The
    // avatar object is never recreated, so combat/netId mappings survive.
    if (player->isHandshaken) {
        debugFilePrint("MP: host accept profile update begin netId=%u name='%s' gen=%u",
            player->netId, profile.name, profile.generation);
        if (profile.generation <= player->profileGeneration) {
            debugFilePrint("MP: host accept profile update stale netId=%u gen=%u have=%u",
                player->netId, profile.generation, player->profileGeneration);
            mpSendProfileAck(peer, player->netId, streamId, player->profileGeneration, true);
            return;
        }
        if (!MpProfileApplyRuntimeUpdate(player->netId, profile)) {
            debugFilePrint("MP: host accept profile update apply failed netId=%u",
                player->netId);
            mpSendProfileReject(peer, player->netId, streamId, 6);
            return;
        }
        MpPlayerRuntime* runtime = MpProfileGetRuntime(player->netId);
        if (runtime != nullptr) {
            player->profileGeneration = runtime->profile.generation;
            strncpy(player->name, runtime->profile.name, NET_PEER_NAME_LENGTH - 1);
            player->name[NET_PEER_NAME_LENGTH - 1] = '\0';
        }
        mpSendProfileAck(peer, player->netId, streamId, player->profileGeneration, true);
        // Other clients must see the updated sheet (perks/items/stats) too.
        mpBroadcastProfileToClients(player->netId, player->objNetId,
            runtime != nullptr ? runtime->profile : profile);
        return;
    }

    debugFilePrint("MP: host accept profile begin netId=%u name='%s' gen=%u",
        player->netId, profile.name, profile.generation);

    // The joining player's profile carries their real position on this map
    // (both machines loaded the same .map). Spawning there keeps the first
    // state update from snapping the client's character to a spawn tile.
    // Fall back to a spot near the host only when the profile position is
    // unusable.
    int32_t tile = hexGridTileIsValid(profile.tile)
        ? profile.tile
        : (hexGridTileIsValid(gDude->tile) ? gDude->tile : gMapHeader.enteringTile);
    int32_t elevation = elevationIsValid(profile.elevation)
        ? profile.elevation
        : (gMapHeader.enteringElevation >= 0 ? gMapHeader.enteringElevation : gDude->elevation);
    int32_t rotation = profile.rotation >= 0 && profile.rotation < ROTATION_COUNT
        ? profile.rotation
        : (gMapHeader.enteringRotation >= 0 ? gMapHeader.enteringRotation : ROTATION_NE);
    tile = mpFindPlayerSpawnTile(tile, elevation);
    MpPlayerRuntime* runtime = MpProfileCreateRuntime(player->netId, profile,
        tile, elevation, rotation);
    if (runtime == nullptr) {
        debugFilePrint("MP: host accept profile runtime create failed netId=%u", player->netId);
        mpSendProfileReject(peer, player->netId, streamId, 2);
        return;
    }
    debugFilePrint("MP: host accept profile runtime created netId=%u obj=%p pid=0x%X",
        player->netId, (void*)runtime->object, runtime->syntheticPid);

    player->obj = runtime->object;
    player->objNetId = MpAllocObjNetId();
    player->profileReady = true;
    player->hasInitialState = false;
    player->profileGeneration = profile.generation;
    player->isHandshaken = true;
    player->lastSafeTile = player->obj->tile;
    player->lastSafeElevation = player->obj->elevation;
    player->lastSafeRotation = player->obj->rotation;
    player->hasSafePosition = true;
    strncpy(player->name, profile.name, NET_PEER_NAME_LENGTH - 1);
    player->name[NET_PEER_NAME_LENGTH - 1] = '\0';
    MpRegisterObjNetId(player->obj, player->objNetId);
    objectReorder(player->obj);
    gMpSession.numPlayers++;

    mpSendProfileAck(peer, player->netId, streamId, profile.generation, true);

    NetWelcomePayload welcome;
    memset(&welcome, 0, sizeof(welcome));
    welcome.assignedNetId = player->netId;
    welcome.objNetId = player->objNetId;
    mpBuildMapSyncPayload(&welcome.map);
    NetSendPacket(peer, NET_CHANNEL_RELIABLE, NET_PKT_WELCOME, &welcome, sizeof(welcome));

    // The joining client must learn every canonical profile before it receives
    // player lifecycle packets or the map snapshot.
    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        MultiplayerPlayer* other = &gMpSession.players[index];
        if (!other->isConnected || !other->profileReady || other->obj == nullptr) {
            continue;
        }
        MpPlayerRuntime* otherRuntime = MpProfileGetRuntime(other->netId);
        if (otherRuntime == nullptr) continue;
        mpSendProfile(peer, other->netId, other->objNetId, otherRuntime->profile);
    }

    NetPlayerJoinedPayload hostJoined;
    memset(&hostJoined, 0, sizeof(hostJoined));
    hostJoined.netId = gMpSession.players[0].netId;
    hostJoined.objNetId = gMpSession.players[0].objNetId;
    MpPlayerRuntime* hostRuntime = MpProfileGetRuntime(hostJoined.netId);
    if (hostRuntime != nullptr) {
        memcpy(hostJoined.name, hostRuntime->profile.name, sizeof(hostJoined.name));
    } else {
        memcpy(hostJoined.name, gMpSession.players[0].name, sizeof(hostJoined.name));
    }
    NetSendPacket(peer, NET_CHANNEL_RELIABLE, NET_PKT_PLAYER_JOINED,
        &hostJoined, sizeof(hostJoined));

    NetPlayerJoinedPayload joined;
    memset(&joined, 0, sizeof(joined));
    joined.netId = player->netId;
    joined.objNetId = player->objNetId;
    memcpy(joined.name, player->name, sizeof(joined.name));
    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        MultiplayerPlayer* other = &gMpSession.players[index];
        if (other->isConnected && other->isHandshaken && other->peer != nullptr
            && other->peer != peer) {
            MpPlayerRuntime* newRuntime = MpProfileGetRuntime(player->netId);
            if (newRuntime != nullptr) {
                mpSendProfile(other->peer, player->netId, player->objNetId, newRuntime->profile);
            }
            NetSendPacket(other->peer, NET_CHANNEL_RELIABLE, NET_PKT_PLAYER_JOINED,
                &joined, sizeof(joined));
        }
    }

    MpBroadcastMapFullSync(peer);
    win_timed_msg("Player joined", COLOR_GREEN);
    debugFilePrint("MP: host accept profile done netId=%u name='%s' objNet=%u players=%d",
        player->netId, player->name, player->objNetId, gMpSession.numPlayers);
}

static void mpHandleProfileBegin(ENetPeer* peer, const void* payload, size_t payloadLen)
{
    if (peer == nullptr || payload == nullptr || payloadLen != sizeof(NetPlayerProfileBeginPayload)) {
        return;
    }
    const NetPlayerProfileBeginPayload* begin = (const NetPlayerProfileBeginPayload*)payload;
    uint8_t actualNetId = begin->netId;
    if (gMpIsHost) {
        MultiplayerPlayer* player = mpPlayerFindByPeer(peer);
        if (player == nullptr || (actualNetId != 0 && actualNetId != player->netId)) {
            return;
        }
        actualNetId = player->netId;
    } else if (actualNetId == 0 || actualNetId > NET_MAX_PLAYERS) {
        return;
    }
    if (begin->schemaVersion != MP_PROFILE_SCHEMA_VERSION
        || begin->totalBytes == 0 || begin->totalBytes > MP_PROFILE_MAX_BYTES
        || begin->chunkCount == 0 || begin->chunkCount > UINT16_MAX) {
        debugFilePrint("MP: profile begin invalid schema=%u bytes=%u chunks=%u",
            begin->schemaVersion, begin->totalBytes, begin->chunkCount);
        if (gMpIsHost) mpSendProfileReject(peer, actualNetId, begin->streamId, 3);
        return;
    }
    debugFilePrint("MP: profile begin netId=%u stream=%u gen=%u bytes=%u chunks=%u hash=%08X",
        actualNetId, begin->streamId, begin->generation, begin->totalBytes, begin->chunkCount,
        begin->contentHash);

    MpProfileReceiveState state;
    state.active = true;
    state.streamId = begin->streamId;
    state.netId = actualNetId;
    state.generation = begin->generation;
    state.objNetId = begin->objNetId;
    state.expectedBytes = begin->totalBytes;
    state.expectedChunks = begin->chunkCount;
    state.expectedHash = begin->contentHash;
    state.bytes.resize(begin->totalBytes);
    gMpProfileReceives[actualNetId] = std::move(state);
}

static void mpHandleProfileChunk(ENetPeer* peer, const void* payload, size_t payloadLen)
{
    if (peer == nullptr || payload == nullptr || payloadLen < sizeof(NetPlayerProfileChunkHeader)) {
        return;
    }
    NetPlayerProfileChunkHeader chunk;
    memcpy(&chunk, payload, sizeof(chunk));
    if (payloadLen != sizeof(chunk) + chunk.dataLength) return;
    uint8_t actualNetId = chunk.netId;
    if (gMpIsHost && actualNetId == 0) {
        MultiplayerPlayer* player = mpPlayerFindByPeer(peer);
        if (player == nullptr) return;
        actualNetId = player->netId;
    }
    auto it = gMpProfileReceives.find(actualNetId);
    if (it == gMpProfileReceives.end()) return;
    MpProfileReceiveState& state = it->second;
    if (!state.active || state.streamId != chunk.streamId
        || chunk.chunkCount != state.expectedChunks
        || chunk.chunkIndex != state.nextChunk
        || state.receivedBytes + chunk.dataLength > state.expectedBytes) {
        debugFilePrint("MP: profile chunk rejected netId=%u stream=%u idx=%u/%u have=%u want=%u",
            actualNetId, chunk.streamId, chunk.chunkIndex, chunk.chunkCount,
            state.nextChunk, state.expectedChunks);
        return;
    }
    const uint8_t* bytes = (const uint8_t*)payload + sizeof(chunk);
    memcpy(state.bytes.data() + state.receivedBytes, bytes, chunk.dataLength);
    state.receivedBytes += chunk.dataLength;
    state.nextChunk++;
    if (state.nextChunk % 50 == 0 || state.nextChunk == state.expectedChunks) {
        debugFilePrint("MP: profile chunk progress netId=%u stream=%u %u/%u bytes=%u/%u",
            actualNetId, chunk.streamId, state.nextChunk, state.expectedChunks,
            state.receivedBytes, state.expectedBytes);
    }
}

static void mpHandleProfileEnd(ENetPeer* peer, const void* payload, size_t payloadLen)
{
    if (peer == nullptr || payload == nullptr || payloadLen != sizeof(NetPlayerProfileEndPayload)) {
        return;
    }
    const NetPlayerProfileEndPayload* end = (const NetPlayerProfileEndPayload*)payload;
    uint8_t actualNetId = end->netId;
    if (gMpIsHost && actualNetId == 0) {
        MultiplayerPlayer* player = mpPlayerFindByPeer(peer);
        if (player == nullptr) return;
        actualNetId = player->netId;
    }
    auto it = gMpProfileReceives.find(actualNetId);
    if (it == gMpProfileReceives.end()) return;
    MpProfileReceiveState state = std::move(it->second);
    gMpProfileReceives.erase(it);
    if (!state.active || state.streamId != end->streamId
        || state.generation != end->generation
        || state.expectedHash != end->contentHash
        || state.receivedBytes != state.expectedBytes
        || state.nextChunk != state.expectedChunks) {
        debugFilePrint("MP: profile end mismatch netId=%u stream=%u gen=%u/%u bytes=%u/%u chunks=%u/%u hash=%08X/%08X",
            state.netId, state.streamId, state.generation, end->generation,
            state.receivedBytes, state.expectedBytes, state.nextChunk, state.expectedChunks,
            state.expectedHash, end->contentHash);
        mpSendProfileReject(peer, state.netId, state.streamId, 4);
        return;
    }
    MpPlayerProfile profile;
    if (!MpProfileDeserialize(state.bytes.data(), state.bytes.size(), &profile)
        || profile.generation != state.generation
        || MpProfileHash(profile) != state.expectedHash) {
        debugFilePrint("MP: profile end deserialize/hash failed netId=%u stream=%u bytes=%zu",
            state.netId, state.streamId, state.bytes.size());
        mpSendProfileReject(peer, state.netId, state.streamId, 5);
        return;
    }
    debugFilePrint("MP: profile end complete netId=%u stream=%u name='%s' gen=%u bytes=%zu",
        state.netId, state.streamId, profile.name, profile.generation, state.bytes.size());

    if (gMpIsHost) {
        MultiplayerPlayer* player = mpPlayerFindByPeer(peer);
        mpHostAcceptProfile(player, peer, profile, state.streamId);
    } else {
        mpApplyReceivedProfile(state.netId, state.objNetId, profile);
        mpSendProfileAck(peer, state.netId, state.streamId, profile.generation, true);
    }
}

static void mpBuildMapSyncPayload(NetMapSyncPayload* payload)
{
    if (payload == nullptr) {
        return;
    }

    memset(payload, 0, sizeof(*payload));
    payload->mapId = gMapHeader.index;
    memcpy(payload->mapName, gMapHeader.name, sizeof(payload->mapName));
    payload->enteringTile = gMapHeader.enteringTile;
    payload->enteringElevation = gMapHeader.enteringElevation;
    payload->enteringRotation = gMapHeader.enteringRotation;
    payload->centerTile = gCenterTile;
    payload->elevation = gElevation;
    payload->flags = gMapHeader.flags;
    payload->darkness = gMapHeader.darkness;
    // The client's map script may not apply the same ambient (its script
    // state differs), so ship the host's authoritative ambient light level.
    payload->ambientIntensity = lightGetAmbientIntensity();
}

static MultiplayerPlayer* mpHostFindPlayerByObject(Object* obj);

// Ids of objects that came from the local map file. Both machines load the
// same .map, so static world geometry (walls, scenery, map-placed critters)
// is already identical on the client — the full sync must not re-ship it
// (that caused the net-id collisions and the vanishing walls). Runtime
// modifications of these objects are covered by the per-tick delta broadcast.
static std::unordered_set<int> gMpMapStaticObjIds;

// Host-side object -> netId map. Keyed by object pointer (not obj->id):
// map files can contain duplicate object ids, and an id-keyed table mapped
// every same-id pair onto ONE netId, making clients overwrite one wall with
// another (vanishing walls, teleporting objects, per-tick delta floods).
static std::unordered_map<const Object*, uint32_t> gMpHostObjNetIds;

static void mpSnapshotMapStaticObjects()
{
    gMpMapStaticObjIds.clear();
    for (Object* it = objectFindFirst(); it != nullptr; it = objectFindNext()) {
        if (gMpIsHost && mpHostFindPlayerByObject(it) != nullptr) {
            continue; // player critters travel via the player-state channel
        }
        if (it->id >= 0) {
            gMpMapStaticObjIds.insert(it->id);
        }
    }
    debugFilePrint("MP: map static objects snapshot=%zu", gMpMapStaticObjIds.size());
}

static int mpClientLoadMap(const NetMapSyncPayload* payload)
{
    if (payload == nullptr || payload->mapId < 0) {
        return -1;
    }

    char mapName[NET_MAP_NAME_LENGTH];
    memcpy(mapName, payload->mapName, sizeof(mapName));
    mapName[sizeof(mapName) - 1] = '\0';
    if (mapName[0] == '\0') {
        return -1;
    }

    gMpSession.currentMapId = payload->mapId;
    memcpy(&gMpSession.clientMapMetadata, payload, sizeof(*payload));
    gMpSession.clientMapMetadataValid = true;
    // The map load's _obj_remove_all() frees gDude's inventory items (they
    // live in the head list); protect them for the load window, mirroring the
    // map-change paths. Without this the client's dude loses every item at
    // the initial WELCOME map load (the host never reloads its map at join,
    // which is why only the client's inventory vanished).
    mpSetDudeInventoryProtected(true);
    int rc = mapLoadByName(mapName);
    mpSetDudeInventoryProtected(false);
    debugFilePrint("MPDBG after map load: rc=%d dude=%p pid=0x%X pt=%d tile=%d elev=%d hidden=%d st=%d carry=%d weight=%d",
        rc, (void*)gDude,
        gDude != nullptr ? gDude->pid : 0,
        gDude != nullptr ? PID_TYPE(gDude->pid) : -1,
        gDude != nullptr ? gDude->tile : -1,
        gDude != nullptr ? gDude->elevation : -1,
        gDude != nullptr ? ((gDude->flags & OBJECT_HIDDEN) != 0) : -1,
        gDude != nullptr ? critterGetStat(gDude, STAT_STRENGTH) : -1,
        gDude != nullptr ? critterGetStat(gDude, STAT_CARRY_WEIGHT) : -1,
        gDude != nullptr ? objectGetInventoryWeight(gDude) : -1);
    mpDebugDumpWalls("client-pre-sync", 12);
    mpDebugDumpLightState("client-after-load");
    if (rc == 0) {
        mpSnapshotMapStaticObjects();
        mpDebugSnapshotPreSyncWalls();
        mpShowClientPlayer(gDude);
    }
    return rc;
}

static void mpDebugDumpWalls(const char* tag, int limit)
{
    int total = 0;
    int shown = 0;
    for (Object* it = objectFindFirst(); it != nullptr; it = objectFindNext()) {
        int objType = FID_TYPE(it->fid);
        if (objType == OBJ_TYPE_WALL || objType == OBJ_TYPE_SCENERY) {
            total++;
            if (shown < limit) {
                uint32_t netId = 0;
                if (gMpIsHost) {
                    auto netIt = gMpHostObjNetIds.find(it);
                    if (netIt != gMpHostObjNetIds.end()) {
                        netId = netIt->second;
                    }
                }
                debugFilePrint("MPDBG wall dump[%s]: pid=0x%X fid=0x%X type=%d tile=%d elev=%d flags=0x%X id=%d netId=%u",
                    tag, it->pid, it->fid, objType, it->tile, it->elevation, it->flags, it->id, netId);
                shown++;
            }
        }
    }
    debugFilePrint("MPDBG wall dump[%s]: total=%d shown=%d", tag, total, shown);
}

static void mpDebugDumpLightState(const char* tag)
{
    int ambient = lightGetAmbientIntensity();
    int dudeLight = (gDude != nullptr && hexGridTileIsValid(gDude->tile))
        ? lightGetTileIntensity(gDude->elevation, gDude->tile)
        : -1;
    debugFilePrint("MPDBG light[%s]: ambient=%d dudeTile=%d", tag, ambient, dudeLight);
}

struct MpPreSyncWall
{
    int pid;
    int tile;
};

static std::vector<MpPreSyncWall> gMpPreSyncWalls;

static void mpDebugSnapshotPreSyncWalls()
{
    gMpPreSyncWalls.clear();
    for (Object* it = objectFindFirst(); it != nullptr; it = objectFindNext()) {
        int objType = FID_TYPE(it->fid);
        if (objType == OBJ_TYPE_WALL || objType == OBJ_TYPE_SCENERY) {
            MpPreSyncWall wall;
            wall.pid = it->pid;
            wall.tile = it->tile;
            gMpPreSyncWalls.push_back(wall);
        }
    }
    debugFilePrint("MPDBG wall pre-sync snapshot: %zu", gMpPreSyncWalls.size());
}

static void mpDebugReportMissingWalls()
{
    int missing = 0;
    for (const MpPreSyncWall& wall : gMpPreSyncWalls) {
        bool found = false;
        for (Object* it = objectFindFirst(); it != nullptr; it = objectFindNext()) {
            int objType = FID_TYPE(it->fid);
            if ((objType == OBJ_TYPE_WALL || objType == OBJ_TYPE_SCENERY)
                && it->pid == wall.pid && it->tile == wall.tile) {
                found = true;
                break;
            }
        }
        if (!found) {
            if (missing < 25) {
                debugFilePrint("MPDBG wall MISSING: pid=0x%X tile=%d", wall.pid, wall.tile);
            }
            missing++;
        }
    }
    debugFilePrint("MPDBG wall missing total=%d", missing);
}

static void mpClientApplyMapMetadata()
{
    if (!gMpSession.clientMapMetadataValid) {
        return;
    }

    const NetMapSyncPayload* metadata = &gMpSession.clientMapMetadata;
    gMapHeader.enteringTile = metadata->enteringTile;
    gMapHeader.enteringElevation = metadata->enteringElevation;
    gMapHeader.enteringRotation = metadata->enteringRotation;
    gMapHeader.flags = metadata->flags;
    gMapHeader.darkness = metadata->darkness;

    if (elevationIsValid(metadata->elevation)) {
        mapSetElevation(metadata->elevation);
    }
    // Apply the host's authoritative ambient light level: the client's map
    // script may not darken the map (script state differs from the host),
    // leaving everything fully bright.
    if (metadata->ambientIntensity >= LIGHT_INTENSITY_MIN
        && metadata->ambientIntensity <= LIGHT_INTENSITY_MAX) {
        lightSetAmbientIntensity(metadata->ambientIntensity, true);
    }
    if (hexGridTileIsValid(metadata->centerTile)) {
        tileSetCenter(metadata->centerTile,
            TILE_SET_CENTER_REFRESH_WINDOW | TILE_SET_CENTER_FLAG_IGNORE_SCROLL_RESTRICTIONS);
    }
    mpShowClientPlayer(gDude);
}

static void mpClientTryFinishMapSync()
{
    if (!gMpIsClient || gMpSession.state != MP_STATE_CLIENT_SYNCING) {
        return;
    }
    if (!gMpSession.clientMapMetadataValid) {
        debugFilePrint("MPDBG sync wait: map metadata not valid");
        return;
    }
    if (gMpSession.clientSyncExpectedChunks == 0) {
        debugFilePrint("MPDBG sync wait: no object chunks yet");
        return;
    }
    if (gMpSession.clientSyncNextChunk < gMpSession.clientSyncExpectedChunks) {
        debugFilePrint("MPDBG sync wait: object chunks %u/%u",
            gMpSession.clientSyncNextChunk, gMpSession.clientSyncExpectedChunks);
        return;
    }
    if (gMpSession.clientTileSyncExpectedChunks == 0) {
        debugFilePrint("MPDBG sync wait: no tile chunks yet");
        return;
    }
    if (gMpSession.clientTileSyncNextChunk < gMpSession.clientTileSyncExpectedChunks) {
        debugFilePrint("MPDBG sync wait: tile chunks %u/%u",
            gMpSession.clientTileSyncNextChunk, gMpSession.clientTileSyncExpectedChunks);
        return;
    }

    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        const MultiplayerPlayer* player = &gMpSession.players[index];
        if (player->isConnected && (!player->profileReady || player->obj == nullptr
                || !player->hasInitialState)) {
            debugFilePrint("MPDBG sync wait: player netId=%u name='%s' ready=%d obj=%p initial=%d",
                player->netId, player->name, player->profileReady ? 1 : 0,
                (void*)player->obj, player->hasInitialState ? 1 : 0);
            return;
        }
    }

    mpClientApplyMapMetadata();
    debugFilePrint("MPDBG sync done: dude=%p pid=0x%X pt=%d fid=0x%X anim=%d tile=%d elev=%d hidden=%d st=%d carry=%d weight=%d center=%d",
        (void*)gDude,
        gDude != nullptr ? gDude->pid : 0,
        gDude != nullptr ? PID_TYPE(gDude->pid) : -1,
        gDude != nullptr ? gDude->fid : 0,
        gDude != nullptr ? animationTypeFromFid(gDude->fid) : -1,
        gDude != nullptr ? gDude->tile : -1,
        gDude != nullptr ? gDude->elevation : -1,
        gDude != nullptr ? ((gDude->flags & OBJECT_HIDDEN) != 0) : -1,
        gDude != nullptr ? critterGetStat(gDude, STAT_STRENGTH) : -1,
        gDude != nullptr ? critterGetStat(gDude, STAT_CARRY_WEIGHT) : -1,
        gDude != nullptr ? objectGetInventoryWeight(gDude) : -1,
        gCenterTile);
    mpDebugDumpWalls("client-synced", 30);
    mpDebugReportMissingWalls();
    mpDebugDumpLightState("client-sync-done");
    if (gDude != nullptr) {
        for (int itemIndex = 0; itemIndex < gDude->data.inventory.length; itemIndex++) {
            const Object* invItem = gDude->data.inventory.items[itemIndex].item;
            debugFilePrint("MPDBG sync item[%d]: obj=%p pid=0x%X qty=%d weight=%d",
                itemIndex, (void*)invItem,
                invItem != nullptr ? invItem->pid : 0,
                invItem != nullptr ? gDude->data.inventory.items[itemIndex].quantity : -1,
                invItem != nullptr ? itemGetWeight((Object*)invItem) : -1);
        }
    }
    // The host's center tile belongs to the host's player. Center the local
    // camera on the local player so he is on screen.
    if (gDude != nullptr && hexGridTileIsValid(gDude->tile)) {
        tileSetCenter(gDude->tile,
            TILE_SET_CENTER_REFRESH_WINDOW | TILE_SET_CENTER_FLAG_IGNORE_SCROLL_RESTRICTIONS);
    }
    tileWindowRefreshFull();
    gMpSession.state = MP_STATE_CLIENT_PLAYING;
}

// Enter CLIENT_SYNCING and arm the sync watchdog. Every path that switches the
// client into the syncing state must go through here so the timeout clock and
// the state stay consistent.
static void mpEnterClientSync()
{
    gMpSession.state = MP_STATE_CLIENT_SYNCING;
    gMpSession.clientSyncStartTick = getTicks();
    debugFilePrint("MP: enter client syncing state map=%d", gMpSession.currentMapId);
}

static MultiplayerPlayer* mpHostFindPlayerByObject(Object* obj)
{
    if (obj == nullptr) {
        return nullptr;
    }

    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        MultiplayerPlayer* player = &gMpSession.players[index];
        if (player->isConnected && player->obj == obj) {
            return player;
        }
    }

    return nullptr;
}

static int mpHostRegisterPlayerMovement(Object* obj, bool isRun, int tile, int elevation)
{
    if (obj == nullptr) {
        debugFilePrint("MP: movement rejected null obj tile=%d elev=%d", tile, elevation);
        return -1;
    }
    if (!hexGridTileIsValid(tile)) {
        debugFilePrint("MP: movement rejected invalid tile=%d elev=%d obj=%p tile=%d elev=%d hidden=%d",
            tile, elevation, (void*)obj, obj->tile, obj->elevation,
            (obj->flags & OBJECT_HIDDEN) != 0 ? 1 : 0);
        return -1;
    }
    if (!elevationIsValid(elevation)) {
        debugFilePrint("MP: movement rejected invalid elevation=%d tile=%d obj=%p tile=%d elev=%d hidden=%d",
            elevation, tile, (void*)obj, obj->tile, obj->elevation,
            (obj->flags & OBJECT_HIDDEN) != 0 ? 1 : 0);
        return -1;
    }

    // Co-op combat: movement during combat flows exclusively through the
    // AP-gated intent path (mpCombatResolveMove). Free-form walk packets
    // here bypass AP entirely, and stale packets queued before combat
    // started would replay afterwards and drag the avatar around.
    if (MpCombatIsActive()) {
        debugFilePrint("MP: movement rejected in combat obj=%p tile=%d elev=%d",
            (void*)obj, tile, elevation);
        return -1;
    }

    // A networked click has the same interrupt semantics as a local click.
    // Without clearing the current sequence, animationRegister* rejects the
    // request while the player is already walking.
    if (settings.system.interrupt_walk) {
        reg_anim_clear(obj);
    }

    if (reg_anim_begin(ANIMATION_REQUEST_RESERVED) != 0) {
        debugFilePrint("MP: movement reg_anim_begin failed obj=%p netObj=%u busy=%d tile=%d elev=%d hidden=%d",
            (void*)obj, MpGetObjNetId(obj), animationIsBusy(obj) != 0 ? 1 : 0,
            obj->tile, obj->elevation, (obj->flags & OBJECT_HIDDEN) != 0 ? 1 : 0);
        return -1;
    }

    int rc = isRun
        ? animationRegisterRunToTile(obj, tile, elevation, -1, 0)
        : animationRegisterMoveToTile(obj, tile, elevation, -1, 0);
    int endRc = reg_anim_end();
    if (rc != 0 || endRc != 0) {
        int walkFid = buildFid(FID_TYPE(obj->fid), obj->fid & 0xFFF,
            isRun ? ANIM_RUNNING : ANIM_WALK, (obj->fid & 0xF000) >> 12, obj->rotation + 1);
        debugFilePrint("MP: movement registration failed netObj=%u run=%d tile=%d rc=%d end=%d objTile=%d objElev=%d hidden=%d busy=%d objFid=0x%X walkFid=0x%X artWalk=%d artRun=%d",
            MpGetObjNetId(obj), isRun ? 1 : 0, tile, rc, endRc,
            obj->tile, obj->elevation,
            (obj->flags & OBJECT_HIDDEN) != 0 ? 1 : 0,
            animationIsBusy(obj) != 0 ? 1 : 0,
            obj->fid, walkFid,
            artExists(buildFid(FID_TYPE(obj->fid), obj->fid & 0xFFF, ANIM_WALK, (obj->fid & 0xF000) >> 12, obj->rotation + 1)) ? 1 : 0,
            artExists(buildFid(FID_TYPE(obj->fid), obj->fid & 0xFFF, ANIM_RUNNING, (obj->fid & 0xF000) >> 12, obj->rotation + 1)) ? 1 : 0);
        return -1;
    }

    // Co-op diagnostic (throttled): remote movement registration. Lets the
    // log show whether the avatar's walk is actually being registered and
    // where it starts from, e.g. while the host sits in a modal.
    static uint32_t gMpRemoteMoveLogTick = 0;
    uint32_t nowTicks = getTicks();
    if (nowTicks - gMpRemoteMoveLogTick > 500) {
        gMpRemoteMoveLogTick = nowTicks;
        debugFilePrint("MP: remote move registered obj=%p tile=%d->%d elev=%d run=%d busy=%d",
            (void*)obj, obj->tile, tile, elevation, isRun ? 1 : 0,
            animationIsBusy(obj) != 0 ? 1 : 0);
    }

    return 0;
}

static bool mpBuildObjectState(Object* obj, NetMapFullSyncObjectPayload* state)
{
    if (obj == nullptr || state == nullptr) {
        return false;
    }

    auto netIt = gMpHostObjNetIds.find(obj);
    if (netIt == gMpHostObjNetIds.end()) {
        return false;
    }
    uint32_t netId = netIt->second;
    if (netId == 0 || (obj->flags & OBJECT_HIDDEN) != 0
        || mpObjectIsInAnyPlayerInventory(obj)) {
        return false;
    }

    memset(state, 0, sizeof(*state));
    state->netId = netId;
    state->pid = obj->pid;
    state->tile = obj->tile;
    state->fid = obj->fid;
    state->frame = obj->frame;
    state->x = obj->x;
    state->y = obj->y;
    state->rotation = obj->rotation;
    state->elevation = obj->elevation;
    state->flags = obj->flags;
    if (FID_TYPE(obj->fid) == OBJ_TYPE_CRITTER) {
        state->hp = obj->data.critter.hp;
        state->ap = obj->data.critter.combat.ap;
        state->radiation = obj->data.critter.radiation;
        state->poison = obj->data.critter.poison;
        state->combatTeam = obj->data.critter.combat.team;
        state->combatManeuver = obj->data.critter.combat.maneuver;
        state->combatResults = obj->data.critter.combat.results;
    }
    return true;
}

static int mpHostFindObjectRecord(uint32_t netId)
{
    for (int index = 0; index < (int)gMpHostObjectRecords.size(); index++) {
        if (gMpHostObjectRecords[index].netId == netId) {
            return index;
        }
    }
    return -1;
}

// True when netId is the object netId of a connected player's critter. The
// client binds those netIds to its player objects, so any ordinary map object
// broadcasting with one would be rejected (and lost) by every client.
static bool mpHostNetIdIsPlayerObjNetId(uint32_t netId)
{
    if (netId == 0 || !gMpIsHost) {
        return false;
    }
    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        const MultiplayerPlayer* player = &gMpSession.players[index];
        if (player->isConnected && player->objNetId == netId) {
            return true;
        }
    }
    return false;
}

static void mpDestroyNetworkObject(Object* obj)
{
    if (obj == nullptr || obj == gDude) {
        return;
    }
    obj->flags &= ~OBJECT_NO_REMOVE;
    objectDestroy(obj, nullptr);
}

// Find a player slot by netId. Returns nullptr if not found.
static MultiplayerPlayer* mpPlayerFindByNetId(uint8_t netId)
{
    if (netId == 0 || netId > NET_MAX_PLAYERS) {
        return nullptr;
    }
    MultiplayerPlayer* p = &gMpSession.players[netId - 1];
    return p->isConnected ? p : nullptr;
}

// Find a player slot by peer pointer. Returns nullptr if not found.
static MultiplayerPlayer* mpPlayerFindByPeer(ENetPeer* peer)
{
    if (peer == nullptr) {
        return nullptr;
    }
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        if (gMpSession.players[i].isConnected && gMpSession.players[i].peer == peer) {
            return &gMpSession.players[i];
        }
    }
    return nullptr;
}

static Object* mpHostFindObjectByNetId(uint32_t netId)
{
    if (netId == 0) {
        return nullptr;
    }

    Object* obj = objectFindFirst();
    while (obj != nullptr) {
        auto it = gMpHostObjNetIds.find(obj);
        if (it != gMpHostObjNetIds.end() && it->second == netId) {
            return obj;
        }
        obj = objectFindNext();
    }

    return nullptr;
}

// Find first free slot and return its (1-based) netId; 0 if none free.
static uint8_t mpAllocPlayerSlot(ENetPeer* peer)
{
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        if (!gMpSession.players[i].isConnected) {
            return (uint8_t)(i + 1);
        }
    }
    return 0;
}

static int mpFindPlayerSpawnTile(int preferredTile, int elevation)
{
    if (hexGridTileIsValid(preferredTile) && !_obj_occupied(preferredTile, elevation)) {
        return preferredTile;
    }

    for (int distance = 1; distance <= 3; distance++) {
        for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
            int tile = tileGetTileInDirection(preferredTile, rotation, distance);
            if (hexGridTileIsValid(tile) && !_obj_occupied(tile, elevation)) {
                return tile;
            }
        }
    }

    return preferredTile;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

int MpInit()
{
    debugFilePrint("MP: init");
    mpZeroSession();
    MpProfileDestroyAllRuntimes();
    gMpProfileReceives.clear();
    gMpPendingClientProfileValid = false;
    MpVoteInit();
    if (!NetInit()) {
        debugFilePrint("MP: init NetInit failed");
        debugPrint("MpInit: NetInit failed\n");
        return -1;
    }
    return 0;
}

void MpShutdown()
{
    debugFilePrint("MP: shutdown begin active=%d", gMpActive);
    if (gMpActive) {
        if (gMpIsHost) {
            MpHostStop();
        } else if (gMpIsClient) {
            MpClientDisconnect();
        }
    }
    MpVoteShutdown();
    MpClearNetIdMappings();
    MpProfileDestroyAllRuntimes();
    gMpProfileReceives.clear();
    NetShutdown();
    mpZeroSession();
    gMpIsHost = false;
    gMpIsClient = false;
    gMpActive = false;
    gMpPendingHostStartAfterLoad = 0;
    gMpPendingClientStartAfterLoad = 0;
    gMpPendingClientAddress[0] = '\0';
    gMpPendingClientProfileValid = false;
    debugFilePrint("MP: shutdown done");
}

void MpReset()
{
    debugFilePrint("MP: reset begin active=%d", gMpActive);
    if (gMpActive) {
        if (gMpIsHost) {
            MpHostStop();
        } else if (gMpIsClient) {
            MpClientDisconnect();
        }
    }
    MpVoteReset();
    MpCombatReset();
    MpClearNetIdMappings();
    MpProfileDestroyAllRuntimes();
    gMpProfileReceives.clear();
    mpZeroSession();
    gMpIsHost = false;
    gMpIsClient = false;
    gMpActive = false;
    gMpPendingClientProfileValid = false;
    // ENet itself stays initialized across game resets; NetInit is idempotent.
    NetInit();
    debugFilePrint("MP: reset done");
}

// ---------------------------------------------------------------------------
// Host side
// ---------------------------------------------------------------------------

int MpHostStart(int32_t mapId)
{
    debugFilePrint("MP: MpHostStart begin map=%d active=%d loaded=%d dude=%p",
        mapId, gMpActive, gGameLoaded, (void*)gDude);
    if (gMpActive || !gGameLoaded || gDude == nullptr || mapId < 0) {
        debugPrint("MpHostStart: invalid game state (active=%d, loaded=%d, dude=%p, map=%d)\n",
            gMpActive, gGameLoaded, (void*)gDude, mapId);
        debugFilePrint("MP: MpHostStart rejected invalid state");
        return -1;
    }

    ENetHost* host = NetHostCreate(NET_DEFAULT_PORT, NET_MAX_PLAYERS);
    debugFilePrint("MP: NetHostCreate result=%p", (void*)host);
    if (host == nullptr) {
        win_timed_msg("Failed to host on port 7777", COLOR_RED);
        debugPrint("MpHostStart: NetHostCreate failed\n");
        return -1;
    }

    gMpSession.enetHost = host;
    gMpSession.state = MP_STATE_HOST_PLAYING;
    gMpSession.localNetId = 1;
    gMpSession.numPlayers = 1;
    gMpSession.currentMapId = mapId;
    gMpIsHost = true;
    gMpActive = true;

    MpPlayerProfile hostProfile;
    if (!MpProfileCaptureLocal(&hostProfile)) {
        debugFilePrint("MP: MpHostStart failed profile capture");
        NetHostDestroy(host);
        mpZeroSession();
        gMpIsHost = false;
        gMpActive = false;
        return -1;
    }
    if (!MpProfileBindLocal(1, hostProfile, gDude)) {
        debugFilePrint("MP: MpHostStart failed profile bind");
        NetHostDestroy(host);
        mpZeroSession();
        gMpIsHost = false;
        gMpActive = false;
        return -1;
    }

    // Host player is slot 1, owning gDude.
    MultiplayerPlayer* hostPlayer = &gMpSession.players[0];
    memset(hostPlayer, 0, sizeof(*hostPlayer));
    hostPlayer->netId = 1;
    strncpy(hostPlayer->name, hostProfile.name, NET_PEER_NAME_LENGTH - 1);
    hostPlayer->obj = gDude;
    hostPlayer->objNetId = MpAllocObjNetId();
    hostPlayer->peer = nullptr;
    hostPlayer->isLocal = true;
    hostPlayer->isConnected = true;
    hostPlayer->profileReady = true;
    hostPlayer->profileGeneration = hostProfile.generation;
    hostPlayer->lastSafeTile = gDude->tile;
    hostPlayer->lastSafeElevation = gDude->elevation;
    hostPlayer->lastSafeRotation = gDude->rotation;
    hostPlayer->hasSafePosition = true;
    MpRegisterObjNetId(gDude, hostPlayer->objNetId);
    objectReorder(gDude);
    debugFilePrint("MP: host player registered object id=%d netId=%u", gDude->id, hostPlayer->objNetId);

    // Assign netIds to every other object on the current map.
    MpAssignNetIdsToAllObjects();
    mpSnapshotMapStaticObjects();
    debugFilePrint("MP: object net ids assigned");
    mpDebugDumpWalls("host", 12);
    MpResetObjectSyncBaseline();
    debugFilePrint("MP: object sync baseline reset");

    gMpHostFirstTickPending = true;
    win_timed_msg("Hosting on port 7777", COLOR_GREEN);
    debugFilePrint("MP: MpHostStart success");
    return 0;
}

int MpHostCurrentGame()
{
    debugFilePrint("MP: MpHostCurrentGame begin loaded=%d active=%d combat=%d",
        gGameLoaded, gMpActive, isInCombat());
    if (!gGameLoaded || gDude == nullptr || gMpActive) {
        debugFilePrint("MP: MpHostCurrentGame rejected state");
        return -1;
    }

    if (isInCombat()) {
        debugFilePrint("MP: MpHostCurrentGame rejected during combat");
        win_timed_msg("Finish combat before hosting", COLOR_RED);
        return -1;
    }

    // Persist the exact state the player is currently playing before the
    // network session starts. Uses the reserved hidden co-op slot so the
    // player's own save slots are never touched or overwritten.
    int saveRc = lsgQuickSaveGameCoop();
    debugFilePrint("MP: MpHostCurrentGame quick save rc=%d", saveRc);
    if (saveRc != 1) {
        return -1;
    }

    return MpHostStart(gMapHeader.index);
}

int MpHostStop()
{
    if (!gMpIsHost) {
        return 0;
    }
    debugFilePrint("MP: MpHostStop begin players=%d", gMpSession.numPlayers);
    // Tell all clients we're going away.
    NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE, NET_PKT_DISCONNECT, nullptr, 0);
    NetHostDestroy(gMpSession.enetHost);
    MpVoteReset();

    // Free spawned critters for clients (host owns them).
    for (int i = 1; i < NET_MAX_PLAYERS; i++) {
        MultiplayerPlayer* p = &gMpSession.players[i];
        if (p->isConnected && p->obj != nullptr) {
            gMpHostObjNetIds.erase(p->obj);
        }
        if (MpProfileGetRuntime(p->netId) != nullptr) {
            MpProfileDestroyRuntime(p->netId);
        } else if (p->isConnected && p->obj != nullptr) {
            mpDestroyNetworkObject(p->obj);
        }
        memset(p, 0, sizeof(*p));
    }

    gMpHostObjNetIds.clear();
    gMpHostObjectRecords.clear();
    gMpProfileReceives.clear();
    MpProfileDestroyRuntime(1);

    mpZeroSession();
    gMpIsHost = false;
    gMpActive = false;
    gMpHostFirstTickPending = false;
    debugFilePrint("MP: MpHostStop done");
    return 0;
}

// ---------------------------------------------------------------------------
// Client side
// ---------------------------------------------------------------------------

int MpClientConnect(const char* address, uint16_t port)
{
    if (gMpActive) {
        return -1;
    }

    gMpPendingClientProfileValid = MpProfileCaptureLocal(&gMpPendingClientProfile);
    if (!gMpPendingClientProfileValid) {
        debugFilePrint("MP: MpClientConnect profile capture failed addr='%s'", address);
        win_timed_msg("Could not capture player profile", COLOR_RED);
        return -1;
    }
    debugFilePrint("MP: MpClientConnect begin addr='%s' name='%s' nodes=%zu models=%zu",
        address, gMpPendingClientProfile.name,
        gMpPendingClientProfile.inventory.size(), gMpPendingClientProfile.modelFiles.size());

    ENetHost* client = NetClientCreate();
    if (client == nullptr) {
        debugFilePrint("MP: MpClientConnect NetClientCreate failed");
        win_timed_msg("Could not create net client", COLOR_RED);
        return -1;
    }
    ENetPeer* peer = NetClientConnect(client, address, port);
    if (peer == nullptr) {
        debugFilePrint("MP: MpClientConnect NetClientConnect failed addr='%s'", address);
        NetHostDestroy(client);
        win_timed_msg("Connection failed", COLOR_RED);
        return -1;
    }

    gMpSession.enetHost = client;
    gMpSession.hostPeer = peer;
    gMpSession.state = MP_STATE_CLIENT_CONNECTING;
    gMpIsClient = true;
    gMpActive = true;

    // Pre-allocate netIdToObj for incoming map sync deltas.
    gMpSession.netIdToObjCapacity = MP_NETID_TO_OBJ_INITIAL_CAPACITY;
    gMpSession.netIdToObj = (Object**)calloc(gMpSession.netIdToObjCapacity, sizeof(Object*));
    gMpSession.netIdToObjCount = 0;

    return 0;
}

static void mpClientDisconnectNow(bool notifyPeer)
{
    if (!gMpIsClient) {
        return;
    }
    debugFilePrint("MP: client disconnect begin notify=%d", notifyPeer ? 1 : 0);

    // Tear down the combat mirror (card windows, UI restore, acting-NPC
    // outline) BEFORE the session teardown and the main-menu map unload. A
    // disconnect that lands mid-combat leaves the mirror alive, and the stale
    // card windows / outline can crash the transition to the main menu.
    MpCombatForceExit();

    if (gMpSession.hostPeer != nullptr) {
        if (notifyPeer) {
            NetSendPacket(gMpSession.hostPeer, NET_CHANNEL_RELIABLE, NET_PKT_DISCONNECT, nullptr, 0);
        }
        NetPeerDisconnect(gMpSession.hostPeer);
    }
    if (gMpSession.enetHost != nullptr) {
        NetHostDestroy(gMpSession.enetHost);
    }

    MpVoteReset();

    if (gMpSession.netIdToObj != nullptr) {
        free(gMpSession.netIdToObj);
        gMpSession.netIdToObj = nullptr;
        gMpSession.netIdToObjCapacity = 0;
        gMpSession.netIdToObjCount = 0;
    }

    MpProfileDestroyAllRuntimes();
    gMpProfileReceives.clear();

    mpZeroSession();
    gMpIsClient = false;
    gMpActive = false;
    debugFilePrint("MP: client disconnect done");
}

static void mpRequestClientDisconnect(bool notifyPeer)
{
    if (!gMpIsClient) {
        return;
    }
    gMpSession.clientDisconnectPending = true;
    gMpSession.clientDisconnectNotifyPeer = notifyPeer;
}

int MpClientDisconnect()
{
    if (!gMpIsClient) {
        return 0;
    }

    // Public callers are outside NetHostService. Network callbacks use
    // mpRequestClientDisconnect() and are finalized by MpTick instead.
    mpClientDisconnectNow(true);
    return 0;
}

// ---------------------------------------------------------------------------
// Per-tick
// ---------------------------------------------------------------------------

static void mpOnNetEvent(ENetPeer* peer, int eventType, const void* data, size_t dataLength, void* userData);

// Client-side deferred packets. Session-changing packets (profile applies,
// map changes, combat starts, votes) must never be applied while a modal
// blocks the main loop — an own-netId profile apply rebuilds the dude's
// inventory under an open inventory window and would dangle its item
// pointers. The receive dispatcher queues them here and MpTick drains them,
// and MpTick only runs when no modal is open.
struct MpDeferredPacket {
    ENetPeer* peer;
    std::vector<uint8_t> data;
};
static std::vector<MpDeferredPacket> gMpDeferredPackets;
// Set while MpTick drains the deferred queue. The drain re-dispatches through
// mpOnNetEvent, and WITHOUT this flag every packet would hit the deferral
// branch again and be re-queued at the tail forever (WELCOME never applies,
// the client stays stuck "connecting" and sees nothing). While set, the
// deferral branch falls through to the normal inline dispatch.
static bool gMpDrainingDeferredPackets = false;

// Ghost-item diagnostic: after a client DROP, watch the drop tile for 30s and
// dump its object list (2s throttle) so a surviving tile node can be caught.
static int gMpDropDiagTile = -1;
static int gMpDropDiagElevation = -1;
static int gMpDropDiagPid = 0;
static uint32_t gMpDropDiagDeadline = 0;
static uint32_t gMpDropDiagLastDump = 0;

// Network pump used by the blocking combat loops (multiplayer_combat.cc).
// Pumps the ENet host and dispatches incoming packets through the same
// callback as MpTick. Must never run scripts or map transitions.
void MpPumpNetwork()
{
    if (gMpActive && gMpSession.enetHost != nullptr) {
        NetHostService(gMpSession.enetHost, mpOnNetEvent, nullptr);
    }
}

static void mpHostRemovePlayer(MultiplayerPlayer* player)
{
    if (player == nullptr || !player->isConnected) {
        return;
    }

    uint8_t netId = player->netId;
    uint32_t objNetId = player->objNetId;
    bool wasHandshaken = player->isHandshaken;
    debugFilePrint("MP: host remove player netId=%u name='%s' handshaken=%d",
        netId, player->name, wasHandshaken ? 1 : 0);

    if (wasHandshaken && gVoteSession.state == VOTE_STATE_ACTIVE) {
        // A disconnected initiator cancels. A disconnected voter is an
        // immediate NO, so unanimity can never wait for a missing peer.
        if (netId == gVoteSession.initiatorNetId) {
            MpVoteCancel();
        } else if (netId >= 1 && netId <= NET_MAX_PLAYERS
            && gVoteSession.votes[netId - 1] == 0) {
            MpVoteCastVote(netId, 0);
        }
        if (gVoteSession.state == VOTE_STATE_ACTIVE && gVoteSession.totalPlayers > 0) {
            gVoteSession.totalPlayers--;
            if (gVoteSession.noCount == 0
                && gVoteSession.yesCount == gVoteSession.totalPlayers) {
                MpVoteResolve();
            }
        }
    }

    if (wasHandshaken) {
        NetPlayerLeftPayload left;
        left.netId = netId;
        left.objNetId = objNetId;
        NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE,
            NET_PKT_PLAYER_LEFT, &left, sizeof(left));
    }

    if (player->obj != nullptr) {
        gMpHostObjNetIds.erase(player->obj);
    }
    gMpProfileReceives.erase(netId);
    if (MpProfileGetRuntime(netId) != nullptr) {
        MpProfileDestroyRuntime(netId);
    } else if (player->obj != nullptr) {
        mpDestroyNetworkObject(player->obj);
    }

    memset(player, 0, sizeof(*player));
    if (wasHandshaken && gMpSession.numPlayers > 1) {
        gMpSession.numPlayers--;
    }
}

static void mpBroadcastProfileToClients(uint8_t netId, uint32_t objNetId,
    const MpPlayerProfile& profile)
{
    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        MultiplayerPlayer* other = &gMpSession.players[index];
        if (other->isConnected && other->isHandshaken && other->peer != nullptr) {
            mpSendProfile(other->peer, netId, objNetId, profile);
        }
    }
}

// Host-authoritative profile change detection. Runs once per tick; captures a
// model-free snapshot of each player's live state and compares it against the
// stored canonical profile. On change the profile is refreshed (model payload
// preserved when the model identity is unchanged) and re-broadcast to every
// client with an incremented generation.
static void mpHostSyncProfiles()
{
    if (!gMpIsHost || gMpSession.enetHost == nullptr) {
        return;
    }
    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        MultiplayerPlayer* player = &gMpSession.players[index];
        if (!player->isConnected || !player->profileReady || player->obj == nullptr) {
            continue;
        }
        uint8_t netId = player->netId;
        MpPlayerRuntime* runtime = MpProfileGetRuntime(netId);
        if (runtime == nullptr || runtime->object == nullptr) {
            continue;
        }

        MpPlayerProfile captured;
        bool ok = player->isLocal
            ? MpProfileCaptureLocalNoModel(&captured)
            : MpProfileCaptureObjectNoModel(runtime->object, &captured);
        if (!ok) {
            continue;
        }

        if (!player->isLocal) {
            // PC-only state lives in the profile, not in the avatar object.
            // Keep the authoritative stored values for the comparison.
            memcpy(captured.taggedSkills, runtime->profile.taggedSkills,
                sizeof(captured.taggedSkills));
            memcpy(captured.selectedTraits, runtime->profile.selectedTraits,
                sizeof(captured.selectedTraits));
            memcpy(captured.perkRanks, runtime->profile.perkRanks,
                sizeof(captured.perkRanks));
            memcpy(captured.killCounts, runtime->profile.killCounts,
                sizeof(captured.killCounts));
            memcpy(captured.skillUseTimes, runtime->profile.skillUseTimes,
                sizeof(captured.skillUseTimes));
            captured.sneakWorking = runtime->profile.sneakWorking;
            captured.editorLastLevel = runtime->profile.editorLastLevel;
            captured.editorHasFreePerk = runtime->profile.editorHasFreePerk;
            captured.remainingCharacterPoints = runtime->profile.remainingCharacterPoints;
        }

        MpPlayerProfile stored = runtime->profile;
        stored.modelFiles.clear();
        stored.modelHash = 0;
        MpPlayerProfile cmp = captured;
        cmp.modelFiles.clear();
        cmp.modelHash = 0;
        // Volatile runtime fields are owned by the per-tick player-state
        // channel (transform, fid, hp/ap/radiation/poison, combat results).
        // Exclude them from change detection so combat/movement never spams
        // full profile broadcasts; only persistent state changes trigger one.
        cmp.tile = stored.tile = -1;
        cmp.x = stored.x = 0;
        cmp.y = stored.y = 0;
        cmp.sx = stored.sx = 0;
        cmp.sy = stored.sy = 0;
        cmp.frame = stored.frame = 0;
        cmp.rotation = stored.rotation = ROTATION_NE;
        cmp.fid = stored.fid = 0;
        cmp.elevation = stored.elevation = 0;
        cmp.hp = stored.hp = 0;
        cmp.radiation = stored.radiation = 0;
        cmp.poison = stored.poison = 0;
        cmp.combatAp = stored.combatAp = 0;
        cmp.combatResults = stored.combatResults = 0;
        cmp.combatDamageLastTurn = stored.combatDamageLastTurn = 0;
        cmp.whoHitMeNetId = stored.whoHitMeNetId = 0;
        if (MpProfileHash(cmp) == MpProfileHash(stored)) {
            continue;
        }

        captured.generation = runtime->profile.generation + 1;
        if (captured.generation == 0) {
            captured.generation = 1;
        }

        bool modelIdentitySame = runtime->profile.modelName[0] != '\0'
            && strncmp(runtime->profile.modelName, captured.modelName,
                sizeof(captured.modelName)) == 0;
        if (player->isLocal) {
            if (modelIdentitySame) {
                captured.modelFiles = runtime->profile.modelFiles;
                captured.modelHash = runtime->profile.modelHash;
                captured.localModelIndex = runtime->profile.localModelIndex;
                MpProfileBindLocal(netId, captured, gDude);
            } else {
                MpPlayerProfile full;
                if (!MpProfileCaptureLocal(&full)) {
                    continue;
                }
                full.generation = captured.generation;
                MpProfileBindLocal(netId, full, gDude);
            }
        } else {
            MpProfileUpdateRuntime(netId, captured);
        }

        runtime = MpProfileGetRuntime(netId);
        if (runtime == nullptr) {
            continue;
        }
        strncpy(player->name, runtime->profile.name, NET_PEER_NAME_LENGTH - 1);
        player->name[NET_PEER_NAME_LENGTH - 1] = '\0';
        mpBroadcastProfileToClients(netId, player->objNetId, runtime->profile);
        debugFilePrint("MP: profile changed netId=%u generation=%u",
            netId, runtime->profile.generation);
    }
}

// Change-detection hash of the local character sheet. Volatile fields that
// ride the per-tick state channel (transform, hp/ap/radiation/poison, combat
// results) are excluded, mirroring the host's mpHostSyncProfiles comparison,
// so walking/fighting never triggers an upload — only real sheet changes do.
static uint32_t mpLocalProfileChangeHash(const MpPlayerProfile& captured)
{
    MpPlayerProfile cmp = captured;
    cmp.tile = -1;
    cmp.x = 0;
    cmp.y = 0;
    cmp.sx = 0;
    cmp.sy = 0;
    cmp.frame = 0;
    cmp.rotation = ROTATION_NE;
    cmp.fid = 0;
    cmp.elevation = 0;
    cmp.hp = 0;
    cmp.radiation = 0;
    cmp.poison = 0;
    cmp.combatAp = 0;
    cmp.combatResults = 0;
    cmp.combatDamageLastTurn = 0;
    cmp.whoHitMeNetId = 0;
    return MpProfileHash(cmp);
}

// Unified client->host character-sheet sync: capture the local sheet once a
// second and upload it through the profile channel when it changed. The host
// applies it onto the avatar in place (MpProfileApplyRuntimeUpdate), so
// level-up perks, spent skill points, looted/dropped items, script XP and
// stat changes all reach the host's combat resolution and every other client
// without any per-attribute protocol.
static void mpClientSyncLocalProfile()
{
    if (!gMpIsClient || !gMpActive || gMpSession.hostPeer == nullptr
        || gMpSession.state != MP_STATE_CLIENT_PLAYING) {
        return;
    }
    if (gMpLocalProfileSyncTick != 0
        && getTicksSince(gMpLocalProfileSyncTick) < 1000) {
        return;
    }
    gMpLocalProfileSyncTick = getTicks();

    uint8_t localNetId = gMpSession.localNetId;
    if (localNetId == 0 || localNetId > NET_MAX_PLAYERS) {
        return;
    }
    MultiplayerPlayer* player = &gMpSession.players[localNetId - 1];
    if (!player->isConnected || player->obj == nullptr) {
        return;
    }

    MpPlayerProfile captured;
    if (!MpProfileCaptureLocalNoModel(&captured)) {
        return;
    }

    uint32_t hash = mpLocalProfileChangeHash(captured);
    bool modelChanged = gMpLastUploadedModelName[0] == '\0'
        || strncmp(gMpLastUploadedModelName, captured.modelName,
            sizeof(captured.modelName)) != 0;
    if (gMpLocalProfileSyncReady
        && hash == gMpLastUploadedProfileHash && !modelChanged) {
        return;
    }

    if (!gMpLocalProfileSyncReady) {
        // First detection after join: the join-time upload is the baseline —
        // never immediately re-upload an identical sheet.
        gMpLastUploadedProfileHash = hash;
        strncpy(gMpLastUploadedModelName, captured.modelName,
            sizeof(gMpLastUploadedModelName));
        gMpLastUploadedModelName[sizeof(gMpLastUploadedModelName) - 1] = '\0';
        gMpLocalProfileSyncReady = true;
        return;
    }

    // Build the upload. Same-model updates keep the installed model payload
    // (the host preserves it); a model identity change (armor swap) requires
    // the full capture so the new files ride along.
    MpPlayerProfile upload = captured;
    if (modelChanged) {
        if (!MpProfileCaptureLocal(&upload)) {
            debugFilePrint("MP: local profile upload model capture failed");
            return;
        }
    }
    upload.generation = player->profileGeneration + 1;
    if (upload.generation == 0) {
        upload.generation = 1;
    }
    if (!mpSendProfile(gMpSession.hostPeer, localNetId, player->objNetId, upload)) {
        debugFilePrint("MP: local profile upload failed netId=%u gen=%u",
            localNetId, upload.generation);
        return;
    }
    // Optimistically advance the slot's generation so the next upload carries
    // a strictly higher generation (the host rejects gen <= its own). Echo
    // broadcasts of this generation bump it further — either way it only
    // ever climbs.
    player->profileGeneration = upload.generation;
    gMpLastUploadedProfileHash = hash;
    strncpy(gMpLastUploadedModelName, captured.modelName,
        sizeof(gMpLastUploadedModelName));
    gMpLastUploadedModelName[sizeof(gMpLastUploadedModelName) - 1] = '\0';
    debugFilePrint("MP: local profile uploaded netId=%u gen=%u hash=%08X modelChanged=%d",
        localNetId, upload.generation, hash, modelChanged ? 1 : 0);
}

void MpTick()
{
    if (!gMpActive) {
        return;
    }
    bool logFirstHostTick = gMpIsHost && gMpHostFirstTickPending;
    if (logFirstHostTick) {
        debugFilePrint("MP: first host tick begin");
    }

    // Drain client-deferred packets (queued while a modal blocked the main
    // loop). No modal is open now, so the applies are safe. New deferrals
    // from NetHostService below go to the tail and drain next tick.
    size_t deferredCount = gMpDeferredPackets.size();
    if (deferredCount > 0) {
        gMpDrainingDeferredPackets = true;
        for (size_t index = 0; index < deferredCount; index++) {
            MpDeferredPacket& pkt = gMpDeferredPackets[index];
            mpOnNetEvent(pkt.peer, 3, pkt.data.data(), pkt.data.size(), nullptr);
        }
        gMpDrainingDeferredPackets = false;
        gMpDeferredPackets.erase(gMpDeferredPackets.begin(),
            gMpDeferredPackets.begin() + deferredCount);
        debugFilePrint("MP: drained deferred packets count=%zu", deferredCount);
    }

    NetHostService(gMpSession.enetHost, mpOnNetEvent, nullptr);

    if (gMpSession.clientDisconnectPending) {
        bool notifyPeer = gMpSession.clientDisconnectNotifyPeer;
        mpClientDisconnectNow(notifyPeer);
        return;
    }

    // Co-op combat: deferred starts/turns, end-request drain, card refresh.
    MpCombatTick();

    if (gMpIsHost) {
        if (gVoteSession.state == VOTE_STATE_ACTIVE
            && gVoteSession.initiatorNetId >= 1
            && gVoteSession.initiatorNetId <= NET_MAX_PLAYERS) {
            MultiplayerPlayer* initiator = &gMpSession.players[gVoteSession.initiatorNetId - 1];
            if (initiator->isConnected && initiator->obj != nullptr) {
                // The transition hook can fire in the middle of an animation
                // step. Clear the remaining path on the next safe tick so the
                // initiator stays on the exit tile while voting.
                reg_anim_clear(initiator->obj);
            }
        }
        MpAssignNetIdsToAllObjects();
        if (logFirstHostTick) {
            debugFilePrint("MP: first host tick after object assignment");
        }
        MpBroadcastObjectStates();
        if (logFirstHostTick) {
            debugFilePrint("MP: first host tick after object broadcast");
        }
        mpHostSyncProfiles();
        if (!gMpHostTileBaseline.empty()) {
            bool tilesChanged = false;
            for (int elevation = 0; elevation < ELEVATION_COUNT && !tilesChanged; elevation++) {
                if (_square[elevation] == nullptr) {
                    continue;
                }
                const int32_t* tiles = _square[elevation]->field_0;
                size_t offset = (size_t)elevation * SQUARE_GRID_SIZE;
                for (int tile = 0; tile < SQUARE_GRID_SIZE; tile++) {
                    if (gMpHostTileBaseline[offset + tile] != tiles[tile]) {
                        tilesChanged = true;
                        break;
                    }
                }
            }
            if (tilesChanged) {
                MpBroadcastMapFullSync(nullptr);
            }
        }
        MpBroadcastPlayerStates();
        if (logFirstHostTick) {
            debugFilePrint("MP: first host tick after player broadcast");
            gMpHostFirstTickPending = false;
        }
        // Vote housekeeping: check timeout and resolve the vote if its
        // window has elapsed.
        MpVoteCheckTimeout();
    } else {
        // Sync watchdog: a stuck sync (host crash mid-change, lost host) must
        // not leave the client frozen in CLIENT_SYNCING forever. Bail to the
        // main menu once the deadline passes.
        if (gMpSession.state == MP_STATE_CLIENT_SYNCING
            && getTicksSince(gMpSession.clientSyncStartTick) >= MP_SYNC_TIMEOUT_MS) {
            debugFilePrint("MP: client sync timed out state=%d waited=%u ms",
                gMpSession.state, (unsigned)getTicksSince(gMpSession.clientSyncStartTick));
            win_timed_msg("Synchronization timed out", COLOR_RED);
            mpRequestClientDisconnect(false);
            _game_user_wants_to_quit = GAME_QUIT_REQUEST_MAIN_MENU;
        }
        // Host-death watchdog: in steady state the host broadcasts every tick,
        // so a silent gap far longer than that means the host is gone (crashed
        // hard — ENet only reports it after its own ~30s peer timeout).
        // While combat is active the host's broadcasts are delta-synced, so a
        // player sitting idle on their turn produces NO packets for a long
        // time — a 12s gap is perfectly normal. Skip the watchdog during
        // combat; ENet's own peer timeout still catches a truly dead host.
        if (gMpSession.state == MP_STATE_CLIENT_PLAYING
            && !MpCombatIsActive()
            && getTicksSince(gMpSession.lastHostPacketTime) >= MP_HOST_DEAD_TIMEOUT_MS) {
            debugFilePrint("MP: host heartbeat lost state=%d silent=%u ms",
                gMpSession.state, (unsigned)getTicksSince(gMpSession.lastHostPacketTime));
            win_timed_msg("Host disconnected", COLOR_RED);
            mpRequestClientDisconnect(false);
            _game_user_wants_to_quit = GAME_QUIT_REQUEST_MAIN_MENU;
        }
        mpEnsureClientPlayersVisible();
        // Unified character-sheet sync: push local sheet changes (XP, perks,
        // skills, items, stats) up to the host through the profile channel.
        mpClientSyncLocalProfile();
    }
    // Show the vote modal if one is pending (host and client alike). This
    // blocks while the modal is up, so it must run at top level — never from
    // inside a NetHostService callback.
    MpVoteMaybeShowUI();
    // Refresh the vote modal's countdown/tallies once per frame so it ticks.
    // No-op when no vote UI is open.
    MpVoteUpdateUI();

    // Remote critters can arrive on tiles whose dirty rects nobody marked;
    // nothing redraws them until the mouse happens to refresh the view. Do a
    // cheap throttled full refresh so players always appear in a timely
    // fashion even when nothing else changes.
    if (gMpLastTileRefreshTick == 0
        || getTicksSince(gMpLastTileRefreshTick) >= MP_TILE_REFRESH_INTERVAL_MS) {
        gMpLastTileRefreshTick = getTicks();
        tileWindowRefreshFull();
    }
}

// ---------------------------------------------------------------------------
// Network event dispatch (host & client)
// ---------------------------------------------------------------------------

static void mpOnNetEvent(ENetPeer* peer, int eventType, const void* data, size_t dataLength, void* /*userData*/)
{
    if (gMpIsHost) {
        switch (eventType) {
        case 1: { // connect
            uint8_t netId = mpAllocPlayerSlot(peer);
            if (netId == 0) {
                // Server is full — kick immediately.
                NetSendPacket(peer, NET_CHANNEL_RELIABLE, NET_PKT_KICK, nullptr, 0);
                NetPeerDisconnect(peer);
                return;
            }
            MultiplayerPlayer* p = &gMpSession.players[netId - 1];
            memset(p, 0, sizeof(*p));
            p->netId = netId;
            p->peer = peer;
            p->isLocal = false;
            p->isConnected = true;
            p->isHandshaken = false;

            // We won't spawn a critter for the new client yet — we learn their
            // version hash from the HELLO packet and finalize in the receive
            // dispatch below.
            win_timed_msg("Player connecting...", COLOR_GREEN);
            break;
        }
        case 2: { // disconnect
            MultiplayerPlayer* p = mpPlayerFindByPeer(peer);
            if (p != nullptr) {
                mpHostRemovePlayer(p);
            }
            win_timed_msg("Player left", COLOR_RED);
            break;
        }
        case 3: { // receive
            uint8_t packetType;
            const void* packetPayload;
            size_t packetPayloadLength;
            if (!mpDecodePacket(data, dataLength, &packetType, &packetPayload, &packetPayloadLength)) {
                return;
            }
            // Reliable-channel traffic only; the per-tick unreliable state
            // updates (100/101) would spam this log to uselessness.
            if (packetType <= 32) {
                debugFilePrint("MP: raw receive type=%u len=%zu", packetType, packetPayloadLength);
            }
            const void* payload = packetPayload;
            size_t payloadLen = packetPayloadLength;
            switch (packetType) {
            case NET_PKT_HELLO: {
                if (payloadLen != sizeof(NetHelloPayload)) {
                    debugFilePrint("MP: hello bad length=%zu", payloadLen);
                    return;
                }
                const NetHelloPayload* hello = (const NetHelloPayload*)payload;
                debugFilePrint("MP: hello received name='%s' versionHash=%08X want=%08X",
                    hello->peerName, hello->versionHash, NetGetVersionHash());
                if (hello->versionHash != NetGetVersionHash()) {
                    debugFilePrint("MP: hello version mismatch kick peer=%p", (void*)peer);
                    NetSendPacket(peer, NET_CHANNEL_RELIABLE, NET_PKT_KICK, nullptr, 0);
                    NetPeerDisconnect(peer);
                    return;
                }
                MultiplayerPlayer* p = mpPlayerFindByPeer(peer);
                if (p == nullptr) {
                    return;
                }
                if (p->isHandshaken) {
                    return;
                }
                strncpy(p->name, hello->peerName, NET_PEER_NAME_LENGTH - 1);
                p->name[NET_PEER_NAME_LENGTH - 1] = '\0';
                break;
            }
            case NET_PKT_PLAYER_PROFILE_BEGIN:
                mpHandleProfileBegin(peer, payload, payloadLen);
                break;
            case NET_PKT_PLAYER_PROFILE_CHUNK:
                mpHandleProfileChunk(peer, payload, payloadLen);
                break;
            case NET_PKT_PLAYER_PROFILE_END:
                mpHandleProfileEnd(peer, payload, payloadLen);
                break;
            case NET_PKT_PLAYER_INPUT: {
                if (payloadLen != sizeof(NetPlayerInputPayload)) {
                    return;
                }
                const NetPlayerInputPayload* in = (const NetPlayerInputPayload*)payload;
                MultiplayerPlayer* p = mpPlayerFindByPeer(peer);
                if (p == nullptr || !p->isHandshaken || p->obj == nullptr) {
                    return;
                }
                if (!hexGridTileIsValid(in->tile) || !elevationIsValid(in->elevation)
                    || in->isRun > 1) {
                    return;
                }
                if (gMpSession.initiatorFrozen && p->netId == gVoteSession.initiatorNetId && gVoteSession.state == VOTE_STATE_ACTIVE) {
                    // Initiator is frozen pending the vote; drop their move
                    // commands until the vote resolves.
                    return;
                }
                mpHostRegisterPlayerMovement(p->obj, in->isRun != 0, in->tile, in->elevation);
                break;
            }
            case NET_PKT_PLAYER_ACTION: {
                if (payloadLen != sizeof(NetPlayerActionPayload)) {
                    return;
                }

                const NetPlayerActionPayload* action = (const NetPlayerActionPayload*)payload;
                MultiplayerPlayer* p = mpPlayerFindByPeer(peer);
                if (p == nullptr || !p->isHandshaken || p->obj == nullptr) {
                    return;
                }

                if (action->action == NET_PLAYER_ACTION_WALK
                    || action->action == NET_PLAYER_ACTION_RUN) {
                    if (!hexGridTileIsValid(action->tile)
                        || !elevationIsValid(action->elevation)) {
                        return;
                    }

                    mpHostRegisterPlayerMovement(p->obj,
                        action->action == NET_PLAYER_ACTION_RUN,
                        action->tile, action->elevation);
                    break;
                }

                if (action->action == NET_PLAYER_ACTION_DROP) {
                    // Client dropped an item from its own inventory. Pull the
                    // matching item (by pid) from the avatar's inventory and
                    // drop it on the ground at the avatar's tile; the ground
                    // object then streams to every client via the object sync,
                    // and the sheet channel reconciles the inventories.
                    if (p->obj != nullptr) {
                        Object* item = nullptr;
                        for (int itemIndex = 0;
                            itemIndex < p->obj->data.inventory.length; itemIndex++) {
                            Object* candidate = p->obj->data.inventory.items[itemIndex].item;
                            if (candidate != nullptr
                                && candidate->pid == (int)action->targetNetId) {
                                item = candidate;
                                break;
                            }
                        }
                        if (item != nullptr) {
                            debugFilePrint("MP: drop action netId=%u pid=0x%X tile=%d",
                                p->netId, item->pid, p->obj->tile);
                            // Ghost-spear diagnostic: watch the drop tile for
                            // 30s so the next test shows whether a tile node
                            // survives the pickup.
                            gMpDropDiagTile = p->obj->tile;
                            gMpDropDiagElevation = p->obj->elevation;
                            gMpDropDiagPid = item->pid;
                            gMpDropDiagDeadline = getTicks() + 30000;
                            gMpDropDiagLastDump = 0;
                            objectDrop(p->obj, item);
                        } else {
                            debugFilePrint("MP: drop action no item netId=%u pid=0x%X",
                                p->netId, action->targetNetId);
                        }
                    }
                    break;
                }

                Object* target = mpHostFindObjectByNetId(action->targetNetId);
                if (target == nullptr) {
                    return;
                }

                switch (action->action) {
                case NET_PLAYER_ACTION_INSPECT:
                    if (objectExamine(p->obj, target) == -1) {
                        objectLookAt(p->obj, target);
                    }
                    break;
                case NET_PLAYER_ACTION_TALK:
                    actionTalk(p->obj, target);
                    break;
                case NET_PLAYER_ACTION_TOUCH:
                    _action_use_an_object(p->obj, target);
                    break;
                case NET_PLAYER_ACTION_PICK_UP:
                    actionPickUp(p->obj, target);
                    break;
                case NET_PLAYER_ACTION_LOOT:
                    actionLootCritter(p->obj, target);
                    break;
                case NET_PLAYER_ACTION_USE_SKILL:
                    if (skillIsValid(action->skill)) {
                        actionUseSkill(p->obj, target, (Skill)action->skill);
                    }
                    break;
                case NET_PLAYER_ACTION_PUSH:
                    actionPush(p->obj, target);
                    break;
                case NET_PLAYER_ACTION_ROTATE:
                    if (target == p->obj) {
                        objectRotateClockwise(p->obj, nullptr);
                    }
                    break;
                default:
                    break;
                }
                break;
            }
            case NET_PKT_DISCONNECT: {
                MultiplayerPlayer* p = mpPlayerFindByPeer(peer);
                if (p != nullptr) {
                    mpHostRemovePlayer(p);
                }
                NetPeerDisconnect(peer);
                break;
            }
            case NET_PKT_VOTE_START_REQUEST: {
                if (payloadLen != sizeof(NetVoteStartRequestPayload)) {
                    return;
                }
                MultiplayerPlayer* p = mpPlayerFindByPeer(peer);
                if (p == nullptr || !p->isHandshaken) {
                    return;
                }
                if (gVoteSession.state != VOTE_STATE_NONE && gVoteSession.state != VOTE_STATE_ACTIVE) {
                    // A vote already concluded (resolving) — drop the request.
                    return;
                }
                if (gVoteSession.state == VOTE_STATE_ACTIVE) {
                    // Already voting — drop duplicates.
                    return;
                }
                // Translate the network request into a local MapTransition
                // struct and start the vote with the sender as initiator.
                MapTransition t;
                const NetVoteStartRequestPayload* in = (const NetVoteStartRequestPayload*)payload;
                t.map = in->targetMap;
                t.tile = in->targetTile;
                t.elevation = in->targetElevation;
                t.rotation = in->targetRotation;
                MpVoteStart(&t, p->netId);
                break;
            }
            case NET_PKT_VOTE_CAST: {
                if (payloadLen != sizeof(NetVoteCastPayload)) {
                    return;
                }
                const NetVoteCastPayload* in = (const NetVoteCastPayload*)payload;
                MultiplayerPlayer* p = mpPlayerFindByPeer(peer);
                if (p == nullptr || !p->isHandshaken || in->voterNetId != p->netId) {
                    return;
                }
                MpVoteCastVote(in->voterNetId, in->vote);
                break;
            }
            case NET_PKT_COMBAT_START_REQUEST: {
                if (payloadLen != sizeof(NetCombatStartRequestPayload)) {
                    debugFilePrint("MP: combat start request bad length=%zu", payloadLen);
                    return;
                }
                MultiplayerPlayer* p = mpPlayerFindByPeer(peer);
                if (p == nullptr || !p->isHandshaken || p->obj == nullptr) {
                    return;
                }
                const NetCombatStartRequestPayload* req = (const NetCombatStartRequestPayload*)payload;
                MpCombatOnStartRequest(p->obj, req->targetNetId);
                break;
            }
            case NET_PKT_COMBAT_CMD: {
                if (payloadLen != sizeof(NetCombatCmdPayload)) {
                    return;
                }
                MultiplayerPlayer* p = mpPlayerFindByPeer(peer);
                if (p == nullptr || !p->isHandshaken) {
                    return;
                }
                MpCombatOnCmd(p->netId, (const NetCombatCmdPayload*)payload);
                break;
            }
            case NET_PKT_COMBAT_TURN_END: {
                MultiplayerPlayer* p = mpPlayerFindByPeer(peer);
                if (p == nullptr || !p->isHandshaken) {
                    return;
                }
                MpCombatOnTurnEnd(p->netId);
                break;
            }
            case NET_PKT_COMBAT_END_REQUEST: {
                MultiplayerPlayer* p = mpPlayerFindByPeer(peer);
                if (p == nullptr || !p->isHandshaken) {
                    return;
                }
                MpCombatOnEndRequest(p->netId);
                break;
            }
            case NET_PKT_COMBAT_STARTED: {
                MpCombatOnStartedPacket();
                break;
            }
            case NET_PKT_COMBAT_TURN_START: {
                if (payloadLen != sizeof(NetCombatTurnStartPayload)) {
                    return;
                }
                MpCombatOnTurnStart((const NetCombatTurnStartPayload*)payload);
                break;
            }
            case NET_PKT_COMBAT_END_DENIED: {
                MpCombatOnEndDenied();
                break;
            }
            case NET_PKT_COMBAT_ENDED: {
                MpCombatOnEndedPacket();
                break;
            }
            default:
                break;
            }
            break;
        }
        default:
            break;
        }
        return;
    }

    // ----- client -----
    switch (eventType) {
    case 1: { // connect to host
        debugFilePrint("MP: client connected to host, sending HELLO");
        NetHelloPayload hello;
        hello.versionHash = NetGetVersionHash();
        memset(hello.peerName, 0, sizeof(hello.peerName));
        if (gMpPendingClientProfileValid) {
            strncpy(hello.peerName, gMpPendingClientProfile.name,
                NET_PEER_NAME_LENGTH - 1);
        } else {
            strncpy(hello.peerName, "Player", NET_PEER_NAME_LENGTH - 1);
        }
        if (!NetSendPacket(peer, NET_CHANNEL_RELIABLE, NET_PKT_HELLO, &hello, sizeof(hello))
            || !gMpPendingClientProfileValid
            || !mpSendProfile(peer, 0, 0, gMpPendingClientProfile)) {
            debugFilePrint("MP: client hello/profile upload failed, disconnecting");
            mpRequestClientDisconnect(false);
        }
        break;
    }
    case 2: { // disconnect (host gone)
        debugFilePrint("MP: client lost host connection");
        win_timed_msg("Host disconnected", COLOR_RED);
        mpRequestClientDisconnect(false);
        _game_user_wants_to_quit = GAME_QUIT_REQUEST_MAIN_MENU;
        break;
    }
    case 3: { // receive
        // Any host packet proves the host is alive; the host-death watchdog
        // in MpTick uses this stamp to bail to the main menu when the host
        // crashes hard (ENet only notices a dead peer after its ~30s timeout).
        gMpSession.lastHostPacketTime = getTicks();
        uint8_t packetType;
        const void* packetPayload;
        size_t packetPayloadLength;
        if (!mpDecodePacket(data, dataLength, &packetType, &packetPayload, &packetPayloadLength)) {
            return;
        }
        // Co-op modal safety: session-changing packets are deferred to
        // MpTick, which never runs while a modal is open. Per-tick state
        // packets (player/object states, object removed) apply immediately —
        // they are idempotent and safe under a modal.
        switch (packetType) {
        case NET_PKT_WELCOME:
        case NET_PKT_MAP_FULL_SYNC:
        case NET_PKT_MAP_TILE_SYNC:
        case NET_PKT_MAP_CHANGED:
        case NET_PKT_MAP_CHANGE_ABORT:
        case NET_PKT_VOTE_START:
        case NET_PKT_VOTE_RESULT:
        case NET_PKT_VOTE_TALLY:
        case NET_PKT_COMBAT_STARTED:
        case NET_PKT_COMBAT_TURN_START:
        case NET_PKT_COMBAT_END_DENIED:
        case NET_PKT_COMBAT_ENDED:
        case NET_PKT_PLAYER_JOINED:
        case NET_PKT_PLAYER_LEFT:
        case NET_PKT_PLAYER_PROFILE_BEGIN:
        case NET_PKT_PLAYER_PROFILE_CHUNK:
        case NET_PKT_PLAYER_PROFILE_END:
        case NET_PKT_DISCONNECT:
        case NET_PKT_KICK: {
            if (gMpDrainingDeferredPackets) {
                // Re-dispatch from MpTick's drain: apply inline (no modal is
                // open) instead of re-queuing at the tail forever.
                break;
            }
            MpDeferredPacket deferred;
            deferred.peer = peer;
            deferred.data.assign(static_cast<const uint8_t*>(data),
                static_cast<const uint8_t*>(data) + dataLength);
            gMpDeferredPackets.push_back(std::move(deferred));
            if (packetType != NET_PKT_PLAYER_PROFILE_CHUNK) {
                debugFilePrint("MP: packet deferred type=%u queue=%zu",
                    packetType, gMpDeferredPackets.size());
            }
            return;
        }
        default:
            break;
        }
        const void* payload = packetPayload;
        size_t payloadLen = packetPayloadLength;
        switch (packetType) {
        case NET_PKT_PLAYER_PROFILE_BEGIN:
            mpHandleProfileBegin(peer, payload, payloadLen);
            break;
        case NET_PKT_PLAYER_PROFILE_CHUNK:
            mpHandleProfileChunk(peer, payload, payloadLen);
            break;
        case NET_PKT_PLAYER_PROFILE_END:
            mpHandleProfileEnd(peer, payload, payloadLen);
            break;
        case NET_PKT_PLAYER_PROFILE_REJECT: {
            if (payloadLen != sizeof(NetPlayerProfileAckPayload)) return;
            const NetPlayerProfileAckPayload* ack = (const NetPlayerProfileAckPayload*)payload;
            if (ack->accepted == 0) {
                win_timed_msg("Host rejected player profile", COLOR_RED);
                mpRequestClientDisconnect(false);
                _game_user_wants_to_quit = GAME_QUIT_REQUEST_MAIN_MENU;
            }
            break;
        }
        case NET_PKT_WELCOME: {
            if (payloadLen != sizeof(NetWelcomePayload)) {
                return;
            }
            const NetWelcomePayload* w = (const NetWelcomePayload*)payload;
            if (w->assignedNetId == 0 || w->assignedNetId > NET_MAX_PLAYERS) {
                debugFilePrint("MP: welcome bad netId=%u", w->assignedNetId);
                return;
            }
            debugFilePrint("MP: welcome received netId=%u objNet=%u map=%d",
                w->assignedNetId, w->objNetId, w->map.mapId);
            gMpSession.localNetId = w->assignedNetId;
            gMpSession.currentMapId = w->map.mapId;
            // Set up the player-slot for self.
            uint8_t selfIdx = (uint8_t)(w->assignedNetId - 1);
            if (selfIdx < NET_MAX_PLAYERS) {
                MultiplayerPlayer* p = &gMpSession.players[selfIdx];
                memset(p, 0, sizeof(*p));
                p->netId = w->assignedNetId;
                p->isLocal = true;
                p->isConnected = true;
                p->peer = nullptr;
                p->obj = gDude;
                p->isHandshaken = true;
                p->profileReady = gMpPendingClientProfileValid;
                p->hasInitialState = false;
                p->profileGeneration = gMpPendingClientProfileValid
                    ? gMpPendingClientProfile.generation
                    : 0;
                p->objNetId = w->objNetId;
                if (gDude != nullptr && gMpPendingClientProfileValid) {
                    MpProfileBindLocal(w->assignedNetId, gMpPendingClientProfile, gDude);
                    MpRegisterObjNetId(gDude, w->objNetId);
                }
            }
            // Mark local player slot as connected in gMpSession.numPlayers so
            // host broadcasts include us — but the count lives on host, not
            // here. Client never uses numPlayers.
            gMpSession.numPlayers = selfIdx + 1;
            debugFilePrint("MPDBG welcome: dude=%p pid=0x%X pt=%d tile=%d elev=%d hidden=%d st=%d carry=%d weight=%d",
                (void*)gDude,
                gDude != nullptr ? gDude->pid : 0,
                gDude != nullptr ? PID_TYPE(gDude->pid) : -1,
                gDude != nullptr ? gDude->tile : -1,
                gDude != nullptr ? gDude->elevation : -1,
                gDude != nullptr ? ((gDude->flags & OBJECT_HIDDEN) != 0) : -1,
                gDude != nullptr ? critterGetStat(gDude, STAT_STRENGTH) : -1,
                gDude != nullptr ? critterGetStat(gDude, STAT_CARRY_WEIGHT) : -1,
                gDude != nullptr ? objectGetInventoryWeight(gDude) : -1);
            if (mpClientLoadMap(&w->map) != 0) {
                win_timed_msg("Could not load the host map", COLOR_RED);
                mpRequestClientDisconnect(false);
                _game_user_wants_to_quit = GAME_QUIT_REQUEST_MAIN_MENU;
                return;
            }
            mpEnterClientSync();
            break;
        }
        case NET_PKT_MAP_FULL_SYNC: {
            MpApplyMapFullSync(payload, payloadLen);
            break;
        }
        case NET_PKT_MAP_TILE_SYNC: {
            MpApplyMapTileSync(payload, payloadLen);
            break;
        }
        case NET_PKT_PLAYER_STATE_UPDATE: {
            if (payloadLen != sizeof(NetPlayerStateUpdatePayload)) {
                return;
            }
            if (gMpSession.state != MP_STATE_CLIENT_SYNCING
                && gMpSession.state != MP_STATE_CLIENT_PLAYING) {
                return;
            }
            MpApplyPlayerState((const NetPlayerStateUpdatePayload*)payload);
            break;
        }
        case NET_PKT_OBJECT_STATE_UPDATE: {
            if (payloadLen != sizeof(NetMapFullSyncObjectPayload)
                || gMpSession.state != MP_STATE_CLIENT_PLAYING) {
                return;
            }
            MpApplyObjectState((const NetMapFullSyncObjectPayload*)payload);
            break;
        }
        case NET_PKT_PLAYER_JOINED: {
            if (payloadLen != sizeof(NetPlayerJoinedPayload)) {
                return;
            }
            MpApplyPlayerJoined((const NetPlayerJoinedPayload*)payload);
            break;
        }
        case NET_PKT_PLAYER_LEFT: {
            if (payloadLen != sizeof(NetPlayerLeftPayload)) {
                return;
            }
            MpApplyPlayerLeft((const NetPlayerLeftPayload*)payload);
            break;
        }
        case NET_PKT_OBJECT_REMOVED: {
            if (payloadLen != sizeof(uint32_t)) {
                return;
            }
            uint32_t netId;
            memcpy(&netId, payload, sizeof(netId));
            MpApplyObjectRemoved(netId);
            break;
        }
        case NET_PKT_MAP_CHANGED: {
            if (payloadLen != sizeof(NetMapChangedPayload)) {
                return;
            }
            const NetMapChangedPayload* m = (const NetMapChangedPayload*)payload;
            MpApplyMapChanged(&m->map);
            break;
        }
        case NET_PKT_MAP_CHANGE_ABORT: {
            // The host could not load the target map. The session stays on the
            // current map; the client must not sit in CLIENT_SYNCING waiting
            // for a MAP_CHANGED that will never arrive.
            debugFilePrint("MP: map change abort received state=%d map=%d",
                gMpSession.state, gMpSession.currentMapId);
            if (gMpSession.state == MP_STATE_CLIENT_SYNCING) {
                // Only reachable if a MAP_CHANGED was never applied (the
                // client only enters SYNCING via MpApplyMapChanged, and the
                // host only aborts before broadcasting MAP_CHANGED).
                gMpSession.state = MP_STATE_CLIENT_PLAYING;
            }
            win_timed_msg("The host could not change the map", COLOR_RED);
            break;
        }
        case NET_PKT_KICK: {
            win_timed_msg("Kicked by host", COLOR_RED);
            mpRequestClientDisconnect(false);
            _game_user_wants_to_quit = GAME_QUIT_REQUEST_MAIN_MENU;
            break;
        }
        case NET_PKT_DISCONNECT: {
            win_timed_msg("Host disconnected", COLOR_RED);
            mpRequestClientDisconnect(false);
            _game_user_wants_to_quit = GAME_QUIT_REQUEST_MAIN_MENU;
            break;
        }
        case NET_PKT_VOTE_START: {
            if (payloadLen != sizeof(NetVoteStartPayload)) {
                return;
            }
            MpVoteOnVoteStart((const NetVoteStartPayload*)payload);
            break;
        }
        case NET_PKT_VOTE_RESULT: {
            if (payloadLen != sizeof(NetVoteResultPayload)) {
                return;
            }
            MpVoteOnVoteResult((const NetVoteResultPayload*)payload);
            break;
        }
        case NET_PKT_VOTE_TALLY: {
            if (payloadLen != sizeof(NetVoteTallyPayload)) {
                return;
            }
            MpVoteOnVoteTally((const NetVoteTallyPayload*)payload);
            break;
        }
        case NET_PKT_COMBAT_STARTED: {
            MpCombatOnStartedPacket();
            break;
        }
        case NET_PKT_COMBAT_TURN_START: {
            if (payloadLen != sizeof(NetCombatTurnStartPayload)) {
                return;
            }
            MpCombatOnTurnStart((const NetCombatTurnStartPayload*)payload);
            break;
        }
        case NET_PKT_COMBAT_END_DENIED: {
            MpCombatOnEndDenied();
            break;
        }
        case NET_PKT_COMBAT_ENDED: {
            MpCombatOnEndedPacket();
            break;
        }
        case NET_PKT_COMBAT_MESSAGE: {
            if (payloadLen == 0 || payloadLen > 256) {
                debugFilePrint("MP: combat message bad length=%zu", payloadLen);
                return;
            }
            MpCombatOnMonitorMessage((const char*)payload, payloadLen);
            break;
        }
        default:
            break;
        }
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Input (client → host)
// ---------------------------------------------------------------------------

void MpSendPlayerAction(uint8_t action, uint32_t targetNetId, int32_t tile, int32_t elevation, uint8_t skill)
{
    if (!gMpIsClient || gMpSession.hostPeer == nullptr) {
        return;
    }
    // While syncing (initial join or map change) the local player has no
    // authoritative critter: the map is being rebuilt around him and a click
    // carries stale-map coordinates that would apply to a freshly respawned
    // avatar on the host. Drop actions until PLAYING resumes.
    if (gMpSession.state != MP_STATE_CLIENT_PLAYING) {
        debugFilePrint("MP: player action dropped state=%d action=%d tile=%d elev=%d",
            gMpSession.state, action, tile, elevation);
        return;
    }
    NetPlayerActionPayload p;
    memset(&p, 0, sizeof(p));
    p.action = action;
    p.skill = skill;
    p.targetNetId = targetNetId;
    p.tile = tile;
    p.elevation = elevation;
    NetSendPacket(gMpSession.hostPeer, NET_CHANNEL_RELIABLE, NET_PKT_PLAYER_ACTION, &p, sizeof(p));
}

// ---------------------------------------------------------------------------
// Broadcast (host → clients)
// ---------------------------------------------------------------------------

void MpBroadcastPlayerStates()
{
    if (!gMpIsHost || gMpSession.enetHost == nullptr) {
        return;
    }
    // Heartbeat: the per-player delta means an idle host broadcasts NOTHING.
    // Any silence longer than the client's host-death watchdog (12s) kicks
    // the client even when the host is fine (e.g. sitting in a modal that
    // still pumps, or simply idle). Force-broadcast every 2s so the client
    // always hears the host.
    static uint32_t lastHeartbeatTick = 0;
    bool force = getTicksSince(lastHeartbeatTick) >= 2000;
    if (force) {
        lastHeartbeatTick = getTicks();
    }
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        MultiplayerPlayer* p = &gMpSession.players[i];
        if (!p->isConnected || p->obj == nullptr) {
            continue;
        }
        Object* o = p->obj;
        NetPlayerStateUpdatePayload s;
        s.netId = p->netId;
        s.objNetId = p->objNetId;
        s.tile = o->tile;
        s.x = o->x;
        s.y = o->y;
        s.rotation = o->rotation;
        s.fid = o->fid;
        s.frame = o->frame;
        s.elevation = o->elevation;
        s.hp = o->data.critter.hp;
        s.ap = o->data.critter.combat.ap;
        s.radiation = o->data.critter.radiation;
        s.poison = o->data.critter.poison;
        s.combatResults = o->data.critter.combat.results;

        if (!force
            && p->hasLastState
            && p->lastTile == s.tile
            && p->lastX == s.x
            && p->lastY == s.y
            && p->lastRotation == s.rotation
            && p->lastFid == s.fid
            && p->lastFrame == s.frame
            && p->lastElevation == s.elevation
            && p->lastHp == s.hp
            && p->lastAp == s.ap
            && p->lastRadiation == s.radiation
            && p->lastPoison == s.poison
            && p->lastCombatResults == s.combatResults) {
            continue;
        }
        int channel = p->hasLastState ? NET_CHANNEL_UNRELIABLE : NET_CHANNEL_RELIABLE;
        NetBroadcastPacket(gMpSession.enetHost, channel,
            NET_PKT_PLAYER_STATE_UPDATE, &s, sizeof(s));
        p->lastTile = s.tile;
        p->lastX = s.x;
        p->lastY = s.y;
        p->lastRotation = s.rotation;
        p->lastFid = s.fid;
        p->lastFrame = s.frame;
        p->lastElevation = s.elevation;
        p->lastHp = s.hp;
        p->lastAp = s.ap;
        p->lastRadiation = s.radiation;
        p->lastPoison = s.poison;
        p->lastCombatResults = s.combatResults;
        p->hasLastState = true;
    }
}

// Ghost-item diagnostic (host): while the drop-watch window is open, dump the
// objects at the drop tile (and every matching-pid object anywhere) so a
// surviving tile node / stray object is visible in the log.
static void mpDropDiagDump()
{
    uint32_t now = getTicks();
    if (now < gMpDropDiagDeadline) {
        if (gMpDropDiagLastDump == 0 || now - gMpDropDiagLastDump >= 2000) {
            gMpDropDiagLastDump = now;
            Object* obj = objectFindFirst();
            while (obj != nullptr) {
                bool atTile = gMpDropDiagTile >= 0
                    && obj->tile == gMpDropDiagTile
                    && obj->elevation == gMpDropDiagElevation;
                if (atTile || obj->pid == gMpDropDiagPid) {
                    debugFilePrint("MP: dropdiag pid=0x%X fid=0x%X tile=%d elev=%d owner=%p flags=0x%X %s",
                        obj->pid, obj->fid, obj->tile, obj->elevation,
                        (void*)obj->owner, obj->flags,
                        atTile ? "(at drop tile)" : "(pid match)");
                }
                obj = objectFindNext();
            }
        }
    } else if (gMpDropDiagLastDump != 0) {
        debugFilePrint("MP: dropdiag watch ended");
        gMpDropDiagLastDump = 0;
        gMpDropDiagTile = -1;
        gMpDropDiagPid = 0;
    }
}

void MpResetObjectSyncBaseline()
{
    gMpHostObjectRecords.clear();
    if (!gMpIsHost) {
        return;
    }

    Object* obj = objectFindFirst();
    while (obj != nullptr) {
        if (mpHostFindPlayerByObject(obj) != nullptr) {
            obj = objectFindNext();
            continue;
        }
        NetMapFullSyncObjectPayload state;
        if (mpBuildObjectState(obj, &state)) {
            MpHostObjectRecord record;
            record.netId = state.netId;
            record.state = state;
            gMpHostObjectRecords.push_back(record);
        }
        obj = objectFindNext();
    }
}

void MpBroadcastObjectStates()
{
    if (!gMpIsHost || gMpSession.enetHost == nullptr) {
        return;
    }

    // Ghost-item diagnostic: while a drop watch is open, dump the drop tile.
    mpDropDiagDump();

    std::vector<MpHostObjectRecord> currentRecords;
    Object* obj = objectFindFirst();
    while (obj != nullptr) {
        if (mpHostFindPlayerByObject(obj) != nullptr) {
            obj = objectFindNext();
            continue;
        }
        NetMapFullSyncObjectPayload state;
        if (mpBuildObjectState(obj, &state)) {
            int oldIndex = mpHostFindObjectRecord(state.netId);
            bool changed = oldIndex < 0
                || memcmp(&gMpHostObjectRecords[oldIndex].state, &state, sizeof(state)) != 0;
            MultiplayerPlayer* player = mpHostFindPlayerByObject(obj);
            if (changed && player == nullptr) {
                NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_UNRELIABLE,
                    NET_PKT_OBJECT_STATE_UPDATE, &state, sizeof(state));
            }

            MpHostObjectRecord record;
            record.netId = state.netId;
            record.state = state;
            currentRecords.push_back(record);
        }
        obj = objectFindNext();
    }

    // An object that disappeared from the host map must not remain visible on
    // clients. Player critters have their own PLAYER_LEFT packet and are
    // intentionally excluded from this generic removal path.
    for (const MpHostObjectRecord& oldRecord : gMpHostObjectRecords) {
        bool stillPresent = false;
        for (const MpHostObjectRecord& currentRecord : currentRecords) {
            if (currentRecord.netId == oldRecord.netId) {
                stillPresent = true;
                break;
            }
        }
        if (!stillPresent) {
            bool isPlayer = false;
            for (int index = 0; index < NET_MAX_PLAYERS; index++) {
                MultiplayerPlayer* player = &gMpSession.players[index];
                if (player->isConnected && player->objNetId == oldRecord.netId) {
                    isPlayer = true;
                    break;
                }
            }
            if (!isPlayer) {
                debugFilePrint("MP: object removed broadcast netId=%u pid=0x%X fid=0x%X tile=%d",
                    oldRecord.netId, oldRecord.state.pid, oldRecord.state.fid, oldRecord.state.tile);
                NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE,
                    NET_PKT_OBJECT_REMOVED, &oldRecord.netId, sizeof(oldRecord.netId));
            }
        }
    }

    gMpHostObjectRecords.swap(currentRecords);
}

void MpBroadcastMapFullSync(ENetPeer* toPeer)
{
    if (!gMpIsHost || gMpSession.enetHost == nullptr) {
        return;
    }

    std::vector<NetMapFullSyncObjectPayload> objects;
    int skipHidden = 0;
    int skipNoNetId = 0;
    int skipInventory = 0;
    int skipStatic = 0;
    int skipOther = 0;
    Object* obj = objectFindFirst();
    while (obj != nullptr) {
        // Static map-file objects are identical on every machine: the client
        // already has them from its own map load. Skipping them here removes
        // ~450 states from the sync and the net-id collision/vanishing-wall
        // failure mode with them. Living beings (critters) are NOT static:
        // the host owns them and streams their states, so they ship in the
        // sync with their netIds and the client's vanilla copies get replaced.
        if (gMpMapStaticObjIds.count(obj->id) != 0
            && FID_TYPE(obj->fid) != OBJ_TYPE_CRITTER) {
            skipStatic++;
            obj = objectFindNext();
            continue;
        }
        if (gMpMapStaticObjIds.count(obj->id) != 0) {
            skipStatic++;
            obj = objectFindNext();
            continue;
        }
        NetMapFullSyncObjectPayload state;
        if (mpBuildObjectState(obj, &state)) {
            objects.push_back(state);
        } else if (gMpHostObjNetIds.find(obj) == gMpHostObjNetIds.end()) {
            skipNoNetId++;
        } else if ((obj->flags & OBJECT_HIDDEN) != 0) {
            skipHidden++;
        } else if (mpObjectIsInAnyPlayerInventory(obj)) {
            skipInventory++;
        } else {
            skipOther++;
        }
        obj = objectFindNext();
    }
    debugFilePrint("MP: full sync objects=%zu sent=%zu skipNoNetId=%d skipHidden=%d skipInventory=%d skipStatic=%d skipOther=%d",
        objects.size(), objects.size(), skipNoNetId, skipHidden, skipInventory, skipStatic, skipOther);

    uint32_t syncId = ++gMpSession.nextFullSyncId;
    if (syncId == 0) {
        syncId = ++gMpSession.nextFullSyncId;
    }
    size_t chunkCountSize = (objects.size() + MP_FULL_SYNC_MAX_OBJECTS_PER_PACKET - 1)
        / MP_FULL_SYNC_MAX_OBJECTS_PER_PACKET;
    uint16_t chunkCount = (uint16_t)(chunkCountSize == 0 ? 1 : chunkCountSize);

    for (uint16_t chunkIndex = 0; chunkIndex < chunkCount; chunkIndex++) {
        size_t first = (size_t)chunkIndex * MP_FULL_SYNC_MAX_OBJECTS_PER_PACKET;
        size_t remaining = objects.size() > first ? objects.size() - first : 0;
        int32_t count = (int32_t)(remaining > MP_FULL_SYNC_MAX_OBJECTS_PER_PACKET
            ? MP_FULL_SYNC_MAX_OBJECTS_PER_PACKET
            : remaining);

        NetMapFullSyncChunkHeader chunk;
        chunk.syncId = syncId;
        chunk.chunkIndex = chunkIndex;
        chunk.chunkCount = chunkCount;
        chunk.objectCount = count;

        uint8_t buf[NET_MAX_PACKET_SIZE];
        NetPacketHeader packetHeader;
        packetHeader.type = NET_PKT_MAP_FULL_SYNC;
        packetHeader.length = (uint16_t)(sizeof(chunk) + count * sizeof(NetMapFullSyncObjectPayload));
        size_t total = sizeof(packetHeader) + packetHeader.length;
        if (total > sizeof(buf)) {
            return;
        }
        memcpy(buf, &packetHeader, sizeof(packetHeader));
        memcpy(buf + sizeof(packetHeader), &chunk, sizeof(chunk));
        if (count > 0) {
            memcpy(buf + sizeof(packetHeader) + sizeof(chunk),
                &objects[first], count * sizeof(NetMapFullSyncObjectPayload));
        }

        if (toPeer != nullptr) {
            NetPeerSend(toPeer, NET_CHANNEL_RELIABLE, buf, total);
        } else {
            NetHostBroadcast(gMpSession.enetHost, NET_CHANNEL_RELIABLE, buf, total);
        }
    }

    size_t tileChunkCountPerElevation = (SQUARE_GRID_SIZE + MP_MAP_TILE_VALUES_PER_PACKET - 1)
        / MP_MAP_TILE_VALUES_PER_PACKET;
    uint16_t tileChunkCount = (uint16_t)(tileChunkCountPerElevation * ELEVATION_COUNT);
    uint32_t tileSyncId = ++gMpSession.nextTileSyncId;
    if (tileSyncId == 0) {
        tileSyncId = ++gMpSession.nextTileSyncId;
    }

    for (int elevation = 0; elevation < ELEVATION_COUNT; elevation++) {
        if (_square[elevation] == nullptr) {
            continue;
        }
        for (size_t chunkIndex = 0; chunkIndex < tileChunkCountPerElevation; chunkIndex++) {
            int tileOffset = (int)chunkIndex * MP_MAP_TILE_VALUES_PER_PACKET;
            int tileCount = SQUARE_GRID_SIZE - tileOffset;
            if (tileCount > MP_MAP_TILE_VALUES_PER_PACKET) {
                tileCount = MP_MAP_TILE_VALUES_PER_PACKET;
            }

            NetMapTileSyncChunkHeader tileChunk;
            tileChunk.syncId = tileSyncId;
            tileChunk.mapId = gMapHeader.index;
            tileChunk.chunkIndex = (uint16_t)(elevation * tileChunkCountPerElevation + chunkIndex);
            tileChunk.chunkCount = tileChunkCount;
            tileChunk.elevation = (uint8_t)elevation;
            tileChunk.tileOffset = (uint16_t)tileOffset;
            tileChunk.tileCount = (uint16_t)tileCount;

            uint8_t buf[NET_MAX_PACKET_SIZE];
            NetPacketHeader packetHeader;
            packetHeader.type = NET_PKT_MAP_TILE_SYNC;
            packetHeader.length = (uint16_t)(sizeof(tileChunk) + tileCount * sizeof(int32_t));
            size_t total = sizeof(packetHeader) + packetHeader.length;
            memcpy(buf, &packetHeader, sizeof(packetHeader));
            memcpy(buf + sizeof(packetHeader), &tileChunk, sizeof(tileChunk));
            memcpy(buf + sizeof(packetHeader) + sizeof(tileChunk),
                &_square[elevation]->field_0[tileOffset], tileCount * sizeof(int32_t));

            if (toPeer != nullptr) {
                NetPeerSend(toPeer, NET_CHANNEL_RELIABLE, buf, total);
            } else {
                NetHostBroadcast(gMpSession.enetHost, NET_CHANNEL_RELIABLE, buf, total);
            }
        }
    }

    gMpHostTileBaseline.resize((size_t)ELEVATION_COUNT * SQUARE_GRID_SIZE);
    for (int elevation = 0; elevation < ELEVATION_COUNT; elevation++) {
        if (_square[elevation] != nullptr) {
            memcpy(&gMpHostTileBaseline[(size_t)elevation * SQUARE_GRID_SIZE],
                _square[elevation]->field_0, sizeof(_square[elevation]->field_0));
        }
    }

    // The initial state is sent reliably after the reliable map snapshot. This
    // prevents a stationary player from remaining invisible when the first
    // unreliable delta arrived before the client's full-sync state transition.
    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        MultiplayerPlayer* player = &gMpSession.players[index];
        if (!player->isConnected || player->obj == nullptr) {
            continue;
        }

        NetPlayerStateUpdatePayload state;
        state.netId = player->netId;
        state.objNetId = player->objNetId;
        state.tile = player->obj->tile;
        state.x = player->obj->x;
        state.y = player->obj->y;
        state.rotation = player->obj->rotation;
        state.fid = player->obj->fid;
        state.frame = player->obj->frame;
        state.elevation = player->obj->elevation;
        state.hp = player->obj->data.critter.hp;
        state.ap = player->obj->data.critter.combat.ap;
        state.radiation = player->obj->data.critter.radiation;
        state.poison = player->obj->data.critter.poison;
        state.combatResults = player->obj->data.critter.combat.results;

        if (toPeer != nullptr) {
            NetSendPacket(toPeer, NET_CHANNEL_RELIABLE,
                NET_PKT_PLAYER_STATE_UPDATE, &state, sizeof(state));
        } else {
            NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE,
                NET_PKT_PLAYER_STATE_UPDATE, &state, sizeof(state));
        }

        player->lastTile = state.tile;
        player->lastX = state.x;
        player->lastY = state.y;
        player->lastRotation = state.rotation;
        player->lastFid = state.fid;
        player->lastFrame = state.frame;
        player->lastElevation = state.elevation;
        player->lastHp = state.hp;
        player->lastAp = state.ap;
        player->lastRadiation = state.radiation;
        player->lastPoison = state.poison;
        player->lastCombatResults = state.combatResults;
        player->hasLastState = true;
    }
}

void MpBroadcastMapChanged(int32_t mapId)
{
    if (!gMpIsHost || gMpSession.enetHost == nullptr) {
        return;
    }
    NetMapChangedPayload p;
    memset(&p, 0, sizeof(p));
    mpBuildMapSyncPayload(&p.map);
    p.map.mapId = mapId;
    // Anchor client-side avatar rebuilds at the host's ACTUAL post-transition
    // position instead of the map file's default entering tile. MpFinishHostMapChange
    // respawns every player around gDude's tile; the client mirrors that logic in
    // MpApplyMapChanged (mpFindPlayerSpawnTile on the same anchor), so giving the
    // client the same anchor makes both sides pick identical spawn tiles and the
    // first player-state update has nothing to snap.
    if (gDude != nullptr) {
        if (hexGridTileIsValid(gDude->tile)) {
            p.map.enteringTile = gDude->tile;
        }
        if (elevationIsValid(gDude->elevation)) {
            p.map.enteringElevation = gDude->elevation;
        }
        if (gDude->rotation >= 0 && gDude->rotation < ROTATION_COUNT) {
            p.map.enteringRotation = gDude->rotation;
        }
    }
    debugFilePrint("MP: broadcast map changed map=%d enteringTile=%d elev=%d",
        mapId, p.map.enteringTile, p.map.enteringElevation);
    NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE, NET_PKT_MAP_CHANGED, &p, sizeof(p));
}

void MpBroadcastMapChangeAbort()
{
    if (!gMpIsHost || gMpSession.enetHost == nullptr) {
        return;
    }
    debugFilePrint("MP: broadcast map change abort map=%d", gMapHeader.index);
    NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE, NET_PKT_MAP_CHANGE_ABORT, nullptr, 0);
}

void MpPrepareForMapChange()
{
    if (!gMpIsHost) {
        return;
    }
    debugFilePrint("MP: prepare map change begin map=%d", gMapHeader.index);

    // The map load below places gDude at the entering tile and
    // MpFinishHostMapChange respawns remote avatars; if any of those tiles
    // is itself an exit grid, the connect hook must not re-trigger a vote.
    gMpSuppressExitGridCheck = true;
    // The map load's _obj_remove_all() would free gDude's inventory items
    // (they live in the head list); protect them for the load window.
    mpSetDudeInventoryProtected(true);
    gMpMapStaticObjIds.clear();

    // Remote player critters belong to the old map's object list. Remove them
    // before mapLoadById can tear that list down, while preserving their
    // player/object network IDs and full profiles for the new map.
    for (int index = 1; index < NET_MAX_PLAYERS; index++) {
        MultiplayerPlayer* player = &gMpSession.players[index];
        if (player->isConnected && player->obj != nullptr) {
            debugFilePrint("MP: prepare map change detach netId=%u", player->netId);
            if (MpProfileGetRuntime(player->netId) != nullptr) {
                MpProfileDetachAvatar(player->netId);
            } else {
                mpDestroyNetworkObject(player->obj);
            }
            player->obj = nullptr;
            player->hasLastState = false;
            player->hasInitialState = false;
        }
    }

    gMpHostObjNetIds.clear();
    gMpSession.nextObjNetId = MP_OBJ_NETID_BASE;
    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        MultiplayerPlayer* player = &gMpSession.players[index];
        if (player->isConnected && player->objNetId >= gMpSession.nextObjNetId) {
            gMpSession.nextObjNetId = player->objNetId + 1;
        }
    }
    gMpHostObjectRecords.clear();
    debugFilePrint("MP: prepare map change done");
}

void MpFinishHostMapChange()
{
    if (!gMpIsHost) {
        return;
    }
    debugFilePrint("MP: finish map change begin map=%d", gMapHeader.index);

    MultiplayerPlayer* hostPlayer = &gMpSession.players[0];
    hostPlayer->obj = gDude;
    hostPlayer->lastSafeTile = gDude->tile;
    hostPlayer->lastSafeElevation = gDude->elevation;
    hostPlayer->lastSafeRotation = gDude->rotation;
    hostPlayer->hasSafePosition = true;
    MpRegisterObjNetId(gDude, hostPlayer->objNetId);
    objectReorder(gDude);
    mpSnapshotMapStaticObjects();

    int tile = hexGridTileIsValid(gDude->tile)
        ? gDude->tile
        : gMapHeader.enteringTile;
    tile = hexGridTileIsValid(tile)
        ? tile
        : gDude->tile;
    int elevation = elevationIsValid(gMapHeader.enteringElevation)
        ? gMapHeader.enteringElevation
        : gDude->elevation;
    int rotation = gMapHeader.enteringRotation >= 0
        ? gMapHeader.enteringRotation
        : ROTATION_NE;

    for (int index = 1; index < NET_MAX_PLAYERS; index++) {
        MultiplayerPlayer* player = &gMpSession.players[index];
        if (!player->isConnected || !player->isHandshaken || !player->profileReady) {
            continue;
        }
        MpPlayerRuntime* runtime = MpProfileGetRuntime(player->netId);
        if (runtime == nullptr) {
            debugFilePrint("MP: finish map change no runtime netId=%u", player->netId);
            continue;
        }
        MpPlayerProfile profileCopy = runtime->profile;
        MpProfileDetachAvatar(player->netId);
        int playerTile = mpFindPlayerSpawnTile(tile, elevation);
        runtime = MpProfileCreateRuntime(player->netId, profileCopy,
            playerTile, elevation, rotation);
        if (runtime == nullptr) {
            debugFilePrint("MP: finish map change respawn failed netId=%u", player->netId);
            continue;
        }
        debugFilePrint("MP: finish map change respawned netId=%u tile=%d", player->netId, runtime->object->tile);
        player->obj = runtime->object;
        player->lastSafeTile = runtime->object->tile;
        player->lastSafeElevation = runtime->object->elevation;
        player->lastSafeRotation = runtime->object->rotation;
        player->hasSafePosition = true;
        player->hasLastState = false;
        player->hasInitialState = false;
        MpRegisterObjNetId(runtime->object, player->objNetId);
        objectReorder(runtime->object);
    }

    gMpSession.currentMapId = gMapHeader.index;
    MpAssignNetIdsToAllObjects();
    MpResetObjectSyncBaseline();
    gMpSuppressExitGridCheck = false;
    mpSetDudeInventoryProtected(false);
    debugFilePrint("MP: finish map change done");
    mpDebugDumpLightState("host-after-finish");
}

// ---------------------------------------------------------------------------
// Apply (client)
// ---------------------------------------------------------------------------

static MultiplayerPlayer* mpClientFindPlayerByObjNetId(uint32_t objNetId)
{
    if (objNetId == 0) {
        return nullptr;
    }

    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        MultiplayerPlayer* player = &gMpSession.players[index];
        if (player->isConnected && player->objNetId == objNetId) {
            return player;
        }
    }

    return nullptr;
}

static void mpApplyObjectTransform(Object* obj, int tile, int x, int y, int rotation, int fid, int frame, int elevation, int flags, bool applyFlags)
{
    if (obj == nullptr || !hexGridTileIsValid(tile) || !elevationIsValid(elevation)) {
        return;
    }

    Rect oldRect;
    objectGetRect(obj, &oldRect);
    int oldElevation = obj->elevation;
    bool wasApplyingNetworkState = gMpSession.applyingNetworkState;
    gMpSession.applyingNetworkState = true;

    objectSetFid(obj, fid, nullptr);
    if (objectSetFrame(obj, frame, nullptr) != 0) {
        obj->frame = frame;
    }
    if (objectSetRotation(obj, rotation, nullptr) != 0) {
        obj->rotation = rotation;
    }

    if (obj->tile != tile || obj->elevation != elevation) {
        objectSetLocation(obj, tile, elevation, nullptr);
    }

    // objectSetLocation maintains the engine's tile lists. The host's x/y
    // offsets are applied after that bookkeeping is complete.
    obj->x = x;
    obj->y = y;

    // Synced movement bypasses _obj_move_to, which is what hides roofs above
    // the local player in vanilla. Keep roofs consistent: run the same roof
    // state machine for the local dude so walls hide when he is under a roof.
    if (obj == gDude) {
        debugFilePrint("MPDBG roof update: tile=%d elev=%d", tile, elevation);
        objUpdateRoofsForTile(tile, elevation);
    }

    if (applyFlags) {
        int localObjectFlags = obj == gDude
            ? obj->flags & (OBJECT_NO_REMOVE | OBJECT_NO_SAVE | OBJECT_LIGHT_THRU)
            : 0;
        obj->flags = flags | localObjectFlags;
    }

    gMpSession.applyingNetworkState = wasApplyingNetworkState;

    Rect newRect;
    objectGetRect(obj, &newRect);
    tileWindowRefreshRect(&oldRect, oldElevation);
    tileWindowRefreshRect(&newRect, obj->elevation);
}

static void mpApplyCritterState(Object* obj, int hp, int ap, int radiation, int poison, int combatTeam, int combatManeuver, int combatResults)
{
    if (obj == nullptr || FID_TYPE(obj->fid) != OBJ_TYPE_CRITTER) {
        return;
    }

    obj->data.critter.hp = hp;
    obj->data.critter.combat.ap = ap;
    obj->data.critter.radiation = radiation;
    obj->data.critter.poison = poison;
    obj->data.critter.combat.team = combatTeam;
    obj->data.critter.combat.maneuver = combatManeuver;
    obj->data.critter.combat.results = combatResults;
}

static void mpShowClientPlayer(Object* obj)
{
    if (obj == nullptr) {
        return;
    }

    objectShow(obj, nullptr);
    objectReorder(obj);

    if (hexGridTileIsValid(obj->tile) && elevationIsValid(obj->elevation)) {
        Rect rect;
        objectGetRect(obj, &rect);
        tileWindowRefreshRect(&rect, obj->elevation);
    }
}

static void mpEnsureClientPlayersVisible()
{
    if (!gMpIsClient) {
        return;
    }

    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        MultiplayerPlayer* player = &gMpSession.players[index];
        if (!player->isConnected || player->obj == nullptr) {
            continue;
        }
        if ((player->obj->flags & OBJECT_HIDDEN) != 0) {
            mpShowClientPlayer(player->obj);
        }
    }
}

static void mpClearClientMapObjectsForFullSync()
{
    int keptInventoryItems = 0;
    int keptStaticObjects = 0;
    std::vector<Object*> objects;
    for (Object* obj = objectFindFirst(); obj != nullptr; obj = objectFindNext()) {
        if (MpProfileIsNetworkPlayer(obj)) {
            continue;
        }
        if (obj != gDude && obj != gEgg
            && !mpObjectIsInCritterInventory(obj, gDude)
            && (obj->flags & OBJECT_NO_REMOVE) == 0) {
            if (gMpMapStaticObjIds.count(obj->id) != 0) {
                // Map-file non-critter objects are kept: the full sync no
                // longer ships them (the host skips them too), and the
                // client's copy is identical to the host's. Living beings
                // (critters) are host-owned: the vanilla map-load copies must
                // go — the full sync recreates them with netIds and live
                // states, and the per-tick channel keeps them in sync.
                if (FID_TYPE(obj->fid) == OBJ_TYPE_CRITTER) {
                    objects.push_back(obj);
                } else {
                    keptStaticObjects++;
                    continue;
                }
            } else {
                objects.push_back(obj);
            }
        } else if (mpObjectIsInCritterInventory(obj, gDude)) {
            keptInventoryItems++;
        }
    }
    debugFilePrint("MPDBG clear: destroying=%d keptDudeItems=%d keptStatic=%d",
        (int)objects.size(), keptInventoryItems, keptStaticObjects);

    for (Object* obj : objects) {
        mpDestroyNetworkObject(obj);
    }

    if (gMpSession.netIdToObj != nullptr) {
        memset(gMpSession.netIdToObj, 0,
            gMpSession.netIdToObjCapacity * sizeof(Object*));
        gMpSession.netIdToObjCount = 0;
    }
    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        if (gMpSession.players[index].isLocal) {
            gMpSession.players[index].obj = gDude;
            // The first state of the fresh sync must snap the local player
            // into the host's position again.
            gMpSession.players[index].hasLastState = false;
        } else {
            // Profile-owned remote avatars survive the map-object cleanup:
            // they live in the profile runtime map, not the generic object
            // sync, so their Object* must be preserved (only the runtime map
            // knows how to rebuild them). Players without a profile runtime
            // are stale and get cleared.
            MpPlayerRuntime* runtime = MpProfileGetRuntime(gMpSession.players[index].netId);
            if (runtime != nullptr && runtime->object != nullptr) {
                gMpSession.players[index].obj = runtime->object;
            } else {
                gMpSession.players[index].obj = nullptr;
            }
            gMpSession.players[index].hasInitialState = false;
        }
    }
}

static Object* mpCreateClientObject(const NetMapFullSyncObjectPayload* state)
{
    if (state == nullptr) {
        return nullptr;
    }

    Object* obj = nullptr;
    int result;
    if (FID_TYPE(state->fid) == OBJ_TYPE_CRITTER) {
        result = objectCreateWithFidPid(&obj, state->fid, state->pid);
    } else {
        result = objectCreateWithPid(&obj, state->pid);
    }
    if (result != 0 || obj == nullptr) {
        return nullptr;
    }
    objectShow(obj, nullptr);
    return obj;
}

static Object* mpApplyObjectStateInternal(const NetMapFullSyncObjectPayload* state, bool allowMapMatch)
{
    if (state == nullptr || state->netId == 0) {
        return nullptr;
    }

    MultiplayerPlayer* player = mpClientFindPlayerByObjNetId(state->netId);
    Object* obj = MpFindObjByNetId(state->netId);
    if (obj == nullptr && player != nullptr) {
        obj = player->obj;
    }

    // Player avatars are owned by the profile stream and player-state channel;
    // generic map-object packets must never overwrite their prototypes,
    // inventory, or appearance.
    if (player != nullptr) {
        return obj;
    }

    // The local player object must stay the vault-dude critter. A non-critter
    // state bound to his netId is corruption (it would turn gDude into a wall
    // and zero his stats); refuse to apply it.
    if (player != nullptr && player->isLocal
        && FID_TYPE(state->fid) != OBJ_TYPE_CRITTER) {
        debugFilePrint("MPDBG reject non-critter state for local player: netId=%u pid=0x%X fid=0x%X",
            state->netId, state->pid, state->fid);
        return obj;
    }
    if (player != nullptr && player->isLocal) {
        debugFilePrint("MPDBG apply local state: netId=%u pid=0x%X fid=0x%X tile=%d elev=%d objpid=0x%X",
            state->netId, state->pid, state->fid, state->tile, state->elevation,
            obj != nullptr ? obj->pid : 0);
    }

    if (obj == nullptr && player == nullptr && allowMapMatch && hexGridTileIsValid(state->tile)
        && elevationIsValid(state->elevation)) {
        Object* match = objectFindFirstAtLocation(state->elevation, state->tile);
        while (match != nullptr) {
            if (match->pid == state->pid) {
                obj = match;
                break;
            }
            match = objectFindNextAtLocation();
        }
    }

    // Fate trace: every wall/scenery state during the full sync. If an object
    // with this netId already exists, the state REUSES it (overwriting the
    // previous object) — duplicate netIds lose the earlier object.
    if (obj == nullptr) {
        obj = mpCreateClientObject(state);
    }
    if (obj == nullptr) {
        return nullptr;
    }

    // Keep the local player's critter pid even if a state arrives with a
    // non-critter pid (corruption guard — a wall pid here would make
    // critterGetStat return 0 and render the dude as wall art).
    if (player == nullptr || !player->isLocal || PID_TYPE(state->pid) == OBJ_TYPE_CRITTER) {
        obj->pid = state->pid;
    }
    MpRegisterObjNetId(obj, state->netId);
    if (player != nullptr) {
        player->obj = obj;
    }
    mpApplyObjectTransform(obj, state->tile, state->x, state->y,
        state->rotation, state->fid, state->frame, state->elevation,
        state->flags, true);
    if (player != nullptr) {
        mpShowClientPlayer(obj);
    }
    mpApplyCritterState(obj, state->hp, state->ap, state->radiation,
        state->poison, state->combatTeam, state->combatManeuver,
        state->combatResults);
    return obj;
}

void MpApplyObjectState(const NetMapFullSyncObjectPayload* state)
{
    if (!gMpIsClient || state == nullptr) {
        return;
    }
    // Map-match enabled: kept map-file objects are not netId-registered, so a
    // runtime change to one of them (door opened, wall state modified) must be
    // matched by tile+pid to update the client's local copy.
    mpApplyObjectStateInternal(state, true);
}

void MpApplyObjectRemoved(uint32_t netId)
{
    if (!gMpIsClient || netId == 0) {
        return;
    }

    MultiplayerPlayer* player = mpClientFindPlayerByObjNetId(netId);
    if (player != nullptr) {
        // Player lifecycle is reliable and explicit. Do not let a stale
        // generic removal packet destroy a player avatar before PLAYER_LEFT.
        return;
    }

    Object* obj = MpFindObjByNetId(netId);
    if (gMpSession.netIdToObj != nullptr
        && netId < (uint32_t)gMpSession.netIdToObjCapacity) {
        gMpSession.netIdToObj[netId] = nullptr;
    }
    if (obj != nullptr && obj != gDude) {
        debugFilePrint("MP: object removed applied netId=%u pid=0x%X tile=%d elev=%d",
            netId, obj->pid, obj->tile, obj->elevation);
        mpDestroyNetworkObject(obj);
    } else {
        debugFilePrint("MP: object removed miss netId=%u obj=%p",
            netId, (void*)obj);
    }
}

void MpApplyPlayerState(const NetPlayerStateUpdatePayload* s)
{
    if (s == nullptr || s->netId == 0 || s->netId > NET_MAX_PLAYERS || s->objNetId == 0) {
        return;
    }

    MultiplayerPlayer* p = &gMpSession.players[s->netId - 1];
    if (!p->isConnected || !p->profileReady) {
        return;
    }

    bool isLocalPlayer = (p->isLocal || s->netId == gMpSession.localNetId);

    p->objNetId = s->objNetId;
    Object* obj = isLocalPlayer ? gDude : p->obj;
    if (obj == nullptr && !isLocalPlayer) {
        obj = MpFindObjByNetId(p->objNetId);
    }
    if (obj == nullptr) {
        return;
    }
    p->obj = obj;

    bool localMovementIsActive = isLocalPlayer && animationIsBusy(obj) != 0;
    // The very first state must snap the local player into place (the map
    // reload may have left him at the map's entering tile, off-screen from
    // the host's camera). Later updates yield to local prediction. A frozen
    // initiator (map-change vote in progress) never yields — the host has
    // stopped the critter on the exit grid and drives its position
    // authoritatively; yielding would let the local walk continue past the
    // zone on the client's own screen.
    if (!localMovementIsActive || !p->hasLastState || gMpSession.initiatorFrozen) {
        // Co-op diagnostic (throttled): a local-player transform snap. Shows
        // how far the client's dude is being pulled by the authoritative
        // state (expected to be sub-tile; large snaps indicate the avatar
        // lags behind — e.g. its walk never advanced).
        if (isLocalPlayer && p->hasLastState && obj->tile != s->tile) {
            static uint32_t gMpLocalSnapLogTick = 0;
            uint32_t nowTicks = getTicks();
            if (nowTicks - gMpLocalSnapLogTick > 500) {
                gMpLocalSnapLogTick = nowTicks;
                debugFilePrint("MP: local snap tile=%d->%d anim=%d",
                    obj->tile, s->tile, localMovementIsActive ? 1 : 0);
            }
        }
        mpApplyObjectTransform(obj, s->tile, s->x, s->y, s->rotation, s->fid, s->frame, s->elevation, 0, false);
        p->hasLastState = true;
    }
    if (!isLocalPlayer) {
        // The host's FID model index is process-local (custom models may
        // resolve to different indices per machine). Remap through the
        // profile's locally-resolved model index; animation/weapon/rotation
        // bits are machine-independent.
        MpPlayerRuntime* runtime = MpProfileGetRuntime(s->netId);
        if (runtime != nullptr && runtime->profile.localModelIndex >= 0) {
            obj->fid = (obj->fid & ~0xFFF) | runtime->profile.localModelIndex;
        }
    }
    int oldLocalHp = 0;
    if (isLocalPlayer) {
        oldLocalHp = obj->data.critter.hp;
    }
    mpApplyCritterState(obj, s->hp, s->ap, s->radiation, s->poison, obj->data.critter.combat.team, obj->data.critter.combat.maneuver, s->combatResults);
    // The host is authoritative over the local player's HP, but the vanilla
    // HUD only re-renders on local damage events. A hit taken from a remote
    // actor (or host-resolved damage) arrives via this state and would never
    // reach the HP bar — refresh it whenever the synced value changes.
    if (isLocalPlayer && obj->data.critter.hp != oldLocalHp) {
        interfaceRenderHitPoints(true);
    }
    p->hasInitialState = true;
    mpShowClientPlayer(obj);
    // A player state may arrive after the last sync chunk completed; without
    // this re-check the client would sit in SYNCING forever waiting on the
    // final readiness condition.
    mpClientTryFinishMapSync();
}

void MpApplyPlayerJoined(const NetPlayerJoinedPayload* payload)
{
    if (!gMpIsClient || payload == nullptr
        || payload->netId == 0 || payload->netId > NET_MAX_PLAYERS
        || payload->objNetId == 0) {
        return;
    }

    MultiplayerPlayer* player = &gMpSession.players[payload->netId - 1];
    if (player->isLocal || payload->netId == gMpSession.localNetId) {
        return;
    }

    player->netId = payload->netId;
    player->objNetId = payload->objNetId;
    player->peer = nullptr;
    player->isLocal = false;
    player->isConnected = true;
    player->isHandshaken = true;
    memcpy(player->name, payload->name, sizeof(player->name));
    player->name[NET_PEER_NAME_LENGTH - 1] = '\0';
    player->obj = MpFindObjByNetId(payload->objNetId);
    MpPlayerRuntime* runtime = MpProfileGetRuntime(payload->netId);
    if (runtime != nullptr && runtime->object != nullptr) {
        player->obj = runtime->object;
        player->profileReady = true;
    }
    if (player->obj != nullptr) {
        MpRegisterObjNetId(player->obj, payload->objNetId);
    }
}

void MpApplyPlayerLeft(const NetPlayerLeftPayload* payload)
{
    if (!gMpIsClient || payload == nullptr
        || payload->netId == 0 || payload->netId > NET_MAX_PLAYERS) {
        return;
    }

    MultiplayerPlayer* player = &gMpSession.players[payload->netId - 1];
    if (player->isConnected && !player->isLocal && player->name[0] != '\0') {
        char message[NET_PEER_NAME_LENGTH + 16];
        snprintf(message, sizeof(message), "%s left", player->name);
        win_timed_msg(message, COLOR_RED);
    }
    uint32_t objNetId = payload->objNetId != 0 ? payload->objNetId : player->objNetId;
    Object* obj = player->obj;
    if (obj == nullptr && objNetId != 0) {
        obj = MpFindObjByNetId(objNetId);
    }
    if (gMpSession.netIdToObj != nullptr && objNetId != 0
        && objNetId < (uint32_t)gMpSession.netIdToObjCapacity) {
        gMpSession.netIdToObj[objNetId] = nullptr;
    }
    if (MpProfileGetRuntime(payload->netId) != nullptr) {
        MpProfileDestroyRuntime(payload->netId);
    } else if (obj != nullptr && obj != gDude) {
        mpDestroyNetworkObject(obj);
    }
    memset(player, 0, sizeof(*player));
}

void MpApplyMapFullSync(const void* data, size_t dataLength)
{
    if (data == nullptr || dataLength < sizeof(NetMapFullSyncChunkHeader)) {
        return;
    }

    NetMapFullSyncChunkHeader chunk;
    memcpy(&chunk, data, sizeof(chunk));
    if (chunk.chunkCount == 0 || chunk.chunkIndex >= chunk.chunkCount
        || chunk.objectCount < 0
        || (size_t)chunk.objectCount > (dataLength - sizeof(chunk))
            / sizeof(NetMapFullSyncObjectPayload)) {
        return;
    }
    if (dataLength != sizeof(chunk)
        + (size_t)chunk.objectCount * sizeof(NetMapFullSyncObjectPayload)) {
        return;
    }

    if (chunk.chunkIndex == 0) {
        if (gMpSession.state == MP_STATE_CLIENT_SYNCING) {
            mpClearClientMapObjectsForFullSync();
        }
        gMpSession.clientSyncId = chunk.syncId;
        gMpSession.clientSyncExpectedChunks = chunk.chunkCount;
        gMpSession.clientSyncNextChunk = 0;
    }
    if (chunk.syncId != gMpSession.clientSyncId
        || chunk.chunkCount != gMpSession.clientSyncExpectedChunks
        || chunk.chunkIndex != gMpSession.clientSyncNextChunk) {
        return;
    }

    const uint8_t* cursor = (const uint8_t*)data + sizeof(chunk);
    size_t remaining = dataLength - sizeof(chunk);

    for (int i = 0; i < chunk.objectCount; i++) {
        if (remaining < sizeof(NetMapFullSyncObjectPayload)) {
            break;
        }
        const NetMapFullSyncObjectPayload* o = (const NetMapFullSyncObjectPayload*)cursor;
        cursor += sizeof(NetMapFullSyncObjectPayload);
        remaining -= sizeof(NetMapFullSyncObjectPayload);

        if (o->netId == 0) {
            continue;
        }
        mpApplyObjectStateInternal(o, true);
    }

    gMpSession.clientSyncNextChunk++;
    if (gMpSession.clientSyncNextChunk >= gMpSession.clientSyncExpectedChunks) {
        mpClientTryFinishMapSync();
    }
}

void MpApplyMapTileSync(const void* data, size_t dataLength)
{
    if (!gMpIsClient || data == nullptr || dataLength < sizeof(NetMapTileSyncChunkHeader)) {
        return;
    }

    NetMapTileSyncChunkHeader chunk;
    memcpy(&chunk, data, sizeof(chunk));
    size_t expectedLength = sizeof(chunk) + (size_t)chunk.tileCount * sizeof(int32_t);
    if (chunk.mapId != gMpSession.currentMapId
        || chunk.chunkCount == 0
        || chunk.chunkIndex >= chunk.chunkCount
        || !elevationIsValid(chunk.elevation)
        || !squareGridTileIsValid(chunk.tileOffset)
        || chunk.tileCount == 0
        || chunk.tileOffset + chunk.tileCount > SQUARE_GRID_SIZE
        || dataLength != expectedLength) {
        return;
    }

    if (chunk.chunkIndex == 0) {
        gMpSession.clientTileSyncId = chunk.syncId;
        gMpSession.clientTileSyncExpectedChunks = chunk.chunkCount;
        gMpSession.clientTileSyncNextChunk = 0;
    }
    if (chunk.syncId != gMpSession.clientTileSyncId
        || chunk.chunkCount != gMpSession.clientTileSyncExpectedChunks
        || chunk.chunkIndex != gMpSession.clientTileSyncNextChunk
        || _square[chunk.elevation] == nullptr) {
        return;
    }

    int32_t* dest = &_square[chunk.elevation]->field_0[chunk.tileOffset];
    const int32_t* src = (const int32_t*)((const uint8_t*)data + sizeof(chunk));
    for (int index = 0; index < chunk.tileCount; index++) {
        int32_t incoming = src[index];
        int32_t local = dest[index];

        // The host's roof word carries the world's roof-hidden state: roofs
        // above the host's player are hidden as he walks under them, and that
        // state lives in the tile word itself. The client must adopt it, or
        // roofs the host has hidden would draw over the client's walls.
        // Keep only the local hidden bit (bit 0 of the roof flags) so the
        // client's own roof-fill state above the local player survives.
        int32_t roof = (incoming >> 16) & 0xFFFF;
        int32_t localRoof = (local >> 16) & 0xFFFF;
        roof = (roof & ~0x0001) | (localRoof & 0x0001);

        dest[index] = (incoming & 0xFFFF) | (roof << 16);
    }

    gMpSession.clientTileSyncNextChunk++;
    if (gMpSession.clientTileSyncNextChunk >= gMpSession.clientTileSyncExpectedChunks) {
        if (gMpSession.state == MP_STATE_CLIENT_SYNCING) {
            mpClientTryFinishMapSync();
        } else if (gMpSession.state == MP_STATE_CLIENT_PLAYING) {
            tileWindowRefreshFull();
        }
    }
}

void MpApplyMapChanged(const NetMapSyncPayload* payload)
{
    if (payload == nullptr || payload->mapId < 0) {
        debugFilePrint("MP: apply map changed bad payload mapId=%d", payload != nullptr ? payload->mapId : -1);
        return;
    }
    debugFilePrint("MP: apply map changed begin map=%d name='%s'", payload->mapId, payload->mapName);

    // A passed vote's modal stays up showing the final tally until the map
    // change actually begins — close it now. The vote session is also reset:
    // a stale PASSED state must not let a later grid step through the
    // transition interceptor as if it were the host's resolve path.
    MpVoteHideUI();
    gVoteSession.state = VOTE_STATE_NONE;

    // The map load below places gDude at the entering tile and the avatar
    // rebuild below re-places remote critters; if any spawn tile is itself an
    // exit grid, the connect hook must not re-trigger a vote.
    gMpSuppressExitGridCheck = true;
    // The map load's _obj_remove_all() would free gDude's inventory items
    // (they live in the head list); protect them for the load window.
    mpSetDudeInventoryProtected(true);

    // Detach remote avatars first: their Object* instances belong to the old
    // map's object list, which the map load tears down. Their profiles stay.
    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        MultiplayerPlayer* player = &gMpSession.players[index];
        if (!player->isLocal && MpProfileGetRuntime(player->netId) != nullptr) {
            MpProfileDetachAvatar(player->netId);
        }
    }

    MpClearNetIdMappings();
    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        if (!gMpSession.players[index].isLocal) {
            gMpSession.players[index].obj = nullptr;
            gMpSession.players[index].hasInitialState = false;
        } else {
            gMpSession.players[index].obj = gDude;
        }
    }
    if (mpClientLoadMap(payload) != 0) {
        win_timed_msg("Could not load the host map", COLOR_RED);
        mpRequestClientDisconnect(false);
        _game_user_wants_to_quit = GAME_QUIT_REQUEST_MAIN_MENU;
        return;
    }

    // Rebuild remote avatars from the preserved profiles. The host does not
    // re-send profiles on map change, only the initial player states; the
    // fresh avatar Object* instances must exist before those states apply.
    int tile = hexGridTileIsValid(payload->enteringTile)
        ? payload->enteringTile
        : (gDude != nullptr && hexGridTileIsValid(gDude->tile) ? gDude->tile : payload->centerTile);
    int elevation = elevationIsValid(payload->enteringElevation)
        ? payload->enteringElevation
        : (gDude != nullptr ? gDude->elevation : 0);
    int rotation = payload->enteringRotation >= 0
        ? payload->enteringRotation
        : ROTATION_NE;
    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        MultiplayerPlayer* player = &gMpSession.players[index];
        if (player->isLocal || !player->isConnected || !player->profileReady) {
            continue;
        }
        MpPlayerRuntime* runtime = MpProfileGetRuntime(player->netId);
        if (runtime == nullptr || runtime->object != nullptr) {
            continue;
        }
        MpPlayerProfile profileCopy = runtime->profile;
        int playerTile = mpFindPlayerSpawnTile(tile, elevation);
        runtime = MpProfileCreateRuntime(player->netId, profileCopy,
            playerTile, elevation, rotation);
        if (runtime == nullptr) {
            continue;
        }
        player->obj = runtime->object;
        player->hasLastState = false;
        player->hasInitialState = false;
        MpRegisterObjNetId(runtime->object, player->objNetId);
        mpShowClientPlayer(runtime->object);
        debugFilePrint("MP: apply map changed rebuilt avatar netId=%u tile=%d",
            player->netId, runtime->object->tile);
    }

    int area = -1;
    if (wmMatchAreaContainingMapIdx(payload->mapId, &area) == 0 && wmTeleportToArea(area) == -1) {
        debugFilePrint("MP: client could not synchronize world-map area for map=%d area=%d", payload->mapId, area);
    }
    gMpSuppressExitGridCheck = false;
    mpSetDudeInventoryProtected(false);
    // The fresh MAP_FULL_SYNC will be appended under MP_STATE_CLIENT_SYNCING.
    mpEnterClientSync();
    debugFilePrint("MP: apply map changed done state=syncing");
}

// ---------------------------------------------------------------------------
// NetId mapping
// ---------------------------------------------------------------------------

uint32_t MpAllocObjNetId()
{
    if (gMpSession.nextObjNetId == 0) {
        gMpSession.nextObjNetId = MP_OBJ_NETID_BASE;
    }
    uint32_t netId;
    do {
        netId = gMpSession.nextObjNetId++;
        // Never hand an object a netId that belongs to a connected player's
        // critter: clients bind those netIds to their player objects and
        // reject every colliding object state, so the object vanishes.
    } while (mpHostNetIdIsPlayerObjNetId(netId));
    return netId;
}

void MpRegisterObjNetId(Object* obj, uint32_t netId)
{
    if (obj == nullptr || netId == 0) {
        return;
    }
    if (gMpIsHost) {
        gMpHostObjNetIds[obj] = netId;
    } else {
        if (gMpSession.netIdToObj == nullptr) {
            gMpSession.netIdToObjCapacity = MP_NETID_TO_OBJ_INITIAL_CAPACITY;
            gMpSession.netIdToObj = (Object**)calloc(gMpSession.netIdToObjCapacity, sizeof(Object*));
        }
        if (netId >= gMpSession.netIdToObjCapacity) {
            int newCap = gMpSession.netIdToObjCapacity;
            while (newCap <= (int)netId) {
                newCap *= 2;
            }
            Object** grown = (Object**)realloc(gMpSession.netIdToObj, newCap * sizeof(Object*));
            if (grown == nullptr) {
                return;
            }
            memset(grown + gMpSession.netIdToObjCapacity, 0,
                (newCap - gMpSession.netIdToObjCapacity) * sizeof(Object*));
            gMpSession.netIdToObj = grown;
            gMpSession.netIdToObjCapacity = newCap;
        }
        gMpSession.netIdToObj[netId] = obj;
        if ((int)netId + 1 > gMpSession.netIdToObjCount) {
            gMpSession.netIdToObjCount = netId + 1;
        }
    }
}

void MpAssignNetIdsToAllObjects()
{
    if (!gMpIsHost) {
        return;
    }
    Object* obj = objectFindFirst();
    while (obj != nullptr) {
        // Keyed by object pointer, so duplicate obj ids (common in map
        // files) can never collapse two objects onto one netId.
        if (gMpHostObjNetIds.find(obj) == gMpHostObjNetIds.end()) {
            gMpHostObjNetIds[obj] = MpAllocObjNetId();
        }
        obj = objectFindNext();
    }
}

Object* MpFindObjByNetId(uint32_t netId)
{
    if (gMpIsHost) {
        // Host: reverse-lookup through the host's object->netId map.
        for (auto it = gMpHostObjNetIds.begin(); it != gMpHostObjNetIds.end(); ++it) {
            if (it->second == netId) {
                return const_cast<Object*>(it->first);
            }
        }
        return nullptr;
    }
    if (gMpSession.netIdToObj == nullptr || netId >= (uint32_t)gMpSession.netIdToObjCapacity) {
        return nullptr;
    }
    return gMpSession.netIdToObj[netId];
}

uint32_t MpGetObjNetId(Object* obj)
{
    if (obj == nullptr) {
        return 0;
    }

    if (gMpIsHost) {
        auto it = gMpHostObjNetIds.find(obj);
        if (it == gMpHostObjNetIds.end()) {
            return 0;
        }
        return it->second;
    }

    if (gMpSession.netIdToObj == nullptr) {
        return 0;
    }
    for (int netId = 1; netId < gMpSession.netIdToObjCount; netId++) {
        if (gMpSession.netIdToObj[netId] == obj) {
            return (uint32_t)netId;
        }
    }
    return 0;
}

bool MpIsNetworkedCritter(Object* obj)
{
    if (!gMpActive || obj == nullptr) {
        return false;
    }
    if (gMpIsHost) {
        // The host's object-id table also contains scenery, cursor helpers,
        // and gEgg. Only player critters participate in networked movement;
        // treating every mapped object as a critter makes objectSetLocation()
        // recursively relocate gEgg until the stack overflows.
        for (int index = 0; index < NET_MAX_PLAYERS; index++) {
            MultiplayerPlayer* player = &gMpSession.players[index];
            if (player->isConnected && player->obj == obj
                && FID_TYPE(obj->fid) == OBJ_TYPE_CRITTER) {
                return true;
            }
        }
        return false;
    }
    // Client: any critter whose Object* is tracked in netIdToObj is networked
    // (i.e. its state is driven by the host). Scan by walking the netId table.
    if (gMpSession.netIdToObj == nullptr) {
        return false;
    }
    for (int i = 0; i < gMpSession.netIdToObjCount; i++) {
        if (gMpSession.netIdToObj[i] == obj) {
            return true;
        }
    }
    return false;
}

void MpClearNetIdMappings()
{
    gMpHostObjNetIds.clear();
    if (gMpSession.netIdToObj != nullptr) {
        free(gMpSession.netIdToObj);
        gMpSession.netIdToObj = nullptr;
    }
    gMpSession.netIdToObjCapacity = 0;
    gMpSession.netIdToObjCount = 0;
    gMpSession.nextObjNetId = MP_OBJ_NETID_BASE;
    gMpSession.clientSyncId = 0;
    gMpSession.clientSyncExpectedChunks = 0;
    gMpSession.clientSyncNextChunk = 0;
    gMpSession.clientTileSyncId = 0;
    gMpSession.clientTileSyncExpectedChunks = 0;
    gMpSession.clientTileSyncNextChunk = 0;
    memset(&gMpSession.clientMapMetadata, 0, sizeof(gMpSession.clientMapMetadata));
    gMpSession.clientMapMetadataValid = false;
    gMpHostObjectRecords.clear();
}

// ---------------------------------------------------------------------------
// Map transition hook (called from map.cc)
// ---------------------------------------------------------------------------

int MpOnMapTransitionRequested(MapTransition* transition)
{
    if (!gMpActive) {
        return 0; // single-player passthrough
    }
    if (transition == nullptr) {
        return 0;
    }
    if (transition->map <= 0) {
        // Worldmap travel is deliberately outside v1. Keeping the session on
        // one loaded map is safer than allowing the host and clients to enter
        // different worldmap/game modes.
        win_timed_msg("Worldmap travel is unavailable in co-op", COLOR_RED);
        return 1;
    }
    if (gMpSession.applyingNetworkState) {
        // A host-authoritative position update may place gDude on an exit-grid
        // tile. This is state application, not a new local transition request.
        return 1;
    }
    // If a vote concluded PASSED, let the real mapSetTransition proceed —
    // this is the host's resolve path calling the real transition after the
    // passed-display beat. FAILED/CANCELLED never start transitions, and
    // clients never start transitions on their own: their map changes are
    // always driven by the host's MAP_CHANGED.
    if (gMpIsHost && gVoteSession.state == VOTE_STATE_PASSED) {
        return 0;
    }
    // A vote is already in flight — block further transitions until it
    // resolves.
    if (gVoteSession.state == VOTE_STATE_ACTIVE) {
        return 1;
    }

    if (gMpIsHost) {
        // Local host player stepped on an exit grid: start the vote locally.
        MpVoteStart(transition, gMpSession.localNetId);
        return 1;
    }

    if (gMpIsClient) {
        // Ask the host to start a vote on our behalf. Freeze local input
        // until we hear back.
        NetVoteStartRequestPayload p;
        p.targetMap = transition->map;
        p.targetTile = transition->tile;
        p.targetElevation = transition->elevation;
        p.targetRotation = transition->rotation;
        NetSendPacket(gMpSession.hostPeer, NET_CHANNEL_RELIABLE, NET_PKT_VOTE_START_REQUEST, &p, sizeof(p));
        gMpSession.initiatorFrozen = true;
        return 1;
    }

    return 0;
}

int MpOnNetworkedPlayerTransitionRequested(Object* obj, MapTransition* transition)
{
    if (!gMpActive || !gMpIsHost || obj == nullptr || transition == nullptr) {
        return 0;
    }
    if (transition->map <= 0) {
        win_timed_msg("Worldmap travel is unavailable in co-op", COLOR_RED);
        return 1;
    }

    MultiplayerPlayer* player = mpHostFindPlayerByObject(obj);
    if (player == nullptr || !player->isHandshaken) {
        debugFilePrint("MP: networked transition rejected (no player) netObj=%u map=%d",
            MpGetObjNetId(obj), transition->map);
        return 0;
    }
    if (gVoteSession.state == VOTE_STATE_ACTIVE) {
        debugFilePrint("MP: networked transition blocked (vote active) netId=%u map=%d",
            player->netId, transition->map);
        return 1;
    }
    debugFilePrint("MP: networked transition vote start netId=%u map=%d tile=%d elev=%d",
        player->netId, transition->map, transition->tile, transition->elevation);
    // Start the vote BEFORE clearing the walk: the hook runs inside
    // _obj_connect_to_tile, and clearing a running move sequence re-places the
    // dude, which re-enters the exit-grid check. With the vote state already
    // ACTIVE the re-entry short-circuits (blocked) instead of recursing into
    // another vote start until the stack overflows.
    MpVoteStart(transition, player->netId);
    reg_anim_clear(obj);
    return 1;
}

} // namespace fallout
