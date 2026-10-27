// -*- coding: utf-8 -*-
// Preflight dialog — verifies the dialog exposes expected controls,
// consumes a ComplianceResult, and populates the issue tree by
// severity. Real PDF/A validation is already covered by
// test_pdfa_validator.cpp; here we test the UI wiring in isolation
// by feeding synthetic ComplianceResult instances.
#include <gtest/gtest.h>
#include "ui/PreflightDialog.h"
#include "compliance/ComplianceTypes.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>

using pdfe::compliance::ComplianceIssue;
using pdfe::compliance::ComplianceResult;
using pdfe::compliance::IssueSeverity;
using pdfe::compliance::PdfALevel;

namespace {
ComplianceResult makeSampleResult(int nErrors, int nWarnings)
{
    ComplianceResult r;
    r.targetLevel = PdfALevel::PdfA_2b;
    r.success = (nErrors == 0);
    for (int i = 0; i < nErrors; ++i) {
        ComplianceIssue issue;
        issue.severity = IssueSeverity::Error;
        issue.rule = "6.2.11.3";
        issue.description = "Font missing /ToUnicode mapping";
        issue.pageIndex = i;
        r.issues.push_back(issue);
    }
    for (int i = 0; i < nWarnings; ++i) {
        ComplianceIssue issue;
        issue.severity = IssueSeverity::Warning;
        issue.rule = "6.1.2";
        issue.description = "Non-standard metadata namespace";
        issue.pageIndex = -1;  // document-level
        r.issues.push_back(issue);
    }
    return r;
}
} // namespace

TEST(PreflightDialogTest, ExposesNamedControlsForAutomation)
{
    pdfe::ui::PreflightDialog dlg(nullptr, nullptr);
    EXPECT_EQ(dlg.objectName(), QString("preflightDialog"));
    EXPECT_NE(dlg.findChild<QComboBox*>("preflightProfileCombo"), nullptr);
    EXPECT_NE(dlg.findChild<QPushButton*>("preflightRunButton"), nullptr);
    EXPECT_NE(dlg.findChild<QPushButton*>("preflightExportButton"), nullptr);
    EXPECT_NE(dlg.findChild<QTreeWidget*>("preflightIssueTree"), nullptr);
    EXPECT_NE(dlg.findChild<QLabel*>("preflightSummaryLabel"), nullptr);
    EXPECT_NE(dlg.findChild<QDialogButtonBox*>("preflightCloseBox"), nullptr);
}

TEST(PreflightDialogTest, ProfileComboOffersAllSupportedTargets)
{
    pdfe::ui::PreflightDialog dlg(nullptr, nullptr);
    auto* combo = dlg.findChild<QComboBox*>("preflightProfileCombo");
    ASSERT_NE(combo, nullptr);
    // 5 PDF/A variants + PDF/UA.
    EXPECT_EQ(combo->count(), 6);
    // Spot check: 1b is the first entry (most permissive).
    EXPECT_EQ(combo->itemText(0), QString("PDF/A-1b"));
}

TEST(PreflightDialogTest, ExportButtonStartsDisabled)
{
    pdfe::ui::PreflightDialog dlg(nullptr, nullptr);
    auto* btn = dlg.findChild<QPushButton*>("preflightExportButton");
    ASSERT_NE(btn, nullptr);
    EXPECT_FALSE(btn->isEnabled());
}

TEST(PreflightDialogTest, SetResultPopulatesTreeGroupedBySeverity)
{
    pdfe::ui::PreflightDialog dlg(nullptr, nullptr);
    dlg.setResult(makeSampleResult(3, 2));

    auto* tree = dlg.findChild<QTreeWidget*>("preflightIssueTree");
    ASSERT_NE(tree, nullptr);
    ASSERT_EQ(tree->topLevelItemCount(), 2);

    QTreeWidgetItem* errors = tree->topLevelItem(0);
    QTreeWidgetItem* warnings = tree->topLevelItem(1);
    EXPECT_EQ(errors->childCount(), 3);
    EXPECT_EQ(warnings->childCount(), 2);
}

TEST(PreflightDialogTest, SummaryLabelReflectsIssueCounts)
{
    pdfe::ui::PreflightDialog dlg(nullptr, nullptr);
    dlg.setResult(makeSampleResult(4, 7));
    auto* label = dlg.findChild<QLabel*>("preflightSummaryLabel");
    ASSERT_NE(label, nullptr);
    EXPECT_TRUE(label->text().contains("4"));
    EXPECT_TRUE(label->text().contains("7"));
}

TEST(PreflightDialogTest, EmptyResultSaysNoIssues)
{
    pdfe::ui::PreflightDialog dlg(nullptr, nullptr);
    ComplianceResult empty;
    empty.success = true;
    dlg.setResult(empty);
    auto* label = dlg.findChild<QLabel*>("preflightSummaryLabel");
    ASSERT_NE(label, nullptr);
    EXPECT_FALSE(label->text().contains("error", Qt::CaseInsensitive))
        << label->text().toStdString();
}

TEST(PreflightDialogTest, ExportButtonEnabledAfterResultWithIssues)
{
    pdfe::ui::PreflightDialog dlg(nullptr, nullptr);
    dlg.setResult(makeSampleResult(1, 0));
    auto* btn = dlg.findChild<QPushButton*>("preflightExportButton");
    ASSERT_NE(btn, nullptr);
    EXPECT_TRUE(btn->isEnabled());
}

TEST(PreflightDialogTest, TreeItemsContainRuleAndDescription)
{
    pdfe::ui::PreflightDialog dlg(nullptr, nullptr);
    dlg.setResult(makeSampleResult(1, 0));
    auto* tree = dlg.findChild<QTreeWidget*>("preflightIssueTree");
    ASSERT_NE(tree, nullptr);
    ASSERT_GE(tree->topLevelItemCount(), 1);
    QTreeWidgetItem* errors = tree->topLevelItem(0);
    ASSERT_EQ(errors->childCount(), 1);
    QTreeWidgetItem* child = errors->child(0);
    // Columns: Severity, Rule, Page, Description.
    EXPECT_EQ(child->text(1), QString("6.2.11.3"));
    EXPECT_TRUE(child->text(3).contains("ToUnicode"));
}

TEST(PreflightDialogTest, DocumentLevelIssueShowsDashForPage)
{
    pdfe::ui::PreflightDialog dlg(nullptr, nullptr);
    dlg.setResult(makeSampleResult(0, 1));
    auto* tree = dlg.findChild<QTreeWidget*>("preflightIssueTree");
    auto* warnings = tree->topLevelItem(1);
    ASSERT_EQ(warnings->childCount(), 1);
    QTreeWidgetItem* child = warnings->child(0);
    // pageIndex == -1 renders as an em-dash.
    EXPECT_TRUE(child->text(2).contains("—") || child->text(2).isEmpty());
}

TEST(PreflightDialogTest, CurrentResultAccessorReturnsStoredValue)
{
    pdfe::ui::PreflightDialog dlg(nullptr, nullptr);
    auto r = makeSampleResult(2, 3);
    dlg.setResult(r);
    const auto& out = dlg.currentResult();
    EXPECT_EQ(out.errorCount(), 2);
    EXPECT_EQ(out.warningCount(), 3);
}
