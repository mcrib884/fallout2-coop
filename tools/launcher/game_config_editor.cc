#include "game_config_editor.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace launcher {

namespace fs = std::filesystem;

namespace {

std::string trim(const std::string& value)
{
    size_t first = 0;
    while (first < value.size() && std::isspace((unsigned char)value[first]) != 0)
        ++first;

    size_t last = value.size();
    while (last > first && std::isspace((unsigned char)value[last - 1]) != 0)
        --last;

    return value.substr(first, last - first);
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    return value;
}

std::string mapKey(const std::string& section, const std::string& key)
{
    return lower(section) + "\n" + lower(key);
}

GameConfigOption textOption(const char* key, const char* label, const char* description,
                            const char* defaultValue, bool legacy = false)
{
    GameConfigOption option;
    option.key = key;
    option.label = label;
    option.description = description;
    option.defaultValue = defaultValue;
    option.type = ConfigValueType::Text;
    option.legacy = legacy;
    return option;
}

GameConfigOption integerOption(const char* key, const char* label, const char* description,
                               const char* defaultValue, bool hasRange = false,
                               double minValue = 0.0, double maxValue = 0.0, bool legacy = false)
{
    GameConfigOption option;
    option.key = key;
    option.label = label;
    option.description = description;
    option.defaultValue = defaultValue;
    option.type = ConfigValueType::Integer;
    option.hasRange = hasRange;
    option.minValue = minValue;
    option.maxValue = maxValue;
    option.legacy = legacy;
    return option;
}

GameConfigOption decimalOption(const char* key, const char* label, const char* description,
                               const char* defaultValue, bool hasRange = false,
                               double minValue = 0.0, double maxValue = 0.0)
{
    GameConfigOption option;
    option.key = key;
    option.label = label;
    option.description = description;
    option.defaultValue = defaultValue;
    option.type = ConfigValueType::Decimal;
    option.hasRange = hasRange;
    option.minValue = minValue;
    option.maxValue = maxValue;
    return option;
}

GameConfigOption booleanOption(const char* key, const char* label, const char* description,
                               bool defaultValue)
{
    GameConfigOption option;
    option.key = key;
    option.label = label;
    option.description = description;
    option.defaultValue = defaultValue ? "1" : "0";
    option.type = ConfigValueType::Boolean;
    return option;
}

GameConfigOption choiceOption(const char* key, const char* label, const char* description,
                              const char* defaultValue,
                              std::initializer_list<ConfigChoice> choices)
{
    GameConfigOption option;
    option.key = key;
    option.label = label;
    option.description = description;
    option.defaultValue = defaultValue;
    option.type = ConfigValueType::Choice;
    option.choices = choices;
    return option;
}

GameConfigSection section(const char* key, const char* title, const char* description,
                          std::initializer_list<GameConfigOption> options)
{
    GameConfigSection result;
    result.key = key;
    result.title = title;
    result.description = description;
    result.options = options;
    return result;
}

const GameConfigOption* findOption(const std::vector<GameConfigSection>& sections,
                                   const std::string& sectionName, const std::string& key)
{
    for (const GameConfigSection& section : sections) {
        if (lower(section.key) != lower(sectionName))
            continue;
        for (const GameConfigOption& option : section.options) {
            if (lower(option.key) == lower(key))
                return &option;
        }
    }
    return nullptr;
}

GameConfigOption* findOption(std::vector<GameConfigSection>& sections,
                             const std::string& sectionName, const std::string& key)
{
    for (GameConfigSection& section : sections) {
        if (lower(section.key) != lower(sectionName))
            continue;
        for (GameConfigOption& option : section.options) {
            if (lower(option.key) == lower(key))
                return &option;
        }
    }
    return nullptr;
}

bool parseInteger(const std::string& text, long long& result)
{
    const char* begin = text.c_str();
    char* end = nullptr;
    result = std::strtoll(begin, &end, 10);
    if (end == begin)
        return false;
    while (*end != '\0' && std::isspace((unsigned char)*end) != 0)
        ++end;
    return *end == '\0';
}

bool parseDecimal(const std::string& text, double& result)
{
    const char* begin = text.c_str();
    char* end = nullptr;
    result = std::strtod(begin, &end);
    if (end == begin)
        return false;
    while (*end != '\0' && std::isspace((unsigned char)*end) != 0)
        ++end;
    return *end == '\0';
}

bool parseBoolean(const std::string& text, bool& result)
{
    std::string value = lower(trim(text));
    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        result = true;
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
        result = false;
        return true;
    }
    return false;
}

