#ifndef FALLOUT_LAUNCHER_CONFIG_H
#define FALLOUT_LAUNCHER_CONFIG_H

#include <filesystem>
#include <string>

namespace launcher {

// Directory containing the launcher executable itself.
std::filesystem::path exeDir();

// Persisted launcher state, stored in launcher.ini next to the executable.
struct LauncherConfig {
    std::string sourceDir;              // utf-8
    std::string installDir;             // utf-8
    bool minimizeWhilePlaying = true;

    static std::filesystem::path iniPath();
    static LauncherConfig load();
    void save() const;
};

} // namespace launcher

#endif
