#pragma once
#include "common.h"
#include <string>
#include <vector>

//-----------------------------------------------------------------------------
// FileEntry — one item in the file browser list
//-----------------------------------------------------------------------------
struct FileEntry {
    std::string name;       // display name (filename)
    std::string fullPath;   // absolute path on SD card
    DocType     type;
    float       progress;   // 0.0–1.0 reading progress (from progress.json), -1 = none
};

//-----------------------------------------------------------------------------
// SortMode — design doc §5.1
//-----------------------------------------------------------------------------
enum class SortMode { Name, Recent, FileType };

//-----------------------------------------------------------------------------
// FileBrowser — design doc §5
//
// Displayed on startup and on return from reading. Device held in landscape.
// Touch: tap to select, long-press for context menu.
// D-Pad / Circle Pad: move selection; A or touch to open.
//
// Returns the path of the file the user selected, or "" if none yet.
//-----------------------------------------------------------------------------
class FileBrowser {
public:
    // Scan the books directory and build the list
    void scan();

    // Re-sort without re-scanning
    void sort(SortMode mode);
    void cycleSortMode();

    // Render the file list to the given screen targets
    void render(C3D_RenderTarget* top, C3D_RenderTarget* bot, const Theme& theme);

    // Process input. Returns selected file path or "" if nothing selected yet.
    // `longPressPath` is set if the user long-pressed an entry (for context menu).
    std::string update(u32 keysDown, u32 keysHeld,
                       const touchPosition& touch, bool touchDown, bool touchReleased,
                       std::string* longPressPath = nullptr);

    // Context menu result
    enum class ContextAction { None, Open, DeleteProgress, Info };
    ContextAction contextAction() const { return ctxAction_; }
    void          clearContextAction()  { ctxAction_ = ContextAction::None; }

    const std::vector<FileEntry>& entries() const { return entries_; }

private:
    std::vector<FileEntry> entries_;
    int     selected_    = 0;
    int     scrollOffset_ = 0;
    SortMode sortMode_   = SortMode::Name;

    bool    ctxMenuOpen_ = false;
    int     ctxEntry_    = -1;
    int     ctxSel_      = 0;
    ContextAction ctxAction_ = ContextAction::None;

    // Touch state
    int     touchStartY_    = 0;
    bool    touchHolding_   = false;
    int     holdFrames_     = 0;

    static constexpr int ROW_H    = 32;   // pixels per list row (landscape)
    static constexpr int VISIBLE_ROWS = 6; // rows visible on bottom screen (240/32 - margin)
    static constexpr int LONG_PRESS_FRAMES = 30;

    void renderList(C3D_RenderTarget* bot, const Theme& theme);
    void renderTopInfo(C3D_RenderTarget* top, const Theme& theme);
    void renderContextMenu(C3D_RenderTarget* bot, const Theme& theme);
    void ensureVisible();
};
