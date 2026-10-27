#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <stdexcept>
#include <string>

#include "core/MuPdfContext.h"

using namespace pdfe::core;

namespace {

// GitHub #287 oracle — the exact quantity fitz asserts on.
//
// `fz_try` pushes an error frame and `fz_catch` pops it. fz_drop_context (and
// the thread-exit path) checks the stack came back empty:
//
//   Assertion failed: ctx->error.top == ctx->error.stack_base,
//       file source/fitz/context.c, line 218
//
// `fz_error_context` is fully public in mupdf/fitz/context.h (`fz_error_stack_slot
// *top; fz_error_stack_slot stack[256]; fz_error_stack_slot padding;
// fz_error_stack_slot *stack_base;`), so the depth is directly readable. This is
// not reaching into a private implementation detail to manufacture an oracle --
// it is reading the one word whose value decides whether the process aborts.
//
// WHY MEASURE THIS AND NOT "DOES IT CRASH". The assertion does not fire where the
// frame leaks. It fires arbitrarily later, when the context is dropped or a thread
// holding a clone exits, and it ABORTS the process -- so the leaking test takes
// the whole shard down with it and names an innocent neighbour. That is precisely
// how #287 presented. Measuring the depth puts the failure at the line that
// causes it.
int errorStackDepth(fz_context* c)
{
    return static_cast<int>(c->error.top - c->error.stack_base);
}

struct CallableBlewUp : std::runtime_error {
    CallableBlewUp() : std::runtime_error("callable blew up") {}
};

} // namespace

class MuPdfContextTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(MuPdfContextTest, CreateAndDestroy)
{
    MuPdfContext ctx;
    EXPECT_NE(ctx.get(), nullptr);
}

TEST_F(MuPdfContextTest, CreateWithCustomStoreSize)
{
    MuPdfContext ctx(128 * 1024 * 1024); // 128MB
    EXPECT_NE(ctx.get(), nullptr);
}

TEST_F(MuPdfContextTest, MoveConstruction)
{
    MuPdfContext ctx1;
    fz_context* raw = ctx1.get();
    EXPECT_NE(raw, nullptr);

    MuPdfContext ctx2(std::move(ctx1));
    EXPECT_EQ(ctx2.get(), raw);
    EXPECT_EQ(ctx1.get(), nullptr);
}

TEST_F(MuPdfContextTest, MoveAssignment)
{
    MuPdfContext ctx1;
    MuPdfContext ctx2;
    fz_context* raw1 = ctx1.get();

    ctx2 = std::move(ctx1);
    EXPECT_EQ(ctx2.get(), raw1);
    EXPECT_EQ(ctx1.get(), nullptr);
}

TEST_F(MuPdfContextTest, ExecuteSuccess)
{
    MuPdfContext ctx;
    bool executed = false;

    ctx.execute([&executed](fz_context*) {
        executed = true;
    });

    EXPECT_TRUE(executed);
}

TEST_F(MuPdfContextTest, ExecuteReturnSuccess)
{
    MuPdfContext ctx;

    int result = ctx.executeReturn<int>([](fz_context*) -> int {
        return 42;
    });

    EXPECT_EQ(result, 42);
}

TEST_F(MuPdfContextTest, ExecuteThrowsOnMuPdfError)
{
    MuPdfContext ctx;

    EXPECT_THROW(
        ctx.execute([](fz_context* c) {
            // Attempt to open a non-existent file to trigger fz_throw
            fz_open_document(c, "nonexistent_file_that_does_not_exist.pdf");
        }),
        MuPdfException
    );
}

TEST_F(MuPdfContextTest, ThreadLocalClone)
{
    MuPdfContext ctx;
    std::atomic<bool> success{false};
    std::atomic<bool> cloneNotNull{false};

    std::thread worker([&ctx, &success, &cloneNotNull]() {
        try {
            fz_context* clone = ctx.threadLocalClone();
            cloneNotNull.store(clone != nullptr);
            // Clone should be different from parent
            success.store(clone != ctx.get());
            MuPdfContext::releaseThreadLocalContext();
        } catch (...) {
            success.store(false);
        }
    });

    worker.join();
    EXPECT_TRUE(cloneNotNull.load());
    EXPECT_TRUE(success.load());
}

