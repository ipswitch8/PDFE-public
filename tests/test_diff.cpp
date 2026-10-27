// -*- coding: utf-8 -*-
#include <gtest/gtest.h>
#include "diff/DiffEngine.h"
#include "diff/DiffTypes.h"
#include "ui/CompareDialog.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include <QApplication>
#include <QPushButton>
#include <QLabel>
#include <QScrollArea>
#include <QSpinBox>
#include <QTextEdit>
#include <QComboBox>
#include <QImage>
#include <filesystem>

using namespace pdfe::diff;
using namespace pdfe::core;

// ========== DiffEngine Unit Tests ==========

class DiffEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();
    }
    std::shared_ptr<MuPdfContext> context;
};

TEST_F(DiffEngineTest, ConstructDestroy) {
    DiffEngine engine(context);
    SUCCEED();
}

TEST_F(DiffEngineTest, IdenticalImagesHaveFullSimilarity) {
    QImage img(100, 100, QImage::Format_RGBA8888);
    img.fill(QColor(128, 128, 128));

    double sim = DiffEngine::computeSimilarity(img, img);
    EXPECT_NEAR(sim, 1.0, 0.001);
}

TEST_F(DiffEngineTest, DifferentImagesHaveLowSimilarity) {
    QImage white(100, 100, QImage::Format_RGBA8888);
    white.fill(Qt::white);
    QImage black(100, 100, QImage::Format_RGBA8888);
    black.fill(Qt::black);

    double sim = DiffEngine::computeSimilarity(white, black);
    EXPECT_LT(sim, 0.1);
}

TEST_F(DiffEngineTest, PixelDiffIdenticalProducesTransparent) {
    QImage img(50, 50, QImage::Format_RGBA8888);
    img.fill(QColor(100, 100, 100));

    QImage diff = DiffEngine::computePixelDiff(img, img, 255, 0, 0);
    EXPECT_FALSE(diff.isNull());

    bool allTransparent = true;
    for (int y = 0; y < diff.height(); ++y) {
        for (int x = 0; x < diff.width(); ++x) {
            if (qAlpha(diff.pixel(x, y)) > 0) {
                allTransparent = false;
                break;
            }
        }
        if (!allTransparent) break;
    }
    EXPECT_TRUE(allTransparent);
}

TEST_F(DiffEngineTest, PixelDiffDifferentProducesHighlight) {
    QImage white(50, 50, QImage::Format_RGBA8888);
    white.fill(Qt::white);
    QImage black(50, 50, QImage::Format_RGBA8888);
    black.fill(Qt::black);

    QImage diff = DiffEngine::computePixelDiff(white, black, 255, 0, 0);
    EXPECT_FALSE(diff.isNull());

    int coloredPixels = 0;
    for (int y = 0; y < diff.height(); ++y) {
        for (int x = 0; x < diff.width(); ++x) {
            if (qAlpha(diff.pixel(x, y)) > 0) coloredPixels++;
        }
    }
    EXPECT_GT(coloredPixels, 0);
}

TEST_F(DiffEngineTest, FindChangeRegionsOnCleanImage) {
    QImage clean(100, 100, QImage::Format_RGBA8888);
    clean.fill(Qt::transparent);

    auto regions = DiffEngine::findChangeRegions(clean);
    EXPECT_TRUE(regions.empty());
}

TEST_F(DiffEngineTest, FindChangeRegionsOnDirtyImage) {
    QImage dirty(100, 100, QImage::Format_RGBA8888);
    dirty.fill(Qt::transparent);
    dirty.setPixelColor(10, 10, QColor(255, 0, 0, 200));

    auto regions = DiffEngine::findChangeRegions(dirty);
    EXPECT_GT(regions.size(), 0u);
}

TEST_F(DiffEngineTest, NullImageSimilarityIsZero) {
    QImage img(50, 50, QImage::Format_RGBA8888);
    img.fill(Qt::white);
    QImage null;

    EXPECT_NEAR(DiffEngine::computeSimilarity(img, null), 0.0, 0.001);
    EXPECT_NEAR(DiffEngine::computeSimilarity(null, img), 0.0, 0.001);
}

// ========== Text Diff Tests ==========

TEST(TextDiffTest, IdenticalTextProducesNoChanges) {
    auto changes = DiffEngine::diffText("hello\nworld", "hello\nworld");
    for (const auto& c : changes) {
        EXPECT_EQ(c.type, ChangeType::Unchanged);
    }
}

TEST(TextDiffTest, InsertionDetected) {
    auto changes = DiffEngine::diffText("hello", "hello\nworld");
    bool hasInsertion = false;
    for (const auto& c : changes) {
        if (c.type == ChangeType::Inserted) {
            hasInsertion = true;
            EXPECT_EQ(c.text, "world");
        }
    }
    EXPECT_TRUE(hasInsertion);
}

TEST(TextDiffTest, DeletionDetected) {
    auto changes = DiffEngine::diffText("hello\nworld", "hello");
    bool hasDeletion = false;
    for (const auto& c : changes) {
        if (c.type == ChangeType::Deleted) {
            hasDeletion = true;
            EXPECT_EQ(c.text, "world");
        }
    }
    EXPECT_TRUE(hasDeletion);
}

TEST(TextDiffTest, EmptyToTextAllInsertions) {
    auto changes = DiffEngine::diffText("", "line1\nline2");
    int insertions = 0;
    for (const auto& c : changes) {
        if (c.type == ChangeType::Inserted) insertions++;
    }
    EXPECT_EQ(insertions, 2);
}

TEST(TextDiffTest, TextToEmptyAllDeletions) {
    auto changes = DiffEngine::diffText("line1\nline2", "");
    int deletions = 0;
    for (const auto& c : changes) {
        if (c.type == ChangeType::Deleted) deletions++;
    }
    EXPECT_EQ(deletions, 2);
}

