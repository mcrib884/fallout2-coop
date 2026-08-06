#include "detect.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cstdlib>
#include <unistd.h>
#endif

namespace launcher {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

static std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static bool iequals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
            return false;
    return true;
}

static bool icontains(const std::string& hay, const std::string& needle)
{
    return toLower(hay).find(toLower(needle)) != std::string::npos;
}

// utf-8 string -> fs::path
static fs::path p8(const std::string& utf8)
{
    return fs::u8path(utf8);
}

// fs::path -> utf-8 string
static std::string s8(const fs::path& p)
{
    return p.u8string();
}

static bool existsDir(const fs::path& p)
{
    std::error_code ec;
    return fs::is_directory(p, ec);
}

static bool existsFile(const fs::path& p)
{
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

// Case-insensitive file existence inside a directory.
// On Windows the plain check is already case-insensitive; on other
// platforms fall back to scanning the directory.
static bool fileExistsCI(const fs::path& dir, const std::string& name)
{
    std::error_code ec;
    if (fs::is_regular_file(dir / name, ec))
        return true;
#ifndef _WIN32
    fs::directory_iterator it(dir, ec);
    if (ec)
        return false;
    for (auto& entry : it) {
        if (entry.is_regular_file(ec) && iequals(entry.path().filename().string(), name))
            return true;
    }
#else
    (void)ec;
#endif
    return false;
}

bool isValidInstall(const fs::path& dir)
{
    if (!existsDir(dir))
        return false;
    return fileExistsCI(dir, "master.dat") && fileExistsCI(dir, "critter.dat");
}

// ---------------------------------------------------------------------------
// VDF / ACF flat parser: extracts every  "key"  "value"  pair in the file.
// Enough for libraryfolders.vdf ("path" values) and appmanifest_*.acf
// ("installdir"). Handles \\ and \" escapes.
// ---------------------------------------------------------------------------

static bool readQuotedString(const std::string& text, size_t& i, std::string& out)
{
    // text[i] must be '"'
    if (i >= text.size() || text[i] != '"')
        return false;
    ++i;
    out.clear();
    while (i < text.size()) {
        char c = text[i];
        if (c == '\\' && i + 1 < text.size()) {
            char next = text[i + 1];
            if (next == '\\' || next == '"') {
                out.push_back(next);
                i += 2;
                continue;
            }
            out.push_back(c); // keep unknown escapes literally
            ++i;
            continue;
        }
        if (c == '"') {
            ++i;
            return true;
        }
        out.push_back(c);
        ++i;
    }
    return false;
}

static std::vector<std::pair<std::string, std::string>> parseVdfPairs(const std::string& text)
{
    std::vector<std::pair<std::string, std::string>> pairs;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] != '"') {
            ++i;
            continue;
        }
        std::string key;
        if (!readQuotedString(text, i, key))
            break;
        // skip whitespace between key and value
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n'))
            ++i;
        if (i >= text.size() || text[i] != '"')
            continue; // key without a string value (a section header)
        std::string value;
        if (!readQuotedString(text, i, value))
            break;
        pairs.emplace_back(std::move(key), std::move(value));
    }
    return pairs;
}

