#pragma once
#include "common.h"
#include "formats/zip_archive.h"
#include <string>
#include <vector>

//-----------------------------------------------------------------------------
// EpubItem — one entry from the OPF manifest
//-----------------------------------------------------------------------------
struct EpubItem {
    std::string id;
    std::string href;      // path within ZIP, relative to OPF dir
    std::string mediaType;
};

//-----------------------------------------------------------------------------
// EpubChapter — one spine item.
//
// Note there is no `text` member any more. Chapter text is fetched on demand
// via EpubDocument::chapterText(), because eagerly decompressing and
// XML-parsing every chapter at open is what made opening a book take minutes.
//-----------------------------------------------------------------------------
struct EpubChapter {
    std::string id;
    std::string href;      // full path within the ZIP
    std::string title;     // from NCX/NAV, may be empty
};

//-----------------------------------------------------------------------------
// EpubMetadata
//-----------------------------------------------------------------------------
struct EpubMetadata {
    std::string title;
    std::string author;
    std::string language;
    std::string uid;
};

//-----------------------------------------------------------------------------
// EpubDocument — an opened book.
//
// Holds the structure of the book (metadata, spine, TOC) plus a live handle on
// the ZIP. Chapter bodies are pulled through on demand and a few recently used
// ones are kept in memory.
//
// The document owns the archive, so it must outlive any reading of it, and it
// is non-copyable.
//-----------------------------------------------------------------------------
class EpubDocument {
public:
    EpubDocument() = default;

    EpubDocument(const EpubDocument&)            = delete;
    EpubDocument& operator=(const EpubDocument&) = delete;

    EpubMetadata             meta;
    std::vector<EpubChapter> chapters;   // in spine order

    int  chapterCount() const { return (int)chapters.size(); }
    bool isOpen()       const { return zip_.isOpen(); }

    /// Plain UTF-8 text of one chapter, decompressed and stripped of markup on
    /// first request. Returns an empty string for an out-of-range index or an
    /// unreadable chapter — callers treat empty as "skip", since real-world
    /// EPUBs routinely contain stub documents.
    const std::string& chapterText(int index);

    /// Drop cached chapter bodies, keeping the book open. Used when the layout
    /// is rebuilt (font size change etc.) to avoid holding stale memory.
    void dropCache();

    void close();

private:
    friend class EpubParser;

    static constexpr int CACHE_SIZE = 3;   // current chapter plus both neighbours

    ZipArchive zip_;

    // Tiny direct-mapped cache. Three entries is enough to make page-turns
    // across a chapter boundary free in both directions, and costs a few
    // hundred KB at most.
    struct CacheSlot {
        int         index = -1;
        std::string text;
    };
    CacheSlot cache_[CACHE_SIZE];
    int       cacheNext_ = 0;

    static const std::string kEmpty;
};

//-----------------------------------------------------------------------------
// EpubParser — design doc §8.1
//
// Open pipeline (cheap — structure only):
//   1. Index the ZIP central directory
//   2. Confirm mimetype = "application/epub+zip"
//   3. META-INF/container.xml → OPF path
//   4. OPF → metadata, manifest, spine order
//   5. NCX (EPUB 2) or nav.xhtml (EPUB 3) → chapter titles
//
// Chapter bodies are NOT read here. See EpubDocument::chapterText().
//-----------------------------------------------------------------------------
class EpubParser {
public:
    enum class Result { OK, NotEpub, BadZip, BadOPF, BadContent };

    /// Open `epubPath` and populate `out` with the book's structure.
    Result open(const std::string& epubPath, EpubDocument& out);

private:
    std::string parseOPFPath(const std::string& containerXml);
    bool        parseOPF(const std::string& opfXml, const std::string& opfDir,
                         std::vector<EpubItem>& manifest, std::vector<std::string>& spine,
                         EpubMetadata& meta, std::string& tocId);
    bool        parseNCX(const std::string& ncxXml, std::vector<std::string>& ids,
                         std::vector<std::string>& titles);
    bool        parseNavXhtml(const std::string& navXhtml, std::vector<std::string>& ids,
                              std::vector<std::string>& titles);
};

/// Strip XHTML markup down to plain text. Exposed because EpubDocument needs it
/// when it lazily materialises a chapter.
std::string epubExtractTextFromXhtml(const std::string& xhtml);

/// Count non-whitespace characters in a string.
size_t epubCountVisibleChars(const std::string& s);
