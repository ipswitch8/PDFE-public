// test_recent_files.cpp - Unit tests for RecentFilesMenu
#include <gtest/gtest.h>
#include "ui/RecentFilesMenu.h"
#include "app/AppSettings.h"
#include <QApplication>
#include <QAction>
#include <QSignalSpy>
#include <QSettings>
#include <memory>
#include <optional>

#include "support/SettingsIsolation.h"

using namespace pdfe::ui;
using namespace pdfe::app;

// F33: the clears below used to name HKCU\Software\PDFE\PDFE, the SHIPPED
// application's hive — so this fixture deleted the developer's real recent-file
// list on every run. F28 measured that and carried it. A SettingsScope now
// redirects the identity into a purged PDFE-Test hive; AppSettings follows it
// because it default-constructs its QSettings.
class RecentFilesMenuTest : public ::testing::Test {
protected:
    void SetUp() override {
        scope_.emplace(QStringLiteral("PDFE-Test"),
                       QStringLiteral("RecentFilesMenuTest"),
                       /*purgeOnExit=*/true);
        // Clear settings before each test to ensure isolation
        QSettings().clear();
        QSettings().sync();

        // Create fresh AppSettings instance
        settings_ = std::make_unique<AppSettings>();
    }

    void TearDown() override {
        // Clean up menu before settings
        menu_.reset();
        settings_.reset();

        // Clear settings after each test
        QSettings().clear();
        QSettings().sync();
        scope_.reset();
    }

    void createMenu(const QString& title = "Recent Files") {
        menu_ = std::make_unique<RecentFilesMenu>(title, settings_.get());
    }

    std::optional<pdfe_test::SettingsScope> scope_;
    std::unique_ptr<AppSettings> settings_;
    std::unique_ptr<RecentFilesMenu> menu_;
};

TEST_F(RecentFilesMenuTest, Construct_Empty) {
    // Arrange - settings already created with no recent files

    // Act
    createMenu();

    // Assert
    ASSERT_NE(menu_, nullptr);
    EXPECT_EQ(menu_->objectName(), QString("recentFilesMenu"));
    EXPECT_FALSE(menu_->isEnabled()) << "Menu should be disabled when no recent files exist";

    // Verify menu has the "(No Recent Files)" action
    QList<QAction*> actions = menu_->actions();
    ASSERT_GE(actions.size(), 1) << "Menu should have at least one action";
    EXPECT_FALSE(actions[0]->isEnabled()) << "The '(No Recent Files)' action should be disabled";
}

TEST_F(RecentFilesMenuTest, AddFile_AppearsInMenu) {
    // Arrange
    createMenu();
    const QString testFilePath = "C:/test.pdf";

    // Act
    menu_->addFile(testFilePath);

    // Assert
    QList<QAction*> actions = menu_->actions();
    ASSERT_GE(actions.size(), 1) << "Menu should have at least one action after adding file";

    // Find the action with the test file path
    bool foundFile = false;
    for (QAction* action : actions) {
        if (action->data().toString() == testFilePath) {
            foundFile = true;
            EXPECT_TRUE(action->isEnabled()) << "File action should be enabled";
            break;
        }
    }

    EXPECT_TRUE(foundFile) << "Menu should contain action with data 'C:/test.pdf'";
    EXPECT_TRUE(menu_->isEnabled()) << "Menu should be enabled after adding a file";
}

TEST_F(RecentFilesMenuTest, AddFile_Ordering) {
    // Arrange
    createMenu();
    const QString firstFile = "a.pdf";
    const QString secondFile = "b.pdf";

    // Act - add files in order
    menu_->addFile(firstFile);
    menu_->addFile(secondFile);

    // Assert - most recent file (b.pdf) should be first
    QList<QAction*> actions = menu_->actions();
    ASSERT_GE(actions.size(), 2) << "Menu should have at least two file actions";

    // First action should be the most recently added file
    EXPECT_EQ(actions[0]->data().toString(), secondFile)
        << "Most recently added file should appear first in menu";

    // Find the second file action (skip separator if present)
    bool foundFirstFile = false;
    for (int i = 1; i < actions.size(); ++i) {
        if (actions[i]->data().toString() == firstFile) {
            foundFirstFile = true;
            break;
        }
    }

    EXPECT_TRUE(foundFirstFile) << "First file should still be in the menu";
}

