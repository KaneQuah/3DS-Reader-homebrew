#include "ui/reader_view.h"
#include "trace.h"
#include "perf.h"
#include "renderer/font_engine.h"
#include "renderer/page_layout.h"
#include "formats/epub_parser.h"
#include "formats/txt_parser.h"
#include "formats/pdf_renderer.h"
#include "settings/config.h"
#include "settings/progress.h"
#include "input/touch_handler.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <utility>
#include <citro3d.h>

ReaderView g_reader;

static TouchHandler s_touch;

//-----------------------------------------------------------------------------
// open()
//-----------------------------------------------------------------------------
bool ReaderView::open(const std::string& path, DocType type,
                      bool isNewHardware, std::string& errorOut) {
    close();
    docType_   = type;
    isNew3DS_  = isNewHardware;
    bookmarks_.clear();

    // Resume position. A book with no stored progress starts at chapter 0,
    // spread 0 — which is also the cheapest possible open, since only the
    // first chapter is ever touched.
    int startChapter = 0, startSpread = 0;
    const BookProgress* bp = g_progress.get(path);
    if (bp) {
        startChapter = bp->chapter;
        startSpread  = bp->spread;
        bookmarks_   = bp->bookmarks;
    }

    if (type == DocType::EPUB) {
        EpubParser parser;
        auto result = parser.open(path, epub_);
        if (result != EpubParser::Result::OK) {
            errorOut = "Failed to open EPUB (error " + std::to_string((int)result) + ")";
            return false;
        }
        chapterCount_  = epub_.chapterCount();
        isFixedLayout_ = false;
    } else if (type == DocType::TXT) {
        TxtParser txt;
        if (!txt.parse(path, txtText_)) {
            errorOut = "Failed to read text file";
            return false;
        }
        // A plain text file is one chapter, so it shares the EPUB code path.
        chapterCount_  = 1;
        isFixedLayout_ = false;
    } else if (type == DocType::PDF) {
        if (!g_pdfRenderer.open(path)) {
            errorOut = "PDF support is not built in (rebuild with ENABLE_PDF=1)";
            return false;
        }
        isFixedLayout_  = true;
        chapterCount_   = 1;
        curChapter_     = 0;
        totalSpreads_   = (g_pdfRenderer.pageCount() + 1) / 2;
        spread_         = std::min(startSpread, std::max(0, totalSpreads_ - 1));
        return true;
    } else {
        errorOut = "Unsupported file type";
        return false;
    }

    // Lay out only the chapter we are about to display.
    if (startChapter < 0 || startChapter >= chapterCount_) startChapter = 0;
    if (!loadChapter(startChapter)) {
        errorOut = "Book contains no readable chapters";
        return false;
    }
    spread_ = std::min(startSpread, std::max(0, totalSpreads_ - 1));
    return true;
}

//-----------------------------------------------------------------------------
// chapterTextAt — the one place EPUB and TXT diverge
//-----------------------------------------------------------------------------
const std::string& ReaderView::chapterTextAt(int i) {
    if (docType_ == DocType::EPUB) return epub_.chapterText(i);
    return txtText_;   // TXT: single chapter
}

// "No prose" cannot mean "empty string": image-only cover pages are an SVG
// wrapping a JPEG, and typically carry a stray caption or URL in metadata.
// Those produce a handful of characters and would otherwise be presented as
// the book's first page. Anything under kMinChapterChars visible (i.e.
// non-whitespace) characters is treated as front matter and stepped over.
static constexpr size_t kMinChapterChars = 120;

//-----------------------------------------------------------------------------
// resolveReadableChapter — see reader_view.h.
//
// Only the index is carried through the probe loop, never a reference into
// the chapter cache: the cache holds three slots and recycles them, so a
// reference taken here would silently start pointing at a different
// chapter's text after three probes.
//-----------------------------------------------------------------------------
int ReaderView::resolveReadableChapter(int index, int step) {
    int probe = index;
    // The count is only ever compared against kMinChapterChars, so stop there
    // rather than scanning a whole chapter to find out it has 40,000
    // characters when 120 would have answered the question.
    while (epubCountVisibleChars(chapterTextAt(probe), kMinChapterChars) < kMinChapterChars) {
        int next = probe + step;
        if (next < 0 || next >= chapterCount_) {
            // Nothing substantial in that direction. Fall back to the requested
            // chapter if it has any text at all, rather than refusing to open a
            // book made entirely of short sections.
            if (!chapterTextAt(index).empty()) return index;
            return -1;
        }
        probe = next;
    }
    return probe;
}

