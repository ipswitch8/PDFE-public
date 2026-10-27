// PHASE1_HARDENING: skip=0 expect_ge=5 no_throw=0  (2026-05-08)
#include <gtest/gtest.h>
#include "forms/FormManager.h"
#include "forms/FormCommands.h"
#include "forms/FormTool.h"
#include "forms/FormFieldType.h"
#include "forms/JavaScriptEngine.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/UndoManager.h"
#include <filesystem>
#include <memory>

class FormCreationTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        testPdf_ = fixturesDir / "test_single.pdf";

        if (std::filesystem::exists(testPdf_)) {
            document_ = pdfe::core::Document::open(context_, testPdf_);
            manager_ = std::make_unique<pdfe::forms::FormManager>(context_);
            undoManager_ = std::make_unique<pdfe::commands::UndoManager>();
            jsEngine_ = std::make_unique<pdfe::forms::JavaScriptEngine>(context_);
        }
    }

    void TearDown() override {
        jsEngine_.reset();
        undoManager_.reset();
        manager_.reset();
        document_.reset();
        context_.reset();
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::filesystem::path testPdf_;
    std::unique_ptr<pdfe::core::Document> document_;
    std::unique_ptr<pdfe::forms::FormManager> manager_;
    std::unique_ptr<pdfe::commands::UndoManager> undoManager_;
    std::unique_ptr<pdfe::forms::JavaScriptEngine> jsEngine_;
};

TEST_F(FormCreationTest, CreateTextField) {
    if (!document_) {
        GTEST_SKIP() << "test_single.pdf not available";
    }

    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    // Get initial field count
    auto fieldsBefore = manager_->getFormFields(page.get());
    size_t initialCount = fieldsBefore.size();

    // Create a text field
    QRectF rect(100, 100, 200, 30);
    std::string fieldName = "testTextField";
    int widgetIndex = manager_->createFormField(
        page.get(), pdfe::forms::FormFieldType::Text, rect, fieldName);

    EXPECT_NE(widgetIndex, -1) << "createFormField must return a valid widgetIndex, not -1 sentinel";

    // Verify field was created
    auto fieldsAfter = manager_->getFormFields(page.get());
    EXPECT_EQ(fieldsAfter.size(), initialCount + 1);

    // Verify field properties
    auto field = manager_->getFormField(page.get(), widgetIndex);
    EXPECT_EQ(field.type, pdfe::forms::FormFieldType::Text);
    EXPECT_EQ(field.rect, rect);
    EXPECT_EQ(field.fieldName, fieldName);
    EXPECT_EQ(field.widgetIndex, widgetIndex);
}

TEST_F(FormCreationTest, CreateCheckbox) {
    if (!document_) {
        GTEST_SKIP() << "test_single.pdf not available";
    }

    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    // Create a checkbox field
    QRectF rect(100, 200, 20, 20);
    std::string fieldName = "testCheckbox";
    int widgetIndex = manager_->createFormField(
        page.get(), pdfe::forms::FormFieldType::Checkbox, rect, fieldName);

    EXPECT_NE(widgetIndex, -1) << "createFormField must return a valid widgetIndex, not -1 sentinel";

    // Verify type is Checkbox
    auto field = manager_->getFormField(page.get(), widgetIndex);
    EXPECT_EQ(field.type, pdfe::forms::FormFieldType::Checkbox);
    EXPECT_EQ(field.fieldName, fieldName);
}

TEST_F(FormCreationTest, CreateComboBox) {
    if (!document_) {
        GTEST_SKIP() << "test_single.pdf not available";
    }

    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    // Create a combo box field
    QRectF rect(100, 300, 150, 25);
    std::string fieldName = "testComboBox";
    int widgetIndex = manager_->createFormField(
        page.get(), pdfe::forms::FormFieldType::ComboBox, rect, fieldName);

    EXPECT_NE(widgetIndex, -1) << "createFormField must return a valid widgetIndex, not -1 sentinel";

    // Verify type is ComboBox
    auto field = manager_->getFormField(page.get(), widgetIndex);
    EXPECT_EQ(field.type, pdfe::forms::FormFieldType::ComboBox);
}

