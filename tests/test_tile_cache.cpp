#include <gtest/gtest.h>
#include "rendering/TileCache.h"
#include "rendering/TileId.h"
#include <QImage>

using namespace pdfe::rendering;

class TileCacheTest : public ::testing::Test {
protected:
    QImage createTestImage(int width = 512, int height = 512) {
        QImage image(width, height, QImage::Format_ARGB32);
        image.fill(Qt::white);
        return image;
    }

    TileId createTileId(int pageIndex = 0, int col = 0, int row = 0, float zoom = 1.0f) {
        TileId id;
        id.pageIndex = pageIndex;
        id.col = col;
        id.row = row;
        id.zoom = zoom;
        return id;
    }
};

TEST_F(TileCacheTest, InitialState) {
    TileCache cache;

    EXPECT_EQ(cache.tileCount(), 0);
    EXPECT_EQ(cache.memoryUsage(), 0);
}

TEST_F(TileCacheTest, PutAndGet) {
    TileCache cache;
    TileId id = createTileId(0, 0, 0);
    QImage image = createTestImage();

    cache.put(id, image);

    EXPECT_EQ(cache.tileCount(), 1);
    EXPECT_GT(cache.memoryUsage(), 0);

    auto tile = cache.get(id);
    ASSERT_TRUE(tile.has_value());
    EXPECT_EQ(tile->id, id);
    EXPECT_EQ(tile->state, TileState::Ready);
    EXPECT_FALSE(tile->image.isNull());
}

TEST_F(TileCacheTest, GetNonExistent) {
    TileCache cache;
    TileId id = createTileId(0, 0, 0);

    auto tile = cache.get(id);
    EXPECT_FALSE(tile.has_value());
}

TEST_F(TileCacheTest, UpdateExisting) {
    TileCache cache;
    TileId id = createTileId(0, 0, 0);
    QImage image1 = createTestImage(512, 512);
    QImage image2 = createTestImage(256, 256);

    cache.put(id, image1);
    size_t memory1 = cache.memoryUsage();

    cache.put(id, image2);
    size_t memory2 = cache.memoryUsage();

    EXPECT_EQ(cache.tileCount(), 1);
    EXPECT_NE(memory1, memory2);

    auto tile = cache.get(id);
    ASSERT_TRUE(tile.has_value());
    EXPECT_EQ(tile->image.width(), 256);
    EXPECT_EQ(tile->image.height(), 256);
}

TEST_F(TileCacheTest, LRUEviction) {
    // Create cache with small memory limit
    size_t tileMemory = createTestImage().sizeInBytes();
    TileCache cache(tileMemory * 3); // Only hold 3 tiles

    // Add 5 tiles
    for (int i = 0; i < 5; ++i) {
        TileId id = createTileId(0, i, 0);
        cache.put(id, createTestImage());
    }

    // Should have evicted to stay under memory limit
    EXPECT_LE(cache.tileCount(), 3);
    EXPECT_LE(cache.memoryUsage(), tileMemory * 3);

    // First two tiles should be evicted (LRU)
    EXPECT_FALSE(cache.get(createTileId(0, 0, 0)).has_value());
    EXPECT_FALSE(cache.get(createTileId(0, 1, 0)).has_value());

    // Last three should still be present
    EXPECT_TRUE(cache.get(createTileId(0, 2, 0)).has_value());
    EXPECT_TRUE(cache.get(createTileId(0, 3, 0)).has_value());
    EXPECT_TRUE(cache.get(createTileId(0, 4, 0)).has_value());
}

TEST_F(TileCacheTest, LRUOrdering) {
    size_t tileMemory = createTestImage().sizeInBytes();
    TileCache cache(tileMemory * 3);

    // Add 3 tiles
    TileId id0 = createTileId(0, 0, 0);
    TileId id1 = createTileId(0, 1, 0);
    TileId id2 = createTileId(0, 2, 0);

    cache.put(id0, createTestImage());
    cache.put(id1, createTestImage());
    cache.put(id2, createTestImage());

    EXPECT_EQ(cache.tileCount(), 3);

    // Access id0 to make it most recently used
    cache.get(id0);

    // Add a new tile, should evict id1 (least recently used)
    TileId id3 = createTileId(0, 3, 0);
    cache.put(id3, createTestImage());

    EXPECT_TRUE(cache.get(id0).has_value());   // Accessed, should remain
    EXPECT_FALSE(cache.get(id1).has_value());  // Evicted (LRU)
    EXPECT_TRUE(cache.get(id2).has_value());   // Should remain
    EXPECT_TRUE(cache.get(id3).has_value());   // Just added
}

