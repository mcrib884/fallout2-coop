#ifndef MULTIPLAYER_DEBUG_H
#define MULTIPLAYER_DEBUG_H

#include <stdint.h>

namespace fallout {

struct Object;

// Per-player co-op debug menu (F11): money, XP, HP, skill points, level,
// skills, SPECIAL stats and perks — every player edits their own sheet.
// Persistent fields (stats/skills/perks/money/xp) ride the profile channel;
// only the volatile runtime HP goes through a host command.
void MpDebugMenuShow();

// Client: request the host to heal this player's avatar (value <= 0 = full).
void MpDebugSendHeal(int value);

// Host: apply a debug heal to a player's avatar; revives a downed player.
void MpDebugApplyHeal(Object* critter, int value);

} // namespace fallout

#endif
