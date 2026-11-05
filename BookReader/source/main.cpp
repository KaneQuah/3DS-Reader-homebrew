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
#include "perf.h"
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

//-----------------------------------------------------------------------------
// Quit combo — L + R + B, held.
//
// Homebrew launched from hbmenu exits by falling out of aptMainLoop(), and
// until now this app had no way to do that from its own UI: the only routes
// out were the HOME menu (which on Old 3DS can reclaim the app's memory before
// APTHOOK_ONEXIT runs) and pulling the power. Neither is a graceful shutdown,
// and both are awkward in a 3dslink test cycle.
//
// Why L+R+B, and why held:
//   • Every single button is already bound in the reader — Start is Settings,
//     Y is Go-To, X is TTS, Select is bookmark, B is back-to-library, the
//     D-Pad turns pages. Only a chord is free.
//   • L and R are inert during reflowable reading (page turns moved to
//     Up/Down), so the chord costs nothing there, and requiring B on top of
//     them means the fixed-layout PDF binding for L/R can simply stand down
//     while B is held (see ReaderView::update).
//   • It does not collide with Luma3DS's own combos — Rosalina's default
//     trigger is L+Down+Select, and neither it nor the screenshot shortcut
//     uses this chord.
//   • The hold requirement is what makes it safe to give a destructive-ish
//     action three buttons that are otherwise live. A stray simultaneous
//     press does not quit the app out from under a reader; deliberately
//     holding for half a second does.
//-----------------------------------------------------------------------------
static constexpr u32  QUIT_COMBO       = KEY_L | KEY_R | KEY_B;
static constexpr int  QUIT_HOLD_FRAMES = 30;   // ~0.5 s at 60 fps
static int            s_quitHoldFrames = 0;

//-----------------------------------------------------------------------------
// Progress write-behind (see progress.h)
//
// ReaderView::saveProgress() only updates the in-memory store; the SD write
// happens here, at the points where an unwritten position could actually be
// lost. Doing it on every page turn cost an fopen + fclose + rename round trip
// to the FS process on the one code path the user is waiting on.
//
// AUTOSAVE_FRAMES is the backstop for the case none of the explicit flush
// points cover: the battery dying, or the console being force-powered-off,
// mid-chapter. Half a minute of unsaved reading is at most a page or two.
//-----------------------------------------------------------------------------
static constexpr int  AUTOSAVE_FRAMES = 60 * 30;   // every 30 seconds at 60 fps
static int            s_autosaveCounter = 0;

static void flushProgress() {
    if (g_progress.saveIfDirty(PROGRESS_PATH))
        TRACE("progress.json written");
}

//=============================================================================
// Shutdown
//
// There used to be two copies of this sequence: one in the APTHOOK_ONEXIT
// branch of aptHookCallback() below, and one after aptMainLoop() returns. Both
// ran C2D_Fini / C3D_Fini / gfxExit and the service *Exit calls, and neither
// knew about the other — so whichever way the app closed, one of the two was
// wrong:
//
//   • if the hook fires while the exit request is being handled and the loop
//     then falls out, the post-loop copy tears everything down a second time:
//     C3D_Fini frees its buffers twice, and the refcounted services
//     (cfguExit, ptmuExit, gspLcdExit) decrement past zero;
//   • if the hook never fires, the carefully written branch in it was dead
//     code that only looked like it was providing a graceful exit.
//
// One function with an idempotency guard answers both. Calling it twice is
// harmless, which is what makes it safe to call from the hook AND after the
// loop without having to know which path ran first.
//
// Order matters in one place: FontEngine::exit() calls C3D_TexDelete on the
// glyph atlas, so it has to run before C3D_Fini().
//=============================================================================
static bool s_shutdownDone = false;

static void shutdownAll() {
    if (s_shutdownDone) return;
    s_shutdownDone = true;

    TRACE("=== shutdown ===");

    // Persist everything first — this is the last chance.
    if (!s_openPath.empty()) g_reader.saveProgress(s_openPath);
    flushProgress();
    g_config.save();

    // Subsystems.
    g_tts.stop();
    g_tts.exit();
    s_gyro.exit();
    g_reader.close();
    g_pdfRenderer.close();
    g_fontEngine.exit();   // C3D_TexDelete — must precede C3D_Fini()

    // Graphics.
    C2D_Fini();
    C3D_Fini();
    gfxExit();

    // Services.
    ptmuExit();
    gspLcdExit();
    cfguExit();
    romfsExit();
}

