// -*- coding: utf-8 -*-
// See XmlSafety.h for the rationale. Implementation notes only here.
#include "XmlSafety.h"

#include <cctype>
#include <cstddef>
#include <string>

namespace pdfe::core::xmlsafety {
namespace {

bool starts_with_at(std::string_view s, std::size_t pos, std::string_view lit)
{
    return s.size() - pos >= lit.size() && s.compare(pos, lit.size(), lit) == 0;
}

bool is_name_char(char c)
{
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) != 0 || c == '_' || c == '-' || c == '.' || c == ':';
}

// True when `lit` occurs at `pos` as a whole token (delimited by non-name
// characters on both sides). Prevents "MYSYSTEMID" matching "SYSTEM".
bool token_at(std::string_view s, std::size_t pos, std::string_view lit)
{
    if (!starts_with_at(s, pos, lit)) return false;
    if (pos > 0 && is_name_char(s[pos - 1])) return false;
    const std::size_t after = pos + lit.size();
    if (after < s.size() && is_name_char(s[after])) return false;
    return true;
}

// --- encoding normalisation (F27) -------------------------------------------
//
// The scan below is an ASCII lexer. pugixml's load_buffer() auto-detects the
// encoding of the same bytes, so anything it can decode this must also be able
// to read, or the guard is bypassable by re-encoding the payload.
//
// This is a FAITHFUL PORT of pugixml's `guess_buffer_encoding`
// (pugixml.cpp:2024-2071 in the vcpkg 1.15 build), in its order, not a
// paraphrase. The first cut of this function WAS a paraphrase -- it kept the
// four-byte patterns and dropped the two-byte fallback -- and the F27
// `security-audit` gate broke it with a working bypass. The ordering is load
// bearing in both directions:
//
//   * the UTF-32LE BOM `FF FE 00 00` BEGINS with the UTF-16LE BOM, so the
//     four-byte BOM tests must precede the two-byte ones;
//   * `3C 00 00 00` is UTF-32LE and also matches the two-byte UTF-16LE
//     fallback `3C 00`, so the four-byte CONTENT tests must precede that too.
//
// `XmlSafetyPugixmlDifferential.*` pins every branch below against what
// pugixml actually reports in `xml_parse_result::encoding`.

using Enc = Encoding;

} // namespace

Encoding sniffEncoding(std::string_view s) noexcept
{
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const std::size_t n = s.size();
    const auto at = [&](std::size_t k) -> unsigned {
        return k < n ? static_cast<unsigned>(p[k]) : 0x100u;  // never equal to a byte
    };
    const unsigned d0 = at(0), d1 = at(1), d2 = at(2), d3 = at(3);

    // --- BOM (pugixml :2031-2035) -------------------------------------------
    if (d0 == 0x00 && d1 == 0x00 && d2 == 0xFE && d3 == 0xFF) return Enc::Utf32BE;
    if (d0 == 0xFF && d1 == 0xFE && d2 == 0x00 && d3 == 0x00) return Enc::Utf32LE;
    if (d0 == 0xFE && d1 == 0xFF) return Enc::Utf16BE;
    if (d0 == 0xFF && d1 == 0xFE) return Enc::Utf16LE;
    if (d0 == 0xEF && d1 == 0xBB && d2 == 0xBF) return Enc::Utf8;

    // DELIBERATE DIVERGENCE, in the strict direction. pugixml bails out of
    // autodetection entirely for any buffer under four bytes (:2027) and calls
    // even a BOM'd one UTF-8; the tests above have no such floor, so a 2- or
    // 3-byte BOM'd buffer is transcoded here and is not by pugixml. Measured
    // and harmless: no DTD construct fits in fewer than four bytes, so neither
    // reading can carry a declaration, and being stricter than the parser can
    // only refuse -- never wave through. Pinned by
    // CoreXmlSafetyEncoding.ShortBuffersAreStricterThanPugixmlAndSayWhy.

    // --- `<`, `<?` or `<?xm` in various encodings (pugixml :2038-2043) ------
    // These MUST precede the two-byte fallback below (see the header comment).
    if (d0 == 0x00 && d1 == 0x00 && d2 == 0x00 && d3 == 0x3C) return Enc::Utf32BE;
    if (d0 == 0x3C && d1 == 0x00 && d2 == 0x00 && d3 == 0x00) return Enc::Utf32LE;
    if (d0 == 0x00 && d1 == 0x3C && d2 == 0x00 && d3 == 0x3F) return Enc::Utf16BE;
    if (d0 == 0x3C && d1 == 0x00 && d2 == 0x3F && d3 == 0x00) return Enc::Utf16LE;

    // --- two-byte fallback (pugixml :2046-2047) ----------------------------
    // "look for utf16 < followed by node name (this may fail, but is better
    // than utf8 since it's zero terminated so early)". THERE IS NO CONSTRAINT
    // ON BYTES 2-3. This is the branch the first cut of F27 omitted, and it is
    // the one that matters most in practice: the four-byte patterns above
    // additionally require the SECOND character to encode with a zero high
    // byte, i.e. codepoint < 0x100, so every document whose second character
    // is Greek, Cyrillic, CJK, Hebrew or Arabic falls through to here.
    //
    // Broadening the sniff this far cannot steal a legitimate UTF-8 document:
    // a UTF-8 XML document cannot begin `00 3C` or `3C 00`, because XML 1.0
    // sec. 2.2 excludes U+0000 from the legal Char production entirely. Pinned
    // by CoreXmlSafetyEncoding.BenignNonUtf8DocumentsAreStillAccepted and by
    // the whole benign corpus.
    if (d0 == 0x00 && d1 == 0x3C) return Enc::Utf16BE;
    if (d0 == 0x3C && d1 == 0x00) return Enc::Utf16LE;

    // pugixml then parses the XML declaration and may return `latin1`
    // (:2050-2069). Falling through to Utf8 here is CORRECT and is measured,
    // not assumed: latin1 and ASCII coincide over 0x00-0x7F, so every byte of
    // every construct this scanner looks for is identical under both, and no
    // byte value decodes to a structural character it was not already.
    // See CoreXmlSafetyEncoding.Latin1IsByteCompatibleWithTheAsciiScan.
    return Enc::Utf8;
}

