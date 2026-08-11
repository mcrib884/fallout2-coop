#include "multiplayer.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

#include "animation.h"
#include "actions.h"
#include "art.h"
#include "combat.h"
#include "combat_defs.h"
#include "color.h"
#include "critter.h"
#include "debug.h"
#include "display_monitor.h"
#include "font_manager.h"
#include "game.h"
#include "geometry.h"
#include "input.h"
#include "interface.h"
#include "interpreter_extra.h"
#include "inventory.h"
#include "item.h"
#include "light.h"
#include "loadsave.h"
#include "map.h"
#include "multiplayer_vote.h"
#include "multiplayer_combat.h"
#include "multiplayer_debug.h"
#include "multiplayer_dialog.h"
#include "multiplayer_loot.h"
#include "multiplayer_perf.h"
#include "multiplayer_profile.h"
#include "queue.h"
#include "random.h"
#include "object.h"
#include "palette.h"
#include "perk.h"
#include "sfall_script_hooks.h"
#include "svga.h"
#include "window_manager.h"
#include "skill.h"
#include "party_member.h"
#include "message.h"
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
// The slot the client session was built from (see multiplayer.h). Set by the
// join paths before MpClientConnect; the connect fallback covers stragglers.
int gMpSessionSlot = -1;
// The netId of the player whose NET_PLAYER_ACTION is currently being handled
// on the host (0 when idle). Scripts run synchronously inside the action
// handler, so any monitor line emitted or elevator requested during that
// window belongs to the remote player's action — display_monitor.cc uses it
// to relay non-combat script feedback, scripts.cc uses it to block remote
// elevator use.
static uint32_t gMpRemoteActionNetId = 0;

// Latest host reply to a client's called-shot query (NET_PKT_TO_HIT_RESULT).
// The aiming window's modal loop consumes it via MpToHitResultTake and must
// match the target/mode against its own window before drawing.
static bool gToHitResultPending = false;
static uint32_t gToHitResultTarget = 0;
static uint8_t gToHitResultMode = 0;
static int gToHitResultProbs[8] = { 0 };

bool MpRemoteActionActive()
{
    return gMpRemoteActionNetId != 0;
}

uint32_t MpRemoteActionNetId()
{
    return gMpRemoteActionNetId;
}

void MpSetRemoteActionNetId(uint32_t netId)
{
    gMpRemoteActionNetId = netId;
}
// The current map's NATURAL entering position, captured on the client right
// after the map file load. The co-op map metadata overwrites
// gMapHeader.entering* with the host's position; client saves must carry the
// natural entrance instead of any session position.
static int gMpClientMapEnteringTile = -1;
static int gMpClientMapEnteringElevation = -1;
static int gMpClientMapEnteringRotation = -1;

// The client's map-entrance snapshot (see the statics above). Used by the
// save redirect so a client save never carries a co-op session position.
void MpGetClientMapEnteringPosition(int* tile, int* elevation, int* rotation)
{
    if (tile != nullptr) {
        *tile = gMpClientMapEnteringTile;
    }
    if (elevation != nullptr) {
        *elevation = gMpClientMapEnteringElevation;
    }
    if (rotation != nullptr) {
        *rotation = gMpClientMapEnteringRotation;
    }
}

static int mpRandomSpawnAnchor(int preferredTile, int elevation);
bool gMpSuppressExitGridCheck = false;
// Set while the synchronized dialogue modal is open on a client: MpTick
// skips the deferred-packet drain so session-changing packets never apply
// under the modal (the host's DIALOG_END closes the modal first).
bool gMpModalActive = false;
static uint32_t gMpLastTileRefreshTick = 0;
int gMpPendingHostStartAfterLoad = 0;
int gMpPendingClientStartAfterLoad = 0;
char gMpPendingClientAddress[64] = {};

struct MpHostObjectRecord {
    uint32_t netId;
    NetMapFullSyncObjectPayload state;
};

static std::vector<MpHostObjectRecord> gMpHostObjectRecords;

// Set by MpCombatPump (which runs inside inputGetInput) when it performed
// the host's object/player state broadcasts. MpTick reads this to avoid a
// second identical sweep in the same main-loop frame.
static uint32_t gMpLastHostBroadcastTick = 0;

void MpNoteHostBroadcastTick()
{
    gMpLastHostBroadcastTick = getTicks();
}

static bool mpHostBroadcastDoneThisTick(uint32_t nowTick)
{
    return nowTick == gMpLastHostBroadcastTick;
}
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
static bool gMpLocalProfileSyncReady = false;
static MpPlayerProfile gMpLastUploadedProfile;

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
    const MpPlayerProfile& profile, bool includeModel, uint8_t receiverNetId = 0,
    uint32_t changedSections = 0);
static void mpBroadcastProfileToClients(uint8_t netId, uint32_t objNetId,
    const MpPlayerProfile& profile, bool skipOwner, uint32_t changedSections = 0);
static void mpApplyReceivedProfile(uint8_t netId, uint32_t objNetId,
    const MpPlayerProfile& profile, uint32_t changedSections);
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

// Model-delivery knowledge. gMpReceiverKnownModels: host-side — models each
// connected client has already received (keyed by the receiver's player
// netId). gMpHostKnownModels: client-side — models the host has acked.
static std::unordered_map<uint8_t, std::unordered_set<uint32_t>> gMpReceiverKnownModels;
static std::unordered_set<uint32_t> gMpHostKnownModels;

static void mpZeroSession()
{
    memset(&gMpSession, 0, sizeof(gMpSession));
    gMpSession.state = MP_STATE_NONE;
    // Model-delivery knowledge lives outside the session struct (mpZeroSession
    // memsets it): host-side per-receiver sets of models each client already
    // holds, and the client-side set of models the host has acked.
    gMpReceiverKnownModels.clear();
    gMpHostKnownModels.clear();
}

// Host-side: has this receiver already been sent the model payload for
// modelHash? (modelHash 0 = no model — always "known".)
static bool mpReceiverHasModel(uint8_t receiverNetId, uint32_t modelHash)
{
    if (modelHash == 0) {
        return true;
    }
    auto it = gMpReceiverKnownModels.find(receiverNetId);
    return it != gMpReceiverKnownModels.end() && it->second.count(modelHash) != 0;
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
    const MpPlayerProfile& profile, bool includeModel, uint8_t receiverNetId,
    uint32_t changedSections)
{
    if (peer == nullptr || !MpProfileValidate(profile)) {
        debugFilePrint("MP: send profile invalid peer=%p netId=%u", (void*)peer, netId);
        return false;
    }

    std::vector<uint8_t> bytes;
    NetProfileSectionInfo infos[PROFILE_SECTION_COUNT];
    uint16_t sectionCount = 0;
    uint32_t bodyHash = 0;
    if (!MpProfileBuildBody(profile, changedSections, includeModel,
            &bytes, infos, &sectionCount, &bodyHash)
        || bytes.empty() || bytes.size() > MP_PROFILE_MAX_BYTES) {
        debugFilePrint("MP: send profile build failed netId=%u name='%s' sections=%u",
            netId, profile.name, changedSections);
        return false;
    }
    // Diag: the exact model state the body builder saw. files=0 with
    // includeModel=1 means the model payload never rode — the receiver then
    // installs the registry payload for the (possibly stale) hash.
    // (Commented: root-caused — the MODEL section bit now travels with picks.)
    // if (includeModel) {
    //     debugFilePrint("MP: send profile model diag netId=%u name='%.12s' modelName='%.12s' modelHash=%08X files=%zu sections=%u",
    //         netId, profile.name, profile.modelName, profile.modelHash,
    //         profile.modelFiles.size(), sectionCount);
    // }
    // Diag: the first bytes must be section headers [id][0][size]; if the
    // wire body starts with text the sender is not using the sectioned path.
    {
        size_t n = bytes.size() < 24 ? bytes.size() : 24;
        std::string hex;
        for (size_t i = 0; i < n; i++) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02X", bytes[i]);
            hex += buf;
        }
        debugFilePrint("MP: send profile head netId=%u bytes=%zu head=%s",
            netId, bytes.size(), hex.c_str());
    }

    constexpr size_t kChunkDataSize = NET_MAX_PACKET_SIZE
        - sizeof(NetPacketHeader) - sizeof(NetPlayerProfileChunkHeader);
    uint32_t chunkCount = (uint32_t)((bytes.size() + kChunkDataSize - 1) / kChunkDataSize);
    if (chunkCount == 0 || chunkCount > UINT16_MAX) {
        debugFilePrint("MP: send profile chunk count invalid bytes=%zu chunks=%u", bytes.size(), chunkCount);
        return false;
    }

    uint32_t streamId = mpAllocProfileStreamId();
    debugFilePrint("MP: send profile begin netId=%u name='%s' gen=%u objNet=%u bytes=%zu chunks=%u stream=%u model=%d sections=%u",
        netId, profile.name, profile.generation, objNetId, bytes.size(), chunkCount, streamId,
        includeModel ? 1 : 0, sectionCount);
    NetPlayerProfileBeginPayload begin;
    memset(&begin, 0, sizeof(begin));
    begin.streamId = streamId;
    begin.netId = netId;
    begin.modelIncluded = includeModel ? 1 : 0;
    begin.schemaVersion = profile.schemaVersion;
    begin.generation = profile.generation;
    begin.objNetId = objNetId;
    begin.totalBytes = (uint32_t)bytes.size();
    begin.chunkCount = chunkCount;
    begin.contentHash = bodyHash;
    begin.sectionCount = sectionCount;
    for (uint16_t i = 0; i < sectionCount && i < PROFILE_SECTION_COUNT; i++) {
        begin.sections[i] = infos[i];
    }
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
    bool sent = NetSendPacket(peer, NET_CHANNEL_RELIABLE, NET_PKT_PLAYER_PROFILE_END,
        &end, sizeof(end));
    if (sent && gMpIsHost && includeModel && receiverNetId != 0
        && !profile.modelFiles.empty() && profile.modelHash != 0) {
        // The receiver now holds this model — skip the payload on the next
        // profile to it (until the model changes).
        gMpReceiverKnownModels[receiverNetId].insert(profile.modelHash);
    }
    return sent;
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
    uint32_t generation, bool accepted, uint16_t reason = 0, uint32_t modelHash = 0)
{
    if (peer == nullptr) return;
    NetPlayerProfileAckPayload ack;
    memset(&ack, 0, sizeof(ack));
    ack.streamId = streamId;
    ack.netId = netId;
    ack.accepted = accepted ? 1 : 0;
    ack.reason = reason;
    ack.generation = generation;
    ack.modelHash = modelHash;
    NetSendPacket(peer, NET_CHANNEL_RELIABLE, accepted
        ? NET_PKT_PLAYER_PROFILE_ACK
        : NET_PKT_PLAYER_PROFILE_REJECT, &ack, sizeof(ack));
}

static void mpApplyReceivedProfile(uint8_t netId, uint32_t objNetId,
    const MpPlayerProfile& profile, uint32_t changedSections)
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
        MpProfileApplyLocal(profile, /*applyPcStats=*/false, changedSections);
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
        // The runtime avatar is the player's only interactive object on this
        // machine — register it under its object netId so reverse lookups
        // (attack targets, USE_SKILL targets) resolve.
        MpRegisterObjNetId(runtime->object, player->objNetId);
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

// Sends the host's full gvar table to one peer (join seeding, resync after a
// client load). The client's quest mirror then stays live via GVAR_CHANGE.
static void mpHostSendGvarSnapshot(ENetPeer* peer, uint32_t netId)
{
    if (peer == nullptr || gGameGlobalVars == nullptr || gGameGlobalVarsLength <= 0) {
        return;
    }
    const int valueCount = (gGameGlobalVarsLength < NET_GVAR_MAX_VALUES)
        ? gGameGlobalVarsLength : NET_GVAR_MAX_VALUES;
    const int payloadSize = (int)sizeof(NetGvarSnapshotPayload);
    NetGvarSnapshotPayload* snapshot = (NetGvarSnapshotPayload*)malloc(payloadSize);
    if (snapshot == nullptr) {
        return;
    }
    snapshot->count = (uint32_t)valueCount;
    memcpy(snapshot->values, gGameGlobalVars, valueCount * sizeof(int32_t));
    NetSendPacket(peer, NET_CHANNEL_RELIABLE, NET_PKT_GVAR_SNAPSHOT, snapshot, payloadSize);
    free(snapshot);
    debugFilePrint("MP: gvar snapshot sent netId=%u count=%d (local=%d)",
        netId, valueCount, gGameGlobalVarsLength);
}

static void mpHostAcceptProfile(MultiplayerPlayer* player, ENetPeer* peer,
    const MpPlayerProfile& profile, uint32_t streamId, uint32_t changedSections)
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
        if (!MpProfileApplyRuntimeUpdate(player->netId, profile, changedSections)) {
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
        mpSendProfileAck(peer, player->netId, streamId, player->profileGeneration, true,
            0, runtime != nullptr ? runtime->profile.modelHash : 0);
        // Other clients must see the updated sheet (perks/items/stats) too.
        // The owner is skipped: it just uploaded this exact generation, and
        // the echo's inventory rebuild is pure waste. Host-side changes to
        // this player's sheet are still broadcast back to it by the per-tick
        // detect (mpHostSyncProfiles, skipOwner=false).
        mpBroadcastProfileToClients(player->netId, player->objNetId,
            runtime != nullptr ? runtime->profile : profile, /*skipOwner=*/true,
            changedSections);
        player->lastProfileBroadcastGeneration = player->profileGeneration;
        return;
    }

    debugFilePrint("MP: host accept profile begin netId=%u name='%s' gen=%u",
        player->netId, profile.name, profile.generation);

    // The joining player's profile carries their real position on this map
    // (both machines loaded the same .map). Spawning there keeps the first
    // state update from snapping the client's character to a spawn tile.
    // Fall back to a spot near the host only when the profile position is
    // unusable.
    // Co-op: spawn the joiner around the HOST critter (small random radius),
    // not at the profile's own SP position — the session world may be a
    // different map entirely, and the profile tile could land the avatar
    // anywhere. The client follows via the player-state sync.
    int32_t tile = hexGridTileIsValid(gDude->tile)
        ? mpRandomSpawnAnchor(gDude->tile, gDude->elevation)
        : (hexGridTileIsValid(gMapHeader.enteringTile) ? gMapHeader.enteringTile : gDude->tile);
    int32_t elevation = gDude != nullptr && elevationIsValid(gDude->elevation)
        ? gDude->elevation
        : (gMapHeader.enteringElevation >= 0 ? gMapHeader.enteringElevation : 0);
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

    mpSendProfileAck(peer, player->netId, streamId, profile.generation, true, 0,
        profile.modelHash);

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
        // The joining peer starts with an empty model registry — always ship
        // the payload, and record it under the joiner's netId so later
        // broadcasts skip it.
        mpSendProfile(peer, other->netId, other->objNetId, otherRuntime->profile,
            /*includeModel=*/true, player->netId);
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
                // Ship the joiner's model to this client only if it does not
                // already hold the payload (a previous player may have used
                // the same model).
                mpSendProfile(other->peer, player->netId, player->objNetId,
                    newRuntime->profile,
                    /*includeModel=*/!mpReceiverHasModel(other->netId,
                        newRuntime->profile.modelHash),
                    other->netId);
            }
            NetSendPacket(other->peer, NET_CHANNEL_RELIABLE, NET_PKT_PLAYER_JOINED,
                &joined, sizeof(joined));
        }
    }

    MpBroadcastMapFullSync(peer);

    // Quest state: seed the joiner with the host's full gvar table. Every
    // write after this point rides NET_PKT_GVAR_CHANGE (gameSetGlobalVar
    // hook), so the client's quest mirror stays live for its own saves.
    mpHostSendGvarSnapshot(peer, player->netId);

    // Session cheat policy: the joiner must know before it can open the F11
    // cheat menu (client cheats may be disabled by the host).
    MpDebugSendCheatPolicyTo(peer);

    // A client that joins while combat is already running never saw the
    // STARTED broadcast (it predates the connection). Enter it into the
    // mirror immediately, then tell it whose turn it is — without the turn
    // info, a client whose turn is live on the host would never send
    // TURN_END and the host's remote-turn loop would stall forever.
    if (gMpCombat.inCombat) {
        NetSendPacket(peer, NET_CHANNEL_RELIABLE, NET_PKT_COMBAT_STARTED, nullptr, 0);
        NetCombatTurnStartPayload turnPayload;
        memset(&turnPayload, 0, sizeof(turnPayload));
        turnPayload.netId = gMpCombat.whoseTurn;
        if (gMpCombat.whoseTurn != 0) {
            MultiplayerPlayer* turnPlayer = &gMpSession.players[gMpCombat.whoseTurn - 1];
            if (turnPlayer->obj != nullptr) {
                turnPayload.ap = (uint16_t)std::clamp(
                    turnPlayer->obj->data.critter.combat.ap, 0, 65535);
                turnPayload.maxAp = (uint16_t)std::clamp(
                    critterGetStat(turnPlayer->obj, STAT_MAXIMUM_ACTION_POINTS), 0, 65535);
            }
        }
        // targetNetId stays 0: the acting-NPC netId is not tracked host-side;
        // the next NPC TURN_START broadcast restores the red outline.
        NetSendPacket(peer, NET_CHANNEL_RELIABLE, NET_PKT_COMBAT_TURN_START,
            &turnPayload, sizeof(turnPayload));
        debugFilePrint("MP: join mid-combat start sent netId=%u turn=%u",
            player->netId, gMpCombat.whoseTurn);
    }

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
    // The content hash covers exactly the shipped bytes (the received body).
    if (MpProfileHashBytes(state.bytes.data(), state.bytes.size()) != state.expectedHash) {
        debugFilePrint("MP: profile end hash failed netId=%u stream=%u bytes=%zu",
            state.netId, state.streamId, state.bytes.size());
        mpSendProfileReject(peer, state.netId, state.streamId, 5);
        return;
    }
    {
        // Diag: the reassembled body must start with section headers; a text
        // head here means the sender's body is not sectioned.
        size_t n = state.bytes.size() < 24 ? state.bytes.size() : 24;
        std::string hex;
        for (size_t i = 0; i < n; i++) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02X", state.bytes[i]);
            hex += buf;
        }
        debugFilePrint("MP: profile end head netId=%u bytes=%zu head=%s",
            state.netId, state.bytes.size(), hex.c_str());
    }

    // Partial transfers merge over the stored profile: absent sections keep
    // their current values. The changed-sections mask tells the apply paths
    // exactly what to touch.
    MpPlayerProfile merged;
    MpPlayerRuntime* storedRuntime = MpProfileGetRuntime(state.netId);
    if (storedRuntime != nullptr) {
        merged = storedRuntime->profile;
    }
    uint32_t changedSections = MpProfileDeserializeSections(state.bytes.data(),
        state.bytes.size(), &merged);
    if (changedSections == 0) {
        debugFilePrint("MP: profile end deserialize failed netId=%u stream=%u bytes=%zu",
            state.netId, state.streamId, state.bytes.size());
        mpSendProfileReject(peer, state.netId, state.streamId, 5);
        return;
    }
    merged.generation = state.generation;
    merged.schemaVersion = MP_PROFILE_SCHEMA_VERSION;
    // The model payload may have been skipped (the sender assumed this peer
    // already holds it). If we genuinely don't have it, ask for a re-send
    // WITH the payload and drop this copy — applying without the model would
    // fail anyway.
    if (!MpProfileResolveModel(&merged)) {
        NetModelRequestPayload req;
        req.netId = state.netId;
        req.modelHash = merged.modelHash;
        NetSendPacket(peer, NET_CHANNEL_RELIABLE, NET_PKT_MODEL_REQUEST, &req, sizeof(req));
        debugFilePrint("MP: profile model missing, requested netId=%u hash=%08X",
            state.netId, merged.modelHash);
        return;
    }
    debugFilePrint("MP: profile end complete netId=%u stream=%u name='%s' gen=%u bytes=%zu sections=%08X",
        state.netId, state.streamId, merged.name, merged.generation, state.bytes.size(),
        changedSections);

    if (gMpIsHost) {
        MultiplayerPlayer* player = mpPlayerFindByPeer(peer);
        mpHostAcceptProfile(player, peer, merged, state.streamId, changedSections);
    } else {
        mpApplyReceivedProfile(state.netId, state.objNetId, merged, changedSections);
        mpSendProfileAck(peer, state.netId, state.streamId, merged.generation, true);
    }
}

