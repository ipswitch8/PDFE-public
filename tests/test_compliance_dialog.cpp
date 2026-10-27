// -*- coding: utf-8 -*-
#include <gtest/gtest.h>
#include <QApplication>
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "ui/ComplianceDialog.h"
#include "compliance/StructureTypes.h"
#include "compliance/AutoTagger.h"
#include <filesystem>
#include <memory>

#include <QComboBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QProgressBar>
#include <QLabel>

class ComplianceDialogTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        auto pdfPath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
        if (!std::filesystem::exists(pdfPath)) {
            GTEST_SKIP() << "test_single.pdf not found";
        }
        document_ = pdfe::core::Document::open(context_, pdfPath);
        ASSERT_NE(document_, nullptr);
    }

    void TearDown() override {
        document_.reset();
        context_.reset();
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::core::Document> document_;
};

TEST_F(ComplianceDialogTest, ConstructDestroy) {
    pdfe::ui::ComplianceDialog dialog(context_, document_.get());
    SUCCEED();
}

TEST_F(ComplianceDialogTest, HasAccessibleNames) {
    pdfe::ui::ComplianceDialog dialog(context_, document_.get());

    // GitHub #185: the accessible name is what a screen reader SPEAKS, so it
    // is the window title, not the class name. objectName stays the
    // automation id — asserted below as the separate property it is.
    EXPECT_EQ(dialog.accessibleName(), dialog.windowTitle());
    EXPECT_EQ(dialog.accessibleName(), "PDF Standards Compliance");
    EXPECT_EQ(dialog.objectName(), "ComplianceDialog");

    auto* combo = dialog.findChild<QComboBox*>("LevelCombo");
    EXPECT_NE(combo, nullptr);
    EXPECT_EQ(combo->accessibleName(), "Target");

    auto* analyzeBtn = dialog.findChild<QPushButton*>("AnalyzeButton");
    EXPECT_NE(analyzeBtn, nullptr);

    auto* validateBtn = dialog.findChild<QPushButton*>("ValidateButton");
    EXPECT_NE(validateBtn, nullptr);

    auto* convertBtn = dialog.findChild<QPushButton*>("ConvertButton");
    EXPECT_NE(convertBtn, nullptr);

    auto* table = dialog.findChild<QTableWidget*>("IssueTable");
    EXPECT_NE(table, nullptr);

    auto* status = dialog.findChild<QLabel*>("StatusLabel");
    EXPECT_NE(status, nullptr);
}

TEST_F(ComplianceDialogTest, LevelComboHasOptions) {
    pdfe::ui::ComplianceDialog dialog(context_, document_.get());

    auto* combo = dialog.findChild<QComboBox*>("LevelCombo");
    ASSERT_NE(combo, nullptr);

    EXPECT_GE(combo->count(), 5) << "Should have at least 5 compliance levels";
}

TEST_F(ComplianceDialogTest, IssueTableHasColumns) {
    pdfe::ui::ComplianceDialog dialog(context_, document_.get());

    auto* table = dialog.findChild<QTableWidget*>("IssueTable");
    ASSERT_NE(table, nullptr);

    EXPECT_EQ(table->columnCount(), 4) << "Should have Severity, Rule, Description, Page columns";
}

TEST_F(ComplianceDialogTest, AnalyzeButtonTriggersAnalysis) {
    pdfe::ui::ComplianceDialog dialog(context_, document_.get());

    auto* analyzeBtn = dialog.findChild<QPushButton*>("AnalyzeButton");
    ASSERT_NE(analyzeBtn, nullptr);

    // Click analyze
    analyzeBtn->click();

    // Status should have been updated
    auto* status = dialog.findChild<QLabel*>("StatusLabel");
    ASSERT_NE(status, nullptr);
    EXPECT_FALSE(status->text().isEmpty()) << "Status should update after analyze";
}

TEST_F(ComplianceDialogTest, ValidateButtonTriggersValidation) {
    pdfe::ui::ComplianceDialog dialog(context_, document_.get());

    auto* validateBtn = dialog.findChild<QPushButton*>("ValidateButton");
    ASSERT_NE(validateBtn, nullptr);

    validateBtn->click();

    auto* status = dialog.findChild<QLabel*>("StatusLabel");
    ASSERT_NE(status, nullptr);
    EXPECT_FALSE(status->text().isEmpty()) << "Status should update after validate";

    // Issue table should have entries for a fresh (non-compliant) doc
    auto* table = dialog.findChild<QTableWidget*>("IssueTable");
    ASSERT_NE(table, nullptr);
    EXPECT_GT(table->rowCount(), 0) << "Fresh doc should have validation issues";
}

// ========== Tag Structure Tree Tests ==========

TEST_F(ComplianceDialogTest, TagStructureTreeExists) {
    pdfe::ui::ComplianceDialog dialog(context_, document_.get());
    auto* tree = dialog.findChild<QTreeWidget*>("tagStructureTree");
    ASSERT_NE(tree, nullptr);
    EXPECT_EQ(tree->accessibleName(), QString("Tag Structure Tree"));
}

