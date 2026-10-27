#include "ToUnicodeRecovery.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <utility>
#include <set>
#include <string>
#include <vector>

namespace pdfe::core {
namespace {

// ─── glyph-name resolution ───────────────────────────────────────────────────

/// Parse the ``uniXXXX`` / ``uXXXX``..``uXXXXXX`` numeric glyph-name
/// conventions. Only the FIRST codepoint of a ``uniXXXXXXXX`` sequence would be
/// representable in the single-value form the rest of this file emits, so a
/// multi-codepoint name is deliberately REFUSED (returns 0) rather than
/// silently truncated to a half-truth.
int unicodeFromNumericName(const std::string& n) {
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    auto parseHex = [&](size_t from, size_t len) -> int {
        int v = 0;
        for (size_t i = 0; i < len; ++i) {
            const int d = hexVal(n[from + i]);
            if (d < 0) return -1;
            v = (v << 4) | d;
        }
        return v;
    };
    if (n.size() == 7 && n.compare(0, 3, "uni") == 0)
        return parseHex(3, 4);
    if (n.size() > 7 && n.compare(0, 3, "uni") == 0)
        return 0;  // multi-codepoint sequence — refuse rather than truncate
    if (n.size() >= 5 && n.size() <= 7 && n[0] == 'u')
        return parseHex(1, n.size() - 1);
    return 0;
}

/// Strict AGL lookup that REJECTS an unusable answer (notably the Corporate-Use
/// PUA codepoints the Adobe Expert names legitimately map to).
int usableStrictAgl(const std::string& n) {
    if (n.empty()) return 0;
    const int cp = fz_unicode_from_glyph_name_strict(n.c_str());
    return isUnusableExtractedCodepoint(cp) ? 0 : cp;
}

/// Steps 1-2 of the documented order, applied to one candidate spelling.
int resolveBaseSpelling(const std::string& n) {
    if (int cp = usableStrictAgl(n)) return cp;
    const int num = unicodeFromNumericName(n);
    if (num > 0 && !isUnusableExtractedCodepoint(num)) return num;
    return 0;
}

/// The Adobe Expert-set descriptive suffixes. ``Ssmall`` is a SMALL CAPITAL S —
/// the character the reader sees, and therefore searches for, is ``S``.
constexpr const char* kExpertSuffixes[] = {
    "smallhyphen", "small", "oldstyle", "superior", "inferior",
    "numerator", "denominator",
};

bool endsWith(const std::string& s, const char* suffix) {
    const size_t n = std::char_traits<char>::length(suffix);
    return s.size() > n && s.compare(s.size() - n, n, suffix) == 0;
}

}  // namespace

bool isUnusableExtractedCodepoint(int cp) {
    if (cp <= 0) return true;
    if (cp == 0xFFFD) return true;
    if (cp < 0x20) return true;                      // C0 controls
    if (cp >= 0x7F && cp <= 0x9F) return true;       // DEL + C1 controls
    if (cp >= 0xE000 && cp <= 0xF8FF) return true;   // BMP Private Use Area
    return false;
}

int recoverUnicodeFromGlyphName(const std::string& glyphName) {
    if (glyphName.empty()) return 0;

    // 1-2: the name as written.
    if (int cp = resolveBaseSpelling(glyphName)) return cp;

    // 3: variant conventions — "S.sc", "one.tf", "S_alt". Take the part before
    //    the FIRST separator, which is the base glyph by convention.
    const size_t sep = glyphName.find_first_of("._");
    if (sep != std::string::npos && sep > 0) {
        const std::string base = glyphName.substr(0, sep);
        if (int cp = resolveBaseSpelling(base)) return cp;
        // A variant spelling may itself carry an Expert suffix ("Ssmall.alt").
        for (const char* suffix : kExpertSuffixes) {
            if (!endsWith(base, suffix)) continue;
            const std::string stem =
                base.substr(0, base.size() - std::char_traits<char>::length(suffix));
            if (int cp = resolveBaseSpelling(stem)) return cp;
        }
    }

    // 4: Adobe Expert-set descriptive suffixes on the plain name.
    for (const char* suffix : kExpertSuffixes) {
        if (!endsWith(glyphName, suffix)) continue;
        const std::string stem =
            glyphName.substr(0, glyphName.size() - std::char_traits<char>::length(suffix));
        if (int cp = resolveBaseSpelling(stem)) return cp;
    }

    return 0;
}

namespace {

// ─── per-font analysis ───────────────────────────────────────────────────────

/// One font's synthesized code -> Unicode table. POD only: instances of this are
/// filled while MuPDF calls that can longjmp are in flight, so it must have no
/// destructor to skip.
struct FontPlan {
    pdf_obj* fontRef = nullptr;   ///< the (possibly indirect) value from /Font
    pdf_obj* fontDict = nullptr;  ///< resolved dictionary
    std::array<int, 256> cp{};    ///< 0 == no mapping for that code
    bool improved = false;        ///< PDFE beat MuPDF for at least one code
};

/// The base-encoding glyph-name table implied by a font's /Encoding, or nullptr
/// when the font has no base encoding (a Type 3 font with only /Differences).
const char** baseEncodingTable(fz_context* ctx, pdf_obj* encoding,
                               bool isType3) {
    const char* named = nullptr;
    if (pdf_is_name(ctx, encoding))
        named = pdf_to_name(ctx, encoding);
    else if (pdf_is_dict(ctx, encoding))
        named = pdf_to_name(ctx, pdf_dict_get(ctx, encoding, PDF_NAME(BaseEncoding)));

    if (named && named[0]) {
        if (!std::string("WinAnsiEncoding").compare(named))
            return fz_glyph_name_from_win_ansi;
        if (!std::string("MacRomanEncoding").compare(named))
            return fz_glyph_name_from_mac_roman;
        if (!std::string("MacExpertEncoding").compare(named))
            return fz_glyph_name_from_mac_expert;
        return fz_glyph_name_from_adobe_standard;
    }
    // No /BaseEncoding. A simple font's implicit encoding is StandardEncoding;
    // a Type 3 font has NO implicit base encoding at all (PDF 32000-1 9.6.5.4),
    // so guessing one there would invent mappings the file never declared.
    return isType3 ? nullptr : fz_glyph_name_from_adobe_standard;
}

/// True for the simple (single-byte, glyph-name-encoded) font subtypes. Type0
/// composites are excluded: their CIDs carry no glyph names, so nothing in this
/// file can say anything about them.
bool isSimpleFontSubtype(const char* subtype) {
    if (!subtype) return false;
    const std::string s(subtype);
    return s == "Type1" || s == "MMType1" || s == "TrueType" || s == "Type3";
}

/// Fill *plan* for one font dictionary. Returns false when the font is out of
/// scope or nothing can be improved.
bool planFont(fz_context* ctx, pdf_obj* fontRef, FontPlan& plan) {
    pdf_obj* font = pdf_resolve_indirect(ctx, fontRef);
    if (!font || !pdf_is_dict(ctx, font)) return false;
    // Already has an authored /ToUnicode: the file told us the answer and we
    // must not second-guess it.
    if (pdf_dict_get(ctx, font, PDF_NAME(ToUnicode))) return false;

    const char* subtype = pdf_to_name(ctx, pdf_dict_get(ctx, font, PDF_NAME(Subtype)));
    if (!isSimpleFontSubtype(subtype)) return false;
    const bool isType3 = std::string("Type3") == subtype;

    pdf_obj* encoding = pdf_resolve_indirect(ctx, pdf_dict_get(ctx, font, PDF_NAME(Encoding)));
    const char** base = baseEncodingTable(ctx, encoding, isType3);

    // Gather the effective glyph name per code as RAW POINTERS into MuPDF's own
    // storage — no owning C++ object is built while pdf_* calls can longjmp.
    const char* names[256] = {nullptr};
    for (int i = 0; i < 256; ++i)
        names[i] = base ? base[i] : nullptr;

    if (pdf_is_dict(ctx, encoding)) {
        pdf_obj* diffs = pdf_resolve_indirect(
            ctx, pdf_dict_get(ctx, encoding, PDF_NAME(Differences)));
        if (pdf_is_array(ctx, diffs)) {
            const int n = pdf_array_len(ctx, diffs);
            int code = -1;
            for (int i = 0; i < n; ++i) {
                pdf_obj* item = pdf_array_get(ctx, diffs, i);
                if (pdf_is_number(ctx, item)) {
                    code = pdf_to_int(ctx, item);
                } else if (pdf_is_name(ctx, item) && code >= 0 && code < 256) {
                    names[code] = pdf_to_name(ctx, item);
                    ++code;
                }
            }
        }
    }

    plan.fontRef = fontRef;
    plan.fontDict = font;
    plan.cp.fill(0);
    plan.improved = false;

    for (int code = 0; code < 256; ++code) {
        const char* name = names[code];
        // What MuPDF will do unaided: resolve the glyph name non-strictly, and
        // if that yields nothing, pass the raw character code through as if it
        // were Unicode.
        int mupdfCp = name ? fz_unicode_from_glyph_name(name) : 0;
        if (mupdfCp <= 0) mupdfCp = code;

        if (!isUnusableExtractedCodepoint(mupdfCp)) {
            plan.cp[static_cast<size_t>(code)] = mupdfCp;
            continue;
        }

        // MuPDF's answer is unusable. Try PDFE's extended glyph-name rules, then
        // fall back to interpreting the CODE through StandardEncoding (the
        // implicit encoding a simple font is defined to have).
        int recovered = 0;
        if (name) {
            const std::string owned(name);
            recovered = recoverUnicodeFromGlyphName(owned);
        }
        if (!recovered) {
            const char* stdName = fz_glyph_name_from_adobe_standard[code];
            if (stdName && stdName[0]) {
                const std::string owned(stdName);
                recovered = recoverUnicodeFromGlyphName(owned);
            }
        }
        if (recovered && !isUnusableExtractedCodepoint(recovered)) {
            plan.cp[static_cast<size_t>(code)] = recovered;
            plan.improved = true;
        }
    }

    return plan.improved;
}

/// Recursively plan every font reachable from a resources dict (one level per
/// Form XObject, cycle-guarded), mirroring FontNameUtil.cpp's established walk.
void collectPlans(fz_context* ctx, pdf_obj* resources,
                  std::vector<FontPlan>& out, std::set<pdf_obj*>& visited,
                  int depth) {
    if (!resources || depth > 8) return;
    pdf_obj* res = pdf_resolve_indirect(ctx, resources);
    if (!res || visited.count(res)) return;
    visited.insert(res);

    pdf_obj* fonts = pdf_dict_get(ctx, res, PDF_NAME(Font));
    if (fonts && pdf_is_dict(ctx, fonts)) {
        const int n = pdf_dict_len(ctx, fonts);
        for (int i = 0; i < n; ++i) {
            FontPlan plan;
            if (planFont(ctx, pdf_dict_get_val(ctx, fonts, i), plan))
                out.push_back(plan);
        }
    }

    pdf_obj* xobjs = pdf_dict_get(ctx, res, PDF_NAME(XObject));
    if (xobjs && pdf_is_dict(ctx, xobjs)) {
        const int n = pdf_dict_len(ctx, xobjs);
        for (int i = 0; i < n; ++i) {
            pdf_obj* xo = pdf_resolve_indirect(ctx, pdf_dict_get_val(ctx, xobjs, i));
            if (!xo) continue;
            pdf_obj* xres = pdf_dict_get(ctx, xo, PDF_NAME(Resources));
            if (xres) collectPlans(ctx, xres, out, visited, depth + 1);
        }
    }
}

/// Render one plan as a PostScript ToUnicode CMap. Pure string work, done with
/// no MuPDF call in flight.
std::string buildCMap(const FontPlan& plan) {
    std::vector<std::pair<int, int>> entries;
    for (int code = 0; code < 256; ++code) {
        const int cp = plan.cp[static_cast<size_t>(code)];
        // Only BMP scalars are expressible as a single UTF-16BE code unit; the
        // recovery rules above never produce anything else, and a surrogate
        // would be a bug rather than a mapping, so both are excluded.
        if (cp > 0 && cp <= 0xFFFF && !(cp >= 0xD800 && cp <= 0xDFFF))
            entries.emplace_back(code, cp);
    }
    if (entries.empty()) return std::string();

    std::string s;
    s += "/CIDInit /ProcSet findresource begin\n";
    s += "12 dict begin\nbegincmap\n";
    s += "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n";
    s += "/CMapName /PDFE-Glyphname-Recovery-UCS def\n";
    s += "/CMapType 2 def\n";
    s += "1 begincodespacerange\n<00> <FF>\nendcodespacerange\n";

    char buf[64];
    for (size_t i = 0; i < entries.size(); i += 100) {
        const size_t n = std::min<size_t>(100, entries.size() - i);
        std::snprintf(buf, sizeof(buf), "%zu beginbfchar\n", n);
        s += buf;
        for (size_t k = 0; k < n; ++k) {
            std::snprintf(buf, sizeof(buf), "<%02X> <%04X>\n",
                          entries[i + k].first, entries[i + k].second);
            s += buf;
        }
        s += "endbfchar\n";
    }
    s += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
    return s;
}

}  // namespace

int recoverMissingToUnicode(struct fz_context* ctx, struct fz_page* page) {
    if (!ctx || !page) return 0;

    // Declared before any fz_try so a longjmp cannot skip their destructors.
    std::vector<FontPlan> plans;
    std::set<pdf_obj*> visited;
    std::vector<std::string> cmaps;
    pdf_document* doc = nullptr;

    fz_try(ctx) {
        pdf_page* ppage = pdf_page_from_fz_page(ctx, page);
        if (ppage && ppage->obj) {
            doc = ppage->doc;
            collectPlans(ctx, pdf_dict_get(ctx, ppage->obj, PDF_NAME(Resources)),
                         plans, visited, 0);
        }
    }
    fz_catch(ctx) {
        // Best-effort: a document that cannot be walked simply gets no recovery.
        return 0;
    }

    if (!doc || plans.empty()) return 0;

    // String building happens with NO MuPDF call in flight.
    cmaps.reserve(plans.size());
    for (const FontPlan& plan : plans)
        cmaps.push_back(buildCMap(plan));

    int patched = 0;
    for (size_t i = 0; i < plans.size(); ++i) {
        if (cmaps[i].empty()) continue;
        // Each font is installed in its own guard so one malformed font cannot
        // cost the others their recovery.
        const char* text = cmaps[i].c_str();
        const size_t len = cmaps[i].size();
        pdf_obj* fontDict = plans[i].fontDict;
        pdf_obj* fontRef = plans[i].fontRef;
        fz_buffer* buf = nullptr;
        int ok = 0;
        fz_try(ctx) {
            buf = fz_new_buffer_from_copied_data(
                ctx, reinterpret_cast<const unsigned char*>(text), len);
            // A NULL dict argument makes pdf_add_stream create the stream's own
            // (indirect) dictionary, so there is no second object to own here.
            pdf_obj* streamRef = pdf_add_stream(ctx, doc, buf, nullptr, 0);
            // GITHUB #188 — THIS LINE MUTATES THE DOCUMENT, DELIBERATELY.
            //
            // It writes into the in-memory object graph, not into an
            // extraction-time view, so a user who opens a qualifying document
            // and later saves it gets a file whose fonts carry a /ToUnicode the
            // input lacked. That is a STATED product behaviour, not an
            // oversight: the alternative (an extraction-only side table) would
            // have to be consulted at all eleven fz_new_stext_page_from_page
            // sites — two of them the redaction scrubber and the redaction
            // VERIFIER, where a forgotten consult silently recreates the A3-48
            // false negative this recovery exists to remove — and fz_stext_char
            // carries no source byte code to key such a table on.
            //
            // The full trade-off, and the triggers that would reopen it, are in
            // docs/tounicode-recovery-write-side-decision.md. The bounds that
            // make it defensible are enforced by
            // tests/test_tounicode_recovery_write_side.cpp: an authored
            // /ToUnicode is never replaced (planFont's early return above), a
            // font carrying no Unicode information gains nothing (plan.improved
            // gates entry to this loop), and opening never marks the document
            // modified — so this can only reach disk inside a save the user
            // asked for. Change any of that and change the document with it.
            pdf_dict_put_drop(ctx, fontDict, PDF_NAME(ToUnicode), streamRef);
            // The font may already sit in the resource store from an earlier
            // render of this page, in which case the CACHED pdf_font_desc still
            // has no to_unicode. Purging forces a reload that sees the CMap, so
            // recovery does not depend on extraction happening before the first
            // paint.
            if (pdf_is_indirect(ctx, fontRef))
                pdf_purge_object_from_store(ctx, doc, pdf_to_num(ctx, fontRef));
            ok = 1;
        }
        fz_always(ctx) {
            fz_drop_buffer(ctx, buf);
        }
        fz_catch(ctx) {
            ok = 0;
        }
        patched += ok;
    }
    return patched;
}

}  // namespace pdfe::core