TEST_F(FormCreationTest, CreateListBox) {
    if (!document_) {
        GTEST_SKIP() << "test_single.pdf not available";
    }

    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    // Create a list box field
    QRectF rect(100, 400, 150, 80);
    std::string fieldName = "testListBox";
    int widgetIndex = manager_->createFormField(
        page.get(), pdfe::forms::FormFieldType::ListBox, rect, fieldName);

    EXPECT_NE(widgetIndex, -1) << "createFormField must return a valid widgetIndex, not -1 sentinel";

    // Verify type is ListBox
    auto field = manager_->getFormField(page.get(), widgetIndex);
    EXPECT_EQ(field.type, pdfe::forms::FormFieldType::ListBox);
}

TEST_F(FormCreationTest, DeleteFormField) {
    if (!document_) {
        GTEST_SKIP() << "test_single.pdf not available";
    }

    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    // Create a field
    QRectF rect(100, 100, 200, 30);
    int widgetIndex = manager_->createFormField(
        page.get(), pdfe::forms::FormFieldType::Text, rect, "deleteTest");

    auto fieldsAfterCreate = manager_->getFormFields(page.get());
    size_t countAfterCreate = fieldsAfterCreate.size();

    // Delete the field
    auto deletedData = manager_->deleteFormField(page.get(), widgetIndex);

    // Verify field count decreased
    auto fieldsAfterDelete = manager_->getFormFields(page.get());
    EXPECT_EQ(fieldsAfterDelete.size(), countAfterCreate - 1);

    // Verify deleted data is valid
    EXPECT_EQ(deletedData.type, pdfe::forms::FormFieldType::Text);
    EXPECT_EQ(deletedData.fieldName, "deleteTest");
}

TEST_F(FormCreationTest, RestoreFormField) {
    if (!document_) {
        GTEST_SKIP() << "test_single.pdf not available";
    }

    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    // Create a field
    QRectF rect(100, 100, 200, 30);
    int originalIndex = manager_->createFormField(
        page.get(), pdfe::forms::FormFieldType::Text, rect, "restoreTest");

    auto fieldsAfterCreate = manager_->getFormFields(page.get());
    size_t countAfterCreate = fieldsAfterCreate.size();

    // Delete the field
    auto deletedData = manager_->deleteFormField(page.get(), originalIndex);

    // Verify field was deleted
    auto fieldsAfterDelete = manager_->getFormFields(page.get());
    EXPECT_EQ(fieldsAfterDelete.size(), countAfterCreate - 1);

    // Restore the field
    int restoredIndex = manager_->restoreFormField(page.get(), deletedData);
    EXPECT_NE(restoredIndex, -1) << "restoreFormField must return a valid widgetIndex, not -1 sentinel";

    // Verify field count is back up
    auto fieldsAfterRestore = manager_->getFormFields(page.get());
    EXPECT_EQ(fieldsAfterRestore.size(), countAfterCreate);

    // Verify restored field properties
    auto restoredField = manager_->getFormField(page.get(), restoredIndex);
    EXPECT_EQ(restoredField.type, pdfe::forms::FormFieldType::Text);
    EXPECT_EQ(restoredField.fieldName, "restoreTest");
}

TEST_F(FormCreationTest, CreateFieldCommand_ExecuteUndo) {
    if (!document_) {
        GTEST_SKIP() << "test_single.pdf not available";
    }

    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    auto fieldsBefore = manager_->getFormFields(page.get());
    size_t initialCount = fieldsBefore.size();

    // Create command
    QRectF rect(150, 150, 180, 25);
    auto cmd = std::make_unique<pdfe::forms::CreateFormFieldCommand>(
        manager_.get(), page.get(),
        pdfe::forms::FormFieldType::Text, rect, "commandTest");

    // Execute command
    undoManager_->execute(std::move(cmd));

    auto fieldsAfterExecute = manager_->getFormFields(page.get());
    EXPECT_EQ(fieldsAfterExecute.size(), initialCount + 1);

    // Undo command
    undoManager_->undo();

    auto fieldsAfterUndo = manager_->getFormFields(page.get());
    EXPECT_EQ(fieldsAfterUndo.size(), initialCount);

    // Redo command
    undoManager_->redo();

    auto fieldsAfterRedo = manager_->getFormFields(page.get());
    EXPECT_EQ(fieldsAfterRedo.size(), initialCount + 1);
}

