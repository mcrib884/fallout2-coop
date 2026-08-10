#ifndef MULTIPLAYER_PERF_H
#define MULTIPLAYER_PERF_H

#include <stdint.h>

namespace fallout {

// Co-op performance meter: per-frame section timings, FPS and broadcast
// sweep counters. The host's per-frame work (object-state sweeps, profile
// sync, tile delta checks) can dominate frame time; the meter shows where
// the frame actually goes - on screen (small top-left overlay) and in the
// log (one MPPERF line every 5 seconds).
//
// Main-loop sections are measured with MpPerfMark (sequential: each call
// closes the previous section). MpTick internals are measured with
// MpPerfBegin/MpPerfEnd (nested; their time is also part of the MPTICK
// total). Counters are accumulated per frame into 1s windows.

enum MpPerfSection {
    MP_PERF_INPUT = 0,        // inputGetInput (pumps UI + tickers)
    MP_PERF_GLOBAL_SCRIPTS,   // sfall_gl_scr_process_main (host)
    MP_PERF_GAME_KEY,         // gameHandleKey
    MP_PERF_SCRIPT_REQUESTS,  // scriptsHandleRequests
    MP_PERF_MAP_TRANSITION,   // mapHandleTransition
    MP_PERF_MPTICK,           // MpTick total
    MP_PERF_MPTICK_NET,       // NetHostService (receive pump)
    MP_PERF_MPTICK_ASSIGN,    // MpAssignNetIdsToAllObjects
    MP_PERF_MPTICK_OBJECTS,   // MpBroadcastObjectStates
    MP_PERF_MPTICK_PROFILES,  // mpHostSyncProfiles
    MP_PERF_MPTICK_TILES,     // host tile baseline scan
    MP_PERF_MPTICK_PLAYERS,   // MpBroadcastPlayerStates
    MP_PERF_RENDER,           // renderPresent + throttle
    MP_PERF_SECTION_COUNT
};

// Host broadcast-sweep counters.
enum MpPerfCounter {
    MP_PERF_CNT_OBJ_SCANNED = 0, // objects visited by the sweep
    MP_PERF_CNT_OBJ_RECORDS,     // records built this frame (level, not sum)
    MP_PERF_CNT_OBJ_CHANGED,     // deltas sent
    MP_PERF_CNT_OBJ_REM_CMP,     // removal-check comparisons (old x current)
    MP_PERF_CNT_OBJ_PKTS,        // object-state packets sent
    MP_PERF_CNT_PLAYER_PKTS,     // player-state packets sent
    MP_PERF_CNT_COUNT
};

// Start of a frame. Also opens MP_PERF_INPUT as the active section.
void MpPerfFrameStart();

// Sequential section boundary: closes the active section, opens [section].
void MpPerfMark(int section);

// Nested section begin/end (MpTick internals).
void MpPerfBegin(int section);
void MpPerfEnd(int section);

// Closes the active section and the frame; folds the frame into the 1s
// window (FPS, average/max frame time, section sums, counters).
void MpPerfFrameEnd();

// Once per frame: refreshes the overlay window (1 Hz) and writes the
// periodic log line (every 5 s). No-ops when disabled (overlay) / not in a
// session (log).
void MpPerfTick();

// Counter accumulation. Delta counters add; the RECORDS level uses
// MpPerfSetCounter.
void MpPerfAddCounter(int counter, uint32_t delta);
void MpPerfSetCounter(int counter, uint32_t value);

bool MpPerfIsEnabled();
void MpPerfSetEnabled(bool enabled);

} // namespace fallout

#endif // MULTIPLAYER_PERF_H
