// Fallout 2 Co-op launcher
// Modern SDL2-rendered UI that detects a vanilla Fallout 2 install,
// copies the required game files next to the co-op engine executable
// and launches it.

#include <SDL.h>
#include <tinyfiledialogs.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "detect.h"
#include "font_data.h"
#include "install.h"
#include "launcher_config.h"
#include "ui.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace launcher;

namespace {

const int WINDOW_W = 680;
const int WINDOW_H = 576;

#ifdef _WIN32
std::wstring utf8ToWide(const std::string& s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}
#endif

fs::path p8(const std::string& utf8) { return fs::u8path(utf8); }
std::string s8(const fs::path& p) { return p.u8string(); }

struct App {
    LauncherConfig cfg;
    std::vector<FoundInstall> installs;
    std::vector<std::string> comboItems;
    int selectedInstall = -1;
    std::string sourceDir;
    std::string destDir;

    Installer installer;
    bool installHandled = true; // false while a finished install awaits its message box

    bool gameRunning = false;
#ifdef _WIN32
    HANDLE gameProcess = nullptr;
#else
    pid_t gamePid = -1;
#endif

    bool startEnabled() const
    {
        if (destDir.empty())
            return false;
        std::error_code ec;
        fs::path dest = p8(destDir);
        return fs::is_regular_file(dest / ceExeName(), ec) && isValidInstall(dest);
    }

