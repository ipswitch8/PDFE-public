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
#include <QColor>

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

// ---------------------------------------------------------------------------
// Bug 3 (form-field-bugs phase-5) — changing a form text field's font family /
// size / style must actually rewrite the widget's /DA and regenerate its
// appearance. Before the fix there was NO code path to do this: the properties
// panel had no font controls and FormManager had no set-font method, so the /DA
// stayed pinned at the hardcoded "/Helv 12 Tf 0 g" from creation forever.
//
// These drive the real FormManager path (not "a call was made") and read the
// /DA back through the public API to prove the font/size landed, that the old
// /DA is returned for undo, that the command stack round-trips, and that the
// change survives a save→reopen.
// ---------------------------------------------------------------------------
TEST_F(FormCreationTest, SetFieldDefaultAppearance_RewritesFontAndSize) {
    if (!document_) {
        GTEST_SKIP() << "test_single.pdf not available";
    }
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    int idx = manager_->createFormField(
        page.get(), pdfe::forms::FormFieldType::Text,
        QRectF(100, 100, 200, 30), "fontField");
    ASSERT_NE(idx, -1);

    // The field starts with the hardcoded creation /DA.
    const std::string da0 = manager_->getFieldDefaultAppearance(page.get(), idx);
    EXPECT_NE(da0.find("/Helv"), std::string::npos)
        << "created text field should carry the default Helv /DA, got: " << da0;
    EXPECT_NE(da0.find("12 Tf"), std::string::npos) << da0;

    // The PREVIOUS /DA must be recoverable for undo. Pipeline form-reconcile:
    // the one setter returns bool (accepted / rejected), and the undo snapshot
    // master's string return value carried is getFieldOwnDefaultAppearance,
    // which SetFieldAppearanceCommand takes before it calls the setter.
    const auto old = manager_->getFieldOwnDefaultAppearance(page.get(), idx);
    ASSERT_TRUE(old.has_value()) << "the undo snapshot must be readable";
    EXPECT_TRUE(old->hasDa);
    EXPECT_EQ(old->da, da0)
        << "the old /DA must be recoverable, byte for byte, for undo";

    // Change the font to Times 20pt.
    EXPECT_TRUE(manager_->setFieldDefaultAppearance(
        page.get(), idx, "TiRo", 20.0f, QColor(Qt::black)))
        << "a base-14 font, a valid size and an opaque colour must be accepted";

    // Read back through getFieldDefaultAppearance...
    const std::string da1 = manager_->getFieldDefaultAppearance(page.get(), idx);
    EXPECT_NE(da1.find("/TiRo"), std::string::npos)
        << "font family not written into /DA, got: " << da1;
    EXPECT_NE(da1.find("20 Tf"), std::string::npos)
        << "font size not written into /DA, got: " << da1;

    // ...and through the normal public FormFieldData read path.
    const auto fd = manager_->getFormField(page.get(), idx);
    EXPECT_TRUE(fd.defaultAppearanceStringCaptured);
    EXPECT_EQ(fd.defaultAppearanceString, da1)
        << "FormFieldData.defaultAppearanceString must reflect the live /DA";

    // Putting the snapshot back restores the old /DA exactly (the undo path).
    ASSERT_TRUE(manager_->restoreFieldOwnDefaultAppearance(page.get(), idx, *old));
    EXPECT_EQ(manager_->getFieldDefaultAppearance(page.get(), idx), da0)
        << "restoring the snapshot did not put the old /DA back";
}

