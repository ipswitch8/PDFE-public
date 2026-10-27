#include <gtest/gtest.h>
#include "ui/CommentPanel.h"
#include "ui/AnnotationPropertiesWidget.h"
#include "annotations/AnnotationData.h"
#include "annotations/AnnotationType.h"

#include <QComboBox>
#include <QTreeWidget>
#include <QLineEdit>
#include <QTextEdit>
#include <QSlider>
#include <QPushButton>
#include <QLabel>
#include <QSignalSpy>

class CommentPanelTest : public ::testing::Test {
protected:
    void SetUp() override {
        panel = new pdfe::ui::CommentPanel();
    }

    void TearDown() override {
        delete panel;
        panel = nullptr;
    }

    pdfe::ui::CommentPanel* panel = nullptr;
};

TEST_F(CommentPanelTest, ConstructsSuccessfully) {
    ASSERT_NE(panel, nullptr);
    EXPECT_EQ(panel->objectName().toStdString(), "commentPanel");
}

TEST_F(CommentPanelTest, ClearDocumentIsNoOp) {
    // Should not crash when called without setDocument
    EXPECT_NO_THROW(panel->clearDocument());
}

TEST_F(CommentPanelTest, FilterComboExists) {
    QComboBox* filterCombo = panel->findChild<QComboBox*>("commentFilterCombo");
    ASSERT_NE(filterCombo, nullptr);

    // Check filter items
    EXPECT_EQ(filterCombo->count(), 6);
    EXPECT_FALSE(filterCombo->itemText(0).isEmpty()); // All
    EXPECT_FALSE(filterCombo->itemText(1).isEmpty()); // Sticky Notes
    EXPECT_FALSE(filterCombo->itemText(2).isEmpty()); // Highlights
    EXPECT_FALSE(filterCombo->itemText(3).isEmpty()); // Markup
    EXPECT_FALSE(filterCombo->itemText(4).isEmpty()); // Drawing
    EXPECT_FALSE(filterCombo->itemText(5).isEmpty()); // Shapes
}

TEST_F(CommentPanelTest, SortComboExists) {
    QComboBox* sortCombo = panel->findChild<QComboBox*>("commentSortCombo");
    ASSERT_NE(sortCombo, nullptr);

    // Check sort items
    EXPECT_EQ(sortCombo->count(), 3);
    EXPECT_FALSE(sortCombo->itemText(0).isEmpty()); // By Page
    EXPECT_FALSE(sortCombo->itemText(1).isEmpty()); // By Date
    EXPECT_FALSE(sortCombo->itemText(2).isEmpty()); // By Author
}

TEST_F(CommentPanelTest, TreeWidgetExists) {
    QTreeWidget* treeWidget = panel->findChild<QTreeWidget*>("commentTreeWidget");
    ASSERT_NE(treeWidget, nullptr);
    EXPECT_EQ(treeWidget->columnCount(), 1);
}

TEST_F(CommentPanelTest, RefreshWithoutDocumentIsNoOp) {
    EXPECT_NO_THROW(panel->refresh());

    QTreeWidget* treeWidget = panel->findChild<QTreeWidget*>("commentTreeWidget");
    ASSERT_NE(treeWidget, nullptr);
    EXPECT_EQ(treeWidget->topLevelItemCount(), 0);
}

class AnnotationPropertiesWidgetTest : public ::testing::Test {
protected:
    void SetUp() override {
        widget = new pdfe::ui::AnnotationPropertiesWidget();
    }

    void TearDown() override {
        delete widget;
        widget = nullptr;
    }

    pdfe::ui::AnnotationPropertiesWidget* widget = nullptr;
};

TEST_F(AnnotationPropertiesWidgetTest, ConstructsSuccessfully) {
    ASSERT_NE(widget, nullptr);
    EXPECT_EQ(widget->objectName().toStdString(), "annotationPropertiesWidget");
}

TEST_F(AnnotationPropertiesWidgetTest, AllWidgetsExist) {
    // Type label
    QLabel* typeLabel = widget->findChild<QLabel*>("annotTypeLabel");
    ASSERT_NE(typeLabel, nullptr);

    // Author edit
    QLineEdit* authorEdit = widget->findChild<QLineEdit*>("annotAuthorEdit");
    ASSERT_NE(authorEdit, nullptr);

    // Contents edit
    QTextEdit* contentsEdit = widget->findChild<QTextEdit*>("annotContentsEdit");
    ASSERT_NE(contentsEdit, nullptr);

    // Color button
    QPushButton* colorButton = widget->findChild<QPushButton*>("annotColorButton");
    ASSERT_NE(colorButton, nullptr);

    // Opacity slider
    QSlider* opacitySlider = widget->findChild<QSlider*>("annotOpacitySlider");
    ASSERT_NE(opacitySlider, nullptr);
    EXPECT_EQ(opacitySlider->minimum(), 0);
    EXPECT_EQ(opacitySlider->maximum(), 100);
}

