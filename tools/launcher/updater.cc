// Update machinery for the Fallout 2 Co-op launcher.
//
// Fetches the latest GitHub release of the co-op repository, downloads a
// component (game exe or launcher exe) and installs it. The game exe is
// swapped in the install destination; the launcher replaces itself through a
// staged batch that waits for the process to exit.

#include "updater.h"

#include "install.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wininet.h>
#include <bcrypt.h>
#endif

namespace launcher {

namespace fs = std::filesystem;

const char* launcherExeName()
{
#ifdef _WIN32
    return "fallout2coop_launcher.exe";
#else
    return "fallout2coop_launcher";
#endif
}

std::string s8(const fs::path& p) { return p.u8string(); }

// Staged name for a self-update download: "fallout2coop_launcher.new.exe".
std::string launcherStagedExeName()
{
    std::string name = launcherExeName();
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0)
        name.insert(name.size() - 4, ".new");
    else
        name += ".new";
    return name;
}

// Section markers in the release description. Whichever marker has content
// under it says which component that release updates.
static const char* kCeMarker = "{Fallout 2 Coop}";
static const char* kLauncherMarker = "{Launcher}";

// ---------------------------------------------------------------------------
// minimal JSON parser (objects/arrays/strings/numbers/bools/null)
// ---------------------------------------------------------------------------

namespace {

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string str;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;

    const JsonValue* find(const std::string& key) const
    {
        for (const auto& kv : object)
            if (kv.first == key)
                return &kv.second;
        return nullptr;
    }
};

void jsonAppendUtf8(std::string& out, unsigned int cp)
{
    if (cp < 0x80) {
        out.push_back((char)cp);
    } else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

struct JsonParser {
    const std::string& s;
    size_t i = 0;

    void skipWs()
    {
        while (i + 2 < s.size()
               && (unsigned char)s[i] == 0xEF
               && (unsigned char)s[i + 1] == 0xBB
               && (unsigned char)s[i + 2] == 0xBF) {
            i += 3;
        }
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
            ++i;
    }

    bool parseHex4(unsigned int& out)
    {
        if (i + 4 > s.size())
            return false;
        out = 0;
        for (int k = 0; k < 4; ++k) {
            char h = s[i + k];
            out <<= 4;
            if (h >= '0' && h <= '9')
                out |= (unsigned int)(h - '0');
            else if (h >= 'a' && h <= 'f')
                out |= (unsigned int)(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F')
                out |= (unsigned int)(h - 'A' + 10);
            else
                return false;
        }
        i += 4;
        return true;
    }

    bool parseString(std::string& out)
    {
        if (i >= s.size() || s[i] != '"')
            return false;
        ++i;
        out.clear();
        while (i < s.size()) {
            char c = s[i];
            if (c == '"') {
                ++i;
                return true;
            }
            if (c != '\\') {
                out.push_back(c);
                ++i;
                continue;
            }
            ++i;
            if (i >= s.size())
                return false;
            char e = s[i++];
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                unsigned int cp = 0;
                if (!parseHex4(cp))
                    return false;
                // UTF-16 surrogate pair
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i] == '\\' &&
                    s[i + 1] == 'u') {
                    i += 2;
                    unsigned int lo = 0;
                    if (!parseHex4(lo))
                        return false;
                    if (lo >= 0xDC00 && lo <= 0xDFFF)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                jsonAppendUtf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false; // unterminated
    }

    bool parseNumber(JsonValue& v)
    {
        size_t start = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+'))
            ++i;
        bool digits = false;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            ++i;
            digits = true;
        }
        if (i < s.size() && s[i] == '.') {
            ++i;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
                ++i;
                digits = true;
            }
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-'))
                ++i;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9')
                ++i;
        }
        if (!digits)
            return false;
        v.type = JsonValue::Type::Number;
        v.number = std::strtod(s.c_str() + start, nullptr);
        return true;
    }

    bool parseValue(JsonValue& v)
    {
        skipWs();
        if (i >= s.size())
            return false;
        char c = s[i];
        if (c == '{') {
            ++i;
            v.type = JsonValue::Type::Object;
            skipWs();
            if (i < s.size() && s[i] == '}') {
                ++i;
                return true;
            }
            for (;;) {
                skipWs();
                std::string key;
                if (!parseString(key))
                    return false;
                skipWs();
                if (i >= s.size() || s[i] != ':')
                    return false;
                ++i;
                JsonValue val;
                if (!parseValue(val))
                    return false;
                v.object.emplace_back(std::move(key), std::move(val));
                skipWs();
                if (i >= s.size())
                    return false;
                if (s[i] == ',') {
                    ++i;
                    continue;
                }
                if (s[i] == '}') {
                    ++i;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            ++i;
            v.type = JsonValue::Type::Array;
            skipWs();
            if (i < s.size() && s[i] == ']') {
                ++i;
                return true;
            }
            for (;;) {
                JsonValue val;
                if (!parseValue(val))
                    return false;
                v.array.push_back(std::move(val));
                skipWs();
                if (i >= s.size())
                    return false;
                if (s[i] == ',') {
                    ++i;
                    continue;
                }
                if (s[i] == ']') {
                    ++i;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            v.type = JsonValue::Type::String;
            return parseString(v.str);
        }
        if (c == 't') {
            if (s.compare(i, 4, "true") == 0) {
                i += 4;
                v.type = JsonValue::Type::Bool;
                v.boolean = true;
                return true;
            }
            return false;
        }
        if (c == 'f') {
            if (s.compare(i, 5, "false") == 0) {
                i += 5;
                v.type = JsonValue::Type::Bool;
                v.boolean = false;
                return true;
            }
            return false;
        }
        if (c == 'n') {
            if (s.compare(i, 4, "null") == 0) {
                i += 4;
                v.type = JsonValue::Type::Null;
                return true;
            }
            return false;
        }
        return parseNumber(v);
    }
};

bool parseJson(const std::string& text, JsonValue& root, std::string& error)
{
    JsonParser p{ text };
    if (!p.parseValue(root)) {
        error = "Invalid JSON from the release server.";
        return false;
    }
    p.skipWs();
    if (p.i != text.size()) {
        error = "Trailing data in the release JSON.";
        return false;
    }
    return true;
}

} // namespace

const ReleaseAsset* ReleaseEntry::findAsset(const std::string& name) const
{
    for (const ReleaseAsset& a : assets)
        if (a.name == name)
            return &a;
    return nullptr;
}

const ReleaseEntry* ReleaseFeed::ceLatest() const
{
    return ceLatestIdx_ >= 0 ? &releases[(size_t)ceLatestIdx_] : nullptr;
}

const ReleaseEntry* ReleaseFeed::launcherLatest() const
{
    return launcherLatestIdx_ >= 0 ? &releases[(size_t)launcherLatestIdx_] : nullptr;
}

// ---------------------------------------------------------------------------
// release description sections
// ---------------------------------------------------------------------------

namespace {

bool iequals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
            return false;
    return true;
}

std::string trimWs(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b]))
        ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1]))
        --e;
    return s.substr(b, e - b);
}

