// PHASE1_HARDENING: skip=19 expect_ge=2 no_throw=0  (2026-05-08)
#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/UndoManager.h"
#include "forms/FormManager.h"
#include "forms/FormTool.h"
#include "forms/JavaScriptEngine.h"
#include <QMouseEvent>
#include <QKeyEvent>
#include <filesystem>
#include <memory>

class FormToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<pdfe::core::MuPdfContext>();
        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        formPdf = fixturesDir / "test_form.pdf";

        if (std::filesystem::exists(formPdf)) {
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
    }

    void TearDown() override {
        tool.reset();
        page.reset();
        undoManager.reset();
        jsEngine.reset();
        manager.reset();
        document.reset();
        context.reset();
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context;
    std::filesystem::path formPdf;
    std::unique_ptr<pdfe::core::Document> document;
    std::unique_ptr<pdfe::forms::FormManager> manager;
    std::unique_ptr<pdfe::forms::JavaScriptEngine> jsEngine;
    std::unique_ptr<pdfe::commands::UndoManager> undoManager;
    std::unique_ptr<pdfe::forms::FormTool> tool;
    std::unique_ptr<pdfe::core::Page> page;
};

TEST_F(FormToolTest, InitialState) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    EXPECT_EQ(tool->selectedFieldIndex(), -1);
    EXPECT_EQ(tool->toolType(), pdfe::annotations::AnnotationType::Unknown);
}

TEST_F(FormToolTest, ClickToSelectTextField) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    auto fields = manager->getFormFields(page.get());
    ASSERT_FALSE(fields.empty()) << "test_form.pdf must contain form fields";

    // Find a text field
    int textIdx = -1;
    for (const auto& f : fields) {
        if (f.type == pdfe::forms::FormFieldType::Text && !f.isReadOnly) {
            textIdx = f.widgetIndex;
            break;
        }
    }
    ASSERT_GE(textIdx, 0) << "test_form.pdf must contain an editable text field";

    auto& fieldData = fields[textIdx];
    QPointF center = fieldData.rect.center();

    bool selected = false;
    tool->fieldSelected = [&selected](int) { selected = true; };

    QMouseEvent event(QEvent::MouseButtonPress, QPointF(0, 0),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMousePress(&event, 0, center);

    EXPECT_TRUE(selected);
    EXPECT_EQ(tool->selectedFieldIndex(), textIdx);
}

TEST_F(FormToolTest, ClickCheckboxToggles) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    auto fields = manager->getFormFields(page.get());

    // Find a checkbox
    int cbIdx = -1;
    for (const auto& f : fields) {
        if (f.type == pdfe::forms::FormFieldType::Checkbox) {
            cbIdx = f.widgetIndex;
            break;
        }
    }
    ASSERT_GE(cbIdx, 0) << "test_form.pdf must contain a checkbox";

    auto before = manager->getFormField(page.get(), cbIdx);
    QPointF center = before.rect.center();

    QMouseEvent event(QEvent::MouseButtonPress, QPointF(0, 0),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMousePress(&event, 0, center);

    auto after = manager->getFormField(page.get(), cbIdx);
    EXPECT_NE(after.value, before.value);
}

TEST_F(FormToolTest, TabNavigatesForward) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    auto fields = manager->getFormFields(page.get());
    ASSERT_GE(fields.size(), 2u) << "test_form.pdf must contain >= 2 fields for tab test";

    tool->tabToNextField();
    int first = tool->selectedFieldIndex();
    EXPECT_NE(first, -1) << "tabToNextField must select a valid field index";
    EXPECT_LT(first, static_cast<int>(fields.size()))
        << "selected field index must be in range";

    tool->tabToNextField();
    int second = tool->selectedFieldIndex();
    EXPECT_NE(second, -1) << "second tabToNextField must select a valid field";
    EXPECT_LT(second, static_cast<int>(fields.size()));
    EXPECT_NE(first, second);
}

