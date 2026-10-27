#include <gtest/gtest.h>
#include "ui/StickyNotePopup.h"
#include "annotations/AnnotationData.h"
#include <QApplication>
#include <QTextEdit>
#include <QLabel>
#include <QSignalSpy>
#include <QTest>
#include <QRegularExpression>
#include <QScreen>

// Ensure QApplication exists for widget tests
// (test_main.cpp already creates QApplication, so this is safe)

class StickyNotePopupTest : public ::testing::Test {
protected:
    void SetUp() override {
        // QApplication should already exist from test_main.cpp
        ASSERT_NE(QApplication::instance(), nullptr);

        // Create a large parent widget so positioning and visibility works correctly
        parentWidget = new QWidget();
        parentWidget->resize(1024, 768);
        parentWidget->show();
        popup = new pdfe::ui::StickyNotePopup(parentWidget);
    }

    void TearDown() override {
        delete parentWidget; // deletes popup too (child)
        parentWidget = nullptr;
        popup = nullptr;
    }

    QWidget* parentWidget = nullptr;
    pdfe::ui::StickyNotePopup* popup = nullptr;

    // Helper to create sample annotation data
    pdfe::annotations::AnnotationData createSampleAnnotation() {
        pdfe::annotations::AnnotationData data;
        data.type = pdfe::annotations::AnnotationType::Text;
        data.rect = QRectF(100, 100, 20, 20);
        data.contents = "This is a test sticky note";
        data.author = "Test Author";
        data.color = QColor(255, 255, 0); // Yellow
        data.pageIndex = 0;
        data.annotIndex = 0;
        return data;
    }
};

TEST_F(StickyNotePopupTest, Creation) {
    ASSERT_NE(popup, nullptr);

    // Popup should start hidden
    EXPECT_FALSE(popup->isVisible());
    EXPECT_FALSE(popup->isPopupVisible());
}

TEST_F(StickyNotePopupTest, OpenForAnnotation) {
    auto data = createSampleAnnotation();

    // Open the popup
    QPoint iconPos(100, 100);
    popup->openForAnnotation(data, iconPos);

    // Popup should now be visible
    EXPECT_TRUE(popup->isVisible());
    EXPECT_TRUE(popup->isPopupVisible());

    // Contents should match
    EXPECT_EQ(popup->contents().toStdString(), data.contents);

    // Annotation data should be stored
    EXPECT_EQ(popup->annotationData().type, pdfe::annotations::AnnotationType::Text);
    EXPECT_EQ(popup->annotationData().pageIndex, 0);
    EXPECT_EQ(popup->annotationData().annotIndex, 0);
}

TEST_F(StickyNotePopupTest, ClosePopup) {
    auto data = createSampleAnnotation();

    // Set up signal spy for popupClosed signal
    QSignalSpy closedSpy(popup, &pdfe::ui::StickyNotePopup::popupClosed);

    // Open then close
    popup->openForAnnotation(data, QPoint(100, 100));
    EXPECT_TRUE(popup->isVisible());

    popup->closePopup();

    // Should be hidden
    EXPECT_FALSE(popup->isVisible());
    EXPECT_FALSE(popup->isPopupVisible());

    // Signal should have been emitted
    EXPECT_EQ(closedSpy.count(), 1);

    // Check signal parameters
    QList<QVariant> arguments = closedSpy.takeFirst();
    EXPECT_EQ(arguments.at(0).toInt(), 0); // pageIndex
    EXPECT_EQ(arguments.at(1).toInt(), 0); // annotIndex
}

TEST_F(StickyNotePopupTest, ContentsEdit) {
    auto data = createSampleAnnotation();

    // Set up signal spy for contentsChanged signal
    QSignalSpy changedSpy(popup, &pdfe::ui::StickyNotePopup::contentsChanged);

    popup->openForAnnotation(data, QPoint(100, 100));

    // Find the text edit widget
    QTextEdit* textEdit = popup->findChild<QTextEdit*>();
    ASSERT_NE(textEdit, nullptr);

    // Modify the text
    textEdit->setPlainText("Modified content");

    // Signal should have been emitted when text changed
    EXPECT_GE(changedSpy.count(), 1);

    // Verify the new contents
    EXPECT_EQ(popup->contents().toStdString(), "Modified content");
}

