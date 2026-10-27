// -*- coding: utf-8 -*-
// Pipeline G phase-11: lock the objectName contracts for the
// "planned" matrix rows in tests/ui_automation/matrix/ui_test_matrix.csv.
// pywinauto reads QObject::objectName() as the UIA AutomationId, so a
// C++ unit-test that asserts findChild<T>(objectName) returns non-null
// is a strong verification that the row's widget is reachable in the
// UIA tree by automation_id. This complements the runtime pywinauto
// suite (which requires an interactive Windows desktop).
//
// Rows covered:
//   AboutDialog:          aboutContent, licenseTextEdit
//                         (the per-label rows colorLogoLabel /
//                          broughtToYouByLabel / itwerxLogoLabel /
//                          donationLabel were collapsed into the
//                          single ``aboutContent`` QTextBrowser
//                          on 2026-05-06 when the dialog body
//                          became a Markdown-driven render so
//                          marketing can edit text/links/layout
//                          without code changes.)
//   EncryptionDialog:     passwordStrengthBar, passwordStrengthLabel
//   SignDialog:           tsaGroup, tsaCheckBox, tsaUrlEdit,
//                         advancedGroup, invisibleCheckBox,
//                         certificationLevelCombo
//   ComplianceDialog:     previewTagsButton, progressBar,
//                         complianceSplitter, tagTreeLabel,
//                         tagStructureTree
//   SignaturePanel:       ltvStatusLabel
#include <gtest/gtest.h>

#include "ui/AboutDialog.h"
#include "ui/EncryptionDialog.h"
#include "ui/SignDialog.h"
#include "ui/SignaturePanel.h"
#include "ui/ComplianceDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QTextBrowser>
#include <QTextEdit>
#include <QTreeView>

using namespace pdfe::ui;

// -------- AboutDialog --------

TEST(DialogObjectNames_AboutDialog, AboutContent)
{
    // Replaces the per-label tests (colorLogoLabel /
    // broughtToYouByLabel / itwerxLogoLabel / donationLabel). All
    // marketing content now lives in one QTextBrowser rendering
    // :/branding/about.md so marketing can edit without code changes.
    AboutDialog dlg;
    EXPECT_NE(dlg.findChild<QTextBrowser*>("aboutContent"), nullptr);
}

TEST(DialogObjectNames_AboutDialog, LicenseTextEdit)
{
    AboutDialog dlg;
    EXPECT_NE(dlg.findChild<QTextEdit*>("licenseTextEdit"), nullptr);
}

// -------- EncryptionDialog --------

TEST(DialogObjectNames_EncryptionDialog, PasswordStrengthBar)
{
    EncryptionDialog dlg;
    EXPECT_NE(dlg.findChild<QProgressBar*>("passwordStrengthBar"), nullptr);
}

TEST(DialogObjectNames_EncryptionDialog, PasswordStrengthLabel)
{
    EncryptionDialog dlg;
    EXPECT_NE(dlg.findChild<QLabel*>("passwordStrengthLabel"), nullptr);
}

// -------- SignDialog --------

TEST(DialogObjectNames_SignDialog, TsaGroup)
{
    SignDialog dlg;
    EXPECT_NE(dlg.findChild<QGroupBox*>("tsaGroup"), nullptr);
}

TEST(DialogObjectNames_SignDialog, TsaCheckBox)
{
    SignDialog dlg;
    EXPECT_NE(dlg.findChild<QCheckBox*>("tsaCheckBox"), nullptr);
}

TEST(DialogObjectNames_SignDialog, TsaUrlEdit)
{
    SignDialog dlg;
    EXPECT_NE(dlg.findChild<QLineEdit*>("tsaUrlEdit"), nullptr);
}

TEST(DialogObjectNames_SignDialog, AdvancedGroup)
{
    SignDialog dlg;
    EXPECT_NE(dlg.findChild<QGroupBox*>("advancedGroup"), nullptr);
}

TEST(DialogObjectNames_SignDialog, InvisibleCheckBox)
{
    SignDialog dlg;
    EXPECT_NE(dlg.findChild<QCheckBox*>("invisibleCheckBox"), nullptr);
}

TEST(DialogObjectNames_SignDialog, CertificationLevelCombo)
{
    SignDialog dlg;
    EXPECT_NE(dlg.findChild<QComboBox*>("certificationLevelCombo"), nullptr);
}

// -------- ComplianceDialog --------

TEST(DialogObjectNames_ComplianceDialog, PreviewTagsButton)
{
    ComplianceDialog dlg(nullptr, nullptr, nullptr);
    EXPECT_NE(dlg.findChild<QPushButton*>("previewTagsButton"), nullptr);
}

TEST(DialogObjectNames_ComplianceDialog, ProgressBar)
{
    ComplianceDialog dlg(nullptr, nullptr, nullptr);
    EXPECT_NE(dlg.findChild<QProgressBar*>("progressBar"), nullptr);
}

TEST(DialogObjectNames_ComplianceDialog, ComplianceSplitter)
{
    ComplianceDialog dlg(nullptr, nullptr, nullptr);
    EXPECT_NE(dlg.findChild<QSplitter*>("complianceSplitter"), nullptr);
}

TEST(DialogObjectNames_ComplianceDialog, TagTreeLabel)
{
    ComplianceDialog dlg(nullptr, nullptr, nullptr);
    EXPECT_NE(dlg.findChild<QLabel*>("tagTreeLabel"), nullptr);
}

TEST(DialogObjectNames_ComplianceDialog, TagStructureTree)
{
    ComplianceDialog dlg(nullptr, nullptr, nullptr);
    EXPECT_NE(dlg.findChild<QTreeView*>("tagStructureTree"), nullptr);
}

// -------- SignaturePanel --------

TEST(DialogObjectNames_SignaturePanel, LtvStatusLabel)
{
    SignaturePanel panel;
    EXPECT_NE(panel.findChild<QLabel*>("ltvStatusLabel"), nullptr);
}
