// Regression oracles for two MuPDF concurrency defects in MuPdfContext:
//
//   R2-06 (High)   — Per-thread fz_context clones leaked as worker threads
//                    expire. The clone lived in a raw `thread_local fz_context*`
//                    with a trivial destructor, and the only drop
//                    (releaseThreadLocalContext) is never called in src/. When a
//                    QThreadPool worker hits its idle-expiry and the OS thread
//                    exits, no destructor runs → fz_drop_context is never called
//                    → the clone (and the parent document store it pins) leaks.
//
//   R2-07 (Medium) — A SINGLE static thread_local clone was shared across ALL
//                    MuPdfContext instances. A worker that first cloned from
//                    document A and later ran execute() on document B's context
//                    ran against A's clone + A's store (cross-document store
//                    contamination). Masked today only by an undocumented
//                    1:1-pool-per-document arrangement.
//
// The fix replaces the single thread_local pointer with a per-thread MAP keyed
// on the owning parent context, wrapped in a thread_local object whose
// destructor drops every clone at thread exit. That closes BOTH bugs:
//   (a) the non-trivial destructor drops clones when the thread exits (R2-06);
//   (b) the map is keyed on the parent context, so A and B get distinct clones
//       on the same worker thread (R2-07).
//
// These oracles are RED on the unpatched (single-static-thread_local) code and
// GREEN after the fix. Reverting the fix re-REDs both (mutation survivor check).

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "core/MuPdfContext.h"

using namespace pdfe::core;

namespace {

// Drive a clone onto the calling worker thread the same way TileRenderer does:
// establish the thread-local clone, then run an operation through execute().
// The worker then EXITS WITHOUT calling releaseThreadLocalContext() — exactly
// the QThreadPool idle-expiry path that leaks clones on the unpatched code.
void cloneAndExitWithoutRelease(MuPdfContext& ctx, std::atomic<int>& cloned)
{
    fz_context* clone = ctx.threadLocalClone();
    if (clone != nullptr) {
        cloned.fetch_add(1, std::memory_order_relaxed);
        // Exercise execute() so the clone is actually used, not just created.
        ctx.execute([](fz_context*) { /* no-op MuPDF work */ });
    }
    // Deliberately NO releaseThreadLocalContext(): the fix must reclaim the
    // clone at thread exit via the thread_local destructor.
}

} // namespace

// -----------------------------------------------------------------------------
// R2-06: clones must return to baseline once all worker threads have exited.
// -----------------------------------------------------------------------------
TEST(MuPdfContextCloneLifecycle, ClonesReturnToBaselineAfterWorkerThreadsExit)
{
    const size_t baseline = MuPdfContext::liveThreadCloneCount();

    MuPdfContext ctx;
    constexpr int kThreads = 8;

    {
        std::atomic<int> cloned{0};
        std::vector<std::thread> workers;
        workers.reserve(kThreads);
        for (int i = 0; i < kThreads; ++i) {
            workers.emplace_back(cloneAndExitWithoutRelease,
                                 std::ref(ctx), std::ref(cloned));
        }

        // While workers are alive we expect the counter to have climbed above
        // baseline (proves the clones were really created, not optimized away).
        for (auto& t : workers) {
            t.join();
        }
        EXPECT_EQ(cloned.load(), kThreads);
    }

    // All worker threads have now exited. On the unpatched raw-thread_local code
    // no destructor ran, so the counter stays at baseline + kThreads (RED). The
    // fix drops each clone at thread exit, returning the counter to baseline.
    EXPECT_EQ(MuPdfContext::liveThreadCloneCount(), baseline)
        << "Per-thread context clones leaked: worker threads exited without "
           "their clones being dropped (R2-06).";
}

// A second wave on fresh threads must not accumulate either: repeated
// spawn/exit cycles must not grow the live-clone count without bound.
TEST(MuPdfContextCloneLifecycle, RepeatedWorkerWavesDoNotAccumulate)
{
    const size_t baseline = MuPdfContext::liveThreadCloneCount();
    MuPdfContext ctx;

    for (int wave = 0; wave < 3; ++wave) {
        std::atomic<int> cloned{0};
        std::vector<std::thread> workers;
        for (int i = 0; i < 4; ++i) {
            workers.emplace_back(cloneAndExitWithoutRelease,
                                 std::ref(ctx), std::ref(cloned));
        }
        for (auto& t : workers) {
            t.join();
        }
        EXPECT_EQ(cloned.load(), 4);
        EXPECT_EQ(MuPdfContext::liveThreadCloneCount(), baseline)
            << "Clone count did not return to baseline after wave " << wave;
    }
}

// -----------------------------------------------------------------------------
// R2-07: two documents used on the SAME worker thread must get DISTINCT clones,
// and execute() must route each instance's work to its own clone.
// -----------------------------------------------------------------------------
TEST(MuPdfContextCloneLifecycle, DistinctClonesPerInstanceOnSameThread)
{
    MuPdfContext ctxA;
    MuPdfContext ctxB;

    std::atomic<void*> cloneA{nullptr};
    std::atomic<void*> cloneB{nullptr};
    std::atomic<void*> executeCtxA{nullptr};
    std::atomic<void*> executeCtxB{nullptr};
    std::atomic<void*> reFetchA{nullptr};
    std::atomic<bool> threw{false};

    std::thread worker([&]() {
        try {
            fz_context* cA = ctxA.threadLocalClone();
            fz_context* cB = ctxB.threadLocalClone();
            cloneA.store(cA);
            cloneB.store(cB);

            // Which context does execute() actually route to for each instance?
            ctxA.execute([&](fz_context* c) { executeCtxA.store(c); });
            ctxB.execute([&](fz_context* c) { executeCtxB.store(c); });

            // Per-instance caching must still hold: re-fetching A returns cA.
            reFetchA.store(ctxA.threadLocalClone());

            MuPdfContext::releaseThreadLocalContext();
        } catch (...) {
            threw.store(true);
        }
    });
    worker.join();

    ASSERT_FALSE(threw.load());
    ASSERT_NE(cloneA.load(), nullptr);
    ASSERT_NE(cloneB.load(), nullptr);

    // Each clone must differ from its own parent context.
    EXPECT_NE(cloneA.load(), ctxA.get());
    EXPECT_NE(cloneB.load(), ctxB.get());

    // Core R2-07 oracle: the two instances must NOT share a clone.
    EXPECT_NE(cloneA.load(), cloneB.load())
        << "Both MuPdfContext instances received the SAME thread-local clone "
           "(single shared static thread_local — R2-07 cross-document store "
           "contamination).";

    // execute() must route each instance to ITS OWN clone, not a shared one.
    EXPECT_EQ(executeCtxA.load(), cloneA.load())
        << "ctxA.execute() did not run on ctxA's clone.";
    EXPECT_EQ(executeCtxB.load(), cloneB.load())
        << "ctxB.execute() ran on the wrong context (R2-07): expected ctxB's "
           "clone but got a different context.";

    // Per-instance caching preserved.
    EXPECT_EQ(reFetchA.load(), cloneA.load())
        << "Re-fetching ctxA's clone on the same thread returned a different "
           "context; per-instance caching regressed.";
}
