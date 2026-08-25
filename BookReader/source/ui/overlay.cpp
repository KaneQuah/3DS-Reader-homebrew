#include "ui/overlay.h"
#include "settings/config.h"
#include "renderer/font_engine.h"
#include <algorithm>
#include <cstdio>

OverlayPanel g_overlay;

void OverlayPanel::open(Tab tab) {
    open_   = true;
    tab_    = tab;
    focusRow_  = 0;
    scrollTop_ = 0;
    buildSettingRows();
}

void OverlayPanel::close() {
    open_ = false;
}

//-----------------------------------------------------------------------------
// Build settings rows from current Config
//-----------------------------------------------------------------------------
void OverlayPanel::buildSettingRows() {
    rows_.clear();

    auto addRow = [&](const char* label, const char* val, const char* mn="", const char* mx="") {
        rows_.push_back({ label, val, mn, mx });
    };

    addRow("Theme",        g_config.theme_name.c_str(), "Light", "Sepia");
    char szBuf[16]; snprintf(szBuf, sizeof(szBuf), "%d pt", g_config.font_size);
    addRow("Font Size",    szBuf, "12pt", "28pt");
    addRow("Line Spacing", g_config.line_spacing.c_str(), "Compact", "Relaxed");
    addRow("Margins",      g_config.margins.c_str(), "Narrow", "Wide");
    addRow("Justify",      g_config.justify ? "On" : "Off");
    addRow("Orientation",  g_config.orientation_lock ? "Locked" : "Unlocked");
    char brightBuf[8]; snprintf(brightBuf, sizeof(brightBuf), "%d/5", g_config.bright_level);
    addRow("Brightness",   brightBuf, "1", "5");
    char scrollBuf[16];
    if (g_config.auto_scroll_speed == 0) snprintf(scrollBuf, sizeof(scrollBuf), "Off");
    else snprintf(scrollBuf, sizeof(scrollBuf), "%d/min", g_config.auto_scroll_speed);
    addRow("Auto-Scroll",  scrollBuf, "Off", "10/min");
    // Diagnostic, not a reading preference — see Config::show_timings. It sits
    // here rather than in a debug build so page-turn cost can be measured on a
    // normal ENABLE_TRACE=0 build, which is the only kind whose timings are
    // worth anything.
    addRow("Show Timings", g_config.show_timings ? "On" : "Off");
    addRow("TTS",          g_config.tts_enabled ? "On" : "Off");
    char ttsBuf[16]; snprintf(ttsBuf, sizeof(ttsBuf), "%d WPM", g_config.tts_speed);
    addRow("TTS Speed",    ttsBuf, "80", "300");
    addRow("Font File",    g_config.font_file.c_str());
}

