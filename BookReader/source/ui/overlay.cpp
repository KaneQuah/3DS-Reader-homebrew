#include "ui/overlay.h"
#include "settings/config.h"
#include "renderer/font_engine.h"
#include <algorithm>
#include <cstdio>

OverlayPanel g_overlay;

void OverlayPanel::open(Tab tab) {
    open_   = true;
    tab_    = tab;
    focusRow_ = 0;
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
    addRow("TTS",          g_config.tts_enabled ? "On" : "Off");
    char ttsBuf[16]; snprintf(ttsBuf, sizeof(ttsBuf), "%d WPM", g_config.tts_speed);
    addRow("TTS Speed",    ttsBuf, "80", "300");
    addRow("Font File",    g_config.font_file.c_str());
}

void OverlayPanel::applySettingChange(int row, int delta) {
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
        case 8: g_config.tts_enabled = !g_config.tts_enabled; break;
        case 9: // TTS speed
            g_config.tts_speed = std::max(80, std::min(300, g_config.tts_speed + delta * 10));
            break;
        default: break;
    }
    (void)relayout_flag;
    buildSettingRows();
    g_config.save();
}

//-----------------------------------------------------------------------------
// Render
//-----------------------------------------------------------------------------
void OverlayPanel::render(C3D_RenderTarget* botTarget, const Theme& theme,
                           const std::vector<Bookmark>& bookmarks, int totalPages) {
    if (!open_) return;
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

    // Tab bar
    const char* tabs[] = { "Settings", "Bookmarks", "Search" };
    for (int i = 0; i < 3; i++) {
        u32 bg = (tab_ == (Tab)i) ? theme.accent : C2D_Color32(60,60,60,255);
        C2D_DrawRectSolid(4 + i * 104, 4, 1.0f, 102, 22, bg);
        g_fontEngine.drawText(tabs[i], 8 + i * 104, 8,
                              C2D_Color32(255,255,255,255), 11);
    }

    switch (tab_) {
        case Tab::Settings:  renderSettings(botTarget, theme); break;
        case Tab::Bookmarks: renderBookmarks(botTarget, theme, bookmarks, totalPages); break;
        case Tab::Search:    renderSearchPanel(botTarget, theme); break;
    }

    // Footer hint
    g_fontEngine.drawText("[B/Start]=Close  [A/LR]=Change  [DPad]=Navigate",
                          6, 225, C2D_Color32(180,180,180,255), 9);
}

void OverlayPanel::renderSettings(C3D_RenderTarget* target, const Theme& theme) {
    for (int i = 0; i < (int)rows_.size(); i++) {
        bool sel = (i == focusRow_);
        u32 bg = sel ? C2D_Color32(60,80,120,255) : C2D_Color32(0,0,0,0);
        C2D_DrawRectSolid(6, 28 + i * ROW_H, 0.95f, 308, ROW_H - 2, bg);

        u32 fg = C2D_Color32(220,220,220,255);
        g_fontEngine.drawText(rows_[i].label.c_str(), 10, 31 + i * ROW_H, fg, 11);

        int vw = g_fontEngine.textWidth(rows_[i].value, 11);
        g_fontEngine.drawText(rows_[i].value.c_str(), 310 - vw, 31 + i * ROW_H,
                              theme.accent, 11);
        if (i * ROW_H + ROW_H + 28 > 220) break;
    }
}

void OverlayPanel::renderBookmarks(C3D_RenderTarget* target, const Theme& theme,
                                    const std::vector<Bookmark>& bookmarks, int totalPages) {
    if (bookmarks.empty()) {
        g_fontEngine.drawText("No bookmarks", 40, 120, C2D_Color32(150,150,150,255), 13);
        return;
    }
    for (int i = 0; i < (int)bookmarks.size(); i++) {
        bool sel = (i == bkmFocus_);
        u32 bg = sel ? C2D_Color32(60,80,120,255) : C2D_Color32(0,0,0,0);
        C2D_DrawRectSolid(6, 30 + i * 28, 0.95f, 308, 26, bg);
        char buf[48];
        snprintf(buf, sizeof(buf), "Chapter %d  -  page %d",
                 bookmarks[i].chapter + 1, bookmarks[i].spread * 2 + 1);
        g_fontEngine.drawText(buf, 10, 34 + i * 28, C2D_Color32(220,220,220,255), 12);
        if ((i + 1) * 28 + 30 > 220) break;
    }
}

