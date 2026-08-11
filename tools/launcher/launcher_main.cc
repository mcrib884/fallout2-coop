// Fallout 2 Co-op launcher
// Modern SDL2-rendered UI that detects a vanilla Fallout 2 install,
// copies the required game files next to the co-op engine executable
// and launches it.

#include <SDL.h>
#include <tinyfiledialogs.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "detect.h"
#include "font_data.h"
#include "game_config_editor.h"
#include "install.h"
#include "launcher_config.h"
#include "ui.h"
#include "updater.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
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
    enum class Page {
        Launcher,
        GameConfig,
        Update,
    };

    LauncherConfig cfg;
    std::vector<FoundInstall> installs;
    std::vector<std::string> comboItems;
    int selectedInstall = -1;
    std::string sourceDir; // where the game files are
    std::string destDir;

    // Install version picker (online): every release with a game exe asset,
    // newest first, latest pre-selected. installVersionFeedIdx maps a combo
    // index to a position in feed.releases.
    std::vector<std::string> installVersionItems;
    std::vector<int> installVersionFeedIdx;
    int installVersionSel = -1;
    std::string lastInstallVersion; // version of the release installed last
    std::string installNotice;      // result text after an online install

    Installer installer;
    bool installHandled = true; // false while a finished install awaits its message box

    bool gameRunning = false;
    Page page = Page::Launcher;

    GameConfigEditor gameConfig;
    std::string gameConfigNotice;
    bool gameConfigNoticeError = false;
    int configSection = 0;
    std::vector<float> configScroll;

    Updater updater;
    ReleaseFeed feed;
    std::string installedLauncher; // own exe file version (file properties)
    std::string installedCe;       // game exe file version in destDir
    std::string ceCheckDest;       // destDir the cached installedCe belongs to
    float releasesScroll = 0.0f;
    std::string updateNotice;
    bool updateNoticeError = false;
    int pendingConfirm = 0;  // 0 = none, 1 = game, 2 = launcher (armed confirm)
    Uint64 confirmTicks = 0;
    int updateKind = 0;      // 0 = none, 1 = game, 2 = launcher (active download)
    bool installing = false; // online install job in flight (drives the label)
    bool autoChecked = false;
    bool updatePageAutoCheck = true; // disabled in screenshot mode
    bool quit = false;
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

// The folder that supplies the game files for an install: the chosen
// source, or the launcher's own folder when it is a full distribution.
// Empty when no game files are available anywhere.
fs::path gameFilesSource(const App& app)
{
    if (!app.sourceDir.empty() && isValidInstall(p8(app.sourceDir)))
        return p8(app.sourceDir);
    if (isValidInstall(exeDir()))
        return exeDir();
    return {};
}

void tryStartInstall(App& app, SDL_Window* window)
{
    std::error_code ec;

    if (app.destDir.empty()) {
        tinyfd_messageBox("Missing destination", "Choose a destination folder first.", "ok",
                          "warning", 0);
        return;
    }
    fs::path dest = p8(app.destDir);

    // An install always needs the original game files; refuse without them.
    fs::path filesSrc = gameFilesSource(app);
    if (filesSrc.empty()) {
        tinyfd_messageBox("Missing game files",
                          "Pick the folder with the original Fallout 2 files first "
                          "(the folder containing master.dat).",
                          "ok", "warning", 0);
        return;
    }

    // Online: download the game exe of the chosen release version.
    if (!app.feed.releases.empty()) {
        if (app.installVersionSel < 0 ||
            app.installVersionSel >= (int)app.installVersionFeedIdx.size()) {
            tinyfd_messageBox("No version", "No downloadable game version is available.", "ok",
                              "warning", 0);
            return;
        }
        int feedIdx = app.installVersionFeedIdx[(size_t)app.installVersionSel];
        const ReleaseEntry* entry =
            (feedIdx >= 0 && feedIdx < (int)app.feed.releases.size())
            ? &app.feed.releases[(size_t)feedIdx]
            : nullptr;
        const ReleaseAsset* asset = entry ? entry->findAsset(ceExeName()) : nullptr;
        if (!asset) {
            tinyfd_messageBox("No version", "The chosen release has no game executable.", "ok",
                              "warning", 0);
            return;
        }
        app.lastInstallVersion = !entry->version.empty() ? entry->version : entry->tag;
        std::string error;
        if (!app.updater.startInstallGame(*asset, dest, error)) {
            tinyfd_messageBox("Cannot install", error.c_str(), "ok", "error", 0);
            return;
        }
        app.installing = true;
        app.updateKind = 1;
        app.installNotice.clear();
        return;
    }

    // Offline: game files from the chosen source (or the launcher folder
    // when it is a full distribution), exe from next to the launcher.
    if (!fs::is_regular_file(app.ceExePath(), ec)) {
        tinyfd_messageBox("No internet connection",
                          "theres no internet connection. please put the fallout2coop.exe "
                          "next to the launcher and run again.",
                          "ok", "error", 0);
        return;
    }
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
    if (!app.installer.start(filesSrc, dest, app.ceExePath(), precheckError)) {
        tinyfd_messageBox("Cannot install", precheckError.c_str(), "ok", "error", 0);
        return;
    }
    app.installHandled = false;
    (void)window;
}

// ---------------------------------------------------------------------------
// rendering
// ---------------------------------------------------------------------------

fs::path gameConfigPath(const App& app)
{
    if (app.destDir.empty())
        return {};
    return p8(app.destDir) / "fallout2.cfg";
}

bool gameConfigPathMatchesDestination(const App& app)
{
    return app.gameConfig.loaded() && !app.destDir.empty() &&
           app.gameConfig.path() == gameConfigPath(app);
}