bool OverlayPanel::applySettingChange(int row, int delta) {
    // See the declaration in overlay.h: the return value is "does this change
    // invalidate the current text layout?", and only the four layout-affecting
    // settings answer yes.
    bool relayout_flag = false;
    switch (row) {
        case 0: { // Theme
            static const char* themes[] = { "light", "dark", "sepia" };
            int idx = 0;
            for (int i=0;i<3;i++) if (g_config.theme_name==themes[i]) idx=i;
            idx = (idx + delta + 3) % 3;
            g_config.theme_name = themes[idx];
            break;
        }
        case 1: // Font size
            g_config.font_size = std::max(12, std::min(28, g_config.font_size + delta * 2));
            relayout_flag = true;
            break;
        case 2: { // Line spacing
            static const char* ls[] = { "compact", "normal", "relaxed" };
            int idx = 0;
            for (int i=0;i<3;i++) if (g_config.line_spacing==ls[i]) idx=i;
            idx = std::max(0, std::min(2, idx + delta));
            g_config.line_spacing = ls[idx];
            relayout_flag = true;
            break;
        }
        case 3: { // Margins
            static const char* mg[] = { "narrow", "normal", "wide" };
            int idx = 0;
            for (int i=0;i<3;i++) if (g_config.margins==mg[i]) idx=i;
            idx = std::max(0, std::min(2, idx + delta));
            g_config.margins = mg[idx];
            relayout_flag = true;
            break;
        }
        case 4: g_config.justify = !g_config.justify; relayout_flag = true; break;
        case 5: g_config.orientation_lock = !g_config.orientation_lock; break;
        case 6: // Brightness
            g_config.bright_level = std::max(1, std::min(5, g_config.bright_level + delta));
            GSPLCD_SetBrightness(GSPLCD_SCREEN_BOTH, g_config.bright_level);
            break;
        case 7: // Auto-scroll
            g_config.auto_scroll_speed = std::max(0, std::min(10, g_config.auto_scroll_speed + delta));
            break;
        case 8: g_config.show_timings = !g_config.show_timings; break;
        case 9: g_config.tts_enabled  = !g_config.tts_enabled;  break;
        case 10: // TTS speed
            g_config.tts_speed = std::max(80, std::min(300, g_config.tts_speed + delta * 10));
            break;
        default: break;
    }
    buildSettingRows();
    // No g_config.save() here. Every D-Pad nudge wrote config.ini to the SD
    // card — an fopen/fprintf/fclose round trip per keypress while the user
    // scrubbed through font sizes. main.cpp saves when the panel closes, and
    // the APT suspend hook saves if the console is put to sleep with the panel
    // still open, so nothing is lost by dropping the per-change write.
    return relayout_flag;
}

//-----------------------------------------------------------------------------
// Render
//-----------------------------------------------------------------------------
void OverlayPanel::render(C3D_RenderTarget* botTarget, const Theme& theme,
                           const std::vector<Bookmark>& bookmarks, int totalPages,
                           int chapterCount) {
    if (!open_) return;
    lastChapterCount_ = chapterCount;   // renderGoTo() needs this for the hint text
    C2D_SceneBegin(botTarget);

    // The reader rotates the scene 180° for portrait reading and citro2d's
    // model matrix is global state that survives C2D_SceneBegin. Without this
    // reset the whole panel is drawn rotated about the page centre and lands
    // off-screen — the menu opens, but nothing inside it is visible.
    C2D_ViewReset();

    // Dim background
    C2D_DrawRectSolid(0, 0, 0.8f, 320, 240, C2D_Color32(0,0,0,180));

    // Panel background
    C2D_DrawRectSolid(4, 4, 0.9f, 312, 232, C2D_Color32(30,30,30,230));

    // Tab bar — width divides evenly for however many tabs there are, so
    // adding Controls didn't need touching anything but this constant.
    const char* tabs[] = { "Settings", "Bookmarks", "Go To", "Controls" };
    for (int i = 0; i < TAB_COUNT; i++) {
        u32 bg = (tab_ == (Tab)i) ? theme.accent : C2D_Color32(60,60,60,255);
        C2D_DrawRectSolid(4 + i * TAB_W, 4, 1.0f, TAB_W - 2, 22, bg);
        g_fontEngine.drawText(tabs[i], 8 + i * TAB_W, 8,
                              C2D_Color32(255,255,255,255), 10, 1.0f);
    }

    switch (tab_) {
        case Tab::Settings:  renderSettings(botTarget, theme); break;
        case Tab::Bookmarks: renderBookmarks(botTarget, theme, bookmarks, totalPages); break;
        case Tab::GoTo:      renderGoTo(botTarget, theme); break;
        case Tab::Controls:  renderControls(botTarget, theme); break;
    }

    // Footer hint. L/R switch tabs and Up/Down move the row focus; the actual
    // value change is D-Pad/Circle Pad Left/Right (or A to bump it up by one).
    g_fontEngine.drawText("[Up/Down]=Row  [Left/Right]=Change  [B/Start]=Close",
                          6, 225, C2D_Color32(180,180,180,255), 9, 1.0f);
}