    fs::path ceExePath() const { return exeDir() / ceExeName(); }
};

// ---------------------------------------------------------------------------
// game launch / tracking
// ---------------------------------------------------------------------------

bool launchGame(App& app, std::string& error)
{
    fs::path exe = p8(app.destDir) / ceExeName();
    std::error_code ec;
    if (!fs::is_regular_file(exe, ec)) {
        error = std::string(ceExeName()) + " is missing in the destination.";
        return false;
    }

#ifdef _WIN32
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring exeW = utf8ToWide(s8(exe));
    std::wstring cwdW = utf8ToWide(app.destDir);
    std::wstring cmd = L"\"" + exeW + L"\"";
    if (!CreateProcessW(exeW.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        cwdW.c_str(), &si, &pi)) {
        error = "Could not start " + std::string(ceExeName()) + " (CreateProcess failed).";
        return false;
    }
    CloseHandle(pi.hThread);
    app.gameProcess = pi.hProcess;
#else
    pid_t pid = fork();
    if (pid < 0) {
        error = "Could not fork a child process.";
        return false;
    }
    if (pid == 0) {
        if (chdir(app.destDir.c_str()) == 0)
            execl("./" + std::string(ceExeName()), ceExeName(), (char*)nullptr);
        _exit(127);
    }
    app.gamePid = pid;
#endif
    app.gameRunning = true;
    return true;
}

// Returns true when the game exited this frame.
bool pollGame(App& app)
{
    if (!app.gameRunning)
        return false;
#ifdef _WIN32
    if (WaitForSingleObject(app.gameProcess, 0) == WAIT_OBJECT_0) {
        CloseHandle(app.gameProcess);
        app.gameProcess = nullptr;
        app.gameRunning = false;
        return true;
    }
#else
    int status = 0;
    if (waitpid(app.gamePid, &status, WNOHANG) == app.gamePid) {
        app.gamePid = -1;
        app.gameRunning = false;
        return true;
    }
#endif
    return false;
}

// ---------------------------------------------------------------------------
// install flow
// ---------------------------------------------------------------------------

void tryStartInstall(App& app, SDL_Window* window)
{
    std::error_code ec;

    if (app.sourceDir.empty() || !isValidInstall(p8(app.sourceDir))) {
        tinyfd_messageBox("Missing source",
                          "Pick a valid Fallout 2 installation first "
                          "(the folder containing master.dat).",
                          "ok", "warning", 0);
        return;
    }
    if (!fs::is_regular_file(app.ceExePath(), ec)) {
        std::string msg = std::string(ceExeName()) +
                          " was not found next to the launcher.\n"
                          "Copy the launcher into the same folder as " + ceExeName() + ".";
        tinyfd_messageBox("Engine executable missing", msg.c_str(), "ok", "error", 0);
        return;
    }
    if (app.destDir.empty()) {
        tinyfd_messageBox("Missing destination", "Choose a destination folder first.", "ok",
                          "warning", 0);
        return;
    }

    fs::path dest = p8(app.destDir);
    if (fs::is_regular_file(dest / ceExeName(), ec)) {
        int yes = tinyfd_messageBox(
            "Overwrite existing installation?",
            "The destination already contains a Fallout 2 co-op installation.\n"
            "Overwrite it?",
            "yesno", "question", 0);
        if (!yes)
            return;
    }

    std::string precheckError;
    if (!app.installer.start(p8(app.sourceDir), dest, app.ceExePath(), precheckError)) {
        tinyfd_messageBox("Cannot install", precheckError.c_str(), "ok", "error", 0);
        return;
    }
    app.installHandled = false;
    (void)window;
}

// ---------------------------------------------------------------------------
// rendering
// ---------------------------------------------------------------------------

void render(App& app)
{
    using namespace ui;

    roundedRect(0, 0, WINDOW_W, WINDOW_H, 0.0f, palette::bg);

    const float m = 24.0f;      // outer margin
    const float cw = WINDOW_W - m * 2;

    // header
    drawText(m, 22, 22, palette::text, "Fallout 2 Co-op");
    drawText(m, 54, 13, palette::textDim, "Set up and launch the co-op multiplayer edition");

    // --- card 1: source installation ---
    float c1y = 86.0f, c1h = 122.0f;
    roundedRect(m, c1y, cw, c1h, 12.0f, palette::card);
    drawText(m + 16, c1y + 14, 11, palette::textDim, "FALLOUT 2 INSTALLATION");

    float fieldW = cw - 32 - 106;
    float row1 = c1y + 36;
    if (combo("srcCombo", m + 16, row1, fieldW, 34, app.comboItems, app.selectedInstall,
              "Detected installations...")) {
        if (app.selectedInstall >= 0 && app.selectedInstall < (int)app.installs.size())
            app.sourceDir = app.installs[(size_t)app.selectedInstall].path;
    }
    if (button("srcBrowse", m + 16 + fieldW + 10, row1, 96, 34, "Browse...", ButtonStyle::Subtle)) {
        std::string start = app.sourceDir.empty() ? std::string() : app.sourceDir;
#ifdef _WIN32
        std::string def = start.empty() ? "fallout2.exe" : start + "\\fallout2.exe";
        const char* patterns[] = { "*.exe" };
        char* picked = tinyfd_openFileDialog("Locate fallout2.exe", def.c_str(), 1, patterns,
                                             "Fallout 2 executable", 0);
        if (picked && *picked) {
            fs::path pickedPath = p8(picked);
            app.sourceDir = s8(pickedPath.parent_path());
        }
#else
        char* picked = tinyfd_selectFolderDialog("Locate the Fallout 2 folder", start.c_str());
        if (picked && *picked)
            app.sourceDir = picked;
#endif
        // deselect combo if the typed/browsed path differs
        app.selectedInstall = -1;
        for (size_t i = 0; i < app.installs.size(); ++i) {
            if (app.installs[i].path == app.sourceDir) {
                app.selectedInstall = (int)i;
                break;
            }
        }
    }

    float row2 = row1 + 42;
    textInput("srcInput", m + 16, row2, cw - 32, 34, app.sourceDir,
              "Path to the original Fallout 2 folder");
    // keep combo highlight in sync with manual edits
    {
        int match = -1;
        for (size_t i = 0; i < app.installs.size(); ++i)
            if (app.installs[i].path == app.sourceDir)
                match = (int)i;
        app.selectedInstall = match;
    }

    // --- card 2: destination ---
    float c2y = c1y + c1h + 16, c2h = 92.0f;
    roundedRect(m, c2y, cw, c2h, 12.0f, palette::card);
    drawText(m + 16, c2y + 14, 11, palette::textDim, "INSTALL DESTINATION");
    float drow = c2y + 40;
    textInput("dstInput", m + 16, drow, fieldW, 34, app.destDir,
              "Where Fallout 2 Co-op will be installed");
    if (button("dstBrowse", m + 16 + fieldW + 10, drow, 96, 34, "Browse...",
               ButtonStyle::Subtle)) {
        char* picked = tinyfd_selectFolderDialog("Choose install destination",
                                                 app.destDir.c_str());
        if (picked && *picked)
            app.destDir = picked;
    }

    // --- install button + progress ---
    float instY = c2y + c2h + 16;
    bool busy = app.installer.busy();
    std::shared_ptr<InstallProgress> prog = app.installer.progress();

    std::string installLabel = busy ? "Installing..." : "Install Fallout 2 Co-op";
    if (button("install", m, instY, cw, 46, installLabel, ButtonStyle::Accent, !busy))
        tryStartInstall(app, nullptr);

    float statusY = instY + 58;
    if (busy && prog) {
        long long total = prog->bytesTotal.load();
        float frac = total > 0 ? (float)((double)prog->bytesDone.load() / (double)total) : 0.0f;
        progressBar(m, statusY, cw, 10, frac);
        std::string file;
        {
            std::lock_guard<std::mutex> lock(prog->textMutex);
            file = prog->currentFile;
        }
        char line[512];
        std::snprintf(line, sizeof(line), "%s  -  %d%%  (%d/%d files)", file.c_str(),
                      (int)(frac * 100), prog->filesDone.load(), prog->filesTotal.load());
        drawText(m, statusY + 18, 12, palette::textDim, line);
    } else if (prog && prog->finished.load() && prog->failed.load()) {
        std::string err;
        {
            std::lock_guard<std::mutex> lock(prog->textMutex);
            err = prog->error;
        }
        drawText(m, statusY, 12, palette::error, err);
    } else if (app.startEnabled()) {
        drawText(m, statusY, 12, palette::success, "Ready to play.");
    }

    // --- card 3: play ---
    float c3y = statusY + 40, c3h = 92.0f;
    roundedRect(m, c3y, cw, c3h, 12.0f, palette::card);

    bool canStart = app.startEnabled() && !app.gameRunning && !busy;
    std::string startLabel = app.gameRunning ? "Running..." : "Start Fallout 2 Co-op";
    if (button("start", m + 16, c3y + 24, 250, 44, startLabel, ButtonStyle::Accent, canStart)) {
        std::string error;
        if (launchGame(app, error)) {
            if (app.cfg.minimizeWhilePlaying)
                ; // minimized below via SDL after the frame
        } else {
            tinyfd_messageBox("Could not start the game", error.c_str(), "ok", "error", 0);
        }
    }
    toggle("minToggle", m + 16 + 250 + 32, c3y + 35, app.cfg.minimizeWhilePlaying,
           "Minimize launcher while playing");

    // footer
    drawTextRight(WINDOW_W - m, WINDOW_H - 24, 11, palette::textDim, "fallout2coop launcher");
}

// ---------------------------------------------------------------------------
// CLI modes
// ---------------------------------------------------------------------------

int runDetectCli()
{
    auto installs = detectInstalls();
    if (installs.empty()) {
        std::printf("No Fallout 2 installations found.\n");
        return 0;
    }
    for (const auto& inst : installs)
        std::printf("[%s] %s\n", inst.source.c_str(), inst.path.c_str());
    return 0;
}

int runInstallCli(const std::string& src, const std::string& dst)
{
    Installer installer;
    fs::path ceExe = exeDir() / ceExeName();
    std::string precheckError;
    if (!installer.start(p8(src), p8(dst), ceExe, precheckError)) {
        std::printf("error: %s\n", precheckError.c_str());
        return 1;
    }
    auto prog = installer.progress();
    int lastPct = -1;
    while (!prog->finished.load()) {
        long long total = prog->bytesTotal.load();
        int pct = total > 0 ? (int)(100.0 * prog->bytesDone.load() / total) : 0;
        if (pct != lastPct) {
            std::printf("\rinstalling... %d%%", pct);
            std::fflush(stdout);
            lastPct = pct;
        }
        SDL_Delay(100);
    }
    std::printf("\rinstalling... 100%%\n");
    if (prog->failed.load()) {
        std::lock_guard<std::mutex> lock(prog->textMutex);
        std::printf("error: %s\n", prog->error.c_str());
        return 1;
    }
    std::printf("done.\n");
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// entry
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    // --- CLI modes (no window) ---
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--detect")
            return runDetectCli();
        if (arg == "--install" && i + 2 < argc)
            return runInstallCli(argv[i + 1], argv[i + 2]);
    }