TEST_F(FormToolTest, ShiftTabNavigatesBackward) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    auto fields = manager->getFormFields(page.get());
    ASSERT_GE(fields.size(), 2u) << "test_form.pdf must contain >= 2 fields";

    // Navigate forward twice
    tool->tabToNextField();
    tool->tabToNextField();
    int afterForward = tool->selectedFieldIndex();

    // Navigate backward
    tool->tabToPreviousField();
    int afterBack = tool->selectedFieldIndex();
    EXPECT_NE(afterForward, afterBack);
}

TEST_F(FormToolTest, EscapeCancelsEdit) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    auto fields = manager->getFormFields(page.get());

    // Find a text field
    int textIdx = -1;
    for (const auto& f : fields) {
        if (f.type == pdfe::forms::FormFieldType::Text && !f.isReadOnly) {
            textIdx = f.widgetIndex;
            break;
        }
    }
    ASSERT_GE(textIdx, 0) << "test_form.pdf must contain an editable text field";

    // Click to start editing
    auto& fieldData = fields[textIdx];
    QPointF center = fieldData.rect.center();
    QMouseEvent mouseEvent(QEvent::MouseButtonPress, QPointF(0, 0),
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMousePress(&mouseEvent, 0, center);

    // Type some text
    QKeyEvent keyH(QEvent::KeyPress, Qt::Key_H, Qt::NoModifier, "H");
    tool->onKeyPress(&keyH);

    // Press Escape
    QKeyEvent escEvent(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    tool->onKeyPress(&escEvent);

    // Value should be unchanged (edit was cancelled)
    auto after = manager->getFormField(page.get(), textIdx);
    EXPECT_EQ(after.value, fieldData.value);
}

TEST_F(FormToolTest, ClickOutsideCommitsEdit) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    auto fields = manager->getFormFields(page.get());

    int textIdx = -1;
    for (const auto& f : fields) {
        if (f.type == pdfe::forms::FormFieldType::Text && !f.isReadOnly) {
            textIdx = f.widgetIndex;
            break;
        }
    }
    ASSERT_GE(textIdx, 0) << "test_form.pdf must contain an editable text field";

    // Click to start editing
    auto fieldData = fields[textIdx];
    QPointF center = fieldData.rect.center();
    QMouseEvent mouseEvent(QEvent::MouseButtonPress, QPointF(0, 0),
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMousePress(&mouseEvent, 0, center);

    // Type text
    for (char c : std::string("Test")) {
        QKeyEvent keyEvent(QEvent::KeyPress, 0, Qt::NoModifier, QString(QChar(c)));
        tool->onKeyPress(&keyEvent);
    }

    // Click outside all fields
    QMouseEvent outsideClick(QEvent::MouseButtonPress, QPointF(0, 0),
                             Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMousePress(&outsideClick, 0, QPointF(-100, -100));

    // Value should now be committed
    auto after = manager->getFormField(page.get(), textIdx);
    EXPECT_EQ(after.value, "Test");
}

TEST_F(FormToolTest, DeactivateCommitsEdit) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    auto fields = manager->getFormFields(page.get());

    int textIdx = -1;
    for (const auto& f : fields) {
        if (f.type == pdfe::forms::FormFieldType::Text && !f.isReadOnly) {
            textIdx = f.widgetIndex;
            break;
        }
    }
    ASSERT_GE(textIdx, 0) << "test_form.pdf must contain an editable text field";

    // Click to start editing
    QPointF center = fields[textIdx].rect.center();
    QMouseEvent mouseEvent(QEvent::MouseButtonPress, QPointF(0, 0),
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMousePress(&mouseEvent, 0, center);

    // Type text
    QKeyEvent keyX(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, "X");
    tool->onKeyPress(&keyX);

    // Deactivate
    tool->deactivate();

    auto after = manager->getFormField(page.get(), textIdx);
    EXPECT_EQ(after.value, "X");
}

TEST_F(FormToolTest, SetCreationFieldType) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    // Initially not in creation mode
    EXPECT_FALSE(tool->isCreationMode());

    // Set creation field type to Text
    tool->setCreationFieldType(pdfe::forms::FormFieldType::Text);

    // Verify creation mode is active
    EXPECT_TRUE(tool->isCreationMode());
    EXPECT_EQ(tool->creationFieldType(), pdfe::forms::FormFieldType::Text);
}

TEST_F(FormToolTest, ClearCreationFieldType) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    // Set creation field type first
    tool->setCreationFieldType(pdfe::forms::FormFieldType::Checkbox);
    ASSERT_TRUE(tool->isCreationMode());

    // Clear by setting to Unknown
    tool->setCreationFieldType(pdfe::forms::FormFieldType::Unknown);

    // Verify creation mode is deactivated
    EXPECT_FALSE(tool->isCreationMode());
    EXPECT_EQ(tool->creationFieldType(), pdfe::forms::FormFieldType::Unknown);
}

