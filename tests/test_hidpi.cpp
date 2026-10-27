#include <gtest/gtest.h>
#include "rendering/TileId.h"
#include "rendering/TileCache.h"

#include <QApplication>
#include <QWidget>
#include <QScreen>

using namespace pdfe::rendering;

// ─── HiDPI Support Tests ────────────────────────────────────────────────────

class HiDPITest : public ::testing::Test {
protected:
    void SetUp() override {
        // QApplication must exist for screen queries
        // (it's created by GTest main with Qt integration)
    }
};

TEST_F(HiDPITest, DetectDevicePixelRatio) {
    // Get primary screen's device pixel ratio
    auto* screen = QApplication::primaryScreen();
    ASSERT_NE(screen, nullptr) << "Should have a primary screen";

    qreal dpr = screen->devicePixelRatio();
    EXPECT_GE(dpr, 1.0) << "Device pixel ratio should be >= 1.0";
    EXPECT_LE(dpr, 8.0) << "Device pixel ratio should be <= 8.0 (reasonable max)";
}

TEST_F(HiDPITest, TileIdIncorporatesScale) {
    // TileIds at different zoom levels should be different
    TileId id1{0, 0, 0, 1.0f};
    TileId id2{0, 0, 0, 2.0f};

    EXPECT_NE(id1, id2) << "Different zoom levels should produce different TileIds";

    // Same zoom should produce same TileId
    TileId id3{0, 0, 0, 1.0f};
    EXPECT_EQ(id1, id3);
}

TEST_F(HiDPITest, ScaledTileRendering) {
    // When rendering for HiDPI, the effective zoom should be zoom * DPR.
    // This test verifies the math, not actual rendering.
    float baseZoom = 1.0f;
    float dpr = 2.0f;  // Simulated 2x HiDPI
    float effectiveZoom = baseZoom * dpr;

    EXPECT_NEAR(effectiveZoom, 2.0f, 0.001f);

    // Tile size in device pixels should be TILE_SIZE * DPR
    constexpr int TILE_SIZE = 512;
    int deviceTileSize = static_cast<int>(TILE_SIZE * dpr);
    EXPECT_EQ(deviceTileSize, 1024);
}

TEST_F(HiDPITest, TileCacheMemoryScaling) {
    // HiDPI tiles use more memory. Verify the cache handles this correctly.
    TileCache cache(64 * 1024 * 1024);  // 64MB test cache

    // A regular tile (512x512 RGBA) = ~1MB
    // A 2x HiDPI tile (1024x1024 RGBA) = ~4MB
    QImage regularTile(512, 512, QImage::Format_RGBA8888);
    regularTile.fill(Qt::white);

    QImage hidpiTile(1024, 1024, QImage::Format_RGBA8888);
    hidpiTile.fill(Qt::white);

    TileId id1{0, 0, 0, 1.0f};
    TileId id2{0, 0, 0, 2.0f};

    cache.put(id1, regularTile);
    cache.put(id2, hidpiTile);

    // Both should be retrievable
    auto retrieved1 = cache.get(id1);
    auto retrieved2 = cache.get(id2);

    EXPECT_TRUE(retrieved1.has_value());
    EXPECT_TRUE(retrieved2.has_value());

    // Memory usage should reflect different tile sizes
    EXPECT_GT(cache.memoryUsage(), 0u);
}

TEST_F(HiDPITest, ScreenDPRQuery) {
    // Verify we can query screen DPR without crashes
    auto screens = QApplication::screens();
    EXPECT_FALSE(screens.isEmpty()) << "Should have at least one screen";

    for (auto* screen : screens) {
        qreal dpr = screen->devicePixelRatio();
        EXPECT_GE(dpr, 1.0);
        EXPECT_LE(dpr, 8.0);
    }
}