// Splits on \n and strips a trailing \r from each line.
std::vector<std::string> splitLines(const std::string& s)
{
    std::vector<std::string> lines;
    size_t p = 0;
    while (p < s.size()) {
        size_t nl = s.find('\n', p);
        std::string line = s.substr(p, nl == std::string::npos ? std::string::npos : nl - p);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
        if (nl == std::string::npos)
            break;
        p = nl + 1;
    }
    if (s.empty())
        lines.push_back("");
    return lines;
}

// Content under `marker` in the description, up to the next marker or the end
// of the body. Empty when the marker is absent or has nothing under it.
std::string parseSection(const std::string& body, const std::string& marker,
                         const std::string& otherMarker)
{
    std::vector<std::string> lines = splitLines(body);
    std::string out;
    bool inSection = false;
    for (const std::string& line : lines) {
        std::string t = trimWs(line);
        if (iequals(t, marker)) {
            inSection = true;
            continue;
        }
        if (inSection && (iequals(t, otherMarker) || iequals(t, marker))) {
            inSection = false;
            continue;
        }
        if (inSection)
            out += line + "\n";
    }
    return trimWs(out);
}

} // namespace

// ---------------------------------------------------------------------------
// HTTP (WinINet)
// ---------------------------------------------------------------------------

#ifdef _WIN32
namespace {

std::wstring wstr(const std::string& s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}

std::string astr(const std::wstring& s)
{
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

HINTERNET openHttpHandle()
{
    HINTERNET h = InternetOpenW(L"fallout2coop-launcher/1.0", INTERNET_OPEN_TYPE_PRECONFIG,
                                nullptr, nullptr, 0);
    if (!h)
        return nullptr;
    DWORD v = 15000; // connect/send
    InternetSetOptionW(h, INTERNET_OPTION_CONNECT_TIMEOUT, &v, sizeof(v));
    InternetSetOptionW(h, INTERNET_OPTION_SEND_TIMEOUT, &v, sizeof(v));
    v = 30000; // receive
    InternetSetOptionW(h, INTERNET_OPTION_RECEIVE_TIMEOUT, &v, sizeof(v));
    return h;
}

HINTERNET openUrlHandle(HINTERNET http, const std::string& url)
{
    static const wchar_t kHeaders[] =
        L"User-Agent: fallout2coop-launcher/1.0\r\n"
        L"Accept: application/vnd.github+json\r\n";
    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI;
    if (url.rfind("https://", 0) == 0 || url.rfind("HTTPS://", 0) == 0)
        flags |= INTERNET_FLAG_SECURE;
    return InternetOpenUrlW(http, wstr(url).c_str(), kHeaders, -1, flags, 0);
}

DWORD httpStatus(HINTERNET conn)
{
    DWORD status = 0, len = sizeof(status);
    if (!HttpQueryInfoW(conn, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &len,
                        nullptr))
        return 0;
    return status;
}

} // namespace
#endif

// Atomic-ish file replacement: moves `from` over `to`, failing when `to` is
// locked (for example a running executable). Copy is allowed so temp-folder
// staging works across volumes.
bool replaceFile(const fs::path& from, const fs::path& to)
{
#ifdef _WIN32
    return MoveFileExW(wstr(s8(from)).c_str(), wstr(s8(to)).c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0;
#else
    std::error_code ec;
    fs::rename(from, to, ec);
    return !ec;
#endif
}

bool fetchReleaseFeedUrl(const std::string& apiUrl, ReleaseFeed& out, std::string& error)
{
    std::string body;
#ifdef _WIN32
    HINTERNET http = openHttpHandle();
    if (!http) {
        error = "Could not open the internet connection.";
        return false;
    }
    HINTERNET conn = openUrlHandle(http, apiUrl);
    if (!conn) {
        DWORD code = GetLastError();
        error = "Could not reach the update server (error " + std::to_string(code) + ").";
        InternetCloseHandle(http);
        return false;
    }
    DWORD status = httpStatus(conn);
    if (status != 200) {
        error = "Update server answered HTTP " + std::to_string(status) +
                (status == 404 ? " (no releases published yet)." : ".");
        InternetCloseHandle(conn);
        InternetCloseHandle(http);
        return false;
    }
    body.clear();
    char buf[16384];
    DWORD read = 0;
    while (InternetReadFile(conn, buf, sizeof(buf), &read) && read > 0) {
        body.append(buf, read);
        read = 0;
    }
    InternetCloseHandle(conn);
    InternetCloseHandle(http);
#else
    (void)apiUrl;
    error = "Online updates are only supported on Windows.";
    return false;
#endif

    JsonValue root;
    if (!parseJson(body, root, error))
        return false;
    if (root.type != JsonValue::Type::Array) {
        error = "The release server did not return a release list.";
        return false;
    }

    for (const JsonValue& r : root.array) {
        if (r.type != JsonValue::Type::Object)
            continue;
        ReleaseEntry entry;
        const JsonValue* tag = r.find("tag_name");
        if (!tag || tag->type != JsonValue::Type::String)
            continue;
        entry.tag = tag->str;
        const JsonValue* bodyV = r.find("body");
        if (bodyV && bodyV->type == JsonValue::Type::String)
            entry.body = bodyV->str;
        entry.ceChangelog = parseSection(entry.body, kCeMarker, kLauncherMarker);
        entry.launcherChangelog = parseSection(entry.body, kLauncherMarker, kCeMarker);

        const JsonValue* assets = r.find("assets");
        if (assets && assets->type == JsonValue::Type::Array) {
            for (const JsonValue& a : assets->array) {
                if (a.type != JsonValue::Type::Object)
                    continue;
                ReleaseAsset asset;
                const JsonValue* name = a.find("name");
                if (!name || name->type != JsonValue::Type::String)
                    continue;
                asset.name = name->str;
                const JsonValue* url = a.find("browser_download_url");
                if (url && url->type == JsonValue::Type::String)
                    asset.url = url->str;
                const JsonValue* digest = a.find("digest");
                if (digest && digest->type == JsonValue::Type::String)
                    asset.digest = digest->str;
                const JsonValue* size = a.find("size");
                if (size && size->type == JsonValue::Type::Number)
                    asset.size = (long long)size->number;
                entry.assets.push_back(std::move(asset));
            }
        }
        out.releases.push_back(std::move(entry));
    }

    // Newest first (API contract): the newest release with content under a
    // component's marker is that component's latest version.
    for (size_t i = 0; i < out.releases.size(); ++i) {
        const ReleaseEntry& e = out.releases[i];
        if (out.ceLatestTag.empty() && e.updatesCe()) {
            out.ceLatestTag = e.tag;
            out.ceLatestIdx_ = (int)i;
        }
        if (out.launcherLatestTag.empty() && e.updatesLauncher()) {
            out.launcherLatestTag = e.tag;
            out.launcherLatestIdx_ = (int)i;
        }
    }
    return true;
}

bool fetchReleaseFeed(const std::string& repo, ReleaseFeed& out, std::string& error)
{
    return fetchReleaseFeedUrl("https://api.github.com/repos/" + repo + "/releases?per_page=100",
                               out, error);
}

bool httpGet(const std::string& url, std::string& body, std::string& error)
{
#ifdef _WIN32
    HINTERNET http = openHttpHandle();
    if (!http) {
        error = "Could not open the internet connection.";
        return false;
    }
    HINTERNET conn = openUrlHandle(http, url);
    if (!conn) {
        DWORD code = GetLastError();
        error = "Could not reach the update server (error " + std::to_string(code) + ").";
        InternetCloseHandle(http);
        return false;
    }
    DWORD status = httpStatus(conn);
    if (status != 200) {
        error = "Update server answered HTTP " + std::to_string(status) + ".";
        InternetCloseHandle(conn);
        InternetCloseHandle(http);
        return false;
    }
    body.clear();
    char buf[16384];
    DWORD read = 0;
    while (InternetReadFile(conn, buf, sizeof(buf), &read) && read > 0) {
        body.append(buf, read);
        read = 0;
    }
    InternetCloseHandle(conn);
    InternetCloseHandle(http);
    return true;
#else
    (void)url;
    (void)body;
    error = "Online updates are only supported on Windows.";
    return false;
#endif
}

bool fetchReleaseFeedWithVersions(const std::string& apiUrl, ReleaseFeed& out,
                                  std::string& error)
{
    if (!fetchReleaseFeedUrl(apiUrl, out, error))
        return false;
    // Assign each release its version from its version.json asset. A missing
    // or unreadable record leaves the release at version "" (listed as "?");
    // only a failed list fetch is fatal.
    for (ReleaseEntry& e : out.releases) {
        const ReleaseAsset* record = e.findAsset("version.json");
        if (!record || record->url.empty())
            continue;
        std::string body;
        std::string err;
        if (!httpGet(record->url, body, err))
            continue;
        JsonValue root;
        if (!parseJson(body, root, err))
            continue;
        const JsonValue* ce = root.find("ce");
        const JsonValue* launcher = root.find("launcher");
        std::string version =
            (ce && ce->type == JsonValue::Type::String) ? ce->str : std::string();
        if (version.empty() && launcher && launcher->type == JsonValue::Type::String)
            version = launcher->str;
        e.version = version;
    }
    return true;
}

bool downloadFile(const std::string& url, const fs::path& dest,
                  std::atomic<long long>& bytesDone, std::atomic<long long>& bytesTotal,
                  std::string& error)
{
#ifdef _WIN32
    HINTERNET http = openHttpHandle();
    if (!http) {
        error = "Could not open the internet connection.";
        return false;
    }
    HINTERNET conn = openUrlHandle(http, url);
    if (!conn) {
        DWORD code = GetLastError();
        error = "Could not reach the download server (error " + std::to_string(code) + ").";
        InternetCloseHandle(http);
        return false;
    }
    DWORD status = httpStatus(conn);
    if (status != 200) {
        error = "Download server answered HTTP " + std::to_string(status) + ".";
        InternetCloseHandle(conn);
        InternetCloseHandle(http);
        return false;
    }
    bytesTotal.store(0);
    DWORD len = 0, lenSz = sizeof(len);
    if (HttpQueryInfoW(conn, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &len, &lenSz,
                       nullptr))
        bytesTotal.store((long long)len);

    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "Could not write " + dest.filename().u8string() + " (folder not writable?).";
        InternetCloseHandle(conn);
        InternetCloseHandle(http);
        return false;
    }
    bytesDone.store(0);
    char buf[65536];
    DWORD read = 0;
    while (InternetReadFile(conn, buf, sizeof(buf), &read) && read > 0) {
        out.write(buf, read);
        if (!out) {
            error = "Disk write failed while downloading.";
            InternetCloseHandle(conn);
            InternetCloseHandle(http);
            return false;
        }
        bytesDone.fetch_add(read, std::memory_order_relaxed);
        read = 0;
    }
    out.flush();
    bool ok = (bool)out;
    InternetCloseHandle(conn);
    InternetCloseHandle(http);
    if (!ok)
        error = "Disk write failed while downloading.";
    return ok;
#else
    (void)url;
    (void)dest;
    (void)bytesDone;
    (void)bytesTotal;
    error = "Online updates are only supported on Windows.";
    return false;
#endif
}

// ---------------------------------------------------------------------------
// SHA-256 (BCrypt)
// ---------------------------------------------------------------------------

std::string sha256Hex(const fs::path& file)
{
#ifdef _WIN32
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return {};
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    char buf[65536];
    while (in) {
        in.read(buf, sizeof(buf));
        std::streamsize got = in.gcount();
        if (got > 0)
            BCryptHashData(hash, (PUCHAR)buf, (ULONG)got, 0);
    }
    unsigned char digest[32];
    NTSTATUS st = BCryptFinishHash(hash, digest, sizeof(digest), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (st != 0)
        return {};
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (unsigned char b : digest) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
#else
    (void)file;
    return {};
#endif
}

bool verifyDigest(const fs::path& file, const std::string& digest)
{
    // GitHub reports asset digests as "sha256:<hex>"; anything else is not
    // verifiable and passes.
    const char* prefix = "sha256:";
    if (digest.rfind(prefix, 0) != 0)
        return true;
    std::string want = digest.substr(7);
    std::string got = sha256Hex(file);
    if (got.empty() || want.size() != got.size())
        return false;
    std::string wantLower = want;
    for (char& c : wantLower)
        c = (char)std::tolower((unsigned char)c);
    return wantLower == got;
}

// ---------------------------------------------------------------------------
// version.json (per release) and exe file versions
// ---------------------------------------------------------------------------

std::string exeFileVersion(const fs::path& path)
{
#ifdef _WIN32
    std::wstring wpath = wstr(s8(path));
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(wpath.c_str(), &handle);
    if (size == 0)
        return {};
    std::vector<unsigned char> data(size);
    if (!GetFileVersionInfoW(wpath.c_str(), handle, size, data.data()))
        return {};
    wchar_t* value = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\StringFileInfo\\040904b0\\FileVersion",
                        (void**)&value, &len))
        return {};
    if (!value || len == 0)
        return {};
    size_t n = len;
    while (n > 0 && value[n - 1] == L'\0')
        --n;
    return astr(std::wstring(value, n));
#else
    (void)path;
    return {};
#endif
}