//-----------------------------------------------------------------------------
// queueNextPrefetch — see "Next-chapter prefetch" in reader_view.h.
//-----------------------------------------------------------------------------
void ReaderView::queueNextPrefetch() {
    if (isFixedLayout_) return;   // PDF doesn't use PageLayout at all
    int next = curChapter_ + 1;
    if (next >= chapterCount_) { prefetchQueuedFor_ = -1; return; }
    // Already cached, under either the index that was requested or the one the
    // probe resolved to.
    if (next == prefetchChapter_ || next == prefetchRequestedFor_) {
        prefetchQueuedFor_ = -1;
        return;
    }
    prefetchQueuedFor_ = next;
}

//-----------------------------------------------------------------------------
// servicePrefetch — see reader_view.h. Must be called between frames, same
// as servicePendingWork(), and only when hasPendingWork() is false.
//-----------------------------------------------------------------------------
void ReaderView::servicePrefetch() {
    int target = prefetchQueuedFor_;
    prefetchQueuedFor_ = -1;
    if (isFixedLayout_ || target < 0 || target >= chapterCount_) return;

    int probe = resolveReadableChapter(target, +1);
    if (probe < 0 || probe == prefetchChapter_) return;

    // Remember the index the *caller* will ask for, not just the one the probe
    // landed on — see prefetchRequestedFor_ in reader_view.h.
    prefetchRequestedFor_ = target;

    const std::string& text = chapterTextAt(probe);
    TRACE("prefetch: chapter %d has %u chars", probe, (unsigned)text.size());

    int margin = g_config.marginPx();
    int pw = PORTRAIT_TOP_W - margin * 2;
    int ph = PORTRAIT_TOP_H - margin * 2 - 24;

    prefetchLayout_.run(text, pw, ph,
                        g_config.font_size, g_config.lineSpacingMul(),
                        g_config.justify, isNew3DS_);

    if (prefetchLayout_.pageCount() == 0) {
        // Same "produced nothing" case loadChapter() guards against — leave
        // the prefetch empty rather than caching a layout for a page count
        // of zero; loadChapter() will just recompute for real when it gets
        // there (resolveReadableChapter() should normally have already
        // steered around this, so this is a belt-and-braces fallback).
        TRACE("  prefetch chapter %d laid out to zero pages, discarding", probe);
        prefetchChapter_      = -1;
        prefetchRequestedFor_ = -1;
        return;
    }

    prefetchChapter_ = probe;
    TRACE("  prefetch done: chapter %d, %u pages", probe, (unsigned)prefetchLayout_.pageCount());
}

