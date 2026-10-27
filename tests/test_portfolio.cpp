// -*- coding: utf-8 -*-
// Portfolio (PDF package) detection + view mode. A portfolio PDF
// has a /Collection dict in /Catalog and is typically browsed as a
// container of embedded files rather than a single page-body document.
#include <gtest/gtest.h>
#include "attachments/AttachmentManager.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <filesystem>
#include <memory>

using namespace pdfe::attachments;
using namespace pdfe::core;

namespace {
std::filesystem::path fixturesDir() {
    return std::filesystem::path(TEST_FIXTURES_DIR);
}
} // namespace

class PortfolioTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx = std::make_shared<MuPdfContext>();
        mgr = std::make_unique<AttachmentManager>(ctx);
    }
    std::shared_ptr<MuPdfContext> ctx;
    std::unique_ptr<AttachmentManager> mgr;
};

TEST_F(PortfolioTest, PortfolioFixtureIsDetected)
{
    auto p = fixturesDir() / "test_portfolio.pdf";
    if (!std::filesystem::exists(p)) {
        GTEST_SKIP() << "test_portfolio.pdf not generated";
    }
    auto doc = Document::open(ctx, p);
    ASSERT_NE(doc, nullptr);
    EXPECT_TRUE(mgr->isPortfolio(doc.get()));
}

TEST_F(PortfolioTest, PortfolioViewDefaultsToD)
{
    auto p = fixturesDir() / "test_portfolio.pdf";
    if (!std::filesystem::exists(p)) {
        GTEST_SKIP() << "test_portfolio.pdf not generated";
    }
    auto doc = Document::open(ctx, p);
    // The fixture explicitly sets /View /D.
    EXPECT_EQ(mgr->portfolioView(doc.get()), "D");
}

TEST_F(PortfolioTest, NonPortfolioReturnsFalse)
{
    auto p = fixturesDir() / "test_single.pdf";
    if (!std::filesystem::exists(p)) {
        GTEST_SKIP() << "test_single.pdf not available";
    }
    auto doc = Document::open(ctx, p);
    EXPECT_FALSE(mgr->isPortfolio(doc.get()));
}

TEST_F(PortfolioTest, NonPortfolioViewFallsBackToD)
{
    auto p = fixturesDir() / "test_single.pdf";
    if (!std::filesystem::exists(p)) {
        GTEST_SKIP() << "test_single.pdf not available";
    }
    auto doc = Document::open(ctx, p);
    // Regardless of whether /Collection is set, the view getter returns
    // the PDF spec default "D" when no explicit value is present.
    EXPECT_EQ(mgr->portfolioView(doc.get()), "D");
}

TEST_F(PortfolioTest, NullDocReturnsFalse)
{
    EXPECT_FALSE(mgr->isPortfolio(nullptr));
    EXPECT_EQ(mgr->portfolioView(nullptr), "D");
}

TEST_F(PortfolioTest, PortfolioExposesEmbeddedFilesViaList)
{
    auto p = fixturesDir() / "test_portfolio.pdf";
    if (!std::filesystem::exists(p)) {
        GTEST_SKIP();
    }
    auto doc = Document::open(ctx, p);
    // The fixture has two embedded files ("a.txt", "b.txt"). The
    // existing list() code path is reused for portfolios — isPortfolio
    // is purely additive metadata, not a separate access path.
    auto entries = mgr->list(doc.get());
    EXPECT_GE(entries.size(), 2u);
    bool sawA = false, sawB = false;
    for (const auto& e : entries) {
        if (e.name == "a.txt") sawA = true;
        if (e.name == "b.txt") sawB = true;
    }
    EXPECT_TRUE(sawA);
    EXPECT_TRUE(sawB);
}
