#pragma once
// zip_archive.h — minimal read-only ZIP reader for EPUB containers
//
// Why this exists
// ---------------
// The previous implementation scanned local file headers sequentially from
// byte 0 on *every* entry lookup, reopening the file each time and reading
// each header field with its own 2/4-byte fread. For a book with N chapters
// that is O(N^2) tiny reads against the SD card — the single largest cost in
// opening a book.
//
// It was also incorrect. When general-purpose flag bit 3 is set, a local
// header's compressed/uncompressed sizes are ZERO and the real values trail
// the file data in a "data descriptor". Many EPUB writers stream their output
// this way. Reading those zeroes desynchronised the scan and the chapter was
// silently dropped.
//
// This reader instead parses the central directory once at open:
//   • one pass, exact offsets and sizes for every entry
//   • sizes come from the central directory, which is always authoritative,
//     so data descriptors are a non-issue
//   • the file handle stays open, and reads are buffered
//
// ZIP64 and encrypted entries are not supported; neither appears in practice
// in EPUBs, and both are reported as errors rather than misparsed.

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <cstdio>

class ZipArchive {
public:
    ZipArchive() = default;
    ~ZipArchive();

    ZipArchive(const ZipArchive&)            = delete;
    ZipArchive& operator=(const ZipArchive&) = delete;

    /// Open the archive and index its central directory.
    /// Returns false if the file is missing, truncated, or not a ZIP.
    bool open(const std::string& path);
    void close();

    bool isOpen() const { return file_ != nullptr; }

    /// True if an entry with this exact name exists.
    bool has(const std::string& name) const { return entries_.count(name) != 0; }

    /// Decompress an entry into `out`. Returns false if absent or corrupt.
    /// Unlike the previous version this checks the inflate result, so a
    /// truncated entry is reported rather than silently yielding garbage.
    bool read(const std::string& name, std::string& out);

    /// Uncompressed size of an entry, or 0 if absent. Cheap — no decompression.
    uint32_t sizeOf(const std::string& name) const;

    /// All entry names, in central-directory order.
    std::vector<std::string> entryNames() const;

private:
    struct Entry {
        uint32_t localHeaderOffset = 0;
        uint32_t compressedSize    = 0;
        uint32_t uncompressedSize  = 0;
        uint16_t method            = 0;   // 0 = stored, 8 = deflated
        uint16_t flags             = 0;
    };

    FILE*                                  file_ = nullptr;
    std::unordered_map<std::string, Entry> entries_;
    std::vector<std::string>               order_;

    /// Locate the End Of Central Directory record by scanning backwards.
    bool findEndOfCentralDirectory(long& cdOffset, uint16_t& cdCount, uint32_t& cdSize);
    bool readCentralDirectory(long cdOffset, uint16_t cdCount, uint32_t cdSize);

    /// Seek past an entry's local header to the first byte of its data.
    /// The local header's name/extra lengths can differ from the central
    /// directory's, so they must be read here rather than assumed.
    bool seekToEntryData(const Entry& e);
};