void loadGameConfig(App& app, bool force)
{
    if (app.destDir.empty()) {
        app.gameConfigNotice = "Choose an install destination to edit fallout2.cfg.";
        app.gameConfigNoticeError = true;
        return;
    }

    fs::path target = gameConfigPath(app);
    if (!force && gameConfigPathMatchesDestination(app))
        return;
    if (!force && app.gameConfig.loaded() && app.gameConfig.dirty()) {
        app.gameConfigNotice = "The destination changed. Save or reload before editing the new folder.";
        app.gameConfigNoticeError = true;
        return;
    }

    std::string error;
    if (!app.gameConfig.load(target, error)) {
        app.gameConfigNotice = error;
        app.gameConfigNoticeError = true;
        return;
    }

    app.configScroll.assign(app.gameConfig.sections().size(), 0.0f);
    app.gameConfigNotice = app.gameConfig.fileExists()
        ? "Loaded fallout2.cfg."
        : "No fallout2.cfg found; engine defaults are shown until you save.";
    app.gameConfigNoticeError = false;
}

bool parseConfigBool(const std::string& value)
{
    std::string lowerValue;
    lowerValue.reserve(value.size());
    for (unsigned char c : value)
        lowerValue.push_back((char)std::tolower(c));
    return lowerValue == "1" || lowerValue == "true" || lowerValue == "yes" ||
           lowerValue == "on";
}

std::string fitConfigText(const std::string& text, float size, float maxWidth)
{
    if (ui::textWidth(size, text) <= maxWidth)
        return text;

    std::string result = text;
    const std::string suffix = "...";
    while (!result.empty() && ui::textWidth(size, result + suffix) > maxWidth)
        result.pop_back();
    return result.empty() ? suffix : result + suffix;
}

bool configRowVisible(float y, float h, float top, float bottom)
{
    return y >= top && y + h <= bottom;
}

void renderConfigOption(GameConfigEditor& editor, GameConfigSection& section,
                        GameConfigOption& option, float x, float y, float w)
{
    using namespace ui;

    const float rowH = 54.0f;
    const float controlW = 208.0f;
    const float controlX = x + w - controlW - 16.0f;
    const float descriptionW = controlX - x - 34.0f;

    roundedRect(x + 8.0f, y + 2.0f, w - 16.0f, rowH - 4.0f, 6.0f, palette::input);

    std::string label = option.label;
    if (option.legacy)
        label += "  [legacy]";
    drawText(x + 20.0f, y + 8.0f, 13.0f, palette::text, label);
    drawText(x + 20.0f, y + 28.0f, 10.0f, palette::textDim,
             fitConfigText(option.description, 10.0f, descriptionW));

    const std::string id = "cfg_" + section.key + "_" + option.key;
    std::string before = option.value;

    if (option.type == ConfigValueType::Boolean) {
        bool value = parseConfigBool(option.value);
        bool initialValue = value;
        ui::toggle(id.c_str(), controlX, y + 16.0f, value, "");
        drawText(controlX + 54.0f, y + 21.0f, 12.0f, palette::textDim,
                 value ? "On" : "Off");
        if (value != initialValue)
            option.value = value ? "1" : "0";
    } else if (option.type == ConfigValueType::Choice) {
        std::vector<std::string> labels;
        labels.reserve(option.choices.size());
        int selected = -1;
        for (size_t i = 0; i < option.choices.size(); ++i) {
            labels.push_back(option.choices[i].label);
            if (option.choices[i].value == option.value)
                selected = (int)i;
        }
        if (ui::combo(id.c_str(), controlX, y + 10.0f, controlW, 34.0f, labels, selected,
                      option.value)) {
            if (selected >= 0 && selected < (int)option.choices.size())
                option.value = option.choices[(size_t)selected].value;
        }
    } else {
        const std::string placeholder = option.defaultValue.empty() ? "Value" : option.defaultValue;
        ui::textInput(id.c_str(), controlX, y + 10.0f, controlW, 34.0f, option.value,
                      placeholder);
    }

    if (option.value != before)
        editor.markDirty();
}