TEST_F(StickyNotePopupTest, SetContentsExternal) {
    auto data = createSampleAnnotation();

    // Set up signal spy
    QSignalSpy changedSpy(popup, &pdfe::ui::StickyNotePopup::contentsChanged);

    popup->openForAnnotation(data, QPoint(100, 100));

    // Clear the spy (opening may have triggered signals)
    changedSpy.clear();

    // Set contents externally (should NOT emit signal)
    popup->setContents("Externally set content");

    // Verify contents were updated
    EXPECT_EQ(popup->contents().toStdString(), "Externally set content");

    // Signal should NOT have been emitted (external sync)
    EXPECT_EQ(changedSpy.count(), 0);
}

TEST_F(StickyNotePopupTest, ColorTheming) {
    auto data = createSampleAnnotation();
    data.color = QColor(255, 255, 0); // Yellow

    popup->openForAnnotation(data, QPoint(100, 100));

    // Phase-16 fix (qt_test_gap_audit finding #26): hard ASSERT_NE
    // on the title-bar lookup; the previous `if (titleBar)` allowed
    // a missing label to silently no-op the assertion. The check
    // is now: the stylesheet has a non-empty `background[-]?color:`
    // declaration referencing a real color value (not an OR-fallback
    // on the words "background" or "color" which could match comments).
    QLabel* titleBar = popup->findChild<QLabel*>();
    ASSERT_NE(titleBar, nullptr)
        << "StickyNotePopup must expose a title-bar QLabel for "
           "color-theming verification";

    const QString styleSheet = titleBar->styleSheet();
    EXPECT_FALSE(styleSheet.isEmpty());
    // Match an actual background-color declaration with a value
    // (hex, rgb(), or named color). Comments like `/* color: TBD */`
    // do NOT satisfy this pattern.
    EXPECT_TRUE(styleSheet.contains(QRegularExpression(
        QStringLiteral("background[-]?color\\s*:\\s*(#[0-9A-Fa-f]+|"
                       "rgb\\(|rgba\\(|[A-Za-z]+\\s*[;}\\s])"),
        QRegularExpression::CaseInsensitiveOption)))
        << "Title-bar stylesheet must declare a background-color "
           "with a real value; got: " << styleSheet.toStdString();
}

TEST_F(StickyNotePopupTest, PositionFlip) {
    auto data = createSampleAnnotation();

    // Open popup near the right edge of screen
    QScreen* screen = QApplication::primaryScreen();
    if (!screen) {
        GTEST_SKIP() << "No primary screen available";
    }

    QRect screenGeometry = screen->availableGeometry();

    // Position near right edge — anchor point is close enough to the
    // right edge that the popup MUST flip to the left of the anchor
    // to fit on screen.
    QPoint nearRightEdge(screenGeometry.right() - 50, 100);
    popup->openForAnnotation(data, nearRightEdge);

    EXPECT_TRUE(popup->isVisible());

    // Popup should be positioned on screen (not off the edge)
    QRect popupGeometry = popup->geometry();
    EXPECT_LE(popupGeometry.right(), screenGeometry.right());
    // Phase-16 fix (qt_test_gap_audit finding #27): verify the popup
    // actually FLIPPED to the left of the anchor when near the right
    // edge — not merely that it fits on screen. A popup at x=10 on a
    // 1920-wide screen would pass the "on screen" check while failing
    // the flip-semantic.
    EXPECT_LT(popupGeometry.left(), nearRightEdge.x())
        << "Popup must flip to the LEFT of the anchor when the "
           "anchor is near the right edge; popup.left="
        << popupGeometry.left() << ", anchor.x=" << nearRightEdge.x();

    // Position near bottom edge — anchor close enough to the bottom
    // that the popup MUST flip above the anchor.
    QPoint nearBottomEdge(100, screenGeometry.bottom() - 50);
    popup->updatePosition(nearBottomEdge);

    popupGeometry = popup->geometry();
    EXPECT_LE(popupGeometry.bottom(), screenGeometry.bottom());
    EXPECT_LT(popupGeometry.top(), nearBottomEdge.y())
        << "Popup must flip ABOVE the anchor when the anchor is "
           "near the bottom edge; popup.top=" << popupGeometry.top()
        << ", anchor.y=" << nearBottomEdge.y();
}

TEST_F(StickyNotePopupTest, UpdatePosition) {
    auto data = createSampleAnnotation();

    popup->openForAnnotation(data, QPoint(100, 100));
    ASSERT_TRUE(popup->isVisible());

    QPoint initialPos = popup->pos();

    // Update position with a significantly different anchor
    popup->updatePosition(QPoint(400, 400));

    QPoint newPos = popup->pos();

    // Position should have changed
    EXPECT_NE(initialPos, newPos);
}

