// test_cache_stats.cpp - Unit tests for CacheStats and TileCache instrumentation
// Uses shared test_main.cpp with QApplication setup

#include <gtest/gtest.h>
#include "rendering/CacheStats.h"
#include "rendering/TileCache.h"
#include "rendering/TileId.h"
#include <QImage>

using namespace pdfe::rendering;

// ============================================================================
// CacheStats Tests
// ============================================================================

TEST(CacheStats, DefaultValues) {
    // Arrange & Act
    CacheStats stats;

    // Assert - All fields should be zero initially
    EXPECT_EQ(0u, stats.hitCount);
    EXPECT_EQ(0u, stats.missCount);
    EXPECT_EQ(0u, stats.evictionCount);
    EXPECT_EQ(0u, stats.putCount);
    EXPECT_EQ(0u, stats.currentMemoryBytes);
    EXPECT_EQ(0u, stats.maxMemoryBytes);
    EXPECT_EQ(0u, stats.tileCount);
}

TEST(CacheStats, HitRatio_ZeroTotal) {
    // Arrange
    CacheStats stats;
    stats.hitCount = 0;
    stats.missCount = 0;

    // Act
    double ratio = stats.hitRatio();

    // Assert - Should return 0.0 when total is zero (avoid division by zero)
    EXPECT_DOUBLE_EQ(0.0, ratio);
}

TEST(CacheStats, HitRatio_Normal) {
    // Arrange
    CacheStats stats;
    stats.hitCount = 3;
    stats.missCount = 1;

    // Act
    double ratio = stats.hitRatio();

    // Assert - 3/(3+1) = 0.75
    EXPECT_DOUBLE_EQ(0.75, ratio);
}

TEST(CacheStats, MemoryUtilization) {
    // Arrange
    CacheStats stats;
    stats.currentMemoryBytes = 256;
    stats.maxMemoryBytes = 512;

    // Act
    double utilization = stats.memoryUtilization();

    // Assert - 256/512 = 0.5
    EXPECT_DOUBLE_EQ(0.5, utilization);
}

TEST(CacheStats, Reset) {
    // Arrange
    CacheStats stats;
    stats.hitCount = 10;
    stats.missCount = 5;
    stats.evictionCount = 3;
    stats.putCount = 15;
    stats.currentMemoryBytes = 1024;
    stats.maxMemoryBytes = 2048;
    stats.tileCount = 8;

    // Act
    stats.reset();

    // Assert - Operation counters should be zero after reset
    EXPECT_EQ(0u, stats.hitCount);
    EXPECT_EQ(0u, stats.missCount);
    EXPECT_EQ(0u, stats.evictionCount);
    EXPECT_EQ(0u, stats.putCount);
    // Note: reset() only resets operation counters, not memory/tile tracking fields
}

// ============================================================================
// TileCache Instrumentation Tests
// ============================================================================

TEST(TileCache, Stats_PutIncrementsCount) {
    // Arrange
    TileCache cache;
    cache.setMaxMemory(10 * 1024 * 1024); // 10 MB

    TileId tile1{0, 0, 0, 1.0f};
    TileId tile2{0, 1, 0, 1.0f};
    TileId tile3{0, 0, 1, 1.0f};

    QImage image1(256, 256, QImage::Format_ARGB32);
    QImage image2(256, 256, QImage::Format_ARGB32);
    QImage image3(256, 256, QImage::Format_ARGB32);

    // Act - Put 3 tiles
    cache.put(tile1, image1);
    cache.put(tile2, image2);
    cache.put(tile3, image3);

    CacheStats stats = cache.stats();

    // Assert - putCount should be 3
    EXPECT_EQ(3u, stats.putCount);
    EXPECT_EQ(3u, stats.tileCount);
}

