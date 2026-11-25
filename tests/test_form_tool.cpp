// PHASE1_HARDENING: skip=19 expect_ge=2 no_throw=0  (2026-05-08)
#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/UndoManager.h"
#include "forms/FormManager.h"
#include "forms/FormCommands.h"
#include "forms/FormTool.h"
#include "forms/JavaScriptEngine.h"
#include <QMouseEvent>
#include <QKeyEvent>
#include <filesystem>
#include <memory>
#include <vector>

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

// GitHub #369, ADAPTED for form-field-bugs phase-9 (bug 10, sticky placement).
//
// ORIGINAL #369 intent: after placing a signature field through the real
// toolbar flow (arm creation mode, drag-place), a CLICK on that field must be
// able to reach the #328 signing activation, rather than forever re-entering
// the rubber-band. The defect had two gates:
//   1. Nothing ever reset creationFieldType_ after a placement, so every
//      later click re-entered the rubber-band branch and the #328 activation
//      wiring was unreachable (a plain click died on the <10pt minimum).
//   2. The `widgetIdx != selectedWidget_` gate made a re-click on the
//      already-selected field inert.
//
// WHAT PHASE-9 CHANGED, AND WHY THIS TEST WAS UPDATED: #369 originally bought
// its reachability by AUTO-EXITING creation mode after every placement. Bug 10
// requires the OPPOSITE — placement is now STICKY so many fields can be placed
// without re-arming. The old assertion "placing a field exits creation mode"
// therefore now asserts the wrong behaviour and is replaced by its sticky
// counterpart. #369's real requirement (reachability of the placed field) is
// preserved and still asserted here, but it is reached through the EXPLICIT
// exit path — Esc — exactly as the new design intends: sticky place, Esc to
// exit, then interaction-mode clicks select/activate. Gate #2 (the re-click
// swallow) is untouched by phase-9 and is still asserted in step (6).
TEST_F(FormToolTest,
       StickyPlacementStaysArmed_ExplicitExitRestoresClickActivation_Issue369) {
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

    // (3) PHASE-9 STICKY MECHANISM: the tool STAYS armed after a placement, and
    // DID NOT notify the UI — so the toolbar action stays checked and the next
    // drag would place another field. (Pre-phase-9 this asserted the opposite:
    // auto-exit + exitedCallbackFired.)
    EXPECT_TRUE(tool->isCreationMode())
        << "bug 10: placement is sticky — the tool stays armed after a placement";
    EXPECT_EQ(tool->creationFieldType(), pdfe::forms::FormFieldType::Signature)
        << "bug 10: the armed field type is unchanged after a placement";
    EXPECT_FALSE(exitedCallbackFired)
        << "bug 10: a placement must NOT fire creationModeExited (no auto-exit)";

    // (4) EXPLICIT EXIT restores #369 reachability: Esc leaves creation mode and
    // notifies the UI (so the toolbar action unchecks). This is the user action
    // that trades the sticky rubber-band for interaction-mode selection.
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    tool->onKeyPress(&esc);
    EXPECT_FALSE(tool->isCreationMode())
        << "GitHub #369 (via explicit exit): Esc must leave creation mode";
    EXPECT_TRUE(exitedCallbackFired)
        << "GitHub #369 (via explicit exit): the UI must be notified so the "
           "toolbar action unchecks";

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

    // (5) First click on the placed field: must reach the #328 activation —
    // pre-fix this press re-entered the rubber-band branch instead. After the
    // explicit Esc exit above, the tool is in interaction mode, so this click
    // hit-tests and selects/activates the field (the #369 reachability property).
    QMouseEvent click1(QEvent::MouseButtonPress, QPointF(0, 0),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMousePress(&click1, 0, center);
    EXPECT_EQ(activations, 1)
        << "GitHub #369: a click on the just-placed signature field must activate signing";
    EXPECT_EQ(gotPage, page->index());
    EXPECT_EQ(gotWidget, sigIdx);

    // (6) Re-click while the field is still selected: must fire AGAIN —
    // pre-fix the widgetIdx != selectedWidget_ gate swallowed it. Phase-9 does
    // not touch this gate; still asserted to prevent a #369 regression.
    QMouseEvent click2(QEvent::MouseButtonPress, QPointF(0, 0),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMousePress(&click2, 0, center);
    EXPECT_EQ(activations, 2)
        << "GitHub #369: re-clicking the selected signature field must re-activate signing";
}

// ============================================================================
// form-field-bugs phase-9, bug 10 — placement/creation tools must be STICKY:
// after placing one field the tool stays armed so the next drag places another,
// WITHOUT re-clicking the toolbar button. The tool is left ONLY by an explicit
// action (Esc — phase-8 cancelInProgress — or a tool switch / deactivate), never
// automatically after a placement. See docs/audit/
// form-field-bugs-phase-9-sticky-audit.md for the cross-tool audit.
// ============================================================================

// Helper: drive one press/drag/release placement of the armed field type over
// @p rect, routing fieldCreated through the real FormManager create path so the
// field actually exists afterwards. Returns the created widget handle (or -1).
namespace {
int placeFieldViaDrag(pdfe::forms::FormTool* tool,
                      pdfe::forms::FormManager* manager,
                      pdfe::core::Page* page, const QRectF& rect,
                      const std::string& name) {
    int createdWidget = -1;
    tool->fieldCreated = [&](int, pdfe::forms::FormFieldType type, QRectF r) {
        createdWidget = manager->createFormField(page, type, r, name);
    };
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(0, 0),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMousePress(&press, 0, rect.topLeft());
    QMouseEvent move(QEvent::MouseMove, QPointF(0, 0),
                     Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMouseMove(&move, 0, rect.bottomRight());
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(0, 0),
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMouseRelease(&release, 0, rect.bottomRight());
    return createdWidget;
}
} // namespace

// The core of bug 10: one placement leaves the tool ARMED (not dropped back to
// interaction mode) so a second placement needs no re-arming.
TEST_F(FormToolTest, StickyPlacement_ToolStaysArmedAfterOnePlacement) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    tool->setCreationFieldType(pdfe::forms::FormFieldType::Text);
    ASSERT_TRUE(tool->isCreationMode());

    bool exited = false;
    tool->creationModeExited = [&]() { exited = true; };

    int w = placeFieldViaDrag(tool.get(), manager.get(), page.get(),
                              QRectF(72, 72, 120, 24), "Sticky1");
    ASSERT_GE(w, 0) << "the drag must have created a field";

    // The whole point of the fix: still armed, same type, no exit notification.
    EXPECT_TRUE(tool->isCreationMode())
        << "bug 10: the tool must stay armed after a placement";
    EXPECT_EQ(tool->creationFieldType(), pdfe::forms::FormFieldType::Text)
        << "bug 10: the armed field type is unchanged after a placement";
    EXPECT_FALSE(exited)
        << "bug 10: a placement must NOT fire creationModeExited";
}

// Two consecutive drags both create fields with NO re-arming between them —
// the behaviour a user gets from the sticky tool. Oracle: field count rises by
// exactly two.
TEST_F(FormToolTest, StickyPlacement_TwoConsecutivePlacementsBothCreate) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    const size_t before = manager->getFormFields(page.get()).size();

    tool->setCreationFieldType(pdfe::forms::FormFieldType::Text);
    ASSERT_TRUE(tool->isCreationMode());

    int w1 = placeFieldViaDrag(tool.get(), manager.get(), page.get(),
                               QRectF(72, 72, 120, 24), "Sticky_A");
    ASSERT_GE(w1, 0) << "first placement must create a field";
    ASSERT_TRUE(tool->isCreationMode())
        << "bug 10: tool must remain armed WITHOUT re-clicking the toolbar";

    // Second placement — NO setCreationFieldType() in between.
    int w2 = placeFieldViaDrag(tool.get(), manager.get(), page.get(),
                               QRectF(72, 150, 120, 24), "Sticky_B");
    ASSERT_GE(w2, 0) << "second placement must create a field with no re-arming";

    const size_t after = manager->getFormFields(page.get()).size();
    EXPECT_EQ(after, before + 2)
        << "bug 10: two consecutive sticky placements must create two fields";
    EXPECT_TRUE(tool->isCreationMode())
        << "bug 10: the tool is still armed after the second placement";
}

// Explicit exit via a TOOL SWITCH (deactivate) also leaves creation mode and
// notifies the UI — the other half of "left only by an explicit action". After
// it, the tool is in interaction mode and a click can select a placed field.
TEST_F(FormToolTest, StickyPlacement_DeactivateExitsCreationModeAndNotifies) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    tool->setCreationFieldType(pdfe::forms::FormFieldType::Text);
    bool exited = false;
    tool->creationModeExited = [&]() { exited = true; };

    int w = placeFieldViaDrag(tool.get(), manager.get(), page.get(),
                              QRectF(72, 72, 120, 24), "StickySwitch");
    ASSERT_GE(w, 0);
    ASSERT_TRUE(tool->isCreationMode()) << "sticky: still armed after placement";
    ASSERT_FALSE(exited) << "sticky: placement did not notify exit";

    // Switching tools tears the FormTool down (deactivate) — an explicit exit.
    tool->deactivate();
    EXPECT_FALSE(tool->isCreationMode())
        << "bug 10: a tool switch (deactivate) leaves creation mode";
    EXPECT_TRUE(exited)
        << "bug 10: the tool switch notifies the UI so the toolbar unchecks";
}