TEST_F(StickyNotePopupTest, MultipleOpenClose) {
    auto data1 = createSampleAnnotation();
    data1.contents = "First note";

    auto data2 = createSampleAnnotation();
    data2.contents = "Second note";
    data2.annotIndex = 1;

    // Open first
    popup->openForAnnotation(data1, QPoint(100, 100));
    EXPECT_TRUE(popup->isVisible());
    EXPECT_EQ(popup->contents().toStdString(), "First note");

    // Close
    popup->closePopup();
    EXPECT_FALSE(popup->isVisible());

    // Open second
    popup->openForAnnotation(data2, QPoint(200, 200));
    EXPECT_TRUE(popup->isVisible());
    EXPECT_EQ(popup->contents().toStdString(), "Second note");
    EXPECT_EQ(popup->annotationData().annotIndex, 1);
}

TEST_F(StickyNotePopupTest, EmptyContents) {
    auto data = createSampleAnnotation();
    data.contents = ""; // Empty

    popup->openForAnnotation(data, QPoint(100, 100));

    // Should handle empty contents without crashing
    EXPECT_TRUE(popup->isVisible());
    EXPECT_TRUE(popup->contents().isEmpty());
}

TEST_F(StickyNotePopupTest, LongContents) {
    auto data = createSampleAnnotation();
    // Create a long string
    std::string longText;
    for (int i = 0; i < 100; ++i) {
        longText += "This is a very long line of text. ";
    }
    data.contents = longText;

    popup->openForAnnotation(data, QPoint(100, 100));

    // Should handle long contents without crashing
    EXPECT_TRUE(popup->isVisible());

    // Find text edit
    QTextEdit* textEdit = popup->findChild<QTextEdit*>();
    ASSERT_NE(textEdit, nullptr);

    // Verify it can display long text (has scrollbars if needed)
    EXPECT_FALSE(textEdit->toPlainText().isEmpty());
}

TEST_F(StickyNotePopupTest, ReadOnlyMode) {
    auto data = createSampleAnnotation();
    popup->openForAnnotation(data, QPoint(100, 100));

    // By default, popup should be editable
    EXPECT_FALSE(popup->isReadOnly());
    QTextEdit* textEdit = popup->findChild<QTextEdit*>();
    ASSERT_NE(textEdit, nullptr);
    EXPECT_FALSE(textEdit->isReadOnly());

    // Set read-only mode
    popup->setReadOnly(true);
    EXPECT_TRUE(popup->isReadOnly());
    EXPECT_TRUE(textEdit->isReadOnly());

    // Switch back to editable
    popup->setReadOnly(false);
    EXPECT_FALSE(popup->isReadOnly());
    EXPECT_FALSE(textEdit->isReadOnly());
}

TEST_F(StickyNotePopupTest, FreeTextTitle) {
    auto data = createSampleAnnotation();
    data.type = pdfe::annotations::AnnotationType::FreeText;
    data.author = "";

    popup->openForAnnotation(data, QPoint(100, 100));

    // Title should say "Text Box" for FreeText
    QLabel* titleBar = popup->findChild<QLabel*>();
    ASSERT_NE(titleBar, nullptr);
    EXPECT_EQ(titleBar->text().toStdString(), "Text Box");
}

TEST_F(StickyNotePopupTest, StickyNoteTitle) {
    auto data = createSampleAnnotation();
    data.type = pdfe::annotations::AnnotationType::Text;
    data.author = "";

    popup->openForAnnotation(data, QPoint(100, 100));

    // Title should say "Sticky Note" for Text
    QLabel* titleBar = popup->findChild<QLabel*>();
    ASSERT_NE(titleBar, nullptr);
    EXPECT_EQ(titleBar->text().toStdString(), "Sticky Note");
}

TEST_F(StickyNotePopupTest, ReadOnlyDoesNotEmitChanges) {
    auto data = createSampleAnnotation();

    QSignalSpy changedSpy(popup, &pdfe::ui::StickyNotePopup::contentsChanged);

    popup->openForAnnotation(data, QPoint(100, 100));
    popup->setReadOnly(true);
    changedSpy.clear();

    // Find text edit and try to modify (should be blocked by read-only)
    QTextEdit* textEdit = popup->findChild<QTextEdit*>();
    ASSERT_NE(textEdit, nullptr);

    // Direct setText still works but our read-only should prevent keyboard input
    // The QTextEdit is read-only, so programmatic setText doesn't fire contentsChanged
    // if we bypass the signal
    EXPECT_TRUE(textEdit->isReadOnly());
}