void renderGameConfigPage(App& app, SDL_Renderer* renderer, float m, float cw)
{
    using namespace ui;

    loadGameConfig(app, false);

    if (app.destDir.empty()) {
        roundedRect(m, 86.0f, cw, 110.0f, 12.0f, palette::card);
        drawText(m + 16.0f, 104.0f, 15.0f, palette::text, "No installation destination");
        drawText(m + 16.0f, 132.0f, 12.0f, palette::textDim,
                 "Choose a destination on the Launcher tab before editing fallout2.cfg.");
        return;
    }

    const std::vector<GameConfigSection>& constSections = app.gameConfig.sections();
    if (constSections.empty())
        return;
    if (app.configSection < 0 || app.configSection >= (int)constSections.size())
        app.configSection = 0;
    if (app.configScroll.size() != constSections.size())
        app.configScroll.assign(constSections.size(), 0.0f);

    static const char* kSectionLabels[] = {
        "Debug", "Preferences", "Sound", "System", "Screen", "Interface", "QoL",
    };

    const float subY = 86.0f;
    const float subGap = 6.0f;
    const float subW = (cw - subGap * 6.0f) / 7.0f;
    for (size_t i = 0; i < constSections.size() && i < 7; ++i) {
        float x = m + i * (subW + subGap);
        std::string id = "cfg_subtab_" + constSections[i].key;
        ui::ButtonStyle style = ((int)i == app.configSection) ? ui::ButtonStyle::Accent
                                                                : ui::ButtonStyle::Subtle;
        if (ui::button(id.c_str(), x, subY, subW, 30.0f, kSectionLabels[i], style)) {
            app.configSection = (int)i;
            app.configScroll[i] = 0.0f;
        }
    }

    bool pathMatches = gameConfigPathMatchesDestination(app);
    bool canSave = pathMatches;
    if (ui::button("cfgSave", m, 124.0f, 138.0f, 32.0f, "Save changes",
                   ui::ButtonStyle::Accent, canSave)) {
        std::string error;
        if (app.gameConfig.save(error)) {
            app.gameConfigNotice = "Saved fallout2.cfg. Restart the game for changes to take effect.";
            app.gameConfigNoticeError = false;
        } else {
            app.gameConfigNotice = error;
            app.gameConfigNoticeError = true;
        }
    }
    if (ui::button("cfgReload", m + 146.0f, 124.0f, 118.0f, 32.0f, "Reload",
                   ui::ButtonStyle::Subtle, !app.destDir.empty())) {
        bool reload = true;
        if (app.gameConfig.dirty()) {
            reload = tinyfd_messageBox(
                         "Discard unsaved changes?",
                         "Reloading fallout2.cfg will discard the changes made in the launcher.",
                         "yesno", "question", 0) != 0;
        }
        if (reload)
            loadGameConfig(app, true);
    }

    ui::drawText(m + 280.0f, 133.0f, 11.0f,
                 app.gameConfigNoticeError ? ui::palette::error : ui::palette::textDim,
                 fitConfigText(app.gameConfigNotice, 11.0f, cw - 280.0f));

    GameConfigSection& section = app.gameConfig.sections()[(size_t)app.configSection];
    const float viewportTop = 166.0f;
    const float viewportBottom = WINDOW_H - 42.0f;
    const float viewportHeight = viewportBottom - viewportTop;
    const float rowH = 54.0f;
    float contentHeight = rowH * section.options.size() + 8.0f;
    float maxScroll = std::max(0.0f, contentHeight - viewportHeight);
    if (ui::mouseWheelDelta() != 0.0f &&
        ui::mouseOver(m, viewportTop, cw, viewportHeight)) {
        app.configScroll[(size_t)app.configSection] -= ui::mouseWheelDelta() * 30.0f;
        app.configScroll[(size_t)app.configSection] = std::max(
            0.0f, std::min(maxScroll, app.configScroll[(size_t)app.configSection]));
    }

    float scroll = app.configScroll[(size_t)app.configSection];
    float panelY = viewportTop - scroll;
    SDL_Rect clip{
        (int)std::lround(m * ui::scale()),
        (int)std::lround(viewportTop * ui::scale()),
        (int)std::lround(cw * ui::scale()),
        (int)std::lround(viewportHeight * ui::scale()),
    };
    SDL_RenderSetClipRect(renderer, &clip);

    float optionY = panelY;
    for (GameConfigOption& option : section.options) {
        if (configRowVisible(optionY, rowH, viewportTop, viewportBottom))
            renderConfigOption(app.gameConfig, section, option, m, optionY, cw);
        optionY += rowH;
    }

    SDL_RenderSetClipRect(renderer, nullptr);
}

// ---------------------------------------------------------------------------
// update page
// ---------------------------------------------------------------------------

bool componentUpToDate(const std::string& installed, const std::string& latestTag)
{
    if (installed.empty() || latestTag.empty())
        return false;
    return versionCompare(installed, latestTag) >= 0;
}

void startUpdateCheck(App& app)
{
    if (app.updater.busy())
        return;
    std::string url =
        "https://api.github.com/repos/" + app.cfg.updatesRepo + "/releases?per_page=100";
    std::string error;
    if (!app.updater.startCheck(url, error)) {
        app.updateNotice = error;
        app.updateNoticeError = true;
    }
}

void startGameUpdate(App& app)
{
    const ReleaseEntry* latest = app.feed.ceLatest();
    const ReleaseAsset* asset = latest ? latest->findAsset(ceExeName()) : nullptr;
    if (!asset || app.destDir.empty())
        return;
    std::string error;
    if (!app.updater.startGameUpdate(*asset, p8(app.destDir), error)) {
        app.updateNotice = error;
        app.updateNoticeError = true;
        return;
    }
    app.updateKind = 1;
}

void startLauncherUpdate(App& app)
{
    const ReleaseEntry* latest = app.feed.launcherLatest();
    const ReleaseAsset* asset = latest ? latest->findAsset(launcherExeName()) : nullptr;
    if (!asset)
        return;
    std::string error;
    if (!app.updater.startLauncherUpdate(*asset, exeDir(), error)) {
        app.updateNotice = error;
        app.updateNoticeError = true;
        return;
    }
    app.updateKind = 2;
}

