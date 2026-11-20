#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/UndoManager.h"
#include "core/SaveWithRebind.h"
#include "forms/FormManager.h"
#include "forms/FormCommands.h"
#include "forms/FormFieldData.h"
#include "forms/FormTool.h"
#include "helpers/RoundTripHelper.h"
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

class FormCommandsTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<pdfe::core::MuPdfContext>();
        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        testPdf = fixturesDir / "test_form.pdf";

        if (std::filesystem::exists(testPdf)) {
            document = pdfe::core::Document::open(context, testPdf);
            manager = std::make_unique<pdfe::forms::FormManager>(context);
            undoManager = std::make_unique<pdfe::commands::UndoManager>();
        }
    }

    void TearDown() override {
        undoManager.reset();
        manager.reset();
        document.reset();
        context.reset();
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context;
    std::filesystem::path testPdf;
    std::unique_ptr<pdfe::core::Document> document;
    std::unique_ptr<pdfe::forms::FormManager> manager;
    std::unique_ptr<pdfe::commands::UndoManager> undoManager;
};

TEST_F(FormCommandsTest, SetFieldValue_ExecuteAndUndo) {
    if (!document) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    auto fields = manager->getFormFields(page.get());

    // Find an editable text field
    int textIdx = -1;
    std::string originalValue;
    for (const auto& f : fields) {
        if (f.type == pdfe::forms::FormFieldType::Text && !f.isReadOnly) {
            textIdx = f.widgetIndex;
            originalValue = f.value;
            break;
        }
    }

    if (textIdx < 0) {
        GTEST_SKIP() << "No editable text field in test_form.pdf";
    }

    // Execute set value command
    const std::string newValue = "Test Value 123";
    auto cmd = std::make_unique<pdfe::forms::SetFieldValueCommand>(
        manager.get(),
        page.get(),
        textIdx,
        newValue
    );
    undoManager->execute(std::move(cmd));

    // Verify value changed
    auto updated = manager->getFormField(page.get(), textIdx);
    EXPECT_EQ(updated.value, newValue);

    // R-10 strengthening: the in-memory check above is satisfied by a
    // "stages the value in the FormManager but never serializes it to the
    // pdf_obj" bug. Save the document, REOPEN it from disk in a fresh
    // Document/FormManager, and assert the value actually persisted — so the
    // never-serializes bug class is caught, not just in-memory state.
    {
        const auto tmpDir = std::filesystem::temp_directory_path()
            / "pdfe_form_setvalue_roundtrip";
        std::filesystem::create_directories(tmpDir);
        auto reopened = pdfe::test::SaveAndReopen(*document, context, tmpDir);
        ASSERT_NE(reopened, nullptr) << "save+reopen produced no document.";
        auto rPage = reopened->page(0);
        ASSERT_NE(rPage, nullptr);
        pdfe::forms::FormManager rMgr(context);
        EXPECT_EQ(rMgr.getFormField(rPage.get(), textIdx).value, newValue)
            << "SetFieldValueCommand's value must survive save+reopen, not "
               "merely live in the in-memory FormManager.";
    }

    // Undo
    ASSERT_TRUE(undoManager->canUndo());
    undoManager->undo();

    // Verify value reverted
    auto reverted = manager->getFormField(page.get(), textIdx);
    EXPECT_EQ(reverted.value, originalValue);
}

TEST_F(FormCommandsTest, SetFieldValue_Redo) {
    if (!document) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    auto fields = manager->getFormFields(page.get());

    // Find an editable text field
    int textIdx = -1;
    for (const auto& f : fields) {
        if (f.type == pdfe::forms::FormFieldType::Text && !f.isReadOnly) {
            textIdx = f.widgetIndex;
            break;
        }
    }

    if (textIdx < 0) {
        GTEST_SKIP() << "No editable text field in test_form.pdf";
    }

    // Execute
    const std::string newValue = "Redo Test";
    auto cmd = std::make_unique<pdfe::forms::SetFieldValueCommand>(
        manager.get(),
        page.get(),
        textIdx,
        newValue
    );
    undoManager->execute(std::move(cmd));

    // Undo
    undoManager->undo();

    // Redo
    ASSERT_TRUE(undoManager->canRedo());
    undoManager->redo();

    // Verify value is back to new value
    auto redone = manager->getFormField(page.get(), textIdx);
    EXPECT_EQ(redone.value, newValue);
}

