#include <gtest/gtest.h>
#include "fonts/FontSubstitutionEngine.h"
#include "textedit/TextEditModel.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// MuPDF is already visible via core/MuPdfContext.h (fitz.h + pdf.h, each with
// its own extern "C" guard). The read-back oracle below uses it to open the
// SAVED bytes with an INDEPENDENT handle, because Document::fzDocument() is
// friend-private to the test.

using namespace pdfe::fonts;
using namespace pdfe::textedit;
using namespace pdfe::core;

class FontSubstitutionEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();
        engine_ = std::make_unique<FontSubstitutionEngine>(context_.get());

        std::filesystem::path testPdf = std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
        if (!std::filesystem::exists(testPdf)) {
            hasTestPdf_ = false;
            return;
        }
        doc_ = Document::open(context_, testPdf);
        if (doc_ && doc_->pageCount() > 0) {
            page_ = doc_->page(0);
            hasTestPdf_ = true;
        }
    }

    // Helper to create minimal spans
    TextSpan makeSpan(const std::string& fontName, float fontSize, const std::u32string& text) {
        TextSpan span;
        span.fontName = fontName;
        span.fontSize = fontSize;
        span.text = text;
        span.originalText = text;
        span.boundingBox = QRectF(0, 0, 100, 20);
        return span;
    }

    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<FontSubstitutionEngine> engine_;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<Page> page_;
    bool hasTestPdf_ = false;
};

// Test 1: No modifications returns empty result
TEST_F(FontSubstitutionEngineTest, NoModificationsReturnsEmpty) {
    if (!hasTestPdf_) {
        GTEST_SKIP() << "No test PDF available";
    }

    std::vector<TextSpan> spans;
    spans.push_back(makeSpan("Helvetica", 12.0f, U"Hello World"));

    std::vector<std::pair<int, std::u32string>> emptyMods;

    auto result = engine_->processEdits(page_.get(), doc_.get(), spans, emptyMods);

    EXPECT_TRUE(result.empty()) << "processEdits with no modifications should return empty vector";
}

// Test 2: ASCII text no substitution needed
TEST_F(FontSubstitutionEngineTest, AsciiTextNoSubstitution) {
    if (!hasTestPdf_) {
        GTEST_SKIP() << "No test PDF available";
    }

    std::vector<TextSpan> spans;
    spans.push_back(makeSpan("Helvetica", 12.0f, U"Original Text"));

    std::vector<std::pair<int, std::u32string>> modifications;
    modifications.push_back({0, U"Simple ASCII Text"});

    auto result = engine_->processEdits(page_.get(), doc_.get(), spans, modifications);

    // ASCII text should be covered by original font, so no substitution needed
    EXPECT_TRUE(result.empty()) << "Simple ASCII text should not require font substitution";
}

// Test 3: Null page returns empty
TEST_F(FontSubstitutionEngineTest, NullPageReturnsEmpty) {
    if (!hasTestPdf_) {
        GTEST_SKIP() << "No test PDF available";
    }

    std::vector<TextSpan> spans;
    spans.push_back(makeSpan("Helvetica", 12.0f, U"Hello"));

    std::vector<std::pair<int, std::u32string>> modifications;
    modifications.push_back({0, U"World"});

    auto result = engine_->processEdits(nullptr, doc_.get(), spans, modifications);

    EXPECT_TRUE(result.empty()) << "processEdits with null page should return empty vector";
}

// Test 4: Null document returns empty
TEST_F(FontSubstitutionEngineTest, NullDocReturnsEmpty) {
    if (!hasTestPdf_) {
        GTEST_SKIP() << "No test PDF available";
    }

    std::vector<TextSpan> spans;
    spans.push_back(makeSpan("Helvetica", 12.0f, U"Hello"));

    std::vector<std::pair<int, std::u32string>> modifications;
    modifications.push_back({0, U"World"});

    auto result = engine_->processEdits(page_.get(), nullptr, spans, modifications);

    EXPECT_TRUE(result.empty()) << "processEdits with null document should return empty vector";
}

