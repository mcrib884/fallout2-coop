#ifndef FALLOUT_LAUNCHER_DETECT_H
#define FALLOUT_LAUNCHER_DETECT_H

#include <filesystem>
#include <string>
#include <vector>

namespace launcher {

struct FoundInstall {
    std::string path;    // utf-8, canonical
    std::string source;  // "Steam", "GOG", ...
};

// True when the directory looks like a Fallout 2 data install
// (master.dat + critter.dat present, case-insensitive).
bool isValidInstall(const std::filesystem::path& dir);

// Scan the usual places (Steam registry/library folders on every disk,
// GOG registry/folders) and return every valid Fallout 2 install found.
std::vector<FoundInstall> detectInstalls();

} // namespace launcher

#endif