namespace {

void append_utf8(std::string& out, unsigned cp)
{
    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) cp = 0xFFFDu;
    if (cp < 0x80u) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800u) {
        out += static_cast<char>(0xC0u | (cp >> 6));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    } else if (cp < 0x10000u) {
        out += static_cast<char>(0xE0u | (cp >> 12));
        out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    } else {
        out += static_cast<char>(0xF0u | (cp >> 18));
        out += static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
        out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    }
}

// Non-ASCII code points are re-encoded faithfully rather than dropped: the
// lexer's is_name_char() must see the same token boundaries the parser will.
std::string transcode_to_utf8(std::string_view s, Enc enc)
{
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const std::size_t n = s.size();
    std::string out;
    out.reserve(n);

    std::size_t i = 0;
    switch (enc) {
    case Enc::Utf32LE:
        if (n >= 4 && p[0] == 0xFF && p[1] == 0xFE && p[2] == 0 && p[3] == 0) i = 4;
        break;
    case Enc::Utf32BE:
        if (n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0xFE && p[3] == 0xFF) i = 4;
        break;
    case Enc::Utf16LE:
        if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) i = 2;
        break;
    case Enc::Utf16BE:
        if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) i = 2;
        break;
    case Enc::Utf8:
        break;
    }

    if (enc == Enc::Utf32LE || enc == Enc::Utf32BE) {
        for (; i + 4 <= n; i += 4) {
            const unsigned cp =
                (enc == Enc::Utf32LE)
                    ? (unsigned(p[i]) | (unsigned(p[i + 1]) << 8) |
                       (unsigned(p[i + 2]) << 16) | (unsigned(p[i + 3]) << 24))
                    : ((unsigned(p[i]) << 24) | (unsigned(p[i + 1]) << 16) |
                       (unsigned(p[i + 2]) << 8) | unsigned(p[i + 3]));
            append_utf8(out, cp);
        }
        return out;
    }

    const bool le = (enc == Enc::Utf16LE);
    for (; i + 2 <= n; i += 2) {
        const unsigned hi = le ? (unsigned(p[i]) | (unsigned(p[i + 1]) << 8))
                               : ((unsigned(p[i]) << 8) | unsigned(p[i + 1]));
        if (hi >= 0xD800u && hi <= 0xDBFFu && i + 4 <= n) {
            const unsigned lo =
                le ? (unsigned(p[i + 2]) | (unsigned(p[i + 3]) << 8))
                   : ((unsigned(p[i + 2]) << 8) | unsigned(p[i + 3]));
            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                append_utf8(out,
                            0x10000u + ((hi - 0xD800u) << 10) + (lo - 0xDC00u));
                i += 2;
                continue;
            }
        }
        append_utf8(out, hi);
    }
    return out;
}

