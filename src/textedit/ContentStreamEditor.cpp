#include "ContentStreamEditor.h"
#include "TextEditModel.h"
#include "fonts/FontSubstitutionEngine.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/RetainedObjHolder.h"

#include <QDebug>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace pdfe::textedit {

// One element of a TJ-style encoded sequence: either a string of glyph
// bytes (advance == 0) or a pure horizontal adjustment (bytes empty,
// advance in 1/1000 of font units; PDF spec: subtracted from current x,
// so NEGATIVE advance moves the text matrix forward — this is what
// inserts a space-like gap when the font has no space glyph).
struct EncodedRun {
    std::string bytes;
    int advance = 0;
};

// Codepoints we treat as "spacing whitespace" — when the font cannot
// render them, we emit a TJ horizontal advance instead of a glyph so
// they don't render as missing-glyph boxes (or worse, get silently
// dropped because the .notdef glyph has zero width).
static bool isSpacingWhitespace(char32_t cp)
{
    return cp == U' '       // U+0020 SPACE
        || cp == U'\t'      // U+0009 TAB
        || cp == 0x00A0     // NO-BREAK SPACE
        || cp == 0x2002     // EN SPACE
        || cp == 0x2003     // EM SPACE
        || cp == 0x2009     // THIN SPACE
        || cp == 0x202F;    // NARROW NO-BREAK SPACE
}

// Default TJ advance (in 1/1000 em, NEGATIVE for forward motion) when
// the font is missing a glyph for a whitespace character. 250 is the
// near-universal proportional-font space width; a future improvement
// would query the font's /Widths array for byte 0x20 or its
// FontDescriptor /MissingWidth, but 250 produces a visually correct
// gap for typical body text and is harmless if the font is wider
// (the gap is just slightly tighter than ideal).
static constexpr int kDefaultSpaceAdvance = -250;

// Probe whether a font can actually render a Unicode codepoint. Returns
// true if the font's cmap (or to_unicode reverse map for CID fonts)
// resolves the codepoint to a non-zero glyph.
static bool fontCanEncode(fz_context* ctx, pdf_font_desc* fd, char32_t cp)
{
    if (!fd || !fd->font) return false;
    int gid = fz_encode_character(ctx, fd->font, static_cast<int>(cp));
    return gid != 0;
}

// Encode ONE codepoint for a simple (single-byte) PDF font.
// For WinAnsiEncoding / MacRomanEncoding, codepoints < 256 map directly to
// byte values, which covers ASCII and Latin-1.
//
// A codepoint outside that range cannot be represented at all by a simple
// font. It is still emitted as '?' so the run keeps its character count and
// the page geometry does not shift — but it is RECORDED in `diag`, because
// the silent version of this substitution is the defect: a CJK edit saved a
// row of '?' and reported success. Returns false when the substitution
// happened.
//
// This is the single implementation of that rule. `encodeTextWithFallback`
// used to carry an inline copy of the same three lines, which is exactly the
// duplicate-fix hazard DCA 0036 describes; it now calls this instead, so a
// change here cannot be half-applied.
static bool encodeSimpleCodepoint(char32_t cp, std::string& out,
                                  EncodingDiagnostics* diag)
{
    if (cp < 256) {
        out.push_back(static_cast<char>(static_cast<unsigned char>(cp)));
        return true;
    }
    if (diag) diag->unencodableSimple.push_back(cp);
    out.push_back('?');
    return false;
}

// NOTE on the removed `utf32ToFontEncoded`. The reverification recorded this
// defect as living in two places — a function at :64-76 and an inline copy at
// :204-209 — and asked for both to be fixed. A repo-wide search for the name
// found it defined once and CALLED nowhere: the function was dead, and the
// only live encoder was the inline copy. It is deleted here rather than
// repaired, because keeping a second spelling of the rule is the hazard, not
// the cure. `encodeSimpleCodepoint` above is now the only implementation.

// Helper: convert UTF-32 to CID-encoded bytes (2-byte big-endian) using the font's
// ToUnicode reverse mapping. This handles subset fonts where fz_encode_character()
// returns 0 because the FreeType cmap has been stripped.
//
// Strategy: Build Unicode→CID reverse map from the font's cid_to_ucs array or
// to_unicode CMap. For characters not found, try fz_encode_character() as fallback.
static std::string utf32ToCidEncoded(fz_context* ctx, pdf_font_desc* fontDesc,
                                     const std::u32string& s,
                                     EncodingDiagnostics* diag = nullptr)
{
    // Build reverse mapping: Unicode codepoint → CID value
    std::unordered_map<uint32_t, uint16_t> unicodeToCid;

    // Method 1: Use cid_to_ucs array (simple flat array: CID → Unicode)
    if (fontDesc->cid_to_ucs && fontDesc->cid_to_ucs_len > 0) {
        for (size_t cid = 0; cid < fontDesc->cid_to_ucs_len; cid++) {
            unsigned short ucs = fontDesc->cid_to_ucs[cid];
            if (ucs != 0 && ucs != 0xFFFD) {
                unicodeToCid[static_cast<uint32_t>(ucs)] = static_cast<uint16_t>(cid);
            }
        }
    }

    // Method 2: If cid_to_ucs didn't give us anything, try the to_unicode CMap
    if (unicodeToCid.empty() && fontDesc->to_unicode) {
        // Scan the CMap's ranges/xranges to build the reverse mapping
        pdf_cmap* cmap = fontDesc->to_unicode;
        // Scan regular ranges
        for (int i = 0; i < cmap->rlen; i++) {
            unsigned int lo = cmap->ranges[i].low;
            unsigned int hi = cmap->ranges[i].high;
            unsigned int out = cmap->ranges[i].out;
            for (unsigned int cid = lo; cid <= hi; cid++) {
                unsigned int ucs = out + (cid - lo);
                if (ucs != 0 && ucs != 0xFFFD && ucs <= 0xFFFF) {
                    unicodeToCid[static_cast<uint32_t>(ucs)] = static_cast<uint16_t>(cid);
                }
            }
        }
        // Scan extended ranges
        for (int i = 0; i < cmap->xlen; i++) {
            unsigned int lo = cmap->xranges[i].low;
            unsigned int hi = cmap->xranges[i].high;
            unsigned int out = cmap->xranges[i].out;
            for (unsigned int cid = lo; cid <= hi; cid++) {
                unsigned int ucs = out + (cid - lo);
                if (ucs != 0 && ucs != 0xFFFD && ucs <= 0xFFFF) {
                    unicodeToCid[static_cast<uint32_t>(ucs)] = static_cast<uint16_t>(cid);
                }
            }
        }
        // Scan one-to-many ranges (mranges): each entry maps a single CID to
        // one or more Unicode codepoints stored in cmap->dict[out..].
        // Only reverse-map entries where the output is a single codepoint.
        for (int i = 0; i < cmap->mlen; i++) {
            unsigned int cid = cmap->mranges[i].low;
            int dictIdx = cmap->mranges[i].out;
            if (dictIdx >= 0 && dictIdx < cmap->dlen) {
                int len = cmap->dict[dictIdx];
                if (len == 1 && dictIdx + 1 < cmap->dlen) {
                    unsigned int ucs = static_cast<unsigned int>(cmap->dict[dictIdx + 1]);
                    if (ucs != 0 && ucs != 0xFFFD && ucs <= 0xFFFF) {
                        unicodeToCid[static_cast<uint32_t>(ucs)] = static_cast<uint16_t>(cid);
                    }
                }
            }
        }
    }

    std::string result;
    result.reserve(s.size() * 2);
    for (char32_t cp : s) {
        uint16_t cid = 0;
        auto it = unicodeToCid.find(static_cast<uint32_t>(cp));
        if (it != unicodeToCid.end()) {
            cid = it->second;
        } else if (fontDesc->font) {
            // Fallback: try fz_encode_character (works for non-subset fonts)
            int gid = fz_encode_character(ctx, fontDesc->font, static_cast<int>(cp));
            cid = static_cast<uint16_t>(gid);
        }
        // CID 0 is .notdef. Writing it is what the PDF format requires once we
        // are on this path, but writing it SILENTLY is the defect: the saved
        // stream then contains two raw NUL bytes where the user's character
        // was, and nothing reports that the character did not survive. This is
        // the ordinary outcome of handing CJK text to a Latin CID font, so
        // record it and let the caller decide.
        if (cid == 0 && diag) diag->notdefCid.push_back(cp);
        // Big-endian 2-byte encoding
        result.push_back(static_cast<char>((cid >> 8) & 0xFF));
        result.push_back(static_cast<char>(cid & 0xFF));
    }

    return result;
}

// Run-aware encoder: walks UTF-32 input and produces a sequence of
// (bytes, advance) runs suitable for emission via PDF's TJ operator.
// Whitespace codepoints that the font cannot render become pure-advance
// runs (no glyph bytes, advance = -spaceAdvance) so they show up as a
// horizontal gap instead of a missing-glyph box. All other characters
// flow through the existing single/CID encoding paths and accumulate
// in the current run's byte buffer.
static std::vector<EncodedRun> encodeTextWithFallback(
    fz_context* ctx, pdf_font_desc* fontDesc,
    const std::u32string& text, bool multiByte,
    EncodingDiagnostics* diag = nullptr)
{
    std::vector<EncodedRun> runs;
    EncodedRun current;
    auto flushTextRun = [&]() {
        if (!current.bytes.empty()) {
            runs.push_back(std::move(current));
            current = EncodedRun{};
        }
    };
    for (char32_t cp : text) {
        if (isSpacingWhitespace(cp) && !fontCanEncode(ctx, fontDesc, cp)) {
            // Font has no glyph for this whitespace — emit a TJ
            // horizontal advance instead. Otherwise the renderer would
            // either draw .notdef (a missing-glyph box) or silently
            // drop the character (zero-width .notdef).
            flushTextRun();
            EncodedRun gap;
            gap.advance = kDefaultSpaceAdvance;
            runs.push_back(std::move(gap));
            continue;
        }
        // Encodable character — append its bytes to the current run.
        if (multiByte) {
            // Inline the CID lookup to avoid building the reverse map
            // multiple times per call. Simpler implementation: piggyback
            // on the existing utf32ToCidEncoded for a single-char string.
            std::u32string single(1, cp);
            current.bytes += utf32ToCidEncoded(ctx, fontDesc, single, diag);
        } else {
            encodeSimpleCodepoint(cp, current.bytes, diag);
        }
    }
    flushTextRun();
    return runs;
}

// Check if a pdf_font_desc uses multi-byte (CID) encoding.
// CID fonts (Type0/composite) encode characters as 2+ byte codes.
// Simple fonts (Type1/TrueType) use single-byte encoding.
//
// Detection strategy: A CID font's encoding CMap has ALL codespace ranges with n >= 2,
// and the CMap typically has a name like "Identity-H" or a CJK CMap name.
// Simple fonts either have no CMap codespace or have codespace ranges with n == 1.
static bool isMultiByteFont(const pdf_font_desc* fontDesc)
{
    if (!fontDesc || !fontDesc->encoding) return false;

    const pdf_cmap* cmap = fontDesc->encoding;

    // No codespace means we can't determine — assume simple font
    if (cmap->codespace_len == 0) return false;

    // Check if ALL codespace ranges use multi-byte encoding (n >= 2).
    // CID fonts have exclusively 2-byte (or more) codespace ranges.
    // Simple fonts have at least one 1-byte codespace range.
    bool allMultiByte = true;
    for (int i = 0; i < cmap->codespace_len; i++) {
        if (cmap->codespace[i].n < 2) {
            allMultiByte = false;
            break;
        }
    }

    return allMultiByte;
}

// R3-06: the source graphics-state FILL colourspace + operands active at a
// text-showing operation, captured by the colour-tracking chain processor
// (ColorTracker, below). The MuPDF sanitize text_filter callback exposes only
// trm/ctm/bbox — NOT the fill colourspace — so without this an edited CMYK/spot
// span was re-emitted as a visually-equivalent DeviceRGB `rg`, silently
// flattening the print-critical colourspace identity. TrackedFill lets the
// injection callbacks reproduce the ORIGINAL operator (`k` for DeviceCMYK,
// `g` for DeviceGray, `/name cs … scn` for a /Separation or /DeviceN spot).
enum class FillKind { None, Gray, RGB, CMYK, Separation };

