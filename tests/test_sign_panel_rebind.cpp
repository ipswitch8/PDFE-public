// -*- coding: utf-8 -*-
//
// GitHub #379 — every sign flow that commits REPLACES document_ (saveAs +
// reopen in MainWindow::signTargetFieldWithOptions), but the six panels that
// store a raw Document* (PagePanel, SearchPanel, BookmarkPanel, LayerPanel,
// CommentPanel, AttachmentPanel) were bound exactly once, in openFile().
// After ANY sign-reopen — success path included — they all kept the freed
// pre-sign pointer, and the next panel refresh was a use-after-free
// (CommentPanel::refresh() after a sign-reopen UAF'd under cdb during the
// #372 work; the #372 fix deliberately sidestepped panel refresh on its
// recovery path for exactly this reason).
//
// Fix under test: rebindPanelsToCurrentDocument(), called wherever the
// tileRenderer_/canvas binding is rebuilt (success path AND the #372
// catch-path recovery).
//
// Modeled on test_ui_sign_error_recovery.cpp.

#include <gtest/gtest.h>

#include "ui/MainWindow.h"
#include "ui/AttachmentPanel.h"
#include "ui/BookmarkPanel.h"
#include "ui/CommentPanel.h"
#include "ui/LayerPanel.h"
#include "ui/PagePanel.h"
#include "ui/SearchPanel.h"
#include "core/Document.h"

#include <QApplication>
#include <QTemporaryDir>

#include <filesystem>
#include <memory>

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

} // namespace

class SignPanelRebindTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        const auto src = fixturesDir() / "test_single.pdf";
        pfx_ = fixturesDir() / "test_signer.pfx";
        if (!std::filesystem::exists(src)) {
            GTEST_SKIP() << "test_single.pdf fixture not available";
        }
        if (!std::filesystem::exists(pfx_)) {
            GTEST_SKIP() << "test_signer.pfx fixture not available";
        }

        tempDir_ = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(tempDir_->isValid());
        workPdf_ = std::filesystem::path(tempDir_->path().toStdString())
                   / "panel_rebind_work.pdf";
        std::filesystem::copy_file(src, workPdf_);

        mw_ = std::make_unique<pdfe::ui::MainWindow>();
        QApplication::processEvents();
        mw_->openFile(QString::fromStdString(workPdf_.string()));
        QApplication::processEvents();

        pagePanel_ = mw_->findChild<pdfe::ui::PagePanel*>();
        searchPanel_ = mw_->findChild<pdfe::ui::SearchPanel*>();
        bookmarkPanel_ = mw_->findChild<pdfe::ui::BookmarkPanel*>();
        layerPanel_ = mw_->findChild<pdfe::ui::LayerPanel*>();
        commentPanel_ = mw_->findChild<pdfe::ui::CommentPanel*>();
        attachmentPanel_ = mw_->findChild<pdfe::ui::AttachmentPanel*>();
        ASSERT_NE(pagePanel_, nullptr);
        ASSERT_NE(searchPanel_, nullptr);
        ASSERT_NE(bookmarkPanel_, nullptr);
        ASSERT_NE(layerPanel_, nullptr);
        ASSERT_NE(commentPanel_, nullptr);
        ASSERT_NE(attachmentPanel_, nullptr);
    }

    void TearDown() override
    {
        if (mw_) {
            mw_->setSignFlowTestInputsForTesting(std::nullopt);
        }
        mw_.reset();
        QApplication::processEvents();
        tempDir_.reset();
    }

    // All six panels bound to exactly `doc` (the live document).
    void expectPanelsBoundTo(const pdfe::core::Document* doc,
                             const char* where)
    {
        EXPECT_EQ(pagePanel_->documentForTesting(), doc)
            << "PagePanel stale after " << where;
        EXPECT_EQ(searchPanel_->documentForTesting(), doc)
            << "SearchPanel stale after " << where;
        EXPECT_EQ(bookmarkPanel_->documentForTesting(), doc)
            << "BookmarkPanel stale after " << where;
        EXPECT_EQ(layerPanel_->documentForTesting(), doc)
            << "LayerPanel stale after " << where;
        EXPECT_EQ(commentPanel_->documentForTesting(), doc)
            << "CommentPanel stale after " << where;
        EXPECT_EQ(attachmentPanel_->documentForTesting(), doc)
            << "AttachmentPanel stale after " << where;
    }

    std::unique_ptr<QTemporaryDir> tempDir_;
    std::filesystem::path workPdf_;
    std::filesystem::path pfx_;
    std::unique_ptr<pdfe::ui::MainWindow> mw_;
    pdfe::ui::PagePanel* pagePanel_ = nullptr;
    pdfe::ui::SearchPanel* searchPanel_ = nullptr;
    pdfe::ui::BookmarkPanel* bookmarkPanel_ = nullptr;
    pdfe::ui::LayerPanel* layerPanel_ = nullptr;
    pdfe::ui::CommentPanel* commentPanel_ = nullptr;
    pdfe::ui::AttachmentPanel* attachmentPanel_ = nullptr;
};