TEST_F(FormCommandsTest, SetFieldValue_MergeConsecutiveEdits) {
    if (!document) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    auto fields = manager->getFormFields(page.get());

    // Find an editable text field
    int textIdx = -1;
    std::string originalValue;
    for (const auto& f : fields) {
        if (f.type == pdfe::forms::FormFieldType::Text && !f.isReadOnly) {
            textIdx = f.widgetIndex;
            originalValue = f.value;
            break;
        }
    }

    if (textIdx < 0) {
        GTEST_SKIP() << "No editable text field in test_form.pdf";
    }

    // Execute 3 consecutive edits (simulating typing)
    auto cmd1 = std::make_unique<pdfe::forms::SetFieldValueCommand>(
        manager.get(),
        page.get(),
        textIdx,
        "H"
    );
    undoManager->execute(std::move(cmd1));

    auto cmd2 = std::make_unique<pdfe::forms::SetFieldValueCommand>(
        manager.get(),
        page.get(),
        textIdx,
        "He"
    );
    undoManager->execute(std::move(cmd2));

    auto cmd3 = std::make_unique<pdfe::forms::SetFieldValueCommand>(
        manager.get(),
        page.get(),
        textIdx,
        "Hello"
    );
    undoManager->execute(std::move(cmd3));

    // Verify final value
    auto updated = manager->getFormField(page.get(), textIdx);
    EXPECT_EQ(updated.value, "Hello");

    // Should be able to undo once (all three merged into one)
    ASSERT_TRUE(undoManager->canUndo());
    undoManager->undo();

    // Should revert to original value
    auto reverted = manager->getFormField(page.get(), textIdx);
    EXPECT_EQ(reverted.value, originalValue);

    // No more undo steps
    EXPECT_FALSE(undoManager->canUndo());
}

TEST_F(FormCommandsTest, ToggleCheckbox_ExecuteAndUndo) {
    if (!document) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    auto fields = manager->getFormFields(page.get());

    // Find a checkbox
    int cbIdx = -1;
    for (const auto& f : fields) {
        if (f.type == pdfe::forms::FormFieldType::Checkbox) {
            cbIdx = f.widgetIndex;
            break;
        }
    }

    if (cbIdx < 0) {
        GTEST_SKIP() << "No checkbox in test_form.pdf";
    }

    // Get original value
    auto before = manager->getFormField(page.get(), cbIdx);
    std::string originalValue = before.value;

    // Execute toggle command
    auto cmd = std::make_unique<pdfe::forms::ToggleFieldCommand>(
        manager.get(),
        page.get(),
        cbIdx
    );
    undoManager->execute(std::move(cmd));

    // Verify value changed
    auto after = manager->getFormField(page.get(), cbIdx);
    EXPECT_NE(after.value, originalValue);

    // Undo (should toggle back)
    ASSERT_TRUE(undoManager->canUndo());
    undoManager->undo();

    // Verify value reverted
    auto reverted = manager->getFormField(page.get(), cbIdx);
    EXPECT_EQ(reverted.value, originalValue);
}

TEST_F(FormCommandsTest, SetChoiceSelection_ExecuteAndUndo) {
    if (!document) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    auto fields = manager->getFormFields(page.get());

    // Find a choice field (combo or list)
    int choiceIdx = -1;
    std::vector<int> originalSelection;
    for (const auto& f : fields) {
        if (f.type == pdfe::forms::FormFieldType::ComboBox ||
            f.type == pdfe::forms::FormFieldType::ListBox) {
            choiceIdx = f.widgetIndex;
            originalSelection = f.selectedIndices;
            break;
        }
    }

    if (choiceIdx < 0) {
        GTEST_SKIP() << "No choice field in test_form.pdf";
    }

    // Get field data to find available options
    auto fieldData = manager->getFormField(page.get(), choiceIdx);
    if (fieldData.options.empty()) {
        GTEST_SKIP() << "Choice field has no options";
    }

    // Create a new selection (select first option if available)
    std::vector<int> newSelection = {0};

    // Execute set choice selection command
    auto cmd = std::make_unique<pdfe::forms::SetChoiceSelectionCommand>(
        manager.get(),
        page.get(),
        choiceIdx,
        originalSelection,
        newSelection
    );
    undoManager->execute(std::move(cmd));

    // Verify selection changed
    auto updated = manager->getFormField(page.get(), choiceIdx);
    EXPECT_EQ(updated.selectedIndices, newSelection);

    // Undo
    ASSERT_TRUE(undoManager->canUndo());
    undoManager->undo();

    // Verify selection reverted
    auto reverted = manager->getFormField(page.get(), choiceIdx);
    EXPECT_EQ(reverted.selectedIndices, originalSelection);
}