struct TrackedFill {
    FillKind kind = FillKind::None;
    int n = 0;                         // number of colour components
    float comps[FZ_MAX_COLORS] = {0};  // component values (device or tint)
    char csName[64] = {0};             // resource name for Separation/DeviceN
    bool valid = false;                // true once a fill colour is known
};

// State passed through the filter callbacks via opaque pointer.
struct FilterState {
    // R3-06 colour tracking: the running fill colour maintained by the
    // ColorTracker chain processor (upstream of sanitize), read in
    // textFilterCallback at the moment a replacement span is matched so the
    // injection can reproduce the source colourspace operator.
    TrackedFill currentFill;

    // Input: which spans to replace
    struct Replacement {
        QRectF bbox;              // Original span bounding box in fitz space
        std::u32string newText;   // Replacement text
        std::string fontName;     // Font name (PostScript name from extraction)
        float fontSize;           // Original font size
        uint32_t textColor = 0x000000; // RGB fill color (0x000000 = black)
        bool textColorKnown = false;   // true => textColor is the captured source
                                       // color and must be emitted explicitly (so
                                       // the edit can't fall to the ambient fill)
        bool injectionQueued = false;  // Set true when replacement is queued

        /// Per-run segments for multi-formatting-run spans
        /// (UI-TE-SAVE-08). Empty → legacy single-Tf emission runs
        /// unchanged. Non-empty → phase-2 emission walks the segments.
        /// Mirrors SubstitutionInfo::runSegments; reuses the RunSegment
        /// type from fonts/FontSubstitutionEngine.h.
        std::vector<pdfe::fonts::RunSegment> runSegments;
    };
    std::vector<Replacement> replacements;

    // Accumulated per-text-object state
    struct PendingInjection {
        std::u32string text;
        fz_matrix trm;        // Text rendering matrix at the match point
        fz_matrix ctm;        // Current transformation matrix
        QRectF spanBbox;      // User-clicked span bbox in fitz space (for
                              // baseline positioning — see canonical helper)
        std::string fontName;
        float fontSize;
        uint32_t textColor = 0x000000; // RGB fill color
        bool textColorKnown = false;   // emit textColor explicitly when true

        // R3-06: the source fill colourspace/operands captured from the
        // ColorTracker at the matched text-show. When valid and non-RGB, the
        // injection emits the original operator (k/g/scn) instead of flattening
        // to rg. RGB/None fall through to the legacy textColor `rg` path.
        TrackedFill capturedFill;

        /// Per-run segments propagated from the source Replacement.
        /// Empty → emission walks the legacy single-Tf path.
        /// Non-empty → emission walks the segment list and emits a
        /// Tf/Tj pair per segment (UI-TE-SAVE-08).
        std::vector<pdfe::fonts::RunSegment> runSegments;
    };
    std::vector<PendingInjection> pendingInjections;

    // Font substitution info for font-switched spans
    struct FontSwitch {
        std::string substituteResName;  // e.g., "Fsub1"
        float horizontalScale = 1.0f;
    };
    // Map from original font name (e.g., "F1") → font switch info
    std::unordered_map<std::string, FontSwitch> fontSwitches;

    // Whether font substitution is active for this filter run
    bool hasSubstitutions = false;

    // MuPDF handles needed for font loading in callbacks
    pdf_document* pdfDoc = nullptr;
    pdf_obj* pageResources = nullptr;
    pdf_obj* xobjResources = nullptr;  // Form XObject resources (fallback for font lookup)

    // Page height in points (for converting PDF user space Y-up to fitz Y-down)
    float pageHeight = 0.0f;
    // Page width in points (used to extend the right-edge of the span bbox
    // when capturing overflow glyphs from prior longer edits — see comment
    // at the bbox match in textFilterCallback).
    float pageWidth = 0.0f;

    // bugfix-3e91a7c4 #4/#5: the page's user(Y-up) -> device(Y-down, top-left,
    // CropBox-normalized) transform, exactly the one fz_new_stext_page_from_page
    // applies. The span bboxes we match against (repl.bbox) come from stext and
    // are in THIS space, so the glyph box must be mapped into it too. Using a
    // plain `pageHeight - y` flip (below) silently discards the page's origin
    // (CropBox/MediaBox y0), so on a page with a nonzero origin the glyph and
    // span boxes diverge by a constant Δ = origin.y0 — which small (short) spans
    // cannot absorb, so no glyph matches and the edit no-ops.
    fz_matrix pageCtm = fz_identity;

    // Map from PostScript font name → PDF resource key (e.g., "TimesNewRomanPSMT" → "F1")
    std::unordered_map<std::string, std::string> psNameToResKey;

    // Tolerance for bounding box matching (in PDF points)
    static constexpr float BBOX_TOLERANCE = 2.0f;

    // Text rendering mode to emit for injected text (-1 = don't emit Tr, 3 = invisible)
    int textRenderMode = -1;

    // phase-75: codepoints the encoder could not represent in the font it was
    // handed. Filled by the injection callbacks, copied back onto the editor
    // after the filter run so callers (and tests) can see that an edit lost
    // characters instead of only being told it succeeded.
    EncodingDiagnostics diag;
};

// Check if two bounding boxes overlap (with tolerance for matching)
static bool bboxOverlaps(fz_rect a, const QRectF& b, float tolerance)
{
    return !(a.x1 < b.left() - tolerance ||
             a.x0 > b.right() + tolerance ||
             a.y1 < b.top() - tolerance ||
             a.y0 > b.bottom() + tolerance);
}

// B1 fix (rotate × text-edit clean-inverse). Populate state.pageHeight/pageWidth
// /pageCtm and, when the page is rotated, re-express the span match boxes into
// UNROTATED page space.
//
// The page content stream is authored in UNROTATED user space; a page /Rotate
// only rotates the rendered output — it is NOT a content-stream `cm`. The
// glyph<->span bbox match in textFilterCallback (and the injection positioning
// in repositionFromSpanBbox) rely on line-oriented heuristics — a vertical-
// centre Y-band plus a right-edge extension to the page's right margin — that
// are sound ONLY while text lines run horizontally. Under a 90/270° /Rotate the
// text extractor reports span boxes in ROTATED device space, where lines run
// vertically; span2's tall Y-band + right-extension then overlap glyphs from
// unrelated lines, the sanitize filter removes them, and the injection re-adds
// only span2 — permanently losing the rest of the page and making undo a
// non-inverse (the B1 data-loss defect).
//
// pdf_page_transform() returns T, the user->device transform WITH the rotation
// folded in. The unrotated device transform is U = [1,0,0,-1,-mb.x0, mb.y1]
// (y-flip + origin translate, no rotation), which maps the page box to
// [0,0,w,h] just like fz_bound_page on an unrotated page. Mapping each span box
// rotated-device -> unrotated-device via C = T⁻¹·U (and matching glyphs through
// U instead of T) restores horizontal lines, so the heuristics hold and the
// edit + its undo become a clean inverse. For an unrotated page T == U within
// tolerance and this is a no-op: the legacy path runs byte-identically.
static void setupSpanMatchSpace(fz_context* ctx, pdf_page* pdfPage,
                                fz_page* fzPage, FilterState& state)
{
    fz_rect mb;
    fz_matrix T;
    pdf_page_transform(ctx, pdfPage, &mb, &T);

    const fz_matrix U =
        fz_make_matrix(1.0f, 0.0f, 0.0f, -1.0f, -mb.x0, mb.y1);
    auto close = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };
    const bool unrotated =
        close(T.a, U.a) && close(T.b, U.b) && close(T.c, U.c) &&
        close(T.d, U.d) && close(T.e, U.e) && close(T.f, U.f);

    if (unrotated) {
        // Legacy (unrotated) path — byte-identical to the pre-B1-fix code.
        fz_rect pb = fz_bound_page(ctx, fzPage);
        state.pageHeight = pb.y1 - pb.y0;
        state.pageWidth = pb.x1 - pb.x0;
        state.pageCtm = T;
        return;
    }

    // Rotated page: re-express every span match box into unrotated device
    // space and match glyphs through the unrotated transform U.
    const fz_matrix deRot = fz_concat(fz_invert_matrix(T), U);
    for (auto& repl : state.replacements) {
        fz_rect r = fz_make_rect(
            static_cast<float>(repl.bbox.left()),
            static_cast<float>(repl.bbox.top()),
            static_cast<float>(repl.bbox.right()),
            static_cast<float>(repl.bbox.bottom()));
        r = fz_transform_rect(r, deRot);
        repl.bbox = QRectF(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
    }
    state.pageHeight = mb.y1 - mb.y0;
    state.pageWidth = mb.x1 - mb.x0;
    state.pageCtm = U;
}

// Strip PDF subset prefix (e.g., "ABCDEF+TimesNewRomanPSMT" → "TimesNewRomanPSMT").
// Subset prefixes are exactly 6 uppercase ASCII letters followed by '+'.
static std::string stripSubsetPrefix(const char* name)
{
    if (!name || !name[0]) return "";
    std::string s(name);
    if (s.size() > 7 && s[6] == '+') {
        bool allUpper = true;
        for (int i = 0; i < 6; i++) {
            if (s[i] < 'A' || s[i] > 'Z') { allUpper = false; break; }
        }
        if (allUpper) return s.substr(7);
    }
    return s;
}

// Add font entries from a resource dictionary into state.psNameToResKey
static void addFontMappingsFromDict(fz_context* ctx, FilterState& state, pdf_obj* resources)
{
    if (!resources) return;
    pdf_obj* fontDict = pdf_dict_get(ctx, resources, PDF_NAME(Font));
    if (!fontDict) return;

    int n = pdf_dict_len(ctx, fontDict);
    for (int i = 0; i < n; i++) {
        pdf_obj* key = pdf_dict_get_key(ctx, fontDict, i);
        pdf_obj* val = pdf_dict_get_val(ctx, fontDict, i);
        if (!key || !val) continue;

        const char* resKey = pdf_to_name(ctx, key);
        pdf_obj* baseFont = pdf_dict_get(ctx, val, PDF_NAME(BaseFont));
        if (baseFont) {
            const char* psName = pdf_to_name(ctx, baseFont);
            if (psName && psName[0] && resKey && resKey[0]) {
                // Map full name (e.g., "ABCDEF+ArialMT" → "F1")
                state.psNameToResKey[psName] = resKey;
                // Also map stripped name (e.g., "ArialMT" → "F1") for spans
                // that store PostScript names without subset prefix
                std::string stripped = stripSubsetPrefix(psName);
                if (stripped != psName) {
                    state.psNameToResKey[stripped] = resKey;
                }
            }
        }
    }
}

// Build mapping from PostScript font names to PDF resource keys.
// Iterates the page's /Font resource dictionary AND any Form XObject resource
// dictionaries so that fonts inside Form XObjects are also resolved.
static void buildFontNameMapping(fz_context* ctx, FilterState& state)
{
    if (!state.pageResources) return;

    // Map page-level font resources
    addFontMappingsFromDict(ctx, state, state.pageResources);

    // Also map fonts from Form XObject resources (needed when recurse=1)
    pdf_obj* xobjDict = pdf_dict_get(ctx, state.pageResources, PDF_NAME(XObject));
    if (xobjDict) {
        int n = pdf_dict_len(ctx, xobjDict);
        for (int i = 0; i < n; i++) {
            pdf_obj* val = pdf_dict_get_val(ctx, xobjDict, i);
            if (!val) continue;
            val = pdf_resolve_indirect(ctx, val);
            if (!val) continue;
            // Check if it's a Form XObject
            pdf_obj* subtype = pdf_dict_get(ctx, val, PDF_NAME(Subtype));
            if (subtype && pdf_name_eq(ctx, subtype, PDF_NAME(Form))) {
                pdf_obj* xobjRes = pdf_dict_get(ctx, val, PDF_NAME(Resources));
                if (xobjRes) {
                    addFontMappingsFromDict(ctx, state, xobjRes);
                    // Store first XObject resource dict for font loading fallback
                    if (!state.xobjResources) {
                        state.xobjResources = xobjRes;
                    }
                }
            }
        }
    }
}

