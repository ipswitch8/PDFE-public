// -*- coding: utf-8 -*-
// GitHub #253 / p97 phase-9 — the CHOICE-ROUTING regression test.
//
// WHY THIS FILE EXISTS
// ====================
// `MainWindow::onFormFieldValueChanged` chooses between two commands:
//
//   * `SetChoiceSelectionCommand` — models a `std::vector<int>` of selected
//     indices;
//   * `SetFieldValueCommand`      — takes a single value STRING.
//
// Every choice edit used to go through the second one. For a multi-select list
// box that is lossy by construction: the panel collects every selected row into
// `selectedIndices` and sets `value` to the text of the FIRST one, so
// committing `value` silently discarded every selection after the first. That
// was real data loss, and routing list boxes and non-editable combos through
// `SetChoiceSelectionCommand` is what fixed it.
//
// The `claim-check` gate FAILED phase-9 because nothing in the tree exercised
// that decision. Its finding, which was correct:
//
//     "test_form_commands.cpp tests the command class in total isolation from
//      MainWindow; the live Python tests never commit a multi-item selection
//      and re-read it. A revert of the `isListBox || isPlainCombo` branch back
//      to the old SetFieldValueCommand-always behaviour -- i.e. exactly the bug
//      #253 fixed -- would pass every test in the suite today."
//
// `test_form_commands.cpp::SetChoiceSelection_ExecuteAndUndo` constructs the
// command directly against a `FormManager` with no `MainWindow` in the fixture,
// so it pins the COMMAND and not the ROUTING. It also only ever selects a
// single index, so it cannot tell the two commands apart even in principle.
//
// WHAT MAKES THIS TEST DISCRIMINATING
// ===================================
// It drives the real dispatch — `FormFieldPropertiesPanel::fieldValueChanged`,
// the signal MainWindow actually connects — and commits a selection of TWO
// indices while setting `value` to the first option's text, exactly as the
// panel does. Then it reads back through the live `FormManager`.
//
//   * Routed correctly  -> selectedIndices == {0, 1}
//   * Routed the old way -> the vector never reaches the document; only the
//                           value string is committed
//
// Asserting on TWO indices is the whole point: with one index the two routes
// agree and the test would be vacuous. This is the project's "assert against an
// input, never another output" rule — the expectation is the vector the test
// itself supplied, not something re-derived from the same code path.
//
// Undo is asserted too, because the second half of the bug was that undoing a
// multi-select edit restored one item of the prior selection instead of all of
// it.
#include <gtest/gtest.h>

#include "app/WindowManager.h"
#include "forms/FormFieldData.h"
#include "ui/FormFieldPropertiesPanel.h"
#include "ui/MainWindow.h"

#include <QAction>
#include <QFileInfo>
#include <QString>

#include <string>
#include <vector>

using namespace pdfe::ui;

namespace {

// The fixture is chosen by MEASURED property, not by hope: this file has a
// ListBox named "Interests" with 5 options (confirmed by enumerating every
// fixture's widgets). `test_form_commands.cpp` already uses the same PDF.
const char* kFormFixture = "tests/fixtures/test_form.pdf";

QString fixturePath()
{
    QFileInfo fi(QString::fromUtf8(kFormFixture));
    return fi.absoluteFilePath();
}

class FormChoiceRoutingTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        const QString path = fixturePath();
        // A missing fixture is a BROKEN CHECKOUT, not an environment gate, so
        // it fails rather than skipping. A skip here would be the WIN-45 shape
        // (GitHub #229) this pipeline exists to remove: the product could not
        // be driven and the run would report not-a-failure.
        ASSERT_TRUE(QFileInfo::exists(path))
            << "fixture missing: " << path.toStdString()
            << " -- this test cannot be satisfied without it and must not pass "
               "vacuously";

        window_ = manager_.openPath(path);
        ASSERT_NE(window_, nullptr) << "WindowManager::openPath returned null";

        panel_ = window_->findChild<FormFieldPropertiesPanel*>(
            QStringLiteral("formFieldPropertiesPanel"));
        ASSERT_NE(panel_, nullptr)
            << "FormFieldPropertiesPanel not found by objectName; if it was "
               "renamed, this test is no longer driving the real dispatch";

        listBoxIndex_ = window_->firstListBoxForTesting(2);
        ASSERT_GE(listBoxIndex_, 0)
            << "no ListBox with >= 2 options on page 0 of " << kFormFixture
            << " -- the fixture changed, and with a single-option field this "
               "test could not distinguish the two commands at all";
    }

    pdfe::app::WindowManager manager_;
    MainWindow* window_ = nullptr;
    FormFieldPropertiesPanel* panel_ = nullptr;
    int listBoxIndex_ = -1;
};

