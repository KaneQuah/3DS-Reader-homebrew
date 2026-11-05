#pragma once
// utf8.h — the project's single UTF-8 decoder.
//
// There used to be two copies of this function, one in font_engine.cpp and one
// in page_layout.cpp (whose comment already said "duplicate of
// font_engine.cpp version — consider moving to util"). Both read past the end
// of the buffer on a truncated multi-byte sequence:
//
//   font_engine.cpp:  uint8_t b = (uint8_t)*p++;
//                     if ((b & 0xC0) != 0x80) return 0xFFFD;
//   page_layout.cpp:  while (extra-- > 0) { uint8_t b = (uint8_t)*p++; ... }
//
// Both increment `p` BEFORE (or instead of) testing the byte, so a string
// ending mid-sequence — "...\xC3" — consumes the NUL terminator and leaves `p`
// pointing past the allocation. Every caller drives this through
//
//   while ((cp = utf8Next(p)) != 0) { ... }
//
// so decoding then walks off the end of the string and keeps going until it
// happens to land on a zero byte: an out-of-bounds read of unbounded length.
//
// That is reachable from ordinary input, not just hostile files. TxtParser cuts
// at a fixed byte count (STREAM_THRESHOLD, and readChunk's chunk size), and a
// cut lands mid-character for any multi-byte text.
//
// The version below tests each continuation byte before advancing, so a
// truncated sequence stops ON the terminator: it reports U+FFFD and the
// caller's loop ends on the next call, inside the buffer.

#include <cstdint>

/// Decode the next codepoint from a NUL-terminated UTF-8 string and advance
/// `p` past it. Returns 0 at end of string, or U+FFFD for an invalid or
/// truncated sequence. `p` is never advanced past the NUL terminator.
inline uint32_t utf8Next(const char*& p) {
    uint8_t c = (uint8_t)*p;
    if (c == 0) return 0;
    p++;
    if (c < 0x80) return c;

    uint32_t cp;
    int      extra;
    if      ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else return 0xFFFD;   // stray continuation byte, or a 5+ byte lead

    while (extra-- > 0) {
        uint8_t b = (uint8_t)*p;
        // Checked before the increment. A NUL fails this test (0x00 & 0xC0 is
        // not 0x80), so a truncated sequence leaves `p` on the terminator and
        // the next call returns 0 rather than reading past it.
        if ((b & 0xC0) != 0x80) return 0xFFFD;
        p++;
        cp = (cp << 6) | (b & 0x3F);
    }
    return cp;
}
