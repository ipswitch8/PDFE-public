// Phase-8 C3/C4: panel interaction tests + PrintDialog integration tests.
//
// C3: LayerPanel, DefaultsLogPanel, RedactionPanel — 2 interaction tests each.
// C4: PrintDialog — 3 integration tests (page setup, print range, options).

#include <gtest/gtest.h>

#include "ui/LayerPanel.h"
#include "ui/DefaultsLogPanel.h"
#include "ui/RedactionPanel.h"
#include "ui/PrintDialog.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/PrintEngine.h"
#include "redaction/RedactionData.h"

#include <QApplication>
#include <QTreeWidget>
#include <QPushButton>
#include <QLabel>
#include <QTextEdit>
#include <QRadioButton>
#include <QSpinBox>
#include <QCheckBox>
#include <QSignalSpy>
#include <filesystem>
#include <memory>
#include <vector>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

// ─────────────────────────────────────────────────────────────────────
// C3: LayerPanel interaction tests
// ─────────────────────────────────────────────────────────────────────

class LayerPanelInteractionTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!qApp) {
            int argc = 0;
            app_ = new QApplication(argc, nullptr);
        }
        panel_ = new pdfe::ui::LayerPanel();
    }

    void TearDown() override {
        delete panel_;
        panel_ = nullptr;
    }

    QApplication* app_ = nullptr;
    pdfe::ui::LayerPanel* panel_ = nullptr;
};

TEST_F(LayerPanelInteractionTest, WidgetObjectNamesAndInitialState) {
    EXPECT_EQ(panel_->objectName().toStdString(), "layerPanel");

    auto* tree = panel_->findChild<QTreeWidget*>("layerTreeWidget");
    ASSERT_NE(tree, nullptr) << "layerTreeWidget missing";
    EXPECT_EQ(tree->topLevelItemCount(), 0);

    auto* createBtn = panel_->findChild<QPushButton*>("createLayerButton");
    ASSERT_NE(createBtn, nullptr) << "createLayerButton missing";

    auto* deleteBtn = panel_->findChild<QPushButton*>("deleteLayerButton");
    ASSERT_NE(deleteBtn, nullptr) << "deleteLayerButton missing";

    auto* renameBtn = panel_->findChild<QPushButton*>("renameLayerButton");
    ASSERT_NE(renameBtn, nullptr) << "renameLayerButton missing";

    EXPECT_EQ(tree->currentItem(), nullptr)
        << "No item should be selected in empty tree";
}

TEST_F(LayerPanelInteractionTest, EmptyStateShowsStatusLabel) {
    auto* status = panel_->findChild<QLabel*>("layerStatusLabel");
    ASSERT_NE(status, nullptr) << "layerStatusLabel missing";
    EXPECT_FALSE(status->text().isEmpty())
        << "Status label should show a message when no document is loaded";

    auto* tree = panel_->findChild<QTreeWidget*>("layerTreeWidget");
    ASSERT_NE(tree, nullptr);
    EXPECT_EQ(tree->topLevelItemCount(), 0)
        << "Tree should have no items without a document";
}

// ─────────────────────────────────────────────────────────────────────
// C3: DefaultsLogPanel interaction tests
// ─────────────────────────────────────────────────────────────────────

class DefaultsLogPanelInteractionTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!qApp) {
            int argc = 0;
            app_ = new QApplication(argc, nullptr);
        }
        panel_ = new pdfe::ui::DefaultsLogPanel();
    }

    void TearDown() override {
        delete panel_;
        panel_ = nullptr;
    }

    QApplication* app_ = nullptr;
    pdfe::ui::DefaultsLogPanel* panel_ = nullptr;
};

TEST_F(DefaultsLogPanelInteractionTest, LogDefaultIncrementsEntryCount) {
    EXPECT_EQ(panel_->entryCount(), 0);

    panel_->logDefault("Overlay font", "GlyphLessFont", "Arial",
                       "Font not found on system");
    EXPECT_EQ(panel_->entryCount(), 1);

    panel_->logDefault("Script font", "Noto Sans CJK", "MS Gothic",
                       "CJK font not available");
    EXPECT_EQ(panel_->entryCount(), 2);

    auto* logView = panel_->findChild<QTextEdit*>("defaultsLogView");
    ASSERT_NE(logView, nullptr);
    EXPECT_FALSE(logView->toPlainText().isEmpty())
        << "Log view should contain text after logging entries";
}