// Test 5 (A1-47): Latin-1 accented text over the page's NON-embedded base-14
// Helvetica must produce NO substitution, and must not fabricate one.
//
// AUTHORED GROUND TRUTH — tests/generate_fixtures.py :: generate_single_page_pdf()
// writes test_single.pdf with FPDF's core font "Helvetica", which is NOT
// embedded (the page /Font dict holds a single simple font, no /FontFile*).
// MuPDF resolves the standard-14 Helvetica to its built-in URW
// NimbusSans-Regular (resources/fonts/urw/NimbusSans-Regular.cff, 654 glyphs),
// and that face DOES carry eacute / idieresis / the whole Latin-1 Supplement
// and Latin Extended-A. Every codepoint of "Café résumé naïve" is therefore
// renderable by the page's own font, so the CORRECT result is an EMPTY
// substitution list — substituting here would needlessly re-embed a font and
// change the glyph shapes of an edit that did not need it.
//
// The previous oracle here was `starts_with("Fsub") || !empty()` — a tautology
// (the second disjunct is implied by the EXPECT_FALSE(empty()) above it), and
// the whole block sat behind `if (!result.empty())` so it never ran at all.
//
// Mutations killed:
//   * removing the `if (!needsSubstitution) continue;` guard in
//     FontSubstitutionEngine::processEdits (every edit would substitute);
//   * inverting the coverage heuristic so COVERED codepoints are reported
//     missing (`needsSubstitution = missing.empty()` / dropping the `!` on the
//     `!embeddedProfile->hasLatinExtA` gate).
TEST_F(FontSubstitutionEngineTest, AccentedTextCoveredByBase14NeedsNoSubstitution) {
    if (!hasTestPdf_) {
        GTEST_SKIP() << "No test PDF available";
    }

    std::vector<TextSpan> spans;
    spans.push_back(makeSpan("Helvetica", 12.0f, U"Hello"));

    // NOTE on the literal below: this file carries no BOM and the build sets no
    // /utf-8, so the exact codepoints the compiler produces for the raw accents
    // depend on its narrow source charset. That does not weaken this oracle —
    // under EITHER reading every codepoint lands in Latin-1 Supplement, which
    // the standard-14 substitute (URW NimbusSans-Regular) covers in full, so
    // "no substitution" is the correct answer either way. The subset-fixture
    // test below, whose assertions DO depend on specific codepoints, builds its
    // string numerically instead.
    std::vector<std::pair<int, std::u32string>> modifications;
    modifications.push_back({0, U"Café résumé naïve"});

    auto result = engine_->processEdits(page_.get(), doc_.get(), spans, modifications);

    std::ostringstream got;
    for (const auto& sub : result) {
        got << " {spanIndex=" << sub.spanIndex
            << ", resName=" << sub.substituteResName
            << ", hScale=" << sub.horizontalScale << "}";
    }

    EXPECT_TRUE(result.empty())
        << "Latin-1 accented text is fully covered by the standard-14 Helvetica "
           "substitute (URW NimbusSans-Regular has eacute/idieresis), so NO font "
           "substitution must be emitted. Got " << result.size()
        << " substitution(s):" << got.str();
}

// Test 6 (A1-47): Multiple spans processed independently.
//
// The previous oracle was `EXPECT_GE(result.size(), 0u)` — vacuous for an
// unsigned type: size() >= 0 is true for every possible value, so the
// assertion could not fail even if processEdits returned garbage.
//
// AUTHORED GROUND TRUTH: all three replacement strings are pure Basic Latin,
// and test_single.pdf's only font resource is the non-embedded standard-14
// Helvetica whose MuPDF substitute covers Basic Latin in full. No span
// requires a substitute, so the correct result is EXACTLY zero entries.
//
// Mutations killed: substituting unconditionally; substituting once per SPAN
// rather than once per uncovered span; treating ASCII as uncovered.
TEST_F(FontSubstitutionEngineTest, MultipleSpansProcessed) {
    if (!hasTestPdf_) {
        GTEST_SKIP() << "No test PDF available";
    }

    std::vector<TextSpan> spans;
    spans.push_back(makeSpan("Helvetica", 12.0f, U"First span"));
    spans.push_back(makeSpan("Times-Roman", 14.0f, U"Second span"));
    spans.push_back(makeSpan("Courier", 10.0f, U"Third span"));

    std::vector<std::pair<int, std::u32string>> modifications;
    modifications.push_back({0, U"Modified first"});
    modifications.push_back({1, U"Modified second"});
    modifications.push_back({2, U"Modified third"});

    std::vector<SubstitutionInfo> result;
    ASSERT_NO_THROW({
        result = engine_->processEdits(page_.get(), doc_.get(), spans, modifications);
    });

    std::ostringstream got;
    for (const auto& sub : result) {
        got << " {spanIndex=" << sub.spanIndex
            << ", resName=" << sub.substituteResName << "}";
    }

    EXPECT_EQ(result.size(), 0u)
        << "All three replacements are pure ASCII and the page's only font "
           "covers Basic Latin, so processEdits must emit no substitutions. "
           "Got:" << got.str();
}