TEST_F(RecentFilesMenuTest, FileTriggered_Signal) {
    // Arrange
    createMenu();
    const QString testFilePath = "C:/documents/test.pdf";
    menu_->addFile(testFilePath);

    // Setup signal spy
    QSignalSpy spy(menu_.get(), &RecentFilesMenu::fileTriggered);
    ASSERT_TRUE(spy.isValid()) << "Signal spy should be valid";

    // Find the action for our test file
    QAction* fileAction = nullptr;
    QList<QAction*> actions = menu_->actions();
    for (QAction* action : actions) {
        if (action->data().toString() == testFilePath) {
            fileAction = action;
            break;
        }
    }

    ASSERT_NE(fileAction, nullptr) << "File action should exist in menu";

    // Act - trigger the action
    fileAction->trigger();

    // Assert - signal should be emitted with correct path
    ASSERT_EQ(spy.count(), 1) << "fileTriggered signal should be emitted exactly once";
    QList<QVariant> arguments = spy.takeFirst();
    ASSERT_EQ(arguments.size(), 1) << "Signal should have one argument";
    EXPECT_EQ(arguments.at(0).toString(), testFilePath)
        << "Signal should emit the correct file path";
}

TEST_F(RecentFilesMenuTest, ClearAction_ClearsFiles) {
    // Arrange
    createMenu();
    menu_->addFile("C:/file1.pdf");
    menu_->addFile("C:/file2.pdf");
    menu_->addFile("C:/file3.pdf");

    // Verify files were added
    ASSERT_FALSE(settings_->recentFiles().isEmpty())
        << "Settings should have recent files before clearing";

    // Find the "Clear Recent Files" action
    QAction* clearAction = nullptr;
    QList<QAction*> actions = menu_->actions();
    for (QAction* action : actions) {
        if (action->text().contains("Clear", Qt::CaseInsensitive)) {
            clearAction = action;
            break;
        }
    }

    ASSERT_NE(clearAction, nullptr) << "Clear Recent Files action should exist";

    // Act - trigger the clear action
    clearAction->trigger();

    // Assert - recent files should be cleared
    EXPECT_TRUE(settings_->recentFiles().isEmpty())
        << "Settings recent files should be empty after clearing";
    EXPECT_FALSE(menu_->isEnabled())
        << "Menu should be disabled after clearing all files";
}

TEST_F(RecentFilesMenuTest, Refresh_RebuildFromSettings) {
    // Arrange
    createMenu();
    const QString testFilePath = "C:/direct_add.pdf";

    // Directly add file to settings (bypassing menu)
    settings_->addRecentFile(testFilePath);

    // Verify menu doesn't have the file yet
    QList<QAction*> actionsBefore = menu_->actions();
    bool foundBeforeRefresh = false;
    for (QAction* action : actionsBefore) {
        if (action->data().toString() == testFilePath) {
            foundBeforeRefresh = true;
            break;
        }
    }
    EXPECT_FALSE(foundBeforeRefresh)
        << "Menu should not have the file before refresh is called";

    // Act - refresh the menu
    menu_->refresh();

    // Assert - menu should now contain the file
    QList<QAction*> actionsAfter = menu_->actions();
    bool foundAfterRefresh = false;
    for (QAction* action : actionsAfter) {
        if (action->data().toString() == testFilePath) {
            foundAfterRefresh = true;
            break;
        }
    }

    EXPECT_TRUE(foundAfterRefresh)
        << "Menu should contain the file after refresh";
    EXPECT_TRUE(menu_->isEnabled())
        << "Menu should be enabled after refresh when files exist";
}