Verdict scan_utf8(std::string_view xml, Policy policy);
std::size_t doctype_group(std::string_view xml, std::size_t s) noexcept;

} // namespace

const char* describe(Reject r) noexcept
{
    switch (r) {
    case Reject::Ok:                 return "accepted";
    case Reject::DoctypeDeclaration: return "document type (DTD) declaration";
    case Reject::EntityDeclaration:  return "entity declaration";
    case Reject::ExternalIdentifier: return "external identifier (SYSTEM/PUBLIC)";
    case Reject::ParameterEntityRef: return "parameter-entity reference";
    case Reject::EncodingNotUnderstood:
        return "non-UTF-8 encoding that could not be normalised for inspection";
    }
    return "unknown";
}

Verdict inspect(std::string_view xml, Policy policy) noexcept
{
    const Enc enc = sniffEncoding(xml);
    if (enc == Enc::Utf8) {
        return scan_utf8(xml, policy);
    }
    // The parser would decode these bytes; so must the guard. Transcoding
    // allocates, and inspect() is noexcept, so an allocation failure must fail
    // CLOSED rather than terminate or silently accept.
    try {
        const std::string normalised = transcode_to_utf8(xml, enc);
        return scan_utf8(normalised, policy);
    } catch (...) {
        Verdict v;
        v.reason = Reject::EncodingNotUnderstood;
        try {
            v.detail = "XML is not UTF-8 and could not be normalised for "
                       "inspection; PDFE refuses input its guard cannot read";
        } catch (...) {
        }
        return v;
    }
}

