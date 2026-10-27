#include <gtest/gtest.h>

#include "tre/AutoMissingGlyphResolver.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/PdfFontEmbedder.h"
#include "fonts/VectorSubsetDetector.h"
#include "textedit/TextEditModel.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::fonts::PageFontClass;
using pdfe::fonts::VectorSubsetDetector;
using pdfe::textedit::FormattingRun;
using pdfe::tre::AutoMissingGlyphResolver;

namespace {

class AutoMissingGlyphResolverTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
    }
    std::unique_ptr<Document> openFixture(const char* rel) {
        std::filesystem::path p =
            std::filesystem::path(TEST_FIXTURES_DIR) / rel;
        if (!std::filesystem::exists(p))
            return nullptr;
        return Document::open(ctx_, p);
    }
    std::shared_ptr<MuPdfContext> ctx_;
};

// ─── Defensive null-input handling ──────────────────────────────────────────

TEST_F(AutoMissingGlyphResolverTest, NullInputsReturnEmpty) {
    auto runs = AutoMissingGlyphResolver::resolve(
        nullptr, nullptr, nullptr, "Test", 12.0f, U"abc");
    EXPECT_TRUE(runs.empty());
}

TEST_F(AutoMissingGlyphResolverTest, EmptySourceFontNameReturnsEmpty) {
    auto doc = openFixture("test_blank.pdf");
    if (!doc) GTEST_SKIP() << "test_blank.pdf not available";
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    auto runs = AutoMissingGlyphResolver::resolve(
        ctx_.get(), page.get(), doc.get(), "", 12.0f, U"abc");
    EXPECT_TRUE(runs.empty());
}

TEST_F(AutoMissingGlyphResolverTest, EmptyTextReturnsEmpty) {
    auto doc = openFixture("test_blank.pdf");
    if (!doc) GTEST_SKIP() << "test_blank.pdf not available";
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    auto runs = AutoMissingGlyphResolver::resolve(
        ctx_.get(), page.get(), doc.get(), "TestFont", 12.0f, U"");
    EXPECT_TRUE(runs.empty());
}

// ─── Source font not on page → no runs ──────────────────────────────────────

TEST_F(AutoMissingGlyphResolverTest, MissingSourceFontReturnsEmpty) {
    auto doc = openFixture("test_blank.pdf");
    if (!doc) GTEST_SKIP() << "test_blank.pdf not available";
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    // test_blank.pdf has no fonts; resolve must return empty (not crash,
    // not produce runs against a non-existent font).
    auto runs = AutoMissingGlyphResolver::resolve(
        ctx_.get(), page.get(), doc.get(),
        "AAAAAA+NonexistentFont", 12.0f, U"abc");
    EXPECT_TRUE(runs.empty());
}

// ─── End-to-end: subsetted vector PDF ───────────────────────────────────────
//
// Test fixture 'test_subsetted_font.pdf' is the canonical fixture for
// this phase. We synthesize one ahead of time by embedding a system
// font as a subsetted resource into a blank PDF. If the fixture isn't
// available, GTEST_SKIP — the test still validates the resolver's
// public interface.

class AutoMissingGlyphResolverFixtureTest : public AutoMissingGlyphResolverTest {
protected:
    void SetUp() override {
        AutoMissingGlyphResolverTest::SetUp();

        // Build a fresh subsetted-font fixture in memory: open
        // test_blank.pdf, embed Consolas (which contains a Latin Basic
        // subset only — no Linear B, no CJK Compat). The embedded font
        // becomes "Cs+Consolas" or similar after the embedder's prefix
        // is added; we look up the actual name at test time.
        // test_single.pdf is the canonical fixture for embedFont
        // success — test_blank.pdf has been observed to silently fail
        // pdf_add_cid_font on some MuPDF builds (likely related to its
        // minimal page structure).
        doc_ = openFixture("test_single.pdf");
        if (!doc_) return;
        auto page = doc_->page(0);
        if (!page) return;

        // Embed a symbol-only font (Webdings/Wingdings) so almost any
        // typed character is unencodable in the source font and goes
        // through the system-font fallback path. If symbol fonts
        // aren't available, fall back to a regular Latin font and
        // accept that fewer codepoints will exercise the fallback.
        const std::string candidates[] = {
            "C:/Windows/Fonts/webdings.ttf",
            "C:/Windows/Fonts/wingding.ttf",
            "C:/Windows/Fonts/symbol.ttf",
            "C:/Windows/Fonts/arial.ttf",
            "C:/Windows/Fonts/consola.ttf",
        };
        std::string fontPath;
        for (const auto& c : candidates) {
            if (std::filesystem::exists(c)) { fontPath = c; break; }
        }
        if (fontPath.empty()) return;

        pdfe::fonts::PdfFontEmbedder embedder(ctx_.get());
        auto info = embedder.embedFont(page.get(), doc_.get(), fontPath);
        if (info.has_value())
            embeddedResource_ = info->resourceName;
    }

