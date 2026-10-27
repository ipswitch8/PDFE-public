#include "tre/AutoMissingGlyphResolver.h"

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/FontAnalysisCache.h"
#include "fonts/FontFamilyResolver.h"
#include "fonts/SubstituteFontSelector.h"
#include "fonts/VectorSubsetDetector.h"
#include "tre/EnrichedAtlas.h"
#include "tre/GlyphSynthesizer.h"
#include "tre/SynthesizedType3FontBundler.h"
#include "tre/VectorEnrichedAtlasBuilder.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <QDebug>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <optional>
#include <set>
#include <vector>

namespace pdfe::tre {

namespace {

/// Strip the PDF subset prefix (six uppercase letters + '+') from a
/// font name to yield the underlying family name. "AAAAAA+Alef" → "Alef".
std::string stripSubsetPrefix(const std::string& fontName) {
    if (pdfe::fonts::VectorSubsetDetector::hasSubsetPrefix(fontName))
        return fontName.substr(7);
    return fontName;
}

/// Parse a PDF base font name into a system-resolvable family plus
/// bold/italic flags. PDF base names encode style as a suffix after the
/// family — e.g. "Arial,Bold", "Arial-BoldItalic", "TimesNewRoman,Italic".
/// Without this, the substitute font for a stripped glyph would resolve to
/// the regular weight (D8-1: editing a bold "Invoice" heading substituted
/// non-bold Arial, so the replacement glyphs rendered lighter than the
/// surrounding word). Returns the bare family (style descriptors removed).
struct ParsedFontStyle {
    std::string family;
    bool bold = false;
    bool italic = false;
};

ParsedFontStyle parseFontStyle(const std::string& baseFontName,
                               bool descriptorItalic = false,
                               bool descriptorBold = false) {
    std::string name = stripSubsetPrefix(baseFontName);
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    ParsedFontStyle out;
    out.bold = lower.find("bold") != std::string::npos;
    out.italic = lower.find("italic") != std::string::npos ||
                 lower.find("oblique") != std::string::npos;

    // The family is everything before the first style separator (',' or '-')
    // that introduces a recognized style word, falling back to the whole
    // name when there is no style suffix.
    std::string family = name;
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] == ',' || name[i] == '-') {
            std::string rest = lower.substr(i + 1);
            if (rest.find("bold") != std::string::npos ||
                rest.find("italic") != std::string::npos ||
                rest.find("oblique") != std::string::npos ||
                rest.find("regular") != std::string::npos) {
                family = name.substr(0, i);
                break;
            }
        }
    }
    out.family = family;

    // Descriptor-sourced flags (name-INDEPENDENT, from /FontDescriptor) take
    // precedence over the name-substring scan when they carry a positive signal
    // — this is what lets the new-character fallback match a style the font name
    // does not spell out. Both-false is ambiguous (possibly no descriptor), so
    // the name-parse result is kept in that case. Family is always name-derived
    // (the descriptor does not name a family).
    if (descriptorItalic || descriptorBold) {
        out.italic = descriptorItalic;
        out.bold = descriptorBold;
    }
    return out;
}

/// Heuristic serif classification from a font name. Used to pick a serif vs
/// sans-serif universal fallback when the requested family can't be resolved,
/// so a serif source (e.g. Palatino) does not get a sans replacement glyph.
/// Mirrors the serif-fragment list in TextEditTool::buildOverlayFont so the
/// overlay and the inserted-glyph paths classify serif-ness identically.
bool looksSerifFontName(const std::string& fontName) {
    std::string lower = stripSubsetPrefix(fontName);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const char* kSerifFragments[] = {
        "serif", "times", "roman", "garamond", "caslon", "baskerville",
        "palatino", "georgia", "cambria", "charis", "doulos", "gentium",
        "minion", "goudy", "century", "book antiqua", "palladio"};
    for (const char* frag : kSerifFragments) {
        if (lower.find(frag) != std::string::npos) return true;
    }
    return false;
}

} // namespace

