#include "settings/progress.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>

ProgressStore g_progress;

//-----------------------------------------------------------------------------
// SHA-1 implementation (RFC 3174) — used to key progress entries
//-----------------------------------------------------------------------------
static void sha1Block(uint32_t H[5], const uint8_t block[64]) {
    auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    uint32_t W[80];
    for (int i = 0; i < 16; i++) {
        W[i] = ((uint32_t)block[i*4]   << 24) | ((uint32_t)block[i*4+1] << 16)
             | ((uint32_t)block[i*4+2] <<  8) |  (uint32_t)block[i*4+3];
    }
    for (int i = 16; i < 80; i++)
        W[i] = rol(W[i-3] ^ W[i-8] ^ W[i-14] ^ W[i-16], 1);

    uint32_t a = H[0], b = H[1], c = H[2], d = H[3], e = H[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if      (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;           k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c)|(b & d)|(c & d); k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;           k = 0xCA62C1D6; }
        uint32_t tmp = rol(a,5) + f + e + k + W[i];
        e = d; d = c; c = rol(b,30); b = a; a = tmp;
    }
    H[0] += a; H[1] += b; H[2] += c; H[3] += d; H[4] += e;
}

std::string ProgressStore::sha1Hex(const std::string& s) {
    uint32_t H[5] = { 0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0 };
    uint64_t len  = s.size();
    uint64_t bitLen = len * 8;

    // Process full 64-byte blocks
    size_t i = 0;
    for (; i + 64 <= len; i += 64)
        sha1Block(H, reinterpret_cast<const uint8_t*>(s.data() + i));

    // Padding
    uint8_t pad[128] = {};
    size_t rem = len - i;
    memcpy(pad, s.data() + i, rem);
    pad[rem] = 0x80;
    size_t padLen = (rem < 56) ? 64 : 128;
    // Store bit-length big-endian in last 8 bytes
    for (int k = 0; k < 8; k++)
        pad[padLen - 8 + k] = (uint8_t)(bitLen >> (56 - 8*k));
    sha1Block(H, pad);
    if (padLen == 128) sha1Block(H, pad + 64);

    // Cast explicitly: on devkitARM uint32_t is `unsigned long`, which does not
    // match %x's expected `unsigned int` even though both are 32-bit here.
    char out[41];
    snprintf(out, sizeof(out), "%08x%08x%08x%08x%08x",
             (unsigned)H[0], (unsigned)H[1], (unsigned)H[2],
             (unsigned)H[3], (unsigned)H[4]);
    return std::string(out);
}

//-----------------------------------------------------------------------------
// Minimal hand-rolled JSON serialiser / deserialiser.
// Only handles the fixed schema from §12.2 — not a general JSON library.
//-----------------------------------------------------------------------------

// Escape a string for JSON output
static std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else                out += c;
    }
    return out;
}

bool ProgressStore::saveIfDirty(const char* path) {
    if (!dirty_) return false;
    if (!save(path)) return false;
    dirty_ = false;
    return true;
}

bool ProgressStore::save(const char* path) const {
    // Write to .tmp then rename (atomic)
    std::string tmp = std::string(path) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return false;

    fprintf(f, "{\n  \"version\": 1,\n  \"books\": {\n");

    bool first = true;
    for (auto& [key, bp] : entries_) {
        if (!first) fprintf(f, ",\n");
        first = false;

        fprintf(f, "    \"%s\": {\n", key.c_str());
        fprintf(f, "      \"path\": \"%s\",\n", jsonEscape(bp.path).c_str());
        fprintf(f, "      \"chapter\": %d,\n", bp.chapter);
        fprintf(f, "      \"spread\": %d,\n", bp.spread);
        fprintf(f, "      \"chapter_count\": %d,\n", bp.chapter_count);
        fprintf(f, "      \"scroll_offset\": %d,\n", bp.scroll_offset);
        // Bookmarks are [chapter, spread] pairs.
        fprintf(f, "      \"bookmarks\": [");
        for (size_t i = 0; i < bp.bookmarks.size(); i++) {
            if (i > 0) fprintf(f, ", ");
            fprintf(f, "[%d, %d]", bp.bookmarks[i].chapter, bp.bookmarks[i].spread);
        }
        fprintf(f, "]\n    }");
    }

    fprintf(f, "\n  }\n}\n");
    fclose(f);

    // Atomic rename
    rename(tmp.c_str(), path);
    return true;
}

