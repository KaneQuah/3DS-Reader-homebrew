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
// Tabs: Settings | Bookmarks | Search/Go-To-Page
//-----------------------------------------------------------------------------
class OverlayPanel {
public:
    enum class Tab { Settings, Bookmarks, Search };

    bool isOpen() const { return open_; }
    void open(Tab tab = Tab::Settings);
    void close();

    // Render the panel on top of whatever is already on the bottom screen target.
    // Call after the reader has rendered its bottom-screen content.
    void render(C3D_RenderTarget* botTarget, const Theme& theme,
                const std::vector<Bookmark>& bookmarks, int totalPages);

    // Input; returns true when overlay should close (B or Start pressed)
    // `relayout` is set to true if font/size/margin settings changed
    bool update(u32 keysDown, const touchPosition& touch, bool touchReleased,
                bool& relayout, int& gotoPage, std::string& searchQuery);

private:
    bool open_    = false;
    Tab  tab_     = Tab::Settings;
    int  focusRow_ = 0;

    // Settings rows
    void buildSettingRows();
    void applySettingChange(int row, int delta);
    std::vector<SettingRow> rows_;

    // Bookmark tab
    int  bkmFocus_ = 0;

    // Search / go-to-page tab
    bool gotoMode_    = false;  // false = search, true = go-to-page
    std::string searchBuf_;

    void renderSettings(C3D_RenderTarget* target, const Theme& theme);
    void renderBookmarks(C3D_RenderTarget* target, const Theme& theme,
                         const std::vector<Bookmark>& bookmarks, int totalPages);
    void renderSearchPanel(C3D_RenderTarget* target, const Theme& theme);

    static constexpr int ROW_H     = 28;
    static constexpr int PANEL_Y   = 10;
    static constexpr int PANEL_H   = 220;
};

extern OverlayPanel g_overlay;
