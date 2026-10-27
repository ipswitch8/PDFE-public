// -*- coding: utf-8 -*-
//
// Pipeline 3f1e9b2c phase-2 (NEW-2): widget audit tests for 7
// previously-untracked dialog/panel classes. Each class gets a
// construction test plus a key-widget objectName presence check.
//
// Cross-references existing coverage where applicable:
//   PreferencesDialog   — test_preferences_dialog.cpp
//   HeaderFooterDialog  — test_uncovered_dialogs.cpp
//   AltTextDialog       — test_alt_text_dialog.cpp
//   CalculationOrderDialog — test_calculation_order.cpp
//   PageLabelsDialog    — test_uncovered_dialogs.cpp
//   AttachmentPanel     — test_attachment_panel.cpp
//   PropertiesPanel     — test_uncovered_dialogs.cpp

#include <gtest/gtest.h>

#include "app/AppSettings.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationType.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "forms/FormManager.h"
#include "ui/AltTextDialog.h"
#include "ui/AttachmentPanel.h"
#include "ui/CalculationOrderDialog.h"
#include "ui/HeaderFooterDialog.h"
#include "ui/PageLabelsDialog.h"
#include "ui/PreferencesDialog.h"
#include "ui/PropertiesPanel.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QTreeWidget>

#include <filesystem>
#include <memory>

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

} // namespace

// =====================================================================
// 1. PreferencesDialog
// =====================================================================

TEST(DialogWidgetAudit, PreferencesDialog_ConstructAndWidgetAudit)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    pdfe::app::AppSettings settings;
    pdfe::ui::PreferencesDialog dlg(&settings, nullptr);

    EXPECT_EQ(dlg.objectName().toStdString(), "preferencesDialog");

    // Tab widget with all 5 tabs
    auto* tabs = dlg.findChild<QTabWidget*>("preferencesTabs");
    ASSERT_NE(tabs, nullptr);
    EXPECT_GE(tabs->count(), 5);

    EXPECT_NE(nullptr, dlg.findChild<QWidget*>("preferencesGeneralTab"));
    EXPECT_NE(nullptr, dlg.findChild<QWidget*>("preferencesViewTab"));
    EXPECT_NE(nullptr, dlg.findChild<QWidget*>("preferencesSecurityTab"));
    EXPECT_NE(nullptr, dlg.findChild<QWidget*>("preferencesOcrTab"));
    EXPECT_NE(nullptr, dlg.findChild<QWidget*>("preferencesAdvancedTab"));

    // Key controls
    EXPECT_NE(nullptr, dlg.findChild<QSpinBox*>("maxRecentFilesSpinBox"));
    EXPECT_NE(nullptr, dlg.findChild<QDoubleSpinBox*>("defaultZoomSpinBox"));
    EXPECT_NE(nullptr, dlg.findChild<QComboBox*>("ocrLanguageCombo"));
    EXPECT_NE(nullptr, dlg.findChild<QCheckBox*>("javaScriptEnabledCheck"));
    EXPECT_NE(nullptr, dlg.findChild<QDialogButtonBox*>("preferencesButtonBox"));
}

TEST(DialogWidgetAudit, PreferencesDialog_ClosesCleanly)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    {
        pdfe::app::AppSettings settings;
        pdfe::ui::PreferencesDialog dlg(&settings, nullptr);
        dlg.reject();
    }
    SUCCEED();
}

// =====================================================================
// 2. HeaderFooterDialog
// =====================================================================

TEST(DialogWidgetAudit, HeaderFooterDialog_ConstructAndWidgetAudit)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    auto pdfPath = fixturesDir() / "test_single.pdf";
    if (!std::filesystem::exists(pdfPath))
        GTEST_SKIP() << "test_single.pdf fixture unavailable";

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, pdfPath);
    ASSERT_NE(doc, nullptr);

    pdfe::ui::HeaderFooterDialog dlg(ctx, doc.get(), nullptr);

    EXPECT_EQ(dlg.objectName().toStdString(), "headerFooterDialog");

    EXPECT_NE(nullptr, dlg.findChild<QLineEdit*>("headerFooterLeftHeader"));
    EXPECT_NE(nullptr, dlg.findChild<QLineEdit*>("headerFooterCenterHeader"));
    EXPECT_NE(nullptr, dlg.findChild<QLineEdit*>("headerFooterRightHeader"));
    EXPECT_NE(nullptr, dlg.findChild<QLineEdit*>("headerFooterLeftFooter"));
    EXPECT_NE(nullptr, dlg.findChild<QLineEdit*>("headerFooterCenterFooter"));
    EXPECT_NE(nullptr, dlg.findChild<QLineEdit*>("headerFooterRightFooter"));
    EXPECT_NE(nullptr, dlg.findChild<QComboBox*>("headerFooterRangeCombo"));
    EXPECT_NE(nullptr, dlg.findChild<QDoubleSpinBox*>("headerFooterMarginSpin"));
    EXPECT_NE(nullptr, dlg.findChild<QDoubleSpinBox*>("headerFooterFontSizeSpin"));
    EXPECT_NE(nullptr, dlg.findChild<QPushButton*>("headerFooterApplyButton"));
    EXPECT_NE(nullptr, dlg.findChild<QPushButton*>("headerFooterCancelButton"));
}