//-----------------------------------------------------------------------------
// loadChapter — fetch text, lay it out, position the cursor
//
// This is the only expensive operation in the reader, and its cost is bounded
// by one chapter rather than by the size of the book — except when the
// target chapter matches a completed prefetch (see reader_view.h), in which
// case it's a move-assignment instead of a FreeType run.
//-----------------------------------------------------------------------------
bool ReaderView::loadChapter(int index, bool landOnLastSpread) {
    if (index < 0 || index >= chapterCount_) return false;
    TRACE("loadChapter(%d, back=%d) of %d", index, (int)landOnLastSpread, chapterCount_);

    const u64 tStart = perfNow();
    lastLoad_ = LoadTiming{};

    int margin = g_config.marginPx();
    topPageW_  = PORTRAIT_TOP_W - margin * 2;
    topPageH_  = PORTRAIT_TOP_H - margin * 2 - 24;   // room for page number
    botPageW_  = PORTRAIT_BOT_W - margin * 2;
    botPageH_  = PORTRAIT_BOT_H - margin * 2 - 24;

    // Prefetch hit: the forward-turn common case. The layout for this exact
    // chapter was already computed while the user was sitting on the
    // previous page — swap it in and skip FreeType entirely. Only valid
    // going forward (landOnLastSpread means a backward turn, which this
    // feature deliberately doesn't cover — see reader_view.h).
    //
    // This test deliberately runs BEFORE resolveReadableChapter(), and matches
    // on the requested index rather than the resolved one. The probe is what
    // pulls chapter text through the ZIP-inflate/XHTML-parse path, so running
    // it first meant the "skip the expensive work" branch had already done a
    // decompress and a DOM walk before it could decide to skip anything —
    // which is a good candidate for why the prefetch, though it was landing,
    // did not make turns feel any faster. servicePrefetch() already resolved
    // this index; prefetchChapter_ is the answer it got.
    if (!landOnLastSpread && index == prefetchRequestedFor_ &&
        prefetchChapter_ >= 0 && prefetchLayout_.pageCount() > 0) {
        TRACE("  prefetch hit for chapter %d (requested %d), skipping layout",
              prefetchChapter_, index);
        curChapter_ = prefetchChapter_;
        g_layout    = std::move(prefetchLayout_);
        prefetchChapter_      = -1;
        prefetchRequestedFor_ = -1;

        totalSpreads_ = ((int)g_layout.pageCount() + 1) / 2;
        spread_       = 0;
        queueNextPrefetch();

        lastLoad_.prefetchHit = true;
        lastLoad_.total       = perfMsSince(tStart);
        return true;
    }

    int step  = landOnLastSpread ? -1 : +1;
    const u64 tText = perfNow();
    int probe = resolveReadableChapter(index, step);
    if (probe < 0) {
        TRACE("  no readable chapter found from %d", index);
        return false;
    }

    curChapter_ = probe;

    const std::string& text = chapterTextAt(probe);
    lastLoad_.text = perfMsSince(tText);
    TRACE("  chapter %d has %u chars (text %.0f ms)", probe, (unsigned)text.size(),
          (double)lastLoad_.text);
    TRACE("  layout start (w=%d h=%d pt=%d)", topPageW_, topPageH_, g_config.font_size);

    // New 3DS has the headroom for Knuth-Plass line breaking; Old 3DS uses greedy.
    const u64 tLayout = perfNow();
    g_layout.run(text, topPageW_, topPageH_,
                 g_config.font_size, g_config.lineSpacingMul(),
                 g_config.justify, isNew3DS_);
    lastLoad_.layout = perfMsSince(tLayout);

    TRACE("  layout done: %u pages (%.0f ms)", (unsigned)g_layout.pageCount(),
          (double)lastLoad_.layout);

    // A chapter that produced no pages must not be presented: totalSpreads_ was
    // previously clamped up to 1 here, which left the renderer indexing into an
    // empty page list.
    //
    // curChapter_ was already set to `probe` above, so on this failure path it
    // now names a chapter whose g_layout is empty — but totalSpreads_/spread_
    // still held whatever the *previous* successful chapter left behind. That
    // mismatch let nextSpread()/prevSpread() keep incrementing spread_ against
    // a stale totalSpreads_ that no longer described anything, paging through
    // a chapter that only ever renders blank. Reset both here so the empty
    // chapter's state is internally consistent even though open()/nextSpread()
    // report failure and the caller falls back to whatever it was already
    // showing.
    if (g_layout.pageCount() == 0) {
        TRACE("  chapter %d laid out to zero pages", probe);
        totalSpreads_ = 0;
        spread_       = 0;
        lastLoad_.total = perfMsSince(tStart);
        return false;
    }

    totalSpreads_ = ((int)g_layout.pageCount() + 1) / 2;
    spread_       = landOnLastSpread ? totalSpreads_ - 1 : 0;
    queueNextPrefetch();
    lastLoad_.total = perfMsSince(tStart);
    return true;
}

//-----------------------------------------------------------------------------
// relayoutCurrentChapter — after a font/margin change
//-----------------------------------------------------------------------------
void ReaderView::relayoutCurrentChapter() {
    if (isFixedLayout_ || chapterCount_ <= 0) return;

    // Preserve position proportionally: the spread count changes with font
    // size, so an absolute index would jump the reader around.
    float ratio = (totalSpreads_ > 0) ? (float)spread_ / (float)totalSpreads_ : 0.0f;

    int chapter = curChapter_;
    if (loadChapter(chapter))
        spread_ = std::min((int)(ratio * totalSpreads_), totalSpreads_ - 1);
}

std::string ReaderView::currentChapterTitle() const {
    if (docType_ != DocType::EPUB) return std::string();
    if (curChapter_ < 0 || curChapter_ >= (int)epub_.chapters.size()) return std::string();
    return epub_.chapters[(size_t)curChapter_].title;
}