// ---------------------------------------------------------------------------
// version comparison
// ---------------------------------------------------------------------------

int versionCompare(const std::string& a, const std::string& b)
{
    auto parts = [](const std::string& raw, std::vector<long long>& nums, std::string& suffix) {
        std::string v = raw;
        if (!v.empty() && (v[0] == 'v' || v[0] == 'V'))
            v.erase(0, 1);
        size_t dash = v.find('-');
        if (dash != std::string::npos) {
            suffix = v.substr(dash + 1);
            v = v.substr(0, dash);
        }
        size_t p = 0;
        while (p < v.size()) {
            size_t dot = v.find('.', p);
            std::string tok = v.substr(p, dot == std::string::npos ? std::string::npos : dot - p);
            nums.push_back(std::atoll(tok.c_str()));
            if (dot == std::string::npos)
                break;
            p = dot + 1;
        }
    };

    std::vector<long long> an, bn;
    std::string as, bs;
    parts(a, an, as);
    parts(b, bn, bs);
    size_t n = std::max(an.size(), bn.size());
    for (size_t k = 0; k < n; ++k) {
        long long x = k < an.size() ? an[k] : 0;
        long long y = k < bn.size() ? bn[k] : 0;
        if (x != y)
            return x < y ? -1 : 1;
    }
    // Same numeric version: a release suffix sorts older than a plain release.
    if (as.empty() != bs.empty())
        return as.empty() ? 1 : -1;
    if (as != bs)
        return as < bs ? -1 : 1;
    return 0;
}