static std::string readFileToString(const fs::path& path)
{
    std::error_code ec;
    if (!fs::is_regular_file(path, ec))
        return {};
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------------------
// Steam probing (shared between platforms)
// ---------------------------------------------------------------------------

static const char* kSteamAppIdManifest = "appmanifest_38410.acf"; // Fallout 2
static const char* kDefaultInstallDirName = "Fallout 2";

struct DetectContext {
    std::vector<FoundInstall> results;
    std::vector<std::string> seenKeys; // canonical lowercase for dedupe

    void add(const fs::path& dir, const std::string& source)
    {
        std::error_code ec;
        fs::path canon = fs::weakly_canonical(dir, ec);
        if (ec)
            canon = dir;
        if (!isValidInstall(canon))
            return;
        std::string key = toLower(s8(canon));
        if (std::find(seenKeys.begin(), seenKeys.end(), key) != seenKeys.end())
            return;
        seenKeys.push_back(key);
        results.push_back({ s8(canon), source });
    }
};

// Check one Steam library folder (a directory that contains steamapps/).
static void probeSteamLibrary(DetectContext& ctx, const fs::path& lib)
{
    if (!existsDir(lib))
        return;

    fs::path steamapps = lib / "steamapps";
    if (!existsDir(steamapps))
        steamapps = lib / "SteamApps"; // very old layouts
    if (!existsDir(steamapps))
        return;

    // Preferred: the app manifest tells us the exact install dir name.
    fs::path acf = steamapps / kSteamAppIdManifest;
    if (existsFile(acf)) {
        auto pairs = parseVdfPairs(readFileToString(acf));
        for (auto& kv : pairs) {
            if (iequals(kv.first, "installdir") && !kv.second.empty()) {
                ctx.add(steamapps / "common" / p8(kv.second), "Steam");
                break;
            }
        }
    }

    // Fallback: the default folder name.
    ctx.add(steamapps / "common" / kDefaultInstallDirName, "Steam");
}

// Check a Steam root: itself a library, plus every library listed in its
// libraryfolders.vdf (config/ is the modern location, steamapps/ the legacy one).
static void probeSteamRoot(DetectContext& ctx, const fs::path& root)
{
    if (!existsDir(root))
        return;

    probeSteamLibrary(ctx, root);

    fs::path vdfCandidates[2] = {
        root / "config" / "libraryfolders.vdf",
        root / "steamapps" / "libraryfolders.vdf",
    };
    for (const fs::path& vdfPath : vdfCandidates) {
        if (!existsFile(vdfPath))
            continue;
        auto pairs = parseVdfPairs(readFileToString(vdfPath));
        for (auto& kv : pairs) {
            if (iequals(kv.first, "path") && !kv.second.empty())
                probeSteamLibrary(ctx, p8(kv.second));
        }
    }
}

// ---------------------------------------------------------------------------
// GOG probing helpers
// ---------------------------------------------------------------------------

// Scan "<parent>/<something fallout 2-ish>" directories.
static void probeGogParent(DetectContext& ctx, const fs::path& parent)
{
    if (!existsDir(parent))
        return;
    std::error_code ec;
    fs::directory_iterator it(parent, ec);
    if (ec)
        return;
    for (auto& entry : it) {
        if (!entry.is_directory(ec))
            continue;
        std::string name = entry.path().filename().string();
        if (icontains(name, "fallout 2") || icontains(name, "fallout2"))
            ctx.add(entry.path(), "GOG");
    }
}

// ---------------------------------------------------------------------------
// platform specifics
// ---------------------------------------------------------------------------

#ifdef _WIN32

static std::string wideToUtf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), n, nullptr, nullptr);
    return out;
}

static std::wstring utf8ToWide(const std::string& s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}

// Read a REG_SZ value, trying both registry views.
static std::string regQueryString(HKEY root, const wchar_t* subKey, const wchar_t* valueName)
{
    const REGSAM views[2] = { KEY_READ | KEY_WOW64_64KEY, KEY_READ | KEY_WOW64_32KEY };
    for (REGSAM view : views) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, subKey, 0, view, &key) != ERROR_SUCCESS)
            continue;
        wchar_t buf[2048];
        DWORD size = sizeof(buf);
        DWORD type = 0;
        LSTATUS status = RegQueryValueExW(key, valueName, nullptr, &type, (LPBYTE)buf, &size);
        RegCloseKey(key);
        if (status != ERROR_SUCCESS)
            continue;
        if (type != REG_SZ && type != REG_EXPAND_SZ)
            continue;
        size_t len = size / sizeof(wchar_t);
        while (len > 0 && buf[len - 1] == L'\0')
            --len;
        return wideToUtf8(std::wstring(buf, len));
    }
    return {};
}