void ReaderView::close() {
    if (docType_ == DocType::PDF) g_pdfRenderer.close();
    g_layout.clear();
    prefetchLayout_.clear();
    prefetchChapter_      = -1;
    prefetchQueuedFor_    = -1;
    prefetchRequestedFor_ = -1;
    progressPath_.clear();
    progressKey_.clear();
    lastLoad_ = LoadTiming{};
    txtText_.clear();
    txtText_.shrink_to_fit();
    epub_.close();
    curChapter_   = 0;
    chapterCount_ = 0;
    spread_       = 0;
    totalSpreads_ = 0;
    bookmarks_.clear();
    docType_      = DocType::Unknown;
}

//-----------------------------------------------------------------------------
// Portrait projection setup
//
// The 3DS framebuffer is stored in a column-major (90° rotated) format.
// Mtx_OrthoTilt accounts for this and maps our logical portrait coordinate
// system (width × height) to the physical landscape framebuffer correctly.
//
// We call C2D_SceneBegin which internally calls Mtx_OrthoTilt with the
// screen's physical dimensions. For portrait content we then apply an
// additional transform that remaps our portrait coordinate space.
//-----------------------------------------------------------------------------
void ReaderView::setupPortraitProjection(C3D_RenderTarget* target, int portW, int portH) {
    C2D_SceneBegin(target);

    // Use citro2d's own scene API rather than poking the vertex shader's
    // uniforms directly.
    //
    // The previous version built a matrix and pushed it with
    //   C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, 0, &proj)
    // which assumes citro2d keeps its projection matrix in uniform register 0.
    // It does not — citro2d owns that shader, tracks its own uniform locations,
    // and re-uploads the projection lazily on the next draw whenever its dirty
    // flag is set. So the hand-written matrix was either overwritten before it
    // took effect or landed on an unrelated uniform. Either way the reader's
    // geometry was transformed by something other than the intended matrix,
    // which is why the pages laid out correctly but nothing appeared on screen
    // — while the file browser, which never overrode the projection, drew fine.
    //
    // C2D_SceneSize does the same job through the supported path and keeps
    // citro2d's internal state consistent.
    //
    // tilt = false: the 3DS framebuffers are stored rotated already, so their
    // native axes ARE our portrait axes once the user turns the console 90°.
    // Tilt correction is what produces the usual landscape coordinate system,
    // which is precisely what we do not want here.
    C2D_SceneSize((u32)portW, (u32)portH, false);

    // The framebuffer's native portrait axes run the opposite way to the
    // direction the console is actually held, so the page comes out upside
    // down. Rotate the scene 180° about its own centre.
    //
    // Composed as translate(centre) · rotate · translate(-centre) so the
    // rotation pivots on the middle of the page rather than the origin.
    C2D_ViewReset();
    C2D_ViewTranslate(portW * 0.5f, portH * 0.5f);
    C2D_ViewRotateDegrees(180.0f);
    C2D_ViewTranslate(portW * -0.5f, portH * -0.5f);
}

//-----------------------------------------------------------------------------
// render()
//-----------------------------------------------------------------------------
void ReaderView::render(C3D_RenderTarget* topTarget, C3D_RenderTarget* botTarget,
                        const Theme& theme, const GyroHandler::Orientation& orient,
                        float flipAlpha) {
    int topPage, botPage;
    if (orient == GyroHandler::Orientation::Normal) {
        topPage = spread_ * 2;      // verso (left/even)
        botPage = spread_ * 2 + 1;  // recto (right/odd)
    } else {
        // Flipped: screens swap roles
        topPage = spread_ * 2 + 1;
        botPage = spread_ * 2;
    }

    // ── Top screen (left page in normal orientation) ──
    C2D_TargetClear(topTarget, theme.background);
    setupPortraitProjection(topTarget, PORTRAIT_TOP_W, PORTRAIT_TOP_H);
    if (isFixedLayout_)
        renderPdfPage(topTarget, topPage, PORTRAIT_TOP_W, PORTRAIT_TOP_H, theme);
    else
        renderReflowPage(topTarget, topPage, PORTRAIT_TOP_W, PORTRAIT_TOP_H, theme);
    renderPageMeta(topTarget, topPage, PORTRAIT_TOP_W, PORTRAIT_TOP_H, theme);
    renderProgressBar(topTarget, theme);
    if (toastFrames_ > 0)   // toasts are only ever shown on the top screen
        renderToast(topTarget, theme);

    // ── Bottom screen (right page) ──
    C2D_TargetClear(botTarget, theme.background);
    setupPortraitProjection(botTarget, PORTRAIT_BOT_W, PORTRAIT_BOT_H);
    if (isFixedLayout_)
        renderPdfPage(botTarget, botPage, PORTRAIT_BOT_W, PORTRAIT_BOT_H, theme);
    else
        renderReflowPage(botTarget, botPage, PORTRAIT_BOT_W, PORTRAIT_BOT_H, theme);
    renderPageMeta(botTarget, botPage, PORTRAIT_BOT_W, PORTRAIT_BOT_H, theme);

    // Flip fade overlay
    if (flipAlpha > 0.0f) {
        u8 alpha = (u8)(flipAlpha * 255.0f);
        u32 black = C2D_Color32(0, 0, 0, alpha);
        C2D_SceneBegin(topTarget);
        C2D_DrawRectSolid(0, 0, 0.9f, (float)PORTRAIT_TOP_W, (float)PORTRAIT_TOP_H, black);
        C2D_SceneBegin(botTarget);
        C2D_DrawRectSolid(0, 0, 0.9f, (float)PORTRAIT_BOT_W, (float)PORTRAIT_BOT_H, black);
    }

    // Decrement toast counter
    if (toastFrames_ > 0) toastFrames_--;
}