// ---------------------------------------------------------------------------
// self-update staging
// ---------------------------------------------------------------------------

bool spawnSelfUpdate(const fs::path& exeDirPath, std::string& error)
{
#ifdef _WIN32
    const std::string exeName = launcherExeName();
    fs::path oldExe = exeDirPath / exeName;
    fs::path newExe = exeDirPath / launcherStagedExeName();

    std::error_code ec;
    if (!fs::is_regular_file(newExe, ec)) {
        error = "The staged launcher update file is missing.";
        return false;
    }

    // Updater batch in %TEMP%; it waits for this process to exit, swaps the
    // binaries and relaunches. The launcher is replaced only after it has
    // fully exited, so the running exe is never locked.
    wchar_t tmpBuf[MAX_PATH + 1] = {};
    if (GetTempPathW(MAX_PATH, tmpBuf) == 0) {
        error = "Could not resolve the temp folder.";
        return false;
    }
    fs::path batPath = fs::path(std::wstring(tmpBuf)) / "fallout2coop_updater.bat";

    std::string bat;
    bat += "@echo off\r\n";
    bat += "timeout /t 2 /nobreak >nul\r\n";
    bat += "del /f /q \"" + s8(oldExe) + "\" 2>nul\r\n";
    bat += "ren \"" + s8(newExe) + "\" \"" + exeName + "\"\r\n";
    bat += "start \"\" \"" + s8(exeDirPath / exeName) + "\"\r\n";
    bat += "del \"%~f0\"\r\n";
    {
        std::ofstream out(batPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "Could not write the updater batch.";
            return false;
        }
        out << bat;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // CreateProcessW cannot resolve a bare "cmd.exe" reliably (it does not
    // always search System32), so use the system directory explicitly.
    wchar_t sysDir[MAX_PATH + 1] = {};
    UINT sysLen = GetSystemDirectoryW(sysDir, MAX_PATH);
    std::wstring cmdPath = (sysLen > 0 && sysLen < MAX_PATH)
        ? (std::wstring(sysDir) + L"\\cmd.exe")
        : L"cmd.exe";
    std::wstring cmdline = L"cmd.exe /c \"\"" + wstr(s8(batPath)) + L"\"\"";
    if (!CreateProcessW(cmdPath.c_str(), cmdline.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        error = "Could not start the updater batch (error " + std::to_string(GetLastError()) +
                ").";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    (void)dir;
    error = "Self-update is only supported on Windows.";
    return false;
#endif
}

// ---------------------------------------------------------------------------
// worker-thread jobs
// ---------------------------------------------------------------------------

Updater::~Updater()
{
    join();
}

bool Updater::busy() const
{
    return worker_.joinable();
}

void Updater::join()
{
    if (worker_.joinable())
        worker_.join();
}

bool Updater::startCheck(const std::string& apiUrl, std::string& error)
{
    if (busy()) {
        error = "Another update job is still running.";
        return false;
    }
    state_ = std::make_shared<UpdateState>();
    state_->phase.store(UpdatePhase::Checking);
    worker_ = std::thread([this, apiUrl]() { runCheck(apiUrl); });
    return true;
}

void Updater::runCheck(const std::string& apiUrl)
{
    UpdateState& st = *state_;
    try {
        ReleaseFeed feed;
        std::string err;
        if (!fetchReleaseFeedWithVersions(apiUrl, feed, err)) {
            std::lock_guard<std::mutex> lock(st.mutex);
            st.error = err;
            st.phase.store(UpdatePhase::Failed);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(st.mutex);
            st.feed = std::move(feed);
            st.result = "check";
        }
        st.phase.store(UpdatePhase::Done);
    } catch (...) {
        std::lock_guard<std::mutex> lock(st.mutex);
        st.error = "Unexpected error while checking for updates.";
        st.phase.store(UpdatePhase::Failed);
    }
}

bool Updater::startGameUpdate(const ReleaseAsset& asset, const fs::path& destDir,
                              std::string& error)
{
    if (busy()) {
        error = "Another update job is still running.";
        return false;
    }
    state_ = std::make_shared<UpdateState>();
    state_->phase.store(UpdatePhase::Downloading);
    worker_ = std::thread([this, asset, destDir]() { runGameDownload(asset, destDir, "game"); });
    return true;
}

bool Updater::startInstallGame(const ReleaseAsset& asset, const fs::path& destDir,
                               std::string& error)
{
    if (busy()) {
        error = "Another update job is still running.";
        return false;
    }
    state_ = std::make_shared<UpdateState>();
    state_->phase.store(UpdatePhase::Downloading);
    worker_ = std::thread([this, asset, destDir]() { runGameDownload(asset, destDir, "install"); });
    return true;
}

void Updater::runGameDownload(const ReleaseAsset& asset, const fs::path& destDir,
                              const std::string& result)
{
    UpdateState& st = *state_;
    const std::string exeName = ceExeName();
    // Download to the temp folder first; the exe only touches the target
    // folder when it is time to install.
    fs::path tempDir = fs::temp_directory_path() / "fallout2coop_update";
    fs::path newFile = tempDir / exeName;
    const fs::path target = destDir / exeName;
    const fs::path backup = destDir / (exeName + ".bak");

    auto fail = [&st](const std::string& msg) {
        std::lock_guard<std::mutex> lock(st.mutex);
        st.error = msg;
        st.phase.store(UpdatePhase::Failed);
    };

    try {
        std::error_code ec;
        fs::create_directories(tempDir, ec);
        fs::remove(newFile, ec);
        std::string err;
        if (!downloadFile(asset.url, newFile, st.bytesDone, st.bytesTotal, err)) {
            fs::remove(newFile, ec);
            fail(err);
            return;
        }
        if (!verifyDigest(newFile, asset.digest)) {
            fs::remove(newFile, ec);
            fail("The downloaded game exe failed the SHA-256 check. The update was cancelled.");
            return;
        }
        // Back up the current exe (if present) so a failed swap can be undone.
        if (fs::exists(target, ec)) {
            fs::remove(backup, ec);
            if (!replaceFile(target, backup)) {
                fs::remove(newFile, ec);
                fail("Could not back up " + exeName + ". Is the game still running?");
                return;
            }
        }
        if (!replaceFile(newFile, target)) {
            if (fs::exists(backup, ec))
                replaceFile(backup, target);
            fail("Could not install the new " + exeName + ".");
            return;
        }
        fs::remove(backup, ec);
        fs::remove(newFile, ec);
        {
            std::lock_guard<std::mutex> lock(st.mutex);
            st.result = result;
        }
        st.phase.store(UpdatePhase::Done);
    } catch (...) {
        fail("Unexpected error during the game update.");
    }
}

bool Updater::startLauncherUpdate(const ReleaseAsset& asset, const fs::path& exeDirPath,
                                  std::string& error)
{
    if (busy()) {
        error = "Another update job is still running.";
        return false;
    }
    state_ = std::make_shared<UpdateState>();
    state_->phase.store(UpdatePhase::Downloading);
    worker_ = std::thread([this, asset, exeDirPath]() { runLauncherUpdate(asset, exeDirPath); });
    return true;
}

void Updater::runLauncherUpdate(const ReleaseAsset& asset, const fs::path& exeDirPath)
{
    UpdateState& st = *state_;
    const fs::path newExe = exeDirPath / launcherStagedExeName();

    auto fail = [&st](const std::string& msg) {
        std::lock_guard<std::mutex> lock(st.mutex);
        st.error = msg;
        st.phase.store(UpdatePhase::Failed);
    };

    try {
        std::error_code ec;
        fs::remove(newExe, ec);
        std::string err;
        if (!downloadFile(asset.url, newExe, st.bytesDone, st.bytesTotal, err)) {
            fs::remove(newExe, ec);
            fail(err);
            return;
        }
        if (!verifyDigest(newExe, asset.digest)) {
            fs::remove(newExe, ec);
            fail("The downloaded launcher failed the SHA-256 check. The update was cancelled.");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(st.mutex);
            st.result = "launcher";
        }
        st.phase.store(UpdatePhase::Done);
    } catch (...) {
        fail("Unexpected error during the launcher update.");
    }
}

} // namespace launcher
