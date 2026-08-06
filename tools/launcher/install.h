#ifndef FALLOUT_LAUNCHER_INSTALL_H
#define FALLOUT_LAUNCHER_INSTALL_H

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace launcher {

// Name of the co-op engine executable the launcher expects next to itself.
const char* ceExeName();

// Shared progress state between the copy worker thread and the UI.
struct InstallProgress {
    std::atomic<long long> bytesDone{ 0 };
    std::atomic<long long> bytesTotal{ 0 };
    std::atomic<int> filesDone{ 0 };
    std::atomic<int> filesTotal{ 0 };
    std::atomic<bool> finished{ false };
    std::atomic<bool> failed{ false };

    std::mutex textMutex;
    std::string currentFile;
    std::string error;
};

// Copies the co-op engine exe plus the required vanilla game files
// (root *.dat, root *.dll, data/**, sound/**) into the destination.
class Installer {
public:
    ~Installer();

    // Runs all pre-checks (source valid, engine exe present, destination
    // writable) and starts the copy thread. Returns false when a
    // pre-check failed and fills precheckError.
    bool start(const std::filesystem::path& sourceDir,
               const std::filesystem::path& destDir,
               const std::filesystem::path& ceExe,
               std::string& precheckError);

    bool busy() const;
    std::shared_ptr<InstallProgress> progress() const { return progress_; }
    void join();

private:
    void run(const std::filesystem::path& sourceDir,
             const std::filesystem::path& destDir,
             const std::filesystem::path& ceExe);

    std::thread worker_;
    std::shared_ptr<InstallProgress> progress_;
};

} // namespace launcher

#endif