TEST_F(ComplianceDialogTest, TagStructureTreeHasThreeColumns) {
    pdfe::ui::ComplianceDialog dialog(context_, document_.get());
    auto* tree = dialog.findChild<QTreeWidget*>("tagStructureTree");
    ASSERT_NE(tree, nullptr);
    EXPECT_EQ(tree->columnCount(), 3);
}

TEST_F(ComplianceDialogTest, PreviewTagsButtonExists) {
    pdfe::ui::ComplianceDialog dialog(context_, document_.get());
    auto* btn = dialog.findChild<QPushButton*>("previewTagsButton");
    ASSERT_NE(btn, nullptr);
    EXPECT_EQ(btn->accessibleName(), QString("Preview Tags"));
}

TEST_F(ComplianceDialogTest, ProgressBarExistsAndHidden) {
    pdfe::ui::ComplianceDialog dialog(context_, document_.get());
    auto* bar = dialog.findChild<QProgressBar*>("progressBar");
    ASSERT_NE(bar, nullptr);
    EXPECT_FALSE(bar->isVisible());
}

TEST_F(ComplianceDialogTest, PreviewTagsPopulatesTree) {
    pdfe::ui::ComplianceDialog dialog(context_, document_.get());
    auto* btn = dialog.findChild<QPushButton*>("previewTagsButton");
    ASSERT_NE(btn, nullptr);
    btn->click();

    auto* tree = dialog.findChild<QTreeWidget*>("tagStructureTree");
    ASSERT_NE(tree, nullptr);
    EXPECT_GT(tree->topLevelItemCount(), 0);

    auto* rootItem = tree->topLevelItem(0);
    ASSERT_NE(rootItem, nullptr);
    EXPECT_EQ(rootItem->text(0), QString("Document"));
    EXPECT_GT(rootItem->childCount(), 0);
}

TEST_F(ComplianceDialogTest, PreviewTagsUpdatesStatus) {
    pdfe::ui::ComplianceDialog dialog(context_, document_.get());
    auto* btn = dialog.findChild<QPushButton*>("previewTagsButton");
    btn->click();

    auto* label = dialog.findChild<QLabel*>("StatusLabel");
    ASSERT_NE(label, nullptr);
    EXPECT_TRUE(label->text().contains("Tag preview:"));
}

TEST_F(ComplianceDialogTest, TagTreeChildrenHaveTagTypes) {
    pdfe::ui::ComplianceDialog dialog(context_, document_.get());
    auto* btn = dialog.findChild<QPushButton*>("previewTagsButton");
    btn->click();

    auto* tree = dialog.findChild<QTreeWidget*>("tagStructureTree");
    auto* rootItem = tree->topLevelItem(0);
    ASSERT_NE(rootItem, nullptr);
    ASSERT_GT(rootItem->childCount(), 0);

    QString childTag = rootItem->child(0)->text(0);
    EXPECT_FALSE(childTag.isEmpty());
    QStringList knownTags = {"P", "H1", "H2", "H3", "H4", "H5", "H6",
                              "Figure", "Table", "Span", "Link", "Artifact",
                              "Sect", "Part", "L", "LI"};
    EXPECT_TRUE(knownTags.contains(childTag))
        << "Unknown tag: " << childTag.toStdString();
}

TEST_F(ComplianceDialogTest, TagTreeChildrenHavePageNumbers) {
    pdfe::ui::ComplianceDialog dialog(context_, document_.get());
    auto* btn = dialog.findChild<QPushButton*>("previewTagsButton");
    btn->click();

    auto* tree = dialog.findChild<QTreeWidget*>("tagStructureTree");
    auto* rootItem = tree->topLevelItem(0);
    ASSERT_NE(rootItem, nullptr);

    bool hasPageNumber = false;
    for (int i = 0; i < rootItem->childCount(); ++i) {
        if (!rootItem->child(i)->text(1).isEmpty()) {
            hasPageNumber = true;
            break;
        }
    }
    EXPECT_TRUE(hasPageNumber);
}

TEST_F(ComplianceDialogTest, LevelComboHasPdfUAOption) {
    pdfe::ui::ComplianceDialog dialog(context_, document_.get());
    auto* combo = dialog.findChild<QComboBox*>("LevelCombo");
    ASSERT_NE(combo, nullptr);

    bool hasPdfUA = false;
    for (int i = 0; i < combo->count(); ++i) {
        if (combo->itemText(i).contains("PDF/UA")) {
            hasPdfUA = true;
            break;
        }
    }
    EXPECT_TRUE(hasPdfUA);
}

// ========== AutoTagger Integration Tests ==========

TEST_F(ComplianceDialogTest, AutoTaggerReturnsDocumentRoot) {
    pdfe::compliance::AutoTagger tagger(context_);
    auto root = tagger.tagDocument(document_.get());
    EXPECT_EQ(root.tag, pdfe::compliance::StructureTag::Document);
}

TEST_F(ComplianceDialogTest, AutoTaggerProducesChildren) {
    pdfe::compliance::AutoTagger tagger(context_);
    auto root = tagger.tagDocument(document_.get());
    EXPECT_GT(root.children.size(), 0u);
}

