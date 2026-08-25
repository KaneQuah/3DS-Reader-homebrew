#include "formats/zip_archive.h"
#include <zlib.h>
#include <cstring>
#include <algorithm>

namespace {

//-----------------------------------------------------------------------------
// Little-endian scalar reads from a byte buffer
//-----------------------------------------------------------------------------
inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

constexpr uint32_t SIG_EOCD   = 0x06054B50;  // End of central directory
constexpr uint32_t SIG_CDFH   = 0x02014B50;  // Central directory file header
constexpr uint32_t SIG_LFH    = 0x04034B50;  // Local file header

constexpr uint16_t FLAG_ENCRYPTED = 0x0001;

// EOCD is 22 bytes plus up to 65535 bytes of comment
constexpr long EOCD_MIN_SIZE = 22;
constexpr long EOCD_MAX_SCAN = 22 + 65535;

// Decompression is done in chunks so we never hold both a full compressed
// copy and a full uncompressed copy of a large chapter at once.
constexpr size_t INFLATE_CHUNK = 16 * 1024;

} // namespace

ZipArchive::~ZipArchive() { close(); }

void ZipArchive::close() {
    if (file_) { fclose(file_); file_ = nullptr; }
    entries_.clear();
    order_.clear();
}

//-----------------------------------------------------------------------------
// open
//-----------------------------------------------------------------------------
bool ZipArchive::open(const std::string& path) {
    close();

    file_ = fopen(path.c_str(), "rb");
    if (!file_) return false;

    // A large stdio buffer turns the central-directory walk into a handful of
    // big SD reads instead of thousands of small ones.
    setvbuf(file_, nullptr, _IOFBF, 32 * 1024);

    long     cdOffset = 0;
    uint16_t cdCount  = 0;
    uint32_t cdSize   = 0;
    if (!findEndOfCentralDirectory(cdOffset, cdCount, cdSize))  { close(); return false; }
    if (!readCentralDirectory(cdOffset, cdCount, cdSize))       { close(); return false; }

    return true;
}

//-----------------------------------------------------------------------------
// findEndOfCentralDirectory
//
// The EOCD sits at the very end of the file, but a trailing comment of up to
// 64 KB may follow it, so we read the tail into memory and scan backwards for
// the signature.
//-----------------------------------------------------------------------------
bool ZipArchive::findEndOfCentralDirectory(long& cdOffset, uint16_t& cdCount, uint32_t& cdSize) {
    if (fseek(file_, 0, SEEK_END) != 0) return false;
    long fileSize = ftell(file_);
    if (fileSize < EOCD_MIN_SIZE) return false;

    long scanLen = std::min<long>(fileSize, EOCD_MAX_SCAN);
    long scanPos = fileSize - scanLen;

    std::vector<uint8_t> tail((size_t)scanLen);
    if (fseek(file_, scanPos, SEEK_SET) != 0) return false;
    if (fread(tail.data(), 1, (size_t)scanLen, file_) != (size_t)scanLen) return false;

    // Walk backwards so a comment that happens to contain the signature does
    // not shadow the real record.
    for (long i = scanLen - EOCD_MIN_SIZE; i >= 0; i--) {
        if (rd32(&tail[(size_t)i]) != SIG_EOCD) continue;

        const uint8_t* e = &tail[(size_t)i];
        cdCount  = rd16(e + 10);          // entries in this disk
        cdSize   = rd32(e + 12);
        uint32_t cdStart  = rd32(e + 16);

        // ZIP64 sentinel — we do not support it, and misreading it would be
        // worse than refusing the file.
        if (cdStart == 0xFFFFFFFFu || cdSize == 0xFFFFFFFFu ||
            cdCount == 0xFFFFu) return false;

        cdOffset = (long)cdStart;
        return cdOffset >= 0 && cdOffset < fileSize;
    }
    return false;
}

