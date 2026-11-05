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
    std::vector<int> tokenIndices;      // indices into the token array
    int              totalWidth  = 0;
    bool             isLastInPara = false;  // suppress justification on last line

    // Number of Space tokens on this line. Justification needs it to divide
    // the slack evenly, and it never changes once the line is broken — but
    // renderReflowPage() used to recount it by walking tokenIndices on every
    // line of every frame. Recorded once here instead.
    int              spaceCount  = 0;

    /// This is not a line of text: it is a marker emitted for an explicit page
    /// break (a form feed in the source), and paginateLines() ends the current
    /// page on it. It carries no tokens and is never rendered.
    ///
    /// Previously the marker was encoded by giving the line an impossible
    /// width — `totalWidth = pageW + 1` — and paginateLines() looked for it
    /// with `totalWidth > pageH * 10`. Those are two different dimensions:
    /// with normal margins the marker was written as ~221 and tested against
    /// ~3560, so the branch never fired and explicit page breaks were silently
    /// dropped. A flag says what is meant instead of encoding it in a value
    /// that has to be compared against an unrelated one.
    bool             isPageBreak = false;
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
//   layout.run(text, pageW, pageHEven, pageHOdd, ptSize, lineSpacingMul, justify)
//   Then iterate pages and render lines.
//-----------------------------------------------------------------------------
class PageLayout {
public:
    // Tokenise and paginate a UTF-8 plain-text string.
    //
    // pageW and both heights are in portrait pixels. The two heights exist
    // because the two screens are not the same size: in portrait the top
    // screen is 240×400 and the bottom is 240×320. A spread shows page 2S on
    // the top screen and page 2S+1 on the bottom, so EVEN pages are filled to
    // `pageHEven` (top) and ODD pages to `pageHOdd` (bottom).
    //
    // This used to take a single height, and the one caller passed the TOP
    // screen's. Every page was therefore filled to ~356 px while
    // renderReflowPage() stops drawing at the bottom screen's ~310 px — so the
    // last two or three lines of every right-hand page were never drawn, and
    // because the next page resumes at the following line index, they were
    // never shown anywhere. Text was silently lost on every odd page.
    void run(const std::string& utf8Text,
             int pageW, int pageHEven, int pageHOdd,
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
    void paginateLines(int pageHEven, int pageHOdd, int ptSize, float lineSpacingMul);

    // Greedy line-break
    void greedyBreak(int pageW, int ptSize);
};

extern PageLayout g_layout;
