#pragma once
#include "common.h"
#include "renderer/page_layout.h"
#include "formats/epub_parser.h"
#include "formats/pdf_renderer.h"
#include "input/gyro_handler.h"
#include "settings/progress.h"
#include <string>
#include <vector>

//-----------------------------------------------------------------------------
// ReaderView — design doc §6
//
// Portrait layout engine: renders two pages simultaneously to top and bottom
// framebuffers. Device is rotated 90° anti-clockwise.
//
// Left  page (verso, even) → Top screen    (240×400 px in portrait)
// Right page (recto, odd)  → Bottom screen (240×320 px in portrait)
//
// Page numbering: page 0 = verso (top), page 1 = recto (bottom).
// Spread = two pages shown at once. Spread index S shows pages 2S and 2S+1.
//
// For reflowable formats (EPUB/TXT): text layout via page_layout.
// For fixed layout (PDF): MuPDF rasterisation via pdf_renderer.
//
// Chapter-scoped pagination
// -------------------------
// Only the CURRENT chapter is laid out. Laying out a whole book up front means
// running FreeType metrics over every character before the first page can be
// drawn, which on a 268 MHz ARM11 takes minutes for a full-length novel.
//
// Spread indices are therefore relative to the current chapter, and page turns
// roll over into the adjacent chapter. The consequence is that a book's total
// page count is not known until it has all been visited — so the UI reports
// position as "chapter N of M" rather than a global page number, which is what
// commercial readers do while pagination is still incomplete.
//
// A TXT file is modelled as a single chapter, so both formats share one path.
//-----------------------------------------------------------------------------
class ReaderView {
public:
    // Open a document. Returns false on failure with an error description.
    bool open(const std::string& path, DocType type, bool isNewHardware,
              std::string& errorOut);
    void close();

    // Render current spread to top and bottom targets
    // topTarget = left page  (portrait: 240×400, framebuffer 400×240)
    // botTarget = right page (portrait: 240×320, framebuffer 320×240)
    void render(C3D_RenderTarget* topTarget, C3D_RenderTarget* botTarget,
                const Theme& theme, const GyroHandler::Orientation& orient,
                float flipAlpha);   // flipAlpha: 0=normal, 1=solid black (fade)

    // Input: returns true if a navigation action occurred
    bool update(u32 keysDown, u32 keysHeld,
                const touchPosition& touch, bool touchDown, bool touchReleased,
                const GyroHandler& gyro);

    // Position within the current chapter
    int  currentSpread() const { return spread_; }
    int  totalSpreads()  const { return totalSpreads_; }

    // Position within the book
    int  currentChapter() const { return curChapter_; }
    int  chapterCount()   const { return chapterCount_; }
    std::string currentChapterTitle() const;

    /// Whole-book progress, approximated as chapter position plus the fraction
    /// read within the current chapter. Exact global page numbers are not
    /// available without laying out the entire book.
    float progressRatio() const {
        if (chapterCount_ <= 0) return 0.0f;
        float within = (totalSpreads_ > 0)
                     ? (float)spread_ / (float)totalSpreads_ : 0.0f;
        return ((float)curChapter_ + within) / (float)chapterCount_;
    }

    // Bookmarks
    void toggleBookmark();
    bool hasBookmark(int chapter, int spread) const;

    /// Jump to an absolute position. Loads and lays out the target chapter.
    void goToPosition(int chapter, int spread);

    /// Re-run layout for the current chapter, preserving position as closely as
    /// possible. Call when font size, margins or justification change — much
    /// cheaper than reopening the book.
    void relayoutCurrentChapter();

    // Save progress to g_progress store
    void saveProgress(const std::string& path);

    // Auto-scroll: call once per frame
    void tickAutoScroll(int pagesPerMin);

    // Toast notification (set externally, cleared after ~120 frames)
    void setToast(const std::string& msg);

    // TTS: current page text (for audio/tts_engine)
    std::string currentPageText(bool leftPage) const;

private:
    DocType     docType_     = DocType::Unknown;
    bool        isNew3DS_    = false;

    // Reflowable (EPUB / TXT)
    EpubDocument  epub_;
    std::string   txtText_;      // whole file, for the single-chapter TXT case

    // Chapter cursor. For TXT this is always chapter 0 of 1.
    int           curChapter_   = 0;
    int           chapterCount_ = 0;

    // Page layout cache
    // layout is shared (g_layout), but we cache page boundaries here
    int          topPageW_   = PORTRAIT_TOP_W  - 20;  // 220 px (10px each side)
    int          topPageH_   = PORTRAIT_TOP_H  - 20;  // 380 px
    int          botPageW_   = PORTRAIT_BOT_W  - 20;
    int          botPageH_   = PORTRAIT_BOT_H  - 20;

    int          spread_     = 0;   // current spread index
    int          totalSpreads_ = 0;

    std::vector<Bookmark> bookmarks_;

    // Auto-scroll
    int          autoScrollFrameCounter_ = 0;

    // Toast
    std::string  toastMsg_;
    int          toastFrames_ = 0;

    // PDF-specific
    bool         isFixedLayout_ = false;

    // Portrait projection matrix helpers
    void setupPortraitProjection(C3D_RenderTarget* target, int portW, int portH);

    // Render helpers
    void renderReflowPage(C3D_RenderTarget* target, int pageIndex,
                          int portW, int portH, const Theme& theme);
    void renderPdfPage(C3D_RenderTarget* target, int pageIndex,
                       int portW, int portH, const Theme& theme);
    void renderPageMeta(C3D_RenderTarget* target, int pageIndex,
                        int portW, int portH, const Theme& theme);
    void renderProgressBar(C3D_RenderTarget* target, const Theme& theme);
    void renderToast(C3D_RenderTarget* target, const Theme& theme);

    // Navigation
    void nextSpread();
    void prevSpread();

    /// Text of chapter `i` — from the EPUB (lazily decompressed) or, for TXT,
    /// the single buffered file.
    const std::string& chapterTextAt(int i);

    /// Load a chapter and lay it out. `landOnLastSpread` is used when paging
    /// backwards across a chapter boundary, so the reader arrives at the end of
    /// the previous chapter rather than its start.
    /// Returns false if the chapter index is out of range.
    bool loadChapter(int index, bool landOnLastSpread = false);
};

extern ReaderView g_reader;
