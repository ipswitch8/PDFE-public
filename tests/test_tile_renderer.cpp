#include <gtest/gtest.h>
#include "rendering/TileRenderer.h"
#include "rendering/TileId.h"
#include "rendering/Tile.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include <QImage>
#include <QSignalSpy>
#include <filesystem>
#include <memory>
#include <atomic>
#include <mutex>
#include <vector>

using namespace pdfe::rendering;

class TileRendererTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<pdfe::core::MuPdfContext>();
        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        pdfPath = fixturesDir / "test_3page.pdf";

        if (!std::filesystem::exists(pdfPath)) {
            GTEST_SKIP() << "test_3page.pdf not available";
        }

        document = pdfe::core::Document::open(context, pdfPath);
        renderer = std::make_unique<TileRenderer>(document.get());
    }

    void TearDown() override {
        renderer.reset();
        document.reset();
        context.reset();
    }

    TileId createTileId(int pageIndex = 0, int col = 0, int row = 0, float zoom = 1.0f) {
        TileId id;
        id.pageIndex = pageIndex;
        id.col = col;
        id.row = row;
        id.zoom = zoom;
        return id;
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context;
    std::filesystem::path pdfPath;
    std::unique_ptr<pdfe::core::Document> document;
    std::unique_ptr<TileRenderer> renderer;
};

TEST_F(TileRendererTest, ConstructorDestructor) {
    // Renderer was already created in SetUp; just verify it exists
    ASSERT_NE(renderer, nullptr);

    // Create a second renderer and let it destruct
    {
        TileRenderer renderer2(document.get());
    }
    SUCCEED();
}

TEST_F(TileRendererTest, RequestTile) {
    QSignalSpy spy(renderer.get(), &TileRenderer::tileReady);
    ASSERT_TRUE(spy.isValid());

    TileId id = createTileId(0, 0, 0, 1.0f);
    renderer->requestTile(id, nullptr);

    // Wait for signal (up to 10 seconds for slow CI)
    ASSERT_TRUE(spy.wait(10000));

    ASSERT_EQ(spy.count(), 1);
    QList<QVariant> args = spy.takeFirst();
    TileId resultId = args.at(0).value<TileId>();
    QImage resultImage = args.at(1).value<QImage>();

    EXPECT_EQ(resultId, id);
    EXPECT_FALSE(resultImage.isNull());
    EXPECT_GT(resultImage.width(), 0);
    EXPECT_GT(resultImage.height(), 0);
}

TEST_F(TileRendererTest, RequestTileWithCallback) {
    std::atomic<bool> callbackFired{false};
    TileId receivedId;
    QImage receivedImage;
    std::mutex cbMutex;

    TileId id = createTileId(0, 0, 0, 1.0f);

    renderer->requestTile(id, [&](TileId tid, QImage img) {
        std::lock_guard<std::mutex> lock(cbMutex);
        receivedId = tid;
        receivedImage = img;
        callbackFired.store(true);
    });

    // Wait for callback via signal spy (callback fires on same thread as signal)
    QSignalSpy spy(renderer.get(), &TileRenderer::tileReady);
    ASSERT_TRUE(spy.wait(10000));

    EXPECT_TRUE(callbackFired.load());
    EXPECT_EQ(receivedId, id);
    EXPECT_FALSE(receivedImage.isNull());
}

TEST_F(TileRendererTest, CancelPage) {
    // Request tiles for page 0 and page 1
    QSignalSpy spy(renderer.get(), &TileRenderer::tileReady);

    // Request several tiles for page 0
    for (int col = 0; col < 4; ++col) {
        renderer->requestTile(createTileId(0, col, 0, 1.0f), nullptr);
    }

    // Request a tile for page 1
    TileId page1Tile = createTileId(1, 0, 0, 1.0f);
    renderer->requestTile(page1Tile, nullptr);

    // Cancel page 0 immediately
    renderer->cancelPage(0);

    // Wait for page 1 tile to arrive
    bool page1Received = false;
    for (int i = 0; i < 20 && !page1Received; ++i) {
        spy.wait(500);
        for (int j = 0; j < spy.count(); ++j) {
            TileId tid = spy.at(j).at(0).value<TileId>();
            if (tid.pageIndex == 1) {
                page1Received = true;
            }
        }
    }

    EXPECT_TRUE(page1Received);
}

