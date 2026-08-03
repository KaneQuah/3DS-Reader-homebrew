#include "settings/config.h"
#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>

Config g_config;

//-----------------------------------------------------------------------------
// Helpers
//-----------------------------------------------------------------------------
const Theme& Config::theme() const {
    if (theme_name == "dark")  return THEME_DARK;
    if (theme_name == "sepia") return THEME_SEPIA;
    return THEME_LIGHT;
}

float Config::lineSpacingMul() const {
    if (line_spacing == "compact")  return 0.9f;
    if (line_spacing == "relaxed")  return 1.3f;
    return 1.0f;
}

int Config::marginPx() const {
    if (margins == "narrow") return 5;
    if (margins == "wide")   return 18;
    return 10;
}

//-----------------------------------------------------------------------------
// Minimal INI parser: supports [sections] and key = value lines.
// Lines beginning with ';' or '#' are treated as comments.
//-----------------------------------------------------------------------------
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool Config::load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return false;

    char line[256];
    std::string section;
    while (fgets(line, sizeof(line), f)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == ';' || s[0] == '#') continue;

        if (s[0] == '[') {
            size_t e = s.find(']');
            if (e != std::string::npos)
                section = trim(s.substr(1, e - 1));
            continue;
        }

        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(s.substr(0, eq));
        std::string val = trim(s.substr(eq + 1));
        // strip inline comment
        size_t sc = val.find(';');
        if (sc != std::string::npos) val = trim(val.substr(0, sc));

        if (section == "Display") {
            if      (key == "theme")        theme_name   = val;
            else if (key == "font_file")    font_file    = val;
            else if (key == "font_size")    font_size    = std::stoi(val);
            else if (key == "line_spacing") line_spacing = val;
            else if (key == "margins")      margins      = val;
            else if (key == "justify")      justify      = (val == "true");
        } else if (section == "Controls") {
            if (key == "orientation_lock")  orientation_lock = (val == "true");
        } else if (section == "Reader") {
            if      (key == "auto_scroll_speed") auto_scroll_speed = std::stoi(val);
            else if (key == "bright_level")      bright_level      = std::stoi(val);
        } else if (section == "TTS") {
            if      (key == "tts_enabled") tts_enabled = (val == "true");
            else if (key == "tts_speed")   tts_speed   = std::stoi(val);
            else if (key == "tts_voice")   tts_voice   = val;
        }
    }
    fclose(f);
    return true;
}

bool Config::save(const char* path) const {
    FILE* f = fopen(path, "w");
    if (!f) return false;

    fprintf(f,
        "[Display]\n"
        "theme = %s\n"
        "font_file = %s\n"
        "font_size = %d\n"
        "line_spacing = %s\n"
        "margins = %s\n"
        "justify = %s\n"
        "\n"
        "[Controls]\n"
        "orientation_lock = %s\n"
        "\n"
        "[Reader]\n"
        "auto_scroll_speed = %d\n"
        "bright_level = %d\n"
        "\n"
        "[TTS]\n"
        "tts_enabled = %s\n"
        "tts_speed = %d\n"
        "tts_voice = %s\n",
        theme_name.c_str(),
        font_file.c_str(),
        font_size,
        line_spacing.c_str(),
        margins.c_str(),
        justify ? "true" : "false",
        orientation_lock ? "true" : "false",
        auto_scroll_speed,
        bright_level,
        tts_enabled ? "true" : "false",
        tts_speed,
        tts_voice.c_str()
    );

    fclose(f);
    return true;
}
