// -*- coding: utf-8 -*-
#include <gtest/gtest.h>
#include "layers/LayerManager.h"
#include "ui/LayerPanel.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/UndoManager.h"
#include <QApplication>
#include <QTreeWidget>
#include <QPushButton>
#include <QLabel>
#include <filesystem>
#include <memory>

using namespace pdfe::layers;
using namespace pdfe::core;
using namespace pdfe::commands;

// ========== LayerManager Unit Tests ==========

class LayerManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();
        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        pdfPath = fixturesDir / "test_3page.pdf";
        if (!std::filesystem::exists(pdfPath)) {
            GTEST_SKIP() << "test_3page.pdf not available";
        }
        document = Document::open(context, pdfPath);
    }
    std::shared_ptr<MuPdfContext> context;
    std::filesystem::path pdfPath;
    std::unique_ptr<Document> document;
};

TEST_F(LayerManagerTest, ConstructDestroy) {
    LayerManager mgr(context);
    SUCCEED();
}

TEST_F(LayerManagerTest, DocumentWithNoLayersReturnsZero) {
    LayerManager mgr(context);
    EXPECT_EQ(mgr.layerCount(document.get()), 0);
    EXPECT_FALSE(mgr.hasLayers(document.get()));
}

TEST_F(LayerManagerTest, ListLayersEmptyForStandardPdf) {
    LayerManager mgr(context);
    auto layers = mgr.listLayers(document.get());
    EXPECT_TRUE(layers.empty());
}

TEST_F(LayerManagerTest, NullDocumentSafe) {
    LayerManager mgr(context);
    EXPECT_EQ(mgr.layerCount(nullptr), 0);
    EXPECT_FALSE(mgr.hasLayers(nullptr));
    auto layers = mgr.listLayers(nullptr);
    EXPECT_TRUE(layers.empty());
    EXPECT_EQ(mgr.layerName(nullptr, 0), "");
    EXPECT_FALSE(mgr.isVisible(nullptr, 0));
}

TEST_F(LayerManagerTest, CreateLayerAddsToDocument) {
    LayerManager mgr(context);
    int initialCount = mgr.layerCount(document.get());

    int newIdx = mgr.createLayer(document.get(), "TestLayer");
    EXPECT_GE(newIdx, 0);

    int afterCount = mgr.layerCount(document.get());
    EXPECT_EQ(afterCount, initialCount + 1);
    EXPECT_TRUE(mgr.hasLayers(document.get()));
}

TEST_F(LayerManagerTest, CreateLayerWithNamePreservesName) {
    LayerManager mgr(context);
    int idx = mgr.createLayer(document.get(), "MyCustomLayer");
    ASSERT_GE(idx, 0);

    std::string name = mgr.layerName(document.get(), idx);
    EXPECT_EQ(name, "MyCustomLayer");
}

TEST_F(LayerManagerTest, CreateEmptyNameReturnsInvalid) {
    LayerManager mgr(context);
    int idx = mgr.createLayer(document.get(), "");
    EXPECT_EQ(idx, -1);
}

TEST_F(LayerManagerTest, SetVisibilityChangesState) {
    LayerManager mgr(context);
    int idx = mgr.createLayer(document.get(), "TestLayer");
    ASSERT_GE(idx, 0);

    mgr.setVisible(document.get(), idx, false);
    EXPECT_FALSE(mgr.isVisible(document.get(), idx));

    mgr.setVisible(document.get(), idx, true);
    EXPECT_TRUE(mgr.isVisible(document.get(), idx));
}

TEST_F(LayerManagerTest, RenameLayerChangesName) {
    LayerManager mgr(context);
    int idx = mgr.createLayer(document.get(), "Original");
    ASSERT_GE(idx, 0);

    mgr.renameLayer(document.get(), idx, "Renamed");
    EXPECT_EQ(mgr.layerName(document.get(), idx), "Renamed");
}

TEST_F(LayerManagerTest, DeleteLayerReducesCount) {
    LayerManager mgr(context);
    int idx = mgr.createLayer(document.get(), "ToDelete");
    ASSERT_GE(idx, 0);

    int beforeCount = mgr.layerCount(document.get());
    mgr.deleteLayer(document.get(), idx);
    int afterCount = mgr.layerCount(document.get());
    EXPECT_EQ(afterCount, beforeCount - 1);
}

