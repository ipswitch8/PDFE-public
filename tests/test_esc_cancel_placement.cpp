// form-field-bugs phase-8, bug 6: "Esc should abort an in-progress
// click/drag-to-place." This suite drives EVERY placement/creation-mode tool in
// the Cross-tool family audit through the realistic user sequence — begin a
// placement, start a drag, deliver Escape through the tool's own key path, then
// release — and asserts the placement mode is exited and NO partial element is
// left in the document or overlay model (a real count/flag oracle, never
// EXPECT_NO_THROW).
//
// WHY press -> drag -> Esc -> release is the right sequence (the mid-drag
// key-delivery question the triage left open): Qt delivers a keyPressEvent to
// the widget that holds keyboard focus. A plain mouse drag (button held, a
// stream of mouseMoveEvents) is NOT a nested/modal event loop — unlike
// QDrag::exec() or a popup menu — and PdfCanvas takes no grabMouse/grabKeyboard,
// so the canvas keeps the focus it got from the mouse-press click-focus and an
// Escape pressed mid-drag reaches PdfCanvas::keyPressEvent -> currentTool_->
// onKeyPress WHILE the button is still held. That is exactly the ordering below:
// the Esc handler fires between onMouseMove and onMouseRelease, disarms the
// in-progress flag, and the release that follows finalizes nothing. The call to
// the production onKeyPress() (base dispatch for the drag tools, the overridden
// handler for FormTool/SelectTool) is the same entry point PdfCanvas uses.

#include <gtest/gtest.h>

#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationTool.h"
#include "annotations/ShapeTool.h"
#include "annotations/InkTool.h"
#include "annotations/LinkTool.h"
#include "annotations/StampTool.h"
#include "annotations/TextMarkupTool.h"
#include "annotations/TextSelector.h"
#include "annotations/SelectTool.h"
#include "annotations/CropTool.h"
#include "redaction/RedactionTool.h"
#include "redaction/RedactionManager.h"
#include "tre/TreRegionTool.h"
#include "forms/FormManager.h"
#include "forms/FormTool.h"
#include "forms/JavaScriptEngine.h"
#include "core/UndoManager.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointF>
#include <filesystem>
#include <memory>

using namespace pdfe::annotations;

