#include "multiplayer_worldmap.h"

#include <string.h>

#include "debug.h"
#include "input.h"
#include "map.h"
#include "multiplayer.h"
#include "multiplayer_chat.h"
#include "multiplayer_dialog.h"
#include "multiplayer_loot.h"
#include "multiplayer_log.h"
#include "scripts.h"
#include "worldmap.h"

namespace fallout {

// Session state: the host is inside the worldmap modal (or a client's mirror
// is up / queued). Read-only is client-only: the mirror renders the vanilla
// loop but takes no travel action.
bool gMpWorldmapActive = false;
bool gMpWorldmapReadOnly = false;
int gMpWorldmapMode = 0;
// Client: the mirror should open on the next top-level MpTick (never from
// inside a NetHostService callback — the modal blocks and pumps MpTick).
bool gMpWorldmapUiPending = false;
// Client: a STATE packet landed while the mirror is up; the loop must
// re-render the worldmap surface to show the new party/destination/car.
bool gMpWorldmapDirty = false;

// Full-snapshot buffers for the discovery sync (100 vanilla tiles * 11 packed
// bytes + area pairs; sized for NET_MAX_PACKET_SIZE).
static uint8_t gMpWmFullSubTiles[WM_DISCOVERY_BYTES_PER_TILE * 300];
static uint8_t gMpWmFullAreaVisited[CITY_COUNT];
static uint8_t gMpWmFullAreaState[CITY_COUNT];

// Client-side encounter-id stash. The host ships the encounter triple in the
// final worldmap state right before EXIT/MAP_CHANGED; the client's deferred
// map-enter needs it to spawn its encounter copy. wmGenData's copy is reset
// somewhere in the transition window (mirror teardown / map load) before the
// deferred run reads it, so the triple is stashed here at apply time and
// consumed by the deferred runner (consume-once; a new ENTER clears it).
static int gMpClientEncounterStash[3] = { -1, -1, -1 };

void mpWorldmapClientTakeEncounterState(int* mapId, int* tableId, int* entryId)
{
    if (mapId != nullptr) {
        *mapId = gMpClientEncounterStash[0];
    }
    if (tableId != nullptr) {
        *tableId = gMpClientEncounterStash[1];
    }
    if (entryId != nullptr) {
        *entryId = gMpClientEncounterStash[2];
    }
    gMpClientEncounterStash[0] = -1;
    gMpClientEncounterStash[1] = -1;
    gMpClientEncounterStash[2] = -1;
}

// Sends the full discovery state (subtile fog + area visited/known), then
// re-baselines the host diff so subsequent broadcasts carry only deltas.
static void mpWorldmapSendFullDiscovery()
{
    if (!gMpIsHost || !gMpActive || gMpSession.enetHost == nullptr) {
        return;
    }
    int tileCount = wmDiscoveryBuildFull(gMpWmFullSubTiles, sizeof(gMpWmFullSubTiles),
        gMpWmFullAreaVisited, gMpWmFullAreaState, CITY_COUNT);
    if (tileCount <= 0) {
        MpLogAlways(MP_LOG_WORLDMAP, "host discovery full build failed");
        return;
    }

    NetWorldmapDiscoveryPayload header;
    header.mode = 1;
    header.count = (uint16_t)tileCount;
    header.reserved = 0;

    char buffer[NET_MAX_PACKET_SIZE];
    size_t offset = 0;
    memcpy(buffer, &header, sizeof(header));
    offset += sizeof(header);
    size_t subtileBytes = (size_t)tileCount * WM_DISCOVERY_BYTES_PER_TILE;
    memcpy(buffer + offset, gMpWmFullSubTiles, subtileBytes);
    offset += subtileBytes;
    uint8_t areaCount = (uint8_t)(wmAreaCount() < CITY_COUNT ? wmAreaCount() : CITY_COUNT);
    buffer[offset++] = areaCount;
    for (int i = 0; i < areaCount; i++) {
        buffer[offset++] = gMpWmFullAreaVisited[i];
        buffer[offset++] = gMpWmFullAreaState[i];
    }

    NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE,
        NET_PKT_WORLDMAP_DISCOVERY, buffer, offset);
    wmDiscoveryBaseline();
    MpLog(MP_LOG_WORLDMAP, "host discovery full tiles=%d areas=%d bytes=%zu",
        tileCount, areaCount, offset);
}