void OverlayPanel::renderSearchPanel(C3D_RenderTarget* target, const Theme& theme) {
    // Tab toggle: Search / Go To Page
    g_fontEngine.drawText(gotoMode_ ? "[Search]  [Go-To ●]" : "[Search ●]  [Go-To]",
                          10, 32, C2D_Color32(200,200,200,255), 11);

    if (!gotoMode_) {
        g_fontEngine.drawText("Query: (press Y for keyboard)", 10, 60,
                              C2D_Color32(180,180,180,255), 11);
        g_fontEngine.drawText(searchBuf_.c_str(), 10, 80, theme.accent, 13);
    } else {
        g_fontEngine.drawText("Enter page number:", 10, 60,
                              C2D_Color32(180,180,180,255), 11);
        g_fontEngine.drawText(searchBuf_.c_str(), 10, 80, theme.accent, 13);
    }
    g_fontEngine.drawText("[Y]=Keyboard  [A]=Confirm  [X]=Toggle mode",
                          6, 110, C2D_Color32(150,150,150,255), 10);
}

//-----------------------------------------------------------------------------
// Update (input)
//-----------------------------------------------------------------------------
bool OverlayPanel::update(u32 keysDown, const touchPosition& touch, bool touchReleased,
                           bool& relayout, int& gotoPage, std::string& searchQuery) {
    if (!open_) return false;
    relayout    = false;
    gotoPage    = -1;
    searchQuery = "";

    // B or Start: close
    if ((keysDown & KEY_B) || (keysDown & KEY_START)) { close(); return true; }

    // Tab switching via L/R
    if (keysDown & KEY_L) tab_ = (Tab)(((int)tab_ + 2) % 3);
    if (keysDown & KEY_R) tab_ = (Tab)(((int)tab_ + 1) % 3);

    // Tab switching via touch on tab bar
    if (touchReleased && touch.py < 26) {
        int tabIdx = touch.px / 104;
        if (tabIdx >= 0 && tabIdx < 3) tab_ = (Tab)tabIdx;
    }

    if (tab_ == Tab::Settings) {
        int rows = (int)rows_.size();
        if (keysDown & KEY_DUP)   focusRow_ = std::max(0, focusRow_ - 1);
        if (keysDown & KEY_DDOWN) focusRow_ = std::min(rows - 1, focusRow_ + 1);
        if (keysDown & KEY_DLEFT)  { applySettingChange(focusRow_, -1); relayout = true; }
        if (keysDown & KEY_DRIGHT) { applySettingChange(focusRow_, +1); relayout = true; }
        if (keysDown & KEY_A)      { applySettingChange(focusRow_, +1); relayout = true; }

        // Touch on setting row
        if (touchReleased && touch.py > 28) {
            int row = (touch.py - 28) / ROW_H;
            if (row >= 0 && row < rows) focusRow_ = row;
        }
    } else if (tab_ == Tab::Bookmarks) {
        if (keysDown & KEY_DUP)   bkmFocus_ = std::max(0, bkmFocus_ - 1);
        if (keysDown & KEY_DDOWN) bkmFocus_++;
        // A: jump to bookmark — caller handles
    } else if (tab_ == Tab::Search) {
        if (keysDown & KEY_X) gotoMode_ = !gotoMode_;  // toggle search/goto

        if (keysDown & KEY_Y) {
            // Invoke software keyboard
            SwkbdState swkbd;
            if (gotoMode_) {
                swkbdInit(&swkbd, SWKBD_TYPE_NUMPAD, 2, 6);
                swkbdSetHintText(&swkbd, "Page number");
            } else {
                swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, 64);
                swkbdSetHintText(&swkbd, "Search query");
            }
            char kbuf[128] = {};
            SwkbdButton btn = swkbdInputText(&swkbd, kbuf, sizeof(kbuf));
            if (btn == SWKBD_BUTTON_CONFIRM) {
                searchBuf_ = kbuf;
                if (gotoMode_) {
                    gotoPage = std::atoi(kbuf) - 1;  // convert to 0-indexed
                } else {
                    searchQuery = kbuf;
                }
            }
        }

        if (keysDown & KEY_A) {
            if (gotoMode_ && !searchBuf_.empty())
                gotoPage = std::atoi(searchBuf_.c_str()) - 1;
            else if (!searchBuf_.empty())
                searchQuery = searchBuf_;
        }
    }
    return false;
}
