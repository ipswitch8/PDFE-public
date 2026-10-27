#include <gtest/gtest.h>
#include "tre/TreRegionTool.h"
#include "annotations/AnnotationType.h"
#include <QMouseEvent>
#include <QPainter>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <functional>

using namespace pdfe::tre;
using namespace pdfe::annotations;

// Test fixture for TreRegionTool tests
class TreRegionToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        tool = std::make_unique<TreRegionTool>();
    }

    void TearDown() override {
        tool.reset();
    }

    std::unique_ptr<TreRegionTool> tool;

    // Helper function to create mouse press event
    QMouseEvent createPressEvent(const QPointF& pos, Qt::MouseButton button = Qt::LeftButton) {
        return QMouseEvent(QEvent::MouseButtonPress, pos, button, button, Qt::NoModifier);
    }

    // Helper function to create mouse move event
    QMouseEvent createMoveEvent(const QPointF& pos) {
        return QMouseEvent(QEvent::MouseMove, pos, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    }

    // Helper function to create mouse release event
    QMouseEvent createReleaseEvent(const QPointF& pos, Qt::MouseButton button = Qt::LeftButton) {
        return QMouseEvent(QEvent::MouseButtonRelease, pos, button, button, Qt::NoModifier);
    }
};

// Test 1: ConstructAndDestroy
TEST_F(TreRegionToolTest, ConstructAndDestroy) {
    // Should construct and destruct without crash
    auto tempTool = std::make_unique<TreRegionTool>();
    EXPECT_NE(tempTool, nullptr);
    // Destruction happens automatically - no crash expected
}

// Test 2: ToolTypeReturnsUnknown
TEST_F(TreRegionToolTest, ToolTypeReturnsUnknown) {
    EXPECT_EQ(tool->toolType(), AnnotationType::Unknown);
}

// Test 3: MousePressStartsDrawing
TEST_F(TreRegionToolTest, MousePressStartsDrawing) {
    // Track whether callback was fired
    bool callbackFired = false;
    int receivedPageIndex = -1;
    QRectF receivedRect;

    tool->regionSelected = [&](int pageIndex, QRectF rect) {
        callbackFired = true;
        receivedPageIndex = pageIndex;
        receivedRect = rect;
    };

    // Simulate left mouse press
    QMouseEvent pressEvent = createPressEvent(QPointF(0, 0));
    tool->onMousePress(&pressEvent, 0, QPointF(100, 200));

    // Move to a different point
    QMouseEvent moveEvent = createMoveEvent(QPointF(0, 0));
    tool->onMouseMove(&moveEvent, 0, QPointF(300, 400));

    // Release at that point
    QMouseEvent releaseEvent = createReleaseEvent(QPointF(0, 0));
    tool->onMouseRelease(&releaseEvent, 0, QPointF(300, 400));

    // Verify callback fired with correct data
    EXPECT_TRUE(callbackFired);
    EXPECT_EQ(receivedPageIndex, 0);
    EXPECT_FLOAT_EQ(receivedRect.left(), 100.0f);
    EXPECT_FLOAT_EQ(receivedRect.top(), 200.0f);
    EXPECT_FLOAT_EQ(receivedRect.width(), 200.0f);
    EXPECT_FLOAT_EQ(receivedRect.height(), 200.0f);
}

// Test 4: MouseMoveUpdatesEndPoint
TEST_F(TreRegionToolTest, MouseMoveUpdatesEndPoint) {
    bool callbackFired = false;
    QRectF receivedRect;

    tool->regionSelected = [&](int pageIndex, QRectF rect) {
        callbackFired = true;
        receivedRect = rect;
    };

    // Press at start point
    QMouseEvent pressEvent = createPressEvent(QPointF(0, 0));
    tool->onMousePress(&pressEvent, 0, QPointF(50, 100));

    // Move to intermediate point (should update end point)
    QMouseEvent moveEvent1 = createMoveEvent(QPointF(0, 0));
    tool->onMouseMove(&moveEvent1, 0, QPointF(200, 250));

    // Move to final point (should update end point again)
    QMouseEvent moveEvent2 = createMoveEvent(QPointF(0, 0));
    tool->onMouseMove(&moveEvent2, 0, QPointF(300, 400));

    // Release at current position
    QMouseEvent releaseEvent = createReleaseEvent(QPointF(0, 0));
    tool->onMouseRelease(&releaseEvent, 0, QPointF(300, 400));

    // Verify callback fired with rect using final end point
    EXPECT_TRUE(callbackFired);
    EXPECT_FLOAT_EQ(receivedRect.left(), 50.0f);
    EXPECT_FLOAT_EQ(receivedRect.top(), 100.0f);
    EXPECT_FLOAT_EQ(receivedRect.width(), 250.0f);  // 300 - 50
    EXPECT_FLOAT_EQ(receivedRect.height(), 300.0f); // 400 - 100
}

