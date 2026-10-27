#include <gtest/gtest.h>
#include "app/StartupTimer.h"
#include <QThread>

using namespace pdfe::app;

class StartupTimerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // StartupTimer is simple and doesn't need complex setup
    }
};

TEST_F(StartupTimerTest, Construct_NoCheckpoints) {
    StartupTimer timer;

    // Newly constructed timer should have no checkpoints
    EXPECT_EQ(timer.count(), 0);

    // Total elapsed should be >= 0 (timer is running)
    EXPECT_GE(timer.totalElapsedMs(), 0);
}

TEST_F(StartupTimerTest, Checkpoint_RecordsLabel) {
    StartupTimer timer;

    timer.checkpoint("test");

    // Should have exactly one checkpoint
    EXPECT_EQ(timer.count(), 1);

    // Retrieve checkpoints and verify label
    auto checkpoints = timer.checkpoints();
    ASSERT_EQ(checkpoints.size(), 1);
    EXPECT_EQ(checkpoints[0].first, "test");

    // Elapsed time should be >= 0
    EXPECT_GE(checkpoints[0].second, 0);
}

TEST_F(StartupTimerTest, Checkpoint_Monotonic) {
    StartupTimer timer;

    // Add first checkpoint
    timer.checkpoint("first");
    qint64 firstElapsed = timer.checkpoints()[0].second;

    // Sleep for at least 10 ms
    QThread::msleep(10);

    // Add second checkpoint
    timer.checkpoint("second");

    // Verify we now have 2 checkpoints
    EXPECT_EQ(timer.count(), 2);

    auto checkpoints = timer.checkpoints();
    ASSERT_EQ(checkpoints.size(), 2);

    qint64 secondElapsed = checkpoints[1].second;

    // Second checkpoint's elapsed time should be >= first checkpoint's elapsed time
    EXPECT_GE(secondElapsed, firstElapsed);

    // Verify the labels are correct
    EXPECT_EQ(checkpoints[0].first, "first");
    EXPECT_EQ(checkpoints[1].first, "second");
}

TEST_F(StartupTimerTest, Summary_Format) {
    StartupTimer timer;

    // Add some checkpoints
    timer.checkpoint("initialize");
    QThread::msleep(5);
    timer.checkpoint("load_config");
    QThread::msleep(5);
    timer.checkpoint("ready");

    QString summary = timer.summary();

    // Summary should not be empty
    EXPECT_FALSE(summary.isEmpty());

    // Summary should contain all the checkpoint labels
    EXPECT_TRUE(summary.contains("initialize"));
    EXPECT_TRUE(summary.contains("load_config"));
    EXPECT_TRUE(summary.contains("ready"));

    // Summary should contain "Startup timing:" header
    EXPECT_TRUE(summary.contains("Startup timing:"));

    // Summary should contain "Total:" footer
    EXPECT_TRUE(summary.contains("Total:"));
}
