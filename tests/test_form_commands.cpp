#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/UndoManager.h"
#include "forms/FormManager.h"
#include "forms/FormCommands.h"
#include "forms/FormFieldData.h"
#include "helpers/RoundTripHelper.h"
#include <filesystem>
#include <memory>

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