// Sends whatever subtile/area discovery changed since the last broadcast.
// On overflow (more changes than fit) falls back to a full snapshot.
static void mpWorldmapSendDiscoveryChanges()
{
    if (!gMpIsHost || !gMpActive || gMpSession.enetHost == nullptr) {
        return;
    }
    static WmDiscoveryChange gMpWmChanges[WM_DISCOVERY_CHANGE_MAX];
    int count = 0;
    bool fullDirty = false;
    wmDiscoveryTakeChanges(gMpWmChanges, WM_DISCOVERY_CHANGE_MAX, &count, &fullDirty);
    if (fullDirty) {
        mpWorldmapSendFullDiscovery();
        return;
    }
    if (count <= 0) {
        return;
    }

    NetWorldmapDiscoveryPayload header;
    header.mode = 2;
    header.count = (uint16_t)count;
    header.reserved = 0;

    char buffer[NET_MAX_PACKET_SIZE];
    size_t offset = 0;
    memcpy(buffer, &header, sizeof(header));
    offset += sizeof(header);
    for (int i = 0; i < count; i++) {
        memcpy(buffer + offset, &gMpWmChanges[i].tile, 2);
        offset += 2;
        buffer[offset++] = gMpWmChanges[i].subX;
        buffer[offset++] = gMpWmChanges[i].subY;
        buffer[offset++] = gMpWmChanges[i].state;
    }

    NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE,
        NET_PKT_WORLDMAP_DISCOVERY, buffer, offset);
    MpLog(MP_LOG_WORLDMAP, "host discovery changes=%d bytes=%zu", count, offset);
}

// Builds the current authoritative state payload from the vanilla WmGenData
// (host side). Used by the throttled broadcast and the mid-travel join send.
static void mpWorldmapFillStatePayload(NetWorldmapStatePayload* p)
{
    memset(p, 0, sizeof(*p));
    int worldX = 0;
    int worldY = 0;
    wmGetPartyWorldPos(&worldX, &worldY);
    p->worldPosX = worldX;
    p->worldPosY = worldY;
    int destX = 0;
    int destY = 0;
    bool walking = false;
    bool inCar = false;
    wmGetPartyTravelState(&destX, &destY, &walking, &inCar);
    p->walkDestinationX = destX;
    p->walkDestinationY = destY;
    p->isWalking = walking ? 1 : 0;
    p->isInCar = inCar ? 1 : 0;
    int area = -1;
    wmGetPartyCurArea(&area);
    p->currentAreaId = area;
    p->carFuel = wmCarGasAmount();
    p->currentCarAreaId = wmCarCurrentArea();
    wmGetEncounterState(&p->encounterMapId, &p->encounterTableId, &p->encounterEntryId);
}

void MpWorldmapHostEntered(int mode)
{
    if (!gMpIsHost || !gMpActive) {
        return;
    }
    gMpWorldmapActive = true;
    gMpWorldmapMode = mode;
    NetWorldmapEnterPayload payload;
    payload.mode = (uint8_t)mode;
    NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE,
        NET_PKT_WORLDMAP_ENTER, &payload, sizeof(payload));
    // Full discovery state so the client mirror starts with the host's fog
    // and town list (its save may know a different set of tiles).
    mpWorldmapSendFullDiscovery();
    MpLog(MP_LOG_WORLDMAP, "host entered worldmap mode=%d", mode);
}