// Result of computeCanonicalTextMatrix(): a Tfs and Tm value pair that
// renders replacement text right-side-up at the same position as the
// matched original character, in the canonical convention used by the
// surrounding body-text BTs (Tm = [1, 0, 0, -1, e, f]).
//
// Why this exists (see memory/textedit-tm-sign-bug.md):
//   The previous implementation recovered Tm via fz_concat(trm, ctm⁻¹) /
//   inj.fontSize. That reproduces the recorded trm exactly when the
//   resulting Tm is reapplied with the same CTM in the same matrix
//   convention — but a downstream PDF renderer (Adobe, PyMuPDF, etc.)
//   evaluates Tm in PDF user-space, where the recovery's "fontSize ==
//   visible point size + Tm.d carrying the inverted scale" produces a
//   Tm.d sign opposite of every surrounding block. Result: rendered
//   replacement text is upside-down and 2× oversized.
//
// The fix: ignore the trm-derived Tm.a/d/etc and emit Tm in the
// canonical [1, 0, 0, -1, e, f] form. Compute the pre-CTM font size
// from the visible point size (inj.fontSize) divided by the CTM's
// horizontal scale, so the rendered glyph height matches the visible
// point size after the surrounding CTM's scale is applied.
//
// Limitation: assumes a diagonal CTM (no rotation or shear). Lean
// Startup p2 — and every other body-text PDF this path has been
// exercised on — meets that assumption. If we later need to support
// rotated text, this helper is the one place to extend.
struct CanonicalTextMatrix {
    float Tfs;       // Pre-CTM font size to emit via op_Tf
    float Tm_a;      // +sign(ctm.a)
    float Tm_b;      // Always 0
    float Tm_c;      // Always 0
    float Tm_d;      // +sign(ctm.d) — see note below
    float Tm_e;      // Pre-CTM x position
    float Tm_f;      // Pre-CTM y position
};

static CanonicalTextMatrix computeCanonicalTextMatrix(
    fz_matrix trm, fz_matrix ctm, float visibleFontSize)
{
    CanonicalTextMatrix r;

    // Tm.d sign: must match sign(ctm.d) so that the effective rendering
    // transform `Tm.d × Tfs × CTM.d` is positive (right-side-up in PDF
    // user-space). Two important cases the codebase has to support:
    //   1. Y-flipping scope: ctm.d < 0 (Lean Startup p2 image-replacer
    //      content has ctm.d = -0.48). Surrounding /F7 BT blocks here
    //      use Tm.d = -1, giving effective trm.d = (-1) × 26 × (-0.48)
    //      = +12.48 (right-side-up).
    //   2. Normal scope: ctm.d > 0 (the default for typeset PDFs without
    //      a Y-flipping cm). Surrounding BT blocks use Tm.d = +1.
    // Hardcoding Tm.d = -1 was the original fix's bug — it broke the
    // round-trip for normal-CTM PDFs (test_single.pdf, etc.), shifting
    // rendered text by ~1 em-height on the Y axis.
    r.Tm_a = (ctm.a >= 0.0f) ? 1.0f : -1.0f;
    r.Tm_b = 0.0f;
    r.Tm_c = 0.0f;
    r.Tm_d = (ctm.d >= 0.0f) ? 1.0f : -1.0f;

    float ctmScaleA = std::abs(ctm.a);
    if (ctmScaleA < 1e-6f) ctmScaleA = 1.0f;
    r.Tfs = visibleFontSize / ctmScaleA;
    if (r.Tfs < 0.001f) r.Tfs = 12.0f;

    // Recover pre-CTM position by inverting the (assumed diagonal) CTM:
    //   trm.e = Tm.e * ctm.a + ctm.e   (no contribution from Tm.f for
    //                                   diagonal CTM since ctm.c = 0)
    //   trm.f = Tm.f * ctm.d + ctm.f   (similarly ctm.b = 0)
    float denomA = (std::abs(ctm.a) > 1e-6f) ? ctm.a : 1.0f;
    float denomD = (std::abs(ctm.d) > 1e-6f) ? ctm.d : 1.0f;
    r.Tm_e = (trm.e - ctm.e) / denomA;
    r.Tm_f = (trm.f - ctm.f) / denomD;
    return r;
}

// Override the canonical Tm's translation (Tm.e, Tm.f) so the new text
// renders at the user's clicked span bbox rather than wherever the
// bbox-overlap matcher's first matching character happened to be.
//
// Why this exists:
//   textFilterCallback iterates the page's content stream and matches
//   characters whose bbox overlaps the user-clicked span (with 2pt
//   tolerance). Adjacent text lines can both fall within the tolerance
//   window, and document-order iteration may pick a character on a
//   different line than the user clicked. Using inj.trm.e/inj.trm.f
//   (the matched character's position) then renders the new text at
//   that line's baseline — ~1 line off from the user's intent.
//
//   Span bbox is in MuPDF fitz space (Y-down, top-left origin). For
//   diagonal CTM with no rotation:
//     user_x_left  = bbox.x
//     user_y_bot   = pageH - (bbox.y + bbox.height)   (Y-up convention)
//     baseline_y_user ≈ user_y_bot + 0.2 × bbox.height
//                       (descender ≈ 20% of em-height for most Latin fonts)
//
//   Then solve:
//     Tm.e = (user_x_left   - ctm.e) / ctm.a
//     Tm.f = (baseline_y_user - ctm.f) / ctm.d
//
// Falls through silently if pageHeight or bbox aren't available — in
// that case the trm-derived Tm.e/Tm.f from canonicalTextMatrixForReplacement
// is used as-is (close enough when the matcher's match is on the right line).
static void repositionFromSpanBbox(CanonicalTextMatrix& tm,
                                   const QRectF& spanBbox,
                                   const fz_matrix& ctm,
                                   float pageHeight)
{
    if (pageHeight <= 0.0f || spanBbox.height() <= 0.0f) {
        return;
    }
    if (std::abs(ctm.a) < 1e-6f || std::abs(ctm.d) < 1e-6f) {
        return;
    }
    const float userXLeft = static_cast<float>(spanBbox.x());
    const float userYBottom = pageHeight -
        static_cast<float>(spanBbox.y() + spanBbox.height());
    const float baselineUserY =
        userYBottom + 0.2f * static_cast<float>(spanBbox.height());
    tm.Tm_e = (userXLeft - ctm.e) / ctm.a;
    tm.Tm_f = (baselineUserY - ctm.f) / ctm.d;
}

// R3-06: emit the captured source fill colour operator into the downstream
// (buffer/output) processor `chain`, reproducing the ORIGINAL colourspace
// identity of an edited span instead of the legacy unconditional `rg` flatten.
// Returns true if a colour operator was emitted (so the caller skips the legacy
// textColor `rg` path). Returns false for RGB/None so DeviceRGB edits keep
// emitting `rg` exactly as before.
static bool emitCapturedFill(fz_context* ctx, pdf_processor* chain,
                            const TrackedFill& f)
{
    if (!f.valid) return false;
    switch (f.kind) {
    case FillKind::CMYK: {
        if (!chain->op_k) return false;
        // 4-component DeviceCMYK (`c m y k k`). Covers pure black K=1
        // (0 0 0 1 k), rich black (c m y k), and any process colour.
        chain->op_k(ctx, chain, f.comps[0], f.comps[1], f.comps[2], f.comps[3]);
        return true;
    }
    case FillKind::Gray: {
        if (!chain->op_g) return false;
        chain->op_g(ctx, chain, f.comps[0]);
        return true;
    }
    case FillKind::Separation: {
        // Spot colour: re-select the named /Separation (or /DeviceN)
        // colourspace, then its tint via scn. The colourspace resource
        // (e.g. /CS1) is preserved on the page across the content rewrite,
        // so emitting it by name round-trips the spot identity. The sink
        // processor's op_cs ignores the fz_colorspace* (writes the name
        // only), so passing nullptr is safe here.
        if (!chain->op_cs || !chain->op_sc_color || !f.csName[0] || f.n <= 0)
            return false;
        chain->op_cs(ctx, chain, f.csName, nullptr);
        float comps[FZ_MAX_COLORS];
        for (int i = 0; i < f.n && i < FZ_MAX_COLORS; ++i) comps[i] = f.comps[i];
        chain->op_sc_color(ctx, chain, f.n, comps);
        return true;
    }
    case FillKind::RGB:
    case FillKind::None:
    default:
        return false;  // DeviceRGB / unknown → legacy textColor `rg` path
    }
}

// ─────────────────────────────────────────────────────────────────────────
// R3-06 ColorTracker: a pass-through chain processor placed UPSTREAM of the
// sanitize filter. It forwards every operator verbatim to the sanitize
// processor (so the sanitized output is byte-identical to the un-tracked
// run) while observing the FILL colour operators (g/rg/k, and cs+scn) to
// maintain FilterState::currentFill with q/Q save-restore. Because it runs
// before sanitize, currentFill is already up to date when sanitize invokes
// textFilterCallback for the text that follows a colour operator — closing
// the gap that the sanitize text_filter callback (trm/ctm/bbox only) cannot.
//
// EVERY op is forwarded: a dropped operator would corrupt the output stream.
// All processor arguments are borrowed (the interpreter owns/frees strings,
// arrays, fonts, colorspaces around the call), so forwarding just passes the
// pointers through — no ownership transfer, no frees here.
// ─────────────────────────────────────────────────────────────────────────
struct ColorTracker {
    pdf_processor super;      // MUST be first member (pdf_new_processor cast)
    pdf_processor* chain;     // downstream processor (the sanitize filter)
    FilterState* st;          // shared filter state (currentFill sink)
    TrackedFill stack[64];    // fill-colour q/Q save stack
    int depth;                // current top index; current = stack[depth]
};

static inline ColorTracker* ctSelf(pdf_processor* p) {
    return reinterpret_cast<ColorTracker*>(p);
}
static inline void ctSync(ColorTracker* t) {
    if (t->st) t->st->currentFill = t->stack[t->depth];
}
static FillKind ctKindOf(fz_context* ctx, fz_colorspace* cs) {
    if (!cs) return FillKind::None;
    if (fz_colorspace_is_device_n(ctx, cs)) return FillKind::Separation;
    switch (fz_colorspace_type(ctx, cs)) {
    case FZ_COLORSPACE_GRAY:       return FillKind::Gray;
    case FZ_COLORSPACE_RGB:
    case FZ_COLORSPACE_BGR:        return FillKind::RGB;
    case FZ_COLORSPACE_CMYK:       return FillKind::CMYK;
    case FZ_COLORSPACE_SEPARATION: return FillKind::Separation;
    default:                       return FillKind::None;
    }
}

// Forwarding macros. CT_C(p) = the downstream chain processor.
#define CT_C(p) (ctSelf(p)->chain)
#define CT_FWD0(NAME) \
    static void ct_##NAME(fz_context* ctx, pdf_processor* p) { \
        pdf_processor* c = CT_C(p); if (c->NAME) c->NAME(ctx, c); }
#define CT_FWD_f(NAME) \
    static void ct_##NAME(fz_context* ctx, pdf_processor* p, float a) { \
        pdf_processor* c = CT_C(p); if (c->NAME) c->NAME(ctx, c, a); }
#define CT_FWD_i(NAME) \
    static void ct_##NAME(fz_context* ctx, pdf_processor* p, int a) { \
        pdf_processor* c = CT_C(p); if (c->NAME) c->NAME(ctx, c, a); }
#define CT_FWD_2f(NAME) \
    static void ct_##NAME(fz_context* ctx, pdf_processor* p, float a, float b) { \
        pdf_processor* c = CT_C(p); if (c->NAME) c->NAME(ctx, c, a, b); }

// General graphics state
CT_FWD_f(op_w) CT_FWD_i(op_j) CT_FWD_i(op_J) CT_FWD_f(op_M) CT_FWD_f(op_i)
static void ct_op_d(fz_context* ctx, pdf_processor* p, pdf_obj* a, float ph) {
    pdf_processor* c = CT_C(p); if (c->op_d) c->op_d(ctx, c, a, ph); }
static void ct_op_ri(fz_context* ctx, pdf_processor* p, const char* n) {
    pdf_processor* c = CT_C(p); if (c->op_ri) c->op_ri(ctx, c, n); }

// ExtGState
static void ct_op_gs_begin(fz_context* ctx, pdf_processor* p, const char* n, pdf_obj* e) {
    pdf_processor* c = CT_C(p); if (c->op_gs_begin) c->op_gs_begin(ctx, c, n, e); }
static void ct_op_gs_BM(fz_context* ctx, pdf_processor* p, const char* b) {
    pdf_processor* c = CT_C(p); if (c->op_gs_BM) c->op_gs_BM(ctx, c, b); }
