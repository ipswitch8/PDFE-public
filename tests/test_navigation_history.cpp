// -*- coding: utf-8 -*-
// Browser-style back/forward history for PDFE page navigation.
#include <gtest/gtest.h>
#include "ui/NavigationHistory.h"

using pdfe::ui::NavigationHistory;

TEST(NavigationHistoryTest, NewHistoryIsEmpty)
{
    NavigationHistory h;
    EXPECT_EQ(h.size(), 0);
    EXPECT_EQ(h.cursor(), -1);
    EXPECT_FALSE(h.canGoBack());
    EXPECT_FALSE(h.canGoForward());
    EXPECT_FALSE(h.isReplaying());
}

TEST(NavigationHistoryTest, RecordVisitGrowsHistory)
{
    NavigationHistory h;
    h.recordVisit(0);
    h.recordVisit(5);
    h.recordVisit(2);
    EXPECT_EQ(h.size(), 3);
    EXPECT_EQ(h.cursor(), 2);
}

TEST(NavigationHistoryTest, CoalescesConsecutiveDuplicates)
{
    NavigationHistory h;
    h.recordVisit(3);
    h.recordVisit(3);
    h.recordVisit(3);
    EXPECT_EQ(h.size(), 1);
}

TEST(NavigationHistoryTest, NegativePageIgnored)
{
    NavigationHistory h;
    h.recordVisit(-1);
    EXPECT_EQ(h.size(), 0);
    h.recordVisit(2);
    h.recordVisit(-5);
    EXPECT_EQ(h.size(), 1);
}

TEST(NavigationHistoryTest, CanGoBackAfterMultipleVisits)
{
    NavigationHistory h;
    h.recordVisit(0);
    EXPECT_FALSE(h.canGoBack());
    h.recordVisit(1);
    EXPECT_TRUE(h.canGoBack());
}

TEST(NavigationHistoryTest, GoBackReturnsPreviousPage)
{
    NavigationHistory h;
    h.recordVisit(0);
    h.recordVisit(4);
    h.recordVisit(7);
    EXPECT_EQ(h.goBack(), 4);
    EXPECT_EQ(h.cursor(), 1);
    EXPECT_EQ(h.goBack(), 0);
    EXPECT_EQ(h.cursor(), 0);
    EXPECT_FALSE(h.canGoBack());
    EXPECT_EQ(h.goBack(), -1);  // edge — no further back
}

TEST(NavigationHistoryTest, GoForwardAfterGoBackReturnsOriginalTarget)
{
    NavigationHistory h;
    h.recordVisit(0);
    h.recordVisit(5);
    h.goBack();
    EXPECT_TRUE(h.canGoForward());
    EXPECT_EQ(h.goForward(), 5);
    EXPECT_FALSE(h.canGoForward());
    EXPECT_EQ(h.goForward(), -1);
}

TEST(NavigationHistoryTest, RecordAfterBackTruncatesForwardHistory)
{
    NavigationHistory h;
    h.recordVisit(0);
    h.recordVisit(1);
    h.recordVisit(2);
    h.goBack();       // cursor at idx 1 (page 1)
    h.goBack();       // cursor at idx 0 (page 0)
    h.recordVisit(9); // should truncate forward history
    EXPECT_EQ(h.size(), 2);
    EXPECT_EQ(h.cursor(), 1);
    EXPECT_FALSE(h.canGoForward());
    // Going back now returns the original page 0.
    EXPECT_EQ(h.goBack(), 0);
}

TEST(NavigationHistoryTest, ReplayingFlagSuppressesRecording)
{
    NavigationHistory h;
    h.recordVisit(0);
    h.recordVisit(5);
    // Simulate a back action: the canvas page-change signal would
    // otherwise re-record the visit. setReplaying(true) suppresses it.
    h.setReplaying(true);
    h.recordVisit(0);  // would normally grow history
    EXPECT_EQ(h.size(), 2);
    h.setReplaying(false);
}

TEST(NavigationHistoryTest, GoBackSuppressesAutomaticRecording)
{
    // goBack itself should leave isReplaying() false when it returns
    // (the slot toggles it around the actual canvas goToPage call).
    NavigationHistory h;
    h.recordVisit(0);
    h.recordVisit(5);
    h.goBack();
    EXPECT_FALSE(h.isReplaying());
}

TEST(NavigationHistoryTest, ClearResetsEverything)
{
    NavigationHistory h;
    h.recordVisit(0);
    h.recordVisit(1);
    h.recordVisit(2);
    h.setReplaying(true);
    h.clear();
    EXPECT_EQ(h.size(), 0);
    EXPECT_EQ(h.cursor(), -1);
    EXPECT_FALSE(h.canGoBack());
    EXPECT_FALSE(h.canGoForward());
    EXPECT_FALSE(h.isReplaying());
}
