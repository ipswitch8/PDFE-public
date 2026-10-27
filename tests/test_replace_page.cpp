// -*- coding: utf-8 -*-
#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/PageCommands.h"
#include "core/UndoManager.h"
#include <filesystem>
#include <memory>

using namespace pdfe::core;
using namespace pdfe::commands;

class ReplacePageTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx = std::make_shared<MuPdfContext>();
        std::filesystem::path fixtures = TEST_FIXTURES_DIR;
        targetPath = fixtures / "test_3page.pdf";
        sourcePath = fixtures / "test_single.pdf";
        if (!std::filesystem::exists(targetPath) || !std::filesystem::exists(sourcePath)) {
            GTEST_SKIP() << "Required fixtures not available";
        }
    }
    std::shared_ptr<MuPdfContext> ctx;
    std::filesystem::path targetPath;
    std::filesystem::path sourcePath;
};

TEST_F(ReplacePageTest, ReplacePagePreservesPageCount) {
    auto target = Document::open(ctx, targetPath);
    auto source = Document::open(ctx, sourcePath);
    ASSERT_NE(target, nullptr);
    ASSERT_NE(source, nullptr);

    int before = target->pageCount();
    target->replacePage(0, *source, 0);
    int after = target->pageCount();

    EXPECT_EQ(after, before);  // Same count — one replaced, not added or removed
}

TEST_F(ReplacePageTest, ReplacePageOutOfRangeThrows) {
    auto target = Document::open(ctx, targetPath);
    auto source = Document::open(ctx, sourcePath);

    EXPECT_THROW(target->replacePage(-1, *source, 0), std::exception);
    EXPECT_THROW(target->replacePage(99, *source, 0), std::exception);
}

TEST_F(ReplacePageTest, ReplacePageCommandUndoRestoresOriginal) {
    auto target = Document::open(ctx, targetPath);
    auto source = Document::open(ctx, sourcePath);

    UndoManager undoMgr;

    // Record page 0's text before replacement
    auto page0Before = target->page(0);
    std::string textBefore = page0Before ? page0Before->extractPlainText() : "";
    page0Before.reset();  // Release before command execution

    std::shared_ptr<Document> srcShared(source.release());
    auto cmd = std::make_unique<ReplacePageCommand>(target.get(), 0, srcShared, 0);
    undoMgr.execute(std::move(cmd));

    // After replacement — text should differ (different source file)
    auto page0After = target->page(0);
    std::string textAfter = page0After ? page0After->extractPlainText() : "";
    page0After.reset();

    // Undo
    undoMgr.undo();

    // After undo — text should match original
    auto page0Restored = target->page(0);
    std::string textRestored = page0Restored ? page0Restored->extractPlainText() : "";

    EXPECT_EQ(textRestored, textBefore)
        << "Undo did not restore the original page";
}

TEST_F(ReplacePageTest, ReplacePageCommandDescription) {
    auto target = Document::open(ctx, targetPath);
    auto source = Document::open(ctx, sourcePath);
    std::shared_ptr<Document> srcShared(source.release());
    ReplacePageCommand cmd(target.get(), 2, srcShared, 0);
    EXPECT_EQ(cmd.description(), "Replace Page 3");
}