TEST(DialogWidgetAudit, HeaderFooterDialog_ClosesCleanly)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    auto pdfPath = fixturesDir() / "test_single.pdf";
    if (!std::filesystem::exists(pdfPath))
        GTEST_SKIP() << "test_single.pdf fixture unavailable";

    {
        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        auto doc = pdfe::core::Document::open(ctx, pdfPath);
        pdfe::ui::HeaderFooterDialog dlg(ctx, doc.get(), nullptr);
        dlg.reject();
    }
    SUCCEED();
}

// =====================================================================
// 3. AltTextDialog
// =====================================================================

TEST(DialogWidgetAudit, AltTextDialog_ConstructAndWidgetAudit)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    auto pdfPath = fixturesDir() / "test_single.pdf";
    if (!std::filesystem::exists(pdfPath))
        GTEST_SKIP() << "test_single.pdf fixture unavailable";

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto tmpDir = std::filesystem::temp_directory_path() / "pdfe_audit_alttext";
    std::filesystem::create_directories(tmpDir);
    auto workPath = tmpDir / "audit_work.pdf";
    std::filesystem::copy_file(pdfPath, workPath,
        std::filesystem::copy_options::overwrite_existing);

    auto doc = pdfe::core::Document::open(ctx, workPath);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    pdfe::annotations::AnnotationManager mgr(ctx);
    auto a = mgr.createAnnotation(page.get(),
        pdfe::annotations::AnnotationType::Square, QRectF(10, 10, 50, 50));

    pdfe::ui::AltTextDialog dlg(&mgr, page.get(), a.annotIndex, nullptr);

    EXPECT_EQ(dlg.objectName().toStdString(), "altTextDialog");
    EXPECT_NE(nullptr, dlg.findChild<QLineEdit*>("altTextAltEdit"));
    EXPECT_NE(nullptr, dlg.findChild<QTextEdit*>("altTextActualEdit"));
    EXPECT_NE(nullptr, dlg.findChild<QPushButton*>("altTextSaveButton"));
    EXPECT_NE(nullptr, dlg.findChild<QPushButton*>("altTextCancelButton"));
    EXPECT_NE(nullptr, dlg.findChild<QLabel*>("altTextStatusLabel"));
}

TEST(DialogWidgetAudit, AltTextDialog_ClosesCleanly)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    auto pdfPath = fixturesDir() / "test_single.pdf";
    if (!std::filesystem::exists(pdfPath))
        GTEST_SKIP() << "test_single.pdf fixture unavailable";

    {
        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        auto tmpDir = std::filesystem::temp_directory_path() / "pdfe_audit_alttext2";
        std::filesystem::create_directories(tmpDir);
        auto workPath = tmpDir / "audit_close.pdf";
        std::filesystem::copy_file(pdfPath, workPath,
            std::filesystem::copy_options::overwrite_existing);

        auto doc = pdfe::core::Document::open(ctx, workPath);
        auto page = doc->page(0);
        pdfe::annotations::AnnotationManager mgr(ctx);
        auto a = mgr.createAnnotation(page.get(),
            pdfe::annotations::AnnotationType::Square, QRectF(10, 10, 50, 50));

        pdfe::ui::AltTextDialog dlg(&mgr, page.get(), a.annotIndex, nullptr);
        dlg.reject();
    }
    SUCCEED();
}

// =====================================================================
// 4. CalculationOrderDialog
// =====================================================================

TEST(DialogWidgetAudit, CalculationOrderDialog_ConstructAndWidgetAudit)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    auto pdfPath = fixturesDir() / "test_form.pdf";
    if (!std::filesystem::exists(pdfPath))
        GTEST_SKIP() << "test_form.pdf fixture unavailable";

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, pdfPath);
    ASSERT_NE(doc, nullptr);

    pdfe::forms::FormManager mgr(ctx);
    pdfe::ui::CalculationOrderDialog dlg(&mgr, doc.get(), nullptr);

    EXPECT_EQ(dlg.objectName().toStdString(), "calculationOrderDialog");
    EXPECT_NE(nullptr, dlg.findChild<QLabel*>("calculationOrderLabel"));
    EXPECT_NE(nullptr, dlg.findChild<QListWidget*>("calculationOrderFieldList"));
    EXPECT_NE(nullptr, dlg.findChild<QPushButton*>("calculationOrderMoveUpButton"));
    EXPECT_NE(nullptr, dlg.findChild<QPushButton*>("calculationOrderMoveDownButton"));
    EXPECT_NE(nullptr, dlg.findChild<QPushButton*>("calculationOrderApplyButton"));
    EXPECT_NE(nullptr, dlg.findChild<QPushButton*>("calculationOrderCancelButton"));
}

