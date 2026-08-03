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

    auto flushWord = [&]() {
        if (cur.empty()) return;
        Token t;
        t.type  = TokenType::Word;
        t.text  = cur;
        t.width = g_fontEngine.textWidth(cur, ptSize);
        tokens_.push_back(t);
        cur.clear();
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
            int spW = g_fontEngine.textWidth(" ", ptSize);
            tokens_.push_back({ TokenType::Space, " ", spW });
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
    Line curLine;
    curLine.totalWidth    = 0;
    curLine.isLastInPara  = false;
    bool lineStart = true;

    for (int i = 0; i < (int)tokens_.size(); i++) {
        const Token& tok = tokens_[i];

        if (tok.type == TokenType::PageBreak || tok.type == TokenType::Newline) {
            if (!curLine.tokenIndices.empty()) {
                curLine.isLastInPara = true;
                lines_.push_back(curLine);
                curLine.tokenIndices.clear();
                curLine.totalWidth = 0;
            }
            if (tok.type == TokenType::PageBreak) {
                // Sentinel line to force page break
                Line pb;
                pb.isLastInPara = true;
                pb.totalWidth   = pageW + 1;  // overwide → triggers page break
                lines_.push_back(pb);
            }
            lineStart = true;
            continue;
        }

        if (tok.type == TokenType::Space) {
            if (lineStart) continue;  // skip leading space
            int newW = curLine.totalWidth + spaceW + (i+1 < (int)tokens_.size() ? tokens_[i+1].width : 0);
            if (newW > pageW && !curLine.tokenIndices.empty()) {
                curLine.isLastInPara = false;
                lines_.push_back(curLine);
                curLine.tokenIndices.clear();
                curLine.totalWidth = 0;
                lineStart = true;
            } else {
                curLine.tokenIndices.push_back(i);
                curLine.totalWidth += spaceW;
            }
            continue;
        }

        // Word token
        if (!lineStart && curLine.totalWidth + tok.width > pageW) {
            curLine.isLastInPara = false;
            lines_.push_back(curLine);
            curLine.tokenIndices.clear();
            curLine.totalWidth = 0;
            lineStart = true;
        }
        curLine.tokenIndices.push_back(i);
        curLine.totalWidth += tok.width;
        lineStart = false;
    }

    if (!curLine.tokenIndices.empty()) {
        curLine.isLastInPara = true;
        lines_.push_back(curLine);
    }
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
        // Overwide sentinel = forced page break
        if (lines_[i].totalWidth > pageH * 10) {
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
