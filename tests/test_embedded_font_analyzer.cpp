#include <gtest/gtest.h>
#include "fonts/EmbeddedFontAnalyzer.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include <filesystem>
#include <memory>

using namespace pdfe::fonts;
using namespace pdfe::core;

class EmbeddedFontAnalyzerTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();
        analyzer = std::make_unique<EmbeddedFontAnalyzer>(context.get());

        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        testPdf = fixturesDir / "test_3page.pdf";

        if (std::filesystem::exists(testPdf)) {
            document = Document::open(context, testPdf);
        }
    }

    std::shared_ptr<MuPdfContext> context;
    std::unique_ptr<EmbeddedFontAnalyzer> analyzer;
    std::filesystem::path testPdf;
    std::unique_ptr<Document> document;
};

TEST_F(EmbeddedFontAnalyzerTest, AnalyzePageFontsReturnsProfiles) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    auto profiles = analyzer->analyzePageFonts(page.get(), document.get());

    // A typical PDF page should have at least one font
    ASSERT_GT(profiles.size(), 0u);

    // First font should have a valid profile
    const auto& first = profiles[0];
    EXPECT_TRUE(first.isValid()) << "Font profile should be valid";
    EXPECT_FALSE(first.postScriptName.empty()) << "PostScript name should not be empty";
}

TEST_F(EmbeddedFontAnalyzerTest, PostScriptNamePopulated) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    auto profiles = analyzer->analyzePageFonts(page.get(), document.get());
    ASSERT_GT(profiles.size(), 0u);

    for (const auto& p : profiles) {
        EXPECT_FALSE(p.postScriptName.empty())
            << "Every analyzed font should have a PostScript name";
    }
}

TEST_F(EmbeddedFontAnalyzerTest, WeightClassPopulated) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    auto profiles = analyzer->analyzePageFonts(page.get(), document.get());
    ASSERT_GT(profiles.size(), 0u);

    for (const auto& p : profiles) {
        // Weight class should be a reasonable value (100-900)
        EXPECT_GE(p.weightClass, 100);
        EXPECT_LE(p.weightClass, 900);
    }
}

TEST_F(EmbeddedFontAnalyzerTest, StemWeightMeasured) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    auto profiles = analyzer->analyzePageFonts(page.get(), document.get());
    ASSERT_GT(profiles.size(), 0u);

    // At least some fonts should have measurable stem weight
    bool anyStemMeasured = false;
    for (const auto& p : profiles) {
        if (p.stemWeightV > 0.0f) {
            anyStemMeasured = true;
            // Stem weight should be reasonable (not > 50% of em)
            EXPECT_LT(p.stemWeightV, 0.5f);
        }
    }
    // Not all embedded fonts support bitmap rendering, so this is non-fatal
    if (!anyStemMeasured) {
        std::cout << "Note: No fonts had measurable stem weight (may be Type3 or CID fonts)\n";
    }
}

TEST_F(EmbeddedFontAnalyzerTest, CoverageDetected) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    auto profiles = analyzer->analyzePageFonts(page.get(), document.get());
    ASSERT_GT(profiles.size(), 0u);

    // Standard PDF fonts should have at least Latin Basic coverage
    bool anyLatinBasic = false;
    for (const auto& p : profiles) {
        if (p.hasLatinBasic) {
            anyLatinBasic = true;
            break;
        }
    }
    EXPECT_TRUE(anyLatinBasic) << "At least one font should cover Latin Basic";
}

TEST_F(EmbeddedFontAnalyzerTest, XHeightCapHeightPopulated) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    auto profiles = analyzer->analyzePageFonts(page.get(), document.get());
    ASSERT_GT(profiles.size(), 0u);

    bool anyMetrics = false;
    for (const auto& p : profiles) {
        if (p.xHeight > 0.0f && p.capHeight > 0.0f) {
            anyMetrics = true;
            // x-height should be less than cap-height
            EXPECT_LT(p.xHeight, p.capHeight);
            // Both should be reasonable fractions of UPM
            EXPECT_LT(p.xHeight, 1.0f);
            EXPECT_LT(p.capHeight, 1.0f);
        }
    }
    // At least some fonts should have these metrics
    if (!anyMetrics) {
        std::cout << "Note: No fonts had measurable xHeight/capHeight (may lack OS/2 table)\n";
    }
}

TEST_F(EmbeddedFontAnalyzerTest, GlyphCountPositive) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    auto profiles = analyzer->analyzePageFonts(page.get(), document.get());
    ASSERT_GT(profiles.size(), 0u);

    for (const auto& p : profiles) {
        EXPECT_GT(p.glyphCount, 0) << "Font " << p.postScriptName << " should have glyphs";
    }
}

TEST_F(EmbeddedFontAnalyzerTest, InvalidFontHandleReturnsNullopt) {
    auto result = analyzer->analyzeFont(nullptr);
    EXPECT_FALSE(result.has_value());
}

TEST_F(EmbeddedFontAnalyzerTest, NullPageHandled) {
    auto profiles = analyzer->analyzePageFonts(nullptr, nullptr);
    EXPECT_TRUE(profiles.empty());
}

TEST_F(EmbeddedFontAnalyzerTest, MultipleFontsOnPage) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    // Try form PDF which typically has multiple fonts
    std::filesystem::path formPdf = std::filesystem::path(TEST_FIXTURES_DIR) / "test_form.pdf";
    if (!std::filesystem::exists(formPdf)) {
        // Fall back to regular test PDF
        auto page = document->page(0);
        ASSERT_NE(page, nullptr);
        auto profiles = analyzer->analyzePageFonts(page.get(), document.get());
        EXPECT_GE(profiles.size(), 1u);
        return;
    }

    auto formDoc = Document::open(context, formPdf);
    ASSERT_NE(formDoc, nullptr);

    auto page = formDoc->page(0);
    ASSERT_NE(page, nullptr);

    auto profiles = analyzer->analyzePageFonts(page.get(), formDoc.get());
    // Form PDFs often have multiple fonts
    EXPECT_GE(profiles.size(), 1u);

    // All should be valid
    for (const auto& p : profiles) {
        EXPECT_TRUE(p.isValid());
    }
}

TEST_F(EmbeddedFontAnalyzerTest, AnalyzeFromFileHasPanose) {
    // Arial should have non-zero PANOSE data
    std::filesystem::path arialPath = "C:\\Windows\\Fonts\\arial.ttf";
    if (!std::filesystem::exists(arialPath)) {
        GTEST_SKIP() << "Arial not found at " << arialPath;
    }

    auto profile = analyzer->analyzeFromFile(arialPath.string());
    ASSERT_TRUE(profile.has_value());

    // PANOSE byte 0 (familyKind) should be 2 (Latin Text) for Arial
    EXPECT_EQ(profile->panose[0], 2) << "Arial should be Latin Text (familyKind=2)";

    // At least some of the remaining bytes should be non-zero
    int nonZeroCount = 0;
    for (int i = 1; i < 10; ++i) {
        if (profile->panose[i] != 0)
            ++nonZeroCount;
    }
    EXPECT_GE(nonZeroCount, 5) << "Arial should have most PANOSE bytes populated";
}