// User report (2026-11-22): "form fields are properly displayed when added now
// but they cannot be edited (content cannot be entered!)". The sticky tool is
// STILL ARMED after a placement, and onMousePress used to start a new rubber
// band for every press in creation mode -- so clicking the field just placed
// never reached startEditing(). This drives that exact sequence through the
// tool: arm -> drag-place -> click the new field -> type -> click away.
TEST_F(FormToolTest, StickyPlacement_ClickOnPlacedFieldEditsItWhileArmed) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    tool->setCreationFieldType(pdfe::forms::FormFieldType::Text);
    const QRectF rect(72, 72, 180, 30);
    const int w = placeFieldViaDrag(tool.get(), manager.get(), page.get(), rect,
                                    "StickyEditMe");
    ASSERT_GE(w, 0) << "the drag must have created a field";
    ASSERT_TRUE(tool->isCreationMode()) << "precondition: sticky tool still armed";

    int idx = -1;
    for (const auto& f : manager->getFormFields(page.get())) {
        if (f.fieldName == "StickyEditMe") { idx = f.widgetIndex; break; }
    }
    ASSERT_GE(idx, 0);
    const size_t fieldsAfterPlacement = manager->getFormFields(page.get()).size();

    // From here on any fieldCreated call is a defect: count them (and stop the
    // helper's by-reference lambda from outliving its frame).
    int unexpectedCreates = 0;
    tool->fieldCreated = [&](int, pdfe::forms::FormFieldType, QRectF) {
        ++unexpectedCreates;
    };

    // Click (press + release, no drag) on the placed field.
    const QPointF center = manager->getFormField(page.get(), idx).rect.center();
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(0, 0), QPointF(0, 0),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMousePress(&press, 0, center);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(0, 0), QPointF(0, 0),
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMouseRelease(&release, 0, center);

    EXPECT_EQ(tool->selectedFieldIndex(), idx)
        << "clicking the placed field must select it, not start a rubber band";
    ASSERT_TRUE(tool->isEditingField())
        << "clicking a placed text field while the tool is armed must enter editing";

    for (char c : std::string("Hello")) {
        QKeyEvent k(QEvent::KeyPress, 0, Qt::NoModifier, QString(QChar(c)));
        tool->onKeyPress(&k);
    }
    EXPECT_EQ(tool->editBufferForTesting(), "Hello");

    // Click away on empty page space: commits the value.
    const QPointF empty(rect.left(), rect.bottom() + 200);
    ASSERT_FALSE(manager->getFormFieldAt(page.get(), empty).has_value())
        << "precondition: the click-away point is empty page space";
    tool->onMousePress(&press, 0, empty);
    tool->onMouseRelease(&release, 0, empty);

    EXPECT_EQ(manager->getFormField(page.get(), idx).value, "Hello")
        << "the typed text must be committed to the field";
    EXPECT_FALSE(tool->isEditingField());
    EXPECT_EQ(unexpectedCreates, 0) << "clicks (no drag) must not place fields";
    EXPECT_EQ(manager->getFormFields(page.get()).size(), fieldsAfterPlacement);
    EXPECT_TRUE(tool->isCreationMode())
        << "editing a field does not disarm sticky placement";
}

