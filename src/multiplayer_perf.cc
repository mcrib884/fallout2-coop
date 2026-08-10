#include "multiplayer_perf.h"

#include <SDL.h>

#include <stdio.h>
#include <string.h>

#include "color.h"
#include "debug.h"
#include "map.h"
#include "multiplayer.h"
#include "text_font.h"
#include "tile.h"
#include "window_manager.h"

namespace fallout {

// The overlay sits at the top-left, below the indicator inset band, and is
// recreated once per second (same technique as the player-edge indicators).
#define MP_PERF_WINDOW_WIDTH 360
#define MP_PERF_WINDOW_HEIGHT 42
#define MP_PERF_WINDOW_X 8
#define MP_PERF_WINDOW_Y 34
#define MP_PERF_WINDOW_FONT 101

// Microsecond timer (SDL high-performance counter).
static uint64_t mpPerfNowUs()
{
    return SDL_GetPerformanceCounter() * 1000000ULL / SDL_GetPerformanceFrequency();
}

// One rolling second of frame + section samples.
struct PerfWindow {
    uint32_t frameCount;
    uint64_t frameSumUs;
    uint64_t frameMaxUs;
    uint64_t sectionSumUs[MP_PERF_SECTION_COUNT];
    uint32_t sectionCount[MP_PERF_SECTION_COUNT];
    uint64_t sectionMaxUs[MP_PERF_SECTION_COUNT];
    uint32_t counters[MP_PERF_CNT_COUNT];
};

static bool gPerfEnabled = false;

static PerfWindow gPerfWindowData;

// Per-frame accumulators.
static uint64_t gFrameSectionUs[MP_PERF_SECTION_COUNT];
static uint32_t gFrameSectionCount[MP_PERF_SECTION_COUNT];
static uint64_t gFrameSectionMaxUs[MP_PERF_SECTION_COUNT];
static uint32_t gFrameCounters[MP_PERF_CNT_COUNT];

// Open-section bookkeeping.
static int gActiveSection = -1;
static uint64_t gActiveSectionStartUs = 0;
static uint64_t gFrameStartUs = 0;
static uint64_t gFrameElapsedUs = 0;
static uint64_t gFrameMaxUs = 0;
static uint64_t gNestedSectionStartUs[MP_PERF_SECTION_COUNT];

// Window clock.
static uint64_t gWindowStartUs = 0;
static int gWindowCount = 0; // windows since the last log line

// Overlay window handle.
static int gPerfWindow = -1;

// Section display names (log + overlay).
static const char* const kSectionNames[MP_PERF_SECTION_COUNT] = {
    "input",
    "glob",
    "key",
    "req",
    "trans",
    "tick",
    "net",
    "assign",
    "objB",
    "prof",
    "tiles",
    "plyB",
    "render",
};

static void mpPerfResetWindow()
{
    memset(&gPerfWindowData, 0, sizeof(gPerfWindowData));
    gWindowStartUs = mpPerfNowUs();
    // NOTE: gWindowCount is deliberately NOT reset here. MpPerfTick
    // increments it per completed window and resets it after the
    // every-5th-window log line, so the counter must survive the reset.
}

static void mpPerfHideOverlay()
{
    if (gPerfWindow == -1) {
        return;
    }
    Rect rect;
    rect.left = MP_PERF_WINDOW_X;
    rect.top = MP_PERF_WINDOW_Y;
    rect.right = rect.left + MP_PERF_WINDOW_WIDTH;
    rect.bottom = rect.top + MP_PERF_WINDOW_HEIGHT;
    windowDestroy(gPerfWindow);
    gPerfWindow = -1;
    tileWindowRefreshRect(&rect, gElevation);
}

static void mpPerfShowOverlay()
{
    mpPerfHideOverlay();

    int win = windowCreate(MP_PERF_WINDOW_X, MP_PERF_WINDOW_Y,
        MP_PERF_WINDOW_WIDTH, MP_PERF_WINDOW_HEIGHT,
        COLOR_BLACK, WINDOW_MOVE_ON_TOP);
    if (win == -1) {
        return;
    }
    windowDrawBorder(win);

    char line1[MP_PERF_WINDOW_WIDTH / 2 + 8];
    char line2[MP_PERF_WINDOW_WIDTH / 2 + 8];

    double fps = gPerfWindowData.frameCount > 0
        ? (double)gPerfWindowData.frameCount * 1000000.0 / (double)(mpPerfNowUs() - gWindowStartUs)
        : 0.0;
    double frameAvgMs = gPerfWindowData.frameSumUs / 1000.0
        / (gPerfWindowData.frameCount > 0 ? (double)gPerfWindowData.frameCount : 1.0);
    double frameMaxMs = gPerfWindowData.frameMaxUs / 1000.0;

    // Section averages (ms) for the headline sections.
    auto sectionAvg = [](int section) -> double {
        if (gPerfWindowData.sectionCount[section] == 0) {
            return 0.0;
        }
        return gPerfWindowData.sectionSumUs[section] / 1000.0
            / (double)gPerfWindowData.sectionCount[section];
    };

    snprintf(line1, sizeof(line1), "MP %.1ffps  fr %.1f/%.1fms  tick %.1f  objB %.1f  prof %.1f",
        fps, frameAvgMs, frameMaxMs,
        sectionAvg(MP_PERF_MPTICK), sectionAvg(MP_PERF_MPTICK_OBJECTS),
        sectionAvg(MP_PERF_MPTICK_PROFILES));
    snprintf(line2, sizeof(line2), "obj %u rec %u chg %u remCmp %u | in %.1f rend %.1f net %.1f",
        gPerfWindowData.counters[MP_PERF_CNT_OBJ_SCANNED],
        gPerfWindowData.counters[MP_PERF_CNT_OBJ_RECORDS],
        gPerfWindowData.counters[MP_PERF_CNT_OBJ_CHANGED],
        gPerfWindowData.counters[MP_PERF_CNT_OBJ_REM_CMP],
        sectionAvg(MP_PERF_INPUT), sectionAvg(MP_PERF_RENDER),
        sectionAvg(MP_PERF_MPTICK_NET));

    fontSetCurrent(MP_PERF_WINDOW_FONT);
    windowDrawText(win, line1, 0, 5, 4, COLOR_LIGHT_YELLOW);
    windowDrawText(win, line2, 0, 5, 19, COLOR_LIGHT_YELLOW);
    windowRefresh(win);
    gPerfWindow = win;
}

void MpPerfFrameStart()
{
    if (gActiveSection == -1) {
        gActiveSection = MP_PERF_INPUT;
        gActiveSectionStartUs = mpPerfNowUs();
    }
    gFrameStartUs = mpPerfNowUs();
}

void MpPerfMark(int section)
{
    if (section < 0 || section >= MP_PERF_SECTION_COUNT || section == gActiveSection) {
        return;
    }
    uint64_t nowUs = mpPerfNowUs();
    if (gActiveSection >= 0) {
        uint64_t elapsedUs = nowUs - gActiveSectionStartUs;
        gFrameSectionUs[gActiveSection] += elapsedUs;
        gFrameSectionCount[gActiveSection]++;
        if (elapsedUs > gFrameSectionMaxUs[gActiveSection]) {
            gFrameSectionMaxUs[gActiveSection] = elapsedUs;
        }
    }
    gActiveSection = section;
    gActiveSectionStartUs = nowUs;
}

void MpPerfBegin(int section)
{
    if (section < 0 || section >= MP_PERF_SECTION_COUNT) {
        return;
    }
    gNestedSectionStartUs[section] = mpPerfNowUs();
}

void MpPerfEnd(int section)
{
    if (section < 0 || section >= MP_PERF_SECTION_COUNT) {
        return;
    }
    uint64_t elapsedUs = mpPerfNowUs() - gNestedSectionStartUs[section];
    gFrameSectionUs[section] += elapsedUs;
    gFrameSectionCount[section]++;
    if (elapsedUs > gFrameSectionMaxUs[section]) {
        gFrameSectionMaxUs[section] = elapsedUs;
    }
}

void MpPerfFrameEnd()
{
    uint64_t nowUs = mpPerfNowUs();

    // Close the active section (RENDER).
    if (gActiveSection >= 0) {
        uint64_t elapsedUs = nowUs - gActiveSectionStartUs;
        gFrameSectionUs[gActiveSection] += elapsedUs;
        gFrameSectionCount[gActiveSection]++;
        if (elapsedUs > gFrameSectionMaxUs[gActiveSection]) {
            gFrameSectionMaxUs[gActiveSection] = elapsedUs;
        }
        gActiveSection = -1;
    }

    gFrameElapsedUs = nowUs - gFrameStartUs;
    if (gFrameElapsedUs > gFrameMaxUs) {
        gFrameMaxUs = gFrameElapsedUs;
    }

    // Fold the frame into the 1s window.
    gPerfWindowData.frameCount++;
    gPerfWindowData.frameSumUs += gFrameElapsedUs;
    if (gFrameElapsedUs > gPerfWindowData.frameMaxUs) {
        gPerfWindowData.frameMaxUs = gFrameElapsedUs;
    }
    for (int section = 0; section < MP_PERF_SECTION_COUNT; section++) {
        gPerfWindowData.sectionSumUs[section] += gFrameSectionUs[section];
        gPerfWindowData.sectionCount[section] += gFrameSectionCount[section];
        if (gFrameSectionMaxUs[section] > gPerfWindowData.sectionMaxUs[section]) {
            gPerfWindowData.sectionMaxUs[section] = gFrameSectionMaxUs[section];
        }
    }
    for (int counter = 0; counter < MP_PERF_CNT_COUNT; counter++) {
        if (counter == MP_PERF_CNT_OBJ_RECORDS) {
            // Level: keep the latest frame's value.
            gPerfWindowData.counters[counter] = gFrameCounters[counter];
        } else {
            gPerfWindowData.counters[counter] += gFrameCounters[counter];
        }
    }

    memset(gFrameSectionUs, 0, sizeof(gFrameSectionUs));
    memset(gFrameSectionCount, 0, sizeof(gFrameSectionCount));
    memset(gFrameSectionMaxUs, 0, sizeof(gFrameSectionMaxUs));
    memset(gFrameCounters, 0, sizeof(gFrameCounters));
}

void MpPerfTick()
{
    if (!gMpActive) {
        return;
    }
    uint64_t nowUs = mpPerfNowUs();

    if (gPerfWindowData.frameCount > 0 && nowUs - gWindowStartUs >= 1000000ULL) {
        // 1s window complete: refresh the overlay, log every 5th window.
        gWindowCount++;
        if (gPerfEnabled) {
            mpPerfShowOverlay();
        }
        if (gWindowCount >= 5) {
            double fps = (double)gPerfWindowData.frameCount * 1000000.0
                / (double)(nowUs - gWindowStartUs);
            double frameAvgMs = gPerfWindowData.frameSumUs / 1000.0
                / (double)gPerfWindowData.frameCount;
            double frameMaxMs = gPerfWindowData.frameMaxUs / 1000.0;

            char sections[512];
            int offset = 0;
            for (int section = 0; section < MP_PERF_SECTION_COUNT; section++) {
                double avgMs = gPerfWindowData.sectionCount[section] > 0
                    ? gPerfWindowData.sectionSumUs[section] / 1000.0
                        / (double)gPerfWindowData.sectionCount[section]
                    : 0.0;
                offset += snprintf(sections + offset, sizeof(sections) - (size_t)offset,
                    "%s=%.1f ", kSectionNames[section], avgMs);
            }

            debugFilePrint("MPPERF: fps=%.1f frameAvg=%.1f frameMax=%.1f | %s| obj=%u rec=%u chg=%u remCmp=%u pktO=%u pktP=%u",
                fps, frameAvgMs, frameMaxMs, sections,
                gPerfWindowData.counters[MP_PERF_CNT_OBJ_SCANNED],
                gPerfWindowData.counters[MP_PERF_CNT_OBJ_RECORDS],
                gPerfWindowData.counters[MP_PERF_CNT_OBJ_CHANGED],
                gPerfWindowData.counters[MP_PERF_CNT_OBJ_REM_CMP],
                gPerfWindowData.counters[MP_PERF_CNT_OBJ_PKTS],
                gPerfWindowData.counters[MP_PERF_CNT_PLAYER_PKTS]);
            gWindowCount = 0;
        }
        mpPerfResetWindow();
    }
}

bool MpPerfIsEnabled()
{
    return gPerfEnabled;
}

void MpPerfSetEnabled(bool enabled)
{
    if (gPerfEnabled == enabled) {
        return;
    }
    gPerfEnabled = enabled;
    if (!gPerfEnabled) {
        mpPerfHideOverlay();
    } else {
        mpPerfShowOverlay();
    }
    debugFilePrint("MPPERF: meter %s", enabled ? "enabled" : "disabled");
}

void MpPerfAddCounter(int counter, uint32_t delta)
{
    if (counter < 0 || counter >= MP_PERF_CNT_COUNT || counter == MP_PERF_CNT_OBJ_RECORDS) {
        return;
    }
    gFrameCounters[counter] += delta;
}

void MpPerfSetCounter(int counter, uint32_t value)
{
    if (counter < 0 || counter >= MP_PERF_CNT_COUNT) {
        return;
    }
    gFrameCounters[counter] = value;
}

} // namespace fallout
