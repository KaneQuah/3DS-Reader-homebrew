#pragma once
// common.h — shared constants, types, and forward declarations

#include <3ds.h>
#include <citro2d.h>
#include <string>
#include <vector>
#include <cstdint>
#include <sys/stat.h>   // mkdir()

//-----------------------------------------------------------------------------
// libctru's hid.h defines legacy compatibility macros that alias short names
// onto the real functions:
//
//     #define keysDown  hidKeysDown
//     #define keysHeld  hidKeysHeld
//     ... and scanKeys, keysUp, touchRead, circleRead
//
// This project uses `keysDown` / `keysHeld` as ordinary variable and parameter
// names, which the preprocessor would rewrite into the function names — turning
// `u32 keysDown = hidKeysDown();` into `u32 hidKeysDown = hidKeysDown();` and
// shadowing the function. We call the hid* functions directly everywhere, so
// the aliases are safe to drop.
//-----------------------------------------------------------------------------
#undef scanKeys
#undef keysHeld
#undef keysDown
#undef keysUp
#undef touchRead
#undef circleRead

//-----------------------------------------------------------------------------
// Screen geometry (portrait orientation: device rotated 90° anti-clockwise)
//
// Physical framebuffers are stored landscape (400×240, 320×240). When the
// device is rotated ACW 90°, the top screen appears on the LEFT (240×400 px)
// and the bottom screen appears on the RIGHT (240×320 px).
//
// PORTRAIT_TOP_W / PORTRAIT_TOP_H describe the visible area after rotation.
// Page layout uses these dimensions. The render pipeline applies a 90° CW
// rotation via the projection matrix so content appears upright after the
// device is physically rotated.
//-----------------------------------------------------------------------------
static constexpr int SCREEN_TOP_W       = 400;   // physical landscape width
static constexpr int SCREEN_TOP_H       = 240;   // physical landscape height
static constexpr int SCREEN_BOT_W       = 320;
static constexpr int SCREEN_BOT_H       = 240;

static constexpr int PORTRAIT_TOP_W     = 240;   // logical portrait width  (top screen)
static constexpr int PORTRAIT_TOP_H     = 400;   // logical portrait height (top screen)
static constexpr int PORTRAIT_BOT_W     = 240;   // logical portrait width  (bottom screen)
static constexpr int PORTRAIT_BOT_H     = 320;   // logical portrait height (bottom screen)

//-----------------------------------------------------------------------------
// SD card paths
//-----------------------------------------------------------------------------
static constexpr const char* BASE_PATH       = "/3DS/BookReader";
static constexpr const char* BOOKS_PATH      = "/3DS/BookReader/books";
static constexpr const char* FONTS_PATH      = "/3DS/BookReader/fonts";
static constexpr const char* TTS_PATH        = "/3DS/BookReader/tts_voices";
static constexpr const char* CONFIG_PATH     = "/3DS/BookReader/config.ini";
static constexpr const char* PROGRESS_PATH   = "/3DS/BookReader/progress.json";
static constexpr const char* PROGRESS_TMP    = "/3DS/BookReader/progress.json.tmp";

//-----------------------------------------------------------------------------
// Theme colours
//-----------------------------------------------------------------------------
struct Theme {
    u32 background;   // C2D_Color32 format (RGBA)
    u32 foreground;
    u32 accent;
    u32 overlay_bg;
    u32 progress_bar;
};

static const Theme THEME_LIGHT  = { C2D_Color32(255,255,255,255), C2D_Color32(20,20,20,255),
                                    C2D_Color32(0,100,200,255),   C2D_Color32(0,0,0,180),
                                    C2D_Color32(0,120,215,255) };
static const Theme THEME_DARK   = { C2D_Color32(20,20,20,255),   C2D_Color32(230,230,230,255),
                                    C2D_Color32(80,160,255,255),  C2D_Color32(0,0,0,200),
                                    C2D_Color32(80,160,255,255) };
static const Theme THEME_SEPIA  = { C2D_Color32(242,234,210,255), C2D_Color32(60,40,10,255),
                                    C2D_Color32(160,100,40,255),  C2D_Color32(30,20,0,190),
                                    C2D_Color32(160,100,40,255) };

//-----------------------------------------------------------------------------
// App state enum — drives the main loop dispatch
//-----------------------------------------------------------------------------
enum class AppState {
    FileBrowser,
    Reading,
    Settings,
    Error,
};

//-----------------------------------------------------------------------------
// Document type
//-----------------------------------------------------------------------------
enum class DocType {
    EPUB,
    TXT,
    PDF,
    Unknown,
};

DocType docTypeFromPath(const std::string& path);