CT_FWD_f(op_gs_ca) CT_FWD_f(op_gs_CA)
static void ct_op_gs_SMask(fz_context* ctx, pdf_processor* p, pdf_obj* sm,
                          fz_colorspace* cs, float* bc, int lum, pdf_obj* tr) {
    pdf_processor* c = CT_C(p);
    if (c->op_gs_SMask) c->op_gs_SMask(ctx, c, sm, cs, bc, lum, tr); }
CT_FWD0(op_gs_end)

// Special graphics state — q/Q also save/restore the tracked fill.
static void ct_op_q(fz_context* ctx, pdf_processor* p) {
    ColorTracker* t = ctSelf(p);
    if (t->depth < 63) { t->stack[t->depth + 1] = t->stack[t->depth]; t->depth++; }
    pdf_processor* c = t->chain; if (c->op_q) c->op_q(ctx, c);
}
static void ct_op_Q(fz_context* ctx, pdf_processor* p) {
    ColorTracker* t = ctSelf(p);
    if (t->depth > 0) t->depth--;
    ctSync(t);
    pdf_processor* c = t->chain; if (c->op_Q) c->op_Q(ctx, c);
}
static void ct_op_cm(fz_context* ctx, pdf_processor* p,
                    float a, float b, float cc, float d, float e, float f) {
    pdf_processor* c = CT_C(p); if (c->op_cm) c->op_cm(ctx, c, a, b, cc, d, e, f); }

// Path construction
CT_FWD_2f(op_m) CT_FWD_2f(op_l)
static void ct_op_c(fz_context* ctx, pdf_processor* p,
                   float a, float b, float cc, float d, float e, float f) {
    pdf_processor* ch = CT_C(p); if (ch->op_c) ch->op_c(ctx, ch, a, b, cc, d, e, f); }
static void ct_op_v(fz_context* ctx, pdf_processor* p, float a, float b, float cc, float d) {
    pdf_processor* ch = CT_C(p); if (ch->op_v) ch->op_v(ctx, ch, a, b, cc, d); }
static void ct_op_y(fz_context* ctx, pdf_processor* p, float a, float b, float cc, float d) {
    pdf_processor* ch = CT_C(p); if (ch->op_y) ch->op_y(ctx, ch, a, b, cc, d); }
CT_FWD0(op_h)
static void ct_op_re(fz_context* ctx, pdf_processor* p, float a, float b, float cc, float d) {
    pdf_processor* ch = CT_C(p); if (ch->op_re) ch->op_re(ctx, ch, a, b, cc, d); }

// Path painting / clipping
CT_FWD0(op_S) CT_FWD0(op_s) CT_FWD0(op_F) CT_FWD0(op_f) CT_FWD0(op_fstar)
CT_FWD0(op_B) CT_FWD0(op_Bstar) CT_FWD0(op_b) CT_FWD0(op_bstar) CT_FWD0(op_n)
CT_FWD0(op_W) CT_FWD0(op_Wstar)

// Text objects / state / positioning
CT_FWD0(op_BT) CT_FWD0(op_ET)
CT_FWD_f(op_Tc) CT_FWD_f(op_Tw) CT_FWD_f(op_Tz) CT_FWD_f(op_TL)
CT_FWD_i(op_Tr) CT_FWD_f(op_Ts)
static void ct_op_Tf(fz_context* ctx, pdf_processor* p, const char* n,
                    pdf_font_desc* font, float sz) {
    pdf_processor* c = CT_C(p); if (c->op_Tf) c->op_Tf(ctx, c, n, font, sz); }
CT_FWD_2f(op_Td) CT_FWD_2f(op_TD)
static void ct_op_Tm(fz_context* ctx, pdf_processor* p,
                    float a, float b, float cc, float d, float e, float f) {
    pdf_processor* c = CT_C(p); if (c->op_Tm) c->op_Tm(ctx, c, a, b, cc, d, e, f); }
CT_FWD0(op_Tstar)

// Text showing
static void ct_op_TJ(fz_context* ctx, pdf_processor* p, pdf_obj* arr) {
    pdf_processor* c = CT_C(p); if (c->op_TJ) c->op_TJ(ctx, c, arr); }
static void ct_op_Tj(fz_context* ctx, pdf_processor* p, char* s, size_t l) {
    pdf_processor* c = CT_C(p); if (c->op_Tj) c->op_Tj(ctx, c, s, l); }
static void ct_op_squote(fz_context* ctx, pdf_processor* p, char* s, size_t l) {
    pdf_processor* c = CT_C(p); if (c->op_squote) c->op_squote(ctx, c, s, l); }
static void ct_op_dquote(fz_context* ctx, pdf_processor* p, float aw, float ac,
                        char* s, size_t l) {
    pdf_processor* c = CT_C(p);
    if (c->op_dquote) c->op_dquote(ctx, c, aw, ac, s, l); }

// Type 3 fonts
CT_FWD_2f(op_d0)
static void ct_op_d1(fz_context* ctx, pdf_processor* p, float wx, float wy,
                    float llx, float lly, float urx, float ury) {
    pdf_processor* c = CT_C(p);
    if (c->op_d1) c->op_d1(ctx, c, wx, wy, llx, lly, urx, ury); }

// ── Colour: forward all, TRACK fill (lowercase) operators ──
// Stroke colourspace (uppercase) — forward only.
static void ct_op_CS(fz_context* ctx, pdf_processor* p, const char* n, fz_colorspace* cs) {
    pdf_processor* c = CT_C(p); if (c->op_CS) c->op_CS(ctx, c, n, cs); }
// Fill colourspace — forward AND record kind/name (value arrives via scn).
static void ct_op_cs(fz_context* ctx, pdf_processor* p, const char* n, fz_colorspace* cs) {
    ColorTracker* t = ctSelf(p);
    TrackedFill& cur = t->stack[t->depth];
    cur.kind = ctKindOf(ctx, cs);
    cur.n = cs ? fz_colorspace_n(ctx, cs) : 0;
    cur.csName[0] = '\0';
    if (cur.kind == FillKind::Separation && n) {
        std::strncpy(cur.csName, n, sizeof(cur.csName) - 1);
        cur.csName[sizeof(cur.csName) - 1] = '\0';
    }
    // The colour VALUE is not known yet — it arrives via the following
    // sc/scn (op_sc_color). Mark invalid until then so a bare `cs` with no
    // subsequent value never drives the injection.
    cur.valid = false;
    ctSync(t);
    pdf_processor* c = t->chain; if (c->op_cs) c->op_cs(ctx, c, n, cs);
}
static void ct_op_SC_pattern(fz_context* ctx, pdf_processor* p, const char* n,
                            pdf_pattern* pat, int nc, float* col) {
    pdf_processor* c = CT_C(p);
    if (c->op_SC_pattern) c->op_SC_pattern(ctx, c, n, pat, nc, col); }
static void ct_op_sc_pattern(fz_context* ctx, pdf_processor* p, const char* n,
                            pdf_pattern* pat, int nc, float* col) {
    // Pattern fill — not a plain colourspace identity; leave fill invalid so
    // the injection falls back to the legacy path. Forward verbatim.
    pdf_processor* c = CT_C(p);
    if (c->op_sc_pattern) c->op_sc_pattern(ctx, c, n, pat, nc, col); }
static void ct_op_SC_shade(fz_context* ctx, pdf_processor* p, const char* n, fz_shade* sh) {
    pdf_processor* c = CT_C(p); if (c->op_SC_shade) c->op_SC_shade(ctx, c, n, sh); }
static void ct_op_sc_shade(fz_context* ctx, pdf_processor* p, const char* n, fz_shade* sh) {
    pdf_processor* c = CT_C(p); if (c->op_sc_shade) c->op_sc_shade(ctx, c, n, sh); }
static void ct_op_SC_color(fz_context* ctx, pdf_processor* p, int nc, float* col) {
    pdf_processor* c = CT_C(p); if (c->op_SC_color) c->op_SC_color(ctx, c, nc, col); }
// Fill colour value under the current fill colourspace (scn / sc numeric form).
static void ct_op_sc_color(fz_context* ctx, pdf_processor* p, int nc, float* col) {
    ColorTracker* t = ctSelf(p);
    TrackedFill& cur = t->stack[t->depth];
    cur.n = nc;
    for (int i = 0; i < nc && i < FZ_MAX_COLORS; ++i) cur.comps[i] = col[i];
    if (cur.kind == FillKind::None) {
        // scn without a preceding cs we could classify — infer from arity.
        cur.kind = (nc == 1) ? FillKind::Gray
                 : (nc == 3) ? FillKind::RGB
                 : (nc == 4) ? FillKind::CMYK : FillKind::None;
    }
    cur.valid = (cur.kind != FillKind::None);
    ctSync(t);
    pdf_processor* c = t->chain; if (c->op_sc_color) c->op_sc_color(ctx, c, nc, col);
}
// DeviceGray / DeviceRGB / DeviceCMYK direct fill operators — forward AND set.
static void ct_op_G(fz_context* ctx, pdf_processor* p, float g) {
    pdf_processor* c = CT_C(p); if (c->op_G) c->op_G(ctx, c, g); }
static void ct_op_g(fz_context* ctx, pdf_processor* p, float g) {
    ColorTracker* t = ctSelf(p);
    TrackedFill& cur = t->stack[t->depth];
    cur.kind = FillKind::Gray; cur.n = 1; cur.comps[0] = g;
    cur.csName[0] = '\0'; cur.valid = true;
    ctSync(t);
    pdf_processor* c = t->chain; if (c->op_g) c->op_g(ctx, c, g);
}
static void ct_op_RG(fz_context* ctx, pdf_processor* p, float r, float g, float b) {
    pdf_processor* c = CT_C(p); if (c->op_RG) c->op_RG(ctx, c, r, g, b); }
static void ct_op_rg(fz_context* ctx, pdf_processor* p, float r, float g, float b) {
    ColorTracker* t = ctSelf(p);
    TrackedFill& cur = t->stack[t->depth];
    cur.kind = FillKind::RGB; cur.n = 3;
    cur.comps[0] = r; cur.comps[1] = g; cur.comps[2] = b;
    cur.csName[0] = '\0'; cur.valid = true;
    ctSync(t);
    pdf_processor* c = t->chain; if (c->op_rg) c->op_rg(ctx, c, r, g, b);
}
static void ct_op_K(fz_context* ctx, pdf_processor* p, float c0, float m, float y, float k) {
    pdf_processor* c = CT_C(p); if (c->op_K) c->op_K(ctx, c, c0, m, y, k); }
static void ct_op_k(fz_context* ctx, pdf_processor* p, float cc, float m, float y, float k) {
    ColorTracker* t = ctSelf(p);
    TrackedFill& cur = t->stack[t->depth];
    cur.kind = FillKind::CMYK; cur.n = 4;
    cur.comps[0] = cc; cur.comps[1] = m; cur.comps[2] = y; cur.comps[3] = k;
    cur.csName[0] = '\0'; cur.valid = true;
    ctSync(t);
    pdf_processor* c = t->chain; if (c->op_k) c->op_k(ctx, c, cc, m, y, k);
}

// Shadings, images, XObjects
static void ct_op_BI(fz_context* ctx, pdf_processor* p, fz_image* img, const char* cs) {
    pdf_processor* c = CT_C(p); if (c->op_BI) c->op_BI(ctx, c, img, cs); }
static void ct_op_sh(fz_context* ctx, pdf_processor* p, const char* n, fz_shade* sh) {
    pdf_processor* c = CT_C(p); if (c->op_sh) c->op_sh(ctx, c, n, sh); }
static void ct_op_Do_image(fz_context* ctx, pdf_processor* p, const char* n, fz_image* img) {
    pdf_processor* c = CT_C(p); if (c->op_Do_image) c->op_Do_image(ctx, c, n, img); }
static void ct_op_Do_form(fz_context* ctx, pdf_processor* p, const char* n, pdf_obj* form) {
    pdf_processor* c = CT_C(p); if (c->op_Do_form) c->op_Do_form(ctx, c, n, form); }

// Marked content
static void ct_op_MP(fz_context* ctx, pdf_processor* p, const char* t) {
    pdf_processor* c = CT_C(p); if (c->op_MP) c->op_MP(ctx, c, t); }
