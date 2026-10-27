// Copyright (c) 2025 - PDFE Project
// PHASE1_HARDENING: skip=0 expect_ge=0 no_throw=4  (2026-05-08)
// Unit tests for PdfCanvas keyboard navigation functionality

#include <gtest/gtest.h>
#include "ui/PdfCanvas.h"
#include <QKeyEvent>
#include <QTest>
#include <Qt>

using namespace pdfe::ui;

// Test fixture for keyboard navigation tests
class KeyboardNavigationTest : public ::testing::Test {
protected:
    void SetUp() override {
        canvas = new PdfCanvas();
        // Ensure canvas can receive key events
        canvas->setFocus();
    }

    void TearDown() override {
        delete canvas;
        canvas = nullptr;
    }

    PdfCanvas* canvas = nullptr;
};

// Test 1: PageDown without document should not crash
TEST_F(KeyboardNavigationTest, PageDown_NoDoc_DoesNotCrash) {
    EXPECT_NO_THROW({
        QTest::keyClick(canvas, Qt::Key_PageDown);
    });
    // Should remain at page 0 since no document is loaded
    EXPECT_EQ(canvas->currentPage(), 0);
}

// Test 2: PageUp without document should not crash
TEST_F(KeyboardNavigationTest, PageUp_NoDoc_DoesNotCrash) {
    EXPECT_NO_THROW({
        QTest::keyClick(canvas, Qt::Key_PageUp);
    });
    // Should remain at page 0 since no document is loaded
    EXPECT_EQ(canvas->currentPage(), 0);
}

// Test 3: Home key without document should not crash
TEST_F(KeyboardNavigationTest, Home_NoDoc_DoesNotCrash) {
    EXPECT_NO_THROW({
        QTest::keyClick(canvas, Qt::Key_Home);
    });
    // Should remain at page 0 since no document is loaded
    EXPECT_EQ(canvas->currentPage(), 0);
}

// Test 4: End key without document should not crash
TEST_F(KeyboardNavigationTest, End_NoDoc_DoesNotCrash) {
    EXPECT_NO_THROW({
        QTest::keyClick(canvas, Qt::Key_End);
    });
    // Should remain at page 0 since no document is loaded
    EXPECT_EQ(canvas->currentPage(), 0);
}

// Test 5: Plus key should zoom in
TEST_F(KeyboardNavigationTest, ZoomIn_Plus) {
    const float initialZoom = canvas->zoom();

    QTest::keyClick(canvas, Qt::Key_Plus);

    EXPECT_GT(canvas->zoom(), initialZoom)
        << "Zoom level should increase after pressing + key";
}

// Test 6: Minus key should zoom out
TEST_F(KeyboardNavigationTest, ZoomOut_Minus) {
    const float initialZoom = canvas->zoom();

    QTest::keyClick(canvas, Qt::Key_Minus);

    EXPECT_LT(canvas->zoom(), initialZoom)
        << "Zoom level should decrease after pressing - key";
}

// Test 7: Arrow keys must not crash AND must not silently mutate
// the canvas's no-document state. EXPECT_NO_THROW alone would pass
// if a key handler accidentally advanced currentPage_ to a negative
// value or NaN'd the zoom — the no-state-change invariant catches that.
TEST_F(KeyboardNavigationTest, ArrowKeys_DoNotCrash) {
    const int pageBefore = canvas->currentPage();
    const float zoomBefore = canvas->zoom();

    EXPECT_NO_THROW({
        QTest::keyClick(canvas, Qt::Key_Up);
    }) << "Up arrow key should not crash";
    EXPECT_EQ(canvas->currentPage(), pageBefore);
    EXPECT_FLOAT_EQ(canvas->zoom(), zoomBefore);

    EXPECT_NO_THROW({
        QTest::keyClick(canvas, Qt::Key_Down);
    }) << "Down arrow key should not crash";
    EXPECT_EQ(canvas->currentPage(), pageBefore);
    EXPECT_FLOAT_EQ(canvas->zoom(), zoomBefore);

    EXPECT_NO_THROW({
        QTest::keyClick(canvas, Qt::Key_Left);
    }) << "Left arrow key should not crash";
    EXPECT_EQ(canvas->currentPage(), pageBefore);
    EXPECT_FLOAT_EQ(canvas->zoom(), zoomBefore);

    EXPECT_NO_THROW({
        QTest::keyClick(canvas, Qt::Key_Right);
    }) << "Right arrow key should not crash";
    EXPECT_EQ(canvas->currentPage(), pageBefore);
    EXPECT_FLOAT_EQ(canvas->zoom(), zoomBefore);
}

// Test 8: Canvas should have strong focus policy for keyboard navigation
TEST_F(KeyboardNavigationTest, FocusPolicy) {
    const Qt::FocusPolicy policy = canvas->focusPolicy();

    EXPECT_TRUE(policy & Qt::StrongFocus)
        << "PdfCanvas should have StrongFocus policy to accept keyboard input. "
        << "Current policy: " << static_cast<int>(policy);
}