bool validateOption(const GameConfigSection& section, const GameConfigOption& option,
                    std::string& error)
{
    if (option.type == ConfigValueType::Text)
        return true;

    if (option.type == ConfigValueType::Boolean) {
        bool value = false;
        if (!parseBoolean(option.value, value)) {
            error = section.title + "." + option.key + " must be on or off.";
            return false;
        }
        return true;
    }

    if (option.type == ConfigValueType::Choice) {
        for (const ConfigChoice& choice : option.choices) {
            if (choice.value == option.value)
                return true;
        }
        error = section.title + "." + option.key + " has an unsupported value.";
        return false;
    }

    double numericValue = 0.0;
    if (option.type == ConfigValueType::Integer) {
        long long integerValue = 0;
        if (!parseInteger(option.value, integerValue)) {
            error = section.title + "." + option.key + " must be a whole number.";
            return false;
        }
        numericValue = (double)integerValue;
    } else if (!parseDecimal(option.value, numericValue)) {
        error = section.title + "." + option.key + " must be a number.";
        return false;
    }

    if (option.hasRange && (numericValue < option.minValue || numericValue > option.maxValue)) {
        std::ostringstream range;
        range << section.title << "." << option.key << " must be between " << option.minValue
              << " and " << option.maxValue << ".";
        error = range.str();
        return false;
    }
    return true;
}

std::string normalizedValue(const GameConfigOption& option)
{
    if (option.type != ConfigValueType::Boolean)
        return option.value;
    bool value = false;
    if (!parseBoolean(option.value, value))
        return option.value;
    return value ? "1" : "0";
}

} // namespace

