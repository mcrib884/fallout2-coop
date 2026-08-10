#ifndef FALLOUT_LAUNCHER_UI_H
#define FALLOUT_LAUNCHER_UI_H

#include <SDL.h>

#include <string>
#include <vector>

namespace ui {

struct Color {
    float r, g, b, a;
};

Color lerp(const Color& a, const Color& b, float t);

namespace palette {
extern const Color bg;
extern const Color card;
extern const Color input;
extern const Color inputHover;
extern const Color border;
extern const Color text;
extern const Color textDim;
extern const Color accent;
extern const Color accentHover;
extern const Color accentDown;
extern const Color error;
extern const Color success;
extern const Color track;
extern const Color popup;
} // namespace palette

// lifecycle
void init(SDL_Renderer* renderer, const unsigned char* ttfData, unsigned int ttfSize);
void shutdown();
// Sets the logical design size of the UI; the window can be resized freely
// and the canvas is scaled to fit (letterboxed).
void setCanvas(float w, float h);
float scale();
void beginFrame(float dt); // dt in seconds

// events & input
void handleEvent(const SDL_Event& e);
void setMouse(float x, float y, bool leftDown); // logical coordinates
bool mouseClicked();                            // left button pressed this frame
float mouseWheelDelta();                        // positive when the wheel moves up
bool mouseOver(float x, float y, float w, float h);

// drawing primitives (logical coordinates)
void roundedRect(float x, float y, float w, float h, float r, const Color& c);
void roundedRectBorder(float x, float y, float w, float h, float r, float thickness, const Color& c);
void drawText(float x, float y, float size, const Color& c, const std::string& str);
void drawTextCenter(float cx, float y, float size, const Color& c, const std::string& str);
void drawTextRight(float right, float y, float size, const Color& c, const std::string& str);
float textWidth(float size, const std::string& str);
float lineHeight(float size);

// widgets
enum class ButtonStyle { Accent, Subtle };
bool button(const char* id, float x, float y, float w, float h, const std::string& label,
            ButtonStyle style, bool enabled = true);
void textInput(const char* id, float x, float y, float w, float h, std::string& value,
               const std::string& placeholder);
void toggle(const char* id, float x, float y, bool& value, const std::string& label);
void progressBar(float x, float y, float w, float h, float fraction);
// Dropdown; returns true and updates `selected` when the user picks an item.
bool combo(const char* id, float x, float y, float w, float h,
           const std::vector<std::string>& items, int& selected, const std::string& placeholder);
void flushOverlays(); // draw queued popups above everything; call at frame end

} // namespace ui

#endif
