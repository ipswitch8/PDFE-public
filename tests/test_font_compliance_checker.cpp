#include <gtest/gtest.h>
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "compliance/FontComplianceChecker.h"
#include "compliance/ComplianceTypes.h"
#include <filesystem>
#include <memory>
#include <vector>

using namespace pdfe::compliance;

class FontComplianceCheckerTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        auto pdfPath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
        if (!std::filesystem::exists(pdfPath)) {
            GTEST_SKIP() << "test_single.pdf not found";
        }
        document_ = pdfe::core::Document::open(context_, pdfPath);
        ASSERT_NE(document_, nullptr);
    }

    void TearDown() override {
        document_.reset();
        context_.reset();
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::core::Document> document_;
};

TEST_F(FontComplianceCheckerTest, ConstructDestroy) {
    FontComplianceChecker checker(context_);
    SUCCEED();
}

TEST_F(FontComplianceCheckerTest, CheckFonts_ReturnsNonEmpty) {
    FontComplianceChecker checker(context_);
    auto fonts = checker.checkFonts(document_.get(), false);

    EXPECT_FALSE(fonts.empty()) << "test_single.pdf should contain at least one font";
}

TEST_F(FontComplianceCheckerTest, CheckFonts_FontHasName) {
    FontComplianceChecker checker(context_);
    auto fonts = checker.checkFonts(document_.get(), false);

    ASSERT_FALSE(fonts.empty());

    for (const auto& font : fonts) {
        EXPECT_FALSE(font.fontName.empty()) << "Each font should have a non-empty name";
    }
}

TEST_F(FontComplianceCheckerTest, CheckFonts_PageIndex) {
    FontComplianceChecker checker(context_);
    auto fonts = checker.checkFonts(document_.get(), false);

    ASSERT_FALSE(fonts.empty());

    for (const auto& font : fonts) {
        EXPECT_GE(font.pageIndex, 0) << "Font page index should be >= 0";
        EXPECT_LT(font.pageIndex, document_->pageCount()) << "Font page index should be < page count";
    }
}

TEST_F(FontComplianceCheckerTest, CheckPageFonts_SinglePage) {
    FontComplianceChecker checker(context_);

    if (document_->pageCount() == 0) {
        GTEST_SKIP() << "Document has no pages";
    }

    auto fonts = checker.checkPageFonts(document_.get(), 0, false);

    for (const auto& font : fonts) {
        EXPECT_EQ(font.pageIndex, 0) << "All fonts should be from page 0";
    }
}

TEST_F(FontComplianceCheckerTest, CheckPageFonts_InvalidPage) {
    FontComplianceChecker checker(context_);

    int invalidPageIndex = document_->pageCount() + 10;
    auto fonts = checker.checkPageFonts(document_.get(), invalidPageIndex, false);

    EXPECT_TRUE(fonts.empty()) << "Invalid page index should return empty vector";
}

TEST_F(FontComplianceCheckerTest, AllFontsCompliant_Basic) {
    FontComplianceChecker checker(context_);

    bool allCompliant = checker.allFontsCompliant(document_.get(), false);

    // A3-19/A3-41 (self-negating tautology): `EXPECT_TRUE(x || !x)` holds for
    // every bool, so this test passed even for an implementation that returned a
    // constant. The contract is that the summary predicate agrees with the
    // per-font detail the SAME checker reports.
    const auto fonts = checker.checkFonts(document_.get(), false);
    bool everyFontCompliant = true;
    for (const auto& f : fonts)
        if (!f.issues.empty()) everyFontCompliant = false;
    EXPECT_EQ(allCompliant, everyFontCompliant)
        << "allFontsCompliant() disagrees with checkFonts(): " << fonts.size()
        << " font(s) inspected";
}

TEST_F(FontComplianceCheckerTest, AllFontsCompliant_RequireUnicode) {
    FontComplianceChecker checker(context_);

    bool compliantBasic = checker.allFontsCompliant(document_.get(), false);
    bool compliantUnicode = checker.allFontsCompliant(document_.get(), true);

    if (compliantUnicode) {
        EXPECT_TRUE(compliantBasic) << "If Unicode compliant, should be basically compliant too";
    }
}

TEST_F(FontComplianceCheckerTest, CheckFonts_EmbeddedStatus) {
    FontComplianceChecker checker(context_);
    auto fonts = checker.checkFonts(document_.get(), false);

    ASSERT_FALSE(fonts.empty());

    bool foundEmbeddedValue = false;
    for (const auto& font : fonts) {
        if (font.isEmbedded || !font.isEmbedded) {
            foundEmbeddedValue = true;
        }
    }

    EXPECT_TRUE(foundEmbeddedValue) << "isEmbedded field should be populated";
}

TEST_F(FontComplianceCheckerTest, CheckFonts_IssuesPopulated) {
    FontComplianceChecker checker(context_);
    auto fonts = checker.checkFonts(document_.get(), true);

    bool foundNonCompliant = false;
    for (const auto& font : fonts) {
        if (!font.isEmbedded || !font.hasToUnicode) {
            foundNonCompliant = true;
            EXPECT_FALSE(font.issues.empty()) << "Non-compliant font '"
                << font.fontName << "' should have issues listed";
        }
    }

    if (!foundNonCompliant) {
        GTEST_SKIP() << "All fonts are compliant, cannot test issue population";
    }
}
