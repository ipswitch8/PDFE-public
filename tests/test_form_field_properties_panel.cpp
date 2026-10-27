#include <gtest/gtest.h>
#include "ui/FormFieldPropertiesPanel.h"
#include "forms/FormFieldData.h"
#include "forms/FormFieldType.h"
#include <QApplication>
#include <QLineEdit>
#include <QTextEdit>
#include <QCheckBox>
#include <QComboBox>
#include <QListWidget>
#include <QStackedWidget>
#include <QLabel>

using namespace pdfe::forms;
using namespace pdfe::ui;

class FormFieldPropertiesPanelTest : public ::testing::Test {
protected:
    void SetUp() override {
        panel = std::make_unique<FormFieldPropertiesPanel>();
    }

    void TearDown() override {
        panel.reset();
    }

    std::unique_ptr<FormFieldPropertiesPanel> panel;
};

TEST_F(FormFieldPropertiesPanelTest, ObjectNameSet) {
    EXPECT_EQ(panel->objectName(), QString("formFieldPropertiesPanel"));
}

TEST_F(FormFieldPropertiesPanelTest, SetTextFieldShowsLineEdit) {
    FormFieldData data;
    data.type = FormFieldType::Text;
    data.fieldName = "TestField";
    data.value = "Hello";
    data.isMultiline = false;

    panel->setFormField(data);

    auto* stack = panel->findChild<QStackedWidget*>("formFieldEditorStack");
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->currentIndex(), 0); // Single-line text

    auto* textEdit = panel->findChild<QLineEdit*>("formFieldTextEdit");
    ASSERT_NE(textEdit, nullptr);
    EXPECT_EQ(textEdit->text(), QString("Hello"));
}

TEST_F(FormFieldPropertiesPanelTest, SetMultilineFieldShowsTextEdit) {
    FormFieldData data;
    data.type = FormFieldType::Text;
    data.fieldName = "MultiField";
    data.value = "Line1\nLine2";
    data.isMultiline = true;

    panel->setFormField(data);

    auto* stack = panel->findChild<QStackedWidget*>("formFieldEditorStack");
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->currentIndex(), 1); // Multiline text
}

TEST_F(FormFieldPropertiesPanelTest, SetCheckboxFieldShowsCheckbox) {
    FormFieldData data;
    data.type = FormFieldType::Checkbox;
    data.fieldName = "Check1";
    data.value = "Yes";

    panel->setFormField(data);

    auto* stack = panel->findChild<QStackedWidget*>("formFieldEditorStack");
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->currentIndex(), 2); // Checkbox

    auto* cb = panel->findChild<QCheckBox*>("formFieldCheckbox");
    ASSERT_NE(cb, nullptr);
    EXPECT_TRUE(cb->isChecked());
}

TEST_F(FormFieldPropertiesPanelTest, SetComboFieldShowsCombo) {
    FormFieldData data;
    data.type = FormFieldType::ComboBox;
    data.fieldName = "Country";
    data.value = "Canada";
    data.options = {"USA", "Canada", "UK"};

    panel->setFormField(data);

    auto* stack = panel->findChild<QStackedWidget*>("formFieldEditorStack");
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->currentIndex(), 3); // Combo

    auto* combo = panel->findChild<QComboBox*>("formFieldCombo");
    ASSERT_NE(combo, nullptr);
    EXPECT_EQ(combo->count(), 3);
    EXPECT_EQ(combo->currentText(), QString("Canada"));
}

TEST_F(FormFieldPropertiesPanelTest, SetListFieldShowsList) {
    FormFieldData data;
    data.type = FormFieldType::ListBox;
    data.fieldName = "Interests";
    data.options = {"Music", "Sports", "Reading"};
    data.selectedIndices = {1};

    panel->setFormField(data);

    auto* stack = panel->findChild<QStackedWidget*>("formFieldEditorStack");
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->currentIndex(), 4); // List

    auto* list = panel->findChild<QListWidget*>("formFieldList");
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->count(), 3);
}

