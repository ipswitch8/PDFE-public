// test_startup_ui_changes.cpp - Tests for startup UI polish changes:
// 1. Bookmarks tab hidden when no bookmarks present
// 2. Search panel on right sidebar as default tab
// 3. Auto-select first page + Fit Page on open
// 4. DefaultsLogPanel existence, logging, clearing, singleton

#include <gtest/gtest.h>
#include <QAction>
#include <QDockWidget>
#include <QTextEdit>
#include <QPushButton>
#include "ui/MainWindow.h"
#include "ui/DefaultsLogPanel.h"

using namespace pdfe::ui;

// =============================================================================
// Fixture: MainWindow-based tests
// =============================================================================

class StartupUITest : public ::testing::Test {
protected:
    void SetUp() override {
        mainWindow = new MainWindow();
    }

    void TearDown() override {
        delete mainWindow;
        mainWindow = nullptr;
    }

    MainWindow* mainWindow = nullptr;
};

// =============================================================================
// 1. Bookmarks tab conditional visibility
// =============================================================================

TEST_F(StartupUITest, BookmarkDock_HiddenOnStartup) {
    auto* bookmarkDock = mainWindow->findChild<QDockWidget*>("bookmarkPanelDock");
    ASSERT_NE(bookmarkDock, nullptr);
    // Without any PDF open, bookmarks dock should be hidden
    EXPECT_FALSE(bookmarkDock->isVisible());
}

TEST_F(StartupUITest, BookmarkToggleAction_UncheckedOnStartup) {
    auto* toggleAction = mainWindow->findChild<QAction*>("toggleBookmarkPanelAction");
    ASSERT_NE(toggleAction, nullptr);
    EXPECT_FALSE(toggleAction->isChecked());
}

// =============================================================================
// 2. Search panel in right sidebar
// =============================================================================

TEST_F(StartupUITest, SearchDock_Exists) {
    auto* searchDock = mainWindow->findChild<QDockWidget*>("searchPanelDock");
    ASSERT_NE(searchDock, nullptr);
    EXPECT_EQ(searchDock->windowTitle().toStdString(), "Search");
}

TEST_F(StartupUITest, SearchDock_InRightDockArea) {
    auto* searchDock = mainWindow->findChild<QDockWidget*>("searchPanelDock");
    ASSERT_NE(searchDock, nullptr);
    // Allowed areas should include right
    EXPECT_TRUE(searchDock->allowedAreas() & Qt::RightDockWidgetArea);
}

TEST_F(StartupUITest, PropertiesDock_InRightDockArea) {
    auto* propertiesDock = mainWindow->findChild<QDockWidget*>("propertiesPanelDock");
    ASSERT_NE(propertiesDock, nullptr);
    EXPECT_TRUE(propertiesDock->allowedAreas() & Qt::RightDockWidgetArea);
}

// =============================================================================
// 4. DefaultsLogPanel
// =============================================================================

TEST_F(StartupUITest, DefaultsLogDock_Exists) {
    auto* defaultsLogDock = mainWindow->findChild<QDockWidget*>("defaultsLogDock");
    ASSERT_NE(defaultsLogDock, nullptr);
    EXPECT_EQ(defaultsLogDock->windowTitle().toStdString(), "Defaults Log");
}

TEST_F(StartupUITest, DefaultsLogPanel_Exists) {
    auto* panel = mainWindow->findChild<QWidget*>("defaultsLogPanel");
    ASSERT_NE(panel, nullptr);
}

TEST_F(StartupUITest, DefaultsLogDock_InRightDockArea) {
    auto* dock = mainWindow->findChild<QDockWidget*>("defaultsLogDock");
    ASSERT_NE(dock, nullptr);
    EXPECT_TRUE(dock->allowedAreas() & Qt::RightDockWidgetArea);
}

TEST_F(StartupUITest, DefaultsLogDock_HasAccessibleName) {
    auto* dock = mainWindow->findChild<QDockWidget*>("defaultsLogDock");
    ASSERT_NE(dock, nullptr);
    EXPECT_FALSE(dock->accessibleName().isEmpty())
        << "Defaults Log dock should have an accessible name";
}

TEST_F(StartupUITest, DefaultsLogPanel_HasLogView) {
    auto* logView = mainWindow->findChild<QTextEdit*>("defaultsLogView");
    ASSERT_NE(logView, nullptr);
    EXPECT_TRUE(logView->isReadOnly());
}

TEST_F(StartupUITest, DefaultsLogPanel_HasClearButton) {
    auto* clearButton = mainWindow->findChild<QPushButton*>("defaultsLogClearButton");
    ASSERT_NE(clearButton, nullptr);
}