// Settles finished update jobs once per frame, shared by every page:
// copies the feed, refreshes installed versions and produces notices.
void settleUpdateJob(App& app)
{
    std::shared_ptr<UpdateState> st = app.updater.state();
    if (!st)
        return;
    UpdatePhase phase = st->phase.load();
    if (phase == UpdatePhase::Done) {
        std::string result;
        {
            std::lock_guard<std::mutex> lock(st->mutex);
            result = st->result;
        }
        if (result == "check") {
            {
                std::lock_guard<std::mutex> lock(st->mutex);
                app.feed = st->feed;
            }
            // The newest release with content under a component's marker is
            // that component's latest version; the two can differ. Compare
            // against the release's version (from its version.json), falling
            // back to the tag when the record is missing.
            const ReleaseEntry* ceLatest = app.feed.ceLatest();
            const ReleaseEntry* launcherLatest = app.feed.launcherLatest();
            std::string ceVer = ceLatest
                ? (!ceLatest->version.empty() ? ceLatest->version : ceLatest->tag)
                : std::string();
            std::string launcherVer = launcherLatest
                ? (!launcherLatest->version.empty() ? launcherLatest->version
                                                    : launcherLatest->tag)
                : std::string();
            // A missing game exe is NOT up to date - componentUpToDate already
            // returns false for an empty installed version.
            bool gameUp = ceLatest == nullptr ||
                          componentUpToDate(app.installedCe, ceVer);
            bool launcherUp = launcherLatest == nullptr ||
                              componentUpToDate(app.installedLauncher, launcherVer);
            std::string what;
            if (!gameUp)
                what = "game " + app.feed.ceLatestTag;
            if (!launcherUp) {
                if (!what.empty())
                    what += ", ";
                what += "launcher " + app.feed.launcherLatestTag;
            }
            app.updateNotice = what.empty() ? "Up to date." : "Update available: " + what + ".";
            app.updateNoticeError = false;
            app.releasesScroll = 0.0f;

            // Rebuild the install version picker: every release that carries
            // a game exe asset, newest first, latest selected by default.
            app.installVersionItems.clear();
            app.installVersionFeedIdx.clear();
            for (size_t i = 0; i < app.feed.releases.size(); ++i) {
                const ReleaseEntry& e = app.feed.releases[i];
                if (!e.findAsset(ceExeName()))
                    continue;
                app.installVersionItems.push_back(
                    e.tag + "  (" + (e.version.empty() ? "?" : e.version) + ")");
                app.installVersionFeedIdx.push_back((int)i);
            }
            app.installVersionSel = app.installVersionItems.empty() ? -1 : 0;
            st->phase.store(UpdatePhase::Idle);
        } else if (result == "game") {
            // refresh the installed game version from the exe properties
            app.installedCe = app.destDir.empty()
                ? std::string()
                : exeFileVersion(p8(app.destDir) / ceExeName());
            app.ceCheckDest = app.destDir;
            app.updateKind = 0;
            const ReleaseEntry* ceLatest = app.feed.ceLatest();
            std::string updatedTo = ceLatest
                ? (!ceLatest->version.empty() ? ceLatest->version : ceLatest->tag)
                : std::string();
            app.updateNotice = "The game was updated to " +
                               (updatedTo.empty() ? "the latest release" : updatedTo) + ".";
            app.updateNoticeError = false;
            st->phase.store(UpdatePhase::Idle);
        } else if (result == "install") {
            // refresh the installed game version from the exe properties
            app.installedCe = app.destDir.empty()
                ? std::string()
                : exeFileVersion(p8(app.destDir) / ceExeName());
            app.ceCheckDest = app.destDir;
            app.updateKind = 0;
            app.installNotice =
                "Fallout 2 Co-op (" +
                (app.lastInstallVersion.empty() ? "the chosen release" : app.lastInstallVersion) +
                ") was installed to " + app.destDir + ".";
            app.installing = false;
            // The exe is in place from the download; copy the game files
            // from the source as well (it was required before installing).
            {
                fs::path filesSrc = gameFilesSource(app);
                if (!filesSrc.empty()) {
                    std::string perr;
                    if (app.installer.startFilesOnly(filesSrc, p8(app.destDir), perr))
                        app.installHandled = false;
                } else {
                    app.installNotice +=
                        " The game files are missing - pick the original Fallout 2 folder above "
                        "and install again to copy them.";
                }
            }
            app.updateNotice = app.installNotice;
            app.updateNoticeError = false;
            st->phase.store(UpdatePhase::Idle);
        } else if (result == "launcher") {
            std::string error;
            if (spawnSelfUpdate(exeDir(), error)) {
                app.updateNotice = "Launcher updated - restarting...";
                app.updateNoticeError = false;
                app.quit = true;
            } else {
                app.updateKind = 0;
                app.updateNotice = error;
                app.updateNoticeError = true;
                st->phase.store(UpdatePhase::Idle);
            }
        }
    } else if (phase == UpdatePhase::Failed) {
        std::string error;
        {
            std::lock_guard<std::mutex> lock(st->mutex);
            error = st->error;
        }
        app.updateKind = 0;
        app.installing = false;
        app.updateNotice = error;
        app.updateNoticeError = true;
        st->phase.store(UpdatePhase::Idle);
    }

    // Reap the finished worker so busy() reflects reality again; a finished
    // but unjoined thread kept every busy-gated button disabled forever.
    if (phase == UpdatePhase::Done || phase == UpdatePhase::Failed)
        app.updater.join();
}

