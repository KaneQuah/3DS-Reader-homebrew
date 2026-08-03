#include "formats/pdf_renderer.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>

// Build flag: ENABLE_PDF=0 compiles this class to no-op stubs so the project
// links without MuPDF (which is not available as a devkitPro portlib and must
// be cross-compiled from source).
#ifndef ENABLE_PDF
#define ENABLE_PDF 0
#endif

#if ENABLE_PDF
// MuPDF headers — available once libmupdf is compiled for devkitARM.
// See design doc §8.3 open question: verify clean ARM11 compilation.
#include <mupdf/fitz.h>
#endif

PdfRenderer g_pdfRenderer;

//-----------------------------------------------------------------------------
// MuPDF context wrapper
//-----------------------------------------------------------------------------
struct PdfRenderer::MuPdfContext {
#if ENABLE_PDF
    fz_context*  ctx  = nullptr;
    fz_document* doc  = nullptr;
#endif
};

PdfRenderer::PdfRenderer() {
    LightLock_Init(&bgLock_);
}

PdfRenderer::~PdfRenderer() { close(); }

bool PdfRenderer::open(const std::string& path) {
#if !ENABLE_PDF
    // PDF support not compiled in — reject the file so the caller can show
    // an "unsupported format" message instead of crashing.
    (void)path;
    close();
    return false;
#else
    close();
    ctx_ = new MuPdfContext();
    ctx_->ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx_->ctx) { delete ctx_; ctx_ = nullptr; return false; }

    fz_register_document_handlers(ctx_->ctx);

    fz_try(ctx_->ctx) {
        ctx_->doc = fz_open_document(ctx_->ctx, path.c_str());
    } fz_catch(ctx_->ctx) {
        fz_drop_context(ctx_->ctx);
        delete ctx_; ctx_ = nullptr;
        return false;
    }

    pageCount_ = fz_count_pages(ctx_->ctx, ctx_->doc);
    return pageCount_ > 0;
#endif
}

void PdfRenderer::close() {
    bgRunning_ = false;
    if (bgThread_) { threadJoin(bgThread_, U64_MAX); threadFree(bgThread_); bgThread_ = nullptr; }
    for (auto& r : ring_) r.clear();
    ringHead_ = 0;
    if (ctx_) {
#if ENABLE_PDF
        if (ctx_->doc) fz_drop_document(ctx_->ctx, ctx_->doc);
        if (ctx_->ctx) fz_drop_context(ctx_->ctx);
#endif
        delete ctx_; ctx_ = nullptr;
    }
    pageCount_ = 0;
    zoomEnabled_ = false;
    panX_ = panY_ = 0;
}

//-----------------------------------------------------------------------------
// Ring buffer
//-----------------------------------------------------------------------------
RasterisedPage* PdfRenderer::findInRing(int pageIndex) {
    for (auto& r : ring_)
        if (r.pageIndex == pageIndex) return &r;
    return nullptr;
}

RasterisedPage* PdfRenderer::allocRingSlot() {
    RasterisedPage* slot = &ring_[ringHead_];
    ringHead_ = (ringHead_ + 1) % RING_SIZE;
    return slot;
}

//-----------------------------------------------------------------------------
// Rasterise a single page using MuPDF
//-----------------------------------------------------------------------------
bool PdfRenderer::rasterisePage(int pageIndex, int targetW, int targetH,
                                 RasterisedPage& out) {
#if !ENABLE_PDF
    (void)pageIndex; (void)targetW; (void)targetH; (void)out;
    return false;
#else
    if (!ctx_ || pageIndex < 0 || pageIndex >= pageCount_) return false;

    fz_page*   page   = nullptr;
    fz_pixmap* pixmap = nullptr;

    fz_try(ctx_->ctx) {
        page = fz_load_page(ctx_->ctx, ctx_->doc, pageIndex);

        // Compute scale to fit targetW × targetH at 72 DPI (base)
        fz_rect  bounds = fz_bound_page(ctx_->ctx, page);
        float    pageW  = bounds.x1 - bounds.x0;
        float    pageH  = bounds.y1 - bounds.y0;
        float    scaleX = (float)targetW / pageW;
        float    scaleY = (float)targetH / pageH;
        float    scale  = std::min(scaleX, scaleY);
        if (zoomEnabled_) scale *= ZOOM_SCALE;

        fz_matrix transform = fz_scale(scale, scale);
        fz_rect   bbox      = fz_transform_rect(bounds, transform);
        fz_irect  ibbox     = fz_round_rect(bbox);

        fz_colorspace* cs = fz_device_rgb(ctx_->ctx);
        pixmap = fz_new_pixmap_with_bbox(ctx_->ctx, cs, ibbox, nullptr, 1);
        fz_clear_pixmap_with_value(ctx_->ctx, pixmap, 255);

        fz_device* dev = fz_new_draw_device(ctx_->ctx, transform, pixmap);
        fz_run_page(ctx_->ctx, page, dev, fz_identity, nullptr);
        fz_close_device(ctx_->ctx, dev);
        fz_drop_device(ctx_->ctx, dev);

        // Copy pixmap (RGBA) into ring buffer, scaled/letterboxed to targetW×targetH
        int pmW = fz_pixmap_width(ctx_->ctx, pixmap);
        int pmH = fz_pixmap_height(ctx_->ctx, pixmap);

        out.width     = targetW;
        out.height    = targetH;
        out.pageIndex = pageIndex;
        out.rgba.assign(targetW * targetH * 4, 0);  // black background

        scaleWithLetterbox(fz_pixmap_samples(ctx_->ctx, pixmap), pmW, pmH,
                           out.rgba.data(), targetW, targetH);
    } fz_always(ctx_->ctx) {
        if (pixmap) fz_drop_pixmap(ctx_->ctx, pixmap);
        if (page)   fz_drop_page(ctx_->ctx, page);
    } fz_catch(ctx_->ctx) {
        return false;
    }
    return true;
#endif
}