TEST_F(FormCommandsTest, SetChoiceSelection_Redo) {
    if (!document) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    auto fields = manager->getFormFields(page.get());

    // Find a choice field
    int choiceIdx = -1;
    std::vector<int> originalSelection;
    for (const auto& f : fields) {
        if (f.type == pdfe::forms::FormFieldType::ComboBox ||
            f.type == pdfe::forms::FormFieldType::ListBox) {
            choiceIdx = f.widgetIndex;
            originalSelection = f.selectedIndices;
            break;
        }
    }

    if (choiceIdx < 0) {
        GTEST_SKIP() << "No choice field in test_form.pdf";
    }

    // Get field data
    auto fieldData = manager->getFormField(page.get(), choiceIdx);
    if (fieldData.options.empty()) {
        GTEST_SKIP() << "Choice field has no options";
    }

    std::vector<int> newSelection = {0};

    // Execute
    auto cmd = std::make_unique<pdfe::forms::SetChoiceSelectionCommand>(
        manager.get(),
        page.get(),
        choiceIdx,
        originalSelection,
        newSelection
    );
    undoManager->execute(std::move(cmd));

    // Undo
    undoManager->undo();

    // Redo
    ASSERT_TRUE(undoManager->canRedo());
    undoManager->redo();

    // Verify selection is back to new selection
    auto redone = manager->getFormField(page.get(), choiceIdx);
    EXPECT_EQ(redone.selectedIndices, newSelection);
}

TEST_F(FormCommandsTest, ResetForm_ExecuteAndUndo) {
    if (!document) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    auto fields = manager->getFormFields(page.get());
    if (fields.empty()) {
        GTEST_SKIP() << "No form fields in test_form.pdf";
    }

    // Find an editable text field
    int textIdx = -1;
    for (const auto& f : fields) {
        if (f.type == pdfe::forms::FormFieldType::Text && !f.isReadOnly) {
            textIdx = f.widgetIndex;
            break;
        }
    }
    if (textIdx < 0) {
        GTEST_SKIP() << "No editable text field in test_form.pdf";
    }

    // Set a known value
    const std::string knownValue = "Modified Value";
    manager->setFieldValue(page.get(), textIdx, knownValue);

    // Verify it was set
    auto modified = manager->getFormField(page.get(), textIdx);
    ASSERT_EQ(modified.value, knownValue);

    // Execute reset form command (saves state then resets)
    auto cmd = std::make_unique<pdfe::forms::ResetFormCommand>(
        manager.get(),
        page.get()
    );
    EXPECT_NO_THROW(undoManager->execute(std::move(cmd)));

    // Note the state after reset (may or may not differ depending on DV entries)
    auto afterReset = manager->getFormField(page.get(), textIdx);

    // Undo reset -- should restore pre-reset state
    ASSERT_TRUE(undoManager->canUndo());
    undoManager->undo();

    // Verify the text field value is restored to what we set before reset
    auto restored = manager->getFormField(page.get(), textIdx);
    EXPECT_EQ(restored.value, knownValue);
}

