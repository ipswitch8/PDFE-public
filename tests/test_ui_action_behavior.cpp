// -*- coding: utf-8 -*-
//
// Harness-remediation PHASE-6, register target R-20 (High) —
// widget-exists-only → REAL behavioral assertions.
//
// OLD WEAKNESS
// ------------
// The highest-traffic UI oracles asserted only that a control EXISTS: e.g.
// test_ui_integration.cpp's AllActionsExist / *HasStandardShortcut assert
// findChild<QAction*>("saveAction") != nullptr, that openAction owns Ctrl+O,
// etc. A build in which Save silently did nothing, Open loaded no model, or
// Find opened no panel would still pass every one of those — the action's
// EFFECT was never exercised. (A missing control was also frequently a SKIP,
// not a failure.)
//
// NEW ORACLE (this file)
// ----------------------
// For the highest-value actions — Open, Save, Find — assert the ACTION'S EFFECT
// on document/model state, headlessly, through the real QAction/slot path. A
// missing control is a hard FAILURE (ASSERT_NE), never a skip. These strengthen
// the "widget exists" checks into "the action changes state to the expected
// value":
//   * Open   — after openFile the document model is non-null with the expected
//              page count and known page text (open LOADS a model).
//   * Save   — a Replace-All edit, saved via the real saveAction QAction, is
//              PERSISTED TO DISK: reopening the file in a fresh window shows the
//              replacement and not the original (Save writes the edit).
//   * Find   — triggering the real findAction SHOWS the search panel dock (the
//              action has its user-visible effect), and the model actually
//              matches known text (Replace-All count > 0 on a known token).
//
// Runs headless under test_main.cpp's QApplication + crash guards.

#include <gtest/gtest.h>

#include "ui/MainWindow.h"
#include "core/Document.h"
#include "core/Page.h"

#include <QApplication>
#include <QAction>
#include <QDockWidget>
#include <QString>

#include <filesystem>
#include <string>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

namespace {

using pdfe::ui::MainWindow;

std::filesystem::path fixturesDir() {
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

// Three-page fixture with a known 18pt title per page (PAGEBIG0..2) and a
// repeated 9pt body token (REPLNINE) — deterministic, replace-able native text.
const char* kFixture = "multipage_smalltext_edit.pdf";

QAction* action(MainWindow& mw, const QString& name) {
    return mw.findChild<QAction*>(name);
}

// ── Open: the action LOADS a real document model (not just "openAction exists")
TEST(UiActionBehavior, OpenActionLoadsDocumentModel) {
    auto pdf = fixturesDir() / kFixture;
    ASSERT_TRUE(std::filesystem::exists(pdf)) << pdf.string();

    MainWindow mw;
    QApplication::processEvents();

    // Behavioral precondition: no model before Open.
    EXPECT_EQ(mw.documentForTesting(), nullptr)
        << "no document should be loaded before Open";

    // The openAction control must exist (a missing control is a FAILURE).
    ASSERT_NE(action(mw, "openAction"), nullptr)
        << "openAction must exist";

    mw.openFile(QString::fromStdString(pdf.string()));
    QApplication::processEvents();

    auto* doc = mw.documentForTesting();
    ASSERT_NE(doc, nullptr) << "Open must load a document model";
    EXPECT_EQ(doc->pageCount(), 3) << "the fixture has 3 pages";
    auto page0 = doc->page(0);
    ASSERT_NE(page0, nullptr);
    EXPECT_NE(page0->extractPlainText().find("PAGEBIG0"), std::string::npos)
        << "Open must load the page content, not an empty model";
}

// ── Save: the action PERSISTS a model edit to disk.
TEST(UiActionBehavior, SaveActionPersistsReplaceAllToDisk) {
    auto src = fixturesDir() / kFixture;
    ASSERT_TRUE(std::filesystem::exists(src)) << src.string();

    // Work on a temp copy so File>Save writes in place without touching the
    // committed fixture.
    auto temp = std::filesystem::temp_directory_path() /
                "pdfe_r20_save_persist.pdf";
    std::error_code ec;
    std::filesystem::remove(temp, ec);
    std::filesystem::copy_file(
        src, temp, std::filesystem::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();

    {
        MainWindow mw;
        QApplication::processEvents();
        mw.openFile(QString::fromStdString(temp.string()));
        QApplication::processEvents();
        ASSERT_NE(mw.documentForTesting(), nullptr);

        // The Replace-All edit (model-state change through the real path).
        mw.replaceAllForTesting("REPLNINE", "DONE9SAVE");
        EXPECT_EQ(mw.lastReplaceAllCountForTesting(), 3)
            << "Replace All must report the 3 landed replacements";

        // Exercise the REAL Save action (a missing control is a FAILURE).
        auto* saveAct = action(mw, "saveAction");
        ASSERT_NE(saveAct, nullptr) << "saveAction must exist";
        saveAct->trigger();  // MainWindow::onSaveFile — in-place full-rewrite
        QApplication::processEvents();
    }  // window destroyed; file closed

    // Reopen the SAVED FILE in a fresh window and prove the edit persisted.
    {
        MainWindow mw2;
        QApplication::processEvents();
        mw2.openFile(QString::fromStdString(temp.string()));
        QApplication::processEvents();
        auto* doc = mw2.documentForTesting();
        ASSERT_NE(doc, nullptr);
        ASSERT_EQ(doc->pageCount(), 3);

        int landedOnDisk = 0;
        for (int i = 0; i < doc->pageCount(); ++i) {
            auto page = doc->page(i);
            ASSERT_NE(page, nullptr);
            const std::string t = page->extractPlainText();
            EXPECT_EQ(t.find("REPLNINE"), std::string::npos)
                << "page " << i << ": original token survived on disk — Save "
                   "did not persist the edit";
            for (size_t p = t.find("DONE9SAVE"); p != std::string::npos;
                 p = t.find("DONE9SAVE", p + 1))
                ++landedOnDisk;
        }
        EXPECT_EQ(landedOnDisk, 3)
            << "the Save action must persist all 3 replacements to disk";
    }

    std::filesystem::remove(temp, ec);
}

// ── Find: the action SHOWS the search panel (its user-visible effect).
TEST(UiActionBehavior, FindActionShowsSearchPanel) {
    auto pdf = fixturesDir() / kFixture;
    ASSERT_TRUE(std::filesystem::exists(pdf)) << pdf.string();

    MainWindow mw;
    QApplication::processEvents();
    mw.openFile(QString::fromStdString(pdf.string()));
    QApplication::processEvents();

    auto* dock = mw.findChild<QDockWidget*>("searchPanelDock");
    ASSERT_NE(dock, nullptr) << "searchPanelDock must exist";

    // Force a known 'hidden' starting state so the assertion measures the
    // action's EFFECT, not an incidental default.
    dock->hide();
    QApplication::processEvents();
    ASSERT_TRUE(dock->isHidden());

    auto* findAct = action(mw, "findAction");
    ASSERT_NE(findAct, nullptr) << "findAction must exist";
    findAct->trigger();  // MainWindow::onFind — shows + raises the search dock
    QApplication::processEvents();

    EXPECT_FALSE(dock->isHidden())
        << "triggering Find must SHOW the search panel, not merely exist";
}

}  // namespace
