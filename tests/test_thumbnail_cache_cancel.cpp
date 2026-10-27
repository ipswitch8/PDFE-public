// R2-08 oracle: ThumbnailRenderer must (1) serve repeated (page,size) requests
// from a result cache instead of re-rasterizing the full page every time, and
// (2) honor cancellation so cancelPage/cancelAll actually prevent not-yet-run
// render work — not merely erase a bookkeeping map after the work already ran.
//
// Both oracles are state oracles over the static render-count instrumentation
// (ThumbnailRenderer::renderCount()), which counts ONLY real full-page
// rasterizations. A cache hit or a cancelled task must not increment it.

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

// Spin the Qt event loop until `predicate()` is true or `timeoutMs` elapses.
// Deterministic delivery template mirrors tests/test_thumbnails.cpp.
template <typename Pred>
void pumpUntil(Pred predicate, int timeoutMs) {
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);

    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, [&]() {
        if (predicate()) loop.quit();
    });
    poll.start(10);
    deadline.start(timeoutMs);
    if (!predicate()) loop.exec();
}

// Pump the event loop for a fixed duration so any queued render work has a
// chance to run (used to PROVE cancelled work does not run).
void pumpFor(int ms) {
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(ms);
    loop.exec();
}
} // anonymous namespace

class ThumbnailCacheCancelTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<pdfe::core::MuPdfContext>();
        pdfe::rendering::ThumbnailRenderer::resetRenderCount();
    }

    void TearDown() override {
        context.reset();
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context;
};

// Oracle 1 — fast-scroll no-re-raster: requesting the SAME (page,size) twice
// rasterizes exactly once; the second request is a cache hit.
TEST_F(ThumbnailCacheCancelTest, RepeatedRequestServedFromCache) {
    if (!fixtureExists("test_3page.pdf")) {
        GTEST_SKIP() << "test_3page.pdf fixture not available";
    }

    auto doc = pdfe::core::Document::open(context, getFixturePath("test_3page.pdf"));
    ASSERT_NE(doc, nullptr);

    pdfe::rendering::ThumbnailRenderer renderer(doc.get());
    const QSize size(128, 128);

    // First request: must rasterize once and deliver.
    int deliveries = 0;
    QImage first;
    renderer.requestThumbnail(0, size, [&](int, QImage img) {
        ++deliveries;
        first = img;
    });
    pumpUntil([&] { return deliveries >= 1; }, 5000);

    ASSERT_EQ(deliveries, 1);
    ASSERT_FALSE(first.isNull());
    const std::size_t afterFirst = pdfe::rendering::ThumbnailRenderer::renderCount();
    EXPECT_EQ(afterFirst, 1u) << "first request must rasterize exactly once";

    // Second identical request: must be served from the cache WITHOUT a new
    // rasterization. renderCount() must NOT advance.
    QImage second;
    renderer.requestThumbnail(0, size, [&](int, QImage img) {
        ++deliveries;
        second = img;
    });
    pumpUntil([&] { return deliveries >= 2; }, 5000);

    EXPECT_EQ(deliveries, 2) << "cache hit must still deliver an image";
    EXPECT_FALSE(second.isNull());
    EXPECT_EQ(pdfe::rendering::ThumbnailRenderer::renderCount(), afterFirst)
        << "second identical request re-rasterized -> no result cache (R2-08)";
    EXPECT_EQ(pdfe::rendering::ThumbnailRenderer::renderCount(), 1u);
}

// Oracle 2 — cancellation stops work: a batch cancelled before it is processed
// must never reach the render path (renderCount stays 0) and must not deliver.
TEST_F(ThumbnailCacheCancelTest, CancelAllPreventsRenderWork) {
    if (!fixtureExists("test_3page.pdf")) {
        GTEST_SKIP() << "test_3page.pdf fixture not available";
    }

    auto doc = pdfe::core::Document::open(context, getFixturePath("test_3page.pdf"));
    ASSERT_NE(doc, nullptr);

    pdfe::rendering::ThumbnailRenderer renderer(doc.get());

    int deliveries = 0;
    for (int i = 0; i < 3; ++i) {
        renderer.requestThumbnail(i, QSize(64, 64), [&](int, QImage) {
            ++deliveries;
        });
    }

    // Cancel the whole batch before the event loop has processed any of it.
    renderer.cancelAll();

    // Give any (wrongly) scheduled work ample time to run.
    pumpFor(1000);

    EXPECT_EQ(pdfe::rendering::ThumbnailRenderer::renderCount(), 0u)
        << "cancelAll() did not stop render work -> cancellation is a no-op (R2-08)";
    EXPECT_EQ(deliveries, 0) << "cancelled thumbnails must not be delivered";
}

// Oracle 2b — cancelPage stops a single page's not-yet-run work.
TEST_F(ThumbnailCacheCancelTest, CancelPagePreventsRenderWork) {
    if (!fixtureExists("test_3page.pdf")) {
        GTEST_SKIP() << "test_3page.pdf fixture not available";
    }

    auto doc = pdfe::core::Document::open(context, getFixturePath("test_3page.pdf"));
    ASSERT_NE(doc, nullptr);

    pdfe::rendering::ThumbnailRenderer renderer(doc.get());

    int deliveries = 0;
    renderer.requestThumbnail(2, QSize(64, 64), [&](int, QImage) {
        ++deliveries;
    });
    renderer.cancelPage(2);

    pumpFor(1000);

    EXPECT_EQ(pdfe::rendering::ThumbnailRenderer::renderCount(), 0u)
        << "cancelPage() did not stop render work -> cancellation is a no-op (R2-08)";
    EXPECT_EQ(deliveries, 0) << "cancelled page must not be delivered";
}
