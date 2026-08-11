#ifndef FALLOUT_LAUNCHER_UPDATER_H
#define FALLOUT_LAUNCHER_UPDATER_H

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace launcher {

// Name of the launcher executable (used to pick release assets and to stage
// self-updates).
const char* launcherExeName();

// One file attached to a GitHub release.
struct ReleaseAsset {
    std::string name;
    std::string url;    // browser_download_url
    std::string digest; // "sha256:<hex>" when GitHub provides it
    long long size = 0;
};

// The release description carries two section markers that say which
// components the release actually updates:
//   {Fallout 2 Coop}  -> the game exe
//   {Launcher}        -> the launcher
// A section with no content (or no marker at all) means that component was
// not updated in this release. Each release also carries a version.json
// asset ("ce" / "launcher" fields) whose values match the file version
// embedded in the exes; that is the release's version.
struct ReleaseEntry {
    std::string tag;                // e.g. "v0.2.0"
    std::string version;            // from the release's version.json (ce field)
    std::string body;               // raw description
    std::string ceChangelog;        // {Fallout 2 Coop} section (trimmed)
    std::string launcherChangelog;  // {Launcher} section (trimmed)
    std::vector<ReleaseAsset> assets;

    bool updatesCe() const { return !ceChangelog.empty(); }
    bool updatesLauncher() const { return !launcherChangelog.empty(); }
    const ReleaseAsset* findAsset(const std::string& name) const;
};

// Result of scanning the release list (newest first): the newest release
// that updates a component counts as that component's latest version.
struct ReleaseFeed {
    std::vector<ReleaseEntry> releases;
    std::string ceLatestTag;       // empty when no release updates the game
    std::string launcherLatestTag; // empty when no release updates the launcher

    const ReleaseEntry* ceLatest() const;
    const ReleaseEntry* launcherLatest() const;

private:
    int ceLatestIdx_ = -1;
    int launcherLatestIdx_ = -1;
    friend bool fetchReleaseFeedUrl(const std::string&, ReleaseFeed&, std::string&);
};

// Fetches the GitHub releases list from a full API URL, parses the
// per-release description sections and derives the latest version of each
// component. Fills error on failure.
bool fetchReleaseFeedUrl(const std::string& apiUrl, ReleaseFeed& out, std::string& error);

// fetchReleaseFeedUrl plus, for every release, downloads its version.json
// asset and assigns it to entry.version (missing/failed fetches leave it
// empty; the list itself is still returned).
bool fetchReleaseFeedWithVersions(const std::string& apiUrl, ReleaseFeed& out,
                                  std::string& error);

// Convenience wrapper: https://api.github.com/repos/<repo>/releases?per_page=100
bool fetchReleaseFeed(const std::string& repo, ReleaseFeed& out, std::string& error);

// Downloads url to dest, streaming; reports progress through the atomics.
// bytesTotal is 0 when the server sends no content-length (indeterminate).
bool downloadFile(const std::string& url, const std::filesystem::path& dest,
                  std::atomic<long long>& bytesDone, std::atomic<long long>& bytesTotal,
                  std::string& error);

// SHA-256 of a file as lowercase hex; empty string on failure.
std::string sha256Hex(const std::filesystem::path& file);

// True when the file matches a GitHub asset digest ("sha256:<hex>").
// Digests without that prefix are not verifiable and pass.
bool verifyDigest(const std::filesystem::path& file, const std::string& digest);

// Windows file version of an exe (FileVersion property), e.g. "0.1.0".
// Empty when the file is missing, has no version resource, or on non-Windows.
std::string exeFileVersion(const std::filesystem::path& path);

// Compares dotted versions ("1.2.3", "v1.2.3", "1.2.3-rc1"); -1/0/1.
// A release suffix sorts older than the same version without one.
int versionCompare(const std::string& a, const std::string& b);

// Writes a small updater batch into %TEMP% and spawns it detached. The batch
// waits 2s for this process to exit, swaps fallout2coop_launcher.new.exe in
// and relaunches. Windows only.
bool spawnSelfUpdate(const std::filesystem::path& exeDir, std::string& error);

// ---------------------------------------------------------------------------
// Worker-thread update jobs (mirrors the Installer pattern).
// ---------------------------------------------------------------------------

enum class UpdatePhase { Idle, Checking, Downloading, Done, Failed };

struct UpdateState {
    std::atomic<UpdatePhase> phase{ UpdatePhase::Idle };
    std::atomic<long long> bytesDone{ 0 };
    std::atomic<long long> bytesTotal{ 0 };

    std::mutex mutex; // guards everything below
    ReleaseFeed feed; // filled after a successful check
    std::string error;   // filled on failure
    std::string result;  // "check" | "game" | "install" | "launcher" on success
};

class Updater {
public:
    ~Updater();

    bool busy() const; // worker thread alive
    void join();
    std::shared_ptr<UpdateState> state() const { return state_; }

    // Fetches the release list with per-release versions. Returns false
    // (with error) when busy.
    bool startCheck(const std::string& apiUrl, std::string& error);
    // Downloads the game exe asset to a temp folder, verifies it, then
    // installs it into destDir (backing up the current exe).
    bool startGameUpdate(const ReleaseAsset& asset, const std::filesystem::path& destDir,
                         std::string& error);
    // Same as startGameUpdate but for a chosen release version during an
    // install; the result is reported as "install".
    bool startInstallGame(const ReleaseAsset& asset, const std::filesystem::path& destDir,
                          std::string& error);
    // Downloads the launcher asset staged next to the running exe. The UI
    // calls spawnSelfUpdate() when this finishes.
    bool startLauncherUpdate(const ReleaseAsset& asset, const std::filesystem::path& exeDir,
                             std::string& error);

private:
    void runCheck(const std::string& apiUrl);
    void runGameDownload(const ReleaseAsset& asset, const std::filesystem::path& destDir,
                         const std::string& result);
    void runLauncherUpdate(const ReleaseAsset& asset, const std::filesystem::path& exeDir);

    std::thread worker_;
    std::shared_ptr<UpdateState> state_;
};

} // namespace launcher

#endif