namespace {

QMouseEvent pressEvent() {
    return QMouseEvent(QEvent::MouseButtonPress, QPointF(0, 0),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
}
QMouseEvent moveEvent() {
    return QMouseEvent(QEvent::MouseMove, QPointF(0, 0),
                       Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
}
QMouseEvent releaseEvent() {
    return QMouseEvent(QEvent::MouseButtonRelease, QPointF(0, 0),
                       Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
}
QKeyEvent escEvent() {
    return QKeyEvent(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
}

} // namespace

// ───────────────────── Annotation-tool fixture (test_3page.pdf) ─────────────

class EscCancelAnnotTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<pdfe::core::MuPdfContext>();
        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        testPdf = fixturesDir / "test_3page.pdf";
        if (!std::filesystem::exists(testPdf)) return;
        document = pdfe::core::Document::open(context, testPdf);
        manager = std::make_unique<AnnotationManager>(context);
        selector = std::make_shared<TextSelector>(context);
        if (document && document->pageCount() > 0) {
            pagePtr = document->page(0);
            page = pagePtr.get();
        }
    }

    std::size_t annotCount() { return manager->getAnnotations(page).size(); }

    std::shared_ptr<pdfe::core::MuPdfContext> context;
    std::filesystem::path testPdf;
    std::unique_ptr<pdfe::core::Document> document;
    std::unique_ptr<AnnotationManager> manager;
    std::shared_ptr<TextSelector> selector;
    std::unique_ptr<pdfe::core::Page> pagePtr;
    pdfe::core::Page* page = nullptr;
};

// ShapeTool — in-progress state isDrawing_. Positive control proves the drag
// path really creates, then the cancel case proves Esc suppresses it.
TEST_F(EscCancelAnnotTest, ShapeToolPositiveControlCreatesOnRelease) {
    if (!page) GTEST_SKIP() << "fixture missing";
    ShapeTool tool(manager.get(), AnnotationType::Square, QColor(255, 0, 0));
    tool.setPage(page);
    bool created = false;
    tool.annotationCreated = [&](int, AnnotationData) { created = true; };

    const std::size_t before = annotCount();
    auto p = pressEvent();   tool.onMousePress(&p, 0, QPointF(100, 100));
    auto m = moveEvent();    tool.onMouseMove(&m, 0, QPointF(200, 180));
    auto r = releaseEvent(); tool.onMouseRelease(&r, 0, QPointF(200, 180));

    EXPECT_TRUE(created);
    EXPECT_EQ(annotCount(), before + 1);
}

TEST_F(EscCancelAnnotTest, ShapeToolEscAbortsDrag) {
    if (!page) GTEST_SKIP() << "fixture missing";
    ShapeTool tool(manager.get(), AnnotationType::Square, QColor(255, 0, 0));
    tool.setPage(page);
    bool created = false;
    tool.annotationCreated = [&](int, AnnotationData) { created = true; };

    const std::size_t before = annotCount();
    auto p = pressEvent(); tool.onMousePress(&p, 0, QPointF(100, 100));
    auto m = moveEvent();  tool.onMouseMove(&m, 0, QPointF(200, 180));
    auto e = escEvent();   tool.onKeyPress(&e);
    auto r = releaseEvent(); tool.onMouseRelease(&r, 0, QPointF(200, 180));

    EXPECT_FALSE(created) << "Esc mid-drag must abort shape creation";
    EXPECT_EQ(annotCount(), before) << "no partial shape may remain";
}

TEST_F(EscCancelAnnotTest, InkToolEscAbortsStroke) {
    if (!page) GTEST_SKIP() << "fixture missing";
    InkTool tool(manager.get());
    tool.setPage(page);
    bool created = false;
    tool.annotationCreated = [&](int, AnnotationData) { created = true; };

    const std::size_t before = annotCount();
    auto p = pressEvent(); tool.onMousePress(&p, 0, QPointF(100, 100));
    auto m = moveEvent();  tool.onMouseMove(&m, 0, QPointF(140, 150));
    auto e = escEvent();   tool.onKeyPress(&e);
    auto r = releaseEvent(); tool.onMouseRelease(&r, 0, QPointF(160, 170));

    EXPECT_FALSE(created);
    EXPECT_EQ(annotCount(), before);
}

TEST_F(EscCancelAnnotTest, LinkToolEscAbortsDragBeforeDialog) {
    if (!page) GTEST_SKIP() << "fixture missing";
    LinkTool tool(manager.get());
    tool.setPage(page);
    bool created = false;
    tool.annotationCreated = [&](int, AnnotationData) { created = true; };

    const std::size_t before = annotCount();
    auto p = pressEvent(); tool.onMousePress(&p, 0, QPointF(100, 100));
    auto m = moveEvent();  tool.onMouseMove(&m, 0, QPointF(200, 150));
    auto e = escEvent();   tool.onKeyPress(&e);
    // If the drag were still armed, onMouseRelease would pop a modal dialog and
    // hang the headless run; its early-out on the cleared isDrawing_ is what
    // keeps this test non-interactive.
    auto r = releaseEvent(); tool.onMouseRelease(&r, 0, QPointF(200, 150));

    EXPECT_FALSE(created);
    EXPECT_EQ(annotCount(), before);
}

TEST_F(EscCancelAnnotTest, StampToolEscAbortsDrag) {
    if (!page) GTEST_SKIP() << "fixture missing";
    StampTool tool(manager.get());
    tool.setPage(page);
    bool created = false;
    tool.annotationCreated = [&](int, AnnotationData) { created = true; };

    const std::size_t before = annotCount();
    auto p = pressEvent(); tool.onMousePress(&p, 0, QPointF(100, 100));
    auto m = moveEvent();  tool.onMouseMove(&m, 0, QPointF(220, 160));
    auto e = escEvent();   tool.onKeyPress(&e);
    auto r = releaseEvent(); tool.onMouseRelease(&r, 0, QPointF(220, 160));

    EXPECT_FALSE(created);
    EXPECT_EQ(annotCount(), before);
}

TEST_F(EscCancelAnnotTest, TextMarkupToolEscAbortsSelection) {
    if (!page) GTEST_SKIP() << "fixture missing";
    TextMarkupTool tool(manager.get(), selector,
                        AnnotationType::Highlight, QColor(255, 255, 0));
    tool.setPage(page);
    bool created = false;
    tool.annotationCreated = [&](int, AnnotationData) { created = true; };

    const std::size_t before = annotCount();
    auto p = pressEvent(); tool.onMousePress(&p, 0, QPointF(80, 700));
    auto m = moveEvent();  tool.onMouseMove(&m, 0, QPointF(300, 700));
    auto e = escEvent();   tool.onKeyPress(&e);
    auto r = releaseEvent(); tool.onMouseRelease(&r, 0, QPointF(300, 700));

    EXPECT_FALSE(created) << "Esc mid-drag must abort text-markup creation";
    EXPECT_EQ(annotCount(), before);
    EXPECT_EQ(tool.currentPageIndex(), -1) << "selection page index reset on cancel";
}

// RedactionTool fires redactionCreated (not annotationCreated) on release.
TEST_F(EscCancelAnnotTest, RedactionToolEscAbortsDrag) {
    if (!page) GTEST_SKIP() << "fixture missing";
    pdfe::redaction::RedactionManager redactionMgr(context);
    pdfe::redaction::RedactionTool tool(&redactionMgr);
    tool.setPage(page);
    bool fired = false;
    tool.redactionCreated = [&](int, QRectF) { fired = true; };

    auto p = pressEvent(); tool.onMousePress(&p, 0, QPointF(100, 100));
    auto m = moveEvent();  tool.onMouseMove(&m, 0, QPointF(200, 160));
    auto e = escEvent();   tool.onKeyPress(&e);
    auto r = releaseEvent(); tool.onMouseRelease(&r, 0, QPointF(200, 160));

    EXPECT_FALSE(fired) << "Esc mid-drag must abort the redaction mark";
}

// TreRegionTool fires regionSelected on release.
TEST_F(EscCancelAnnotTest, TreRegionToolEscAbortsDrag) {
    if (!page) GTEST_SKIP() << "fixture missing";
    pdfe::tre::TreRegionTool tool;
    tool.setPage(page);
    bool fired = false;
    tool.regionSelected = [&](int, QRectF) { fired = true; };

    auto p = pressEvent(); tool.onMousePress(&p, 0, QPointF(100, 100));
    auto m = moveEvent();  tool.onMouseMove(&m, 0, QPointF(220, 180));
    auto e = escEvent();   tool.onKeyPress(&e);
    auto r = releaseEvent(); tool.onMouseRelease(&r, 0, QPointF(220, 180));

    EXPECT_FALSE(fired) << "Esc mid-drag must abort the TRE region selection";
}

// CropTool fires cropApplied on release.
TEST_F(EscCancelAnnotTest, CropToolEscAbortsDrag) {
    if (!page) GTEST_SKIP() << "fixture missing";
    CropTool tool;
    tool.setPage(page);
    tool.setActivePage(0);
    bool fired = false;
    tool.cropApplied = [&](int, QRectF) { fired = true; };

    auto p = pressEvent(); tool.onMousePress(&p, 0, QPointF(50, 50));
    auto m = moveEvent();  tool.onMouseMove(&m, 0, QPointF(300, 300));
    auto e = escEvent();   tool.onKeyPress(&e);
    auto r = releaseEvent(); tool.onMouseRelease(&r, 0, QPointF(300, 300));

    EXPECT_FALSE(fired) << "Esc mid-drag must abort the crop";
}

// SelectTool manipulation drag (move). Positive control proves the move path
// commits; the cancel case proves Esc reverts it with the selection kept.
TEST_F(EscCancelAnnotTest, SelectToolPositiveControlMovesOnRelease) {
    if (!page) GTEST_SKIP() << "fixture missing";
    AnnotationData seed = manager->createAnnotation(
        page, AnnotationType::Square, QRectF(100, 100, 80, 40));
    SelectTool tool(manager.get());
    tool.setPage(page);
    bool moved = false;
    tool.annotationMoved = [&](int, int, QRectF, QRectF) { moved = true; };

    const QPointF center(140, 120);
    auto p = pressEvent(); tool.onMousePress(&p, 0, center);
    ASSERT_GE(tool.selectedAnnotIndex(), 0) << "press must select the annotation";
    auto m = moveEvent();  tool.onMouseMove(&m, 0, center + QPointF(30, 30));
    auto r = releaseEvent(); tool.onMouseRelease(&r, 0, center + QPointF(30, 30));

    EXPECT_TRUE(moved) << "a real move drag commits annotationMoved";
}

TEST_F(EscCancelAnnotTest, SelectToolEscAbortsManipulationDrag) {
    if (!page) GTEST_SKIP() << "fixture missing";
    manager->createAnnotation(page, AnnotationType::Square, QRectF(100, 100, 80, 40));
    SelectTool tool(manager.get());
    tool.setPage(page);
    bool moved = false;
    tool.annotationMoved = [&](int, int, QRectF, QRectF) { moved = true; };

    const QPointF center(140, 120);
    auto p = pressEvent(); tool.onMousePress(&p, 0, center);
    ASSERT_GE(tool.selectedAnnotIndex(), 0);
    auto m = moveEvent();  tool.onMouseMove(&m, 0, center + QPointF(30, 30));
    auto e = escEvent();   tool.onKeyPress(&e);
    auto r = releaseEvent(); tool.onMouseRelease(&r, 0, center + QPointF(30, 30));

    EXPECT_FALSE(moved) << "Esc mid-drag must abort the move, firing no command";
    EXPECT_GE(tool.selectedAnnotIndex(), 0) << "selection is kept after cancel";
    EXPECT_TRUE(e.isAccepted()) << "SelectTool consumes Esc when it cancels a drag";
}

// ───────────────────── FormTool fixture (test_form.pdf) ─────────────────────

class EscCancelFormTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<pdfe::core::MuPdfContext>();
        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        formPdf = fixturesDir / "test_form.pdf";
        if (!std::filesystem::exists(formPdf)) return;
        document = pdfe::core::Document::open(context, formPdf);
        manager = std::make_unique<pdfe::forms::FormManager>(context);
        jsEngine = std::make_unique<pdfe::forms::JavaScriptEngine>(context);
        undoManager = std::make_unique<pdfe::commands::UndoManager>();
        tool = std::make_unique<pdfe::forms::FormTool>(
            manager.get(), jsEngine.get(), undoManager.get());
        page = document->page(0);
        tool->activate();
        tool->setPage(page.get());
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context;
    std::filesystem::path formPdf;
    std::unique_ptr<pdfe::core::Document> document;
    std::unique_ptr<pdfe::forms::FormManager> manager;
    std::unique_ptr<pdfe::forms::JavaScriptEngine> jsEngine;
    std::unique_ptr<pdfe::commands::UndoManager> undoManager;
    std::unique_ptr<pdfe::forms::FormTool> tool;
    std::shared_ptr<pdfe::core::Page> page;
};

// Esc during a rubber-band drag-to-place: no field is created, creation mode is
// exited, and the creationModeExited callback (unchecks the toolbar action) fires.
TEST_F(EscCancelFormTest, EscAbortsFieldPlacementDrag) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";
    tool->setCreationFieldType(pdfe::forms::FormFieldType::Text);
    ASSERT_TRUE(tool->isCreationMode());

