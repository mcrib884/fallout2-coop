#ifndef GAME_DIALOG_H
#define GAME_DIALOG_H

#include "interpreter.h"
#include "obj_types.h"

namespace fallout {

extern Object* gGameDialogSpeaker;
extern bool gGameDialogSpeakerIsPartyMember;
extern int gGameDialogHeadFid;
extern int gGameDialogReactionOrFidget;
extern int gGameDialogSid;

int gameDialogInit();
int gameDialogReset();
int gameDialogExit();
bool _gdialogActive();
void gameDialogEnter(Object* speaker, int mode);
void _gdialogSystemEnter();
void gameDialogStartLips(const char* audioFileName);
// Co-op: true while lip-sync speech is active; the host reads the current
// audio base name (lipsLoad stores it in gLipsData.file_name) to ship it to
// clients so they can play voice + phonemes on their head portrait.
bool gameDialogIsLipSyncActive();
const char* gameDialogGetLipFileName();
int gameDialogEnable();
int gameDialogDisable();
int _gdialogInitFromScript(int headFid, int reaction);
int _gdialogExitFromScript();
// Co-op host-join: the blocking vanilla dialogue modal (the script is parked;
// the modal overlays an active session and pumps MpTick itself).
int gameDialogProcessUI();
// Co-op host-join: create/destroy the vanilla dialogue screen (background,
// head portrait, main window with the red buttons) around a parked session.
int _gdCreateHeadWindow();
void _gdDestroyHeadWindow();
// Co-op host-join: re-inject the current session node (reply + options with
// resolved text) into the vanilla file-statics and re-render the head
// portrait. The director capture consumed the script's option entries, so
// without this the join modal's first frame renders no options.
void gameDialogCoopHostJoinShowNode(const char* replyText, const int* reactions, const char* const* texts, int optionCount, const int* procs);
void gameDialogSetBackground(int background);
void gameDialogRenderSupplementaryMessage(const char* msg);
int _gdialogStart();
int _gdialogSayMessage();
int gameDialogAddMessageOptionWithProcIdentifier(int messageListId, int messageId, const char* procName, int reaction);
int gameDialogAddTextOptionWithProcIdentifier(int messageListId, const char* text, const char* procName, int reaction);
int gameDialogAddMessageOptionWithProc(int messageListId, int messageId, int proc, int reaction);
int gameDialogAddTextOptionWithProc(int messageListId, const char* text, int proc, int reaction);
int gameDialogSetMessageReply(Program* program, int messageListId, int messageId);
int gameDialogSetTextReply(Program* program, int messageListId, const char* text);
int _gdialogGo();
void _gdialogUpdatePartyStatus();
void _talk_to_critter_reacts(int reaction);
int gameDialogGetBarterModifier();
void gameDialogSetBarterModifier(int modifier);
int gameDialogBarter(int modifier);
void gameDialogEndBarter();
bool gameDialogIsBarterWindowExpanded();
int gameDialogGetWindow();
int gameDialogGetBackgroundWindow();
void gameDialogSetPartyMemberCcMsgIds(int pid, int startMsgId, int endMsgId);
void gameDialogResetPartyMemberCcMsgIds();
int gameDialogGetReplyWindow();
int gameDialogGetOptionsWindow();
// Co-op: run the vanilla option-proc path for a resolved vote (host only).
int gameDialogChooseOption(int optionIndex);
// Co-op: director-host choice execution (no window work) — returns -1 when
// the reply proc built no next node (conversation over).
int MpDialogDirectorProcessChoice(int optionIndex);
// Co-op: full vanilla teardown of the parked director-host dialogue state.
void MpDialogDirectorFinishDialogue();
// Co-op: true while a director-host session parks this program — the script's
// trailing exit_proc/stop_proc must not kill it before the reply procs ran.
bool mpDialogIsDirectorReplyProgram(Program* program);

// --- Co-op client dialogue screen (multiplayer_dialog.cc drives these) ---
// The client never runs dialogue scripts; the host feeds it resolved node
// text, and these helpers render it with the vanilla dialogue windows (head
// portrait, reply box, option buttons) plus a small participant panel.

// Opens the vanilla dialogue screen for the client's mp session. Creates the
// background, main dialog window (with the barter/review buttons), the head
// portrait, and the reply/options windows. Returns 0 on success.
int gameDialogCoopOpen(int headFid, int reaction, Object* speaker);
// Replaces the current node: destroys old option buttons, injects the reply
// and option texts (numbered like the vanilla screen), re-renders. `procs`
// carries the per-option script procedures (host-side; pass nullptr on the
// client — the client never runs reply procs).
void gameDialogCoopApplyNode(const char* replyText, const int* reactions, const char* const* texts, int optionCount, const int* procs);
// Closes the vanilla dialogue screen and restores the game state.
void gameDialogCoopClose();
bool gameDialogCoopIsOpen();
// Hide/show the whole vanilla dialogue screen (used while bartering).
void gameDialogCoopHideVanilla();
void gameDialogCoopShowVanilla();
// Co-op barter window swap: destroy the dialogue window / create the dedicated
// barter window (barter.frm|trade.frm) without the vanilla hidden tables.
void gameDialogCoopDestroyDialogueWindow();
int gameDialogCoopCreateBarterWindow();
void gameDialogCoopDestroyBarterWindow();
void gameDialogCoopRecreateDialogueWindow();
// Hide/show only the reply + options windows (vanilla barter gdHide parity —
// the background, head and red buttons stay visible over the trade screen).
void gameDialogCoopHideDialogue();
void gameDialogCoopShowDialogue();
// Hover highlight for the vanilla option buttons (client modal feeds the
// window-manager hover keycodes 1200+/1300+ into these).
void gameDialogCoopOptionHover(int optionIndex);
void gameDialogCoopOptionHoverExit(int optionIndex);

} // namespace fallout

#endif /* GAME_DIALOG_H */
