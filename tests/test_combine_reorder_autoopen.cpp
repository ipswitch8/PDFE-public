// -*- coding: utf-8 -*-
// many-bugs-2026-11 phase-7:
//   GitHub #357 — Combine Files "Drag to reorder" hint must be real.
//   GitHub #358 — the combined PDF must open automatically after creation.
//
// #357: the step-1 label promised "Drag to reorder" but the QListWidget was
// left in its default NoDragDrop mode, so only the Move Up/Down buttons
// reordered anything. The fix sets QAbstractItemView::InternalMove. These
// tests assert the mode is set AND that a simulated internal-move drop (the
// take+insert QListWidget performs internally) changes the order that
// runCombine() consumes — i.e. inputFiles() reads the live widget, not a
// stale parallel vector.
//
// #358: onCombineFiles() ran wiz.exec() and discarded the result, and the
// wizard exposed no completion signal, so the user had to find the output on
// disk. The fix adds CombineFilesWizard::combineFinished(QString), emitted on
// a successful merge, and wires it to MainWindow::openFile(). The auto-open
// test replays that exact connection via connectCombineWizardForTesting() and
// observes openFile() via setOpenFileObserverForTesting(), so a real combine
// is driven end to end without the modal exec() loop.
#include <gtest/gtest.h>

#include "core/MuPdfContext.h"
#include "ui/CombineFilesWizard.h"
#include "ui/MainWindow.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCoreApplication>
#include <QListWidget>
#include <QListWidgetItem>
#include <QString>

#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace pdfe::core;
using namespace pdfe::ui;

namespace {

void ensureApp() {
    if (!QCoreApplication::instance()) {
        static int argc = 1;
        static const char* argv0 = "test_combine_reorder_autoopen";
        static char* argv[] = { const_cast<char*>(argv0), nullptr };
        // MainWindow needs a GUI application, not merely QCoreApplication.
        new QApplication(argc, argv);
    }
}

std::filesystem::path fixturePath(const char* name) {
    return std::filesystem::path(TEST_FIXTURES_DIR) / name;
}

std::filesystem::path tempOut(const char* tag) {
    return std::filesystem::temp_directory_path()
           / (std::string("pdfe_phase7_") + tag + "_"
              + std::to_string(std::time(nullptr)) + ".pdf");
}

} // namespace

class CombineReorderTest : public ::testing::Test {
protected:
    void SetUp() override {
        ensureApp();
        ctx = std::make_shared<MuPdfContext>();
    }
    std::shared_ptr<MuPdfContext> ctx;
};

// #357 (1): the file list is in InternalMove mode, so the "Drag to reorder"
// hint is true. Pre-fix the mode is the default NoDragDrop and this fails.
TEST_F(CombineReorderTest, FileListIsInternalMoveDraggable) {
    CombineFilesWizard wiz(ctx);
    QListWidget* lw = wiz.fileListForTesting();
    ASSERT_NE(lw, nullptr);
    EXPECT_EQ(lw->dragDropMode(), QAbstractItemView::InternalMove)
        << "the step-1 label promises drag-to-reorder; the list must enable it";
}

// #357 (2): a simulated internal-move drop reorders the path payload that
// runCombine() consumes. Proves inputFiles() reads the widget order (with the
// Qt::UserRole path carried on each moved row), not a stale vector.
TEST_F(CombineReorderTest, InternalMoveReordersPathPayload) {
    // Distinct synthetic paths — this test manipulates list rows only and
    // touches no files, so fixtures are unnecessary.
    const std::filesystem::path a = "/pdfe/test/aaa.pdf";
    const std::filesystem::path b = "/pdfe/test/bbb.pdf";
    const std::filesystem::path c = "/pdfe/test/ccc.pdf";

    CombineFilesWizard wiz(ctx);
    wiz.setInputFiles({a, b, c});

    QListWidget* lw = wiz.fileListForTesting();
    ASSERT_NE(lw, nullptr);
    ASSERT_EQ(lw->count(), 3);

    auto before = wiz.inputFiles();
    ASSERT_EQ(before.size(), 3u);
    EXPECT_EQ(before[0], a);
    EXPECT_EQ(before[1], b);
    EXPECT_EQ(before[2], c);

    // Reproduce exactly what QListWidget's InternalMove does on a drop: take
    // the row and re-insert it elsewhere. The item (and its Qt::UserRole path)
    // move together.
    QListWidgetItem* moved = lw->takeItem(0);
    ASSERT_NE(moved, nullptr);
    lw->insertItem(lw->count(), moved);  // 0 -> end: [b, c, a]

    auto after = wiz.inputFiles();
    ASSERT_EQ(after.size(), 3u);
    EXPECT_EQ(after[0], b);
    EXPECT_EQ(after[1], c);
    EXPECT_EQ(after[2], a)
        << "the moved row's file path must follow the row to its new position";
}

// #358: a successful combine opens the output automatically. The wizard emits
// combineFinished(path); MainWindow::openFile() receives it exactly once with
// the combined output path.
TEST_F(CombineReorderTest, SuccessfulCombineOpensOutputAutomatically) {
    auto a = fixturePath("test_3page.pdf");
    auto b = fixturePath("test_single.pdf");
    if (!std::filesystem::exists(a) || !std::filesystem::exists(b)) {
        GTEST_SKIP() << "fixtures missing";
    }

    MainWindow win;
    QString observedPath;
    int observedCalls = 0;
    win.setOpenFileObserverForTesting([&](const QString& p) {
        observedPath = p;
        ++observedCalls;
    });

    auto out = tempOut("autoopen");

    CombineFilesWizard wiz(ctx, &win);
    // Replay the production wiring (combineFinished -> openFile).
    win.connectCombineWizardForTesting(&wiz);
    wiz.setInputFiles({a, b});
    wiz.setOutputPath(out);

    auto r = wiz.runCombine();
    ASSERT_TRUE(r.ok) << r.errorMessage;
    QCoreApplication::processEvents();

    EXPECT_EQ(observedCalls, 1)
        << "the combined file must be opened exactly once on success";
    EXPECT_EQ(observedPath, QString::fromStdString(out.string()))
        << "openFile must receive the combined output path";

    std::error_code ec;
    std::filesystem::remove(out, ec);
}

// #358 negative control: a FAILED combine must not auto-open anything.
TEST_F(CombineReorderTest, FailedCombineOpensNothing) {
    auto a = fixturePath("test_3page.pdf");
    if (!std::filesystem::exists(a)) GTEST_SKIP() << "fixture missing";

    MainWindow win;
    int observedCalls = 0;
    win.setOpenFileObserverForTesting([&](const QString&) { ++observedCalls; });

    auto out = tempOut("failopen");

    CombineFilesWizard wiz(ctx, &win);
    win.connectCombineWizardForTesting(&wiz);
    // The failure path shows a modal QMessageBox::warning, which blocks a
    // headless run with no event loop to dismiss it; suppress only that dialog
    // (the returned Result + status text are unchanged) so this path is testable.
    wiz.setSuppressDialogsForTesting(true);
    // A single input fails the merge (needs >= 2).
    wiz.setInputFiles({a});
    wiz.setOutputPath(out);

    auto r = wiz.runCombine();
    ASSERT_FALSE(r.ok);
    QCoreApplication::processEvents();

    EXPECT_EQ(observedCalls, 0)
        << "combineFinished must not fire on failure, so nothing opens";

    std::error_code ec;
    std::filesystem::remove(out, ec);
}