TEST_F(LayerManagerTest, MultipleLayersHaveUniqueIndices) {
    LayerManager mgr(context);
    mgr.createLayer(document.get(), "Layer1");
    mgr.createLayer(document.get(), "Layer2");
    mgr.createLayer(document.get(), "Layer3");

    auto layers = mgr.listLayers(document.get());
    EXPECT_GE(layers.size(), 3u);

    std::vector<int> indices;
    for (const auto& l : layers) indices.push_back(l.index);
    std::sort(indices.begin(), indices.end());
    for (size_t i = 1; i < indices.size(); ++i) {
        EXPECT_NE(indices[i], indices[i - 1]);
    }
}

// ========== Layer Command Tests (Undo/Redo) ==========

TEST_F(LayerManagerTest, SetVisibilityCommandUndoable) {
    auto mgr = std::make_unique<LayerManager>(context);
    int idx = mgr->createLayer(document.get(), "UndoTest");
    ASSERT_GE(idx, 0);

    UndoManager undoMgr;
    mgr->setVisible(document.get(), idx, true);

    auto cmd = std::make_unique<SetLayerVisibilityCommand>(
        mgr.get(), document.get(), idx, false);
    undoMgr.execute(std::move(cmd));
    EXPECT_FALSE(mgr->isVisible(document.get(), idx));

    undoMgr.undo();
    EXPECT_TRUE(mgr->isVisible(document.get(), idx));
}

TEST_F(LayerManagerTest, CreateLayerCommandUndoable) {
    auto mgr = std::make_unique<LayerManager>(context);
    UndoManager undoMgr;

    int beforeCount = mgr->layerCount(document.get());

    auto cmd = std::make_unique<CreateLayerCommand>(
        mgr.get(), document.get(), "UndoableLayer");
    undoMgr.execute(std::move(cmd));

    EXPECT_EQ(mgr->layerCount(document.get()), beforeCount + 1);

    undoMgr.undo();
    EXPECT_EQ(mgr->layerCount(document.get()), beforeCount);
}

TEST_F(LayerManagerTest, RenameLayerCommandUndoable) {
    auto mgr = std::make_unique<LayerManager>(context);
    int idx = mgr->createLayer(document.get(), "OriginalName");
    ASSERT_GE(idx, 0);

    UndoManager undoMgr;
    auto cmd = std::make_unique<RenameLayerCommand>(
        mgr.get(), document.get(), idx, "NewName");
    undoMgr.execute(std::move(cmd));
    EXPECT_EQ(mgr->layerName(document.get(), idx), "NewName");

    undoMgr.undo();
    EXPECT_EQ(mgr->layerName(document.get(), idx), "OriginalName");
}

// ========== LayerPanel UI Tests ==========

class LayerPanelTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();
        panel = std::make_unique<pdfe::ui::LayerPanel>();
        panel->setContext(context);
    }
    void TearDown() override {
        panel.reset();
    }
    std::shared_ptr<MuPdfContext> context;
    std::unique_ptr<pdfe::ui::LayerPanel> panel;
};

TEST_F(LayerPanelTest, PanelHasObjectName) {
    EXPECT_EQ(panel->objectName(), QString("layerPanel"));
}

TEST_F(LayerPanelTest, PanelHasAccessibleName) {
    EXPECT_EQ(panel->accessibleName(), QString("Layer Panel"));
}

TEST_F(LayerPanelTest, TreeWidgetExists) {
    auto* tree = panel->findChild<QTreeWidget*>("layerTreeWidget");
    ASSERT_NE(tree, nullptr);
    EXPECT_EQ(tree->accessibleName(), QString("OCG Layer Tree"));
}

TEST_F(LayerPanelTest, ButtonsExist) {
    EXPECT_NE(panel->findChild<QPushButton*>("createLayerButton"), nullptr);
    EXPECT_NE(panel->findChild<QPushButton*>("deleteLayerButton"), nullptr);
    EXPECT_NE(panel->findChild<QPushButton*>("renameLayerButton"), nullptr);
}

TEST_F(LayerPanelTest, StatusLabelExists) {
    auto* label = panel->findChild<QLabel*>("layerStatusLabel");
    ASSERT_NE(label, nullptr);
    EXPECT_TRUE(label->text().contains("No document"));
}

TEST_F(LayerPanelTest, AllButtonsHaveAccessibleNames) {
    auto buttons = panel->findChildren<QPushButton*>();
    for (auto* btn : buttons) {
        EXPECT_FALSE(btn->accessibleName().isEmpty())
            << "Button missing accessibleName: " << btn->objectName().toStdString();
    }
}

// ========== LayerInfo Struct Tests ==========

TEST(LayerInfoTest, DefaultValues) {
    LayerInfo info;
    EXPECT_EQ(info.index, -1);
    EXPECT_TRUE(info.name.empty());
    EXPECT_TRUE(info.visible);
    EXPECT_FALSE(info.locked);
}