//-----------------------------------------------------------------------------
// readCentralDirectory
//
// Previously issued up to three stdio calls per entry (fread the 46-byte
// header, fread the name, fseek past extra+comment) — 1187 entries meant
// ~3500 separate stdio round trips, measured at 5.8s to index even with a
// 32KB setvbuf buffer. The central directory is one contiguous block on
// disk; read all `cdSize` bytes in a single fread and parse the in-memory
// copy instead, so indexing costs one big SD read plus pure CPU work.
//-----------------------------------------------------------------------------
bool ZipArchive::readCentralDirectory(long cdOffset, uint16_t cdCount, uint32_t cdSize) {
    if (fseek(file_, cdOffset, SEEK_SET) != 0) return false;

    std::vector<uint8_t> buf(cdSize);
    if (cdSize > 0 && fread(buf.data(), 1, cdSize, file_) != cdSize) return false;

    entries_.reserve(cdCount);
    order_.reserve(cdCount);

    const uint8_t* p   = buf.data();
    const uint8_t* end = buf.data() + buf.size();

    for (uint16_t i = 0; i < cdCount; i++) {
        if (end - p < 46) return false;   // truncated central directory
        if (rd32(p) != SIG_CDFH) return false;

        Entry e;
        e.flags             = rd16(p + 8);
        e.method            = rd16(p + 10);
        e.compressedSize    = rd32(p + 20);
        e.uncompressedSize  = rd32(p + 24);
        uint16_t nameLen    = rd16(p + 28);
        uint16_t extraLen   = rd16(p + 30);
        uint16_t commentLen = rd16(p + 32);
        e.localHeaderOffset = rd32(p + 42);
        p += 46;

        long tailLen = (long)nameLen + (long)extraLen + (long)commentLen;
        if (end - p < tailLen) return false;   // truncated central directory

        std::string name(reinterpret_cast<const char*>(p), nameLen);
        p += tailLen;

        // Skip encrypted entries and directory markers rather than failing the
        // whole archive — a stray entry should not make a book unreadable.
        if (e.flags & FLAG_ENCRYPTED) continue;
        if (!name.empty() && name.back() == '/') continue;

        if (entries_.emplace(name, e).second)
            order_.push_back(name);
    }
    return !entries_.empty();
}

//-----------------------------------------------------------------------------
// seekToEntryData
//
// The central directory gives us the offset of the *local header*, not of the
// data. The local header's name and extra-field lengths may differ from the
// central directory's (extra fields commonly do), so they must be read.
//-----------------------------------------------------------------------------
bool ZipArchive::seekToEntryData(const Entry& e) {
    uint8_t lfh[30];
    if (fseek(file_, (long)e.localHeaderOffset, SEEK_SET) != 0) return false;
    if (fread(lfh, 1, sizeof(lfh), file_) != sizeof(lfh)) return false;
    if (rd32(lfh) != SIG_LFH) return false;

    uint16_t nameLen  = rd16(lfh + 26);
    uint16_t extraLen = rd16(lfh + 28);
    return fseek(file_, nameLen + extraLen, SEEK_CUR) == 0;
}

//-----------------------------------------------------------------------------
// read
//-----------------------------------------------------------------------------
bool ZipArchive::read(const std::string& name, std::string& out) {
    out.clear();
    if (!file_) return false;

    auto it = entries_.find(name);
    if (it == entries_.end()) return false;
    const Entry& e = it->second;

    if (!seekToEntryData(e)) return false;

    //--- Stored ---------------------------------------------------------
    if (e.method == 0) {
        out.resize(e.uncompressedSize);
        if (e.uncompressedSize == 0) return true;
        return fread(&out[0], 1, e.uncompressedSize, file_) == e.uncompressedSize;
    }

    //--- Deflated -------------------------------------------------------
    if (e.method != 8) return false;   // unsupported compression method

    z_stream zs{};
    // Negative window bits selects raw deflate (no zlib header), which is what
    // ZIP stores.
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;

    out.resize(e.uncompressedSize);
    zs.next_out  = reinterpret_cast<Bytef*>(e.uncompressedSize ? &out[0] : nullptr);
    zs.avail_out = e.uncompressedSize;

    std::vector<uint8_t> inBuf(INFLATE_CHUNK);
    uint32_t remaining = e.compressedSize;
    int      status    = Z_OK;

    while (status != Z_STREAM_END) {
        if (zs.avail_in == 0) {
            if (remaining == 0) break;   // ran out of input before end of stream
            size_t want = std::min<size_t>(INFLATE_CHUNK, remaining);
            size_t got  = fread(inBuf.data(), 1, want, file_);
            if (got == 0) break;
            remaining  -= (uint32_t)got;
            zs.next_in  = inBuf.data();
            zs.avail_in = (uInt)got;
        }

        status = inflate(&zs, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) break;   // corrupt entry
        if (zs.avail_out == 0 && status != Z_STREAM_END) break; // size mismatch
    }

    inflateEnd(&zs);

    // Trust the stream, not the header: if the entry decompressed short, report
    // failure rather than handing back a partially-filled buffer.
    if (status != Z_STREAM_END) { out.clear(); return false; }

    out.resize(zs.total_out);
    return true;
}

//-----------------------------------------------------------------------------
uint32_t ZipArchive::sizeOf(const std::string& name) const {
    auto it = entries_.find(name);
    return it == entries_.end() ? 0 : it->second.uncompressedSize;
}

std::vector<std::string> ZipArchive::entryNames() const {
    return order_;
}
