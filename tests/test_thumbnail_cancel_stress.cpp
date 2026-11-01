// tests/test_thumbnail_cancel_stress.cpp
//
// Mid-render cancellation guard for ThumbnailRenderer's worker pool.
//
// WHY THIS EXISTS
// ===============
// ThumbnailRenderer was moved off the GUI thread onto a private QThreadPool with
// per-thread MuPDF context clones, deliberately mirroring TileRenderer. That
// makes it heir to the SAME bug class TileRenderer already has a named
// regression for: GitHub #287, the fitz error-stack assertion
//
//     Assertion failed: ctx->error.top == ctx->error.stack_base,
//         file source/fitz/context.c, line 218
//
// which aborts a whole shard rather than failing one test. fitz checks the
// error stack when a thread holding a cloned context exits; a private
// QThreadPool means every renderer teardown exits its workers and drops their
// clones — the moment an unbalanced frame is noticed. A C++ exception escaping
// the callable passed to MuPdfContext::execute would unwind past fz_catch and
// leave a frame pushed forever, and cancellation is cooperative, so after
// cancelPage/cancelAll returns the in-flight ThumbnailRenderTasks are still
// inside execute(...). Cancelling WHILE workers are mid-render, then destroying
// the renderer, is the narrowest expression of that race.
//
// The existing thumbnail cancel tests (test_thumbnail_cache_cancel.cpp,
// test_thumbnails.cpp) all cancel synchronously BEFORE the Qt::QueuedConnection
// dispatch lambda runs, so they only cover the "cancelled before the worker
// ever started" case. This test is the other half of the acceptance criterion
// ("safe before OR DURING its worker render"): it pumps the event loop once so
// the dispatch lambdas run and workers genuinely start, THEN cancels.
//
// HOW IT REPORTS. A leaked frame does not fail an assertion here — it aborts the
// process, exactly as in the shard. So the verdict is survival: reaching the end
// means every clone dropped balanced. The harness suppresses CRT assert dialogs
// (tests/test_main.cpp), so an abort is a non-zero exit, not a modal.
//
// COST. Default iteration count is small so this is an ordinary regression test.
// Raise it with PDFE_CANCEL_STRESS_ITERS for a hunt:
//
//   PDFE_CANCEL_STRESS_ITERS=2000 pdfe_tests.exe \
//       --gtest_filter=ThumbnailCancelStress.*

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QImage>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "rendering/ThumbnailRenderer.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <memory>

using namespace pdfe::rendering;

namespace {

int stressIterations()
{
    if (const char* s = std::getenv("PDFE_CANCEL_STRESS_ITERS")) {
        try {
            const int n = std::stoi(s);
            if (n > 0) return n;
        } catch (...) {
            // fall through to the default rather than silently running zero
        }
    }
    return 12;
}

}  // namespace

// Cancel while workers are mid-render, then tear the renderer (and its pool)
// down immediately. Reaching the end with every clone balanced is the verdict.
TEST(ThumbnailCancelStress, CancelDuringRenderLeavesEveryCloneBalanced)
{
    const std::filesystem::path pdf =
        std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
    ASSERT_TRUE(std::filesystem::exists(pdf))
        << "test_3page.pdf is required and missing: " << pdf.string();

    const int iterations = stressIterations();
    ASSERT_GT(iterations, 0) << "a zero-iteration stress run proves nothing";

    int rendererTeardowns = 0;
    int deliveriesAfterCancel = 0;

    for (int i = 0; i < iterations; ++i) {
        // A FRESH context, document, renderer AND POOL every iteration. The
        // per-iteration teardown is the point: it exits the worker threads and
        // drops their MuPDF clones, which is when fitz checks the error stack.
        auto context = std::make_shared<pdfe::core::MuPdfContext>();
        auto document = pdfe::core::Document::open(context, pdf);
        ASSERT_NE(document, nullptr) << "iteration " << i;

        {
            auto renderer = std::make_unique<ThumbnailRenderer>(document.get());

            // Request every page, several sizes each, so the pool has genuine
            // concurrent work. The delivery callback records any delivery that
            // slips through AFTER we cancel — cancelled work must not deliver.
            std::atomic<bool> cancelled{false};
            for (int pass = 0; pass < 3; ++pass) {
                const int side = 96 + pass * 16;  // distinct cache keys
                for (int page = 0; page < 3; ++page) {
                    renderer->requestThumbnail(
                        page, QSize(side, side),
                        [&cancelled, &deliveriesAfterCancel](int, QImage) {
                            if (cancelled.load()) ++deliveriesAfterCancel;
                        });
                }
            }

            // Pump ONCE so the deferred Qt::QueuedConnection dispatch lambdas
            // run: that is what builds the display lists and starts the pool
            // tasks. After this returns the workers are running on pool threads
            // and are, with high probability, INSIDE execute(...) — the
            // mid-render window this test exists to hit.
            if (QCoreApplication::instance()) {
                QCoreApplication::processEvents();
            }

            // Cancel while workers are mid-render. Cooperative: sets flags and
            // drops bookkeeping without waiting, so workers are still unwinding
            // when we tear the renderer down next.
            cancelled.store(true);
            renderer->cancelAll();

            // Destroy the renderer right away. Its ~Impl does cancelAll() +
            // pool.waitForDone(), so this is not a use-after-free -- it is the
            // narrowest window in which a worker can be unwinding as its pool is
            // torn down and its clone dropped.
            renderer.reset();
            ++rendererTeardowns;
        }

        // Deliver anything the teardown queued before the context goes away, so
        // a dropped clone is not blamed on an event still in flight.
        if (QCoreApplication::instance()) {
            QCoreApplication::processEvents();
        }

        document.reset();
        context.reset();
    }

    // Reaching here IS the verdict: an unbalanced error stack aborts the process
    // rather than failing an assertion. Assert the loop actually ran the
    // teardowns it claims, so a future edit that skipped the body cannot "pass".
    EXPECT_EQ(rendererTeardowns, iterations)
        << "the loop did not perform one renderer teardown per iteration, so "
           "the worker-thread-exit path this test exists to exercise did not run";

    // Cancellation is authoritative: no thumbnail cancelled mid-flight should be
    // delivered to the callback after cancelAll().
    EXPECT_EQ(deliveriesAfterCancel, 0)
        << "a thumbnail was delivered AFTER cancelAll() -- mid-render "
           "cancellation is not honored on the delivery path";
}