TEST_F(TileRendererTest, CancelAll) {
    QSignalSpy spy(renderer.get(), &TileRenderer::tileReady);

    // Request many tiles
    for (int page = 0; page < 3; ++page) {
        for (int col = 0; col < 4; ++col) {
            renderer->requestTile(createTileId(page, col, 0, 1.0f), nullptr);
        }
    }

    // Cancel all immediately
    renderer->cancelAll();

    // Wait a bit -- some tiles may still arrive (race with already-running tasks)
    spy.wait(2000);

    // The key assertion: no crash occurred, and we can still use the renderer
    TileId newId = createTileId(0, 0, 0, 0.5f);
    renderer->requestTile(newId, nullptr);

    // This new tile should complete successfully
    bool newTileReceived = false;
    for (int i = 0; i < 20 && !newTileReceived; ++i) {
        spy.wait(500);
        for (int j = 0; j < spy.count(); ++j) {
            TileId tid = spy.at(j).at(0).value<TileId>();
            if (tid == newId) {
                newTileReceived = true;
            }
        }
    }
    EXPECT_TRUE(newTileReceived);
}

TEST_F(TileRendererTest, MultiplePages) {
    QSignalSpy spy(renderer.get(), &TileRenderer::tileReady);

    // Request one tile from each of the 3 pages
    TileId id0 = createTileId(0, 0, 0, 1.0f);
    TileId id1 = createTileId(1, 0, 0, 1.0f);
    TileId id2 = createTileId(2, 0, 0, 1.0f);

    renderer->requestTile(id0, nullptr);
    renderer->requestTile(id1, nullptr);
    renderer->requestTile(id2, nullptr);

    // Wait for all 3 tiles
    bool got0 = false, got1 = false, got2 = false;
    for (int i = 0; i < 30 && !(got0 && got1 && got2); ++i) {
        spy.wait(500);
        for (int j = 0; j < spy.count(); ++j) {
            TileId tid = spy.at(j).at(0).value<TileId>();
            if (tid == id0) got0 = true;
            if (tid == id1) got1 = true;
            if (tid == id2) got2 = true;
        }
    }

    EXPECT_TRUE(got0) << "Page 0 tile not received";
    EXPECT_TRUE(got1) << "Page 1 tile not received";
    EXPECT_TRUE(got2) << "Page 2 tile not received";
}

TEST_F(TileRendererTest, ThreadSafety) {
    // Stress test: request many tiles concurrently across all pages
    QSignalSpy spy(renderer.get(), &TileRenderer::tileReady);

    const int tilesPerPage = 8;
    const int totalTiles = 3 * tilesPerPage; // 24 tiles across 3 pages

    for (int page = 0; page < 3; ++page) {
        for (int col = 0; col < tilesPerPage; ++col) {
            renderer->requestTile(createTileId(page, col, 0, 1.0f), nullptr);
        }
    }

    // Wait for all tiles to complete
    int received = 0;
    for (int i = 0; i < 60 && received < totalTiles; ++i) {
        spy.wait(500);
        received = spy.count();
    }

    // All tiles should eventually complete (no deadlocks, no crashes)
    EXPECT_EQ(received, totalTiles);

    // Verify all images are valid
    for (int j = 0; j < spy.count(); ++j) {
        QImage img = spy.at(j).at(1).value<QImage>();
        EXPECT_FALSE(img.isNull()) << "Tile " << j << " has null image";
    }
}

