// -*- coding: utf-8 -*-
//
// Pipeline f8a12c4d phase-3 (SEC-PAGE-OVERFLOW): Page manipulation
// integer overflow and boundary condition tests.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/PageCommands.h"
#include "core/UndoManager.h"

#include <climits>
#include <filesystem>
#include <memory>

using namespace pdfe::core;
using namespace pdfe::commands;

namespace {
std::string fixturesDir() { return std::string(TEST_FIXTURES_DIR); }
}

class PageOverflowTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        auto fixture = std::filesystem::path(fixturesDir()) / "test_single.pdf";
        if (!std::filesystem::exists(fixture))
            GTEST_SKIP() << "test_single.pdf missing";
        doc_ = Document::open(ctx_, fixture);
        if (!doc_) GTEST_SKIP() << "Could not open fixture";
    }
    std::shared_ptr<MuPdfContext> ctx_;
    std::unique_ptr<Document> doc_;
};

TEST_F(PageOverflowTest, NegativePageIndexThrowsCleanly)
{
    EXPECT_THROW(doc_->page(-1), PdfeException)
        << "Negative page index must throw PdfeException, not crash";
}

TEST_F(PageOverflowTest, PageIndexBeyondCountThrowsCleanly)
{
    int count = doc_->pageCount();
    EXPECT_THROW(doc_->page(count), PdfeException);
    EXPECT_THROW(doc_->page(count + 1000), PdfeException);
}

TEST_F(PageOverflowTest, IntMaxPageIndexThrowsCleanly)
{
    EXPECT_THROW(doc_->page(INT_MAX), PdfeException);
}

TEST_F(PageOverflowTest, RotateOutOfRangeAngle)
{
    UndoManager mgr;
    // 360 degrees should be a no-op
    EXPECT_NO_THROW({
        auto cmd = std::make_unique<RotatePageCommand>(doc_.get(), 0, 360);
        mgr.execute(std::move(cmd));
    });
    // Large angle
    EXPECT_NO_THROW({
        auto cmd = std::make_unique<RotatePageCommand>(doc_.get(), 0, 99999);
        mgr.execute(std::move(cmd));
    });
    // Negative large angle
    EXPECT_NO_THROW({
        auto cmd = std::make_unique<RotatePageCommand>(doc_.get(), 0, -99999);
        mgr.execute(std::move(cmd));
    });
}

TEST_F(PageOverflowTest, DeletePageAtInvalidIndex)
{
    UndoManager mgr;
    // Deleting page at out-of-range index
    try {
        auto cmd = std::make_unique<DeletePageCommand>(doc_.get(), -1);
        mgr.execute(std::move(cmd));
    } catch (const std::exception&) {
        // Expected
    }

    try {
        auto cmd = std::make_unique<DeletePageCommand>(doc_.get(), INT_MAX);
        mgr.execute(std::move(cmd));
    } catch (const std::exception&) {
        // Expected
    }
}

TEST_F(PageOverflowTest, InsertBlankPageAtBoundary)
{
    UndoManager mgr;
    // Insert at index 0 (beginning) — should work
    EXPECT_NO_THROW({
        auto cmd = std::make_unique<InsertBlankPageCommand>(
            doc_.get(), 0, 612.0, 792.0);
        mgr.execute(std::move(cmd));
    });
    int countAfter = doc_->pageCount();
    EXPECT_EQ(countAfter, 2);

    // Undo to restore
    mgr.undo();
    EXPECT_EQ(doc_->pageCount(), 1);
}