TEST_F(FormCreationTest, DeleteFieldCommand_ExecuteUndo) {
    if (!document_) {
        GTEST_SKIP() << "test_single.pdf not available";
    }

    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    // First create a field to delete
    QRectF rect(100, 100, 200, 30);
    int widgetIndex = manager_->createFormField(
        page.get(), pdfe::forms::FormFieldType::Text, rect, "toDelete");

    auto fieldsAfterCreate = manager_->getFormFields(page.get());
    size_t countAfterCreate = fieldsAfterCreate.size();

    // Create delete command
    auto cmd = std::make_unique<pdfe::forms::DeleteFormFieldCommand>(
        manager_.get(), page.get(), widgetIndex);

    // Execute delete command
    undoManager_->execute(std::move(cmd));

    auto fieldsAfterDelete = manager_->getFormFields(page.get());
    EXPECT_EQ(fieldsAfterDelete.size(), countAfterCreate - 1);

    // Undo delete
    undoManager_->undo();

    auto fieldsAfterUndo = manager_->getFormFields(page.get());
    EXPECT_EQ(fieldsAfterUndo.size(), countAfterCreate);

    // Redo delete
    undoManager_->redo();

    auto fieldsAfterRedo = manager_->getFormFields(page.get());
    EXPECT_EQ(fieldsAfterRedo.size(), countAfterCreate - 1);
}

TEST_F(FormCreationTest, FormTool_CreationMode) {
    if (!document_) {
        GTEST_SKIP() << "test_single.pdf not available";
    }

    auto tool = std::make_unique<pdfe::forms::FormTool>(
        manager_.get(), jsEngine_.get(), undoManager_.get());

    // Initially not in creation mode
    EXPECT_FALSE(tool->isCreationMode());
    EXPECT_EQ(tool->creationFieldType(), pdfe::forms::FormFieldType::Unknown);

    // Set creation field type to Text
    tool->setCreationFieldType(pdfe::forms::FormFieldType::Text);

    // Verify creation mode is active
    EXPECT_TRUE(tool->isCreationMode());
    EXPECT_EQ(tool->creationFieldType(), pdfe::forms::FormFieldType::Text);

    // Clear creation mode
    tool->setCreationFieldType(pdfe::forms::FormFieldType::Unknown);

    // Verify creation mode is deactivated
    EXPECT_FALSE(tool->isCreationMode());
}

TEST_F(FormCreationTest, FormTool_FieldCreatedCallback) {
    if (!document_) {
        GTEST_SKIP() << "test_single.pdf not available";
    }

    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    auto tool = std::make_unique<pdfe::forms::FormTool>(
        manager_.get(), jsEngine_.get(), undoManager_.get());

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

    // Verify callback can be assigned and called
    if (tool->fieldCreated) {
        QRectF testRect(50, 50, 100, 20);
        tool->fieldCreated(0, pdfe::forms::FormFieldType::Checkbox, testRect);

        EXPECT_TRUE(callbackInvoked);
        EXPECT_EQ(capturedPageIndex, 0);
        EXPECT_EQ(capturedType, pdfe::forms::FormFieldType::Checkbox);
        EXPECT_EQ(capturedRect, testRect);
    }
}

// ---------------------------------------------------------------------------
// Regression (pipeline c4a9e271 phase-6): creating a form field AFTER the form
// has been flattened must not crash.
//
// flattenForm() calls pdf_bake_document(), a destructive structural rewrite that
// bakes widgets into page content and removes the AcroForm /Fields. The batch-2
// forms UI suite caught pdfe.exe DYING (process death) when a signature-field
// create-drag ran after flattenForm in the same session — while the identical
// signature create on a fresh form was fine. These tests reproduce that sequence
// at the FormManager level (no UI) so the fix is locked headlessly. The UI
// re-fetches a fresh Page handle for each create, so we do the same here.
// ---------------------------------------------------------------------------
TEST_F(FormCreationTest, FlattenThenCreateSignatureFieldDoesNotCrash) {
    if (!document_) {
        GTEST_SKIP() << "test_single.pdf not available";
    }
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    // Establish a form, then flatten it (destructive rewrite).
    manager_->createFormField(page.get(), pdfe::forms::FormFieldType::Text,
                              QRectF(100, 100, 200, 30), "preFlattenText");
    ASSERT_NO_THROW(manager_->flattenForm(page.get()));

    // Fresh page handle, exactly as MainWindow::fieldCreated does.
    page = document_->page(0);
    ASSERT_NE(page, nullptr);

    // The reported crash site: signature field create after flatten.
    int idx = -2;
    ASSERT_NO_THROW({
        idx = manager_->createFormField(
            page.get(), pdfe::forms::FormFieldType::Signature,
            QRectF(100, 200, 200, 40), "postFlattenSig");
    });
    EXPECT_NE(idx, -1)
        << "signature field must be creatable after flatten (no crash, no -1)";

    auto fields = manager_->getFormFields(page.get());
    EXPECT_GE(fields.size(), 1u)
        << "the post-flatten signature field must be present";
}

