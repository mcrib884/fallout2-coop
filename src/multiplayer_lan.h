#ifndef MULTIPLAYER_LAN_H
#define MULTIPLAYER_LAN_H

namespace fallout {

// LAN browser panel size — the browser opens as a NARROW window beside the
// multiplayer dialog / F11 menu (callers position it at the screen's right
// edge or next to their own window).
// Side-panel dimensions. 172 wide so the panel fits at the screen's right
// edge (x = screenGetWidth()-172-8 = 460) next to BOTH the main-menu dialog
// (180..460) and the F11 window moved to x=0 (0..460) — no overlap.
constexpr int kLanBrowserWidth = 172;
constexpr int kLanBrowserHeight = 300;

// LAN browser modal — a separate window shown BESIDE the multiplayer menus
// (main-menu dialog and F11 CO-OP SETTINGS), opened from a button inside
// them. Scans the local network (and loopback) for co-op hosts and joins the
// selected one. Returns nonzero when a client session was started — the
// caller must then leave the menu stack so the game takes over; returns 0 on
// cancel.
int MpLanBrowserShow(int x, int y);
bool MpLanBrowserIsOpen();

} // namespace fallout

#endif /* MULTIPLAYER_LAN_H */
