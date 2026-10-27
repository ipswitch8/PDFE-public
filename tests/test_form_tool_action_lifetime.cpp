// -*- coding: utf-8 -*-
// GitHub #243 — the Forms field-type actions must target the FormTool that is
// alive NOW, and must not be wired to one that has been destroyed.
//
// WHAT THE DEFECT WAS
//
// MainWindow::onAnnotationToolSelected() allocated a FormTool and then, in the
// same block, made seven connect() calls whose lambdas captured a raw
// FormTool*. The receiver context was `this` (the MainWindow), so those
// connections lived as long as the window while the tool they pointed at was
// destroyed on every tool change — PdfCanvas::clearCurrentTool() resets the
// owning unique_ptr. Nothing in MainWindow.cpp ever called disconnect().
//
// Two consequences:
//   * Activating the form tool N times left N connections per action, N-1 of
//     them holding freed pointers.
//   * onFlattenForm() destroyed the tool while leaving formToolAction_ CHECKED,
//     so afterwards there was no live FormTool at all and every field-type
//     action had only dangling receivers.
//
// WHY THESE TESTS COUNT INSTANCES RATHER THAN WAITING FOR A CRASH
//
// The replacement FormTool is allocated immediately after the old one is freed
// and is the same size, so the allocator usually returns the very block just
// released — the stale pointer then aliases the live object and the write is
// harmless. That is why the field report was intermittent, and it is also why
// "it did not crash" is a vacuous oracle here: a passing run proves nothing
// about whether the pointer was dangling.
//
// So these tests assert the STRUCTURAL properties instead, each of which is
// deterministic and each of which the pre-fix code violates:
//   * exactly one FormTool is alive after repeated activation, and the trigger
//     reaches THAT one;
//   * flattening leaves zero FormTools and no checked form action;
//   * triggering a field-type action with no live tool changes nothing;
//   * form mode can be re-entered after a flatten.
#include <gtest/gtest.h>

#include "core/Document.h"
#include "forms/FormFieldType.h"
#include "forms/FormTool.h"
#include "ui/MainWindow.h"
#include "ui/PdfCanvas.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>

#include <filesystem>
#include <string>

using pdfe::forms::FormFieldType;
using pdfe::forms::FormTool;
using pdfe::ui::MainWindow;
using pdfe::ui::PdfCanvas;

namespace {

/// A form fixture with real fields, so flattening has something to flatten.
std::string formFixturePath()
{
    for (const char* rel : {"tests/fixtures/test_form.pdf",
                            "../tests/fixtures/test_form.pdf",
                            "../../tests/fixtures/test_form.pdf"}) {
        if (std::filesystem::exists(rel)) return rel;
    }
    return {};
}

/// The live FormTool owned by the canvas, or nullptr when the current tool is
/// not a FormTool (or there is no tool at all).
FormTool* liveFormTool(MainWindow& mw)
{
    auto* canvas = mw.findChild<PdfCanvas*>();
    if (!canvas) return nullptr;
    return dynamic_cast<FormTool*>(canvas->currentTool());
}

QAction* action(MainWindow& mw, const char* objectName)
{
    return mw.findChild<QAction*>(QString::fromLatin1(objectName));
}

/// activateToolByObjectName() defers the trigger with QTimer::singleShot(0),
/// so the activation has not happened when it returns.
void settle()
{
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

/// MainWindow with the flatten confirmation answered in code.
///
/// onFlattenForm() shows a modal QMessageBox::warning, and QMessageBox::exec()
/// blocks the event loop — a headless test that triggers flattenFormAction
/// against a stock MainWindow hangs forever rather than failing (measured: the
/// first draft of this file sat for ten minutes and had to be killed). The
/// confirmFlattenForm() seam exists so the flatten path can be tested at all.
class FlattenConfirmingMainWindow : public MainWindow {
protected:
    bool confirmFlattenForm() override { return true; }
};

class FormToolActionLifetimeTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        fixture_ = formFixturePath();
        if (fixture_.empty()) GTEST_SKIP() << "test_form.pdf not available";
    }

    std::string fixture_;
};

}  // namespace