/// Find the page's embedded font matching @p targetBaseFont and return
/// its raw bytes (FontFile2/FontFile3/FontFile stream contents). Walks
/// the top-level /Font dict and the descendant /CIDFont if the match
/// is on a Type0 wrapper. Returns an empty vector on miss.
std::vector<unsigned char> AutoMissingGlyphResolver::extractEmbeddedFontBytes(
    pdfe::core::MuPdfContext* context,
    pdfe::core::Page* page,
    pdfe::core::Document* document,
    const std::string& targetBaseFont)
{
    std::vector<unsigned char> bytes;
    if (!context || !page || !document)
        return bytes;

    try {
        context->execute([&](fz_context* ctx) {
            fz_page* fz_pg = page->fzPage();
            if (!fz_pg) return;
            pdf_page* pdf_pg = pdf_page_from_fz_page(ctx, fz_pg);
            if (!pdf_pg) return;
            pdf_obj* resources = pdf_page_resources(ctx, pdf_pg);
            if (!resources) return;
            pdf_obj* fontDict = pdf_dict_get(ctx, resources, PDF_NAME(Font));
            if (!fontDict) return;

            const int n = pdf_dict_len(ctx, fontDict);
            for (int i = 0; i < n; ++i) {
                pdf_obj* fontRef = pdf_dict_get_val(ctx, fontDict, i);
                if (!fontRef) continue;
                fontRef = pdf_resolve_indirect(ctx, fontRef);
                if (!fontRef) continue;

                bool matches = false;
                pdf_obj* baseFont = pdf_dict_get(ctx, fontRef, PDF_NAME(BaseFont));
                if (baseFont) {
                    const char* nm = pdf_to_name(ctx, baseFont);
                    if (nm && std::strcmp(nm, targetBaseFont.c_str()) == 0)
                        matches = true;
                }

                pdf_obj* fontDescriptor =
                    pdf_dict_get(ctx, fontRef, PDF_NAME(FontDescriptor));
                if (!fontDescriptor) {
                    // Type0 wrapper — descend into DescendantFonts[0]
                    pdf_obj* desc = pdf_dict_get(ctx, fontRef, PDF_NAME(DescendantFonts));
                    if (desc && pdf_array_len(ctx, desc) > 0) {
                        pdf_obj* cid = pdf_resolve_indirect(
                            ctx, pdf_array_get(ctx, desc, 0));
                        if (cid) {
                            pdf_obj* cidBase = pdf_dict_get(ctx, cid, PDF_NAME(BaseFont));
                            if (!matches && cidBase) {
                                const char* nm = pdf_to_name(ctx, cidBase);
                                if (nm && std::strcmp(nm, targetBaseFont.c_str()) == 0)
                                    matches = true;
                            }
                            fontDescriptor = pdf_dict_get(ctx, cid,
                                PDF_NAME(FontDescriptor));
                        }
                    }
                }

                if (!matches || !fontDescriptor)
                    continue;

                pdf_obj* fontFile =
                    pdf_dict_get(ctx, fontDescriptor, PDF_NAME(FontFile2));
                if (!fontFile)
                    fontFile = pdf_dict_get(ctx, fontDescriptor, PDF_NAME(FontFile3));
                if (!fontFile)
                    fontFile = pdf_dict_get(ctx, fontDescriptor, PDF_NAME(FontFile));
                if (!fontFile)
                    return;

                fz_buffer* buf = pdf_load_stream(ctx, fontFile);
                if (!buf)
                    return;
                unsigned char* data = nullptr;
                size_t len = fz_buffer_storage(ctx, buf, &data);
                if (data && len > 0)
                    bytes.assign(data, data + len);
                fz_drop_buffer(ctx, buf);
                return;  // First match wins
            }
        });
    } catch (...) {
        bytes.clear();
    }
    return bytes;
}

