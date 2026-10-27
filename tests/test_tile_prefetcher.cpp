// test_tile_prefetcher.cpp - Unit tests for TilePrefetcher
// Uses shared test_main.cpp with QApplication setup

#include <gtest/gtest.h>
#include "rendering/TilePrefetcher.h"
#include "rendering/TileCache.h"
#include "rendering/TileId.h"
#include <QImage>
#include <algorithm>

using namespace pdfe::rendering;

// ============================================================================
// TilePrefetcher Construction and Configuration Tests
// ============================================================================

TEST(TilePrefetcher, Construct_DefaultRingSize) {
    // Arrange & Act
    TilePrefetcher prefetcher;

    // Assert - Default ring size should be 1
    EXPECT_EQ(1, prefetcher.ringSize());
}

TEST(TilePrefetcher, SetRingSize_Clamp) {
    // Arrange
    TilePrefetcher prefetcher;

    // Act & Assert - Test clamping to valid range [1, 3]

    // Set to 0 should clamp to 1
    prefetcher.setRingSize(0);
    EXPECT_EQ(1, prefetcher.ringSize())
        << "Ring size 0 should be clamped to minimum of 1";

    // Set to negative should clamp to 1
    prefetcher.setRingSize(-5);
    EXPECT_EQ(1, prefetcher.ringSize())
        << "Negative ring size should be clamped to minimum of 1";

    // Set to 5 should clamp to 3
    prefetcher.setRingSize(5);
    EXPECT_EQ(3, prefetcher.ringSize())
        << "Ring size 5 should be clamped to maximum of 3";

    // Set to 2 should remain 2 (valid range)
    prefetcher.setRingSize(2);
    EXPECT_EQ(2, prefetcher.ringSize())
        << "Ring size 2 is in valid range and should not be clamped";
}

// ============================================================================
// TilePrefetcher Computation Tests
// ============================================================================

TEST(TilePrefetcher, ComputePrefetch_SingleTile) {
    // Arrange
    TilePrefetcher prefetcher;
    prefetcher.setRingSize(1);

    TileId visibleTile{0, 1, 1, 1.0f}; // page=0, col=1, row=1, zoom=1.0
    std::vector<TileId> visibleTiles = {visibleTile};
    int pageCount = 5;

    // Act - Compute prefetch tiles around the visible tile
    auto prefetchTiles = prefetcher.computePrefetchTiles(visibleTiles, pageCount);

    // Assert - Result should be non-empty
    EXPECT_FALSE(prefetchTiles.empty())
        << "Prefetch should generate neighboring tiles for ring size 1";

    // The visible tile itself should NOT be in the prefetch list
    auto foundVisible = std::find_if(prefetchTiles.begin(), prefetchTiles.end(),
        [&visibleTile](const TileId& tile) {
            return tile.pageIndex == visibleTile.pageIndex &&
                   tile.col == visibleTile.col &&
                   tile.row == visibleTile.row &&
                   std::abs(tile.zoom - visibleTile.zoom) < 0.001f;
        });

    EXPECT_EQ(prefetchTiles.end(), foundVisible)
        << "Visible tile should NOT be included in prefetch results";
}

TEST(TilePrefetcher, ComputePrefetch_NegativeFilter) {
    // Arrange
    TilePrefetcher prefetcher;
    prefetcher.setRingSize(1);

    TileId visibleTile{0, 0, 0, 1.0f}; // At origin (0,0) - neighbors could be negative
    std::vector<TileId> visibleTiles = {visibleTile};
    int pageCount = 5;

    // Act - Compute prefetch tiles
    auto prefetchTiles = prefetcher.computePrefetchTiles(visibleTiles, pageCount);

    // Assert - No tiles should have negative col or row
    for (const auto& tile : prefetchTiles) {
        EXPECT_GE(tile.col, 0)
            << "Prefetch tile col should not be negative (got col=" << tile.col << ")";
        EXPECT_GE(tile.row, 0)
            << "Prefetch tile row should not be negative (got row=" << tile.row << ")";
    }
}

TEST(TilePrefetcher, ComputePrefetch_PageBoundary) {
    // Arrange
    TilePrefetcher prefetcher;
    prefetcher.setRingSize(1);

    TileId visibleTile{0, 1, 1, 1.0f}; // page=0
    std::vector<TileId> visibleTiles = {visibleTile};
    int pageCount = 1; // Only page 0 exists

    // Act - Compute prefetch tiles
    auto prefetchTiles = prefetcher.computePrefetchTiles(visibleTiles, pageCount);

    // Assert - All tiles should be within page boundaries [0, pageCount)
    for (const auto& tile : prefetchTiles) {
        EXPECT_GE(tile.pageIndex, 0)
            << "Prefetch tile should not be on page -1 (got page=" << tile.pageIndex << ")";
        EXPECT_LT(tile.pageIndex, pageCount)
            << "Prefetch tile should not exceed page count (got page=" << tile.pageIndex
            << ", pageCount=" << pageCount << ")";
    }
}