// The other half: with the tool still armed, a DRAG on empty space after
// editing a field still places a new field (sticky placement is intact).
TEST_F(FormToolTest, StickyPlacement_DragOnEmptySpaceStillPlacesAfterEditing) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    tool->setCreationFieldType(pdfe::forms::FormFieldType::Text);
    ASSERT_GE(placeFieldViaDrag(tool.get(), manager.get(), page.get(),
                                QRectF(72, 72, 180, 30), "StickyFirst"), 0);

    // Edit the first field.
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(0, 0), QPointF(0, 0),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMousePress(&press, 0, QPointF(162, 87));
    ASSERT_TRUE(tool->isEditingField());
    QKeyEvent k(QEvent::KeyPress, 0, Qt::NoModifier, QStringLiteral("x"));
    tool->onKeyPress(&k);

    const size_t before = manager->getFormFields(page.get()).size();
    const int w2 = placeFieldViaDrag(tool.get(), manager.get(), page.get(),
                                     QRectF(72, 300, 180, 30), "StickySecond");
    EXPECT_GE(w2, 0) << "a drag on empty space must still place a field";
    EXPECT_EQ(manager->getFormFields(page.get()).size(), before + 1);
    EXPECT_TRUE(tool->isCreationMode());
    EXPECT_FALSE(tool->isEditingField())
        << "starting a new placement commits the edit in progress";
    tool->fieldCreated = nullptr;  // the helper's lambda captured its locals
}

