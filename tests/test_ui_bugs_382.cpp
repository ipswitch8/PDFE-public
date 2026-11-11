// -*- coding: utf-8 -*-
//
// GitHub #382 — non-UIA regression oracles for the three deterministic UI
// regressions the v0.5.1 sweep surfaced. All three were caught only by the
// pywinauto matrix because no C++ test opened a document into a live
// MainWindow and checked dirty-after-undo, the status-bar form-field wiring, or
// the UI UNC error text. These gtests re-home the real invariants off the
// flaky/unreachable UIA layer (ui-bugs-381-383 phase-3).
//
// MainWindow is heap-allocated via a fixture: stack-allocating it overflows the
// test thread's CRT stack-guard region (see test_main_window_q_invokable.cpp).

#include <gtest/gtest.h>

#include <QApplication>
#include <QLabel>
#include <QMessageBox>
#include <QMetaObject>
#include <QString>
#include <QTimer>
#include <QWidget>

#include "ui/MainWindow.h"
#include "ui/StatusBar.h"
#include "app/AppSettings.h"

#include <filesystem>
#include <memory>

using namespace pdfe::ui;

namespace {

std::filesystem::path fixture(const char* name) {
    return std::filesystem::path(TEST_FIXTURES_DIR) / name;
}

void settle() {
    for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
}

// Promote the protected dirty-state accessor so the test can read it without
// reimplementing the undo-stack clean-index logic it verifies.
class TestMainWindow : public MainWindow {
public:
    using MainWindow::hasUnsavedChanges;
};

class UiBugs382Test : public ::testing::Test {
protected:
    void SetUp() override { mw_ = std::make_unique<TestMainWindow>(); }
    void TearDown() override { mw_.reset(); }
    std::unique_ptr<TestMainWindow> mw_;
};

}  // namespace

// ─── #382(a) — undo of a delete returns the document to CLEAN (not dirty) ────
//
// The old oracle (test_final_phase8.py annot_select_delete) asserted the
// document STAYS dirty after undoing the only change back to the as-opened
// baseline. That is wrong: dirty state is the undo-stack clean index
// (UndoManager::updateCleanState), so undoing to the saved point is clean. A
// harness focus-recovery fix merely started landing the Ctrl+Z, exposing the
// bad assertion. The real invariant: open=clean -> delete=dirty -> undo=clean.
TEST_F(UiBugs382Test, UndoOfDeleteReturnsDocumentToClean) {
    const auto pdf = fixture("test_with_freetext_annot.pdf");
    if (!std::filesystem::exists(pdf))
        GTEST_SKIP() << "test_with_freetext_annot.pdf fixture unavailable";

    mw_->openFile(QString::fromStdString(pdf.string()));
    settle();
    ASSERT_FALSE(mw_->hasUnsavedChanges())
        << "a freshly opened document must be clean (no unsaved changes)";

    // Select annotation 0 on page 0 and delete it.
    mw_->setSelectedAnnotationForTesting(0, 0);
    bool deleted = false;
    ASSERT_TRUE(QMetaObject::invokeMethod(
        mw_.get(), "deleteSelectedAnnotation",
        Qt::DirectConnection, Q_RETURN_ARG(bool, deleted)));
    ASSERT_TRUE(deleted)
        << "fixture precondition: annotation 0 of page 0 must exist and be "
           "deletable — otherwise this guard is vacuous";
    settle();
    EXPECT_TRUE(mw_->hasUnsavedChanges())
        << "deleting an annotation must dirty the document";

    // Undo the delete — back to the as-opened baseline.
    ASSERT_TRUE(QMetaObject::invokeMethod(mw_.get(), "onUndo",
                                          Qt::DirectConnection));
    settle();
    EXPECT_FALSE(mw_->hasUnsavedChanges())
        << "undoing the only change back to the saved baseline must return the "
           "document to CLEAN (undo-stack clean index) — the assertion the old "
           "oracle had inverted (GitHub #382a)";

    // Restoration is real: the annotation is back and selectable/deletable
    // again (proves the undo restored it, not merely cleared the flag).
    mw_->setSelectedAnnotationForTesting(0, 0);
    bool deletedAgain = false;
    ASSERT_TRUE(QMetaObject::invokeMethod(
        mw_.get(), "deleteSelectedAnnotation",
        Qt::DirectConnection, Q_RETURN_ARG(bool, deletedAgain)));
    EXPECT_TRUE(deletedAgain)
        << "after undo the annotation must be restored (re-deletable)";
}