const std::vector<GameConfigSection>& gameConfigSchema()
{
    static const std::vector<GameConfigSection> schema = {
        section("debug", "Debug", "Diagnostics and debug console behavior.", {
            textOption("mode", "Debug mode", "Debug mode used by the engine.", "environment"),
            booleanOption("output_map_data_info", "Map data info", "Write map data details to debug output.", false),
            booleanOption("show_load_info", "Load info", "Show map and resource load details.", false),
            booleanOption("show_script_messages", "Script messages", "Print script messages to debug output.", false),
            booleanOption("show_tile_num", "Tile numbers", "Show tile numbers while debugging maps.", false),
            textOption("console_output_path", "Console output", "File that receives console and debug output.", "coop_debug.log"),
            integerOption("window_height", "Console height", "Debug console height in pixels.", "192", true, 100, 1080),
            integerOption("window_width", "Console width", "Debug console width in pixels.", "300", true, 200, 1920),
        }),
        section("preferences", "Preferences", "Gameplay preferences saved by the engine.", {
            decimalOption("brightness", "Brightness", "Display brightness multiplier.", "1.000000", true, 1.0, 1.1799926758),
            choiceOption("combat_difficulty", "Combat difficulty", "Combat difficulty: easy, normal, or hard.", "1", {
                { "0", "Easy" }, { "1", "Normal" }, { "2", "Hard" },
            }),
            booleanOption("combat_looks", "Combat looks", "Show combat animations and visual effects.", false),
            booleanOption("combat_messages", "Combat messages", "Show combat messages during fights.", true),
            integerOption("combat_speed", "Combat speed", "Extra combat walk animation speed.", "0", true, 0, 50),
            booleanOption("combat_taunts", "Combat taunts", "Allow combat taunts and barks.", true),
            choiceOption("game_difficulty", "Game difficulty", "Overall game difficulty: easy, normal, or hard.", "1", {
                { "0", "Easy" }, { "1", "Normal" }, { "2", "Hard" },
            }),
            choiceOption("item_highlight", "Item highlight", "Highlight usable items on the ground.", "1", {
                { "0", "Off" }, { "1", "On" },
            }),
            booleanOption("language_filter", "Language filter", "Filter strong language in messages.", false),
            decimalOption("mouse_sensitivity", "Mouse sensitivity", "Mouse sensitivity multiplier.", "1.000000", true, 0.25, 2.5),
            booleanOption("player_speedup", "Player speedup", "Apply combat speed changes to the player.", false),
            booleanOption("running", "Running", "Allow the player to run by default.", false),
            booleanOption("subtitles", "Subtitles", "Show subtitles for spoken dialogue.", false),
            choiceOption("target_highlight", "Target highlight", "Target highlighting: off, on, or targeting only.", "2", {
                { "0", "Off" }, { "1", "On" }, { "2", "Targeting only" },
            }),
            decimalOption("text_base_delay", "Text delay", "Base dialogue text delay in seconds.", "3.500000", true, 1.0, 6.0),
            decimalOption("text_line_delay", "Line delay", "Additional delay between dialogue lines.", "1.399994", true, 0.0, 2.0),
            choiceOption("violence_level", "Violence level", "Blood and violence level.", "3", {
                { "0", "None" }, { "1", "Minimal" }, { "2", "Normal" }, { "3", "Maximum" },
            }),
            booleanOption("running_burning_guy", "Burning-guy running", "Use the burning-guy running animation.", true),
        }),
        section("sound", "Sound", "Audio initialization, volumes, and music paths.", {
            integerOption("cache_size", "Cache size", "Audio cache size used by the engine.", "448", true, 0, 2147483647),
            integerOption("device", "Audio device", "Legacy DOS audio field; ignored by the SDL backend.", "-1", false, 0, 0, true),
            integerOption("dma", "DMA", "Legacy DOS audio field; ignored by the SDL backend.", "-1", false, 0, 0, true),
            booleanOption("initialize", "Initialize audio", "Initialize the audio subsystem at startup.", true),
            integerOption("irq", "IRQ", "Legacy DOS audio field; ignored by the SDL backend.", "-1", false, 0, 0, true),
            integerOption("master_volume", "Master volume", "Overall audio volume.", "22281", true, 0, 32767),
            booleanOption("music", "Music", "Enable background music.", true),
            textOption("music_path1", "Music path 1", "Primary music directory.", "sound\\music\\"),
            textOption("music_path2", "Music path 2", "Fallback music directory.", "sound\\music\\"),
            integerOption("music_volume", "Music volume", "Background music volume.", "22281", true, 0, 32767),
            integerOption("port", "Port", "Legacy DOS audio field; ignored by the SDL backend.", "-1", false, 0, 0, true),
            integerOption("sndfx_volume", "Effects volume", "Sound effects volume.", "22281", true, 0, 32767),
            booleanOption("sounds", "Sound effects", "Enable sound effects.", true),
            booleanOption("speech", "Speech", "Enable spoken dialogue.", true),
            integerOption("speech_volume", "Speech volume", "Spoken dialogue volume.", "22281", true, 0, 32767),
            booleanOption("debug", "Audio debug", "Enable audio debug logging.", false),
            booleanOption("debug_sfxc", "Sound cache debug", "Enable sound cache diagnostics.", true),
            booleanOption("gapless_music", "Gapless music", "Avoid gaps when changing music tracks.", true),
        }),
        section("system", "System", "Game data paths and engine-level behavior.", {
            integerOption("art_cache_size", "Art cache size", "Number of art resources kept in memory.", "8", true, 0, 2147483647),
            booleanOption("color_cycling", "Color cycling", "Enable animated palette color cycling.", true),
            textOption("critter_dat", "Critter data", "Critter data archive path.", "critter.dat"),
            textOption("critter_patches", "Critter patches", "Directory containing critter patches.", "data"),
            integerOption("cycle_speed_factor", "Cycle speed", "Multiplier for palette cycle speed.", "1", true, 0, 100),
            textOption("executable", "Executable name", "Base executable name used by the engine.", "game"),
            integerOption("free_space", "Free space", "Required free disk space in kilobytes.", "20480", true, 0, 2147483647),
            booleanOption("hashing", "Hashing", "Use file hashing while loading resources.", true),
            booleanOption("interrupt_walk", "Interrupt walking", "Allow input to interrupt walking.", true),
            textOption("language", "Language", "Game language name.", "english"),
            textOption("master_dat", "Master data", "Main game data archive path.", "master.dat"),
            textOption("master_patches", "Master patches", "Primary patch directory.", "data"),
            booleanOption("scroll_lock", "Scroll lock", "Use scroll lock for the alternate input mode.", false),
            integerOption("splash", "Splash screen", "Splash screen selection.", "0"),
            textOption("screenshots_format", "Screenshot format", "Format used for screenshots.", "png"),
        }),
        section("screen", "Screen", "Resolution, scaling, and window mode.", {
            integerOption("resolution_x", "Width", "Game render width in pixels.", "640", true, 640, 7680),
            integerOption("resolution_y", "Height", "Game render height in pixels.", "480", true, 480, 4320),
            integerOption("scale", "Scale", "Integer display scale from 1 to 4.", "1", true, 1, 4),
            choiceOption("windowed", "Window mode", "Choose fullscreen, windowed, or borderless fullscreen.", "0", {
                { "0", "Fullscreen" }, { "1", "Windowed" }, { "2", "Borderless fullscreen" },
            }),
            booleanOption("mouse_lock", "Lock mouse", "Keep the mouse inside the game window when windowed.", false),
        }),
        section("ui", "Interface", "Interface layout, display, and quality-of-life options.", {
            choiceOption("iface_bar_mode", "Interface bar mode", "Place the game window above or below the interface bar.", "0", {
                { "0", "Default" }, { "1", "Full height" },
            }),
            integerOption("iface_bar_width", "Interface bar width", "Width of the interface bar in pixels.", "800", true, 640, 4320),
            integerOption("iface_bar_side_art", "Interface side art", "Side art style; common values are 0 through 8.", "2", true, 0, 999),
            choiceOption("iface_bar_sides_ori", "Side art orientation", "Choose how interface side graphics extend.", "0", {
                { "0", "Bar outward" }, { "1", "Screen inward" },
            }),
            choiceOption("main_menu_scale_mode", "Main menu scale", "Main menu background and control scaling.", "1", {
                { "0", "Native size" }, { "1", "Fit background" }, { "2", "Fit background and controls" },
            }),
            booleanOption("in_game_menu_help", "In-game menu help", "Show the Help option in the in-game menu.", true),
            choiceOption("splash_screen_size", "Splash size", "How the splash screen is scaled.", "1", {
                { "0", "Native size" }, { "1", "Fit" }, { "2", "Stretch" },
            }),
            booleanOption("movie_aspect_fit", "Fit movie aspect", "Fit movies while preserving their aspect ratio.", true),
            booleanOption("edg_support", "EDG support", "Load EDG map edge files when present.", true),
            booleanOption("ignore_map_edges", "Ignore map edges", "Disable vanilla map edge blocking.", false),
            booleanOption("quick_toolbar_visible", "Quick toolbar", "Show the quick-actions toolbar when supported.", false),
            decimalOption("anim_speed", "Animation speed", "UI transition animation speed multiplier.", "1.000000", true, 0.1, 100.0),
            choiceOption("skip_opening_movies", "Opening movies", "Opening movie behavior.", "1", {
                { "0", "Play all" }, { "1", "Skip movies" }, { "2", "Skip movies and splash" },
            }),
            booleanOption("display_karma_changes", "Karma changes", "Show karma changes in the notification window.", false),
            booleanOption("display_bonus_damage", "Bonus damage", "Show bonus damage numbers in combat.", false),
            booleanOption("numbers_in_dialogue", "Dialogue numbers", "Show response numbers in dialogue.", false),
            booleanOption("dialog_border", "High-resolution dialog border", "Use high-resolution dialogue borders.", true),
            integerOption("auto_quick_save", "Auto quick-save slots", "Number of automatic quick-save slots.", "0", true, 0, 10),
            booleanOption("enable_high_resolution_stencil", "High-resolution stencil", "Enable high-resolution map stencil rendering.", true),
            booleanOption("extend_ap_bar", "Extended AP bar", "Show up to 16 action points instead of 10.", false),
            booleanOption("expand_barter_window", "Expanded barter window", "Add a fourth item slot to each barter side.", false),
            integerOption("inventory_columns", "Inventory columns", "Number of inventory and loot columns.", "1", true, 1, 2),
            choiceOption("loot_weight_indicator", "Loot weight indicator", "How item weight is shown in containers.", "1", {
                { "0", "Off" }, { "1", "Simple" }, { "2", "Detailed" }, { "3", "Container size" },
            }),
            integerOption("loot_container_size_indicator_threshold", "Container threshold", "Container load percentage before showing its indicator.", "50", true, 0, 100),
        }),
        section("qol", "Quality of Life", "Convenience options added by the engine.", {
            integerOption("use_walk_distance", "Walk distance", "Distance limit used when choosing a walk destination.", "3", true, 0, 100),
            booleanOption("auto_open_doors", "Auto-open doors", "Open doors automatically while walking.", false),
            booleanOption("party_trade_from_menu", "Party trade from menu", "Allow party trade directly from the menu.", true),
            booleanOption("party_loot_and_barter", "Party loot and barter", "Allow party members to loot and barter together.", false),
        }),
    };

    return schema;
}