TEST_F(MuPdfContextTest, MultipleThreadClones)
{
    MuPdfContext ctx;
    constexpr int numThreads = 4;
    std::vector<std::thread> threads;
    std::atomic<int> successCount{0};

    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([&ctx, &successCount]() {
            try {
                fz_context* clone = ctx.threadLocalClone();
                if (clone != nullptr && clone != ctx.get()) {
                    successCount.fetch_add(1);
                }
                MuPdfContext::releaseThreadLocalContext();
            } catch (...) {
                // Don't count failures
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(successCount.load(), numThreads);
}

TEST_F(MuPdfContextTest, ThreadLocalCloneIsCached)
{
    MuPdfContext ctx;
    std::atomic<bool> same{false};

    std::thread worker([&ctx, &same]() {
        try {
            fz_context* clone1 = ctx.threadLocalClone();
            fz_context* clone2 = ctx.threadLocalClone();
            same.store(clone1 == clone2);
            MuPdfContext::releaseThreadLocalContext();
        } catch (...) {
            same.store(false);
        }
    });

    worker.join();
    EXPECT_TRUE(same.load());
}

// The oracle's GREEN DIRECTION, asserted before anything relies on it.
//
// Every test below concludes "the depth came back to where it started". That
// sentence is equally true of an oracle that always reads the same number, so on
// its own it is worth nothing. This pins that the depth genuinely MOVES: inside a
// callable -- i.e. inside fz_try, with the frame pushed -- it must read deeper
// than at rest. Only then does "it came back" carry information.
TEST_F(MuPdfContextTest, ErrorStackDepthOracleActuallyMoves)
{
    MuPdfContext ctx;
    fz_context* c = ctx.get();

    const int atRest = errorStackDepth(c);
    int insideTheTry = atRest;
    ctx.execute([&insideTheTry, c](fz_context* inner) {
        ASSERT_EQ(inner, c) << "the callable did not receive the context this "
                               "test is measuring, so the measurement is of "
                               "some other stack";
        insideTheTry = errorStackDepth(c);
    });

    EXPECT_GT(insideTheTry, atRest)
        << "the depth did not change while a frame was pushed, so this oracle "
           "cannot distinguish a balanced stack from a leaked frame and every "
           "assertion built on it is vacuous";
    EXPECT_EQ(errorStackDepth(c), atRest);
    // ...and that "at rest" is fitz's own base, not merely wherever we happened
    // to start. Comparing two numbers this code produced would be comparing an
    // output against another output; stack_base is the independent anchor the
    // real assertion uses.
    EXPECT_EQ(atRest, 0) << "error.top was not at error.stack_base on a freshly "
                            "created context";
}

// GitHub #287 — THE FIX. A C++ exception escaping the callable must not leave
// the frame pushed.
TEST_F(MuPdfContextTest, ExecuteKeepsTheErrorStackBalancedWhenTheCallableThrows)
{
    MuPdfContext ctx;
    fz_context* c = ctx.get();
    const int atRest = errorStackDepth(c);

    EXPECT_THROW(ctx.execute([](fz_context*) { throw CallableBlewUp(); }),
                 CallableBlewUp)
        << "the callable's own exception must reach the caller unchanged -- "
           "swallowing it, or replacing it with MuPdfException, would hide the "
           "original fault";

    EXPECT_EQ(errorStackDepth(c), atRest)
        << "a C++ exception unwound through fz_try and left its error frame "
           "pushed. Nothing fails here in production: the process aborts later, "
           "on context drop or thread exit, blaming whatever test is running "
           "then (GitHub #287)";
}

TEST_F(MuPdfContextTest, ExecuteReturnKeepsTheErrorStackBalancedWhenTheCallableThrows)
{
    MuPdfContext ctx;
    fz_context* c = ctx.get();
    const int atRest = errorStackDepth(c);

    // The template in the header is a SEPARATE copy of the same fz_try/fz_catch
    // shape, so fixing execute() in the .cpp proves nothing about it. Both are
    // reachable from production code and both needed the fix.
    EXPECT_THROW(ctx.executeReturn<int>([](fz_context*) -> int {
                     throw CallableBlewUp();
                 }),
                 CallableBlewUp);

    EXPECT_EQ(errorStackDepth(c), atRest);
}

// The MuPDF-error path must still work, and must still balance. The fix inserted
// an inner try/catch and a rethrow around the fz_try body, which is exactly the
// kind of edit that can swallow the fz_throw translation instead.
TEST_F(MuPdfContextTest, MuPdfErrorsStillTranslateAndStillBalance)
{
    MuPdfContext ctx;
    fz_context* c = ctx.get();
    const int atRest = errorStackDepth(c);

    EXPECT_THROW(ctx.execute([](fz_context* inner) {
                     fz_open_document(inner, "nonexistent_file_287.pdf");
                 }),
                 MuPdfException);

    EXPECT_EQ(errorStackDepth(c), atRest);
}

// The leak is UNBOUNDED, which is why it is fatal rather than untidy. The error
// stack is a fixed 256 slots; a frame lost per call exhausts it. This drives more
// iterations than there are slots, so it is a second oracle that does not depend
// on reading the struct at all -- under the unfixed code the stack cannot
// survive 512 leaks.
TEST_F(MuPdfContextTest, RepeatedThrowingCallablesDoNotAccumulateErrorFrames)
{
    MuPdfContext ctx;
    fz_context* c = ctx.get();
    const int atRest = errorStackDepth(c);

    constexpr int kIterations = 512;  // > the 256-slot fz error stack
    for (int i = 0; i < kIterations; ++i) {
        EXPECT_THROW(ctx.execute([](fz_context*) { throw CallableBlewUp(); }),
                     CallableBlewUp);
        ASSERT_EQ(errorStackDepth(c), atRest)
            << "depth grew by iteration " << i
            << "; the leak is per-call and therefore unbounded";
    }
}

// The REAL #287 scenario, reproduced deterministically.
//
// `TileRenderer` owns a private QThreadPool, so its destruction exits worker
// threads -- and a thread holding a cloned context exiting is exactly when fitz
// checks the stack. This runs a throwing callable on a worker thread against
// that thread's own CLONE, then lets the thread exit. Under the unfixed code the
// clone is dropped with a frame still pushed, which is the abort.
TEST_F(MuPdfContextTest, AThrowingCallableOnAWorkerThreadLeavesItsCloneDroppable)
{
    MuPdfContext ctx;
    std::atomic<int> depthAfter{-1};
    std::atomic<int> depthAtRest{-1};
    std::atomic<bool> sawTheException{false};
    std::atomic<bool> cloneWasDistinct{false};

    std::thread worker([&]() {
        fz_context* clone = ctx.threadLocalClone();
        cloneWasDistinct.store(clone != nullptr && clone != ctx.get());
        depthAtRest.store(errorStackDepth(clone));

        try {
            ctx.execute([](fz_context*) { throw CallableBlewUp(); });
        } catch (const CallableBlewUp&) {
            sawTheException.store(true);
        }

        depthAfter.store(errorStackDepth(clone));
        // Deliberately NOT releaseThreadLocalContext() -- let the thread_local
        // destructor drop the clone at thread exit, which is the production path
        // an expiring QThreadPool worker takes.
    });
    worker.join();

    EXPECT_TRUE(cloneWasDistinct.load())
        << "the worker never got its own clone, so this test did not exercise "
           "the thread-exit drop path at all";
    EXPECT_TRUE(sawTheException.load());
    EXPECT_EQ(depthAfter.load(), depthAtRest.load())
        << "the worker's clone was dropped at thread exit with an error frame "
           "still pushed -- the measured #287 abort";
}
