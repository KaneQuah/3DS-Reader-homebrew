#include "renderer/page_layout.h"
#include "renderer/font_engine.h"
#include <algorithm>
#include <cctype>

PageLayout g_layout;

//-----------------------------------------------------------------------------
// UTF-8 decode (duplicate of font_engine.cpp version — consider moving to util)
//-----------------------------------------------------------------------------
static uint32_t utf8Next(const char*& p) {
    uint8_t c = (uint8_t)*p;
    if (c == 0) return 0;
    p++;
    if (c < 0x80) return c;
    uint32_t cp; int extra;
    if      ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else return 0xFFFD;
    while (extra-- > 0) { uint8_t b = (uint8_t)*p++; cp = (cp<<6)|(b&0x3F); }
    return cp;
}

//-----------------------------------------------------------------------------
// Tokenise: split into words, spaces, newlines, and page-break markers
//-----------------------------------------------------------------------------
void PageLayout::tokenise(const std::string& utf8, int ptSize) {
    const char* p = utf8.c_str();
    std::string cur;

    // The space glyph's advance is the same for every space in the chapter.
    // Measuring it per space meant a full textWidth() call — UTF-8 decode,
    // glyph lookup, atlas dirty check — roughly once every six characters of
    // the book.
    const int spaceW = g_fontEngine.textWidth(" ", ptSize);

    // English prose averages a little under 3 characters per token once spaces
    // are counted as tokens of their own. Reserving up front avoids the
    // repeated grow-and-copy of a vector whose elements each own a std::string,
    // which is the expensive kind of reallocation.
    tokens_.reserve(utf8.size() / 3 + 16);

    auto flushWord = [&]() {
        if (cur.empty()) return;
        Token t;
        t.type  = TokenType::Word;
        t.width = g_fontEngine.textWidth(cur, ptSize);
        // Move, don't copy: `cur` is cleared immediately afterwards either way,
        // so the word's buffer can be handed straight to the token.
        t.text  = std::move(cur);
        tokens_.push_back(std::move(t));
        cur.clear();   // moved-from strings are valid but unspecified
    };

    uint32_t cp;
    while ((cp = utf8Next(p)) != 0) {
        if (cp == '\f') {           // form feed = explicit page break
            flushWord();
            tokens_.push_back({ TokenType::PageBreak, "", 0 });
        } else if (cp == '\n') {
            flushWord();
            // Two consecutive newlines → paragraph break (treated as newline)
            Token t;
            t.type  = TokenType::Newline;
            t.text  = "";
            t.width = 0;
            tokens_.push_back(t);
        } else if (cp == ' ' || cp == '\t' || cp == 0x00A0 /* NBSP */) {
            flushWord();
            tokens_.push_back({ TokenType::Space, " ", spaceW });
        } else {
            // Encode cp back to UTF-8 for the word buffer
            if (cp < 0x80) {
                cur += (char)cp;
            } else if (cp < 0x800) {
                cur += (char)(0xC0 | (cp >> 6));
                cur += (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                cur += (char)(0xE0 | (cp >> 12));
                cur += (char)(0x80 | ((cp >> 6) & 0x3F));
                cur += (char)(0x80 | (cp & 0x3F));
            } else {
                cur += (char)(0xF0 | (cp >> 18));
                cur += (char)(0x80 | ((cp >> 12) & 0x3F));
                cur += (char)(0x80 | ((cp >>  6) & 0x3F));
                cur += (char)(0x80 | ( cp        & 0x3F));
            }
        }
    }
    flushWord();
}

//-----------------------------------------------------------------------------
// Greedy line-break: fill each line with as many tokens as fit within pageW
//-----------------------------------------------------------------------------
void PageLayout::greedyBreak(int pageW, int ptSize) {
    int spaceW = g_fontEngine.textWidth(" ", ptSize);

    // Each Line owns a heap-allocated vector of token indices, and pushing one
    // by value allocated a second copy of that vector before the original was
    // cleared — two allocations and a copy per line. `emitLine` moves instead,
    // so the buffer curLine built is handed straight to lines_.
    lines_.reserve(tokens_.size() / 8 + 16);

    Line curLine;
    curLine.totalWidth    = 0;
    curLine.isLastInPara  = false;
    curLine.spaceCount    = 0;
    bool lineStart = true;

    auto emitLine = [&](bool lastInPara) {
        curLine.isLastInPara = lastInPara;
        lines_.push_back(std::move(curLine));
        curLine.tokenIndices.clear();
        curLine.totalWidth  = 0;
        curLine.spaceCount  = 0;
        curLine.isPageBreak = false;
    };

    for (int i = 0; i < (int)tokens_.size(); i++) {
        const Token& tok = tokens_[i];

        if (tok.type == TokenType::PageBreak || tok.type == TokenType::Newline) {
            if (!curLine.tokenIndices.empty()) emitLine(true);
            if (tok.type == TokenType::PageBreak) {
                // Marker line: carries no tokens, is never rendered, and ends
                // the page it lands on. See Line::isPageBreak in page_layout.h
                // for why this is a flag rather than an out-of-range width.
                Line pb;
                pb.isLastInPara = true;
                pb.isPageBreak  = true;
                lines_.push_back(std::move(pb));
            }
            lineStart = true;
            continue;
        }

        if (tok.type == TokenType::Space) {
            if (lineStart) continue;  // skip leading space
            int newW = curLine.totalWidth + spaceW + (i+1 < (int)tokens_.size() ? tokens_[i+1].width : 0);
            if (newW > pageW && !curLine.tokenIndices.empty()) {
                emitLine(false);
                lineStart = true;
            } else {
                curLine.tokenIndices.push_back(i);
                curLine.totalWidth += spaceW;
                curLine.spaceCount++;
            }
            continue;
        }

        // Word token
        if (!lineStart && curLine.totalWidth + tok.width > pageW) {
            emitLine(false);
            lineStart = true;
        }
        curLine.tokenIndices.push_back(i);
        curLine.totalWidth += tok.width;
        lineStart = false;
    }

    if (!curLine.tokenIndices.empty()) emitLine(true);
}

void PageLayout::breakLines(int pageW, int ptSize, bool justify, bool useKnuthPlass) {
    // Knuth-Plass on New 3DS is a stretch; always use greedy for now.
    (void)justify; (void)useKnuthPlass;
    greedyBreak(pageW, ptSize);
}

//-----------------------------------------------------------------------------
// Paginate: group lines into pages based on available height
//-----------------------------------------------------------------------------
void PageLayout::paginateLines(int pageH, int ptSize, float lineSpacingMul) {
    int lh = static_cast<int>(g_fontEngine.lineHeight(ptSize) * lineSpacingMul);
    if (lh < 1) lh = ptSize + 2;

    int y    = 0;
    Page cur;
    cur.lineStart = 0;
    cur.lineEnd   = 0;

    for (size_t i = 0; i < lines_.size(); i++) {
        // Explicit page break: close the current page and resume *after* the
        // marker, so it never becomes part of a page's line range and never
        // reaches the renderer.
        if (lines_[i].isPageBreak) {
            if (cur.lineStart < i) { cur.lineEnd = i; pages_.push_back(cur); }
            cur.lineStart = i + 1;
            cur.lineEnd   = i + 1;
            y = 0;
            continue;
        }

        if (y + lh > pageH && cur.lineStart < i) {
            cur.lineEnd = i;
            pages_.push_back(cur);
            cur.lineStart = i;
            y = 0;
        }
        y += lh;
        cur.lineEnd = i + 1;
    }

    if (cur.lineStart < cur.lineEnd)
        pages_.push_back(cur);
}

//-----------------------------------------------------------------------------
// run() — full layout pipeline
//-----------------------------------------------------------------------------
#include "trace.h"

void PageLayout::run(const std::string& utf8Text,
                     int pageW, int pageH,
                     int ptSize, float lineSpacingMul,
                     bool justify, bool useKnuthPlass) {
    clear();
    tokenise(utf8Text, ptSize);
    TRACE("    tokenise: %u tokens", (unsigned)tokens_.size());
    breakLines(pageW, ptSize, justify, useKnuthPlass);
    TRACE("    breakLines: %u lines", (unsigned)lines_.size());
    paginateLines(pageH, ptSize, lineSpacingMul);
    TRACE("    paginate: %u pages", (unsigned)pages_.size());
}

void PageLayout::clear() {
    tokens_.clear();
    lines_.clear();
    pages_.clear();
}
