#pragma once
#include "common.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <string>
#include <unordered_map>
#include <vector>

//-----------------------------------------------------------------------------
// GlyphInfo — one cached glyph entry
//-----------------------------------------------------------------------------
struct GlyphInfo {
    int      bearingX;   // horizontal offset from pen position (pixels)
    int      bearingY;   // vertical offset from baseline (pixels)
    int      advance;    // pen advance width (pixels)
    int      width;      // bitmap width
    int      height;     // bitmap height

    // Position within the texture atlas (in pixels)
    int      atlasX;
    int      atlasY;
};

//-----------------------------------------------------------------------------
// FontEngine — FreeType loader + 512-glyph cache per font/size combination
//
// Design doc §9.1–9.2:
//   • Scans /3DS/BookReader/fonts/ for TTF/OTF
//   • Built-in fallback font embedded as romfs asset
//   • Glyph cache of up to 512 glyphs per font/size
//   • Uploads bitmaps to a GPU texture atlas via citro2d
//   • Renders textured quads for each glyph via C2D_DrawImageAt
//-----------------------------------------------------------------------------
class FontEngine {
public:
    static constexpr int GLYPH_CACHE_MAX   = 512;
    static constexpr int ATLAS_SIZE        = 512;  // texture atlas dimensions (square, pot)
    static constexpr int GLYPH_PAD         = 1;    // 1-pixel padding between glyphs

    FontEngine();
    ~FontEngine();

    bool init();
    void exit();

    // Load a font by filename from /3DS/BookReader/fonts/, or "built-in" for embedded font.
    // Returns false on failure; falls back to built-in automatically.
    bool loadFont(const std::string& filename);

    // Measure the pixel width of a UTF-32 codepoint at the current size
    int  glyphAdvance(uint32_t codepoint);

    // Measure the pixel width of a UTF-8 string
    int  textWidth(const std::string& utf8, int ptSize);

    // Render a UTF-8 string at portrait canvas position (px, py) in colour `colour`.
    // `depth` follows the same convention as C2D_Draw*'s z argument (citro2d's
    // depth test passes GEQUAL, so higher values draw in front). Callers layering
    // text over their own background rects must pass a depth at or above that
    // rect's, or the glyphs are depth-culled and nothing appears — see overlay.cpp.
    void drawText(const std::string& utf8, float px, float py, u32 colour, int ptSize,
                  float depth = 0.5f);

    // Current font metrics at given size
    int  ascender(int ptSize);
    int  descender(int ptSize);
    int  lineHeight(int ptSize);

    // Enumerate fonts in /3DS/BookReader/fonts/
    std::vector<std::string> listFonts() const;

private:
    struct CacheKey {
        uint32_t codepoint;
        int      ptSize;
        bool operator==(const CacheKey& o) const {
            return codepoint == o.codepoint && ptSize == o.ptSize;
        }
    };
    struct CacheKeyHash {
        size_t operator()(const CacheKey& k) const {
            return std::hash<uint64_t>()((uint64_t)k.codepoint << 16 | k.ptSize);
        }
    };

    FT_Library  ftLib_      = nullptr;
    FT_Face     ftFace_     = nullptr;

    // Size currently programmed into ftFace_, so setSize() can skip the
    // FT_Set_Char_Size round trip when nothing has changed. -1 means unknown
    // (no face loaded yet, or the face was just replaced).
    //
    // This matters far more than it looks: ascender() called setSize()
    // unconditionally, and drawText() calls ascender() once per call, and
    // renderReflowPage() calls drawText() once per *token*. A full spread of
    // prose is ~600 tokens, so a steady 60 fps was re-programming the
    // FreeType size ~36,000 times a second to get back a number that had not
    // changed since the last font-size change.
    int         curSize_    = -1;

    // Per-size font metrics, memoised. Indexed by point size directly; sizes
    // outside the table fall through to a live FreeType query.
    static constexpr int MAX_PT_SIZE = 64;
    struct SizeMetrics {
        int  ascender   = 0;
        int  descender  = 0;
        int  lineHeight = 0;
        bool valid      = false;
    };
    SizeMetrics metrics_[MAX_PT_SIZE + 1];
    const SizeMetrics& metricsFor(int ptSize);

    // Direct-mapped ASCII glyph cache for the size most recently requested.
    //
    // getOrLoad()'s unordered_map lookup (build a CacheKey, hash it, walk the
    // bucket, compare) runs once per character of every measurement and every
    // draw — twice over the whole chapter during layout, then again for every
    // visible glyph on every frame. Latin prose is ~99% ASCII, so a 128-entry
    // pointer table keyed on the current size removes almost all of that.
    //
    // Storing raw pointers is safe: std::unordered_map is node-based, so
    // pointers to mapped values stay valid across insertion and rehashing.
    // They are only invalidated when the map itself is cleared, which is what
    // resetGlyphCaches() exists to keep in sync.
    const GlyphInfo* asciiCache_[128] = {};
    int              asciiCacheSize_  = -1;

    /// Drop the glyph cache, the atlas cursor and the ASCII fast path
    /// together. Every place that clears one must clear all three, or the
    /// fast path hands out pointers into a map that no longer has them.
    void        resetGlyphCaches();

    std::unordered_map<CacheKey, GlyphInfo, CacheKeyHash> cache_;
    int         cacheOrder_[GLYPH_CACHE_MAX];  // LRU eviction order (indices)
    int         cacheCount_ = 0;

    // Texture atlas
    C3D_Tex     atlasTex_;
    bool        atlasInit_  = false;
    int         atlasX_     = 0;  // next insertion point
    int         atlasY_     = 0;
    int         atlasRowH_  = 0;  // tallest glyph in current row

    // uploadGlyph() used to call C3D_TexFlush (a full 512x512 cache flush)
    // after every single new glyph. A chapter's worth of layout touches
    // every unique (codepoint, size) pair exactly once — maybe ~100 for a
    // page of English prose — so that was up to ~100 full-atlas flushes
    // serialised into the ~800ms/chapter layout cost the handoff measured.
    // Glyphs are batched instead: uploadGlyph() only marks the atlas dirty,
    // and textWidth()/drawText() flush once each, after all the glyphs *that
    // call* needed have been uploaded but before returning — i.e. still
    // strictly before anything the caller does with those glyphs, just no
    // longer once per character.
    bool        atlasDirty_ = false;
    void        flushAtlasIfDirty();

    // Helpers
    bool        setSize(int ptSize);

    /// Glyph lookup with the ASCII fast path in front of it. The returned
    /// pointer is owned by cache_ and stays valid until the cache is reset.
    const GlyphInfo* getOrLoad(uint32_t codepoint, int ptSize);

    /// The slow path: unordered_map lookup, then a FreeType render and atlas
    /// upload on a miss. Called only by getOrLoad().
    const GlyphInfo* loadGlyphUncached(uint32_t codepoint, int ptSize);
    void        evictOldest();
    bool        atlasInsert(int w, int h, int& outX, int& outY);
    void        uploadGlyph(int atlasX, int atlasY, FT_Bitmap& bmp);
};

extern FontEngine g_fontEngine;