// Test 5: MouseReleaseFiresCallback
TEST_F(TreRegionToolTest, MouseReleaseFiresCallback) {
    bool callbackFired = false;
    int receivedPageIndex = -1;
    QRectF receivedRect;

    tool->regionSelected = [&](int pageIndex, QRectF rect) {
        callbackFired = true;
        receivedPageIndex = pageIndex;
        receivedRect = rect;
    };

    // Press at (10, 20)
    QMouseEvent pressEvent = createPressEvent(QPointF(0, 0));
    tool->onMousePress(&pressEvent, 0, QPointF(10, 20));

    // Release at (200, 300)
    QMouseEvent releaseEvent = createReleaseEvent(QPointF(0, 0));
    tool->onMouseRelease(&releaseEvent, 0, QPointF(200, 300));

    // Verify callback fired with correct data
    EXPECT_TRUE(callbackFired);
    EXPECT_EQ(receivedPageIndex, 0);
    EXPECT_FLOAT_EQ(receivedRect.left(), 10.0f);
    EXPECT_FLOAT_EQ(receivedRect.top(), 20.0f);
    EXPECT_FLOAT_EQ(receivedRect.width(), 190.0f);  // 200 - 10
    EXPECT_FLOAT_EQ(receivedRect.height(), 280.0f); // 300 - 20
}

// Test 6: MinimumSizeCheck
TEST_F(TreRegionToolTest, MinimumSizeCheck) {
    bool callbackFired = false;

    tool->regionSelected = [&](int pageIndex, QRectF rect) {
        callbackFired = true;
    };

    // Press at (10, 20)
    QMouseEvent pressEvent = createPressEvent(QPointF(0, 0));
    tool->onMousePress(&pressEvent, 0, QPointF(10, 20));

    // Release at (12, 22) - creates a 2x2 rect (too small)
    QMouseEvent releaseEvent = createReleaseEvent(QPointF(0, 0));
    tool->onMouseRelease(&releaseEvent, 0, QPointF(12, 22));

    // Verify callback did NOT fire (rect too small - both dimensions < 5)
    EXPECT_FALSE(callbackFired);
}

// Test 6b: Minimum size check with one dimension >= 5
TEST_F(TreRegionToolTest, MinimumSizeCheckOneDimensionLarge) {
    bool callbackFired = false;

    tool->regionSelected = [&](int pageIndex, QRectF rect) {
        callbackFired = true;
    };

    // Press at (10, 20)
    QMouseEvent pressEvent = createPressEvent(QPointF(0, 0));
    tool->onMousePress(&pressEvent, 0, QPointF(10, 20));

    // Release at (12, 30) - creates a 2x10 rect (width < 5 but height >= 5)
    QMouseEvent releaseEvent = createReleaseEvent(QPointF(0, 0));
    tool->onMouseRelease(&releaseEvent, 0, QPointF(12, 30));

    // Verify callback DOES fire (one dimension >= 5)
    EXPECT_TRUE(callbackFired);
}

// Test 7: PaintOverlayNoCrash
TEST_F(TreRegionToolTest, PaintOverlayNoCrash) {
    // Create a QImage and QPainter
    QImage image(100, 100, QImage::Format_ARGB32);
    QPainter painter(&image);

    // Call paint overlay without drawing - should not crash
    EXPECT_NO_THROW({
        tool->onPaintOverlay(painter, 0, 1.0f, QPointF(0, 0), 800.0f);
    });

    // Now start drawing
    QMouseEvent pressEvent = createPressEvent(QPointF(0, 0));
    tool->onMousePress(&pressEvent, 0, QPointF(50, 100));

    QMouseEvent moveEvent = createMoveEvent(QPointF(0, 0));
    tool->onMouseMove(&moveEvent, 0, QPointF(150, 200));

    // Call paint overlay while drawing - should not crash
    EXPECT_NO_THROW({
        tool->onPaintOverlay(painter, 0, 1.0f, QPointF(0, 0), 800.0f);
    });

    // Paint on different page - should not crash
    EXPECT_NO_THROW({
        tool->onPaintOverlay(painter, 1, 1.0f, QPointF(0, 0), 800.0f);
    });
}

// Test 8: RightButtonIgnored
TEST_F(TreRegionToolTest, RightButtonIgnored) {
    bool callbackFired = false;

    tool->regionSelected = [&](int pageIndex, QRectF rect) {
        callbackFired = true;
    };

    // Press with right button
    QMouseEvent pressEvent = createPressEvent(QPointF(0, 0), Qt::RightButton);
    tool->onMousePress(&pressEvent, 0, QPointF(10, 20));

    // Try to release with left button
    QMouseEvent releaseEvent = createReleaseEvent(QPointF(0, 0), Qt::LeftButton);
    tool->onMouseRelease(&releaseEvent, 0, QPointF(200, 300));

    // Verify callback did NOT fire (drawing never started)
    EXPECT_FALSE(callbackFired);
}

