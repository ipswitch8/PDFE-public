// -*- coding: utf-8 -*-
#pragma once

// Minimal UTF-8 boundary arithmetic (phase-74 / WIN-49 prerequisite).
//
// WHY THIS EXISTS. `FormTool::editBuffer_` is a std::string holding UTF-8 while
// `cursorPosition_` was used as a raw BYTE index: Backspace erased exactly one
// byte, Left/Right stepped exactly one byte, and insert added the byte length.
// Every character outside ASCII — every CJK glyph, every accented Latin letter,
// every emoji — is 2-4 bytes, so a single Backspace split it mid-sequence and
// left INVALID UTF-8 in the field value, which is then written into the PDF.
// That is broken today, with no IME involved.
//
// The cursor stays a BYTE offset deliberately. Converting it to a codepoint
// index would mean translating at every one of the ~12 sites that index the
// buffer, and any site missed would be a silent mixed-unit bug — the same
// failure in a harder-to-see form. Keeping bytes and moving only in whole
// codepoints keeps the invariant local and checkable: the cursor must always
// sit on a codepoint boundary.
//
// Not a general Unicode library: no normalisation, no grapheme clusters. A
// combining sequence (e + U+0301) is two codepoints here, so Backspace removes
// the accent first. That is ordinary text-editor behaviour and is what the
// acceptance criterion asks for; grapheme-cluster editing is a separate,
// larger question and is recorded as such rather than half-implemented.

#include <cstddef>
#include <string>

namespace pdfe::core::utf8 {

/// True if the byte at `pos` starts a codepoint (i.e. is not a continuation
/// byte `10xxxxxx`). Positions at or past the end count as boundaries so
/// callers can use it on an exclusive end index.
inline bool isBoundary(const std::string& s, std::size_t pos)
{
    if (pos >= s.size()) return true;
    return (static_cast<unsigned char>(s[pos]) & 0xC0) != 0x80;
}

/// Byte offset of the codepoint boundary at or before `pos`. Clamps to the
/// string. Used to repair a cursor that some other code left mid-sequence
/// rather than trusting it.
inline std::size_t alignLeft(const std::string& s, std::size_t pos)
{
    if (pos > s.size()) return s.size();
    while (pos > 0 && !isBoundary(s, pos)) --pos;
    return pos;
}

/// Byte offset of the START of the codepoint that PRECEDES `pos`.
/// Returns 0 when `pos` is 0. Never returns a position inside a sequence.
inline std::size_t prevBoundary(const std::string& s, std::size_t pos)
{
    if (pos == 0) return 0;
    if (pos > s.size()) pos = s.size();
    --pos;
    while (pos > 0 && !isBoundary(s, pos)) --pos;
    return pos;
}

/// Byte offset just PAST the codepoint that begins at (or covers) `pos`.
/// Returns `s.size()` when already at or past the end.
///
/// Advances over continuation bytes rather than trusting the lead byte's
/// declared length: a truncated or malformed sequence must still make progress,
/// or a cursor movement loop hangs on it.
inline std::size_t nextBoundary(const std::string& s, std::size_t pos)
{
    const std::size_t n = s.size();
    if (pos >= n) return n;
    ++pos;
    while (pos < n && !isBoundary(s, pos)) ++pos;
    return pos;
}

/// Erase the whole codepoint immediately before `pos` (Backspace).
/// Returns the new cursor position. No-op at the start of the string.
inline std::size_t erasePrev(std::string& s, std::size_t pos)
{
    if (pos == 0) return 0;
    if (pos > s.size()) pos = s.size();
    const std::size_t start = prevBoundary(s, pos);
    s.erase(start, pos - start);
    return start;
}

/// Erase the whole codepoint at `pos` (Delete). Cursor does not move.
inline void eraseAt(std::string& s, std::size_t pos)
{
    if (pos >= s.size()) return;
    const std::size_t end = nextBoundary(s, pos);
    s.erase(pos, end - pos);
}

/// True if `s` is well-formed UTF-8. Used by the tests as the invariant the
/// whole header exists to preserve, and cheap enough to assert in debug paths.
///
/// Rejects over-long encodings, surrogates (D800-DFFF) and anything past
/// U+10FFFF, because "every byte has a plausible shape" is not the same as
/// "this is valid UTF-8" — and a test that only checks continuation-byte
/// patterns would pass on a split sequence that happened to land tidily.
inline bool isValid(const std::string& s)
{
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        std::size_t len = 0;
        char32_t cp = 0;
        if (c < 0x80)             { len = 1; cp = c; }
        else if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        else return false;                     // continuation or invalid lead

        if (i + len > n) return false;         // truncated sequence
        for (std::size_t k = 1; k < len; ++k) {
            if ((p[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if (len == 2 && cp < 0x80)    return false;   // over-long
        if (len == 3 && cp < 0x800)   return false;   // over-long
        if (len == 4 && cp < 0x10000) return false;   // over-long
        if (cp > 0x10FFFF)            return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;  // surrogate half
        i += len;
    }
    return true;
}

/// Number of codepoints in `s` (malformed bytes count as one each, so this
/// always terminates and never under-counts a damaged buffer).
inline std::size_t codepointCount(const std::string& s)
{
    std::size_t count = 0;
    for (std::size_t i = 0; i < s.size(); i = nextBoundary(s, i)) ++count;
    return count;
}

} // namespace pdfe::core::utf8
