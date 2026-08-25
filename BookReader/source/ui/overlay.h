#pragma once
#include "common.h"
#include "settings/progress.h"   // Bookmark
#include "settings/config.h"
#include "renderer/font_engine.h"
#include <string>
#include <vector>
#include <functional>

//-----------------------------------------------------------------------------
// SettingRow — one row in the settings overlay
//-----------------------------------------------------------------------------
struct SettingRow {
    std::string label;
    std::string value;        // current value as display string
    std::string minHint;      // leftmost label (e.g. "12pt")
    std::string maxHint;
};

//-----------------------------------------------------------------------------
// OverlayPanel — design doc §7
//
// Semi-transparent settings panel on the bottom screen.
// Background content (reader or file browser) is dimmed but visible through
// the panel (achieved by drawing a translucent black rectangle first).
//
// Tabs: Settings | Bookmarks | Go To | Controls
//-----------------------------------------------------------------------------
class OverlayPanel {
public:
    enum class Tab { Settings, Bookmarks, GoTo, Controls };
    static constexpr int TAB_COUNT = 4;

    bool isOpen() const { return open_; }
    void open(Tab tab = Tab::Settings);
    void close();

    // Render the panel on top of whatever is already on the bottom screen target.
    // Call after the reader has rendered its bottom-screen content.
    // `chapterCount` feeds the Go-To-Chapter hint text ("Enter chapter 1-N").
    void render(C3D_RenderTarget* botTarget, const Theme& theme,
                const std::vector<Bookmark>& bookmarks, int totalPages,
                int chapterCount = 0);

    // Input; returns true when overlay should close (B or Start pressed)
    //
    // `relayout` is set to true only if a setting that actually affects text
    // layout changed — font size, line spacing, margins, justify. See
    // applySettingChange().
    //
    // `gotoPage` (spread within the current chapter), `gotoChapter`, and
    // `gotoBookmark` (an INDEX into the bookmark vector passed to render())
    // are each left at -1 unless that specific action was confirmed this call.
    //
    // All four are reset at the top of every call, so each only ever reports
    // something that happened on THIS frame. Callers must consume them
    // immediately rather than waiting for the panel to close — see the
    // `relayout` handling in main.cpp for the bug that caused.
    bool update(u32 keysDown, const touchPosition& touch, bool touchReleased,
                bool& relayout, int& gotoPage, int& gotoChapter,
                int& gotoBookmark);

private:
    bool open_    = false;
    Tab  tab_     = Tab::Settings;
    int  focusRow_ = 0;

    /// Index of the first settings row drawn. The panel only has room for
    /// SETTINGS_VISIBLE_ROWS at a time, and renderSettings() used to draw from
    /// row 0 and stop when it ran out of space — so every row past the sixth
    /// was unreachable: the focus highlight moved down onto rows that were
    /// never drawn. Kept in step with focusRow_ by update().
    int  scrollTop_ = 0;

    // Settings rows
    void buildSettingRows();

    /// Apply a +1/-1 nudge to the focused setting.
    ///
    /// Returns true ONLY if the setting that changed actually affects text
    /// layout (font size, line spacing, margins, justify). A relayout is the
    /// single most expensive operation in the reader — a full FreeType pass
    /// over the chapter — so it must not be triggered by settings that merely
    /// change colours, brightness or audio. update() previously raised its
    /// `relayout` out-param on every value change regardless, which meant
    /// nudging brightness cost a multi-second stall.
    bool applySettingChange(int row, int delta);

    std::vector<SettingRow> rows_;

    // Bookmark tab
    int  bkmFocus_ = 0;

    // Go-To tab. X toggles between the two sub-modes.
    enum class GotoMode { Page, Chapter };
    GotoMode    gotoMode_ = GotoMode::Page;
    std::string gotoBuf_;
    int         lastChapterCount_ = 0;   // stashed from render() for the hint text

    void renderSettings(C3D_RenderTarget* target, const Theme& theme);
    void renderBookmarks(C3D_RenderTarget* target, const Theme& theme,
                         const std::vector<Bookmark>& bookmarks, int totalPages);
    void renderGoTo(C3D_RenderTarget* target, const Theme& theme);
    void renderControls(C3D_RenderTarget* target, const Theme& theme);

    static constexpr int ROW_H     = 28;
    static constexpr int PANEL_Y   = 10;
    static constexpr int PANEL_H   = 220;
    static constexpr int TAB_W     = 78;   // TAB_COUNT * TAB_W = 312 = panel width

    // Settings rows start at y=28 and the footer hint sits at y=225, so
    // (220 - 28) / 28 rows fit on screen at once.
    static constexpr int SETTINGS_ROW_Y0        = 28;
    static constexpr int SETTINGS_VISIBLE_ROWS  = 6;
};

extern OverlayPanel g_overlay;
