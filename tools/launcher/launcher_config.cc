#include "launcher_config.h"

#include <fstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace launcher {

namespace fs = std::filesystem;

fs::path exeDir()
{
#ifdef _WIN32
    wchar_t buf[4096];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0]))
        return fs::current_path();
    return fs::path(std::wstring(buf, n)).parent_path();
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return fs::current_path();
    buf[n] = '\0';
    return fs::path(buf).parent_path();
#endif
}

fs::path LauncherConfig::iniPath()
{
    return exeDir() / "launcher.ini";
}

LauncherConfig LauncherConfig::load()
{
    LauncherConfig cfg;
    std::ifstream in(iniPath());
    if (!in)
        return cfg;

    std::string line;
    while (std::getline(in, line)) {
        // strip \r
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        if (key == "source_dir")
            cfg.sourceDir = value;
        else if (key == "install_dir")
            cfg.installDir = value;
        else if (key == "minimize_while_playing")
            cfg.minimizeWhilePlaying = (value == "1" || value == "true" || value == "yes");
    }
    return cfg;
}

void LauncherConfig::save() const
{
    std::ofstream out(iniPath(), std::ios::trunc);
    if (!out)
        return;
    out << "source_dir=" << sourceDir << "\n";
    out << "install_dir=" << installDir << "\n";
    out << "minimize_while_playing=" << (minimizeWhilePlaying ? "1" : "0") << "\n";
}

} // namespace launcher