TEST_F(FormToolTest, FieldCreatedCallback) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    // Set up callback tracking
    bool callbackInvoked = false;
    int capturedPageIndex = -1;
    pdfe::forms::FormFieldType capturedType = pdfe::forms::FormFieldType::Unknown;
    QRectF capturedRect;

    tool->fieldCreated = [&](int pageIndex, pdfe::forms::FormFieldType type, QRectF rect) {
        callbackInvoked = true;
        capturedPageIndex = pageIndex;
        capturedType = type;
        capturedRect = rect;
    };

    // Verify callback can be invoked
    if (tool->fieldCreated) {
        QRectF testRect(100, 100, 200, 30);
        tool->fieldCreated(0, pdfe::forms::FormFieldType::Text, testRect);

        EXPECT_TRUE(callbackInvoked);
        EXPECT_EQ(capturedPageIndex, 0);
        EXPECT_EQ(capturedType, pdfe::forms::FormFieldType::Text);
        EXPECT_EQ(capturedRect, testRect);
    }
}

// GitHub #328 — clicking a Signature field in interaction mode must route into
// the signing flow. The exact defect was a MISSING `case FormFieldType::
// Signature:` in onMousePress's switch: the click fell to `default:` and only
// selected the field, so nothing on the canvas ever reached signing. Drive a
// real press onto a placed signature field and assert the activation callback
// fires once with the right page/widget — the seam MainWindow connects to open
// the Sign dialog against THAT field.
TEST_F(FormToolTest, ClickSignatureFieldEmitsActivation_Issue328) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    // Place a signature field (createFormField sets /FT /Sig).
    QRectF sigRect(72, 72, 180, 54);
    int newWidget = manager->createFormField(
        page.get(), pdfe::forms::FormFieldType::Signature, sigRect,
        "Issue328SigField");
    ASSERT_GE(newWidget, 0) << "failed to create signature field";

    // Find the widget index the tool will report (page widget-list index).
    auto fields = manager->getFormFields(page.get());
    int sigIdx = -1;
    for (const auto& f : fields) {
        if (f.type == pdfe::forms::FormFieldType::Signature) {
            sigIdx = f.widgetIndex;
            break;
        }
    }
    ASSERT_GE(sigIdx, 0) << "created field is not reported as a Signature field";

    auto sigField = manager->getFormField(page.get(), sigIdx);
    QPointF center = sigField.rect.center();

    int gotPage = -999, gotWidget = -999, calls = 0;
    tool->signatureFieldActivated = [&](int p, int w) {
        gotPage = p; gotWidget = w; ++calls;
    };

    QMouseEvent event(QEvent::MouseButtonPress, QPointF(0, 0),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMousePress(&event, 0, center);

    EXPECT_EQ(calls, 1)
        << "signatureFieldActivated must fire exactly once on a signature-field click";
    EXPECT_EQ(gotPage, page->index());
    EXPECT_EQ(gotWidget, sigIdx);
    EXPECT_EQ(tool->selectedFieldIndex(), sigIdx)
        << "the clicked signature field should also become selected";
}

