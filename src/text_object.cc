#include "text_object.h"

#include <algorithm>
#include <string.h>

#include "debug.h"
#include "draw.h"
#include "input.h"
#include "memory.h"
#include "object.h"
#include "settings.h"
#include "svga.h"
#include "text_font.h"
#include "tile.h"
#include "word_wrap.h"
#include "multiplayer_log.h"

namespace fallout {

// The maximum number of text objects that can exist at the same time.
#define TEXT_OBJECTS_MAX_COUNT (200)

typedef enum TextObjectFlags {
    TEXT_OBJECT_MARKED_FOR_REMOVAL = 0x01,
    TEXT_OBJECT_UNBOUNDED = 0x02,
} TextObjectFlags;

typedef struct TextObject {
    int flags;
    Object* owner;
    unsigned int time;
    int linesCount;
    int sx;
    int sy;
    int tile;
    int x;
    int y;
    int width;
    int height;
    unsigned char* data;
} TextObject;

static void textObjectsTicker();
static void textObjectFindPlacement(TextObject* textObject);

// 0x51D944 text_object_index
static int gTextObjectsCount = 0;

// 0x51D948 text_object_base_delay
static unsigned int gTextObjectsBaseDelay = 3500;

// 0x51D94C text_object_line_delay
static unsigned int gTextObjectsLineDelay = 1399;

// 0x6681C0 text_object_list
static TextObject* gTextObjects[TEXT_OBJECTS_MAX_COUNT];

// 0x668210 display_width
static int gTextObjectsWindowWidth;

// 0x668214 display_height
static int gTextObjectsWindowHeight;

// 0x668218 display_buffer
static unsigned char* gTextObjectsWindowBuffer;

// 0x66821C text_object_enabled
static bool gTextObjectsEnabled;

// 0x668220 text_object_initialized
static bool gTextObjectsInitialized;

// 0x4B0130 text_object_init
int textObjectsInit(unsigned char* windowBuffer, int width, int height)
{
    if (gTextObjectsInitialized) {
        return -1;
    }

    gTextObjectsWindowBuffer = windowBuffer;
    gTextObjectsWindowWidth = width;
    gTextObjectsWindowHeight = height;
    gTextObjectsCount = 0;

    tickersAdd(textObjectsTicker);

    gTextObjectsBaseDelay = (unsigned int)(settings.preferences.text_base_delay * 1000.0);
    gTextObjectsLineDelay = (unsigned int)(settings.preferences.text_line_delay * 1000.0);

    gTextObjectsEnabled = true;
    gTextObjectsInitialized = true;

    return 0;
}

// 0x4B021C text_object_reset
int textObjectsReset()
{
    if (!gTextObjectsInitialized) {
        return -1;
    }

    for (int index = 0; index < gTextObjectsCount; index++) {
        internal_free(gTextObjects[index]->data);
        internal_free(gTextObjects[index]);
    }

    gTextObjectsCount = 0;
    tickersAdd(textObjectsTicker);

    return 0;
}

// 0x4B0280 text_object_exit
void textObjectsFree()
{
    if (gTextObjectsInitialized) {
        textObjectsReset();
        tickersRemove(textObjectsTicker);
        gTextObjectsInitialized = false;
    }
}

// 0x4B02A4 text_object_disable
void textObjectsDisable()
{
    gTextObjectsEnabled = false;
}

// 0x4B02B0 text_object_enable
void textObjectsEnable()
{
    gTextObjectsEnabled = true;
}

// 0x4B02C4 text_object_set_base_delay
void textObjectsSetBaseDelay(double value)
{
    if (value < 1.0) {
        value = 1.0;
    }

    gTextObjectsBaseDelay = (int)(value * 1000.0);
}

// 0x4B031C text_object_set_line_delay
void textObjectsSetLineDelay(double value)
{
    if (value < 0.0) {
        value = 0.0;
    }

    gTextObjectsLineDelay = (int)(value * 1000.0);
}

// text_object_create
// 0x4B036C text_object_create
static int textObjectAddInternal(Object* object, char* string, int font, int color, int outlineColor, Rect* rect, bool replacePrevious);
int textObjectAdd(Object* object, char* string, int font, int color, int outlineColor, Rect* rect)
{
    return textObjectAddInternal(object, string, font, color, outlineColor, rect, true);
}

// Co-op: like textObjectAdd, but never removes the owner's previous floats.
// Used by the synchronized dialogue floats so old lines stay up and fade
// naturally while the stack keeps them apart.
int textObjectAddNoReplace(Object* object, char* string, int font, int color, int outlineColor, Rect* rect)
{
    return textObjectAddInternal(object, string, font, color, outlineColor, rect, false);
}

// Co-op: returns the height a float created with the same string/font/outline
// would occupy, using the identical wrap math as textObjectAddInternal. The
// dialogue float stack shifts BEFORE adding the new line, so the newest float
// is never caught in its own shift (which would lift it into the middle of
// the stack and collide with the previous line). Returns 0 on failure.
int textObjectMeasure(char* string, int font, int outlineColor)
{
    if (string == nullptr || *string == '\0' || !gTextObjectsInitialized) {
        return 0;
    }

    int oldFont = fontGetCurrent();
    fontSetCurrent(font);

    short beginnings[WORD_WRAP_MAX_COUNT];
    short count;
    int height = 0;
    if (wordWrap(string, 200, beginnings, &count) == 0) {
        int lines = count - 1;
        if (lines < 1) {
            lines = 1;
        }
        height = (fontGetLineHeight() + 1) * lines;
        if (outlineColor != -1) {
            height += 2;
        }
    }

    fontSetCurrent(oldFont);
    return height;
}

static int textObjectAddInternal(Object* object, char* string, int font, int color, int outlineColor, Rect* rect, bool replacePrevious)
{
    if (!gTextObjectsInitialized) {
        return -1;
    }

    // SFALL: Fix incorrect value of the limit number of floating messages.
    if (gTextObjectsCount >= TEXT_OBJECTS_MAX_COUNT) {
        return -1;
    }

    if (string == nullptr) {
        return -1;
    }

    if (*string == '\0') {
        return -1;
    }

    TextObject* textObject = (TextObject*)internal_malloc(sizeof(*textObject));
    if (textObject == nullptr) {
        return -1;
    }

    memset(textObject, 0, sizeof(*textObject));

    int oldFont = fontGetCurrent();
    fontSetCurrent(font);

    short beginnings[WORD_WRAP_MAX_COUNT];
    short count;
    if (wordWrap(string, 200, beginnings, &count) != 0) {
        fontSetCurrent(oldFont);
        return -1;
    }

    textObject->linesCount = count - 1;
    if (textObject->linesCount < 1) {
        debugPrint("**Error in text_object_create()\n");
    }

    textObject->width = 0;

    for (int index = 0; index < textObject->linesCount; index++) {
        char* ending = string + beginnings[index + 1];
        char* beginning = string + beginnings[index];
        if (ending[-1] == ' ') {
            --ending;
        }

        char c = *ending;
        *ending = '\0';

        // NOTE: Calls `fontGetStringWidth` twice.
        textObject->width = std::max(textObject->width, fontGetStringWidth(beginning));

        *ending = c;
    }

    textObject->height = (fontGetLineHeight() + 1) * textObject->linesCount;

    if (outlineColor != -1) {
        textObject->width += 2;
        textObject->height += 2;
    }

    int size = textObject->width * textObject->height;
    textObject->data = (unsigned char*)internal_malloc(size);
    if (textObject->data == nullptr) {
        fontSetCurrent(oldFont);
        return -1;
    }

    memset(textObject->data, 0, size);

    unsigned char* dest = textObject->data;
    int skip = textObject->width * (fontGetLineHeight() + 1);

    if (outlineColor != -1) {
        dest += textObject->width;
    }

    for (int index = 0; index < textObject->linesCount; index++) {
        char* beginning = string + beginnings[index];
        char* ending = string + beginnings[index + 1];
        if (ending[-1] == ' ') {
            --ending;
        }

        char c = *ending;
        *ending = '\0';

        int width = fontGetStringWidth(beginning);
        fontDrawText(dest + (textObject->width - width) / 2, beginning, textObject->width, textObject->width, color);

        *ending = c;

        dest += skip;
    }

    if (outlineColor != -1) {
        bufferOutline(textObject->data, textObject->width, textObject->height, textObject->width, outlineColor);
    }

    if (object != nullptr) {
        textObject->tile = object->tile;
    } else {
        textObject->flags |= TEXT_OBJECT_UNBOUNDED;
        textObject->tile = gCenterTile;
    }

    textObjectFindPlacement(textObject);

    if (rect != nullptr) {
        rect->left = textObject->x;
        rect->top = textObject->y;
        rect->right = textObject->x + textObject->width - 1;
        rect->bottom = textObject->y + textObject->height - 1;
    }

    if (replacePrevious) {
        textObjectsRemoveByOwner(object);
    }

    textObject->owner = object;
    textObject->time = _get_bk_time();

    gTextObjects[gTextObjectsCount] = textObject;
    gTextObjectsCount++;

    static int sAddDiag = 0;
    if (sAddDiag < 20) {
        sAddDiag++;
        MpLog(MP_LOG_CHAT, "add owner=%p replace=%d lines=%d total=%d",
            (void*)object, replacePrevious ? 1 : 0, textObject->linesCount, gTextObjectsCount);
    }

    fontSetCurrent(oldFont);

    return 0;
}

// 0x4B06E8 text_object_render
void textObjectsRenderInRect(Rect* rect)
{
    if (!gTextObjectsInitialized) {
        return;
    }

    for (int index = 0; index < gTextObjectsCount; index++) {
        TextObject* textObject = gTextObjects[index];
        tileToScreenXY(textObject->tile, &(textObject->x), &(textObject->y));
        textObject->x += textObject->sx;
        textObject->y += textObject->sy;

        Rect textObjectRect;
        textObjectRect.left = textObject->x;
        textObjectRect.top = textObject->y;
        textObjectRect.right = textObject->width + textObject->x - 1;
        textObjectRect.bottom = textObject->height + textObject->y - 1;
        if (rectIntersection(&textObjectRect, rect, &textObjectRect) == 0) {
            blitBufferToBufferTrans(textObject->data + textObject->width * (textObjectRect.top - textObject->y) + (textObjectRect.left - textObject->x),
                textObjectRect.right - textObjectRect.left + 1,
                textObjectRect.bottom - textObjectRect.top + 1,
                textObject->width,
                gTextObjectsWindowBuffer + gTextObjectsWindowWidth * textObjectRect.top + textObjectRect.left,
                gTextObjectsWindowWidth);
        }
    }
}

// 0x4B07F0 text_object_count
int textObjectsGetCount()
{
    return gTextObjectsCount;
}

// 0x4B07F8 text_object_bk
static void textObjectsTicker()
{
    if (!gTextObjectsEnabled) {
        return;
    }

    bool textObjectsRemoved = false;
    Rect dirtyRect;

    for (int index = 0; index < gTextObjectsCount; index++) {
        TextObject* textObject = gTextObjects[index];

        unsigned int delay = gTextObjectsLineDelay * textObject->linesCount + gTextObjectsBaseDelay;
        if ((textObject->flags & TEXT_OBJECT_MARKED_FOR_REMOVAL) != 0 || (getTicksBetween(_get_bk_time(), textObject->time) > delay)) {
            static int sTickerDiag = 0;
            if (sTickerDiag < 20) {
                sTickerDiag++;
                MpLog(MP_LOG_CHAT, "ticker removed owner=%p age=%u lines=%d delay=%u marked=%d total=%d",
                    (void*)textObject->owner,
                    getTicksBetween(_get_bk_time(), textObject->time),
                    textObject->linesCount, delay,
                    (textObject->flags & TEXT_OBJECT_MARKED_FOR_REMOVAL) != 0 ? 1 : 0,
                    gTextObjectsCount);
            }
            tileToScreenXY(textObject->tile, &(textObject->x), &(textObject->y));
            textObject->x += textObject->sx;
            textObject->y += textObject->sy;

            Rect textObjectRect;
            textObjectRect.left = textObject->x;
            textObjectRect.top = textObject->y;
            textObjectRect.right = textObject->width + textObject->x - 1;
            textObjectRect.bottom = textObject->height + textObject->y - 1;

            if (textObjectsRemoved) {
                rectUnion(&dirtyRect, &textObjectRect, &dirtyRect);
            } else {
                rectCopy(&dirtyRect, &textObjectRect);
                textObjectsRemoved = true;
            }

            internal_free(textObject->data);
            internal_free(textObject);

            memmove(&(gTextObjects[index]), &(gTextObjects[index + 1]), sizeof(*gTextObjects) * (gTextObjectsCount - index - 1));

            gTextObjectsCount--;
            index--;
        }
    }

    if (textObjectsRemoved) {
        tileWindowRefreshRect(&dirtyRect, gElevation);
    }
}

// Finds best position for placing text object.
//
// 0x4B0954 text_object_get_offset
static void textObjectFindPlacement(TextObject* textObject)
{
    int tileScreenX;
    int tileScreenY;
    tileToScreenXY(textObject->tile, &tileScreenX, &tileScreenY);
    textObject->x = tileScreenX + 16 - textObject->width / 2;
    textObject->y = tileScreenY;

    if ((textObject->flags & TEXT_OBJECT_UNBOUNDED) == 0) {
        textObject->y -= textObject->height + 60;
    }

    if ((textObject->x >= 0 && textObject->x + textObject->width - 1 < gTextObjectsWindowWidth)
        && (textObject->y >= 0 && textObject->y + textObject->height - 1 < gTextObjectsWindowHeight)) {
        textObject->sx = textObject->x - tileScreenX;
        textObject->sy = textObject->y - tileScreenY;
        return;
    }

    textObject->x -= textObject->width / 2;
    if ((textObject->x >= 0 && textObject->x + textObject->width - 1 < gTextObjectsWindowWidth)
        && (textObject->y >= 0 && textObject->y + textObject->height - 1 < gTextObjectsWindowHeight)) {
        textObject->sx = textObject->x - tileScreenX;
        textObject->sy = textObject->y - tileScreenY;
        return;
    }

    textObject->x += textObject->width;
    if ((textObject->x >= 0 && textObject->x + textObject->width - 1 < gTextObjectsWindowWidth)
        && (textObject->y >= 0 && textObject->y + textObject->height - 1 < gTextObjectsWindowHeight)) {
        textObject->sx = textObject->x - tileScreenX;
        textObject->sy = textObject->y - tileScreenY;
        return;
    }

    textObject->x = tileScreenX - 16 - textObject->width;
    textObject->y = tileScreenY - 16 - textObject->height;
    if ((textObject->x >= 0 && textObject->x + textObject->width - 1 < gTextObjectsWindowWidth)
        && (textObject->y >= 0 && textObject->y + textObject->height - 1 < gTextObjectsWindowHeight)) {
        textObject->sx = textObject->x - tileScreenX;
        textObject->sy = textObject->y - tileScreenY;
        return;
    }

    textObject->x += textObject->width + 64;
    if ((textObject->x >= 0 && textObject->x + textObject->width - 1 < gTextObjectsWindowWidth)
        && (textObject->y >= 0 && textObject->y + textObject->height - 1 < gTextObjectsWindowHeight)) {
        textObject->sx = textObject->x - tileScreenX;
        textObject->sy = textObject->y - tileScreenY;
        return;
    }

    textObject->x = tileScreenX + 16 - textObject->width / 2;
    textObject->y = tileScreenY;
    if ((textObject->x >= 0 && textObject->x + textObject->width - 1 < gTextObjectsWindowWidth)
        && (textObject->y >= 0 && textObject->y + textObject->height - 1 < gTextObjectsWindowHeight)) {
        textObject->sx = textObject->x - tileScreenX;
        textObject->sy = textObject->y - tileScreenY;
        return;
    }

    textObject->x -= textObject->width / 2;
    if ((textObject->x >= 0 && textObject->x + textObject->width - 1 < gTextObjectsWindowWidth)
        && (textObject->y >= 0 && textObject->y + textObject->height - 1 < gTextObjectsWindowHeight)) {
        textObject->sx = textObject->x - tileScreenX;
        textObject->sy = textObject->y - tileScreenY;
        return;
    }

    textObject->x += textObject->width;
    if ((textObject->x >= 0 && textObject->x + textObject->width - 1 < gTextObjectsWindowWidth)
        && (textObject->y >= 0 && textObject->y + textObject->height - 1 < gTextObjectsWindowHeight)) {
        textObject->sx = textObject->x - tileScreenX;
        textObject->sy = textObject->y - tileScreenY;
        return;
    }

    textObject->x = tileScreenX + 16 - textObject->width / 2;
    textObject->y = tileScreenY - (textObject->height + 60);
    textObject->sx = textObject->x - tileScreenX;
    textObject->sy = textObject->y - tileScreenY;
}

// Marks text objects attached to [object] for removal.
//
// 0x4B0C00 text_object_remove
void textObjectsRemoveByOwner(Object* object)
{
    int removedCount = 0;
    for (int index = 0; index < gTextObjectsCount; index++) {
        if (gTextObjects[index]->owner == object) {
            gTextObjects[index]->flags |= TEXT_OBJECT_MARKED_FOR_REMOVAL;
            removedCount++;
        }
    }
    static int sRemoveDiag = 0;
    if (sRemoveDiag < 20) {
        sRemoveDiag++;
        MpLog(MP_LOG_CHAT, "removeByOwner owner=%p removed=%d total=%d",
            (void*)object, removedCount, gTextObjectsCount);
    }
}

// Co-op: shifts every live text object owned by [object] vertically by [dy]
// pixels. The offset persists for the object's lifetime. Used to stack
// dialogue floats: every new line pushes the older lines higher so the most
// recent text always sits closest to the critter. The old and new screen
// rects are refreshed so nothing ghosts.
void textObjectsShiftVertically(Object* object, int dy)
{
    if (dy == 0 || object == nullptr) {
        return;
    }

    for (int index = 0; index < gTextObjectsCount; index++) {
        TextObject* textObject = gTextObjects[index];
        if (textObject->owner != object) {
            continue;
        }

        Rect rect;
        tileToScreenXY(textObject->tile, &(rect.left), &(rect.top));
        rect.left += textObject->sx;
        rect.top += textObject->sy;
        rect.right = rect.left + textObject->width - 1;
        rect.bottom = rect.top + textObject->height - 1;

        textObject->sy += dy;

        Rect movedRect;
        tileToScreenXY(textObject->tile, &(movedRect.left), &(movedRect.top));
        movedRect.left += textObject->sx;
        movedRect.top += textObject->sy;
        movedRect.right = movedRect.left + textObject->width - 1;
        movedRect.bottom = movedRect.top + textObject->height - 1;

        rectUnion(&rect, &movedRect, &rect);
        tileWindowRefreshRect(&rect, gElevation);
    }
    static int sShiftDiag = 0;
    if (sShiftDiag < 20) {
        sShiftDiag++;
        MpLog(MP_LOG_CHAT, "shift owner=%p dy=%d total=%d", (void*)object, dy, gTextObjectsCount);
    }
}

// Co-op: returns the screen-Y of the lowest (bottom-most) live text object's
// bottom edge, or -1 when no text object is alive.
int textObjectsGetLowestBottomY()
{
    int lowest = -1;
    for (int index = 0; index < gTextObjectsCount; index++) {
        TextObject* textObject = gTextObjects[index];
        int x = 0;
        int y = 0;
        tileToScreenXY(textObject->tile, &x, &y);
        int bottom = y + textObject->sy + textObject->height - 1;
        if (bottom > lowest) {
            lowest = bottom;
        }
    }
    return lowest;
}

// Co-op: the uniform screen-space shift (0 when none) needed so a new float
// with the measured [floatHeight] anchored to [anchorTile] clears the lowest
// live float by 2px. Every old float must climb by this SAME amount — a
// per-owner shift would break the spacing between old floats, because owners
// on adjacent tiles (NPC and choosing player) anchor their floats to
// different tile screen positions and would move by different amounts.
int textObjectsComputeStackShift(int anchorTile, int floatHeight)
{
    if (floatHeight <= 0) {
        return 0;
    }

    int anchorY = 0;
    int unusedScreenX = 0;
    tileToScreenXY(anchorTile, &unusedScreenX, &anchorY);

    // Mirrors textObjectFindPlacement's in-bounds placement:
    // y = tileScreenY - height - 60.
    const int newFloatTop = anchorY - (floatHeight + 60);
    const int lowestBottom = textObjectsGetLowestBottomY();
    if (lowestBottom < 0) {
        return 0;
    }

    const int dy = lowestBottom - newFloatTop + 2;
    return dy > 0 ? dy : 0;
}

} // namespace fallout