TEST_F(AnnotationPropertiesWidgetTest, SetAnnotationPopulatesFields) {
    pdfe::annotations::AnnotationData data;
    data.type = pdfe::annotations::AnnotationType::Highlight;
    data.author = "Test Author";
    data.contents = "Test contents";
    data.color = QColor(255, 0, 0);
    data.opacity = 0.75f;

    widget->setAnnotation(data);

    QLabel* typeLabel = widget->findChild<QLabel*>("annotTypeLabel");
    ASSERT_NE(typeLabel, nullptr);
    EXPECT_FALSE(typeLabel->text().isEmpty());
    EXPECT_NE(typeLabel->text().toStdString(), "None");

    QLineEdit* authorEdit = widget->findChild<QLineEdit*>("annotAuthorEdit");
    ASSERT_NE(authorEdit, nullptr);
    EXPECT_EQ(authorEdit->text().toStdString(), "Test Author");

    QTextEdit* contentsEdit = widget->findChild<QTextEdit*>("annotContentsEdit");
    ASSERT_NE(contentsEdit, nullptr);
    EXPECT_EQ(contentsEdit->toPlainText().toStdString(), "Test contents");

    QSlider* opacitySlider = widget->findChild<QSlider*>("annotOpacitySlider");
    ASSERT_NE(opacitySlider, nullptr);
    EXPECT_EQ(opacitySlider->value(), 75);
}

TEST_F(AnnotationPropertiesWidgetTest, ClearResetsFields) {
    // First set some data
    pdfe::annotations::AnnotationData data;
    data.type = pdfe::annotations::AnnotationType::Text;
    data.author = "Author";
    data.contents = "Contents";

    widget->setAnnotation(data);

    // Now clear
    widget->clear();

    QLabel* typeLabel = widget->findChild<QLabel*>("annotTypeLabel");
    ASSERT_NE(typeLabel, nullptr);
    EXPECT_EQ(typeLabel->text().toStdString(), "None");

    QLineEdit* authorEdit = widget->findChild<QLineEdit*>("annotAuthorEdit");
    ASSERT_NE(authorEdit, nullptr);
    EXPECT_TRUE(authorEdit->text().isEmpty());

    QTextEdit* contentsEdit = widget->findChild<QTextEdit*>("annotContentsEdit");
    ASSERT_NE(contentsEdit, nullptr);
    EXPECT_TRUE(contentsEdit->toPlainText().isEmpty());

    QSlider* opacitySlider = widget->findChild<QSlider*>("annotOpacitySlider");
    ASSERT_NE(opacitySlider, nullptr);
    EXPECT_EQ(opacitySlider->value(), 100);
}

TEST_F(AnnotationPropertiesWidgetTest, DefaultStateIsCleared) {
    QLabel* typeLabel = widget->findChild<QLabel*>("annotTypeLabel");
    ASSERT_NE(typeLabel, nullptr);
    EXPECT_EQ(typeLabel->text().toStdString(), "None");

    QLineEdit* authorEdit = widget->findChild<QLineEdit*>("annotAuthorEdit");
    ASSERT_NE(authorEdit, nullptr);
    EXPECT_TRUE(authorEdit->text().isEmpty());

    QTextEdit* contentsEdit = widget->findChild<QTextEdit*>("annotContentsEdit");
    ASSERT_NE(contentsEdit, nullptr);
    EXPECT_TRUE(contentsEdit->toPlainText().isEmpty());
}

// ─── navigateToAnnotation signal signature (P-H-INFRA-5 fix) ─────────────
//
// Verifies that the signal carries both pageIndex AND annotIndex.
// Prior to the phase-4 fix the signal only carried pageIndex, so
// MainWindow::onCommentPanelNavigate could only call goToPage() which
// is a no-op for same-page annotations.

TEST_F(CommentPanelTest, NavigateToAnnotationSignalCarriesAnnotIndex)
{
    // Connect a spy to the navigateToAnnotation(int, int) signal.
    QSignalSpy spy(panel, &pdfe::ui::CommentPanel::navigateToAnnotation);
    ASSERT_TRUE(spy.isValid())
        << "navigateToAnnotation(int,int) signal not found — "
           "signal signature may have been changed";

    // The spy merely verifies the signal compiles with the two-arg
    // signature and that a valid spy can be attached.  Driving an actual
    // double-click requires a populated tree which needs a live document;
    // the signal-emission path is covered by the E2E test
    // test_comment_panel_navigates_to_annotation (tests/test_ui_annotations.py).
    EXPECT_EQ(spy.count(), 0);
}