namespace {

/// True when @p face has no usable outline for glyph @p gid. A subset font
/// commonly keeps a codepoint's cmap entry while STRIPPING the glyph's
/// outline (the subsetter only embeds outlines for glyphs the original text
/// used). FreeType still reports a non-zero glyph index for such a stripped
/// glyph, so a cmap-only test (FT_Get_Char_Index != 0) wrongly classifies it
/// as present — and the edited text renders the empty glyph as a blank
/// .notdef. Loading the glyph and checking the contour count distinguishes a
/// real outline from a stripped one. (TrueType `glyf` and CFF both populate
/// `outline.n_contours`; a genuinely blank glyph such as space yields 0, but
/// callers skip space separately, so an in-text visible codepoint resolving
/// to 0 contours is a stripped/absent glyph we must substitute.)
bool glyphOutlineIsEmpty(FT_Face face, FT_UInt gid) {
    if (FT_Load_Glyph(face, gid, FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP) != 0)
        return true;  // can't load — treat as absent so we substitute
    return face->glyph->outline.n_contours <= 0;
}

/// Test which codepoints in @p text the source font can actually RENDER.
/// Returns the unrenderable subset (deduped, in input order). A codepoint is
/// unrenderable when the font's cmap has no glyph for it (FT_Get_Char_Index
/// == 0) OR the mapped glyph's outline was stripped by subsetting
/// (see glyphOutlineIsEmpty). Skips ASCII space because PDFs normally space
/// via TJ adjustments rather than rendering a glyph.
std::vector<char32_t> findUnencodable(
    FT_Face face, const std::u32string& text)
{
    std::vector<char32_t> miss;
    std::set<char32_t> seen;
    for (char32_t cp : text) {
        if (cp == U' ' || cp == 0)
            continue;
        if (!seen.insert(cp).second)
            continue;
        FT_UInt gid = FT_Get_Char_Index(face, static_cast<FT_ULong>(cp));
        if (gid == 0 || glyphOutlineIsEmpty(face, gid))
            miss.push_back(cp);
    }
    return miss;
}

/// Walk the unencodable codepoint set in input order and produce
/// `FormattingRun` entries that group consecutive same-font ranges.
/// Each FormattingRun covers a contiguous range of indices in @p text
/// that share the same destination font (Type 3 or system).
std::vector<pdfe::textedit::FormattingRun> buildRunsFromMissingMap(
    const std::u32string& text,
    const std::unordered_map<char32_t, std::string>& cpToType3Name,
    const std::unordered_map<char32_t, std::string>& cpToSystemPath,
    float baseSizePt)
{
    std::vector<pdfe::textedit::FormattingRun> runs;
    const int n = static_cast<int>(text.size());
    int i = 0;
    while (i < n) {
        char32_t cp = text[i];
        auto t3It = cpToType3Name.find(cp);
        auto sysIt = cpToSystemPath.find(cp);
        if (t3It == cpToType3Name.end() && sysIt == cpToSystemPath.end()) {
            ++i;
            continue;
        }
        // Start of a missing run. Extend while subsequent indices share
        // the same destination font.
        const bool isType3 = (t3It != cpToType3Name.end());
        const std::string& destName = isType3 ? t3It->second : sysIt->second;
        int j = i + 1;
        while (j < n) {
            char32_t cj = text[j];
            auto t3J = cpToType3Name.find(cj);
            auto sysJ = cpToSystemPath.find(cj);
            const bool jIsType3 = (t3J != cpToType3Name.end());
            const std::string& jDest =
                jIsType3 ? t3J->second : (sysJ != cpToSystemPath.end()
                                              ? sysJ->second : std::string());
            if (jDest.empty()) break;
            if (jIsType3 != isType3) break;
            if (jDest != destName) break;
            ++j;
        }
        pdfe::textedit::FormattingRun run;
        run.charStart = i;
        run.charEnd = j;
        run.fontSize = baseSizePt;
        if (isType3) {
            run.preInstalledResName = destName;
        } else {
            run.fontFilePath = destName;
        }
        runs.push_back(std::move(run));
        i = j;
    }
    return runs;
}

} // namespace