TEST_F(FormCommandsTest, ResetForm_Redo) {
    if (!document) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    auto fields = manager->getFormFields(page.get());
    if (fields.empty()) {
        GTEST_SKIP() << "No form fields in test_form.pdf";
    }

    // Set values in some fields
    for (const auto& f : fields) {
        if (f.type == pdfe::forms::FormFieldType::Text && !f.isReadOnly) {
            manager->setFieldValue(page.get(), f.widgetIndex, "Test");
        }
    }

    // Execute reset
    auto cmd = std::make_unique<pdfe::forms::ResetFormCommand>(
        manager.get(),
        page.get()
    );
    undoManager->execute(std::move(cmd));

    // Get reset state
    auto resetState = manager->getFormFields(page.get());

    // Undo reset
    undoManager->undo();

    // Redo reset
    ASSERT_TRUE(undoManager->canRedo());
    undoManager->redo();

    // Verify form is reset again
    auto redoneState = manager->getFormFields(page.get());
    for (size_t i = 0; i < resetState.size() && i < redoneState.size(); ++i) {
        EXPECT_EQ(redoneState[i].value, resetState[i].value);
    }
}

TEST_F(FormCommandsTest, CleanState_DirtyAfterFormEdit) {
    if (!document) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    auto fields = manager->getFormFields(page.get());

    // Find an editable text field
    int textIdx = -1;
    for (const auto& f : fields) {
        if (f.type == pdfe::forms::FormFieldType::Text && !f.isReadOnly) {
            textIdx = f.widgetIndex;
            break;
        }
    }

    if (textIdx < 0) {
        GTEST_SKIP() << "No editable text field in test_form.pdf";
    }

    // Set clean state
    undoManager->setClean();
    EXPECT_TRUE(undoManager->isClean());

    // Execute set value command
    auto cmd = std::make_unique<pdfe::forms::SetFieldValueCommand>(
        manager.get(),
        page.get(),
        textIdx,
        "New Value"
    );
    undoManager->execute(std::move(cmd));

    // Should be dirty now
    EXPECT_FALSE(undoManager->isClean());

    // Undo should return to clean state
    undoManager->undo();
    EXPECT_TRUE(undoManager->isClean());
}

TEST_F(FormCommandsTest, MultipleCommands_UndoRedo) {
    if (!document) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    auto fields = manager->getFormFields(page.get());

    // Find editable fields
    std::vector<int> textIndices;
    for (const auto& f : fields) {
        if (f.type == pdfe::forms::FormFieldType::Text && !f.isReadOnly) {
            textIndices.push_back(f.widgetIndex);
        }
    }

    if (textIndices.size() < 2) {
        GTEST_SKIP() << "Need at least 2 editable text fields in test_form.pdf";
    }

    // Execute multiple commands
    auto cmd1 = std::make_unique<pdfe::forms::SetFieldValueCommand>(
        manager.get(),
        page.get(),
        textIndices[0],
        "Value 1"
    );
    undoManager->execute(std::move(cmd1));

    auto cmd2 = std::make_unique<pdfe::forms::SetFieldValueCommand>(
        manager.get(),
        page.get(),
        textIndices[1],
        "Value 2"
    );
    undoManager->execute(std::move(cmd2));

    // Verify both changed
    auto field1 = manager->getFormField(page.get(), textIndices[0]);
    auto field2 = manager->getFormField(page.get(), textIndices[1]);
    EXPECT_EQ(field1.value, "Value 1");
    EXPECT_EQ(field2.value, "Value 2");

    // Undo both
    undoManager->undo();
    undoManager->undo();

    // Both should be reverted (we don't check exact values as we don't know originals)
    EXPECT_FALSE(undoManager->canUndo());

    // Redo both
    undoManager->redo();
    undoManager->redo();

    // Verify both changed again
    field1 = manager->getFormField(page.get(), textIndices[0]);
    field2 = manager->getFormField(page.get(), textIndices[1]);
    EXPECT_EQ(field1.value, "Value 1");
    EXPECT_EQ(field2.value, "Value 2");
}