static void mpBuildMapSyncPayload(NetMapSyncPayload* payload)
{
    if (payload == nullptr) {
        return;
    }

    memset(payload, 0, sizeof(*payload));
    payload->mapId = gMapHeader.index;
    // gMapHeader.name can carry the file extension (a host that loaded its
    // save with a map .SAV present stores "MAP.SAV" — vanilla behavior). The
    // joining client must resolve the bare name against its own map files,
    // so strip the extension before it goes on the wire.
    {
        char nameBuf[NET_MAP_NAME_LENGTH];
        strncpy(nameBuf, gMapHeader.name, sizeof(nameBuf) - 1);
        nameBuf[sizeof(nameBuf) - 1] = '\0';
        char* dot = strrchr(nameBuf, '.');
        if (dot != nullptr) {
            *dot = '\0';
        }
        memcpy(payload->mapName, nameBuf, sizeof(payload->mapName));
    }
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

// Fullscreen blackout held while a client is joining (CLIENT_SYNCING), so the
// world-streaming phase and the final camera reveal are not visible. The
// reveal is the state flip to CLIENT_PLAYING: the camera is already centered
// on the local player when the blackout lifts — no camera reset jump.
static int gMpJoinBlackoutWin = -1;

static void mpJoinBlackoutShow()
{
    if (!gMpIsClient || gMpJoinBlackoutWin != -1) {
        return;
    }
    gMpJoinBlackoutWin = windowCreate(0, 0, screenGetWidth(), screenGetHeight(),
        COLOR_BLACK, WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    if (gMpJoinBlackoutWin != -1) {
        windowRefresh(gMpJoinBlackoutWin);
        debugFilePrint("MP: join blackout shown");
    }
}

static void mpJoinBlackoutHide()
{
    if (gMpJoinBlackoutWin == -1) {
        return;
    }
    windowDestroy(gMpJoinBlackoutWin);
    gMpJoinBlackoutWin = -1;
    debugFilePrint("MP: join blackout hidden");
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
    // Defer the map-enter script until the full sync arrives: the cutscene
    // (e.g. the temple exit's vault-suit scene) checks globals/objects that
    // only exist once the host's state is applied. Running it now returns
    // instantly and the client skips the cutscene.
    gMpClientDeferMapEnterScript = true;
    int rc = mapLoadByName(mapName);
    if (rc == -1 && strstr(mapName, ".MAP") == nullptr && strstr(mapName, ".SAV") == nullptr) {
        // Bare map name: resolve it against the .MAP file (loose or in the
        // data archive) — the map loader only finds the file with the full
        // extension.
        char extName[NET_MAP_NAME_LENGTH + 8];
        snprintf(extName, sizeof(extName), "%s.MAP", mapName);
        rc = mapLoadByName(extName);
    }
    gMpClientDeferMapEnterScript = false;
    mpSetDudeInventoryProtected(false);
    debugFilePrint("MPDBG after map load: rc=%d dude=%p pid=0x%X pt=%d tile=%d elev=%d hidden=%d st=%d carry=%d weight=%d",
        rc, (void*)gDude,
        gDude != nullptr ? gDude->pid : 0,
        gDude != nullptr ? objectTypeFromPid(gDude->pid) : -1,
        gDude != nullptr ? gDude->tile : -1,
        gDude != nullptr ? gDude->elevation : -1,
        gDude != nullptr ? ((gDude->flags & OBJECT_HIDDEN) != 0) : -1,
        gDude != nullptr ? critterGetStat(gDude, STAT_STRENGTH) : -1,
        gDude != nullptr ? critterGetStat(gDude, STAT_CARRY_WEIGHT) : -1,
        gDude != nullptr ? objectGetInventoryWeight(gDude) : -1);
    // mpDebugDumpWalls("client-pre-sync", 12);
    // mpDebugDumpLightState("client-after-load");
    if (rc == 0) {
        // Snapshot the map file's natural entering position before the co-op
        // metadata overwrites gMapHeader.entering* with the host's position.
        gMpClientMapEnteringTile = gMapHeader.enteringTile;
        gMpClientMapEnteringElevation = gMapHeader.enteringElevation;
        gMpClientMapEnteringRotation = gMapHeader.enteringRotation;
        debugFilePrint("MP: client map entering snapshot tile=%d elev=%d rot=%d",
            gMapHeader.enteringTile, gMapHeader.enteringElevation, gMapHeader.enteringRotation);
        mpSnapshotMapStaticObjects();
        mpDebugSnapshotPreSyncWalls();
        mpShowClientPlayer(gDude);
    }
    return rc;
}

static void mpDebugDumpWalls(const char* tag, int limit)
{
    // (Commented: temporary join-time wall dump — the 3000+ line dumps at
    // every map sync were pure noise. See git history to re-enable.)
    (void)tag;
    (void)limit;
}

static void mpDebugDumpLightState(const char* tag)
{
    // (Commented: temporary join-time light dump — see git history.)
    (void)tag;
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
        int objType = objectTypeFromFid(it->fid);
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
    // (Commented: temporary join-time wall diff - see git history.)
    (void)0;
}

static void mpClientApplyMapMetadata()
{
    if (!gMpSession.clientMapMetadataValid) {
        return;
    }

    const NetMapSyncPayload* metadata = &gMpSession.clientMapMetadata;
    gMapHeader.enteringTile = metadata->enteringTile;
    gMapHeader.enteringElevation = metadata->enteringElevation;
    gMapHeader.enteringRotation = static_cast<Rotation>(metadata->enteringRotation);
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
        // debugFilePrint("MPDBG sync wait: map metadata not valid");
        return;
    }
    if (gMpSession.clientSyncExpectedChunks == 0) {
        // debugFilePrint("MPDBG sync wait: no object chunks yet");
        return;
    }
    if (gMpSession.clientSyncNextChunk < gMpSession.clientSyncExpectedChunks) {
        // debugFilePrint("MPDBG sync wait: object chunks %u/%u",
        //     gMpSession.clientSyncNextChunk, gMpSession.clientSyncExpectedChunks);
        return;
    }
    if (gMpSession.clientTileSyncExpectedChunks == 0) {
        // debugFilePrint("MPDBG sync wait: no tile chunks yet");
        return;
    }
    if (gMpSession.clientTileSyncNextChunk < gMpSession.clientTileSyncExpectedChunks) {
        // debugFilePrint("MPDBG sync wait: tile chunks %u/%u",
        //     gMpSession.clientTileSyncNextChunk, gMpSession.clientTileSyncExpectedChunks);
        return;
    }

    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        const MultiplayerPlayer* player = &gMpSession.players[index];
        if (player->isConnected && (!player->profileReady || player->obj == nullptr
                || !player->hasInitialState)) {
            // debugFilePrint("MPDBG sync wait: player netId=%u name='%s' ready=%d obj=%p initial=%d",
            //     player->netId, player->name, player->profileReady ? 1 : 0,
            //     (void*)player->obj, player->hasInitialState ? 1 : 0);
            return;
        }
    }

    mpClientApplyMapMetadata();
    // debugFilePrint("MPDBG sync done: dude=%p pid=0x%X pt=%d fid=0x%X anim=%d tile=%d elev=%d hidden=%d st=%d carry=%d weight=%d center=%d",
    //     (void*)gDude,
    //     gDude != nullptr ? gDude->pid : 0,
    //     gDude != nullptr ? objectTypeFromPid(gDude->pid) : -1,
    //     gDude != nullptr ? gDude->fid : 0,
    //     gDude != nullptr ? animationTypeFromFid(gDude->fid) : -1,
    //     gDude != nullptr ? gDude->tile : -1,
    //     gDude != nullptr ? gDude->elevation : -1,
    //     gDude != nullptr ? ((gDude->flags & OBJECT_HIDDEN) != 0) : -1,
    //     gDude != nullptr ? critterGetStat(gDude, STAT_STRENGTH) : -1,
    //     gDude != nullptr ? critterGetStat(gDude, STAT_CARRY_WEIGHT) : -1,
    //     gDude != nullptr ? objectGetInventoryWeight(gDude) : -1,
    //     gCenterTile);
    // mpDebugDumpWalls("client-synced", 30);
    // mpDebugReportMissingWalls();
    // mpDebugDumpLightState("client-sync-done");
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
    // camera on the local player so he is on screen — this is the join reveal:
    // the blackout lifts already focused on the player, no camera jump.
    if (gDude != nullptr && hexGridTileIsValid(gDude->tile)) {
        tileSetCenter(gDude->tile,
            TILE_SET_CENTER_REFRESH_WINDOW | TILE_SET_CENTER_FLAG_IGNORE_SCROLL_RESTRICTIONS);
    }
    // Run the deferred map-enter script now that the full sync (globals,
    // objects, tiles, player states) is applied, mirroring the vanilla
    // map-load prologue. A cutscene like the temple exit's vault-suit scene
    // needs this state to evaluate its conditions.
    //
    // Co-op: the script must NOT run inline here - this function executes
    // inside the network receive path (packet handlers). A cutscene that
    // opens a dialog, waits on timed events, or blocks in any way would
    // deadlock: while scriptExecProc blocks, the receive handler never
    // returns, so no further packets, script requests, or queue drains can
    // be serviced. Instead we mark the run pending and let MpTick execute it
    // on the next top-level tick, where the full main-loop services (input,
    // dialog modal pumping, timed-event queue) are available.
    if (gMpClientDeferredMapEnterPending) {
        gMpClientDeferredMapEnterPending = false;
        gMpClientDeferredMapEnterRun = true;
        debugFilePrint("MAP: client deferred map enter queued sid=%d (exec from MpTick)",
            gMapSid);
    }
    tileWindowRefreshFull();
    gMpSession.state = MP_STATE_CLIENT_PLAYING;
    mpJoinBlackoutHide();
}

