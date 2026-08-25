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
    // Gyro-based auto-flip (see gyro_handler.cpp) has swapped the screens'
    // left/right roles unprompted on real hardware even after adding
    // stationary-drift rejection. Defaulting to locked means new installs
    // never see a surprise flip; auto-detect is available as an explicit
    // opt-in via the Orientation row in Settings.
    bool orientation_lock   = true;

    // [Reader]
    int  auto_scroll_speed  = 0;   // 0 = off, 1–10 pages/min
    int  bright_level       = 3;   // 1–5

    // Report how long a chapter load actually took, as a toast on the top
    // screen ("Chapter load 812 ms — text 190, layout 610").
    //
    // Page-turn speed has been the project's open problem across several
    // sessions, and it has been chased twice from symptoms rather than
    // numbers. TRACE() is the usual measuring tool, but it writes to the SD
    // card and fflush()es every line, so it distorts exactly the timings that
    // matter — and it is compiled out of release builds entirely. This is the
    // one channel that reports real hardware timings from a normal
    // ENABLE_TRACE=0 build. Off by default; toggled from Settings.
    bool show_timings       = false;

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