void renderUpdatePage(App& app, SDL_Renderer* renderer, float m, float cw)
{
    using namespace ui;

    std::shared_ptr<UpdateState> st = app.updater.state();
    UpdatePhase phase = st ? st->phase.load() : UpdatePhase::Idle;

    const Uint64 now = SDL_GetTicks();
    if (app.pendingConfirm != 0 && now - app.confirmTicks > 5000)
        app.pendingConfirm = 0;

    const bool busy = app.updater.busy();
    const float cardX = m, cardW = cw;

    // --- card 1: release status ---
    const float c1y = 86.0f, c1h = 112.0f;
    roundedRect(cardX, c1y, cardW, c1h, 12.0f, palette::card);
    drawText(cardX + 16.0f, c1y + 14.0f, 11.0f, palette::textDim, "RELEASE STATUS");
    if (button("updateCheck", cardX + cardW - 136.0f, c1y + 14.0f, 120.0f, 30.0f,
               "Check now", ButtonStyle::Subtle, !busy && phase != UpdatePhase::Checking)) {
        app.pendingConfirm = 0;
        startUpdateCheck(app);
    }

    std::string installedLine = "Installed: launcher " +
        (app.installedLauncher.empty() ? "?" : app.installedLauncher) +
        "   game " + (app.installedCe.empty() ? "-" : app.installedCe);
    drawText(cardX + 16.0f, c1y + 46.0f, 12.0f, palette::text, installedLine);

    std::string latestLine;
    Color latestColor = palette::textDim;
    if (phase == UpdatePhase::Checking) {
        latestLine = "Checking for updates...";
    } else if (phase == UpdatePhase::Downloading) {
        latestLine = "Downloading update...";
    } else if (phase == UpdatePhase::Failed) {
        std::string error;
        {
            std::lock_guard<std::mutex> lock(st->mutex);
            error = st->error;
        }
        latestLine = fitConfigText(error, 11.0f, cardW - 32.0f);
        latestColor = palette::error;
    } else if (!app.feed.ceLatestTag.empty() || !app.feed.launcherLatestTag.empty()) {
        latestLine = "Latest:";
        if (!app.feed.ceLatestTag.empty())
            latestLine += "  game " + app.feed.ceLatestTag;
        if (!app.feed.launcherLatestTag.empty()) {
            if (!app.feed.ceLatestTag.empty())
                latestLine += "   |   ";
            latestLine += "launcher " + app.feed.launcherLatestTag;
        }
        latestColor = palette::text;
    } else {
        latestLine = "Press Check now to see the latest release.";
    }
    drawText(cardX + 16.0f, c1y + 64.0f, 12.0f, latestColor, latestLine);
    drawText(cardX + 16.0f, c1y + 84.0f, 11.0f,
             app.updateNoticeError ? palette::error : palette::textDim,
             fitConfigText(app.updateNotice, 11.0f, cardW - 32.0f));

    // --- card 2: game exe ---
    const float c2y = c1y + c1h + 16.0f, c2h = 104.0f;
    roundedRect(cardX, c2y, cardW, c2h, 12.0f, palette::card);
    drawText(cardX + 16.0f, c2y + 14.0f, 11.0f, palette::textDim, "GAME  (fallout2coop.exe)");

    // cache the installed game version per destination (exe properties)
    if (app.ceCheckDest != app.destDir) {
        app.ceCheckDest = app.destDir;
        app.installedCe = app.destDir.empty()
            ? std::string()
            : exeFileVersion(p8(app.destDir) / ceExeName());
    }

    const ReleaseEntry* ceLatest = app.feed.ceLatest();
    const ReleaseAsset* gameAsset = ceLatest ? ceLatest->findAsset(ceExeName()) : nullptr;
    std::string ceLatestVersion = ceLatest
        ? (!ceLatest->version.empty() ? ceLatest->version : ceLatest->tag)
        : std::string();
    bool gameNotInstalled = !app.destDir.empty() && app.installedCe.empty();
    bool gameUpToDate = ceLatest == nullptr ||
                        componentUpToDate(app.installedCe, ceLatestVersion);

    std::string gameStatus;
    if (app.gameRunning) {
        gameStatus = "Close the game before updating.";
    } else if (app.updateKind == 1) {
        gameStatus = "Downloading the new game exe...";
    } else if (app.destDir.empty()) {
        gameStatus = "Set an install destination on the Launcher tab first.";
    } else if (gameNotInstalled) {
        gameStatus = "Game not installed in the destination.";
    } else if (ceLatest == nullptr) {
        gameStatus = "No game update has been released.";
    } else if (gameAsset == nullptr) {
        gameStatus = "The latest game release has no " + std::string(ceExeName()) + " asset.";
    } else if (gameUpToDate) {
        gameStatus = "Up to date (" + app.installedCe + ").";
    } else {
        gameStatus = "Update available: " + ceLatestVersion + ".";
    }
    drawText(cardX + 16.0f, c2y + 40.0f, 12.0f, palette::textDim, gameStatus);

    if (app.updateKind == 1 && st) {
        long long total = st->bytesTotal.load();
        float frac = total > 0 ? (float)((double)st->bytesDone.load() / (double)total) : 0.0f;
        progressBar(cardX + 16.0f, c2y + 64.0f, 300.0f, 8.0f, frac);
        char pct[32];
        std::snprintf(pct, sizeof(pct), "%d%%", (int)(frac * 100.0f));
        drawText(cardX + 330.0f, c2y + 61.0f, 11.0f, palette::textDim, pct);
    }

    if (gameNotInstalled && !app.gameRunning) {
        // no game exe in the destination -> offer the installer
        if (button("updGame", cardX + cardW - 166.0f, c2y + 38.0f, 150.0f, 40.0f,
                   "Install game", ButtonStyle::Accent, !busy)) {
            app.pendingConfirm = 0;
            tryStartInstall(app, nullptr);
        }
    } else {
        bool gameCanUpdate = ceLatest != nullptr && gameAsset != nullptr && !app.gameRunning &&
                             !app.destDir.empty() && !app.installedCe.empty() && !busy &&
                             !gameUpToDate;
        std::string gameLabel = app.pendingConfirm == 1 ? "Confirm update?" : "Update game";
        if (button("updGame", cardX + cardW - 166.0f, c2y + 38.0f, 150.0f, 40.0f, gameLabel,
                   ButtonStyle::Accent, gameCanUpdate)) {
            if (app.pendingConfirm == 1) {
                app.pendingConfirm = 0;
                startGameUpdate(app);
            } else {
                app.pendingConfirm = 1;
                app.confirmTicks = now;
            }
        }
    }

    // --- card 3: launcher exe ---
    const float c3y = c2y + c2h + 16.0f, c3h = 104.0f;
    roundedRect(cardX, c3y, cardW, c3h, 12.0f, palette::card);
    drawText(cardX + 16.0f, c3y + 14.0f, 11.0f, palette::textDim,
             "LAUNCHER  (fallout2coop_launcher.exe)");

    const ReleaseEntry* launcherLatest = app.feed.launcherLatest();
    const ReleaseAsset* launcherAsset =
        launcherLatest ? launcherLatest->findAsset(launcherExeName()) : nullptr;
    std::string launcherLatestVersion = launcherLatest
        ? (!launcherLatest->version.empty() ? launcherLatest->version
                                            : launcherLatest->tag)
        : std::string();
    bool launcherUpToDate = launcherLatest == nullptr ||
                            componentUpToDate(app.installedLauncher, launcherLatestVersion);

    std::string launcherStatus;
    if (app.updateKind == 2) {
        launcherStatus = "Downloading the new launcher...";
    } else if (launcherLatest == nullptr) {
        launcherStatus = "No launcher update has been released.";
    } else if (launcherAsset == nullptr) {
        launcherStatus = "The latest launcher release has no " +
                         std::string(launcherExeName()) + " asset.";
    } else if (launcherUpToDate) {
        launcherStatus = "Up to date (" + app.installedLauncher + ").";
    } else {
        launcherStatus = "Update available: " + launcherLatestVersion + ".";
    }
    drawText(cardX + 16.0f, c3y + 40.0f, 12.0f, palette::textDim, launcherStatus);

    if (app.updateKind == 2 && st) {
        long long total = st->bytesTotal.load();
        float frac = total > 0 ? (float)((double)st->bytesDone.load() / (double)total) : 0.0f;
        progressBar(cardX + 16.0f, c3y + 64.0f, 300.0f, 8.0f, frac);
        char pct[32];
        std::snprintf(pct, sizeof(pct), "%d%%", (int)(frac * 100.0f));
        drawText(cardX + 330.0f, c3y + 61.0f, 11.0f, palette::textDim, pct);
    }

    bool launcherCanUpdate = launcherLatest != nullptr && launcherAsset != nullptr && !busy &&
                             !launcherUpToDate;
    std::string launcherLabel =
        app.pendingConfirm == 2 ? "Confirm update?" : "Update launcher";
    if (button("updLauncher", cardX + cardW - 166.0f, c3y + 38.0f, 150.0f, 40.0f,
               launcherLabel, ButtonStyle::Accent, launcherCanUpdate)) {
        if (app.pendingConfirm == 2) {
            app.pendingConfirm = 0;
            startLauncherUpdate(app);
        } else {
            app.pendingConfirm = 2;
            app.confirmTicks = now;
        }
    }
    drawText(cardX + 16.0f, c3y + 88.0f, 11.0f, palette::textDim,
             "The launcher closes and restarts after updating.");

    // --- card 4: all releases ---
    const float c4y = c3y + c3h + 16.0f, c4h = WINDOW_H - 8.0f - c4y;
    roundedRect(cardX, c4y, cardW, c4h, 12.0f, palette::card);
    drawText(cardX + 16.0f, c4y + 12.0f, 11.0f, palette::textDim, "RELEASES");

    const float listTop = c4y + 34.0f;
    const float listBottom = c4y + c4h - 6.0f;
    const float rowH = 24.0f;
    float contentH = rowH * app.feed.releases.size();
    float maxScroll = std::max(0.0f, contentH - (listBottom - listTop));
    if (ui::mouseWheelDelta() != 0.0f && ui::mouseOver(cardX, listTop, cardW, listBottom - listTop)) {
        app.releasesScroll -= ui::mouseWheelDelta() * 30.0f;
        app.releasesScroll = std::max(0.0f, std::min(maxScroll, app.releasesScroll));
    }
    {
        SDL_Rect clip{
            (int)std::lround(cardX * ui::scale()),
            (int)std::lround(listTop * ui::scale()),
            (int)std::lround(cardW * ui::scale()),
            (int)std::lround((listBottom - listTop) * ui::scale()),
        };
        SDL_RenderSetClipRect(renderer, &clip);
        float rowY = listTop - app.releasesScroll;
        for (const ReleaseEntry& e : app.feed.releases) {
            if (configRowVisible(rowY, rowH, listTop, listBottom)) {
                roundedRect(cardX + 8.0f, rowY + 2.0f, cardW - 16.0f, rowH - 4.0f, 4.0f,
                            palette::input);
                std::string line = e.tag;
                line += "   (" + (e.version.empty() ? "?" : e.version) + ")";
                if (e.updatesCe())
                    line += "   game";
                if (e.updatesLauncher())
                    line += "   launcher";
                drawText(cardX + 16.0f, rowY + 5.0f, 12.0f, palette::text, line);
            }
            rowY += rowH;
        }
        SDL_RenderSetClipRect(renderer, nullptr);
    }
}