// SUCCESS path: a committed sign replaces the document twice; every panel
// must end up on the final reopened document (pre-fix: all six kept the
// freed pre-sign pointer).
TEST_F(SignPanelRebindTest, SuccessfulSignRebindsEveryPanelToTheReopenedDocument)
{
    const pdfe::core::Document* preSignDoc = mw_->documentForTesting();
    ASSERT_NE(preSignDoc, nullptr);
    // PREMISE: openFile() bound the panels — otherwise this test could pass
    // vacuously against panels that were never bound at all.
    expectPanelsBoundTo(preSignDoc, "openFile (premise)");

    const auto outPdf = std::filesystem::path(tempDir_->path().toStdString())
                        / "panel_rebind_signed.pdf";
    pdfe::ui::MainWindow::SignFlowTestInputs inputs;
    inputs.dialogAccepted = true;
    inputs.pfxPath = QString::fromStdString(pfx_.string());
    inputs.pfxPassword = QStringLiteral("test123");
    inputs.savePath = QString::fromStdString(outPdf.string());
    inputs.suppressErrorDialogs = true;
    mw_->setSignFlowTestInputsForTesting(inputs);

    mw_->triggerSignDocumentForTesting();
    QApplication::processEvents();

    ASSERT_TRUE(mw_->lastSigningErrorForTesting().isEmpty())
        << "premise: the sign must SUCCEED on this path; got: "
        << mw_->lastSigningErrorForTesting().toStdString();
    const pdfe::core::Document* reopened = mw_->documentForTesting();
    ASSERT_NE(reopened, nullptr);

    expectPanelsBoundTo(reopened, "a successful sign-reopen (GitHub #379)");

    // The measured crash shape: a comment-tree rebuild after the reopen.
    // With the rebind in place this walks the LIVE document. (Under the
    // debug CRT a pre-fix run dereferences 0xDD-filled freed memory here.)
    commentPanel_->refresh();
    QApplication::processEvents();
}

// RECOVERY path (#372's catch-path): a post-reopen failure reopens from the
// saved path; the panels must be rebound to THAT document too.
TEST_F(SignPanelRebindTest, PostReopenFailureRecoveryRebindsEveryPanel)
{
    const pdfe::core::Document* preSignDoc = mw_->documentForTesting();
    ASSERT_NE(preSignDoc, nullptr);
    expectPanelsBoundTo(preSignDoc, "openFile (premise)");

    const auto outPdf = std::filesystem::path(tempDir_->path().toStdString())
                        / "panel_rebind_recovery.pdf";
    const auto missingImage =
        std::filesystem::path(tempDir_->path().toStdString())
        / "no_such_image.png";
    ASSERT_FALSE(std::filesystem::exists(missingImage));

    pdfe::ui::MainWindow::SignFlowTestInputs inputs;
    inputs.dialogAccepted = true;
    inputs.pfxPath = QString::fromStdString(pfx_.string());
    inputs.pfxPassword = QStringLiteral("test123");   // probe succeeds
    inputs.savePath = QString::fromStdString(outPdf.string());
    inputs.imagePath = QString::fromStdString(missingImage.string());
    inputs.suppressErrorDialogs = true;
    mw_->setSignFlowTestInputsForTesting(inputs);

    mw_->triggerSignDocumentForTesting();
    QApplication::processEvents();

    // PREMISE: the injected failure fired AFTER the reopen (the save file
    // exists, and the error is the generic signing error, not the PFX one).
    const QString err = mw_->lastSigningErrorForTesting();
    ASSERT_FALSE(err.isEmpty());
    ASSERT_FALSE(err.contains(QStringLiteral("PFX")));
    ASSERT_TRUE(std::filesystem::exists(outPdf));

    const pdfe::core::Document* recovered = mw_->documentForTesting();
    ASSERT_NE(recovered, nullptr)
        << "the #372 recovery must leave a live document bound";

    expectPanelsBoundTo(recovered,
                        "the #372 catch-path recovery reopen (GitHub #379)");

    commentPanel_->refresh();
    QApplication::processEvents();
}