namespace {

// ---------------------------------------------------------------------------
// DOCTYPE STRUCTURE -- a faithful mirror of pugixml's three mutually-recursive
// scanners (pugixml.cpp:2983-3090 in the vcpkg 1.15 build).
//
// pugixml states the grammar these implement in its own header comment:
//
//     // DOCTYPE consists of nested sections of the following possible types:
//     // <!-- ... -->, <? ... ?>, "...", '...'
//     // <![...]]>
//     // <!...>
//     // First group can not contain nested groups
//     // Second group can contain nested groups of the same type
//     // Third group can contain all other groups
//
// WHY THESE ARE PORTS AND NOT APPROXIMATIONS
// ------------------------------------------
// Four of this phase's `security-audit` gate findings were one defect wearing
// four faces: a hand-written boundary rule computed a SHORTER extent than the
// parser computes, so bytes pugixml still considers part of the DTD were handed
// back to the element-content rules, where the CDATA-inert rule then hid an
// `<!ENTITY ... SYSTEM ...>` from every detection path. Three hand-written
// approximations of three mutually-recursive functions produced four bypasses,
// each found by construction after the previous fix looked complete.
//
// So the structure is mirrored directly rather than approximated: one function
// per counterpart, named for it, in its order, with its exact predicates.
// `XmlSafetyPugixmlDoctypeDifferential` pins the resulting extent against what
// pugixml itself reports through a `parse_doctype` node, so a fifth divergence
// fails a test instead of shipping.
//
// FAILURE DIRECTION, UNIFORM AND DELIBERATE. Every error path returns `n`, the
// end of the buffer -- never a partial extent. pugixml THROWS
// status_bad_doctype on all of them, and a document that will not parse cannot
// carry a reachable attack, so the longest possible extent is both safe and
// cheap. A SHORT extent is the bug; a LONG one is at worst a refusal.
// ---------------------------------------------------------------------------

// Out-of-range yields 0x100, which is never equal to any byte. That folds the
// bounds checks into the character comparisons instead of scattering them
// through every predicate, where one omission is a crash.
unsigned byte_at(std::string_view s, std::size_t k) noexcept
{
    return k < s.size() ? static_cast<unsigned>(static_cast<unsigned char>(s[k]))
                        : 0x100u;
}

// <-> pugixml `parse_doctype_primitive` (:2983-3015).
// Quoted literal, `<? ... ?>`, or `<!-- ... -->`. Cannot contain nested groups.
// Anything else is status_bad_doctype -> `n`.
std::size_t doctype_primitive(std::string_view xml, std::size_t s) noexcept
{
    const std::size_t n = xml.size();
    const unsigned c0 = byte_at(xml, s);

    if (c0 == '"' || c0 == '\'') {
        const char quote = static_cast<char>(c0);
        ++s;
        while (s < n && xml[s] != quote) ++s;
        return (s < n) ? s + 1 : n;
    }
    if (c0 == '<' && byte_at(xml, s + 1) == '?') {
        const std::size_t e = xml.find("?>", s + 2);
        return (e == std::string_view::npos) ? n : e + 2;
    }
    if (c0 == '<' && byte_at(xml, s + 1) == '!' && byte_at(xml, s + 2) == '-' &&
        byte_at(xml, s + 3) == '-') {
        const std::size_t e = xml.find("-->", s + 4);
        return (e == std::string_view::npos) ? n : e + 3;
    }
    return n;
}

// <-> pugixml `parse_doctype_ignore` (:3017-3046).
// `<![ ... ]]>`, DEPTH-COUNTED: a nested `<![` raises the depth and only a
// `]]>` at depth zero ends the section. Returning at the FIRST `]]>` was the
// gate's THIRD finding -- the leftover ` ]]> ` was then read as subset text and
// its leading `]` taken for the subset's close.
//
// DIVERGENCE, DELIBERATE AND MATCHED: pugixml does NO quote tracking here, so
// a `]]>` inside a quoted literal ends the section for it. An earlier cut of
// this function skipped quoted literals, which made the section run LONGER
// than pugixml's. Matched to pugixml instead.
//
// THE JUSTIFICATION ORIGINALLY WRITTEN HERE WAS FALSE, and correcting it is
// worth more than the code. It claimed the divergence was UNOBSERVABLE --
// that every document distinguishing the two readings is one pugixml refuses,
// so no verdict could depend on the choice and no test could kill a mutation
// reintroducing tracking. That came from sampling BALANCED-quote documents,
// all of which pugixml does refuse (the leftover quote opens a primitive that
// never closes). The F27 security-audit gate disproved it with an ODD number
// of quotes, where the unbalanced quote is consumed as the closing half of a
// later primitive and the document still parses:
//
//     <!DOCTYPE html [ <![X[ "]]>" y" ]><r/>
//       pugixml: ACCEPTED, doctype ends at 34
//       here (no tracking): 34      with tracking: 38
//
// DIRECTION, and it is measured rather than argued. Tracking can only make the
// section scan SKIP a region, which moves the cursor forward past candidate
// `]]>` terminators and never back -- so a tracking section end is always >=
// a non-tracking one, and the extent with it. A LONGER extent is the safe
// direction against a bypass but the WRONG one for false positives: element
// content the parser treats as element content would be scanned as DTD, and a
// legitimate `<![CDATA[ ... ]]>` quoting markup as text after the DOCTYPE
// would be refused -- "a functional regression wearing a security control's
// badge", which XmlSafety.h names as how controls get switched off. Under the
// mutation that consequence is observed directly: the legitimate document in
// the test below is refused as an external identifier.
//
// Pinned by
// CoreXmlSafetyDoctypeStructure.MarkedSectionQuoteHandlingMatchesPugixmlOnDocumentsThatDistinguishThem,
// which asserts this function's extent against the extent pugixml itself
// reports for documents the parser ACCEPTS.
std::size_t doctype_ignore(std::string_view xml, std::size_t s) noexcept
{
    const std::size_t n = xml.size();
    std::size_t depth = 0;
    s += 3;  // past "<!["

    while (s < n) {
        if (byte_at(xml, s) == '<' && byte_at(xml, s + 1) == '!' &&
            byte_at(xml, s + 2) == '[') {
            s += 3;
            ++depth;
        } else if (byte_at(xml, s) == ']' && byte_at(xml, s + 1) == ']' &&
                   byte_at(xml, s + 2) == '>') {
            s += 3;
            if (depth == 0) return s;
            --depth;
        } else {
            ++s;
        }
    }
    return n;
}

// <-> pugixml `parse_doctype_group` (:3048-3090).
// `<! ... >`, DEPTH-COUNTED against nested `<!...` control groups, delegating
// `<![` to doctype_ignore and `<` / `"` / `'` to doctype_primitive.
//
// The depth counting is the gate's FOURTH finding. `<!ELEMENT x <!FOO> >` is
// ONE group to pugixml -- `<!FOO` raises the depth, the first `>` lowers it,
// and the declaration ends at the SECOND `>`. A stepper that stops at the first
// unquoted `>` resumes inside bytes the parser still considers part of the
// declaration, and a `]` sitting there is then read as the subset's close.
// Measured: `<!DOCTYPE html [ <!1 <!2> ] > <![CDATA[<!ENTITY e SYSTEM ...>]]> ]>`
// was ACCEPTED, parsed, pageWidth 800.
//
// Returns the offset just PAST the closing `>` (pugixml returns the offset OF
// it); this is an exclusive bound, which is what the callers compare against.
std::size_t doctype_group(std::string_view xml, std::size_t s) noexcept
{
    const std::size_t n = xml.size();
    std::size_t depth = 0;
    s += 2;  // past "<!"

    while (s < n) {
        const unsigned c0 = byte_at(xml, s);

        if (c0 == '<' && byte_at(xml, s + 1) == '!' &&
            byte_at(xml, s + 2) != '-') {
            if (byte_at(xml, s + 2) == '[') {
                s = doctype_ignore(xml, s);
                if (s >= n) return n;
            } else {
                s += 2;
                ++depth;
            }
        } else if (c0 == '<' || c0 == '"' || c0 == '\'') {
            s = doctype_primitive(xml, s);
            if (s >= n) return n;
        } else if (c0 == '>') {
            if (depth == 0) return s + 1;
            --depth;
            ++s;
        } else {
            ++s;
        }
    }
    return n;  // depth != 0, or no closing '>' -- pugixml throws here
}

// NO pugixml counterpart -- pugixml never looks for parameter-entity
// references, because it never processes DTD content at all. This walks the
// internal subset's own TEXT for a bare `%name;` RECALL, the second half of the
// standard blind-XXE construction:
//     <!DOCTYPE r [ <!ENTITY % p SYSTEM "http://h/"> %p; ]>
// XmlSafety.h lists parameter-entity references among "the primitives that
// actually matter" under BOTH policies.
//
// Nested constructs are stepped over by the ports above, so this agrees with
// the parser about where a construct ENDS even though it looks for something
// the parser never looks for. It also reports where the subset closes
// (`endOut`), which the outer scan folds into its DTD extent.
//
// DIVERGENCE, DELIBERATE: a bare quoted literal in subset TEXT is not skipped
// here, though pugixml's group would treat it as a primitive. A quoted literal
// sitting between declarations is not valid DTD, and not skipping it means a
// `%pe;` hidden inside one is still flagged -- the stricter direction, and the
// one this function exists for. Pinned by
// CoreXmlSafetyDoctypeStructure.SubsetTextQuotesAreNotSkippedAndWhy.
bool scan_internal_subset(std::string_view xml, std::size_t start,
                          std::size_t& endOut) noexcept
{
    const std::size_t n = xml.size();
    bool sawParamRef = false;
    std::size_t q = start;

    while (q < n) {
        const unsigned c = byte_at(xml, q);

        if (c == ']') { endOut = q; return sawParamRef; }

        if (c == '<') {
            const unsigned c1 = byte_at(xml, q + 1);
            const unsigned c2 = byte_at(xml, q + 2);
            if (c1 == '!' && c2 == '[') {
                q = doctype_ignore(xml, q);          // marked section
            } else if ((c1 == '!' && c2 == '-') || c1 == '?') {
                q = doctype_primitive(xml, q);       // comment or PI
            } else if (c1 == '!') {
                q = doctype_group(xml, q);           // declaration, depth-aware
            } else {
                ++q;                                 // not markup we know
            }
            continue;
        }

        if (c == '%') {
            std::size_t r = q + 1;
            while (r < n && is_name_char(xml[r])) ++r;
            if (r > q + 1 && r < n && xml[r] == ';') {
                sawParamRef = true;   // keep walking: `endOut` is still needed
                q = r + 1;
                continue;
            }
            q = (r > q) ? r : q + 1;
            continue;
        }

        ++q;
    }

    endOut = n;   // never closed -> the rest of the buffer is subset
    return sawParamRef;
}

Verdict scan_utf8(std::string_view xml, Policy policy)
{
    bool sawDoctype       = false;
    bool sawEntityDecl    = false;
    bool sawOtherDecl     = false;
    bool sawExternalId    = false;   // in a declaration OTHER than the DOCTYPE
    bool sawDoctypeExtId  = false;   // the DOCTYPE's OWN PUBLIC/SYSTEM id
    bool sawParamRef      = false;

    const std::size_t n = xml.size();
    std::size_t i = 0;

    // Where the DTD currently being scanned ends, as an absolute offset. 0
    // means "not in a DTD" (no byte can precede offset 0). This exists because
    // the CDATA rule below is CONTEXT-DEPENDENT and was once applied
    // unconditionally -- see the comment there.
    //
    // TWO independent readings are folded in, and the LARGER always wins:
    //
    //   * `doctype_group()` -- the parser's own notion, the offset just past
    //     the `>` that closes the whole `<!DOCTYPE ...>`. This is the
    //     authoritative one, because it is a port of what pugixml computes.
    //   * the internal subset's closing `]`, from `scan_internal_subset()`.
    //     pugixml has no notion of the subset at all -- it treats `[` and `]`
    //     as ordinary characters inside the group -- so this reading exists
    //     only for PDFE's own `%pe;` scanning. Folding it in costs nothing and
    //     covers the case where a stray depth-0 `>` ends the group early.
    //
    // Held as a MAXIMUM, never overwritten downward. Nested constructs open
    // extents of their own, and letting an inner, shorter one replace the
    // enclosing extent would declare the DTD finished while still inside it and
    // re-open the hole a few bytes further on.
    //
    // NOTE, and it is the gate's own correction of an earlier claim made here:
    // this maximum rule governs how extents are COMBINED. It cannot rescue an
    // extent computed too small in the first place -- it will faithfully
    // preserve a wrong value. That is why the computations above are PORTS
    // rather than approximations. An invariant over the combination step is no
    // guarantee about its inputs.
    std::size_t dtdEnd = 0;

    while (i < n) {
        const std::size_t lt = xml.find('<', i);
        if (lt == std::string_view::npos) break;

        // A COMMENT is inert in BOTH contexts. XML 1.0 sec. 2.5 makes its
        // content non-markup wherever it appears, and sec. 2.8 explicitly
        // admits comments in the internal subset, so this skip is
        // unconditional and must stay that way.
        if (starts_with_at(xml, lt, "<!--")) {
            const std::size_t end = xml.find("-->", lt + 4);
            i = (end == std::string_view::npos) ? n : end + 3;
            continue;
        }

        // A CDATA SECTION is inert ONLY IN ELEMENT CONTENT -- and that
        // distinction is the whole point of the `lt >= dtdEnd` guard.
        //
        // XML 1.0 sec. 2.7 says a CDATA section's content is character data,
        // so a `<!ENTITY …>` written inside one in element content really is
        // text and really must not be flagged. But sec. 2.8 does not admit
        // CDATA sections in the internal DTD subset AT ALL: the subset takes
        // markup declarations, PEReferences, comments, PIs and conditional
        // sections, and a CDATA section is none of those. So inside a subset
        // there is no "its contents are text" rule to apply, and the bytes
        // skipped as text were bytes no conforming parser would ever treat as
        // text.
        //
        // Applying the element-content rule inside the DTD was this scanner's
        // second gate-found bypass (F27): measured,
        //     <!DOCTYPE html [ <![CDATA[<!ENTITY xxe SYSTEM "http://h/">]]> ]>
        // was ACCEPTED and pugixml parsed the document (hOCR pageWidth 800).
        // `<![INCLUDE[` and `<![IGNORE[` wrapping the same declaration were
        // caught, because they do not match this literal and fall through to
        // the declaration path -- an asymmetry that pointed straight at the
        // cause. Inside the subset every `<![…` construct now takes the same
        // path, which turns that incidental catch into a designed one.
        if (lt >= dtdEnd && starts_with_at(xml, lt, "<![CDATA[")) {
            const std::size_t end = xml.find("]]>", lt + 9);
            i = (end == std::string_view::npos) ? n : end + 3;
            continue;
        }

        if (lt + 1 >= n || xml[lt + 1] != '!') {
            i = lt + 1;
            continue;
        }

        // --- a markup declaration: <!NAME ... >
        std::size_t p = lt + 2;
        const std::size_t nameStart = p;
        while (p < n && std::isalpha(static_cast<unsigned char>(xml[p]))) ++p;
        const std::string_view name = xml.substr(nameStart, p - nameStart);

        const bool isDoctype = (name == "DOCTYPE");
        if (isDoctype) {
            sawDoctype = true;
            // The parser's own extent for this declaration. Only computed when
            // not already inside a known DTD region: that keeps a document full
            // of `<!DOCTYPE` tokens linear rather than quadratic, and a nested
            // one cannot extend the region past its parent anyway.
            if (lt >= dtdEnd) {
                const std::size_t de = doctype_group(xml, lt);
                if (de > dtdEnd) dtdEnd = de;
            }
        }
        else if (name == "ENTITY")  sawEntityDecl = true;
        else if (!name.empty())     sawOtherDecl = true;  // ELEMENT/ATTLIST/NOTATION

        // Walk the declaration body. Quoted literals may contain '>' and '<',
        // so they are skipped wholesale. A '[' opens the internal subset,
        // whose nested declarations are found by resuming the outer scan.
        bool inSingle = false;
        bool inDouble = false;
        while (p < n) {
            const char c = xml[p];
            if (inSingle) {
                if (c == '\'') inSingle = false;
                ++p;
                continue;
            }
            if (inDouble) {
                if (c == '"') inDouble = false;
                ++p;
                continue;
            }
            if (c == '\'') { inSingle = true; ++p; continue; }
            if (c == '"')  { inDouble = true; ++p; continue; }
            if (c == '[') {
                // The DOCTYPE internal subset. Its nested DECLARATIONS are
                // found by the outer scan, which resumes just past this '['
                // -- but the subset's own text between them is never revisited
                // there, and that is exactly where a `%pe;` RECALL sits in the
                // standard blind-XXE construction:
                //     <!DOCTYPE r [ <!ENTITY % p SYSTEM "http://h/"> %p; ]>
                // XmlSafety.h lists parameter-entity references among "the
                // primitives that actually matter" under BOTH policies, so the
                // subset text is walked here.
                //
                // The walk ALSO reports where the subset ends, and the outer
                // scan needs that: several of its rules (the CDATA one above,
                // most importantly) are correct in element content and wrong
                // inside a DTD.
                std::size_t end = n;
                if (scan_internal_subset(xml, p + 1, end)) sawParamRef = true;
                if (end > dtdEnd) dtdEnd = end;
                ++p;
                break;   // internal subset -> outer scan finds the declarations
            }
            if (c == '>')  { ++p; break; }
            if (c == '%') {
                std::size_t q = p + 1;
                while (q < n && is_name_char(xml[q])) ++q;
                if (q > p + 1 && q < n && xml[q] == ';') sawParamRef = true;
                ++p;
                continue;
            }
            if (token_at(xml, p, "SYSTEM") || token_at(xml, p, "PUBLIC")) {
                // Attribute the external id to the declaration it sits in: a
                // DOCTYPE's own external subset is never fetched by pugixml or
                // QXmlStreamReader, whereas one on an ENTITY declaration is the
                // XXE primitive itself.
                if (isDoctype) sawDoctypeExtId = true;
                else           sawExternalId   = true;
                ++p;
                continue;
            }
            ++p;
        }
        i = p;
    }

    Verdict v;
    // Most specific reason first: an external identifier is the SSRF / file
    // disclosure primitive, an entity declaration is the expansion primitive,
    // and a bare DTD is the enabling construct for both.
    const bool strict = (policy == Policy::Strict);
    if (sawExternalId || (strict && sawDoctypeExtId)) {
        v.reason = Reject::ExternalIdentifier;
        v.detail = "XML declares an external identifier (SYSTEM/PUBLIC); "
                   "PDFE never resolves external entities or DTDs";
    } else if (sawEntityDecl) {
        v.reason = Reject::EntityDeclaration;
        v.detail = "XML declares an entity; PDFE refuses entity declarations "
                   "(entity-expansion / XXE attack class)";
    } else if (sawParamRef) {
        v.reason = Reject::ParameterEntityRef;
        v.detail = "XML references a parameter entity; PDFE refuses DTD "
                   "parameter entities";
    } else if (sawOtherDecl || (strict && sawDoctype)) {
        v.reason = Reject::DoctypeDeclaration;
        v.detail = "XML carries a document type (DTD) declaration; PDFE "
                   "accepts no DTD in XMP, hOCR or XFDF input";
    }
    return v;
}

} // namespace

std::size_t doctypeExtent(std::string_view xml, std::size_t pos) noexcept
{
    return doctype_group(xml, pos);
}

} // namespace pdfe::core::xmlsafety