    bool screenshotMode = false;
    std::string screenshotPath;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--screenshot" && i + 1 < argc) {
            screenshotMode = true;
            screenshotPath = argv[i + 1];
        }
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "0");
    SDL_Window* window = SDL_CreateWindow("Fallout 2 Co-op", SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED, WINDOW_W, WINDOW_H,
                                          SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Renderer* renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | (screenshotMode ? 0 : SDL_RENDERER_PRESENTVSYNC));
    if (!renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return 1;
    }

    {
        int dw = 0, dh = 0, ww = 0, wh = 0;
        SDL_GetRendererOutputSize(renderer, &dw, &dh);
        SDL_GetWindowSize(window, &ww, &wh);
        if (ww > 0)
            ui::setScale((float)dw / (float)ww);
    }

    ui::init(renderer, g_inter_font_ttf, g_inter_font_ttf_size);

    App app;
    app.cfg = LauncherConfig::load();
    app.sourceDir = app.cfg.sourceDir;
    app.destDir = app.cfg.installDir;
    app.installs = detectInstalls();
    for (const auto& inst : app.installs)
        app.comboItems.push_back(inst.path + "   [" + inst.source + "]");
    for (size_t i = 0; i < app.installs.size(); ++i) {
        if (app.installs[i].path == app.sourceDir)
            app.selectedInstall = (int)i;
    }
    // No remembered source yet: default to the first detected install.
    if (app.sourceDir.empty() && !app.installs.empty()) {
        app.selectedInstall = 0;
        app.sourceDir = app.installs[0].path;
    }

    Uint64 lastTicks = SDL_GetTicks();
    bool quit = false;
    bool minimizeRequest = false;

    while (!quit) {
        Uint64 now = SDL_GetTicks();
        float dt = (float)(now - lastTicks) / 1000.0f;
        lastTicks = now;
        if (dt > 0.1f)
            dt = 0.1f;

        ui::beginFrame(dt);

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                quit = true;
            } else {
                ui::handleEvent(e);
            }
        }

        int mx = 0, my = 0;
        Uint32 buttons = SDL_GetMouseState(&mx, &my);
        ui::setMouse((float)mx, (float)my, (buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0);

        // installer finished? -> message box once
        auto prog = app.installer.progress();
        if (!app.installHandled && prog && prog->finished.load()) {
            app.installHandled = true;
            app.installer.join();
            if (prog->failed.load()) {
                std::string err;
                {
                    std::lock_guard<std::mutex> lock(prog->textMutex);
                    err = prog->error;
                }
                tinyfd_messageBox("Installation failed", err.c_str(), "ok", "error", 0);
            } else {
                app.cfg.sourceDir = app.sourceDir;
                app.cfg.installDir = app.destDir;
                app.cfg.save();
                tinyfd_messageBox("Installed",
                                  "Fallout 2 Co-op was installed successfully.\n"
                                  "You can start the game now.",
                                  "ok", "info", 0);
            }
        }

        // game process tracking
        if (pollGame(app)) {
            SDL_RestoreWindow(window);
            SDL_RaiseWindow(window);
        }

        render(app);

        if (app.gameRunning && app.cfg.minimizeWhilePlaying && !minimizeRequest) {
            SDL_MinimizeWindow(window);
            minimizeRequest = true;
        }
        if (!app.gameRunning)
            minimizeRequest = false;

        ui::flushOverlays();
        SDL_RenderPresent(renderer);

        if (screenshotMode) {
            int dw = 0, dh = 0;
            SDL_GetRendererOutputSize(renderer, &dw, &dh);
            std::vector<unsigned char> pixels((size_t)dw * dh * 4);
            if (SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_RGBA32, pixels.data(),
                                     dw * 4) == 0) {
                SDL_Surface* shot = SDL_CreateRGBSurfaceWithFormatFrom(
                    pixels.data(), dw, dh, 32, dw * 4, SDL_PIXELFORMAT_RGBA32);
                if (shot) {
                    SDL_SaveBMP(shot, screenshotPath.c_str());
                    SDL_FreeSurface(shot);
                }
            }
            quit = true;
        }
    }

    app.cfg.sourceDir = app.sourceDir;
    app.cfg.installDir = app.destDir;
    app.cfg.save();

    app.installer.join();
    ui::shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
