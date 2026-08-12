#ifndef MULTIPLAYER_CHAT_H
#define MULTIPLAYER_CHAT_H

#include <stdint.h>

namespace fallout {

// Open the chat modal (T key). Blocks until closed, pumping the network
// while it is up (MpTick), so incoming lines appear live. Returns 1 when a
// message was sent, 0 when it was dismissed without sending.
int MpChatShow();

// Combat-log mirror: every line the display monitor actually shows is
// appended to the chat history verbatim (1:1). Called from
// displayMonitorAddMessage on the same path that draws the green monitor,
// so the chat always shows exactly what the combat log shows — including
// lines routed to this player only and the authoritative echoes of its own
// suppressed predictions.
void MpChatAppendCombatLine(const char* text);

// Clear the chat history together with the display monitor (map changes,
// new game, ...).
void MpChatReset();

// A user message from the local player. Appends to the local chat, floats
// above the local critter, and relays to the session: the host forwards it
// to every other connected player, the client sends it to the host. Outside
// co-op the message only shows locally.
void MpChatSendMessage(const char* text);

// Host side: a validated chat message from a client peer (or the host's own
// send). Appends locally, floats above the sender's critter, and forwards
// to every other connected player. The sender never gets an echo.
void MpChatHostOnMessage(uint8_t senderNetId, const char* text);

// Client side: the host relayed another player's message. Appends locally
// and floats above the sender's critter.
void MpChatClientOnIncoming(uint8_t senderNetId, const char* text);

} // namespace fallout

#endif /* MULTIPLAYER_CHAT_H */