//-----------------------------------------------------------------------------
// Render a reflowable page (from g_layout)
//-----------------------------------------------------------------------------
void ReaderView::renderReflowPage(C3D_RenderTarget* target, int pageIndex,
                                   int portW, int portH, const Theme& theme) {
    if (pageIndex < 0 || pageIndex >= (int)g_layout.pageCount()) return;

    int margin = g_config.marginPx();
    const Page&  pg    = g_layout.page(pageIndex);
    const auto&  lines = g_layout.lines();
    const auto&  toks  = g_layout.tokens();

    int ptSize = g_config.font_size;
    int lh     = (int)(g_fontEngine.lineHeight(ptSize) * g_config.lineSpacingMul());
    int y      = margin;
    int contentW = portW - margin * 2;

    for (size_t li = pg.lineStart; li < pg.lineEnd; li++) {
        const Line& line = lines[li];
        int x = margin;

        // Justification: distribute extra space among spaces on non-last lines.
        // line.spaceCount is recorded when the line is broken (see
        // page_layout.h) — this used to re-walk every token on every line of
        // every frame to count them again.
        int extraPerSpace = 0;
        if (g_config.justify && !line.isLastInPara && line.totalWidth < contentW &&
            line.spaceCount > 0) {
            extraPerSpace = (contentW - line.totalWidth) / line.spaceCount;
        }

        for (int ti : line.tokenIndices) {
            const Token& tok = toks[ti];
            if (tok.type == TokenType::Space) {
                x += tok.width + extraPerSpace;
            } else {
                g_fontEngine.drawText(tok.text, (float)x, (float)y,
                                      theme.foreground, ptSize);
                x += tok.width;
            }
        }
        y += lh;
        if (y + lh > portH - margin) break;
    }

    // Bookmark ribbon. Bookmarks are per-spread, so the ribbon shows on the
    // verso (even) page of a bookmarked spread.
    if ((pageIndex % 2) == 0 && hasBookmark(curChapter_, pageIndex / 2)) {
        C2D_DrawTriangle((float)(portW - 16), 0, C2D_Color32(255,50,50,255),
                         (float)(portW),      0, C2D_Color32(255,50,50,255),
                         (float)(portW),     16, C2D_Color32(255,50,50,255), 0.8f);
    }
}

//-----------------------------------------------------------------------------
// Render a PDF page
//-----------------------------------------------------------------------------
void ReaderView::renderPdfPage(C3D_RenderTarget* target, int pageIndex,
                                int portW, int portH, const Theme& theme) {
    const RasterisedPage* pg = g_pdfRenderer.getPage(pageIndex, portW, portH, isNew3DS_);
    if (!pg) return;

    // Upload RGBA pixmap as a GPU texture and draw it
    // We create a temporary C3D_Tex from the RGBA data
    C3D_Tex tex;
    if (!C3D_TexInit(&tex, pg->width, pg->height, GPU_RGBA8)) return;
    memcpy(tex.data, pg->rgba.data(), pg->rgba.size());
    GSPGPU_FlushDataCache(tex.data, pg->rgba.size());

    C2D_Image img;
    img.tex = &tex;
    Tex3DS_SubTexture sub{ (u16)pg->width, (u16)pg->height, 0.0f, 1.0f, 1.0f, 0.0f };
    img.subtex = &sub;

    int panX = g_pdfRenderer.panX();
    int panY = g_pdfRenderer.panY();
    C2D_DrawImageAt(img, (float)panX, (float)panY, 0.5f, nullptr,
                    (float)pg->width, (float)pg->height);

    C3D_TexDelete(&tex);
}