void OverlayPanel::renderSettings(C3D_RenderTarget* target, const Theme& theme) {
    const int total = (int)rows_.size();
    const int last  = std::min(total, scrollTop_ + SETTINGS_VISIBLE_ROWS);

    for (int i = scrollTop_; i < last; i++) {
        const int slot = i - scrollTop_;
        const int y    = SETTINGS_ROW_Y0 + slot * ROW_H;

        bool sel = (i == focusRow_);
        u32 bg = sel ? C2D_Color32(60,80,120,255) : C2D_Color32(0,0,0,0);
        C2D_DrawRectSolid(6, y, 0.95f, 308, ROW_H - 2, bg);

        u32 fg = C2D_Color32(220,220,220,255);
        g_fontEngine.drawText(rows_[i].label.c_str(), 10, y + 3, fg, 11, 1.0f);

        int vw = g_fontEngine.textWidth(rows_[i].value, 11);
        g_fontEngine.drawText(rows_[i].value.c_str(), 310 - vw, y + 3,
                              theme.accent, 11, 1.0f);
    }

    // Scroll indicators, so it is obvious there is more list off-screen.
    u32 hint = C2D_Color32(150,150,150,255);
    if (scrollTop_ > 0)
        g_fontEngine.drawText("^", 300, SETTINGS_ROW_Y0 - 1, hint, 10, 1.0f);
    if (last < total)
        g_fontEngine.drawText("v", 300, SETTINGS_ROW_Y0 + SETTINGS_VISIBLE_ROWS * ROW_H - 12,
                              hint, 10, 1.0f);
}

void OverlayPanel::renderBookmarks(C3D_RenderTarget* target, const Theme& theme,
                                    const std::vector<Bookmark>& bookmarks, int totalPages) {
    if (bookmarks.empty()) {
        g_fontEngine.drawText("No bookmarks", 40, 120, C2D_Color32(150,150,150,255), 13, 1.0f);
        return;
    }
    for (int i = 0; i < (int)bookmarks.size(); i++) {
        bool sel = (i == bkmFocus_);
        u32 bg = sel ? C2D_Color32(60,80,120,255) : C2D_Color32(0,0,0,0);
        C2D_DrawRectSolid(6, 30 + i * 28, 0.95f, 308, 26, bg);
        char buf[48];
        snprintf(buf, sizeof(buf), "Chapter %d  -  page %d",
                 bookmarks[i].chapter + 1, bookmarks[i].spread * 2 + 1);
        g_fontEngine.drawText(buf, 10, 34 + i * 28, C2D_Color32(220,220,220,255), 12, 1.0f);
        if ((i + 1) * 28 + 30 > 220) break;
    }
}

void OverlayPanel::renderGoTo(C3D_RenderTarget* target, const Theme& theme) {
    // Mode toggle: Go To Page / Go To Chapter. X cycles between the two;
    // the bullet marks which one is active.
    const char* label0 = (gotoMode_ == GotoMode::Page)    ? "[Go-To Page *]"    : "[Go-To Page]";
    const char* label1 = (gotoMode_ == GotoMode::Chapter) ? "[Go-To Chapter *]" : "[Go-To Chapter]";
    char modeLine[64];
    snprintf(modeLine, sizeof(modeLine), "%s %s", label0, label1);
    g_fontEngine.drawText(modeLine, 10, 32, C2D_Color32(200,200,200,255), 10, 1.0f);

    if (gotoMode_ == GotoMode::Page) {
        g_fontEngine.drawText("Enter page number (current chapter):", 10, 60,
                              C2D_Color32(180,180,180,255), 11, 1.0f);
        g_fontEngine.drawText(gotoBuf_.c_str(), 10, 80, theme.accent, 13, 1.0f);
    } else {
        char hint[48];
        snprintf(hint, sizeof(hint), "Enter chapter (1-%d):",
                 lastChapterCount_ > 0 ? lastChapterCount_ : 1);
        g_fontEngine.drawText(hint, 10, 60,
                              C2D_Color32(180,180,180,255), 11, 1.0f);
        g_fontEngine.drawText(gotoBuf_.c_str(), 10, 80, theme.accent, 13, 1.0f);
    }
    g_fontEngine.drawText("[Y]=Keyboard  [A]=Confirm  [X]=Switch mode",
                          6, 110, C2D_Color32(150,150,150,255), 10, 1.0f);
}

