// Control fixture for tools/gate/check_magnitude_guard.py (PERF-02 / F37).
//
// POSITIVE control: this file MUST be flagged. It reproduces, in its own
// words, the exact shape F27 found twice and fixed in d85ec1d -- a wall-clock
// threshold that is permanently false on fast hardware, gating the only
// comparative assertion in the test.
//
// It is NOT compiled or run. It lives outside tests/ precisely so the gate's
// default scan root (tests) cannot pick it up and contaminate the real count;
// tools/gate/test_check_magnitude_guard.py points the scanner at it explicitly.
//
// Deliberately worded UNLIKE the two real instances (different variable names,
// different constants, no "Only assert if ..." comment) so that a scanner
// which secretly keys on phrasing rather than shape fails this control.

#include <gtest/gtest.h>

TEST(PlantedPositive, RenderThroughputScales)
{
    double elapsedMs = measureSmallRender();
    double bigMs = measureBigRender();

    // The trap: on a fast host elapsedMs never reaches 100, so the ratio
    // assertion never executes and the test is green having measured nothing.
    if (elapsedMs > 100) {
        EXPECT_LT(bigMs / elapsedMs, 12.0)
            << "render time must not blow up with page size";
    }
}

TEST(PlantedPositive, UnbracedSingleStatementFormAlsoCounts)
{
    double seconds = measureLoad();
    // Same defect written without braces. A brace-only scanner misses this.
    if (seconds >= 0.25) EXPECT_LT(seconds, 2.0);
}