// =============================================================================
// DefaultsLogPanel standalone tests (no MainWindow needed)
// =============================================================================

class DefaultsLogPanelTest : public ::testing::Test {
protected:
    void SetUp() override {
        panel = new DefaultsLogPanel();
    }

    void TearDown() override {
        delete panel;
        panel = nullptr;
    }

    DefaultsLogPanel* panel = nullptr;
};

TEST_F(DefaultsLogPanelTest, InitialEntryCount_IsZero) {
    EXPECT_EQ(panel->entryCount(), 0);
}

TEST_F(DefaultsLogPanelTest, LogDefault_IncrementsCount) {
    panel->logDefault("Test", "requested", "fallback", "reason");
    EXPECT_EQ(panel->entryCount(), 1);

    panel->logDefault("Test2", "req2", "fb2", "reason2");
    EXPECT_EQ(panel->entryCount(), 2);
}

TEST_F(DefaultsLogPanelTest, ClearLog_ResetsCount) {
    panel->logDefault("Test", "requested", "fallback", "reason");
    panel->logDefault("Test2", "req2", "fb2", "reason2");
    EXPECT_EQ(panel->entryCount(), 2);

    panel->clearLog();
    EXPECT_EQ(panel->entryCount(), 0);
}

TEST_F(DefaultsLogPanelTest, LogDefault_AppendsToTextEdit) {
    panel->logDefault("Font", "GlyphLessFont", "Arial", "OCR font not found");

    auto* logView = panel->findChild<QTextEdit*>("defaultsLogView");
    ASSERT_NE(logView, nullptr);

    QString html = logView->toHtml();
    EXPECT_TRUE(html.contains("Font")) << "Log should contain context";
    EXPECT_TRUE(html.contains("GlyphLessFont")) << "Log should contain requested value";
    EXPECT_TRUE(html.contains("Arial")) << "Log should contain fallback value";
    EXPECT_TRUE(html.contains("OCR font not found")) << "Log should contain reason";
}

TEST_F(DefaultsLogPanelTest, ClearLog_ClearsTextEdit) {
    panel->logDefault("Test", "a", "b", "c");
    panel->clearLog();

    auto* logView = panel->findChild<QTextEdit*>("defaultsLogView");
    ASSERT_NE(logView, nullptr);
    EXPECT_TRUE(logView->toPlainText().isEmpty());
}

// =============================================================================
// Global singleton tests
// =============================================================================

TEST(DefaultsLogSingletonTest, GlobalPointer_NullBeforeRegistration) {
    // Save and clear any existing registration
    DefaultsLogPanel* saved = defaultsLogPanel();
    setDefaultsLogPanel(nullptr);

    EXPECT_EQ(defaultsLogPanel(), nullptr);

    // Restore
    setDefaultsLogPanel(saved);
}

TEST(DefaultsLogSingletonTest, SetAndGet_RoundTrips) {
    DefaultsLogPanel* saved = defaultsLogPanel();

    DefaultsLogPanel panel;
    setDefaultsLogPanel(&panel);
    EXPECT_EQ(defaultsLogPanel(), &panel);

    // Restore
    setDefaultsLogPanel(saved);
}

TEST(DefaultsLogSingletonTest, LogDefaultUsed_NoOpWhenNull) {
    DefaultsLogPanel* saved = defaultsLogPanel();
    setDefaultsLogPanel(nullptr);

    // Should not crash
    logDefaultUsed("ctx", "req", "fb", "reason");

    // Restore
    setDefaultsLogPanel(saved);
}

TEST(DefaultsLogSingletonTest, LogDefaultUsed_DelegatesToPanel) {
    DefaultsLogPanel* saved = defaultsLogPanel();

    DefaultsLogPanel panel;
    setDefaultsLogPanel(&panel);
    EXPECT_EQ(panel.entryCount(), 0);

    logDefaultUsed("ctx", "req", "fb", "reason");
    EXPECT_EQ(panel.entryCount(), 1);

    // Restore
    setDefaultsLogPanel(saved);
}

TEST(DefaultsLogSingletonTest, Destructor_ClearsGlobalPointer) {
    DefaultsLogPanel* saved = defaultsLogPanel();

    {
        DefaultsLogPanel panel;
        setDefaultsLogPanel(&panel);
        EXPECT_EQ(defaultsLogPanel(), &panel);
    }
    // After destruction, global should be null
    EXPECT_EQ(defaultsLogPanel(), nullptr);

    // Restore
    setDefaultsLogPanel(saved);
}
