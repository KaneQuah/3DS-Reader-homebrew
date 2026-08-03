// main.cpp — 3DS EPUB Reader entry point
//
// Design doc §4.2 Main Application Loop:
//   1. Initialise hardware services
//   2. Register APT hooks (sleep/wake/home/exit)
//   3. Show file browser
//   4. On selection: load document at saved progress or page 1
//   5. Reader loop at 60 Hz: input → layout → render → swap
//   6. On Start: settings overlay
//   7. On sleep/home: atomic progress save
//   8. On wake: refresh framebuffers, re-poll gyro
//   9. On exit: graceful shutdown

#include "common.h"
#include "trace.h"
#include "ui/file_browser.h"
#include "ui/reader_view.h"
#include "ui/overlay.h"
#include "renderer/font_engine.h"
#include "renderer/page_layout.h"
#include "settings/config.h"
#include "settings/progress.h"
#include "input/gyro_handler.h"
#include "input/touch_handler.h"
#include "audio/tts_engine.h"
#include "formats/pdf_renderer.h"
#include <3ds.h>
#include <citro2d.h>
#include <cstdio>
#include <cstring>
#include <string>

//=============================================================================
// Globals
//=============================================================================
static AppState       s_state       = AppState::FileBrowser;
static std::string    s_openPath;
static DocType        s_openType    = DocType::Unknown;
static bool           s_isNew3DS   = false;
static std::string    s_errorMsg;

// Book open request, serviced between frames — see the main loop.
static std::string    s_pendingOpenPath;
static DocType        s_pendingOpenType = DocType::Unknown;

// Trace the first few frames after entering the reader, so a failure during
// rendering shows up in the log. Unbounded per-frame tracing would be far too
// slow: every line is flushed and closed.
static constexpr int  RENDER_TRACE_FRAMES = 12;
static int            s_renderTraceFrames = 0;

// Render targets
static C3D_RenderTarget* s_topTarget = nullptr;
static C3D_RenderTarget* s_botTarget = nullptr;

// APT hook cookie
static aptHookCookie s_aptHook;

// Gyroscope handler
static GyroHandler    s_gyro;

// Battery poll counter
static int            s_batteryCounter = 0;
static constexpr int  BATTERY_POLL_FRAMES = 60 * 30; // every 30 seconds at 60 fps

//=============================================================================
// APT hook callback — handles sleep, wake, home, and exit events
//=============================================================================
static void aptHookCallback(APT_HookType type, void* /*param*/) {
    switch (type) {
        case APTHOOK_ONSUSPEND:
        case APTHOOK_ONSLEEP:
            // Save reading progress atomically
            if (!s_openPath.empty() && s_state == AppState::Reading) {
                g_reader.saveProgress(s_openPath);
            }
            // Pause TTS
            g_tts.pause();
            break;

        case APTHOOK_ONRESTORE:
        case APTHOOK_ONWAKEUP:
            // Refresh framebuffers from in-memory layout — no disk read needed
            // Re-poll gyroscope to get current orientation
            // (render() will pick up the new gyro state on the next frame)
            g_tts.resume();
            break;

        case APTHOOK_ONEXIT:
            // Full graceful shutdown
            if (!s_openPath.empty()) g_reader.saveProgress(s_openPath);
            g_config.save();
            g_tts.stop();
            g_tts.exit();
            s_gyro.exit();
            g_fontEngine.exit();
            g_reader.close();
            g_pdfRenderer.close();
            C2D_Fini();
            C3D_Fini();
            gfxExit();
            romfsExit();
            ptmuExit();
            gspLcdExit();
            cfguExit();
            break;

        default:
            break;
    }
}

