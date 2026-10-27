// -*- coding: utf-8 -*-
//
// Comprehensive Q_INVOKABLE seam coverage test. Exercises every
// Q_INVOKABLE method via QMetaObject::invokeMethod to verify the
// pywinauto dispatch path works.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "ui/PdfCanvas.h"
#include "ui/PagePanel.h"
#include "ui/CustomizeToolbarsDialog.h"
#include "ui/AtlasComparisonDialog.h"
#include "tre/EnrichedAtlas.h"
#include "tre/BitmapCompositor.h"
#include "tre/VisualFontRanker.h"
#include "tre/AtlasMode.h"

#include <QAction>
#include <QImage>
#include <QApplication>
#include <QMetaObject>
#include <QRectF>

#include <filesystem>
#include <memory>

using namespace pdfe::core;

namespace {
std::string fixturesDir() { return std::string(TEST_FIXTURES_DIR); }
}

class QInvokableCoverageTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!qApp) GTEST_SKIP() << "QApplication not available";
    }
};

// =====================================================================
// PdfCanvas seams — verify invokable dispatch works
// =====================================================================

TEST_F(QInvokableCoverageTest, PdfCanvas_ViewRotation_Invokable)
{
    pdfe::ui::PdfCanvas canvas(nullptr);
    int rotation = -1;
    EXPECT_TRUE(QMetaObject::invokeMethod(
        &canvas, "viewRotation", Q_RETURN_ARG(int, rotation)));
    EXPECT_EQ(rotation, 0);
}

TEST_F(QInvokableCoverageTest, PdfCanvas_PdfRectFromCanvas_Invokable)
{
    pdfe::ui::PdfCanvas canvas(nullptr);
    QRectF result;
    EXPECT_TRUE(QMetaObject::invokeMethod(
        &canvas, "pdfRectFromCanvas",
        Q_RETURN_ARG(QRectF, result),
        Q_ARG(int, 0),
        Q_ARG(QRectF, QRectF(0, 0, 100, 100))));
}

TEST_F(QInvokableCoverageTest, PdfCanvas_CanvasRectFromPdf_Invokable)
{
    pdfe::ui::PdfCanvas canvas(nullptr);
    QRectF result;
    EXPECT_TRUE(QMetaObject::invokeMethod(
        &canvas, "canvasRectFromPdf",
        Q_RETURN_ARG(QRectF, result),
        Q_ARG(int, 0),
        Q_ARG(QRectF, QRectF(0, 0, 100, 100))));
}

TEST_F(QInvokableCoverageTest, PdfCanvas_AddRedactionMarkAtPdfRect_Invokable)
{
    pdfe::ui::PdfCanvas canvas(nullptr);
    bool ok = true;
    EXPECT_TRUE(QMetaObject::invokeMethod(
        &canvas, "addRedactionMarkAtPdfRect",
        Q_RETURN_ARG(bool, ok),
        Q_ARG(int, 0),
        Q_ARG(QRectF, QRectF(0, 0, 100, 50))));
    EXPECT_FALSE(ok) << "Should fail with no document loaded";
}

TEST_F(QInvokableCoverageTest, PdfCanvas_ClickByPdfCoord_Invokable)
{
    pdfe::ui::PdfCanvas canvas(nullptr);
    bool ok = true;
    EXPECT_TRUE(QMetaObject::invokeMethod(
        &canvas, "clickByPdfCoord",
        Q_RETURN_ARG(bool, ok),
        Q_ARG(int, 0),
        Q_ARG(qreal, 100.0), Q_ARG(qreal, 200.0)));
    EXPECT_FALSE(ok) << "Should fail with no document";
}

TEST_F(QInvokableCoverageTest, PdfCanvas_DragByPdfCoord_Invokable)
{
    pdfe::ui::PdfCanvas canvas(nullptr);
    bool ok = true;
    EXPECT_TRUE(QMetaObject::invokeMethod(
        &canvas, "dragByPdfCoord",
        Q_RETURN_ARG(bool, ok),
        Q_ARG(int, 0),
        Q_ARG(qreal, 100.0), Q_ARG(qreal, 200.0),
        Q_ARG(qreal, 200.0), Q_ARG(qreal, 300.0)));
    EXPECT_FALSE(ok) << "Should fail with no document";
}

