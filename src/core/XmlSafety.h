// -*- coding: utf-8 -*-
//
// Meta-audit 2026-07 follow-up, phase F13 (SEC-40 / SEC-41 / SEC-42):
// a single fail-closed pre-parse guard for every attacker-reachable XML
// entry point in PDFE.
//
// WHY THIS EXISTS
// ---------------
// PDFE parses attacker-controlled XML in three places:
//
//   * src/compliance/XmpMetadataWriter.cpp -- the /Metadata XMP packet of an
//     opened PDF, via pugixml load_buffer() (`:447`; it was load_string()
//     until follow-up phase F36 -- see below);
//   * src/tre/HocrParser.cpp -- hOCR markup, via pugixml load_buffer() (`:676`);
//   * src/forms/FormDataIO.cpp -- imported XFDF, via QXmlStreamReader.
//
// Neither library expands external entities by default (pugixml has no DTD
// entity machinery at all; QXmlStreamReader refuses to resolve external
// entities and caps internal entity expansion). That is a LIBRARY-PROVIDED
// property, not something PDFE implemented -- and phase-4 of the meta audit
// found that nothing in the tree ASSERTED it, so a future library swap,
// a pugixml `parse_doctype`/custom-entity option, or a Qt behaviour change
// could silently re-open XXE and nothing would notice.
//
// This guard turns the implicit property into an explicit, PDFE-owned,
// testable refusal: any document that so much as DECLARES a DTD, an entity,
// or an external identifier is rejected BEFORE it reaches a parser. None of
// the three formats above has any legitimate need for a DTD:
//
//   * XMP (ISO 16684-1 §7.1) forbids a DOCTYPE in an XMP packet outright;
//   * hOCR is PDFE's own Tesseract output re-parsed, and never carries one;
//   * XFDF (ISO 32000-2 §12.7.8) declares no entities beyond the XML built-ins.
//
// So the refusal costs no legitimate input and removes the entire entity
// attack class (XXE, billion-laughs / quadratic blowup, external DTD SSRF)
// from PDFE's own code rather than delegating it to a dependency's defaults.
//
// NOT A VULNERABILITY CLAIM. No exploitable XXE was demonstrated in PDFE
// during phase F13; see docs/audit/meta-audit-2026-07/followup/
// phase-13-dispositions.md, where the residual risk is recorded as a
// hypothesis with reproduction steps rather than as a confirmed defect.
#pragma once

#include <string>
#include <string_view>

namespace pdfe::core::xmlsafety {

/// Why a document was refused. `Ok` is the only accepted value.
enum class Reject {
    Ok = 0,
    DoctypeDeclaration,   ///< `<!DOCTYPE ...>` present (DTDs are never accepted)
    EntityDeclaration,    ///< `<!ENTITY ...>` present (internal or external)
    ExternalIdentifier,   ///< a `SYSTEM` / `PUBLIC` external id in a markup decl
    ParameterEntityRef,   ///< a `%name;` parameter-entity reference in a decl
    /// The bytes are not UTF-8 (a UTF-16/UTF-32 BOM or byte pattern) AND could
    /// not be normalised to UTF-8 for inspection. Fail-closed: the scan refuses
    /// what it cannot read rather than waving it through. See `inspect()`.
    EncodingNotUnderstood,
};

struct Verdict {
    Reject reason = Reject::Ok;
    /// Human-readable explanation; empty when `reason == Reject::Ok`.
    std::string detail;

