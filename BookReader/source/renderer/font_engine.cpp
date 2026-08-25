#include "renderer/font_engine.h"
#include <3ds.h>
#include <citro2d.h>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <algorithm>

FontEngine g_fontEngine;

//-----------------------------------------------------------------------------
// UTF-8 decode: returns the next Unicode codepoint and advances `p`.
// Returns 0 on end-of-string.
//-----------------------------------------------------------------------------
static uint32_t utf8Next(const char*& p) {
    uint8_t c = (uint8_t)*p;
    if (c == 0)   return 0;
    p++;
    if (c < 0x80) return c;
    uint32_t cp;
    int extra;
    if      ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else return 0xFFFD; // invalid
    while (extra-- > 0) {
        uint8_t b = (uint8_t)*p++;
        if ((b & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (b & 0x3F);
    }
    return cp;
}

//-----------------------------------------------------------------------------
// FontEngine
//-----------------------------------------------------------------------------
FontEngine::FontEngine() {
    memset(&atlasTex_, 0, sizeof(atlasTex_));
}

FontEngine::~FontEngine() { exit(); }

bool FontEngine::init() {
    if (FT_Init_FreeType(&ftLib_) != 0) return false;

    // Allocate texture atlas in linear RAM (required by GPU).
    //
    // GPU_A8, not GPU_L8: FreeType gives us 8-bit coverage, which we want to
    // act as *alpha* so the glyph is transparent where there is no ink and the
    // tint colour supplies the RGB. With L8 the coverage would become luminance
    // with alpha forced to 1.0, painting opaque grey boxes over the page.
    if (!C3D_TexInit(&atlasTex_, ATLAS_SIZE, ATLAS_SIZE, GPU_A8)) return false;
    C3D_TexSetFilter(&atlasTex_, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&atlasTex_, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    memset(atlasTex_.data, 0, ATLAS_SIZE * ATLAS_SIZE);
    atlasInit_ = true;

    // Tint mode must be Solid so the tint colour *replaces* the texture RGB
    // rather than multiplying with it (the A8 atlas has no meaningful RGB).
    // C2D_Init/C2D_Prepare must already have run — see initHardware().
    C2D_SetTintMode(C2D_TintSolid);

    // Load built-in font from romfs
    return loadFont("built-in");
}

void FontEngine::exit() {
    if (ftFace_)  { FT_Done_Face(ftFace_);     ftFace_ = nullptr; }
    if (ftLib_)   { FT_Done_FreeType(ftLib_);  ftLib_  = nullptr; }
    if (atlasInit_) { C3D_TexDelete(&atlasTex_); atlasInit_ = false; }
    resetGlyphCaches();
}

//-----------------------------------------------------------------------------
// resetGlyphCaches — see font_engine.h.
//
// The glyph map, the ASCII fast-path pointer table and the memoised size
// metrics all describe the same face at the same sizes, so they are only ever
// correct together. Clearing the map on its own would leave asciiCache_
// holding dangling pointers into freed nodes, which is a use-after-free that
// only shows up once the atlas happens to fill mid-chapter.
//-----------------------------------------------------------------------------
void FontEngine::resetGlyphCaches() {
    cache_.clear();
    cacheCount_     = 0;
    asciiCacheSize_ = -1;
    memset(asciiCache_, 0, sizeof(asciiCache_));
}

bool FontEngine::loadFont(const std::string& filename) {
    if (ftFace_) { FT_Done_Face(ftFace_); ftFace_ = nullptr; }
    resetGlyphCaches();
    // A new face invalidates both the programmed size and every cached metric.
    curSize_ = -1;
    for (auto& m : metrics_) m.valid = false;
    atlasX_ = atlasY_ = atlasRowH_ = 0;
    if (atlasInit_) memset(atlasTex_.data, 0, ATLAS_SIZE * ATLAS_SIZE);

    std::string path;
    if (filename == "built-in") {
        path = "romfs:/fonts/LiberationSerif-Regular.ttf";
    } else {
        path = std::string(FONTS_PATH) + "/" + filename;
    }

    if (FT_New_Face(ftLib_, path.c_str(), 0, &ftFace_) != 0) {
        // Fall back to built-in
        if (filename != "built-in")
            return loadFont("built-in");
        return false;
    }
    return true;
}

bool FontEngine::setSize(int ptSize) {
    if (!ftFace_) return false;
    // Already programmed — FT_Set_Char_Size would recompute the same scaling
    // factors and hand back the same metrics. See curSize_ in font_engine.h
    // for why skipping this is worth a branch.
    if (ptSize == curSize_) return true;
    // 3DS top screen DPI is approximately 96 dpi; bottom screen ~91 dpi.
    // We use 96 dpi as a practical constant.
    if (FT_Set_Char_Size(ftFace_, 0, ptSize * 64, 96, 96) != 0) {
        curSize_ = -1;
        return false;
    }
    curSize_ = ptSize;
    return true;
}

//-----------------------------------------------------------------------------
// metricsFor — memoised ascender/descender/line height for one point size.
//
// These three numbers are fixed for a given face and size, but were being
// re-derived from FreeType on every single call. lineHeight() in particular is
// asked for once per line during pagination and once per line during layout;
// ascender() once per drawText(), i.e. once per token per frame.
//-----------------------------------------------------------------------------
const FontEngine::SizeMetrics& FontEngine::metricsFor(int ptSize) {
    // Out-of-table sizes (and the no-face case) get a scratch entry that is
    // recomputed every time rather than cached — nothing in this app asks for
    // one, but silently indexing out of bounds would be much worse.
    static SizeMetrics scratch;
    SizeMetrics& m = (ptSize >= 0 && ptSize <= MAX_PT_SIZE) ? metrics_[ptSize]
                                                            : scratch;
    if (m.valid && &m != &scratch) return m;

    if (!setSize(ptSize)) {
        // Plausible fallbacks, matching what the old per-call code returned
        // when FreeType was unavailable.
        m.ascender   = ptSize;
        m.descender  = 2;
        m.lineHeight = ptSize + 4;
        m.valid      = false;   // retry once a face is available
        return m;
    }

    m.ascender   = static_cast<int>(  ftFace_->size->metrics.ascender  >> 6);
    m.descender  = static_cast<int>(-(ftFace_->size->metrics.descender >> 6));
    m.lineHeight = static_cast<int>(  ftFace_->size->metrics.height    >> 6);
    m.valid      = true;
    return m;
}

int FontEngine::ascender(int ptSize)   { return metricsFor(ptSize).ascender;   }
int FontEngine::descender(int ptSize)  { return metricsFor(ptSize).descender;  }
int FontEngine::lineHeight(int ptSize) { return metricsFor(ptSize).lineHeight; }

//-----------------------------------------------------------------------------
// Atlas insertion — simple shelf packing
//-----------------------------------------------------------------------------
bool FontEngine::atlasInsert(int w, int h, int& outX, int& outY) {
    if (w > ATLAS_SIZE || h > ATLAS_SIZE) return false;
    if (atlasX_ + w + GLYPH_PAD > ATLAS_SIZE) {
        // Move to next row
        atlasX_  = 0;
        atlasY_  += atlasRowH_ + GLYPH_PAD;
        atlasRowH_ = 0;
    }
    if (atlasY_ + h + GLYPH_PAD > ATLAS_SIZE) {
        // Atlas full — clear and start over (simplistic; LRU eviction is better)
        atlasX_ = atlasY_ = atlasRowH_ = 0;
        resetGlyphCaches();
        memset(atlasTex_.data, 0, ATLAS_SIZE * ATLAS_SIZE);
    }
    outX = atlasX_;
    outY = atlasY_;
    atlasX_ += w + GLYPH_PAD;
    if (h > atlasRowH_) atlasRowH_ = h;
    return true;
}

//-----------------------------------------------------------------------------
// Upload an L8 greyscale FreeType bitmap into the atlas texture
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
// 3DS GPU textures are NOT stored row-major. They are split into 8x8 tiles,
// and within each tile the pixels are in Morton (Z-order) sequence. Writing
// linearly (as this code previously did) scatters every glyph into noise.
//
// Byte offset of pixel (x, y) in a `texW`-wide 8-bit texture:
//   tile index      = (y/8) * (texW/8) + (x/8)
//   offset in tile  = Morton interleave of the low 3 bits of x and y
//-----------------------------------------------------------------------------
static inline unsigned tiledOffset(unsigned x, unsigned y, unsigned texW) {
    unsigned inX = x & 7, inY = y & 7;
    unsigned morton = 0;
    for (unsigned i = 0; i < 3; i++) {
        morton |= ((inX >> i) & 1u) << (2 * i);
        morton |= ((inY >> i) & 1u) << (2 * i + 1);
    }
    return ((y >> 3) * (texW >> 3) + (x >> 3)) * 64 + morton;
}

void FontEngine::uploadGlyph(int ax, int ay, FT_Bitmap& bmp) {
    uint8_t* data = reinterpret_cast<uint8_t*>(atlasTex_.data);

    for (unsigned row = 0; row < bmp.rows; row++) {
        const uint8_t* src = bmp.buffer + row * bmp.pitch;
        for (unsigned col = 0; col < bmp.width; col++) {
            unsigned px = (unsigned)ax + col;
            unsigned py = (unsigned)ay + row;
            if (px >= ATLAS_SIZE || py >= ATLAS_SIZE) continue;
            data[tiledOffset(px, py, ATLAS_SIZE)] = src[col];
        }
    }

    // Deferred — see the atlasDirty_ comment in font_engine.h. The caller
    // (textWidth()/drawText()) flushes once, after every glyph it needed has
    // been uploaded, instead of once per glyph here.
    atlasDirty_ = true;
}

//-----------------------------------------------------------------------------
// flushAtlasIfDirty — one GPU cache flush per batch of new glyph uploads,
// not one per glyph. Must be called before returning control to any code
// that might issue a draw call referencing glyphs uploaded since the last
// flush (textWidth() doesn't draw, but drawText() does, so both flush at
// their own end to keep that guarantee regardless of which one triggered
// the upload).
//-----------------------------------------------------------------------------
void FontEngine::flushAtlasIfDirty() {
    if (!atlasDirty_) return;
    C3D_TexFlush(&atlasTex_);
    atlasDirty_ = false;
}

//-----------------------------------------------------------------------------
// Glyph cache lookup / load
//
// getOrLoad() is on the hot path twice over: once per character while a
// chapter is laid out, and once per visible glyph on every rendered frame. The
// ASCII table in front of the hash map is what makes those two loops cheap;
// see asciiCache_ in font_engine.h.
//-----------------------------------------------------------------------------
const GlyphInfo* FontEngine::getOrLoad(uint32_t cp, int ptSize) {
    if (cp < 128) {
        if (ptSize != asciiCacheSize_) {
            // The table only ever describes one size. Switching size (page
            // text at 16pt, then the page-number label at 11pt) drops it and
            // refills lazily, which is still far cheaper than hashing every
            // character of the body text.
            memset(asciiCache_, 0, sizeof(asciiCache_));
            asciiCacheSize_ = ptSize;
        } else if (asciiCache_[cp]) {
            return asciiCache_[cp];
        }
    }

    const GlyphInfo* gi = loadGlyphUncached(cp, ptSize);

    // Re-check asciiCacheSize_: loadGlyphUncached() can fill the atlas, which
    // resets every cache and sets asciiCacheSize_ back to -1. Caching `gi`
    // then would store a pointer into a map entry that was just erased.
    if (gi && cp < 128 && ptSize == asciiCacheSize_)
        asciiCache_[cp] = gi;
    return gi;
}

const GlyphInfo* FontEngine::loadGlyphUncached(uint32_t cp, int ptSize) {
    CacheKey key{ cp, ptSize };
    auto it = cache_.find(key);
    if (it != cache_.end()) return &it->second;

    if (!setSize(ptSize)) return nullptr;

    FT_UInt glyphIdx = FT_Get_Char_Index(ftFace_, cp);
    if (glyphIdx == 0) glyphIdx = FT_Get_Char_Index(ftFace_, 0xFFFD); // replacement char

    if (FT_Load_Glyph(ftFace_, glyphIdx, FT_LOAD_RENDER) != 0) return nullptr;

    FT_GlyphSlot slot = ftFace_->glyph;
    int w = slot->bitmap.width;
    int h = slot->bitmap.rows;

    GlyphInfo gi;
    gi.bearingX = slot->bitmap_left;
    gi.bearingY = slot->bitmap_top;
    gi.advance  = static_cast<int>(slot->advance.x >> 6);
    gi.width    = w;
    gi.height   = h;
    gi.atlasX   = 0;
    gi.atlasY   = 0;

    if (w > 0 && h > 0) {
        if (!atlasInsert(w, h, gi.atlasX, gi.atlasY)) return nullptr;
        uploadGlyph(gi.atlasX, gi.atlasY, slot->bitmap);
    }

    // Evict oldest if cache full
    if (cacheCount_ >= GLYPH_CACHE_MAX) evictOldest();

    // emplace() hands back an iterator to the stored node, so the value can be
    // returned without a second hash lookup.
    auto ins = cache_.emplace(key, gi);
    cacheCount_++;
    return &ins.first->second;
}

void FontEngine::evictOldest() {
    // Simple strategy: clear entire cache when full.
    // A production implementation would use an LRU list.
    resetGlyphCaches();
    atlasX_ = atlasY_ = atlasRowH_ = 0;
    if (atlasInit_) memset(atlasTex_.data, 0, ATLAS_SIZE * ATLAS_SIZE);
}

//-----------------------------------------------------------------------------
// Measure
//-----------------------------------------------------------------------------
int FontEngine::glyphAdvance(uint32_t cp) {
    const GlyphInfo* gi = getOrLoad(cp, 16); // use current font size — caller should call textWidth
    return gi ? gi->advance : 8;
}

int FontEngine::textWidth(const std::string& utf8, int ptSize) {
    int total = 0;
    const char* p = utf8.c_str();
    uint32_t cp;
    while ((cp = utf8Next(p)) != 0) {
        const GlyphInfo* gi = getOrLoad(cp, ptSize);
        if (gi) total += gi->advance;
    }
    flushAtlasIfDirty();
    return total;
}

//-----------------------------------------------------------------------------
// Draw a UTF-8 string at portrait canvas position (px, py).
// Portrait coordinate system: (0,0) = top-left of the page.
// C2D coordinates are in landscape framebuffer space; the projection matrix
// applied in reader_view.cpp maps portrait→landscape.
//-----------------------------------------------------------------------------
void FontEngine::drawText(const std::string& utf8, float px, float py, u32 colour, int ptSize,
                          float depth) {
    if (!atlasInit_) return;
    const SizeMetrics& sm = metricsFor(ptSize);
    float penX = px;
    float penY = py + sm.ascender;  // shift baseline down from top of line

    // The tint is the same for every glyph in the string, so build it once.
    // C2D_PlainImageTint fills four corner colours plus four blend factors;
    // doing that per character was ~600 redundant struct fills per spread per
    // frame.
    C2D_ImageTint tint;
    C2D_PlainImageTint(&tint, colour, 1.0f);

    // Likewise the image header: only the subtexture rectangle changes between
    // glyphs, and `img.subtex` points at `sub`, so both can be hoisted.
    C2D_Image         img;
    Tex3DS_SubTexture sub;
    img.tex    = &atlasTex_;
    img.subtex = &sub;

    const char* p = utf8.c_str();
    uint32_t cp;
    while ((cp = utf8Next(p)) != 0) {
        if (cp == '\n') { penX = px; penY += sm.lineHeight; continue; }

        const GlyphInfo* gi = getOrLoad(cp, ptSize);
        if (!gi) continue;

        if (gi->width > 0 && gi->height > 0) {
            float gx = penX + gi->bearingX;
            float gy = penY - gi->bearingY;

            // Normalised UVs. The atlas is addressed top-down (row 0 = top),
            // while texture V runs bottom-up, hence the 1.0 - y/size flip.
            sub.width  = (u16)gi->width;
            sub.height = (u16)gi->height;
            sub.left   = (float)gi->atlasX / ATLAS_SIZE;
            sub.top    = 1.0f - (float)gi->atlasY / ATLAS_SIZE;
            sub.right  = (float)(gi->atlasX + gi->width)  / ATLAS_SIZE;
            sub.bottom = 1.0f - (float)(gi->atlasY + gi->height) / ATLAS_SIZE;

            // The last two args of C2D_DrawImageAt are SCALE FACTORS, not a
            // pixel size — the drawn size is scale * subtex->width. Passing the
            // glyph's pixel width here blew every glyph up by a factor of its
            // own size. The subtexture already carries the correct dimensions,
            // so 1:1 scaling is what we want.
            //
            // The tint supplies the text colour: the A8 atlas contributes only
            // coverage (alpha), so without a tint the glyph would draw black
            // regardless of the requested colour. Built once above the loop.
            C2D_DrawImageAt(img, gx, gy, depth, &tint, 1.0f, 1.0f);
        }

        penX += gi->advance;
    }

    flushAtlasIfDirty();
}

//-----------------------------------------------------------------------------
// Enumerate fonts
//-----------------------------------------------------------------------------
std::vector<std::string> FontEngine::listFonts() const {
    std::vector<std::string> out;
    DIR* d = opendir(FONTS_PATH);
    if (!d) return out;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        std::string n = de->d_name;
        if (n.size() > 4) {
            auto ext = n.substr(n.rfind('.') + 1);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == "ttf" || ext == "otf") out.push_back(n);
        }
    }
    closedir(d);
    return out;
}