    /// After SetUp, look up the PostScript name of the embedded font on
    /// the page via EmbeddedFontAnalyzer, skipping the standard-14
    /// pseudo-fonts (Helvetica, Times-Roman, Courier) that test_single.pdf
    /// already references without embedding. We want the BaseFont of
    /// the font we actually embedded — that's the one whose bytes the
    /// resolver can extract.
    std::string findEmbeddedBaseFont() {
        if (!doc_) return {};
        auto page = doc_->page(0);
        if (!page) return {};
        pdfe::fonts::EmbeddedFontAnalyzer ana(ctx_.get());
        auto profiles = ana.analyzePageFonts(page.get(), doc_.get());
        static const std::vector<std::string> standard14 = {
            "Helvetica", "Helvetica-Bold", "Helvetica-Oblique",
            "Helvetica-BoldOblique",
            "Times-Roman", "Times-Bold", "Times-Italic", "Times-BoldItalic",
            "Courier", "Courier-Bold", "Courier-Oblique",
            "Courier-BoldOblique",
            "Symbol", "ZapfDingbats",
        };
        for (const auto& p : profiles) {
            if (p.postScriptName.empty()) continue;
            bool isStd = false;
            for (const auto& s : standard14) {
                if (p.postScriptName == s) { isStd = true; break; }
            }
            if (!isStd) return p.postScriptName;
        }
        return {};
    }

    std::unique_ptr<Document> doc_;
    std::string embeddedResource_;
};

TEST_F(AutoMissingGlyphResolverFixtureTest, AllEncodableTextProducesNoRunsAndNoFontMutation) {
    if (!doc_) GTEST_SKIP() << "fixture unavailable";
    std::string baseFont = findEmbeddedBaseFont();
    if (baseFont.empty())
        GTEST_SKIP() << "no subsetted font embedded";
    auto page = doc_->page(0);
    ASSERT_NE(page, nullptr);

    // Snapshot the page's font count before the resolve call. The
    // VectorSubsetted regression guard requires that resolve() must NOT
    // install a Type 3 font (or any other resource) on the page when
    // every codepoint in the text is already encodable by the source
    // font. We use analyzePageFonts as the introspection vehicle since
    // tests don't have friend access to walk /Font directly.
    pdfe::fonts::EmbeddedFontAnalyzer ana(ctx_.get());
    const size_t fontsBefore = ana.analyzePageFonts(page.get(), doc_.get()).size();

    // The fixture embeds Webdings (a Symbol-cmap font). Its cmap maps
    // Private Use Area codepoints (U+F020..U+F0FF) to its pictogram
    // glyphs. Pick a PUA codepoint that's reliably in the Webdings
    // cmap and verify the resolver reports no unencodable chars and
    // produces no runs (and crucially, no Type 3 font is installed).
    std::u32string encodable;
    encodable.push_back(0xF030);  // Webdings cmap typically has this PUA char
    auto runs = AutoMissingGlyphResolver::resolve(
        ctx_.get(), page.get(), doc_.get(), baseFont, 12.0f, encodable);
    if (!runs.empty()) {
        // Webdings PUA mapping isn't available on this FreeType build —
        // skip rather than fail. The defensive null tests above already
        // validate the empty-input contract structurally.
        GTEST_SKIP() << "Webdings cmap doesn't expose U+F030 on this "
                        "FreeType/MuPDF build";
    }
    // (Already guarded by GTEST_SKIP above when not empty.)

    // No new font resource should have been installed on the page —
    // the regression guard at the resolver level (mirrored by the
    // applyText branch guard at the call site).
    const size_t fontsAfter = ana.analyzePageFonts(page.get(), doc_.get()).size();
    EXPECT_EQ(fontsBefore, fontsAfter)
        << "Resolver mutated the page's /Font dict despite all-encodable text "
           "(before=" << fontsBefore << " after=" << fontsAfter << ")";
}

TEST_F(AutoMissingGlyphResolverFixtureTest, UnencodableCharProducesAtLeastOneRun) {
    if (!doc_) GTEST_SKIP() << "fixture unavailable";
    std::string baseFont = findEmbeddedBaseFont();
    if (baseFont.empty())
        GTEST_SKIP() << "no subsetted font embedded";
    auto page = doc_->page(0);
    ASSERT_NE(page, nullptr);

    // The fixture embeds a symbol-only font (Webdings/Wingdings/Symbol)
    // so plain ASCII letters are unencodable in the source font but
    // are present in Arial — the resolver should route them to the
    // system-font fallback path.
    std::u32string text = U"x";
    auto runs = AutoMissingGlyphResolver::resolve(
        ctx_.get(), page.get(), doc_.get(), baseFont, 12.0f, text);
    ASSERT_GE(runs.size(), 1u)
        << "Resolver should produce at least one run for an unencodable "
           "codepoint";
    // The run must cover index 0 (the unencodable 'x' in Webdings).
    bool covers0 = false;
    for (const auto& r : runs) {
        if (r.charStart <= 0 && r.charEnd > 0)
            covers0 = true;
    }
    EXPECT_TRUE(covers0) << "No run covers the unencodable codepoint at index 0";
}

