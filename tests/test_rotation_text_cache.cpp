// -*- coding: utf-8 -*-
//
// GitHub #334 regression — editing text after rotating a page must read
// POST-rotation text, not pre-rotation text.
//
// ROOT CAUSE (see docs/audit/ui-reorder-rotate-2026-11-investigation.md, Bug B)
// ----------------------------------------------------------------------------
// Document::rotatePage() updates the page /Rotate and the rotate handlers call
// MainWindow::onDocumentStructureChanged(), which invalidated the render tile
// cache but NONE of the page-index-keyed text-extraction caches that feed the
// selection-rect → text path. So the bitmap and the click→PDF mapping updated
// while three caches kept pre-rotation geometry:
//
//   1. CoordMapper::cache_   — the AffineTransform bakes pg->rotation().
//   2. OcrPipeline HOCR cache — the scanned-page (TRE region) layout.
//   3. TextEditTool::model_   — live-text span boundingBox/charQuads;
//                               setPage() early-returns on the unchanged index.
//
// The phase-3 fix invalidates all three in onDocumentStructureChanged(),
// symmetric to the existing tileCache_->invalidateAll(). These tests drive the
// REAL rotate QAction (MainWindow::onRotatePageCW → onDocumentStructureChanged)
// and assert each cache reflects the NEW rotation — i.e. was invalidated. Each
// FAILS against the pre-fix tree (the stale cached value survives) and PASSES
// against the fix.
//
// Runs headless under test_main.cpp's QApplication + crash guards.

#include <gtest/gtest.h>

#include "ui/MainWindow.h"
#include "ui/PdfCanvas.h"
#include "textedit/TextEditTool.h"
#include "tre/CoordMapper.h"
#include "tre/OcrPipeline.h"
#include "core/Document.h"

#include <QAction>
#include <QApplication>
#include <QRectF>
#include <QString>

#include <filesystem>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

namespace {

using pdfe::ui::MainWindow;

// Native-text, three-page fixture (known 18pt title + 9pt body per page), so
// page 0 yields hit-testable live-text spans for the TextEditTool path.
const char* kFixture = "multipage_smalltext_edit.pdf";

class RotationTextCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        fixture_ = std::filesystem::path(TEST_FIXTURES_DIR) / kFixture;
        if (!std::filesystem::exists(fixture_)) {
            GTEST_SKIP() << "Fixture missing: " << fixture_.string();
        }
    }

    static QAction* action(MainWindow& mw, const QString& name) {
        return mw.findChild<QAction*>(name);
    }

    // Normalize a raw degree value into [0, 360).
    static int norm(int deg) { return ((deg % 360) + 360) % 360; }

    std::filesystem::path fixture_;
};

// ─────────────────────────────────────────────────────────────────────
// Cache 1 — CoordMapper AffineTransform reflects the new page rotation.
// ─────────────────────────────────────────────────────────────────────
TEST_F(RotationTextCacheTest, CoordMapperTransformReflectsRotationAfterRotate) {
    MainWindow mw;
    QApplication::processEvents();
    mw.openFile(QString::fromStdString(fixture_.string()));
    QApplication::processEvents();

    auto* doc = mw.documentForTesting();
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);

    mw.initTreComponentsForTesting();
    auto* cm = mw.coordMapperForTesting();
    ASSERT_NE(cm, nullptr) << "TRE CoordMapper must exist after init";

    // Warm the page-0 transform at the ORIGINAL rotation (caches it).
    const int startRot = norm(doc->currentRotation(0));
    const int warmedRotation = cm->getTransform(0).rotation;
    ASSERT_EQ(warmedRotation, startRot)
        << "warmed transform should match the page's starting /Rotate";

    // Drive the REAL rotate action (+90 CW). With no page-panel selection it
    // targets the current page (0).
    auto* rot = action(mw, "rotatePageCWAction");
    ASSERT_NE(rot, nullptr) << "rotatePageCWAction must exist";
    ASSERT_TRUE(rot->isEnabled()) << "rotate must be enabled with a document open";
    rot->trigger();
    QApplication::processEvents();

    const int newRot = norm(doc->currentRotation(0));
    ASSERT_EQ(newRot, norm(startRot + 90))
        << "precondition: the CW rotate must land +90 on page 0";

    // The transform must now reflect the NEW rotation — proving the page-index
    // keyed cache was invalidated. Pre-fix, getTransform(0) returns the warmed
    // pre-rotation transform (rotation == startRot) and this fails.
    const int afterRotation = cm->getTransform(0).rotation;
    EXPECT_EQ(afterRotation, newRot)
        << "CoordMapper returned a stale pre-rotation transform (rotation "
        << afterRotation << " != " << newRot << "): the rotate did not "
           "invalidate the page-index-keyed transform cache (GitHub #334)";
}

