#pragma once
#include "common.h"
#include <string>
#include <vector>
#include <functional>

//-----------------------------------------------------------------------------
// RasterisedPage — one decoded PDF page as an RGBA framebuffer
//-----------------------------------------------------------------------------
struct RasterisedPage {
    int            pageIndex  = -1;
    int            width      = 0;
    int            height     = 0;
    std::vector<uint8_t> rgba;   // width * height * 4 bytes, row-major

    bool valid() const { return pageIndex >= 0 && !rgba.empty(); }
    void clear()       { pageIndex = -1; rgba.clear(); }
};

//-----------------------------------------------------------------------------
// PdfRenderer — design doc §8.3
//
// Wraps MuPDF (fz_context / fz_document) with a ring buffer of 3–5 pages
// for instant adjacent navigation.
//
// • Each page rasterised at 72 DPI, scaled to fit 240×400 or 240×320 px
// • Aspect ratio maintained with letterboxing (black bars)
// • New 3DS: background pre-render thread uses APP_CPU_USAGE_30
// • Touch zoom: double-tap toggles fit↔2× zoom; single-finger drag to pan
//-----------------------------------------------------------------------------
class PdfRenderer {
public:
    static constexpr int RING_SIZE      = 5;   // pages cached in ring buffer
    static constexpr int RENDER_DPI     = 72;
    static constexpr int ZOOM_SCALE     = 2;   // 2× zoom factor

    PdfRenderer();
    ~PdfRenderer();

    // Open a PDF document. Returns false on error.
    bool open(const std::string& path);
    void close();

    int  pageCount() const { return pageCount_; }

    // Request a page. If not in ring buffer, rasterise synchronously.
    // On New 3DS, also triggers background pre-render of adjacent pages.
    const RasterisedPage* getPage(int pageIndex,
                                  int targetW, int targetH,
                                  bool isNewHardware);

    // Zoom state (for touch zoom)
    bool  zoomEnabled() const  { return zoomEnabled_; }
    void  toggleZoom()         { zoomEnabled_ = !zoomEnabled_; panX_ = panY_ = 0; }
    void  pan(int dx, int dy)  { panX_ += dx; panY_ += dy; clampPan(); }
    int   panX() const         { return panX_; }
    int   panY() const         { return panY_; }

    // Text extraction (for search — not available in raster-only mode)
    bool  canExtractText() const { return false; }

private:
    // MuPDF context/document handles — forward declared to avoid pulling
    // MuPDF headers into every TU that includes this header.
    struct MuPdfContext;
    MuPdfContext* ctx_ = nullptr;

    int pageCount_ = 0;
    bool zoomEnabled_ = false;
    int  panX_ = 0, panY_ = 0;
    int  targetW_ = 0, targetH_ = 0;

    // Ring buffer
    RasterisedPage ring_[RING_SIZE];
    int            ringHead_ = 0;

    // Background thread (New 3DS)
    Thread         bgThread_;
    bool           bgRunning_ = false;
    LightLock      bgLock_;
    int            bgNextPage_ = -1;

    RasterisedPage* findInRing(int pageIndex);
    RasterisedPage* allocRingSlot();
    bool            rasterisePage(int pageIndex, int targetW, int targetH, RasterisedPage& out);
    void            scaleWithLetterbox(const uint8_t* src, int srcW, int srcH,
                                       uint8_t* dst, int dstW, int dstH);
    void            clampPan();

    static void     bgThreadFunc(void* arg);
};

extern PdfRenderer g_pdfRenderer;
