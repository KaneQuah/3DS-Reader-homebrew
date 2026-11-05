#include "ui/file_browser.h"
#include "settings/config.h"
#include "settings/progress.h"
#include "renderer/font_engine.h"
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <cstring>
#include <cstdio>

//-----------------------------------------------------------------------------
// Scan /3DS/BookReader/books/ recursively for supported file types
//-----------------------------------------------------------------------------
static void scanDir(const std::string& dir, std::vector<FileEntry>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        if (de->d_name[0] == '.') continue;
        std::string name = de->d_name;
        std::string full = dir + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            scanDir(full, out);   // recurse
        } else {
            DocType t = docTypeFromPath(name);
            if (t == DocType::Unknown) continue;

            FileEntry fe;
            fe.name     = name;
            fe.fullPath = full;
            fe.type     = t;
            fe.progress = -1.0f;

            // Reading progress, approximated as chapters completed. Exact page
            // counts would require opening and laying out every book in the
            // directory, which is far too slow for a directory listing.
            const BookProgress* bp = g_progress.get(full);
            if (bp && bp->chapter_count > 0) {
                fe.progress = (float)bp->chapter / (float)bp->chapter_count;
                if (fe.progress > 1.0f) fe.progress = 1.0f;
            }
            out.push_back(std::move(fe));
        }
    }
    closedir(d);
}

void FileBrowser::scan() {
    entries_.clear();
    scanDir(BOOKS_PATH, entries_);
    sort(sortMode_);
    selected_    = 0;
    scrollOffset_ = 0;
}

void FileBrowser::sort(SortMode mode) {
    sortMode_ = mode;
    if (mode == SortMode::Name) {
        std::sort(entries_.begin(), entries_.end(), [](const FileEntry& a, const FileEntry& b) {
            return a.name < b.name;
        });
    } else if (mode == SortMode::FileType) {
        std::sort(entries_.begin(), entries_.end(), [](const FileEntry& a, const FileEntry& b) {
            return (int)a.type < (int)b.type || (a.type == b.type && a.name < b.name);
        });
    }
    // Recent: would require mtime — skipped for now (entries_ keeps insertion order)
}

void FileBrowser::cycleSortMode() {
    int next = ((int)sortMode_ + 1) % 3;
    sort((SortMode)next);
}

//-----------------------------------------------------------------------------
// Render
//-----------------------------------------------------------------------------
void FileBrowser::render(C3D_RenderTarget* top, C3D_RenderTarget* bot, const Theme& theme) {
    // The reader leaves citro2d's model matrix rotated 180° for portrait
    // reading, and that state is global — it survives across scenes and
    // frames. Reset it here so returning from a book doesn't draw the browser
    // upside down and off-screen.
    C2D_ViewReset();
    renderTopInfo(top, theme);
    if (ctxMenuOpen_)
        renderContextMenu(bot, theme);
    else
        renderList(bot, theme);
}

static const char* typeLabel(DocType t) {
    switch (t) {
        case DocType::EPUB: return "[EPUB]";
        case DocType::TXT:  return "[TXT] ";
        case DocType::PDF:  return "[PDF] ";
        default:            return "[???] ";
    }
}

void FileBrowser::renderTopInfo(C3D_RenderTarget* top, const Theme& theme) {
    C2D_TargetClear(top, theme.background);
    C2D_SceneBegin(top);

    // Title bar
    C2D_DrawRectSolid(0, 0, 0.5f, 400, 28, theme.accent);
    g_fontEngine.drawText("3DS Book Reader", 8, 4, C2D_Color32(255,255,255,255), 14);

    // Sort mode label
    const char* sortLabel = "Name";
    if (sortMode_ == SortMode::Recent)   sortLabel = "Recent";
    if (sortMode_ == SortMode::FileType) sortLabel = "Type";
    char buf[64];
    snprintf(buf, sizeof(buf), "Sort: %s  [Y=cycle]", sortLabel);
    g_fontEngine.drawText(buf, 8, 36, theme.foreground, 12);

    // Selected file detail
    if (selected_ >= 0 && selected_ < (int)entries_.size()) {
        const auto& fe = entries_[selected_];
        snprintf(buf, sizeof(buf), "%s", fe.name.c_str());
        g_fontEngine.drawText(buf, 8, 60, theme.foreground, 13);
        if (fe.progress >= 0) {
            snprintf(buf, sizeof(buf), "Progress: %.0f%%", fe.progress * 100.0f);
            g_fontEngine.drawText(buf, 8, 80, theme.accent, 12);
        }
    }

    g_fontEngine.drawText("[A/Touch]=Open  [Start]=Settings  [Y]=Sort", 8, 220, theme.foreground, 10);
}