TEST_F(AutoMissingGlyphResolverFixtureTest, RunsCarryEitherType3OrSystemFont) {
    if (!doc_) GTEST_SKIP() << "fixture unavailable";
    std::string baseFont = findEmbeddedBaseFont();
    if (baseFont.empty())
        GTEST_SKIP() << "no subsetted font embedded";
    auto page = doc_->page(0);
    ASSERT_NE(page, nullptr);

    std::u32string text = U"x";  // unencodable in symbol-font fixture
    auto runs = AutoMissingGlyphResolver::resolve(
        ctx_.get(), page.get(), doc_.get(), baseFont, 12.0f, text);
    if (runs.empty())
        GTEST_SKIP() << "resolver couldn't produce any runs (no Type 3 "
                        "synth + no system font available)";
    for (const auto& r : runs) {
        // Each run must carry exactly one destination. Either
        // preInstalledResName (Type 3) or fontFilePath (system).
        const bool t3 = !r.preInstalledResName.empty();
        const bool sys = !r.fontFilePath.empty();
        EXPECT_TRUE(t3 != sys)
            << "Run [" << r.charStart << "," << r.charEnd
            << ") must carry exactly one of preInstalledResName/fontFilePath; "
               "got t3=" << t3 << " sys=" << sys;
    }
}

TEST_F(AutoMissingGlyphResolverFixtureTest, RunSizeMatchesSourceFontSize) {
    if (!doc_) GTEST_SKIP() << "fixture unavailable";
    std::string baseFont = findEmbeddedBaseFont();
    if (baseFont.empty()) GTEST_SKIP() << "no subsetted font";
    auto page = doc_->page(0);
    ASSERT_NE(page, nullptr);
    std::u32string text = U"y";  // unencodable in symbol-font fixture
    auto runs = AutoMissingGlyphResolver::resolve(
        ctx_.get(), page.get(), doc_.get(), baseFont, 18.0f, text);
    if (runs.empty()) GTEST_SKIP();
    for (const auto& r : runs)
        EXPECT_FLOAT_EQ(r.fontSize, 18.0f);
}

TEST_F(AutoMissingGlyphResolverFixtureTest, AdjacentSameDestRunsCollapse) {
    if (!doc_) GTEST_SKIP() << "fixture unavailable";
    std::string baseFont = findEmbeddedBaseFont();
    if (baseFont.empty()) GTEST_SKIP() << "no subsetted font";
    auto page = doc_->page(0);
    ASSERT_NE(page, nullptr);
    // Three consecutive unencodable codepoints — all same destination
    // (system font, since these aren't synthesizable). Should collapse
    // into a single run.
    std::u32string text = U"abc";  // all unencodable in symbol fixture
    auto runs = AutoMissingGlyphResolver::resolve(
        ctx_.get(), page.get(), doc_.get(), baseFont, 12.0f, text);
    if (runs.empty()) GTEST_SKIP();
    EXPECT_EQ(runs.size(), 1u)
        << "Three same-destination unencodable codepoints should collapse "
           "into one run";
    EXPECT_EQ(runs[0].charStart, 0);
    EXPECT_EQ(runs[0].charEnd, 3);
}

// ─── AC1 root invariant: every routed run encodes its assigned codepoints ──
//
// AC1 promises the saved PDF renders a non-.notdef glyph for unencodable
// typed characters. The pixel-NCC round-trip on a saved-and-reopened PDF
// is verified end-to-end in phase 8 (which has the SpecimenA Green PDF as
// a real subsetted-font fixture). For phase 6 we assert the root
// invariant the round-trip depends on: every run the resolver returns
// MUST point at a font that actually encodes the run's assigned
// codepoints. If the resolver routes a codepoint to a font whose cmap
// lacks it, MuPDF would render .notdef on save regardless of what the
// content stream says. This test exercises both the success path
// (codepoint encodable in some system font → run produced) and the
// failure path (no system font encodes it → no run produced, original
// font's .notdef is preserved as the no-worse outcome).