//-----------------------------------------------------------------------------
// Nearest-neighbour scale + letterbox into dst (black fill)
//-----------------------------------------------------------------------------
void PdfRenderer::scaleWithLetterbox(const uint8_t* src, int srcW, int srcH,
                                      uint8_t* dst, int dstW, int dstH) {
    // Compute centred destination rect
    float scaleX = (float)dstW / srcW;
    float scaleY = (float)dstH / srcH;
    float scale  = std::min(scaleX, scaleY);
    int   drawW  = (int)(srcW * scale);
    int   drawH  = (int)(srcH * scale);
    int   offX   = (dstW - drawW) / 2;
    int   offY   = (dstH - drawH) / 2;

    for (int y = 0; y < drawH; y++) {
        int sy = (int)(y / scale);
        if (sy >= srcH) sy = srcH - 1;
        for (int x = 0; x < drawW; x++) {
            int sx = (int)(x / scale);
            if (sx >= srcW) sx = srcW - 1;
            int di = ((offY + y) * dstW + (offX + x)) * 4;
            int si = (sy * srcW + sx) * 4;
            dst[di+0] = src[si+0];  // R
            dst[di+1] = src[si+1];  // G
            dst[di+2] = src[si+2];  // B
            dst[di+3] = src[si+3];  // A
        }
    }
}

//-----------------------------------------------------------------------------
// getPage — main access point
//-----------------------------------------------------------------------------
const RasterisedPage* PdfRenderer::getPage(int pageIndex, int targetW, int targetH,
                                            bool isNewHardware) {
    targetW_ = targetW;
    targetH_ = targetH;

    // Check ring buffer
    RasterisedPage* cached = findInRing(pageIndex);
    if (cached && cached->width == targetW && cached->height == targetH)
        return cached;

    // Rasterise synchronously
    RasterisedPage* slot = allocRingSlot();
    if (!rasterisePage(pageIndex, targetW, targetH, *slot)) return nullptr;

    // New 3DS: kick off background pre-render of adjacent pages
    if (isNewHardware && !bgRunning_) {
        bgNextPage_  = pageIndex + 1;
        bgRunning_   = true;
        bgThread_    = nullptr;
        s32 prio;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        threadCreate(bgThreadFunc, this, 16 * 1024, prio + 1, 1 /*core 1*/, true);
    }

    return slot;
}

void PdfRenderer::bgThreadFunc(void* arg) {
    PdfRenderer* self = static_cast<PdfRenderer*>(arg);
    int page = self->bgNextPage_;
    if (page < self->pageCount_ && !self->findInRing(page)) {
        RasterisedPage* slot = self->allocRingSlot();
        self->rasterisePage(page, self->targetW_, self->targetH_, *slot);
    }
    self->bgRunning_ = false;
}

void PdfRenderer::clampPan() {
    // Clamp pan so the page stays on screen (simplified)
    int maxPan = zoomEnabled_ ? 100 : 0;
    if (panX_ < -maxPan) panX_ = -maxPan;
    if (panX_ >  maxPan) panX_ =  maxPan;
    if (panY_ < -maxPan) panY_ = -maxPan;
    if (panY_ >  maxPan) panY_ =  maxPan;
}
