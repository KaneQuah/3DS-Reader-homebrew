#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

//-----------------------------------------------------------------------------
// Bookmark — a position within a book.
//
// Positions are (chapter, spread) rather than a global page number, because
// only the current chapter is ever laid out. A global page index would be
// meaningless until the whole book had been paginated, and would silently
// change meaning whenever the font size did.
//-----------------------------------------------------------------------------
struct Bookmark {
    int chapter = 0;
    int spread  = 0;

    bool operator==(const Bookmark& o) const {
        return chapter == o.chapter && spread == o.spread;
    }
};

//-----------------------------------------------------------------------------
// BookProgress — per-book reading state, keyed by SHA-1 of the absolute path
//-----------------------------------------------------------------------------
struct BookProgress {
    std::string path;            // absolute path on SD card
    int         chapter = 0;     // spine index of the last-read chapter
    int         spread  = 0;     // spread within that chapter
    int         scroll_offset = 0;

    // Total chapters in the book, recorded when the book was last read.
    // Cached here so the file browser can show a progress bar without opening
    // and parsing every EPUB in the directory. 0 means "never opened".
    int         chapter_count = 0;

    std::vector<Bookmark> bookmarks;
};

//-----------------------------------------------------------------------------
// ProgressStore — loads/saves progress.json
//
// File format matches design doc §12.2:
//   { "version": 1, "books": { "<sha1>": { ... } } }
//
// Writes are atomic: write to progress.json.tmp then rename.
//-----------------------------------------------------------------------------
class ProgressStore {
public:
    bool load(const char* path);
    bool save(const char* path) const;

    // Returns nullptr if no progress recorded for this book path
    const BookProgress* get(const std::string& bookPath) const;
    void                set(const BookProgress& bp);

private:
    // SHA-1 of the absolute book path (40 hex chars) -> BookProgress
    std::unordered_map<std::string, BookProgress> entries_;

    static std::string sha1Hex(const std::string& s);
};

extern ProgressStore g_progress;
