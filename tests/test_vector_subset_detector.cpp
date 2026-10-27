#include <gtest/gtest.h>

#include "fonts/VectorSubsetDetector.h"
#include "fonts/EmbeddedFontAnalyzer.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <cstdint>
#include <iomanip>

#include <filesystem>
#include <memory>

using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::fonts::PageFontClass;
using pdfe::fonts::VectorSubsetDetector;

namespace {

class VectorSubsetDetectorTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
    }

    std::unique_ptr<Document> openFixture(const char* relPath) {
        std::filesystem::path p = std::filesystem::path(TEST_FIXTURES_DIR) / relPath;
        if (!std::filesystem::exists(p))
            return nullptr;
        return Document::open(ctx_, p);
    }

    std::shared_ptr<MuPdfContext> ctx_;
};

// ─── hasSubsetPrefix unit tests ─────────────────────────────────────────────

TEST(VectorSubsetDetector_HasSubsetPrefix, AcceptsCanonicalPrefix) {
    EXPECT_TRUE(VectorSubsetDetector::hasSubsetPrefix("AAAAAA+Alef"));
    EXPECT_TRUE(VectorSubsetDetector::hasSubsetPrefix("REFXWK+Liberation Serif"));
    EXPECT_TRUE(VectorSubsetDetector::hasSubsetPrefix("ZZZZZZ+X"));
}

TEST(VectorSubsetDetector_HasSubsetPrefix, RejectsBareName) {
    EXPECT_FALSE(VectorSubsetDetector::hasSubsetPrefix("Alef"));
    EXPECT_FALSE(VectorSubsetDetector::hasSubsetPrefix("Liberation Serif"));
}

TEST(VectorSubsetDetector_HasSubsetPrefix, RejectsLowercaseOrShortPrefix) {
    EXPECT_FALSE(VectorSubsetDetector::hasSubsetPrefix("aaaaaa+Alef"));   // lowercase
    EXPECT_FALSE(VectorSubsetDetector::hasSubsetPrefix("AAAAA+Alef"));    // 5 letters
    EXPECT_FALSE(VectorSubsetDetector::hasSubsetPrefix("AAAAAAA+Alef"));  // 7 letters
    EXPECT_FALSE(VectorSubsetDetector::hasSubsetPrefix("AAAAA1+Alef"));   // digit in prefix
    EXPECT_FALSE(VectorSubsetDetector::hasSubsetPrefix("AAAAAA-Alef"));   // wrong delimiter
    EXPECT_FALSE(VectorSubsetDetector::hasSubsetPrefix(""));              // empty
    EXPECT_FALSE(VectorSubsetDetector::hasSubsetPrefix("AAAAAA+"));       // missing tail
}

// ─── classify() integration tests ───────────────────────────────────────────

TEST_F(VectorSubsetDetectorTest, NullPageReturnsVectorComplete) {
    auto pageClass =
        VectorSubsetDetector::classify(nullptr, nullptr, ctx_.get());
    EXPECT_EQ(pageClass, PageFontClass::VectorComplete);
}

TEST_F(VectorSubsetDetectorTest, ScannedPdfClassifiesAsImageDominated) {
    auto doc = openFixture("test_scanned.pdf");
    if (!doc)
        GTEST_SKIP() << "Fixture test_scanned.pdf not available";
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    auto pageClass =
        VectorSubsetDetector::classify(page.get(), doc.get(), ctx_.get());
    EXPECT_EQ(pageClass, PageFontClass::ImageDominated)
        << "test_scanned.pdf should be image-dominated; got "
        << static_cast<int>(pageClass);
}

TEST_F(VectorSubsetDetectorTest, BlankPageClassifiesAsVectorComplete) {
    auto doc = openFixture("test_blank.pdf");
    if (!doc)
        GTEST_SKIP() << "Fixture test_blank.pdf not available";
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    auto pageClass =
        VectorSubsetDetector::classify(page.get(), doc.get(), ctx_.get());
    // A blank page has no fonts → falls into VectorComplete by default.
    // Image-dominated is also acceptable if the rasterizer flags the
    // empty page as such; both are non-VectorSubsetted, which is the
    // contract being tested. The point is it must NOT misclassify as
    // VectorSubsetted.
    EXPECT_NE(pageClass, PageFontClass::VectorSubsetted);
}

TEST_F(VectorSubsetDetectorTest, ThreePagePdfDoesNotMisclassifyAsImageDominated) {
    auto doc = openFixture("test_3page.pdf");
    if (!doc)
        GTEST_SKIP() << "Fixture test_3page.pdf not available";
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    auto pageClass =
        VectorSubsetDetector::classify(page.get(), doc.get(), ctx_.get());
    EXPECT_NE(pageClass, PageFontClass::ImageDominated)
        << "test_3page.pdf is vector text; should not be ImageDominated";
}