// The core property: after the tool has been destroyed and rebuilt, triggering
// a field-type action must reach the tool that is alive now.
//
// Pre-fix this passed for the WRONG reason — the last-registered lambda held
// the newest tool, so the right value did get set, while six older lambdas also
// fired into freed memory. The live-instance assertion is what makes it
// meaningful: it pins that there is exactly one tool for the trigger to reach.
TEST_F(FormToolActionLifetimeTest, TriggerReachesTheLiveToolAfterRepeatedActivation)
{
    MainWindow mw;
    mw.openFile(QString::fromStdString(fixture_));
    settle();
    ASSERT_NE(mw.documentForTesting(), nullptr) << "fixture failed to open";

    auto* formAct = action(mw, "formToolAction");
    auto* selectAct = action(mw, "selectToolAction");
    auto* sigAct = action(mw, "formSignatureAction");
    ASSERT_NE(formAct, nullptr);
    ASSERT_NE(selectAct, nullptr);
    ASSERT_NE(sigAct, nullptr);

    // Enter form mode, leave it, and enter it again. Each entry allocates a new
    // FormTool and destroys the previous one.
    for (int i = 0; i < 3; ++i) {
        formAct->trigger();
        settle();
        ASSERT_NE(liveFormTool(mw), nullptr) << "no FormTool after activation " << i;
        EXPECT_EQ(FormTool::liveInstanceCountForTesting(), 1)
            << "activation " << i << " should leave exactly one live FormTool";
        selectAct->trigger();
        settle();
    }

    formAct->trigger();
    settle();
    FormTool* live = liveFormTool(mw);
    ASSERT_NE(live, nullptr);
    ASSERT_EQ(FormTool::liveInstanceCountForTesting(), 1);

    // THE assertion for this fix. Four activations means four historical
    // FormTools; pre-fix, each left its own lambda connected to this action, so
    // one trigger entered setCreationFieldType four times — three of them on
    // freed objects. Post-fix there is one connection, so exactly one entry.
    //
    // Asserting on creationFieldType() alone cannot see this: the live lambda
    // was registered last and wrote the correct value either way.
    FormTool::resetSetCreationFieldTypeCallsForTesting();
    sigAct->trigger();
    settle();

    EXPECT_EQ(FormTool::setCreationFieldTypeCallsForTesting(), 1)
        << "one trigger must reach setCreationFieldType exactly once; more than "
           "one means destroyed FormTools are still wired to this QAction";
    EXPECT_EQ(live->creationFieldType(), FormFieldType::Signature)
        << "the signature action did not reach the live FormTool";
    EXPECT_EQ(liveFormTool(mw), live)
        << "the trigger must not have replaced the tool";
}

// GitHub #243's OWN designated repro: the radio-button sibling, reached through
// the exact sequence the UI suite ran.
//
// The issue names the radio-button case as "the deterministic handle" because it
// reproduced on both attempts while the signature case did not — same allocation
// sequence, same outcome, whereas signature ran after dialog- and file-I/O-heavy
// tests that perturbed the heap. The sequence that matters is
// `createFormFieldAction` FIRST: MainWindow::onCreateFormField() routes through
// onFormToolSelected() → onAnnotationToolSelected(formToolAction_), which
// destroys the FormTool and builds a new one — orphaning the first connection
// set. Every later field-type trigger in the module then fired at least one
// lambda on a freed object.
//
// Also the only test anywhere that exercises the RadioButton field type through
// the UI actions; it and Signature were the two uncovered branches, and they are
// the two that crashed.
TEST_F(FormToolActionLifetimeTest, TheRadioButtonHandleTargetsOnlyTheLiveTool)
{
    MainWindow mw;
    mw.openFile(QString::fromStdString(fixture_));
    settle();
    ASSERT_NE(mw.documentForTesting(), nullptr) << "fixture failed to open";

    auto* formAct = action(mw, "formToolAction");
    auto* createAct = action(mw, "createFormFieldAction");
    auto* radioAct = action(mw, "formRadioButtonAction");
    ASSERT_NE(formAct, nullptr);
    ASSERT_NE(createAct, nullptr);
    ASSERT_NE(radioAct, nullptr);

    formAct->trigger();
    settle();
    ASSERT_NE(liveFormTool(mw), nullptr);

    // The rebuild step. Pre-fix this is what orphaned connection set #1.
    createAct->trigger();
    settle();
    FormTool* live = liveFormTool(mw);
    ASSERT_NE(live, nullptr) << "the create-field action left no form tool";
    ASSERT_EQ(FormTool::liveInstanceCountForTesting(), 1)
        << "the rebuild must leave exactly one live FormTool";

    FormTool::resetSetCreationFieldTypeCallsForTesting();
    radioAct->trigger();
    settle();

    EXPECT_EQ(FormTool::setCreationFieldTypeCallsForTesting(), 1)
        << "the radio-button action reached setCreationFieldType "
        << FormTool::setCreationFieldTypeCallsForTesting()
        << " times; more than once means the FormTool destroyed by the "
           "create-field rebuild is still wired to this QAction";
    EXPECT_EQ(live->creationFieldType(), FormFieldType::RadioButton);
    EXPECT_EQ(liveFormTool(mw), live);
}