    bool safe() const { return reason == Reject::Ok; }
    explicit operator bool() const { return safe(); }
};

/// Maximum internal entity expansion PDFE will let QXmlStreamReader perform.
/// Qt's own default is 4096; XFDF needs none at all, so the value is set low
/// enough that a nested-entity bomb trips it on the first level of nesting
/// while leaving room for the built-in `&amp; &lt; &gt; &quot; &apos;`.
inline constexpr int kEntityExpansionLimit = 16;

/// How strict `inspect()` should be about a bare document type declaration.
///
/// `Strict` is correct for formats whose own specifications forbid a DTD —
/// XMP (ISO 16684-1 §7.1) and XFDF (ISO 32000-2 §12.7.8). There, a DOCTYPE is
/// evidence of tampering on its own.
///
/// `AllowDoctype` exists for hOCR, and the reason is worth stating plainly:
/// **real hOCR legitimately carries an XHTML DOCTYPE.** Tesseract emits
/// `<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.0 Transitional//EN" "...dtd">`
/// at the top of every hOCR document. Rejecting that would break OCR on valid
/// input — a functional regression dressed up as a security control, which is
/// how security controls get switched off. So hOCR is scanned for the
/// primitives that actually matter (entity declarations, parameter-entity
/// references, and external identifiers anywhere OTHER than the DOCTYPE's own
/// external id) while the plain XHTML doctype is allowed through. pugixml
/// never fetches a DOCTYPE's external subset, so allowing it costs nothing.
enum class Policy {
    Strict,        ///< any DTD construct is refused (XMP, XFDF)
    AllowDoctype,  ///< a bare/PUBLIC-identified DOCTYPE is permitted (hOCR)
};

/// Scan `xml` for DTD / entity / external-identifier constructs.
///
/// Deliberately a pre-parse LEXICAL scan, not a parse: it must be able to
/// refuse input that a parser has not yet looked at, and it must not itself
/// be a new attack surface. It skips comments, CDATA sections and quoted
/// literals inside markup declarations so that a `<!DOCTYPE` appearing in a
/// comment or as element text is not mistaken for a declaration.
///
/// The encoding `inspect()` normalises FROM before it scans. Exposed —
/// deliberately, as the only non-`inspect` entry point in this header — so that
/// `XmlSafetyPugixmlDifferential.*` can pin it against `xml_parse_result::
/// encoding`, i.e. against what pugixml ACTUALLY detected, rather than against
/// a table someone typed out by hand.
///
/// The reason that seam exists is recorded rather than tidied away: the first
/// cut of this sniff was hand-mirrored from a reading of pugixml's
/// `guess_buffer_encoding` and **was wrong on the day it was written** — it
/// implemented the 4-byte patterns (pugixml.cpp:2038-2043) and missed the
/// 2-byte fallback at :2046-2047, so any document whose SECOND character was
/// >= U+0100 (Greek, Cyrillic, CJK, Hebrew, Arabic — most of the world) was
/// scanned as UTF-8 while pugixml decoded the whole buffer as UTF-16. The F27
/// `security-audit` gate found it with a working exploit. A hand-mirrored
/// heuristic with no differential test is the same shape of defect F27 was
/// chartered to fix; the differential test is what stops it recurring under the
/// next pugixml bump.
///
/// `Latin1` is deliberately absent. pugixml also detects it (from the XML
/// declaration, :2053-2069), but latin1 and ASCII coincide over 0x00-0x7F, so
/// every byte of `<!DOCTYPE`, `<!ENTITY`, `SYSTEM`, `PUBLIC`, `%`, `;` is
/// identical under both and the UTF-8 scan reads a latin1 document correctly.
/// That is MEASURED, not assumed — see
/// `CoreXmlSafetyEncoding.Latin1IsByteCompatibleWithTheAsciiScan`.
enum class Encoding {
    Utf8,      ///< also the fall-through for latin1 and for undetected input
    Utf16LE,
    Utf16BE,
    Utf32LE,
    Utf32BE,
};

/// Classify `xml` the way `pugi::xml_document::load_buffer` would.
/// Never throws; a short or empty buffer yields `Encoding::Utf8`.
Encoding sniffEncoding(std::string_view xml) noexcept;

/// End offset (EXCLUSIVE) of the `<!DOCTYPE ...>` declaration beginning at
/// `pos`, computed the way pugixml's `parse_doctype_group` computes it:
/// depth-counted over nested `<!...` control groups, delegating `<![...]]>`
/// marked sections and `<!-- -->` / `<? ?>` / quoted literals to their own
/// rules. Returns `xml.size()` when the declaration is unterminated or
/// malformed — pugixml throws `status_bad_doctype` in exactly those cases, and
/// a document that will not parse cannot carry a reachable attack, so the
/// longest possible extent is the safe answer.
///
/// Exposed for the same reason as `sniffEncoding`: this is a hand-maintained
/// mirror of a library function, and four `security-audit` gate findings in
/// phase F27 were all the same defect — an extent computed SHORTER than the
/// parser's, which let DTD bytes be re-classified as element content where the
/// CDATA-inert rule then hid an entity declaration. `XmlSafetyPugixmlDoctype
/// Differential` pins this against the extent pugixml itself reports through a
/// `parse_doctype` node, so the next divergence fails a test rather than
/// shipping.
///
/// `pos` must index a `<` that begins a `<!DOCTYPE`; behaviour is defined but
/// uninteresting otherwise. Never throws.
std::size_t doctypeExtent(std::string_view xml, std::size_t pos) noexcept;

/// ENCODING NORMALISATION (meta-audit follow-up F27).
///
/// The scan is an ASCII lexer, but pugixml's `load_buffer()` auto-detects the
/// encoding of the SAME buffer -- from a BOM, and, failing that, from the NUL
/// pattern of the first four bytes (`guess_buffer_encoding`). A UTF-16LE hOCR
/// document encodes `<` as `3C 00`, so a literal `<!ENTITY x SYSTEM "...">`
/// inside it matched nothing in the raw ASCII scan while pugixml decoded and
/// parsed the document normally. Measured in F27: the guard was fully bypassed
/// (verdict `accepted`, document parsed, `pageWidth` 800) both WITH and WITHOUT
/// a BOM. The attack did not pay off only because pugixml has no entity
/// machinery -- i.e. the control protecting PDFE was the library's, not ours.
///
/// So `inspect()` now sniffs the encoding the same way the parser does (see
/// `sniffEncoding` above) and transcodes UTF-16/UTF-32 input to UTF-8 before
/// scanning. It TRANSCODES
/// rather than rejects on purpose: XMP packets may legitimately be UTF-16
/// (ISO 16684-1), and refusing them would be a functional regression wearing a
/// security control's badge. `EncodingNotUnderstood` is returned only when the
/// normalisation itself cannot be completed.
///
/// F36 CLOSED THE OTHER HALF OF THAT SENTENCE. When F27 wrote "XMP packets may
/// legitimately be UTF-16", `XmpMetadataWriter` could not actually read one:
/// it parsed with `load_string(xmpData.c_str())`, which stops at the first NUL
/// -- byte 2 of any UTF-16LE packet. So the guard SAW a UTF-16 packet the
/// reader could not USE, and `detectPdfALevel` answered `None` for every
/// UTF-16 PDF/A file. That is now `load_buffer(data, size, parse_default,
/// encoding_auto)`. The guard's position is unchanged: `inspect()` still runs
/// on the RAW bytes BEFORE the parser, so widening what the parser accepts did
/// not widen what PDFE accepts -- pinned by
/// `XmpEncodingTest.Utf16HostilePacketIsStillRefusedByXmlSafetyNotByTruncation`,
/// which first proves pugixml parses the hostile bytes and only then asserts
/// the refusal, so the refusal cannot be credited to a parse failure.
///
/// Never throws; a malformed/truncated document simply yields whatever
/// verdict the bytes seen so far justify.
Verdict inspect(std::string_view xml, Policy policy = Policy::Strict) noexcept;

/// `inspect(...).detail`-style one-liner for log/exception messages.
const char* describe(Reject r) noexcept;

} // namespace pdfe::core::xmlsafety