TEST_F(TileCacheTest, InvalidatePage) {
    TileCache cache;

    // Add tiles for multiple pages
    cache.put(createTileId(0, 0, 0), createTestImage());
    cache.put(createTileId(0, 1, 0), createTestImage());
    cache.put(createTileId(1, 0, 0), createTestImage());
    cache.put(createTileId(1, 1, 0), createTestImage());
    cache.put(createTileId(2, 0, 0), createTestImage());

    EXPECT_EQ(cache.tileCount(), 5);

    // Invalidate page 1
    cache.invalidate(1);

    EXPECT_EQ(cache.tileCount(), 3);
    EXPECT_TRUE(cache.get(createTileId(0, 0, 0)).has_value());
    EXPECT_TRUE(cache.get(createTileId(0, 1, 0)).has_value());
    EXPECT_FALSE(cache.get(createTileId(1, 0, 0)).has_value());
    EXPECT_FALSE(cache.get(createTileId(1, 1, 0)).has_value());
    EXPECT_TRUE(cache.get(createTileId(2, 0, 0)).has_value());
}

TEST_F(TileCacheTest, InvalidateAll) {
    TileCache cache;

    cache.put(createTileId(0, 0, 0), createTestImage());
    cache.put(createTileId(1, 0, 0), createTestImage());
    cache.put(createTileId(2, 0, 0), createTestImage());

    EXPECT_EQ(cache.tileCount(), 3);

    cache.invalidateAll();

    EXPECT_EQ(cache.tileCount(), 0);
    EXPECT_EQ(cache.memoryUsage(), 0);
}

TEST_F(TileCacheTest, Clear) {
    TileCache cache;

    cache.put(createTileId(0, 0, 0), createTestImage());
    cache.put(createTileId(1, 0, 0), createTestImage());

    cache.clear();

    EXPECT_EQ(cache.tileCount(), 0);
    EXPECT_EQ(cache.memoryUsage(), 0);
}

TEST_F(TileCacheTest, MemoryTracking) {
    TileCache cache;

    QImage image = createTestImage();
    size_t expectedMemory = image.sizeInBytes();

    cache.put(createTileId(0, 0, 0), image);

    EXPECT_EQ(cache.memoryUsage(), expectedMemory);

    cache.put(createTileId(0, 1, 0), image);

    EXPECT_EQ(cache.memoryUsage(), expectedMemory * 2);

    cache.invalidate(0);

    EXPECT_EQ(cache.memoryUsage(), 0);
}

TEST_F(TileCacheTest, SetMaxMemory) {
    size_t tileMemory = createTestImage().sizeInBytes();
    TileCache cache(tileMemory * 5);

    // Add 5 tiles
    for (int i = 0; i < 5; ++i) {
        cache.put(createTileId(0, i, 0), createTestImage());
    }

    EXPECT_EQ(cache.tileCount(), 5);

    // Reduce max memory to hold only 2 tiles
    cache.setMaxMemory(tileMemory * 2);

    EXPECT_LE(cache.tileCount(), 2);
    EXPECT_LE(cache.memoryUsage(), tileMemory * 2);
}

TEST_F(TileCacheTest, DifferentZoomLevels) {
    TileCache cache;

    TileId id1 = createTileId(0, 0, 0, 1.0f);
    TileId id2 = createTileId(0, 0, 0, 2.0f);

    cache.put(id1, createTestImage());
    cache.put(id2, createTestImage());

    EXPECT_EQ(cache.tileCount(), 2);
    EXPECT_TRUE(cache.get(id1).has_value());
    EXPECT_TRUE(cache.get(id2).has_value());
}

TEST_F(TileCacheTest, TileIdEquality) {
    TileId id1 = createTileId(0, 0, 0, 1.0f);
    TileId id2 = createTileId(0, 0, 0, 1.0f);
    TileId id3 = createTileId(0, 1, 0, 1.0f);

    EXPECT_EQ(id1, id2);
    EXPECT_NE(id1, id3);
}

TEST_F(TileCacheTest, TileIdHash) {
    std::hash<TileId> hasher;

    TileId id1 = createTileId(0, 0, 0, 1.0f);
    TileId id2 = createTileId(0, 0, 0, 1.0f);
    TileId id3 = createTileId(0, 1, 0, 1.0f);

    EXPECT_EQ(hasher(id1), hasher(id2));
    EXPECT_NE(hasher(id1), hasher(id3));
}
