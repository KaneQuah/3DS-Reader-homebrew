#include "settings/config.h"
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <cerrno>
#include <climits>
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

//-----------------------------------------------------------------------------
// Numeric INI values
//
// These were read with std::stoi, which THROWS on a value that is not a
// number (std::invalid_argument) or does not fit (std::out_of_range). This
// project is built with -fno-exceptions (see CXXFLAGS in the Makefile), so
// there is no handler for either: libstdc++ throws, the exception cannot
// propagate through these frames, and the process calls std::terminate.
//
// A config.ini containing
//
//     font_size = large
//
// therefore aborted the app during startup — and config.ini is a plain text
// file on the SD card that the app invites the user to hand-edit, as well as
// the first thing to be left half-written if the console loses power during
// Config::save().
//
// parseIntStrict() cannot throw, and rejects trailing junk ("16pt") rather
// than silently accepting the leading digits the way atoi would.
//-----------------------------------------------------------------------------
static bool parseIntStrict(const std::string& s, int& out) {
    if (s.empty()) return false;

    errno = 0;
    char* end = nullptr;
    long v = strtol(s.c_str(), &end, 10);

    if (end == s.c_str()) return false;                   // no digits at all
    while (*end == ' ' || *end == '\t') end++;             // trailing blanks ok
    if (*end != '\0') return false;                       // trailing junk
    if (errno == ERANGE || v < INT_MIN || v > INT_MAX) return false;

    out = (int)v;
    return true;
}

/// Read a numeric setting, ignoring the line entirely if the value is not a
/// plain integer, and clamping what is accepted to the range the rest of the
/// app assumes. bright_level in particular is handed straight to
/// GSPLCD_SetBrightness(), and font_size drives the layout engine.
static void readClamped(const std::string& val, int lo, int hi, int& field) {
    int v;
    if (!parseIntStrict(val, v)) return;    // keep the existing/default value
    field = std::max(lo, std::min(hi, v));
}

/// Read a setting whose value must be one of a fixed set of names, ignoring
/// anything else. Without this a junk theme name would be written back out by
/// Config::save() and shown as the current value in the settings panel, even
/// though theme() silently falls back to light.
static void readEnum(const std::string& val, const char* const* allowed,
                     std::string& field) {
    for (int i = 0; allowed[i]; i++)
        if (val == allowed[i]) { field = val; return; }
}

/// Reject a filename that tries to escape the directory it is looked up in.
/// font_file is concatenated onto FONTS_PATH and handed to FreeType, and
/// tts_voice onto TTS_PATH; neither should be able to name a path elsewhere on
/// the SD card.
static bool isPlainFilename(const std::string& s) {
    return !s.empty() &&
           s.find('/')  == std::string::npos &&
           s.find('\\') == std::string::npos &&
           s.find("..")  == std::string::npos;
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

        static const char* const kThemes[]  = { "light", "dark", "sepia", nullptr };
        static const char* const kSpacing[] = { "compact", "normal", "relaxed", nullptr };
        static const char* const kMargins[] = { "narrow", "normal", "wide", nullptr };

        if (section == "Display") {
            if      (key == "theme")        readEnum(val, kThemes, theme_name);
            else if (key == "font_file")    { if (val == "built-in" || isPlainFilename(val))
                                                  font_file = val; }
            else if (key == "font_size")    readClamped(val, 12, 28, font_size);
            else if (key == "line_spacing") readEnum(val, kSpacing, line_spacing);
            else if (key == "margins")      readEnum(val, kMargins, margins);
            else if (key == "justify")      justify      = (val == "true");
        } else if (section == "Controls") {
            if (key == "orientation_lock")  orientation_lock = (val == "true");
        } else if (section == "Reader") {
            if      (key == "auto_scroll_speed") readClamped(val, 0, 10, auto_scroll_speed);
            else if (key == "bright_level")      readClamped(val, 1, 5,  bright_level);
            else if (key == "show_timings")      show_timings = (val == "true");
        } else if (section == "TTS") {
            if      (key == "tts_enabled") tts_enabled = (val == "true");
            else if (key == "tts_speed")   readClamped(val, 80, 300, tts_speed);
            else if (key == "tts_voice")   { if (isPlainFilename(val)) tts_voice = val; }
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
        "show_timings = %s\n"
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
        show_timings ? "true" : "false",
        tts_enabled ? "true" : "false",
        tts_speed,
        tts_voice.c_str()
    );

    fclose(f);
    return true;
}