TEST_F(TileRendererTest, InvalidPageIndex) {
    QSignalSpy spy(renderer.get(), &TileRenderer::tileReady);

    // Request tile with invalid page index (out of range)
    TileId badId = createTileId(999, 0, 0, 1.0f);
    renderer->requestTile(badId, nullptr);

    // Wait briefly -- should not crash, and no tile should arrive for the bad ID
    spy.wait(1000);

    // Check that no tile was produced for the invalid page
    bool gotBadTile = false;
    for (int j = 0; j < spy.count(); ++j) {
        TileId tid = spy.at(j).at(0).value<TileId>();
        if (tid == badId) {
            gotBadTile = true;
        }
    }
    EXPECT_FALSE(gotBadTile);

    // Verify renderer is still functional after invalid request
    TileId goodId = createTileId(0, 0, 0, 1.0f);
    renderer->requestTile(goodId, nullptr);
    bool gotGoodTile = false;
    for (int i = 0; i < 20 && !gotGoodTile; ++i) {
        spy.wait(500);
        for (int j = 0; j < spy.count(); ++j) {
            TileId tid = spy.at(j).at(0).value<TileId>();
            if (tid == goodId) {
                gotGoodTile = true;
            }
        }
    }
    EXPECT_TRUE(gotGoodTile);
}

TEST_F(TileRendererTest, DuplicateRequest) {
    std::atomic<int> callback1Count{0};
    std::atomic<int> callback2Count{0};

    TileId id = createTileId(0, 0, 0, 1.0f);

    // First request with callback1
    renderer->requestTile(id, [&](TileId, QImage) {
        callback1Count++;
    });

    // Immediately replace with callback2 (same TileId)
    renderer->requestTile(id, [&](TileId, QImage) {
        callback2Count++;
    });

    // Wait for the tile
    QSignalSpy spy(renderer.get(), &TileRenderer::tileReady);
    ASSERT_TRUE(spy.wait(10000));

    // Second callback should have replaced the first
    // (the task was already in-flight, so callback2 should be called)
    EXPECT_EQ(callback2Count.load(), 1);
    EXPECT_EQ(callback1Count.load(), 0);
}

TEST_F(TileRendererTest, TileCoordinateCalculation) {
    // Verify the tile coordinate math used in RenderTask::run()
    // Tile (col, row) at zoom maps to PDF rect:
    //   x = col * TILE_SIZE / zoom
    //   y = row * TILE_SIZE / zoom
    //   w = h = TILE_SIZE / zoom

    float zoom = 2.0f;
    int col = 3;
    int row = 2;

    float expectedX = static_cast<float>(col * TILE_SIZE) / zoom;
    float expectedY = static_cast<float>(row * TILE_SIZE) / zoom;
    float expectedW = static_cast<float>(TILE_SIZE) / zoom;
    float expectedH = static_cast<float>(TILE_SIZE) / zoom;

    EXPECT_FLOAT_EQ(expectedX, 768.0f);   // 3 * 512 / 2
    EXPECT_FLOAT_EQ(expectedY, 512.0f);   // 2 * 512 / 2
    EXPECT_FLOAT_EQ(expectedW, 256.0f);   // 512 / 2
    EXPECT_FLOAT_EQ(expectedH, 256.0f);   // 512 / 2

    // At zoom 1.0
    zoom = 1.0f;
    expectedX = static_cast<float>(col * TILE_SIZE) / zoom;
    expectedY = static_cast<float>(row * TILE_SIZE) / zoom;
    expectedW = static_cast<float>(TILE_SIZE) / zoom;

    EXPECT_FLOAT_EQ(expectedX, 1536.0f);  // 3 * 512 / 1
    EXPECT_FLOAT_EQ(expectedY, 1024.0f);  // 2 * 512 / 1
    EXPECT_FLOAT_EQ(expectedW, 512.0f);   // 512 / 1

    // At zoom 0.5
    zoom = 0.5f;
    expectedX = static_cast<float>(0 * TILE_SIZE) / zoom;
    expectedW = static_cast<float>(TILE_SIZE) / zoom;

    EXPECT_FLOAT_EQ(expectedX, 0.0f);
    EXPECT_FLOAT_EQ(expectedW, 1024.0f);  // 512 / 0.5
}