// ===========================================================================
// A1-47 — real substitution path, pinned end-to-end (resource-name FORM plus
// a tofu/.notdef oracle on the font that actually lands in the saved PDF).
// ===========================================================================
//
// AUTHORED GROUND TRUTH — tests/fixtures/make_embedded_italic_serif_subset.py
// writes embedded_italic_serif_subset.pdf with EXACTLY ONE font resource: a
// Type0 / Identity-H CIDFontType2 subset of a serif bold-italic face
// (BaseFont "SSTISF+PalatinoLTPro BoldItalic"). The generator's own
// _self_check() asserts the embedded program has NO 'cmap' table at all, so
// the page font can encode nothing by codepoint. Replacement text containing a
// Latin-1 accented letter therefore genuinely REQUIRES a substitute — which is
// what makes this fixture (unlike test_single.pdf) exercise the substitution
// path deterministically.
namespace {

// The page font's PostScript name minus the (randomised) 6-letter subset
// prefix. Used verbatim as the span's font name so processEdits' name match
// resolves to the real profile rather than the "first profile" fallback.
constexpr const char* kSubsetFontName = "PalatinoLTPro BoldItalic";

// Result of re-opening the SAVED pdf and probing the substituted font.
struct SubstituteFontProbe {
    bool opened = false;
    bool fontResourcePresent = false;   ///< /Page/Resources/Font/<resName>
    bool fontProgramPresent = false;    ///< ... /FontDescriptor /FontFile[23]
    bool fontLoaded = false;            ///< program parsed by FreeType via MuPDF
    std::vector<char32_t> tofuCodepoints;  ///< encoded to GID 0 (.notdef / tofu)
};

// Open @p pdfPath with an INDEPENDENT MuPDF handle, walk to the font registered
// under @p resName on page 0, pull its embedded font program out of the
// FontDescriptor, and ask the real font cmap for the glyph index of every
// codepoint in @p codepoints. GID 0 is .notdef — i.e. the tofu box the audit
// note ("blind to tofu/.notdef") is about.
SubstituteFontProbe probeSubstituteFont(pdfe::core::MuPdfContext* mu,
                                        const std::filesystem::path& pdfPath,
                                        const std::string& resName,
                                        const std::vector<char32_t>& codepoints)
{
    SubstituteFontProbe out;
    // Build every container BEFORE any fz_try frame (MuPDF setjmp/longjmp rule).
    out.tofuCodepoints.reserve(codepoints.size());
    const std::string pathStr = pdfPath.string();

    mu->execute([&](fz_context* c) {
        fz_document* fzDoc = nullptr;
        fz_buffer* buf = nullptr;
        fz_font* font = nullptr;
        // Locals assigned inside fz_try and read in fz_always must be fz_var'd
        // (setjmp may otherwise clobber them).
        fz_var(fzDoc);
        fz_var(buf);
        fz_var(font);

        fz_try(c) {
            fzDoc = fz_open_document(c, pathStr.c_str());
            pdf_document* pdfDoc = pdf_document_from_fz_document(c, fzDoc);
            if (pdfDoc) {
                out.opened = true;
                pdf_obj* pageObj = pdf_lookup_page_obj(c, pdfDoc, 0);
                pdf_obj* res = pdf_dict_get_inheritable(c, pageObj, PDF_NAME(Resources));
                pdf_obj* fonts = pdf_dict_get(c, res, PDF_NAME(Font));
                pdf_obj* fontRef = pdf_dict_gets(c, fonts, resName.c_str());
                out.fontResourcePresent = (fontRef != nullptr);

                pdf_obj* descriptor = pdf_dict_get(c, fontRef, PDF_NAME(FontDescriptor));
                if (!descriptor) {
                    // Type0 wrapper: the descriptor lives on the descendant.
                    pdf_obj* descendants =
                        pdf_dict_get(c, fontRef, PDF_NAME(DescendantFonts));
                    if (descendants && pdf_array_len(c, descendants) > 0) {
                        descriptor = pdf_dict_get(
                            c, pdf_array_get(c, descendants, 0), PDF_NAME(FontDescriptor));
                    }
                }

                pdf_obj* fontFile = pdf_dict_get(c, descriptor, PDF_NAME(FontFile2));
                if (!fontFile) fontFile = pdf_dict_get(c, descriptor, PDF_NAME(FontFile3));
                if (!fontFile) fontFile = pdf_dict_get(c, descriptor, PDF_NAME(FontFile));
                out.fontProgramPresent = (fontFile != nullptr);

                if (fontFile) {
                    buf = pdf_load_stream(c, fontFile);
                    font = fz_new_font_from_buffer(c, "pdfe-substitute-probe", buf, 0, 0);
                    if (font) {
                        out.fontLoaded = true;
                        for (char32_t cp : codepoints) {
                            if (fz_encode_character(c, font, static_cast<int>(cp)) == 0)
                                out.tofuCodepoints.push_back(cp);
                        }
                    }
                }
            }
        }
        fz_always(c) {
            if (font) fz_drop_font(c, font);
            if (buf) fz_drop_buffer(c, buf);
            if (fzDoc) fz_drop_document(c, fzDoc);
        }
        fz_catch(c) {
            // Leave the partially-filled probe; the test asserts on the flags.
        }
    });

    return out;
}

} // namespace

class FontSubstitutionEngineSubsetTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();
        engine_ = std::make_unique<FontSubstitutionEngine>(context_.get());

        fixturePath_ = std::filesystem::path(TEST_FIXTURES_DIR) /
                       "embedded_italic_serif_subset.pdf";
        if (!std::filesystem::exists(fixturePath_))
            return;

        doc_ = Document::open(context_, fixturePath_);
        if (doc_ && doc_->pageCount() > 0) {
            page_ = doc_->page(0);
            hasFixture_ = true;
        }
    }

    TextSpan makeSpan(const std::string& fontName, float fontSize,
                      const std::u32string& text) {
        TextSpan span;
        span.fontName = fontName;
        span.fontSize = fontSize;
        span.text = text;
        span.originalText = text;
        span.boundingBox = QRectF(60, 120, 400, 40);
        return span;
    }

    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<FontSubstitutionEngine> engine_;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<Page> page_;
    std::filesystem::path fixturePath_;
    bool hasFixture_ = false;
};

// A1-47: the substitution actually produced must be pinned, and the font that
// ends up in the file must MAP the requested characters to real glyphs.
//
// Mutations killed:
//   * resource-name form: emitting the substitute under the ORIGINAL font's
//     resource name, an empty name, or a name that is never registered in the
//     page /Font dict (the read-back finds no /Fsub1 → RED). The old
//     `starts_with("Fsub") || !empty()` accepted literally any non-empty name.
//   * count/contents: substituting for a span index that was never modified,
//     or emitting one entry per span instead of one per uncovered span.
//   * tofu/.notdef: embedding a font that does not cover the replacement text
//     (e.g. dropping the `findMissingGlyphs(...).empty()` coverage filter in
//     SubstituteFontSelector, or embedding the ORIGINAL cmap-less subset
//     program instead of the chosen substitute). Every accented codepoint would
//     then encode to GID 0 and render as a tofu box — invisible to every
//     pre-existing assertion in this file.
TEST_F(FontSubstitutionEngineSubsetTest, SubstitutionEmbedsFontThatMapsEveryRequestedGlyph) {
    if (!hasFixture_) {
        GTEST_SKIP() << "embedded_italic_serif_subset.pdf not available";
    }

    // Built numerically (no BOM / no /utf-8 on this build, so raw source
    // accents would be codepage-dependent). Authored text: "Cafe resume naive"
    // with U+00E9 LATIN SMALL LETTER E WITH ACUTE and U+00EF LATIN SMALL LETTER
    // I WITH DIAERESIS — both absent from the fixture's cmap-less embedded
    // subset, both present in any real Latin text font.
    std::u32string replacement = U"Caf";
    replacement.push_back(static_cast<char32_t>(0x00E9));
    replacement += U" r";
    replacement.push_back(static_cast<char32_t>(0x00E9));
    replacement += U"sum";
    replacement.push_back(static_cast<char32_t>(0x00E9));
    replacement += U" na";
    replacement.push_back(static_cast<char32_t>(0x00EF));
    replacement += U"ve";

    // Every non-space codepoint of the replacement must resolve to a real glyph
    // (GID != 0) in the font that actually lands in the saved PDF.
    std::vector<char32_t> requiredCodepoints;
    for (char32_t cp : replacement) {
        if (cp != U' ')
            requiredCodepoints.push_back(cp);
    }
    ASSERT_FALSE(requiredCodepoints.empty());

    std::vector<TextSpan> spans;
    spans.push_back(makeSpan(kSubsetFontName, 33.0f, U"A Tale of Two Collards"));

    std::vector<std::pair<int, std::u32string>> modifications;
    modifications.push_back({0, replacement});

    auto result = engine_->processEdits(page_.get(), doc_.get(), spans, modifications);

    // --- Pinned result: exactly one substitution, for the one modified span ---
    ASSERT_EQ(result.size(), 1u)
        << "The page's only font is a cmap-less embedded subset, so the single "
           "accented modification must yield EXACTLY one substitution.";

    const SubstitutionInfo& sub = result.front();
    EXPECT_EQ(sub.spanIndex, 0)
        << "Substitution must be attributed to the span that was modified.";
    EXPECT_EQ(sub.originalFontName, kSubsetFontName)
        << "originalFontName must echo the span's font, not the substitute's.";
    EXPECT_FLOAT_EQ(sub.originalFontSize, 33.0f)
        << "originalFontSize must echo the span's authored size.";

    // --- Pinned resource-name FORM: "Fsub<N>", N >= 1, and the fixture page
    // has no pre-existing Fsub* key, so the first substitute is exactly Fsub1.
    EXPECT_EQ(sub.substituteResName, "Fsub1")
        << "PdfFontEmbedder allocates the first free FsubN key; the fixture's "
           "/Font dict contains only the original resource, so this must be Fsub1.";

    // Scale stays a correction FACTOR, never a raw width or a percentage.
    EXPECT_GE(sub.horizontalScale, 0.5f);
    EXPECT_LE(sub.horizontalScale, 2.0f);

    // --- Tofu/.notdef oracle on the SAVED bytes -----------------------------
    const auto tempPath = std::filesystem::temp_directory_path() /
                          "pdfe_a1_47_substituted_font.pdf";
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
    ASSERT_NO_THROW(doc_->saveAs(tempPath));
    ASSERT_TRUE(std::filesystem::exists(tempPath));

    auto probeContext = std::make_shared<MuPdfContext>();
    const auto probe = probeSubstituteFont(probeContext.get(), tempPath,
                                           sub.substituteResName, requiredCodepoints);

    EXPECT_TRUE(probe.opened) << "Saved PDF could not be re-opened.";
    EXPECT_TRUE(probe.fontResourcePresent)
        << "No /Font/" << sub.substituteResName
        << " on page 0 of the saved PDF — the reported resource name does not "
           "name a font that actually exists in the file.";
    EXPECT_TRUE(probe.fontProgramPresent)
        << "/Font/" << sub.substituteResName
        << " has no embedded font program (FontFile/FontFile2/FontFile3); the "
           "substitute was referenced but never embedded.";
    EXPECT_TRUE(probe.fontLoaded)
        << "Embedded substitute font program failed to parse.";

    std::ostringstream tofu;
    for (char32_t cp : probe.tofuCodepoints)
        tofu << " U+" << std::hex << std::uppercase << static_cast<unsigned>(cp);
    EXPECT_TRUE(probe.tofuCodepoints.empty())
        << "Substituted font maps " << probe.tofuCodepoints.size()
        << " requested codepoint(s) to GID 0 (.notdef/tofu):" << tofu.str()
        << " — the replacement text would render as empty boxes.";

    std::filesystem::remove(tempPath, ec);
}