// Build the payload the way FormFieldPropertiesPanel::onListSelectionChanged
// does: the full selection in `selectedIndices`, and `value` set to the text of
// the FIRST selected option. Reproducing that asymmetry is essential -- it is
// precisely what made the old route lossy.
pdfe::forms::FormFieldData makeListBoxEdit(int widgetIndex,
                                           const std::vector<int>& selection,
                                           const std::string& firstOptionText)
{
    pdfe::forms::FormFieldData data;
    data.widgetIndex = widgetIndex;
    data.type = pdfe::forms::FormFieldType::ListBox;
    data.isEditable = false;
    data.selectedIndices = selection;
    data.value = firstOptionText;  // std::string, not QString
    return data;
}

} // namespace

TEST_F(FormChoiceRoutingTest, MultiSelectListBoxCommitsEverySelectionNotJustTheFirst)
{
    const std::vector<int> twoSelected{0, 1};

    emit panel_->fieldValueChanged(
        makeListBoxEdit(listBoxIndex_, twoSelected, std::string("ignored")));

    const std::vector<int> committed =
        window_->committedSelectionForTesting(listBoxIndex_);

    EXPECT_EQ(committed, twoSelected)
        << "MainWindow::onFormFieldValueChanged did not commit the whole "
           "selection. Committing " << committed.size() << " of "
        << twoSelected.size() << " indices is the GitHub #253 data-loss bug: a "
           "choice edit routed through SetFieldValueCommand carries one value "
           "STRING, so every selection after the first is discarded. Check the "
           "`isListBox || isPlainCombo` branch still routes through "
           "SetChoiceSelectionCommand.";
}

TEST_F(FormChoiceRoutingTest, UndoRestoresTheWholePriorSelection)
{
    const std::vector<int> before =
        window_->committedSelectionForTesting(listBoxIndex_);

    emit panel_->fieldValueChanged(
        makeListBoxEdit(listBoxIndex_, {0, 1}, std::string("ignored")));

    // Guard the premise against the INPUT, not merely against `before`.
    //
    // This started as ASSERT_NE(afterEdit, before) -- "something changed" --
    // and MUTATION TESTING showed that was too weak: with the routing branch
    // disabled, the lossy path still perturbs the selection (it ends up empty),
    // so `afterEdit != before` held, the premise passed, undo restored the old
    // value, and THIS TEST STAYED GREEN ON THE MUTANT while its two siblings
    // went red. A premise guard that the bug can satisfy is not a guard.
    //
    // Asserting the full expected vector fixes it: only the selection route can
    // produce {0, 1}, so the mutant now fails here instead of sailing past.
    const std::vector<int> afterEdit =
        window_->committedSelectionForTesting(listBoxIndex_);
    ASSERT_EQ(afterEdit, (std::vector<int>{0, 1}))
        << "the two-index edit did not commit as a two-index selection, so this "
           "test cannot say anything about undo. This is the routing bug "
           "itself, reported from the undo test's premise.";

    // Undo through the SHIPPED ACTION rather than a test seam. `undoAction` is
    // what the Edit menu and Ctrl+Z both trigger, so this exercises the same
    // path a user takes -- and it needs no new seam, which is better than
    // adding one.
    auto* undoAct = window_->findChild<QAction*>(QStringLiteral("undoAction"));
    ASSERT_NE(undoAct, nullptr) << "undoAction not found by objectName";
    ASSERT_TRUE(undoAct->isEnabled())
        << "undoAction is disabled after a committed edit, so the edit was "
           "never pushed onto the undo stack -- itself a defect";
    undoAct->trigger();

    EXPECT_EQ(window_->committedSelectionForTesting(listBoxIndex_), before)
        << "undo did not restore the full prior selection. The second half of "
           "GitHub #253 was that undoing a multi-select edit restored one item "
           "of the previous selection rather than all of it, because the undo "
           "step held a single string.";
}

TEST_F(FormChoiceRoutingTest, AnEmptySelectionIsCommittedAsADeliberateEdit)
{
    // Deselecting everything is a real edit, and it is the case where `value`
    // is stale rather than merely lossy -- there is no "first option" to take
    // the text of. The routing comment in MainWindow calls this out explicitly
    // as why a ListBox always takes the selection route, so it is worth
    // pinning: a future "optimisation" that skips empty selections would
    // silently drop the edit.
    emit panel_->fieldValueChanged(
        makeListBoxEdit(listBoxIndex_, {0, 1}, std::string("ignored")));
    ASSERT_EQ(window_->committedSelectionForTesting(listBoxIndex_),
              (std::vector<int>{0, 1}));

    emit panel_->fieldValueChanged(
        makeListBoxEdit(listBoxIndex_, {}, std::string()));

    EXPECT_TRUE(window_->committedSelectionForTesting(listBoxIndex_).empty())
        << "deselecting every option was not committed; an empty selection is "
           "a deliberate user edit, not a no-op";
}