TEST(TileCache, Stats_GetHitMiss) {
    // Arrange
    TileCache cache;
    cache.setMaxMemory(10 * 1024 * 1024); // 10 MB

    TileId tileInCache{0, 0, 0, 1.0f};
    TileId tileNotInCache{1, 1, 1, 1.0f};

    QImage image(256, 256, QImage::Format_ARGB32);
    cache.put(tileInCache, image);

    // Act - Get cached tile (should be a hit)
    auto result1 = cache.get(tileInCache);

    // Get non-cached tile (should be a miss)
    auto result2 = cache.get(tileNotInCache);

    CacheStats stats = cache.stats();

    // Assert
    EXPECT_TRUE(result1.has_value()) << "Cached tile should be found";
    EXPECT_FALSE(result2.has_value()) << "Non-cached tile should not be found";
    EXPECT_EQ(1u, stats.hitCount) << "Should have 1 cache hit";
    EXPECT_EQ(1u, stats.missCount) << "Should have 1 cache miss";
}

TEST(TileCache, Stats_Eviction) {
    // Arrange - Create cache with tiny memory limit to force evictions
    TileCache cache;
    cache.setMaxMemory(1024); // Only 1 KB - can hold ~1 tile max

    // Create multiple large tiles to force evictions
    // Each 256x256 ARGB32 image is ~256 KB
    QImage largeImage(256, 256, QImage::Format_ARGB32);

    TileId tile1{0, 0, 0, 1.0f};
    TileId tile2{0, 1, 0, 1.0f};
    TileId tile3{0, 2, 0, 1.0f};

    // Act - Put tiles that exceed memory limit
    cache.put(tile1, largeImage);
    cache.put(tile2, largeImage);
    cache.put(tile3, largeImage);

    CacheStats stats = cache.stats();

    // Assert - Should have evictions due to memory pressure
    EXPECT_GT(stats.evictionCount, 0u) << "Should have evicted tiles to stay under memory limit";
    EXPECT_EQ(3u, stats.putCount) << "Should have attempted to put 3 tiles";
}

TEST(TileCache, Stats_Reset) {
    // Arrange
    TileCache cache;
    cache.setMaxMemory(10 * 1024 * 1024); // 10 MB

    TileId tile1{0, 0, 0, 1.0f};
    TileId tile2{1, 1, 1, 1.0f};

    QImage image(256, 256, QImage::Format_ARGB32);

    // Generate some stats activity
    cache.put(tile1, image);
    cache.get(tile1); // Hit
    cache.get(tile2); // Miss

    // Verify stats are non-zero before reset
    CacheStats statsBefore = cache.stats();
    EXPECT_GT(statsBefore.putCount, 0u);
    EXPECT_GT(statsBefore.hitCount, 0u);
    EXPECT_GT(statsBefore.missCount, 0u);

    // Act - Reset stats
    cache.resetStats();

    CacheStats statsAfter = cache.stats();

    // Assert - Counters should be zero after reset
    EXPECT_EQ(0u, statsAfter.hitCount);
    EXPECT_EQ(0u, statsAfter.missCount);
    EXPECT_EQ(0u, statsAfter.evictionCount);
    EXPECT_EQ(0u, statsAfter.putCount);

    // Note: currentMemoryBytes and tileCount may not reset since tiles are still in cache
    // Only the operation counters (hit/miss/eviction/put) reset
}

TEST(TileCache, Contains) {
    // Arrange
    TileCache cache;
    cache.setMaxMemory(10 * 1024 * 1024); // 10 MB

    TileId tileInCache{0, 5, 3, 1.0f};
    TileId tileNotInCache{1, 2, 4, 2.0f};

    QImage image(256, 256, QImage::Format_ARGB32);

    // Act - Put one tile into cache
    cache.put(tileInCache, image);

    // Assert - Check contains() for both tiles
    EXPECT_TRUE(cache.contains(tileInCache))
        << "Cache should contain the tile we just put";
    EXPECT_FALSE(cache.contains(tileNotInCache))
        << "Cache should not contain a tile we never put";
}
