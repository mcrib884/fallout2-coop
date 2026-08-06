#include "debug.h"

#include <SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "memory.h"
#include "platform_compat.h"
#include "window_manager_private.h"

namespace fallout {

static int _debug_puts(char* string);
static void _debug_clear();
static int _debug_mono(const char* string);
static int _debug_log(const char* string);
static int _debug_screen(const char* string);
static void _debug_putc(int ch);
static void _debug_scroll();
static void debugFlushBuffer();

// Messages logged before any debug proc is registered are held here and
// flushed when the first proc is registered.
static std::string debugBuffer;
static constexpr size_t kDebugBufferMaxSize = 64 * 1024;
static bool debugBufferDisabled = false;

// 0x51DEF8 fd
static FILE* _fd = nullptr;

static std::string debugCrashLogPath = "coop_debug.log";
static bool debugCrashLogInitialized = false;
static uint32_t gDebugLogStartTick = 0;
static uint32_t gDebugLogLastTick = 0;
static uint32_t gDebugLogLine = 0;

// 0x51DEFC curx
static int _curx = 0;

// 0x51DF00 cury
static int _cury = 0;

// 0x51DF04 debug_func
static DebugPrintProc* gDebugPrintProc = nullptr;

#if defined(_WIN32)
static LONG WINAPI debugUnhandledExceptionFilter(EXCEPTION_POINTERS* exceptionInfo)
{
    FILE* stream = compat_fopen(debugCrashLogPath.c_str(), "at");
    if (stream != nullptr) {
        fprintf(stream, "\n=== FALLOUT2COOP FATAL EXCEPTION ===\n");
        if (exceptionInfo != nullptr && exceptionInfo->ExceptionRecord != nullptr) {
            fprintf(stream, "code=0x%08lX address=%p flags=0x%08lX\n",
                exceptionInfo->ExceptionRecord->ExceptionCode,
                exceptionInfo->ExceptionRecord->ExceptionAddress,
                exceptionInfo->ExceptionRecord->ExceptionFlags);
        } else {
            fprintf(stream, "exception details unavailable\n");
        }

        if (exceptionInfo != nullptr && exceptionInfo->ContextRecord != nullptr) {
            fprintf(stream, "module_base=%p context_rip=%p context_rsp=%p context_rbp=%p context_eflags=0x%08llX\n",
                (void*)GetModuleHandleW(nullptr),
                (void*)exceptionInfo->ContextRecord->Rip,
                (void*)exceptionInfo->ContextRecord->Rsp,
                (void*)exceptionInfo->ContextRecord->Rbp,
                (unsigned long long)exceptionInfo->ContextRecord->EFlags);
        }

        void* frames[32];
        USHORT frameCount = CaptureStackBackTrace(0, ARRAYSIZE(frames), frames, nullptr);
        fprintf(stream, "stack_frames=%u\n", frameCount);
        for (USHORT index = 0; index < frameCount; index++) {
            fprintf(stream, "  #%u %p\n", index, frames[index]);
        }
        fflush(stream);
        fclose(stream);
    }

    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void debugModeInit(const char* debugMode)
{
    debugBufferDisabled = true;

    if (debugMode == nullptr) {
        return;
    }

    // CE: Handle debug mode (exactly as seen in `mapper2.exe`).
    if (compat_stricmp(debugMode, "environment") == 0) {
        _debug_register_env();
    } else if (compat_stricmp(debugMode, "screen") == 0) {
        _debug_register_screen();
    } else if (compat_stricmp(debugMode, "log") == 0) {
        _debug_register_log("debug.log", "wt");
    } else if (compat_stricmp(debugMode, "mono") == 0) {
        _debug_register_mono();
    } else if (compat_stricmp(debugMode, "gnw") == 0) {
        _debug_register_func(_win_debug);
    }

    if (gDebugPrintProc == nullptr) {
        debugBuffer.clear();
    }
}

// 0x4C6CD0
void _GNW_debug_init()
{
    atexit(_debug_exit);
}

// 0x4C6CDC
void _debug_register_mono()
{
    if (gDebugPrintProc != _debug_mono) {
        if (_fd != nullptr) {
            fclose(_fd);
            _fd = nullptr;
        }

        gDebugPrintProc = _debug_mono;
        _debug_clear();
        debugFlushBuffer();
    }
}

// 0x4C6D18
void _debug_register_log(const char* fileName, const char* mode)
{
    if ((mode[0] == 'w' || mode[0] == 'a') && mode[1] == 't') {
        if (_fd != nullptr) {
            fclose(_fd);
        }

        _fd = compat_fopen(fileName, mode);
        gDebugPrintProc = _debug_log;
        debugFlushBuffer();
    }
}

// 0x4C6D5C
void _debug_register_screen()
{
    if (gDebugPrintProc != _debug_screen) {
        if (_fd != nullptr) {
            fclose(_fd);
            _fd = nullptr;
        }

        gDebugPrintProc = _debug_screen;
        debugFlushBuffer();
    }
}

// 0x4C6D90
void _debug_register_env()
{
    const char* type = getenv("DEBUGACTIVE");
    if (type == nullptr) {
        return;
    }

    char* copy = (char*)internal_malloc(strlen(type) + 1);
    if (copy == nullptr) {
        return;
    }

    strcpy(copy, type);
    compat_strlwr(copy);

    if (strcmp(copy, "mono") == 0) {
        // NOTE: Uninline.
        _debug_register_mono();
    } else if (strcmp(copy, "log") == 0) {
        _debug_register_log("debug.log", "wt");
    } else if (strcmp(copy, "screen") == 0) {
        // NOTE: Uninline.
        _debug_register_screen();
    } else if (strcmp(copy, "gnw") == 0) {
        // NOTE: Uninline.
        _debug_register_func(_win_debug);
    }

    internal_free(copy);
}

// 0x4C6F18
void _debug_register_func(DebugPrintProc* proc)
{
    if (gDebugPrintProc != proc) {
        if (_fd != nullptr) {
            fclose(_fd);
            _fd = nullptr;
        }

        gDebugPrintProc = proc;
        debugFlushBuffer();
    }
}

// 0x4C6F48
int debugPrint(const char* format, ...)
{
    va_list args;
    va_start(args, format);

    char string[260];
    int len = vsnprintf(string, sizeof(string), format, args);
    if (len < 0) {
        string[0] = '\0';
    }
    va_end(args);

    int rc;
    if (gDebugPrintProc != nullptr) {
        rc = gDebugPrintProc(string);
    } else {
        if (!debugBufferDisabled && debugBuffer.size() + strlen(string) <= kDebugBufferMaxSize) {
            debugBuffer += string;
        }
        rc = -1;
    }

#ifndef NDEBUG
    SDL_Log("%s", string);
#endif

    return rc;
}

void debugInstallCrashHandler(const char* logPath)
{
    if (logPath != nullptr && logPath[0] != '\0') {
        debugCrashLogPath = logPath;
    }

    if (!debugCrashLogInitialized) {
        FILE* stream = compat_fopen(debugCrashLogPath.c_str(), "wt");
        if (stream != nullptr) {
            fclose(stream);
        }
        debugCrashLogInitialized = true;
    }

    debugFilePrint("=== fallout2coop session started ===");
#if defined(_WIN32)
    debugFilePrint("module_base=%p", (void*)GetModuleHandleW(nullptr));
#endif

#if defined(_WIN32)
    SetUnhandledExceptionFilter(debugUnhandledExceptionFilter);
#endif
}

void debugFilePrint(const char* format, ...)
{
    if (format == nullptr) {
        return;
    }

    FILE* stream = compat_fopen(debugCrashLogPath.c_str(), "at");
    if (stream == nullptr) {
        return;
    }

    uint32_t now = SDL_GetTicks();
    if (gDebugLogStartTick == 0) {
        gDebugLogStartTick = now;
        gDebugLogLastTick = now;
    }
    uint32_t elapsed = now - gDebugLogStartTick;
    uint32_t delta = now - gDebugLogLastTick;
    gDebugLogLastTick = now;
    gDebugLogLine++;

    time_t rawTime;
    time(&rawTime);
    struct tm* timeInfo = localtime(&rawTime);
    char wallClock[32];
    wallClock[0] = '\0';
    if (timeInfo != nullptr) {
        strftime(wallClock, sizeof(wallClock), "%H:%M:%S", timeInfo);
    }

    fprintf(stream, "[%s +%02u:%02u.%03u dt=%04u #%u] ",
        wallClock,
        (unsigned)(elapsed / 60000),
        (unsigned)((elapsed / 1000) % 60),
        (unsigned)(elapsed % 1000),
        (unsigned)delta,
        (unsigned)gDebugLogLine);

    va_list args;
    va_start(args, format);
    vfprintf(stream, format, args);
    va_end(args);
    fputc('\n', stream);
    fflush(stream);
    fclose(stream);
}

static void debugFlushBuffer()
{
    if (debugBuffer.empty() || gDebugPrintProc == nullptr) {
        return;
    }
    gDebugPrintProc(debugBuffer.c_str());
    debugBuffer.clear();
}

// 0x4C6F94
static int _debug_puts(char* string)
{
    if (gDebugPrintProc != nullptr) {
        return gDebugPrintProc(string);
    }

    return -1;
}

// 0x4C6FAC
static void _debug_clear()
{
    char* buffer;
    int x;
    int y;

    buffer = nullptr;

    if (gDebugPrintProc == _debug_mono) {
        buffer = (char*)0xB0000;
    } else if (gDebugPrintProc == _debug_screen) {
        buffer = (char*)0xB8000;
    }

    if (buffer != nullptr) {
        for (y = 0; y < 25; y++) {
            for (x = 0; x < 80; x++) {
                *buffer++ = ' ';
                *buffer++ = 7;
            }
        }
        _cury = 0;
        _curx = 0;
    }
}

// 0x4C7004
static int _debug_mono(const char* string)
{
    if (gDebugPrintProc == _debug_mono) {
        while (*string != '\0') {
            char ch = *string++;
            _debug_putc(ch);
        }
    }
    return 0;
}

// 0x4C7028
static int _debug_log(const char* string)
{
    if (gDebugPrintProc == _debug_log) {
        if (_fd == nullptr) {
            return -1;
        }

        if (fprintf(_fd, "%s", string) < 0) {
            return -1;
        }

        if (fflush(_fd) == EOF) {
            return -1;
        }
    }

    return 0;
}

// 0x4C7068
static int _debug_screen(const char* string)
{
    if (gDebugPrintProc == _debug_screen) {
        printf("%s", string);
    }

    return 0;
}

// 0x4C709C
static void _debug_putc(int ch)
{
    char* buffer;

    buffer = (char*)0xB0000;

    switch (ch) {
    case 7:
        printf("\x07");
        return;
    case 8:
        if (_curx > 0) {
            _curx--;
            buffer += 2 * _curx + 2 * 80 * _cury;
            *buffer++ = ' ';
            *buffer = 7;
        }
        return;
    case 9:
        do {
            _debug_putc(' ');
        } while ((_curx - 1) % 4 != 0);
        return;
    case 13:
        _curx = 0;
        return;
    default:
        buffer += 2 * _curx + 2 * 80 * _cury;
        *buffer++ = ch;
        *buffer = 7;
        _curx++;
        if (_curx < 80) {
            return;
        }
        // FALLTHROUGH
    case 10:
        _curx = 0;
        _cury++;
        if (_cury > 24) {
            _cury = 24;
            _debug_scroll();
        }
        return;
    }
}

// 0x4C71AC
static void _debug_scroll()
{
    char* buffer;
    int x;
    int y;

    buffer = (char*)0xB0000;

    for (y = 0; y < 24; y++) {
        for (x = 0; x < 80 * 2; x++) {
            buffer[0] = buffer[80 * 2];
            buffer++;
        }
    }

    for (x = 0; x < 80; x++) {
        *buffer++ = ' ';
        *buffer++ = 7;
    }
}

// 0x4C71E8
void _debug_exit(void)
{
    if (_fd != nullptr) {
        fclose(_fd);
    }
}

} // namespace fallout
