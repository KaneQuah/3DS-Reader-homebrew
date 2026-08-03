#include "formats/txt_parser.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

//-----------------------------------------------------------------------------
// UTF-8 validation: attempt to decode a byte buffer as UTF-8.
// Returns false if any invalid sequence is found.
//-----------------------------------------------------------------------------
static bool isValidUtf8(const char* buf, size_t len) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(buf);
    const uint8_t* e = p + len;
    while (p < e) {
        if (*p < 0x80) { p++; continue; }
        if ((*p & 0xE0) == 0xC0) {
            if (p+1 >= e || (p[1]&0xC0) != 0x80) return false;
            p += 2;
        } else if ((*p & 0xF0) == 0xE0) {
            if (p+2 >= e || (p[1]&0xC0) != 0x80 || (p[2]&0xC0) != 0x80) return false;
            p += 3;
        } else if ((*p & 0xF8) == 0xF0) {
            if (p+3 >= e || (p[1]&0xC0) != 0x80 || (p[2]&0xC0) != 0x80
                         || (p[3]&0xC0) != 0x80) return false;
            p += 4;
        } else {
            return false;
        }
    }
    return true;
}

//-----------------------------------------------------------------------------
// Latin-1 → UTF-8 conversion
//-----------------------------------------------------------------------------
std::string TxtParser::toUtf8(const char* buf, size_t len, bool latin1) {
    if (!latin1) return std::string(buf, len);
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)buf[i];
        if (c < 0x80) { out += (char)c; }
        else { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
    }
    return out;
}

bool TxtParser::stripBOM(std::string& s) {
    if (s.size() >= 3 &&
        (uint8_t)s[0] == 0xEF &&
        (uint8_t)s[1] == 0xBB &&
        (uint8_t)s[2] == 0xBF) {
        s.erase(0, 3);
        return true;
    }
    return false;
}

//-----------------------------------------------------------------------------
// Normalise: CR+LF → LF, bare CR → LF; two+ consecutive LF → paragraph break
//-----------------------------------------------------------------------------
std::string TxtParser::normalise(const std::string& raw) {
    std::string s;
    s.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); i++) {
        char c = raw[i];
        if (c == '\r') {
            s += '\n';
            if (i+1 < raw.size() && raw[i+1] == '\n') i++;  // skip LF of CRLF
        } else {
            s += c;
        }
    }
    return s;
}

//-----------------------------------------------------------------------------
// parse() — load whole file (≤ 1 MB) or first chunk
//-----------------------------------------------------------------------------
bool TxtParser::parse(const std::string& filePath, std::string& out) {
    FILE* f = fopen(filePath.c_str(), "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 0) { fclose(f); return false; }

    size_t readSz = std::min((long)STREAM_THRESHOLD, sz);
    std::string buf(readSz, '\0');
    fread(buf.data(), 1, readSz, f);
    fclose(f);

    bool latin1 = false;
    stripBOM(buf);
    if (!isValidUtf8(buf.data(), buf.size())) {
        latin1 = true;
    }

    out = normalise(toUtf8(buf.data(), buf.size(), latin1));
    return true;
}

//-----------------------------------------------------------------------------
// Streaming mode
//-----------------------------------------------------------------------------
bool TxtParser::open(const std::string& filePath) {
    close();
    fp_ = fopen(filePath.c_str(), "rb");
    if (!fp_) return false;

    // Detect encoding from first few bytes
    char bom[3] = {};
    fread(bom, 1, 3, fp_);
    if ((uint8_t)bom[0]==0xEF && (uint8_t)bom[1]==0xBB && (uint8_t)bom[2]==0xBF) {
        latin1Fallback_ = false;  // UTF-8 BOM
    } else {
        // Re-read to test encoding
        rewind(fp_);
        char test[512]; size_t n = fread(test, 1, sizeof(test), fp_);
        latin1Fallback_ = !isValidUtf8(test, n);
        rewind(fp_);
    }
    eof_ = false;
    return true;
}

bool TxtParser::readChunk(std::string& out, size_t chunkBytes) {
    if (!fp_ || eof_) return false;
    std::string buf(chunkBytes, '\0');
    size_t n = fread(buf.data(), 1, chunkBytes, fp_);
    if (n < chunkBytes) eof_ = true;
    buf.resize(n);
    out = normalise(toUtf8(buf.data(), buf.size(), latin1Fallback_));
    return n > 0;
}

void TxtParser::close() {
    if (fp_) { fclose(fp_); fp_ = nullptr; }
    eof_ = true;
}