void MpWorldmapHostLeft()
{
    if (!gMpIsHost || !gMpActive || !gMpWorldmapActive) {
        return;
    }
    gMpWorldmapActive = false;
    gMpWorldmapMode = 0;
    MpChatAutoOpenCancel();
    NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE,
        NET_PKT_WORLDMAP_EXIT, nullptr, 0);
    MpLog(MP_LOG_WORLDMAP, "host left worldmap (no map load)");
}

void MpWorldmapHostLoadMap(int mapId)
{
    if (!gMpIsHost || !gMpActive) {
        return;
    }
    MpLog(MP_LOG_WORLDMAP, "host worldmap load map=%d", mapId);

    // Ship the final state BEFORE the transition: the encounter selection
    // (wmRndEncounterPick) ran inside the walking block this same frame, so
    // the throttled 50ms broadcast never carried the ids — and the client's
    // deferred map-enter needs them to spawn its own encounter critters.
    NetWorldmapStatePayload finalState;
    mpWorldmapFillStatePayload(&finalState);
    NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE,
        NET_PKT_WORLDMAP_STATE, &finalState, sizeof(finalState));
    MpLog(MP_LOG_WORLDMAP, "host final state enc=%d,%d,%d pos=%d,%d area=%d",
        finalState.encounterMapId, finalState.encounterTableId, finalState.encounterEntryId,
        finalState.worldPosX, finalState.worldPosY, finalState.currentAreaId);

    // Clients close their mirrors now: EXIT arrives inline (the mirror checks
    // gMpWorldmapActive each frame and breaks), and the MAP_CHANGED that
    // follows lands in the deferred queue to drain on the next top-level tick
    // after the mirror is down.
    gMpWorldmapActive = false;
    gMpWorldmapMode = 0;
    MpChatAutoOpenCancel();
    NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE,
        NET_PKT_WORLDMAP_EXIT, nullptr, 0);

    // Same pipeline as map.cc mapHandleTransition's host branch: detach the
    // old avatars, load the raw .MAP, rebind the local player, respawn the
    // remote avatars from profiles, ship MAP_CHANGED + FULL_SYNC, then run the
    // deferred map-enter script and any random encounter setup.
    MpPrepareForMapChange();
    gMpDeferMapEnterScript = true;
    int mapLoadResult = mapLoadById(mapId);
    gMpDeferMapEnterScript = false;
    if (mapLoadResult != 0) {
        MpFinishHostMapChange();
        MpBroadcastMapChangeAbort();
        MpLogAlways(MP_LOG_WORLDMAP, "host worldmap map load failed map=%d", mapId);
        return;
    }
    MpFinishHostMapChange();
    MpBroadcastMapChanged(gMapHeader.index);
    MpBroadcastMapFullSync(nullptr);
    if (gMpDeferredMapEnterPending) {
        gMpDeferredMapEnterPending = false;
        _scr_spatials_disable();
        scriptExecProc(gMapSid, SCRIPT_PROC_MAP_ENTER);
        _scr_spatials_enable();
        MpLog(MP_LOG_WORLDMAP, "host worldmap deferred map enter done sid=%d", gMapSid);
        int encMap = -1, encTable = -1, encEntry = -1;
        wmGetEncounterState(&encMap, &encTable, &encEntry);
        MpLog(MP_LOG_WORLDMAP, "host encounter setup map=%d table=%d entry=%d",
            encMap, encTable, encEntry);
        // Vanilla spawns encounter critters inside mapLoad before the
        // start/enter pass (their map_enter sets hostility, e.g. slavers);
        // the deferred runner spawns after the pass ran — replay the two
        // procs for the freshly added scripts.
        mpScriptListSnapshot();
        if (wmSetupRandomEncounter() == -1) {
            debugPrint("\nError: couldn't set up random encounter after worldmap map enter!");
        }
        mpScriptRunStartAndEnterForNew();
    }

    // Mirror the vanilla transition path's worldmap snap (map.cc:1500 and
    // client MpApplyMapChanged): put the shared party marker on the area that
    // contains the loaded map, so host and clients agree on the worldmap
    // position after every worldmap-driven map load.
    int city = -1;
    if (wmMatchAreaContainingMapIdx(gMapHeader.index, &city) == 0 && wmTeleportToArea(city) == -1) {
        MpLog(MP_LOG_WORLDMAP, "host worldmap teleport failed map=%d area=%d",
            gMapHeader.index, city);
    }
    MpLog(MP_LOG_WORLDMAP, "host worldmap load done map=%d", gMapHeader.index);
}