//=============================================================================
// Hardware initialisation
//=============================================================================
static bool initHardware() {
    // Graphics
    gfxInitDefault();
    gfxSet3D(false);  // disable stereoscopic 3D (incompatible with portrait mode)

    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    // Create render targets for top and bottom screens
    // Physical framebuffer dimensions (landscape): top 400×240, bottom 320×240
    s_topTarget = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    s_botTarget = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    if (!s_topTarget || !s_botTarget) return false;

    // romfs (built-in font, TTS voice data)
    romfsInit();

    // SD card filesystem
    // libctru auto-mounts sdmc:/ via newlib on startup — no explicit init needed.
    // Ensure our directory structure exists.
    mkdir(BASE_PATH,   0777);
    mkdir(BOOKS_PATH,  0777);
    mkdir(FONTS_PATH,  0777);
    mkdir(TTS_PATH,    0777);

    // Only now can the trace log be created — it lives inside BASE_PATH.
    TRACE_RESET();
    TRACE("initHardware: graphics, romfs and SD paths up");

    // Config service
    cfguInit();

    // LCD backlight control — GSPLCD owns screen brightness on the 3DS.
    // (mcuHwc has no brightness command; it only exposes battery/LED/sliders.)
    gspLcdInit();

    // Power management service — required for PTMU_GetBatteryLevel()
    ptmuInit();

    // New 3DS detection
    APT_CheckNew3DS(&s_isNew3DS);
    if (s_isNew3DS) {
        // Enable extra RAM (linker script must reserve it)
        osSetSpeedupEnable(true);
        // Allow background threads on extra CPU cores
        APT_SetAppCpuTimeLimit(30);
    }

    return true;
}

//=============================================================================
// Loading screen
//
// Opening a book blocks for as long as it takes to decompress and lay out a
// chapter. Without this the screen simply freezes on whatever was last drawn,
// which is indistinguishable from a crash.
//
// Self-contained: opens and closes its own frame. It must NOT be called while
// another frame is in progress.
//=============================================================================
static void showLoading(const char* msg) {
    const Theme& theme = g_config.theme();

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

    C2D_TargetClear(s_topTarget, theme.background);
    C2D_SceneBegin(s_topTarget);
    g_fontEngine.drawText(msg, 12, SCREEN_TOP_H / 2 - 10, theme.foreground, 14);

    C2D_TargetClear(s_botTarget, theme.background);
    C2D_SceneBegin(s_botTarget);

    C3D_FrameEnd(0);
}

//=============================================================================
// Error screen
//
// Draws into the frame the main loop already has open. It must NOT begin or
// end a frame of its own: this is called from inside the state machine, and
// nesting C3D_FrameBegin/C3D_FrameEnd inside an open frame unbalances citro3d's
// command buffer and wedges the GPU — which looks exactly like a hang, on the
// one code path you hit when something else has already gone wrong.
//=============================================================================
static void showError(const std::string& msg) {
    C2D_TargetClear(s_topTarget, C2D_Color32(40,0,0,255));
    C2D_SceneBegin(s_topTarget);
    g_fontEngine.drawText("Error", 8, 8, C2D_Color32(255,80,80,255), 18);
    g_fontEngine.drawText(msg.c_str(), 8, 40, C2D_Color32(220,220,220,255), 13);
    g_fontEngine.drawText("Press B to return to file browser", 8, 200,
                          C2D_Color32(180,180,180,255), 11);
    C2D_TargetClear(s_botTarget, C2D_Color32(30,0,0,255));
    C2D_SceneBegin(s_botTarget);
}

//=============================================================================
// Battery monitoring (design doc §10.5)
//=============================================================================
static void checkBattery() {
    u8 level = 0;
    PTMU_GetBatteryLevel(&level);

    if (level <= 1) {
        // Below 5%: critical — save every page turn (handled by caller)
        // Show persistent warning
        if (!s_openPath.empty()) g_reader.saveProgress(s_openPath);
        g_reader.setToast("Critical battery — save your place!");
    } else if (level <= 2) {
        // Below 15%: non-intrusive toast
        if (!s_openPath.empty()) g_reader.saveProgress(s_openPath);
        g_reader.setToast("Battery low — progress saved");
    }
}