// ─── #382(b) — the status-bar form-field-count label tracks the document ─────
//
// The UIA oracle (test_ui_statusbar.py) failed at the reachability step: a
// permanent QLabel in a QStatusBar is not UIA-discoverable. The label IS
// created+wired (StatusBar.cpp:36-44; MainWindow open-wiring sums per-page
// fields -> setFormFieldCount). This re-homes the invariant to a direct read of
// the label text, asserting the COUNT tracks the open document (not merely that
// the widget exists). The label-to-string mapping is separately locked by
// test_theme_and_field_count.cpp::StatusBarFormFieldCount.
TEST_F(UiBugs382Test, StatusBarFormFieldCountTracksTheDocument) {
    auto* sb = mw_->findChild<StatusBar*>();
    ASSERT_NE(sb, nullptr) << "MainWindow has no StatusBar";
    auto* label = sb->findChild<QLabel*>("formFieldCount");
    ASSERT_NE(label, nullptr) << "formFieldCount label missing";

    // A form document -> a positive, wired count (NOT the unwired default).
    const auto formPdf = fixture("test_form.pdf");
    if (!std::filesystem::exists(formPdf))
        GTEST_SKIP() << "test_form.pdf fixture unavailable";
    mw_->openFile(QString::fromStdString(formPdf.string()));
    settle();
    const QString formText = label->text();
    EXPECT_NE(formText, QString("No form fields"))
        << "after opening a form document the status bar still reads the "
           "unwired default — the open-wiring did not update the count "
           "(GitHub #382b)";
    // Parse the leading integer and assert it is a real positive count.
    bool okNum = false;
    const int n = formText.section(' ', 0, 0).toInt(&okNum);
    EXPECT_TRUE(okNum && n >= 1)
        << "form-field count label did not show a positive integer count; "
           "text was: " << formText.toStdString();

    // A non-form document -> the count resets to zero. Proves the label tracks
    // the document both ways, not a stuck value.
    const auto plainPdf = fixture("test_single.pdf");
    if (std::filesystem::exists(plainPdf)) {
        mw_->openFile(QString::fromStdString(plainPdf.string()));
        settle();
        EXPECT_EQ(label->text(), QString("No form fields"))
            << "opening a non-form document must reset the form-field count to "
               "zero; the label did not track the new document";
    }
}

// ─── #382(c) — the UNC-unreachable Open error names the UNC nature ───────────
//
// The Unreachable branch's main text had dropped "(UNC)" (d085ae2b, #240),
// leaving it only in the informative text the error oracle does not read, which
// quietly weakened the #91 guarantee. This reads the actual QMessageBox main
// text via a modal watcher (QMessageBox::text() — no UIA, no pipe seam) and
// asserts it names the UNC nature. Fails against the pre-fix string.
TEST_F(UiBugs382Test, UncUnreachableOpenErrorNamesUnc) {
    // Default is allow-user-network-paths = TRUE, so an unreachable UNC lands
    // in the Unreachable branch (not Refused). Make it explicit + restore it.
    pdfe::app::AppSettings settings;
    const bool saved = settings.allowUserNetworkPaths();
    settings.setAllowUserNetworkPaths(true);
    settings.sync();

    mw_->setNetworkProbeTimeoutMsForTesting(300);

    // Watch for the modal "openFileErrorDialog" QMessageBox, capture its main
    // text, and close it so openFile()'s exec() returns.
    QString capturedText;
    QTimer watcher;
    QObject::connect(&watcher, &QTimer::timeout, [&]() {
        for (QWidget* w : QApplication::topLevelWidgets()) {
            auto* mb = qobject_cast<QMessageBox*>(w);
            if (mb && mb->objectName() == "openFileErrorDialog"
                && mb->isVisible()) {
                if (capturedText.isEmpty()) capturedText = mb->text();
                mb->close();
            }
        }
    });
    watcher.start(20);

    const QString unc =
        QStringLiteral("\\\\pdfe-test-unreachable-host\\share\\doc.pdf");
    mw_->openFile(unc);
    settle();
    watcher.stop();

    settings.setAllowUserNetworkPaths(saved);
    settings.sync();

    ASSERT_FALSE(capturedText.isEmpty())
        << "no openFileErrorDialog main text was captured — the Unreachable "
           "branch did not report the failure (or the dialog objectName "
           "changed)";
    EXPECT_TRUE(capturedText.contains(QStringLiteral("UNC")))
        << "the unreachable-UNC error main text does not name the UNC nature "
           "(GitHub #382c regression); text was: " << capturedText.toStdString();
    EXPECT_TRUE(capturedText.contains(QStringLiteral("network")))
        << "the error main text does not mention the network location; text "
           "was: " << capturedText.toStdString();
}
