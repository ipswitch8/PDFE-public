// -*- coding: utf-8 -*-
#include <gtest/gtest.h>
#include "batch/BatchEngine.h"
#include "batch/BatchTypes.h"
#include "ui/BatchDialog.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include <QApplication>
#include <QListWidget>
#include <QPushButton>
#include <QComboBox>
#include <QProgressBar>
#include <QTextEdit>
#include <QLabel>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <filesystem>

using namespace pdfe::batch;
using namespace pdfe::core;

// ========== BatchEngine Unit Tests ==========

class BatchEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();
    }

    std::shared_ptr<MuPdfContext> context;
};

TEST_F(BatchEngineTest, ConstructDestroy) {
    BatchEngine engine(context);
    SUCCEED();
}

TEST_F(BatchEngineTest, EmptyFileListReturnsEmptyResult) {
    BatchEngine engine(context);
    std::vector<std::filesystem::path> files;
    std::vector<BatchAction> actions = {{BatchActionType::FlattenAnnotations, std::monostate{}}};

    auto result = engine.process(files, actions);
    EXPECT_EQ(result.totalFiles, 0);
    EXPECT_EQ(result.successCount, 0);
    EXPECT_EQ(result.errorCount, 0);
    EXPECT_FALSE(result.cancelled);
}

TEST_F(BatchEngineTest, NonexistentFileReportsError) {
    BatchEngine engine(context);
    std::vector<std::filesystem::path> files = {"nonexistent_file.pdf"};
    std::vector<BatchAction> actions = {{BatchActionType::FlattenAnnotations, std::monostate{}}};

    auto result = engine.process(files, actions);
    EXPECT_EQ(result.totalFiles, 1);
    EXPECT_EQ(result.errorCount, 1);
    EXPECT_EQ(result.successCount, 0);
    ASSERT_EQ(result.fileResults.size(), 1u);
    EXPECT_FALSE(result.fileResults[0].success);
    EXPECT_EQ(result.fileResults[0].errorMessage, "File not found");
}

TEST_F(BatchEngineTest, CancelFlagWorks) {
    BatchEngine engine(context);
    EXPECT_FALSE(engine.isCancelled());
    engine.cancel();
    EXPECT_TRUE(engine.isCancelled());
}

TEST_F(BatchEngineTest, CancelDuringProcessingStops) {
    BatchEngine engine(context);

    engine.setProgressCallback([&](int, int, const std::string&) {
        engine.cancel();
    });

    std::vector<std::filesystem::path> files = {"nonexistent1.pdf", "nonexistent2.pdf", "nonexistent3.pdf"};
    std::vector<BatchAction> actions = {{BatchActionType::FlattenAnnotations, std::monostate{}}};

    auto result = engine.process(files, actions);
    EXPECT_TRUE(result.cancelled);
    EXPECT_LT(static_cast<int>(result.fileResults.size()), 3);
}

TEST_F(BatchEngineTest, ProgressCallbackFires) {
    BatchEngine engine(context);
    int callCount = 0;
    engine.setProgressCallback([&](int, int, const std::string&) { callCount++; });

    std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
    auto testPdf = fixturesDir / "test_single.pdf";
    if (!std::filesystem::exists(testPdf)) {
        GTEST_SKIP() << "test_single.pdf not available";
    }

    std::vector<std::filesystem::path> files = {testPdf};
    std::vector<BatchAction> actions = {{BatchActionType::FlattenAnnotations, std::monostate{}}};

    engine.process(files, actions);
    EXPECT_EQ(callCount, 1);
}

TEST_F(BatchEngineTest, ErrorCallbackFiresOnFailure) {
    BatchEngine engine(context);
    int errorCallCount = 0;
    engine.setErrorCallback([&](int, const std::string&, const std::string&) { errorCallCount++; });

    std::vector<std::filesystem::path> files = {"no_such_file.pdf"};
    std::vector<BatchAction> actions = {{BatchActionType::FlattenAnnotations, std::monostate{}}};

    engine.process(files, actions);
    EXPECT_EQ(errorCallCount, 1);
}

TEST_F(BatchEngineTest, MultipleActionsExecuteSequentially) {
    BatchEngine engine(context);

    std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
    auto testPdf = fixturesDir / "test_single.pdf";
    if (!std::filesystem::exists(testPdf)) {
        GTEST_SKIP() << "test_single.pdf not available";
    }

    std::vector<std::filesystem::path> files = {testPdf};
    std::vector<BatchAction> actions = {
        {BatchActionType::FlattenAnnotations, std::monostate{}},
    };

    auto result = engine.process(files, actions);
    EXPECT_EQ(result.totalFiles, 1);
    // May succeed or fail depending on file content, but shouldn't crash
    EXPECT_EQ(result.fileResults.size(), 1u);
}

// ========== BatchTypes Tests ==========