void MpWorldmapBroadcastState(bool force)
{
    if (!gMpIsHost || !gMpActive || !gMpWorldmapActive || gMpSession.enetHost == nullptr) {
        return;
    }
    // Walking steps are already throttled by the vanilla travel delay, but a
    // destination change (click / quick travel) must go out immediately.
    static uint32_t gMpLastWorldmapStateTick = 0;
    uint32_t now = getTicks();
    if (!force && now - gMpLastWorldmapStateTick < 50) {
        return;
    }
    gMpLastWorldmapStateTick = now;

    NetWorldmapStatePayload payload;
    mpWorldmapFillStatePayload(&payload);
    NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE,
        NET_PKT_WORLDMAP_STATE, &payload, sizeof(payload));
    // Discovery deltas ride the same cadence (the walk marks subtiles right
    // before this broadcast in the worldmap loop).
    mpWorldmapSendDiscoveryChanges();
    // State heartbeat at a quiet cadence (the broadcast itself runs every
    // ~50ms; a log per packet would dwarf everything else in the session).
    static uint32_t sLastHostStateLogTick = 0;
    if (now - sLastHostStateLogTick >= 1000) {
        sLastHostStateLogTick = now;
        MpLog(MP_LOG_WORLDMAP, "host state pos=%d,%d dest=%d,%d walk=%d car=%d fuel=%d area=%d",
            payload.worldPosX, payload.worldPosY,
            payload.walkDestinationX, payload.walkDestinationY,
            payload.isWalking, payload.isInCar, payload.carFuel, payload.currentAreaId);
    }
}

void MpWorldmapSendStateToPeer(ENetPeer* peer)
{
    if (!gMpIsHost || !gMpActive || peer == nullptr || !gMpWorldmapActive) {
        return;
    }
    NetWorldmapEnterPayload enter;
    enter.mode = (uint8_t)gMpWorldmapMode;
    NetSendPacket(peer, NET_CHANNEL_RELIABLE, NET_PKT_WORLDMAP_ENTER, &enter, sizeof(enter));
    NetWorldmapStatePayload state;
    mpWorldmapFillStatePayload(&state);
    NetSendPacket(peer, NET_CHANNEL_RELIABLE, NET_PKT_WORLDMAP_STATE, &state, sizeof(state));
    // Mid-travel join: the full discovery state, re-baselined for this peer.
    mpWorldmapSendFullDiscovery();
    MpLog(MP_LOG_WORLDMAP, "host sent worldmap state to joining peer mode=%d", gMpWorldmapMode);
}

void MpWorldmapOnEnter(const NetWorldmapEnterPayload* payload)
{
    if (!gMpIsClient || payload == nullptr) {
        return;
    }
    MpLog(MP_LOG_WORLDMAP, "client enter worldmap mode=%d", payload->mode);
    // Decision: dialog/barter/loot are not travel blockers — yank them. The
    // dialog modal pumps MpTick, so closing it from here (same as DIALOG_END)
    // lets its loop exit on the next iteration; the mirror opens on a later
    // top-level tick.
    MpDialogReset();
    MpLootOnClientReset();
    // New worldmap session: any stashed encounter triple from a previous
    // session is stale.
    gMpClientEncounterStash[0] = -1;
    gMpClientEncounterStash[1] = -1;
    gMpClientEncounterStash[2] = -1;
    gMpWorldmapMode = payload->mode;
    gMpWorldmapActive = true;
    gMpWorldmapUiPending = true;
}