//-----------------------------------------------------------------------------
// Controls / Help reference — static text, no interaction beyond viewing.
// Section headers are marked with a leading '\x01' sentinel and drawn in the
// theme accent colour; everything else is a plain binding line. Kept to one
// screen (no scrolling) — trimmed to fit PANEL_H at 12px line spacing.
//-----------------------------------------------------------------------------
void OverlayPanel::renderControls(C3D_RenderTarget* target, const Theme& theme) {
    static const char* lines[] = {
        "\x01READING",
        "Up / Down      Prev / next page",
        "Select         Toggle bookmark",
        "Start          Open / close this menu",
        "B              Return to Library",
        "X              Toggle Text-to-Speech",
        "Y              Open Go-To panel",
        "Touch swipe    Turn page",
        "\x01LIBRARY",
        "A / Touch      Open selected book",
        "Y              Cycle sort order",
        "Start          Open Settings",
        "\x01MENU / GLOBAL",
        "L / R          Switch tab",
        "Hold L+R+B     Quit BookReader",
        nullptr
    };

    int y = 30;
    for (int i = 0; lines[i]; i++) {
        const char* s = lines[i];
        if (s[0] == '\x01') {
            g_fontEngine.drawText(s + 1, 10, y, theme.accent, 10, 1.0f);
        } else {
            g_fontEngine.drawText(s, 10, y, C2D_Color32(210,210,210,255), 9, 1.0f);
        }
        y += 13;
        if (y > 216) break;
    }
}