bool GameConfigEditor::load(const fs::path& path, std::string& error)
{
    path_ = path;
    sourceLines_.clear();
    sections_ = gameConfigSchema();
    for (GameConfigSection& section : sections_) {
        for (GameConfigOption& option : section.options)
            option.value = option.defaultValue;
    }

    loaded_ = false;
    fileExists_ = false;
    dirty_ = false;
    error.clear();

    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        std::error_code ec;
        if (fs::exists(path_, ec)) {
            error = "Could not read " + path_.u8string() + ".";
            return false;
        }
        loaded_ = true;
        return true;
    }

    fileExists_ = true;
    std::string currentSection;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        SourceLine source;
        source.raw = line;
        source.section = currentSection;

        std::string stripped = trim(line);
        if (stripped.size() >= 3 && (unsigned char)stripped[0] == 0xEF &&
            (unsigned char)stripped[1] == 0xBB && (unsigned char)stripped[2] == 0xBF) {
            stripped.erase(0, 3);
            stripped = trim(stripped);
        }
        if (!stripped.empty() && stripped.front() == '[' && stripped.back() == ']') {
            currentSection = lower(trim(stripped.substr(1, stripped.size() - 2)));
            source.section = currentSection;
        } else if (!stripped.empty() && stripped.front() != ';' && stripped.front() != '#') {
            size_t equals = stripped.find('=');
            if (equals != std::string::npos) {
                source.key = lower(trim(stripped.substr(0, equals)));
                source.section = currentSection;
                source.isKeyValue = !source.key.empty() && !currentSection.empty();
                if (source.isKeyValue) {
                    std::string rawValue = stripped.substr(equals + 1);
                    size_t comment = rawValue.find_first_of(";#");
                    if (comment != std::string::npos) {
                        size_t suffixStart = comment;
                        while (suffixStart > 0 &&
                               std::isspace((unsigned char)rawValue[suffixStart - 1]) != 0) {
                            --suffixStart;
                        }
                        source.inlineComment = rawValue.substr(suffixStart);
                        rawValue.resize(suffixStart);
                    }
                    GameConfigOption* option = findOption(sections_, currentSection, source.key);
                    if (option != nullptr)
                        option->value = trim(rawValue);
                }
            }
        }
        sourceLines_.push_back(std::move(source));
    }

    if (!in.eof() && in.fail()) {
        error = "Could not finish reading " + path_.u8string() + ".";
        return false;
    }

    loaded_ = true;
    return true;
}