void MpWorldmapOnState(const NetWorldmapStatePayload* payload)
{
    if (!gMpIsClient || payload == nullptr) {
        return;
    }
    wmSetPartyWorldPos(payload->worldPosX, payload->worldPosY);
    wmSetPartyTravelState(payload->walkDestinationX, payload->walkDestinationY,
        payload->isWalking != 0, payload->isInCar != 0);
    wmSetPartyCurArea(payload->currentAreaId);
    wmCarSetCurrentArea(payload->currentCarAreaId);
    // Encounter selection: the host picked it right before the map load; the
    // client's deferred map-enter runs wmSetupRandomEncounter with these ids
    // to spawn its own copy of the encounter critters.
    wmSetEncounterState(payload->encounterMapId, payload->encounterTableId,
        payload->encounterEntryId);
    // Stash a non-empty encounter triple: the state packet that carries it is
    // the last one before EXIT/MAP_CHANGED, and something in the client's
    // transition window (mirror teardown / map load) resets wmGenData before
    // the deferred run reads it. The deferred runner consumes the stash.
    if (payload->encounterMapId != -1) {
        gMpClientEncounterStash[0] = payload->encounterMapId;
        gMpClientEncounterStash[1] = payload->encounterTableId;
        gMpClientEncounterStash[2] = payload->encounterEntryId;
    }
    int currentFuel = wmCarGasAmount();
    if (payload->carFuel > currentFuel) {
        wmCarFillGas(payload->carFuel - currentFuel);
    } else if (payload->carFuel < currentFuel) {
        wmCarUseGas(currentFuel - payload->carFuel);
    }
    // The mirror repaints the worldmap surface this frame.
    gMpWorldmapDirty = true;
    // State heartbeat at a quiet cadence (the broadcast itself runs every
    // ~50ms; a log per packet would dwarf everything else in the session).
    static uint32_t sLastClientStateLogTick = 0;
    uint32_t nowTicks = getTicks();
    if (nowTicks - sLastClientStateLogTick >= 1000) {
        sLastClientStateLogTick = nowTicks;
        MpLog(MP_LOG_WORLDMAP, "client state pos=%d,%d dest=%d,%d walk=%d car=%d fuel=%d area=%d enc=%d,%d,%d",
            payload->worldPosX, payload->worldPosY,
            payload->walkDestinationX, payload->walkDestinationY,
            payload->isWalking, payload->isInCar, payload->carFuel, payload->currentAreaId,
            payload->encounterMapId, payload->encounterTableId, payload->encounterEntryId);
    }
}