// ============================================================================
// form-field-bugs phase-2, bug-5 — undo after a form text-field edit crashed.
//
// ROOT CAUSE (confirmed): FormTool::commitEdit built a SetFieldValueCommand
// with page_ == the canvas/MainWindow single-slot toolPage_ and executed it
// DIRECTLY, never parking that Page in MainWindow::commandPages_ — unlike the
// panel-edit path (onFormFieldValueChanged) and every other command site. When
// toolPage_ is reset (page/tool switch, or the full-rewrite save that field
// creation forces) the Page wrapper frees, the stacked command's page_ dangles,
// and the next undo() calls manager_->setFieldValue(page_, …) → page_->fzPage()
// on freed memory → access violation (the #234/#235/#360 use-after-free class).
//
// The fix routes the canvas commit through FormTool::commitFieldValue, which
// MainWindow wires to PARK the Page in commandPages_, so the command joins the
// same drain+rebind lifetime machinery saveWithCommandPageRebind drives.
//
// These tests stage their OWN temp copy of the fixture because
// saveWithCommandPageRebind rewrites the file in place — they must never touch
// the committed test_form.pdf.
// ============================================================================

class FormUndoAfterSaveTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::filesystem::path fixture =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_form.pdf";
        if (!std::filesystem::exists(fixture)) {
            GTEST_SKIP() << "test_form.pdf not available";
        }
        tempPdf = std::filesystem::temp_directory_path() /
                  "pdfe_form_undo_after_save_bug5.pdf";
        std::error_code ec;
        std::filesystem::remove(tempPdf, ec);
        std::filesystem::copy_file(
            fixture, tempPdf,
            std::filesystem::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << "failed to stage temp copy: " << ec.message();

        context = std::make_shared<pdfe::core::MuPdfContext>();
        document = pdfe::core::Document::open(context, tempPdf);
        ASSERT_NE(document, nullptr);
        manager = std::make_unique<pdfe::forms::FormManager>(context);
        undoManager = std::make_unique<pdfe::commands::UndoManager>();
    }

    void TearDown() override {
        undoManager.reset();
        manager.reset();
        document.reset();
        context.reset();
        std::error_code ec;
        std::filesystem::remove(tempPdf, ec);
    }

    // Returns the first editable text field's widget index, or -1.
    int firstEditableTextField(pdfe::core::Page* page, std::string& outValue) {
        for (const auto& f : manager->getFormFields(page)) {
            if (f.type == pdfe::forms::FormFieldType::Text && !f.isReadOnly) {
                outValue = f.value;
                return f.widgetIndex;
            }
        }
        return -1;
    }

    std::filesystem::path tempPdf;
    std::shared_ptr<pdfe::core::MuPdfContext> context;
    std::unique_ptr<pdfe::core::Document> document;
    std::unique_ptr<pdfe::forms::FormManager> manager;
    std::unique_ptr<pdfe::commands::UndoManager> undoManager;
};