// ============================================================================
// form-field-bugs phase-6, bug-4 — a placed field cannot be re-edited or
// deleted after content is added and focus leaves. Two independently-grounded
// sub-defects (see docs/audit/form-field-bugs-phase-6-lifecycle-audit.md):
//   (A) DELETE: a field SELECTED but NOT editing had no keyboard delete path —
//       onKeyPress handled Delete/Backspace only while isEditing_ (erase a
//       char); DeleteFormFieldCommand existed but the sole trigger was the
//       toolbar's Delete Field button (#253). FIX: Delete/Backspace on a
//       selected, non-editing field removes it (undoably), routed through a
//       host callback that parks the Page in commandPages_.
//   (B) RE-EDIT: the triage leads suspected getFormFieldAt could not resolve a
//       just-created+committed field. The repro test below drives exactly that
//       sequence headlessly; see the audit doc for the outcome and why.
// ============================================================================

// (A) Delete half, standalone fallback path. A placed TEXT field is reached in
// the "selected, not editing" state via Escape (which exits the edit but keeps
// the selection), then Delete removes it. Oracle: field count drops by one and
// the field stops being hit-testable; undo restores both. No host callback is
// wired here, so this exercises onKeyPress's direct (unparked) fallback execute.
TEST_F(FormToolTest, DeleteKeyRemovesSelectedTextFieldAndUndoRestores_Bug4) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    const int countBefore = static_cast<int>(manager->getFormFields(page.get()).size());

    // Place a text field (rect proven non-overlapping on this fixture by the
    // #328/#369 tests above).
    const QRectF rect(72, 72, 180, 30);
    ASSERT_GE(manager->createFormField(
                  page.get(), pdfe::forms::FormFieldType::Text, rect,
                  "Bug4DeleteField"),
              0);

    // Resolve the page-widget-list index of the field just placed.
    int idx = -1;
    for (const auto& f : manager->getFormFields(page.get())) {
        if (f.fieldName == "Bug4DeleteField") { idx = f.widgetIndex; break; }
    }
    ASSERT_GE(idx, 0) << "created field not enumerated";
    const QPointF center = manager->getFormField(page.get(), idx).rect.center();
    EXPECT_EQ(static_cast<int>(manager->getFormFields(page.get()).size()),
              countBefore + 1);

    // Click selects + starts editing (Text case), then Escape exits the edit but
    // keeps the selection — the "selected, not editing" state the delete path
    // requires.
    QMouseEvent click(QEvent::MouseButtonPress, QPointF(0, 0),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMousePress(&click, 0, center);
    ASSERT_EQ(tool->selectedFieldIndex(), idx);
    ASSERT_TRUE(tool->isEditingField());
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    tool->onKeyPress(&esc);
    ASSERT_FALSE(tool->isEditingField());
    ASSERT_EQ(tool->selectedFieldIndex(), idx)
        << "Escape must keep the field selected (only exit the edit)";

    // Delete removes the selected field.
    QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    tool->onKeyPress(&del);

    EXPECT_EQ(tool->selectedFieldIndex(), -1)
        << "selection must be dropped after the field is deleted";
    EXPECT_EQ(static_cast<int>(manager->getFormFields(page.get()).size()),
              countBefore)
        << "Delete must remove the selected field";
    EXPECT_FALSE(manager->getFormFieldAt(page.get(), center).has_value())
        << "the deleted field must no longer be hit-testable";

    // The delete is undoable.
    ASSERT_TRUE(undoManager->canUndo())
        << "Delete must push an undoable DeleteFormFieldCommand";
    ASSERT_NO_THROW(undoManager->undo());
    EXPECT_EQ(static_cast<int>(manager->getFormFields(page.get()).size()),
              countBefore + 1)
        << "undo must restore the deleted field";
    EXPECT_TRUE(manager->getFormFieldAt(page.get(), center).has_value())
        << "the restored field must be hit-testable again";
}