void MpWorldmapOnDiscovery(const void* data, size_t len)
{
    if (!gMpIsClient || data == nullptr || len < sizeof(NetWorldmapDiscoveryPayload)) {
        return;
    }
    const NetWorldmapDiscoveryPayload* header = (const NetWorldmapDiscoveryPayload*)data;
    const uint8_t* body = (const uint8_t*)data + sizeof(*header);
    size_t bodyLen = len - sizeof(*header);

    if (header->mode == 1) {
        // Full snapshot: packed subtile states, then visited/state area pairs.
        size_t needed = (size_t)header->count * WM_DISCOVERY_BYTES_PER_TILE;
        if (bodyLen < needed) {
            MpLogAlways(MP_LOG_WORLDMAP, "client discovery full truncated tiles=%u have=%zu need=%zu",
                header->count, bodyLen, needed);
            return;
        }
        const uint8_t* areaPairs = body + needed;
        size_t areaBytes = bodyLen - needed;
        int areaCount = (int)(areaBytes / 2);
        if (areaCount > CITY_COUNT) {
            areaCount = CITY_COUNT;
        }
        static uint8_t areaVisited[CITY_COUNT];
        static uint8_t areaState[CITY_COUNT];
        for (int i = 0; i < areaCount; i++) {
            areaVisited[i] = areaPairs[i * 2];
            areaState[i] = areaPairs[i * 2 + 1];
        }
        wmDiscoveryApplyFull(body, header->count, areaVisited, areaState, areaCount);
        wmDiscoveryRebuildLabels();
        MpLog(MP_LOG_WORLDMAP, "client discovery full tiles=%u areas=%d", header->count, areaCount);
    } else if (header->mode == 2) {
        // Incremental: count * 5 bytes (u16 tile, u8 subX, u8 subY, u8 state).
        size_t needed = (size_t)header->count * 5;
        if (bodyLen < needed) {
            MpLogAlways(MP_LOG_WORLDMAP, "client discovery changes truncated count=%u have=%zu need=%zu",
                header->count, bodyLen, needed);
            return;
        }
        static WmDiscoveryChange changes[WM_DISCOVERY_CHANGE_MAX];
        int count = header->count < WM_DISCOVERY_CHANGE_MAX ? header->count : WM_DISCOVERY_CHANGE_MAX;
        const uint8_t* p = body;
        for (int i = 0; i < count; i++) {
            uint16_t tile;
            memcpy(&tile, p, 2);
            p += 2;
            changes[i].tile = tile;
            changes[i].subX = *p++;
            changes[i].subY = *p++;
            changes[i].state = *p++;
        }
        wmDiscoveryApplyChanges(changes, count);
        wmDiscoveryRebuildLabels();
        MpLog(MP_LOG_WORLDMAP, "client discovery changes=%d", count);
    } else {
        MpLogAlways(MP_LOG_WORLDMAP, "client discovery bad mode=%u", header->mode);
        return;
    }

    // The mirror repaints the surface this frame (fog + town list updates).
    gMpWorldmapDirty = true;
}

void MpWorldmapOnExit()
{
    if (!gMpIsClient || !gMpWorldmapActive) {
        return;
    }
    MpLog(MP_LOG_WORLDMAP, "client exit worldmap");
    MpChatAutoOpenCancel();
    gMpWorldmapActive = false;
    gMpWorldmapUiPending = false;
    gMpWorldmapDirty = false;
}

void MpWorldmapMaybeShowUI()
{
    if (!gMpIsClient || !gMpWorldmapActive || !gMpWorldmapUiPending || gMpWorldmapReadOnly) {
        return;
    }
    // Never open the mirror under the synchronized dialogue modal: the mirror
    // pumps MpTick, which would re-enter NetHostService under the dialog. The
    // ENTER handler yanked the dialog, but the modal may still be unwinding —
    // retry on a later top-level tick.
    if (MpDialogAnyModalActive() || gMpModalActive) {
        return;
    }
    gMpWorldmapUiPending = false;
    gMpWorldmapReadOnly = true;
    gMpWorldmapDirty = false;
    MpLog(MP_LOG_WORLDMAP, "client mirror open mode=%d", gMpWorldmapMode);
    // Blocks until WORLDMAP_EXIT (or the MAP_CHANGED that follows it) clears
    // gMpWorldmapActive; the read-only loop pumps MpTick so packets arrive.
    wmWorldMapFunc(gMpWorldmapMode);
    gMpWorldmapReadOnly = false;
    MpLog(MP_LOG_WORLDMAP, "client mirror closed");
}

void MpWorldmapReset()
{
    if (gMpWorldmapActive) {
        MpLog(MP_LOG_WORLDMAP, "reset cleared active session");
    }
    MpChatAutoOpenCancel();
    gMpWorldmapActive = false;
    gMpWorldmapReadOnly = false;
    gMpWorldmapMode = 0;
    gMpWorldmapUiPending = false;
    gMpWorldmapDirty = false;
}

} // namespace fallout
