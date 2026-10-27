#include <gtest/gtest.h>
#include "ui/MainWindow.h"
#include <QDockWidget>

using namespace pdfe::ui;

/// Test fixture for deferred panel creation.
/// These tests verify that panels exist after MainWindow construction,
/// regardless of whether they are created immediately or deferred.
class DeferredPanelsTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create MainWindow (panels should be created during construction)
        mainWindow = new MainWindow();
    }

    void TearDown() override {
        // Clean up MainWindow
        delete mainWindow;
        mainWindow = nullptr;
    }

    MainWindow* mainWindow = nullptr;
};

TEST_F(DeferredPanelsTest, MainWindow_HasPagePanel) {
    ASSERT_NE(mainWindow, nullptr);

    // Find the page panel dock widget by object name
    QDockWidget* pagePanelDock = mainWindow->findChild<QDockWidget*>("pagePanelDock");

    // Page panel dock should exist
    EXPECT_NE(pagePanelDock, nullptr) << "Page panel dock widget should be created";
}

TEST_F(DeferredPanelsTest, MainWindow_HasPropertiesPanel) {
    ASSERT_NE(mainWindow, nullptr);

    // Find the properties panel dock widget by object name
    QDockWidget* propertiesPanelDock = mainWindow->findChild<QDockWidget*>("propertiesPanelDock");

    // Properties panel dock should exist
    EXPECT_NE(propertiesPanelDock, nullptr) << "Properties panel dock widget should be created";
}

TEST_F(DeferredPanelsTest, MainWindow_HasSearchPanel) {
    ASSERT_NE(mainWindow, nullptr);

    // Find the search panel dock widget by object name
    QDockWidget* searchPanelDock = mainWindow->findChild<QDockWidget*>("searchPanelDock");

    // Search panel dock should exist
    EXPECT_NE(searchPanelDock, nullptr) << "Search panel dock widget should be created";
}

TEST_F(DeferredPanelsTest, MainWindow_HasBookmarkPanel) {
    ASSERT_NE(mainWindow, nullptr);

    // Find the bookmark panel dock widget by object name
    QDockWidget* bookmarkPanelDock = mainWindow->findChild<QDockWidget*>("bookmarkPanelDock");

    // Bookmark panel dock should exist
    EXPECT_NE(bookmarkPanelDock, nullptr) << "Bookmark panel dock widget should be created";
}
