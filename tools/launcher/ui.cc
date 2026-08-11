#include "ui.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>

namespace ui {

// ---------------------------------------------------------------------------
// globals
// ---------------------------------------------------------------------------

namespace {

SDL_Renderer* g_renderer = nullptr;
SDL_Window* g_window = nullptr;
const unsigned char* g_ttfData = nullptr;
unsigned int g_ttfSize = 0;

float g_scale = 1.0f;
float g_canvasW = 0.0f;
float g_canvasH = 0.0f;
float g_dt = 0.016f;
float g_time = 0.0f;

float g_mx = 0, g_my = 0;
bool g_leftDown = false;
bool g_clicked = false;    // left pressed this frame
bool g_released = false;   // left released this frame
bool g_clickEaten = false; // a popup handled this frame's click; widgets must ignore it
float g_wheelDelta = 0.0f;

std::string g_textInput;
struct KeyEvent {
    SDL_Keycode key;
    bool ctrl;
    bool shift;
};
std::vector<KeyEvent> g_keys;

std::string g_focusId;
std::string g_openCombo;

struct WState {
    float hoverT = 0;
    float pressT = 0;
    float knobT = 0;
    float dispFrac = 0;
    float scrollX = 0;
    int cursor = -1;
    int anchor = -1;
    bool downInside = false;
};
std::unordered_map<std::string, WState> g_state;

WState& st(const char* id) { return g_state[id]; }

float approach(float current, float target, float speed)
{
    float t = std::min(1.0f, g_dt * speed);
    return current + (target - current) * t;
}

bool mouseInside(float x, float y, float w, float h)
{
    return g_mx >= x && g_mx < x + w && g_my >= y && g_my < y + h;
}

} // namespace

// ---------------------------------------------------------------------------
// colors
// ---------------------------------------------------------------------------

Color lerp(const Color& a, const Color& b, float t)
{
    t = std::max(0.0f, std::min(1.0f, t));
    return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
             a.a + (b.a - a.a) * t };
}

namespace palette {
const Color bg{ 0.090f, 0.094f, 0.110f, 1.0f };          // #17181C
const Color card{ 0.122f, 0.129f, 0.157f, 1.0f };        // #1F2128
const Color input{ 0.149f, 0.161f, 0.200f, 1.0f };       // #262933
const Color inputHover{ 0.176f, 0.188f, 0.231f, 1.0f };  // #2D303B
const Color border{ 0.180f, 0.196f, 0.235f, 1.0f };      // #2E323C
const Color text{ 0.925f, 0.933f, 0.949f, 1.0f };        // #ECEEF2
const Color textDim{ 0.608f, 0.631f, 0.682f, 1.0f };     // #9BA1AE
const Color accent{ 0.357f, 0.549f, 1.0f, 1.0f };        // #5B8CFF
const Color accentHover{ 0.431f, 0.608f, 1.0f, 1.0f };   // #6E9BFF
const Color accentDown{ 0.290f, 0.471f, 0.902f, 1.0f };  // #4A78E6
const Color error{ 0.898f, 0.392f, 0.424f, 1.0f };       // #E5646C
const Color success{ 0.298f, 0.765f, 0.541f, 1.0f };     // #4CC38A
const Color track{ 0.165f, 0.176f, 0.216f, 1.0f };       // #2A2D37
const Color popup{ 0.149f, 0.161f, 0.200f, 1.0f };       // #262933
} // namespace palette

namespace {

SDL_Color toSDL(const Color& c)
{
    return { (Uint8)(c.r * 255), (Uint8)(c.g * 255), (Uint8)(c.b * 255), (Uint8)(c.a * 255) };
}

} // namespace

// ---------------------------------------------------------------------------
// utf-8 helpers
// ---------------------------------------------------------------------------