TEST(TilePrefetcher, ComputePrefetch_SkipsCached) {
    // Arrange
    TileCache cache;
    cache.setMaxMemory(10 * 1024 * 1024); // 10 MB

    TilePrefetcher prefetcher(&cache);
    prefetcher.setRingSize(1);

    TileId visibleTile{0, 5, 5, 1.0f};
    TileId neighborTile{0, 6, 5, 1.0f}; // Right neighbor

    // Put the neighbor tile in cache
    QImage image(256, 256, QImage::Format_ARGB32);
    cache.put(neighborTile, image);

    std::vector<TileId> visibleTiles = {visibleTile};
    int pageCount = 5;

    // Act - Compute prefetch tiles
    auto prefetchTiles = prefetcher.computePrefetchTiles(visibleTiles, pageCount);

    // Assert - The cached neighbor should NOT be in prefetch results
    auto foundCached = std::find_if(prefetchTiles.begin(), prefetchTiles.end(),
        [&neighborTile](const TileId& tile) {
            return tile.pageIndex == neighborTile.pageIndex &&
                   tile.col == neighborTile.col &&
                   tile.row == neighborTile.row &&
                   std::abs(tile.zoom - neighborTile.zoom) < 0.001f;
        });

    EXPECT_EQ(prefetchTiles.end(), foundCached)
        << "Cached neighbor tile should be filtered out from prefetch list";
}

TEST(TilePrefetcher, PrefetchAround_StoresResult) {
    // Arrange
    TilePrefetcher prefetcher;
    prefetcher.setRingSize(1);

    TileId visibleTile{0, 2, 3, 1.0f};
    std::vector<TileId> visibleTiles = {visibleTile};
    int pageCount = 5;

    // Act - Call prefetchAround which should compute and store results
    prefetcher.prefetchAround(visibleTiles, pageCount);

    // Get the stored result
    const auto& lastPrefetched = prefetcher.lastPrefetchedTiles();

    // Assert - lastPrefetchedTiles should match what computePrefetchTiles returns
    auto expectedTiles = prefetcher.computePrefetchTiles(visibleTiles, pageCount);

    EXPECT_EQ(expectedTiles.size(), lastPrefetched.size())
        << "lastPrefetchedTiles should have same size as computed tiles";

    // Verify each expected tile is in lastPrefetched
    for (const auto& expectedTile : expectedTiles) {
        auto found = std::find_if(lastPrefetched.begin(), lastPrefetched.end(),
            [&expectedTile](const TileId& tile) {
                return tile.pageIndex == expectedTile.pageIndex &&
                       tile.col == expectedTile.col &&
                       tile.row == expectedTile.row &&
                       std::abs(tile.zoom - expectedTile.zoom) < 0.001f;
            });

        EXPECT_NE(lastPrefetched.end(), found)
            << "Expected tile (page=" << expectedTile.pageIndex
            << ", col=" << expectedTile.col
            << ", row=" << expectedTile.row << ") not found in lastPrefetchedTiles";
    }
}

TEST(TilePrefetcher, ComputePrefetch_NoCache) {
    // Arrange - Create prefetcher with nullptr cache
    TilePrefetcher prefetcher(nullptr);
    prefetcher.setRingSize(1);

    TileId visibleTile{0, 3, 4, 1.0f};
    std::vector<TileId> visibleTiles = {visibleTile};
    int pageCount = 5;

    // Act - Should work even without cache (just doesn't filter cached tiles)
    auto prefetchTiles = prefetcher.computePrefetchTiles(visibleTiles, pageCount);

    // Assert - Should still return prefetch tiles
    EXPECT_FALSE(prefetchTiles.empty())
        << "Prefetcher should work without cache, just not filtering cached tiles";

    // Verify tiles are valid (non-negative, within bounds)
    for (const auto& tile : prefetchTiles) {
        EXPECT_GE(tile.col, 0);
        EXPECT_GE(tile.row, 0);
        EXPECT_GE(tile.pageIndex, 0);
        EXPECT_LT(tile.pageIndex, pageCount);
    }
}
