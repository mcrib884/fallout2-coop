#ifndef MULTIPLAYER_DEBUG_H
#define MULTIPLAYER_DEBUG_H

#include <stdint.h>

// ENet forward declarations (same pattern as net.h): the game's peer type.
struct _ENetPeer;
typedef struct _ENetPeer ENetPeer;

namespace fallout {

struct Object;

enum MpDebugCheatFlags : uint32_t {
    MP_DEBUG_CHEAT_GOD_MODE = 1u << 0,
    MP_DEBUG_CHEAT_INFINITE_AP = 1u << 1,
    MP_DEBUG_CHEAT_INFINITE_AMMO = 1u << 2,
    MP_DEBUG_CHEAT_UNLIMITED_CARRY = 1u << 3,
    MP_DEBUG_CHEAT_ALWAYS_SUCCEED = 1u << 4,
    MP_DEBUG_CHEAT_NO_RANDOM_ENCOUNTERS = 1u << 5,
    MP_DEBUG_CHEAT_INSTA_KILL = 1u << 6,
};

constexpr uint32_t MP_DEBUG_CHEAT_ALL = MP_DEBUG_CHEAT_GOD_MODE
    | MP_DEBUG_CHEAT_INFINITE_AP
    | MP_DEBUG_CHEAT_INFINITE_AMMO
    | MP_DEBUG_CHEAT_UNLIMITED_CARRY
    | MP_DEBUG_CHEAT_ALWAYS_SUCCEED
    | MP_DEBUG_CHEAT_NO_RANDOM_ENCOUNTERS
    | MP_DEBUG_CHEAT_INSTA_KILL;

// Per-player co-op debug menu (F11): money, XP, HP, skill points, level,
// skills, SPECIAL stats and perks — every player edits their own sheet.
// Persistent fields (stats/skills/perks/money/xp) ride the profile channel;
// only the volatile runtime HP goes through a host command.
void MpDebugMenuShow();

// Client: request the host to heal this player's avatar (value <= 0 = full).
void MpDebugSendHeal(int value);

// Host: apply a debug heal to a player's avatar; revives a downed player.
void MpDebugApplyHeal(Object* critter, int value);

// Client: request the host to refill this player's avatar action points.
void MpDebugSendApRefill();

// Host: apply a debug AP refill to a player's avatar.
void MpDebugApplyApRefill(Object* critter);

// Returns the active cheat flags for a local player or a host-side remote
// player. Used by the engine's authoritative gameplay paths.
uint32_t MpDebugCheatFlagsFor(const Object* critter);
bool MpDebugCheatEnabled(const Object* critter, uint32_t flag);

// Applies active runtime cheats and relays changed client flags to the host.
void MpDebugCheatsTick();

// Host: flip the session's client-cheat policy (only the host may use cheats
// when disabled) and broadcast the new policy to every connected client.
void MpDebugToggleClientCheats();

// Client: the host's cheat policy arrived (NET_PKT_CHEAT_POLICY). When
// disabled, clears the local cheat flags so the menu reads OFF and the
// local effects stop.
void MpDebugSetClientCheatsEnabled(bool enabled);

// Returns the session's client-cheat policy (true = clients may cheat).
bool MpDebugClientCheatsEnabled();

// Send the current cheat policy to one peer (join-time seeding).
void MpDebugSendCheatPolicyTo(ENetPeer* peer);

// Per-machine skin override: -1 = none (the config default / armor decides),
// otherwise the picked critter model index (art list).
int MpDebugSkinOverrideModel();

// Apply a picked critter model to the local dude: writes the choice into the
// dude's proto fid (the profile capture reads it, so the periodic profile
// sync propagates it to every machine) and updates the visible sprite.
void MpDebugApplyModel(int modelIndex);

// Restore the vanilla look: clears the override and rebuilds the dude's fid
// from the config's per-gender default model (armor decides again).
void MpDebugRestoreSkin();

// Opens the skin picker: every critter model from the runtime art list,
// grouped by its two-letter species/gender prefix, per-page navigation,
// select-on-click and a restore-vanilla button.
void MpDebugModelPickerShow();

} // namespace fallout

#endif
