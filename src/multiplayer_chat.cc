// Co-op chat: the combat log extended into a full chat window.
//
// The chat is a half-transparent modal on the right side of the screen
// (2/5 width, 3/8 height — twice the width and 2.25x the height of the
// original 1/5 x 1/6 sizing — vertically centered), opened with the T key in
// and out of combat. It mirrors the combat log 1:1 — every line the green
// display monitor shows is appended verbatim — plus the players' user
// messages as "Name: text" with the name in that player's color. What a
// player writes also floats above their critter for a few seconds
// (engine text objects), in their color. Messages relay host-authoritatively
// over NET_PKT_CHAT_MESSAGE: client -> host (validated by peer) -> every
// other connected player.
//
// Transparency note: the engine's WINDOW_TRANSPARENT flag ghosts and is
// banned by the repo rules, so the translucency is honest — at open time the
// modal captures the screen region behind it and darkens it ~50% through a
// palette LUT; every frame that darkened snapshot is re-blitted into the
// window buffer before the text draws, which also erases the previous
// frame's glyphs without the DRAW_TEXT_FLAG_NO_BG hack.

#include "multiplayer_chat.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "art.h"
#include "color.h"
#include "critter.h"
#include "debug.h"
#include "game.h"
#include "game_sound.h"
#include "input.h"
#include "interface.h"
#include "kb.h"
#include "map.h"
#include "mouse.h"
#include "multiplayer.h"
#include "multiplayer_debug.h"
#include "multiplayer_log.h"
#include "multiplayer_worldmap.h"
#include "net.h"
#include "obj_types.h"
#include "scripts.h"
#include "svga.h"
#include "text_font.h"
#include "text_object.h"
#include "tile.h"
#include "window.h"
#include "window_manager.h"
#include "word_wrap.h"

