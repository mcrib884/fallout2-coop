#ifndef FALLOUT_MULTIPLAYER_MENU_H_
#define FALLOUT_MULTIPLAYER_MENU_H_

namespace fallout {

// Co-op multiplayer launcher dialog (root menu: Host / Join / Cancel).
// Returns 1 if a flow was started (host or join), 0 on cancel.
int MpMenuShow();

// Host flow: New Game / Load Save / Cancel. Returns 1 on New Game,
// 2 on Load Save, 0 on cancel.
int MpHostFlowShow();

// Join flow: IP entry + Connect/Cancel. Returns 1 on connect initiated,
// 0 on cancel.
int MpJoinFlowShow();

} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_MENU_H_ */