TEST_F(FormCreationTest, FlattenThenCreateTextFieldDoesNotCrash) {
    if (!document_) {
        GTEST_SKIP() << "test_single.pdf not available";
    }
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    manager_->createFormField(page.get(), pdfe::forms::FormFieldType::Text,
                              QRectF(100, 100, 200, 30), "preFlattenText2");
    ASSERT_NO_THROW(manager_->flattenForm(page.get()));

    page = document_->page(0);
    ASSERT_NE(page, nullptr);

    // Scopes whether the post-flatten crash is signature-specific or affects all
    // field types (root cause is the shared createFormField path).
    int idx = -2;
    ASSERT_NO_THROW({
        idx = manager_->createFormField(
            page.get(), pdfe::forms::FormFieldType::Text,
            QRectF(100, 260, 200, 30), "postFlattenText");
    });
    EXPECT_NE(idx, -1)
        << "text field must be creatable after flatten (no crash, no -1)";
}

// ---------------------------------------------------------------------------
// GitHub #243 — the two field types that crashed were the two with no direct
// creation test.
//
// FormManager::createFormField has a branch per field type. Grepping the suite
// for RadioButton found it only as an enum value or read back from a prepared
// fixture; nothing created one. Signature was reachable only through
// FlattenThenCreateSignatureFieldDoesNotCrash, which exists because of this very
// crash. So the two uncovered branches and the two reported crashes were the
// same pair — which is not a coincidence worth leaving in place.
//
// These assert the branch produces a real, readable field, not merely that it
// returned without throwing. "It did not crash" was the oracle that let this
// through the first time.
// ---------------------------------------------------------------------------
TEST_F(FormCreationTest, RadioButtonFieldIsCreatableAndReadsBackAsARadioButton) {
    if (!document_) {
        GTEST_SKIP() << "test_single.pdf not available";
    }
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    const size_t before = manager_->getFormFields(page.get()).size();

    int idx = -2;
    ASSERT_NO_THROW({
        idx = manager_->createFormField(page.get(),
                                        pdfe::forms::FormFieldType::RadioButton,
                                        QRectF(72, 400, 20, 20), "p94Radio");
    });
    ASSERT_NE(idx, -1) << "radio-button creation returned failure";

    const auto fields = manager_->getFormFields(page.get());
    ASSERT_EQ(fields.size(), before + 1)
        << "creation reported success but added no widget";

    // Read the type back rather than trusting the return value: a branch that
    // creates the WRONG kind of field also returns a valid index.
    const auto created = manager_->getFormField(page.get(), idx);
    EXPECT_EQ(created.type, pdfe::forms::FormFieldType::RadioButton)
        << "the created field did not read back as a RadioButton";
}

TEST_F(FormCreationTest, SignatureFieldIsCreatableAndReadsBackAsASignature) {
    if (!document_) {
        GTEST_SKIP() << "test_single.pdf not available";
    }
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    const size_t before = manager_->getFormFields(page.get()).size();

    int idx = -2;
    ASSERT_NO_THROW({
        idx = manager_->createFormField(page.get(),
                                        pdfe::forms::FormFieldType::Signature,
                                        QRectF(72, 500, 160, 40), "p94Sig");
    });
    ASSERT_NE(idx, -1) << "signature creation returned failure";

    const auto fields = manager_->getFormFields(page.get());
    ASSERT_EQ(fields.size(), before + 1)
        << "creation reported success but added no widget";

    const auto created = manager_->getFormField(page.get(), idx);
    EXPECT_EQ(created.type, pdfe::forms::FormFieldType::Signature)
        << "the created field did not read back as a Signature";
}