//-----------------------------------------------------------------------------
// Minimal JSON parser — reads exactly the schema produced by save().
// Uses a cursor-based approach: skip whitespace, match literals, extract values.
//-----------------------------------------------------------------------------
static const char* skipWS(const char* p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static bool match(const char*& p, char c) {
    p = skipWS(p);
    if (*p != c) return false;
    p++; return true;
}

// Returns content between next pair of quotes; advances p past closing quote
static std::string parseString(const char*& p) {
    p = skipWS(p);
    if (*p != '"') return "";
    p++;
    std::string out;
    while (*p && *p != '"') {
        if (*p == '\\' && *(p+1)) { p++; out += *p; }
        else out += *p;
        p++;
    }
    if (*p == '"') p++;
    return out;
}

static int parseInt(const char*& p) {
    p = skipWS(p);
    int sign = 1;
    if (*p == '-') { sign = -1; p++; }
    int v = 0;
    while (*p >= '0' && *p <= '9') { v = v*10 + (*p - '0'); p++; }
    return v * sign;
}

bool ProgressStore::load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return false; }

    std::string buf(sz, '\0');
    fread(buf.data(), 1, sz, f);
    fclose(f);

    const char* p = buf.c_str();
    // Skip to "books":
    const char* books = strstr(p, "\"books\"");
    if (!books) return true;  // empty but valid
    p = books + 7;
    if (!match(p, ':')) return false;
    if (!match(p, '{')) return false;

    while (true) {
        p = skipWS(p);
        if (*p == '}') break;
        if (*p == ',') { p++; continue; }

        std::string key = parseString(p);
        if (!match(p, ':')) break;
        if (!match(p, '{')) break;

        BookProgress bp;
        while (true) {
            p = skipWS(p);
            if (*p == '}') { p++; break; }
            if (*p == ',') { p++; continue; }

            std::string k = parseString(p);
            if (!match(p, ':')) break;

            if      (k == "path")          bp.path          = parseString(p);
            else if (k == "chapter")       bp.chapter       = parseInt(p);
            else if (k == "spread")        bp.spread        = parseInt(p);
            else if (k == "chapter_count") bp.chapter_count = parseInt(p);
            else if (k == "scroll_offset") bp.scroll_offset = parseInt(p);
            else if (k == "page") {
                // Legacy schema (global page index, pre chapter-scoped layout).
                // We cannot map it to a chapter without laying out the whole
                // book, so the position is dropped and reading restarts at the
                // beginning. Consumed here so the parser stays in sync.
                (void)parseInt(p);
            }
            else if (k == "bookmarks") {
                if (match(p, '[')) {
                    while (true) {
                        p = skipWS(p);
                        if (*p == ']') { p++; break; }
                        if (*p == ',') { p++; continue; }
                        if (*p == '[') {
                            // Current format: [chapter, spread]
                            p++;
                            Bookmark bm;
                            bm.chapter = parseInt(p);
                            match(p, ',');
                            bm.spread  = parseInt(p);
                            match(p, ']');
                            bp.bookmarks.push_back(bm);
                        } else {
                            // Legacy format: a bare global page index. Not
                            // convertible, so skip it rather than landing the
                            // user somewhere arbitrary.
                            (void)parseInt(p);
                        }
                    }
                }
            }
        }
        entries_[key] = bp;
    }
    // Freshly read from disk: memory and file agree.
    dirty_ = false;
    return true;
}

std::string ProgressStore::keyFor(const std::string& bookPath) {
    return sha1Hex(bookPath);
}

const BookProgress* ProgressStore::get(const std::string& bookPath) const {
    auto key = sha1Hex(bookPath);
    auto it  = entries_.find(key);
    if (it == entries_.end()) return nullptr;
    return &it->second;
}

//-----------------------------------------------------------------------------
// Equality over the fields that actually get persisted. Used to decide whether
// a set() is a real change — see the write-behind note in progress.h.
//-----------------------------------------------------------------------------
static bool sameProgress(const BookProgress& a, const BookProgress& b) {
    return a.chapter       == b.chapter       &&
           a.spread        == b.spread        &&
           a.scroll_offset == b.scroll_offset &&
           a.chapter_count == b.chapter_count &&
           a.path          == b.path          &&
           a.bookmarks     == b.bookmarks;
}

void ProgressStore::set(const BookProgress& bp) {
    set(sha1Hex(bp.path), bp);
}

void ProgressStore::set(const std::string& key, const BookProgress& bp) {
    auto it = entries_.find(key);
    if (it != entries_.end() && sameProgress(it->second, bp)) {
        // Nothing changed — most page turns re-save an entry identical to the
        // one already held (progress is written on the queue *and* again once
        // the deferred load completes). Marking the store dirty here would
        // schedule an SD write with nothing new to say.
        return;
    }

    BookProgress& slot = (it != entries_.end()) ? it->second : entries_[key];
    slot      = bp;
    slot.path = bp.path;   // ensure path is stored
    dirty_    = true;
}
