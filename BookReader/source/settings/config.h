#pragma once
#include "common.h"
#include <string>

//-----------------------------------------------------------------------------
// Config — mirrors config.ini layout from design doc §12.1
//-----------------------------------------------------------------------------
struct Config {
    // [Display]
    std::string theme_name  = "light";   // "light" | "dark" | "sepia"
    std::string font_file   = "built-in";
    int         font_size   = 16;        // pt, 12–28
    std::string line_spacing = "normal"; // "compact" | "normal" | "relaxed"
    std::string margins     = "normal";  // "narrow" | "normal" | "wide"
    bool        justify     = false;

    // [Controls]
    bool orientation_lock   = false;

    // [Reader]
    int  auto_scroll_speed  = 0;   // 0 = off, 1–10 pages/min
    int  bright_level       = 3;   // 1–5

    // [TTS]
    bool        tts_enabled = false;
    int         tts_speed   = 150; // WPM, 80–300
    std::string tts_voice   = "en";

    //--- Derived helpers ---
    const Theme& theme()        const;
    float        lineSpacingMul() const;   // 0.9, 1.0, or 1.3
    int          marginPx()     const;     // 5, 10, or 18

    //--- Persistence ---
    bool load(const char* path = CONFIG_PATH);
    bool save(const char* path = CONFIG_PATH) const;
};

// Global singleton — populated in main before the loop starts
extern Config g_config;