//-----------------------------------------------------------------------------
// Page metadata: "P. N / T" label and chapter name
//-----------------------------------------------------------------------------
void ReaderView::renderPageMeta(C3D_RenderTarget* target, int pageIndex,
                                 int portW, int portH, const Theme& theme) {
    int totalPages = isFixedLayout_ ? g_pdfRenderer.pageCount()
                                    : (int)g_layout.pageCount();

    // A spread's recto index is spread*2+1, which does not exist whenever the
    // chapter's page count is odd — exactly the spread you land on when paging
    // backward into the end of a chapter (loadChapter(..., landOnLastSpread)).
    // renderReflowPage already no-ops for an out-of-range pageIndex and leaves
    // that side blank; this label must agree, or it reports a page number
    // ("P. 6 / 5") for a page that was never laid out.
    if (pageIndex < 0 || pageIndex >= totalPages) return;

    char buf[64];
    snprintf(buf, sizeof(buf), "P. %d / %d", pageIndex + 1, totalPages);
    int textW = g_fontEngine.textWidth(buf, 11);
    g_fontEngine.drawText(buf, (float)(portW - textW - 4), (float)(portH - 16),
                          theme.foreground, 11);
}

//-----------------------------------------------------------------------------
// Progress bar: thin line along the spine edge of the bottom screen
//-----------------------------------------------------------------------------
void ReaderView::renderProgressBar(C3D_RenderTarget* target, const Theme& theme) {
    float ratio = progressRatio();
    float barW  = (float)PORTRAIT_BOT_W * ratio;
    // Thin bar at y=0 (spine edge in portrait = top of framebuffer)
    C2D_DrawRectSolid(0, 0, 0.9f, barW, 3, theme.progress_bar);
}

//-----------------------------------------------------------------------------
// Toast
//-----------------------------------------------------------------------------
void ReaderView::renderToast(C3D_RenderTarget* target, const Theme& theme) {
    if (toastMsg_.empty() || toastFrames_ <= 0) return;
    u8 alpha = toastFrames_ > 30 ? 220 : (u8)(toastFrames_ * 220 / 30);
    float tw = (float)g_fontEngine.textWidth(toastMsg_, 13);
    float tx = (PORTRAIT_BOT_W - tw) / 2.0f;
    C2D_DrawRectSolid(tx - 4, PORTRAIT_TOP_H - 36, 0.8f, tw + 8, 22,
                      C2D_Color32(0, 0, 0, alpha));
    // Depth must be >= the box above (0.8f), or citro2d's GEQUAL depth test
    // culls the glyphs behind their own background — same bug as the overlay
    // panel (see font_engine.h drawText comment).
    g_fontEngine.drawText(toastMsg_, tx, PORTRAIT_TOP_H - 33,
                          C2D_Color32(255, 255, 255, alpha), 13, 0.85f);
}

void ReaderView::setToast(const std::string& msg) {
    toastMsg_   = msg;
    toastFrames_ = 180;  // 3 seconds at 60 fps
}

//-----------------------------------------------------------------------------
// Navigation
//-----------------------------------------------------------------------------
bool ReaderView::nextSpread() {
    if (spread_ < totalSpreads_ - 1) { spread_++; return true; }

    // At the end of the chapter: queue a roll into the next one. Actually
    // loading it is deferred — see "Deferred layout work" in reader_view.h —
    // since it means FreeType layout and GPU texture-atlas flushes, which
    // must not happen inside the open frame this is called from.
    if (!isFixedLayout_ && curChapter_ + 1 < chapterCount_) {
        pendingChapterIndex_     = curChapter_ + 1;
        pendingLandOnLastSpread_ = false;
    }
    return false;
}

bool ReaderView::prevSpread() {
    if (spread_ > 0) { spread_--; return true; }

    // At the start of the chapter: queue a roll back into the previous one,
    // landing on its final spread so reading backwards is continuous.
    if (!isFixedLayout_ && curChapter_ > 0) {
        pendingChapterIndex_     = curChapter_ - 1;
        pendingLandOnLastSpread_ = true;
    }
    return false;
}

