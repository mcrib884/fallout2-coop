#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "net.h"

namespace fallout {

// Co-op worldmap travel: the host drives the vanilla worldmap modal and the
// clients render a read-only mirror of it (one shared party position, host
// authoritative). Combat is the only entry blocker; dialog/barter/loot
// modals are yanked (force-closed) when travel starts.
//
// Host flow:  map.cc / scripts.cc wrap wmWorldMap()/wmTownMap() with
//             MpWorldmapHostEntered(mode) / MpWorldmapHostLeft(); the worldmap
//             loop broadcasts NET_PKT_WORLDMAP_STATE while walking and routes
//             every internal map load through MpWorldmapHostLoadMap, which
//             runs the same pipeline as map.cc (prepare / load / finish /
//             MAP_CHANGED / FULL_SYNC / deferred map-enter script).
// Client flow: NET_PKT_WORLDMAP_ENTER sets the pending flag; the mirror
//             opens on the next top-level MpTick via MpWorldmapMaybeShowUI
//             (never from inside a NetHostService callback) and runs the
//             vanilla loop read-only. STATE applies onto the local WmGenData
//             (world pos, destination, walking, car). EXIT (or the map change
//             that follows it) closes the mirror.

// True while the host is inside the worldmap modal, or a client's mirror is
// up (or queued to open).
extern bool gMpWorldmapActive;
// Client-only: the mirror modal is currently running (read-only rendering,
// no travel input, no walking, no encounters).
extern bool gMpWorldmapReadOnly;
// 0 = world map (wmWorldMapFunc(0)), 1 = town map (wmWorldMapFunc(1)).
extern int gMpWorldmapMode;
// Client-only: a WORLDMAP_STATE packet landed while the mirror is up; the
// worldmap loop must repaint the surface (set by MpWorldmapOnState, consumed
// by wmWorldMapFunc).
extern bool gMpWorldmapDirty;

// --- host ---

// Called right before the host enters the worldmap modal (from map.cc and
// scripts.cc). Broadcasts WORLDMAP_ENTER and marks the session as traveling.
void MpWorldmapHostEntered(int mode);

// Called right after the host leaves the worldmap without loading a map
// (modal closed by quit/failure). Broadcasts WORLDMAP_EXIT. Idempotent: no-op
// if the host already left via MpWorldmapHostLoadMap.
void MpWorldmapHostLeft();

// Host worldmap-loaded a map (encounter, city entry, forced/Horrigan):
// closes the mirror (EXIT), then runs the map.cc pipeline (prepare / load /
// finish / MAP_CHANGED / FULL_SYNC / deferred map-enter + random encounter).
void MpWorldmapHostLoadMap(int mapId);

// Broadcasts the authoritative party state (throttled to ~50 ms; force
// bypasses the throttle for destination changes).
void MpWorldmapBroadcastState(bool force = false);

// Ships ENTER + current STATE to one joining peer (mid-travel join).
void MpWorldmapSendStateToPeer(ENetPeer* peer);

// --- client ---

void MpWorldmapOnEnter(const NetWorldmapEnterPayload* payload);
void MpWorldmapOnState(const NetWorldmapStatePayload* payload);
// Client: applied host discovery state (subtile fog + area visited/known)
// onto the local worldmap; repaints the mirror.
void MpWorldmapOnDiscovery(const void* data, size_t len);
// Client: consume the stashed encounter triple (see the stash comment in
// multiplayer_worldmap.cc). Resets the stash so it fires once per session.
void mpWorldmapClientTakeEncounterState(int* mapId, int* tableId, int* entryId);
void MpWorldmapOnExit();

// Opens the client mirror modal on the next top-level MpTick. Blocks while
// the mirror is up (the loop pumps MpTick).
void MpWorldmapMaybeShowUI();

// Session teardown (MpReset).
void MpWorldmapReset();

} // namespace fallout