TEST_F(FormCreationTest, SetFieldDefaultAppearance_WritesColorOperator) {
    if (!document_) {
        GTEST_SKIP() << "test_single.pdf not available";
    }
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    int idx = manager_->createFormField(
        page.get(), pdfe::forms::FormFieldType::Text,
        QRectF(100, 150, 200, 30), "colorFontField");
    ASSERT_NE(idx, -1);

    EXPECT_TRUE(manager_->setFieldDefaultAppearance(
        page.get(), idx, "Cour", 14.0f, QColor(255, 0, 0)));
    const std::string da = manager_->getFieldDefaultAppearance(page.get(), idx);

    EXPECT_NE(da.find("/Cour"), std::string::npos) << da;
    EXPECT_NE(da.find("14 Tf"), std::string::npos) << da;
    // A non-black colour must use the 3-component rg operator, not "0 g".
    EXPECT_NE(da.find("rg"), std::string::npos)
        << "non-black colour should emit an rg operator, got: " << da;
    EXPECT_NE(da.find("1 0 0"), std::string::npos) << da;
}

TEST_F(FormCreationTest, SetFieldFontCommand_ExecuteUndoRedo) {
    if (!document_) {
        GTEST_SKIP() << "test_single.pdf not available";
    }
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    int idx = manager_->createFormField(
        page.get(), pdfe::forms::FormFieldType::Text,
        QRectF(120, 200, 200, 30), "cmdFontField");
    ASSERT_NE(idx, -1);

    const std::string daBefore =
        manager_->getFieldDefaultAppearance(page.get(), idx);

    // Pipeline form-reconcile: the one /DA command is SetFieldAppearanceCommand.
    auto cmd = std::make_unique<pdfe::forms::SetFieldAppearanceCommand>(
        manager_.get(), page.get(), idx, "Cour", 18.0f, QColor(Qt::black));
    undoManager_->execute(std::move(cmd));

    const std::string daAfter =
        manager_->getFieldDefaultAppearance(page.get(), idx);
    EXPECT_NE(daAfter.find("/Cour"), std::string::npos) << daAfter;
    EXPECT_NE(daAfter.find("18 Tf"), std::string::npos) << daAfter;

    // Undo must restore the exact previous /DA byte-for-byte.
    undoManager_->undo();
    EXPECT_EQ(manager_->getFieldDefaultAppearance(page.get(), idx), daBefore)
        << "undo did not restore the original /DA";

    // Redo must re-apply the font change.
    undoManager_->redo();
    const std::string daRedo =
        manager_->getFieldDefaultAppearance(page.get(), idx);
    EXPECT_NE(daRedo.find("/Cour"), std::string::npos) << daRedo;
    EXPECT_NE(daRedo.find("18 Tf"), std::string::npos) << daRedo;
}

// Pipeline form-reconcile, decision 4 (bold/italic). origin/master mapped a
// UI family plus bold/italic to a base-14 /DA key through the static
// FormManager::base14FontName ("Arial" + bold -> "HeBo"). MuPDF 1.26.10 cannot
// render those style-variant keys: pdf_parse_default_appearance and
// full_font_name map every name other than Helv/TiRo/Cour/Symb/ZaDb to
// Helvetica, so a /HeBo field renders (and reads back) as regular Helvetica.
// The reconciled design keeps the family -> key mapping in ONE place, the
// validated setter's key set, and rejects the variants rather than writing a
// /DA that claims a style the field never shows. This test pins that mapping
// through the single API: each of the five renderable base-14 families lands
// as its key, and every style-variant key (and a UI family name, which is not
// a key) is refused with the field's /DA unchanged.
TEST_F(FormCreationTest, Base14FontNameMapping) {
    using pdfe::forms::isBase14DefaultAppearanceFont;
    ASSERT_NE(document_, nullptr) << "test_single.pdf not available";
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);
    const int idx = manager_->createFormField(
        page.get(), pdfe::forms::FormFieldType::Text,
        QRectF(100, 320, 200, 30), "base14MapField");
    ASSERT_NE(idx, -1);

    // Helvetica, Times, Courier, Symbol and ZapfDingbats: accepted, and the
    // /DA names exactly that key.
    for (const char* key : {"Helv", "TiRo", "Cour", "Symb", "ZaDb"}) {
        EXPECT_TRUE(isBase14DefaultAppearanceFont(key)) << key;
        EXPECT_TRUE(manager_->setFieldDefaultAppearance(
            page.get(), idx, key, 12.0f, QColor(Qt::black)))
            << "base-14 key " << key << " was refused";
        const std::string da = manager_->getFieldDefaultAppearance(page.get(), idx);
        EXPECT_EQ(da.rfind(std::string("/") + key + " ", 0), 0u)
            << "base-14 key " << key << " did not land in the /DA, got: " << da;
    }

    // Bold, italic and bold-italic of Helvetica, Times and Courier, and UI
    // family names: refused, the /DA left exactly as it was.
    for (const char* key : {"HeBo", "HeOb", "HeBO", "TiBo", "TiIt", "TiBI",
                            "CoBo", "CoOb", "CoBO", "Helvetica", "Arial",
                            "Times New Roman", "sans-serif"}) {
        EXPECT_FALSE(isBase14DefaultAppearanceFont(key)) << key;
        const std::string before = manager_->getFieldDefaultAppearance(page.get(), idx);
        EXPECT_FALSE(manager_->setFieldDefaultAppearance(
            page.get(), idx, key, 12.0f, QColor(Qt::black)))
            << key << " is not a renderable base-14 key and must be refused";
        EXPECT_EQ(manager_->getFieldDefaultAppearance(page.get(), idx), before)
            << "a refused " << key << " request modified the /DA";
    }
}

