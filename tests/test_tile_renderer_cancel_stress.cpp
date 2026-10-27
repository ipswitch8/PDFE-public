// tests/test_tile_renderer_cancel_stress.cpp
//
// GitHub #287 — the fitz error-stack assertion that killed a whole shard.
//
//   Assertion failed: ctx->error.top == ctx->error.stack_base,
//       file source/fitz/context.c, line 218
//
// WHY A STRESS TEST, AND WHY THIS SHAPE
// =====================================
// The reported failure cost 408 tests: shard 7 exited rc=3 with no XML, so
// every test assigned to it was recorded as an error. It is intermittent — the
// same tree and binary ran clean three times the same day.
//
// Two reproduction attempts failed first, and they are what determined this
// file's shape:
//
//   * 150 fresh processes of `TileRendererTest.CancelPage` alone — all clean.
//     So the trigger is not reachable from that test's own request/cancel
//     sequence in a clean process.
//   * 25 fresh processes of the whole 409-test shard that owns it — all clean.
//     So it is not simply "that test plus its neighbours" either.
//
// What both attempts under-exercise is the event the assertion actually fires
// on. fitz checks the error stack when a context is dropped **or when a thread
// holding a cloned context exits**. `TileRenderer` owns a PRIVATE
// `QThreadPool` (`new QThreadPool(this)`), so every renderer destruction tears
// down its own pool, every pool teardown exits its worker threads, and every
// worker-thread exit runs the `thread_local` destructor that drops that
// thread's MuPDF clone — which is where an unbalanced error stack would be
// noticed.
//
// A suite run performs that teardown about five times (once per
// `TileRendererTest` case). This performs it once per iteration. That is the
// whole idea: raise the rate of the CHECK, not the rate of the test.
//
// WHAT WOULD MAKE A FRAME LEAK. `MuPdfContext::execute` runs its callable
// inside `fz_try` and throws only after `fz_catch` has popped the frame, which
// is correct. But a C++ exception escaping the callable itself would unwind
// past `fz_catch` and leave the frame pushed forever — and `cancelPage` is
// cooperative, so after it returns, in-flight `RenderTask`s are still inside
// `execute(...)`. Cancelling while workers are mid-render, then immediately
// destroying the renderer, is the narrowest expression of that race.
//
// HOW IT REPORTS. A leaked frame does not fail an assertion here — it aborts
// the process, exactly as it did in the shard. So this test's verdict is its
// own survival: reaching the end means every clone dropped balanced. The
// harness suppresses CRT assert dialogs (`tests/test_main.cpp`), so an abort
// gives a non-zero exit code rather than a modal.
//
// COST. The default iteration count is deliberately small so this is an
// ordinary regression test rather than a tax on every run. Raise it with
// PDFE_CANCEL_STRESS_ITERS for a hunt:
//
//   PDFE_CANCEL_STRESS_ITERS=2000 pdfe_tests.exe \
//       --gtest_filter=TileRendererCancelStress.*

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSignalSpy>
#include <QThread>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "rendering/TileRenderer.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

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

TileId makeTileId(int pageIndex, int col, int row, float zoom)
{
    TileId id;
    id.pageIndex = pageIndex;
    id.col = col;
    id.row = row;
    id.zoom = zoom;
    return id;
}

}  // namespace

TEST(TileRendererCancelStress, CancelDuringRenderLeavesEveryCloneBalanced)
{
    const std::filesystem::path pdf =
        std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
    ASSERT_TRUE(std::filesystem::exists(pdf))
        << "test_3page.pdf is required and missing: " << pdf.string();

    const int iterations = stressIterations();
    ASSERT_GT(iterations, 0) << "a zero-iteration stress run proves nothing";

    int rendererTeardowns = 0;

    for (int i = 0; i < iterations; ++i) {
        // A FRESH context, document, renderer AND POOL every iteration. The
        // per-iteration teardown is the point: it is what exits the worker
        // threads and drops their MuPDF clones, which is the moment fitz
        // checks the error stack.
        auto context = std::make_shared<pdfe::core::MuPdfContext>();
        auto document = pdfe::core::Document::open(context, pdf);
        ASSERT_NE(document, nullptr) << "iteration " << i;

        {
            auto renderer = std::make_unique<TileRenderer>(document.get());

            // Enough tiles that workers are genuinely in flight when the
            // cancel lands. Page 0 is cancelled; page 1 is left alone so the
            // pool has work that is NOT cancelled, which is the mix the
            // reported test used.
            for (int col = 0; col < 6; ++col) {
                renderer->requestTile(makeTileId(0, col, 0, 1.0f), nullptr);
            }
            for (int col = 0; col < 2; ++col) {
                renderer->requestTile(makeTileId(1, col, 0, 1.0f), nullptr);
            }

            // IMMEDIATELY — no wait. cancelPage is cooperative: it sets the
            // flag and drops the bookkeeping without waiting, so after it
            // returns the workers are still inside m_context->execute(...).
            renderer->cancelPage(0);

            // Destroy the renderer right away, while that is still true. The
            // destructor does cancelAll() + waitForDone(), so this is not a
            // use-after-free -- it is the narrowest window in which a worker
            // can be unwinding as its pool is being torn down.
            renderer.reset();
            ++rendererTeardowns;
        }

        // Let Qt deliver anything queued by the teardown before the context
        // goes away, so a dropped clone is not blamed on an event still in
        // flight.
        if (QCoreApplication::instance()) {
            QCoreApplication::processEvents();
        }

        document.reset();
        context.reset();
    }

    // Reaching here IS the verdict: an unbalanced error stack aborts the
    // process rather than failing an assertion, which is how it cost 408 tests
    // in the shard. So assert the loop actually ran the number of teardowns it
    // claims -- otherwise a future edit that skipped the body would "pass".
    EXPECT_EQ(rendererTeardowns, iterations)
        << "the loop did not perform one renderer teardown per iteration, so "
           "the thread-exit path this test exists to exercise did not run as "
           "many times as it reports";
}