// GitHub #369 — the REAL toolbar flow: arm creation mode, drag-place a
// signature field, then CLICK it. The defect had two gates:
//   1. Nothing ever reset creationFieldType_ after a placement, so every
//      later click re-entered the rubber-band branch and the #328 activation
//      wiring was unreachable (a plain click died on the <10pt minimum).
//   2. The `widgetIdx != selectedWidget_` gate made a re-click on the
//      already-selected field inert.
// Steps 3-5 below fail against the pre-fix code. The #328 test above bypasses
// creation mode by creating the field via FormManager directly — this one
// does NOT: the field is placed through the exact press/drag/release path the
// toolbar flow takes, which is the coverage gap the root-cause audit named.
TEST_F(FormToolTest, CreationModeExitsAfterPlacementAndClickActivates_Issue369) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    // (1) Arm creation mode, exactly as MainWindow does when the toolbar's
    // Signature field-type action triggers.
    tool->setCreationFieldType(pdfe::forms::FormFieldType::Signature);

    // Wire fieldCreated to the real creation path (what MainWindow::
    // createFormField does at its core), so the placed field actually exists
    // for the clicks below.
    int createdWidget = -1;
    pdfe::forms::FormFieldType createdType = pdfe::forms::FormFieldType::Unknown;
    tool->fieldCreated = [&](int pageIndex, pdfe::forms::FormFieldType type,
                             QRectF rect) {
        (void)pageIndex;
        createdType = type;
        createdWidget = manager->createFormField(page.get(), type, rect,
                                                 "Issue369SigField");
    };
    bool exitedCallbackFired = false;
    tool->creationModeExited = [&]() { exitedCallbackFired = true; };

    // (2) Press/drag/release to place the field. PREMISE: the press really
    // went down the creation (rubber-band) branch — creation mode was armed.
    ASSERT_TRUE(tool->isCreationMode())
        << "premise: the tool must be in creation mode at press time";
    // Same placement rect the #328 test proved click-hittable on this
    // fixture (72,72,180,54): its center cannot land on one of the fixture's
    // pre-existing fields, which would hijack the hit test below.
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(0, 0),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMousePress(&press, 0, QPointF(72, 72));
    QMouseEvent move(QEvent::MouseMove, QPointF(0, 0),
                     Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMouseMove(&move, 0, QPointF(252, 126));
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(0, 0),
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMouseRelease(&release, 0, QPointF(252, 126));

    ASSERT_GE(createdWidget, 0) << "the drag must have created the field";
    ASSERT_EQ(createdType, pdfe::forms::FormFieldType::Signature);

    // (3) THE FIX'S MECHANISM: the tool dropped back to interaction mode and
    // told the UI. Pre-fix, creationFieldType_ stayed Signature forever.
    EXPECT_FALSE(tool->isCreationMode())
        << "GitHub #369: placing a field must exit creation mode";
    EXPECT_TRUE(exitedCallbackFired)
        << "GitHub #369: the UI must be notified so the toolbar action unchecks";

    int activations = 0;
    int gotPage = -1, gotWidget = -1;
    tool->signatureFieldActivated = [&](int p, int w) {
        gotPage = p;
        gotWidget = w;
        ++activations;
    };

    // Resolve the PAGE widget-list index the tool reports (same caveat as the
    // #328 test above: createFormField's return is not that index space).
    int sigIdx = -1;
    for (const auto& f : manager->getFormFields(page.get())) {
        if (f.type == pdfe::forms::FormFieldType::Signature) {
            sigIdx = f.widgetIndex;
            break;
        }
    }
    ASSERT_GE(sigIdx, 0) << "placed field is not reported as a Signature field";
    auto fieldData = manager->getFormField(page.get(), sigIdx);
    ASSERT_EQ(fieldData.type, pdfe::forms::FormFieldType::Signature);
    QPointF center = fieldData.rect.center();

    // (4) First click on the placed field: must reach the #328 activation —
    // pre-fix this press re-entered the rubber-band branch instead.
    QMouseEvent click1(QEvent::MouseButtonPress, QPointF(0, 0),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMousePress(&click1, 0, center);
    EXPECT_EQ(activations, 1)
        << "GitHub #369: a click on the just-placed signature field must activate signing";
    EXPECT_EQ(gotPage, page->index());
    EXPECT_EQ(gotWidget, sigIdx);

    // (5) Re-click while the field is still selected: must fire AGAIN —
    // pre-fix the widgetIdx != selectedWidget_ gate swallowed it.
    QMouseEvent click2(QEvent::MouseButtonPress, QPointF(0, 0),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMousePress(&click2, 0, center);
    EXPECT_EQ(activations, 2)
        << "GitHub #369: re-clicking the selected signature field must re-activate signing";
}
