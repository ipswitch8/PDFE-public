#include <gtest/gtest.h>
#include "app/LogHandler.h"
#include <QDebug>
#include <QDir>
#include <QTemporaryDir>

using namespace pdfe::app;

class LogHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Get singleton instance
        handler_ = &LogHandler::instance();

        // Ensure we start with a clean state
        if (handler_->isFileLogging()) {
            handler_->setFileLogging(false);
        }

        // Set a known initial state
        handler_->setMinLevel(LogHandler::Warning);
    }

    void TearDown() override {
        // Always uninstall to restore default handler
        handler_->uninstall();

        // Disable file logging
        if (handler_->isFileLogging()) {
            handler_->setFileLogging(false);
        }

        // Reset to default level
        handler_->setMinLevel(LogHandler::Warning);
    }

    LogHandler* handler_ = nullptr;
};

TEST_F(LogHandlerTest, Instance_Singleton) {
    LogHandler& instance1 = LogHandler::instance();
    LogHandler& instance2 = LogHandler::instance();

    // Verify same address (singleton)
    EXPECT_EQ(&instance1, &instance2);
}

TEST_F(LogHandlerTest, Install_Uninstall) {
    // Verify install and uninstall do not throw
    EXPECT_NO_THROW({
        handler_->install();
    });

    EXPECT_NO_THROW({
        handler_->uninstall();
    });
}

TEST_F(LogHandlerTest, SetMinLevel_GetMinLevel) {
    handler_->setMinLevel(LogHandler::Info);

    LogHandler::Level level = handler_->minLevel();
    EXPECT_EQ(level, LogHandler::Info);
}

TEST_F(LogHandlerTest, MessageCapture_Warning) {
    // Install handler
    handler_->install();

    // Set level to Debug to capture all messages
    handler_->setMinLevel(LogHandler::Debug);

    // Log a warning message
    QString testMessage = "test warning message";
    qWarning() << testMessage;

    // Check last entry contains the message
    LogHandler::LogEntry entry = handler_->lastEntry();
    EXPECT_TRUE(entry.message.contains(testMessage));
    EXPECT_EQ(entry.type, QtWarningMsg);

    // Uninstall
    handler_->uninstall();
}

TEST_F(LogHandlerTest, MessageCapture_BelowLevel) {
    // Install handler
    handler_->install();

    // Set level to Critical (higher than Info)
    handler_->setMinLevel(LogHandler::Critical);

    // Log an info message (below the level)
    QString testMessage = "below level info message";
    qInfo() << testMessage;

    // Verify message is still captured in lastEntry
    // (it records all messages but only outputs those above the level)
    LogHandler::LogEntry entry = handler_->lastEntry();
    EXPECT_TRUE(entry.message.contains(testMessage));
    EXPECT_EQ(entry.type, QtInfoMsg);

    // Uninstall
    handler_->uninstall();
}

TEST_F(LogHandlerTest, FileLogging_Enable_Disable) {
    // Create temporary directory for log file
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    QString logPath = tempDir.path() + "/test.log";

    // Enable file logging
    handler_->setFileLogging(true, logPath);

    // Verify file logging is enabled
    EXPECT_TRUE(handler_->isFileLogging());
    EXPECT_EQ(handler_->logFilePath(), logPath);

    // Disable file logging
    handler_->setFileLogging(false);

    // Verify file logging is disabled
    EXPECT_FALSE(handler_->isFileLogging());
}
