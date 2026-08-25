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
//
// Write-behind
// ------------
// set() only updates the in-memory map and raises a dirty flag; it does not
// touch the SD card. Callers ask for the file to be written with
// saveIfDirty(), which no-ops when nothing has changed.
//
// This is not a micro-optimisation. ReaderView::saveProgress() runs on every
// page turn, and it used to serialise the whole store and write it out each
// time: fopen the .tmp, a dozen fprintf calls, fclose, then rename(). On the
// 3DS every one of those is a round trip to the FS process, and each write
// makes the SD card do a FAT read-modify-write of a 512-byte sector — a card
// that benchmarks around class 4. That cost was being paid on the one code
// path the user is waiting on, several times a minute, to persist two
// integers that only matter when the app closes.
//
// Durability is preserved by flushing at every point where the position could
// otherwise be lost: closing a book, opening the settings overlay, the APT
// sleep/suspend/exit hooks, the app's own exit path, and a low-frequency
// idle autosave in the main loop. See flushProgress() in main.cpp.
//-----------------------------------------------------------------------------
class ProgressStore {
public:
    bool load(const char* path);
    bool save(const char* path) const;

    /// Write the store out only if something has changed since the last write.
    /// Returns true if a write actually happened.
    bool saveIfDirty(const char* path);

    /// True when in-memory state differs from what is on the SD card.
    bool dirty() const { return dirty_; }

    // Returns nullptr if no progress recorded for this book path
    const BookProgress* get(const std::string& bookPath) const;

    /// Record a book's progress in memory. Raises the dirty flag only if this
    /// actually changes something — re-setting an identical entry (which
    /// happens whenever a page turn is queued and then serviced) must not
    /// schedule a pointless SD write.
    void                set(const BookProgress& bp);

    /// As set(), but with the key already computed by keyFor(). `bp.path` is
    /// still stored, so the entry is self-describing either way.
    void                set(const std::string& key, const BookProgress& bp);

    /// SHA-1 of a book's absolute path — the key used in progress.json.
    /// Exposed so a caller that re-saves the same book repeatedly can hash the
    /// path once and keep the result. Computing SHA-1 is not free, and
    /// saveProgress() was doing it on every page turn.
    static std::string keyFor(const std::string& bookPath);

private:
    // SHA-1 of the absolute book path (40 hex chars) -> BookProgress
    std::unordered_map<std::string, BookProgress> entries_;

    bool dirty_ = false;

    static std::string sha1Hex(const std::string& s);
};

extern ProgressStore g_progress;
