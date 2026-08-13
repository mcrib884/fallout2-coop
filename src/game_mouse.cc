#include "game_mouse.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#if __APPLE__
#include <TargetConditionals.h>
#endif

#include <algorithm>

#include "actions.h"
#include "animation.h"
#include "art.h"
#include "color.h"
#include "combat.h"
#include "content_config.h"
#include "critter.h"
#include "debug.h"
#include "draw.h"
#include "game.h"
#include "game_sound.h"
#include "input.h"
#include "interface.h"
#include "item.h"
#include "kb.h"
#include "map_edge.h"
#include "mouse.h"
#include "multiplayer.h"
#include "multiplayer_combat.h"
#include "object.h"
#include "party_member.h"
#include "proto.h"
#include "proto_instance.h"
#include "settings.h"
#include "skill.h"
#include "skilldex.h"
#include "svga.h"
#include "text_font.h"
#include "tile.h"
#include "window_manager.h"
#include "multiplayer_log.h"

namespace fallout {

typedef enum ScrollableDirections {
    SCROLLABLE_W = 0x01,
    SCROLLABLE_E = 0x02,
    SCROLLABLE_N = 0x04,
    SCROLLABLE_S = 0x08,
} ScrollableDirections;

static constexpr int REFRESH_BOUNCING_CURSOR = 0x01;
static constexpr int REFRESH_HEX_CURSOR = 0x02;
static constexpr int REFRESH_BOTH_CURSORS = REFRESH_BOUNCING_CURSOR | REFRESH_HEX_CURSOR;

// 0x518BF8 gmouse_initialized
static bool gGameMouseInitialized = false;

// 0x518BFC gmouse_enabled
static bool _gmouse_enabled = false;

// 0x518C00 gmouse_mapper_mode
static int _gmouse_mapper_mode = 0;

// 0x518C04 gmouse_click_to_scroll
static bool _gmouse_click_to_scroll = false;

// 0x518C08 gmouse_scrolling_enabled
static bool _gmouse_scrolling_enabled = true;

// 0x518C0C gmouse_current_cursor
static int gGameMouseCursor = MOUSE_CURSOR_NONE;

// 0x518C10 gmouse_current_cursor_key
static CacheEntry* gGameMouseCursorFrmHandle = INVALID_CACHE_ENTRY;

// 0x518C14 gmouse_cursor_nums
static const int gGameMouseCursorFrmIds[MOUSE_CURSOR_TYPE_COUNT] = {
    266,
    267,
    268,
    269,
    270,
    271,
    272,
    273,
    274,
    275,
    276,
    277,
    330,
    331,
    329,
    328,
    332,
    334,
    333,
    335,
    279,
    280,
    281,
    293,
    310,
    278,
    295,
};

// 0x518C80 gmouse_3d_initialized
static bool gGameMouseObjectsInitialized = false;

// 0x518C84 gmouse_3d_hover_test
static bool _gmouse_3d_hover_test = false;

// 0x518C88 gmouse_3d_last_move_time
static unsigned int _gmouse_3d_last_move_time = 0;

// actmenu.frm
// 0x518C8C gmouse_3d_menu_frame
static Art* gGameMouseActionMenuFrm = nullptr;

// 0x518C90 gmouse_3d_menu_frame_key
static CacheEntry* gGameMouseActionMenuFrmHandle = INVALID_CACHE_ENTRY;

// 0x518C94 gmouse_3d_menu_frame_width
static int gGameMouseActionMenuFrmWidth = 0;

// 0x518C98 gmouse_3d_menu_frame_height
static int gGameMouseActionMenuFrmHeight = 0;

// 0x518C9C gmouse_3d_menu_frame_size
static int gGameMouseActionMenuFrmDataSize = 0;

// 0x518CA0 gmouse_3d_menu_frame_hot_x
static int _gmouse_3d_menu_frame_hot_x = 0;

// 0x518CA4 gmouse_3d_menu_frame_hot_y
static int _gmouse_3d_menu_frame_hot_y = 0;

// 0x518CA8 gmouse_3d_menu_frame_data
static unsigned char* gGameMouseActionMenuFrmData = nullptr;

// actpick.frm
// 0x518CAC gmouse_3d_pick_frame
static Art* gGameMouseActionPickFrm = nullptr;

// 0x518CB0 gmouse_3d_pick_frame_key
static CacheEntry* gGameMouseActionPickFrmHandle = INVALID_CACHE_ENTRY;

// 0x518CB4 gmouse_3d_pick_frame_width
static int gGameMouseActionPickFrmWidth = 0;

// 0x518CB8 gmouse_3d_pick_frame_height
static int gGameMouseActionPickFrmHeight = 0;

// 0x518CBC gmouse_3d_pick_frame_size
static int gGameMouseActionPickFrmDataSize = 0;

// 0x518CC0 gmouse_3d_pick_frame_hot_x
static int _gmouse_3d_pick_frame_hot_x = 0;

// 0x518CC4 gmouse_3d_pick_frame_hot_y
static int _gmouse_3d_pick_frame_hot_y = 0;

// 0x518CC8 gmouse_3d_pick_frame_data
static unsigned char* gGameMouseActionPickFrmData = nullptr;

// acttohit.frm
// 0x518CCC gmouse_3d_to_hit_frame
static Art* gGameMouseActionHitFrm = nullptr;

// 0x518CD0 gmouse_3d_to_hit_frame_key
static CacheEntry* gGameMouseActionHitFrmHandle = INVALID_CACHE_ENTRY;

// 0x518CD4 gmouse_3d_to_hit_frame_width
static int gGameMouseActionHitFrmWidth = 0;

// 0x518CD8 gmouse_3d_to_hit_frame_height
static int gGameMouseActionHitFrmHeight = 0;

// 0x518CDC gmouse_3d_to_hit_frame_size
static int gGameMouseActionHitFrmDataSize = 0;

// 0x518CE0 gmouse_3d_to_hit_frame_data
static unsigned char* gGameMouseActionHitFrmData = nullptr;

// blank.frm
// 0x518CE4 gmouse_3d_hex_base_frame
static Art* gGameMouseBouncingCursorFrm = nullptr;

// 0x518CE8 gmouse_3d_hex_base_frame_key
static CacheEntry* gGameMouseBouncingCursorFrmHandle = INVALID_CACHE_ENTRY;

// 0x518CEC gmouse_3d_hex_base_frame_width
static int gGameMouseBouncingCursorFrmWidth = 0;

// 0x518CF0 gmouse_3d_hex_base_frame_height
static int gGameMouseBouncingCursorFrmHeight = 0;

// 0x518CF4 gmouse_3d_hex_base_frame_size
static int gGameMouseBouncingCursorFrmDataSize = 0;

// 0x518CF8 gmouse_3d_hex_base_frame_data
static unsigned char* gGameMouseBouncingCursorFrmData = nullptr;

// msef000.frm
// 0x518CFC gmouse_3d_hex_frame
static Art* gGameMouseHexCursorFrm = nullptr;

// 0x518D00 gmouse_3d_hex_frame_key
static CacheEntry* gGameMouseHexCursorFrmHandle = INVALID_CACHE_ENTRY;

// 0x518D04 gmouse_3d_hex_frame_width
static int gGameMouseHexCursorFrmWidth = 0;

// 0x518D08 gmouse_3d_hex_frame_height
static int gGameMouseHexCursorHeight = 0;

// 0x518D0C gmouse_3d_hex_frame_size
static int gGameMouseHexCursorDataSize = 0;

// 0x518D10 gmouse_3d_hex_frame_data
static unsigned char* gGameMouseHexCursorFrmData = nullptr;

// 0x518D14 gmouse_3d_menu_available_actions
static unsigned char gGameMouseActionMenuItemsLength = 0;

// 0x518D18 gmouse_3d_menu_actions_start
static unsigned char* _gmouse_3d_menu_actions_start = nullptr;

// 0x518D1C gmouse_3d_menu_current_action_index
static unsigned char gGameMouseActionMenuHighlightedItemIndex = 0;

// 0x518D1E gmouse_3d_action_nums
static const short gGameMouseActionMenuItemFrmIds[GAME_MOUSE_ACTION_MENU_ITEM_COUNT] = {
    253, // Cancel
    255, // Drop
    257, // Inventory
    259, // Look
    261, // Rotate
    263, // Talk
    265, // Use/Get
    302, // Unload
    304, // Skill
    435, // Push
};

// 0x518D34 gmouse_3d_modes_enabled
static int _gmouse_3d_modes_enabled = 1;

// 0x518D38 gmouse_3d_current_mode
static int gGameMouseMode = GAME_MOUSE_MODE_MOVE;

// 0x518D3C gmouse_3d_mode_nums
static int gGameMouseModeFrmIds[GAME_MOUSE_MODE_COUNT] = {
    249,
    250,
    251,
    293,
    293,
    293,
    293,
    293,
    293,
    293,
    293,
};

// 0x518D68 gmouse_skill_table
static const Skill gGameMouseModeSkills[GAME_MOUSE_MODE_SKILL_COUNT] = {
    SKILL_FIRST_AID,
    SKILL_DOCTOR,
    SKILL_LOCKPICK,
    SKILL_STEAL,
    SKILL_TRAPS,
    SKILL_SCIENCE,
    SKILL_REPAIR,
};

// 0x518D84 gmouse_wait_cursor_frame
static int gGameMouseAnimatedCursorNextFrame = 0;

// 0x518D88 gmouse_wait_cursor_time
static unsigned int gGameMouseAnimatedCursorLastUpdateTimestamp = 0;

// 0x518D8C gmouse_bk_last_cursor
static int _gmouse_bk_last_cursor = -1;

// 0x518D90 gmouse_3d_item_highlight
static bool gGameMouseItemHighlightEnabled = true;

// 0x518D94 outlined_object
static Object* gGameMouseHighlightedItem = nullptr;

// 0x518D98 gmouse_clicked_on_edge
bool _gmouse_clicked_on_edge = false;

// 0x518D9C mouse_flat_tile
static int dword_518D9C = -1;

// 0x596C3C gmouse_3d_menu_frame_actions
static int gGameMouseActionMenuItems[GAME_MOUSE_ACTION_MENU_ITEM_COUNT];

// 0x596C64 gmouse_3d_last_mouse_x
static int gGameMouseLastX;

// 0x596C68 gmouse_3d_last_mouse_y
static int gGameMouseLastY;

// blank.frm
// 0x596C6C obj_mouse
Object* gGameMouseBouncingCursor;

// msef000.frm
// 0x596C70 obj_mouse_flat
Object* gGameMouseHexCursor;

// 0x596C74
static Object* gGameMousePointedObject;

static void _gmouse_3d_enable_modes();
static int gameMouseSetBouncingCursorFid(int fid);
static int gameMouseRenderAccuracy(const char* string, int color);
static int gameMouseRenderActionPoints(const char* string, int color);
static int gameMouseObjectsInit();
static int gameMouseObjectsReset();
static void gameMouseObjectsFree();
static int gameMouseActionMenuInit();
static void gameMouseActionMenuFree();
static int gmouse_3d_set_flat_fid(int fid, Rect* rect);
static int gameMouseUpdateHexCursorFid(Rect* rect);
static int _gmouse_3d_move_to(int x, int y, int elevation, Rect* rect);
static int gameMouseHandleScrolling(int x, int y, int cursor);
static int objectIsDoor(Object* object);
static bool gameMouseClickOnInterfaceBar();
static bool gameMouseLongPressUsesLootActionForCritter(Object* object);

static void customMouseModeFrmsInit();

static bool gameMouseSendPlayerAction(uint8_t action, Object* target, uint8_t skill = 0xFF,
    int32_t extraPid = -1)
{
    if (!gMpActive || !gMpIsClient || target == nullptr) {
        return false;
    }

    uint32_t targetNetId = MpGetObjNetId(target);
    if (targetNetId == 0) {
        uint8_t localNetId = gMpSession.localNetId;
        MultiplayerPlayer* localPlayer = localNetId > 0 && localNetId <= NET_MAX_PLAYERS
            ? &gMpSession.players[localNetId - 1]
            : nullptr;
        MpLogAlways(MP_LOG_NET, "failed action=%d obj=%p pid=0x%X localSlotNetId=%u localObjNetId=%u",
            action, (void*)target, target->pid,
            localPlayer != nullptr ? localPlayer->netId : 0,
            localPlayer != nullptr ? localPlayer->objNetId : 0);
        return false;
    }

    // USE_ITEM_ON rides the item's pid in the tile field (DROP precedent);
    // every other action sends tile=-1.
    int32_t tile = extraPid >= 0 ? extraPid : -1;
    MpSendPlayerAction(action, targetNetId, tile, target->elevation, skill);
    return true;
}

static bool gameMouseLongPressUsesLootActionForCritter(Object* object)
{
    constexpr int kExpandedActionMenuFrmHeight = 302;

    return settings.qol.party_trade_from_menu
        && gGameMouseActionMenuFrmHeight >= kExpandedActionMenuFrmHeight // make sure we can accommodate 7 items in the action menu
        && object != nullptr
        && object != gDude
        && !isInCombat()
        && objectIsPartyMember(object)
        && _obj_action_can_talk_to(object);
}

// 0x44B2B0 gmouse_init
int gameMouseInit()
{
    if (gGameMouseInitialized) {
        return -1;
    }

    if (gameMouseObjectsInit() != 0) {
        return -1;
    }

    gGameMouseInitialized = true;
    _gmouse_enabled = true;

    gameMouseSetCursor(MOUSE_CURSOR_ARROW);

    // SFALL
    customMouseModeFrmsInit();

    return 0;
}

// Co-op: camera drag. Holding the RIGHT mouse button in exploration (ARROW)
// mode and moving the cursor drags the camera at frame rate (no edge-scroll
// throttle); a right press without movement still cycles the cursor mode, so
// the vanilla click behavior is preserved. The same mapScroll bounds apply,
// so the camera still cannot leave the map.
//
// The engine scrolls in whole 32x24 px tiles; a strict 1:1 grab would only
// move the camera after 32 px of travel (a dead zone) and step at most
// ~30 Hz. Scale the delta so the view tracks the cursor responsively and
// emit one step per frame per axis whenever a tile boundary is crossed.
#define MP_CAMERA_DRAG_SCALE 2
#define MP_CAMERA_DRAG_STEP_X 32
#define MP_CAMERA_DRAG_STEP_Y 24
// Movement required (pixels) before a held right-click becomes a drag; a
// press that stays inside this box is still a mode-cycle click.
#define MP_CAMERA_DRAG_START_DIST 10
// Step budget per 16ms frame at 60fps; slow hosts (combat broadcasts + full
// sim) accumulate more cursor travel per frame, so the budget scales with
// the elapsed frame time up to this hard cap.
#define MP_CAMERA_DRAG_STEPS_PER_FRAME 24
#define MP_CAMERA_DRAG_MAX_STEPS 192
// Co-op: after a drag release the cursor can sit inside the edge-scroll band
// (the drag just ended there). Suppress edge-scroll for this window so the
// camera does not lurch on release — the player can lift the button and move
// away, then normal edge-scroll resumes.
#define MP_CAMERA_DRAG_RELEASE_GRACE_MS 250
static bool gMpCameraDragActive = false;
static bool gMpRightDragCandidate = false;
static int gMpRightPressX = 0;
static int gMpRightPressY = 0;
static int gMpCameraDragLastX = 0;
static int gMpCameraDragLastY = 0;
static int gMpCameraDragAccumX = 0;
static int gMpCameraDragAccumY = 0;
static unsigned int gMpCameraDragLastTick = 0;
static unsigned int gMpDragReleaseGraceUntil = 0;
// The cursor mode when the right button went down: a drag restores this mode
// on release (so dragging never strands the player in exploration mode), and
// a plain click that started in exploration mode completes its cycle here.
static int gMpRightPressMode = GAME_MOUSE_MODE_ARROW;
static bool gMpRightPressModeSet = false;

bool gameMouseCameraDragActive()
{
    return gMpCameraDragActive;
}

void gameMouseCameraDragTick()
{
    // No scrolling-enabled gate here: the vanilla edge-scroll is disabled in
    // combat, but the drag is a deliberate camera move and must work there
    // too. The tick only runs from the main loop and MpCombatPump, so modals
    // (which block both) never see it.
    if (!mouseGetRightButton()) {
        if (gMpCameraDragActive || gMpRightDragCandidate) {
            if (gMpCameraDragActive) {
                // A drag owns the cursor mode for its whole gesture: hand it
                // back to the mode the player was in when the press started
                // (e.g. the tile-selector MOVE mode), so dragging never
                // leaves the cursor stuck in exploration mode.
                if (gMpRightPressModeSet) {
                    gameMouseSetMode(gMpRightPressMode);
                    // debugFilePrint("MPDRAG: drag release restore mode=%d",
                    //     gMpRightPressMode);
                }
                // Releasing near a screen edge must not throw the cursor
                // into edge-scroll the instant the drag ends.
                gMpDragReleaseGraceUntil = getTicks() + MP_CAMERA_DRAG_RELEASE_GRACE_MS;
            } else if (gMpRightPressMode == GAME_MOUSE_MODE_ARROW && _gmouse_enabled) {
                // Plain right-click that started in exploration mode: the
                // press did not cycle (see _gmouse_handle_event), so complete
                // the click here - the mode cycle happens on release. A drag
                // never cycles, so releasing after a drag leaves the cursor
                // alone.
                gameMouseCycleMode();
                // debugFilePrint("MPDRAG: click release cycle");
            }
            // debugFilePrint("MPDRAG: tick cleared (right released) drag=%d",
            //     gMpCameraDragActive);
        }
        gMpCameraDragActive = false;
        gMpRightDragCandidate = false;
        gMpRightPressModeSet = false;
        return;
    }
    // Only a press that started in exploration mode can become a drag. If
    // the DOWN event was missed entirely (very fast clicks) but the button
    // is held in exploration mode, arm the candidate from the current
    // position as a fallback.
    if (!gMpCameraDragActive && !gMpRightDragCandidate) {
        if (gGameMouseMode != GAME_MOUSE_MODE_ARROW
            || gameMouseClickOnInterfaceBar()) {
            return;
        }
        if (!gMpRightPressModeSet) {
            gMpRightPressMode = gGameMouseMode;
            gMpRightPressModeSet = true;
        }
        gMpRightDragCandidate = true;
        mouseGetPosition(&gMpRightPressX, &gMpRightPressY);
        // debugFilePrint("MPDRAG: tick self-armed mouse=%d,%d",
        //     gMpRightPressX, gMpRightPressY);
    }
    int x;
    int y;
    mouseGetPosition(&x, &y);
    if (gMpCameraDragActive) {
        // The drag owns the cursor mode: snap it back to exploration every
        // frame (a no-op when it already matches) so no stray event can
        // visibly cycle the cursor mid-drag.
        gameMouseSetMode(GAME_MOUSE_MODE_ARROW);
    }
    if (!gMpCameraDragActive) {
        // The press cycled to crosshair immediately (vanilla). While the
        // cursor stays within the start box this remains a plain click;
        // crossing it turns the hold into a drag and re-enters exploration
        // mode.
        if (abs(x - gMpRightPressX) < MP_CAMERA_DRAG_START_DIST
            && abs(y - gMpRightPressY) < MP_CAMERA_DRAG_START_DIST) {
            return;
        }
        gMpRightDragCandidate = false;
        gMpCameraDragActive = true;
        gameMouseSetMode(GAME_MOUSE_MODE_ARROW);
        gMpCameraDragLastX = x;
        gMpCameraDragLastY = y;
        gMpCameraDragAccumX = 0;
        gMpCameraDragAccumY = 0;
        gMpCameraDragLastTick = getTicks();
        // debugFilePrint("MPDRAG: drag engaged mouse=%d,%d", x, y);
        return;
    }
    const int dx = x - gMpCameraDragLastX;
    const int dy = y - gMpCameraDragLastY;
    gMpCameraDragLastX = x;
    gMpCameraDragLastY = y;
    // Grab-the-world: the content follows the cursor, so the camera pans
    // against the mouse delta (drag right -> the view moves further left).
    gMpCameraDragAccumX -= dx * MP_CAMERA_DRAG_SCALE;
    gMpCameraDragAccumY -= dy * MP_CAMERA_DRAG_SCALE;
    // The step budget scales with the elapsed frame time: a slow host
    // (combat broadcasts + full sim) accumulates more cursor travel per
    // frame, so it needs a matching step budget or the camera lags the
    // cursor and lurches to catch up.
    unsigned int nowTick = getTicks();
    int maxSteps = MP_CAMERA_DRAG_STEPS_PER_FRAME;
    if (gMpCameraDragLastTick != 0) {
        unsigned int elapsed = getTicksBetween(nowTick, gMpCameraDragLastTick);
        maxSteps = (int)std::min<unsigned int>(MP_CAMERA_DRAG_MAX_STEPS,
            MP_CAMERA_DRAG_STEPS_PER_FRAME * std::max<unsigned int>(1, elapsed / 16));
    }
    gMpCameraDragLastTick = nowTick;
    int steps = 0;
    while (steps < maxSteps
        && (gMpCameraDragAccumX >= MP_CAMERA_DRAG_STEP_X
            || gMpCameraDragAccumX <= -MP_CAMERA_DRAG_STEP_X
            || gMpCameraDragAccumY >= MP_CAMERA_DRAG_STEP_Y
            || gMpCameraDragAccumY <= -MP_CAMERA_DRAG_STEP_Y)) {
        const int sx = gMpCameraDragAccumX >= MP_CAMERA_DRAG_STEP_X ? 1
            : (gMpCameraDragAccumX <= -MP_CAMERA_DRAG_STEP_X ? -1 : 0);
        const int sy = gMpCameraDragAccumY >= MP_CAMERA_DRAG_STEP_Y ? 1
            : (gMpCameraDragAccumY <= -MP_CAMERA_DRAG_STEP_Y ? -1 : 0);
        if (mapScrollUnthrottled(sx, sy) != 0) {
            // A map edge rejected the step; drop the pending movement.
            gMpCameraDragAccumX = 0;
            gMpCameraDragAccumY = 0;
            break;
        }
        gMpCameraDragAccumX -= sx * MP_CAMERA_DRAG_STEP_X;
        gMpCameraDragAccumY -= sy * MP_CAMERA_DRAG_STEP_Y;
        steps++;
    }
}

// 0x44B2E8 gmouse_reset
int gameMouseReset()
{
    if (!gGameMouseInitialized) {
        return -1;
    }

    // NOTE: Uninline.
    if (gameMouseObjectsReset() != 0) {
        return -1;
    }

    // NOTE: Uninline.
    _gmouse_enable();

    _gmouse_scrolling_enabled = true;
    gameMouseSetCursor(MOUSE_CURSOR_ARROW);
    gGameMouseAnimatedCursorNextFrame = 0;
    gGameMouseAnimatedCursorLastUpdateTimestamp = 0;
    _gmouse_clicked_on_edge = false;

    return 0;
}

// 0x44B3B8 gmouse_exit
void gameMouseExit()
{
    if (!gGameMouseInitialized) {
        return;
    }

    mouseHideCursor();

    mouseSetFrame(nullptr, 0, 0, 0, 0, 0, 0);

    // NOTE: Uninline.
    gameMouseObjectsFree();

    if (gGameMouseCursorFrmHandle != INVALID_CACHE_ENTRY) {
        artUnlock(gGameMouseCursorFrmHandle);
    }
    gGameMouseCursorFrmHandle = INVALID_CACHE_ENTRY;

    _gmouse_enabled = false;
    gGameMouseInitialized = false;
    gGameMouseCursor = -1;
}

// 0x44B454 gmouse_enable
void _gmouse_enable()
{
    if (!_gmouse_enabled) {
        gGameMouseCursor = -1;
        gameMouseSetCursor(MOUSE_CURSOR_NONE);
        _gmouse_scrolling_enabled = true;
        _gmouse_enabled = true;
        _gmouse_bk_last_cursor = -1;
    }
}

// 0x44B48C gmouse_disable
void _gmouse_disable(int allowScrolling)
{
    if (_gmouse_enabled) {
        gameMouseSetCursor(MOUSE_CURSOR_NONE);
        _gmouse_enabled = false;

        if (allowScrolling & 1) {
            _gmouse_scrolling_enabled = true;
        } else {
            _gmouse_scrolling_enabled = false;
        }
    }
}

// 0x44B4CC gmouse_enable_scrolling
void _gmouse_enable_scrolling()
{
    _gmouse_scrolling_enabled = true;
}

// 0x44B4D8 gmouse_disable_scrolling
void _gmouse_disable_scrolling()
{
    _gmouse_scrolling_enabled = false;
}

// NOTE: Inlined.
//
// 0x44B4E4 gmouse_scrolling_is_enabled
bool gmouse_scrolling_is_enabled()
{
    return _gmouse_scrolling_enabled;
}

// 0x44B504 gmouse_get_click_to_scroll
bool _gmouse_get_click_to_scroll()
{
    return _gmouse_click_to_scroll;
}

// gmouse_set_click_to_scroll
void _gmouse_set_click_to_scroll(bool value)
{
    if (value != _gmouse_click_to_scroll) {
        _gmouse_click_to_scroll = value;
        _gmouse_clicked_on_edge = false;
    }
}

// 0x44B54C gmouse_is_scrolling
int _gmouse_is_scrolling()
{
    int isScrolling = 0;

    if (_gmouse_scrolling_enabled) {
        int x;
        int y;
        mouseGetPosition(&x, &y);
        if (x == _scr_size.left || x == _scr_size.right || y == _scr_size.top || y == _scr_size.bottom) {
            switch (gGameMouseCursor) {
            case MOUSE_CURSOR_SCROLL_NW:
            case MOUSE_CURSOR_SCROLL_N:
            case MOUSE_CURSOR_SCROLL_NE:
            case MOUSE_CURSOR_SCROLL_E:
            case MOUSE_CURSOR_SCROLL_SE:
            case MOUSE_CURSOR_SCROLL_S:
            case MOUSE_CURSOR_SCROLL_SW:
            case MOUSE_CURSOR_SCROLL_W:
            case MOUSE_CURSOR_SCROLL_NW_INVALID:
            case MOUSE_CURSOR_SCROLL_N_INVALID:
            case MOUSE_CURSOR_SCROLL_NE_INVALID:
            case MOUSE_CURSOR_SCROLL_E_INVALID:
            case MOUSE_CURSOR_SCROLL_SE_INVALID:
            case MOUSE_CURSOR_SCROLL_S_INVALID:
            case MOUSE_CURSOR_SCROLL_SW_INVALID:
            case MOUSE_CURSOR_SCROLL_W_INVALID:
                isScrolling = 1;
                break;
            default:
                return isScrolling;
            }
        }
    }

    return isScrolling;
}

// Co-op diagnostics for the "invisible cursor on the client" bug. Dumps the
// full cursor state so the log shows exactly which flag or fid leaves nothing
// on screen. Fires (throttled) whenever a co-op client is in the anomaly
// state: gmouse enabled but the 3D cursor objects hidden — the field cursor
// is invisible while input still works. MP_LOG_UI-gated via MpLog.
static void mpCursorDebugDump(const char* why)
{
    if (!gMpActive || !gMpIsClient || !_gmouse_enabled) {
        return;
    }
    bool hexHidden = (gGameMouseHexCursor->flags & OBJECT_HIDDEN) != 0;
    bool bounceHidden = (gGameMouseBouncingCursor->flags & OBJECT_HIDDEN) != 0;
    if (!hexHidden && !bounceHidden) {
        return;
    }
    int atPoint = -1;
    int mouseX;
    int mouseY;
    mouseGetPosition(&mouseX, &mouseY);
    atPoint = windowGetAtPoint(mouseX, mouseY);
    MpLog(MP_LOG_UI, "cursor anomaly [%s]: cursor=%d mode=%d gmouseEnabled=%d uiDisabled=%d hexHidden=%d hexFid=0x%X hexTile=%d hexElev=%d bounceHidden=%d atPointWin=%d isoWin=%d elev=%d",
        why,
        gGameMouseCursor,
        gGameMouseMode,
        _gmouse_enabled ? 1 : 0,
        gameUiIsDisabled() ? 1 : 0,
        hexHidden ? 1 : 0,
        gGameMouseHexCursor->fid,
        gGameMouseHexCursor->tile,
        gGameMouseHexCursor->elevation,
        bounceHidden ? 1 : 0,
        atPoint,
        gIsoWindow,
        gElevation);
}

// 0x44B684 gmouse_bk_process
void gameMouseRefresh()
{
    if (!gGameMouseInitialized) {
        return;
    }

    // Co-op diagnostics: periodic cursor-state dump (1s) while a co-op client
    // is in the invisible-cursor anomaly state, so the stuck steady state is
    // captured no matter how it got there.
    if (gMpActive && gMpIsClient) {
        static unsigned int gMpCursorLastDumpTick = 0;
        unsigned int now = getTicks();
        if (gMpCursorLastDumpTick == 0 || getTicksBetween(now, gMpCursorLastDumpTick) >= 1000) {
            gMpCursorLastDumpTick = now;
            mpCursorDebugDump("tick");
        }
    }

    int mouseX;
    int mouseY;

    if (gGameMouseCursor >= FIRST_GAME_MOUSE_ANIMATED_CURSOR) {
        _mouse_info();

        // NOTE: Uninline.
        if (gmouse_scrolling_is_enabled()) {
            mouseGetPosition(&mouseX, &mouseY);
            int oldMouseCursor = gGameMouseCursor;

            if (gameMouseHandleScrolling(mouseX, mouseY, gGameMouseCursor) == 0) {
                switch (oldMouseCursor) {
                case MOUSE_CURSOR_SCROLL_NW:
                case MOUSE_CURSOR_SCROLL_N:
                case MOUSE_CURSOR_SCROLL_NE:
                case MOUSE_CURSOR_SCROLL_E:
                case MOUSE_CURSOR_SCROLL_SE:
                case MOUSE_CURSOR_SCROLL_S:
                case MOUSE_CURSOR_SCROLL_SW:
                case MOUSE_CURSOR_SCROLL_W:
                case MOUSE_CURSOR_SCROLL_NW_INVALID:
                case MOUSE_CURSOR_SCROLL_N_INVALID:
                case MOUSE_CURSOR_SCROLL_NE_INVALID:
                case MOUSE_CURSOR_SCROLL_E_INVALID:
                case MOUSE_CURSOR_SCROLL_SE_INVALID:
                case MOUSE_CURSOR_SCROLL_S_INVALID:
                case MOUSE_CURSOR_SCROLL_SW_INVALID:
                case MOUSE_CURSOR_SCROLL_W_INVALID:
                    break;
                default:
                    _gmouse_bk_last_cursor = oldMouseCursor;
                    break;
                }
                return;
            }

            if (_gmouse_bk_last_cursor != -1) {
                gameMouseSetCursor(_gmouse_bk_last_cursor);
                _gmouse_bk_last_cursor = -1;
                return;
            }
        }

        gameMouseSetCursor(gGameMouseCursor);
        return;
    }

    if (!_gmouse_enabled) {
        // NOTE: Uninline.
        if (gmouse_scrolling_is_enabled()) {
            mouseGetPosition(&mouseX, &mouseY);
            int oldMouseCursor = gGameMouseCursor;

            if (gameMouseHandleScrolling(mouseX, mouseY, gGameMouseCursor) == 0) {
                switch (oldMouseCursor) {
                case MOUSE_CURSOR_SCROLL_NW:
                case MOUSE_CURSOR_SCROLL_N:
                case MOUSE_CURSOR_SCROLL_NE:
                case MOUSE_CURSOR_SCROLL_E:
                case MOUSE_CURSOR_SCROLL_SE:
                case MOUSE_CURSOR_SCROLL_S:
                case MOUSE_CURSOR_SCROLL_SW:
                case MOUSE_CURSOR_SCROLL_W:
                case MOUSE_CURSOR_SCROLL_NW_INVALID:
                case MOUSE_CURSOR_SCROLL_N_INVALID:
                case MOUSE_CURSOR_SCROLL_NE_INVALID:
                case MOUSE_CURSOR_SCROLL_E_INVALID:
                case MOUSE_CURSOR_SCROLL_SE_INVALID:
                case MOUSE_CURSOR_SCROLL_S_INVALID:
                case MOUSE_CURSOR_SCROLL_SW_INVALID:
                case MOUSE_CURSOR_SCROLL_W_INVALID:
                    break;
                default:
                    _gmouse_bk_last_cursor = oldMouseCursor;
                    break;
                }

                return;
            }

            if (_gmouse_bk_last_cursor != -1) {
                gameMouseSetCursor(_gmouse_bk_last_cursor);
                _gmouse_bk_last_cursor = -1;
            }
        }

        return;
    }

    mouseGetPosition(&mouseX, &mouseY);

    int oldMouseCursor = gGameMouseCursor;
    if (gameMouseHandleScrolling(mouseX, mouseY, MOUSE_CURSOR_NONE) == 0) {
        switch (oldMouseCursor) {
        case MOUSE_CURSOR_SCROLL_NW:
        case MOUSE_CURSOR_SCROLL_N:
        case MOUSE_CURSOR_SCROLL_NE:
        case MOUSE_CURSOR_SCROLL_E:
        case MOUSE_CURSOR_SCROLL_SE:
        case MOUSE_CURSOR_SCROLL_S:
        case MOUSE_CURSOR_SCROLL_SW:
        case MOUSE_CURSOR_SCROLL_W:
        case MOUSE_CURSOR_SCROLL_NW_INVALID:
        case MOUSE_CURSOR_SCROLL_N_INVALID:
        case MOUSE_CURSOR_SCROLL_NE_INVALID:
        case MOUSE_CURSOR_SCROLL_E_INVALID:
        case MOUSE_CURSOR_SCROLL_SE_INVALID:
        case MOUSE_CURSOR_SCROLL_S_INVALID:
        case MOUSE_CURSOR_SCROLL_SW_INVALID:
        case MOUSE_CURSOR_SCROLL_W_INVALID:
            break;
        default:
            _gmouse_bk_last_cursor = oldMouseCursor;
            break;
        }
        return;
    }

    if (_gmouse_bk_last_cursor != -1) {
        gameMouseSetCursor(_gmouse_bk_last_cursor);
        _gmouse_bk_last_cursor = -1;
    }

    if (windowGetAtPoint(mouseX, mouseY) != gIsoWindow) {
        if (gGameMouseCursor == MOUSE_CURSOR_NONE) {
            gameMouseObjectsHide();
            gameMouseSetCursor(MOUSE_CURSOR_ARROW);

            if (gGameMouseMode >= 2 && !isInCombat()) {
                gameMouseSetMode(GAME_MOUSE_MODE_MOVE);
            }
        }
        return;
    }

    // NOTE: Strange set of conditions and jumps. Not sure about this one.
    switch (gGameMouseCursor) {
    case MOUSE_CURSOR_NONE:
    case MOUSE_CURSOR_ARROW:
    case MOUSE_CURSOR_SMALL_ARROW_UP:
    case MOUSE_CURSOR_SMALL_ARROW_DOWN:
    case MOUSE_CURSOR_CROSSHAIR:
    case MOUSE_CURSOR_USE_CROSSHAIR:
        if (gGameMouseCursor != MOUSE_CURSOR_NONE) {
            gameMouseSetCursor(MOUSE_CURSOR_NONE);
        }

        if ((gGameMouseHexCursor->flags & OBJECT_HIDDEN) != 0) {
            gameMouseObjectsShow();
        }

        break;
    }

    Rect r1;
    if (_gmouse_3d_move_to(mouseX, mouseY, gElevation, &r1) == 0) {
        tileWindowRefreshRect(&r1, gElevation);
    }

    if ((gGameMouseHexCursor->flags & OBJECT_HIDDEN) != 0 || _gmouse_mapper_mode != 0) {
        return;
    }

    unsigned int v3 = _get_bk_time();
    if (mouseX == gGameMouseLastX && mouseY == gGameMouseLastY) {
        if (_gmouse_3d_hover_test || getTicksBetween(v3, _gmouse_3d_last_move_time) < 250) {
            return;
        }

        if (gGameMouseMode != GAME_MOUSE_MODE_MOVE) {
            if (gGameMouseMode == GAME_MOUSE_MODE_ARROW) {
                _gmouse_3d_last_move_time = v3;
                _gmouse_3d_hover_test = true;

                Object* pointedObject = gameMouseGetObjectUnderCursor(OBJ_TYPE_INVALID, true, gElevation);
                if (pointedObject != nullptr) {
                    int primaryAction = -1;
                    ObjectType objectType = objectTypeFromFid(pointedObject->fid);
                    switch (objectType) {
                    case OBJ_TYPE_SCENERY:
                    case OBJ_TYPE_WALL:
                    case OBJ_TYPE_MISC: {
                        // if the pointedObject is OBJ_TYPE_SCENERY/OBJ_TYPE_WALL/OBJ_TYPE_MISC object then try to find possible itemObject behind it
                        // this must be in sync with the switch/case in _gmouse_handle_event
                        Object* itemObject = gameMouseGetObjectUnderCursor(OBJ_TYPE_ITEM, true, gElevation);

                        if (itemObject == nullptr && objectType == OBJ_TYPE_MISC) {
                            break;
                        }

                        bool itemIsOutlined = objectHasVisibleOutline(itemObject);

                        if (!itemIsOutlined) {
                            // filter out potential itemObject behind usable scenery which is not outlined already
                            if (objectType == OBJ_TYPE_SCENERY && _obj_action_can_use(pointedObject)) {
                                primaryAction = GAME_MOUSE_ACTION_MENU_ITEM_USE;
                                break;
                            }

                            bool canBeShootOrSeenThroughObject = (pointedObject->flags & (OBJECT_SHOOT_THRU | OBJECT_LIGHT_THRU)) != 0;

                            // filter out not found itemObject and itemObject behind the blocking walls which is not outlined already
                            if (itemObject == nullptr || (objectType == OBJ_TYPE_WALL && !canBeShootOrSeenThroughObject)) {
                                primaryAction = GAME_MOUSE_ACTION_MENU_ITEM_LOOK;
                                break;
                            }
                        }

                        // intentionally fall trough the OBJ_TYPE_ITEM to enforce auto outlining of the itemObject and changing the mouse action menu
                        pointedObject = itemObject;
                    }
                    // FALLTHROUGH
                    case OBJ_TYPE_ITEM:
                        primaryAction = GAME_MOUSE_ACTION_MENU_ITEM_USE;
                        if (gGameMouseItemHighlightEnabled) {
                            Rect tmp;
                            if (objectSetOutline(pointedObject, OUTLINE_TYPE_ITEM, &tmp) == 0) {
                                tileWindowRefreshRect(&tmp, gElevation);
                                gGameMouseHighlightedItem = pointedObject;
                            }
                        }
                        break;
                    case OBJ_TYPE_CRITTER:
                        if (pointedObject == gDude) {
                            primaryAction = GAME_MOUSE_ACTION_MENU_ITEM_ROTATE;
                        } else {
                            if (_obj_action_can_talk_to(pointedObject)) {
                                if (isInCombat()) {
                                    primaryAction = GAME_MOUSE_ACTION_MENU_ITEM_LOOK;
                                } else {
                                    primaryAction = GAME_MOUSE_ACTION_MENU_ITEM_TALK;
                                }
                            } else {
                                if (critterFlagCheck(pointedObject->pid, CRITTER_NO_STEAL)) {
                                    primaryAction = GAME_MOUSE_ACTION_MENU_ITEM_LOOK;
                                } else {
                                    primaryAction = GAME_MOUSE_ACTION_MENU_ITEM_USE;
                                }
                            }
                        }
                        break;
                    default:
                        break;
                    }

                    if (primaryAction != -1) {
                        if (gameMouseRenderPrimaryAction(mouseX, mouseY, primaryAction, _scr_size.right - _scr_size.left + 1, _scr_size.bottom - _scr_size.top - 99) == 0) {
                            Rect tmp;
                            int fid = buildFid(OBJ_TYPE_INTERFACE, 282);
                            // NOTE: Uninline.
                            if (gmouse_3d_set_flat_fid(fid, &tmp) == 0) {
                                tileWindowRefreshRect(&tmp, gElevation);
                            }
                        }
                    }

                    if (pointedObject != gGameMousePointedObject) {
                        gGameMousePointedObject = pointedObject;
                        objectLookAt(gDude, gGameMousePointedObject);
                    }
                }
            } else if (gGameMouseMode == GAME_MOUSE_MODE_CROSSHAIR) {
                Object* pointedObject = gameMouseGetObjectUnderCursor(OBJ_TYPE_CRITTER, false, gElevation);
                if (pointedObject == nullptr) {
                    pointedObject = gameMouseGetObjectUnderCursor(OBJ_TYPE_INVALID, false, gElevation);
                    if (!objectIsDoor(pointedObject)) {
                        pointedObject = nullptr;
                    }
                }

                if (pointedObject != nullptr) {
                    bool pointedObjectIsCritter = objectTypeFromFid(pointedObject->fid) == OBJ_TYPE_CRITTER;

                    if (settings.preferences.combat_looks) {
                        if (objectExamine(gDude, pointedObject) == -1) {
                            objectLookAt(gDude, pointedObject);
                        }
                    }

                    int color;
                    int accuracy;
                    char formattedAccuracy[8];
                    if (_combat_to_hit(pointedObject, &accuracy)) {
                        snprintf(formattedAccuracy, sizeof(formattedAccuracy), "%d%%", accuracy);

                        if (pointedObjectIsCritter) {
                            if (pointedObject->data.critter.combat.team != 0) {
                                color = COLOR_WHITE;
                            } else {
                                color = COLOR_LIGHT_ORANGE;
                            }
                        } else {
                            color = COLOR_GREY;
                        }
                    } else {
                        snprintf(formattedAccuracy, sizeof(formattedAccuracy), " %c ", 'X');

                        if (pointedObjectIsCritter) {
                            if (pointedObject->data.critter.combat.team != 0) {
                                color = COLOR_RED;
                            } else {
                                color = COLOR_SAGE_GREEN;
                            }
                        } else {
                            color = COLOR_LIGHT_PINK;
                        }
                    }

                    if (gameMouseRenderAccuracy(formattedAccuracy, color) == 0) {
                        Rect tmp;
                        int fid = buildFid(OBJ_TYPE_INTERFACE, 284);
                        // NOTE: Uninline.
                        if (gmouse_3d_set_flat_fid(fid, &tmp) == 0) {
                            tileWindowRefreshRect(&tmp, gElevation);
                        }
                    }

                    if (gGameMousePointedObject != pointedObject) {
                        gGameMousePointedObject = pointedObject;
                    }
                } else {
                    Rect tmp;
                    if (gameMouseUpdateHexCursorFid(&tmp) == 0) {
                        tileWindowRefreshRect(&tmp, gElevation);
                    }
                }

                _gmouse_3d_last_move_time = v3;
                _gmouse_3d_hover_test = true;
            }
            return;
        }

        char formattedActionPoints[8];
        int color;
        int distance = _make_path(gDude, gDude->tile, gGameMouseHexCursor->tile, nullptr, 1);
        if (distance != 0) {
            if (!isInCombat()) {
                formattedActionPoints[0] = '\0';
                color = COLOR_RED;
            } else {
                int actionPointsMax = critterGetMovementPointCostAdjustedForCrippledLegs(gDude, distance);
                int actionPointsRequired = std::max(0, actionPointsMax - _combat_free_move);

                if (actionPointsRequired <= gDude->data.critter.combat.ap) {
                    snprintf(formattedActionPoints, sizeof(formattedActionPoints), "%d", actionPointsRequired);
                    color = COLOR_WHITE;
                } else {
                    snprintf(formattedActionPoints, sizeof(formattedActionPoints), "%c", 'X');
                    color = COLOR_RED;
                }
            }
        } else {
            snprintf(formattedActionPoints, sizeof(formattedActionPoints), "%c", 'X');
            color = COLOR_RED;
        }

        if (gameMouseRenderActionPoints(formattedActionPoints, color) == 0) {
            Rect tmp;
            objectGetRect(gGameMouseHexCursor, &tmp);
            tileWindowRefreshRect(&tmp, 0);
        }

        _gmouse_3d_last_move_time = v3;
        _gmouse_3d_hover_test = true;
        dword_518D9C = gGameMouseHexCursor->tile;
        return;
    }

    _gmouse_3d_last_move_time = v3;
    _gmouse_3d_hover_test = false;
    gGameMouseLastX = mouseX;
    gGameMouseLastY = mouseY;

    if (!_gmouse_mapper_mode) {
        int fid = buildFid(OBJ_TYPE_INTERFACE, 0);
        gameMouseSetBouncingCursorFid(fid);
    }

    int v34 = 0;

    Rect r2;
    Rect r26;
    if (gameMouseUpdateHexCursorFid(&r2) == 0) {
        v34 |= 1;
    }

    if (gGameMouseHighlightedItem != nullptr) {
        if (objectClearOutline(gGameMouseHighlightedItem, &r26) == 0) {
            v34 |= 2;
        }
        gGameMouseHighlightedItem = nullptr;
    }

    switch (v34) {
    case 3:
        rectUnion(&r2, &r26, &r2);
        // FALLTHROUGH
    case 1:
        tileWindowRefreshRect(&r2, gElevation);
        break;
    case 2:
        tileWindowRefreshRect(&r26, gElevation);
        break;
    }
}

bool gameMouseClickOnInterfaceBar()
{
    Rect interfaceBarWindowRect;
    windowGetRect(gInterfaceBarWindow, &interfaceBarWindowRect);

    int interfaceBarWindowRectLeft = 0;
    int interfaceBarWindowRectRight = _scr_size.right;

    if (settings.ui.iface_bar_mode) {
        interfaceBarWindowRectLeft = interfaceBarWindowRect.left;
        interfaceBarWindowRectRight = interfaceBarWindowRect.right;
    }

    return _mouse_click_in(interfaceBarWindowRectLeft, interfaceBarWindowRect.top, interfaceBarWindowRectRight, interfaceBarWindowRect.bottom);
}

// 0x44BFA8 gmouse_handle_event
void _gmouse_handle_event(int mouseX, int mouseY, int mouseState)
{
    // Co-op: while the local player initiated a map-change vote they are
    // frozen on the exit grid and can't issue move commands.
    if (gMpSession.initiatorFrozen) {
        return;
    }

    if (!gGameMouseInitialized) {
        return;
    }

    if (gGameMouseCursor >= MOUSE_CURSOR_WAIT_PLANET) {
        return;
    }

    if (!_gmouse_enabled) {
        return;
    }

    if (_gmouse_clicked_on_edge) {
        if (_gmouse_get_click_to_scroll()) {
            return;
        }
    }

    if (gameMouseClickOnInterfaceBar()) {
        return;
    }
    if (mapEdgeIsOverClippedArea(mouseX, mouseY)) {
        return;
    }

    // Check if we should block mouse button up events for inventoryOpenUseItemOn inventory window
    if (gBlockMouseUpEvent && (mouseState & MOUSE_EVENT_LEFT_BUTTON_UP) != 0) {
        gBlockMouseUpEvent = false;
        return;
    }

    if ((mouseState & MOUSE_EVENT_RIGHT_BUTTON_DOWN) != 0) {
        // While a camera drag is active the right button belongs to the drag
        // alone: no candidate re-arming and no mode cycling, even if a stray
        // down event (button flicker, repeat re-emission) arrives mid-hold.
        if (!gMpCameraDragActive) {
            // Remember the mode the press started in (before any cycle) so
            // a drag can restore it on release.
            if ((mouseState & MOUSE_EVENT_RIGHT_BUTTON_REPEAT) == 0) {
                gMpRightPressMode = gGameMouseMode;
                gMpRightPressModeSet = true;
            }
            // Camera-drag candidate: armed on ANY exploration-mode press,
            // outside the vanilla hex-cursor gate below - the cursor
            // visibility must not be able to starve the drag (the hex cursor
            // is hidden by every mapScroll call and can be hidden in some
            // modes).
            //
            // An exploration-mode press does NOT cycle the cursor: a drag
            // must keep exploration mode for the whole gesture, and a plain
            // click completes its cycle on release instead (see
            // gameMouseCameraDragTick). Releasing after a drag therefore
            // never changes the cursor mode.
            if ((mouseState & MOUSE_EVENT_RIGHT_BUTTON_REPEAT) == 0
                && gGameMouseMode == GAME_MOUSE_MODE_ARROW) {
                gMpRightDragCandidate = true;
                gMpRightPressX = mouseX;
                gMpRightPressY = mouseY;
                // debugFilePrint("MPDRAG: press candidate armed mouse=%d,%d",
                //     mouseX, mouseY);
            } else if ((mouseState & MOUSE_EVENT_RIGHT_BUTTON_REPEAT) == 0
                && ((gMpActive && gMpIsClient) || (gGameMouseHexCursor->flags & OBJECT_HIDDEN) == 0)) {
                // Non-exploration presses keep the vanilla cycle-on-press.
                gameMouseCycleMode();
            }
        }
        return;
    }

    if ((mouseState & MOUSE_EVENT_LEFT_BUTTON_UP) != 0) {
        if (gGameMouseMode == GAME_MOUSE_MODE_MOVE) {
            int actionPoints;
            if (isInCombat()) {
                actionPoints = _combat_free_move + gDude->data.critter.combat.ap;
            } else {
                actionPoints = -1;
            }

            bool shiftHeld = gPressedPhysicalKeys[SDL_SCANCODE_LSHIFT]
                || gPressedPhysicalKeys[SDL_SCANCODE_RSHIFT];

            if (gMpActive && gMpIsClient) {
                // Co-op client: execute the exact same movement locally for
                // immediate response, then send the semantic command so the
                // host can apply it to the authoritative player object.
                int tile = _check_move(&actionPoints);
                if (tile != -1) {
                    bool isRun = false;
                    bool usesMoveMode = shiftHeld
                        ? settings.preferences.running
                        : !settings.preferences.running;
                    int movementRc = usesMoveMode
                        ? _dude_move_to_tile(tile, gDude->elevation, actionPoints, &isRun)
                        : _dude_run_to_tile(tile, gDude->elevation, actionPoints);
                    if (!usesMoveMode) {
                        isRun = true;
                    }
                    if (movementRc == 0) {
                        // The repeated-destination rule lives in the shared
                        // movement helper, so prediction and the host receive
                        // the same walk/run decision.
                        MpLog(MP_LOG_INPUT, "move tile=%d elev=%d mode=%d shift=%d run=%d rc=%d sneak=%d",
                            tile, gDude->elevation, usesMoveMode ? 1 : 0, shiftHeld ? 1 : 0,
                            isRun ? 1 : 0, movementRc,
                            dudeHasState(DUDE_STATE_SNEAKING) ? 1 : 0);
                        MpSendPlayerAction(isRun ? NET_PLAYER_ACTION_RUN : NET_PLAYER_ACTION_WALK,
                            0, tile, gDude->elevation);
                    }
                }
                return;
            }

            if (shiftHeld) {
                if (settings.preferences.running) {
                    _dude_move(actionPoints);
                    return;
                }
            } else {
                if (!settings.preferences.running) {
                    _dude_move(actionPoints);
                    return;
                }
            }

            _dude_run(actionPoints);
            return;
        }

        if (gGameMouseMode == GAME_MOUSE_MODE_ARROW) {
            Object* targetObj = gameMouseGetObjectUnderCursor(OBJ_TYPE_INVALID, true, gElevation);
            if (targetObj != nullptr) {
                if (gMpActive && gMpIsClient) {
                    switch (objectTypeFromFid(targetObj->fid)) {
                    case OBJ_TYPE_ITEM:
                        gameMouseSendPlayerAction(NET_PLAYER_ACTION_PICK_UP, targetObj);
                        break;
                    case OBJ_TYPE_CRITTER:
                        if (targetObj == gDude) {
                            gameMouseSendPlayerAction(NET_PLAYER_ACTION_ROTATE, targetObj);
                        } else if (_obj_action_can_talk_to(targetObj)) {
                            if (isInCombat()) {
                                // Co-op: the host relays the authoritative
                                // examine text; only fall back to the local
                                // generic examine when the send failed.
                                if (!gameMouseSendPlayerAction(NET_PLAYER_ACTION_INSPECT, targetObj)) {
                                    if (objectExamine(gDude, targetObj) == -1) {
                                        objectLookAt(gDude, targetObj);
                                    }
                                }
                            } else {
                                gameMouseSendPlayerAction(NET_PLAYER_ACTION_TALK, targetObj);
                            }
                        } else {
                            gameMouseSendPlayerAction(NET_PLAYER_ACTION_LOOT, targetObj);
                        }
                        break;
                    case OBJ_TYPE_SCENERY:
                        if (_obj_action_can_use(targetObj)) {
                            gameMouseSendPlayerAction(NET_PLAYER_ACTION_TOUCH, targetObj);
                        } else if (!gameMouseSendPlayerAction(NET_PLAYER_ACTION_INSPECT, targetObj)) {
                            if (objectExamine(gDude, targetObj) == -1) {
                                objectLookAt(gDude, targetObj);
                            }
                        }
                        break;
                    case OBJ_TYPE_WALL:
                        if (!gameMouseSendPlayerAction(NET_PLAYER_ACTION_INSPECT, targetObj)) {
                            if (objectExamine(gDude, targetObj) == -1) {
                                objectLookAt(gDude, targetObj);
                            }
                        }
                        break;
                    }
                    return;
                }

                ObjectType objectType = objectTypeFromFid(targetObj->fid);
                switch (objectType) {
                case OBJ_TYPE_WALL:
                case OBJ_TYPE_SCENERY:
                case OBJ_TYPE_MISC: {
                    // if the targetObj is OBJ_TYPE_SCENERY/OBJ_TYPE_WALL/OBJ_TYPE_MISC object then it allows to pickup outlined itemObj potentially behind it
                    // this must be in sync with the switch/case in gameMouseRefresh
                    Object* itemObj = gameMouseGetObjectUnderCursor(OBJ_TYPE_ITEM, true, gElevation);
                    if (!objectHasVisibleOutline(itemObj)) {
                        if (objectType == OBJ_TYPE_SCENERY && _obj_action_can_use(targetObj)) {
                            _action_use_an_object(gDude, targetObj);
                            break;
                        }

                        if (objectType != OBJ_TYPE_MISC && objectExamine(gDude, targetObj) == -1) {
                            objectLookAt(gDude, targetObj);
                        }
                        break;
                    }

                    // intentionally fall through the OBJ_TYPE_ITEM to enforce actionPickUp
                    targetObj = itemObj;
                }
                // FALLTHROUGH
                case OBJ_TYPE_ITEM:
                    actionPickUp(gDude, targetObj);
                    break;
                case OBJ_TYPE_CRITTER:
                    if (targetObj == gDude) {
                        if (animationTypeFromFid(gDude->fid) == ANIM_STAND) {
                            Rect dudeRect;
                            if (objectRotateClockwise(targetObj, &dudeRect) == 0) {
                                tileWindowRefreshRect(&dudeRect, targetObj->elevation);
                            }
                        }
                    } else {
                        if (_obj_action_can_talk_to(targetObj)) {
                            if (isInCombat()) {
                                if (objectExamine(gDude, targetObj) == -1) {
                                    objectLookAt(gDude, targetObj);
                                }
                            } else {
                                actionTalk(gDude, targetObj);
                            }
                        } else {
                            actionLootCritter(gDude, targetObj);
                        }
                    }
                    break;
                default:
                    break;
                }
            }
            return;
        }

        if (gGameMouseMode == GAME_MOUSE_MODE_CROSSHAIR) {
            Object* targetObj = gameMouseGetObjectUnderCursor(OBJ_TYPE_CRITTER, false, gElevation);
            if (targetObj == nullptr) {
                targetObj = gameMouseGetObjectUnderCursor(OBJ_TYPE_INVALID, false, gElevation);
                if (!objectIsDoor(targetObj)) {
                    targetObj = nullptr;
                }
            }

            if (targetObj != nullptr) {
                _combat_attack_this(targetObj);
                _gmouse_3d_hover_test = true;
                gGameMouseLastY = mouseY;
                gGameMouseLastX = mouseX;
                _gmouse_3d_last_move_time = getTicks() - 250;
            }
            return;
        }

        if (gGameMouseMode == GAME_MOUSE_MODE_USE_CROSSHAIR) {
            Object* object = gameMouseGetObjectUnderCursor(OBJ_TYPE_INVALID, true, gElevation);
            if (object != nullptr) {
                Object* weapon;
                if (interfaceGetActiveItem(&weapon) != -1) {
                    if (gMpActive && gMpIsClient) {
                        // Co-op: the host owns the world. Relay the
                        // item-on-object use (item pid rides the tile field)
                        // and skip the local execution — the host runs the
                        // target's USE_OBJ_ON script and the object stream
                        // syncs the result back.
                        if (weapon != nullptr) {
                            gameMouseSendPlayerAction(NET_PLAYER_ACTION_USE_ITEM_ON,
                                object, 0, weapon->pid);
                        }
                        gameMouseSetCursor(MOUSE_CURSOR_NONE);
                        gameMouseSetMode(GAME_MOUSE_MODE_MOVE);
                        return;
                    }
                    if (isInCombat()) {
                        HitMode hitMode = interfaceGetCurrentHand()
                            ? HIT_MODE_RIGHT_WEAPON_PRIMARY
                            : HIT_MODE_LEFT_WEAPON_PRIMARY;

                        int actionPointsRequired = itemGetActionPointCost(gDude, hitMode, false);
                        if (actionPointsRequired <= gDude->data.critter.combat.ap) {
                            if (_action_use_an_item_on_object(gDude, object, weapon) != -1) {
                                int actionPoints = gDude->data.critter.combat.ap;
                                if (actionPointsRequired > actionPoints) {
                                    gDude->data.critter.combat.ap = 0;
                                } else {
                                    gDude->data.critter.combat.ap -= actionPointsRequired;
                                }
                                interfaceRenderActionPoints(gDude->data.critter.combat.ap, _combat_free_move);
                            }
                        }
                    } else {
                        _action_use_an_item_on_object(gDude, object, weapon);
                    }
                }
            }
            gameMouseSetCursor(MOUSE_CURSOR_NONE);
            gameMouseSetMode(GAME_MOUSE_MODE_MOVE);
            return;
        }

        if (gGameMouseMode == GAME_MOUSE_MODE_USE_FIRST_AID
            || gGameMouseMode == GAME_MOUSE_MODE_USE_DOCTOR
            || gGameMouseMode == GAME_MOUSE_MODE_USE_LOCKPICK
            || gGameMouseMode == GAME_MOUSE_MODE_USE_STEAL
            || gGameMouseMode == GAME_MOUSE_MODE_USE_TRAPS
            || gGameMouseMode == GAME_MOUSE_MODE_USE_SCIENCE
            || gGameMouseMode == GAME_MOUSE_MODE_USE_REPAIR) {
            Object* object = gameMouseGetObjectUnderCursor(OBJ_TYPE_INVALID, true, gElevation);
            if (gMpActive && gMpIsClient) {
                MpLog(MP_LOG_STATS, "client mode=%d obj=%p pid=0x%X fidType=%d tile=%d",
                    gGameMouseMode, (void*)object,
                    object != nullptr ? object->pid : 0,
                    object != nullptr ? objectTypeFromFid(object->fid) : -1,
                    object != nullptr ? object->tile : -1);
                if (object != nullptr) {
                    int skillIndex = gGameMouseMode - FIRST_GAME_MOUSE_MODE_SKILL;
                    if (gGameMouseModeSkills[skillIndex] == SKILL_SNEAK) {
                        // Co-op: the host flips the avatar's sneak flag in
                        // response to this intent; mirror the toggle on the
                        // local dude so the client's own HUD, footsteps and
                        // pose reflect the sneak state immediately.
                        dudeToggleState(DUDE_STATE_SNEAKING);
                    }
                    gameMouseSendPlayerAction(NET_PLAYER_ACTION_USE_SKILL, object,
                        (uint8_t)gGameMouseModeSkills[skillIndex]);
                }
                gameMouseSetCursor(MOUSE_CURSOR_NONE);
                gameMouseSetMode(GAME_MOUSE_MODE_MOVE);
                return;
            }
            if (object == nullptr || actionUseSkill(gDude, object, gGameMouseModeSkills[gGameMouseMode - FIRST_GAME_MOUSE_MODE_SKILL]) != -1) {
                gameMouseSetCursor(MOUSE_CURSOR_NONE);
                gameMouseSetMode(GAME_MOUSE_MODE_MOVE);
            }
            return;
        }
    }

    if ((mouseState & MOUSE_EVENT_LEFT_BUTTON_DOWN_REPEAT) == MOUSE_EVENT_LEFT_BUTTON_DOWN_REPEAT && gGameMouseMode == GAME_MOUSE_MODE_ARROW) {
        Object* targetObj = gameMouseGetObjectUnderCursor(OBJ_TYPE_INVALID, true, gElevation);
        if (targetObj != nullptr) {
            int actionMenuItemsCount = 0;
            int actionMenuItems[GAME_MOUSE_ACTION_MENU_ITEM_COUNT - 1];
            switch (objectTypeFromFid(targetObj->fid)) {
            case OBJ_TYPE_ITEM:
                actionMenuItems[actionMenuItemsCount++] = GAME_MOUSE_ACTION_MENU_ITEM_USE;
                actionMenuItems[actionMenuItemsCount++] = GAME_MOUSE_ACTION_MENU_ITEM_LOOK;
                if (itemGetType(targetObj) == ITEM_TYPE_CONTAINER) {
                    actionMenuItems[actionMenuItemsCount++] = GAME_MOUSE_ACTION_MENU_ITEM_INVENTORY;
                    actionMenuItems[actionMenuItemsCount++] = GAME_MOUSE_ACTION_MENU_ITEM_USE_SKILL;
                }
                actionMenuItems[actionMenuItemsCount++] = GAME_MOUSE_ACTION_MENU_ITEM_CANCEL;
                break;
            case OBJ_TYPE_CRITTER:
                if (targetObj == gDude) {
                    actionMenuItems[actionMenuItemsCount++] = GAME_MOUSE_ACTION_MENU_ITEM_ROTATE;
                } else {
                    if (_obj_action_can_talk_to(targetObj)) {
                        if (!isInCombat()) {
                            actionMenuItems[actionMenuItemsCount++] = GAME_MOUSE_ACTION_MENU_ITEM_TALK;
                        }

                        if (gameMouseLongPressUsesLootActionForCritter(targetObj)) {
                            actionMenuItems[actionMenuItemsCount++] = GAME_MOUSE_ACTION_MENU_ITEM_USE;
                        }
                    } else {
                        if (!critterFlagCheck(targetObj->pid, CRITTER_NO_STEAL)) {
                            actionMenuItems[actionMenuItemsCount++] = GAME_MOUSE_ACTION_MENU_ITEM_USE;
                        }
                    }

                    if (actionCheckPush(gDude, targetObj)) {
                        actionMenuItems[actionMenuItemsCount++] = GAME_MOUSE_ACTION_MENU_ITEM_PUSH;
                    }
                }

                actionMenuItems[actionMenuItemsCount++] = GAME_MOUSE_ACTION_MENU_ITEM_LOOK;
                actionMenuItems[actionMenuItemsCount++] = GAME_MOUSE_ACTION_MENU_ITEM_INVENTORY;
                actionMenuItems[actionMenuItemsCount++] = GAME_MOUSE_ACTION_MENU_ITEM_USE_SKILL;
                actionMenuItems[actionMenuItemsCount++] = GAME_MOUSE_ACTION_MENU_ITEM_CANCEL;
                break;
            case OBJ_TYPE_SCENERY:
                if (_obj_action_can_use(targetObj)) {
                    actionMenuItems[actionMenuItemsCount++] = GAME_MOUSE_ACTION_MENU_ITEM_USE;
                }

                actionMenuItems[actionMenuItemsCount++] = GAME_MOUSE_ACTION_MENU_ITEM_LOOK;
                actionMenuItems[actionMenuItemsCount++] = GAME_MOUSE_ACTION_MENU_ITEM_INVENTORY;
                actionMenuItems[actionMenuItemsCount++] = GAME_MOUSE_ACTION_MENU_ITEM_USE_SKILL;
                actionMenuItems[actionMenuItemsCount++] = GAME_MOUSE_ACTION_MENU_ITEM_CANCEL;
                break;
            case OBJ_TYPE_WALL:
                actionMenuItems[actionMenuItemsCount++] = GAME_MOUSE_ACTION_MENU_ITEM_LOOK;
                if (_obj_action_can_use(targetObj)) {
                    actionMenuItems[actionMenuItemsCount++] = GAME_MOUSE_ACTION_MENU_ITEM_INVENTORY;
                }
                actionMenuItems[actionMenuItemsCount++] = GAME_MOUSE_ACTION_MENU_ITEM_CANCEL;
                break;
            default:
                break;
            }

            if (gameMouseRenderActionMenuItems(mouseX, mouseY, actionMenuItems, actionMenuItemsCount, _scr_size.right - _scr_size.left + 1, _scr_size.bottom - _scr_size.top - 99) == 0) {
                Rect cursorRect;
                int fid = buildFid(OBJ_TYPE_INTERFACE, 283);
                // NOTE: Uninline.
                if (gmouse_3d_set_flat_fid(fid, &cursorRect) == 0 && _gmouse_3d_move_to(mouseX, mouseY, gElevation, &cursorRect) == 0) {
                    tileWindowRefreshRect(&cursorRect, gElevation);
                    isoDisable();

                    int newMouseY = mouseY;
                    int actionIndex = 0;
                    while ((mouseGetEvent() & MOUSE_EVENT_LEFT_BUTTON_UP) == 0) {
                        sharedFpsLimiter.mark();

                        inputGetInput();

                        if (_game_user_wants_to_quit != GAME_QUIT_REQUEST_NONE) {
                            actionMenuItems[actionIndex] = 0;
                        }

                        int updatedMouseX;
                        int updatedMouseY;
                        mouseGetPosition(&updatedMouseX, &updatedMouseY);

                        if (abs(updatedMouseY - newMouseY) > 10) {
                            int nextActionIndex;
                            if (newMouseY >= updatedMouseY) {
                                nextActionIndex = actionIndex - 1;
                            } else {
                                nextActionIndex = actionIndex + 1;
                            }

                            if (gameMouseHighlightActionMenuItemAtIndex(nextActionIndex) == 0) {
                                tileWindowRefreshRect(&cursorRect, gElevation);
                                actionIndex = nextActionIndex;
                            }
                            newMouseY = updatedMouseY;
                        }

                        renderPresent();
                        sharedFpsLimiter.throttle();
                    }

                    isoEnable();

                    _gmouse_3d_hover_test = false;
                    gGameMouseLastX = mouseX;
                    gGameMouseLastY = mouseY;
                    _gmouse_3d_last_move_time = getTicks();

                    _mouse_set_position(mouseX, mouseY);

                    if (gameMouseUpdateHexCursorFid(&cursorRect) == 0) {
                        tileWindowRefreshRect(&cursorRect, gElevation);
                    }

                    if (gMpActive && gMpIsClient) {
                        switch (actionMenuItems[actionIndex]) {
                        case GAME_MOUSE_ACTION_MENU_ITEM_INVENTORY:
                            inventoryOpenUseItemOn(targetObj);
                            break;
                        case GAME_MOUSE_ACTION_MENU_ITEM_LOOK:
                            // Co-op: host relays the authoritative examine text.
                            if (!gameMouseSendPlayerAction(NET_PLAYER_ACTION_INSPECT, targetObj)) {
                                if (objectExamine(gDude, targetObj) == -1) {
                                    objectLookAt(gDude, targetObj);
                                }
                            }
                            break;
                        case GAME_MOUSE_ACTION_MENU_ITEM_ROTATE:
                            gameMouseSendPlayerAction(NET_PLAYER_ACTION_ROTATE, targetObj);
                            break;
                        case GAME_MOUSE_ACTION_MENU_ITEM_TALK:
                            gameMouseSendPlayerAction(NET_PLAYER_ACTION_TALK, targetObj);
                            break;
                        case GAME_MOUSE_ACTION_MENU_ITEM_USE:
                            switch (objectTypeFromFid(targetObj->fid)) {
                            case OBJ_TYPE_SCENERY:
                                gameMouseSendPlayerAction(NET_PLAYER_ACTION_TOUCH, targetObj);
                                break;
                            case OBJ_TYPE_CRITTER:
                                if (gameMouseLongPressUsesLootActionForCritter(targetObj)) {
                                    gameMouseSendPlayerAction(NET_PLAYER_ACTION_USE_SKILL, targetObj, SKILL_STEAL);
                                } else {
                                    gameMouseSendPlayerAction(NET_PLAYER_ACTION_LOOT, targetObj);
                                }
                                break;
                            default:
                                gameMouseSendPlayerAction(NET_PLAYER_ACTION_PICK_UP, targetObj);
                                break;
                            }
                            break;
                        case GAME_MOUSE_ACTION_MENU_ITEM_USE_SKILL: {
                            int rc = skilldexOpen();
                            Skill skill = SKILL_INVALID;
                            switch (rc) {
                            case SKILLDEX_RC_SNEAK:
                                skill = SKILL_SNEAK;
                                break;
                            case SKILLDEX_RC_LOCKPICK:
                                skill = SKILL_LOCKPICK;
                                break;
                            case SKILLDEX_RC_STEAL:
                                skill = SKILL_STEAL;
                                break;
                            case SKILLDEX_RC_TRAPS:
                                skill = SKILL_TRAPS;
                                break;
                            case SKILLDEX_RC_FIRST_AID:
                                skill = SKILL_FIRST_AID;
                                break;
                            case SKILLDEX_RC_DOCTOR:
                                skill = SKILL_DOCTOR;
                                break;
                            case SKILLDEX_RC_SCIENCE:
                                skill = SKILL_SCIENCE;
                                break;
                            case SKILLDEX_RC_REPAIR:
                                skill = SKILL_REPAIR;
                                break;
                            default:
                                break;
                            }
                            if (skill != SKILL_INVALID) {
                                gameMouseSendPlayerAction(NET_PLAYER_ACTION_USE_SKILL, targetObj, (uint8_t)skill);
                            }
                            break;
                        }
                        case GAME_MOUSE_ACTION_MENU_ITEM_PUSH:
                            gameMouseSendPlayerAction(NET_PLAYER_ACTION_PUSH, targetObj);
                            break;
                        }
                    } else {
                        switch (actionMenuItems[actionIndex]) {
                    case GAME_MOUSE_ACTION_MENU_ITEM_INVENTORY:
                        inventoryOpenUseItemOn(targetObj);
                        break;
                    case GAME_MOUSE_ACTION_MENU_ITEM_LOOK:
                        if (objectExamine(gDude, targetObj) == -1) {
                            objectLookAt(gDude, targetObj);
                        }
                        break;
                    case GAME_MOUSE_ACTION_MENU_ITEM_ROTATE:
                        if (objectRotateClockwise(targetObj, &cursorRect) == 0) {
                            tileWindowRefreshRect(&cursorRect, targetObj->elevation);
                        }
                        break;
                    case GAME_MOUSE_ACTION_MENU_ITEM_TALK:
                        actionTalk(gDude, targetObj);
                        break;
                    case GAME_MOUSE_ACTION_MENU_ITEM_USE:
                        switch (objectTypeFromFid(targetObj->fid)) {
                        case OBJ_TYPE_SCENERY:
                            _action_use_an_object(gDude, targetObj);
                            break;
                        case OBJ_TYPE_CRITTER:
                            if (gameMouseLongPressUsesLootActionForCritter(targetObj)) {
                                // party member: trade via steal skill
                                actionUseSkill(gDude, targetObj, SKILL_STEAL);
                            } else {
                                actionLootCritter(gDude, targetObj);
                            }
                            break;
                        default:
                            actionPickUp(gDude, targetObj);
                            break;
                        }
                        break;
                    case GAME_MOUSE_ACTION_MENU_ITEM_USE_SKILL:
                        if (1) {
                            Skill skill = SKILL_INVALID;

                            SkilldexRC rc = skilldexOpen();
                            switch (rc) {
                            case SKILLDEX_RC_SNEAK:
                                _action_skill_use(SKILL_SNEAK);
                                break;
                            case SKILLDEX_RC_LOCKPICK:
                                skill = SKILL_LOCKPICK;
                                break;
                            case SKILLDEX_RC_STEAL:
                                skill = SKILL_STEAL;
                                break;
                            case SKILLDEX_RC_TRAPS:
                                skill = SKILL_TRAPS;
                                break;
                            case SKILLDEX_RC_FIRST_AID:
                                skill = SKILL_FIRST_AID;
                                break;
                            case SKILLDEX_RC_DOCTOR:
                                skill = SKILL_DOCTOR;
                                break;
                            case SKILLDEX_RC_SCIENCE:
                                skill = SKILL_SCIENCE;
                                break;
                            case SKILLDEX_RC_REPAIR:
                                skill = SKILL_REPAIR;
                                break;
                            default:
                                break;
                            }

                            if (skill != SKILL_INVALID) {
                                actionUseSkill(gDude, targetObj, skill);
                            }
                        }
                        break;
                    case GAME_MOUSE_ACTION_MENU_ITEM_PUSH:
                        actionPush(gDude, targetObj);
                        break;
                    }
                    }
                }
            }
        }
    }
}

// 0x44C840 gmouse_set_cursor
int gameMouseSetCursor(int cursor)
{
    if (!gGameMouseInitialized) {
        return -1;
    }

    int mpCursorPrevForLog = gGameMouseCursor;

    if (cursor != MOUSE_CURSOR_ARROW && cursor == gGameMouseCursor && (gGameMouseCursor < 25 || gGameMouseCursor >= 27)) {
        return -1;
    }

    CacheEntry* mouseCursorFrmHandle;
    int fid = buildFid(OBJ_TYPE_INTERFACE, gGameMouseCursorFrmIds[cursor]);
    Art* mouseCursorFrm = artLock(fid, &mouseCursorFrmHandle);
    if (mouseCursorFrm == nullptr) {
        mpCursorDebugDump("setCursorArtFail");
        return -1;
    }

    bool shouldUpdate = true;
    int frame = 0;
    if (cursor >= FIRST_GAME_MOUSE_ANIMATED_CURSOR) {
        unsigned int tick = getTicks();

        if ((gGameMouseHexCursor->flags & OBJECT_HIDDEN) == 0) {
            gameMouseObjectsHide();
        }

        unsigned int delay = 1000 / artGetFramesPerSecond(mouseCursorFrm);
        if (getTicksBetween(tick, gGameMouseAnimatedCursorLastUpdateTimestamp) < delay) {
            shouldUpdate = false;
        } else {
            if (artGetFrameCount(mouseCursorFrm) <= gGameMouseAnimatedCursorNextFrame) {
                gGameMouseAnimatedCursorNextFrame = 0;
            }

            frame = gGameMouseAnimatedCursorNextFrame;
            gGameMouseAnimatedCursorLastUpdateTimestamp = tick;
            gGameMouseAnimatedCursorNextFrame++;
        }
    }

    if (!shouldUpdate) {
        return -1;
    }

    int width = artGetWidth(mouseCursorFrm, frame);
    int height = artGetHeight(mouseCursorFrm, frame);

    int offsetX;
    int offsetY;
    artGetRotationOffsets(mouseCursorFrm, ROTATION_NE, &offsetX, &offsetY);

    offsetX = width / 2 - offsetX;
    offsetY = height - 1 - offsetY;

    unsigned char* mouseCursorFrmData = artGetFrameData(mouseCursorFrm, frame);
    if (mouseSetFrame(mouseCursorFrmData, width, height, width, offsetX, offsetY, 0) != 0) {
        return -1;
    }

    if (gGameMouseCursorFrmHandle != INVALID_CACHE_ENTRY) {
        artUnlock(gGameMouseCursorFrmHandle);
    }

    gGameMouseCursor = cursor;
    gGameMouseCursorFrmHandle = mouseCursorFrmHandle;

    if (cursor != mpCursorPrevForLog) {
        mpCursorDebugDump("setCursor");
    }

    return 0;
}

// 0x44C9E8 gmouse_get_cursor
int gameMouseGetCursor()
{
    return gGameMouseCursor;
}

// 0x44C9F0 gmouse_set_mapper_mode
void gmouse_set_mapper_mode(int mode)
{
    _gmouse_mapper_mode = mode;
}

// 0x44C9F8 gmouse_3d_enable_modes
void _gmouse_3d_enable_modes()
{
    _gmouse_3d_modes_enabled = 1;
}

// 0x44CA18 gmouse_3d_set_mode
void gameMouseSetMode(int mode)
{
    if (!gGameMouseInitialized) {
        return;
    }

    if (!_gmouse_3d_modes_enabled) {
        return;
    }

    if (mode == gGameMouseMode) {
        return;
    }

    int fid = buildFid(OBJ_TYPE_INTERFACE, 0);
    gameMouseSetBouncingCursorFid(fid);

    fid = buildFid(OBJ_TYPE_INTERFACE, gGameMouseModeFrmIds[mode]);

    Rect rect;
    // NOTE: Uninline.
    if (gmouse_3d_set_flat_fid(fid, &rect) == -1) {
        return;
    }

    int mouseX;
    int mouseY;
    mouseGetPosition(&mouseX, &mouseY);

    Rect cursorRect;
    if (_gmouse_3d_move_to(mouseX, mouseY, gElevation, &cursorRect) == 0) {
        rectUnion(&rect, &cursorRect, &rect);
    }

    int v5 = 0;
    if (gGameMouseMode == GAME_MOUSE_MODE_CROSSHAIR) {
        v5 = -1;
    }

    if (mode != 0) {
        if (mode == GAME_MOUSE_MODE_CROSSHAIR) {
            v5 = 1;
        }

        if (gGameMouseMode == 0) {
            if (objectDisableOutline(gGameMouseHexCursor, &cursorRect) == 0) {
                rectUnion(&rect, &cursorRect, &rect);
            }
        }
    } else {
        if (objectEnableOutline(gGameMouseHexCursor, &cursorRect) == 0) {
            rectUnion(&rect, &cursorRect, &rect);
        }
    }

    gGameMouseMode = mode;
    _gmouse_3d_hover_test = false;
    _gmouse_3d_last_move_time = getTicks();

    mpCursorDebugDump("setMode");

    tileWindowRefreshRect(&rect, gElevation);

    switch (v5) {
    case 1:
        _combat_outline_on();
        break;
    case -1:
        _combat_outline_off();
        break;
    }

    // On iPad, keep the gameplay cursor in trackpad mode so tapping the
    // screen doesn't teleport the cursor mid-combat. UI screens (inventory,
    // skilldex, elevator, menus) still enable touchscreen mode explicitly.
#if __APPLE__ && TARGET_OS_IOS
    touch_set_touchscreen_mode(false);
#else
    touch_set_touchscreen_mode(mode == GAME_MOUSE_MODE_MOVE);
#endif
}

// 0x44CB6C
int gameMouseGetMode()
{
    return gGameMouseMode;
}

// 0x44CB74 gmouse_3d_toggle_mode
void gameMouseCycleMode()
{
    int mode = (gGameMouseMode + 1) % 3;

    if (isInCombat()) {
        Object* item;
        if (interfaceGetActiveItem(&item) == 0) {
            if (item != nullptr && itemGetType(item) != ITEM_TYPE_WEAPON && mode == GAME_MOUSE_MODE_CROSSHAIR) {
                mode = GAME_MOUSE_MODE_MOVE;
            }
        }
    } else {
        if (mode == GAME_MOUSE_MODE_CROSSHAIR) {
            mode = GAME_MOUSE_MODE_MOVE;
        }
    }

    gameMouseSetMode(mode);
}

// 0x44CBD0 gmouse_3d_refresh
void _gmouse_3d_refresh()
{
    gGameMouseLastX = -1;
    gGameMouseLastY = -1;
    _gmouse_3d_hover_test = false;
    _gmouse_3d_last_move_time = 0;
    gameMouseRefresh();
}

// 0x44CBFC gmouse_3d_set_fid
int gameMouseSetBouncingCursorFid(int fid)
{
    if (!gGameMouseInitialized) {
        return -1;
    }

    if (!artExists(fid)) {
        return -1;
    }

    if (gGameMouseBouncingCursor->fid == fid) {
        return -1;
    }

    if (!_gmouse_mapper_mode) {
        return objectSetFid(gGameMouseBouncingCursor, fid, nullptr);
    }

    int refreshFlags = 0;

    Rect oldRect;
    if (gGameMouseBouncingCursor->fid != -1) {
        objectGetRect(gGameMouseBouncingCursor, &oldRect);
        refreshFlags |= REFRESH_BOUNCING_CURSOR;
    }

    int rc = -1;

    Rect rect;
    if (objectSetFid(gGameMouseBouncingCursor, fid, &rect) == 0) {
        rc = 0;
        refreshFlags |= REFRESH_HEX_CURSOR;
    }

    if ((gGameMouseHexCursor->flags & OBJECT_HIDDEN) == 0) {
        if (refreshFlags == REFRESH_BOUNCING_CURSOR) {
            tileWindowRefreshRect(&oldRect, gElevation);
        } else if (refreshFlags == REFRESH_HEX_CURSOR) {
            tileWindowRefreshRect(&rect, gElevation);
        } else if (refreshFlags == REFRESH_BOTH_CURSORS) {
            rectUnion(&oldRect, &rect, &oldRect);
            tileWindowRefreshRect(&oldRect, gElevation);
        }
    }

    return rc;
}

// 0x44CD0C gmouse_3d_reset_fid
void gameMouseResetBouncingCursorFid()
{
    int fid = buildFid(OBJ_TYPE_INTERFACE, 0);
    gameMouseSetBouncingCursorFid(fid);
}

// 0x44CD2C gmouse_3d_on
void gameMouseObjectsShow()
{
    if (!gGameMouseInitialized) {
        return;
    }

    mpCursorDebugDump("objectsShow");

    int refreshFlags = 0;

    Rect rect1;
    if (objectShow(gGameMouseBouncingCursor, &rect1) == 0) {
        refreshFlags |= REFRESH_BOUNCING_CURSOR;
    }

    Rect rect2;
    if (objectShow(gGameMouseHexCursor, &rect2) == 0) {
        refreshFlags |= REFRESH_HEX_CURSOR;
    }

    Rect tmp;
    if (gGameMouseMode != GAME_MOUSE_MODE_MOVE) {
        if (objectDisableOutline(gGameMouseHexCursor, &tmp) == 0) {
            if ((refreshFlags & REFRESH_HEX_CURSOR) != 0) {
                rectUnion(&rect2, &tmp, &rect2);
            } else {
                memcpy(&rect2, &tmp, sizeof(rect2));
                refreshFlags |= REFRESH_HEX_CURSOR;
            }
        }
    }

    if (gameMouseUpdateHexCursorFid(&tmp) == 0) {
        if ((refreshFlags & REFRESH_HEX_CURSOR) != 0) {
            rectUnion(&rect2, &tmp, &rect2);
        } else {
            memcpy(&rect2, &tmp, sizeof(rect2));
            refreshFlags |= REFRESH_HEX_CURSOR;
        }
    }

    if (refreshFlags != 0) {
        Rect* rect;
        switch (refreshFlags) {
        case REFRESH_BOUNCING_CURSOR:
            rect = &rect1;
            break;
        case REFRESH_HEX_CURSOR:
            rect = &rect2;
            break;
        case REFRESH_BOTH_CURSORS:
            rectUnion(&rect1, &rect2, &rect1);
            rect = &rect1;
            break;
        default:
            assert(false && "Should be unreachable");
        }

        tileWindowRefreshRect(rect, gElevation);
    }

    _gmouse_3d_hover_test = false;
    _gmouse_3d_last_move_time = getTicks() - 250;
}

// 0x44CE34 gmouse_3d_off
void gameMouseObjectsHide()
{
    if (!gGameMouseInitialized) {
        return;
    }

    mpCursorDebugDump("objectsHide");

    int refreshFlags = 0;

    Rect rect1;
    if (objectHide(gGameMouseBouncingCursor, &rect1) == 0) {
        refreshFlags |= REFRESH_BOUNCING_CURSOR;
    }

    Rect rect2;
    if (objectHide(gGameMouseHexCursor, &rect2) == 0) {
        refreshFlags |= REFRESH_HEX_CURSOR;
    }

    if (refreshFlags == REFRESH_BOUNCING_CURSOR) {
        tileWindowRefreshRect(&rect1, gElevation);
    } else if (refreshFlags == REFRESH_HEX_CURSOR) {
        tileWindowRefreshRect(&rect2, gElevation);
    } else if (refreshFlags == REFRESH_BOTH_CURSORS) {
        rectUnion(&rect1, &rect2, &rect1);
        tileWindowRefreshRect(&rect1, gElevation);
    }
}

// 0x44CEB0 gmouse_3d_is_on
bool gameMouseObjectsIsVisible()
{
    return (gGameMouseHexCursor->flags & OBJECT_HIDDEN) == 0;
}

// 0x44CEC4 object_under_mouse
Object* gameMouseGetObjectUnderCursor(ObjectType objectType, bool includeDude, int elevation)
{
    int mouseX;
    int mouseY;
    mouseGetPosition(&mouseX, &mouseY);

    bool intersectsRoof = false;
    if (objectType == -1) {
        if (_square_roof_intersect(mouseX, mouseY, elevation)) {
            if (_obj_intersects_with(gEgg, mouseX, mouseY) == 0) {
                intersectsRoof = true;
            }
        }
    }

    Object* found = nullptr;
    if (!intersectsRoof) {
        ObjectWithFlags* entries;
        int count = _obj_create_intersect_list(mouseX, mouseY, elevation, objectType, &entries);
        for (int index = count - 1; index >= 0; index--) {
            ObjectWithFlags* ptr = &(entries[index]);
            if (includeDude || gDude != ptr->object) {
                found = ptr->object;
                if ((ptr->flags & 0x01) != 0) {
                    if ((ptr->flags & 0x04) == 0) {
                        if (objectTypeFromFid(ptr->object->fid) != OBJ_TYPE_CRITTER || (ptr->object->data.critter.combat.results & (DAM_KNOCKED_OUT | DAM_DEAD)) == 0) {
                            break;
                        }
                    }
                }
            }
        }

        if (count != 0) {
            _obj_delete_intersect_list(&entries);
        }
    }
    return found;
}

// 0x44CFA0 gmouse_3d_build_pick_frame
int gameMouseRenderPrimaryAction(int x, int y, int menuItem, int width, int height)
{
    CacheEntry* menuItemFrmHandle;
    int menuItemFid = buildFid(OBJ_TYPE_INTERFACE, gGameMouseActionMenuItemFrmIds[menuItem]);
    Art* menuItemFrm = artLock(menuItemFid, &menuItemFrmHandle);
    if (menuItemFrm == nullptr) {
        return -1;
    }

    CacheEntry* arrowFrmHandle;
    int arrowFid = buildFid(OBJ_TYPE_INTERFACE, gGameMouseModeFrmIds[GAME_MOUSE_MODE_ARROW]);
    Art* arrowFrm = artLock(arrowFid, &arrowFrmHandle);
    if (arrowFrm == nullptr) {
        artUnlock(menuItemFrmHandle);
        // FIXME: Why this is success?
        return 0;
    }

    unsigned char* arrowFrmData = artGetFrameData(arrowFrm);
    int arrowFrmWidth = artGetWidth(arrowFrm);
    int arrowFrmHeight = artGetHeight(arrowFrm);

    unsigned char* menuItemFrmData = artGetFrameData(menuItemFrm);
    int menuItemFrmWidth = artGetWidth(menuItemFrm);
    int menuItemFrmHeight = artGetHeight(menuItemFrm);

    unsigned char* arrowFrmDest = gGameMouseActionPickFrmData;
    unsigned char* menuItemFrmDest = gGameMouseActionPickFrmData;

    _gmouse_3d_pick_frame_hot_x = 0;
    _gmouse_3d_pick_frame_hot_y = 0;

    gGameMouseActionPickFrm->xOffsets[0] = gGameMouseActionPickFrmWidth / 2;
    gGameMouseActionPickFrm->yOffsets[0] = gGameMouseActionPickFrmHeight - 1;

    int maxX = x + menuItemFrmWidth + arrowFrmWidth - 1;
    int maxY = y + menuItemFrmHeight - 1;
    int shiftY = maxY - height + 2;

    if (maxX < width) {
        menuItemFrmDest += arrowFrmWidth;
        if (maxY >= height) {
            _gmouse_3d_pick_frame_hot_y = shiftY;
            gGameMouseActionPickFrm->yOffsets[0] -= shiftY;
            arrowFrmDest += gGameMouseActionPickFrmWidth * shiftY;
        }
    } else {
        // mirrored cursor for far-right side of screen
        artUnlock(arrowFrmHandle);

        arrowFid = buildFid(OBJ_TYPE_INTERFACE, 285);
        arrowFrm = artLock(arrowFid, &arrowFrmHandle);
        arrowFrmData = artGetFrameData(arrowFrm);
        arrowFrmDest += menuItemFrmWidth;

        gGameMouseActionPickFrm->xOffsets[0] = -gGameMouseActionPickFrm->xOffsets[0];
        _gmouse_3d_pick_frame_hot_x += menuItemFrmWidth + arrowFrmWidth;

        if (maxY >= height) {
            _gmouse_3d_pick_frame_hot_y += shiftY;
            gGameMouseActionPickFrm->yOffsets[0] -= shiftY;

            arrowFrmDest += gGameMouseActionPickFrmWidth * shiftY;
        }
    }

    memset(gGameMouseActionPickFrmData, 0, gGameMouseActionPickFrmDataSize);

    blitBufferToBuffer(arrowFrmData, arrowFrmWidth, arrowFrmHeight, arrowFrmWidth, arrowFrmDest, gGameMouseActionPickFrmWidth);
    blitBufferToBuffer(menuItemFrmData, menuItemFrmWidth, menuItemFrmHeight, menuItemFrmWidth, menuItemFrmDest, gGameMouseActionPickFrmWidth);

    artUnlock(arrowFrmHandle);
    artUnlock(menuItemFrmHandle);

    return 0;
}

// 0x44D200 gmouse_3d_pick_frame_hot
int _gmouse_3d_pick_frame_hot(int* x, int* y)
{
    *x = _gmouse_3d_pick_frame_hot_x;
    *y = _gmouse_3d_pick_frame_hot_y;
    return 0;
}

// 0x44D214 gmouse_3d_build_menu_frame
int gameMouseRenderActionMenuItems(int x, int y, const int* menuItems, int menuItemsLength, int width, int height)
{
    _gmouse_3d_menu_actions_start = nullptr;
    gGameMouseActionMenuHighlightedItemIndex = 0;
    gGameMouseActionMenuItemsLength = 0;

    if (menuItems == nullptr) {
        return -1;
    }

    if (menuItemsLength == 0 || menuItemsLength >= GAME_MOUSE_ACTION_MENU_ITEM_COUNT) {
        return -1;
    }

    CacheEntry* menuItemFrmHandles[GAME_MOUSE_ACTION_MENU_ITEM_COUNT];
    Art* menuItemFrms[GAME_MOUSE_ACTION_MENU_ITEM_COUNT];

    for (int index = 0; index < menuItemsLength; index++) {
        int frmId = gGameMouseActionMenuItemFrmIds[menuItems[index]] & 0xFFFF;
        if (index == 0) {
            frmId -= 1;
        }

        int fid = buildFid(OBJ_TYPE_INTERFACE, frmId);

        menuItemFrms[index] = artLock(fid, &(menuItemFrmHandles[index]));
        if (menuItemFrms[index] == nullptr) {
            while (--index >= 0) {
                artUnlock(menuItemFrmHandles[index]);
            }
            return -1;
        }
    }

    int fid = buildFid(OBJ_TYPE_INTERFACE, gGameMouseModeFrmIds[GAME_MOUSE_MODE_ARROW]);
    CacheEntry* arrowFrmHandle;
    Art* arrowFrm = artLock(fid, &arrowFrmHandle);
    if (arrowFrm == nullptr) {
        for (int index = 0; index < menuItemsLength; index++) {
            artUnlock(menuItemFrmHandles[index]);
        }
        return -1;
    }

    int arrowWidth = artGetWidth(arrowFrm);
    int arrowHeight = artGetHeight(arrowFrm);

    int menuItemWidth = artGetWidth(menuItemFrms[0]);
    int menuItemHeight = artGetHeight(menuItemFrms[0]);

    _gmouse_3d_menu_frame_hot_x = 0;
    _gmouse_3d_menu_frame_hot_y = 0;

    gGameMouseActionMenuFrm->xOffsets[0] = gGameMouseActionMenuFrmWidth / 2;
    gGameMouseActionMenuFrm->yOffsets[0] = gGameMouseActionMenuFrmHeight - 1;

    int maxY = y + menuItemsLength * menuItemHeight - 1;
    int shiftY = maxY - height + 2;
    unsigned char* arrowFrmDest = gGameMouseActionMenuFrmData;
    unsigned char* menuItemFrmDest = arrowFrmDest;

    unsigned char* arrowData;
    if (x + arrowWidth + menuItemWidth - 1 < width) {
        arrowData = artGetFrameData(arrowFrm);
        menuItemFrmDest = arrowFrmDest + arrowWidth;
        if (height <= maxY) {
            _gmouse_3d_menu_frame_hot_y += shiftY;
            arrowFrmDest += gGameMouseActionMenuFrmWidth * shiftY;
            gGameMouseActionMenuFrm->yOffsets[0] -= shiftY;
        }
    } else {
        // Mirrored arrow (from left to right).
        artUnlock(arrowFrmHandle);
        fid = buildFid(OBJ_TYPE_INTERFACE, 285);
        arrowFrm = artLock(fid, &arrowFrmHandle);
        arrowData = artGetFrameData(arrowFrm);
        arrowFrmDest += menuItemWidth;

        gGameMouseActionMenuFrm->xOffsets[0] = -gGameMouseActionMenuFrm->xOffsets[0];
        _gmouse_3d_menu_frame_hot_x += menuItemWidth + arrowWidth;
        if (maxY >= height) {
            _gmouse_3d_menu_frame_hot_y += shiftY;
            gGameMouseActionMenuFrm->yOffsets[0] -= shiftY;
            arrowFrmDest += gGameMouseActionMenuFrmWidth * shiftY;
        }
    }

    memset(gGameMouseActionMenuFrmData, 0, gGameMouseActionMenuFrmDataSize);
    blitBufferToBuffer(arrowData, arrowWidth, arrowHeight, arrowWidth, arrowFrmDest, gGameMouseActionPickFrmWidth);

    unsigned char* dest = menuItemFrmDest;
    for (int index = 0; index < menuItemsLength; index++) {
        unsigned char* data = artGetFrameData(menuItemFrms[index]);
        blitBufferToBuffer(data, menuItemWidth, menuItemHeight, menuItemWidth, dest, gGameMouseActionPickFrmWidth);
        dest += gGameMouseActionMenuFrmWidth * menuItemHeight;
    }

    artUnlock(arrowFrmHandle);

    for (int index = 0; index < menuItemsLength; index++) {
        artUnlock(menuItemFrmHandles[index]);
    }

    memcpy(gGameMouseActionMenuItems, menuItems, sizeof(*gGameMouseActionMenuItems) * menuItemsLength);
    gGameMouseActionMenuItemsLength = menuItemsLength;
    _gmouse_3d_menu_actions_start = menuItemFrmDest;

    Sound* sound = soundEffectLoad("iaccuxx1", nullptr);
    if (sound != nullptr) {
        soundEffectPlay(sound);
    }

    return 0;
}

// 0x44D630 gmouse_3d_highlight_menu_frame
int gameMouseHighlightActionMenuItemAtIndex(int menuItemIndex)
{
    if (menuItemIndex < 0 || menuItemIndex >= gGameMouseActionMenuItemsLength) {
        return -1;
    }

    CacheEntry* handle;
    int fid = buildFid(OBJ_TYPE_INTERFACE, gGameMouseActionMenuItemFrmIds[gGameMouseActionMenuItems[gGameMouseActionMenuHighlightedItemIndex]]);
    Art* art = artLock(fid, &handle);
    if (art == nullptr) {
        return -1;
    }

    int width = artGetWidth(art);
    int height = artGetHeight(art);
    unsigned char* data = artGetFrameData(art);
    blitBufferToBuffer(data, width, height, width, _gmouse_3d_menu_actions_start + gGameMouseActionMenuFrmWidth * height * gGameMouseActionMenuHighlightedItemIndex, gGameMouseActionMenuFrmWidth);
    artUnlock(handle);

    fid = buildFid(OBJ_TYPE_INTERFACE, gGameMouseActionMenuItemFrmIds[gGameMouseActionMenuItems[menuItemIndex]] - 1);
    art = artLock(fid, &handle);
    if (art == nullptr) {
        return -1;
    }

    data = artGetFrameData(art);
    blitBufferToBuffer(data, width, height, width, _gmouse_3d_menu_actions_start + gGameMouseActionMenuFrmWidth * height * menuItemIndex, gGameMouseActionMenuFrmWidth);
    artUnlock(handle);

    gGameMouseActionMenuHighlightedItemIndex = menuItemIndex;

    return 0;
}

// 0x44D774 gmouse_3d_build_to_hit_frame
int gameMouseRenderAccuracy(const char* string, int color)
{
    CacheEntry* crosshairFrmHandle;
    int fid = buildFid(OBJ_TYPE_INTERFACE, gGameMouseModeFrmIds[GAME_MOUSE_MODE_CROSSHAIR]);
    Art* crosshairFrm = artLock(fid, &crosshairFrmHandle);
    if (crosshairFrm == nullptr) {
        return -1;
    }

    memset(gGameMouseActionHitFrmData, 0, gGameMouseActionHitFrmDataSize);

    int crosshairFrmWidth = artGetWidth(crosshairFrm);
    int crosshairFrmHeight = artGetHeight(crosshairFrm);
    unsigned char* crosshairFrmData = artGetFrameData(crosshairFrm);
    blitBufferToBuffer(crosshairFrmData,
        crosshairFrmWidth,
        crosshairFrmHeight,
        crosshairFrmWidth,
        gGameMouseActionHitFrmData,
        gGameMouseActionHitFrmWidth);

    int oldFont = fontGetCurrent();
    fontSetCurrent(101);

    fontDrawText(gGameMouseActionHitFrmData + gGameMouseActionHitFrmWidth + crosshairFrmWidth + 1,
        string,
        gGameMouseActionHitFrmWidth - crosshairFrmWidth,
        gGameMouseActionHitFrmWidth,
        color);

    bufferOutline(gGameMouseActionHitFrmData + crosshairFrmWidth,
        gGameMouseActionHitFrmWidth - crosshairFrmWidth,
        gGameMouseActionHitFrmHeight,
        gGameMouseActionHitFrmWidth,
        COLOR_BLACK);

    fontSetCurrent(oldFont);

    artUnlock(crosshairFrmHandle);

    return 0;
}

// 0x44D878 gmouse_3d_build_hex_frame
int gameMouseRenderActionPoints(const char* string, int color)
{
    memset(gGameMouseHexCursorFrmData, 0, gGameMouseHexCursorFrmWidth * gGameMouseHexCursorHeight);

    if (*string == '\0') {
        return 0;
    }

    int oldFont = fontGetCurrent();
    fontSetCurrent(101);

    int length = fontGetStringWidth(string);
    fontDrawText(gGameMouseHexCursorFrmData + gGameMouseHexCursorFrmWidth * (gGameMouseHexCursorHeight - fontGetLineHeight()) / 2 + (gGameMouseHexCursorFrmWidth - length) / 2, string, gGameMouseHexCursorFrmWidth, gGameMouseHexCursorFrmWidth, color);

    bufferOutline(gGameMouseHexCursorFrmData, gGameMouseHexCursorFrmWidth, gGameMouseHexCursorHeight, gGameMouseHexCursorFrmWidth, COLOR_BLACK);

    fontSetCurrent(oldFont);

    int fid = buildFid(OBJ_TYPE_INTERFACE, 1);
    gameMouseSetBouncingCursorFid(fid);

    return 0;
}

// 0x44D954 gmouse_3d_synch_item_highlight
void gameMouseLoadItemHighlight()
{
    gGameMouseItemHighlightEnabled = settings.preferences.item_highlight;
}

// 0x44D984 gmouse_3d_init
int gameMouseObjectsInit()
{
    int fid;

    if (gGameMouseObjectsInitialized) {
        return -1;
    }

    fid = buildFid(OBJ_TYPE_INTERFACE, 0);
    if (objectCreateWithFidPid(&gGameMouseBouncingCursor, fid, -1) != 0) {
        return -1;
    }

    fid = buildFid(OBJ_TYPE_INTERFACE, 1);
    if (objectCreateWithFidPid(&gGameMouseHexCursor, fid, -1) != 0) {
        return -1;
    }

    if (objectSetOutline(gGameMouseHexCursor, OUTLINE_PALETTED | OUTLINE_TYPE_2, nullptr) != 0) {
        return -1;
    }

    if (gameMouseActionMenuInit() != 0) {
        return -1;
    }

    gGameMouseBouncingCursor->flags |= OBJECT_LIGHT_THRU;
    gGameMouseBouncingCursor->flags |= OBJECT_NO_SAVE;
    gGameMouseBouncingCursor->flags |= OBJECT_NO_REMOVE;
    gGameMouseBouncingCursor->flags |= OBJECT_SHOOT_THRU;
    gGameMouseBouncingCursor->flags |= OBJECT_NO_BLOCK;

    gGameMouseHexCursor->flags |= OBJECT_NO_REMOVE;
    gGameMouseHexCursor->flags |= OBJECT_NO_SAVE;
    gGameMouseHexCursor->flags |= OBJECT_LIGHT_THRU;
    gGameMouseHexCursor->flags |= OBJECT_SHOOT_THRU;
    gGameMouseHexCursor->flags |= OBJECT_NO_BLOCK;

    _obj_toggle_flat(gGameMouseHexCursor, nullptr);

    int x;
    int y;
    mouseGetPosition(&x, &y);

    Rect v9;
    _gmouse_3d_move_to(x, y, gElevation, &v9);

    gGameMouseObjectsInitialized = true;

    gameMouseLoadItemHighlight();

    return 0;
}

// NOTE: Inlined.
//
// 0x44DAC0 gmouse_3d_reset
int gameMouseObjectsReset()
{
    if (!gGameMouseObjectsInitialized) {
        return -1;
    }

    // NOTE: Uninline.
    _gmouse_3d_enable_modes();

    // NOTE: Uninline.
    gameMouseResetBouncingCursorFid();

    gameMouseSetMode(GAME_MOUSE_MODE_MOVE);
    gameMouseObjectsShow();

    gGameMouseLastX = -1;
    gGameMouseLastY = -1;
    _gmouse_3d_hover_test = false;
    _gmouse_3d_last_move_time = getTicks();
    gameMouseLoadItemHighlight();

    return 0;
}

// NOTE: Inlined.
//
// 0x44DB34 gmouse_3d_exit
void gameMouseObjectsFree()
{
    if (gGameMouseObjectsInitialized) {
        gameMouseActionMenuFree();

        gGameMouseBouncingCursor->flags &= ~OBJECT_NO_SAVE;
        gGameMouseHexCursor->flags &= ~OBJECT_NO_SAVE;

        objectDestroy(gGameMouseBouncingCursor, nullptr);
        objectDestroy(gGameMouseHexCursor, nullptr);

        gGameMouseObjectsInitialized = false;
    }
}

// 0x44DB78 gmouse_3d_lock_frames
int gameMouseActionMenuInit()
{
    int fid;

    // actmenu.frm - action menu
    fid = buildFid(OBJ_TYPE_INTERFACE, 283);
    gGameMouseActionMenuFrm = artLock(fid, &gGameMouseActionMenuFrmHandle);
    if (gGameMouseActionMenuFrm == nullptr) {
        goto err;
    }

    // actpick.frm - action pick
    fid = buildFid(OBJ_TYPE_INTERFACE, 282);
    gGameMouseActionPickFrm = artLock(fid, &gGameMouseActionPickFrmHandle);
    if (gGameMouseActionPickFrm == nullptr) {
        goto err;
    }

    // acttohit.frm - action to hit
    fid = buildFid(OBJ_TYPE_INTERFACE, 284);
    gGameMouseActionHitFrm = artLock(fid, &gGameMouseActionHitFrmHandle);
    if (gGameMouseActionHitFrm == nullptr) {
        goto err;
    }

    // blank.frm - used be mset000.frm for top of bouncing mouse cursor
    fid = buildFid(OBJ_TYPE_INTERFACE, 0);
    gGameMouseBouncingCursorFrm = artLock(fid, &gGameMouseBouncingCursorFrmHandle);
    if (gGameMouseBouncingCursorFrm == nullptr) {
        goto err;
    }

    // msef000.frm - hex mouse cursor
    fid = buildFid(OBJ_TYPE_INTERFACE, 1);
    gGameMouseHexCursorFrm = artLock(fid, &gGameMouseHexCursorFrmHandle);
    if (gGameMouseHexCursorFrm == nullptr) {
        goto err;
    }

    gGameMouseActionMenuFrmWidth = artGetWidth(gGameMouseActionMenuFrm);
    gGameMouseActionMenuFrmHeight = artGetHeight(gGameMouseActionMenuFrm);
    gGameMouseActionMenuFrmDataSize = gGameMouseActionMenuFrmWidth * gGameMouseActionMenuFrmHeight;
    gGameMouseActionMenuFrmData = artGetFrameData(gGameMouseActionMenuFrm);

    gGameMouseActionPickFrmWidth = artGetWidth(gGameMouseActionPickFrm);
    gGameMouseActionPickFrmHeight = artGetHeight(gGameMouseActionPickFrm);
    gGameMouseActionPickFrmDataSize = gGameMouseActionPickFrmWidth * gGameMouseActionPickFrmHeight;
    gGameMouseActionPickFrmData = artGetFrameData(gGameMouseActionPickFrm);

    gGameMouseActionHitFrmWidth = artGetWidth(gGameMouseActionHitFrm);
    gGameMouseActionHitFrmHeight = artGetHeight(gGameMouseActionHitFrm);
    gGameMouseActionHitFrmDataSize = gGameMouseActionHitFrmWidth * gGameMouseActionHitFrmHeight;
    gGameMouseActionHitFrmData = artGetFrameData(gGameMouseActionHitFrm);

    gGameMouseBouncingCursorFrmWidth = artGetWidth(gGameMouseBouncingCursorFrm);
    gGameMouseBouncingCursorFrmHeight = artGetHeight(gGameMouseBouncingCursorFrm);
    gGameMouseBouncingCursorFrmDataSize = gGameMouseBouncingCursorFrmWidth * gGameMouseBouncingCursorFrmHeight;
    gGameMouseBouncingCursorFrmData = artGetFrameData(gGameMouseBouncingCursorFrm);

    gGameMouseHexCursorFrmWidth = artGetWidth(gGameMouseHexCursorFrm);
    gGameMouseHexCursorHeight = artGetHeight(gGameMouseHexCursorFrm);
    gGameMouseHexCursorDataSize = gGameMouseHexCursorFrmWidth * gGameMouseHexCursorHeight;
    gGameMouseHexCursorFrmData = artGetFrameData(gGameMouseHexCursorFrm);

    return 0;

err:

    // NOTE: Original code is different. There is no call to this function.
    // Instead it either use deep nesting or bunch of goto's to unwind
    // locked frms from the point of failure.
    gameMouseActionMenuFree();

    return -1;
}

// 0x44DE44 gmouse_3d_unlock_frames
void gameMouseActionMenuFree()
{
    if (gGameMouseBouncingCursorFrmHandle != INVALID_CACHE_ENTRY) {
        artUnlock(gGameMouseBouncingCursorFrmHandle);
    }
    gGameMouseBouncingCursorFrm = nullptr;
    gGameMouseBouncingCursorFrmHandle = INVALID_CACHE_ENTRY;

    if (gGameMouseHexCursorFrmHandle != INVALID_CACHE_ENTRY) {
        artUnlock(gGameMouseHexCursorFrmHandle);
    }
    gGameMouseHexCursorFrm = nullptr;
    gGameMouseHexCursorFrmHandle = INVALID_CACHE_ENTRY;

    if (gGameMouseActionHitFrmHandle != INVALID_CACHE_ENTRY) {
        artUnlock(gGameMouseActionHitFrmHandle);
    }
    gGameMouseActionHitFrm = nullptr;
    gGameMouseActionHitFrmHandle = INVALID_CACHE_ENTRY;

    if (gGameMouseActionMenuFrmHandle != INVALID_CACHE_ENTRY) {
        artUnlock(gGameMouseActionMenuFrmHandle);
    }
    gGameMouseActionMenuFrm = nullptr;
    gGameMouseActionMenuFrmHandle = INVALID_CACHE_ENTRY;

    if (gGameMouseActionPickFrmHandle != INVALID_CACHE_ENTRY) {
        artUnlock(gGameMouseActionPickFrmHandle);
    }

    gGameMouseActionPickFrm = nullptr;
    gGameMouseActionPickFrmHandle = INVALID_CACHE_ENTRY;

    gGameMouseActionPickFrmData = nullptr;
    gGameMouseActionPickFrmWidth = 0;
    gGameMouseActionPickFrmHeight = 0;
    gGameMouseActionPickFrmDataSize = 0;
}

// NOTE: Inlined.
//
// 0x44DF1C gmouse_3d_set_flat_fid
static int gmouse_3d_set_flat_fid(int fid, Rect* rect)
{
    if (objectSetFid(gGameMouseHexCursor, fid, rect) == 0) {
        return 0;
    }

    return -1;
}

// 0x44DF40 gmouse_3d_reset_flat_fid
int gameMouseUpdateHexCursorFid(Rect* rect)
{
    int fid = buildFid(OBJ_TYPE_INTERFACE, gGameMouseModeFrmIds[gGameMouseMode]);
    if (gGameMouseHexCursor->fid == fid) {
        return -1;
    }

    // NOTE: Uninline.
    return gmouse_3d_set_flat_fid(fid, rect);
}

// 0x44DF94 gmouse_3d_move_to
int _gmouse_3d_move_to(int x, int y, int elevation, Rect* rect)
{
    if (_gmouse_mapper_mode == 0) {
        if (gGameMouseMode != GAME_MOUSE_MODE_MOVE) {
            int offsetX = 0;
            int offsetY = 0;
            CacheEntry* hexCursorFrmHandle;
            Art* hexCursorFrm = artLock(gGameMouseHexCursor->fid, &hexCursorFrmHandle);
            if (hexCursorFrm != nullptr) {
                artGetRotationOffsets(hexCursorFrm, ROTATION_NE, &offsetX, &offsetY);

                int frameOffsetX;
                int frameOffsetY;
                artGetFrameOffsets(hexCursorFrm, 0, ROTATION_NE, &frameOffsetX, &frameOffsetY);

                offsetX += frameOffsetX;
                offsetY += frameOffsetY;

                artUnlock(hexCursorFrmHandle);
            }

            _obj_move(gGameMouseHexCursor, x + offsetX, y + offsetY, elevation, rect);
        } else {
            int tile = tileFromScreenXY(x, y);
            if (tile != -1) {
                int screenX;
                int screenY;

                bool v1 = false;
                Rect rect1;
                if (tileToScreenXY(tile, &screenX, &screenY) == 0) {
                    if (_obj_move(gGameMouseBouncingCursor, screenX + 16, screenY + 15, 0, &rect1) == 0) {
                        v1 = true;
                    }
                }

                Rect rect2;
                if (objectSetLocation(gGameMouseHexCursor, tile, elevation, &rect2) == 0) {
                    if (v1) {
                        rectUnion(&rect1, &rect2, &rect1);
                    } else {
                        rectCopy(&rect1, &rect2);
                    }

                    rectCopy(rect, &rect1);
                }
            }
        }
        return 0;
    }

    int tile;
    int x1 = 0;
    int y1 = 0;

    int fid = gGameMouseBouncingCursor->fid;
    if (objectTypeFromFid(fid) == OBJ_TYPE_TILE) {
        int squareTile = squareTileFromScreenXY(x, y, elevation);
        if (squareTile != -1) {
            tile = HEX_GRID_WIDTH * (2 * (squareTile / SQUARE_GRID_WIDTH) + 1) + 2 * (squareTile % SQUARE_GRID_WIDTH) + 1;
            x1 = -8;
            y1 = 13;

            if (settings.system.executableIsMapper() && tileRoofIsVisible() && (gDude->flags & OBJECT_HIDDEN) == 0) {
                y1 = -83;
            }
        } else {
            tile = -1;
        }
    } else {
        tile = tileFromScreenXY(x, y);
    }

    if (tile != -1) {
        bool v1 = false;

        Rect rect1;
        Rect rect2;

        if (objectSetLocation(gGameMouseBouncingCursor, tile, elevation, &rect1) == 0) {
            if (x1 != 0 || y1 != 0) {
                if (_obj_offset(gGameMouseBouncingCursor, x1, y1, &rect2) == 0) {
                    rectUnion(&rect1, &rect2, &rect1);
                }
            }
            v1 = true;
        }

        if (gGameMouseMode != GAME_MOUSE_MODE_MOVE) {
            int offsetX = 0;
            int offsetY = 0;
            CacheEntry* hexCursorFrmHandle;
            Art* hexCursorFrm = artLock(gGameMouseHexCursor->fid, &hexCursorFrmHandle);
            if (hexCursorFrm != nullptr) {
                artGetRotationOffsets(hexCursorFrm, ROTATION_NE, &offsetX, &offsetY);

                int frameOffsetX;
                int frameOffsetY;
                artGetFrameOffsets(hexCursorFrm, 0, ROTATION_NE, &frameOffsetX, &frameOffsetY);

                offsetX += frameOffsetX;
                offsetY += frameOffsetY;

                artUnlock(hexCursorFrmHandle);
            }

            if (_obj_move(gGameMouseHexCursor, x + offsetX, y + offsetY, elevation, &rect2) == 0) {
                if (v1) {
                    rectUnion(&rect1, &rect2, &rect1);
                } else {
                    rectCopy(&rect1, &rect2);
                    v1 = true;
                }
            }
        } else {
            if (objectSetLocation(gGameMouseHexCursor, tile, elevation, &rect2) == 0) {
                if (v1) {
                    rectUnion(&rect1, &rect2, &rect1);
                } else {
                    rectCopy(&rect1, &rect2);
                    v1 = true;
                }
            }
        }

        if (v1) {
            rectCopy(rect, &rect1);
        }
    }

    return 0;
}

// 0x44E42C gmouse_check_scrolling
int gameMouseHandleScrolling(int x, int y, int cursor)
{
    if (!_gmouse_scrolling_enabled) {
        return -1;
    }

    // Co-op: while a middle-mouse camera drag is active, the edge-scroll
    // must not fight it (the cursor sits at an edge, the drag owns the pan).
    if (gMpCameraDragActive) {
        return -1;
    }
    // Co-op: right after a camera-drag release the cursor can still sit
    // inside the edge-scroll band (the drag just ended there). Let the
    // player lift the button and move away before normal edge-scroll
    // resumes, so the camera does not lurch the instant the drag ends.
    if (gMpDragReleaseGraceUntil != 0) {
        if (getTicks() < gMpDragReleaseGraceUntil) {
            return -1;
        }
        gMpDragReleaseGraceUntil = 0;
    }

    // Only edge-scroll while the pointer is actually inside the window. The
    // GNW mouse position is clipped to the screen rect, so a cursor parked
    // outside the window reports the clamped edge position and would keep
    // scrolling the camera indefinitely. SDL clears MOUSE_FOCUS the moment
    // the pointer leaves the window.
    if (gSdlWindow != nullptr
        && (SDL_GetWindowFlags(gSdlWindow) & SDL_WINDOW_MOUSE_FOCUS) == 0) {
        return -1;
    }

    int flags = 0;

    // Edge-scroll band width in pixels. Vanilla only fires when the cursor is
    // EXACTLY on the screen edge (a 1px sliver — nearly impossible to hit
    // reliably, and the cursor is often clipped a pixel inside). Widen it so
    // a comfortable band around the border scrolls the camera.
    const int scrollMargin = 12;

    if (x <= _scr_size.left + scrollMargin) {
        flags |= SCROLLABLE_W;
    }

    if (x >= _scr_size.right - scrollMargin) {
        flags |= SCROLLABLE_E;
    }

    if (y <= _scr_size.top + scrollMargin) {
        flags |= SCROLLABLE_N;
    }

    if (y >= _scr_size.bottom - scrollMargin) {
        flags |= SCROLLABLE_S;
    }

    // Click-to-scroll mode (mapper Alt-Z): only scroll while the left button is held at an
    // edge. Track this in _gmouse_clicked_on_edge so callers can react (e.g. suppress the
    // regular 3D cursor update while an edge-click scroll is active).
    // TODO: reconcile with other site where _gmouse_clicked_on_edge is set to true
    if (_gmouse_click_to_scroll) {
        bool atEdge = flags != 0;
        bool leftHeld = (mouseGetEvent() & MOUSE_EVENT_LEFT_BUTTON_REPEAT) != 0;
        _gmouse_clicked_on_edge = atEdge && leftHeld;
        if (!_gmouse_clicked_on_edge) {
            return -1;
        }
    }

    int dx = 0;
    int dy = 0;

    switch (flags) {
    case SCROLLABLE_W:
        dx = -1;
        cursor = MOUSE_CURSOR_SCROLL_W;
        break;
    case SCROLLABLE_E:
        dx = 1;
        cursor = MOUSE_CURSOR_SCROLL_E;
        break;
    case SCROLLABLE_N:
        dy = -1;
        cursor = MOUSE_CURSOR_SCROLL_N;
        break;
    case SCROLLABLE_N | SCROLLABLE_W:
        dx = -1;
        dy = -1;
        cursor = MOUSE_CURSOR_SCROLL_NW;
        break;
    case SCROLLABLE_N | SCROLLABLE_E:
        dx = 1;
        dy = -1;
        cursor = MOUSE_CURSOR_SCROLL_NE;
        break;
    case SCROLLABLE_S:
        dy = 1;
        cursor = MOUSE_CURSOR_SCROLL_S;
        break;
    case SCROLLABLE_S | SCROLLABLE_W:
        dx = -1;
        dy = 1;
        cursor = MOUSE_CURSOR_SCROLL_SW;
        break;
    case SCROLLABLE_S | SCROLLABLE_E:
        dx = 1;
        dy = 1;
        cursor = MOUSE_CURSOR_SCROLL_SE;
        break;
    }

    if (dx == 0 && dy == 0) {
        if (mapEdgeIsOverClippedArea(x, y)) {
            gameMouseObjectsHide();
            gameMouseSetCursor(MOUSE_CURSOR_ARROW);
            return 0;
        }
        return -1;
    }

    int rc = mapScroll(dx, dy);
    switch (rc) {
    case -1:
        // Scrolling is blocked for whatever reason, upgrade cursor to
        // appropriate blocked version.
        cursor += 8;
        // FALLTHROUGH
    case 0:
        gameMouseSetCursor(cursor);
        break;
    }

    return 0;
}

// 0x44E544 gmouse_remove_item_outline
void _gmouse_remove_item_outline(Object* object)
{
    if (gGameMouseHighlightedItem != nullptr && gGameMouseHighlightedItem == object) {
        Rect rect;
        if (objectClearOutline(object, &rect) == 0) {
            tileWindowRefreshRect(&rect, gElevation);
        }
        gGameMouseHighlightedItem = nullptr;
    }
}

// 0x44E580 gmObjIsValidTarget
int objectIsDoor(Object* object)
{
    if (object == nullptr) {
        return false;
    }

    if (objectTypeFromPid(object->pid) != OBJ_TYPE_SCENERY) {
        return false;
    }

    Proto* proto;
    if (protoGetProto(object->pid, &proto) == -1) {
        return false;
    }

    return proto->scenery.type == SCENERY_TYPE_DOOR;
}

static void customMouseModeFrmsInit()
{
    configGetInt(&gContentConfig, CONTENT_CONFIG_SKILLDEX_SECTION, "first_aid", &(gGameMouseModeFrmIds[GAME_MOUSE_MODE_USE_FIRST_AID]), 293);
    configGetInt(&gContentConfig, CONTENT_CONFIG_SKILLDEX_SECTION, "doctor", &(gGameMouseModeFrmIds[GAME_MOUSE_MODE_USE_DOCTOR]), 293);
    configGetInt(&gContentConfig, CONTENT_CONFIG_SKILLDEX_SECTION, "lockpick", &(gGameMouseModeFrmIds[GAME_MOUSE_MODE_USE_LOCKPICK]), 293);
    configGetInt(&gContentConfig, CONTENT_CONFIG_SKILLDEX_SECTION, "steal", &(gGameMouseModeFrmIds[GAME_MOUSE_MODE_USE_STEAL]), 293);
    configGetInt(&gContentConfig, CONTENT_CONFIG_SKILLDEX_SECTION, "traps", &(gGameMouseModeFrmIds[GAME_MOUSE_MODE_USE_TRAPS]), 293);
    configGetInt(&gContentConfig, CONTENT_CONFIG_SKILLDEX_SECTION, "science", &(gGameMouseModeFrmIds[GAME_MOUSE_MODE_USE_SCIENCE]), 293);
    configGetInt(&gContentConfig, CONTENT_CONFIG_SKILLDEX_SECTION, "repair", &(gGameMouseModeFrmIds[GAME_MOUSE_MODE_USE_REPAIR]), 293);
}

void gameMouseRefreshImmediately()
{
    gameMouseRefresh();
    renderPresent();
}

Object* gmouse_get_outlined_object()
{
    return gGameMouseHighlightedItem;
}

} // namespace fallout
