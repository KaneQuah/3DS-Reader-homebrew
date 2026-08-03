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

    // Render a UTF-8 string at portrait canvas position (px, py) in colour `colour`
    void drawText(const std::string& utf8, float px, float py, u32 colour, int ptSize);

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

    std::unordered_map<CacheKey, GlyphInfo, CacheKeyHash> cache_;
    int         cacheOrder_[GLYPH_CACHE_MAX];  // LRU eviction order (indices)
    int         cacheCount_ = 0;

    // Texture atlas
    C3D_Tex     atlasTex_;
    bool        atlasInit_  = false;
    int         atlasX_     = 0;  // next insertion point
    int         atlasY_     = 0;
    int         atlasRowH_  = 0;  // tallest glyph in current row

    // Helpers
    bool        setSize(int ptSize);
    GlyphInfo*  getOrLoad(uint32_t codepoint, int ptSize);
    void        evictOldest();
    bool        atlasInsert(int w, int h, int& outX, int& outY);
    void        uploadGlyph(int atlasX, int atlasY, FT_Bitmap& bmp);
};

extern FontEngine g_fontEngine;
