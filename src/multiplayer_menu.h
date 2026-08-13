#ifndef FALLOUT_MULTIPLAYER_MENU_H_
#define FALLOUT_MULTIPLAYER_MENU_H_

#include <stdint.h>

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

// Shared join path used by the address join and the LAN browser: in-game
// joins back the session with the hidden co-op save first; main-menu joins
// ask for a fresh character or an existing save via the pending-client
// globals. Returns 1 when a join was started, 0 on cancel/failure.
int MpJoinInitiate(const char* address, uint16_t port, const char* password);

} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_MENU_H_ */