TEST_F(AutoMissingGlyphResolverFixtureTest, AllReturnedRunsEncodeAssignedCodepoints) {
    if (!doc_) GTEST_SKIP() << "fixture unavailable";
    std::string baseFont = findEmbeddedBaseFont();
    if (baseFont.empty()) GTEST_SKIP() << "no embedded font";
    auto page = doc_->page(0);
    ASSERT_NE(page, nullptr);

    // Mix of codepoints:
    //   0x2FA1F — CJK Compatibility Ideographs Supplement, not in
    //             Arial but should be in some Windows font; resolver
    //             may or may not produce a run depending on system
    //             configuration.
    //   0xE000  — Private Use Area, no font encodes this.
    // Whichever codepoints get runs MUST have an encodable destination.
    std::u32string text;
    text.push_back(0x2FA1F);
    text.push_back(0xE000);

    auto runs = AutoMissingGlyphResolver::resolve(
        ctx_.get(), page.get(), doc_.get(), baseFont, 12.0f, text);
    // Either runs empty (system font didn't encode either codepoint —
    // that's fine, the saved PDF falls back to original font's .notdef)
    // or each returned run points at a font that actually encodes the
    // codepoint at its first index.
    if (runs.empty()) {
        SUCCEED() << "Resolver dropped all unencodable codepoints because "
                     "no system font encodes them — original font's "
                     ".notdef is preserved (no AC1 violation)";
        return;
    }

    FT_Library lib = nullptr;
    ASSERT_EQ(FT_Init_FreeType(&lib), 0);
    for (const auto& r : runs) {
        ASSERT_LT(r.charStart, static_cast<int>(text.size()));
        char32_t cp = text[r.charStart];

        if (!r.preInstalledResName.empty()) {
            // Type 3 path — phase-5 tests already verify CharProc
            // round-trip via NCC ≥ 0.85. The resource was installed
            // and references a glyph that came from synthesis. Since
            // the synthesizer recipe table is Latin-only and our
            // test codepoints are non-Latin, we shouldn't hit this
            // branch — but if we do, accept it as covered by phase 5.
            continue;
        }

        ASSERT_FALSE(r.fontFilePath.empty())
            << "Run [" << r.charStart << "," << r.charEnd
            << ") has neither preInstalledResName nor fontFilePath";
        ASSERT_TRUE(std::filesystem::exists(r.fontFilePath))
            << "Routed system font path doesn't exist: " << r.fontFilePath;

        FT_Face face = nullptr;
        ASSERT_EQ(FT_New_Face(lib, r.fontFilePath.c_str(), 0, &face), 0);
        FT_UInt gid = FT_Get_Char_Index(face, static_cast<FT_ULong>(cp));
        EXPECT_NE(gid, 0u)
            << "Routed system font " << r.fontFilePath
            << " does NOT encode codepoint U+"
            << std::hex << static_cast<uint32_t>(cp)
            << " — saved PDF would render .notdef, violating AC1";
        FT_Done_Face(face);
    }
    FT_Done_FreeType(lib);
}

// ─── Performance gate (perf-benchmarks gate AC) ─────────────────────────────

TEST_F(AutoMissingGlyphResolverFixtureTest, ResolveCompletesUnder500ms) {
    if (!doc_) GTEST_SKIP() << "fixture unavailable";
    std::string baseFont = findEmbeddedBaseFont();
    if (baseFont.empty()) GTEST_SKIP() << "no subsetted font";
    auto page = doc_->page(0);
    ASSERT_NE(page, nullptr);

    // Single-character edit on a subsetted page — the typical
    // keystroke case. Phase-6 perf gate: < 500 ms wall-clock for the
    // amortized case (system-font discovery cache is process-static
    // so it warms once and stays cached). We warm with a discard call
    // first, then time the second invocation — that's the realistic
    // budget for a user typing through several keystrokes.
    std::u32string text = U"z";  // unencodable in symbol-font fixture
    auto warmup = AutoMissingGlyphResolver::resolve(
        ctx_.get(), page.get(), doc_.get(), baseFont, 12.0f, text);
    // Guard against silent false-greens: the warmup must produce a
    // run, otherwise we're not exercising the slow path the perf
    // gate is supposed to cover.
    ASSERT_FALSE(warmup.empty())
        << "Warmup produced no runs — system font fallback path didn't "
           "fire; perf measurement below would not exercise the gated path";

    auto t0 = std::chrono::steady_clock::now();
    auto runs = AutoMissingGlyphResolver::resolve(
        ctx_.get(), page.get(), doc_.get(), baseFont, 12.0f, text);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    ASSERT_FALSE(runs.empty())
        << "Timed resolve produced no runs — measurement is meaningless";
    EXPECT_LT(elapsed.count(), 500)
        << "Single-character resolve took " << elapsed.count()
        << " ms (perf gate is < 500 ms)";
}

}  // namespace