namespace fallout {

namespace {

// Chat history capacity (ring). The display monitor holds 100 lines; the
// chat keeps 200 so user messages do not crowd combat lines out as fast.
#define MP_CHAT_MAX_LINES (200)

// Single line buffer length (characters, including the NUL).
#define MP_CHAT_LINE_LENGTH (160)

// Font used for the chat window and the floating text (same as nametags
// and the display monitor).
#define MP_CHAT_FONT (101)

// How much darker the captured background becomes (0..255, 255 = opaque).
#define MP_CHAT_DARKEN (128)

// Transcript-only mode auto-closes after this many ms without a new line,
// so it never covers the worldmap or the game indefinitely. Any appended
// line (user message or combat-log mirror) resets the timer.
#define MP_CHAT_TRANSCRIPT_IDLE_MS (8000)

struct ChatLine {
    char text[MP_CHAT_LINE_LENGTH];
    bool isUser;
    uint8_t netId; // sender for user lines, unused for combat lines
};

// Ring buffer of chat lines. Logical line l lives at
// gChatLines[(gChatHead + l) % MP_CHAT_MAX_LINES] for l in [0, gChatCount).
static ChatLine gChatLines[MP_CHAT_MAX_LINES];
static int gChatCount = 0;
static int gChatHead = 0;

// Tick of the last appended line; the transcript modal's idle auto-close
// counts from here (see MP_CHAT_TRANSCRIPT_IDLE_MS).
static uint32_t gChatLastActivity = 0;

// Scroll offset in lines; 0 = newest at the bottom.
static int gChatScroll = 0;

// The open chat window, -1 when closed. Only one instance exists.
static int gChatWindow = -1;

// Input buffer of the open modal.
static char gChatInput[MP_CHAT_MESSAGE_MAX_LENGTH + 1];

// Captured, darkened background of the open modal (winW * winH bytes).
static unsigned char* gChatBackground = nullptr;

// Palette darkening LUT (index -> ~50%-darker index), rebuilt at open.
static unsigned char gChatDarkLut[256];

void mpChatAppendLine(const ChatLine& line)
{
    if (gChatCount < MP_CHAT_MAX_LINES) {
        gChatLines[(gChatHead + gChatCount) % MP_CHAT_MAX_LINES] = line;
        gChatCount++;
    } else {
        gChatLines[gChatHead] = line;
        gChatHead = (gChatHead + 1) % MP_CHAT_MAX_LINES;
    }
    gChatScroll = 0; // stick to the newest lines
    gChatLastActivity = getTicks();
}

// The critter that owns [netId] on this side of the session. Client: the
// netId -> obj mirror, with gDude for the local player (the local critter
// is never in the netIdToObj mirror — log proof of the original bug:
// "MPCHAT: float skipped netId=<self> obj=0000000000000000" while the host
// floated the same message fine). The client resolves REMOTE players the
// same way the nametag pass does: the player slot's obj (populated by
// MpApplyPlayerState from the state mirror), falling back to the slot's
// objNetId and finally a plain netId lookup. A direct MpFindObjByNetId(1)
// on the client fails for the host's critter because player slot ids and
// object netIds are different namespaces ("float skipped netId=1" at 15:00).
Object* mpChatSenderCritter(uint8_t netId)
{
    if (!gMpActive || netId == 0 || netId > NET_MAX_PLAYERS) {
        return nullptr;
    }
    MultiplayerPlayer* p = &gMpSession.players[netId - 1];
    if (netId == gMpSession.localNetId && gDude != nullptr) {
        return gDude;
    }
    if (p->obj != nullptr) {
        return p->obj;
    }
    if (p->objNetId != 0) {
        return MpFindObjByNetId(p->objNetId);
    }
    return MpFindObjByNetId(netId);
}

const char* mpChatSenderName(uint8_t netId)
{
    if (gMpActive && netId >= 1 && netId <= NET_MAX_PLAYERS) {
        const MultiplayerPlayer* p = &gMpSession.players[netId - 1];
        if (p->isConnected && p->name[0] != '\0') {
            return p->name;
        }
    }
    if (gDude != nullptr) {
        return critterGetName(gDude);
    }
    return "?";
}

int mpChatSenderColor(uint8_t netId)
{
    Object* obj = mpChatSenderCritter(netId);
    if (obj != nullptr) {
        int color = MpPlayerColorFor(obj);
        if (color >= 0) {
            return color;
        }
    }
    return COLOR_LIGHT_GREEN_3;
}

// True when the sender's critter is plausibly on this player's screen — the
// message float would be visible there. Mirrors the nametag pass's own
// guards (hidden object, elevation, screen anchor) with a ~half-tile pad so
// a sprite poking into the viewport edge counts as visible.
bool mpChatSenderOnScreen(uint8_t netId)
{
    Object* obj = mpChatSenderCritter(netId);
    if (obj == nullptr || (obj->flags & OBJECT_HIDDEN) != 0
        || !tileIsValid(obj->tile)) {
        return false;
    }
    if (obj->elevation != gElevation) {
        return false;
    }
    int sx;
    int sy;
    if (tileToScreenXY(obj->tile, &sx, &sy) == -1) {
        return false;
    }
    sx += obj->x;
    sy += obj->y;
    const int marginX = 40; // about half a world tile (~80px wide)
    const int marginY = 20; // about half a tile's height (~40px)
    return sx >= -marginX && sx <= gSdlSurface->w + marginX
        && sy >= -marginY && sy <= gSdlSurface->h + marginY;
}

// Max pixel width of a chat float in the world; anything longer is
// truncated so it never spills past the viewport edge (the engine text
// objects draw a single line).
#define MP_CHAT_FLOAT_MAX_WIDTH (200)

// Truncate [text] in place to [maxWidth] pixels (font already selected).
void mpChatTruncateToWidth(char* text, int maxWidth);

// Float [text] above the sender's critter. World floats are the plain
// message in the default float color (yellow) WITHOUT the name — the
// colored "Name: text" form lives in the chat window only. Uses the engine
// text-object system (same as script float_msg chatter). Chat floats STACK
// instead of replacing: the plain textObjectAdd removes the critter's
// previous float (log-proven: the second message wiped the first), so the
// dialogue pattern is used — shift the live floats up by the new float's
// height, then add the new one at the bottom slot, where old lines climb
// and fade on their own lifetime. The whole stack is lifted so its bottom
// edge clears the nametag line (nametag: sy - artH - fontH - 2): chat text
// never sits below or at the same level as the name.
void mpChatFloatMessage(uint8_t netId, const char* text)
{
    Object* obj = mpChatSenderCritter(netId);
    if (obj == nullptr || (obj->flags & OBJECT_HIDDEN) != 0
        || obj->elevation != gElevation) {
        MpLog(MP_LOG_CHAT, "float skipped netId=%u obj=%p", netId, (void*)obj);
        return;
    }
    char buffer[MP_CHAT_MESSAGE_MAX_LENGTH + 1];
    strncpy(buffer, text, MP_CHAT_MESSAGE_MAX_LENGTH);
    buffer[MP_CHAT_MESSAGE_MAX_LENGTH] = '\0';
    int oldFont = fontGetCurrent();
    fontSetCurrent(MP_CHAT_FONT);
    mpChatTruncateToWidth(buffer, MP_CHAT_FLOAT_MAX_WIDTH);
    fontSetCurrent(oldFont);
    static int sFloatDiag = 0;
    if (sFloatDiag < 4) {
        sFloatDiag++;
        MpLog(MP_LOG_CHAT, "float netId=%u text='%s' color=%d", netId, buffer, COLOR_LIGHT_YELLOW);
    }

    // Stack first: shift the critter's live floats up by the new float's
    // height so the new line lands at the bottom slot (dialogue pattern).
    fontSetCurrent(MP_CHAT_FONT);
    int floatHeight = textObjectMeasure(buffer, MP_CHAT_FONT, COLOR_BLACK);
    fontSetCurrent(oldFont);
    int stackDy = 0;
    if (floatHeight > 0) {
        stackDy = textObjectsComputeStackShift(obj->tile, floatHeight);
        if (stackDy > 0) {
            textObjectsShiftVertically(obj, -stackDy);
        }
    }

    static int sFloatInDiag = 0;
    if (sFloatInDiag < 20) {
        sFloatInDiag++;
        MpLog(MP_LOG_CHAT, "float in netId=%u obj=%p tile=%d elev=%d total=%d stackDy=%d",
            netId, (void*)obj, obj->tile, obj->elevation, textObjectsGetCount(), stackDy);
    }

    Rect rect;
    // The float's lifetime stamp (time = _get_bk_time()) is taken inside
    // textObjectAddNoReplace from the CACHED ticker timestamp, which only
    // refreshes in tickersExecute — a pass this SDL-polling modal never
    // runs. Without a refresh here the float is stamped with the pre-open
    // value, so the main loop's first ticker pass after close sees an
    // inflated age (e.g. "age=5337 delay=4500" 19ms after the second
    // message) and erases the previous float instantly. Refresh the cache
    // so the stamp is the true wallclock add time.
    inputRefreshTickerTimestamp();
    if (textObjectAddNoReplace(obj, buffer, MP_CHAT_FONT, COLOR_LIGHT_YELLOW, COLOR_BLACK, &rect) != -1) {
        tileWindowRefreshRect(&rect, obj->elevation);

        // The nametag line sits at sy - artH - fontH - 2 above the tile
        // origin. If the float's bottom edge lands below the nametag's top,
        // shift every live float of this critter up so the gap stays clear.
        int tileScreenX;
        int tileScreenY;
        int liftDy = 0;
        if (tileToScreenXY(obj->tile, &tileScreenX, &tileScreenY) == 0) {
            tileScreenY += obj->y;
            int artW = 0;
            int artH = 0;
            CacheEntry* handle = nullptr;
            Art* art = artLock(obj->fid, &handle);
            if (art != nullptr) {
                artGetSize(art, obj->frame, obj->rotation, &artW, &artH);
                artUnlock(handle);
            }
            int oldFont = fontGetCurrent();
            fontSetCurrent(MP_CHAT_FONT);
            int fontH = fontGetLineHeight();
            fontSetCurrent(oldFont);

            int nametagTop = tileScreenY - artH - fontH - 2;
            liftDy = (nametagTop - 2) - rect.bottom;
            if (liftDy < 0) {
                textObjectsShiftVertically(obj, liftDy);
            }
        }

        static int sFloatOutDiag = 0;
        if (sFloatOutDiag < 20) {
            sFloatOutDiag++;
            MpLog(MP_LOG_CHAT, "float out netId=%u rc=0 rect=%d,%d-%d,%d liftDy=%d total=%d",
                netId, rect.left, rect.top, rect.right, rect.bottom, liftDy, textObjectsGetCount());
        }
    }
}

void mpChatAppendUserLine(uint8_t netId, const char* text)
{
    ChatLine line;
    memset(&line, 0, sizeof(line));
    strncpy(line.text, text, MP_CHAT_LINE_LENGTH - 1);
    line.isUser = true;
    line.netId = netId;
    mpChatAppendLine(line);
}

// Truncate [text] in place to [maxWidth] pixels (font already selected).
void mpChatTruncateToWidth(char* text, int maxWidth)
{
    size_t len = strlen(text);
    while (len > 0) {
        text[len] = '\0';
        if (fontGetStringWidth(text) <= maxWidth) {
            return;
        }
        len--;
    }
}

// Manual word-wrap: breaks at spaces when the row fills, hard-breaks long
// words (same policy as the engine's wordWrap, but measured with the exact
// same font metrics the draw uses, so the rows always fit). The per-character
// width is glyph width + letter spacing — the exact advance the draw applies —
// so a row never runs past the window edge. beginnings[0..n] delimit the n
// rows; row r is [beginnings[r], beginnings[r+1]).
int mpChatWrapManual(const char* text, int maxWidth, short* beginnings)
{
    int count = 0;
    beginnings[count++] = 0;

    int rowWidth = 0;
    int lastSpace = -1; // text index of the last space in the current row
    int letterSpacing = fontGetLetterSpacing();
    const char* p = text;
    while (*p != '\0') {
        int charWidth = fontGetCharacterWidth((unsigned char)*p) + letterSpacing;
        if (rowWidth + charWidth > maxWidth) {
            if (lastSpace >= 0) {
                // Break after the last space: the new row starts past it.
                if (count >= WORD_WRAP_MAX_COUNT) {
                    break;
                }
                beginnings[count++] = (short)(lastSpace + 1);
                p = text + lastSpace + 1;
                rowWidth = 0;
                lastSpace = -1;
                continue;
            }
            // No space in the row: hard-break before this character.
            if (count >= WORD_WRAP_MAX_COUNT) {
                break;
            }
            beginnings[count++] = (short)(p - text);
            rowWidth = charWidth;
            lastSpace = -1;
            p++;
            continue;
        }
        if (*p == ' ') {
            lastSpace = (int)(p - text);
        }
        rowWidth += charWidth;
        p++;
    }

    if (count >= WORD_WRAP_MAX_COUNT) {
        count = WORD_WRAP_MAX_COUNT - 1;
    }
    beginnings[count] = (short)strlen(text);
    return count;
}

// Wrap a chat line into [display] (NUL-terminated) and return how many
// display rows it occupies. User lines wrap as "Name: text" so the name
// stays on the first row in the sender's color; combat lines wrap plain.
// Logs the first wraps so the next session proves the row layout.
int mpChatWrapLine(const ChatLine& line, int maxTextWidth, char* display,
    size_t displaySize, short* beginnings)
{
    if (line.isUser) {
        char nameBuffer[64];
        snprintf(nameBuffer, sizeof(nameBuffer), "%s:", mpChatSenderName(line.netId));
        mpChatTruncateToWidth(nameBuffer, std::max(maxTextWidth - 8, 8));
        snprintf(display, displaySize, "%s %s", nameBuffer, line.text);
    } else {
        strncpy(display, line.text, displaySize - 1);
        display[displaySize - 1] = '\0';
    }

    int rows = mpChatWrapManual(display, maxTextWidth, beginnings);
    if (rows < 1) {
        beginnings[0] = 0;
        beginnings[1] = (short)strlen(display);
        rows = 1;
    }

    static int sWrapDiag = 0;
    if (sWrapDiag < 8) {
        sWrapDiag++;
        MpLog(MP_LOG_CHAT, "wrap rows=%d width=%d text='%s'",
            rows, maxTextWidth, display);
    }
    return rows;
}

// Build the palette darkening LUT from the current screen palette.
void mpChatBuildDarkLut()
{
    for (int index = 0; index < 256; index++) {
        gChatDarkLut[index] = (unsigned char)index;
    }
    if (gSdlSurface == nullptr || gSdlSurface->format->palette == nullptr) {
        return;
    }
    SDL_Palette* palette = gSdlSurface->format->palette;
    for (int index = 0; index < 256; index++) {
        const SDL_Color& src = palette->colors[index];
        int targetR = src.r / 2;
        int targetG = src.g / 2;
        int targetB = src.b / 2;
        int best = index;
        int bestDistance = 3 * 256 * 256;
        for (int candidate = 0; candidate < 256; candidate++) {
            const SDL_Color& c = palette->colors[candidate];
            int dr = c.r - targetR;
            int dg = c.g - targetG;
            int db = c.b - targetB;
            int distance = dr * dr + dg * dg + db * db;
            if (distance < bestDistance) {
                bestDistance = distance;
                best = candidate;
            }
        }
        gChatDarkLut[index] = (unsigned char)best;
    }
}

// Capture the screen region behind the modal and store it darkened. Called
// before the window exists, so the snapshot is the raw world frame.
void mpChatCaptureBackground(int x, int y, int width, int height)
{
    delete[] gChatBackground;
    gChatBackground = new unsigned char[(size_t)width * height];
    if (gSdlSurface == nullptr || x < 0 || y < 0
        || x + width > gSdlSurface->w || y + height > gSdlSurface->h) {
        memset(gChatBackground, 0, (size_t)width * height);
        return;
    }
    for (int row = 0; row < height; row++) {
        const unsigned char* srcRow = (const unsigned char*)gSdlSurface->pixels
            + (size_t)(y + row) * gSdlSurface->pitch + x;
        unsigned char* dstRow = gChatBackground + (size_t)row * width;
        for (int col = 0; col < width; col++) {
            dstRow[col] = gChatDarkLut[srcRow[col]];
        }
    }
}

// Redraw the whole modal: darkened background snapshot, border, chat lines
// (word-wrapped so nothing runs off the right edge — overflow moves to the
// next row), and a wrapping multi-line input field with a blinking caret on
// the last row. Typing past the right edge cuts to a new row instead of
// running off the window.
void mpChatRedraw(int win, int width, int height, bool transcriptOnly)
{
    unsigned char* buffer = windowGetBuffer(win);
    if (buffer == nullptr || gChatBackground == nullptr) {
        return;
    }
    memcpy(buffer, gChatBackground, (size_t)width * height);
    windowDrawBorder(win);

    int oldFont = fontGetCurrent();
    fontSetCurrent(MP_CHAT_FONT);

    int lineHeight = fontGetLineHeight();

    int maxTextWidth = width - 8;
    char display[MP_CHAT_LINE_LENGTH + 80];
    short beginnings[WORD_WRAP_MAX_COUNT];

    // The input wraps like the history: when the typed text reaches the
    // right edge it continues on the next row. Its height decides how many
    // history rows fit above it, so the wrap must run first. Transcript-only
    // mode has no input area — the history owns the whole window.
    bool caretVisible = (getTicks() % 800) < 400;
    char inputDisplay[MP_CHAT_MESSAGE_MAX_LENGTH + 4];
    short inputBeginnings[WORD_WRAP_MAX_COUNT];
    int inputRows = 0;
    int inputRowOffset = 0;
    if (!transcriptOnly) {
        snprintf(inputDisplay, sizeof(inputDisplay), "> %s", gChatInput);
        inputRows = mpChatWrapManual(inputDisplay, maxTextWidth, inputBeginnings);
        if (inputRows < 1) {
            inputRows = 1;
        }
        // A break right after a trailing space yields an empty final row
        // (boundary == strlen); drop it so the caret always rides the last
        // real text row, never a blank line.
        while (inputRows > 1
            && inputBeginnings[inputRows] <= inputBeginnings[inputRows - 1]) {
            inputRows--;
        }
        // Cap the input area so the history keeps room; when the input grows
        // past the cap the oldest rows scroll out of view, never the caret row.
        const int maxInputRows = 4;
        if (inputRows > maxInputRows) {
            inputRowOffset = inputRows - maxInputRows;
            inputRows = maxInputRows;
        }
    }

    int inputHeight = transcriptOnly ? 0 : inputRows * lineHeight + 6;
    int visibleRows = (height - inputHeight - 4) / lineHeight;
    if (visibleRows < 1) {
        visibleRows = 1;
    }

    // Pass 1: total wrapped row count, so the scroll range is in rows.
    int totalRows = 0;
    for (int lineIndex = 0; lineIndex < gChatCount; lineIndex++) {
        const ChatLine& line = gChatLines[(gChatHead + lineIndex) % MP_CHAT_MAX_LINES];
        totalRows += mpChatWrapLine(line, maxTextWidth, display, sizeof(display), beginnings);
    }

    int maxScroll = totalRows > visibleRows ? totalRows - visibleRows : 0;
    if (gChatScroll > maxScroll) {
        gChatScroll = maxScroll;
    }

    // Pass 2: draw the visible row window. Scroll 0 shows the NEWEST rows at
    // the bottom — the last message is always visible while chatting; higher
    // scroll values step back toward the oldest.
    int lastRow = totalRows - gChatScroll; // exclusive
    int firstRow = lastRow - visibleRows;
    if (firstRow < 0) {
        firstRow = 0;
    }

    int y = 3;
    int rowCursor = 0;
    for (int lineIndex = 0; lineIndex < gChatCount && rowCursor < lastRow; lineIndex++) {
        const ChatLine& line = gChatLines[(gChatHead + lineIndex) % MP_CHAT_MAX_LINES];
        int rows = mpChatWrapLine(line, maxTextWidth, display, sizeof(display), beginnings);
        int lineFirstRow = rowCursor;
        rowCursor += rows;
        if (rowCursor <= firstRow) {
            continue;
        }

        int nameLen = 0;
        if (line.isUser) {
            char nameBuffer[64];
            snprintf(nameBuffer, sizeof(nameBuffer), "%s:", mpChatSenderName(line.netId));
            mpChatTruncateToWidth(nameBuffer, std::max(maxTextWidth - 8, 8));
            nameLen = (int)strlen(nameBuffer);
        }

        for (int rowIndex = 0; rowIndex < rows; rowIndex++) {
            int rowAbs = lineFirstRow + rowIndex;
            if (rowAbs < firstRow || rowAbs >= lastRow) {
                continue;
            }
            int rowStart = beginnings[rowIndex];
            int rowEnd = beginnings[rowIndex + 1];
            if (rowEnd > rowStart && display[rowEnd - 1] == ' ') {
                rowEnd--; // wordWrap trims a trailing space from the row
            }
            if (rowEnd <= rowStart) {
                continue;
            }
            char saved = display[rowEnd];
            display[rowEnd] = '\0';

            unsigned char* row = buffer + (size_t)y * width;
            if (line.isUser && rowIndex == 0) {
                // Row 0: "Name: text" — the name in the sender's color, the
                // rest white. The wrap never breaks inside the name (it
                // breaks at spaces), so the name prefix is intact.
                int nameColor = mpChatSenderColor(line.netId);
                static int sColorDiag = 0;
                if (sColorDiag < 10) {
                    sColorDiag++;
                    MpLog(MP_LOG_CHAT, "name color netId=%u color=%d", line.netId, nameColor);
                }
                int colorLen = std::min(nameLen, rowEnd - rowStart);
                char namePart[80];
                strncpy(namePart, display + rowStart, colorLen);
                namePart[colorLen] = '\0';
                int colorW = fontGetStringWidth(namePart);
                fontDrawText(row + 4, namePart, colorW, width, nameColor);
                if (rowEnd - rowStart > colorLen) {
                    fontDrawText(row + 4 + colorW, display + rowStart + colorLen,
                        maxTextWidth - colorW, width, COLOR_WHITE);
                }
            } else {
                fontDrawText(row + 4, display + rowStart, maxTextWidth, width,
                    line.isUser ? COLOR_WHITE : COLOR_GREEN);
            }
            display[rowEnd] = saved;
            y += lineHeight;
        }
    }

    // Input area: the wrapped rows sit at the bottom, the caret rides the
    // last row. Rows are drawn from inputBeginnings (separate from the
    // history's beginnings, which the wrap pass above reused per line).
    // Transcript-only mode skips this entirely — no input, no caret.
    if (!transcriptOnly) {
        unsigned char* inputArea = buffer + (size_t)(height - inputHeight) * width;
        for (int drawnRow = 0; drawnRow < inputRows; drawnRow++) {
            int row = inputRowOffset + drawnRow;
            int rowStart = inputBeginnings[row];
            int rowEnd = inputBeginnings[row + 1];
            if (rowEnd > rowStart && inputDisplay[rowEnd - 1] == ' ') {
                rowEnd--; // the wrap trims a trailing space from the row
            }
            if (rowEnd <= rowStart && drawnRow != inputRows - 1) {
                continue;
            }
            // Row stride: lineHeight PIXEL rows per text row — a plain
            // drawnRow*width would stack every row on the same pixel line.
            unsigned char* inputRow = inputArea + (size_t)drawnRow * lineHeight * width;
            if (drawnRow == inputRows - 1) {
                // Final row: append the caret, then clip at the width limit.
                // If the row is exactly full the caret falls off, but the next
                // character wraps to a new row anyway.
                int len = rowEnd - rowStart;
                if (len < 0) {
                    len = 0;
                }
                char caretBuffer[MP_CHAT_MESSAGE_MAX_LENGTH + 8];
                memcpy(caretBuffer, inputDisplay + rowStart, len);
                caretBuffer[len] = caretVisible ? '|' : ' ';
                caretBuffer[len + 1] = '\0';
                mpChatTruncateToWidth(caretBuffer, maxTextWidth);
                fontDrawText(inputRow + 4, caretBuffer, maxTextWidth, width, COLOR_WHITE);
            } else {
                char saved = inputDisplay[rowEnd];
                inputDisplay[rowEnd] = '\0';
                fontDrawText(inputRow + 4, inputDisplay + rowStart, maxTextWidth, width, COLOR_WHITE);
                inputDisplay[rowEnd] = saved;
            }
        }
    }

    fontSetCurrent(oldFont);
}

} // namespace

// Append one UTF-8 text event to the chat input as engine 8-bit characters.
// The engine's key tables are US-scancode based with a French override only;
// on other physical layouts (e.g. Turkish-Q, where the 'i' key is at the US
// apostrophe scancode) the game's logical keys produce the wrong character
// (log proof: typing 'i' appended 0x27). SDL text input returns the
// layout-correct character, so the chat modal reads SDL events directly
// instead of the engine's key pipeline.
void mpChatAppendTextInput(const char* utf8)
{
    static int sTextDiag = 0;
    if (sTextDiag < 20) {
        sTextDiag++;
        MpLog(MP_LOG_CHAT, "text input '%s'", utf8);
    }
    size_t len = strlen(gChatInput);
    while (*utf8 != '\0' && len < MP_CHAT_MESSAGE_MAX_LENGTH) {
        unsigned char c = (unsigned char)*utf8;
        unsigned int cp = c;
        if ((c & 0xE0) == 0xC0 && (utf8[1] & 0xC0) == 0x80) {
            // 2-byte UTF-8: U+0080..U+07FF
            cp = ((c & 0x1F) << 6) | ((unsigned char)utf8[1] & 0x3F);
            utf8++;
        } else if ((c & 0xF0) == 0xE0 && (utf8[1] & 0xC0) == 0x80
            && (utf8[2] & 0xC0) == 0x80) {
            // 3-byte UTF-8: U+0800..U+FFFF
            cp = ((c & 0x0F) << 12) | (((unsigned char)utf8[1] & 0x3F) << 6)
                | ((unsigned char)utf8[2] & 0x3F);
            utf8 += 2;
        } else if ((c & 0xF8) == 0xF0) {
            // 4-byte: beyond the engine charset
            cp = 0;
            utf8 += 3;
        }
        utf8++;

        unsigned char engineChar;
        if (cp < 0x80) {
            engineChar = (unsigned char)cp; // ASCII passes through
        } else if (cp >= 0xA0 && cp <= 0xFF) {
            engineChar = (unsigned char)cp; // Latin-1 supplement (CP1252-ish)
        } else {
            // Turkish extras the engine fonts carry (CP1254 codes).
            switch (cp) {
            case 0x11E: engineChar = 0xD0; break; // Ğ
            case 0x11F: engineChar = 0xF0; break; // ğ
            case 0x130: engineChar = 0xDD; break; // İ
            case 0x131: engineChar = 0xFD; break; // ı
            case 0x15E: engineChar = 0xDE; break; // Ş
            case 0x15F: engineChar = 0xFE; break; // ş
            default: engineChar = '?'; break;
            }
        }
        gChatInput[len++] = (char)engineChar;
    }
    gChatInput[len] = '\0';
}

// Shared chat modal runner. transcriptOnly = passive transcript: full
// history + live appends + scrolling, but no input field, no text capture,
// no send keys, and an idle auto-close (MP_CHAT_TRANSCRIPT_IDLE_MS) so it
// never covers the game indefinitely. Full mode keeps the vanilla behavior:
// typing, Enter to send, ESC to dismiss.
static int mpChatRunModal(bool transcriptOnly)
{
    if (gChatWindow != -1 || gSdlSurface == nullptr) {
        return 0;
    }

    // Full mode: 2x the original 1/5 width, 2.25x the original 1/6 height.
    // Transcript mode is shorter (1/4 screen) — it only shows history.
    const int winWidth = screenGetWidth() * 2 / 5;
    const int winHeight = transcriptOnly
        ? screenGetHeight() / 4
        : screenGetHeight() * 3 / 8;
    if (winWidth < 40 || winHeight < 30) {
        return 0;
    }
    // Right side of the screen, vertically centered.
    const int winX = screenGetWidth() - winWidth;
    const int winY = (screenGetHeight() - winHeight) / 2;

    mpChatBuildDarkLut();
    mpChatCaptureBackground(winX, winY, winWidth, winHeight);

    int win = windowCreate(winX, winY, winWidth, winHeight, COLOR_BLACK,
        WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    if (win == -1) {
        MpLogAlways(MP_LOG_CHAT, "window create failed w=%d h=%d", winWidth, winHeight);
        delete[] gChatBackground;
        gChatBackground = nullptr;
        return 0;
    }

    soundPlayFile("ib1p1xx1");
    gChatWindow = win;
    gChatScroll = 0;
    gChatInput[0] = '\0';

    MpLog(MP_LOG_CHAT, "open %s x=%d y=%d w=%d h=%d",
        transcriptOnly ? "transcript" : "full", winX, winY, winWidth, winHeight);

    int sent = 0;
    if (!transcriptOnly) {
        SDL_StartTextInput();
    }
    // Idle auto-close base: count from open, not from the message that armed
    // the open (it may have arrived long ago, e.g. deferred through a modal).
    gChatLastActivity = getTicks();
    bool keepGoing = true;
    while (gChatWindow == win && keepGoing) {
        sharedFpsLimiter.mark();

        // The chat reads SDL events directly (text input + physical scan
        // codes) because the engine's logical-key tables are US-scancode
        // based — on other physical layouts printable keys come through
        // wrong (log proof: typing 'i' appended 0x27, the US apostrophe).
        // TEXTINPUT carries the layout-correct characters; KEYDOWN is used
        // only for the control keys, which sit on the same physical
        // positions on every layout. Transcript mode never starts text
        // input and ignores TEXTINPUT/TYPE keys entirely.
        SDL_Event event;
        while (SDL_PollEvent(&event) != 0) {
            switch (event.type) {
            case SDL_TEXTINPUT:
                if (!transcriptOnly) {
                    mpChatAppendTextInput(event.text.text);
                }
                break;
            case SDL_KEYDOWN:
                switch (event.key.keysym.scancode) {
                case SDL_SCANCODE_RETURN:
                    if (!transcriptOnly) {
                        if (gChatInput[0] != '\0') {
                            MpLog(MP_LOG_CHAT, "send text='%s'", gChatInput);
                            MpChatSendMessage(gChatInput);
                            sent = 1;
                        }
                        keepGoing = false;
                    }
                    break;
                case SDL_SCANCODE_ESCAPE:
                    keepGoing = false; // close without sending
                    break;
                case SDL_SCANCODE_BACKSPACE:
                    if (!transcriptOnly) {
                        size_t len = strlen(gChatInput);
                        if (len > 0) {
                            gChatInput[len - 1] = '\0';
                        }
                    }
                    break;
                case SDL_SCANCODE_UP:
                    gChatScroll++;
                    break;
                case SDL_SCANCODE_DOWN:
                    gChatScroll--;
                    if (gChatScroll < 0) {
                        gChatScroll = 0;
                    }
                    break;
                case SDL_SCANCODE_PAGEUP:
                    gChatScroll += 3;
                    break;
                case SDL_SCANCODE_PAGEDOWN:
                    gChatScroll -= 3;
                    if (gChatScroll < 0) {
                        gChatScroll = 0;
                    }
                    break;
                default:
                    break; // printable keys arrive via TEXTINPUT
                }
                break;
            case SDL_MOUSEWHEEL:
                if (event.wheel.y > 0) {
                    gChatScroll++;
                } else if (event.wheel.y < 0) {
                    gChatScroll--;
                }
                if (gChatScroll < 0) {
                    gChatScroll = 0;
                }
                break;
            case SDL_QUIT:
                // Mirror the engine's own SDL_QUIT handling (input.cc).
                SDL_StopTextInput();
                exit(EXIT_SUCCESS);
                break;
            default:
                break;
            }
        }

        // The chat must not stop the game (macu: "chat shouldnt block
        // anything"): pump what the main loop pumps — minus input routing
        // (the chat consumes keys via SDL directly, so inputGetInput is never
        // called and the game keys stay dead while typing). Tickers advance
        // animations and script timers, script requests (incl. combat) flow,
        // pending map transitions proceed, and the network keeps broadcasting
        // states — without this the host freezes the world for everyone and
        // the client's clicks die on the host's blocked main loop.
        tickersExecute();
        scriptsHandleRequests();
        mapHandleTransition();
        MpTick(); // live lines while the modal is up
        MpDrawPlayerIndicators();

        // Full redraw so the world visibly updates around the chat, then
        // re-capture the chat region from the LIVE frame — the world moves
        // under the darkened panel instead of showing the open-time snapshot.
        Rect fullScreen;
        fullScreen.left = 0;
        fullScreen.top = 0;
        fullScreen.right = screenGetWidth() - 1;
        fullScreen.bottom = screenGetHeight() - 1;
        windowRefreshAll(&fullScreen);
        mpChatCaptureBackground(winX, winY, winWidth, winHeight);
        mpChatRedraw(win, winWidth, winHeight, transcriptOnly);
        windowRefresh(win);
        renderPresent();
        sharedFpsLimiter.throttle();

        // Transcript auto-close: a silence gap closes the window. Any line
        // appended during the modal (MpTick above) resets the timer.
        if (transcriptOnly && getTicksSince(gChatLastActivity) >= MP_CHAT_TRANSCRIPT_IDLE_MS) {
            MpLog(MP_LOG_CHAT, "transcript idle close");
            keepGoing = false;
        }
    }
    if (!transcriptOnly) {
        SDL_StopTextInput();
    }

    // Drain any SDL events still queued around the close, then reset the
    // engine's key-repeat bookkeeping. This is the reopen-loop root cause:
    // the engine tracks every keydown in _GNW95_key_time_stamps and clears
    // the stamp only when IT processes the matching keyup. While the chat
    // modal polled SDL directly, the keyups of keys pressed around the open
    // (the T that opened the chat, etc.) were consumed by the modal and
    // never reached the engine — so the engine's per-frame repeat loop kept
    // synthesizing one T keydown per frame forever after the close, which
    // reopened the chat repeatedly (log proof: scan=23 keydowns every frame
    // after every close, with the keyboard state already up). Clearing the
    // stamps kills the phantom repeater at its source.
    SDL_Event drained;
    while (SDL_PollEvent(&drained)) {
    }
    _GNW95_clear_time_stamps();

    windowDestroy(win);
    gChatWindow = -1;
    delete[] gChatBackground;
    gChatBackground = nullptr;
    MpLog(MP_LOG_CHAT, "close %s input='%s' sent=%d",
        transcriptOnly ? "transcript" : "full", gChatInput, sent);
    return sent;
}

int MpChatShow()
{
    return mpChatRunModal(false);
}

int MpChatShowTranscriptOnly()
{
    return mpChatRunModal(true);
}

// Auto-open: armed by the incoming-message handlers when a line lands while
// the worldmap modal is up, or in-game when the sender's critter is off this
// player's screen; consumed by the worldmap loop's / main loop's per-frame
// check (never inside NetHostService — the modal blocks).
static bool gChatAutoOpenWanted = false;
static void mpChatNoteAutoOpen(uint8_t senderNetId);

void MpChatAppendCombatLine(const char* text)
{
    if (text == nullptr) {
        return;
    }
    ChatLine line;
    memset(&line, 0, sizeof(line));
    strncpy(line.text, text, MP_CHAT_LINE_LENGTH - 1);
    line.isUser = false;
    mpChatAppendLine(line);
}

void MpChatReset()
{
    gChatCount = 0;
    gChatHead = 0;
    gChatScroll = 0;
}

void MpChatSendMessage(const char* text)
{
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    char buffer[MP_CHAT_MESSAGE_MAX_LENGTH + 1];
    strncpy(buffer, text, MP_CHAT_MESSAGE_MAX_LENGTH);
    buffer[MP_CHAT_MESSAGE_MAX_LENGTH] = '\0';

    // Trim trailing whitespace.
    size_t len = strlen(buffer);
    while (len > 0 && (buffer[len - 1] == ' ' || buffer[len - 1] == '\t')) {
        buffer[--len] = '\0';
    }
    if (buffer[0] == '\0') {
        return;
    }

    MpLog(MP_LOG_CHAT, "local send text='%s'", buffer);

    if (!gMpActive) {
        // Single player: the chat is a combat-log viewer and the message
        // only shows locally.
        mpChatAppendUserLine(0, buffer);
        mpChatFloatMessage(0, buffer);
        return;
    }

    NetChatMessagePayload payload;
    memset(&payload, 0, sizeof(payload));
    payload.senderNetId = gMpSession.localNetId;
    strncpy(payload.text, buffer, MP_CHAT_MESSAGE_MAX_LENGTH);

    if (gMpIsClient) {
        // Local echo + float; the host validates and relays to the others.
        mpChatAppendUserLine(payload.senderNetId, payload.text);
        mpChatFloatMessage(payload.senderNetId, payload.text);
        if (gMpSession.hostPeer != nullptr) {
            NetSendPacket(gMpSession.hostPeer, NET_CHANNEL_RELIABLE,
                NET_PKT_CHAT_MESSAGE, &payload, sizeof(payload));
        } else {
            MpLogAlways(MP_LOG_CHAT, "client send dropped (no host peer)");
        }
    } else {
        // Host: the host path handles append/float/relay in one place.
        MpChatHostOnMessage(payload.senderNetId, payload.text);
    }
}

void MpChatHostOnMessage(uint8_t senderNetId, const char* text)
{
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    if (!gMpActive) {
        mpChatAppendUserLine(senderNetId, text);
        mpChatFloatMessage(senderNetId, text);
        return;
    }
    if (senderNetId == 0 || senderNetId > NET_MAX_PLAYERS) {
        MpLogAlways(MP_LOG_CHAT, "host reject out-of-range sender netId=%u", senderNetId);
        return;
    }
    const MultiplayerPlayer* sender = &gMpSession.players[senderNetId - 1];
    if (!sender->isConnected) {
        MpLogAlways(MP_LOG_CHAT, "host reject disconnected sender netId=%u", senderNetId);
        return;
    }

    MpLog(MP_LOG_CHAT, "host relay netId=%u text='%s'", senderNetId, text);

    mpChatAppendUserLine(senderNetId, text);
    mpChatFloatMessage(senderNetId, text);
    mpChatNoteAutoOpen(senderNetId);

    // Forward to every other connected player. The sender already has its
    // own line and never gets an echo.
    NetChatMessagePayload payload;
    memset(&payload, 0, sizeof(payload));
    payload.senderNetId = senderNetId;
    strncpy(payload.text, text, MP_CHAT_MESSAGE_MAX_LENGTH);
    payload.text[MP_CHAT_MESSAGE_MAX_LENGTH] = '\0';

    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        const MultiplayerPlayer* player = &gMpSession.players[index];
        if (!player->isConnected || player->peer == nullptr
            || player->netId == senderNetId) {
            continue;
        }
        NetSendPacket(player->peer, NET_CHANNEL_RELIABLE,
            NET_PKT_CHAT_MESSAGE, &payload, sizeof(payload));
    }
}

void MpChatClientOnIncoming(uint8_t senderNetId, const char* text)
{
    if (!gMpIsClient || !gMpActive || text == nullptr || text[0] == '\0') {
        return;
    }
    MpLog(MP_LOG_CHAT, "client received netId=%u text='%s'", senderNetId, text);
    mpChatAppendUserLine(senderNetId, text);
    mpChatFloatMessage(senderNetId, text);
    mpChatNoteAutoOpen(senderNetId);
}

// Arm the transcript auto-open when the incoming line is invisible to this
// player: on the worldmap (no world view at all), or in-game when the
// sender's critter is off this player's screen (the float would not show).
// Own echoes are excluded — the typist is looking at their own text.
static void mpChatNoteAutoOpen(uint8_t senderNetId)
{
    if (gChatWindow != -1 || !gMpActive) {
        return;
    }
    if (senderNetId == 0 || senderNetId > NET_MAX_PLAYERS
        || senderNetId == gMpSession.localNetId) {
        return;
    }
    if (gMpWorldmapActive) {
        gChatAutoOpenWanted = true;
        return;
    }
    if (!mpChatSenderOnScreen(senderNetId)) {
        gChatAutoOpenWanted = true;
    }
}

void MpChatAutoOpenCheck()
{
    if (!gChatAutoOpenWanted) {
        return;
    }
    gChatAutoOpenWanted = false;
    if (!gMpWorldmapActive) {
        // In-game auto-open only in normal world mode; a modal context
        // (dialogue, barter, pipboy, ...) keeps the flag armed and the next
        // main-loop check fires after the modal closes — the line is not
        // lost, just deferred.
        if (!interfaceBarEnabled()) {
            gChatAutoOpenWanted = true;
            return;
        }
        MpLog(MP_LOG_CHAT, "auto-open transcript (sender off screen)");
    } else {
        MpLog(MP_LOG_CHAT, "auto-open transcript (worldmap)");
    }
    MpChatShowTranscriptOnly();
}

void MpChatAutoOpenCancel()
{
    gChatAutoOpenWanted = false;
}

} // namespace fallout