TEST_F(VectorSubsetDetectorTest, SubsettedFontNamesIsSubsetOfClassify) {
    // For every available fixture, subsettedFontNames must be empty
    // when classify() returns ImageDominated or VectorComplete, and
    // non-empty when it returns VectorSubsetted. The relationship is
    // an invariant: `classify == VectorSubsetted` iff
    // `!subsettedFontNames.empty() && !page->isImageDominated()`.
    static const char* kFixtures[] = {
        "test_3page.pdf",
        "test_blank.pdf",
        "test_scanned.pdf",
    };
    int verifiedFixtures = 0;
    for (const char* fx : kFixtures) {
        auto doc = openFixture(fx);
        if (!doc)
            continue;
        auto page = doc->page(0);
        if (!page)
            continue;
        auto pageClass =
            VectorSubsetDetector::classify(page.get(), doc.get(), ctx_.get());
        auto subset =
            VectorSubsetDetector::subsettedFontNames(
                page.get(), doc.get(), ctx_.get());

        if (pageClass == PageFontClass::VectorSubsetted) {
            EXPECT_FALSE(subset.empty())
                << "VectorSubsetted page must have at least one subset font ("
                << fx << ")";
            for (const auto& nm : subset) {
                EXPECT_TRUE(VectorSubsetDetector::hasSubsetPrefix(nm))
                    << "subsettedFontNames returned a non-subset name: " << nm;
            }
        } else {
            EXPECT_TRUE(subset.empty())
                << "Non-VectorSubsetted page must report no subset fonts ("
                << fx << "); got " << subset.size();
        }
        ++verifiedFixtures;
    }
    if (verifiedFixtures == 0) {
        GTEST_SKIP() << "No fixtures available for invariant check";
    }
}

TEST_F(VectorSubsetDetectorTest, ClassifyIsStableAcrossRepeatedCalls) {
    // Detector must be deterministic — two back-to-back calls on the
    // same page must produce identical results (no caching state leak,
    // no MuPDF object lifetime issue).
    auto doc = openFixture("test_3page.pdf");
    if (!doc)
        GTEST_SKIP() << "Fixture test_3page.pdf not available";
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    auto a = VectorSubsetDetector::classify(page.get(), doc.get(), ctx_.get());
    auto b = VectorSubsetDetector::classify(page.get(), doc.get(), ctx_.get());
    EXPECT_EQ(a, b);

    auto sa =
        VectorSubsetDetector::subsettedFontNames(page.get(), doc.get(), ctx_.get());
    auto sb =
        VectorSubsetDetector::subsettedFontNames(page.get(), doc.get(), ctx_.get());
    EXPECT_EQ(sa, sb);
}

// ─── findMissingGlyphs exercise (acceptance criterion phase 1) ──────────────

TEST_F(VectorSubsetDetectorTest, FindMissingGlyphsReportsUncoveredCodepoints) {
    // EmbeddedFontAnalyzer::findMissingGlyphs is the codepoint-coverage
    // primitive that downstream phases (FontSubsetExtender, integration
    // wiring) rely on. Phase 1 must demonstrate it works end-to-end on a
    // real font file. Use a Windows system font that is guaranteed to
    // exist and to lack a non-Latin codepoint.
    pdfe::fonts::EmbeddedFontAnalyzer analyzer(ctx_.get());

    // Hard-coded Windows path is acceptable here — the project's CLAUDE.md
    // declares this is a Windows-only build target. If the file is missing
    // the test should skip rather than fail (defensive against test runners
    // on stripped CI VMs).
    const std::string segoeUi = "C:/Windows/Fonts/segoeui.ttf";
    if (!std::filesystem::exists(segoeUi)) {
        GTEST_SKIP() << "Windows system font segoeui.ttf not available";
    }

    // Latin-only text → no missing glyphs.
    auto missing0 = analyzer.findMissingGlyphs(segoeUi, U"Hello world");
    EXPECT_EQ(missing0.size(), 0u)
        << "Segoe UI should cover ASCII; reported missing="
        << missing0.size();

    // Mixed Latin + a glyph reliably absent from Segoe UI's cmap. The
    // CJK Compatibility Ideographs Supplement (U+2FA1F) is in the
    // supplementary plane and not covered by Latin/UI fonts. The
    // contract is that the analyzer reports this codepoint as missing
    // and only this codepoint — index 0 ('A') must NOT appear in the
    // missing list.
    constexpr char32_t kUncoveredCp = 0x2FA1F;
    std::u32string mixed = U"A";
    mixed.push_back(kUncoveredCp);
    auto missing1 = analyzer.findMissingGlyphs(segoeUi, mixed);
    ASSERT_GE(missing1.size(), 1u)
        << "findMissingGlyphs must report U+2FA1F as missing in Segoe UI; "
           "if this fails, either Segoe UI gained CJK Compat coverage or "
           "the analyzer's cmap walk regressed";
    for (auto cp : missing1) {
        EXPECT_EQ(cp, kUncoveredCp)
            << "Unexpected missing codepoint U+"
            << std::hex << static_cast<uint32_t>(cp);
    }
}

} // namespace