static void detectWindows(DetectContext& ctx)
{
    // --- Steam via registry (HKLM and HKCU, both views) ---
    std::vector<std::string> steamRoots;
    {
        struct RegLoc { HKEY root; const wchar_t* sub; };
        const RegLoc locs[] = {
            { HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam" },
            { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam" },
            { HKEY_CURRENT_USER, L"Software\\Valve\\Steam" },
        };
        for (const RegLoc& loc : locs) {
            std::string steamPath = regQueryString(loc.root, loc.sub, L"SteamPath");
            if (!steamPath.empty())
                steamRoots.push_back(steamPath); // may contain forward slashes
        }
    }
    for (const std::string& root : steamRoots)
        probeSteamRoot(ctx, p8(root));

    // --- Steam: scan every fixed drive for common library locations ---
    {
        DWORD mask = GetLogicalDrives();
        for (int d = 0; d < 26; ++d) {
            if (!(mask & (1u << d)))
                continue;
            std::wstring drive;
            drive.push_back((wchar_t)(L'A' + d));
            drive += L":\\";
            std::string driveUtf8 = wideToUtf8(drive);

            const char* steamCandidates[] = {
                "Steam",
                "SteamLibrary",
                "Program Files\\Steam",
                "Program Files (x86)\\Steam",
            };
            for (const char* cand : steamCandidates)
                probeSteamRoot(ctx, p8(driveUtf8 + cand));

            // GOG folder scans on the same pass.
            probeGogParent(ctx, p8(driveUtf8 + "GOG Games"));
            probeGogParent(ctx, p8(driveUtf8 + "Program Files\\GOG Galaxy\\Games"));
            probeGogParent(ctx, p8(driveUtf8 + "Program Files (x86)\\GOG Galaxy\\Games"));
        }
    }

    // --- GOG via registry: enumerate installed products ---
    {
        const wchar_t* gogSubs[] = {
            L"SOFTWARE\\WOW6432Node\\GOG.com\\Games",
            L"SOFTWARE\\GOG.com\\Games",
        };
        const REGSAM views[2] = { KEY_READ | KEY_WOW64_64KEY, KEY_READ | KEY_WOW64_32KEY };
        for (const wchar_t* sub : gogSubs) {
            for (REGSAM view : views) {
                HKEY key = nullptr;
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub, 0, view, &key) != ERROR_SUCCESS)
                    continue;
                for (DWORD index = 0;; ++index) {
                    wchar_t name[256];
                    DWORD nameLen = 256;
                    if (RegEnumKeyExW(key, index, name, &nameLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                        break;
                    std::wstring productSub = std::wstring(sub) + L"\\" + std::wstring(name, nameLen);
                    std::string gameName = regQueryString(HKEY_LOCAL_MACHINE, productSub.c_str(), L"gameName");
                    if (gameName.empty())
                        gameName = regQueryString(HKEY_LOCAL_MACHINE, productSub.c_str(), L"gameNameEN");
                    if (gameName.empty() || !icontains(gameName, "fallout 2"))
                        continue;
                    std::string path = regQueryString(HKEY_LOCAL_MACHINE, productSub.c_str(), L"path");
                    if (!path.empty())
                        ctx.add(p8(path), "GOG");
                }
                RegCloseKey(key);
            }
        }
    }
}

#else // Linux

static fs::path homeDir()
{
    const char* home = getenv("HOME");
    if (home && *home)
        return fs::path(home);
    return {};
}

// Resolve symlinks when possible (e.g. ~/.steam/root), tolerate failure.
static fs::path canonicalTolerant(const fs::path& p)
{
    std::error_code ec;
    fs::path c = fs::canonical(p, ec);
    if (ec)
        return p;
    return c;
}

static void detectLinux(DetectContext& ctx)
{
    fs::path home = homeDir();
    if (home.empty())
        return;

    // Steam roots: native, ~/.steam aliases, Flatpak and Snap packages.
    fs::path steamRoots[] = {
        home / ".local" / "share" / "Steam",
        home / ".steam" / "root",
        home / ".steam" / "steam",
        home / ".var" / "app" / "com.valvesoftware.Steam" / ".local" / "share" / "Steam",
        home / ".var" / "app" / "com.valvesoftware.Steam" / ".steam" / "steam",
        home / "snap" / "steam" / "common" / ".local" / "share" / "Steam",
    };
    for (const fs::path& root : steamRoots) {
        std::error_code ec;
        if (!fs::exists(root, ec))
            continue;
        probeSteamRoot(ctx, canonicalTolerant(root));
    }

    // GOG: ~/GOG Games/<game>
    probeGogParent(ctx, home / "GOG Games");
}

#endif

// ---------------------------------------------------------------------------

std::vector<FoundInstall> detectInstalls()
{
    DetectContext ctx;
#ifdef _WIN32
    detectWindows(ctx);
#else
    detectLinux(ctx);
#endif
    return ctx.results;
}

} // namespace launcher
