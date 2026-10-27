// -*- coding: utf-8 -*-
//
// Pipeline e4c71f3a phase-4 (GAP-102 + GAP-104): C++ regression tests
// for the final-sweep residual failures and hidden widget accessibility.

#include <gtest/gtest.h>

#include "annotations/AnnotationCommands.h"
#include "annotations/AnnotationManager.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/PageCommands.h"
#include "core/UndoManager.h"
#include "ui/BatchDialog.h"
#include "ui/CompareDialog.h"
#include "ui/ComplianceDialog.h"
#include "ui/PreferencesDialog.h"
#include "ui/MainWindow.h"
#include "app/AppSettings.h"

#include <QAction>
#include <QApplication>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QTabWidget>
#include <QTextEdit>

#include <filesystem>
#include <memory>

using namespace pdfe::core;
using namespace pdfe::commands;
using namespace pdfe::annotations;

namespace {
std::string fixturesDir() { return std::string(TEST_FIXTURES_DIR); }
}

// =====================================================================
// GAP-102 item 1: undo/redo critical-flow correctness
// =====================================================================

class UndoRedoFlowTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        fixture_ = std::filesystem::path(fixturesDir()) / "test_single.pdf";
        if (!std::filesystem::exists(fixture_))
            GTEST_SKIP() << "test_single.pdf missing";
    }
    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path fixture_;
};

TEST_F(UndoRedoFlowTest, UndoRedoAnnotationRoundtrip)
{
    auto doc = Document::open(ctx_, fixture_);
    ASSERT_NE(doc, nullptr);

    AnnotationManager annotMgr(ctx_);
    UndoManager undoMgr;
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    int countBefore = static_cast<int>(annotMgr.getAnnotations(page.get()).size());

    auto cmd = std::make_unique<CreateAnnotationCommand>(
        &annotMgr, page.get(), AnnotationType::Text, QRectF(100, 100, 30, 30));
    undoMgr.execute(std::move(cmd));

    EXPECT_EQ(static_cast<int>(annotMgr.getAnnotations(page.get()).size()), countBefore + 1);

    undoMgr.undo();
    EXPECT_EQ(static_cast<int>(annotMgr.getAnnotations(page.get()).size()), countBefore);

    undoMgr.redo();
    EXPECT_EQ(static_cast<int>(annotMgr.getAnnotations(page.get()).size()), countBefore + 1);
}

TEST_F(UndoRedoFlowTest, UndoRedoMultipleAnnotations)
{
    auto doc = Document::open(ctx_, fixture_);
    ASSERT_NE(doc, nullptr);

    AnnotationManager annotMgr(ctx_);
    UndoManager undoMgr;
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    int base = static_cast<int>(annotMgr.getAnnotations(page.get()).size());

    for (int i = 0; i < 5; ++i) {
        auto cmd = std::make_unique<CreateAnnotationCommand>(
            &annotMgr, page.get(), AnnotationType::Text,
            QRectF(50.0 + i * 40, 50, 30, 30));
        undoMgr.execute(std::move(cmd));
    }
    EXPECT_EQ(static_cast<int>(annotMgr.getAnnotations(page.get()).size()), base + 5);

    for (int i = 0; i < 5; ++i) undoMgr.undo();
    EXPECT_EQ(static_cast<int>(annotMgr.getAnnotations(page.get()).size()), base);

    for (int i = 0; i < 3; ++i) undoMgr.redo();
    EXPECT_EQ(static_cast<int>(annotMgr.getAnnotations(page.get()).size()), base + 3);
}

// =====================================================================
// GAP-102 item 3: rotate-CCW command correctness
// =====================================================================

TEST_F(UndoRedoFlowTest, RotateCCWCommandWorks)
{
    auto doc = Document::open(ctx_, fixture_);
    ASSERT_NE(doc, nullptr);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    int origRotation = page->rotation();

    UndoManager undoMgr;
    auto cmd = std::make_unique<RotatePageCommand>(doc.get(), 0, -90);
    undoMgr.execute(std::move(cmd));

    auto page2 = doc->page(0);
    ASSERT_NE(page2, nullptr);
    int newRotation = page2->rotation();
    EXPECT_EQ(newRotation, (origRotation + 270) % 360);

    undoMgr.undo();
    auto page3 = doc->page(0);
    EXPECT_EQ(page3->rotation(), origRotation);
}

TEST_F(UndoRedoFlowTest, RotateCWAndCCWCancel)
{
    auto doc = Document::open(ctx_, fixture_);
    ASSERT_NE(doc, nullptr);

    auto page = doc->page(0);
    int origRotation = page->rotation();

    UndoManager undoMgr;
    undoMgr.execute(std::make_unique<RotatePageCommand>(doc.get(), 0, 90));
    undoMgr.execute(std::make_unique<RotatePageCommand>(doc.get(), 0, -90));

    auto page2 = doc->page(0);
    EXPECT_EQ(page2->rotation(), origRotation)
        << "CW+CCW must restore original rotation";
}

// =====================================================================
// GAP-104: dialog widget accessibility
// =====================================================================

class DialogWidgetAccessTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!qApp) GTEST_SKIP() << "QApplication not available";
        ctx_ = std::make_shared<MuPdfContext>();
        fixture_ = std::filesystem::path(fixturesDir()) / "test_single.pdf";
        if (!std::filesystem::exists(fixture_))
            GTEST_SKIP() << "test_single.pdf missing";
        doc_ = Document::open(ctx_, fixture_);
        if (!doc_) GTEST_SKIP() << "Could not open fixture";
    }
    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path fixture_;
    std::shared_ptr<Document> doc_;
};

TEST_F(DialogWidgetAccessTest, ComplianceProgressBarReachable)
{
    pdfe::ui::ComplianceDialog dlg(ctx_, doc_.get());
    auto* pb = dlg.findChild<QProgressBar*>("progressBar");
    EXPECT_NE(pb, nullptr);
    EXPECT_EQ(dlg.objectName(), "ComplianceDialog");
}

TEST_F(DialogWidgetAccessTest, BatchDialogWidgetsReachable)
{
    pdfe::ui::BatchDialog dlg(ctx_);
    auto* actionList = dlg.findChild<QListWidget*>("actionListWidget");
    auto* errorLog = dlg.findChild<QTextEdit*>("batchErrorLog");
    auto* progressBar = dlg.findChild<QProgressBar*>("batchProgressBar");

    EXPECT_NE(actionList, nullptr);
    EXPECT_NE(errorLog, nullptr);
    EXPECT_NE(progressBar, nullptr);

    if (actionList)
        EXPECT_FALSE(actionList->accessibleName().isEmpty());
    if (errorLog)
        EXPECT_FALSE(errorLog->accessibleName().isEmpty());
}

TEST_F(DialogWidgetAccessTest, CompareDialogReachable)
{
    pdfe::ui::CompareDialog dlg(ctx_);
    EXPECT_EQ(dlg.objectName(), "compareDialog");

    auto* btn = dlg.findChild<QPushButton*>("compareButton");
    EXPECT_NE(btn, nullptr);
}

TEST_F(DialogWidgetAccessTest, BatchErrorLogStartsHidden)
{
    pdfe::ui::BatchDialog dlg(ctx_);
    auto* errorLog = dlg.findChild<QTextEdit*>("batchErrorLog");
    ASSERT_NE(errorLog, nullptr);
    EXPECT_FALSE(errorLog->isVisible());
}

TEST_F(DialogWidgetAccessTest, BatchDialogHasObjectName)
{
    pdfe::ui::BatchDialog dlg(ctx_);
    EXPECT_EQ(dlg.objectName(), "batchDialog");
}

TEST_F(DialogWidgetAccessTest, CompareDialogHasObjectName)
{
    pdfe::ui::CompareDialog dlg(ctx_);
    EXPECT_EQ(dlg.objectName(), "compareDialog");
}

TEST_F(DialogWidgetAccessTest, PreferencesTabWidgetReachable)
{
    pdfe::app::AppSettings settings;
    pdfe::ui::PreferencesDialog dlg(&settings);
    auto* tabs = dlg.findChild<QTabWidget*>("preferencesTabs");
    EXPECT_NE(tabs, nullptr);
    if (tabs) EXPECT_GE(tabs->count(), 3);
    EXPECT_EQ(dlg.objectName(), "preferencesDialog");
}

// =====================================================================
// GAP-102 item 2: File→Open menu action accessibility
// =====================================================================

TEST(MenuActionAccessibility, FileOpenActionHasObjectName)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    pdfe::ui::MainWindow mw;
    auto* openAction = mw.findChild<QAction*>("openAction");
    EXPECT_NE(openAction, nullptr)
        << "GAP-102: openAction must be discoverable by objectName";
    if (openAction)
        EXPECT_FALSE(openAction->text().isEmpty());
}

TEST(MenuActionAccessibility, UndoRedoActionsHaveObjectName)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    pdfe::ui::MainWindow mw;
    auto* undo = mw.findChild<QAction*>("undoAction");
    auto* redo = mw.findChild<QAction*>("redoAction");
    EXPECT_NE(undo, nullptr);
    EXPECT_NE(redo, nullptr);
}

TEST(MenuActionAccessibility, RotateActionsHaveObjectName)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    pdfe::ui::MainWindow mw;
    auto* cw = mw.findChild<QAction*>("rotatePageCWAction");
    auto* ccw = mw.findChild<QAction*>("rotatePageCCWAction");
    EXPECT_NE(cw, nullptr);
    EXPECT_NE(ccw, nullptr);
}

TEST(MenuActionAccessibility, IsPrintEnabledSeamWorks)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    pdfe::ui::MainWindow mw;
    bool ok = false;
    EXPECT_TRUE(QMetaObject::invokeMethod(
        &mw, "isPrintEnabled", Q_RETURN_ARG(bool, ok)));
}

TEST(MenuActionAccessibility, DeleteSelectedAnnotationSeamWorks)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    pdfe::ui::MainWindow mw;
    bool ok = false;
    EXPECT_TRUE(QMetaObject::invokeMethod(
        &mw, "deleteSelectedAnnotation", Q_RETURN_ARG(bool, ok)))
        << "deleteSelectedAnnotation must be invokable via QMetaObject";
    EXPECT_FALSE(ok)
        << "Should return false when no annotation is selected";
}