static void ct_op_DP(fz_context* ctx, pdf_processor* p, const char* t, pdf_obj* r, pdf_obj* ck) {
    pdf_processor* c = CT_C(p); if (c->op_DP) c->op_DP(ctx, c, t, r, ck); }
static void ct_op_BMC(fz_context* ctx, pdf_processor* p, const char* t) {
    pdf_processor* c = CT_C(p); if (c->op_BMC) c->op_BMC(ctx, c, t); }
static void ct_op_BDC(fz_context* ctx, pdf_processor* p, const char* t, pdf_obj* r, pdf_obj* ck) {
    pdf_processor* c = CT_C(p); if (c->op_BDC) c->op_BDC(ctx, c, t, r, ck); }
CT_FWD0(op_EMC)

// Compatibility
CT_FWD0(op_BX) CT_FWD0(op_EX)

// ExtGState virtual ops
CT_FWD_i(op_gs_OP) CT_FWD_i(op_gs_op) CT_FWD_i(op_gs_OPM)
static void ct_op_gs_UseBlackPtComp(fz_context* ctx, pdf_processor* p, pdf_obj* n) {
    pdf_processor* c = CT_C(p);
    if (c->op_gs_UseBlackPtComp) c->op_gs_UseBlackPtComp(ctx, c, n); }

// End-of-data / end-of-stream
CT_FWD0(op_EOD) CT_FWD0(op_END)

// Resource stack — MUST forward (interpreter calls without a NULL guard).
static void ct_push_resources(fz_context* ctx, pdf_processor* p, pdf_obj* res) {
    pdf_processor* c = CT_C(p);
    if (c->push_resources) c->push_resources(ctx, c, res); }
static pdf_obj* ct_pop_resources(fz_context* ctx, pdf_processor* p) {
    pdf_processor* c = CT_C(p);
    return c->pop_resources ? c->pop_resources(ctx, c) : nullptr; }

// Lifecycle — CLOSE chains to the downstream processor (matching the sanitize
// filter's own close, which calls pdf_close_processor(chain)). We deliberately
// provide NO drop_processor: the filter framework (pdf-clean.c) drops EVERY
// processor in its list exactly once — the chain is a borrowed, non-owning
// reference (the sanitize filter likewise neither keeps nor drops its chain).
// Dropping it here would double-free. ColorTracker owns no heap resources, so
// pdf_drop_processor's fz_free is all the teardown it needs.
static void ct_close_processor(fz_context* ctx, pdf_processor* p) {
    pdf_close_processor(ctx, CT_C(p)); }

// Factory: build the tracker as filter factory[0] (upstream of sanitize).
// factory_options carries the FilterState*. `chain` is the sanitize processor.
static pdf_processor* newColorTrackerProcessor(
    fz_context* ctx, pdf_document* /*doc*/, pdf_processor* chain,
    int /*struct_parents*/, fz_matrix /*transform*/,
    pdf_filter_options* /*options*/, void* factory_options)
{
    ColorTracker* t = reinterpret_cast<ColorTracker*>(
        pdf_new_processor(ctx, sizeof(ColorTracker)));  // zeroed by fz_calloc
    t->chain = chain;
    t->st = static_cast<FilterState*>(factory_options);
    t->depth = 0;  // stack[0] = default fill (None)

    pdf_processor& s = t->super;
    s.close_processor = ct_close_processor;
    // No s.drop_processor: chain is borrowed (see ct_close_processor note).
    s.push_resources = ct_push_resources;
    s.pop_resources = ct_pop_resources;

    s.op_w = ct_op_w; s.op_j = ct_op_j; s.op_J = ct_op_J; s.op_M = ct_op_M;
    s.op_d = ct_op_d; s.op_ri = ct_op_ri; s.op_i = ct_op_i;
    s.op_gs_begin = ct_op_gs_begin; s.op_gs_BM = ct_op_gs_BM;
    s.op_gs_ca = ct_op_gs_ca; s.op_gs_CA = ct_op_gs_CA;
    s.op_gs_SMask = ct_op_gs_SMask; s.op_gs_end = ct_op_gs_end;
    s.op_q = ct_op_q; s.op_Q = ct_op_Q; s.op_cm = ct_op_cm;
    s.op_m = ct_op_m; s.op_l = ct_op_l; s.op_c = ct_op_c; s.op_v = ct_op_v;
    s.op_y = ct_op_y; s.op_h = ct_op_h; s.op_re = ct_op_re;
    s.op_S = ct_op_S; s.op_s = ct_op_s; s.op_F = ct_op_F; s.op_f = ct_op_f;
    s.op_fstar = ct_op_fstar; s.op_B = ct_op_B; s.op_Bstar = ct_op_Bstar;
    s.op_b = ct_op_b; s.op_bstar = ct_op_bstar; s.op_n = ct_op_n;
    s.op_W = ct_op_W; s.op_Wstar = ct_op_Wstar;
    s.op_BT = ct_op_BT; s.op_ET = ct_op_ET;
    s.op_Tc = ct_op_Tc; s.op_Tw = ct_op_Tw; s.op_Tz = ct_op_Tz;
    s.op_TL = ct_op_TL; s.op_Tf = ct_op_Tf; s.op_Tr = ct_op_Tr; s.op_Ts = ct_op_Ts;
    s.op_Td = ct_op_Td; s.op_TD = ct_op_TD; s.op_Tm = ct_op_Tm; s.op_Tstar = ct_op_Tstar;
    s.op_TJ = ct_op_TJ; s.op_Tj = ct_op_Tj; s.op_squote = ct_op_squote;
    s.op_dquote = ct_op_dquote;
    s.op_d0 = ct_op_d0; s.op_d1 = ct_op_d1;
    s.op_CS = ct_op_CS; s.op_cs = ct_op_cs;
    s.op_SC_pattern = ct_op_SC_pattern; s.op_sc_pattern = ct_op_sc_pattern;
    s.op_SC_shade = ct_op_SC_shade; s.op_sc_shade = ct_op_sc_shade;
    s.op_SC_color = ct_op_SC_color; s.op_sc_color = ct_op_sc_color;
    s.op_G = ct_op_G; s.op_g = ct_op_g; s.op_RG = ct_op_RG; s.op_rg = ct_op_rg;
    s.op_K = ct_op_K; s.op_k = ct_op_k;
    s.op_BI = ct_op_BI; s.op_sh = ct_op_sh;
    s.op_Do_image = ct_op_Do_image; s.op_Do_form = ct_op_Do_form;
    s.op_MP = ct_op_MP; s.op_DP = ct_op_DP; s.op_BMC = ct_op_BMC;
    s.op_BDC = ct_op_BDC; s.op_EMC = ct_op_EMC;
    s.op_BX = ct_op_BX; s.op_EX = ct_op_EX;
    s.op_gs_OP = ct_op_gs_OP; s.op_gs_op = ct_op_gs_op; s.op_gs_OPM = ct_op_gs_OPM;
    s.op_gs_UseBlackPtComp = ct_op_gs_UseBlackPtComp;
    s.op_EOD = ct_op_EOD; s.op_END = ct_op_END;

    return &t->super;
}

#undef CT_C
#undef CT_FWD0
#undef CT_FWD_f
#undef CT_FWD_i
#undef CT_FWD_2f

// The text_filter callback: called for each text-showing operation.
// Returns 0 to keep the text, 1 to remove it.
static int textFilterCallback(
    fz_context* ctx, void* opaque,
    int* ucsbuf, int ucslen,
    fz_matrix trm, fz_matrix ctm, fz_rect bbox)
{
    (void)ctx;
    auto* state = static_cast<FilterState*>(opaque);

    // Transform glyph bbox to PDF user-space.
    //
    // MuPDF's pdf_filter_processor passes trm = Tm × Tfs WITHOUT CTM
    // compose (verified empirically via per-character logging on Lean
    // Startup p2: 'A' in the title header at user-y≈18 was reported as
    // trm=[34,0,0,-34,20,63] and ctm=[0.48,0,0,-0.48,0,350.68], where
    // 63 is the Tm.f from Td and 0.48/-0.48/350.68 is the page's outer
    // cm scope). To get the glyph's actual user-space bbox, we must
    // compose trm with ctm before transforming.
    //
    // Pre-fix the matcher used `fz_transform_rect(bbox, trm)` directly,
    // which produced bboxes in pre-CTM space — incorrectly placing every
    // glyph many hundreds of points below where it actually rendered on
    // the page. Result: the bbox-overlap check rarely matched anything,
    // including the visible body text the user clicked on, leaving the
    // original line intact alongside the new injection.
    fz_matrix combined = fz_concat(trm, ctm);
    fz_rect charPageBbox = fz_transform_rect(bbox, combined);

    // bugfix-3e91a7c4 #4/#5: map the glyph box into stext device space via the
    // page's captured user->device transform (state->pageCtm) — the SAME space
    // the span boxes (repl.bbox, from fz_new_stext_page_from_page) live in. The
    // old `pageHeight - y` flip silently dropped the page's origin (CropBox/
    // MediaBox y0), so on a nonzero-origin page the glyph and span boxes diverged
    // by a constant Δ = origin.y0 that short (small-font) spans could not absorb,
    // rejecting every glyph and no-op-ing the edit. fz_transform_rect normalizes
    // the transformed corners, so the result is a proper Y-down rect.
    fz_rect fitzCharBbox = fz_transform_rect(charPageBbox, state->pageCtm);

    // Check if this character belongs to the user-clicked span.
    //
    // Use a vertical-center test rather than a tolerance-padded bbox
    // overlap. Body-text lines are usually spaced with only 2–3pt of
    // gap between one line's descender and the next line's cap-top, so
    // any tolerance in the bbox-overlap check ≥ that gap will pull in
    // the adjacent line's characters. The clicked-span bbox is one
    // line's height (cap-top to descender-bottom); a character belongs
    // to that line iff its vertical center lies inside that bbox's y
    // range. Horizontally we still allow a small tolerance for chars
    // that extend just past the span's left/right edges.
    //
    // Right-edge: extend the match to the page's right margin. When a
    // previous edit on the same span produced text that overflowed past
    // the original span's right edge, the overflow glyphs end up at
    // x-positions > bbox.right but on the same baseline. Subsequent
    // edits (including undo back to the original) need to sweep them up
    // too — otherwise they remain as stale text "ea" / "ess" / etc. at
    // the page's right margin after undoing a many-character edit.
    // Extending only the RIGHT edge is safe because overflow always goes
    // right (LTR text); the left edge stays at the user-clicked span's
    // left so adjacent spans on the same line (e.g., a bold word in a
    // mixed paragraph) aren't accidentally captured.
    const float charMidY = (fitzCharBbox.y0 + fitzCharBbox.y1) * 0.5f;
    const float pageRightFitz = state->pageWidth > 0.0f
        ? state->pageWidth : 1e6f;
    auto replIt = std::find_if(state->replacements.begin(),
                               state->replacements.end(),
        [&fitzCharBbox, charMidY, pageRightFitz](const auto& repl) {
            if (charMidY < static_cast<float>(repl.bbox.top()) ||
                charMidY > static_cast<float>(repl.bbox.bottom())) {
                return false;
            }
            const float xtol = FilterState::BBOX_TOLERANCE;
            // Note: right edge extended to page right margin (see comment).
            return !(fitzCharBbox.x1 < static_cast<float>(repl.bbox.left()) - xtol ||
                     fitzCharBbox.x0 > pageRightFitz + xtol);
        });
    if (replIt == state->replacements.end()) {
        return 0; // Keep text
    }

    // Character falls within replacement area — remove it
    auto& repl = *replIt;
    if (!repl.injectionQueued) {
        repl.injectionQueued = true;

        FilterState::PendingInjection inj;
        inj.text = repl.newText;
        inj.trm = trm;
        inj.ctm = ctm;
        inj.spanBbox = repl.bbox;
        inj.fontName = repl.fontName;
        inj.fontSize = repl.fontSize;
        inj.textColor = repl.textColor;
        inj.textColorKnown = repl.textColorKnown;
        inj.capturedFill = state->currentFill;  // R3-06 source colourspace
        inj.runSegments = repl.runSegments;  // UI-TE-SAVE-08 phase-2
        state->pendingInjections.push_back(std::move(inj));
    }
    return 1; // Remove this character
}