void ReaderView::goToPosition(int chapter, int spread) {
    if (isFixedLayout_) {
        spread_ = std::max(0, std::min(totalSpreads_ - 1, spread));
        return;
    }
    if (chapter != curChapter_) {
        // Deferred — loading a different chapter is the same expensive,
        // GPU-flushing work as a chapter-boundary page turn.
        pendingChapterIndex_     = chapter;
        pendingLandOnLastSpread_ = false;
        pendingGotoSpread_       = spread;
        return;
    }
    spread_ = std::max(0, std::min(totalSpreads_ - 1, spread));
}

//-----------------------------------------------------------------------------
// servicePendingWork — must be called between frames (no open C3D frame).
// See "Deferred layout work" in reader_view.h.
//-----------------------------------------------------------------------------
void ReaderView::servicePendingWork() {
    if (pendingChapterIndex_ != PENDING_NONE) {
        int  chapter   = pendingChapterIndex_;
        bool landLast  = pendingLandOnLastSpread_;
        int  gotoSpr   = pendingGotoSpread_;
        pendingChapterIndex_ = PENDING_NONE;
        pendingGotoSpread_   = -1;
        if (loadChapter(chapter, landLast) && gotoSpr >= 0)
            spread_ = std::max(0, std::min(totalSpreads_ - 1, gotoSpr));
    } else if (pendingRelayout_) {
        relayoutCurrentChapter();
    }
    pendingRelayout_ = false;
}

//-----------------------------------------------------------------------------
// Input update
//-----------------------------------------------------------------------------
bool ReaderView::update(u32 keysDown, u32 keysHeld,
                        const touchPosition& touch, bool touchDown, bool touchReleased,
                        const GyroHandler& gyro) {
    bool navigated = false;

    // Page-turn controls. nextSpread()/prevSpread() return false when the
    // turn crossed a chapter boundary and was queued rather than applied
    // immediately (see reader_view.h) — in that case there is nothing to
    // report as "navigated" yet, so saveProgress()/TTS in main.cpp don't fire
    // on stale (pre-load) state. servicePendingWork() handles that once the
    // deferred load actually completes.
    //
    // Reflowable (EPUB/TXT): Up = prev spread, Down = next spread. Left/Right
    // (D-Pad and L/R shoulder) are deliberately inert now — requested change
    // from the L/R + D-Pad-Left/Right scheme, not yet verified on hardware.
    //
    // PDF fixed-layout keeps the old L/R shoulder scheme instead, since
    // Up/Down there are already the (held-key) zoom-pan controls below and
    // binding page-turn to the same keys would fight with panning.
    if (!isFixedLayout_) {
        if (keysDown & KEY_DDOWN) navigated |= nextSpread();
        if (keysDown & KEY_DUP)   navigated |= prevSpread();
    } else {
        // Fixed layout keeps the L/R shoulder scheme — but not while B is
        // held. L+R+B is the app-exit combo (see main.cpp), and a PDF reader
        // that jumped two pages on the way out would save the wrong position
        // as its parting act.
        if (!(keysHeld & KEY_B)) {
            if (keysDown & KEY_R) navigated |= nextSpread();
            if (keysDown & KEY_L) navigated |= prevSpread();
        }
    }

    // D-Pad Up/Down: fine scroll for PDF zoom
    if (isFixedLayout_ && g_pdfRenderer.zoomEnabled()) {
        if (keysHeld & KEY_DUP)   g_pdfRenderer.pan(0, -4);
        if (keysHeld & KEY_DDOWN) g_pdfRenderer.pan(0,  4);
        // C-Stick fine-scroll (design doc §11.7) is deliberately DISABLED for
        // now. Smooth within-page scrolling is only worth having once a page
        // can be produced quickly; while chapter layout still costs seconds,
        // a finer-grained scroll control just gives the user more ways to
        // trigger the stall. Re-enable once the page-turn performance work
        // lands — the pan() calls it needs are still here.
    }

    // Touch gestures
    //
    // hidTouchRead() reports the bottom screen's NATIVE LANDSCAPE coordinates:
    // px in 0..319 across the physical width, py in 0..239 down the physical
    // height. The console is held rotated 90° anti-clockwise, and the reader
    // draws into a 240×320 portrait space that setupPortraitProjection() then
    // rotates a further 180°. So the axis that runs across the *page width* is
    // py, and the axis that runs *down the page* is px — swapped relative to
    // the hardware, and both inverted by that 180° rotation.
    //
    // TouchHandler works entirely in portrait space (it splits left/right at
    // BOT_MID_X = 120 of 240). Converting once here, at the boundary, keeps
    // that assumption true and is the same transform the overlay will need if
    // it is ever converted off its landscape coordinates.
    //
    // Previously the raw landscape `touch` was passed straight through, so the
    // tap left/right split fell at 120 of 320 — 37% across, along the axis that
    // runs down the page — and swipe detection measured vertical finger travel.
    touchPosition portraitTouch{};
    if (touch.px != 0 || touch.py != 0) {
        portraitTouch.px = (u16)(PORTRAIT_BOT_W - 1 - touch.py);   // across page width, 0..239
        portraitTouch.py = (u16)(PORTRAIT_BOT_H - 1 - touch.px);   // down the page,     0..319
    }

    static touchPosition lastTouch{};
    TouchGesture gesture = s_touch.update(portraitTouch, lastTouch, touchDown, touchReleased);
    lastTouch = portraitTouch;

    switch (gesture) {
        case TouchGesture::SwipeLeft:
        case TouchGesture::TapRight:  navigated |= nextSpread(); break;
        case TouchGesture::SwipeRight:
        case TouchGesture::TapLeft:   navigated |= prevSpread(); break;
        default: break;
    }

    // PDF: double-tap to toggle zoom (simplified: detect rapid tap)
    // Full double-tap detection omitted for brevity

    return navigated;
}