TEST_F(DefaultsLogPanelInteractionTest, ClearLogResetsToEmpty) {
    panel_->logDefault("Test context", "requested", "fallback", "reason");
    panel_->logDefault("Test context 2", "requested2", "fallback2", "reason2");
    ASSERT_EQ(panel_->entryCount(), 2);

    panel_->clearLog();
    EXPECT_EQ(panel_->entryCount(), 0);

    auto* logView = panel_->findChild<QTextEdit*>("defaultsLogView");
    ASSERT_NE(logView, nullptr);
    EXPECT_TRUE(logView->toPlainText().isEmpty())
        << "Log view should be empty after clear";

    auto* clearBtn = panel_->findChild<QPushButton*>("defaultsLogClearButton");
    ASSERT_NE(clearBtn, nullptr) << "defaultsLogClearButton missing";
}

// ─────────────────────────────────────────────────────────────────────
// C3: RedactionPanel interaction tests
// ─────────────────────────────────────────────────────────────────────

class RedactionPanelInteractionTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!qApp) {
            int argc = 0;
            app_ = new QApplication(argc, nullptr);
        }
        panel_ = new pdfe::ui::RedactionPanel();
    }

    void TearDown() override {
        delete panel_;
        panel_ = nullptr;
    }

    QApplication* app_ = nullptr;
    pdfe::ui::RedactionPanel* panel_ = nullptr;
};

TEST_F(RedactionPanelInteractionTest, SetMarksPopulatesTree) {
    std::vector<pdfe::redaction::RedactionData> marks;

    pdfe::redaction::RedactionData m1;
    m1.rect = QRectF(10, 20, 200, 50);
    m1.pageIndex = 0;
    m1.annotIndex = 0;
    m1.fillColor = Qt::black;
    m1.enabled = true;
    marks.push_back(m1);

    pdfe::redaction::RedactionData m2;
    m2.rect = QRectF(50, 100, 300, 40);
    m2.pageIndex = 1;
    m2.annotIndex = 0;
    m2.fillColor = Qt::red;
    m2.overlayText = "REDACTED";
    m2.enabled = true;
    marks.push_back(m2);

    pdfe::redaction::RedactionData m3;
    m3.rect = QRectF(0, 0, 100, 100);
    m3.pageIndex = 0;
    m3.annotIndex = 1;
    m3.enabled = false;  // disabled — should not appear
    marks.push_back(m3);

    panel_->setMarks(marks);
    EXPECT_EQ(panel_->topLevelItemCount(), 2)
        << "Only enabled marks should appear in the tree";

    auto* applyBtn = panel_->findChild<QPushButton*>("applyRedactionsButton");
    ASSERT_NE(applyBtn, nullptr);
    EXPECT_TRUE(applyBtn->isEnabled())
        << "Apply button should be enabled when marks are present";

    auto* countLabel = panel_->findChild<QLabel*>("markCountLabel");
    ASSERT_NE(countLabel, nullptr);
    EXPECT_TRUE(countLabel->text().contains("2"))
        << "Count label should show '2' for two enabled marks";
}

TEST_F(RedactionPanelInteractionTest, ClearMarksResetsState) {
    pdfe::redaction::RedactionData m;
    m.rect = QRectF(10, 20, 200, 50);
    m.pageIndex = 0;
    m.annotIndex = 0;
    m.enabled = true;
    panel_->setMarks({m});
    ASSERT_EQ(panel_->topLevelItemCount(), 1);

    panel_->clear();
    EXPECT_EQ(panel_->topLevelItemCount(), 0);

    auto* applyBtn = panel_->findChild<QPushButton*>("applyRedactionsButton");
    ASSERT_NE(applyBtn, nullptr);
    EXPECT_FALSE(applyBtn->isEnabled())
        << "Apply button should be disabled when no marks";

    auto* deleteBtn = panel_->findChild<QPushButton*>("deleteRedactionButton");
    ASSERT_NE(deleteBtn, nullptr);
    EXPECT_FALSE(deleteBtn->isEnabled())
        << "Delete button should be disabled when no marks";

    auto* countLabel = panel_->findChild<QLabel*>("markCountLabel");
    ASSERT_NE(countLabel, nullptr);
    EXPECT_TRUE(countLabel->text().contains("No"))
        << "Count label should show 'No redaction marks' when empty";
}

// ─────────────────────────────────────────────────────────────────────
// C4: PrintDialog integration tests
// ─────────────────────────────────────────────────────────────────────

class PrintDialogIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!qApp) {
            int argc = 0;
            app_ = new QApplication(argc, nullptr);
        }

        std::filesystem::path fixtures = TEST_FIXTURES_DIR;
        auto pdfPath = fixtures / "test_3page.pdf";
        if (!std::filesystem::exists(pdfPath))
            GTEST_SKIP() << "test_3page.pdf fixture unavailable";

        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        doc_ = pdfe::core::Document::open(context_, pdfPath);
        ASSERT_NE(doc_, nullptr);
    }

    void TearDown() override {
        delete dialog_;
        dialog_ = nullptr;
        doc_.reset();
    }

    QApplication* app_ = nullptr;
    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::core::Document> doc_;
    pdfe::ui::PrintDialog* dialog_ = nullptr;
};

TEST_F(PrintDialogIntegrationTest, DefaultPageSetup) {
    dialog_ = new pdfe::ui::PrintDialog(doc_.get(), 0, nullptr);

    auto* allPages = dialog_->findChild<QRadioButton*>("allPagesRadio");
    ASSERT_NE(allPages, nullptr);
    EXPECT_TRUE(allPages->isChecked()) << "AllPages should be default";

    auto* fromSpin = dialog_->findChild<QSpinBox*>("fromPageSpinBox");
    auto* toSpin = dialog_->findChild<QSpinBox*>("toPageSpinBox");
    ASSERT_NE(fromSpin, nullptr);
    ASSERT_NE(toSpin, nullptr);
    EXPECT_FALSE(fromSpin->isEnabled())
        << "From spin should be disabled when AllPages is selected";
    EXPECT_FALSE(toSpin->isEnabled())
        << "To spin should be disabled when AllPages is selected";

    auto opts = dialog_->options();
    EXPECT_EQ(opts.range, pdfe::core::PrintRange::AllPages);
    EXPECT_EQ(opts.fromPage, 1);
    EXPECT_EQ(opts.toPage, doc_->pageCount());
    EXPECT_EQ(opts.copies, 1);
}

TEST_F(PrintDialogIntegrationTest, PageRangeSelectionEnablesSpins) {
    dialog_ = new pdfe::ui::PrintDialog(doc_.get(), 1, nullptr);

    auto* rangeRadio = dialog_->findChild<QRadioButton*>("pageRangeRadio");
    ASSERT_NE(rangeRadio, nullptr);
    rangeRadio->setChecked(true);

    auto* fromSpin = dialog_->findChild<QSpinBox*>("fromPageSpinBox");
    auto* toSpin = dialog_->findChild<QSpinBox*>("toPageSpinBox");
    ASSERT_NE(fromSpin, nullptr);
    ASSERT_NE(toSpin, nullptr);
    EXPECT_TRUE(fromSpin->isEnabled())
        << "From spin should be enabled when PageRange is selected";
    EXPECT_TRUE(toSpin->isEnabled())
        << "To spin should be enabled when PageRange is selected";

    fromSpin->setValue(2);
    toSpin->setValue(3);

    auto opts = dialog_->options();
    EXPECT_EQ(opts.range, pdfe::core::PrintRange::PageRange);
    EXPECT_EQ(opts.fromPage, 2);
    EXPECT_EQ(opts.toPage, 3);
}

TEST_F(PrintDialogIntegrationTest, PrintOptionsToggle) {
    dialog_ = new pdfe::ui::PrintDialog(doc_.get(), 0, nullptr);

    auto* fitCheck = dialog_->findChild<QCheckBox*>("fitToPageCheckBox");
    auto* centerCheck = dialog_->findChild<QCheckBox*>("centerOnPageCheckBox");
    auto* rotateCheck = dialog_->findChild<QCheckBox*>("autoRotateCheckBox");
    ASSERT_NE(fitCheck, nullptr);
    ASSERT_NE(centerCheck, nullptr);
    ASSERT_NE(rotateCheck, nullptr);

    EXPECT_TRUE(fitCheck->isChecked()) << "Fit to page should default to true";
    EXPECT_TRUE(centerCheck->isChecked()) << "Center should default to true";
    EXPECT_TRUE(rotateCheck->isChecked()) << "Auto-rotate should default to true";

    fitCheck->setChecked(false);
    centerCheck->setChecked(false);
    rotateCheck->setChecked(false);

    auto* copiesSpin = dialog_->findChild<QSpinBox*>("copiesSpinBox");
    ASSERT_NE(copiesSpin, nullptr);
    copiesSpin->setValue(3);

    auto opts = dialog_->options();
    EXPECT_FALSE(opts.fitToPage);
    EXPECT_FALSE(opts.centerOnPage);
    EXPECT_FALSE(opts.autoRotate);
    EXPECT_EQ(opts.copies, 3);
}