TEST_F(FormFieldPropertiesPanelTest, ClearResetsPanel) {
    FormFieldData data;
    data.type = FormFieldType::Text;
    data.fieldName = "TestField";
    data.value = "Hello";

    panel->setFormField(data);
    panel->clear();

    auto* typeLabel = panel->findChild<QLabel*>("formFieldTypeLabel");
    ASSERT_NE(typeLabel, nullptr);
    EXPECT_EQ(typeLabel->text(), QObject::tr("None"));

    auto* nameLabel = panel->findChild<QLabel*>("formFieldNameLabel");
    ASSERT_NE(nameLabel, nullptr);
    EXPECT_TRUE(nameLabel->text().isEmpty());

    auto* stack = panel->findChild<QStackedWidget*>("formFieldEditorStack");
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->currentIndex(), 5); // No editor
}

TEST_F(FormFieldPropertiesPanelTest, ValueChangeEmitsSignal) {
    FormFieldData data;
    data.type = FormFieldType::Text;
    data.fieldName = "TestField";
    data.value = "";
    data.isMultiline = false;

    panel->setFormField(data);

    bool signalEmitted = false;
    QObject::connect(panel.get(), &FormFieldPropertiesPanel::fieldValueChanged,
        [&signalEmitted](const FormFieldData& fd) {
            signalEmitted = true;
        });

    auto* textEdit = panel->findChild<QLineEdit*>("formFieldTextEdit");
    ASSERT_NE(textEdit, nullptr);
    textEdit->setText("New Value");
    // Simulate editing finished
    emit textEdit->editingFinished();

    EXPECT_TRUE(signalEmitted);
}

TEST_F(FormFieldPropertiesPanelTest, ReadOnlyFieldDisablesEditor) {
    FormFieldData data;
    data.type = FormFieldType::Text;
    data.fieldName = "ReadOnlyField";
    data.value = "Locked";
    data.isReadOnly = true;

    panel->setFormField(data);

    auto* textEdit = panel->findChild<QLineEdit*>("formFieldTextEdit");
    ASSERT_NE(textEdit, nullptr);
    EXPECT_TRUE(textEdit->isReadOnly());
}

TEST_F(FormFieldPropertiesPanelTest, FlagsDisplayed) {
    FormFieldData data;
    data.type = FormFieldType::Text;
    data.fieldName = "FlagsField";
    data.isReadOnly = true;
    data.isRequired = true;

    panel->setFormField(data);

    auto* flagsLabel = panel->findChild<QLabel*>("formFieldFlagsLabel");
    ASSERT_NE(flagsLabel, nullptr);
    EXPECT_TRUE(flagsLabel->text().contains("Read-Only"));
    EXPECT_TRUE(flagsLabel->text().contains("Required"));
}

// ---------------------------------------------------------------------------
// Pipeline c91e7f3a phase-5: setTextValueForTesting tests
// ---------------------------------------------------------------------------

TEST_F(FormFieldPropertiesPanelTest, SetTextValueForTestingEmitsSignal) {
    // Arrange: single-line text field.
    FormFieldData data;
    data.type = FormFieldType::Text;
    data.fieldName = "TestField";
    data.value = "";
    data.isMultiline = false;
    panel->setFormField(data);

    FormFieldData received;
    bool signalEmitted = false;
    QObject::connect(panel.get(), &FormFieldPropertiesPanel::fieldValueChanged,
        [&](const FormFieldData& fd) {
            received = fd;
            signalEmitted = true;
        });

    // Act: call the test seam.
    panel->setTextValueForTesting("CommittedValue");

    // Assert: signal fired with the new value.
    EXPECT_TRUE(signalEmitted);
    EXPECT_EQ(received.value, std::string("CommittedValue"));
}

TEST_F(FormFieldPropertiesPanelTest, SetTextValueForTestingUpdatesWidget) {
    FormFieldData data;
    data.type = FormFieldType::Text;
    data.fieldName = "F";
    data.value = "OldValue";
    data.isMultiline = false;
    panel->setFormField(data);

    panel->setTextValueForTesting("NewValue");

    auto* textEdit = panel->findChild<QLineEdit*>("formFieldTextEdit");
    ASSERT_NE(textEdit, nullptr);
    EXPECT_EQ(textEdit->text(), QString("NewValue"));
}