//-----------------------------------------------------------------------------
// Update (input)
//-----------------------------------------------------------------------------
bool OverlayPanel::update(u32 keysDown, const touchPosition& touch, bool touchReleased,
                           bool& relayout, int& gotoPage, int& gotoChapter,
                           int& gotoBookmark) {
    if (!open_) return false;
    relayout     = false;
    gotoPage     = -1;
    gotoChapter  = -1;
    gotoBookmark = -1;

    // B or Start: close
    if ((keysDown & KEY_B) || (keysDown & KEY_START)) { close(); return true; }

    // Tab switching via L/R
    if (keysDown & KEY_L) tab_ = (Tab)(((int)tab_ + TAB_COUNT - 1) % TAB_COUNT);
    if (keysDown & KEY_R) tab_ = (Tab)(((int)tab_ + 1) % TAB_COUNT);

    // Tab switching via touch on tab bar
    if (touchReleased && touch.py < 26) {
        int tabIdx = touch.px / TAB_W;
        if (tabIdx >= 0 && tabIdx < TAB_COUNT) tab_ = (Tab)tabIdx;
    }

    if (tab_ == Tab::Settings) {
        int rows = (int)rows_.size();
        // Circle Pad mirrors the D-Pad here (KEY_CPAD_* are libctru's
        // discretised circle-pad-as-dpad flags) so scrubbing font size etc.
        // doesn't require letting go of the D-Pad grip.
        if ((keysDown & KEY_DUP)    || (keysDown & KEY_CPAD_UP))    focusRow_ = std::max(0, focusRow_ - 1);
        if ((keysDown & KEY_DDOWN)  || (keysDown & KEY_CPAD_DOWN))  focusRow_ = std::min(rows - 1, focusRow_ + 1);

        // Keep the focused row inside the visible window. Without this the
        // highlight walks off the bottom of the panel and the rows below it
        // (TTS, TTS Speed, Font File, Show Timings) can be focused and changed
        // but never seen.
        if (focusRow_ < scrollTop_) scrollTop_ = focusRow_;
        if (focusRow_ > scrollTop_ + SETTINGS_VISIBLE_ROWS - 1)
            scrollTop_ = focusRow_ - SETTINGS_VISIBLE_ROWS + 1;
        scrollTop_ = std::max(0, std::min(scrollTop_,
                                          std::max(0, rows - SETTINGS_VISIBLE_ROWS)));
        // Only raise `relayout` when the setting that changed actually
        // invalidates the layout. Raising it unconditionally meant a
        // brightness nudge or a theme change paid a full FreeType re-layout
        // of the chapter, complete with a "Refreshing the Page..." screen.
        if ((keysDown & KEY_DLEFT)  || (keysDown & KEY_CPAD_LEFT))  { if (applySettingChange(focusRow_, -1)) relayout = true; }
        if ((keysDown & KEY_DRIGHT) || (keysDown & KEY_CPAD_RIGHT)) { if (applySettingChange(focusRow_, +1)) relayout = true; }
        if (keysDown & KEY_A)      { if (applySettingChange(focusRow_, +1)) relayout = true; }

        // Touch on setting row. touch.py addresses a slot on screen, so the
        // scroll offset has to be added back to get the row it names.
        if (touchReleased && touch.py > SETTINGS_ROW_Y0) {
            int slot = (touch.py - SETTINGS_ROW_Y0) / ROW_H;
            int row  = scrollTop_ + slot;
            if (slot >= 0 && slot < SETTINGS_VISIBLE_ROWS && row < rows)
                focusRow_ = row;
        }
    } else if (tab_ == Tab::Bookmarks) {
        if ((keysDown & KEY_DUP)   || (keysDown & KEY_CPAD_UP))   bkmFocus_ = std::max(0, bkmFocus_ - 1);
        if ((keysDown & KEY_DDOWN) || (keysDown & KEY_CPAD_DOWN)) bkmFocus_++;
        if (bkmFocus_ < 0) bkmFocus_ = 0;

        // A: jump to the focused bookmark. The panel does not know how many
        // bookmarks exist at input time — render() is what receives the list —
        // so bkmFocus_ is reported as-is and the caller range-checks it
        // against the vector it owns before acting.
        if (keysDown & KEY_A) gotoBookmark = bkmFocus_;
    } else if (tab_ == Tab::GoTo) {
        // X cycles Go-To-Page <-> Go-To-Chapter. Switching mode clears the
        // buffer so a leftover chapter number can't be confirmed as a page
        // (or vice versa) by mistake.
        if (keysDown & KEY_X) {
            gotoMode_ = (gotoMode_ == GotoMode::Page) ? GotoMode::Chapter : GotoMode::Page;
            gotoBuf_.clear();
        }

        if (keysDown & KEY_Y) {
            // Invoke software keyboard
            SwkbdState swkbd;
            swkbdInit(&swkbd, SWKBD_TYPE_NUMPAD, 2, 6);
            swkbdSetHintText(&swkbd, gotoMode_ == GotoMode::Page ? "Page number" : "Chapter number");
            char kbuf[128] = {};
            SwkbdButton btn = swkbdInputText(&swkbd, kbuf, sizeof(kbuf));
            if (btn == SWKBD_BUTTON_CONFIRM) {
                gotoBuf_ = kbuf;
                if (gotoMode_ == GotoMode::Page) gotoPage    = std::atoi(kbuf) - 1;
                else                             gotoChapter = std::atoi(kbuf) - 1;
            }
        }

        if ((keysDown & KEY_A) && !gotoBuf_.empty()) {
            if (gotoMode_ == GotoMode::Page) gotoPage    = std::atoi(gotoBuf_.c_str()) - 1;
            else                             gotoChapter = std::atoi(gotoBuf_.c_str()) - 1;
        }
    }
    return false;
}
