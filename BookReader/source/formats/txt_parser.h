#pragma once
#include <string>
#include <cstdint>

//-----------------------------------------------------------------------------
// TxtParser — design doc §8.2
//
// • Reads UTF-8 with BOM detection; falls back to Latin-1 on decode failure
// • Normalises CR+LF and bare CR to LF
// • Paragraphs separated by one or more blank lines
// • Files > 1 MB are streamed to avoid RAM exhaustion on Old 3DS
//
// The result is a normalised UTF-8 string ready for the page layout engine.
//-----------------------------------------------------------------------------
class TxtParser {
public:
    static constexpr size_t STREAM_THRESHOLD = 1024 * 1024;  // 1 MB

    // Load the entire file (or the first STREAM_THRESHOLD bytes if large).
    // Returns true on success; populates `out` with normalised UTF-8.
    bool parse(const std::string& filePath, std::string& out);

    // Streaming mode: call open(), then readChunk() repeatedly until done.
    // Each chunk is already normalised UTF-8.
    bool   open(const std::string& filePath);
    bool   readChunk(std::string& out, size_t chunkBytes = STREAM_THRESHOLD);
    void   close();
    bool   eof() const { return eof_; }

private:
    FILE*  fp_  = nullptr;
    bool   eof_ = true;
    bool   latin1Fallback_ = false;

    // Convert a raw byte buffer (possibly Latin-1) to UTF-8
    static std::string toUtf8(const char* buf, size_t len, bool latin1);

    // Normalise line endings and paragraph separators
    static std::string normalise(const std::string& raw);

    // Detect BOM; returns true if UTF-8 BOM found (advances buf past it)
    static bool stripBOM(std::string& s);
};