void render(App& app, SDL_Renderer* renderer)
{
    using namespace ui;

    // settle finished update jobs once per frame (any page)
    settleUpdateJob(app);

    // first entry: check automatically for the install version list
    // (Launcher page) and for the Update page (unless disabled, e.g.
    // screenshot mode)
    {
        std::shared_ptr<UpdateState> st = app.updater.state();
        UpdatePhase phase = st ? st->phase.load() : UpdatePhase::Idle;
        if (!app.autoChecked && !app.updater.busy() && phase == UpdatePhase::Idle &&
            (app.page == App::Page::Launcher ||
             (app.page == App::Page::Update && app.updatePageAutoCheck))) {
            app.autoChecked = true;
            startUpdateCheck(app);
        }
    }

    roundedRect(0, 0, WINDOW_W, WINDOW_H, 0.0f, palette::bg);

    const float m = 24.0f;      // outer margin
    const float cw = WINDOW_W - m * 2;

    // header
    drawText(m, 22, 22, palette::text, "Fallout 2 Co-op");
    const char* subtitle = app.page == App::Page::Launcher
        ? "Set up and launch Fallout 2 Co-op edition."
        : (app.page == App::Page::GameConfig
               ? "Configure the game installation"
               : "Update the launcher and the game to the latest release.");
    drawText(m, 54, 13, palette::textDim, subtitle);

    if (button("updateTab", WINDOW_W - m - 378.0f, 18.0f, 128.0f, 32.0f, "Update",
                app.page == App::Page::Update ? ButtonStyle::Accent : ButtonStyle::Subtle))
        app.page = App::Page::Update;
    if (button("launcherTab", WINDOW_W - m - 240.0f, 18.0f, 104.0f, 32.0f, "Launcher",
                app.page == App::Page::Launcher ? ButtonStyle::Accent : ButtonStyle::Subtle))
        app.page = App::Page::Launcher;
    if (button("gameConfigTab", WINDOW_W - m - 128.0f, 18.0f, 128.0f, 32.0f, "Config",
                app.page == App::Page::GameConfig ? ButtonStyle::Accent : ButtonStyle::Subtle))
        app.page = App::Page::GameConfig;

    if (app.page == App::Page::GameConfig) {
        renderGameConfigPage(app, renderer, m, cw);
        return;
    }
    if (app.page == App::Page::Update) {
        renderUpdatePage(app, renderer, m, cw);
        return;
    }

    // --- card 1: where the game files are ---
    float c1y = 86.0f, c1h = 116.0f;
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

    // --- card 2: install destination ---
    float c2y = c1y + c1h + 16, c2h = 80.0f;
    roundedRect(m, c2y, cw, c2h, 12.0f, palette::card);
    drawText(m + 16, c2y + 14, 11, palette::textDim, "INSTALL DESTINATION");
    textInput("dstInput", m + 16, c2y + 34, fieldW, 34, app.destDir,
              "Where Fallout 2 Co-op will be installed");
    if (button("dstBrowse", m + 16 + fieldW + 10, c2y + 34, 96, 34, "Browse...",
               ButtonStyle::Subtle)) {
        char* picked = tinyfd_selectFolderDialog("Choose install destination",
                                                 app.destDir.c_str());
        if (picked && *picked)
            app.destDir = picked;
    }

    // --- card 3: game version (online) / offline warning ---
    float c3y = c2y + c2h + 16, c3h = 80.0f;
    roundedRect(m, c3y, cw, c3h, 12.0f, palette::card);
    drawText(m + 16, c3y + 14, 11, palette::textDim, "GAME VERSION");

    std::shared_ptr<UpdateState> ust = app.updater.state();
    UpdatePhase uphase = ust ? ust->phase.load() : UpdatePhase::Idle;
    const bool online = !app.feed.releases.empty();
    const bool checking =
        !online && (uphase == UpdatePhase::Checking || app.updater.busy());
    const bool localExe = fs::is_regular_file(app.ceExePath());
    const bool filesAvailable = !gameFilesSource(app).empty();

    if (checking) {
        drawText(m + 16, c3y + 38, 12, palette::textDim, "Checking for updates...");
    } else if (online) {
        combo("verCombo", m + 16, c3y + 34, cw - 32, 34, app.installVersionItems,
              app.installVersionSel, "No game versions available");
    } else if (!localExe) {
        drawText(m + 16, c3y + 34, 12, palette::error, "theres no internet connection.");
        drawText(m + 16, c3y + 56, 12, palette::error,
                 "please put the fallout2coop.exe next to the launcher and run again.");
    } else if (!filesAvailable) {
        drawText(m + 16, c3y + 38, 12, palette::error,
                 "No internet - pick the folder with the original");
        drawText(m + 16, c3y + 56, 12, palette::error,
                 "Fallout 2 files above to install.");
    } else {
        drawText(m + 16, c3y + 38, 12, palette::textDim,
                 "No internet connection - installing from the local files.");
    }

    // --- install button + progress ---
    float instY = c3y + c3h + 16;
    bool busy = app.installer.busy() || app.updater.busy();
    std::shared_ptr<InstallProgress> prog = app.installer.progress();

    bool canInstall = !busy && !checking && !app.destDir.empty() && filesAvailable &&
                      (online || localExe);
    bool installing = app.installing || app.installer.busy();
    std::string installLabel = installing ? "Installing..." : "Install Fallout 2 Co-op";
    if (button("install", m, instY, cw, 46, installLabel, ButtonStyle::Accent, canInstall))
        tryStartInstall(app, nullptr);

    float statusY = instY + 54;
    if (app.updateKind == 1 && ust && ust->phase.load() == UpdatePhase::Downloading) {
        long long total = ust->bytesTotal.load();
        float frac = total > 0 ? (float)((double)ust->bytesDone.load() / (double)total) : 0.0f;
        progressBar(m, statusY, cw, 10, frac);
        char pct[32];
        std::snprintf(pct, sizeof(pct), "%d%%", (int)(frac * 100.0f));
        drawText(m, statusY + 18, 12, palette::textDim, pct);
    } else if (busy && prog) {
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
    } else if (!app.installNotice.empty()) {
        drawText(m, statusY, 12, palette::success, fitConfigText(app.installNotice, 12.0f, cw));
    } else if (!filesAvailable) {
        drawText(m, statusY, 12, palette::error,
                 "Pick the folder with the original Fallout 2 files above first.");
    } else if (app.startEnabled()) {
        drawText(m, statusY, 12, palette::success, "Ready to play.");
    }

    // --- card 4: play ---
    float c4y = statusY + 38, c4h = WINDOW_H - 8.0f - c4y;
    roundedRect(m, c4y, cw, c4h, 12.0f, palette::card);

    bool canStart = app.startEnabled() && !app.gameRunning && !busy;
    std::string startLabel = app.gameRunning ? "Running..." : "Start Fallout 2 Co-op";
    if (button("start", m + 16, c4y + 14, 250, 44, startLabel, ButtonStyle::Accent, canStart)) {
        std::string error;
        if (launchGame(app, error)) {
            if (app.cfg.minimizeWhilePlaying)
                ; // minimized below via SDL after the frame
        } else {
            tinyfd_messageBox("Could not start the game", error.c_str(), "ok", "error", 0);
        }
    }
    toggle("minToggle", m + 16 + 250 + 32, c4y + 25, app.cfg.minimizeWhilePlaying,
           "Minimize launcher while playing");
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

int runUpdateCheckCli(const std::string& apiUrl)
{
    ReleaseFeed feed;
    std::string error;
    if (!fetchReleaseFeedWithVersions(apiUrl, feed, error)) {
        std::printf("error: %s\n", error.c_str());
        return 1;
    }
    std::printf("releases: %zu\n", feed.releases.size());
    for (const ReleaseEntry& e : feed.releases) {
        std::printf("%s: version=%s game=%s launcher=%s (%zu assets)\n", e.tag.c_str(),
                    e.version.empty() ? "?" : e.version.c_str(),
                    e.updatesCe() ? "yes" : "no", e.updatesLauncher() ? "yes" : "no",
                    e.assets.size());
    }
    std::printf("latest game: %s\n",
                feed.ceLatestTag.empty() ? "-" : feed.ceLatestTag.c_str());
    std::printf("latest launcher: %s\n",
                feed.launcherLatestTag.empty() ? "-" : feed.launcherLatestTag.c_str());

    auto printAssets = [](const ReleaseEntry* e) {
        if (!e)
            return;
        std::printf("-- %s assets:\n", e->tag.c_str());
        for (const ReleaseAsset& a : e->assets) {
            std::printf("   %s  (%lld bytes)  digest=%s\n     %s\n", a.name.c_str(), a.size,
                        a.digest.empty() ? "-" : a.digest.c_str(), a.url.c_str());
        }
    };
    printAssets(feed.ceLatest());
    if (feed.launcherLatest() != feed.ceLatest())
        printAssets(feed.launcherLatest());
    return 0;
}

int runUpdateDownloadCli(const std::string& url, const std::string& dest,
                         const std::string& digest)
{
    std::atomic<long long> done{ 0 }, total{ 0 };
    std::string error;
    if (!downloadFile(url, p8(dest), done, total, error)) {
        std::printf("error: %s\n", error.c_str());
        return 1;
    }
    std::printf("downloaded %s: %lld bytes\n", dest.c_str(), done.load());
    if (!digest.empty()) {
        if (verifyDigest(p8(dest), digest)) {
            std::printf("sha256: verified\n");
        } else {
            std::printf("sha256: MISMATCH\n");
            return 1;
        }
    }
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
        if (arg == "--update-check" && i + 1 < argc)
            return runUpdateCheckCli(argv[i + 1]);
        if (arg == "--update-download" && i + 2 < argc)
            return runUpdateDownloadCli(argv[i + 1], argv[i + 2],
                                        i + 3 < argc ? argv[i + 3] : "");
        if (arg == "--version") {
            std::string v = exeFileVersion(exeDir() / launcherExeName());
            std::printf("%s\n", v.empty() ? "unknown" : v.c_str());
            return 0;
        }
    }

    bool screenshotMode = false;
    std::string screenshotPath;
    bool screenshotUpdatePage = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--screenshot" && i + 1 < argc) {
            screenshotMode = true;
            screenshotPath = argv[i + 1];
            if (i + 2 < argc && std::string(argv[i + 2]) == "update")
                screenshotUpdatePage = true;
        }
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "0");
    const float kDefaultWindowScale = 1.3f;
    SDL_Window* window = SDL_CreateWindow(
        "Fallout 2 Co-op", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        (int)std::lround(WINDOW_W * kDefaultWindowScale),
        (int)std::lround(WINDOW_H * kDefaultWindowScale),
        SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
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

    ui::init(renderer, g_launcher_font_ttf, g_launcher_font_ttf_size);
    ui::setCanvas(WINDOW_W, WINDOW_H);

    App app;
    app.cfg = LauncherConfig::load();
    app.sourceDir = app.cfg.sourceDir;
    app.destDir = app.cfg.installDir;
    app.installedLauncher = exeFileVersion(exeDir() / launcherExeName());
    app.installedCe = app.destDir.empty()
        ? std::string()
        : exeFileVersion(p8(app.destDir) / ceExeName());
    app.ceCheckDest = app.destDir;
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

    if (screenshotUpdatePage) {
        app.page = App::Page::Update;
        app.updatePageAutoCheck = false;
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

        render(app, renderer);

        if (app.gameRunning && app.cfg.minimizeWhilePlaying && !minimizeRequest) {
            SDL_MinimizeWindow(window);
            minimizeRequest = true;
        }
        if (!app.gameRunning)
            minimizeRequest = false;

        ui::flushOverlays();
        SDL_RenderPresent(renderer);

        if (app.quit)
            quit = true;

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
