#pragma once
#include "common.h"
#include <string>
#include <vector>
#include <cstdint>

//-----------------------------------------------------------------------------
// Token — atomic text unit produced by the tokeniser
//-----------------------------------------------------------------------------
enum class TokenType {
    Word,       // a renderable word
    Space,      // inter-word space
    Newline,    // explicit line break (from <br> or \n)
    PageBreak,  // explicit page break
    ImageRef,   // inline image (TODO: full image support)
};

struct Token {
    TokenType   type;
    std::string text;     // UTF-8 content (for Word/Space)
    int         width;    // pre-measured pixel width at current font/size
};

//-----------------------------------------------------------------------------
// Line — a sequence of tokens that fits on one line
//-----------------------------------------------------------------------------
struct Line {
    std::vector<int> tokenIndices;  // indices into the token array
    int              totalWidth;
    bool             isLastInPara;  // suppress justification on last line
};

//-----------------------------------------------------------------------------
// Page — a range of lines that fits on one screen
//-----------------------------------------------------------------------------
struct Page {
    size_t lineStart;  // index into g_lines
    size_t lineEnd;    // exclusive
};

//-----------------------------------------------------------------------------
// PageLayout — tokenise, measure, line-break, paginate (§9.3)
//
// Usage:
//   layout.run(text, pageW, pageH, ptSize, lineSpacingMul, justify, marginPx)
//   Then iterate pages and render lines.
//-----------------------------------------------------------------------------
class PageLayout {
public:
    // Tokenise and paginate a UTF-8 plain-text string.
    // pageW / pageH are in portrait pixels (e.g. 220, 380 for top screen with 10px margins).
    void run(const std::string& utf8Text,
             int pageW, int pageH,
             int ptSize, float lineSpacingMul,
             bool justify, bool useKnuthPlass = false);

    // Access results
    size_t pageCount()  const { return pages_.size(); }
    const Page& page(size_t i) const { return pages_[i]; }
    const std::vector<Line>&  lines()  const { return lines_; }
    const std::vector<Token>& tokens() const { return tokens_; }

    // Clear all state (call before re-layout)
    void clear();

private:
    std::vector<Token> tokens_;
    std::vector<Line>  lines_;
    std::vector<Page>  pages_;

    void tokenise(const std::string& utf8, int ptSize);
    void breakLines(int pageW, int ptSize, bool justify, bool useKnuthPlass);
    void paginateLines(int pageH, int ptSize, float lineSpacingMul);

    // Greedy line-break
    void greedyBreak(int pageW, int ptSize);
};

extern PageLayout g_layout;
