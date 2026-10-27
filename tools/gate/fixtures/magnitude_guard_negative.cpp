// Control fixture for tools/gate/check_magnitude_guard.py (PERF-02 / F37).
//
// NEGATIVE control: this file MUST pass. Every guard in it is legitimate under
// the rule the backlog states directly beneath the PERF-02 row:
//
//     A guard on measurement AVAILABILITY is legitimate -- the alternative is
//     asserting on a number you do not have. A guard on measurement MAGNITUDE
//     silently disables the oracle on fast hardware, and gets faster every year.
//
// A scanner that merely finds "if (<number>) { EXPECT_ }" flags this file too,
// and would be useless: it would fire on 26 sites in tests/ of which 24 are
// fine. This control is what proves the discrimination is real.
//
// Not compiled or run; see the header of the positive control.

#include <gtest/gtest.h>

TEST(PlantedNegative, MemoryGrowthStaysBounded)
{
    // Availability, not magnitude: getWorkingSetMB() is a `return 0` stub on
    // platforms that cannot measure this at all. Zero threshold -- no amount
    // of hardware improvement can close this guard.
    size_t baselineMB = getWorkingSetMB();
    if (baselineMB > 0) {
        size_t nowMB = afterWorkload();
        EXPECT_LT(nowMB - baselineMB, 200u) << "working set must stay bounded";
    }
}

TEST(PlantedNegative, ContrastRatioAssertedOnBothPaths)
{
    // Non-zero threshold, but BOTH branches assert, so no path is silent.
    // This is the shape of tests/test_high_contrast.cpp:158.
    double ratio = contrastRatio();
    if (ratio >= 4.5) {
        EXPECT_TRUE(passesWcagAA());
    } else {
        EXPECT_FALSE(passesWcagAA());
    }
}

TEST(PlantedNegative, NotFoundSentinel)
{
    int idx = indexOfField("name");
    if (idx < 0) {
        EXPECT_TRUE(fieldListIsEmpty());
    }
}

TEST(PlantedNegative, CommentsAndStringsAreNotCode)
{
    // The blanking stage must make these invisible. If they are not blanked,
    // this test file produces spurious matches and the negative control fails.
    //     if (elapsedMs > 100) { EXPECT_LT(a, b); }
    const char* doc = "if (elapsedMs > 100) { EXPECT_LT(a, b); }";
    /* if (seconds > 3.5) { ASSERT_GT(x, y); } */
    EXPECT_NE(doc, nullptr);
}