// Enter CLIENT_SYNCING and arm the sync watchdog. Every path that switches the
// client into the syncing state must go through here so the timeout clock and
// the state stay consistent.
static void mpEnterClientSync()
{
    // Joining (previous state CONNECTING): hold a fullscreen blackout over the
    // world-streaming phase. Map changes (previous state PLAYING) keep their
    // vanilla transition visuals — the blackout is join-only.
    if (gMpIsClient && gMpSession.state == MP_STATE_CLIENT_CONNECTING) {
        mpJoinBlackoutShow();
    }
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

// ---------------------------------------------------------------------------
// destination truncation (co-op: never stack two avatars on one tile)
// ---------------------------------------------------------------------------

int MpTruncateDestinationAtOccupant(Object* mover, int tile, int elevation)
{
    if (mover == nullptr || tile == mover->tile || elevation != mover->elevation) {
        return tile;
    }

    // Does the destination tile hold a live critter other than the mover?
    bool occupied = false;
    Object* obj = objectFindFirstAtLocation(elevation, tile);
    while (obj != nullptr) {
        if (obj != mover && objectTypeFromFid(obj->fid) == OBJ_TYPE_CRITTER && !critterIsDead(obj)) {
            occupied = true;
            break;
        }
        obj = objectFindNextAtLocation();
    }
    if (!occupied) {
        return tile;
    }

    // Walk the straight line toward the destination and stop at the last
    // free tile before the occupant.
    Rotation rotation = tileGetRotationTo(mover->tile, tile);
    int distance = objectGetDistanceBetweenTiles(mover, mover->tile, mover, tile);
    int lastFree = mover->tile;
    for (int step = 0; step < distance; step++) {
        int next = tileGetTileInDirection(lastFree, rotation, 1);
        if (next == lastFree || next == tile) {
            break;
        }
        bool free = true;
        Object* check = objectFindFirstAtLocation(elevation, next);
        while (check != nullptr) {
            if (check != mover && objectTypeFromFid(check->fid) == OBJ_TYPE_CRITTER && !critterIsDead(check)) {
                free = false;
                break;
            }
            check = objectFindNextAtLocation();
        }
        if (!free) {
            break;
        }
        lastFree = next;
    }
    return lastFree;
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
        // debugFilePrint("MP: movement rejected in combat obj=%p tile=%d elev=%d",
        //     (void*)obj, tile, elevation);
        return -1;
    }

    // Co-op: a walk must never resolve onto a tile occupied by another
    // critter — stacked avatars break targeting for both players.
    int targetTile = MpTruncateDestinationAtOccupant(obj, tile, elevation);
    if (targetTile != tile) {
        debugFilePrint("MP: move truncated obj=%p tile=%d->%d (occupied)",
            (void*)obj, tile, targetTile);
        tile = targetTile;
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
        int walkFid = buildFid(objectTypeFromFid(obj->fid), obj->fid & 0xFFF,
            isRun ? ANIM_RUNNING : ANIM_WALK, (obj->fid & 0xF000) >> 12, obj->rotation + 1);
        debugFilePrint("MP: movement registration failed netObj=%u run=%d tile=%d rc=%d end=%d objTile=%d objElev=%d hidden=%d busy=%d objFid=0x%X walkFid=0x%X artWalk=%d artRun=%d",
            MpGetObjNetId(obj), isRun ? 1 : 0, tile, rc, endRc,
            obj->tile, obj->elevation,
            (obj->flags & OBJECT_HIDDEN) != 0 ? 1 : 0,
            animationIsBusy(obj) != 0 ? 1 : 0,
            obj->fid, walkFid,
            artExists(buildFid(objectTypeFromFid(obj->fid), obj->fid & 0xFFF, ANIM_WALK, (obj->fid & 0xF000) >> 12, obj->rotation + 1)) ? 1 : 0,
            artExists(buildFid(objectTypeFromFid(obj->fid), obj->fid & 0xFFF, ANIM_RUNNING, (obj->fid & 0xF000) >> 12, obj->rotation + 1)) ? 1 : 0);
        return -1;
    }

    // Vanilla parity: running cancels sneaking unless the Silent Running perk
    // (see _dude_run_to_tile). The client already disabled its local dude's
    // sneak at click time; the host's word here clears the avatar's proto
    // flag so the next player-state broadcast carries sneak=off and the
    // client stays out of sneak instead of being re-enabled by the sync.
    if (isRun && perkGetRank(obj, PERK_SILENT_RUNNING) == 0) {
        Proto* playerProto = nullptr;
        if (protoGetProto(obj->pid, &playerProto) == 0
            && (playerProto->critter.data.flags & (1 << DUDE_STATE_SNEAKING)) != 0) {
            playerProto->critter.data.flags &= ~(1 << DUDE_STATE_SNEAKING);
            debugFilePrint("MP: run cancelled sneak netId=%u", MpGetObjNetId(obj));
        }
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

// Process-local UI helpers (hex cursor, bouncing cursor, crosshair, exit-grid
// markers) are OBJ_TYPE_INTERFACE objects that exist on every machine. They
// must never be networked: a broadcast cursor state would render one player's
// tile highlight on the other's screen and ghost the local cursor.
static bool mpIsLocalUiObject(const Object* obj)
{
    return obj != nullptr && objectTypeFromFid(obj->fid) == OBJ_TYPE_INTERFACE;
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
        || mpObjectIsInAnyPlayerInventory(obj) || mpIsLocalUiObject(obj)) {
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
    if (objectTypeFromFid(obj->fid) == OBJ_TYPE_CRITTER) {
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
        for (Rotation rotation = ROTATION_FIRST; rotation < ROTATION_COUNT; rotation++) {
            int tile = tileGetTileInDirection(preferredTile, rotation, distance);
            if (hexGridTileIsValid(tile) && !_obj_occupied(tile, elevation)) {
                return tile;
            }
        }
    }

    return preferredTile;
}

// Randomize a spawn anchor within a small radius (1-2 hexes in a random
// direction) so co-op clients scatter around the host critter instead of
// stacking on one deterministic tile. Falls back to the anchor when no
// candidate is free.
static int mpRandomSpawnAnchor(int preferredTile, int elevation)
{
    if (!hexGridTileIsValid(preferredTile)) {
        return preferredTile;
    }
    for (int attempt = 0; attempt < 8; attempt++) {
        Rotation rotation = static_cast<Rotation>(randomBetween(0, ROTATION_COUNT - 1));
        int distance = randomBetween(1, 2);
        int tile = tileGetTileInDirection(preferredTile, rotation, distance);
        if (hexGridTileIsValid(tile) && !_obj_occupied(tile, elevation)) {
            return tile;
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
    MpDialogInit();
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
    MpDialogShutdown();
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
    MpDialogReset();
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

    // The session slot: the save this game came from (loaded), or the next
    // empty slot (new game — the main.cc new-game path already set it; this
    // is a safety net for any hosting path that missed the explicit set).
    if (gMpSessionSlot < 0) {
        gMpSessionSlot = lsgGetLastLoadedSlot();
        if (gMpSessionSlot < 0) {
            gMpSessionSlot = lsgGetCoopSaveSlot();
        }
        debugFilePrint("MP: host session slot fallback slot=%d", gMpSessionSlot);
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
    // mpDebugDumpWalls("host", 12);
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

    if (gMpSessionSlot < 0) {
        // No join path recorded a slot (should not happen): fall back to the
        // hidden co-op slot so client saves never hit a picker mid-session.
        gMpSessionSlot = lsgGetCoopSaveSlot();
        debugFilePrint("MP: client join slot fallback coop slot=%d", gMpSessionSlot);
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

    // Never leave the join blackout up (disconnect during sync / timeout).
    mpJoinBlackoutHide();

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
    MpPerfBegin(MP_PERF_MPTICK_NET);
    NetHostService(gMpSession.enetHost, mpOnNetEvent, nullptr);
    MpPerfEnd(MP_PERF_MPTICK_NET);
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

    // Co-op: a dialogue participant disconnected — remove + recalc the vote.
    MpDialogHostPlayerDisconnected(netId);

    // Co-op: a loot session owner disconnected — release the session.
    MpLootHostPlayerDisconnected(netId);

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
    const MpPlayerProfile& profile, bool skipOwner, uint32_t changedSections)
{
    // Diag: what the broadcast hands to the send path — files=0 + a stale
    // hash here means the caller passed a model-free copy; the receiver then
    // resolves the registry payload for the stale hash (poisoned install).
    // (Commented: root-caused — see git history.)
    // debugFilePrint("MPROF: broadcast profile netId=%u name='%.12s' modelName='%.12s' modelHash=%08X files=%zu sections=%08X",
    //     netId, profile.name, profile.modelName, profile.modelHash,
    //     profile.modelFiles.size(), changedSections);
    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        MultiplayerPlayer* other = &gMpSession.players[index];
        if (!other->isConnected || !other->isHandshaken || other->peer == nullptr) {
            continue;
        }
        if (skipOwner && other->netId == netId) {
            // The owner is the canonical source of this sheet — it just
            // uploaded it. Echoing it back wastes a full transfer and forces
            // a needless inventory rebuild on the sender. Host-side changes
            // still reach the owner through the detect path (skipOwner=false).
            continue;
        }
        // Skip the model payload for clients that already hold it.
        bool includeModel = !mpReceiverHasModel(other->netId, profile.modelHash);
        mpSendProfile(other->peer, netId, objNetId, profile, includeModel, other->netId,
            changedSections);
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
        // Per-section change detection: the volatile runtime fields (transform,
        // hp/ap, combat state, object flags) are in NO section by construction,
        // so no volatile-zeroing footgun can ever hide here again. Only the
        // persistent sections are compared; the MODEL section compares its
        // identity (hash) only.
        uint32_t changedSections = 0;
        for (int sectionId = PROFILE_SECTION_IDENTITY;
            sectionId <= PROFILE_SECTION_SKILL_USE; sectionId++) {
            if (MpProfileSectionChanged(captured, stored, (uint8_t)sectionId)) {
                changedSections |= (1u << (sectionId - 1));
            }
        }
        if (changedSections == 0) {
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
                // The change-detection snapshot is model-file-free and its
                // modelName/hash never flag the MODEL section for a skin
                // change — without the bit the wire body drops the model
                // payload. Force it so the fresh files ride when they matter.
                changedSections |= (1u << (PROFILE_SECTION_MODEL - 1));
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
        // Generation dedup: a client's own upload was already echoed by the
        // accept path (lastProfileBroadcastGeneration). Only genuinely newer
        // generations — host-side changes the clients cannot know about (XP
        // grants, host-resolved drops, script rewards) — go out here. This
        // breaks the upload->apply->detect->rebroadcast feedback loop.
        if (runtime->profile.generation <= player->lastProfileBroadcastGeneration) {
            debugFilePrint("MP: profile change suppressed netId=%u generation=%u lastBroadcast=%u",
                netId, runtime->profile.generation,
                player->lastProfileBroadcastGeneration);
            continue;
        }
        mpBroadcastProfileToClients(netId, player->objNetId, runtime->profile,
            /*skipOwner=*/false, changedSections);
        player->lastProfileBroadcastGeneration = runtime->profile.generation;
        debugFilePrint("MP: profile changed netId=%u generation=%u sections=%08X broadcast",
            netId, runtime->profile.generation, changedSections);
    }
}

// Forces the next periodic profile sync to run immediately (the skin picker
// calls this so a model change reaches the other players right away instead
// of waiting out the 1s cadence). The host's sync already runs every tick and
// diff-detects changes, so only the client upload needs the nudge.
void MpProfileForceSync()
{
    gMpLocalProfileSyncTick = 0;
}

// Unified client->host character-sheet sync: capture the local sheet once a
// second and upload the CHANGED sections through the profile channel. The
// host applies them onto the avatar in place (MpProfileApplyRuntimeUpdate),
// so level-up perks, spent skill points, looted/dropped items, script XP and
// stat changes all reach the host's combat resolution and every other client
// without any per-attribute protocol. Volatile fields (transform, hp/ap,
// combat state, object flags) are in NO section - they never ride this path.
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

    uint32_t changedSections = 0;
    for (int sectionId = PROFILE_SECTION_IDENTITY;
        sectionId <= PROFILE_SECTION_SKILL_USE; sectionId++) {
        if (MpProfileSectionChanged(captured, gMpLastUploadedProfile,
                (uint8_t)sectionId)) {
            changedSections |= (1u << (sectionId - 1));
        }
    }
    // The model-free capture leaves modelName stale, so a skin change never
    // flags the MODEL section (and the wire body would drop the payload).
    // Compare the local dude's live proto model directly.
    {
        int liveModelId = -1;
        Proto* dudeProto = nullptr;
        if (gDude != nullptr && protoGetProto(gDude->pid, &dudeProto) == 0) {
            liveModelId = dudeProto->critter.fid & 0xFFF;
        }
        if (liveModelId >= 0
            && liveModelId != (gMpLastUploadedProfile.prototypeFid & 0xFFF)) {
            changedSections |= (1u << (PROFILE_SECTION_MODEL - 1));
        }
    }
    if (gMpLocalProfileSyncReady && changedSections == 0) {
        return;
    }

    if (!gMpLocalProfileSyncReady) {
        // First detection after join: the join-time upload is the baseline —
        // never immediately re-upload an identical sheet.
        gMpLastUploadedProfile = captured;
        gMpLocalProfileSyncReady = true;
        return;
    }

    // Build the upload. Same-model updates keep the installed model payload
    // (the host preserves it); a model identity change (armor swap) requires
    // the full capture so the new files ride along.
    MpPlayerProfile upload = captured;
    bool modelChanged = (changedSections & (1u << (PROFILE_SECTION_MODEL - 1))) != 0;
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
    // Skip the model payload once the host has acked holding it; only model
    // identity changes (new hash) or the first upload carry the files.
    bool includeModel = !gMpHostKnownModels.count(upload.modelHash);
    if (!mpSendProfile(gMpSession.hostPeer, localNetId, player->objNetId, upload,
            includeModel, /*receiverNetId=*/0, changedSections)) {
        debugFilePrint("MP: local profile upload failed netId=%u gen=%u sections=%08X",
            localNetId, upload.generation, changedSections);
        return;
    }
    gMpLastUploadedProfile = captured;
    // Optimistically advance the slot's generation so the next upload carries
    // a strictly higher generation (the host rejects gen <= its own). Echo
    // broadcasts of this generation bump it further — either way it only
    // ever climbs.
    player->profileGeneration = upload.generation;
    debugFilePrint("MP: local profile uploaded netId=%u gen=%u sections=%08X",
        localNetId, upload.generation, changedSections);
}

// ---------------------------------------------------------------------------
// Off-screen player indicators
// ---------------------------------------------------------------------------
// Small edge windows that point at remote players whose avatar is outside
// the current viewport. Drawn once per frame from the main loop (after
// MpTick, before renderPresent); each window is recreated only when its
// clamped position changes, hidden when the player is on screen.
#define MP_INDICATOR_WIDTH 104
#define MP_INDICATOR_HEIGHT 16
#define MP_INDICATOR_INSET 8
#define MP_INDICATOR_FONT 101

static int gMpIndicatorWindows[NET_MAX_PLAYERS];
static int gMpIndicatorWinX[NET_MAX_PLAYERS];
static int gMpIndicatorWinY[NET_MAX_PLAYERS];

// Hiding a window leaves its pixels on screen until the map under it is
// repainted, so every destroy path must refresh the window's rect — a
// destroyed-but-unrefreshed rectangle would otherwise stay visible (the
// destroy can also run after combat blocked the main loop, when the viewport
// may not be redrawing at all).
static void mpIndicatorHide(int index)
{
    if (gMpIndicatorWindows[index] == -1) {
        return;
    }
    Rect rect;
    rect.left = gMpIndicatorWinX[index];
    rect.top = gMpIndicatorWinY[index];
    rect.right = rect.left + MP_INDICATOR_WIDTH;
    rect.bottom = rect.top + MP_INDICATOR_HEIGHT;
    windowDestroy(gMpIndicatorWindows[index]);
    gMpIndicatorWindows[index] = -1;
    tileWindowRefreshRect(&rect, gElevation);
}

// Co-op: is this player avatar currently sneaking? The host's dude uses the
// vanilla dude-state; every other avatar (host-side or client mirror) carries
// the sneak bit in its proto flags - synced through the player-state channel.
static bool mpAvatarIsSneaking(Object* obj)
{
    if (obj == gDude) {
        return dudeHasState(DUDE_STATE_SNEAKING);
    }
    Proto* proto;
    if (protoGetProto(obj->pid, &proto) == -1) {
        return false;
    }
    return (proto->critter.data.flags & (1 << DUDE_STATE_SNEAKING)) != 0;
}

// Co-op: the roof/wall transparency circle (gEgg) must be punched at every
// player's position, not just the local dude. Collects the anchor objects the
// renderers use: the local dude first, then every connected remote avatar with
// a valid map position. Single-player returns just the local dude, matching
// the vanilla gEgg behavior exactly.
int MpGetCircleAnchors(Object** outAnchors, int maxAnchors)
{
    int count = 0;
    if (outAnchors == nullptr || maxAnchors <= 0) {
        return 0;
    }
    if (gDude != nullptr) {
        outAnchors[count++] = gDude;
    }
    if (!gMpActive || gMpSession.players == nullptr) {
        return count;
    }
    for (int index = 0; index < NET_MAX_PLAYERS && count < maxAnchors; index++) {
        MultiplayerPlayer* player = &gMpSession.players[index];
        if (!player->isConnected || player->obj == nullptr || player->obj == gDude) {
            continue;
        }
        if (!hexGridTileIsValid(player->obj->tile) || !elevationIsValid(player->obj->elevation)) {
            continue;
        }
        outAnchors[count++] = player->obj;
    }
    return count;
}

bool MpGetPlayerCircleRect(const Object* obj, Rect* rect)
{
    if (!gMpActive || obj == nullptr || rect == nullptr || gEgg == nullptr
        || !hexGridTileIsValid(obj->tile) || !elevationIsValid(obj->elevation)) {
        return false;
    }

    Object* anchors[NET_MAX_PLAYERS + 1];
    int anchorCount = MpGetCircleAnchors(anchors, NET_MAX_PLAYERS + 1);
    bool isAnchor = false;
    for (int index = 0; index < anchorCount; index++) {
        if (anchors[index] == obj) {
            isAnchor = true;
            break;
        }
    }
    if (!isAnchor) {
        return false;
    }

    CacheEntry* eggHandle;
    Art* egg = artLock(gEgg->fid, &eggHandle);
    if (egg == nullptr) {
        return false;
    }

    int eggWidth;
    int eggHeight;
    artGetSize(egg, 0, ROTATION_NE, &eggWidth, &eggHeight);

    int screenX;
    int screenY;
    bool valid = tileToScreenXY(obj->tile, &screenX, &screenY) == 0;
    if (valid) {
        screenX += 16 + egg->xOffsets[0] + obj->x;
        screenY += 8 + egg->yOffsets[0] + obj->y;
        rect->left = screenX - eggWidth / 2;
        rect->top = screenY - eggHeight + 1;
        rect->right = rect->left + eggWidth - 1;
        rect->bottom = screenY;
    }

    artUnlock(eggHandle);
    return valid;
}

void MpDrawPlayerIndicators()
{
    if (!gMpActive || gMpSession.players == nullptr) {
        return;
    }
    if (gMpIsClient && gMpSession.state != MP_STATE_CLIENT_PLAYING) {
        return;
    }
    static bool gMpIndicatorInited = false;
    if (!gMpIndicatorInited) {
        gMpIndicatorInited = true;
        for (int initIndex = 0; initIndex < NET_MAX_PLAYERS; initIndex++) {
            gMpIndicatorWindows[initIndex] = -1;
        }
    }
    // The main loop only calls this while the game is running, so the
    // interface bar is always present; its top edge is the viewport bottom.
    const int screenW = screenGetWidth();
    const int screenH = screenGetHeight();
    const int viewportBottom = screenH - INTERFACE_BAR_HEIGHT;

    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        MultiplayerPlayer* player = &gMpSession.players[index];
        const bool wantWindow = player != nullptr && player->isConnected
            && !player->isLocal && player->netId != 0;
        Object* obj = wantWindow
            ? (player->obj != nullptr ? player->obj : MpFindObjByNetId(player->objNetId))
            : nullptr;

        // Sneaking players are hidden from the indicators - the sneak state
        // is the player's own concealment choice.
        if (!wantWindow || obj == nullptr || !hexGridTileIsValid(obj->tile)
            || !elevationIsValid(obj->elevation) || mpAvatarIsSneaking(obj)) {
            mpIndicatorHide(index);
            continue;
        }

        // Screen position of the avatar (tile top-left + body offset).
        int bodyX;
        int bodyY;
        tileToScreenXY(obj->tile, &bodyX, &bodyY);
        bodyX += 32;
        bodyY += 28;

        const bool onScreen = bodyX >= 0 && bodyX < screenW
            && bodyY >= 0 && bodyY < viewportBottom;
        if (onScreen) {
            mpIndicatorHide(index);
            continue;
        }

        // Clamp to the nearest edge and pick the direction glyph.
        const char* arrow = ">>";
        int winX = 0;
        int winY = 0;
        if (bodyX < 0) {
            arrow = "<<";
            winX = MP_INDICATOR_INSET;
            winY = std::clamp(bodyY, MP_INDICATOR_INSET,
                viewportBottom - MP_INDICATOR_INSET - MP_INDICATOR_HEIGHT);
        } else if (bodyX >= screenW) {
            arrow = ">>";
            winX = screenW - MP_INDICATOR_INSET - MP_INDICATOR_WIDTH;
            winY = std::clamp(bodyY, MP_INDICATOR_INSET,
                viewportBottom - MP_INDICATOR_INSET - MP_INDICATOR_HEIGHT);
        } else if (bodyY < 0) {
            arrow = "^";
            winX = std::clamp(bodyX - MP_INDICATOR_WIDTH / 2, MP_INDICATOR_INSET,
                screenW - MP_INDICATOR_INSET - MP_INDICATOR_WIDTH);
            winY = MP_INDICATOR_INSET;
        } else {
            arrow = "v";
            winX = std::clamp(bodyX - MP_INDICATOR_WIDTH / 2, MP_INDICATOR_INSET,
                screenW - MP_INDICATOR_INSET - MP_INDICATOR_WIDTH);
            winY = viewportBottom - MP_INDICATOR_INSET - MP_INDICATOR_HEIGHT;
        }

        // Recreate the window only when its position moved.
        if (gMpIndicatorWindows[index] != -1
            && gMpIndicatorWinX[index] == winX && gMpIndicatorWinY[index] == winY) {
            continue;
        }
        mpIndicatorHide(index);

        int win = windowCreate(winX, winY, MP_INDICATOR_WIDTH, MP_INDICATOR_HEIGHT,
            COLOR_BLACK, WINDOW_MOVE_ON_TOP);
        if (win == -1) {
            continue;
        }
        windowDrawBorder(win);
        char text[MP_INDICATOR_WIDTH / 2 + 8];
        const char* name = player->name;
        if (name == nullptr || name[0] == '\0') {
            name = "?";
        }
        snprintf(text, sizeof(text), "%s %s", arrow, name);
        // The ambient font state at this point in the main loop is
        // unpredictable (scripts/interface leave any font selected); without
        // an explicit selection windowDrawText can silently skip the draw
        // (line-height/width checks against the window size).
        fontSetCurrent(MP_INDICATOR_FONT);
        windowDrawText(win, text, 0, 6, 3, COLOR_LIGHT_YELLOW);
        windowRefresh(win);

        gMpIndicatorWindows[index] = win;
        gMpIndicatorWinX[index] = winX;
        gMpIndicatorWinY[index] = winY;
    }
}

// Diagnostic (throttled): dump every connected avatar's visible fid once per
// second so skin-pick propagation can be traced from the logs — which fid the
// object renders, which model/animation bits it carries, what the proto says,
// and whether an animation is actively driving the sprite.
// (Commented: skin-pick propagation root-caused — see git history.)
static void mpDbgFidWatch()
{
    if (!gMpActive) {
        return;
    }
    (void)0;
#if 0
    static uint32_t lastTick = 0;
    if (lastTick != 0 && getTicksSince(lastTick) < 1000) {
        return;
    }
    lastTick = getTicks();
    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        MultiplayerPlayer* p = &gMpSession.players[index];
        if (!p->isConnected || p->obj == nullptr) {
            continue;
        }
        int protoFid = -1;
        Proto* proto = nullptr;
        if (protoGetProto(p->obj->pid, &proto) == 0) {
            protoFid = proto->critter.fid;
        }
        // Art resolution probe (throttled with the watch): does the avatar's
        // LIVE fid resolve to a drawable FRM on this machine? A fail here
        // renders the avatar invisible until the fid changes again.
        const char* artResult = "ok";
        CacheEntry* artHandle = nullptr;
        Art* art = artLock(p->obj->fid, &artHandle);
        if (art != nullptr) {
            artUnlock(artHandle);
        } else {
            artResult = "FAIL";
        }
        int model = p->obj->fid & 0xFFF;
        const char* modelName = artGetCritterModelName(model);
        char nameBuf[16];
        if (modelName == nullptr) {
            snprintf(nameBuf, sizeof(nameBuf), "?");
            modelName = nameBuf;
        }
        debugFilePrint("MPDBG fidwatch netId=%u objFid=0x%X model=%d anim=%d busy=%d protoFid=0x%X protoModel=%d frame=%d tile=%d elev=%d hidden=%d art=%s name='%.12s'",
            p->netId, p->obj->fid, model,
            animationTypeFromFid(p->obj->fid),
            animationIsBusy(p->obj) != 0 ? 1 : 0,
            protoFid, protoFid >= 0 ? protoFid & 0xFFF : -1,
            p->obj->frame, p->obj->tile, p->obj->elevation,
            (p->obj->flags & OBJECT_HIDDEN) != 0 ? 1 : 0,
            artResult, modelName);
        // Tile probe: what critters actually sit at the avatar's tile on
        // THIS machine? A stale duplicate (same position, different fid)
        // would render as the wrong look even though the avatar fid is right.
        if (hexGridTileIsValid(p->obj->tile) && elevationIsValid(p->obj->elevation)) {
            Object* other = objectFindFirstAtLocation(p->obj->elevation, p->obj->tile);
            while (other != nullptr) {
                if (objectTypeFromFid(other->fid) == OBJ_TYPE_CRITTER) {
                    debugFilePrint("MPDBG tileprobe netId=%u obj=%p pid=0x%X fid=0x%X model=%d netObj=%u isAvatar=%d",
                        p->netId, (void*)other, other->pid, other->fid, other->fid & 0xFFF,
                        MpGetObjNetId(other), other == p->obj ? 1 : 0);
                }
                other = objectFindNextAtLocation();
            }
        }
    }
#endif
}

void MpTick()
{
    if (!gMpActive) {
        MpDebugCheatsTick();
        return;
    }
    bool logFirstHostTick = gMpIsHost && gMpHostFirstTickPending;
    if (logFirstHostTick) {
        debugFilePrint("MP: first host tick begin");
    }

    // Drain client-deferred packets (queued while a modal blocked the main
    // loop). No modal is open now, so the applies are safe. New deferrals
    // from NetHostService below go to the tail and drain next tick. While the
    // synchronized dialogue modal is open this drain is skipped entirely —
    // the host's DIALOG_END (inline) closes the modal first, and the queued
    // session-changing packets apply on the first main-loop tick after it.
    size_t deferredCount = gMpModalActive ? 0 : gMpDeferredPackets.size();
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

    // Co-op cheats: apply/refill before any broadcast in this tick, so the
    // state packets that go out below already carry the cheated values.
    // MpCombatPump applies the same tick inside the blocking combat loops
    // and modals, where this function never runs.
    MpDebugCheatsTick();

    // Co-op dialogue: a director host (not a participant) never enters the
    // blocking dialogue modal — its session is driven from here each tick.
    // Must run before MpCombatTick so a pending combat start aborts the
    // parked dialogue first.
    if (gMpIsHost) {
        MpDialogHostDirectorTick();
    }

    // Co-op combat: deferred starts/turns, end-request drain, card refresh.
    MpCombatTick();

    // Co-op: the client's deferred map-enter cutscene runs here, on a
    // top-level tick — never inside the receive path. By now the deferred
    // packet drain above and the receive pump have completed, so the script
    // can open dialogs, schedule timed events, and block without starving
    // the network handler. The escape hatch is cleared immediately after.
    if (gMpIsClient && gMpClientDeferredMapEnterRun) {
        gMpClientDeferredMapEnterRun = false;
        debugFilePrint("MAP: client deferred map enter run sid=%d dudeTile=%d dudeElev=%d enteringTile=%d enteringElev=%d gameTime=%d",
            gMapSid,
            gDude != nullptr ? gDude->tile : -999,
            gDude != nullptr ? gDude->elevation : -999,
            gMpClientMapEnteringTile,
            gMpClientMapEnteringElevation,
            gameTimeGetTime());
        _scr_spatials_disable();
        // Co-op: this is the only place a client may execute a script - the
        // map-enter cutscene. The flag is cleared immediately after.
        gMpAllowClientScriptExec = true;
        scriptExecProc(gMapSid, SCRIPT_PROC_MAP_ENTER);
        gMpAllowClientScriptExec = false;
        _scr_spatials_enable();
        debugFilePrint("MAP: client deferred map script enter done sid=%d", gMapSid);
        // debugFilePrint("MAPDBG ambient after client deferred map script enter=%d",
        //     lightGetAmbientIntensity());
        if (wmSetupRandomEncounter() == -1) {
            debugPrint("\nError: couldn't set up random encounter after deferred client map enter!");
        }
    }

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
        MpPerfBegin(MP_PERF_MPTICK_ASSIGN);
        MpAssignNetIdsToAllObjects();
        MpPerfEnd(MP_PERF_MPTICK_ASSIGN);
        // MpCombatPump (inputGetInput) may have already swept the state this
        // same frame - a second sweep is pure duplicate work (~30ms each at
        // map size, the host's dominant cost). MpCombatPump still covers the
        // blocking loops (combat turns, modals) where MpTick never runs.
        uint32_t mpTickNowTick = getTicks();
        if (!mpHostBroadcastDoneThisTick(mpTickNowTick)) {
            MpPerfBegin(MP_PERF_MPTICK_OBJECTS);
            MpBroadcastObjectStates();
            MpPerfEnd(MP_PERF_MPTICK_OBJECTS);
            if (logFirstHostTick) {
                debugFilePrint("MP: first host tick after object broadcast");
            }
            MpPerfBegin(MP_PERF_MPTICK_PROFILES);
            mpHostSyncProfiles();
            MpPerfEnd(MP_PERF_MPTICK_PROFILES);
            if (!gMpHostTileBaseline.empty()) {
                bool tilesChanged = false;
                MpPerfBegin(MP_PERF_MPTICK_TILES);
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
                MpPerfEnd(MP_PERF_MPTICK_TILES);
                if (tilesChanged) {
                    MpBroadcastMapFullSync(nullptr);
                }
            }
            MpPerfBegin(MP_PERF_MPTICK_PLAYERS);
            MpBroadcastPlayerStates();
            MpPerfEnd(MP_PERF_MPTICK_PLAYERS);
            if (logFirstHostTick) {
                debugFilePrint("MP: first host tick after player broadcast");
                gMpHostFirstTickPending = false;
            }
        } else {
            if (logFirstHostTick) {
                debugFilePrint("MP: first host tick (broadcast already done in input pump)");
                gMpHostFirstTickPending = false;
            }
        }
        // Authoritative game clock: clients advance a local mirror between
        // syncs, but the host's value is truth. Throttled to ~1 Hz — the
        // client's own tick keeps it roughly in lockstep in between.
        static uint32_t gMpLastGameTimeSyncTick = 0;
        uint32_t nowTick = getTicks();
        if (nowTick - gMpLastGameTimeSyncTick >= 1000) {
            gMpLastGameTimeSyncTick = nowTick;
            NetGameTimePayload timePayload;
            timePayload.time = (int32_t)gameTimeGetTime();
            NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE,
                NET_PKT_GAME_TIME, &timePayload, sizeof(timePayload));
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
    // Open the synchronized dialogue modal when the host sent us a session and
    // we are a participant. Blocks while the modal is up (it pumps MpTick).
    MpDialogClientMaybeShowUI();

    // Remote critters can arrive on tiles whose dirty rects nobody marked;
    // nothing redraws them until the mouse happens to refresh the view. Do a
    // cheap throttled full refresh so players always appear in a timely
    // fashion even when nothing else changes.
    if (gMpLastTileRefreshTick == 0
        || getTicksSince(gMpLastTileRefreshTick) >= MP_TILE_REFRESH_INTERVAL_MS) {
        gMpLastTileRefreshTick = getTicks();
        tileWindowRefreshFull();
    }
    mpDbgFidWatch();
}

// ---------------------------------------------------------------------------
// Network event dispatch (host & client)
// ---------------------------------------------------------------------------
// Client quest-state apply (NET_PKT_GVAR_SNAPSHOT / NET_PKT_GVAR_CHANGE).
// The host's gvar table is the co-op quest state: the snapshot seeds the
// mirror at join, change packets keep it live. The client's own saves then
// persist the host's quests into the client's slot.
// ---------------------------------------------------------------------------

static void mpGvarOnChange(const NetGvarChangePayload* payload)
{
    if (!gMpIsClient || !gMpActive || payload == nullptr) {
        return;
    }
    if (payload->index < 0 || payload->index >= gGameGlobalVarsLength) {
        debugFilePrint("MP: gvar change out of range idx=%d len=%d", payload->index, gGameGlobalVarsLength);
        return;
    }
    gGameGlobalVars[payload->index] = payload->value;
    debugFilePrint("MP: gvar change idx=%d val=%d", payload->index, payload->value);
}

static void mpGvarOnSnapshot(const NetGvarSnapshotPayload* payload, size_t payloadLen)
{
    if (!gMpIsClient || !gMpActive || payload == nullptr) {
        return;
    }
    const size_t maxValues = (payloadLen - offsetof(NetGvarSnapshotPayload, values)) / sizeof(int32_t);
    const size_t hostCount = payload->count;
    const int count = (hostCount < maxValues) ? (int)hostCount : (int)maxValues;
    const int applyCount = (count < gGameGlobalVarsLength) ? count : gGameGlobalVarsLength;
    if (applyCount > 0) {
        memcpy(gGameGlobalVars, payload->values, (size_t)applyCount * sizeof(int32_t));
    }
    debugFilePrint("MP: gvar snapshot received count=%d applied=%d localLen=%d",
        count, applyCount, gGameGlobalVarsLength);
}

// Forward declaration: applied in the map-transition section below, called
// from the packet handler above.
static void mpApplySessionElevationChange(const MapTransition* transition);

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
            // (Commented: per-packet spam — see git history.)
            // if (packetType <= 32) {
            //     debugFilePrint("MP: raw receive type=%u len=%zu", packetType, packetPayloadLength);
            // }
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

                // Scripts run synchronously inside this handler; the flag
                // tells display_monitor.cc / scripts.cc that any monitor line
                // or elevator request produced now belongs to this remote
                // player's action. Cleared automatically on every exit.
                struct MpRemoteActionScope {
                    explicit MpRemoteActionScope(uint32_t netId)
                    {
                        gMpRemoteActionNetId = netId;
                    }
                    ~MpRemoteActionScope()
                    {
                        gMpRemoteActionNetId = 0;
                    }
                } remoteActionScope(p->netId);

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

                if (action->action == NET_PLAYER_ACTION_USE_ITEM) {
                    // Client armed an explosive from its own inventory. The arm
                    // mutated the pid locally (DYNAMITE_I -> DYNAMITE_II etc.),
                    // so the avatar's copy is still the INACTIVE pid the client
                    // sent. Find it, arm it with the same fuse the client
                    // selected (action->tile = fuse seconds), and queue the
                    // explosion event — the later drop relay (carrying the
                    // ACTIVE pid) then matches and the bomb stays alive.
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
                        if (item != nullptr && explosiveIsExplosive(item->pid)
                            && (item->flags & OBJECT_QUEUED) == 0) {
                            int seconds = action->tile;
                            if (seconds < 0) {
                                seconds = 0;
                            }
                            explosiveActivate(&(item->pid));
                            int delay = 10 * seconds;
                            int roll;
                            if (perkHasRank(p->obj, PERK_DEMOLITION_EXPERT)) {
                                roll = ROLL_SUCCESS;
                            } else {
                                roll = skillRoll(p->obj, SKILL_TRAPS, 0, nullptr);
                            }
                            EventType eventType;
                            switch (roll) {
                            case ROLL_CRITICAL_FAILURE:
                                delay = 0;
                                eventType = EVENT_TYPE_EXPLOSION_FAILURE;
                                break;
                            case ROLL_FAILURE:
                                eventType = EVENT_TYPE_EXPLOSION_FAILURE;
                                delay /= 2;
                                break;
                            default:
                                eventType = EVENT_TYPE_EXPLOSION;
                                break;
                            }
                            if (scriptHooks_ExplosiveTimer(item, 10 * seconds, eventType) == -1) {
                                queueAddEvent(delay, item, nullptr, eventType);
                            }
                            debugFilePrint("MP: use item arm netId=%u pid=0x%X seconds=%d delay=%d type=%d",
                                p->netId, item->pid, seconds, delay, eventType);
                        } else {
                            debugFilePrint("MP: use item no item netId=%u pid=0x%X",
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
                    // Co-op: a talk targeting an NPC with an active session
                    // joins it; otherwise record the pending initiator so the
                    // dialogue session starts with the right player.
                    if (!MpDialogHostTryJoin(target, p->netId)) {
                        MpDialogSetPendingInitiator(p->netId);
                        actionTalk(p->obj, target);
                    }
                    break;
                case NET_PLAYER_ACTION_TOUCH:
                    _action_use_an_object(p->obj, target);
                    break;
                case NET_PLAYER_ACTION_PICK_UP:
                    actionPickUp(p->obj, target);
                    break;
                case NET_PLAYER_ACTION_LOOT:
                    // Co-op: the vanilla loot path is a silent no-op for a
                    // remote looter (inventory.cc guards looter != gDude);
                    // run the synchronized loot session instead.
                    MpLootHostStart(p->netId, target, false);
                    break;
                case NET_PLAYER_ACTION_USE_SKILL:
                    if (skillIsValid(action->skill)) {
                        if ((Skill)action->skill == SKILL_STEAL) {
                            // Co-op: steal opens the synchronized steal
                            // window (host-side rolls, relayed state).
                            MpLootHostStart(p->netId, target, true);
                        } else {
                            actionUseSkill(p->obj, target, (Skill)action->skill);
                        }
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
                case NET_PLAYER_ACTION_USE_ITEM_ON: {
                    // Client used an inventory item on a map object (crosshair
                    // click or picker). Find the item by pid (rides the tile
                    // field), gate/deduct combat AP like vanilla, and run the
                    // use host-side — the target's USE_OBJ_ON script executes
                    // here and the result streams back through the object
                    // sync.
                    Object* item = nullptr;
                    if (p->obj != nullptr) {
                        for (int itemIndex = 0;
                            itemIndex < p->obj->data.inventory.length; itemIndex++) {
                            Object* candidate = p->obj->data.inventory.items[itemIndex].item;
                            if (candidate != nullptr && candidate->pid == action->tile) {
                                item = candidate;
                                break;
                            }
                        }
                    }
                    if (item == nullptr) {
                        debugFilePrint("MP: use-item-on no item netId=%u pid=0x%X",
                            p->netId, action->tile);
                        break;
                    }
                    if (isInCombat()) {
                        // Entry mode 1 (inventory picker) costs a flat 2 AP in
                        // vanilla; the crosshair click costs the item's
                        // weapon-mode AP cost.
                        int cost = action->skill == 1
                            ? 2
                            : itemGetActionPointCost(p->obj,
                                HIT_MODE_RIGHT_WEAPON_PRIMARY, false);
                        if (p->obj->data.critter.combat.ap < cost) {
                            debugFilePrint("MP: use-item-on rejected netId=%u pid=0x%X ap=%d cost=%d",
                                p->netId, item->pid, p->obj->data.critter.combat.ap, cost);
                            break;
                        }
                        if (_action_use_an_item_on_object(p->obj, target, item) != -1) {
                            p->obj->data.critter.combat.ap -= cost;
                            if (p->obj->data.critter.combat.ap < 0) {
                                p->obj->data.critter.combat.ap = 0;
                            }
                            // The avatar's AP rides the player-state channel.
                        }
                    } else {
                        _action_use_an_item_on_object(p->obj, target, item);
                    }
                    debugFilePrint("MP: use-item-on netId=%u targetNetId=%u pid=0x%X combat=%d",
                        p->netId, action->targetNetId, item->pid, isInCombat() ? 1 : 0);
                    break;
                }
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
                t.rotation = static_cast<Rotation>(in->targetRotation);
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
            case NET_PKT_DIALOG_CHOICE:
                MpDialogHostHandleChoice(payload, payloadLen, peer);
                break;
            case NET_PKT_DIALOG_LEAVE:
                MpDialogHostHandleLeave(payload, payloadLen, peer);
                break;
            case NET_PKT_BARTER_CMD:
                MpDialogHostHandleBarterCmd(payload, payloadLen, peer);
                break;
            case NET_PKT_LOOT_CMD:
                MpLootOnHostPacket(payload, payloadLen, peer);
                break;
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
            case NET_PKT_TO_HIT_QUERY: {
                if (payloadLen != sizeof(NetToHitQueryPayload)) {
                    return;
                }
                MultiplayerPlayer* p = mpPlayerFindByPeer(peer);
                if (p == nullptr || !p->isHandshaken || p->obj == nullptr) {
                    return;
                }
                const NetToHitQueryPayload* query = (const NetToHitQueryPayload*)payload;
                Object* toHitTarget = mpHostFindObjectByNetId(query->targetNetId);
                if (toHitTarget == nullptr) {
                    debugFilePrint("MP: to-hit query ignored (no target) netId=%u targetNetId=%u",
                        p->netId, query->targetNetId);
                    return;
                }

                NetToHitResultPayload result;
                memset(&result, 0, sizeof(result));
                result.targetNetId = query->targetNetId;
                result.hitMode = query->hitMode;
                int hostProbs[8];
                combatComputeCalledShotProbabilities(p->obj, toHitTarget,
                    (HitMode)query->hitMode, hostProbs);
                for (int index = 0; index < 8; index++) {
                    result.probs[index] = (int16_t)hostProbs[index];
                }
                NetSendPacket(peer, NET_CHANNEL_RELIABLE,
                    NET_PKT_TO_HIT_RESULT, &result, sizeof(result));
                debugFilePrint("MP: to-hit result sent netId=%u targetNetId=%u mode=%d probs=%d/%d/%d/%d/%d/%d/%d/%d",
                    p->netId, result.targetNetId, result.hitMode,
                    result.probs[0], result.probs[1], result.probs[2], result.probs[3],
                    result.probs[4], result.probs[5], result.probs[6], result.probs[7]);
                break;
            }
            case NET_PKT_PLAYER_CMD: {
                if (payloadLen != sizeof(NetPlayerCmdPayload)) {
                    return;
                }
                MultiplayerPlayer* p = mpPlayerFindByPeer(peer);
                if (p == nullptr || !p->isHandshaken || p->obj == nullptr) {
                    return;
                }
                const NetPlayerCmdPayload* cmd = (const NetPlayerCmdPayload*)payload;
                switch (cmd->opcode) {
                case NET_PLAYER_CMD_HEAL:
                    MpDebugApplyHeal(p->obj, cmd->arg1);
                    break;
                case NET_PLAYER_CMD_INVENTORY_AP: {
                    int cost = cmd->arg1;
                    if (cost < 0) {
                        cost = 0;
                    } else if (cost > 100) {
                        cost = 100;
                    }
                    if (cost > 0) {
                        Object* critter = p->obj;
                        int apBefore = critter->data.critter.combat.ap;
                        critter->data.critter.combat.ap = std::max(apBefore - cost, 0);
                        debugFilePrint("MP: inv ap cost resolved netId=%u cost=%d ap=%d->%d",
                            p->netId, cost, apBefore, critter->data.critter.combat.ap);
                    }
                    break;
                }
                case NET_PLAYER_CMD_AP_REFILL:
                    MpDebugApplyApRefill(p->obj);
                    break;
                case NET_PLAYER_CMD_CHEAT_FLAGS:
                    if (MpDebugClientCheatsEnabled()) {
                        p->debugCheatFlags = (uint32_t)cmd->arg1 & MP_DEBUG_CHEAT_ALL;
                        debugFilePrint("MP: cheat flags netId=%u flags=0x%X",
                            p->netId, p->debugCheatFlags);
                    } else {
                        p->debugCheatFlags = 0;
                        debugFilePrint("MP: cheat flags rejected (client cheats disabled) netId=%u flags=0x%X",
                            p->netId, (uint32_t)cmd->arg1);
                    }
                    break;
                default:
                    debugFilePrint("MP: player cmd unknown opcode=%u netId=%u",
                        cmd->opcode, p->netId);
                    break;
                }
                break;
            }
            case NET_PKT_MODEL_REQUEST: {
                if (payloadLen != sizeof(NetModelRequestPayload)) {
                    return;
                }
                const NetModelRequestPayload* req = (const NetModelRequestPayload*)payload;
                MultiplayerPlayer* requester = mpPlayerFindByPeer(peer);
                if (requester == nullptr || !requester->isHandshaken || req->netId == 0
                    || req->netId > NET_MAX_PLAYERS) {
                    return;
                }
                MultiplayerPlayer* target = &gMpSession.players[req->netId - 1];
                MpPlayerRuntime* targetRuntime = MpProfileGetRuntime(req->netId);
                if (target == nullptr || !target->isConnected
                    || targetRuntime == nullptr || targetRuntime->object == nullptr) {
                    return;
                }
                // The requester lacks the model payload — re-send just the
                // MODEL section WITH it (the receiver merges over its stored
                // copy, which already holds every other section).
                mpSendProfile(peer, req->netId, target->objNetId,
                    targetRuntime->profile, /*includeModel=*/true, requester->netId,
                    (1u << (PROFILE_SECTION_MODEL - 1)));
                debugFilePrint("MP: model resend netId=%u hash=%08X to netId=%u",
                    req->netId, req->modelHash, requester->netId);
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
            || !mpSendProfile(peer, 0, 0, gMpPendingClientProfile, /*includeModel=*/true)) {
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
                // debugFilePrint("MP: packet deferred type=%u queue=%zu",
                //     packetType, gMpDeferredPackets.size());
            }
            return;
        }
        default:
            break;
        }
        const void* payload = packetPayload;
        size_t payloadLen = packetPayloadLength;
        switch (packetType) {
        case NET_PKT_TO_HIT_RESULT: {
            if (payloadLen != sizeof(NetToHitResultPayload)) {
                return;
            }
            const NetToHitResultPayload* result = (const NetToHitResultPayload*)payload;
            gToHitResultTarget = result->targetNetId;
            gToHitResultMode = result->hitMode;
            memcpy(gToHitResultProbs, result->probs, sizeof(gToHitResultProbs));
            gToHitResultPending = true;
            debugFilePrint("MP: to-hit result received targetNetId=%u mode=%d",
                result->targetNetId, result->hitMode);
            break;
        }
        case NET_PKT_CHEAT_POLICY: {
            if (payloadLen != sizeof(NetCheatPolicyPayload)) {
                return;
            }
            const NetCheatPolicyPayload* policy = (const NetCheatPolicyPayload*)payload;
            MpDebugSetClientCheatsEnabled(policy->clientCheatsEnabled != 0);
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
        case NET_PKT_PLAYER_PROFILE_ACK: {
            if (payloadLen != sizeof(NetPlayerProfileAckPayload)) return;
            const NetPlayerProfileAckPayload* ack = (const NetPlayerProfileAckPayload*)payload;
            if (ack->accepted != 0 && ack->modelHash != 0) {
                // The host now holds this model — skip the payload on future
                // uploads with the same hash.
                gMpHostKnownModels.insert(ack->modelHash);
                debugFilePrint("MP: model ack received hash=%08X", ack->modelHash);
            }
            break;
        }
        case NET_PKT_MODEL_REQUEST: {
            // The host asked for our model payload (it never received it).
            if (payloadLen != sizeof(NetModelRequestPayload)) return;
            const NetModelRequestPayload* req = (const NetModelRequestPayload*)payload;
            if (req->netId == gMpSession.localNetId) {
                MpPlayerProfile resend;
                if (MpProfileCaptureLocal(&resend)) {
                    resend.generation = gMpSession.players[req->netId - 1].profileGeneration + 1;
                    if (resend.generation == 0) {
                        resend.generation = 1;
                    }
                    // Only the MODEL section is missing on the host — resend
                    // it with the payload.
                    if (mpSendProfile(gMpSession.hostPeer, req->netId,
                            gMpSession.players[req->netId - 1].objNetId, resend,
                            /*includeModel=*/true, /*receiverNetId=*/0,
                            (1u << (PROFILE_SECTION_MODEL - 1)))) {
                        gMpHostKnownModels.insert(req->modelHash);
                        debugFilePrint("MP: model resend sent netId=%u hash=%08X",
                            req->netId, req->modelHash);
                    }
                }
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
                gDude != nullptr ? objectTypeFromPid(gDude->pid) : -1,
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
        case NET_PKT_DIALOG_BEGIN:
        case NET_PKT_DIALOG_STATE:
        case NET_PKT_DIALOG_VOTE:
        case NET_PKT_DIALOG_TRANSCRIPT:
        case NET_PKT_DIALOG_JOIN:
        case NET_PKT_DIALOG_LEAVE:
        case NET_PKT_DIALOG_END:
        case NET_PKT_BARTER_STATE:
            // Synchronized dialogue: applied inline (they drive the client's
            // dialogue modal directly; the modal pumps MpTick itself).
            MpDialogOnClientPacket(packetType, payload, payloadLen);
            break;
        case NET_PKT_LOOT_STATE:
            // Synchronized loot: applied inline (the vanilla loot window
            // pumps MpTick itself via MpLootLoopTick).
            MpLootOnClientPacket(payload, payloadLen);
            break;
        case NET_PKT_PLAYER_EVENT: {
            if (payloadLen != sizeof(NetPlayerEventPayload)) {
                return;
            }
            const NetPlayerEventPayload* evt = (const NetPlayerEventPayload*)payload;
            switch (evt->opcode) {            case NET_PLAYER_EVENT_DOWNED: {
                NetPlayerStatusPayload status;
                status.netId = evt->netId;
                status.downed = (uint8_t)evt->arg1;
                status.hp = evt->arg2;
                MpApplyPlayerStatus(&status);
                break;
            }
            case NET_PLAYER_EVENT_GAME_OVER: {
                NetGameOverPayload gameOver;
                gameOver.reason = (uint8_t)evt->arg1;
                MpApplyGameOver(&gameOver);
                break;
            }
            case NET_PLAYER_EVENT_ATTACK_RESULT: {
                if (evt->netId == gMpSession.localNetId) {
                    // The host resolved our own attack — replay the feedback
                    // with the authoritative numbers.
                    MpReplayLocalAttackResult(evt->arg1, evt->arg2, evt->arg3);
                } else {
                    debugFilePrint("MP: attack result remote netId=%u dmg=%d flags=0x%X",
                        evt->netId, evt->arg1, evt->arg2);
                }
                break;
            }
            case NET_PLAYER_EVENT_ATTACK_REJECTED: {
                if (evt->netId == gMpSession.localNetId) {
                    // The host refused our attack (out of range, invalid
                    // state) — the swing already played locally with no
                    // effect. Snap the local dude to the host's
                    // authoritative position so the next click targets from
                    // the same space the host will resolve from.
                    debugFilePrint("MP: attack rejected targetNetId=%u tile=%d elev=%d",
                        (unsigned)evt->arg3, evt->arg1, evt->arg2);
                    if (gDude != nullptr && hexGridTileIsValid(evt->arg1)
                        && elevationIsValid(evt->arg2)
                        && (gDude->tile != evt->arg1
                            || gDude->elevation != evt->arg2)) {
                        reg_anim_clear(gDude);
                        objectSetLocation(gDude, evt->arg1, evt->arg2, nullptr);
                        debugFilePrint("MP: attack rejection snap dude tile=%d->%d elev=%d",
                            gDude->tile, evt->arg1, evt->arg2);
                    }
                    displayMonitorAddMessage("Attack failed - target unreachable.");
                } else {
                    debugFilePrint("MP: attack rejected remote netId=%u tile=%d",
                        evt->netId, evt->arg1);
                }
                break;
            }
            case NET_PLAYER_EVENT_SKILL_USE: {
                if (evt->netId != gMpSession.localNetId) {
                    break;
                }
                char text[256];
                if (skillGetMessageText(evt->arg1, text, sizeof(text), evt->arg2, evt->arg3)) {
                    if (evt->arg4 != 0) {
                        paletteFadeTo(gPaletteBlack);
                    }
                    displayMonitorAddMessage(text);
                    if (evt->arg4 != 0) {
                        paletteFadeTo(_cmap);
                    }
                }
                break;
            }
            default:
                debugFilePrint("MP: player event unknown opcode=%u", evt->opcode);
                break;
            }
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
            // Any open loot window is stale across a map change: the host's
            // sessions are closed on the transition, the mirror dies with
            // the old objects.
            MpLootOnClientReset();
            MpApplyMapChanged(&m->map);
            break;
        }
        case NET_PKT_MAP_ELEVATION: {
            if (payloadLen != sizeof(NetMapElevationPayload)) {
                return;
            }
            MpOnMapElevationChange((const NetMapElevationPayload*)payload);
            break;
        }
        case NET_PKT_ITEM_REMOVE: {
            if (payloadLen != sizeof(NetItemRemovePayload)) {
                return;
            }
            MpOnItemRemove((const NetItemRemovePayload*)payload);
            break;
        }
        case NET_PKT_GAME_TIME: {
            if (payloadLen != sizeof(NetGameTimePayload)) {
                return;
            }
            MpOnGameTime((const NetGameTimePayload*)payload);
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
        case NET_PKT_COMBAT_MOVE_RESULT: {
            if (payloadLen < sizeof(NetCombatMoveResultPayload)) {
                debugFilePrint("MP: combat move result bad length=%zu", payloadLen);
                return;
            }
            MpCombatOnMoveResult((const NetCombatMoveResultPayload*)payload);
            break;
        }
        case NET_PKT_FLOAT_MESSAGE: {
            if (payloadLen < sizeof(NetFloatMessagePayload)) {
                debugFilePrint("MP: float message bad length=%zu", payloadLen);
                return;
            }
            MpCombatOnFloatMessage((const NetFloatMessagePayload*)payload);
            break;
        }
        case NET_PKT_GVAR_SNAPSHOT: {
            if (payloadLen < sizeof(NetGvarSnapshotPayload)) {
                debugFilePrint("MP: gvar snapshot bad length=%zu", payloadLen);
                return;
            }
            mpGvarOnSnapshot((const NetGvarSnapshotPayload*)payload, payloadLen);
            break;
        }
        case NET_PKT_GVAR_CHANGE: {
            if (payloadLen < sizeof(NetGvarChangePayload)) {
                debugFilePrint("MP: gvar change bad length=%zu", payloadLen);
                return;
            }
            mpGvarOnChange((const NetGvarChangePayload*)payload);
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

// ---------------------------------------------------------------------------
// downed state (co-op: players are downed instead of killed)
// ---------------------------------------------------------------------------

// True when [critter] is a player-owned critter in the active co-op session.
// Host: the host's own dude or any connected remote player's critter.
// Client: only its own dude — the host is authoritative for everyone else.
bool MpIsCoopPlayerCritter(const Object* critter)
{
    if (!gMpActive || critter == nullptr) {
        return false;
    }
    if (gMpIsHost) {
        if (critter == gDude) {
            return true;
        }
        for (int i = 0; i < NET_MAX_PLAYERS; i++) {
            const MultiplayerPlayer* p = &gMpSession.players[i];
            if (p->isConnected && !p->isLocal && p->obj == critter) {
                return true;
            }
        }
        return false;
    }
    return critter == gDude;
}

bool MpIsPlayerObject(const Object* obj)
{
    if (!gMpActive || obj == nullptr) {
        return false;
    }
    if (obj == gDude) {
        return true;
    }
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        const MultiplayerPlayer* p = &gMpSession.players[i];
        if (p->isConnected && p->obj == obj) {
            return true;
        }
    }
    return false;
}

bool MpPlayerIsDownedByNetId(uint8_t netId)
{
    if (netId == 0 || netId > NET_MAX_PLAYERS) {
        return false;
    }
    return gMpSession.players[netId - 1].downed;
}

// The vanilla critterKill visual: pick the lying death frame, flatten the
// critter, drop collision and kill its light. Shared by the host's downing
// and the client's mirror so both render the body identically.
static void mpApplyDownedVisual(Object* critter, bool refreshRect)
{
    int elevation = critter->elevation;

    bool shouldChangeFid = false;
    int fid;
    if (critterIsProne(critter)) {
        AnimationType current = animationTypeFromFid(critter->fid);
        if (current == ANIM_FALL_BACK || current == ANIM_FALL_FRONT) {
            bool back = false;
            if (current == ANIM_FALL_BACK) {
                back = true;
            } else {
                fid = buildFid(OBJ_TYPE_CRITTER, critter->fid & 0xFFF, ANIM_FALL_FRONT_SF, weaponAnimationFromFid(critter->fid), critter->rotation + 1);
                if (!artExists(fid)) {
                    back = true;
                }
            }

            if (back) {
                fid = buildFid(OBJ_TYPE_CRITTER, critter->fid & 0xFFF, ANIM_FALL_BACK_SF, weaponAnimationFromFid(critter->fid), critter->rotation + 1);
            }

            shouldChangeFid = true;
        }
    } else {
        fid = buildFid(OBJ_TYPE_CRITTER, critter->fid & 0xFFF, LAST_SF_DEATH_ANIM, weaponAnimationFromFid(critter->fid), critter->rotation + 1);
        _obj_fix_violence_settings(&fid);
        if (!artExists(fid)) {
            fid = buildFid(OBJ_TYPE_CRITTER, critter->fid & 0xFFF, ANIM_FALL_BACK_BLOOD_SF, weaponAnimationFromFid(critter->fid), critter->rotation + 1);
            _obj_fix_violence_settings(&fid);
        }

        shouldChangeFid = true;
    }

    Rect updatedRect;
    Rect tempRect;
    objectGetRect(critter, &updatedRect);

    if (shouldChangeFid) {
        objectSetFrame(critter, 0, &updatedRect);
        objectSetFid(critter, fid, &tempRect);
        rectUnion(&updatedRect, &tempRect, &updatedRect);
    }

    if (!critterFlagCheck(critter->pid, CRITTER_FLAT)) {
        critter->flags |= OBJECT_NO_BLOCK;
        _obj_toggle_flat(critter, &tempRect);
        rectUnion(&updatedRect, &tempRect, &updatedRect);
    }

    _obj_turn_off_light(critter, &tempRect);
    rectUnion(&updatedRect, &tempRect, &updatedRect);

    if (refreshRect) {
        tileWindowRefreshRect(&updatedRect, elevation);
    }
}

// Reverse of mpApplyDownedVisual: restore the standing fid, re-enable
// collision and un-flatten. Shared by the host's revive and the client's
// mirror so both render the get-up identically.
static void mpRestoreStandingVisual(Object* critter, int32_t origFid, bool refreshRect)
{
    int elevation = critter->elevation;

    Rect updatedRect;
    Rect tempRect;
    objectGetRect(critter, &updatedRect);

    if (origFid != 0 && critter->fid != origFid) {
        objectSetFrame(critter, 0, &updatedRect);
        objectSetFid(critter, origFid, &tempRect);
        rectUnion(&updatedRect, &tempRect, &updatedRect);
    }

    if (!critterFlagCheck(critter->pid, CRITTER_FLAT)) {
        if ((critter->flags & OBJECT_FLAT) != 0) {
            _obj_toggle_flat(critter, &tempRect);
            rectUnion(&updatedRect, &tempRect, &updatedRect);
        }
        critter->flags &= ~OBJECT_NO_BLOCK;
    }

    _obj_turn_on_light(critter, &tempRect);
    rectUnion(&updatedRect, &tempRect, &updatedRect);

    if (refreshRect) {
        tileWindowRefreshRect(&updatedRect, elevation);
    }
}

// Host: route a skill-use feedback (monitor message + optional time-skip
// fade) to the performing player's client, so player-specific feedback
// reaches the right screen (never the host's for a remote performer).
void MpSendSkillUseFeedback(uint8_t netId, int messageId, int arg2, int arg3, int fade)
{
    if (!gMpIsHost || !gMpActive || netId == 0 || netId > NET_MAX_PLAYERS) {
        return;
    }
    MultiplayerPlayer* p = &gMpSession.players[netId - 1];
    if (!p->isConnected || p->peer == nullptr) {
        return;
    }
    NetPlayerEventPayload payload;
    payload.opcode = NET_PLAYER_EVENT_SKILL_USE;
    payload.netId = netId;
    payload.arg1 = messageId;
    payload.arg2 = arg2;
    payload.arg3 = arg3;
    payload.arg4 = (uint32_t)fade;
    NetSendPacket(p->peer, NET_CHANNEL_RELIABLE, NET_PKT_PLAYER_EVENT,
        &payload, sizeof(payload));
    debugFilePrint("MP: skill use feedback sent netId=%u msg=%d arg2=%d fade=%d",
        netId, messageId, arg2, fade);
}

// Host: broadcast the downed-state change for [netId] to every client.
static void mpBroadcastPlayerStatus(uint8_t netId, bool downed, int32_t hp)
{
    if (!gMpIsHost || gMpSession.enetHost == nullptr || netId == 0) {
        return;
    }
    NetPlayerEventPayload payload;
    payload.opcode = NET_PLAYER_EVENT_DOWNED;
    payload.netId = netId;
    payload.arg1 = downed ? 1 : 0;
    payload.arg2 = hp;
    payload.arg3 = 0;
    payload.arg4 = 0;
    NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE,
        NET_PKT_PLAYER_EVENT, &payload, sizeof(payload));
    debugFilePrint("MP: player status broadcast netId=%u downed=%d hp=%d",
        netId, downed ? 1 : 0, hp);
}

// Host: every connected player is downed — the game is over. Broadcast and
// exit through the normal quit path (mainLoop -> gameExit -> MpShutdown ->
// main menu). The clients follow either via GAME_OVER or the disconnect.
static void mpCheckAllPlayersDown()
{
    if (!gMpIsHost || !gMpActive) {
        return;
    }
    int connected = 0;
    int downedCount = 0;
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        const MultiplayerPlayer* p = &gMpSession.players[i];
        if (!p->isConnected) {
            continue;
        }
        connected++;
        if (p->downed) {
            downedCount++;
        }
    }
    if (connected > 0 && downedCount >= connected) {
        debugFilePrint("MP: GAME OVER — all %d players downed", connected);
        NetPlayerEventPayload payload;
        payload.opcode = NET_PLAYER_EVENT_GAME_OVER;
        payload.netId = 0;
        payload.arg1 = 1;
        payload.arg2 = 0;
        payload.arg3 = 0;
        payload.arg4 = 0;
        NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE,
            NET_PKT_PLAYER_EVENT, &payload, sizeof(payload));
        _game_user_wants_to_quit = GAME_QUIT_REQUEST_MAIN_MENU;
    }
}

// A player critter was killed: convert the death into the downed state
// instead. HP drops to 0 and DAM_DEAD is set, so every vanilla check treats
// the critter as dead (turns skipped, untargetable, lying body) — but the
// irreversible death side effects (party removal, script teardown, death
// ending) never run, and the player revives when combat ends. Called from
// critterKill on both sides; only the host decides the game-over check.
void MpPlayerDown(Object* critter)
{
    if (!gMpActive || critter == nullptr || objectTypeFromPid(critter->pid) != OBJ_TYPE_CRITTER) {
        return;
    }

    uint8_t netId = 0;
    if (gMpIsHost) {
        if (critter == gDude) {
            netId = gMpSession.players[0].netId;
        } else {
            for (int i = 0; i < NET_MAX_PLAYERS; i++) {
                MultiplayerPlayer* p = &gMpSession.players[i];
                if (p->isConnected && !p->isLocal && p->obj == critter) {
                    netId = p->netId;
                    break;
                }
            }
        }
    } else if (critter == gDude) {
        netId = gMpSession.localNetId;
    }
    if (netId == 0 || netId > NET_MAX_PLAYERS) {
        debugFilePrint("MP: downed request without player netId critter=%p",
            (void*)critter);
        return;
    }

    MultiplayerPlayer* player = &gMpSession.players[netId - 1];
    if (player->downed) {
        debugFilePrint("MP: downed ignored (already downed) netId=%u", netId);
        return;
    }

    player->downed = true;
    player->downedOrigFid = critter->fid;
    critter->data.critter.hp = 0;
    critter->data.critter.combat.results |= DAM_DEAD;
    mpApplyDownedVisual(critter, true);

    debugFilePrint("MP: player downed netId=%u critter=%p tile=%d fid=0x%X",
        netId, (void*)critter, critter->tile, critter->fid);

    if (gMpIsHost) {
        // The monitor message mirrors to every client while combat runs.
        displayMonitorAddMessage("A player has been downed!");
        mpBroadcastPlayerStatus(netId, true, 0);
        mpCheckAllPlayersDown();
    } else {
        win_timed_msg("You have been downed!", COLOR_RED);
    }
}

// Host: revive every downed player with 5% of their max HP. Runs when combat
// ends successfully (called from the host's combat end path).
void MpCombatEndReviveDowned()
{
    if (!gMpIsHost || !gMpActive) {
        return;
    }
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        MultiplayerPlayer* p = &gMpSession.players[i];
        if (!p->isConnected || !p->downed) {
            continue;
        }
        Object* critter = p->obj != nullptr ? p->obj : gDude;
        if (critter == nullptr) {
            continue;
        }

        p->downed = false;
        critter->data.critter.combat.results &= ~(DAM_DEAD | DAM_KNOCKED_OUT | DAM_KNOCKED_DOWN | DAM_LOSE_TURN);
        int maxHp = critterGetStat(critter, STAT_MAXIMUM_HIT_POINTS);
        int reviveHp = maxHp * 5 / 100;
        if (reviveHp < 1) {
            reviveHp = 1;
        }
        critter->data.critter.hp = reviveHp;
        // A leftover fall/knockdown animation (or the active-animation state)
        // keeps rendering the corpse after a bare fid restore. Clear pending
        // animations, restore the standing visual, then play the vanilla
        // get-up animation — the same sequence the engine uses out of combat.
        reg_anim_clear(critter);
        mpRestoreStandingVisual(critter, p->downedOrigFid, true);
        _dude_standup(critter);
        p->downedOrigFid = 0;
        if (critter == gDude) {
            // The vanilla _combat_over rendered the HUD from the downed
            // (0 HP) state before the revive ran — redraw it or the bar
            // keeps showing 0.
            interfaceRenderHitPoints(true);
        }

        debugFilePrint("MP: player revived netId=%u hp=%d/%d fid=0x%X",
            p->netId, reviveHp, maxHp, critter->fid);
        // The monitor message mirrors to every client while combat runs.
        displayMonitorAddMessage("A downed player gets back up!");
        mpBroadcastPlayerStatus(p->netId, false, reviveHp);
    }
}

// Host: debug heal from a player's debug menu (value <= 0 = full). HP is
// host-authoritative, so the host applies it to the avatar; the regular
// state/status broadcasts carry the result back to everyone. A downed
// player gets back up — same revive as combat end.
void MpDebugApplyHeal(Object* critter, int value)
{
    if (!gMpIsHost || !gMpActive || critter == nullptr) {
        return;
    }
    MultiplayerPlayer* p = nullptr;
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        MultiplayerPlayer* candidate = &gMpSession.players[i];
        if (candidate->isConnected && candidate->obj == critter) {
            p = candidate;
            break;
        }
    }

    int maxHp = critterGetStat(critter, STAT_MAXIMUM_HIT_POINTS);
    int hp = critter->data.critter.hp;
    int newHp = value <= 0 ? maxHp : hp + value;
    if (newHp > maxHp) {
        newHp = maxHp;
    }

    if (p != nullptr && p->downed) {
        p->downed = false;
        critter->data.critter.combat.results &= ~(DAM_DEAD | DAM_KNOCKED_OUT | DAM_KNOCKED_DOWN | DAM_LOSE_TURN);
        critter->data.critter.hp = newHp > 0 ? newHp : 1;
        // Same get-up sequence as MpCombatEndReviveDowned: clear leftover
        // fall animation state, restore the standing visual, play the
        // vanilla get-up animation.
        reg_anim_clear(critter);
        mpRestoreStandingVisual(critter, p->downedOrigFid, true);
        _dude_standup(critter);
        p->downedOrigFid = 0;
        debugFilePrint("MPDBG: heal revived netId=%u hp=%d", p->netId, critter->data.critter.hp);
        displayMonitorAddMessage("A downed player gets back up!");
        mpBroadcastPlayerStatus(p->netId, false, critter->data.critter.hp);
    } else {
        critter->data.critter.hp = newHp;
        debugFilePrint("MPDBG: heal applied obj=%p hp=%d->%d", critter, hp, newHp);
    }
}

// Host: apply a debug AP refill to a player's avatar. Combat AP is
// host-authoritative; the per-tick state channel carries the result back.
void MpDebugApplyApRefill(Object* critter)
{
    if (!gMpIsHost || !gMpActive || critter == nullptr) {
        return;
    }
    int maxAp = critterGetStat(critter, STAT_MAXIMUM_ACTION_POINTS);
    critter->data.critter.combat.ap = maxAp;
    debugFilePrint("MPDBG: ap refill applied obj=%p ap=%d", critter, maxAp);
}

// Client: a player's downed state changed (host authoritative).
void MpApplyPlayerStatus(const NetPlayerStatusPayload* s)
{
    if (!gMpIsClient || s == nullptr || s->netId == 0 || s->netId > NET_MAX_PLAYERS) {
        return;
    }
    MultiplayerPlayer* p = &gMpSession.players[s->netId - 1];

    if (s->netId != gMpSession.localNetId) {
        // Remote player downed/revived — their visual arrives through the
        // state broadcast; nothing to apply for a non-local avatar.
        debugFilePrint("MP: player status remote netId=%u downed=%d hp=%d",
            s->netId, s->downed, s->hp);
        return;
    }

    Object* dude = gDude;
    bool wasDowned = p->downed;
    p->downed = s->downed != 0;

    if (dude == nullptr) {
        debugFilePrint("MP: player status self netId=%u downed=%d (no dude yet)",
            s->netId, s->downed);
        return;
    }

    if (s->downed) {
        if (!wasDowned) {
            p->downedOrigFid = dude->fid;
            // Cancel any local prediction (walk SADs) — the body must not
            // keep moving after the player drops.
            reg_anim_clear(dude);
        }
        dude->data.critter.hp = 0;
        dude->data.critter.combat.results |= DAM_DEAD;
        // Guarantee the lying visual even if the unreliable state packet is
        // lost — the host heartbeat re-sends it within 2s either way.
        mpApplyDownedVisual(dude, true);
        debugFilePrint("MP: player status self downed hp=0 fid=0x%X", dude->fid);
        win_timed_msg("You have been downed!", COLOR_RED);
    } else {
        dude->data.critter.hp = s->hp;
        dude->data.critter.combat.results &= ~(DAM_DEAD | DAM_KNOCKED_OUT | DAM_KNOCKED_DOWN | DAM_LOSE_TURN);
        // Same get-up sequence as the host's revive: clear leftover fall
        // animation state (the client's own predicted fall may still be
        // queued and would re-corps the dude after the fid restore), restore
        // the standing visual, then play the get-up animation.
        reg_anim_clear(dude);
        mpRestoreStandingVisual(dude, p->downedOrigFid, true);
        _dude_standup(dude);
        p->downedOrigFid = 0;
        interfaceRenderHitPoints(true);
        debugFilePrint("MP: player status self revived hp=%d fid=0x%X", s->hp, dude->fid);
        win_timed_msg("You get back up!", COLOR_WHITE);
    }
}

// Client: the host ended the game (every player is downed). Return to the
// main menu through the normal quit path.
void MpApplyGameOver(const NetGameOverPayload* payload)
{
    if (!gMpIsClient || payload == nullptr) {
        return;
    }
    debugFilePrint("MP: game over received — returning to main menu");
    win_timed_msg("Everyone is downed — game over", COLOR_RED);
    _game_user_wants_to_quit = GAME_QUIT_REQUEST_MAIN_MENU;
}

// ---------------------------------------------------------------------------
// broadcast (host -> clients)
// ---------------------------------------------------------------------------

// Heartbeat: the per-player delta means an idle host broadcasts NOTHING.
void MpBroadcastPlayerStates()
{
    if (!gMpIsHost || gMpSession.enetHost == nullptr) {
        return;
    }
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
        s.flags = 0;
        if (o == gDude) {
            if (dudeHasState(DUDE_STATE_SNEAKING)) {
                s.flags |= 1;
            }
        } else {
            Proto* playerProto;
            if (protoGetProto(o->pid, &playerProto) == 0
                && (playerProto->critter.data.flags & (1 << DUDE_STATE_SNEAKING)) != 0) {
                s.flags |= 1;
            }
        }

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
            && p->lastCombatResults == s.combatResults
            && p->lastFlags == s.flags) {
            continue;
        }
        int channel = p->hasLastState ? NET_CHANNEL_UNRELIABLE : NET_CHANNEL_RELIABLE;
        NetBroadcastPacket(gMpSession.enetHost, channel,
            NET_PKT_PLAYER_STATE_UPDATE, &s, sizeof(s));
        MpPerfAddCounter(MP_PERF_CNT_PLAYER_PKTS, 1);
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
        p->lastFlags = s.flags;
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

// Co-op: immediately re-broadcast a single object's state to every client.
// Script-driven hostility flips (set_team) change the critter's team without
// touching its transform, so the periodic sweep would only catch them on its
// own cadence — and the client mirrors must re-tint the crosshair hover and
// the acting-critter outline at once.
void MpBroadcastObjectStateFor(Object* obj)
{
    if (!gMpIsHost || gMpSession.enetHost == nullptr || obj == nullptr) {
        return;
    }
    if (mpHostFindPlayerByObject(obj) != nullptr) {
        return; // players ride the player-state channel
    }
    NetMapFullSyncObjectPayload state;
    if (!mpBuildObjectState(obj, &state)) {
        return;
    }
    NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE,
        NET_PKT_OBJECT_STATE_UPDATE, &state, sizeof(state));
    debugFilePrint("MP: object state forced netId=%u pid=0x%X tile=%d team=%d",
        state.netId, state.pid, state.tile, state.combatTeam);
    // Keep the sweep's record in sync so it does not re-send the same state.
    int oldIndex = mpHostFindObjectRecord(state.netId);
    if (oldIndex >= 0) {
        gMpHostObjectRecords[oldIndex].state = state;
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
    // Index the previous sweep's records once; per-object linear scans of
    // ~4551 records were the host's main per-frame cost (the meter showed
    // remCmp in the hundreds of millions per window).
    std::unordered_map<uint32_t, int> recordIndex;
    recordIndex.reserve(gMpHostObjectRecords.size());
    for (int r = 0; r < (int)gMpHostObjectRecords.size(); r++) {
        recordIndex.emplace(gMpHostObjectRecords[r].netId, r);
    }
    currentRecords.reserve(gMpHostObjectRecords.size());
    Object* obj = objectFindFirst();
    while (obj != nullptr) {
        MpPerfAddCounter(MP_PERF_CNT_OBJ_SCANNED, 1);
        if (mpHostFindPlayerByObject(obj) != nullptr) {
            obj = objectFindNext();
            continue;
        }
        NetMapFullSyncObjectPayload state;
        if (mpBuildObjectState(obj, &state)) {
            auto foundRecord = recordIndex.find(state.netId);
            int oldIndex = foundRecord != recordIndex.end() ? foundRecord->second : -1;
            bool changed = oldIndex < 0
                || memcmp(&gMpHostObjectRecords[oldIndex].state, &state, sizeof(state)) != 0;
            MultiplayerPlayer* player = mpHostFindPlayerByObject(obj);
            if (changed && player == nullptr) {
                // MPDIAG (temporary): watch critter state broadcasts to verify
                // the dialogue speaker stays in the object stream.
                // (Commented: per-frame spam — see git history.)
                if (false && objectTypeFromFid(state.fid) == OBJ_TYPE_CRITTER) {
                    static struct { uint32_t netId; uint32_t ms; uint32_t key; } last[32] = {};
                    uint32_t nowMs = getTicks();
                    uint32_t key = state.fid ^ (state.tile << 7) ^ (state.flags << 15) ^ (state.hp << 3);
                    int found = -1;
                    int freeSlot = -1;
                    for (int d = 0; d < 32; d++) {
                        if (last[d].netId == state.netId) {
                            found = d;
                            break;
                        }
                        if (freeSlot < 0 && last[d].netId == 0) {
                            freeSlot = d;
                        }
                    }
                    if (found >= 0) {
                        if (nowMs - last[found].ms > 500 || key != last[found].key) {
                            last[found].ms = nowMs;
                            last[found].key = key;
                            debugFilePrint("MPDIAG host broadcast netId=%u pid=0x%X tile=%d fid=0x%X flags=0x%X hp=%d",
                                state.netId, state.pid, state.tile, state.fid, state.flags, state.hp);
                        }
                    } else if (freeSlot >= 0) {
                        last[freeSlot].netId = state.netId;
                        last[freeSlot].ms = nowMs;
                        last[freeSlot].key = key;
                        debugFilePrint("MPDIAG host broadcast netId=%u pid=0x%X tile=%d fid=0x%X flags=0x%X hp=%d (first)",
                            state.netId, state.pid, state.tile, state.fid, state.flags, state.hp);
                    }
                }
                NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_UNRELIABLE,
                    NET_PKT_OBJECT_STATE_UPDATE, &state, sizeof(state));
                MpPerfAddCounter(MP_PERF_CNT_OBJ_CHANGED, 1);
                MpPerfAddCounter(MP_PERF_CNT_OBJ_PKTS, 1);
            }

            MpHostObjectRecord record;
            record.netId = state.netId;
            record.state = state;
            currentRecords.push_back(record);
        }
        obj = objectFindNext();
    }

    MpPerfSetCounter(MP_PERF_CNT_OBJ_RECORDS, (uint32_t)currentRecords.size());

    // An object that disappeared from the host map must not remain visible on
    // clients. Player critters have their own PLAYER_LEFT packet and are
    // intentionally excluded from this generic removal path. Membership via a
    // set: the previous nested old x current scan was O(n^2) and dominated
    // the host frame (remCmp reached hundreds of millions per window).
    std::unordered_set<uint32_t> currentNetIds;
    currentNetIds.reserve(currentRecords.size());
    for (const MpHostObjectRecord& currentRecord : currentRecords) {
        currentNetIds.insert(currentRecord.netId);
    }
    for (const MpHostObjectRecord& oldRecord : gMpHostObjectRecords) {
        MpPerfAddCounter(MP_PERF_CNT_OBJ_REM_CMP, 1);
        if (currentNetIds.find(oldRecord.netId) != currentNetIds.end()) {
            continue;
        }
        bool isPlayer = false;
        for (int index = 0; index < NET_MAX_PLAYERS; index++) {
            MultiplayerPlayer* player = &gMpSession.players[index];
            if (player->isConnected && player->objNetId == oldRecord.netId) {
                isPlayer = true;
                break;
            }
        }
        if (!isPlayer && objectTypeFromFid(oldRecord.state.fid) != OBJ_TYPE_INTERFACE) {
            debugFilePrint("MP: object removed broadcast netId=%u pid=0x%X fid=0x%X tile=%d",
                oldRecord.netId, oldRecord.state.pid, oldRecord.state.fid, oldRecord.state.tile);
            NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE,
                NET_PKT_OBJECT_REMOVED, &oldRecord.netId, sizeof(oldRecord.netId));
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
    int skipOther = 0;
    Object* obj = objectFindFirst();
    while (obj != nullptr) {
        // Static map-file scenery/walls SHIP in the sync. The client keeps its
        // own map-file copies (mpClearClientMapObjectsForFullSync) and the
        // apply path map-matches by tile+pid (mpApplyObjectStateInternal with
        // allowMapMatch=true), so no duplicate objects are created; the states
        // register the local copies with their host netIds and carry the
        // host's CURRENT fid/flags (an already-open door). Without them the
        // client's statics never get netIds and every action on them (TOUCH,
        // INSPECT, PUSH, ROTATE) dies at MpGetObjNetId()==0.
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
    debugFilePrint("MP: full sync objects=%zu sent=%zu skipNoNetId=%d skipHidden=%d skipInventory=%d skipOther=%d",
        objects.size(), objects.size(), skipNoNetId, skipHidden, skipInventory, skipOther);

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

static void mpBuildMapChangedPayload(NetMapChangedPayload* p, int32_t mapId)
{
    memset(p, 0, sizeof(*p));
    mpBuildMapSyncPayload(&p->map);
    p->map.mapId = mapId;
    // Anchor client-side avatar rebuilds at the host's ACTUAL post-transition
    // position instead of the map file's default entering tile. MpFinishHostMapChange
    // respawns every player around gDude's tile; the client mirrors that logic in
    // MpApplyMapChanged (mpFindPlayerSpawnTile on the same anchor), so giving the
    // client the same anchor makes both sides pick identical spawn tiles and the
    // first player-state update has nothing to snap.
    if (gDude != nullptr) {
        if (hexGridTileIsValid(gDude->tile)) {
            p->map.enteringTile = gDude->tile;
        }
        if (elevationIsValid(gDude->elevation)) {
            p->map.enteringElevation = gDude->elevation;
        }
        if (gDude->rotation >= 0 && gDude->rotation < ROTATION_COUNT) {
            p->map.enteringRotation = gDude->rotation;
        }
    }
}

void MpBroadcastMapChanged(int32_t mapId)
{
    if (!gMpIsHost || gMpSession.enetHost == nullptr) {
        return;
    }
    NetMapChangedPayload p;
    mpBuildMapChangedPayload(&p, mapId);
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

    // Co-op: no dialogue or barter survives a map transition.
    MpDialogReset();

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

    // Co-op: loot/steal sessions target objects of the old map — close them
    // all (no XP; the targets died with the map).
    MpLootHostCloseAllSessions();

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
        int playerTile = mpFindPlayerSpawnTile(mpRandomSpawnAnchor(tile, elevation), elevation);
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
    // mpDebugDumpLightState("host-after-finish");
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
    Rect oldCircleRect;
    bool hasCircleRect = MpGetPlayerCircleRect(obj, &oldCircleRect);
    bool wasApplyingNetworkState = gMpSession.applyingNetworkState;
    gMpSession.applyingNetworkState = true;

    objectSetFid(obj, fid, nullptr);
    if (objectSetFrame(obj, frame, nullptr) != 0) {
        obj->frame = frame;
    }
    if (objectSetRotation(obj, static_cast<Rotation>(rotation), nullptr) != 0) {
        obj->rotation = static_cast<Rotation>(rotation);
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
        // debugFilePrint("MPDBG roof update: tile=%d elev=%d", tile, elevation);
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
    Rect oldRefreshRect = oldRect;
    Rect newRefreshRect = newRect;
    if (hasCircleRect) {
        rectUnion(&oldRefreshRect, &oldCircleRect, &oldRefreshRect);
        Rect newCircleRect;
        if (MpGetPlayerCircleRect(obj, &newCircleRect)) {
            rectUnion(&newRefreshRect, &newCircleRect, &newRefreshRect);
        }
    }
    tileWindowRefreshRect(&oldRefreshRect, oldElevation);
    tileWindowRefreshRect(&newRefreshRect, obj->elevation);
}

static void mpApplyCritterState(Object* obj, int hp, int ap, int radiation, int poison, int combatTeam, int combatManeuver, int combatResults)
{
    if (obj == nullptr || objectTypeFromFid(obj->fid) != OBJ_TYPE_CRITTER) {
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
                if (objectTypeFromFid(obj->fid) == OBJ_TYPE_CRITTER) {
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
    // The memset wiped every reverse-map entry, including profile-runtime
    // avatars. The fresh full-sync skips player objects (profile-owned), so
    // their registrations must be restored here or netId lookups (attack
    // targets, skill targets) never resolve for them again.
    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        MultiplayerPlayer* p = &gMpSession.players[index];
        if (p->isConnected && p->obj != nullptr && p->objNetId != 0) {
            MpRegisterObjNetId(p->obj, p->objNetId);
        }
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
    if (objectTypeFromFid(state->fid) == OBJ_TYPE_CRITTER) {
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

    // Local UI helpers (hex cursor etc.) are never sent by the host; a stray
    // INTERFACE-type state is corruption — refuse to materialize it.
    if (objectTypeFromFid(state->fid) == OBJ_TYPE_INTERFACE) {
        debugFilePrint("MPDBG: rejected interface-type object state netId=%u pid=0x%X",
            state->netId, state->pid);
        return nullptr;
    }

    // The local player object must stay the vault-dude critter. A non-critter
    // state bound to his netId is corruption (it would turn gDude into a wall
    // and zero his stats); refuse to apply it.
    if (player != nullptr && player->isLocal
        && objectTypeFromFid(state->fid) != OBJ_TYPE_CRITTER) {
        debugFilePrint("MPDBG reject non-critter state for local player: netId=%u pid=0x%X fid=0x%X",
            state->netId, state->pid, state->fid);
        return obj;
    }
    if (player != nullptr && player->isLocal) {
        debugFilePrint("MPDBG apply local state: netId=%u pid=0x%X fid=0x%X tile=%d elev=%d objpid=0x%X",
            state->netId, state->pid, state->fid, state->tile, state->elevation,
            obj != nullptr ? obj->pid : 0);
    }

    const char* mpDiagMode = "registered";
    if (obj == nullptr && player == nullptr && allowMapMatch && hexGridTileIsValid(state->tile)
        && elevationIsValid(state->elevation)) {
        Object* match = objectFindFirstAtLocation(state->elevation, state->tile);
        while (match != nullptr) {
            if (match->pid == state->pid) {
                obj = match;
                mpDiagMode = "matched";
                break;
            }
            match = objectFindNextAtLocation();
        }

        // Moved objects (critters that walked before dying, dropped items)
        // are not at their map-file positions, so the tile+pid match fails
        // and the client keeps its stale alive map-file copy - a phantom
        // that stands around and looks like a dead enemy "revived". The full
        // sync ships every object the host has, so fall back to any
        // unregistered map-file object with the same pid; each shipped state
        // then consumes one stale copy and no phantoms survive.
        if (obj == nullptr) {
            Object* probe = objectFindFirst();
            while (probe != nullptr) {
                if (probe != gDude && MpGetObjNetId(probe) == 0
                    && probe->pid == state->pid) {
                    obj = probe;
                    mpDiagMode = "matched-any";
                    break;
                }
                probe = objectFindNext();
            }
        }
    }

    // Fate trace: every wall/scenery state during the full sync. If an object
    // with this netId already exists, the state REUSES it (overwriting the
    // previous object) — duplicate netIds lose the earlier object.
    if (obj == nullptr) {
        obj = mpCreateClientObject(state);
        mpDiagMode = "created";
    }
    if (obj == nullptr) {
        return nullptr;
    }

    // MPDIAG (temporary): watch critter state applies to verify the dialogue
    // speaker keeps a visible mirror on the client.
    // (Commented: per-state spam — see git history.)
    if (false && player == nullptr && objectTypeFromFid(state->fid) == OBJ_TYPE_CRITTER) {
        static struct { uint32_t netId; uint32_t ms; uint32_t key; } last[32] = {};
        uint32_t nowMs = getTicks();
        uint32_t key = state->fid ^ (state->tile << 7) ^ (state->flags << 15) ^ (state->hp << 3);
        int found = -1;
        int freeSlot = -1;
        for (int d = 0; d < 32; d++) {
            if (last[d].netId == state->netId) {
                found = d;
                break;
            }
            if (freeSlot < 0 && last[d].netId == 0) {
                freeSlot = d;
            }
        }
        if (found >= 0) {
            if (nowMs - last[found].ms > 500 || key != last[found].key) {
                last[found].ms = nowMs;
                last[found].key = key;
                debugFilePrint("MPDIAG client apply netId=%u pid=0x%X tile=%d fid=0x%X flags=0x%X hp=%d team=%d mode=%s",
                    state->netId, state->pid, state->tile, state->fid, state->flags, state->hp, state->combatTeam, mpDiagMode);
            }
        } else if (freeSlot >= 0) {
            last[freeSlot].netId = state->netId;
            last[freeSlot].ms = nowMs;
            last[freeSlot].key = key;
            debugFilePrint("MPDIAG client apply netId=%u pid=0x%X tile=%d fid=0x%X flags=0x%X hp=%d mode=%s (first)",
                state->netId, state->pid, state->tile, state->fid, state->flags, state->hp, mpDiagMode);
        }
    }

    // Keep the local player's critter pid even if a state arrives with a
    // non-critter pid (corruption guard — a wall pid here would make
    // critterGetStat return 0 and render the dude as wall art).
    if (player == nullptr || !player->isLocal || objectTypeFromPid(state->pid) == OBJ_TYPE_CRITTER) {
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
    // While a combat move intent is in flight, the host's avatar may not have
    // moved yet (its combat cmd queue lags the click by up to ~2s), so the
    // periodic state heartbeat still carries the PRE-move tile. Snapping then
    // would undo the optimistic walk and leave the local dude one or two hexes
    // off the real position — clicks that should be in range read out of
    // range. Yield until the avatar actually reaches the intent tile (or the
    // authoritative move-result packet resolves otherwise).
    int moveIntentTile = -1;
    const bool moveIntentInFlight = isLocalPlayer && MpCombatHasPendingMoveIntent(&moveIntentTile);
    const bool moveIntentBlocksSnap = moveIntentInFlight && s->tile != moveIntentTile;
    // The very first state must snap the local player into place (the map
    // reload may have left him at the map's entering tile, off-screen from
    // the host's camera). Later updates yield to local prediction. A frozen
    // initiator (map-change vote in progress) never yields — the host has
    // stopped the critter on the exit grid and drives its position
    // authoritatively; yielding would let the local walk continue past the
    // zone on the client's own screen.
    if ((!localMovementIsActive || !p->hasLastState || gMpSession.initiatorFrozen)
        && !moveIntentBlocksSnap) {
        // Co-op diagnostic (throttled): a local-player transform snap. Shows
        // how far the client's dude is being pulled by the authoritative
        // state (expected to be sub-tile; large snaps indicate the avatar
        // lags behind - e.g. its walk never advanced).
        // (Commented: throttled noise — see git history.)
        // if (isLocalPlayer && p->hasLastState && obj->tile != s->tile) {
        //     static uint32_t gMpLocalSnapLogTick = 0;
        //     uint32_t nowTicks = getTicks();
        //     if (nowTicks - gMpLocalSnapLogTick > 500) {
        //         gMpLocalSnapLogTick = nowTicks;
        //         debugFilePrint("MP: local snap tile=%d->%d anim=%d",
        //             obj->tile, s->tile, localMovementIsActive ? 1 : 0);
        //     }
        // }
        // Remote avatars are applied directly from the authoritative state
        // (per-heartbeat snap). Attempts to animate their movement with a
        // registered walk made the avatar visibly skip: every re-targeted
        // walk restarts the animation from frame 0 at the state cadence, and
        // blocked destination tiles (stale client-side copies) killed the
        // walk silently. The snap reads smooth at the broadcast cadence and
        // can never freeze or stutter — the combat-speed boost for remote
        // avatars is a host-side walk concern only.
        mpApplyObjectTransform(obj, s->tile, s->x, s->y, s->rotation, s->fid, s->frame, s->elevation, 0, false);
        p->hasLastState = true;
        // The avatar reached the clicked destination: the intent's outcome is
        // fully applied — a later resolution must not snap anything.
        if (moveIntentInFlight && s->tile == moveIntentTile) {
            MpCombatClearMoveIntent();
        }
    }
    if (!isLocalPlayer) {
        // The host's FID model index is process-local ONLY for session
        // custom models (appended past the vanilla art list); vanilla models
        // — including armor/equipment models like the vault suit — resolve
        // to the same index on every machine and must pass through untouched,
        // or an equipped armor would be reverted to the profile's base model.
        // Remap through the profile's locally-resolved model index only when
        // the received model is a session model; animation/weapon/rotation
        // bits are machine-independent.
        MpPlayerRuntime* runtime = MpProfileGetRuntime(s->netId);
        if (runtime != nullptr && runtime->profile.localModelIndex >= 0) {
            int receivedModel = obj->fid & 0xFFF;
            if (receivedModel >= gArtCritterBaseLength) {
                obj->fid = (obj->fid & ~0xFFF) | runtime->profile.localModelIndex;
            }
        }
    }
    int oldLocalHp = 0;
    int oldLocalAp = -1;
    if (isLocalPlayer) {
        oldLocalHp = obj->data.critter.hp;
        oldLocalAp = obj->data.critter.combat.ap;
    }
    mpApplyCritterState(obj, s->hp, s->ap, s->radiation, s->poison, obj->data.critter.combat.team, obj->data.critter.combat.maneuver, s->combatResults);
    // Sneak state: bit 0 of the synced flags is DUDE_STATE_SNEAKING. The
    // local player toggles its own state at send time; the host's word here
    // keeps them aligned. Remote mirrors get their proto flag so the sneak
    // pose renders for other players.
    if (isLocalPlayer) {
        if ((s->flags & 1) != 0) {
            if (!dudeHasState(DUDE_STATE_SNEAKING)) {
                dudeEnableState(DUDE_STATE_SNEAKING);
            }
        } else if (dudeHasState(DUDE_STATE_SNEAKING)) {
            dudeDisableState(DUDE_STATE_SNEAKING);
        }
    } else {
        Proto* mirrorProto;
        if (protoGetProto(obj->pid, &mirrorProto) == 0) {
            if ((s->flags & 1) != 0) {
                mirrorProto->critter.data.flags |= (1 << DUDE_STATE_SNEAKING);
            } else {
                mirrorProto->critter.data.flags &= ~(1 << DUDE_STATE_SNEAKING);
            }
        }
    }
    // The host is authoritative over the local player's HP, but the vanilla
    // HUD only re-renders on local damage events. A hit taken from a remote
    // actor (or host-resolved damage) arrives via this state and would never
    // reach the HP bar — refresh it whenever the synced value changes.
    if (isLocalPlayer && obj->data.critter.hp != oldLocalHp) {
        interfaceRenderHitPoints(true);
        // Diagnostic (throttled): a local-player HP correction from the
        // authoritative channel. A large divergence (e.g. 9 -> 33) means the
        // client's local simulation applied its own damage numbers; the state
        // is truth and this log proves the correction landed.
        static uint32_t gMpLocalHpLogTick = 0;
        uint32_t nowTicks = getTicks();
        if (nowTicks - gMpLocalHpLogTick > 500) {
            gMpLocalHpLogTick = nowTicks;
            debugFilePrint("MP: local hp state %d->%d", oldLocalHp, obj->data.critter.hp);
        }
    }
    // Thin client: the AP bar no longer updates from local action writes (the
    // host owns AP spends). Refresh it when the synced value changes so the
    // mirror stays visible — same full interface refresh the vanilla action
    // path uses, not a one-off blit. The re-render is gated to the local
    // player's own turn (and out-of-combat): the host refreshes every
    // combatant's AP at round start, and re-rendering that while the enemy
    // (or another player) acts would turn the bar green before the player's
    // turn — vanilla keeps the stale render from the player's last spend
    // until their own turn re-renders it.
    if (isLocalPlayer && obj->data.critter.combat.ap != oldLocalAp
        && (!MpCombatIsActive() || gMpCombat.whoseTurn == gMpSession.localNetId)) {
        interfaceRenderActionPoints(obj->data.critter.combat.ap, _combat_free_move);
        // Trace the mirror's AP trajectory (throttled): explains any
        // lost/regained AP the player perceives during combat.
        if (MpCombatIsActive()) {
            static uint32_t gMpStateApLogTick = 0;
            uint32_t nowTicks = getTicks();
            if (nowTicks - gMpStateApLogTick > 500) {
                gMpStateApLogTick = nowTicks;
                debugFilePrint("MP: state ap %d->%d free=%d",
                    oldLocalAp, obj->data.critter.combat.ap, _combat_free_move);
            }
        }
    }
    // Bounds sanity on the authoritative channel: a wild value here would
    // corrupt the local mirror (defense against upstream bugs, not remote
    // tampering — the host is trusted).
    if (isLocalPlayer) {
        int maxHp = critterGetStat(obj, STAT_MAXIMUM_HIT_POINTS);
        int maxAp = critterGetStat(obj, STAT_MAXIMUM_ACTION_POINTS);
        if (obj->data.critter.hp > maxHp || s->hp > maxHp
            || s->ap > maxAp || obj->data.critter.combat.ap > maxAp) {
            static uint32_t gMpStateBoundsLogTick = 0;
            uint32_t nowTicks = getTicks();
            if (nowTicks - gMpStateBoundsLogTick > 2000) {
                gMpStateBoundsLogTick = nowTicks;
                debugFilePrint("MP: state bounds anomaly netId=%u hp=%d/%d ap=%d/%d",
                    s->netId, obj->data.critter.hp, maxHp,
                    obj->data.critter.combat.ap, maxAp);
            }
        }
    }
    p->hasInitialState = true;
    mpShowClientPlayer(obj);
    // A player state may arrive after the last sync chunk completed; without
    // this re-check the client would sit in SYNCING forever waiting on the
    // final readiness condition.
    mpClientTryFinishMapSync();
}

// Co-op: snap the local dude to an authoritative position (combat move
// resolutions that diverge from the optimistic walk). Stops any running
// local animation first so the walk cannot continue past the truth.
void MpApplyLocalDudeSnap(int tile, int elevation)
{
    Object* dude = gDude;
    if (dude == nullptr || !hexGridTileIsValid(tile) || !elevationIsValid(elevation)) {
        return;
    }
    if (animationIsBusy(dude) != 0) {
        reg_anim_clear(dude);
    }
    mpApplyObjectTransform(dude, tile, dude->x, dude->y, dude->rotation,
        dude->fid, 0, elevation, 0, false);
}

// Co-op: keep a critter's FID weapon slot in sync with the weapon actually in
// its hands. Vanilla sets the slot through inven_wield when the local player
// equips — but a co-op inventory apply rebuilds the item graph directly, so
// an equipped weapon arriving through the profile channel (or restored by a
// savegame load) never reaches the sprite. Deriving it here (left hand
// preferred, right hand fallback, mirroring the vanilla wield rules) keeps
// the sprite correct after any apply, join, or load.
void MpSyncCritterWeaponFid(Object* critter)
{
    if (critter == nullptr || objectTypeFromPid(critter->pid) != OBJ_TYPE_CRITTER) {
        return;
    }

    Object* weapon = nullptr;
    Inventory* inventory = &critter->data.inventory;
    for (int index = 0; index < inventory->length; index++) {
        Object* item = inventory->items[index].item;
        if (item == nullptr) {
            continue;
        }
        if ((item->flags & OBJECT_IN_LEFT_HAND) != 0) {
            weapon = item;
            break;
        }
        if (weapon == nullptr && (item->flags & OBJECT_IN_RIGHT_HAND) != 0) {
            weapon = item;
        }
    }

    WeaponAnimation weaponAnimationCode = WEAPON_ANIMATION_NONE;
    if (weapon != nullptr && itemGetType(weapon) == ITEM_TYPE_WEAPON) {
        weaponAnimationCode = weaponGetAnimationCode(weapon);
    }
    if (weaponAnimationFromFid(critter->fid) == weaponAnimationCode) {
        return;
    }

    int fid = buildFid(OBJ_TYPE_CRITTER, critter->fid & 0xFFF,
        animationTypeFromFid(critter->fid), weaponAnimationCode,
        critter->rotation + 1);
    if (fid != critter->fid) {
        Rect rect;
        if (objectSetFid(critter, fid, &rect) == 0) {
            tileWindowRefreshRect(&rect, critter->elevation);
        }
        debugFilePrint("MP: weapon fid synced pid=0x%X anim=%d weapon=%d",
            critter->pid, (int)animationTypeFromFid(critter->fid),
            (int)weaponAnimationCode);
    }
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
        // state lives in the tile word itself. The client must adopt the
        // static roof data (frm id + flags) but NOT the host's hidden state —
        // the host resyncs ALL tiles whenever its own dude walks under a roof,
        // and the client would otherwise inherit roofs hidden under the host's
        // feet and never restore them. The hidden flag is bit 0 of the roof
        // flag nibble, i.e. 0x1000 of the roof word (roof word layout: frm id
        // bits 0-11, flags bits 12-15). Keep only the LOCAL hidden bit so the
        // client's own roof-fill state above the local player survives.
        int32_t roof = (incoming >> 16) & 0xFFFF;
        int32_t localRoof = (local >> 16) & 0xFFFF;
        roof = (roof & ~0x1000) | (localRoof & 0x1000);

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

// A passed vote resolved a transition to another tile/elevation of the
// CURRENT map. The map stays loaded; the whole session switches elevation
// together. Snap the local dude to the destination, switch the view
// elevation, and close the passed-vote tally (the same-map transition applied
// without a MAP_CHANGED).
void MpOnMapElevationChange(const NetMapElevationPayload* payload)
{
    if (!gMpIsClient || payload == nullptr
        || !hexGridTileIsValid(payload->tile) || !elevationIsValid(payload->elevation)) {
        return;
    }
    debugFilePrint("MP: map elevation change tile=%d elev=%d rot=%d",
        payload->tile, payload->elevation, payload->rotation);
    MpVoteHideUI();
    gVoteSession.state = VOTE_STATE_NONE;
    if (gDude != nullptr) {
        gMpSuppressExitGridCheck = true;
        objectSetLocation(gDude, payload->tile, payload->elevation, nullptr);
        objectSetRotation(gDude, static_cast<Rotation>(payload->rotation), nullptr);
        gMpSuppressExitGridCheck = false;
        mapSetElevation(payload->elevation);
        objUpdateRoofsForTile(gDude->tile, gDude->elevation);
        tileSetCenter(gDude->tile,
            TILE_SET_CENTER_REFRESH_WINDOW | TILE_SET_CENTER_FLAG_IGNORE_SCROLL_RESTRICTIONS);
    }
}

// Host-side mirror of a script-driven inventory removal: a script removed an
// item from a player avatar's inventory on the host (e.g. the temple warrior
// strip at the start of the fight). Scripts targeting "the player" act on the
// party in co-op, so the same removal (matched by pid) is applied to EVERY
// other connected player avatar, and each owning client whose inventory
// changed is relayed a NET_PKT_ITEM_REMOVE so its LOCAL inventory follows.
// The originally targeted avatar is already handled by the vanilla opcode.
void MpHostMirrorItemRemoval(Object* avatar, Object* item, int quantity)
{
    if (!gMpIsHost || !gMpActive || avatar == nullptr || item == nullptr || quantity <= 0) {
        return;
    }

    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        MultiplayerPlayer* player = &gMpSession.players[index];
        if (!player->isConnected || player->isLocal || player->obj == nullptr
            || player->obj == avatar || player->peer == nullptr) {
            continue;
        }
        Object* remoteItem = nullptr;
        for (int itemIndex = 0; itemIndex < player->obj->data.inventory.length; itemIndex++) {
            Object* candidate = player->obj->data.inventory.items[itemIndex].item;
            if (candidate != nullptr && candidate->pid == item->pid) {
                remoteItem = candidate;
                break;
            }
        }
        if (remoteItem == nullptr) {
            continue;
        }
        int remoteQty = quantity;
        int have = itemGetQuantity(player->obj, remoteItem);
        if (remoteQty > have) {
            remoteQty = have;
        }
        if (remoteQty <= 0) {
            continue;
        }
        itemRemoveWithReason(player->obj, remoteItem, remoteQty, RemoveInventoryObjectHookReason::ItemRemoved);
        NetItemRemovePayload payload;
        payload.pid = remoteItem->pid;
        payload.quantity = remoteQty;
        debugFilePrint("MP: item remove spread netId=%u pid=0x%X qty=%d",
            player->netId, remoteItem->pid, remoteQty);
        NetSendPacket(player->peer, NET_CHANNEL_RELIABLE, NET_PKT_ITEM_REMOVE, &payload, sizeof(payload));
    }

    // If the script targeted a REMOTE avatar directly, that player's client
    // needs the removal relayed too (its local inventory must drop the item).
    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        MultiplayerPlayer* player = &gMpSession.players[index];
        if (!player->isConnected || player->isLocal || player->obj != avatar
            || player->peer == nullptr) {
            continue;
        }
        NetItemRemovePayload payload;
        payload.pid = item->pid;
        payload.quantity = quantity;
        debugFilePrint("MP: item remove relay netId=%u pid=0x%X qty=%d", player->netId, item->pid, quantity);
        NetSendPacket(player->peer, NET_CHANNEL_RELIABLE, NET_PKT_ITEM_REMOVE, &payload, sizeof(payload));
        break;
    }
}

void MpHostMirrorInventoryMove(Object* sourceAvatar, Object* destContainer,
    const int* pids, const int* qtys, int count)
{
    if (!gMpIsHost || !gMpActive || sourceAvatar == nullptr || destContainer == nullptr
        || pids == nullptr || qtys == nullptr || count <= 0) {
        return;
    }

    // The vanilla move already stripped the SOURCE avatar's inventory into the
    // container. In co-op the strip is a PARTY strip: every connected player
    // loses their gear into the same container (matching the vanilla
    // move_obj_inven_to_obj semantics, which moves the whole inventory), each
    // owning client drops the same items locally, and every affected avatar's
    // sprite is corrected to the unarmed/armor-less fid.
    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        MultiplayerPlayer* player = &gMpSession.players[index];
        // Local players have peer == nullptr (the host's own entry); they must
        // still be stripped.
        if (!player->isConnected || player->obj == nullptr
            || (player->peer == nullptr && !player->isLocal)) {
            continue;
        }

        bool isSource = (player->obj == sourceAvatar);
        bool isLocalDude = (player->obj == gDude) && player->isLocal;

        // Snapshot the gear this avatar is about to lose. The source avatar was
        // already stripped by the vanilla opcode, so relay the caller's
        // snapshot; every other avatar is stripped here.
        std::vector<int> losePids;
        std::vector<int> loseQtys;
        if (isSource) {
            for (int i = 0; i < count; i++) {
                losePids.push_back(pids[i]);
                loseQtys.push_back(qtys[i]);
            }
        } else {
            for (int itemIndex = 0; itemIndex < player->obj->data.inventory.length; itemIndex++) {
                Object* stripItem = player->obj->data.inventory.items[itemIndex].item;
                if (stripItem != nullptr) {
                    losePids.push_back(stripItem->pid);
                    loseQtys.push_back(player->obj->data.inventory.items[itemIndex].quantity);
                }
            }
        }
        if (losePids.empty()) {
            continue;
        }

        // Capture the old weapon/armor for the fid/stat cleanup after the move.
        Object* oldWeapon = nullptr;
        Object* oldArmor = nullptr;
        if (isLocalDude) {
            oldWeapon = interfaceGetCurrentHand() == HAND_RIGHT
                ? critterGetItem2(player->obj)
                : critterGetItem1(player->obj);
            oldArmor = critterGetArmor(player->obj);
        } else {
            oldWeapon = critterGetItem2(player->obj);
            if (oldWeapon == nullptr) {
                oldWeapon = critterGetItem1(player->obj);
            }
        }

        if (!isSource) {
            itemMoveAll(player->obj, destContainer);
        }

        // Relay every removed item to the owning client so the local inventory
        // matches.
        if (!player->isLocal) {
            for (size_t i = 0; i < losePids.size(); i++) {
                NetItemRemovePayload payload;
                payload.pid = losePids[i];
                payload.quantity = loseQtys[i];
                debugFilePrint("MP: item strip %s netId=%u pid=0x%X qty=%d",
                    isSource ? "relay" : "spread",
                    player->netId, losePids[i], loseQtys[i]);
                NetSendPacket(player->peer, NET_CHANNEL_RELIABLE, NET_PKT_ITEM_REMOVE,
                    &payload, sizeof(payload));
            }
        }

        // Correct the avatar's sprite: the vanilla opcode only ran its gDude
        // cleanup when the script targeted the host's own dude — for every
        // avatar actually stripped here (remote avatars AND the local dude when
        // the script targeted a remote avatar), replicate it. The source
        // avatar's fid was already corrected by the vanilla opcode; the others
        // are corrected here.
        if (isLocalDude) {
            if (oldWeapon != nullptr) {
                int flags = 0;
                if ((oldWeapon->flags & OBJECT_IN_LEFT_HAND) != 0) {
                    flags |= OBJECT_IN_LEFT_HAND;
                }
                if ((oldWeapon->flags & OBJECT_IN_RIGHT_HAND) != 0) {
                    flags |= OBJECT_IN_RIGHT_HAND;
                }
                correctFidForRemovedItem(player->obj, oldWeapon, flags);
            }
            if (oldArmor != nullptr) {
                adjustCritterStatsOnArmorChange(gDude, oldArmor, nullptr);
            }
            _proto_dude_update_gender();
            bool animated = !gameUiIsDisabled();
            interfaceUpdateItems(animated, INTERFACE_ITEM_ACTION_DEFAULT, INTERFACE_ITEM_ACTION_DEFAULT);
            debugFilePrint("MP: item strip local dude cleaned up");
        } else if (!isSource && oldWeapon != nullptr) {
            int flags = 0;
            if ((oldWeapon->flags & OBJECT_IN_LEFT_HAND) != 0) {
                flags |= OBJECT_IN_LEFT_HAND;
            }
            if ((oldWeapon->flags & OBJECT_IN_RIGHT_HAND) != 0) {
                flags |= OBJECT_IN_RIGHT_HAND;
            }
            correctFidForRemovedItem(player->obj, oldWeapon, flags);
            // Vanilla's non-gDude correction only un-arms right-hand weapons;
            // force the unarmed fid when no hand weapon remains so the owning
            // client's sprite doesn't stay armed.
            if (weaponAnimationFromFid(player->obj->fid) != WEAPON_ANIMATION_NONE
                && critterGetItem1(player->obj) == nullptr
                && critterGetItem2(player->obj) == nullptr) {
                Rect unarmRect;
                objectSetFid(player->obj, buildFid(objectTypeFromFid(player->obj->fid),
                    player->obj->fid & 0xFFF, animationTypeFromFid(player->obj->fid),
                    WEAPON_ANIMATION_NONE, rotationFromFid(player->obj->fid)), &unarmRect);
            }
            debugFilePrint("MP: item strip avatar fid corrected netId=%u", player->netId);
        }
    }
}

void MpOnItemRemove(const NetItemRemovePayload* payload)
{
    if (!gMpIsClient || payload == nullptr || gDude == nullptr) {
        return;
    }
    Object* item = nullptr;
    for (int index = 0; index < gDude->data.inventory.length; index++) {
        Object* candidate = gDude->data.inventory.items[index].item;
        if (candidate != nullptr && candidate->pid == payload->pid) {
            item = candidate;
            break;
        }
    }
    if (item == nullptr) {
        debugFilePrint("MP: item remove no item pid=0x%X qty=%d", payload->pid, payload->quantity);
        return;
    }
    debugFilePrint("MP: item remove applied pid=0x%X qty=%d", payload->pid, payload->quantity);
    int quantity = payload->quantity;
    int have = itemGetQuantity(gDude, item);
    if (quantity > have) {
        quantity = have;
    }
    if (quantity <= 0) {
        return;
    }
    bool updateFlags = false;
    int flags = 0;
    RemoveInventoryObjectHookReason removeReason = RemoveInventoryObjectHookReason::ItemRemoved;
    // Detect equipped status by pointer comparison against the current hand
    // and armor slots, NOT by the item's flags — the stripped copy may not
    // carry OBJECT_EQUIPPED, and the unequip action (which updates the sprite
    // fid) only fires when the equipped removal reason is used.
    if (item == critterGetItem1(gDude) || (item->flags & OBJECT_IN_LEFT_HAND) != 0) {
        flags |= OBJECT_IN_LEFT_HAND;
        removeReason = RemoveInventoryObjectHookReason::LeftHandEquipped;
    }
    if (item == critterGetItem2(gDude) || (item->flags & OBJECT_IN_RIGHT_HAND) != 0) {
        flags |= OBJECT_IN_RIGHT_HAND;
        removeReason = RemoveInventoryObjectHookReason::RightHandEquipped;
    }
    if (item == critterGetArmor(gDude) || (item->flags & OBJECT_WORN) != 0) {
        flags |= OBJECT_WORN;
        if (removeReason == RemoveInventoryObjectHookReason::ItemRemoved) {
            removeReason = RemoveInventoryObjectHookReason::ArmorEquipped;
        }
    }
    updateFlags = (flags & (OBJECT_IN_ANY_HAND | OBJECT_WORN)) != 0;
    if (itemRemoveWithReason(gDude, item, quantity, removeReason) == 0) {
        if (updateFlags) {
            correctFidForRemovedItem(gDude, item, flags);
        }
        // Belt-and-suspenders: if the dude's fid still shows a weapon
        // animation but no hand weapon remains, force the unarmed fid —
        // mirrors the host-side strip cleanup and covers ambiguous slot state.
        if (weaponAnimationFromFid(gDude->fid) != WEAPON_ANIMATION_NONE
            && critterGetItem1(gDude) == nullptr
            && critterGetItem2(gDude) == nullptr) {
            Rect unarmRect;
            objectSetFid(gDude, buildFid(objectTypeFromFid(gDude->fid),
                gDude->fid & 0xFFF, animationTypeFromFid(gDude->fid),
                WEAPON_ANIMATION_NONE, rotationFromFid(gDude->fid)), &unarmRect);
            debugFilePrint("MP: item remove client force-unarmed pid=0x%X", payload->pid);
        }
        bool animated = !gameUiIsDisabled();
        interfaceUpdateItems(animated, INTERFACE_ITEM_ACTION_DEFAULT, INTERFACE_ITEM_ACTION_DEFAULT);
    }
}

void MpOnGameTime(const NetGameTimePayload* payload)
{
    if (payload == nullptr || payload->time <= 0) {
        return;
    }
    // Adopt the host's authoritative clock. Between syncs the client's own
    // tick advances the mirror (see _script_chk_timed_events), so this keeps
    // time-gated scripts roughly in lockstep with the host without spamming.
    // debugFilePrint("MP: game time sync %u -> %u", gameTimeGetTime(), payload->time);
    gameTimeSetTime((unsigned int)payload->time);
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
        // files) can never collapse two objects onto one netId. Local UI
        // helpers (hex cursor etc.) are never networked.
        if (gMpHostObjNetIds.find(obj) == gMpHostObjNetIds.end()
            && !mpIsLocalUiObject(obj)) {
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

void MpToHitQuery(Object* target, int hitMode)
{
    if (!gMpIsClient || target == nullptr || gMpSession.hostPeer == nullptr) {
        return;
    }

    NetToHitQueryPayload query;
    memset(&query, 0, sizeof(query));
    query.targetNetId = MpGetObjNetId(target);
    query.hitMode = (uint8_t)hitMode;
    if (query.targetNetId == 0) {
        debugFilePrint("MP: to-hit query dropped (target has no netId) mode=%d", hitMode);
        return;
    }

    NetSendPacket(gMpSession.hostPeer, NET_CHANNEL_RELIABLE,
        NET_PKT_TO_HIT_QUERY, &query, sizeof(query));
    debugFilePrint("MP: to-hit query sent targetNetId=%u mode=%d",
        query.targetNetId, query.hitMode);
}

bool MpToHitResultTake(uint32_t* targetNetId, uint8_t* hitMode, int probs[8])
{
    if (!gToHitResultPending) {
        return false;
    }

    *targetNetId = gToHitResultTarget;
    *hitMode = gToHitResultMode;
    memcpy(probs, gToHitResultProbs, sizeof(gToHitResultProbs));
    gToHitResultPending = false;
    return true;
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
                && objectTypeFromFid(obj->fid) == OBJ_TYPE_CRITTER) {
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

// Teleport the whole session to an intra-map destination (an exit grid,
// ladder or stairs pointing at another tile/elevation of the CURRENT map).
// Elevation is shared: every player's critter moves together and the map
// stays loaded — this is a level switch, not a map change. Runs on the host
// after a passed vote; the clients snap via NET_PKT_MAP_ELEVATION + the
// player-state channel.
static void mpApplySessionElevationChange(const MapTransition* transition)
{
    if (!gMpIsHost || transition == nullptr
        || !hexGridTileIsValid(transition->tile) || !elevationIsValid(transition->elevation)) {
        return;
    }
    debugFilePrint("MP: session elevation change tile=%d elev=%d rot=%d",
        transition->tile, transition->elevation, transition->rotation);
    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        MultiplayerPlayer* player = &gMpSession.players[index];
        if (!player->isConnected || player->obj == nullptr) {
            continue;
        }
        reg_anim_clear(player->obj);
        gMpSuppressExitGridCheck = true;
        objectSetLocation(player->obj, transition->tile, transition->elevation, nullptr);
        objectSetRotation(player->obj, transition->rotation, nullptr);
        gMpSuppressExitGridCheck = false;
        player->lastSafeTile = transition->tile;
        player->lastSafeElevation = transition->elevation;
        player->lastSafeRotation = transition->rotation;
    }
    mapSetElevation(transition->elevation);
    if (gDude != nullptr) {
        // The host's own dude was teleported through objectSetLocation() like
        // everyone else; run the roof state machine so the roof-clear circle
        // (and its gEgg anchor) follows him to the new tile/elevation.
        objUpdateRoofsForTile(gDude->tile, gDude->elevation);
        tileSetCenter(gDude->tile,
            TILE_SET_CENTER_REFRESH_WINDOW | TILE_SET_CENTER_FLAG_IGNORE_SCROLL_RESTRICTIONS);
    }
    NetMapElevationPayload p;
    p.tile = transition->tile;
    p.elevation = transition->elevation;
    p.rotation = transition->rotation;
    NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE,
        NET_PKT_MAP_ELEVATION, &p, sizeof(p));
    MpBroadcastPlayerStates();
}

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
    // If a vote concluded PASSED, this is the host's resolve path calling the
    // real transition after the passed-display beat. An intra-map destination
    // (another tile/elevation of the CURRENT map) is applied as a session-wide
    // elevation change — the map stays loaded and elevation is shared; a
    // cross-map destination runs the vanilla reload + full-sync flow.
    // FAILED/CANCELLED never start transitions, and clients never start
    // transitions on their own: their map changes are always driven by the
    // host's MAP_CHANGED.
    if (gMpIsHost && gVoteSession.state == VOTE_STATE_PASSED) {
        if (transition->map == gMapHeader.index) {
            mpApplySessionElevationChange(transition);
            return 1;
        }
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