// THE bug-5 regression. Drives FormTool::commitEdit through its real commit
// path (the commitValueEditForTesting seam → private commitEdit), wired EXACTLY
// as MainWindow wires it (commitFieldValue parks the Page in a commandPages
// vector). Then performs the lifetime event that triggered the crash —
// toolPage freed + a full-rewrite save that drops+reopens the document — and
// asserts the subsequent undo neither crashes NOR silently no-ops: it must
// restore the pre-edit value on the LIVE (reopened) document.
//
// RED without the fix: commitEdit executes an unparked command against the
// single-slot toolPage; saveWithCommandPageRebind cannot rebind a command it
// does not own, so page_ dangles. The undo then either faults (process abort)
// or mutates the dead page, leaving the live doc still showing the new value —
// the deterministic oracle below catches that even when the UAF does not fault.
TEST_F(FormUndoAfterSaveTest, CanvasInlineEditSurvivesFullRewriteSaveUndo_Bug5) {
    std::unique_ptr<pdfe::core::Page> toolPage = document->page(0);
    ASSERT_NE(toolPage, nullptr);

    std::string originalValue;
    const int textIdx = firstEditableTextField(toolPage.get(), originalValue);
    if (textIdx < 0) {
        GTEST_SKIP() << "No editable text field in test_form.pdf";
    }

    // Model MainWindow's page registry.
    std::vector<std::unique_ptr<pdfe::core::Page>> commandPages;

    pdfe::forms::FormTool tool(manager.get(), nullptr, undoManager.get());
    tool.activate();
    tool.setPage(toolPage.get());

    // Wire the host-commit seam EXACTLY as MainWindow does: park the Page in
    // commandPages so the command participates in drain+rebind.
    tool.commitFieldValue = [&](int pageIndex, int widgetIndex,
                                const std::string& v) -> bool {
        auto page = document->page(pageIndex);
        if (!page) return false;
        auto* pagePtr = page.get();
        commandPages.push_back(std::move(page));
        undoManager->execute(std::make_unique<pdfe::forms::SetFieldValueCommand>(
            manager.get(), pagePtr, widgetIndex, v));
        return true;
    };

    const std::string newValue = "Bug5CanvasEditedValue";
    ASSERT_NE(newValue, originalValue);

    tool.commitValueEditForTesting(textIdx, newValue);

    // The edit reached the live model, and a command is on the stack.
    EXPECT_EQ(manager->getFormField(toolPage.get(), textIdx).value, newValue);
    ASSERT_TRUE(undoManager->canUndo())
        << "commitEdit must have executed a SetFieldValueCommand";

    // The lifetime event that produced the crash (mirrors MainWindow::onSaveFile:
    // toolPage_.reset(); saveWithCommandPageRebind(...)). Field creation forces a
    // full rewrite, so mark it.
    document->markRequiresFullRewrite();
    toolPage.reset();  // free the single-slot holder — the dangling trigger
    ASSERT_NO_THROW({
        EXPECT_TRUE(pdfe::core::saveWithCommandPageRebind(
            *document, *undoManager, commandPages, tempPdf));
    });

    // Undo after the save: must not UAF, and must restore the pre-edit value on
    // the LIVE reopened document. Read from a freshly-resolved page so the
    // oracle is deterministic regardless of whether a freed-page deref faults.
    ASSERT_NO_THROW(undoManager->undo());
    {
        auto livePage = document->page(0);
        ASSERT_NE(livePage, nullptr);
        EXPECT_EQ(manager->getFormField(livePage.get(), textIdx).value, originalValue)
            << "undo after a full-rewrite save must restore the pre-edit value on "
               "the live document; an unparked/un-rebound command dangles and "
               "either crashes or no-ops against the freed page.";
    }

    // Redo reapplies on the live document.
    ASSERT_TRUE(undoManager->canRedo());
    ASSERT_NO_THROW(undoManager->redo());
    {
        auto livePage = document->page(0);
        ASSERT_NE(livePage, nullptr);
        EXPECT_EQ(manager->getFormField(livePage.get(), textIdx).value, newValue)
            << "redo after the save must reapply the edit on the live document";
    }
}

// ============================================================================
// §B family audit (phase-2 criterion 2): prove every PAGE-HOLDING form command
// survives a full-rewrite save + undo/redo via the rebind machinery — the same
// undo-after-lifetime-event class as bug-5, applied across the form family.
// Each command's Page is parked in commandPages (as every production site now
// does), then op → markRequiresFullRewrite → saveWithCommandPageRebind → undo →
// redo is driven with a real state oracle (never EXPECT_NO_THROW-only). A
// command left un-rebound would dangle and crash/misbehave here.
//
// (ApplyFormattingCommand is NOT in this set by design: it holds a TextEditModel*
// plus value copies of formatting runs — no raw Page*/pdf_obj and no rebind
// override — so it is not the dangling class; its undo is covered by
// test_text_edit_undo_chain.cpp. EditTextCommand/TreCommands/PageCommands/Image/
// Signature undo-after-save are covered by their dedicated *_undo_after_save /
// undo_after_save_crash / command_family_undo_after_save_probe suites.)
// ============================================================================

enum class FormFam { SetValue, Toggle, Choice, Create, Delete };

static const char* formFamName(FormFam f) {
    switch (f) {
        case FormFam::SetValue: return "SetValue";
        case FormFam::Toggle:   return "Toggle";
        case FormFam::Choice:   return "Choice";
        case FormFam::Create:   return "Create";
        case FormFam::Delete:   return "Delete";
    }
    return "?";
}