// The after_text_object callback: called after each BT...ET block.
// Injects replacement text using the chain processor.
static void afterTextObjectCallback(
    fz_context* ctx, void* opaque,
    pdf_document* doc, pdf_processor* chain, fz_matrix ctm)
{
    auto* state = static_cast<FilterState*>(opaque);

    if (state->pendingInjections.empty())
        return;

    for (auto& inj : state->pendingInjections) {
        if (inj.text.empty()) continue;

        // Emit: BT
        chain->op_BT(ctx, chain);

        // Resolve PostScript font name to PDF resource key
        std::string resKey;
        auto resIt = state->psNameToResKey.find(inj.fontName);
        if (resIt != state->psNameToResKey.end()) {
            resKey = resIt->second;
        } else {
            resKey = inj.fontName;  // Fallback: try fontName directly
        }

        // Load font from page resources (or XObject resources as fallback) and emit Tf
        pdf_font_desc* fontDesc = nullptr;
        {
            // Try page resources first
            pdf_obj* fontDict = state->pageResources
                ? pdf_dict_get(ctx, state->pageResources, PDF_NAME(Font))
                : nullptr;
            pdf_obj* fontObj = fontDict
                ? pdf_dict_gets(ctx, fontDict, resKey.c_str())
                : nullptr;
            if (fontObj) {
                fontDesc = pdf_load_font(ctx, doc, state->pageResources, fontObj);
            }
            // Fallback: try XObject resources (text may be inside Form XObject)
            if (!fontDesc && state->xobjResources) {
                fontDict = pdf_dict_get(ctx, state->xobjResources, PDF_NAME(Font));
                fontObj = fontDict
                    ? pdf_dict_gets(ctx, fontDict, resKey.c_str())
                    : nullptr;
                if (fontObj) {
                    fontDesc = pdf_load_font(ctx, doc, state->xobjResources, fontObj);
                }
            }
        }

        // Encode replacement text into TJ-style runs (whitespace
        // characters that the font cannot render become pure-advance
        // runs so they don't render as missing-glyph boxes when the
        // font subset lacks the space glyph — see encodeTextWithFallback).
        const bool multiByte = (fontDesc && isMultiByteFont(fontDesc));
        std::vector<EncodedRun> runs =
            encodeTextWithFallback(ctx, fontDesc, inj.text, multiByte,
                                   &state->diag);

        // Compute canonical Tm + corrected Tfs. See the note on
        // computeCanonicalTextMatrix for why we don't recover Tm via
        // trm × ctm⁻¹ algebra.
        CanonicalTextMatrix tm =
            computeCanonicalTextMatrix(inj.trm, inj.ctm, inj.fontSize);

        // Override Tm.e / Tm.f to land at the user's clicked span bbox
        // (left edge + baseline) rather than wherever the bbox-overlap
        // matcher's first matching character happened to be. The matcher
        // can match a character on a different line than the clicked
        // span when the clicked-bbox tolerance overlaps adjacent lines —
        // using trm.e/trm.f then renders the new text at the matched
        // character's position, not the user's intended position.
        repositionFromSpanBbox(tm, inj.spanBbox, inj.ctm, state->pageHeight);

        if (fontDesc) {
            chain->op_Tf(ctx, chain, resKey.c_str(), fontDesc, tm.Tfs);
            pdf_drop_font(ctx, fontDesc);
        }

        // Drop empty trailing runs and bail if everything collapsed.
        while (!runs.empty()
               && runs.back().bytes.empty()
               && runs.back().advance == 0) {
            runs.pop_back();
        }
        if (runs.empty()) {
            chain->op_ET(ctx, chain);
            continue;
        }

        // Establish a known text-state basis for the injected replacement.
        // The canonical Tm/Tfs and the span-bbox repositioning both assume the
        // default text-state parameters; if the page left ambient char/word
        // spacing or text rise active when this injection is emitted inline
        // (the same pass-through structure as the -100 Tz mirror bug), the
        // replacement inherits it and is drawn distorted (e.g. a "50 Ts" rise
        // shifts the new text 50 units off the span it replaced). Reset Tz to
        // +100 (neutralize any ambient "-100 Tz" mirror) and Tc/Tw/Ts to 0.
        chain->op_Tz(ctx, chain, 100.0f);
        chain->op_Tc(ctx, chain, 0.0f);
        chain->op_Tw(ctx, chain, 0.0f);
        chain->op_Ts(ctx, chain, 0.0f);

        if (state->textRenderMode >= 0) {
            chain->op_Tr(ctx, chain, state->textRenderMode);
        }

        // R3-06: reproduce the ORIGINAL source colourspace operator when the
        // ColorTracker captured a non-RGB fill (DeviceCMYK `k`, DeviceGray `g`,
        // or a /Separation-/DeviceN spot via `/name cs … scn`) — otherwise the
        // edit silently flattened it to a visually-equivalent `rg`. DeviceRGB
        // and unknown fills fall through to the legacy textColor `rg` path.
        if (!emitCapturedFill(ctx, chain, inj.capturedFill) && inj.textColorKnown) {
            float r = static_cast<float>((inj.textColor >> 16) & 0xFF) / 255.0f;
            float g = static_cast<float>((inj.textColor >> 8) & 0xFF) / 255.0f;
            float b = static_cast<float>(inj.textColor & 0xFF) / 255.0f;
            chain->op_rg(ctx, chain, r, g, b);
        }

        chain->op_Tm(ctx, chain,
                     tm.Tm_a, tm.Tm_b, tm.Tm_c, tm.Tm_d, tm.Tm_e, tm.Tm_f);

        // Pick Tj (single text run, legacy fast path) vs TJ (multiple
        // runs with horizontal advances).
        bool hasAdvance = false;
        for (const auto& r : runs) {
            if (r.advance != 0) { hasAdvance = true; break; }
        }
        if (!hasAdvance) {
            std::string flat;
            for (const auto& r : runs) flat += r.bytes;
            if (!flat.empty()) {
                char* buf = static_cast<char*>(fz_malloc(ctx, flat.size() + 1));
                std::memcpy(buf, flat.data(), flat.size());
                buf[flat.size()] = '\0';
                chain->op_Tj(ctx, chain, buf, flat.size());
                fz_free(ctx, buf);
            }
        } else {
            pdf_obj* arr = pdf_new_array(ctx, doc, static_cast<int>(runs.size()));
            for (const auto& r : runs) {
                if (!r.bytes.empty()) {
                    pdf_obj* s = pdf_new_string(
                        ctx, r.bytes.data(),
                        static_cast<size_t>(r.bytes.size()));
                    pdf_array_push_drop(ctx, arr, s);
                }
                if (r.advance != 0) {
                    pdf_array_push_drop(ctx, arr, pdf_new_int(ctx, r.advance));
                }
            }
            chain->op_TJ(ctx, chain, arr);
            pdf_drop_obj(ctx, arr);
        }

        chain->op_ET(ctx, chain);
    }

    state->pendingInjections.clear();
}

// The after_text_object callback for font substitution: emits Tf/Tz operators.
static void afterTextObjectWithSubstitutionCallback(
    fz_context* ctx, void* opaque,
    pdf_document* doc, pdf_processor* chain, fz_matrix ctm)
{
    auto* state = static_cast<FilterState*>(opaque);

    if (state->pendingInjections.empty())
        return;

    // Helper: load a pdf_font_desc by resource name from page or
    // XObject resources. Returns nullptr if not found. Caller owns
    // the returned descriptor and must pdf_drop_font it.
    auto loadFont = [&](const std::string& resName) -> pdf_font_desc* {
        pdf_font_desc* fd = nullptr;
        pdf_obj* fontDict = state->pageResources
            ? pdf_dict_get(ctx, state->pageResources, PDF_NAME(Font))
            : nullptr;
        pdf_obj* fontObj = fontDict
            ? pdf_dict_gets(ctx, fontDict, resName.c_str())
            : nullptr;
        if (fontObj) {
            fd = pdf_load_font(ctx, doc, state->pageResources, fontObj);
        }
        if (!fd && state->xobjResources) {
            fontDict = pdf_dict_get(ctx, state->xobjResources, PDF_NAME(Font));
            fontObj = fontDict
                ? pdf_dict_gets(ctx, fontDict, resName.c_str())
                : nullptr;
            if (fontObj) {
                fd = pdf_load_font(ctx, doc, state->xobjResources, fontObj);
            }
        }
        return fd;
    };

    // Helper: emit one text run to the filter chain. When the encoder
    // produces only encodable bytes, falls back to op_Tj (the legacy
    // path that 30+ existing dominant-run-locking tests cover). When
    // the encoder produces TJ-style runs (whitespace fallback for
    // unencodable space-like chars in subsetted fonts), builds a
    // pdf_array of alternating strings + numbers and emits op_TJ.
    auto emitText = [&](pdf_font_desc* fd, const std::u32string& text) {
        if (text.empty()) return;
        const bool multiByte = (fd && isMultiByteFont(fd));
        std::vector<EncodedRun> runs =
            encodeTextWithFallback(ctx, fd, text, multiByte, &state->diag);
        // Drop trailing empty pure-text runs.
        while (!runs.empty()
               && runs.back().bytes.empty()
               && runs.back().advance == 0) {
            runs.pop_back();
        }
        if (runs.empty()) return;
        // No advances → simple Tj (legacy fast path).
        bool hasAdvance = false;
        for (const auto& r : runs) {
            if (r.advance != 0) { hasAdvance = true; break; }
        }
        if (!hasAdvance) {
            std::string flat;
            for (const auto& r : runs) flat += r.bytes;
            if (flat.empty()) return;
            char* buf = static_cast<char*>(fz_malloc(ctx, flat.size() + 1));
            std::memcpy(buf, flat.data(), flat.size());
            buf[flat.size()] = '\0';
            chain->op_Tj(ctx, chain, buf, flat.size());
            fz_free(ctx, buf);
            return;
        }
        // Build the TJ array: strings interleaved with numeric advances.
        pdf_obj* arr = pdf_new_array(ctx, doc, static_cast<int>(runs.size()));
        for (const auto& r : runs) {
            if (!r.bytes.empty()) {
                pdf_obj* s = pdf_new_string(
                    ctx, r.bytes.data(),
                    static_cast<size_t>(r.bytes.size()));
                pdf_array_push_drop(ctx, arr, s);
            }
            if (r.advance != 0) {
                pdf_array_push_drop(ctx, arr, pdf_new_int(ctx, r.advance));
            }
        }
        chain->op_TJ(ctx, chain, arr);
        pdf_drop_obj(ctx, arr);
    };

    for (auto& inj : state->pendingInjections) {
        if (inj.text.empty()) continue;

        // ───────────────────────────────────────────────────────
        // UI-TE-SAVE-08 phase-2: when runSegments is non-empty,
        // walk segments in vector order (caller in TextEditCommands
        // sorts them by charStart) and emit one Tf/Tj pair per
        // non-empty segment. charStart/charEnd index into the UTF-32
        // injection text (inj.text); each segment's slice is encoded
        // against its own loaded font. Tm is computed once from the
        // first non-empty segment's fontSize and reused — only Tf
        // (and Tj) are re-emitted per segment, not Tm.
        // ───────────────────────────────────────────────────────
        const bool perRun = !inj.runSegments.empty();

        // Emit: BT
        chain->op_BT(ctx, chain);

        // Resolve the *initial* font name (legacy path uses inj.fontName;
        // per-run path uses the first non-empty segment's fontResName).
        std::string fontNameToUse = inj.fontName;
        // Default 100 so the replacement always neutralizes any ambient horizontal
        // text scaling (e.g. a page-level "-100 Tz" that would otherwise mirror the
        // new text); a font switch overrides with its own scale below.
        float hScale = 100.0f;
        float initialFontSize = inj.fontSize;

        if (perRun) {
            // Find the first non-empty segment for Tm anchoring.
            const pdfe::fonts::RunSegment* anchor = nullptr;
            for (const auto& seg : inj.runSegments) {
                if (seg.charEnd > seg.charStart) { anchor = &seg; break; }
            }
            if (anchor) {
                fontNameToUse = anchor->fontResName;
                initialFontSize = anchor->fontSize;
                // Per-run resName may be a PostScript name OR a PDF
                // resource key — resolve via psNameToResKey for the
                // PS-name case so loadFont() finds the descriptor.
                auto resIt = state->psNameToResKey.find(fontNameToUse);
                if (resIt != state->psNameToResKey.end()) {
                    fontNameToUse = resIt->second;
                }
            }
        } else {
            auto it = state->fontSwitches.find(inj.fontName);
            if (it != state->fontSwitches.end()) {
                fontNameToUse = it->second.substituteResName;
                hScale = it->second.horizontalScale * 100.0f;
            }
            // Resolve PostScript name to PDF resource key if needed
            auto resIt = state->psNameToResKey.find(fontNameToUse);
            if (resIt != state->psNameToResKey.end()) {
                fontNameToUse = resIt->second;
            }
        }

        pdf_font_desc* fontDesc = loadFont(fontNameToUse);

        // Compute canonical Tm + corrected Tfs from the *initial* font
        // size (first segment's size in per-run mode; whole-injection
        // size in legacy mode). The Tfs we emit must be pre-CTM (PDF
        // spec); initialFontSize is the on-page point size, so divide
        // out the CTM's scale.
        CanonicalTextMatrix tm =
            computeCanonicalTextMatrix(inj.trm, inj.ctm, initialFontSize);

        // Override Tm.e/Tm.f to land at the user's clicked span (see
        // afterTextObjectCallback for the rationale).
        repositionFromSpanBbox(tm, inj.spanBbox, inj.ctm, state->pageHeight);

        if (fontDesc) {
            chain->op_Tf(ctx, chain, fontNameToUse.c_str(), fontDesc, tm.Tfs);
            pdf_drop_font(ctx, fontDesc);
            fontDesc = nullptr;
        }

        if (hScale > 0.0f) {
            chain->op_Tz(ctx, chain, hScale);
        }
        // Known text-state basis for the injected replacement: reset ambient
        // char/word spacing and text rise so they don't leak into the new text
        // (see the matching note in afterTextObjectCallback).
        chain->op_Tc(ctx, chain, 0.0f);
        chain->op_Tw(ctx, chain, 0.0f);
        chain->op_Ts(ctx, chain, 0.0f);

        if (state->textRenderMode >= 0) {
            chain->op_Tr(ctx, chain, state->textRenderMode);
        }

        // R3-06: reproduce the captured source colourspace operator (k/g/scn)
        // when non-RGB; DeviceRGB/unknown fall through to the legacy rg path.
        if (!emitCapturedFill(ctx, chain, inj.capturedFill) && inj.textColorKnown) {
            float r = static_cast<float>((inj.textColor >> 16) & 0xFF) / 255.0f;
            float g = static_cast<float>((inj.textColor >> 8) & 0xFF) / 255.0f;
            float b = static_cast<float>(inj.textColor & 0xFF) / 255.0f;
            chain->op_rg(ctx, chain, r, g, b);
        }

        chain->op_Tm(ctx, chain,
                     tm.Tm_a, tm.Tm_b, tm.Tm_c, tm.Tm_d, tm.Tm_e, tm.Tm_f);

        if (perRun) {
            // ───────────────────────────────────────────────────
            // Per-run emission: walk segments in order, emit Tf
            // (when font changes) + Tj for each non-empty slice.
            // Tm is shared (set above); each segment may switch
            // font/size via its own op_Tf inside the BT/ET block.
            // ───────────────────────────────────────────────────
            std::string currentFontResName = fontNameToUse;
            float currentFontSize = initialFontSize;
            const auto totalChars = static_cast<int>(inj.text.size());

            for (const auto& seg : inj.runSegments) {
                // Skip empty slices (model layer accepts charStart==charEnd).
                if (seg.charEnd <= seg.charStart) continue;
                // Clamp to the actual text bounds — the model layer
                // does not validate, so segments could exceed.
                const int s = std::max(0, std::min(seg.charStart, totalChars));
                const int e = std::max(s, std::min(seg.charEnd, totalChars));
                if (e == s) continue;

                std::u32string slice(inj.text.begin() + s, inj.text.begin() + e);

                // Resolve PS-name → PDF resource key for this segment
                // too (same as the initial-font resolution above).
                std::string segResName = seg.fontResName;
                auto segResIt = state->psNameToResKey.find(segResName);
                if (segResIt != state->psNameToResKey.end()) {
                    segResName = segResIt->second;
                }

                // Emit Tf only when the font (name OR size) actually changes.
                const bool fontChanged =
                    segResName != currentFontResName ||
                    std::abs(seg.fontSize - currentFontSize) > 1e-6f;

                pdf_font_desc* segFont = loadFont(segResName);
                if (fontChanged && segFont) {
                    // Tfs in the per-run path uses the segment's own
                    // post-CTM-divided font size for visual consistency
                    // with the legacy path's Tfs computation.
                    CanonicalTextMatrix segTm =
                        computeCanonicalTextMatrix(inj.trm, inj.ctm, seg.fontSize);
                    chain->op_Tf(ctx, chain, segResName.c_str(),
                                 segFont, segTm.Tfs);
                    currentFontResName = segResName;
                    currentFontSize = seg.fontSize;
                }

                emitText(segFont, slice);
                if (segFont) {
                    pdf_drop_font(ctx, segFont);
                }
            }

            chain->op_ET(ctx, chain);
            continue;
        }

        // ───────────────────────────────────────────────────────
        // Legacy single-Tf path (runSegments empty). Unchanged
        // from pre-SAVE-08-phase-2 behavior. The 30+ existing
        // dominant-run-locking tests all flow through here.
        // ───────────────────────────────────────────────────────
        pdf_font_desc* legacyFont = loadFont(fontNameToUse);
        emitText(legacyFont, inj.text);
        if (legacyFont) pdf_drop_font(ctx, legacyFont);

        chain->op_ET(ctx, chain);
    }

    state->pendingInjections.clear();
}