// ─────────────────────────────────────────────────────────────────────
// Cache 2 — OcrPipeline HOCR cache (TRE region path) is cleared on rotate.
// ─────────────────────────────────────────────────────────────────────
TEST_F(RotationTextCacheTest, OcrHocrCacheClearedAfterRotate) {
    MainWindow mw;
    QApplication::processEvents();
    mw.openFile(QString::fromStdString(fixture_.string()));
    QApplication::processEvents();

    auto* doc = mw.documentForTesting();
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);

    mw.initTreComponentsForTesting();
    auto* ocr = mw.ocrPipelineForTesting();
    ASSERT_NE(ocr, nullptr) << "TRE OcrPipeline must exist after init";

    // Prime the page-0 HOCR cache (no Tesseract needed).
    ocr->primeHocrCacheForTesting(0);
    ASSERT_TRUE(ocr->isCached(0)) << "cache should be warm before rotate";

    auto* rot = action(mw, "rotatePageCWAction");
    ASSERT_NE(rot, nullptr);
    ASSERT_TRUE(rot->isEnabled());
    rot->trigger();
    QApplication::processEvents();

    ASSERT_EQ(norm(doc->currentRotation(0)), 90)
        << "precondition: the CW rotate must land on page 0";

    // Pre-fix the stale OCR layout survives; this EXPECT_FALSE fails.
    EXPECT_FALSE(ocr->isCached(0))
        << "the rotate did not invalidate the OcrPipeline HOCR cache for "
           "page 0 (GitHub #334): the TRE region path would reconstruct from "
           "pre-rotation OCR geometry";
}

// ─────────────────────────────────────────────────────────────────────
// Cache 3 — the live TextEditTool model is rebuilt at the new geometry.
// ─────────────────────────────────────────────────────────────────────
TEST_F(RotationTextCacheTest, TextEditModelRebuiltWithPostRotationGeometry) {
    MainWindow mw;
    QApplication::processEvents();
    mw.openFile(QString::fromStdString(fixture_.string()));
    QApplication::processEvents();

    auto* doc = mw.documentForTesting();
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);

    // Activate the live-text edit tool so the canvas owns a TextEditTool with a
    // page-0 model. activateToolByObjectName() defers the trigger via a 0-timer,
    // so pump the event loop for it to arm.
    ASSERT_TRUE(mw.activateToolByObjectName("textEditToolAction"))
        << "the Text Edit tool action must exist and be enabled";
    QApplication::processEvents();
    QApplication::processEvents();

    auto* canvas = mw.canvasForTesting();
    ASSERT_NE(canvas, nullptr);
    auto* tool = canvas->textEditTool();
    ASSERT_NE(tool, nullptr) << "Text Edit tool must be active on the canvas";
    ASSERT_GT(tool->spanCountForTesting(), 0)
        << "native-text fixture page 0 must yield hit-testable spans";

    // Span 0's PDF-space bbox at the ORIGINAL rotation.
    const QRectF before = tool->spanBboxForTesting(0);
    ASSERT_FALSE(before.isNull()) << "span 0 must have a real bbox before rotate";

    auto* rot = action(mw, "rotatePageCWAction");
    ASSERT_NE(rot, nullptr);
    ASSERT_TRUE(rot->isEnabled());
    rot->trigger();
    QApplication::processEvents();

    ASSERT_EQ(norm(doc->currentRotation(0)), 90)
        << "precondition: the CW rotate must land on page 0";

    // After the rebuild, span geometry is re-extracted in the NEW (rotated)
    // coordinate space, so span 0's bbox differs. Pre-fix the model is not
    // rebuilt (setPage() early-returns on the unchanged index), so the bbox is
    // byte-identical and this fails.
    ASSERT_GT(tool->spanCountForTesting(), 0)
        << "page 0 still has spans after rotation";
    const QRectF after = tool->spanBboxForTesting(0);
    EXPECT_NE(before, after)
        << "TextEditTool model was not rebuilt after rotation: span 0 bbox is "
           "unchanged (" << before.x() << "," << before.y() << " "
        << before.width() << "x" << before.height() << "), so a post-rotation "
           "click hit-tests stale pre-rotation geometry (GitHub #334)";
}

}  // namespace