bool GameConfigEditor::save(std::string& error)
{
    if (!loaded_) {
        error = "The Fallout 2 configuration has not been loaded.";
        return false;
    }

    for (const GameConfigSection& section : sections_) {
        for (const GameConfigOption& option : section.options) {
            if (!validateOption(section, option, error))
                return false;
        }
    }

    std::error_code ec;
    if (!path_.parent_path().empty())
        fs::create_directories(path_.parent_path(), ec);
    if (ec) {
        error = "Could not create the configuration directory.";
        return false;
    }

    std::unordered_map<std::string, bool> emitted;
    std::unordered_set<std::string> seenSections;
    std::vector<std::string> output;

    auto appendMissing = [&](const std::string& sectionName) {
        GameConfigSection* section = nullptr;
        for (GameConfigSection& candidate : sections_) {
            if (lower(candidate.key) == lower(sectionName)) {
                section = &candidate;
                break;
            }
        }
        if (section == nullptr)
            return;

        for (const GameConfigOption& option : section->options) {
            const std::string id = mapKey(section->key, option.key);
            if (!emitted[id]) {
                output.push_back(option.key + "=" + normalizedValue(option));
                emitted[id] = true;
            }
        }
    };

    std::string currentSection;
    for (const SourceLine& source : sourceLines_) {
        std::string stripped = trim(source.raw);
        if (!stripped.empty() && stripped.front() == '[' && stripped.back() == ']') {
            appendMissing(currentSection);
            currentSection = lower(trim(stripped.substr(1, stripped.size() - 2)));
            seenSections.insert(currentSection);
            output.push_back(source.raw);
            continue;
        }

        if (source.isKeyValue) {
            GameConfigOption* option = findOption(sections_, source.section, source.key);
            if (option != nullptr) {
                const std::string id = mapKey(source.section, source.key);
                std::string prefix = source.raw;
                size_t equals = prefix.find('=');
                if (equals != std::string::npos)
                    prefix.resize(equals + 1);
                output.push_back(prefix + normalizedValue(*option) + source.inlineComment);
                emitted[id] = true;
                continue;
            }
        }

        output.push_back(source.raw);
    }

    appendMissing(currentSection);
    for (const GameConfigSection& section : sections_) {
        if (seenSections.find(lower(section.key)) != seenSections.end())
            continue;
        if (!output.empty() && !output.back().empty())
            output.push_back("");
        output.push_back("[" + section.key + "]");
        appendMissing(section.key);
    }

    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "Could not write " + path_.u8string() + ".";
        return false;
    }
    for (const std::string& line : output)
        out << line << '\n';
    out.flush();
    if (!out) {
        error = "Could not finish writing " + path_.u8string() + ".";
        return false;
    }

    fileExists_ = true;
    dirty_ = false;
    error.clear();
    return true;
}

} // namespace launcher