TEST_F(FormFieldPropertiesPanelTest, SetTextValueForTestingActsOnMultiline) {
    // Pipeline c91e7f3a phase-5: setTextValueForTesting now handles both
    // single-line (stack 0) and multiline (stack 1) text fields so that the
    // Python test pipe can commit multiline values without the editingFinished race.
    FormFieldData data;
    data.type = FormFieldType::Text;
    data.fieldName = "Multi";
    data.value = "Original";
    data.isMultiline = true;
    panel->setFormField(data);

    FormFieldData received;
    bool signalEmitted = false;
    QObject::connect(panel.get(), &FormFieldPropertiesPanel::fieldValueChanged,
        [&](const FormFieldData& fd) {
            received = fd;
            signalEmitted = true;
        });

    panel->setTextValueForTesting("NewMultiline");

    EXPECT_TRUE(signalEmitted);
    EXPECT_EQ(received.value, std::string("NewMultiline"));

    auto* multiEdit = panel->findChild<QTextEdit*>("formFieldMultilineEdit");
    ASSERT_NE(multiEdit, nullptr);
    EXPECT_EQ(multiEdit->toPlainText(), QString("NewMultiline"));
}

TEST_F(FormFieldPropertiesPanelTest, SetTextValueForTestingNoOpBeforeSetFormField) {
    // Panel in initial (cleared) state — stack index is 5 (no editor).
    // setTextValueForTesting must not crash or emit.
    bool signalEmitted = false;
    QObject::connect(panel.get(), &FormFieldPropertiesPanel::fieldValueChanged,
        [&](const FormFieldData&) { signalEmitted = true; });

    panel->setTextValueForTesting("Ignored");

    EXPECT_FALSE(signalEmitted);
}

// ---------------------------------------------------------------------------
// Pipeline c91e7f3a phase-7: currentTextValue() seam tests
// ---------------------------------------------------------------------------

TEST_F(FormFieldPropertiesPanelTest, CurrentTextValueReturnsSingleLineText) {
    // Arrange: single-line text field with a known value.
    FormFieldData data;
    data.type = FormFieldType::Text;
    data.fieldName = "SingleField";
    data.value = "hello";
    data.isMultiline = false;
    panel->setFormField(data);

    // Verify stack is at index 0 (single-line).
    auto* stack = panel->findChild<QStackedWidget*>("formFieldEditorStack");
    ASSERT_NE(stack, nullptr);
    ASSERT_EQ(stack->currentIndex(), 0);

    // Act + Assert: currentTextValue() returns the displayed text.
    EXPECT_EQ(panel->currentTextValue(), QString("hello"));
}

TEST_F(FormFieldPropertiesPanelTest, CurrentTextValueReturnsMultilineText) {
    // Arrange: multiline text field.
    FormFieldData data;
    data.type = FormFieldType::Text;
    data.fieldName = "MultiField";
    data.value = "line1\nline2";
    data.isMultiline = true;
    panel->setFormField(data);

    // Verify stack is at index 1 (multiline).
    auto* stack = panel->findChild<QStackedWidget*>("formFieldEditorStack");
    ASSERT_NE(stack, nullptr);
    ASSERT_EQ(stack->currentIndex(), 1);

    // Act + Assert: currentTextValue() returns the full multiline text.
    EXPECT_EQ(panel->currentTextValue(), QString("line1\nline2"));
}

TEST_F(FormFieldPropertiesPanelTest, CurrentTextValueReturnsEmptyForOtherStacks) {
    // Arrange: checkbox field — stack index 2, not a text stack.
    FormFieldData data;
    data.type = FormFieldType::Checkbox;
    data.fieldName = "Check1";
    data.value = "Yes";
    panel->setFormField(data);

    // Verify stack is at index 2 (checkbox).
    auto* stack = panel->findChild<QStackedWidget*>("formFieldEditorStack");
    ASSERT_NE(stack, nullptr);
    ASSERT_EQ(stack->currentIndex(), 2);

    // Act + Assert: currentTextValue() returns empty for non-text stacks.
    EXPECT_EQ(panel->currentTextValue(), QString(""));
}