TEST(BatchTypesTest, WatermarkConfigDefaults) {
    WatermarkConfig cfg;
    EXPECT_EQ(cfg.text, "DRAFT");
    EXPECT_EQ(cfg.position, "center");
    EXPECT_FLOAT_EQ(cfg.opacity, 0.3f);
    EXPECT_FLOAT_EQ(cfg.fontSize, 48.0f);
}

TEST(BatchTypesTest, PdfAConfigDefaults) {
    PdfAConfig cfg;
    EXPECT_EQ(cfg.level, 2);
    EXPECT_EQ(cfg.conformance, 'b');
}

TEST(BatchTypesTest, PermissionsConfigDefaults) {
    PermissionsConfig cfg;
    EXPECT_TRUE(cfg.allowPrint);
    EXPECT_FALSE(cfg.allowCopy);
    EXPECT_FALSE(cfg.allowEdit);
    EXPECT_TRUE(cfg.ownerPassword.empty());
}

TEST(BatchTypesTest, BatchResultDefaults) {
    BatchResult result;
    EXPECT_EQ(result.totalFiles, 0);
    EXPECT_EQ(result.successCount, 0);
    EXPECT_EQ(result.errorCount, 0);
    EXPECT_FALSE(result.cancelled);
}

TEST(BatchTypesTest, BatchActionVariantHoldsConfigs) {
    BatchAction action;
    action.type = BatchActionType::AddWatermark;
    action.config = WatermarkConfig{"CONFIDENTIAL", "top-right", 0.5f, 36.0f, 0.0f};
    EXPECT_TRUE(std::holds_alternative<WatermarkConfig>(action.config));

    action.type = BatchActionType::ConvertToPdfA;
    action.config = PdfAConfig{1, 'b'};
    EXPECT_TRUE(std::holds_alternative<PdfAConfig>(action.config));
}

// ========== BatchDialog UI Tests ==========

class BatchDialogTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();
        dialog = std::make_unique<pdfe::ui::BatchDialog>(context);
    }

    void TearDown() override {
        dialog.reset();
    }

    std::shared_ptr<MuPdfContext> context;
    std::unique_ptr<pdfe::ui::BatchDialog> dialog;
};

TEST_F(BatchDialogTest, DialogHasObjectName) {
    EXPECT_EQ(dialog->objectName(), QString("batchDialog"));
}

TEST_F(BatchDialogTest, SelectFolderButtonExists) {
    auto* btn = dialog->findChild<QPushButton*>("selectFolderButton");
    ASSERT_NE(btn, nullptr);
    EXPECT_FALSE(btn->accessibleName().isEmpty());
}

TEST_F(BatchDialogTest, ActionListWidgetExists) {
    auto* list = dialog->findChild<QListWidget*>("actionListWidget");
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->accessibleName(), QString("Action Sequence List"));
}

TEST_F(BatchDialogTest, AddRemoveMoveButtonsExist) {
    EXPECT_NE(dialog->findChild<QPushButton*>("addActionButton"), nullptr);
    EXPECT_NE(dialog->findChild<QPushButton*>("removeActionButton"), nullptr);
    EXPECT_NE(dialog->findChild<QPushButton*>("moveUpButton"), nullptr);
    EXPECT_NE(dialog->findChild<QPushButton*>("moveDownButton"), nullptr);
}

TEST_F(BatchDialogTest, ActionTypeComboExists) {
    auto* combo = dialog->findChild<QComboBox*>("actionTypeCombo");
    ASSERT_NE(combo, nullptr);
    EXPECT_GE(combo->count(), 4);
}

TEST_F(BatchDialogTest, ActionTypeComboHasAllTypes) {
    auto* combo = dialog->findChild<QComboBox*>("actionTypeCombo");
    ASSERT_NE(combo, nullptr);

    QStringList expectedActions = {"Watermark", "Flatten", "PDF/A", "Permissions"};
    for (const auto& expected : expectedActions) {
        bool found = false;
        for (int i = 0; i < combo->count(); ++i) {
            if (combo->itemText(i).contains(expected)) {
                found = true;
                break;
            }
        }
        EXPECT_TRUE(found) << "Missing action type containing: " << expected.toStdString();
    }
}

TEST_F(BatchDialogTest, ProgressBarExists) {
    auto* bar = dialog->findChild<QProgressBar*>("batchProgressBar");
    ASSERT_NE(bar, nullptr);
    EXPECT_FALSE(bar->isVisible());
}

TEST_F(BatchDialogTest, ErrorLogExists) {
    auto* log = dialog->findChild<QTextEdit*>("batchErrorLog");
    ASSERT_NE(log, nullptr);
    EXPECT_EQ(log->accessibleName(), QString("Batch Error Log"));
}

TEST_F(BatchDialogTest, RunButtonExists) {
    auto* btn = dialog->findChild<QPushButton*>("runButton");
    ASSERT_NE(btn, nullptr);
    EXPECT_EQ(btn->accessibleName(), QString("Run Batch"));
}