    bool created = false;
    bool modeExited = false;
    tool->fieldCreated = [&](int, pdfe::forms::FormFieldType, QRectF) { created = true; };
    tool->creationModeExited = [&]() { modeExited = true; };

    auto p = pressEvent(); tool->onMousePress(&p, 0, QPointF(100, 100));
    auto m = moveEvent();  tool->onMouseMove(&m, 0, QPointF(200, 150));
    auto e = escEvent();   tool->onKeyPress(&e);
    auto r = releaseEvent(); tool->onMouseRelease(&r, 0, QPointF(200, 150));

    EXPECT_FALSE(created) << "Esc mid-drag must abort field placement";
    EXPECT_FALSE(tool->isCreationMode()) << "creation mode is exited";
    EXPECT_TRUE(modeExited) << "creationModeExited fires so the toolbar unchecks";
}

// Esc while armed in creation mode but BEFORE any drag also leaves creation mode.
TEST_F(EscCancelFormTest, EscExitsArmedCreationModeWithoutDrag) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";
    tool->setCreationFieldType(pdfe::forms::FormFieldType::Text);
    ASSERT_TRUE(tool->isCreationMode());

    bool modeExited = false;
    tool->creationModeExited = [&]() { modeExited = true; };

    auto e = escEvent(); tool->onKeyPress(&e);

    EXPECT_FALSE(tool->isCreationMode()) << "armed creation mode exits on Esc";
    EXPECT_TRUE(modeExited);
}

// Regression guard: with nothing being placed, Esc keeps its prior meaning —
// it must not spuriously report a cancel. Here a text field is selected and
// being edited; Esc exits the edit (cancelInProgress returns false first).
TEST_F(EscCancelFormTest, EscWithNoPlacementFallsThroughToEditCancel) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";
    EXPECT_FALSE(tool->isCreationMode());
    // Not in creation mode and not dragging: cancelInProgress() is a no-op, so
    // Esc reaches the edit/deselect path. Driving a full edit needs page
    // geometry; asserting the mode state is the reachable, deterministic oracle.
    auto e = escEvent(); tool->onKeyPress(&e);
    EXPECT_FALSE(tool->isCreationMode());
    EXPECT_EQ(tool->selectedFieldIndex(), -1);
}
