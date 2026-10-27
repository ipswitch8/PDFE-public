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