class FormFamilyUndoAfterSaveTest
    : public ::testing::TestWithParam<FormFam> {
protected:
    void SetUp() override {
        std::filesystem::path fixture =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_form.pdf";
        if (!std::filesystem::exists(fixture)) {
            GTEST_SKIP() << "test_form.pdf not available";
        }
        tempPdf = std::filesystem::temp_directory_path() /
                  (std::string("pdfe_form_family_undo_after_save_") +
                   formFamName(GetParam()) + ".pdf");
        std::error_code ec;
        std::filesystem::remove(tempPdf, ec);
        std::filesystem::copy_file(
            fixture, tempPdf,
            std::filesystem::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << "failed to stage temp copy: " << ec.message();

        context = std::make_shared<pdfe::core::MuPdfContext>();
        document = pdfe::core::Document::open(context, tempPdf);
        ASSERT_NE(document, nullptr);
        manager = std::make_unique<pdfe::forms::FormManager>(context);
        undoManager = std::make_unique<pdfe::commands::UndoManager>();
    }

    void TearDown() override {
        undoManager.reset();
        manager.reset();
        document.reset();
        context.reset();
        std::error_code ec;
        std::filesystem::remove(tempPdf, ec);
    }

    int findField(pdfe::core::Page* page, pdfe::forms::FormFieldType type,
                  bool editableOnly) {
        for (const auto& f : manager->getFormFields(page)) {
            if (f.type != type) continue;
            if (editableOnly && f.isReadOnly) continue;
            return f.widgetIndex;
        }
        return -1;
    }

    // Save + undo + redo helper: park already done by caller, this drives the
    // lifetime event and returns the live page after each step via callbacks.
    void fullRewriteSave(std::vector<std::unique_ptr<pdfe::core::Page>>& pages) {
        document->markRequiresFullRewrite();
        ASSERT_NO_THROW({
            EXPECT_TRUE(pdfe::core::saveWithCommandPageRebind(
                *document, *undoManager, pages, tempPdf));
        });
    }

    std::filesystem::path tempPdf;
    std::shared_ptr<pdfe::core::MuPdfContext> context;
    std::unique_ptr<pdfe::core::Document> document;
    std::unique_ptr<pdfe::forms::FormManager> manager;
    std::unique_ptr<pdfe::commands::UndoManager> undoManager;
};

TEST_P(FormFamilyUndoAfterSaveTest, PageHoldingCommandSurvivesFullRewriteSave) {
    using namespace pdfe::forms;
    const FormFam fam = GetParam();

    std::vector<std::unique_ptr<pdfe::core::Page>> commandPages;
    commandPages.push_back(document->page(0));
    ASSERT_NE(commandPages.back(), nullptr);
    pdfe::core::Page* page = commandPages.back().get();

    auto livePage0 = [&]() { return document->page(0); };

    switch (fam) {
        case FormFam::SetValue: {
            std::string original;
            int idx = findField(page, FormFieldType::Text, /*editableOnly=*/true);
            if (idx < 0) GTEST_SKIP() << "no editable text field";
            original = manager->getFormField(page, idx).value;
            const std::string v = "FamSetValue";
            undoManager->execute(std::make_unique<SetFieldValueCommand>(
                manager.get(), page, idx, v));
            ASSERT_EQ(manager->getFormField(page, idx).value, v);

            fullRewriteSave(commandPages);

            ASSERT_TRUE(undoManager->canUndo());
            ASSERT_NO_THROW(undoManager->undo());
            EXPECT_EQ(manager->getFormField(livePage0().get(), idx).value, original);
            ASSERT_TRUE(undoManager->canRedo());
            ASSERT_NO_THROW(undoManager->redo());
            EXPECT_EQ(manager->getFormField(livePage0().get(), idx).value, v);
            break;
        }
        case FormFam::Toggle: {
            int idx = findField(page, FormFieldType::Checkbox, /*editableOnly=*/false);
            if (idx < 0) GTEST_SKIP() << "no checkbox field";
            const std::string before = manager->getFormField(page, idx).value;
            undoManager->execute(std::make_unique<ToggleFieldCommand>(
                manager.get(), page, idx));
            const std::string afterToggle = manager->getFormField(page, idx).value;
            ASSERT_NE(afterToggle, before);

            fullRewriteSave(commandPages);

            ASSERT_TRUE(undoManager->canUndo());
            ASSERT_NO_THROW(undoManager->undo());
            EXPECT_EQ(manager->getFormField(livePage0().get(), idx).value, before);
            ASSERT_TRUE(undoManager->canRedo());
            ASSERT_NO_THROW(undoManager->redo());
            EXPECT_EQ(manager->getFormField(livePage0().get(), idx).value, afterToggle);
            break;
        }
        case FormFam::Choice: {
            int idx = -1;
            for (const auto& f : manager->getFormFields(page)) {
                if ((f.type == FormFieldType::ComboBox ||
                     f.type == FormFieldType::ListBox) && !f.options.empty()) {
                    idx = f.widgetIndex;
                    break;
                }
            }
            if (idx < 0) GTEST_SKIP() << "no choice field with options";
            const std::vector<int> before =
                manager->getFormField(page, idx).selectedIndices;
            const std::vector<int> after = {0};
            undoManager->execute(std::make_unique<SetChoiceSelectionCommand>(
                manager.get(), page, idx, before, after));
            ASSERT_EQ(manager->getFormField(page, idx).selectedIndices, after);

            fullRewriteSave(commandPages);

            ASSERT_TRUE(undoManager->canUndo());
            ASSERT_NO_THROW(undoManager->undo());
            EXPECT_EQ(manager->getFormField(livePage0().get(), idx).selectedIndices,
                      before);
            ASSERT_TRUE(undoManager->canRedo());
            ASSERT_NO_THROW(undoManager->redo());
            EXPECT_EQ(manager->getFormField(livePage0().get(), idx).selectedIndices,
                      after);
            break;
        }
        case FormFam::Create: {
            const size_t before = manager->getFormFields(page).size();
            undoManager->execute(std::make_unique<CreateFormFieldCommand>(
                manager.get(), page, FormFieldType::Text,
                QRectF(72, 72, 160, 24), "FamCreatedField"));
            ASSERT_EQ(manager->getFormFields(page).size(), before + 1);

            fullRewriteSave(commandPages);

            ASSERT_TRUE(undoManager->canUndo());
            ASSERT_NO_THROW(undoManager->undo());  // deletes the created field
            EXPECT_EQ(manager->getFormFields(livePage0().get()).size(), before);
            ASSERT_TRUE(undoManager->canRedo());
            ASSERT_NO_THROW(undoManager->redo());  // restores it
            EXPECT_EQ(manager->getFormFields(livePage0().get()).size(), before + 1);
            break;
        }
        case FormFam::Delete: {
            // Create a field to delete so the test does not depend on the
            // fixture's field set (and so undo/redo have a defined target).
            const int createdIdx = manager->createFormField(
                page, FormFieldType::Text, QRectF(100, 100, 160, 24),
                "FamFieldToDelete");
            ASSERT_GE(createdIdx, 0);
            const size_t before = manager->getFormFields(page).size();

            undoManager->execute(std::make_unique<DeleteFormFieldCommand>(
                manager.get(), page, createdIdx));
            ASSERT_EQ(manager->getFormFields(page).size(), before - 1);

            fullRewriteSave(commandPages);

            ASSERT_TRUE(undoManager->canUndo());
            ASSERT_NO_THROW(undoManager->undo());  // restores the deleted field
            EXPECT_EQ(manager->getFormFields(livePage0().get()).size(), before);
            ASSERT_TRUE(undoManager->canRedo());
            ASSERT_NO_THROW(undoManager->redo());  // deletes it again
            EXPECT_EQ(manager->getFormFields(livePage0().get()).size(), before - 1);
            break;
        }
    }

    // The document itself survived intact and is still queryable (no UAF).
    EXPECT_GT(document->pageCount(), 0);
    ASSERT_NO_THROW({ auto p = document->page(0); EXPECT_NE(p, nullptr); });
}

INSTANTIATE_TEST_SUITE_P(
    FormFamily, FormFamilyUndoAfterSaveTest,
    ::testing::Values(FormFam::SetValue, FormFam::Toggle, FormFam::Choice,
                      FormFam::Create, FormFam::Delete),
    [](const ::testing::TestParamInfo<FormFam>& info) {
        return std::string(formFamName(info.param));
    });
