// -*- coding: utf-8 -*-
// Find + Replace (in-document) coverage (pipeline e5b9c231 Phase 4,
// rtm-coverage.md "Find + Replace — UNCOVERED": the onReplaceAll slot was never
// driven; only the search-index upsert was tested). This drives the real
// MainWindow replace-all slot and asserts the user-visible result after
// save/reopen -- the replacement text is present, the original is gone, and the
// reported count matches.
#include <gtest/gtest.h>
#include "ui/MainWindow.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QApplication>
#include <QAction>
#include <QTemporaryDir>

#include <filesystem>
#include <memory>

namespace {
std::filesystem::path fixtures() { return std::filesystem::path(TEST_FIXTURES_DIR); }
} // namespace

class FindReplaceTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto src = fixtures() / "test_single.pdf";  // "Single Page Test Document"
        if (!std::filesystem::exists(src)) GTEST_SKIP() << "test_single.pdf absent";
        tmp_ = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(tmp_->isValid());
        work_ = std::filesystem::path(tmp_->path().toStdString()) / "fr_work.pdf";
        std::filesystem::copy_file(src, work_);
        mw_ = std::make_unique<pdfe::ui::MainWindow>();
        QApplication::processEvents();
    }
    void TearDown() override {
        mw_.reset();
        QApplication::processEvents();
        tmp_.reset();
    }
    std::unique_ptr<QTemporaryDir> tmp_;
    std::filesystem::path work_;
    std::unique_ptr<pdfe::ui::MainWindow> mw_;
};

TEST_F(FindReplaceTest, ReplaceAllChangesDocumentText) {
    mw_->openFile(QString::fromStdString(work_.string()));
    QApplication::processEvents();

    // Replace "Page" -> "REPLACED" across the document.
    mw_->replaceAllForTesting("Page", "REPLACED");
    QApplication::processEvents();

    // The slot must report at least one match replaced (not the -1 sentinel).
    EXPECT_GT(mw_->lastReplaceAllCountForTesting(), 0)
        << "replace-all reported no matches for a word known to be present";

    // Persist and reopen; assert the user-visible truth.
    auto* saveAction = mw_->findChild<QAction*>("saveAction");
    ASSERT_NE(saveAction, nullptr);
    saveAction->trigger();
    QApplication::processEvents();

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, work_);
    ASSERT_NE(doc, nullptr);
    const std::string text = doc->page(0)->extractPlainText();
    EXPECT_NE(text.find("REPLACED"), std::string::npos)
        << "replacement text absent after save/reopen (text: '" << text << "')";
    EXPECT_EQ(text.find("Page"), std::string::npos)
        << "original word 'Page' still present -- replace-all did not replace it";
}

TEST_F(FindReplaceTest, ReplaceAllNoMatchReportsZero) {
    mw_->openFile(QString::fromStdString(work_.string()));
    QApplication::processEvents();
    mw_->replaceAllForTesting("ZZZ_NOT_PRESENT_QQQ", "X");
    QApplication::processEvents();
    EXPECT_EQ(mw_->lastReplaceAllCountForTesting(), 0)
        << "replace-all reported matches for a token that does not occur";
}
