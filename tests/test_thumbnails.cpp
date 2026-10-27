#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "rendering/ThumbnailRenderer.h"

#include <QApplication>
#include <QEventLoop>
#include <QTimer>
#include <filesystem>
#include <memory>

namespace {
std::filesystem::path getFixturePath(const std::string& filename) {
    std::filesystem::path fixturesDir(TEST_FIXTURES_DIR);
    return fixturesDir / filename;
}

bool fixtureExists(const std::string& filename) {
    return std::filesystem::exists(getFixturePath(filename));
}
} // anonymous namespace

class ThumbnailTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<pdfe::core::MuPdfContext>();
    }

    void TearDown() override {
        context.reset();
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context;
};

TEST_F(ThumbnailTest, RenderThumbnail) {
    if (!fixtureExists("test_3page.pdf")) {
        GTEST_SKIP() << "test_3page.pdf fixture not available";
    }

    auto doc = pdfe::core::Document::open(context, getFixturePath("test_3page.pdf"));
    ASSERT_NE(doc, nullptr);

    pdfe::rendering::ThumbnailRenderer renderer(doc.get());

    bool callbackCalled = false;
    QImage resultImage;

    renderer.requestThumbnail(0, QSize(128, 128), [&](int pageIndex, QImage image) {
        EXPECT_EQ(pageIndex, 0);
        EXPECT_FALSE(image.isNull());
        callbackCalled = true;
        resultImage = image;
    });

    // Wait for thumbnail to be rendered
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&renderer, &pdfe::rendering::ThumbnailRenderer::thumbnailReady,
                     &loop, &QEventLoop::quit);
    timer.start(5000); // 5 second timeout
    loop.exec();

    EXPECT_TRUE(callbackCalled);
    EXPECT_FALSE(resultImage.isNull());
}

TEST_F(ThumbnailTest, ThumbnailSize) {
    if (!fixtureExists("test_single.pdf")) {
        GTEST_SKIP() << "test_single.pdf fixture not available";
    }

    auto doc = pdfe::core::Document::open(context, getFixturePath("test_single.pdf"));
    ASSERT_NE(doc, nullptr);

    pdfe::rendering::ThumbnailRenderer renderer(doc.get());

    QSize requestedSize(100, 100);
    QImage resultImage;

    renderer.requestThumbnail(0, requestedSize, [&](int pageIndex, QImage image) {
        resultImage = image;
    });

    // Wait for thumbnail
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&renderer, &pdfe::rendering::ThumbnailRenderer::thumbnailReady,
                     &loop, &QEventLoop::quit);
    timer.start(5000);
    loop.exec();

    EXPECT_FALSE(resultImage.isNull());

    // Verify thumbnail fits within requested size (aspect ratio preserved)
    EXPECT_LE(resultImage.width(), requestedSize.width());
    EXPECT_LE(resultImage.height(), requestedSize.height());

    // At least one dimension should match requested size
    bool widthMatches = (resultImage.width() == requestedSize.width());
    bool heightMatches = (resultImage.height() == requestedSize.height());
    EXPECT_TRUE(widthMatches || heightMatches);
}

TEST_F(ThumbnailTest, AsyncRendering) {
    if (!fixtureExists("test_3page.pdf")) {
        GTEST_SKIP() << "test_3page.pdf fixture not available";
    }

    auto doc = pdfe::core::Document::open(context, getFixturePath("test_3page.pdf"));
    ASSERT_NE(doc, nullptr);

    pdfe::rendering::ThumbnailRenderer renderer(doc.get());

    int callbackCount = 0;

    // Request multiple thumbnails
    for (int i = 0; i < 3; ++i) {
        renderer.requestThumbnail(i, QSize(64, 64), [&](int pageIndex, QImage image) {
            EXPECT_GE(pageIndex, 0);
            EXPECT_LT(pageIndex, 3);
            EXPECT_FALSE(image.isNull());
            callbackCount++;
        });
    }

    // Wait for all thumbnails
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    QTimer checkTimer;
    QObject::connect(&checkTimer, &QTimer::timeout, [&]() {
        if (callbackCount >= 3) {
            loop.quit();
        }
    });
    checkTimer.start(100);

    timer.start(10000); // 10 second timeout for all 3 thumbnails
    loop.exec();

    EXPECT_EQ(callbackCount, 3);
}

TEST_F(ThumbnailTest, CancelRequest) {
    if (!fixtureExists("test_3page.pdf")) {
        GTEST_SKIP() << "test_3page.pdf fixture not available";
    }

    auto doc = pdfe::core::Document::open(context, getFixturePath("test_3page.pdf"));
    ASSERT_NE(doc, nullptr);

    pdfe::rendering::ThumbnailRenderer renderer(doc.get());

    bool callbackCalled = false;

    renderer.requestThumbnail(1, QSize(128, 128), [&](int, QImage) {
        callbackCalled = true;
    });

    // Cancel immediately
    renderer.cancelPage(1);

    // Wait a bit to ensure no callback
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(1000); // 1 second
    loop.exec();

    // Callback should not have been called
    EXPECT_FALSE(callbackCalled);
}

TEST_F(ThumbnailTest, CancelAll) {
    if (!fixtureExists("test_3page.pdf")) {
        GTEST_SKIP() << "test_3page.pdf fixture not available";
    }

    auto doc = pdfe::core::Document::open(context, getFixturePath("test_3page.pdf"));
    ASSERT_NE(doc, nullptr);

    pdfe::rendering::ThumbnailRenderer renderer(doc.get());

    int callbackCount = 0;

    // Request multiple thumbnails
    for (int i = 0; i < 3; ++i) {
        renderer.requestThumbnail(i, QSize(64, 64), [&](int, QImage) {
            callbackCount++;
        });
    }

    // Cancel all immediately
    renderer.cancelAll();

    // Wait a bit to ensure no callbacks
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(1000);
    loop.exec();

    // No callbacks should have been called
    EXPECT_EQ(callbackCount, 0);
}
