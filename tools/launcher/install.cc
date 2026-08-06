#include "install.h"

#include <cctype>
#include <cstring>
#include <fstream>
#include <system_error>
#include <vector>

namespace launcher {

namespace fs = std::filesystem;

const char* ceExeName()
{
#ifdef _WIN32
    return "fallout2-ce.exe";
#else
    return "fallout2-ce";
#endif
}

namespace {

struct CopyItem {
    fs::path src;
    fs::path dst;
    bool isDir = false;
};

bool icontains(const std::string& hay, const std::string& needle)
{
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) {
                              return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
                          });
    return it != hay.end();
}

// Collect every file that must be copied.
std::vector<CopyItem> buildPlan(const fs::path& src, const fs::path& dst, const fs::path& ceExe)
{
    std::vector<CopyItem> items;
    std::error_code ec;

    // 1. The co-op engine executable.
    items.push_back({ ceExe, dst / ceExe.filename() });

    // 2. Root *.dat and *.dll files.
    fs::directory_iterator rootIt(src, ec);
    if (!ec) {
        for (auto& entry : rootIt) {
            if (!entry.is_regular_file(ec))
                continue;
            std::string ext = entry.path().extension().string();
            if (icontains(ext, ".dat") || icontains(ext, ".dll"))
                items.push_back({ entry.path(), dst / entry.path().filename() });
        }
    }

    // 3. Full data/ and sound/ folders.
    const char* folders[] = { "data", "sound" };
    for (const char* folder : folders) {
        fs::path from = src / folder;
        if (!fs::is_directory(from, ec))
            continue;
        fs::recursive_directory_iterator it(from, ec);
        if (ec)
            continue;
        for (auto& entry : it) {
            fs::path rel = fs::relative(entry.path(), src, ec);
            if (ec) {
                ec.clear();
                continue;
            }
            std::error_code ec2;
            if (entry.is_directory(ec2))
                items.push_back({ entry.path(), dst / rel, true });
            else if (entry.is_regular_file(ec2))
                items.push_back({ entry.path(), dst / rel, false });
        }
    }

    return items;
}

bool copyFileChunked(const fs::path& from, const fs::path& to, InstallProgress& progress)
{
    std::ifstream in(from, std::ios::binary);
    if (!in)
        return false;
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;

    std::vector<char> buffer(1024 * 1024);
    while (in) {
        in.read(buffer.data(), (std::streamsize)buffer.size());
        std::streamsize got = in.gcount();
        if (got <= 0)
            break;
        out.write(buffer.data(), got);
        if (!out)
            return false;
        progress.bytesDone.fetch_add(got, std::memory_order_relaxed);
    }
    out.flush();
    return (bool)out;
}

} // namespace

Installer::~Installer()
{
    join();
}

bool Installer::busy() const
{
    return worker_.joinable();
}

void Installer::join()
{
    if (worker_.joinable())
        worker_.join();
}

bool Installer::start(const fs::path& sourceDir, const fs::path& destDir, const fs::path& ceExe,
                      std::string& precheckError)
{
    join();
    std::error_code ec;

    // Source must be a real Fallout 2 install.
    if (!fs::is_directory(sourceDir, ec) || !fs::is_regular_file(sourceDir / "master.dat", ec) ||
        !fs::is_regular_file(sourceDir / "critter.dat", ec)) {
        precheckError = "The source folder is not a valid Fallout 2 installation "
                        "(master.dat / critter.dat missing).";
        return false;
    }

    // The co-op engine exe must sit next to the launcher.
    if (!fs::is_regular_file(ceExe, ec)) {
        precheckError = std::string(ceExeName()) + " was not found next to the launcher.";
        return false;
    }

    // Create the destination and prove it is writable.
    fs::create_directories(destDir, ec);
    if (ec) {
        precheckError = "Destination not writable. Try running the launcher as admin.";
        return false;
    }
    fs::path writeTest = destDir / ".launcher_write_test";
    {
        std::ofstream probe(writeTest, std::ios::trunc);
        if (!probe) {
            precheckError = "Destination not writable. Try running the launcher as admin.";
            return false;
        }
        probe << "ok";
    }
    fs::remove(writeTest, ec);

    progress_ = std::make_shared<InstallProgress>();
    progress_->bytesTotal = 0;

    worker_ = std::thread([this, sourceDir, destDir, ceExe]() {
        run(sourceDir, destDir, ceExe);
    });
    return true;
}

void Installer::run(const fs::path& sourceDir, const fs::path& destDir, const fs::path& ceExe)
{
    InstallProgress& progress = *progress_;
    try {
        std::vector<CopyItem> items = buildPlan(sourceDir, destDir, ceExe);
        progress.filesTotal.store((int)items.size(), std::memory_order_relaxed);

        long long totalBytes = 0;
        std::error_code ec;
        for (const CopyItem& item : items) {
            if (item.isDir)
                continue;
            auto size = fs::file_size(item.src, ec);
            if (!ec)
                totalBytes += (long long)size;
        }
        progress.bytesTotal.store(totalBytes, std::memory_order_relaxed);

        for (const CopyItem& item : items) {
            {
                std::lock_guard<std::mutex> lock(progress.textMutex);
                progress.currentFile = item.src.filename().u8string();
            }
            if (item.isDir) {
                fs::create_directories(item.dst, ec);
                progress.filesDone.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            fs::create_directories(item.dst.parent_path(), ec);
            if (!copyFileChunked(item.src, item.dst, progress)) {
                std::lock_guard<std::mutex> lock(progress.textMutex);
                progress.error = "Failed to copy " + item.src.filename().u8string() + ".";
                progress.failed.store(true);
                progress.finished.store(true);
                return;
            }
            progress.filesDone.fetch_add(1, std::memory_order_relaxed);
        }

        // Post-verify: engine exe and both master dats must be present.
        bool ok = fs::is_regular_file(destDir / ceExe.filename(), ec) &&
                  fs::is_regular_file(destDir / "master.dat", ec) &&
                  fs::is_regular_file(destDir / "critter.dat", ec);
        if (!ok) {
            std::lock_guard<std::mutex> lock(progress.textMutex);
            progress.error = "Installation finished but key files are missing in the destination.";
            progress.failed.store(true);
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(progress.textMutex);
        progress.error = e.what();
        progress.failed.store(true);
    }
    progress.finished.store(true);
}

} // namespace launcher