// (A) Delete half, HOST (parked-page) path — the production wiring. The host
// callback is wired EXACTLY as MainWindow wires deleteSelectedField (park the
// Page in a commandPages vector, execute DeleteFormFieldCommand, return true).
// Oracles: the callback fires once with the right page/widget; the field count
// drops by EXACTLY one (true ⇒ FormTool must NOT also run its fallback execute,
// i.e. no double delete); selection clears; undo restores. Uses Backspace to
// cover the second accepted key.
TEST_F(FormToolTest, DeleteKeyHostPathParksPageNoDoubleDelete_Bug4) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    const int countBefore = static_cast<int>(manager->getFormFields(page.get()).size());
    const QRectF rect(72, 72, 180, 30);
    ASSERT_GE(manager->createFormField(
                  page.get(), pdfe::forms::FormFieldType::Text, rect,
                  "Bug4HostField"),
              0);
    int idx = -1;
    for (const auto& f : manager->getFormFields(page.get())) {
        if (f.fieldName == "Bug4HostField") { idx = f.widgetIndex; break; }
    }
    ASSERT_GE(idx, 0);
    const QPointF center = manager->getFormField(page.get(), idx).rect.center();

    // Model MainWindow's page registry + deleteSelectedField wiring.
    std::vector<std::unique_ptr<pdfe::core::Page>> commandPages;
    int calls = 0, gotPage = -999, gotWidget = -999;
    tool->deleteSelectedField =
        [&](int pageIndex, int widgetIndex) -> bool {
            ++calls; gotPage = pageIndex; gotWidget = widgetIndex;
            auto p = document->page(pageIndex);
            if (!p) return false;
            auto* pagePtr = p.get();
            commandPages.push_back(std::move(p));
            undoManager->execute(
                std::make_unique<pdfe::forms::DeleteFormFieldCommand>(
                    manager.get(), pagePtr, widgetIndex));
            return true;
        };

    // Select (not editing) via click + Escape, then Backspace.
    QMouseEvent click(QEvent::MouseButtonPress, QPointF(0, 0),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMousePress(&click, 0, center);
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    tool->onKeyPress(&esc);
    ASSERT_EQ(tool->selectedFieldIndex(), idx);
    ASSERT_FALSE(tool->isEditingField());

    QKeyEvent bks(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
    tool->onKeyPress(&bks);

    EXPECT_EQ(calls, 1) << "the host delete callback must fire exactly once";
    EXPECT_EQ(gotPage, page->index());
    EXPECT_EQ(gotWidget, idx);
    EXPECT_EQ(tool->selectedFieldIndex(), -1);
    EXPECT_EQ(static_cast<int>(manager->getFormFields(page.get()).size()),
              countBefore)
        << "field count must drop by exactly one — a handled host delete must "
           "NOT also trigger FormTool's fallback execute (no double delete)";
    EXPECT_EQ(static_cast<int>(commandPages.size()), 1)
        << "the host must have parked the Page for the delete command";

    ASSERT_TRUE(undoManager->canUndo());
    ASSERT_NO_THROW(undoManager->undo());
    EXPECT_EQ(static_cast<int>(manager->getFormFields(page.get()).size()),
              countBefore + 1)
        << "undo of the parked delete must restore the field";
}

// (B) Re-edit half, headless repro. Drives the EXACT sequence the triage named
// as NEEDS-RUNTIME-REPRO: create a text field → click to edit → type → click
// away (commit + deselect) → re-click. The decisive oracle is whether
// getFormFieldAt resolves the just-created+committed field, and whether a
// re-click re-selects and re-enters editing with the committed value. See the
// audit doc for the interpretation of the result.
TEST_F(FormToolTest, PlacedTextFieldStaysReEditableAfterCommitAndClickAway_Bug4) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    const QRectF rect(72, 72, 180, 30);
    ASSERT_GE(manager->createFormField(
                  page.get(), pdfe::forms::FormFieldType::Text, rect,
                  "Bug4ReEditField"),
              0);
    int idx = -1;
    for (const auto& f : manager->getFormFields(page.get())) {
        if (f.fieldName == "Bug4ReEditField") { idx = f.widgetIndex; break; }
    }
    ASSERT_GE(idx, 0);
    const QPointF center = manager->getFormField(page.get(), idx).rect.center();

    // Click to edit, type a value.
    QMouseEvent click1(QEvent::MouseButtonPress, QPointF(0, 0),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMousePress(&click1, 0, center);
    ASSERT_TRUE(tool->isEditingField());
    for (char c : std::string("Hi")) {
        QKeyEvent k(QEvent::KeyPress, 0, Qt::NoModifier, QString(QChar(c)));
        tool->onKeyPress(&k);
    }

    // Click away commits the value and deselects (the "focus leaves" step).
    QMouseEvent clickAway(QEvent::MouseButtonPress, QPointF(0, 0),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMousePress(&clickAway, 0, QPointF(-100, -100));
    EXPECT_EQ(manager->getFormField(page.get(), idx).value, "Hi")
        << "click-away must commit the typed value";
    EXPECT_EQ(tool->selectedFieldIndex(), -1) << "click-away must deselect";

    // DECISIVE ORACLE: the committed field is still hit-testable at its rect, and
    // resolves to the same widget index.
    auto hit = manager->getFormFieldAt(page.get(), center);
    ASSERT_TRUE(hit.has_value())
        << "a just-created+committed field must remain hit-testable (re-edit repro)";
    EXPECT_EQ(hit.value(), idx)
        << "getFormFieldAt must resolve the committed field to its original index";

    // Re-click must re-select AND re-enter editing with the committed value.
    QMouseEvent click2(QEvent::MouseButtonPress, QPointF(0, 0),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMousePress(&click2, 0, center);
    EXPECT_EQ(tool->selectedFieldIndex(), idx)
        << "re-click must re-select the field";
    EXPECT_TRUE(tool->isEditingField())
        << "re-click on a text field must re-enter editing";
    EXPECT_EQ(tool->editBufferForTesting(), "Hi")
        << "re-entered edit must load the committed value";
}
