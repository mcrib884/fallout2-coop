#ifndef MULTIPLAYER_LOG_H
#define MULTIPLAYER_LOG_H

#include <stdarg.h>
#include <stdint.h>

// Categorized co-op logging gate.
//
// Every co-op log line belongs to one category. A global mask enables or
// silences whole areas at once, so a debugging session can focus on exactly
// one subsystem (combat, chat, sync, ...) without drowning in the others.
//
// Two tiers:
//   - MpLog(cat, ...)       gateable diagnostic. Printed only when the
//                           category bit is set in the mask.
//   - MpLogAlways(cat, ...) mandatory house-rule line (entry/exit of
//                           lifecycle, rejections, kicks, disconnects,
//                           validation failures). NEVER gated - the
//                           AGENTS.md logging rules stay binding.
//
// Line format: "MP[TAG]: message" where TAG is the category tag (e.g.
// MP[COMBAT]: ...). Grep with MP\[TAG\] to filter one area.
//
// Defaults: every category ON (preserves the existing always-log behavior).
// fallout2.cfg [debug] section overrides:
//   coop_log_all=0                     - start from everything off
//   coop_log_<category>=1              - turn one area on (names below)
// Names: lifecycle, handshake, net, sync, profile, model, combat, chat,
// ui, input, object, stats, script, vote, dialog, loot, movie, worldmap,
// misc.

namespace fallout {

enum MpLogCategory : uint32_t {
    MP_LOG_LIFECYCLE = 1u << 0,  // session start/stop, host/client connect
    MP_LOG_HANDSHAKE = 1u << 1,  // HELLO -> profile -> WELCOME -> joined -> sync
    MP_LOG_NET = 1u << 2,        // packet send/receive/dispatch, ENet events
    MP_LOG_SYNC = 1u << 3,       // state broadcast/apply, tile sync, map sync
    MP_LOG_PROFILE = 1u << 4,    // profile capture/validate/apply, inventory nodes
    MP_LOG_MODEL = 1u << 5,      // model install/registry
    MP_LOG_COMBAT = 1u << 6,     // combat hooks, turn relay, damage, downed/revive
    MP_LOG_CHAT = 1u << 7,       // chat window, messages, floating text
    MP_LOG_UI = 1u << 8,         // menus, modals, nametags, pickers
    MP_LOG_INPUT = 1u << 9,      // keys, text input, mouse
    MP_LOG_OBJECT = 1u << 10,    // netId mapping, object registry
    MP_LOG_STATS = 1u << 11,     // stat/skill/perk/SPECIAL changes
    MP_LOG_SCRIPT = 1u << 12,    // interpreter/script hooks
    MP_LOG_VOTE = 1u << 13,      // vote requests/results (map changes, kicks)
    MP_LOG_DIALOG = 1u << 14,    // synchronized dialogue + barter
    MP_LOG_LOOT = 1u << 15,      // loot windows, pickups, steal/take-all
    MP_LOG_MOVIE = 1u << 16,     // synchronized movie playback
    MP_LOG_WORLDMAP = 1u << 17,  // worldmap travel: entry/exit, state sync, mirror
    MP_LOG_MISC = 1u << 18,      // everything else
    MP_LOG_ALL = 0xFFFFFFFFu,
};

// Short tag for the log line prefix ("COMBAT", "CHAT", ...).
const char* MpLogCategoryTag(MpLogCategory cat);

// Gateable diagnostic: prints "MP[TAG]: ..." only when the category is on.
void MpLog(MpLogCategory cat, const char* format, ...);

// Mandatory line: same format, never gated (house-rule logging).
void MpLogAlways(MpLogCategory cat, const char* format, ...);

// Reads [debug] coop_log_* keys from fallout2.cfg into the mask.
void MpLogInit();

bool MpLogEnabled(MpLogCategory cat);
void MpLogSetEnabled(MpLogCategory cat, bool enabled);
uint32_t MpLogMask();
void MpLogSetMask(uint32_t mask);

} // namespace fallout

#endif // MULTIPLAYER_LOG_H