// Additional Test: Release with right button ignored
TEST_F(TreRegionToolTest, RightButtonReleaseIgnored) {
    bool callbackFired = false;

    tool->regionSelected = [&](int pageIndex, QRectF rect) {
        callbackFired = true;
    };

    // Press with left button (starts drawing)
    QMouseEvent pressEvent = createPressEvent(QPointF(0, 0), Qt::LeftButton);
    tool->onMousePress(&pressEvent, 0, QPointF(10, 20));

    // Try to release with right button (should be ignored)
    QMouseEvent releaseEvent = createReleaseEvent(QPointF(0, 0), Qt::RightButton);
    tool->onMouseRelease(&releaseEvent, 0, QPointF(200, 300));

    // Verify callback did NOT fire (right button release ignored)
    EXPECT_FALSE(callbackFired);
}

// Additional Test: Mouse move on different page ignored
TEST_F(TreRegionToolTest, MouseMoveOnDifferentPageIgnored) {
    bool callbackFired = false;
    QRectF receivedRect;

    tool->regionSelected = [&](int pageIndex, QRectF rect) {
        callbackFired = true;
        receivedRect = rect;
    };

    // Press on page 0
    QMouseEvent pressEvent = createPressEvent(QPointF(0, 0));
    tool->onMousePress(&pressEvent, 0, QPointF(10, 20));

    // Move on page 1 (different page - should be ignored)
    QMouseEvent moveEvent = createMoveEvent(QPointF(0, 0));
    tool->onMouseMove(&moveEvent, 1, QPointF(500, 600));

    // Release on page 0 at original press point
    QMouseEvent releaseEvent = createReleaseEvent(QPointF(0, 0));
    tool->onMouseRelease(&releaseEvent, 0, QPointF(10, 20));

    // Callback should not fire (rect too small - 0x0)
    EXPECT_FALSE(callbackFired);
}

// Additional Test: Callback not set doesn't crash
TEST_F(TreRegionToolTest, NoCallbackNoCrash) {
    // Don't set regionSelected callback

    // Press and release with valid region
    QMouseEvent pressEvent = createPressEvent(QPointF(0, 0));
    tool->onMousePress(&pressEvent, 0, QPointF(10, 20));

    QMouseEvent releaseEvent = createReleaseEvent(QPointF(0, 0));

    // Should not crash even without callback set
    EXPECT_NO_THROW({
        tool->onMouseRelease(&releaseEvent, 0, QPointF(200, 300));
    });
}

// Additional Test: Rect with inverted coordinates (drag from bottom-right to top-left)
TEST_F(TreRegionToolTest, InvertedCoordinatesNormalized) {
    bool callbackFired = false;
    QRectF receivedRect;

    tool->regionSelected = [&](int pageIndex, QRectF rect) {
        callbackFired = true;
        receivedRect = rect;
    };

    // Press at bottom-right (300, 400)
    QMouseEvent pressEvent = createPressEvent(QPointF(0, 0));
    tool->onMousePress(&pressEvent, 0, QPointF(300, 400));

    // Release at top-left (50, 100)
    QMouseEvent releaseEvent = createReleaseEvent(QPointF(0, 0));
    tool->onMouseRelease(&releaseEvent, 0, QPointF(50, 100));

    // Verify callback fired with normalized rect (top-left corner at 50, 100)
    EXPECT_TRUE(callbackFired);
    EXPECT_FLOAT_EQ(receivedRect.left(), 50.0f);
    EXPECT_FLOAT_EQ(receivedRect.top(), 100.0f);
    EXPECT_FLOAT_EQ(receivedRect.width(), 250.0f);  // 300 - 50
    EXPECT_FLOAT_EQ(receivedRect.height(), 300.0f); // 400 - 100
}

// Additional Test: Multiple regions can be drawn sequentially
TEST_F(TreRegionToolTest, MultipleRegionsSequentially) {
    int callbackCount = 0;
    std::vector<QRectF> receivedRects;

    tool->regionSelected = [&](int pageIndex, QRectF rect) {
        callbackCount++;
        receivedRects.push_back(rect);
    };

    // First region
    QMouseEvent pressEvent1 = createPressEvent(QPointF(0, 0));
    tool->onMousePress(&pressEvent1, 0, QPointF(10, 20));
    QMouseEvent releaseEvent1 = createReleaseEvent(QPointF(0, 0));
    tool->onMouseRelease(&releaseEvent1, 0, QPointF(100, 120));

    // Second region
    QMouseEvent pressEvent2 = createPressEvent(QPointF(0, 0));
    tool->onMousePress(&pressEvent2, 0, QPointF(200, 300));
    QMouseEvent releaseEvent2 = createReleaseEvent(QPointF(0, 0));
    tool->onMouseRelease(&releaseEvent2, 0, QPointF(400, 500));

    // Verify both callbacks fired
    EXPECT_EQ(callbackCount, 2);
    EXPECT_EQ(receivedRects.size(), 2);

    // Verify first rect
    EXPECT_FLOAT_EQ(receivedRects[0].left(), 10.0f);
    EXPECT_FLOAT_EQ(receivedRects[0].top(), 20.0f);

    // Verify second rect
    EXPECT_FLOAT_EQ(receivedRects[1].left(), 200.0f);
    EXPECT_FLOAT_EQ(receivedRects[1].top(), 300.0f);
}