//-----------------------------------------------------------------------------
// Bookmarks
//-----------------------------------------------------------------------------
void ReaderView::toggleBookmark() {
    Bookmark bm{ curChapter_, spread_ };
    auto it = std::find(bookmarks_.begin(), bookmarks_.end(), bm);
    if (it != bookmarks_.end()) bookmarks_.erase(it);
    else                        bookmarks_.push_back(bm);
}

bool ReaderView::hasBookmark(int chapter, int spread) const {
    Bookmark bm{ chapter, spread };
    return std::find(bookmarks_.begin(), bookmarks_.end(), bm) != bookmarks_.end();
}

//-----------------------------------------------------------------------------
// Auto-scroll: called once per frame with speed in pages/min
//-----------------------------------------------------------------------------
void ReaderView::tickAutoScroll(int pagesPerMin) {
    if (pagesPerMin <= 0) return;
    // 60 fps; pages/min → frames per page = 3600 / pagesPerMin
    int framesPerPage = 3600 / std::max(1, pagesPerMin);
    autoScrollFrameCounter_++;
    if (autoScrollFrameCounter_ >= framesPerPage) {
        autoScrollFrameCounter_ = 0;
        nextSpread();
    }
}

//-----------------------------------------------------------------------------
// Save progress
//-----------------------------------------------------------------------------
void ReaderView::saveProgress(const std::string& path) {
    if (path.empty()) return;

    BookProgress bp;
    bp.path          = path;
    bp.chapter       = curChapter_;
    bp.spread        = spread_;
    bp.chapter_count = chapterCount_;
    bp.scroll_offset = 0;
    bp.bookmarks     = bookmarks_;

    // In-memory only. The SD write is batched behind ProgressStore's dirty
    // flag — see the write-behind note in progress.h and flushProgress() in
    // main.cpp. This used to call g_progress.save() straight through, on every
    // page turn.
    if (path != progressPath_) {
        progressPath_ = path;
        progressKey_  = ProgressStore::keyFor(path);
    }
    g_progress.set(progressKey_, bp);
}

//-----------------------------------------------------------------------------
// Current page text (for TTS)
//-----------------------------------------------------------------------------
std::string ReaderView::currentPageText(bool leftPage) const {
    if (isFixedLayout_) return "";  // PDF raster mode: no text
    int pageIndex = leftPage ? spread_ * 2 : spread_ * 2 + 1;
    if (pageIndex >= (int)g_layout.pageCount()) return "";

    const Page& pg = g_layout.page(pageIndex);
    const auto& toks = g_layout.tokens();
    const auto& lines = g_layout.lines();
    std::string out;
    for (size_t li = pg.lineStart; li < pg.lineEnd; li++) {
        for (int ti : lines[li].tokenIndices)
            out += toks[ti].text;
        out += ' ';
    }
    return out;
}
