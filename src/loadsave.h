#ifndef LOAD_SAVE_GAME_H
#define LOAD_SAVE_GAME_H

namespace fallout {

typedef enum LoadSaveMode {
    // Special case - loading game from main menu.
    LOAD_SAVE_MODE_FROM_MAIN_MENU,

    // Normal (full-screen) save/load screen.
    LOAD_SAVE_MODE_NORMAL,

    // Quick load/save.
    LOAD_SAVE_MODE_QUICK,
} LoadSaveMode;

void _InitLoadSave();
void _ResetLoadSave();
int lsgSaveGame(int mode);
int lsgQuickSaveGame();
// Co-op session framework: every session (host or join) is backed by a save
// in a reserved hidden slot — one past the UI's page range, so player saves
// are never touched and the slot never appears in the save/load pages.
// In-game flows save the current game here first; main-menu flows pick an
// existing save or create a new character, save it here, and use that.
// Future mid-session multiplayer saves will use the same helpers.
int lsgGetCoopSaveSlot();
// The slot most recently loaded (SP load or coop load). A client that joined
// by loading one of its own saves writes back into that same slot.
int lsgGetLastLoadedSlot();
// First UI-range slot (01..100) with no SAVE.DAT yet, or -1 when all taken.
// Co-op new-game sessions save their session character into this slot.
int lsgFindNextEmptySlot();
int lsgQuickSaveGameCoop();
int lsgLoadGameCoop();
int lsgLoadGame(int mode);
void lsgDevSetLoadGameSlot(int slot);
int lsgGetTotalSlotCount();
bool _isLoadingGame();
void lsgInit();
int MapDirErase(const char* path, const char* extension);
int _MapDirEraseFile_(const char* relativePath, const char* fileName);

} // namespace fallout

#endif /* LOAD_SAVE_GAME_H */