void FileBrowser::renderList(C3D_RenderTarget* bot, const Theme& theme) {
    C2D_TargetClear(bot, theme.background);
    C2D_SceneBegin(bot);

    int y = 4;
    for (int i = scrollOffset_; i < (int)entries_.size() && y + ROW_H <= 240; i++) {
        const auto& fe = entries_[i];
        bool sel = (i == selected_);

        // Row background
        u32 rowBg = sel ? theme.accent : (i % 2 == 0 ? theme.background
                                                       : C2D_Color32(240,240,240,255));
        C2D_DrawRectSolid(0, y, 0.5f, 320, ROW_H - 2, rowBg);

        // Type label
        u32 fg = sel ? C2D_Color32(255,255,255,255) : theme.foreground;
        g_fontEngine.drawText(typeLabel(fe.type), 4, y + 6, fg, 11);

        // Filename (truncated)
        std::string display = fe.name;
        if (display.size() > 28) display = display.substr(0, 25) + "...";
        g_fontEngine.drawText(display, 52, y + 6, fg, 12);

        // Progress bar
        if (fe.progress >= 0) {
            float barW = 60.0f * fe.progress;
            C2D_DrawRectSolid(256, y + 10, 0.6f, 60, 8, C2D_Color32(200,200,200,255));
            C2D_DrawRectSolid(256, y + 10, 0.6f, barW, 8, theme.progress_bar);
        }

        y += ROW_H;
    }

    // Scrollbar
    if ((int)entries_.size() > VISIBLE_ROWS) {
        float ratio = (float)scrollOffset_ / (float)(entries_.size() - VISIBLE_ROWS);
        int sbH  = 200;
        int sbY  = 4 + (int)(ratio * (sbH - 20));
        C2D_DrawRectSolid(316, 4,   0.7f, 4, sbH,  C2D_Color32(180,180,180,255));
        C2D_DrawRectSolid(316, sbY, 0.7f, 4, 20,   theme.accent);
    }
}

void FileBrowser::renderContextMenu(C3D_RenderTarget* bot, const Theme& theme) {
    C2D_TargetClear(bot, theme.background);
    C2D_SceneBegin(bot);
    if (ctxEntry_ < 0 || ctxEntry_ >= (int)entries_.size()) return;

    g_fontEngine.drawText(entries_[ctxEntry_].name.c_str(), 8, 8, theme.foreground, 13);

    const char* options[] = { "Open", "Delete Progress", "Info" };
    for (int i = 0; i < 3; i++) {
        bool sel = (i == ctxSel_);
        C2D_DrawRectSolid(16, 40 + i * 40, 0.5f, 288, 36,
                          sel ? theme.accent : C2D_Color32(220,220,220,255));
        u32 fg = sel ? C2D_Color32(255,255,255,255) : theme.foreground;
        g_fontEngine.drawText(options[i], 24, 48 + i * 40, fg, 14);
    }
    g_fontEngine.drawText("[B]=Cancel", 8, 220, theme.foreground, 11);
}

//-----------------------------------------------------------------------------
// Update (input handling)
//-----------------------------------------------------------------------------
void FileBrowser::ensureVisible() {
    if (selected_ < scrollOffset_) scrollOffset_ = selected_;
    if (selected_ >= scrollOffset_ + VISIBLE_ROWS)
        scrollOffset_ = selected_ - VISIBLE_ROWS + 1;
}