TEST(DialogWidgetAudit, CalculationOrderDialog_ClosesCleanly)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    auto pdfPath = fixturesDir() / "test_form.pdf";
    if (!std::filesystem::exists(pdfPath))
        GTEST_SKIP() << "test_form.pdf fixture unavailable";

    {
        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        auto doc = pdfe::core::Document::open(ctx, pdfPath);
        pdfe::forms::FormManager mgr(ctx);
        pdfe::ui::CalculationOrderDialog dlg(&mgr, doc.get(), nullptr);
        dlg.reject();
    }
    SUCCEED();
}

// =====================================================================
// 5. PageLabelsDialog
// =====================================================================

TEST(DialogWidgetAudit, PageLabelsDialog_ConstructAndWidgetAudit)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    auto pdfPath = fixturesDir() / "test_single.pdf";
    if (!std::filesystem::exists(pdfPath))
        GTEST_SKIP() << "test_single.pdf fixture unavailable";

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, pdfPath);
    ASSERT_NE(doc, nullptr);

    pdfe::ui::PageLabelsDialog dlg(doc.get(), nullptr);

    EXPECT_EQ(dlg.objectName().toStdString(), "pageLabelsDialog");
    EXPECT_NE(nullptr, dlg.findChild<QTableWidget*>("pageLabelsTable"));
    EXPECT_NE(nullptr, dlg.findChild<QPushButton*>("pageLabelsAddButton"));
    EXPECT_NE(nullptr, dlg.findChild<QPushButton*>("pageLabelsEditButton"));
    EXPECT_NE(nullptr, dlg.findChild<QPushButton*>("pageLabelsDeleteButton"));
    EXPECT_NE(nullptr, dlg.findChild<QDialogButtonBox*>("pageLabelsDialogButtonBox"));
}

TEST(DialogWidgetAudit, PageLabelsDialog_ClosesCleanly)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    auto pdfPath = fixturesDir() / "test_single.pdf";
    if (!std::filesystem::exists(pdfPath))
        GTEST_SKIP() << "test_single.pdf fixture unavailable";

    {
        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        auto doc = pdfe::core::Document::open(ctx, pdfPath);
        pdfe::ui::PageLabelsDialog dlg(doc.get(), nullptr);
        dlg.reject();
    }
    SUCCEED();
}

// =====================================================================
// 6. AttachmentPanel
// =====================================================================

TEST(DialogWidgetAudit, AttachmentPanel_ConstructAndWidgetAudit)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    pdfe::ui::AttachmentPanel panel(nullptr);

    EXPECT_EQ(panel.objectName().toStdString(), "attachmentPanel");
    EXPECT_NE(nullptr, panel.findChild<QTreeWidget*>("attachmentTree"));
    EXPECT_NE(nullptr, panel.findChild<QPushButton*>("attachmentAddButton"));
    EXPECT_NE(nullptr, panel.findChild<QPushButton*>("attachmentExtractButton"));
    EXPECT_NE(nullptr, panel.findChild<QPushButton*>("attachmentDeleteButton"));
    EXPECT_NE(nullptr, panel.findChild<QLabel*>("attachmentStatusLabel"));
}

TEST(DialogWidgetAudit, AttachmentPanel_ClosesCleanly)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    {
        pdfe::ui::AttachmentPanel panel(nullptr);
        panel.show();
        panel.hide();
    }
    SUCCEED();
}

// =====================================================================
// 7. PropertiesPanel
// =====================================================================

TEST(DialogWidgetAudit, PropertiesPanel_ConstructAndWidgetAudit)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    pdfe::ui::PropertiesPanel panel(nullptr);

    EXPECT_EQ(panel.objectName().toStdString(), "propertiesPanel");
    EXPECT_NE(nullptr, panel.findChild<QLabel*>("pageIndexLabel"));
    EXPECT_NE(nullptr, panel.findChild<QLabel*>("pageLabelLabel"));
    EXPECT_NE(nullptr, panel.findChild<QLabel*>("widthLabel"));
    EXPECT_NE(nullptr, panel.findChild<QLabel*>("heightLabel"));
    EXPECT_NE(nullptr, panel.findChild<QLabel*>("rotationLabel"));
    EXPECT_NE(nullptr, panel.findChild<QGroupBox*>("pagePropertiesGroup"));
}

TEST(DialogWidgetAudit, PropertiesPanel_ClosesCleanly)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    {
        pdfe::ui::PropertiesPanel panel(nullptr);
        panel.show();
        panel.hide();
    }
    SUCCEED();
}