// Flatten used to call canvas_->clearCurrentTool() and stop there, leaving the
// window claiming form mode with nothing behind it. This is the assertion that
// reddens if that regresses.
TEST_F(FormToolActionLifetimeTest, FlattenLeavesNoToolAndNoCheckedFormAction)
{
    FlattenConfirmingMainWindow mw;
    mw.openFile(QString::fromStdString(fixture_));
    settle();
    ASSERT_NE(mw.documentForTesting(), nullptr) << "fixture failed to open";

    auto* formAct = action(mw, "formToolAction");
    auto* flattenAct = action(mw, "flattenFormAction");
    ASSERT_NE(formAct, nullptr);
    ASSERT_NE(flattenAct, nullptr);

    formAct->trigger();
    settle();
    ASSERT_NE(liveFormTool(mw), nullptr);
    ASSERT_TRUE(formAct->isChecked());

    flattenAct->trigger();
    settle();

    EXPECT_EQ(liveFormTool(mw), nullptr)
        << "flatten must not leave a FormTool installed";
    EXPECT_EQ(FormTool::liveInstanceCountForTesting(), 0)
        << "flatten must destroy the FormTool, not merely detach it";
    EXPECT_FALSE(formAct->isChecked())
        << "flatten left the form action checked with no tool behind it — the "
           "state that made activateToolByObjectName() a silent no-op";
}

// The reported crash path, reduced to its essentials: a field-type action fired
// when no FormTool exists. Pre-fix this dereferenced a freed object; post-fix it
// is a no-op. Survival alone is weak evidence (see the file header), so this
// also pins that nothing was resurrected behind the scenes.
TEST_F(FormToolActionLifetimeTest, FieldTypeActionWithNoLiveToolChangesNothing)
{
    FlattenConfirmingMainWindow mw;
    mw.openFile(QString::fromStdString(fixture_));
    settle();
    ASSERT_NE(mw.documentForTesting(), nullptr) << "fixture failed to open";

    auto* formAct = action(mw, "formToolAction");
    auto* flattenAct = action(mw, "flattenFormAction");
    auto* sigAct = action(mw, "formSignatureAction");
    ASSERT_NE(formAct, nullptr);
    ASSERT_NE(flattenAct, nullptr);
    ASSERT_NE(sigAct, nullptr);

    formAct->trigger();
    settle();
    flattenAct->trigger();
    settle();
    ASSERT_EQ(FormTool::liveInstanceCountForTesting(), 0)
        << "precondition: flatten should have destroyed the tool";

    // The crash itself, measured rather than waited for. Pre-fix the flattened
    // tool's lambda was still connected, so this trigger entered
    // setCreationFieldType() on released memory — a call that the counter sees
    // even though the object is gone. Post-fix there is no live tool to resolve
    // to and the handler returns without calling anything.
    FormTool::resetSetCreationFieldTypeCallsForTesting();
    sigAct->trigger();
    settle();

    EXPECT_EQ(FormTool::setCreationFieldTypeCallsForTesting(), 0)
        << "a field-type action reached setCreationFieldType with no live "
           "FormTool — that call is the use-after-free behind #243";
    EXPECT_EQ(FormTool::liveInstanceCountForTesting(), 0);
    EXPECT_EQ(liveFormTool(mw), nullptr);
}

// The test-validity half of #243: activateToolByObjectName() early-returns for
// an already-checked checkable action, so while flatten left formToolAction_
// checked, every later attempt to re-enter form mode did nothing at all — and a
// UI test that relied on it was asserting against a window with no form tool.
TEST_F(FormToolActionLifetimeTest, FormModeCanBeReenteredAfterFlatten)
{
    FlattenConfirmingMainWindow mw;
    mw.openFile(QString::fromStdString(fixture_));
    settle();
    ASSERT_NE(mw.documentForTesting(), nullptr) << "fixture failed to open";

    auto* formAct = action(mw, "formToolAction");
    auto* flattenAct = action(mw, "flattenFormAction");
    ASSERT_NE(formAct, nullptr);
    ASSERT_NE(flattenAct, nullptr);

    formAct->trigger();
    settle();
    flattenAct->trigger();
    settle();

    // Go back in through the seam the UI suite actually uses, not through
    // trigger(), because the seam is where the no-op lived.
    EXPECT_TRUE(mw.activateToolByObjectName("formToolAction"));
    settle();

    EXPECT_NE(liveFormTool(mw), nullptr)
        << "form mode could not be re-entered after a flatten";
    EXPECT_EQ(FormTool::liveInstanceCountForTesting(), 1);
}
