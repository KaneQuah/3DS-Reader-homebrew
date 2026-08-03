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
    cache_.clear();
    cacheCount_ = 0;
}

bool FontEngine::loadFont(const std::string& filename) {
    if (ftFace_) { FT_Done_Face(ftFace_); ftFace_ = nullptr; }
    cache_.clear();
    cacheCount_ = 0;
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
    // 3DS top screen DPI is approximately 96 dpi; bottom screen ~91 dpi.
    // We use 96 dpi as a practical constant.
    return FT_Set_Char_Size(ftFace_, 0, ptSize * 64, 96, 96) == 0;
}

int FontEngine::ascender(int ptSize) {
    if (!setSize(ptSize)) return ptSize;
    return static_cast<int>(ftFace_->size->metrics.ascender >> 6);
}

int FontEngine::descender(int ptSize) {
    if (!setSize(ptSize)) return 2;
    return static_cast<int>(-(ftFace_->size->metrics.descender >> 6));
}

int FontEngine::lineHeight(int ptSize) {
    if (!setSize(ptSize)) return ptSize + 4;
    return static_cast<int>(ftFace_->size->metrics.height >> 6);
}

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
        cache_.clear();
        cacheCount_ = 0;
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

    // Flush CPU cache so the GPU sees the updated texture data
    C3D_TexFlush(&atlasTex_);
}

//-----------------------------------------------------------------------------
// Glyph cache lookup / load
//-----------------------------------------------------------------------------
GlyphInfo* FontEngine::getOrLoad(uint32_t cp, int ptSize) {
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

    cache_[key]  = gi;
    cacheCount_++;
    return &cache_[key];
}

void FontEngine::evictOldest() {
    // Simple strategy: clear entire cache when full.
    // A production implementation would use an LRU list.
    cache_.clear();
    cacheCount_ = 0;
    atlasX_ = atlasY_ = atlasRowH_ = 0;
    if (atlasInit_) memset(atlasTex_.data, 0, ATLAS_SIZE * ATLAS_SIZE);
}

//-----------------------------------------------------------------------------
// Measure
//-----------------------------------------------------------------------------
int FontEngine::glyphAdvance(uint32_t cp) {
    GlyphInfo* gi = getOrLoad(cp, 16); // use current font size — caller should call textWidth
    return gi ? gi->advance : 8;
}

int FontEngine::textWidth(const std::string& utf8, int ptSize) {
    int total = 0;
    const char* p = utf8.c_str();
    uint32_t cp;
    while ((cp = utf8Next(p)) != 0) {
        GlyphInfo* gi = getOrLoad(cp, ptSize);
        if (gi) total += gi->advance;
    }
    return total;
}

//-----------------------------------------------------------------------------
// Draw a UTF-8 string at portrait canvas position (px, py).
// Portrait coordinate system: (0,0) = top-left of the page.
// C2D coordinates are in landscape framebuffer space; the projection matrix
// applied in reader_view.cpp maps portrait→landscape.
//-----------------------------------------------------------------------------
void FontEngine::drawText(const std::string& utf8, float px, float py, u32 colour, int ptSize) {
    if (!atlasInit_) return;
    float penX = px;
    float penY = py + ascender(ptSize);  // shift baseline down from top of line

    const char* p = utf8.c_str();
    uint32_t cp;
    while ((cp = utf8Next(p)) != 0) {
        if (cp == '\n') { penX = px; penY += lineHeight(ptSize); continue; }

        GlyphInfo* gi = getOrLoad(cp, ptSize);
        if (!gi) continue;

        if (gi->width > 0 && gi->height > 0) {
            float gx = penX + gi->bearingX;
            float gy = penY - gi->bearingY;

            // Build sub-image from atlas
            C2D_Image img;
            img.tex    = &atlasTex_;
            // Normalised UVs. The atlas is addressed top-down (row 0 = top),
            // while texture V runs bottom-up, hence the 1.0 - y/size flip.
            Tex3DS_SubTexture sub;
            sub.width  = (u16)gi->width;
            sub.height = (u16)gi->height;
            sub.left   = (float)gi->atlasX / ATLAS_SIZE;
            sub.top    = 1.0f - (float)gi->atlasY / ATLAS_SIZE;
            sub.right  = (float)(gi->atlasX + gi->width)  / ATLAS_SIZE;
            sub.bottom = 1.0f - (float)(gi->atlasY + gi->height) / ATLAS_SIZE;
            img.subtex = &sub;

            // The last two args of C2D_DrawImageAt are SCALE FACTORS, not a
            // pixel size — the drawn size is scale * subtex->width. Passing the
            // glyph's pixel width here blew every glyph up by a factor of its
            // own size. The subtexture already carries the correct dimensions,
            // so 1:1 scaling is what we want.
            //
            // The tint supplies the text colour: the A8 atlas contributes only
            // coverage (alpha), so without a tint the glyph would draw black
            // regardless of the requested colour.
            C2D_ImageTint tint;
            C2D_PlainImageTint(&tint, colour, 1.0f);

            C2D_DrawImageAt(img, gx, gy, 0.5f, &tint, 1.0f, 1.0f);
        }

        penX += gi->advance;
    }
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