TEST_F(FormCreationTest, SetFieldFont_PersistsAcrossSaveReopen) {
    if (!document_) {
        GTEST_SKIP() << "test_single.pdf not available";
    }
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    int idx = manager_->createFormField(
        page.get(), pdfe::forms::FormFieldType::Text,
        QRectF(100, 260, 200, 30), "persistFontField");
    ASSERT_NE(idx, -1);
    // Give it a value so the appearance is non-blank, then change the font.
    manager_->setFieldValue(page.get(), idx, "Hello");
    ASSERT_TRUE(manager_->setFieldDefaultAppearance(
        page.get(), idx, "TiRo", 22.0f, QColor(Qt::black)));

    const auto outPath = std::filesystem::temp_directory_path() /
                         "pdfe_phase5_font_roundtrip.pdf";
    std::error_code ec;
    std::filesystem::remove(outPath, ec);
    ASSERT_NO_THROW(document_->saveAs(outPath));
    ASSERT_TRUE(std::filesystem::exists(outPath));

    // Reopen in a FRESH context + manager to prove the /DA is on disk, not just
    // in the live in-memory model.
    auto ctx2 = std::make_shared<pdfe::core::MuPdfContext>();
    auto reopened = pdfe::core::Document::open(ctx2, outPath);
    ASSERT_NE(reopened, nullptr);
    auto mgr2 = std::make_unique<pdfe::forms::FormManager>(ctx2);
    auto rpage = reopened->page(0);
    ASSERT_NE(rpage, nullptr);

    bool found = false;
    for (const auto& f : mgr2->getFormFields(rpage.get())) {
        if (f.fieldName == "persistFontField") {
            found = true;
            EXPECT_EQ(f.type, pdfe::forms::FormFieldType::Text)
                << "field must round-trip as an interactive text field";
            EXPECT_TRUE(f.defaultAppearanceStringCaptured);
            EXPECT_NE(f.defaultAppearanceString.find("/TiRo"), std::string::npos)
                << "font family did not persist across save/reopen, got: "
                << f.defaultAppearanceString;
            EXPECT_NE(f.defaultAppearanceString.find("22 Tf"), std::string::npos)
                << "font size did not persist across save/reopen, got: "
                << f.defaultAppearanceString;
        }
    }
    EXPECT_TRUE(found)
        << "the field with the changed font must survive the round-trip";

    reopened.reset();
    mgr2.reset();
    ctx2.reset();
    std::filesystem::remove(outPath, ec);
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