std::string FileBrowser::update(u32 keysDown, u32 keysHeld,
                                 const touchPosition& touch,
                                 bool touchDown, bool touchReleased,
                                 std::string* longPressPath) {
    ctxAction_ = ContextAction::None;

    // keysHeld is currently unused: the only held-key binding in the browser
    // was C-Stick list scrolling, now disabled (see below). The parameter is
    // kept so re-enabling it does not require touching the call site.
    (void)keysHeld;

    // --- Context menu input ---
    if (ctxMenuOpen_) {
        if (keysDown & KEY_DUP)   { ctxSel_ = std::max(0, ctxSel_ - 1); }
        if (keysDown & KEY_DDOWN) { ctxSel_ = std::min(2, ctxSel_ + 1); }
        if (keysDown & KEY_A) {
            ctxAction_ = (ContextAction)(ctxSel_ + 1);
            ctxMenuOpen_ = false;
        }
        if (keysDown & KEY_B) { ctxMenuOpen_ = false; }

        // Touch on context menu options.
        //
        // The release frame reports (0,0) — there is no contact left to read —
        // so the position has to be the one recorded when the finger went
        // down. Using the release frame's own coordinates meant row came out
        // as (0 - 40) / 40 = -1 and the `py > 200` cancel never fired either,
        // so the menu could not be used by touch at all.
        if (touchDown && !touchHolding_) {
            touchStartY_  = (int)touch.py;
            touchHolding_ = true;
        }
        if (touchReleased) {
            touchHolding_ = false;
            holdFrames_   = 0;
            // touchStartY_ < 0 marks the contact that opened this menu by
            // long-pressing a list row (see below): that finger's position
            // refers to the list underneath, not to a menu option, so its
            // release must not select anything.
            if (touchStartY_ >= 0) {
                int row = (touchStartY_ - 40) / 40;   // options drawn at 40 + i*40
                if (touchStartY_ >= 40 && row >= 0 && row < 3) {
                    ctxSel_      = row;
                    ctxAction_   = (ContextAction)(row + 1);
                    ctxMenuOpen_ = false;
                } else if (touchStartY_ > 200) {      // the "[B]=Cancel" strip
                    ctxMenuOpen_ = false;
                }
            }
            touchStartY_ = 0;
        }
        return "";
    }

    // --- Normal list input ---
    int prev = selected_;
    if (keysDown & KEY_DUP)        { selected_--; }
    if (keysDown & KEY_DDOWN)      { selected_++; }
    // C-Stick list navigation is deliberately DISABLED for now. It was bound
    // to keysHeld, so it moved one row per frame — 60 rows/second, which
    // overshoots any real library instantly and is unusable. Deferred rather
    // than repaired: selecting a book is only the cheap half of the action,
    // and until opening one is fast, a faster way to scroll the list is not
    // the bottleneck. D-Pad Up/Down (above) remains the supported path.
    selected_ = std::max(0, std::min((int)entries_.size() - 1, selected_));
    if (selected_ != prev) ensureVisible();

    // Y button: cycle sort
    if (keysDown & KEY_Y) cycleSortMode();

    // A button: open
    if ((keysDown & KEY_A) && !entries_.empty())
        return entries_[selected_].fullPath;

    // Touch input
    if (touchDown && !touchHolding_) {
        touchStartY_  = (int)touch.py;
        touchHolding_ = true;
        holdFrames_   = 0;
    }
    if (touchHolding_) {
        holdFrames_++;
        // Long press detection
        if (holdFrames_ == LONG_PRESS_FRAMES && touchStartY_ >= 0) {
            int row = scrollOffset_ + touchStartY_ / ROW_H;
            if (row >= 0 && row < (int)entries_.size()) {
                ctxEntry_    = row;
                ctxSel_      = 0;
                ctxMenuOpen_ = true;
                if (longPressPath) *longPressPath = entries_[row].fullPath;
                // The finger is still down. Mark this contact as spent so the
                // release that ends it cannot also be read as a tap on a
                // context-menu option, and reset the hold so the menu starts
                // from a clean touch state.
                touchStartY_  = -1;
                touchHolding_ = false;
                holdFrames_   = 0;
            }
        }
    }
    if (touchReleased) {
        touchHolding_ = false;
        if (holdFrames_ < LONG_PRESS_FRAMES && touchStartY_ >= 0) {
            // touchStartY_, not touch.py. hidTouchRead() reports (0,0) on the
            // frame the finger lifts, so `touch.py / ROW_H` was always 0 and
            // this opened `entries_[scrollOffset_]` — the first visible book —
            // no matter which row was actually tapped.
            int row = scrollOffset_ + touchStartY_ / ROW_H;
            if (row >= 0 && row < (int)entries_.size()) {
                selected_ = row;
                ensureVisible();
                touchStartY_ = 0;
                holdFrames_  = 0;
                return entries_[selected_].fullPath;  // open on tap
            }
        }
        touchStartY_ = 0;
        holdFrames_  = 0;
    }

    return "";
}
