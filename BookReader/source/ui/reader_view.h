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

    /// The current book's bookmarks, in insertion order. Exposed so the
    /// overlay's Bookmarks tab can list them and report a selection index
    /// back — it previously received an empty vector from main.cpp and so
    /// permanently displayed "No bookmarks" no matter what was saved.
    const std::vector<Bookmark>& bookmarks() const { return bookmarks_; }

    /// Jump to an absolute position. If the target chapter differs from the
    /// current one, the load is deferred — see hasPendingWork().
    void goToPosition(int chapter, int spread);

    /// Re-run layout for the current chapter, preserving position as closely as
    /// possible. Call when font size, margins or justification change — much
    /// cheaper than reopening the book.
    void relayoutCurrentChapter();

    // ------------------------------------------------------------------
    // Deferred layout work
    //
    // Laying out a chapter — FreeType text measurement plus glyph-atlas GPU
    // cache flushes (see font_engine.cpp uploadGlyph) — is exactly the class
    // of "long, blocking, GPU-cache-flushing work" the handoff documents as
    // unsafe inside an open C3D frame (bug #8: it wedges the GPU). Book
    // opening already defers that work between frames via
    // s_pendingOpenPath/showLoading() in main.cpp. Page turns that cross a
    // chapter boundary, chapter jumps, and relayouts from a settings change
    // all do the same expensive work but were previously calling loadChapter()
    // straight out of update(), which runs inside the frame main.cpp has open.
    //
    // nextSpread()/prevSpread()/goToPosition()/requestRelayout() now only
    // *queue* that work; main.cpp must check hasPendingWork() at the top of
    // its loop (before C3D_FrameBegin) and call servicePendingWork() there,
    // the same way it already services s_pendingOpenPath.
    // ------------------------------------------------------------------
    bool hasPendingWork() const {
        return pendingChapterIndex_ != PENDING_NONE || pendingRelayout_;
    }
    void servicePendingWork();

    /// True when the pending work is a chapter load (page turn / go-to)
    /// rather than a same-chapter relayout — lets the caller
    /// choose an accurate loading-screen message ("Turning the Page" vs
    /// "Refreshing the Page"). Only meaningful when hasPendingWork() is true;
    /// chapter loads take priority if both were somehow queued at once,
    /// matching servicePendingWork()'s own priority.
    bool pendingIsChapterLoad() const { return pendingChapterIndex_ != PENDING_NONE; }

    /// Queue a relayout of the current chapter (font size / margin / justify
    /// changed). Actually applied by servicePendingWork(). Also invalidates
    /// any in-flight/cached next-chapter prefetch (see below), since it was
    /// computed for the old font size / margins and would otherwise be
    /// silently swapped in as if it matched the new settings.
    void requestRelayout() {
        pendingRelayout_       = true;
        prefetchChapter_       = -1;
        prefetchQueuedFor_     = -1;
        prefetchRequestedFor_  = -1;
    }

    // ------------------------------------------------------------------
    // Next-chapter prefetch
    //
    // loadChapter() (see below) is the one expensive operation in this
    // reader: it decompresses/parses a chapter's text and runs FreeType
    // measurement over every character. Paid synchronously, that's exactly
    // the multi-second stall reported on real hardware for a page turn that
    // crosses a chapter boundary — even after trace.cpp's open/close-per-line
    // overhead was fixed, the FreeType layout work itself is not free.
    //
    // The fix is to pay that cost before it's needed: once a chapter is
    // loaded, the next chapter's layout is queued and computed into a
    // second, off-screen PageLayout while the user is just sitting on the
    // current page — not while they're waiting on a turn. When a forward
    // turn later reaches that chapter, loadChapter() finds the prefetch
    // already sitting there and swaps it in with a move-assignment instead
    // of re-running FreeType.
    //
    // Deliberately one direction (forward) and one slot: reading is
    // overwhelmingly forward, and each cached layout holds a full chapter's
    // worth of tokens/lines/pages — not free on the 3DS's limited RAM.
    // Backward turns still pay the full cost, same as before this feature.
    //
    // main.cpp must check hasPrefetchWork() between frames, same as
    // hasPendingWork() (never inside an open C3D frame — the glyph-atlas GPU
    // flush is the same "unsafe" work as loadChapter's), but at LOWER
    // priority: an actual navigation or relayout in flight must always be
    // serviced first.
    //
    // CAVEAT (unverified on hardware): servicePrefetch() runs the whole
    // chapter's layout in one go, with no C3D_FrameBegin/End around it, so
    // that iteration of the main loop draws nothing — the screen just holds
    // its last frame for however long layout takes. For a short chapter this
    // is imperceptible; for a very long one it could look like a brief
    // freeze while the reader is otherwise idle. If that's visible on
    // hardware, the fix is to chunk PageLayout::run() so it can be resumed
    // across several frames instead of running to completion in one.
    // ------------------------------------------------------------------
    bool hasPrefetchWork() const { return prefetchQueuedFor_ != -1; }
    void servicePrefetch();

    /// Record the current position in the in-memory progress store.
    ///
    /// This does NOT write progress.json — ProgressStore batches that behind a
    /// dirty flag (see progress.h), because writing the file cost an SD
    /// round trip on every single page turn. Call ProgressStore::saveIfDirty()
    /// at the points where the position could actually be lost; main.cpp owns
    /// that policy.
    void saveProgress(const std::string& path);

    /// Milliseconds the last chapter load took, broken down. Populated by
    /// loadChapter() whether or not Settings ▸ Show Timings is on, so a
    /// caller can report them however it likes.
    struct LoadTiming {
        float total   = 0.0f;   // whole loadChapter() call
        float text    = 0.0f;   // resolve + ZIP inflate + XHTML → plain text
        float layout  = 0.0f;   // PageLayout::run (FreeType measurement)
        bool  prefetchHit = false;
    };
    const LoadTiming& lastLoadTiming() const { return lastLoad_; }

    // Auto-scroll: call once per frame
    void tickAutoScroll(int pagesPerMin);

    // Toast notification (set externally, cleared after ~120 frames)
    void setToast(const std::string& msg);

    // TTS: current page text (for audio/tts_engine)
    std::string currentPageText(bool leftPage) const;

    /// Portrait projection matrix helper — public so main.cpp's showLoading()
    /// can apply the reader's 180°-rotated portrait view when drawing a
    /// loading message over a *portrait* (top-screen reader) context.
    /// Without this, loading text drawn with the default (tilt=true)
    /// landscape scene setup renders upside down relative to the physically
    /// rotated screen. See main.cpp showLoading().
    void setupPortraitProjection(C3D_RenderTarget* target, int portW, int portH);

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

    /// Recompute the four page dimensions above from the current margin
    /// setting.
    ///
    /// Shared by loadChapter() and servicePrefetch() because the two MUST
    /// paginate with identical parameters: a prefetch hit swaps its layout in
    /// wholesale (see loadChapter), so if the prefetch had been paginated to
    /// different page heights, the reader would silently start showing pages
    /// broken for a geometry it is no longer using. servicePrefetch()
    /// previously computed its own `pw`/`ph` locally from PORTRAIT_TOP_*,
    /// which is exactly that divergence waiting to happen.
    void computePageMetrics();

    /// Orientation most recently seen by render(), and the orientation the
    /// current layout was paginated for.
    ///
    /// Which screen a page lands on depends on the orientation (flipped swaps
    /// the two), and the screens are different heights, so pagination depends
    /// on it too. render() queues a relayout when the two disagree, which is
    /// what keeps a flip from putting a top-screen-height page on the shorter
    /// bottom screen and clipping its last lines.
    bool         orientFlipped_ = false;
    bool         layoutFlipped_ = false;

    /// Page heights in (even, odd) page order for the current orientation.
    int  pageHeightEven() const { return layoutFlipped_ ? botPageH_ : topPageH_; }
    int  pageHeightOdd()  const { return layoutFlipped_ ? topPageH_ : botPageH_; }

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

    // SHA-1 of the open book's path, and the path it was computed from.
    // Hashing is cheap in isolation, but saveProgress() runs on every page
    // turn and nothing belongs on that path that doesn't have to be there. The
    // path is kept so the key is recomputed if it ever changes, rather than
    // silently writing one book's position under another book's key.
    std::string  progressPath_;
    std::string  progressKey_;

    // Timing of the most recent chapter load — see lastLoadTiming().
    LoadTiming   lastLoad_;

    // Render helpers
    void renderReflowPage(C3D_RenderTarget* target, int pageIndex,
                          int portW, int portH, const Theme& theme);
    void renderPdfPage(C3D_RenderTarget* target, int pageIndex,
                       int portW, int portH, const Theme& theme);
    void renderPageMeta(C3D_RenderTarget* target, int pageIndex,
                        int portW, int portH, const Theme& theme);
    void renderProgressBar(C3D_RenderTarget* target, const Theme& theme);
    void renderToast(C3D_RenderTarget* target, const Theme& theme);

    // Navigation. Return true if spread_ changed immediately (same-chapter
    // turn); false if the turn crossed a chapter boundary and was queued
    // instead — see "Deferred layout work" above.
    bool nextSpread();
    bool prevSpread();

    // Deferred layout work (see public section above)
    static constexpr int PENDING_NONE = -1;
    int  pendingChapterIndex_     = PENDING_NONE;
    bool pendingLandOnLastSpread_ = false;
    int  pendingGotoSpread_       = -1;   // applied once a queued chapter load completes
    bool pendingRelayout_         = false;

    // Next-chapter prefetch (see public section above)
    PageLayout prefetchLayout_;
    int        prefetchChapter_   = -1;   // chapter prefetchLayout_ holds a valid layout for, -1 = empty
    int        prefetchQueuedFor_ = -1;   // chapter still to prefetch, -1 = nothing queued

    /// The *requested* index the prefetch was computed for, before
    /// resolveReadableChapter() stepped over any blank front matter —
    /// prefetchChapter_ is where it landed.
    ///
    /// loadChapter() needs both. It is called with the requested index, and
    /// its prefetch-hit test used to compare against prefetchChapter_ alone,
    /// which meant it had to run resolveReadableChapter() first just to find
    /// out whether it could skip the work. That probe pulls each candidate
    /// chapter's text through the ZIP-inflate + XHTML-parse path, so the
    /// "instant" hit path was still paying for a decompress and a DOM walk
    /// before it could decide it had nothing to do. Matching on the requested
    /// index makes the hit a pure move-assignment.
    int        prefetchRequestedFor_ = -1;

    /// Queue a prefetch of the chapter after curChapter_, unless it's out of
    /// range or already cached. Called after every successful chapter load.
    void queueNextPrefetch();

    /// Walk from `index` in direction `step` (+1/-1) until a chapter with
    /// enough real prose to be worth landing on is found — shared by
    /// loadChapter()'s blank-chapter skip and servicePrefetch()'s "what will
    /// the next real page turn actually land on" guess. Returns -1 if
    /// nothing substantial exists in that direction and `index` itself is
    /// also empty.
    int resolveReadableChapter(int index, int step);

    /// Text of chapter `i` — from the EPUB (lazily decompressed) or, for TXT,
    /// the single buffered file.
    const std::string& chapterTextAt(int i);

    /// Load a chapter and lay it out. `landOnLastSpread` is used when paging
    /// backwards across a chapter boundary, so the reader arrives at the end of
    /// the previous chapter rather than its start.
    ///
    /// If the chapter this resolves to matches a completed prefetch (see
    /// above), the layout is swapped in directly instead of being recomputed.
    /// Returns false if the chapter index is out of range.
    bool loadChapter(int index, bool landOnLastSpread = false);
};

extern ReaderView g_reader;