namespace {

// Decode codepoint at byte index i; returns byte length (0 on error).
int utf8DecodeAt(const std::string& s, size_t i, unsigned int& cp)
{
    if (i >= s.size())
        return 0;
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) {
        cp = c;
        return 1;
    }
    if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
        cp = ((c & 0x1F) << 6) | ((unsigned char)s[i + 1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
        cp = ((c & 0x0F) << 12) | (((unsigned char)s[i + 1] & 0x3F) << 6) |
             ((unsigned char)s[i + 2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
        cp = ((c & 0x07) << 18) | (((unsigned char)s[i + 1] & 0x3F) << 12) |
             (((unsigned char)s[i + 2] & 0x3F) << 6) | ((unsigned char)s[i + 3] & 0x3F);
        return 4;
    }
    cp = '?';
    return 1;
}

size_t utf8Prev(const std::string& s, size_t i)
{
    if (i == 0)
        return 0;
    size_t j = i - 1;
    while (j > 0 && ((unsigned char)s[j] & 0xC0) == 0x80)
        --j;
    return j;
}

size_t utf8Next(const std::string& s, size_t i)
{
    unsigned int cp;
    int len = utf8DecodeAt(s, i, cp);
    return i + (len > 0 ? (size_t)len : 1);
}

} // namespace

// ---------------------------------------------------------------------------
// font atlases (stb_truetype, baked at 2x for smoothness)
// ---------------------------------------------------------------------------

namespace {

struct BakedFont {
    SDL_Texture* tex = nullptr;
    stbtt_bakedchar chars[224];
    int atlasW = 0, atlasH = 0;
    float ascentPx = 0; // in baked pixels
};
std::map<int, BakedFont> g_fonts;

BakedFont& getFont(int pixelH)
{
    auto it = g_fonts.find(pixelH);
    if (it != g_fonts.end())
        return it->second;

    BakedFont f;
    std::memset(f.chars, 0, sizeof(f.chars));
    f.atlasW = 1024;
    f.atlasH = 512;
    std::vector<unsigned char> buf((size_t)f.atlasW * f.atlasH);

    int res = stbtt_BakeFontBitmap(g_ttfData, 0, (float)pixelH, buf.data(), f.atlasW, f.atlasH,
                                   32, 224, f.chars);
    if (res == 0) {
        f.atlasH = 1024;
        buf.assign((size_t)f.atlasW * f.atlasH, 0);
        res = stbtt_BakeFontBitmap(g_ttfData, 0, (float)pixelH, buf.data(), f.atlasW, f.atlasH,
                                   32, 224, f.chars);
    }

    SDL_Surface* surf =
        SDL_CreateRGBSurfaceWithFormat(0, f.atlasW, f.atlasH, 32, SDL_PIXELFORMAT_RGBA32);
    if (surf && res != 0) {
        SDL_LockSurface(surf);
        unsigned char* px = (unsigned char*)surf->pixels;
        size_t n = (size_t)f.atlasW * f.atlasH;
        for (size_t i = 0; i < n; ++i) {
            px[i * 4 + 0] = 255;
            px[i * 4 + 1] = 255;
            px[i * 4 + 2] = 255;
            px[i * 4 + 3] = buf[i];
        }
        SDL_UnlockSurface(surf);
        f.tex = SDL_CreateTextureFromSurface(g_renderer, surf);
        if (f.tex)
            SDL_SetTextureBlendMode(f.tex, SDL_BLENDMODE_BLEND);
    }
    if (surf)
        SDL_FreeSurface(surf);

    stbtt_fontinfo fi;
    if (stbtt_InitFont(&fi, g_ttfData, 0)) {
        float s = stbtt_ScaleForPixelHeight(&fi, (float)pixelH);
        int ascent = 0, descent = 0, lineGap = 0;
        stbtt_GetFontVMetrics(&fi, &ascent, &descent, &lineGap);
        f.ascentPx = (float)ascent * s;
    }

    auto inserted = g_fonts.emplace(pixelH, f);
    return inserted.first->second;
}

// Walk a string, optionally drawing it. Returns width in baked pixels.
float walkText(float size, const std::string& str, float drawX, float drawTopY, const Color* color)
{
    int pixelH = std::max(8, (int)std::lround(size * g_scale * 2.0f));
    BakedFont& f = getFont(pixelH);
    if (!f.tex)
        return 0;
    float factor = (size * g_scale) / (float)pixelH;
    float baseline = drawTopY * g_scale + f.ascentPx * factor;

    SDL_Color sdlc = color ? toSDL(*color) : SDL_Color{ 255, 255, 255, 255 };
    SDL_SetTextureColorMod(f.tex, sdlc.r, sdlc.g, sdlc.b);
    SDL_SetTextureAlphaMod(f.tex, sdlc.a);

    float bx = 0, by = 0;
    size_t i = 0;
    while (i < str.size()) {
        unsigned int cp;
        int len = utf8DecodeAt(str, i, cp);
        if (len == 0)
            break;
        i += (size_t)len;
        if (cp < 32 || cp >= 256)
            cp = '?';
        stbtt_aligned_quad q;
        stbtt_GetBakedQuad(f.chars, f.atlasW, f.atlasH, (int)cp - 32, &bx, &by, &q, 0);
        if (color) {
            SDL_Rect src{ (int)std::lround(q.s0 * f.atlasW), (int)std::lround(q.t0 * f.atlasH),
                          (int)std::lround((q.s1 - q.s0) * f.atlasW),
                          (int)std::lround((q.t1 - q.t0) * f.atlasH) };
            SDL_FRect dst{ drawX * g_scale + q.x0 * factor, baseline + q.y0 * factor,
                           (q.x1 - q.x0) * factor, (q.y1 - q.y0) * factor };
            SDL_RenderCopyF(g_renderer, f.tex, &src, &dst);
        }
    }
    return bx;
}

} // namespace

// ---------------------------------------------------------------------------
// rounded rect textures (SDF antialiasing)
// ---------------------------------------------------------------------------

namespace {

using RectTexKey = std::tuple<int, int, int, int, int>; // w, h, r, border, thickness
std::map<RectTexKey, SDL_Texture*> g_rectTex;

float sdRoundRect(float px, float py, float hx, float hy, float r)
{
    float qx = std::fabs(px) - (hx - r);
    float qy = std::fabs(py) - (hy - r);
    float ox = std::max(qx, 0.0f);
    float oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

SDL_Texture* getRectTexture(int w, int h, int r, bool border, int thickness)
{
    RectTexKey key{ w, h, r, border ? 1 : 0, thickness };
    auto it = g_rectTex.find(key);
    if (it != g_rectTex.end())
        return it->second;

    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    SDL_Texture* tex = nullptr;
    if (surf) {
        SDL_LockSurface(surf);
        unsigned char* px = (unsigned char*)surf->pixels;
        float hx = w * 0.5f, hy = h * 0.5f;
        float rr = std::min((float)r, std::min(hx, hy));
        float t = (float)thickness;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float d = sdRoundRect(x + 0.5f - hx, y + 0.5f - hy, hx, hy, rr);
                float alpha;
                if (!border) {
                    alpha = std::max(0.0f, std::min(1.0f, 0.5f - d));
                } else {
                    float dInner = sdRoundRect(x + 0.5f - hx, y + 0.5f - hy,
                                               std::max(1.0f, hx - t), std::max(1.0f, hy - t),
                                               std::max(0.0f, rr - t));
                    alpha = std::max(0.0f, std::min(1.0f, std::min(0.5f - d, dInner + 0.5f)));
                }
                size_t idx = ((size_t)y * w + x) * 4;
                px[idx + 0] = 255;
                px[idx + 1] = 255;
                px[idx + 2] = 255;
                px[idx + 3] = (unsigned char)(alpha * 255);
            }
        }
        SDL_UnlockSurface(surf);
        tex = SDL_CreateTextureFromSurface(g_renderer, surf);
        if (tex)
            SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_FreeSurface(surf);
    }
    g_rectTex[key] = tex;
    return tex;
}

void drawRectTexture(SDL_Texture* tex, float x, float y, float w, float h, const Color& c)
{
    if (!tex)
        return;
    SDL_Color sdlc = toSDL(c);
    SDL_SetTextureColorMod(tex, sdlc.r, sdlc.g, sdlc.b);
    SDL_SetTextureAlphaMod(tex, sdlc.a);
    SDL_FRect dst{ x * g_scale, y * g_scale, w * g_scale, h * g_scale };
    SDL_RenderCopyF(g_renderer, tex, nullptr, &dst);
}

} // namespace

// ---------------------------------------------------------------------------
// public api: lifecycle, events, primitives
// ---------------------------------------------------------------------------

void init(SDL_Renderer* renderer, const unsigned char* ttfData, unsigned int ttfSize)
{
    g_renderer = renderer;
    g_window = SDL_RenderGetWindow(renderer);
    g_ttfData = ttfData;
    g_ttfSize = ttfSize;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_SetHint(SDL_HINT_RENDER_LOGICAL_SIZE_MODE, "letterbox");
}

void shutdown()
{
    for (auto& kv : g_fonts)
        if (kv.second.tex)
            SDL_DestroyTexture(kv.second.tex);
    g_fonts.clear();
    for (auto& kv : g_rectTex)
        if (kv.second)
            SDL_DestroyTexture(kv.second);
    g_rectTex.clear();
}

void setCanvas(float w, float h)
{
    g_canvasW = w;
    g_canvasH = h;
    if (g_renderer != nullptr)
        SDL_RenderSetLogicalSize(g_renderer, (int)w, (int)h);
}

float scale() { return g_scale; }

void beginFrame(float dt)
{
    g_dt = dt;
    g_time += dt;
    g_clicked = false;
    g_released = false;
    g_clickEaten = false;
    g_wheelDelta = 0.0f;
    g_textInput.clear();
    g_keys.clear();
}

void handleEvent(const SDL_Event& e)
{
    switch (e.type) {
    case SDL_MOUSEBUTTONDOWN:
        if (e.button.button == SDL_BUTTON_LEFT)
            g_clicked = true;
        break;
    case SDL_MOUSEBUTTONUP:
        if (e.button.button == SDL_BUTTON_LEFT)
            g_released = true;
        break;
    case SDL_MOUSEWHEEL:
        g_wheelDelta += (float)e.wheel.y;
        break;
    case SDL_TEXTINPUT:
        g_textInput += e.text.text;
        break;
    case SDL_KEYDOWN: {
        Uint16 mod = e.key.keysym.mod;
        g_keys.push_back({ e.key.keysym.sym, (mod & KMOD_CTRL) != 0, (mod & KMOD_SHIFT) != 0 });
        break;
    }
    default:
        break;
    }
}

void setMouse(float x, float y, bool leftDown)
{
    if (g_window != nullptr && g_canvasW > 0.0f && g_canvasH > 0.0f) {
        // Map window coordinates into the logical canvas, accounting for the
        // uniform scale and the letterbox offset.
        int ww = 0, wh = 0;
        SDL_GetWindowSize(g_window, &ww, &wh);
        float layoutScale = std::min((float)ww / g_canvasW, (float)wh / g_canvasH);
        if (layoutScale <= 0.0f)
            layoutScale = 1.0f;
        float offsetX = ((float)ww - g_canvasW * layoutScale) * 0.5f;
        float offsetY = ((float)wh - g_canvasH * layoutScale) * 0.5f;
        g_mx = (x - offsetX) / layoutScale;
        g_my = (y - offsetY) / layoutScale;
    } else {
        g_mx = x;
        g_my = y;
    }
    g_leftDown = leftDown;
}

bool mouseClicked() { return g_clicked; }

float mouseWheelDelta() { return g_wheelDelta; }

bool mouseOver(float x, float y, float w, float h) { return mouseInside(x, y, w, h); }

void roundedRect(float x, float y, float w, float h, float r, const Color& c)
{
    if (c.a <= 0 || w <= 0 || h <= 0)
        return;
    int pw = std::max(1, (int)std::lround(w * g_scale));
    int ph = std::max(1, (int)std::lround(h * g_scale));
    int pr = std::max(0, (int)std::lround(r * g_scale));
    drawRectTexture(getRectTexture(pw, ph, pr, false, 0), x, y, w, h, c);
}

void roundedRectBorder(float x, float y, float w, float h, float r, float thickness,
                       const Color& c)
{
    if (c.a <= 0 || w <= 0 || h <= 0)
        return;
    int pw = std::max(1, (int)std::lround(w * g_scale));
    int ph = std::max(1, (int)std::lround(h * g_scale));
    int pr = std::max(0, (int)std::lround(r * g_scale));
    int pt = std::max(1, (int)std::lround(thickness * g_scale));
    drawRectTexture(getRectTexture(pw, ph, pr, true, pt), x, y, w, h, c);
}

void drawText(float x, float y, float size, const Color& c, const std::string& str)
{
    if (str.empty() || c.a <= 0)
        return;
    walkText(size, str, x, y, &c);
}

void drawTextCenter(float cx, float y, float size, const Color& c, const std::string& str)
{
    float w = textWidth(size, str);
    drawText(cx - w * 0.5f, y, size, c, str);
}

void drawTextRight(float right, float y, float size, const Color& c, const std::string& str)
{
    float w = textWidth(size, str);
    drawText(right - w, y, size, c, str);
}

float textWidth(float size, const std::string& str)
{
    if (str.empty())
        return 0;
    int pixelH = std::max(8, (int)std::lround(size * g_scale * 2.0f));
    float factor = (size * g_scale) / (float)pixelH;
    float baked = walkText(size, str, 0, 0, nullptr);
    return baked * factor / g_scale;
}

float lineHeight(float size) { return size * 1.21f; }

// ---------------------------------------------------------------------------
// widgets
// ---------------------------------------------------------------------------

bool button(const char* id, float x, float y, float w, float h, const std::string& label,
            ButtonStyle style, bool enabled)
{
    WState& s = st(id);
    bool hovered = enabled && mouseInside(x, y, w, h);
    bool down = hovered && g_leftDown;

    s.hoverT = approach(s.hoverT, hovered ? 1.0f : 0.0f, 10.0f);
    s.pressT = approach(s.pressT, down ? 1.0f : 0.0f, 16.0f);

    bool clicked = false;
    if (g_clicked && hovered && !g_clickEaten)
        s.downInside = true;
    if (g_released) {
        if (enabled && s.downInside && hovered)
            clicked = true;
        s.downInside = false;
    }

    float r = 8.0f;
    Color bgc, textColor;
    if (style == ButtonStyle::Accent) {
        bgc = lerp(palette::accent, palette::accentHover, s.hoverT);
        bgc = lerp(bgc, palette::accentDown, s.pressT);
        if (!enabled)
            bgc = lerp(palette::track, palette::card, 0.35f);
        textColor = enabled ? palette::text : palette::textDim;
    } else {
        bgc = lerp(palette::input, palette::inputHover, s.hoverT);
        if (!enabled)
            bgc = palette::track;
        textColor = enabled ? palette::text : palette::textDim;
    }

    roundedRect(x, y, w, h, r, bgc);
    if (style == ButtonStyle::Subtle)
        roundedRectBorder(x + 0.5f, y + 0.5f, w - 1, h - 1, r, 1.0f, palette::border);
    drawTextCenter(x + w * 0.5f, y + (h - lineHeight(14)) * 0.5f, 14, textColor, label);
    return clicked;
}

namespace {

// Byte offset in `value` closest to pixel position relX (logical units,
// relative to text start).
int hitTestText(float size, const std::string& value, float relX)
{
    int pixelH = std::max(8, (int)std::lround(size * g_scale * 2.0f));
    float factor = (size * g_scale) / (float)pixelH;
    float targetBaked = relX * g_scale / factor;

    BakedFont& f = getFont(pixelH);
    float bx = 0, by = 0;
    size_t i = 0;
    while (i < value.size()) {
        unsigned int cp;
        int len = utf8DecodeAt(value, i, cp);
        if (len == 0)
            break;
        if (cp < 32 || cp >= 256)
            cp = '?';
        float prevBx = bx;
        stbtt_aligned_quad q;
        stbtt_GetBakedQuad(f.chars, f.atlasW, f.atlasH, (int)cp - 32, &bx, &by, &q, 0);
        if ((prevBx + bx) * 0.5f > targetBaked)
            return (int)i;
        i += (size_t)len;
    }
    return (int)value.size();
}

void deleteSelection(std::string& value, WState& s)
{
    int a = std::min(s.anchor, s.cursor);
    int b = std::max(s.anchor, s.cursor);
    value.erase((size_t)a, (size_t)(b - a));
    s.cursor = a;
    s.anchor = a;
}

void processInputKeys(const char* id, std::string& value, WState& s)
{
    // typed text
    if (!g_textInput.empty()) {
        if (s.anchor != s.cursor)
            deleteSelection(value, s);
        value.insert((size_t)s.cursor, g_textInput);
        s.cursor += (int)g_textInput.size();
        s.anchor = s.cursor;
    }

    for (const KeyEvent& k : g_keys) {
        bool hasSel = s.anchor != s.cursor;
        switch (k.key) {
        case SDLK_BACKSPACE:
            if (hasSel) {
                deleteSelection(value, s);
            } else if (s.cursor > 0) {
                size_t prev = utf8Prev(value, (size_t)s.cursor);
                value.erase(prev, (size_t)s.cursor - prev);
                s.cursor = (int)prev;
                s.anchor = s.cursor;
            }
            break;
        case SDLK_DELETE:
            if (hasSel) {
                deleteSelection(value, s);
            } else if (s.cursor < (int)value.size()) {
                size_t next = utf8Next(value, (size_t)s.cursor);
                value.erase((size_t)s.cursor, next - (size_t)s.cursor);
            }
            break;
        case SDLK_LEFT:
            if (hasSel && !k.shift) {
                s.cursor = std::min(s.anchor, s.cursor);
                s.anchor = s.cursor;
            } else if (s.cursor > 0) {
                s.cursor = (int)utf8Prev(value, (size_t)s.cursor);
                if (!k.shift)
                    s.anchor = s.cursor;
            }
            break;
        case SDLK_RIGHT:
            if (hasSel && !k.shift) {
                s.cursor = std::max(s.anchor, s.cursor);
                s.anchor = s.cursor;
            } else if (s.cursor < (int)value.size()) {
                s.cursor = (int)utf8Next(value, (size_t)s.cursor);
                if (!k.shift)
                    s.anchor = s.cursor;
            }
            break;
        case SDLK_HOME:
            s.cursor = 0;
            if (!k.shift)
                s.anchor = 0;
            break;
        case SDLK_END:
            s.cursor = (int)value.size();
            if (!k.shift)
                s.anchor = s.cursor;
            break;
        case SDLK_a:
            if (k.ctrl) {
                s.anchor = 0;
                s.cursor = (int)value.size();
            }
            break;
        case SDLK_c:
            if (k.ctrl && hasSel) {
                int a = std::min(s.anchor, s.cursor), b = std::max(s.anchor, s.cursor);
                SDL_SetClipboardText(value.substr((size_t)a, (size_t)(b - a)).c_str());
            }
            break;
        case SDLK_x:
            if (k.ctrl && hasSel) {
                int a = std::min(s.anchor, s.cursor), b = std::max(s.anchor, s.cursor);
                SDL_SetClipboardText(value.substr((size_t)a, (size_t)(b - a)).c_str());
                deleteSelection(value, s);
            }
            break;
        case SDLK_v:
            if (k.ctrl) {
                char* clip = SDL_GetClipboardText();
                if (clip && *clip) {
                    if (hasSel)
                        deleteSelection(value, s);
                    std::string text(clip);
                    value.insert((size_t)s.cursor, text);
                    s.cursor += (int)text.size();
                    s.anchor = s.cursor;
                }
                if (clip)
                    SDL_free(clip);
            }
            break;
        default:
            break;
        }
    }
    (void)id;
}

} // namespace

void textInput(const char* id, float x, float y, float w, float h, std::string& value,
               const std::string& placeholder)
{
    const float fontSize = 13.0f;
    const float pad = 12.0f;
    WState& s = st(id);
    bool hovered = mouseInside(x, y, w, h);

    if (g_clicked) {
        if (hovered) {
            g_focusId = id;
            if (s.cursor < 0)
                s.cursor = (int)value.size();
            s.cursor = hitTestText(fontSize, value, g_mx - (x + pad) + s.scrollX);
            s.anchor = s.cursor;
            s.downInside = true;
        } else if (g_focusId == id) {
            g_focusId.clear();
        }
    }
    if (g_focusId == id && s.downInside && g_leftDown && hovered)
        s.cursor = hitTestText(fontSize, value, g_mx - (x + pad) + s.scrollX);
    if (!g_leftDown)
        s.downInside = false;

    bool focused = (g_focusId == id);
    if (s.cursor < 0)
        s.cursor = (int)value.size();
    if (s.anchor < 0)
        s.anchor = s.cursor;
    s.cursor = std::max(0, std::min(s.cursor, (int)value.size()));
    s.anchor = std::max(0, std::min(s.anchor, (int)value.size()));

    if (focused)
        processInputKeys(id, value, s);

    // horizontal scroll so the caret stays visible
    float caretX = textWidth(fontSize, value.substr(0, (size_t)s.cursor));
    float visibleW = w - pad * 2;
    if (caretX - s.scrollX > visibleW - 4)
        s.scrollX = caretX - (visibleW - 4);
    if (caretX - s.scrollX < 4)
        s.scrollX = caretX - 4;
    if (s.scrollX < 0)
        s.scrollX = 0;

    // background
    roundedRect(x, y, w, h, 8.0f, hovered ? palette::inputHover : palette::input);
    if (focused)
        roundedRectBorder(x + 0.5f, y + 0.5f, w - 1, h - 1, 8.0f, 1.5f, palette::accent);
    else
        roundedRectBorder(x + 0.5f, y + 0.5f, w - 1, h - 1, 8.0f, 1.0f, palette::border);

    float textY = y + (h - lineHeight(fontSize)) * 0.5f;

    // clip to the field
    SDL_Rect clip{ (int)std::lround((x + pad) * g_scale), (int)std::lround(y * g_scale),
                   (int)std::lround(visibleW * g_scale), (int)std::lround(h * g_scale) };
    SDL_RenderSetClipRect(g_renderer, &clip);

    if (value.empty() && !focused) {
        drawText(x + pad, textY, fontSize, palette::textDim, placeholder);
    } else {
        // selection highlight
        if (focused && s.anchor != s.cursor) {
            int a = std::min(s.anchor, s.cursor), b = std::max(s.anchor, s.cursor);
            float x0 = textWidth(fontSize, value.substr(0, (size_t)a));
            float x1 = textWidth(fontSize, value.substr(0, (size_t)b));
            Color sel = palette::accent;
            sel.a = 0.30f;
            roundedRect(x + pad + x0 - s.scrollX, y + 5, x1 - x0, h - 10, 3.0f, sel);
        }
        drawText(x + pad - s.scrollX, textY, fontSize, palette::text, value);

        // caret
        if (focused && std::fmod(g_time, 1.0f) < 0.6f) {
            SDL_FRect caret{ (x + pad + caretX - s.scrollX) * g_scale, (y + 7) * g_scale,
                             std::max(1.0f, 1.5f * g_scale), (h - 14) * g_scale };
            SDL_Color c = toSDL(palette::text);
            SDL_SetRenderDrawColor(g_renderer, c.r, c.g, c.b, 255);
            SDL_RenderFillRectF(g_renderer, &caret);
        }
    }
    SDL_RenderSetClipRect(g_renderer, nullptr);
}

void toggle(const char* id, float x, float y, bool& value, const std::string& label)
{
    const float trackW = 40.0f, trackH = 22.0f;
    WState& s = st(id);
    s.knobT = approach(s.knobT, value ? 1.0f : 0.0f, 12.0f);

    float labelW = textWidth(13.0f, label);
    bool hovered = mouseInside(x, y, trackW + 10 + labelW, trackH);
    if (g_clicked && hovered && !g_clickEaten)
        value = !value;

    roundedRect(x, y, trackW, trackH, trackH * 0.5f, lerp(palette::track, palette::accent, s.knobT));
    float knobSize = 16.0f;
    float knobX = x + 3.0f + s.knobT * (trackW - knobSize - 6.0f);
    roundedRect(knobX, y + (trackH - knobSize) * 0.5f, knobSize, knobSize, knobSize * 0.5f,
                palette::text);
    drawText(x + trackW + 10, y + (trackH - lineHeight(13.0f)) * 0.5f, 13.0f,
             hovered ? palette::text : palette::textDim, label);
}

void progressBar(float x, float y, float w, float h, float fraction)
{
    WState& s = st("progress");
    fraction = std::max(0.0f, std::min(1.0f, fraction));
    s.dispFrac = approach(s.dispFrac, fraction, 6.0f);

    roundedRect(x, y, w, h, h * 0.5f, palette::track);
    float fillW = s.dispFrac * (w - 4.0f);
    if (fillW > h)
        roundedRect(x + 2, y + 2, fillW, h - 4, (h - 4) * 0.5f, palette::accent);
}

// ---------------------------------------------------------------------------
// combo box
// ---------------------------------------------------------------------------

namespace {

struct Overlay {
    float x, y, w, itemH;
    std::vector<std::string> items;
    int selected;
};
std::vector<Overlay> g_overlays;

} // namespace

bool combo(const char* id, float x, float y, float w, float h,
           const std::vector<std::string>& items, int& selected, const std::string& placeholder)
{
    WState& s = st(id);
    bool open = (g_openCombo == id);
    bool changed = false;

    const float itemH = 30.0f;
    float popupY = y + h + 4.0f;
    float popupH = items.size() * itemH + 8.0f;

    if (open && g_clicked) {
        bool inPopup = mouseInside(x, popupY, w, popupH);
        if (inPopup && !items.empty()) {
            int idx = (int)((g_my - (popupY + 4.0f)) / itemH);
            if (idx >= 0 && idx < (int)items.size()) {
                selected = idx;
                changed = true;
            }
        }
        g_openCombo.clear();
        open = false;
        // The popup consumed this click (selection or dismiss); widgets
        // underneath must not also react to it.
        g_clickEaten = true;
    }

    bool hovered = mouseInside(x, y, w, h);
    s.hoverT = approach(s.hoverT, hovered ? 1.0f : 0.0f, 10.0f);
    if (!open && g_clicked && hovered && !items.empty() && !g_clickEaten) {
        g_openCombo = id;
        open = true;
        g_clickEaten = true;
    }

    // field
    roundedRect(x, y, w, h, 8.0f, lerp(palette::input, palette::inputHover, s.hoverT));
    roundedRectBorder(x + 0.5f, y + 0.5f, w - 1, h - 1, 8.0f, 1.0f,
                      open ? palette::accent : palette::border);

    float textY = y + (h - lineHeight(13.0f)) * 0.5f;
    if (selected >= 0 && selected < (int)items.size()) {
        // clip long paths
        SDL_Rect clip{ (int)std::lround((x + 12) * g_scale), (int)std::lround(y * g_scale),
                       (int)std::lround((w - 40) * g_scale), (int)std::lround(h * g_scale) };
        SDL_RenderSetClipRect(g_renderer, &clip);
        drawText(x + 12, textY, 13.0f, palette::text, items[(size_t)selected]);
        SDL_RenderSetClipRect(g_renderer, nullptr);
    } else {
        drawText(x + 12, textY, 13.0f, palette::textDim, placeholder);
    }

    // chevron
    {
        float cx = x + w - 20.0f;
        float cy = y + h * 0.5f;
        SDL_Vertex verts[3];
        SDL_Color c = toSDL(palette::textDim);
        verts[0] = { { (cx - 5) * g_scale, (cy - 2.5f) * g_scale }, c, { 0, 0 } };
        verts[1] = { { (cx + 5) * g_scale, (cy - 2.5f) * g_scale }, c, { 0, 0 } };
        verts[2] = { { cx * g_scale, (cy + 3.5f) * g_scale }, c, { 0, 0 } };
        SDL_RenderGeometry(g_renderer, nullptr, verts, 3, nullptr, 0);
    }

    if (open)
        g_overlays.push_back({ x, popupY, w, itemH, items, selected });

    return changed;
}

void flushOverlays()
{
    for (const Overlay& o : g_overlays) {
        float popupH = o.items.size() * o.itemH + 8.0f;
        // soft shadow via translucent layers
        Color shadow{ 0, 0, 0, 0.28f };
        roundedRect(o.x - 3, o.y - 1, o.w + 6, popupH + 5, 10.0f, shadow);
        roundedRect(o.x, o.y, o.w, popupH, 8.0f, palette::popup);
        roundedRectBorder(o.x + 0.5f, o.y + 0.5f, o.w - 1, popupH - 1, 8.0f, 1.0f, palette::border);

        for (size_t i = 0; i < o.items.size(); ++i) {
            float iy = o.y + 4.0f + i * o.itemH;
            bool hovered = mouseInside(o.x + 4, iy, o.w - 8, o.itemH);
            if (hovered)
                roundedRect(o.x + 4, iy, o.w - 8, o.itemH, 6.0f, palette::inputHover);
            Color tc = ((int)i == o.selected) ? palette::accent : palette::text;
            SDL_Rect clip{ (int)std::lround((o.x + 14) * g_scale), (int)std::lround(iy * g_scale),
                           (int)std::lround((o.w - 28) * g_scale),
                           (int)std::lround(o.itemH * g_scale) };
            SDL_RenderSetClipRect(g_renderer, &clip);
            drawText(o.x + 14, iy + (o.itemH - lineHeight(13.0f)) * 0.5f, 13.0f, tc, o.items[i]);
            SDL_RenderSetClipRect(g_renderer, nullptr);
        }
    }
    g_overlays.clear();
}

} // namespace ui