//=============================================================================
// APT hook callback — handles sleep, wake, home, and exit events
//=============================================================================
static void aptHookCallback(APT_HookType type, void* /*param*/) {
    switch (type) {
        case APTHOOK_ONSUSPEND:
        case APTHOOK_ONSLEEP:
            // Save reading progress atomically. saveProgress() only updates
            // the in-memory store now, so the flush has to be explicit — a
            // suspend is exactly the moment the write-behind must be paid.
            if (!s_openPath.empty() && s_state == AppState::Reading) {
                g_reader.saveProgress(s_openPath);
            }
            flushProgress();

            // Save settings too (design doc §10.1). Previously config.ini was
            // only written on overlay close and on ONEXIT — but on Old 3DS the
            // OS may reclaim this app's memory the moment another title is
            // launched from the home menu, and ONEXIT then never fires. A
            // settings change followed by a home-button press was silently
            // lost. Config::save() is a handful of fprintf calls to a file of
            // well under 1 KB, so paying it on every suspend is free relative
            // to the 1–2 second shutdown budget.
            g_config.save();

            // Pause TTS.
            //
            // On "signal background threads to pause" (§10.1): there are none
            // to signal yet. PDF pre-render and TTS synthesis are the only two
            // threaded subsystems in the design and both are compiled to stubs
            // (ENABLE_PDF=0 / ENABLE_TTS=0). The next-chapter prefetch is NOT
            // a thread — it is cooperative work serviced from the main loop
            // between frames, so it cannot be mid-flight while suspended. When
            // either stretch goal is actually built, its pause signal belongs
            // here.
            g_tts.pause();
            break;

        case APTHOOK_ONRESTORE:
        case APTHOOK_ONWAKEUP:
            // Framebuffers refresh themselves: the page layout is still in RAM
            // and the next frame redraws from it. No disk read, no re-parse.

            // Resynchronise the gyroscope (§10.2.2 step 33).
            //
            // GyroHandler integrates angular *rate*; it has no absolute
            // reference. Across a suspend or a lid-close it receives no
            // samples at all, and its last tick timestamp is now arbitrarily
            // stale — so the first poll after waking would integrate a garbage
            // dt and could trip the flip threshold with the console never
            // having moved. resync() zeroes the integrator and restamps the
            // clock.
            //
            // Note what this deliberately does NOT do: it does not try to
            // recover the physical orientation the user is now holding the
            // console in. A rate gyro cannot answer that question, and
            // guessing would be worse than keeping the last known-good screen
            // assignment, which is what resync() preserves.
            s_gyro.resync();

            // TTS is deliberately NOT resumed (§10.2.2 step 36 — "Do NOT
            // auto-resume TTS. The user must explicitly press X to restart
            // speech."). Waking the console to sudden speech is startling,
            // and the reason the spec calls this out. This used to call
            // g_tts.resume() unconditionally.
            break;

        case APTHOOK_ONEXIT:
            // Full graceful shutdown. Guarded and shared with the post-loop
            // path — see shutdownAll().
            shutdownAll();
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
//
// `portrait` selects which projection the top-screen message is drawn under:
//   - false (default): the plain landscape scene (tilt=true) that the file
//     browser also uses. Correct for "Opening book..." — that message is
//     shown while still transitioning out of the (landscape) file browser.
//   - true: the reader's own portrait/180°-rotated projection (see
//     ReaderView::setupPortraitProjection). Required for every loading
//     message shown *during* reading — "Turning the Page...", "Refreshing
//     the Page..." — because by then the top screen is displaying reader
//     content in that rotated coordinate space. Using the landscape scene
//     for those drew the text upside down relative to the physically
//     rotated screen (reported bug: chapter-boundary page turns).
//=============================================================================
static void showLoading(const char* msg, bool portrait = false) {
    const Theme& theme = g_config.theme();

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

    C2D_TargetClear(s_topTarget, theme.background);
    if (portrait) {
        g_reader.setupPortraitProjection(s_topTarget, PORTRAIT_TOP_W, PORTRAIT_TOP_H);
        int tw = g_fontEngine.textWidth(msg, 14);
        g_fontEngine.drawText(msg, (PORTRAIT_TOP_W - tw) / 2, PORTRAIT_TOP_H / 2 - 10,
                              theme.foreground, 14);
    } else {
        C2D_SceneBegin(s_topTarget);
        g_fontEngine.drawText(msg, 12, SCREEN_TOP_H / 2 - 10, theme.foreground, 14);
    }

    C2D_TargetClear(s_botTarget, theme.background);
    if (portrait) {
        g_reader.setupPortraitProjection(s_botTarget, PORTRAIT_BOT_W, PORTRAIT_BOT_H);
    } else {
        C2D_SceneBegin(s_botTarget);
    }

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
        // Below 5%: critical — the console could cut out at any moment, so
        // this is one of the few places worth forcing the SD write through
        // rather than leaving it to the autosave.
        if (!s_openPath.empty()) g_reader.saveProgress(s_openPath);
        flushProgress();
        g_reader.setToast("Critical battery — save your place!");
    } else if (level <= 2) {
        // Below 15%: non-intrusive toast
        if (!s_openPath.empty()) g_reader.saveProgress(s_openPath);
        flushProgress();
        g_reader.setToast("Battery low — progress saved");
    }
}

//=============================================================================
// Main
//=============================================================================
int main() {
    if (!initHardware()) return 1;

    // Load persisted state FIRST. The font engine's initial face is chosen
    // from g_config.font_file, which is still the compiled-in default
    // ("built-in") until config.ini has actually been read — so loading the
    // config afterwards, as this used to, meant a saved font choice was never
    // applied at startup and the setting appeared to do nothing.
    g_config.load(CONFIG_PATH);
    g_progress.load(PROGRESS_PATH);

    // Font engine
    if (!g_fontEngine.init()) return 1;
    if (!g_config.font_file.empty() && g_config.font_file != "built-in")
        g_fontEngine.loadFont(g_config.font_file);
    // loadFont() falls back to the built-in face if the file is missing or
    // unreadable. Record what was actually loaded, so the deferred font-change
    // check in the main loop does not see a permanent mismatch and retry the
    // same failing file on every frame.
    g_config.font_file = g_fontEngine.currentFont();

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

        //--- Quit combo: L + R + B held -------------------------------------
        // Checked before anything else dispatches, and before C3D_FrameBegin,
        // so leaving the loop here is always between frames. See QUIT_COMBO.
        if ((keysHeld & QUIT_COMBO) == QUIT_COMBO) {
            if (++s_quitHoldFrames >= QUIT_HOLD_FRAMES) {
                TRACE("=== quit combo (L+R+B) held, exiting ===");
                if (!s_openPath.empty()) g_reader.saveProgress(s_openPath);
                // Commit before drawing anything. The post-loop shutdown does
                // this too, but there is no reason for the user's page to
                // depend on the exit frame rendering successfully first.
                flushProgress();
                showLoading("Exiting BookReader...",
                            /*portrait=*/s_state == AppState::Reading);
                break;
            }
        } else {
            s_quitHoldFrames = 0;
        }

        //--- Gyroscope update (30 Hz internally) ---
        bool flipped = s_gyro.update(g_config.orientation_lock);
        (void)flipped;  // handled by orientation_ in render

        //--- Battery check (every 30 s) ---
        if (++s_batteryCounter >= BATTERY_POLL_FRAMES) {
            s_batteryCounter = 0;
            if (s_state == AppState::Reading) checkBattery();
        }

        //--- Progress autosave backstop (every 30 s) ------------------------
        // The explicit flush points (closing a book, opening Settings, APT
        // suspend/exit, the quit combo) cover every ordinary way of leaving a
        // page behind. This covers the rest: a flat battery or a hard power
        // off. It costs one SD write per half minute, and only when something
        // has actually changed — saveIfDirty() returns immediately otherwise.
        if (++s_autosaveCounter >= AUTOSAVE_FRAMES) {
            s_autosaveCounter = 0;
            flushProgress();
        }

        //--- Deferred software keyboard -------------------------------------
        // Go-To ▸ Y asks for the system keyboard; the applet is launched here,
        // between frames, because it suspends the app and takes the GPU. The
        // panel reports what was typed through its next update() call, so the
        // Go-To handling in the state machine below is unchanged.
        if (g_overlay.keyboardRequested()) {
            g_overlay.serviceKeyboard();
            continue;   // start a clean frame next iteration
        }

        //--- Deferred font change -------------------------------------------
        // Settings ▸ Font File only records the chosen name (see
        // applySettingChange() in overlay.cpp). Loading the face is an SD read
        // plus a full glyph-atlas rebuild, which is the same class of work as
        // a chapter load and belongs here, between frames, not inside the open
        // C3D frame the overlay's input is handled in.
        //
        // Checked before the deferred-work block below so the relayout that
        // follows is computed with the new face, rather than being run against
        // the old one and then immediately invalidated.
        if (g_fontEngine.currentFont() != g_config.font_file) {
            showLoading("Loading font...", /*portrait=*/s_state == AppState::Reading);
            TRACE("font change: %s -> %s", g_fontEngine.currentFont().c_str(),
                  g_config.font_file.c_str());
            g_fontEngine.loadFont(g_config.font_file);
            // Sync back what actually loaded: a missing file falls back to the
            // built-in face, and without this the mismatch would persist and
            // re-trigger this block on every frame.
            g_config.font_file = g_fontEngine.currentFont();
            if (s_state == AppState::Reading) g_reader.requestRelayout();
            continue;   // start a clean frame next iteration
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

        //--- Deferred reader layout work -----------------------------------
        // Crossing a chapter boundary on a page turn, jumping to a different
        // chapter, or re-laying out after a font/margin/justify change all do
        // the same FreeType-layout-plus-glyph-atlas-GPU-flush work as opening
        // a book above, and are just as unsafe inside an open C3D frame.
        // ReaderView queues these (see hasPendingWork() in reader_view.h)
        // instead of doing them straight from update(), so service them here,
        // between frames, the same way s_pendingOpenPath is serviced.
        if (s_state == AppState::Reading && g_reader.hasPendingWork()) {
            // A chapter load (page turn crossing a boundary, go-to) is
            // genuinely "turning the page" to different content. A relayout
            // (font/margin/justify change from the menu) redraws the *same*
            // page at new settings — calling that "turning the page" was
            // confusing since nothing was actually navigated.
            bool wasChapterLoad = g_reader.pendingIsChapterLoad();
            showLoading(wasChapterLoad ? "Turning the Page..."
                                       : "Refreshing the Page...",
                        /*portrait=*/true);
            g_reader.servicePendingWork();
            g_reader.saveProgress(s_openPath);
            if (g_config.tts_enabled)
                g_tts.speak(g_reader.currentPageText(true), g_config.tts_speed);

            // Report what that actually cost, if the user has asked to see it.
            // This is the only timing channel that survives ENABLE_TRACE=0 —
            // see Config::show_timings.
            if (g_config.show_timings && wasChapterLoad) {
                const ReaderView::LoadTiming& t = g_reader.lastLoadTiming();
                char buf[80];
                if (t.prefetchHit)
                    snprintf(buf, sizeof(buf), "Chapter %.0f ms (prefetch hit)",
                             (double)t.total);
                else
                    snprintf(buf, sizeof(buf), "Chapter %.0f ms (text %.0f, layout %.0f)",
                             (double)t.total, (double)t.text, (double)t.layout);
                g_reader.setToast(buf);
            }

            s_renderTraceFrames = RENDER_TRACE_FRAMES;
            continue;   // start a clean frame next iteration
        }

        //--- Background next-chapter prefetch -------------------------------
        // Lower priority than the pending-work check above — an actual
        // navigation or relayout in flight is always serviced first. Runs
        // once per idle frame while the user is just sitting on a page (see
        // "Next-chapter prefetch" in reader_view.h). Deliberately no loading
        // screen here: the whole point is to pay this cost before the user
        // asks for it, not to make them watch it happen. That means this
        // iteration draws nothing (no C3D_FrameBegin/End at all) — the
        // screen simply holds its last frame for however long layout takes.
        // Unverified on hardware: if a long chapter makes that pause
        // noticeable, see the CAVEAT in reader_view.h.
        if (s_state == AppState::Reading && !g_reader.hasPendingWork() && g_reader.hasPrefetchWork()) {
            g_reader.servicePrefetch();
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
                    // No book is open in this state, so there is nothing to
                    // relayout, no page to go to and no bookmark list to
                    // navigate — all four out-params are collected and
                    // discarded. The empty vector / 0 passed to render() below
                    // is correct here for the same reason.
                    int  gotoPage = -1, gotoChapter = -1, gotoBookmark = -1;
                    bool closed = g_overlay.update(keysDown, touch, touchReleased,
                                                   relayout, gotoPage, gotoChapter,
                                                   gotoBookmark);
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

                // Start opens global settings.
                //
                // Y is NOT handled here: FileBrowser::update() already owns
                // the Y/sort-cycle binding. Calling cycleSortMode() again from
                // this level advanced the mode twice per press, so Y appeared
                // to skip a mode entirely (name → file-type → name, never
                // stopping on recent).
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
                    int  gotoPage = -1, gotoChapter = -1, gotoBookmark = -1;
                    bool closed = g_overlay.update(keysDown, touch, touchReleased,
                                                   relayout, gotoPage, gotoChapter,
                                                   gotoBookmark);

                    // Queue a font/margin/justify change the instant it
                    // happens, not when the panel eventually closes. `relayout`
                    // is an out-param that OverlayPanel::update() resets to
                    // false on every call, so it only ever reports a change
                    // made on *this* frame — by the time the user presses B to
                    // close the panel (a different, later frame), that signal
                    // is already gone. Queuing here means the deferred-work
                    // check above picks it up on the very next loop iteration
                    // (between frames, where the layout work it triggers is
                    // actually safe to run — see reader_view.h) instead of
                    // never running at all.
                    if (relayout) g_reader.requestRelayout();
                    if (closed) {
                        g_config.save();
                    }
                    // "Go to page" is a spread within the current chapter —
                    // never crosses a chapter boundary from here, so it's a
                    // cheap, immediate spread_ clamp, not deferred work.
                    if (gotoPage >= 0)
                        g_reader.goToPosition(g_reader.currentChapter(), gotoPage);

                    // "Go to chapter" can cross a boundary; goToPosition()
                    // queues that itself (see reader_view.h), so the deferred
                    // -work check above will pick it up next iteration.
                    if (gotoChapter >= 0)
                        g_reader.goToPosition(gotoChapter, 0);

                    // Jump to a bookmark selected in the Bookmarks tab.
                    //
                    // The overlay reports its focused row index without
                    // knowing how long the list is (render() is what receives
                    // the vector), so the range check belongs here, against
                    // the vector we own. goToPosition() defers the load itself
                    // if the bookmark lands in a different chapter, so this is
                    // safe to call from inside the open frame.
                    if (gotoBookmark >= 0 &&
                        gotoBookmark < (int)g_reader.bookmarks().size()) {
                        const Bookmark& bm = g_reader.bookmarks()[gotoBookmark];
                        g_reader.goToPosition(bm.chapter, bm.spread);
                    }

                    // Render reader in background, then overlay on top
                    float flipA = s_gyro.flipping()
                                  ? (float)s_gyro.flipFrame() / 4.0f
                                  : 0.0f;
                    g_reader.render(s_topTarget, s_botTarget, g_config.theme(),
                                    s_gyro.orientation(), flipA);
                    // Pass the real bookmark list and spread count. These were
                    // hard-coded to `{}` and `0`, which is why the Bookmarks
                    // tab permanently read "No bookmarks" regardless of what
                    // Select had actually saved.
                    g_overlay.render(s_botTarget, g_config.theme(),
                                     g_reader.bookmarks(), g_reader.totalSpreads(),
                                     g_reader.chapterCount());
                    break;
                }

                // Open overlay on Start. The reader has stopped to look at a
                // menu, so this is free time — commit any unwritten position
                // now rather than during a page turn.
                if (keysDown & KEY_START) {
                    g_reader.saveProgress(s_openPath);
                    flushProgress();
                    g_overlay.open(OverlayPanel::Tab::Settings);
                    break;
                }

                // Open Go-To panel on Y
                if (keysDown & KEY_Y) {
                    g_overlay.open(OverlayPanel::Tab::GoTo);
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

                // B: return to file browser.
                //
                // Suppressed while a shoulder button is held: B is also part
                // of the L+R+B quit chord, and without this guard pressing the
                // chord in the order L, R, B would drop the book on the B-down
                // frame and then quit from the library half a second later —
                // so the app would exit having closed the book rather than
                // saved the page the reader was on.
                if ((keysDown & KEY_B) && !(keysHeld & (KEY_L | KEY_R))) {
                    g_reader.saveProgress(s_openPath);
                    // Leaving the book is a natural commit point: flush the
                    // write-behind rather than letting the position ride on
                    // the autosave.
                    flushProgress();
                    g_tts.stop();
                    g_reader.close();
                    s_openPath.clear();
                    browser.scan();
                    s_state = AppState::FileBrowser;
                    break;
                }

                // Reader input
                bool nav = g_reader.update(keysDown, keysHeld, touch, touchDown, touchReleased, s_gyro);

                // On page turn: record progress and sync TTS.
                //
                // Frame tracing is deliberately NOT re-armed here any more. A
                // same-chapter turn is a single integer increment; arming 12
                // frames of TRACE() put a dozen fflush()ed SD writes behind
                // every page of ordinary reading, which is a real cost on this
                // hardware and was measuring the logging rather than the turn.
                // Chapter loads — the case actually worth tracing — still
                // re-arm it, in the deferred-work block above.
                if (nav) {
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
    //
    // Unhook first, so nothing can re-enter the callback while the teardown is
    // in progress, then run the one shared shutdown. If APTHOOK_ONEXIT already
    // ran it, this is a no-op rather than a second teardown.
    //=========================================================================
    aptUnhook(&s_aptHook);
    shutdownAll();

    return 0;
}
