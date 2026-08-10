#ifndef FALLOUT_LAUNCHER_GAME_CONFIG_EDITOR_H
#define FALLOUT_LAUNCHER_GAME_CONFIG_EDITOR_H

#include <filesystem>
#include <string>
#include <vector>

namespace launcher {

enum class ConfigValueType {
    Text,
    Integer,
    Decimal,
    Boolean,
    Choice,
};

struct ConfigChoice {
    std::string value;
    std::string label;
};

struct GameConfigOption {
    std::string key;
    std::string label;
    std::string description;
    std::string defaultValue;
    ConfigValueType type = ConfigValueType::Text;
    bool hasRange = false;
    double minValue = 0.0;
    double maxValue = 0.0;
    bool legacy = false;
    std::vector<ConfigChoice> choices;

    // Current value. This is initialized from defaultValue and replaced by the
    // value in fallout2.cfg when one is present.
    std::string value;
};

struct GameConfigSection {
    std::string key;
    std::string title;
    std::string description;
    std::vector<GameConfigOption> options;
};

// Schema for the config sections supported by the game executable.
const std::vector<GameConfigSection>& gameConfigSchema();

class GameConfigEditor {
public:
    bool load(const std::filesystem::path& path, std::string& error);
    bool save(std::string& error);

    bool loaded() const { return loaded_; }
    bool fileExists() const { return fileExists_; }
    bool dirty() const { return dirty_; }
    void markDirty() { dirty_ = true; }
    const std::filesystem::path& path() const { return path_; }

    std::vector<GameConfigSection>& sections() { return sections_; }
    const std::vector<GameConfigSection>& sections() const { return sections_; }

private:
    struct SourceLine {
        std::string raw;
        std::string section;
        std::string key;
        std::string inlineComment;
        bool isKeyValue = false;
    };

    std::filesystem::path path_;
    std::vector<SourceLine> sourceLines_;
    std::vector<GameConfigSection> sections_;
    bool loaded_ = false;
    bool fileExists_ = false;
    bool dirty_ = false;
};

} // namespace launcher

#endif