class ContentStreamEditor::Impl {
public:
    pdfe::core::MuPdfContext* context;

    // phase-75: diagnostics from the most recent apply call.
    EncodingDiagnostics diag;

    explicit Impl(pdfe::core::MuPdfContext* ctx) : context(ctx) {}
};

const EncodingDiagnostics& ContentStreamEditor::lastEncodingDiagnostics() const
{
    return pImpl->diag;
}

// Copy the filter run's encoding diagnostics back onto the editor and say so
// out loud. Called at the end of both apply paths.
//
// The warning is deliberately unconditional on log level and names the
// codepoints: this is the only place in the save path that knows the edit did
// not survive encoding, and the failure it reports (a CJK edit saved as '?' or
// NUL) produces a file that opens cleanly and looks fine to every other check.
static void publishEncodingDiagnostics(const FilterState& state,
                                       EncodingDiagnostics& out)
{
    out = state.diag;
    if (out.empty()) return;
    auto hex = [](const std::vector<char32_t>& v) {
        QStringList parts;
        for (char32_t cp : v)
            parts << QString("U+%1").arg(static_cast<uint32_t>(cp), 4, 16,
                                         QChar('0')).toUpper();
        return parts.join(", ");
    };
    if (!out.unencodableSimple.empty()) {
        qWarning().noquote()
            << "[ContentStreamEditor] edit lost" << out.unencodableSimple.size()
            << "character(s): the target font is a simple (single-byte) font "
               "and cannot encode" << hex(out.unencodableSimple)
            << "- they were written as '?'";
    }
    if (!out.notdefCid.empty()) {
        qWarning().noquote()
            << "[ContentStreamEditor] edit lost" << out.notdefCid.size()
            << "character(s): the composite font has no glyph for"
            << hex(out.notdefCid) << "- they were written as CID 0 (.notdef)";
    }
}

ContentStreamEditor::ContentStreamEditor(pdfe::core::MuPdfContext* context)
    : context_(context)
    , pImpl(std::make_unique<Impl>(context))
{
}

ContentStreamEditor::~ContentStreamEditor() = default;

void ContentStreamEditor::applyEdits(
    const pdfe::core::Page* page,
    const pdfe::core::Document* document,
    const std::vector<TextSpan>& allSpans,
    const std::vector<std::pair<int, std::u32string>>& modifications,
    int textRenderMode)
{
    pImpl->diag.clear();
    if (!page || !document || modifications.empty())
        return;

    // A text edit rewrites the page content stream via pdf_filter_page_contents,
    // which orphans the ORIGINAL content stream object (containing the pre-edit
    // text). Persisting that incrementally would leave the original text
    // recoverable in the file — a confidentiality regression, since users edit
    // text precisely to remove/replace sensitive strings. Force the full-rewrite
    // path so the orphaned stream is scrubbed (pipeline b2e47d91, Tier 1).
    document->markRequiresFullRewrite();

    // Build filter state
    FilterState state;
    state.textRenderMode = textRenderMode;
    for (const auto& [spanIndex, newText] : modifications) {
        if (spanIndex < 0 || spanIndex >= static_cast<int>(allSpans.size()))
            continue;

        const auto& span = allSpans[spanIndex];
        FilterState::Replacement repl;
        repl.bbox = span.boundingBox;
        repl.newText = newText;
        repl.fontName = span.fontName;
        repl.fontSize = span.fontSize;
        // Original fill color: the span's captured source color (so the edit keeps
        // the original color instead of falling to black / the ambient fill), then
        // let a user-applied formatting run override it.
        if (span.textColorKnown) {
            repl.textColor = span.textColor;
            repl.textColorKnown = true;
        }
        if (!span.formattingRuns.empty()) {
            const auto* dominant = &span.formattingRuns[0];
            for (const auto& run : span.formattingRuns) {
                if ((run.charEnd - run.charStart) > (dominant->charEnd - dominant->charStart))
                    dominant = &run;
            }
            repl.textColor = dominant->textColor;
            repl.textColorKnown = true;
        }
        state.replacements.push_back(std::move(repl));
    }

    if (state.replacements.empty())
        return;

    fz_page* fzPage = page->fzPage();
    fz_document* fzDoc = document->fzDocument();

    context_->execute([&state, fzPage, fzDoc](fz_context* ctx) {
        pdf_page* pdfPage = pdf_page_from_fz_page(ctx, fzPage);
        pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, fzDoc);

        state.pdfDoc = pdfDoc;
        state.pageResources = pdf_page_resources(ctx, pdfPage);

        // bugfix-3e91a7c4 #4/#5 + B1 rotate×edit fix: capture the user->device
        // transform and page dimensions, re-expressing span match boxes into
        // UNROTATED page space when the page carries a /Rotate (see helper).
        setupSpanMatchSpace(ctx, pdfPage, reinterpret_cast<fz_page*>(fzPage),
                            state);

        buildFontNameMapping(ctx, state);

        pdf_sanitize_filter_options sopts;
        std::memset(&sopts, 0, sizeof(sopts));
        sopts.opaque = &state;
        sopts.text_filter = textFilterCallback;
        sopts.after_text_object = afterTextObjectCallback;
        sopts.image_filter = nullptr;
        sopts.culler = nullptr;

        // Set up filter factory chain. R3-06: the ColorTracker runs FIRST
        // (upstream of sanitize) so it observes the raw fill-colour operators
        // and records FilterState::currentFill before sanitize invokes the
        // text_filter callback — letting an edited CMYK/spot span reproduce its
        // source colourspace operator instead of flattening to rg.
        pdf_filter_factory factories[3];
        std::memset(factories, 0, sizeof(factories));
        factories[0].filter = newColorTrackerProcessor;
        factories[0].options = &state;
        factories[1].filter = pdf_new_sanitize_filter;
        factories[1].options = &sopts;
        factories[2].filter = nullptr;
        factories[2].options = nullptr;

        // Set up filter options
        pdf_filter_options opts;
        std::memset(&opts, 0, sizeof(opts));
        opts.recurse = 1;          // Recurse into Form XObjects (text often lives there)
        opts.instance_forms = 1;   // Create private XObject copies to avoid corrupting shared ones
        opts.ascii = 0;
        opts.no_update = 0;
        opts.opaque = nullptr;
        opts.complete = nullptr;
        opts.filters = factories;
        opts.newlines = 1;

        // Apply the filter to the page contents
        pdf_filter_page_contents(ctx, pdfDoc, pdfPage, &opts);

    });

    publishEncodingDiagnostics(state, pImpl->diag);
}

