// -*- coding: utf-8 -*-
// Phase 8c UI wiring: AltTextDialog exercise — construction, loading
// existing /Alt and /ActualText, saving through AnnotationManager.
#include <gtest/gtest.h>
#include "ui/AltTextDialog.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationType.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTextEdit>

#include <filesystem>
#include <memory>

using namespace pdfe::ui;
using namespace pdfe::annotations;
using namespace pdfe::core;

namespace {
std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

std::filesystem::path workCopy(const char* name)
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_alttext_ui";
    std::filesystem::create_directories(d);
    auto p = d / name;
    std::filesystem::copy_file(fixturesDir() / "test_single.pdf", p,
        std::filesystem::copy_options::overwrite_existing);
    return p;
}
} // namespace

class AltTextDialogTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx = std::make_shared<MuPdfContext>();
        if (!std::filesystem::exists(fixturesDir() / "test_single.pdf")) {
            GTEST_SKIP() << "test_single.pdf not available";
        }
        docPath = workCopy("alt_dialog_work.pdf");
        doc = Document::open(ctx, docPath);
        page = doc->page(0);
        mgr = std::make_unique<AnnotationManager>(ctx);
        // Fresh annotation to edit.
        auto a = mgr->createAnnotation(page.get(), AnnotationType::Square,
                                       QRectF(50, 50, 100, 100));
        annotIdx = a.annotIndex;
    }
    std::shared_ptr<MuPdfContext> ctx;
    std::filesystem::path docPath;
    std::unique_ptr<Document> doc;
    std::unique_ptr<Page> page;
    std::unique_ptr<AnnotationManager> mgr;
    int annotIdx = -1;
};

TEST_F(AltTextDialogTest, ExposesNamedChildrenForAutomation)
{
    AltTextDialog dlg(mgr.get(), page.get(), annotIdx);
    EXPECT_EQ(dlg.objectName(), QString("altTextDialog"));
    EXPECT_NE(dlg.findChild<QLineEdit*>("altTextAltEdit"), nullptr);
    EXPECT_NE(dlg.findChild<QTextEdit*>("altTextActualEdit"), nullptr);
    EXPECT_NE(dlg.findChild<QPushButton*>("altTextSaveButton"), nullptr);
    EXPECT_NE(dlg.findChild<QPushButton*>("altTextCancelButton"), nullptr);
}

// Pipeline G phase-6: lock the UIA accessibleName contract that
// pywinauto reads as the Name property. The dialog and its leaf
// widgets must have stable accessibleNames for assistive
// technologies and the automation matrix.
TEST_F(AltTextDialogTest, DialogHasAccessibleName)
{
    AltTextDialog dlg(mgr.get(), page.get(), annotIdx);
    EXPECT_EQ(dlg.accessibleName(), QString("Alt Text Dialog"));
}

TEST_F(AltTextDialogTest, AltEditHasAccessibleName)
{
    AltTextDialog dlg(mgr.get(), page.get(), annotIdx);
    auto* edit = dlg.findChild<QLineEdit*>("altTextAltEdit");
    ASSERT_NE(edit, nullptr);
    EXPECT_EQ(edit->accessibleName(), QString("Alternate Text"));
}

TEST_F(AltTextDialogTest, ActualEditHasAccessibleName)
{
    AltTextDialog dlg(mgr.get(), page.get(), annotIdx);
    auto* edit = dlg.findChild<QTextEdit*>("altTextActualEdit");
    ASSERT_NE(edit, nullptr);
    EXPECT_EQ(edit->accessibleName(), QString("Actual Text"));
}

TEST_F(AltTextDialogTest, SaveAndCancelButtonAccessibleNames)
{
    AltTextDialog dlg(mgr.get(), page.get(), annotIdx);
    auto* saveBtn = dlg.findChild<QPushButton*>("altTextSaveButton");
    ASSERT_NE(saveBtn, nullptr);
    EXPECT_EQ(saveBtn->accessibleName(), QString("Save Alt Text"));

    auto* cancelBtn = dlg.findChild<QPushButton*>("altTextCancelButton");
    ASSERT_NE(cancelBtn, nullptr);
    EXPECT_EQ(cancelBtn->accessibleName(), QString("Cancel"));
}

TEST_F(AltTextDialogTest, LoadsExistingAltAndActualTextOnOpen)
{
    mgr->setAltText(page.get(), annotIdx, "existing alt");
    mgr->setActualText(page.get(), annotIdx, "existing actual");

    AltTextDialog dlg(mgr.get(), page.get(), annotIdx);
    auto* altEdit = dlg.findChild<QLineEdit*>("altTextAltEdit");
    auto* actualEdit = dlg.findChild<QTextEdit*>("altTextActualEdit");
    ASSERT_NE(altEdit, nullptr);
    ASSERT_NE(actualEdit, nullptr);
    EXPECT_EQ(altEdit->text(), QString("existing alt"));
    EXPECT_EQ(actualEdit->toPlainText(), QString("existing actual"));
}

TEST_F(AltTextDialogTest, SaveWritesBackToAnnotation)
{
    AltTextDialog dlg(mgr.get(), page.get(), annotIdx);
    auto* altEdit = dlg.findChild<QLineEdit*>("altTextAltEdit");
    auto* actualEdit = dlg.findChild<QTextEdit*>("altTextActualEdit");
    auto* saveBtn = dlg.findChild<QPushButton*>("altTextSaveButton");
    ASSERT_NE(altEdit, nullptr);
    ASSERT_NE(actualEdit, nullptr);
    ASSERT_NE(saveBtn, nullptr);

    altEdit->setText("new alt via dialog");
    actualEdit->setPlainText("new actual via dialog");

    QSignalSpy spy(&dlg, &AltTextDialog::saved);
    saveBtn->click();
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toInt(), annotIdx);

    // Read back directly from the manager — confirms the write hit
    // the annotation's /Alt and /ActualText entries.
    EXPECT_EQ(mgr->getAltText(page.get(), annotIdx),
              std::string("new alt via dialog"));
    EXPECT_EQ(mgr->getActualText(page.get(), annotIdx),
              std::string("new actual via dialog"));
}

TEST_F(AltTextDialogTest, EmptyInputClearsAnnotationFields)
{
    mgr->setAltText(page.get(), annotIdx, "will be cleared");
    mgr->setActualText(page.get(), annotIdx, "also cleared");

    AltTextDialog dlg(mgr.get(), page.get(), annotIdx);
    dlg.findChild<QLineEdit*>("altTextAltEdit")->setText("");
    dlg.findChild<QTextEdit*>("altTextActualEdit")->setPlainText("");
    dlg.findChild<QPushButton*>("altTextSaveButton")->click();

    EXPECT_EQ(mgr->getAltText(page.get(), annotIdx), "");
    EXPECT_EQ(mgr->getActualText(page.get(), annotIdx), "");
}

TEST_F(AltTextDialogTest, CancelDoesNotWriteChanges)
{
    mgr->setAltText(page.get(), annotIdx, "stays");
    AltTextDialog dlg(mgr.get(), page.get(), annotIdx);
    dlg.findChild<QLineEdit*>("altTextAltEdit")->setText("about-to-be-canceled");
    dlg.findChild<QPushButton*>("altTextCancelButton")->click();

    EXPECT_EQ(mgr->getAltText(page.get(), annotIdx), std::string("stays"));
}
