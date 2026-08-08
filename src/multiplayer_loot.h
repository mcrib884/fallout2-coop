#ifndef FALLOUT_MULTIPLAYER_LOOT_H_
#define FALLOUT_MULTIPLAYER_LOOT_H_

#include <stddef.h>
#include <stdint.h>

namespace fallout {

struct Object;

// --- Host ---
// Start a loot/steal session for a remote player (walks the avatar to the
// target first when needed, then sends the OPEN state).
void MpLootHostStart(uint8_t netId, Object* target, bool isSteal);
// Host-side NET_PKT_LOOT_CMD dispatch (validates peer, applies the op,
// echoes the authoritative state).
void MpLootOnHostPacket(const void* data, size_t dataLength, void* peer);
// Close a host session when the owning player disconnects.
void MpLootHostPlayerDisconnected(uint8_t netId);
// Close every host session (host map change: the targets die with the map).
void MpLootHostCloseAllSessions();

// --- Client ---
// Client-side NET_PKT_LOOT_STATE dispatch (opens the vanilla loot/steal
// window on a local mirror, refreshes it from authoritative echoes, closes
// it on END).
void MpLootOnClientPacket(const void* data, size_t dataLength);
// Close the client session (map change, disconnect). The vanilla window loop
// observes the closed session through MpLootLoopTick and exits cleanly.
void MpLootOnClientReset();
// True while the client's vanilla loot/steal window runs in mp mode.
bool MpLootSessionOpen();
// Per-frame pump for the vanilla loot window loop: returns false when the
// window must close (session ended). Always pumps the network.
bool MpLootLoopTick();
// Consumes the "authoritative echo changed the panes" flag so the vanilla
// loop re-renders both panes and the body.
bool MpLootConsumeDirty();
// The vanilla loot window loop exited: end the session if still open.
void MpLootLoopEnded();
// Client: relay one move (qty signed; + take from target, - plant).
void MpLootClientSendMove(uint32_t pid, int32_t qty);
// Client: relay take-all (loot mode only).
void MpLootClientSendTakeAll();

} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_LOOT_H_ */