AutoMissingGlyphResolver::ResolvedStyle AutoMissingGlyphResolver::parseStyle(
    const std::string& baseFontName, bool descriptorItalic, bool descriptorBold)
{
    const ParsedFontStyle s =
        parseFontStyle(baseFontName, descriptorItalic, descriptorBold);
    return ResolvedStyle{s.family, s.bold, s.italic};
}

std::vector<pdfe::textedit::FormattingRun> AutoMissingGlyphResolver::resolve(
    pdfe::core::MuPdfContext* context,
    pdfe::core::Page* page,
    pdfe::core::Document* document,
    const std::string& sourceFontName,
    float sourceFontSizePt,
    const std::u32string& text,
    bool descriptorItalic,
    bool descriptorBold)
{
    std::vector<pdfe::textedit::FormattingRun> runs;
    if (!context || !page || !document || sourceFontName.empty() || text.empty())
        return runs;

    // Step 1: extract source font bytes from the page.
    auto bytes = AutoMissingGlyphResolver::extractEmbeddedFontBytes(
        context, page, document, sourceFontName);
    if (bytes.empty()) {
        // phase-75: the source font is NOT embedded — one of the standard-14
        // names (Helvetica, Times-Roman, …) that a PDF may reference without
        // carrying a font program. Every step below needs an FT_Face built
        // from those bytes, so the function used to return here with no runs
        // at all, and that early return is where CJK editing died for the
        // commonest kind of document in existence: nothing was routed, the
        // save path fell through to the standard font, and ContentStreamEditor
        // wrote '?' for every codepoint.
        //
        // Without the source face we cannot ask "which codepoints does THIS
        // font lack". We can still answer the question that matters here,
        // because a standard-14 font is a simple single-byte font by
        // definition: everything at or above U+0100 is unrepresentable in it,
        // full stop. Route exactly that set and leave everything below it on
        // the existing path, so no Latin-1 behaviour changes.
        std::vector<char32_t> nonLatin1;
        for (char32_t cp : text) {
            if (cp >= 256
                && std::find(nonLatin1.begin(), nonLatin1.end(), cp)
                       == nonLatin1.end()) {
                nonLatin1.push_back(cp);
            }
        }
        if (nonLatin1.empty())
            return runs;

        std::u32string needed(nonLatin1.begin(), nonLatin1.end());
        pdfe::fonts::EmbeddedFontAnalyzer analyzer(context);
        static pdfe::fonts::FontAnalysisCache stdCache;
        pdfe::fonts::SubstituteFontSelector selector(&analyzer, &stdCache);
        // No profile exists to rank against, so use the coverage-filtered
        // candidate pool directly and pick on serif-ness, which is the one
        // property of a standard-14 name that is reliable.
        auto candidates = selector.findAllNonDecorativeCandidates(needed);
        if (candidates.empty()) {
            qWarning() << "[AutoMissingGlyphResolver] no installed font covers"
                       << static_cast<int>(nonLatin1.size())
                       << "non-Latin-1 codepoint(s) typed into non-embedded"
                       << QString::fromStdString(sourceFontName);
            return runs;
        }
        const bool srcSerif = looksSerifFontName(sourceFontName);
        const std::string* pick = &candidates.front().filePath;
        for (const auto& c : candidates) {
            if (c.profile.isSerif == srcSerif) { pick = &c.filePath; break; }
        }

        std::unordered_map<char32_t, std::string> noType3;
        std::unordered_map<char32_t, std::string> cpToPath;
        for (char32_t cp : nonLatin1)
            cpToPath[cp] = *pick;
        return buildRunsFromMissingMap(text, noType3, cpToPath,
                                       sourceFontSizePt);
    }

    // Step 2: load FT_Face on a private FT_Library.
    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib) != 0)
        return runs;
    FT_Face face = nullptr;
    if (FT_New_Memory_Face(lib, bytes.data(),
                           static_cast<FT_Long>(bytes.size()),
                           0, &face) != 0 || !face) {
        FT_Done_FreeType(lib);
        return runs;
    }

    // Step 3: find unencodable codepoints.
    std::vector<char32_t> missing = findUnencodable(face, text);
    if (missing.empty()) {
        FT_Done_Face(face);
        FT_Done_FreeType(lib);
        return runs;
    }

    // Step 4: partition missing codepoints by WHY they are missing, because
    // the Type 3 atlas path can only help one of the two reasons:
    //   * absent (gid == 0): the font has no glyph for the codepoint at all.
    //     A synthesis recipe may still compose one from OTHER present glyphs
    //     (e.g. an accented letter from base + diacritic), so these are
    //     eligible for the EnrichedAtlas / Type 3 path.
    //   * stripped (gid != 0 but the outline was removed by subsetting): the
    //     cmap entry survived but the glyph is empty. The atlas would
    //     rasterize this font's OWN blank glyph and emit an invisible Type 3
    //     glyph (D8-1: the 'u' in an edited "Invoice"→"Quote" heading rendered
    //     nothing). Type 3 synthesis from this font cannot recover a stripped
    //     outline, so route stripped glyphs straight to the system-font
    //     fallback, which has the real outline.
    std::vector<char32_t> absentCps;
    std::vector<char32_t> strippedCps;
    for (char32_t cp : missing) {
        FT_UInt gid = FT_Get_Char_Index(face, static_cast<FT_ULong>(cp));
        if (gid != 0 && glyphOutlineIsEmpty(face, gid))
            strippedCps.push_back(cp);
        else
            absentCps.push_back(cp);
    }

    // Building the EnrichedAtlas rasterizes every glyph in the source font's
    // cmap (potentially thousands for fonts like Arial) — ~1 s of work. Only
    // build it when an ABSENT codepoint has a synthesis recipe. The recipe
    // table is Latin-only today; non-Latin / stripped codepoints take the
    // system-font fast path.
    bool anyRecipe = false;
    for (char32_t cp : absentCps) {
        if (GlyphSynthesizer::findRecipe(cp) != nullptr) {
            anyRecipe = true;
            break;
        }
    }

    std::vector<char32_t> type3Cps;
    std::vector<char32_t> sysCps = strippedCps;  // stripped → system always
    std::unique_ptr<EnrichedAtlas> enrichedAtlas;

    if (anyRecipe) {
        const int targetHeightPx = 32;
        auto enrichedRes = VectorEnrichedAtlasBuilder::build(
            face, targetHeightPx, text);
        enrichedAtlas = std::move(enrichedRes.atlas);

        if (enrichedAtlas) {
            for (char32_t cp : absentCps) {
                if (enrichedAtlas->contains(cp))
                    type3Cps.push_back(cp);
                else
                    sysCps.push_back(cp);
            }
        } else {
            for (char32_t cp : absentCps)
                sysCps.push_back(cp);
        }
    } else {
        for (char32_t cp : absentCps)
            sysCps.push_back(cp);
    }

    // Serif classification from the source font's EmbeddedFontAnalyzer
    // FontMetricProfile (profile.isSerif: PANOSE byte 1 / measured serif
    // extent — name-INDEPENDENT). Computed here while the source FT_Face is
    // still alive; preferred over the looksSerifFontName name-fragment
    // heuristic for the system-font fallback below, so a serif source whose
    // /BaseFont carries no recognisable keyword still gets a serif substitute
    // (and a sans source whose name happens to contain one does not). -1 when
    // the profile couldn't be built — then the name heuristic is used.
    // (pipeline d4e7f921 phase 3)
    //
    // phase-75: the WHOLE profile is kept, not just isSerif. The CJK fallback
    // below hands it to SubstituteFontSelector, which needs the metric profile
    // to rank candidates; building it twice would mean re-opening and
    // re-analysing the source face after it has already been closed.
    int profileSerif = -1;
    std::optional<pdfe::fonts::FontMetricProfile> sourceProfile;
    {
        pdfe::fonts::EmbeddedFontAnalyzer analyzer(context);
        if (auto prof = analyzer.analyzeFace(face, sourceFontName)) {
            profileSerif = prof->isSerif ? 1 : 0;
            sourceProfile = std::move(prof);
        }
    }

    FT_Done_Face(face);
    FT_Done_FreeType(lib);

    std::unordered_map<char32_t, std::string> cpToType3Name;
    std::unordered_map<char32_t, std::string> cpToSystemPath;

    // Step 6a: bundle Type 3 if any synthesizable codepoints.
    if (!type3Cps.empty() && enrichedAtlas) {
        auto bundle = SynthesizedType3FontBundler::build(
            context, page, document, *enrichedAtlas, type3Cps);
        if (bundle.fontIndirectRef && !bundle.resourceName.empty()) {
            for (char32_t cp : bundle.codepointsBundled) {
                cpToType3Name[cp] = bundle.resourceName;
            }
            // Anything in type3Cps that wasn't in bundle.codepointsBundled
            // (unexpected — bundler skipped it for some reason) falls
            // back to system fonts.
            for (char32_t cp : type3Cps) {
                if (cpToType3Name.count(cp) == 0)
                    sysCps.push_back(cp);
            }
        } else {
            // Bundling failed entirely — fall back everything to system fonts.
            for (char32_t cp : type3Cps)
                sysCps.push_back(cp);
        }
    }

    // Step 6b: resolve system fonts for the rest. Pick a metric-
    // compatible TTF/OTF (or Arial as a universally-available
    // Windows fallback), then VERIFY each codepoint is actually in
    // that font's cmap before assigning. If the picked font doesn't
    // encode a codepoint, the codepoint is dropped from the run map
    // — the saved content stream falls through to the original
    // font, which renders .notdef. Better .notdef than blindly
    // routing to a font that also can't render the glyph (and would
    // render its own .notdef).
    //
    // The resolver is process-static so its system-font enumeration
    // cache (≈300 ms on first call) doesn't get rebuilt on every
    // keystroke. The static is constructed on first use; subsequent
    // calls hit the in-memory cache.
    if (!sysCps.empty()) {
        static pdfe::fonts::FontFamilyResolver resolver;
        ParsedFontStyle style =
            parseFontStyle(sourceFontName, descriptorItalic, descriptorBold);
        std::string sysPath =
            resolver.resolve(style.family, style.bold, style.italic);
        if (sysPath.empty()) {
            // The requested family is not installed. Fall back to a
            // universally-available system font, but MATCH serif-ness so a
            // serif source (e.g. Palatino) does not render its replacement
            // glyphs in a sans-serif face. "Times New Roman" carries free
            // metric-compatible substitutes (Liberation Serif, Tinos, …) so it
            // also resolves on Linux/CI; Arial does the same for the sans case.
            const bool srcSerif = (profileSerif >= 0)
                                      ? (profileSerif == 1)
                                      : looksSerifFontName(sourceFontName);
            const char* fallbackFamily =
                srcSerif ? "Times New Roman" : "Arial";
            sysPath = resolver.resolve(fallbackFamily, style.bold, style.italic);
            if (sysPath.empty())
                sysPath = resolver.resolve("Arial", style.bold, style.italic);
            qDebug() << "[AutoMissingGlyphResolver] family"
                     << QString::fromStdString(style.family)
                     << "not installed; serif=" << srcSerif
                     << "(profileSerif=" << profileSerif << ")"
                     << "fallback=" << fallbackFamily
                     << "bold=" << style.bold << "italic=" << style.italic
                     << "-> path=" << QString::fromStdString(sysPath);
        }
        if (!sysPath.empty()) {
            // Open the candidate and check codepoint coverage.
            FT_Library sysLib = nullptr;
            if (FT_Init_FreeType(&sysLib) == 0) {
                FT_Face sysFace = nullptr;
                if (FT_New_Face(sysLib, sysPath.c_str(), 0, &sysFace) == 0
                    && sysFace) {
                    for (char32_t cp : sysCps) {
                        if (FT_Get_Char_Index(
                                sysFace,
                                static_cast<FT_ULong>(cp)) != 0) {
                            cpToSystemPath[cp] = sysPath;
                        }
                    }
                    FT_Done_Face(sysFace);
                }
                FT_Done_FreeType(sysLib);
            }
        }
        // ─────────────────────────────────────────────────────────────────
        // phase-75 (WIN-49 prerequisite): the script-aware fallback.
        //
        // Everything above resolves ONE family — the source font's own, or
        // Times New Roman / Arial when that family is not installed. Those
        // are Latin faces, so for CJK (or Greek, Cyrillic, Hebrew, Thai …)
        // the FT_Get_Char_Index coverage check above assigns nothing, every
        // codepoint drops out of the run map, and the save path falls back
        // to the original font and writes .notdef. That is the silent CJK
        // data loss this phase exists to remove: the user types 日本語, the
        // edit reports success, and the saved PDF contains nothing readable.
        //
        // SubstituteFontSelector already knows how to do this correctly. Its
        // scoring pool only excludes non-Latin-primary families when the
        // requested text is itself Latin-only (SubstituteFontSelector.cpp
        // textIsLatinOnly / isNonLatinPrimaryFamily), and every candidate
        // must pass findMissingGlyphs coverage — so asking it for 日本語
        // yields a face that actually has those glyphs. It was reachable
        // only from FontSubstitutionEngine::processEdits, which has no
        // production caller; this is the wiring that makes it reachable from
        // the path editing actually takes.
        //
        // It runs ONLY on the codepoints the Latin path could not cover, so
        // Latin editing does not pay for the system-font enumeration and the
        // existing substitution behaviour is untouched.
        std::vector<char32_t> uncovered;
        for (char32_t cp : sysCps) {
            if (cpToSystemPath.count(cp) == 0)
                uncovered.push_back(cp);
        }
        if (!uncovered.empty() && sourceProfile.has_value()) {
            std::u32string needed(uncovered.begin(), uncovered.end());
            pdfe::fonts::EmbeddedFontAnalyzer subAnalyzer(context);
            // Process-static so the system-font enumeration and per-face
            // metric analysis are paid once, not once per keystroke.
            static pdfe::fonts::FontAnalysisCache subCache;
            pdfe::fonts::SubstituteFontSelector selector(&subAnalyzer, &subCache);
            auto sub = selector.findSubstitute(*sourceProfile, needed);
            if (sub.has_value() && !sub->filePath.empty()) {
                // Assigned WITHOUT a second per-codepoint FreeType check.
                // findSubstitute rejects any candidate that fails
                // findMissingGlyphs, so re-checking here can never change the
                // outcome — a re-check no input can make fail is decoration,
                // and decoration is indistinguishable from a working guard
                // until the day it matters. The promise it would have been
                // re-checking is instead pinned as a test
                // (SubstituteSelectorOnlyReturnsCoveringFonts), so a change to
                // the selector's filtering fails there rather than silently
                // reverting CJK to .notdef here.
                for (char32_t cp : uncovered)
                    cpToSystemPath[cp] = sub->filePath;
                qDebug() << "[AutoMissingGlyphResolver] script-aware fallback for"
                         << static_cast<int>(uncovered.size())
                         << "codepoint(s) the Latin fallback could not cover ->"
                         << QString::fromStdString(sub->filePath);
            } else {
                qWarning() << "[AutoMissingGlyphResolver] no installed font "
                              "covers" << static_cast<int>(uncovered.size())
                           << "requested codepoint(s); they will render as "
                              ".notdef in the original font";
            }
        }

        if (cpToSystemPath.empty()) {
            qWarning() << "[AutoMissingGlyphResolver] no system font encodes "
                          "the requested codepoints; they will render as "
                          ".notdef in the original font";
        }
    }

    // Step 7: collapse adjacent same-destination indices into runs.
    runs = buildRunsFromMissingMap(
        text, cpToType3Name, cpToSystemPath, sourceFontSizePt);

    return runs;
}

} // namespace pdfe::tre