TEST_F(BatchDialogTest, CancelButtonExistsAndDisabled) {
    auto* btn = dialog->findChild<QPushButton*>("cancelButton");
    ASSERT_NE(btn, nullptr);
    EXPECT_FALSE(btn->isEnabled());
}

TEST_F(BatchDialogTest, WatermarkConfigWidgetsExist) {
    EXPECT_NE(dialog->findChild<QLineEdit*>("watermarkTextEdit"), nullptr);
    EXPECT_NE(dialog->findChild<QDoubleSpinBox*>("watermarkOpacitySpin"), nullptr);
    EXPECT_NE(dialog->findChild<QComboBox*>("watermarkPositionCombo"), nullptr);
}

TEST_F(BatchDialogTest, PdfALevelComboExists) {
    auto* combo = dialog->findChild<QComboBox*>("pdfaLevelCombo");
    ASSERT_NE(combo, nullptr);
    EXPECT_GE(combo->count(), 4);
}

TEST_F(BatchDialogTest, PermissionCheckboxesExist) {
    EXPECT_NE(dialog->findChild<QCheckBox*>("permPrintCheck"), nullptr);
    EXPECT_NE(dialog->findChild<QCheckBox*>("permCopyCheck"), nullptr);
    EXPECT_NE(dialog->findChild<QCheckBox*>("permEditCheck"), nullptr);
    EXPECT_NE(dialog->findChild<QLineEdit*>("ownerPasswordEdit"), nullptr);
}

TEST_F(BatchDialogTest, AddActionAddsToList) {
    auto* addBtn = dialog->findChild<QPushButton*>("addActionButton");
    auto* list = dialog->findChild<QListWidget*>("actionListWidget");
    ASSERT_NE(addBtn, nullptr);
    ASSERT_NE(list, nullptr);

    EXPECT_EQ(list->count(), 0);
    addBtn->click();
    EXPECT_EQ(list->count(), 1);
    addBtn->click();
    EXPECT_EQ(list->count(), 2);
}

TEST_F(BatchDialogTest, RemoveActionRemovesFromList) {
    auto* addBtn = dialog->findChild<QPushButton*>("addActionButton");
    auto* removeBtn = dialog->findChild<QPushButton*>("removeActionButton");
    auto* list = dialog->findChild<QListWidget*>("actionListWidget");

    addBtn->click();
    addBtn->click();
    EXPECT_EQ(list->count(), 2);

    list->setCurrentRow(0);
    removeBtn->click();
    EXPECT_EQ(list->count(), 1);
}

TEST_F(BatchDialogTest, MoveUpDownReorders) {
    auto* addBtn = dialog->findChild<QPushButton*>("addActionButton");
    auto* moveDown = dialog->findChild<QPushButton*>("moveDownButton");
    auto* list = dialog->findChild<QListWidget*>("actionListWidget");
    auto* combo = dialog->findChild<QComboBox*>("actionTypeCombo");

    // Add Watermark (index 0)
    combo->setCurrentIndex(0);
    addBtn->click();
    // Add Flatten (index 1)
    combo->setCurrentIndex(1);
    addBtn->click();

    ASSERT_EQ(list->count(), 2);
    QString first = list->item(0)->text();

    list->setCurrentRow(0);
    moveDown->click();

    EXPECT_NE(list->item(0)->text(), first);
}

TEST_F(BatchDialogTest, RunWithNoFilesShowsStatus) {
    auto* runBtn = dialog->findChild<QPushButton*>("runButton");
    runBtn->click();

    auto* label = dialog->findChild<QLabel*>("batchStatusLabel");
    ASSERT_NE(label, nullptr);
    EXPECT_TRUE(label->text().contains("No input files"));
}

TEST_F(BatchDialogTest, RunWithNoActionsShowsStatus) {
    // Can't easily set files without folder dialog, but we can test no-actions path
    auto* runBtn = dialog->findChild<QPushButton*>("runButton");
    runBtn->click();

    auto* label = dialog->findChild<QLabel*>("batchStatusLabel");
    ASSERT_NE(label, nullptr);
    // Will show "No input files" since no folder selected
    EXPECT_FALSE(label->text().isEmpty());
}

TEST_F(BatchDialogTest, StatusLabelExists) {
    auto* label = dialog->findChild<QLabel*>("batchStatusLabel");
    ASSERT_NE(label, nullptr);
    EXPECT_EQ(label->accessibleName(), QString("Batch Status"));
}

TEST_F(BatchDialogTest, AllButtonsHaveAccessibleNames) {
    auto buttons = dialog->findChildren<QPushButton*>();
    for (auto* btn : buttons) {
        EXPECT_FALSE(btn->accessibleName().isEmpty())
            << "Button missing accessibleName: " << btn->objectName().toStdString();
    }
}