TEST_F(ComplianceDialogTest, AutoTaggerChildrenHaveMcids) {
    pdfe::compliance::AutoTagger tagger(context_);
    auto root = tagger.tagDocument(document_.get());

    bool hasMcid = false;
    for (const auto& child : root.children) {
        if (child->mcid >= 0) { hasMcid = true; break; }
    }
    EXPECT_TRUE(hasMcid);
}

// ========== Structure Tag Name Tests ==========

TEST(StructureTagTest, AllTagsRoundTrip) {
    using pdfe::compliance::StructureTag;
    StructureTag tags[] = {
        StructureTag::Document, StructureTag::Part, StructureTag::Sect,
        StructureTag::H1, StructureTag::H2, StructureTag::H3,
        StructureTag::P, StructureTag::Span, StructureTag::Figure,
        StructureTag::Table, StructureTag::TR, StructureTag::TH,
        StructureTag::TD, StructureTag::L, StructureTag::LI,
        StructureTag::Link, StructureTag::Artifact
    };
    for (auto tag : tags) {
        auto name = pdfe::compliance::structureTagToName(tag);
        auto rt = pdfe::compliance::nameToStructureTag(name);
        EXPECT_EQ(rt, tag) << "Round-trip failed for " << name;
    }
}

TEST(StructureTagTest, UnknownNameDefaultsToP) {
    EXPECT_EQ(pdfe::compliance::nameToStructureTag("Unknown"), pdfe::compliance::StructureTag::P);
    EXPECT_EQ(pdfe::compliance::nameToStructureTag(nullptr), pdfe::compliance::StructureTag::P);
}

TEST(StructureNodeTest, DefaultValues) {
    pdfe::compliance::StructureNode node;
    EXPECT_EQ(node.tag, pdfe::compliance::StructureTag::P);
    EXPECT_EQ(node.pageIndex, -1);
    EXPECT_EQ(node.mcid, -1);
    EXPECT_TRUE(node.children.empty());
    EXPECT_TRUE(node.altText.empty());
}

// ========== State Transition Tests ==========

TEST_F(ComplianceDialogTest, AnalyzeTransitionsToAnalysisState) {
    pdfe::ui::ComplianceDialog dialog(context_, document_.get());

    auto* label = dialog.findChild<QLabel*>("StatusLabel");
    ASSERT_NE(label, nullptr);
    EXPECT_TRUE(label->text().isEmpty());

    auto* btn = dialog.findChild<QPushButton*>("AnalyzeButton");
    btn->click();

    EXPECT_TRUE(label->text().contains("Analysis:"));
    EXPECT_TRUE(label->text().contains("errors"));
    EXPECT_TRUE(label->text().contains("warnings"));
}

TEST_F(ComplianceDialogTest, ValidateTransitionsToPassOrFailState) {
    pdfe::ui::ComplianceDialog dialog(context_, document_.get());

    auto* btn = dialog.findChild<QPushButton*>("ValidateButton");
    btn->click();

    auto* label = dialog.findChild<QLabel*>("StatusLabel");
    bool isPassOrFail = label->text().contains("PASSED") || label->text().contains("FAILED");
    EXPECT_TRUE(isPassOrFail) << "Status: " << label->text().toStdString();
}

TEST_F(ComplianceDialogTest, ValidationPopulatesIssueTable) {
    pdfe::ui::ComplianceDialog dialog(context_, document_.get());

    auto* btn = dialog.findChild<QPushButton*>("ValidateButton");
    btn->click();

    auto* table = dialog.findChild<QTableWidget*>("IssueTable");
    ASSERT_NE(table, nullptr);
    EXPECT_GT(table->rowCount(), 0) << "Non-compliant doc should have issues";

    auto* sevItem = table->item(0, 0);
    ASSERT_NE(sevItem, nullptr);
    bool isErrorOrWarn = sevItem->text() == "ERROR" || sevItem->text() == "WARN";
    EXPECT_TRUE(isErrorOrWarn);
}

TEST_F(ComplianceDialogTest, ProgressBarHiddenAfterAnalyze) {
    pdfe::ui::ComplianceDialog dialog(context_, document_.get());

    auto* btn = dialog.findChild<QPushButton*>("AnalyzeButton");
    btn->click();

    auto* bar = dialog.findChild<QProgressBar*>("progressBar");
    EXPECT_FALSE(bar->isVisible());
}

TEST_F(ComplianceDialogTest, ButtonsEnabledAfterAnalyze) {
    pdfe::ui::ComplianceDialog dialog(context_, document_.get());

    auto* analyzeBtn = dialog.findChild<QPushButton*>("AnalyzeButton");
    analyzeBtn->click();

    EXPECT_TRUE(analyzeBtn->isEnabled());
    EXPECT_TRUE(dialog.findChild<QPushButton*>("ValidateButton")->isEnabled());
    EXPECT_TRUE(dialog.findChild<QPushButton*>("ConvertButton")->isEnabled());
    EXPECT_TRUE(dialog.findChild<QPushButton*>("previewTagsButton")->isEnabled());
}