// Pipeline F phase-2: snapshot/restore /Contents helpers. Lives here
// because Page + Document grant friend access to ContentStreamEditor;
// EditTextCommand lacks that access and would otherwise need to be
// added as a friend everywhere.
void* ContentStreamEditor::captureContentsSnapshot(
    pdfe::core::Document* document, int pageIndex)
{
    if (!document) return nullptr;
    fz_document* fzDoc = document->fzDocument();
    if (!fzDoc) return nullptr;
    void* snap = nullptr;
    context_->execute([fzDoc, pageIndex, &snap](fz_context* ctx) {
        pdf_document* pdfDoc =
            pdf_document_from_fz_document(ctx, fzDoc);
        if (!pdfDoc) return;
        pdf_obj* pageObj = pdf_lookup_page_obj(ctx, pdfDoc, pageIndex);
        if (!pageObj) return;
        pdf_obj* contents = pdf_dict_get(ctx, pageObj, PDF_NAME(Contents));
        if (contents) {
            // R2-02: deep-copy the /Contents object into its OWN standalone
            // pdf_document (holder) so it survives a full-rewrite save
            // (Document::save drop+reopen / saveAs GC-renumber). A raw
            // pdf_keep_obj would dangle across such a save → post-save undo UAF.
            snap = pdfe::core::makeRetainedObjHolder(ctx, contents);
        }
    });
    return snap;
}

void ContentStreamEditor::restoreContentsSnapshot(
    pdfe::core::Document* document, int pageIndex, void* snapshot)
{
    if (!document || !snapshot) return;
    fz_document* fzDoc = document->fzDocument();
    if (!fzDoc) return;
    context_->execute([fzDoc, pageIndex, snapshot](fz_context* ctx) {
        pdf_document* pdfDoc =
            pdf_document_from_fz_document(ctx, fzDoc);
        if (!pdfDoc) return;
        pdf_obj* pageObj = pdf_lookup_page_obj(ctx, pdfDoc, pageIndex);
        if (!pageObj) return;
        // NON-consuming: graft a COPY of the held /Contents into the LIVE doc
        // and put that; drop the local ref (pdf_dict_put keeps its own). The
        // holder stays owned by the command (freed via dropContentsSnapshot).
        pdf_obj* live = pdfe::core::graftRetainedInto(ctx, pdfDoc, snapshot);
        if (live) {
            pdf_dict_put(ctx, pageObj, PDF_NAME(Contents), live);
            pdf_drop_obj(ctx, live);
        }
    });
}

void ContentStreamEditor::dropContentsSnapshot(void* snapshot)
{
    if (!snapshot) return;
    context_->execute([snapshot](fz_context* ctx) {
        pdfe::core::dropRetainedObjHolder(ctx, snapshot);
    });
}

void ContentStreamEditor::applyEditsWithSubstitution(
    const pdfe::core::Page* page,
    const pdfe::core::Document* document,
    const std::vector<TextSpan>& allSpans,
    const std::vector<std::pair<int, std::u32string>>& modifications,
    const std::vector<pdfe::fonts::SubstitutionInfo>& substitutions,
    int textRenderMode)
{
    pImpl->diag.clear();
    if (!page || !document || modifications.empty())
        return;

    // Content-stream rewrite orphans the original text — see applyEdits.
    // Force the full-rewrite save path (pipeline b2e47d91, Tier 1).
    document->markRequiresFullRewrite();

    // If no substitutions, delegate to existing method
    if (substitutions.empty()) {
        applyEdits(page, document, allSpans, modifications, textRenderMode);
        return;
    }

    FilterState state;
    state.hasSubstitutions = true;
    state.textRenderMode = textRenderMode;

    for (const auto& [spanIndex, newText] : modifications) {
        if (spanIndex < 0 || spanIndex >= static_cast<int>(allSpans.size()))
            continue;

        const auto& span = allSpans[spanIndex];
        FilterState::Replacement repl;
        repl.bbox = span.boundingBox;
        repl.newText = newText;
        repl.fontName = span.fontName;
        repl.fontSize = span.fontSize;
        // Original fill color: the span's captured source color (so the edit keeps
        // the original color instead of falling to black / the ambient fill), then
        // let a user-applied formatting run override it.
        if (span.textColorKnown) {
            repl.textColor = span.textColor;
            repl.textColorKnown = true;
        }
        if (!span.formattingRuns.empty()) {
            const auto* dominant = &span.formattingRuns[0];
            for (const auto& run : span.formattingRuns) {
                if ((run.charEnd - run.charStart) > (dominant->charEnd - dominant->charStart))
                    dominant = &run;
            }
            repl.textColor = dominant->textColor;
            repl.textColorKnown = true;
        }
        state.replacements.push_back(std::move(repl));
    }

    if (state.replacements.empty())
        return;

    // Build font switch map from substitution info
    for (const auto& sub : substitutions) {
        FilterState::FontSwitch fs;
        // Empty substituteResName means keep original font, just apply Tz scaling
        fs.substituteResName = sub.substituteResName.empty()
            ? sub.originalFontName : sub.substituteResName;
        fs.horizontalScale = sub.horizontalScale;
        state.fontSwitches[sub.originalFontName] = std::move(fs);

        // UI-TE-SAVE-08 phase-2: propagate per-run segments onto the
        // matching Replacement (by spanIndex). The matching modification
        // entry's index in `modifications` is the same as the Replacement's
        // index in state.replacements (they're built in the same order).
        if (!sub.runSegments.empty()) {
            for (size_t i = 0; i < modifications.size() &&
                               i < state.replacements.size(); ++i) {
                if (modifications[i].first == sub.spanIndex) {
                    state.replacements[i].runSegments = sub.runSegments;
                    break;
                }
            }
        }
    }

    // Get MuPDF handles via friend access
    fz_page* fzPage = page->fzPage();
    fz_document* fzDoc = document->fzDocument();

    // UI-TE-SAVE-08 phase-4: detect whether any substitution carries
    // per-run segments. Per-run substitutions emit Tf operators (in
    // afterTextObjectWithSubstitutionCallback) whose font names refer
    // to fonts that PdfFontEmbedder added to the page Resources before
    // the filter ran. pdf_filter_page_contents rewrites Resources/Font
    // to only the fonts referenced by the *pre-filter* content; the new
    // Tf names get stranded. We snapshot the pre-filter Font dict and
    // re-insert any entry the filter dropped, so do_garbage in saveAs
    // sees them as reachable.
    bool needsResourceReinsert = false;
    for (const auto& sub : substitutions) {
        if (!sub.runSegments.empty()) { needsResourceReinsert = true; break; }
    }

    context_->execute([&state, fzPage, fzDoc, needsResourceReinsert](fz_context* ctx) {
        pdf_page* pdfPage = pdf_page_from_fz_page(ctx, fzPage);
        pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, fzDoc);

        // Store document/resources in state for callback use
        state.pdfDoc = pdfDoc;
        state.pageResources = pdf_page_resources(ctx, pdfPage);

        // Get page height for coordinate conversion (fitz Y-down ↔ PDF Y-up)
        // and re-express span match boxes into UNROTATED page space when the
        // page carries a /Rotate (B1 rotate×edit clean-inverse fix; see helper).
        setupSpanMatchSpace(ctx, pdfPage, reinterpret_cast<fz_page*>(fzPage),
                            state);

        // Build PostScript name → resource key mapping
        buildFontNameMapping(ctx, state);

        // Phase-4: snapshot the pre-filter Font dict so we can re-insert
        // any entry pdf_filter_page_contents drops. pdf_keep_obj on each
        // value bumps refcount so the object survives even if the filter
        // dereferences the only other reference.
        std::vector<std::pair<std::string, pdf_obj*>> preFilterFonts;
        if (needsResourceReinsert && state.pageResources) {
            pdf_obj* fontDict =
                pdf_dict_get(ctx, state.pageResources, PDF_NAME(Font));
            if (fontDict) {
                int len = pdf_dict_len(ctx, fontDict);
                preFilterFonts.reserve(len);
                for (int i = 0; i < len; ++i) {
                    pdf_obj* key = pdf_dict_get_key(ctx, fontDict, i);
                    pdf_obj* val = pdf_dict_get_val(ctx, fontDict, i);
                    if (!key || !val) continue;
                    const char* keyStr = pdf_to_name(ctx, key);
                    if (!keyStr) continue;
                    pdf_keep_obj(ctx, val);
                    preFilterFonts.emplace_back(keyStr, val);
                }
            }
        }

        // Set up the sanitize filter options with substitution-aware callback
        pdf_sanitize_filter_options sopts;
        std::memset(&sopts, 0, sizeof(sopts));
        sopts.opaque = &state;
        sopts.text_filter = textFilterCallback;
        sopts.after_text_object = afterTextObjectWithSubstitutionCallback;
        sopts.image_filter = nullptr;
        sopts.culler = nullptr;

        // Set up filter factory chain. R3-06: ColorTracker upstream of the
        // sanitize filter (see applyEdits for rationale) so edited CMYK/spot
        // spans keep their source colourspace operator through the rewrite.
        pdf_filter_factory factories[3];
        std::memset(factories, 0, sizeof(factories));
        factories[0].filter = newColorTrackerProcessor;
        factories[0].options = &state;
        factories[1].filter = pdf_new_sanitize_filter;
        factories[1].options = &sopts;
        factories[2].filter = nullptr;
        factories[2].options = nullptr;

        // Set up filter options
        pdf_filter_options opts;
        std::memset(&opts, 0, sizeof(opts));
        opts.recurse = 1;          // Recurse into Form XObjects (text often lives there)
        opts.instance_forms = 1;   // Create private XObject copies to avoid corrupting shared ones
        opts.ascii = 0;
        opts.no_update = 0;
        opts.opaque = nullptr;
        opts.complete = nullptr;
        opts.filters = factories;
        opts.newlines = 1;

        // Phase-4 + D2 phase-2: filter + re-insert in an fz_try block,
        // with the snapshot-keep-release in fz_always so the
        // pdf_keep_obj refs from preFilterFonts are released even on
        // longjmp from pdf_filter_page_contents (which uses fz_throw
        // on failure). The C++ stack unwind from fz_throw bypasses
        // ordinary destructors; without fz_always here, every kept
        // font would leak one refcount whenever the filter throws.
        fz_try(ctx) {
            pdf_filter_page_contents(ctx, pdfDoc, pdfPage, &opts);

            // Re-insert any pre-filter Font entry the filter dropped.
            // Re-fetch page resources because the filter may have
            // produced a new Resources dict. pdf_dict_puts on the
            // post-filter dict bumps the value's refcount; the
            // matching pdf_drop_obj in fz_always releases our snapshot
            // keep, leaving net refs from the dict entry + any
            // preserved pre-filter references.
            if (needsResourceReinsert && !preFilterFonts.empty()) {
                pdf_obj* postResources = pdf_page_resources(ctx, pdfPage);
                pdf_obj* postFontDict = postResources
                    ? pdf_dict_get(ctx, postResources, PDF_NAME(Font))
                    : nullptr;
                if (postResources && !postFontDict) {
                    postFontDict = pdf_new_dict(ctx, pdfDoc, 4);
                    pdf_dict_put_drop(ctx, postResources, PDF_NAME(Font),
                                      postFontDict);
                    postFontDict =
                        pdf_dict_get(ctx, postResources, PDF_NAME(Font));
                }
                if (postFontDict) {
                    for (const auto& [name, val] : preFilterFonts) {
                        if (!pdf_dict_gets(ctx, postFontDict, name.c_str())) {
                            pdf_dict_puts(ctx, postFontDict, name.c_str(),
                                          val);
                        }
                    }
                }
            }
        } fz_always(ctx) {
            for (auto& entry : preFilterFonts) {
                pdf_drop_obj(ctx, entry.second);
            }
        } fz_catch(ctx) {
            fz_rethrow(ctx);
        }
    });

    publishEncodingDiagnostics(state, pImpl->diag);
}

CanonicalTextMatrixResult canonicalTextMatrixForReplacement(
    const float trm[6], const float ctm[6], float visibleFontSize)
{
    fz_matrix t = {trm[0], trm[1], trm[2], trm[3], trm[4], trm[5]};
    fz_matrix c = {ctm[0], ctm[1], ctm[2], ctm[3], ctm[4], ctm[5]};
    CanonicalTextMatrix m = computeCanonicalTextMatrix(t, c, visibleFontSize);
    CanonicalTextMatrixResult r;
    r.Tfs = m.Tfs;
    r.Tm_a = m.Tm_a;
    r.Tm_b = m.Tm_b;
    r.Tm_c = m.Tm_c;
    r.Tm_d = m.Tm_d;
    r.Tm_e = m.Tm_e;
    r.Tm_f = m.Tm_f;
    return r;
}

} // namespace pdfe::textedit