//=============================================================================
// Main
//=============================================================================
int main() {
    if (!initHardware()) return 1;

    // Font engine
    if (!g_fontEngine.init()) return 1;
    if (!g_config.font_file.empty() && g_config.font_file != "built-in")
        g_fontEngine.loadFont(g_config.font_file);

    // Load persisted state
    g_config.load(CONFIG_PATH);
    g_progress.load(PROGRESS_PATH);

    // Apply saved brightness (bright_level is 1–5, matching the hardware range)
    GSPLCD_SetBrightness(GSPLCD_SCREEN_BOTH, g_config.bright_level);

    // Gyroscope
    s_gyro.init();

    // TTS (stretch goal — init may silently fail if eSpeak NG not built in)
    std::string ttsPath = std::string(TTS_PATH) + "/" + g_config.tts_voice;
    g_tts.init(ttsPath, g_config.tts_voice);

    // APT hooks — must be registered after hardware init
    aptHook(&s_aptHook, aptHookCallback, nullptr);
    aptSetSleepAllowed(true);
    aptSetHomeAllowed(true);

    // Initial scan of books directory
    FileBrowser browser;
    browser.scan();

    //=========================================================================
    // Main loop
    //=========================================================================
    while (aptMainLoop()) {
        //--- Input scan ---
        hidScanInput();
        u32 keysDown = hidKeysDown();
        u32 keysHeld = hidKeysHeld();

        touchPosition touch;
        hidTouchRead(&touch);
        bool touchDown     = (keysDown & KEY_TOUCH) != 0;
        bool touchReleased = (hidKeysUp() & KEY_TOUCH) != 0;

        //--- Gyroscope update (30 Hz internally) ---
        bool flipped = s_gyro.update(g_config.orientation_lock);
        (void)flipped;  // handled by orientation_ in render

        //--- Battery check (every 30 s) ---
        if (++s_batteryCounter >= BATTERY_POLL_FRAMES) {
            s_batteryCounter = 0;
            if (s_state == AppState::Reading) checkBattery();
        }

        //--- Deferred book open -------------------------------------------
        // Opening a book takes seconds: it decompresses a chapter, parses XML
        // and runs the layout engine, and it loads glyphs into the font atlas
        // (which issues GPU cache-flush commands).
        //
        // None of that may happen inside an open GPU frame. C3D_FrameBegin
        // holds the command buffer open, so submitting texture flushes from
        // within it — for seconds, while also blocking on SD reads — is a good
        // way to wedge the GPU. So the browser only *requests* an open, and we
        // service it here, between frames.
        if (!s_pendingOpenPath.empty()) {
            std::string path = s_pendingOpenPath;
            DocType     type = s_pendingOpenType;
            s_pendingOpenPath.clear();

            showLoading("Opening book...");

            TRACE("=== opening %s ===", path.c_str());
            std::string err;
            if (!g_reader.open(path, type, s_isNew3DS, err)) {
                TRACE("=== open FAILED: %s ===", err.c_str());
                s_errorMsg = err;
                s_state    = AppState::Error;
            } else {
                TRACE("=== open OK, entering reader ===");
                s_openPath = path;
                s_openType = type;
                s_state    = AppState::Reading;
                s_renderTraceFrames = RENDER_TRACE_FRAMES;

                const BookProgress* bp = g_progress.get(path);
                if (bp && (bp->chapter > 0 || bp->spread > 0)) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "Resumed: chapter %d, page %d",
                             bp->chapter + 1, bp->spread * 2 + 1);
                    g_reader.setToast(buf);
                }

                if (g_config.tts_enabled)
                    g_tts.speak(g_reader.currentPageText(true), g_config.tts_speed);
            }
            continue;   // start a clean frame next iteration
        }

        //--- State machine ---
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

        if (s_renderTraceFrames > 0) {
            TRACE("frame: state=%d ch=%d spread=%d/%d",
                  (int)s_state, g_reader.currentChapter(),
                  g_reader.currentSpread(), g_reader.totalSpreads());
            s_renderTraceFrames--;
        }

        switch (s_state) {

            //----------------------------------------------------------------
            case AppState::FileBrowser: {
                // Settings overlay takes priority while it is open.
                //
                // Previously pressing Start opened the panel and drew it once,
                // inside the same if-block — so it vanished on the very next
                // frame and never received any input. The overlay has to be
                // updated and redrawn every frame for as long as it is open,
                // and it must swallow input so the browser underneath does not
                // also act on it.
                if (g_overlay.isOpen()) {
                    bool relayout = false;
                    int  gotoPage = -1;
                    std::string searchQ;
                    bool closed = g_overlay.update(keysDown, touch, touchReleased,
                                                   relayout, gotoPage, searchQ);
                    browser.render(s_topTarget, s_botTarget, g_config.theme());
                    g_overlay.render(s_botTarget, g_config.theme(), {}, 0);
                    if (closed) g_config.save();
                    break;
                }

                std::string longPressPath;
                std::string selected = browser.update(keysDown, keysHeld,
                                                       touch, touchDown, touchReleased,
                                                       &longPressPath);
                browser.render(s_topTarget, s_botTarget, g_config.theme());

                // Handle context menu action
                if (browser.contextAction() == FileBrowser::ContextAction::DeleteProgress) {
                    // Remove progress entry (not yet implemented in store, just rescan)
                    browser.scan();
                }

                // Y cycles sort mode; Start opens global settings
                if (keysDown & KEY_Y) browser.cycleSortMode();
                if (keysDown & KEY_START) g_overlay.open(OverlayPanel::Tab::Settings);

                if (!selected.empty()) {
                    DocType t = docTypeFromPath(selected);
                    if (t == DocType::Unknown) break;  // skip

                    // Only request the open. The work itself happens between
                    // frames, at the top of the next iteration.
                    s_pendingOpenPath = selected;
                    s_pendingOpenType = t;
                }
                break;
            }

            //----------------------------------------------------------------
            case AppState::Reading: {
                // Settings overlay takes priority when open
                if (g_overlay.isOpen()) {
                    bool relayout = false;
                    int  gotoPage = -1;
                    std::string searchQ;
                    bool closed = g_overlay.update(keysDown, touch, touchReleased,
                                                   relayout, gotoPage, searchQ);
                    if (closed) {
                        g_config.save();
                        if (relayout) {
                            // Font size / margins changed. Only the current
                            // chapter needs re-laying out — reopening the book
                            // would throw away the whole cache for nothing.
                            g_reader.relayoutCurrentChapter();
                        }
                    }
                    // "Go to" is a spread within the current chapter.
                    if (gotoPage >= 0)
                        g_reader.goToPosition(g_reader.currentChapter(), gotoPage);

                    // Render reader in background, then overlay on top
                    float flipA = s_gyro.flipping()
                                  ? (float)s_gyro.flipFrame() / 4.0f
                                  : 0.0f;
                    g_reader.render(s_topTarget, s_botTarget, g_config.theme(),
                                    s_gyro.orientation(), flipA);
                    g_overlay.render(s_botTarget, g_config.theme(),
                                     {}, 0);
                    break;
                }

                // Open overlay on Start
                if (keysDown & KEY_START) {
                    g_overlay.open(OverlayPanel::Tab::Settings);
                    break;
                }

                // Open search/goto on Y
                if (keysDown & KEY_Y) {
                    g_overlay.open(OverlayPanel::Tab::Search);
                    break;
                }

                // Toggle bookmark on Select
                if (keysDown & KEY_SELECT) {
                    g_reader.toggleBookmark();
                }

                // Toggle TTS on X (stretch goal)
                if (keysDown & KEY_X) {
                    g_config.tts_enabled = !g_config.tts_enabled;
                    if (g_config.tts_enabled)
                        g_tts.speak(g_reader.currentPageText(true), g_config.tts_speed);
                    else
                        g_tts.stop();
                }

                // B: return to file browser
                if (keysDown & KEY_B) {
                    g_reader.saveProgress(s_openPath);
                    g_tts.stop();
                    g_reader.close();
                    s_openPath.clear();
                    browser.scan();
                    s_state = AppState::FileBrowser;
                    break;
                }

                // Reader input
                bool nav = g_reader.update(keysDown, keysHeld, touch, touchDown, touchReleased, s_gyro);

                // On page turn: save progress and sync TTS
                if (nav) {
                    // Re-arm frame tracing so navigation is visible in the log.
                    s_renderTraceFrames = RENDER_TRACE_FRAMES;
                    g_reader.saveProgress(s_openPath);
                    if (g_config.tts_enabled)
                        g_tts.speak(g_reader.currentPageText(true), g_config.tts_speed);
                }

                // Auto-scroll
                g_reader.tickAutoScroll(g_config.auto_scroll_speed);

                // Flip fade animation alpha
                float flipA = s_gyro.flipping()
                              ? (float)s_gyro.flipFrame() / 4.0f
                              : 0.0f;

                // Render
                g_reader.render(s_topTarget, s_botTarget, g_config.theme(),
                                s_gyro.orientation(), flipA);
                break;
            }

            //----------------------------------------------------------------
            case AppState::Error: {
                showError(s_errorMsg);
                if (keysDown & KEY_B) {
                    s_state = AppState::FileBrowser;
                    browser.scan();
                }
                break;
            }
        }

        C3D_FrameEnd(0);
    }

    //=========================================================================
    // Graceful shutdown
    //=========================================================================
    aptUnhook(&s_aptHook);

    if (!s_openPath.empty()) g_reader.saveProgress(s_openPath);
    g_config.save();

    g_tts.stop();
    g_tts.exit();
    s_gyro.exit();

    g_reader.close();
    g_pdfRenderer.close();
    g_fontEngine.exit();

    C2D_Fini();
    C3D_Fini();
    gfxExit();
    ptmuExit();
    gspLcdExit();
    cfguExit();
    romfsExit();

    return 0;
}