TEST_F(QInvokableCoverageTest, PdfCanvas_TripleClickByPdfCoord_Invokable)
{
    pdfe::ui::PdfCanvas canvas(nullptr);
    bool ok = true;
    EXPECT_TRUE(QMetaObject::invokeMethod(
        &canvas, "tripleClickByPdfCoord",
        Q_RETURN_ARG(bool, ok),
        Q_ARG(int, 0),
        Q_ARG(qreal, 100.0), Q_ARG(qreal, 200.0)));
    EXPECT_FALSE(ok) << "Should fail with no document";
}

TEST_F(QInvokableCoverageTest, PdfCanvas_ScrollByPixels_Invokable)
{
    pdfe::ui::PdfCanvas canvas(nullptr);
    bool ok = true;
    EXPECT_TRUE(QMetaObject::invokeMethod(
        &canvas, "scrollByPixels",
        Q_RETURN_ARG(bool, ok),
        Q_ARG(int, 100)));
}

// =====================================================================
// PagePanel seams
// =====================================================================

TEST_F(QInvokableCoverageTest, PagePanel_DragItemProgrammatic_Invokable)
{
    pdfe::ui::PagePanel panel(nullptr);
    bool ok = true;
    EXPECT_TRUE(QMetaObject::invokeMethod(
        &panel, "dragItemProgrammatic",
        Q_RETURN_ARG(bool, ok),
        Q_ARG(int, 0), Q_ARG(int, 1)));
    EXPECT_FALSE(ok) << "Should fail with no document";
}

// =====================================================================
// CustomizeToolbarsDialog seams
// =====================================================================

TEST_F(QInvokableCoverageTest, CustomizeToolbarsDialog_AddAction_Invokable)
{
    QAction testAction(nullptr);
    testAction.setObjectName("testAction");
    std::vector<QAction*> registry = {&testAction};
    std::vector<pdfe::ui::ToolbarLayout> layouts;
    pdfe::ui::CustomizeToolbarsDialog dlg(registry, layouts);

    bool ok = true;
    EXPECT_TRUE(QMetaObject::invokeMethod(
        &dlg, "addActionByObjectName",
        Q_RETURN_ARG(bool, ok),
        Q_ARG(QString, QString("nonexistent"))));
    EXPECT_FALSE(ok) << "Should fail for unknown action name";
}

TEST_F(QInvokableCoverageTest, CustomizeToolbarsDialog_RemoveAction_Invokable)
{
    std::vector<QAction*> registry;
    std::vector<pdfe::ui::ToolbarLayout> layouts;
    pdfe::ui::CustomizeToolbarsDialog dlg(registry, layouts);

    bool ok = true;
    EXPECT_TRUE(QMetaObject::invokeMethod(
        &dlg, "removeActionByObjectName",
        Q_RETURN_ARG(bool, ok),
        Q_ARG(QString, QString("nonexistent"))));
    EXPECT_FALSE(ok) << "Should fail for unknown action";
}

TEST_F(QInvokableCoverageTest, CustomizeToolbarsDialog_ResetToDefaults_Invokable)
{
    std::vector<QAction*> registry;
    std::vector<pdfe::ui::ToolbarLayout> layouts;
    pdfe::ui::CustomizeToolbarsDialog dlg(registry, layouts);

    EXPECT_TRUE(QMetaObject::invokeMethod(&dlg, "resetToDefaults"));
}

// =====================================================================
// AtlasComparisonDialog seam
// =====================================================================

TEST_F(QInvokableCoverageTest, AtlasComparisonDialog_IsAtlasReady_Invokable)
{
    QImage emptyImage;
    std::vector<pdfe::tre::VisualFontRanker::RankedCandidate> candidates;
    pdfe::ui::AtlasComparisonDialog dlg(
        nullptr, nullptr, emptyImage, QString(),
        candidates, nullptr, 12.0f, 300);

    bool ready = true;
    EXPECT_TRUE(QMetaObject::invokeMethod(
        &dlg, "isAtlasReady", Q_RETURN_ARG(bool, ready)));
    EXPECT_FALSE(ready) << "Should return false with null atlases";
}