TEST(TextDiffTest, BothEmptyNoChanges) {
    auto changes = DiffEngine::diffText("", "");
    EXPECT_TRUE(changes.empty());
}

// ========== DiffEngine Integration with Documents ==========

class DiffEngineDocTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();
        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        pdfPath = fixturesDir / "test_3page.pdf";
        if (!std::filesystem::exists(pdfPath)) {
            GTEST_SKIP() << "test_3page.pdf not available";
        }
    }
    std::shared_ptr<MuPdfContext> context;
    std::filesystem::path pdfPath;
};

TEST_F(DiffEngineDocTest, CompareIdenticalDocuments) {
    auto docA = Document::open(context, pdfPath);
    auto docB = Document::open(context, pdfPath);
    ASSERT_NE(docA, nullptr);
    ASSERT_NE(docB, nullptr);

    DiffEngine engine(context);
    auto result = engine.compare(docA.get(), docB.get());

    EXPECT_EQ(result.totalPages, docA->pageCount());
    EXPECT_NEAR(result.overallSimilarity, 1.0, 0.05);
    EXPECT_EQ(result.changedPages, 0);
}

TEST_F(DiffEngineDocTest, ComparePageProducesDiffImage) {
    auto docA = Document::open(context, pdfPath);
    auto docB = Document::open(context, pdfPath);

    DiffEngine engine(context);
    auto pageResult = engine.comparePage(docA.get(), docB.get(), 0);

    EXPECT_NEAR(pageResult.similarityScore, 1.0, 0.05);
    EXPECT_FALSE(pageResult.diffImage.isNull());
}

TEST_F(DiffEngineDocTest, CompareWithNullDocReturnsError) {
    auto docA = Document::open(context, pdfPath);

    DiffEngine engine(context);
    auto result = engine.compare(docA.get(), nullptr);
    EXPECT_FALSE(result.errorMessage.empty());
}

// ========== DiffTypes Tests ==========

TEST(DiffTypesTest, PageDiffResultDefaults) {
    PageDiffResult r;
    EXPECT_EQ(r.pageIndex, 0);
    EXPECT_NEAR(r.similarityScore, 1.0, 0.001);
    EXPECT_TRUE(r.diffImage.isNull());
    EXPECT_TRUE(r.changeRegions.empty());
    EXPECT_TRUE(r.textChanges.empty());
}

TEST(DiffTypesTest, DiffResultDefaults) {
    DiffResult r;
    EXPECT_EQ(r.totalPages, 0);
    EXPECT_EQ(r.changedPages, 0);
    EXPECT_TRUE(r.errorMessage.empty());
}

// ========== CompareDialog UI Tests ==========

class CompareDialogTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();
        dialog = std::make_unique<pdfe::ui::CompareDialog>(context);
    }
    void TearDown() override {
        dialog.reset();
    }
    std::shared_ptr<MuPdfContext> context;
    std::unique_ptr<pdfe::ui::CompareDialog> dialog;
};

TEST_F(CompareDialogTest, DialogHasObjectName) {
    EXPECT_EQ(dialog->objectName(), QString("compareDialog"));
}

TEST_F(CompareDialogTest, FileSelectionButtonsExist) {
    EXPECT_NE(dialog->findChild<QPushButton*>("selectFileAButton"), nullptr);
    EXPECT_NE(dialog->findChild<QPushButton*>("selectFileBButton"), nullptr);
}

TEST_F(CompareDialogTest, CompareButtonExistsAndDisabled) {
    auto* btn = dialog->findChild<QPushButton*>("compareButton");
    ASSERT_NE(btn, nullptr);
    EXPECT_FALSE(btn->isEnabled());
}

TEST_F(CompareDialogTest, SideBySidePanelsExist) {
    EXPECT_NE(dialog->findChild<QScrollArea*>("scrollAreaA"), nullptr);
    EXPECT_NE(dialog->findChild<QScrollArea*>("scrollAreaB"), nullptr);
}

TEST_F(CompareDialogTest, NavigationControlsExist) {
    EXPECT_NE(dialog->findChild<QSpinBox*>("pageSpinBox"), nullptr);
    EXPECT_NE(dialog->findChild<QPushButton*>("prevPageButton"), nullptr);
    EXPECT_NE(dialog->findChild<QPushButton*>("nextPageButton"), nullptr);
    EXPECT_NE(dialog->findChild<QLabel*>("pageCountLabel"), nullptr);
}

TEST_F(CompareDialogTest, SummaryLabelsExist) {
    EXPECT_NE(dialog->findChild<QLabel*>("similarityLabel"), nullptr);
    EXPECT_NE(dialog->findChild<QLabel*>("changeCountLabel"), nullptr);
}

TEST_F(CompareDialogTest, TextDiffViewExists) {
    auto* view = dialog->findChild<QTextEdit*>("textDiffView");
    ASSERT_NE(view, nullptr);
    EXPECT_EQ(view->accessibleName(), QString("Text Differences"));
    EXPECT_TRUE(view->isReadOnly());
}

TEST_F(CompareDialogTest, DiffColorComboExists) {
    auto* combo = dialog->findChild<QComboBox*>("diffColorCombo");
    ASSERT_NE(combo, nullptr);
    EXPECT_GE(combo->count(), 3);
}

TEST_F(CompareDialogTest, AllButtonsHaveAccessibleNames) {
    auto buttons = dialog->findChildren<QPushButton*>();
    for (auto* btn : buttons) {
        EXPECT_FALSE(btn->accessibleName().isEmpty())
            << "Missing accessibleName: " << btn->objectName().toStdString();
    }
}
