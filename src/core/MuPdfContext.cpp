#include "MuPdfContext.h"

#include <atomic>
#include <cstring>
#include <unordered_map>
#include <utility>

namespace pdfe::core {

namespace {
// Live per-thread clone counter — production instrumentation and the cleanest
// leak oracle. Incremented on successful clone, decremented on drop.
std::atomic<size_t> g_liveCloneCount{0};

// Monotonic source of MuPdfContext instance ids. Starts at 1 so that 0 is
// reserved for "no instance" (default-/moved-from state), which can never match
// a cache entry. Wrap-around at 2^64 instances is not a reachable state.
std::atomic<std::uint64_t> g_nextInstanceId{1};

// Per-thread cache of fz_context clones, keyed on the OWNING MuPdfContext's
// monotonic INSTANCE ID. A single worker thread that services more than one
// document therefore gets a DISTINCT clone per document (closes R2-07: the old
// single static thread_local shared one clone across all instances, so a worker
// that first cloned document A then ran document B's work ran against A's clone
// and A's store).
//
// GitHub #154 — why the key is an id and not the parent ctx_ pointer:
// the previous key was the raw fz_context* and the entry was never erased when
// its owner was destroyed. A raw pointer is only unique among LIVE objects, so
//   MuPdfContext a;            // ctx_ == 0xABC, worker clones it -> entry[0xABC]
//   (a destroyed)              // entry[0xABC] left behind, clone still cached
//   MuPdfContext b;            // fz_new_context returns 0xABC again
// made b's currentThreadClone() hand back A's clone: a context sharing a store
// that b does not own, whose parent has been fz_drop_context()'d. Instance ids
// are monotonic and never reused, so a dead instance's entry can no longer be
// found by any later instance regardless of what the allocator does with the
// address. The destructor also erases the CALLING thread's entry (see
// dropThreadCloneForInstance) so the ordinary single-threaded lifecycle
// reclaims the clone at once rather than at thread exit.
//
// Entries belonging to a destroyed instance that were established on OTHER
// threads are still reclaimed the way they always were — at that thread's exit,
// or by an explicit releaseThreadLocalContext(). They are inert in the meantime
// because their key is dead: nothing can look them up. Erasing them from here
// would mean mutating another thread's map (and fz_drop_context()ing a clone
// that thread may be inside) with no lock on the hot lookup path, which trades
// a bounded, inert retention for a real data race.
//
// The thread_local object has a NON-TRIVIAL destructor that drops every clone
// when the OS thread exits (closes R2-06: QThreadPool workers hit idle-expiry
// and exit without any explicit release, so a raw thread_local pointer with a
// trivial destructor leaked its clone — and the parent store it pins — on every
// expired worker).
//
// Destructor safety: it runs at thread exit and may outlive the parent
// MuPdfContext instances. The map KEY is now a plain integer, so there is no
// possibly-dangling pointer to dereference at all — it only fz_drop_context()s
// the VALUE (the clone). fz_clone_context keeps the shared store alive by
// refcount, so dropping a clone after its parent is gone is safe; each clone is
// used only by this one thread, so no external mutex is needed for the drop.
struct ThreadCloneCache {
    std::unordered_map<std::uint64_t, fz_context*> clones;  // instance id -> clone
    ~ThreadCloneCache() {
        for (auto& entry : clones) {
            if (entry.second) {
                fz_drop_context(entry.second);
                g_liveCloneCount.fetch_sub(1, std::memory_order_relaxed);
            }
        }
    }
};
thread_local ThreadCloneCache g_threadClones;
} // namespace

// Static members
std::mutex MuPdfContext::cloneMutex_;

// MuPDF lock infrastructure — static mutexes and callbacks.
// fz_clone_context requires a parent context that was created with a valid
// fz_locks_context; without one the clone returns NULL because the cloned
// contexts share a store and need mutual-exclusion around it.
std::array<std::mutex, FZ_LOCK_MAX> MuPdfContext::fzLockMutexes_;

void MuPdfContext::lockMutex(void* /*user*/, int lock)
{
    fzLockMutexes_[static_cast<size_t>(lock)].lock();
}

void MuPdfContext::unlockMutex(void* /*user*/, int lock)
{
    fzLockMutexes_[static_cast<size_t>(lock)].unlock();
}

fz_locks_context MuPdfContext::fzLocksContext_ = {
    nullptr,               // user — not needed; callbacks use the static array directly
    &MuPdfContext::lockMutex,
    &MuPdfContext::unlockMutex
};

MuPdfContext::MuPdfContext(size_t maxStore)
{
    // Claim the instance id BEFORE the context exists, so no id is ever shared
    // with another instance even transiently.
    instanceId_ = g_nextInstanceId.fetch_add(1, std::memory_order_relaxed);

    ctx_ = fz_new_context(nullptr, &fzLocksContext_, maxStore);
    if (!ctx_) {
        throw MuPdfException("Failed to create MuPDF context");
    }

    // Register default document handlers (PDF, XPS, EPUB, etc.)
    // This must be done outside fz_try since it uses fz_throw on failure
    bool hasError = false;
    char errorMsg[256] = {0};

    fz_try(ctx_)
    {
        fz_register_document_handlers(ctx_);
    }
    fz_catch(ctx_)
    {
        hasError = true;
        const char* msg = fz_caught_message(ctx_);
        if (msg) {
            std::strncpy(errorMsg, msg, sizeof(errorMsg) - 1);
        }
    }

    if (hasError) {
        fz_drop_context(ctx_);
        ctx_ = nullptr;
        throw MuPdfException(std::string("Failed to register document handlers: ") + errorMsg);
    }
}

MuPdfContext::~MuPdfContext()
{
    // Erase (and drop) this instance's clone on the destroying thread before the
    // parent context goes away — GitHub #154. Other threads' entries are already
    // unreachable because the id is never reissued; see the ThreadCloneCache
    // comment for why they are not touched from here.
    dropThreadCloneForInstance(instanceId_);
    instanceId_ = 0;

    if (ctx_) {
        fz_drop_context(ctx_);
        ctx_ = nullptr;
    }
}

MuPdfContext::MuPdfContext(MuPdfContext&& other) noexcept
    : ctx_(std::exchange(other.ctx_, nullptr)),
      instanceId_(std::exchange(other.instanceId_, 0u))
{
    // The id moves with the context, so any clone already cached under it stays
    // correctly associated with the (now relocated) owner.
}

MuPdfContext& MuPdfContext::operator=(MuPdfContext&& other) noexcept
{
    if (this != &other) {
        // Release what we are about to overwrite, cache entry included.
        dropThreadCloneForInstance(instanceId_);
        if (ctx_) {
            fz_drop_context(ctx_);
        }
        ctx_ = std::exchange(other.ctx_, nullptr);
        instanceId_ = std::exchange(other.instanceId_, 0u);
    }
    return *this;
}

fz_context* MuPdfContext::threadLocalClone()
{
    // Return this instance's existing clone for the calling thread, if any.
    auto it = g_threadClones.clones.find(instanceId_);
    if (it != g_threadClones.clones.end() && it->second) {
        return it->second;
    }

    // Clone must be done under lock — fz_clone_context is not thread-safe
    // with respect to the parent context's lock table initialization.
    std::lock_guard<std::mutex> lock(cloneMutex_);
    fz_context* clone = fz_clone_context(ctx_);
    if (!clone) {
        throw MuPdfException("Failed to clone MuPDF context for thread");
    }
    g_threadClones.clones[instanceId_] = clone;
    g_liveCloneCount.fetch_add(1, std::memory_order_relaxed);

    return clone;
}

fz_context* MuPdfContext::currentThreadClone() const noexcept
{
    // This instance's per-thread clone if one has been established, else the
    // main context. Never returns ANOTHER instance's clone (R2-07), nor a
    // DESTROYED instance's clone that happened to share our address (#154).
    auto it = g_threadClones.clones.find(instanceId_);
    if (it != g_threadClones.clones.end() && it->second) {
        return it->second;
    }
    return ctx_;
}

void MuPdfContext::dropThreadCloneForInstance(std::uint64_t instanceId) noexcept
{
    if (instanceId == 0) {
        return;  // moved-from / never-constructed: nothing can be cached
    }
    auto it = g_threadClones.clones.find(instanceId);
    if (it == g_threadClones.clones.end()) {
        return;
    }
    if (it->second) {
        fz_drop_context(it->second);
        g_liveCloneCount.fetch_sub(1, std::memory_order_relaxed);
    }
    g_threadClones.clones.erase(it);
}

void MuPdfContext::execute(const std::function<void(fz_context*)>& operation) const
{
    // Use this instance's thread-local clone if available (worker threads),
    // otherwise the main context.
    fz_context* c = currentThreadClone();
    bool hasError = false;
    char errorMsg[256] = {0};

    // GitHub #287 — A C++ EXCEPTION MUST NOT UNWIND THROUGH fz_try.
    //
    // `fz_try` PUSHES an error frame and `fz_catch` pops it. Exactly how is
    // the load-bearing part, so here are the macros (mupdf/fitz/context.h):
    //
    //   #define fz_try(ctx)   if (!fz_setjmp(*fz_push_try(ctx))) \
    //                             if (fz_do_try(ctx)) do
    //   #define fz_catch(ctx) while (0); if (fz_do_catch(ctx))
    //
    // so `fz_try(c) { BODY } fz_catch(c) { H }` is really
    //
    //   if (!fz_setjmp(*fz_push_try(c))) if (fz_do_try(c))
    //       do { BODY } while (0);
    //   if (fz_do_catch(c)) { H }
    //
    // `fz_do_catch()` is the function that POPS the frame, and it sits in an
    // `if` CONDITION placed after the do/while. It is therefore evaluated
    // whether or not the body ran -- which is what makes the MuPDF-error path
    // correct -- but a C++ exception thrown by `operation` unwinds out of the
    // `do { }` block and past that `if` entirely. `fz_do_catch` never runs, so
    // the frame stays pushed forever. fitz notices much later, when the
    // context is dropped or a thread holding a clone exits, and the diagnosis
    // arrives as:
    //
    //   Assertion failed: ctx->error.top == ctx->error.stack_base,
    //       file source/fitz/context.c, line 218
    //
    // which aborts the process. Measured consequence: one such abort killed
    // shard 7 of a 16-shard run, and because the shard writes no XML, all 408
    // of its assigned tests were recorded as errors. The report names
    // `TileRendererTest.CancelPage`, whose renderer owns a PRIVATE QThreadPool
    // -- so its destruction exits worker threads, and a thread exit is exactly
    // when an unbalanced stack is checked.
    //
    // CLAUDE.md already forbids throwing C++ exceptions inside these lambdas,
    // and that rule is correct. But a convention every call site must remember
    // forever is not a mechanism: `execute` is called from across the codebase,
    // and one `std::bad_alloc`, one `std::function` target throwing, or one
    // future lambda that forgets is enough. This makes the frame impossible to
    // leak regardless of what the callable does.
    //
    // WHY exception_ptr AND NOT A RETHROW IN PLACE. Rethrowing inside fz_try
    // would unwind through it again -- the very thing being prevented. The
    // exception is captured, the fz_try block exits normally so fz_catch runs
    // and pops the frame, and only then is it rethrown. The `exception_ptr` is
    // declared BEFORE fz_try so it survives the setjmp, and it is empty unless
    // the inner catch ran, so a longjmp from fz_throw skips no live
    // destructor -- which is the setjmp/longjmp hazard CLAUDE.md warns about.
    std::exception_ptr cppError;

    fz_try(c)
    {
        try {
            operation(c);
        } catch (...) {
            cppError = std::current_exception();
        }
    }
    fz_catch(c)
    {
        hasError = true;
        const char* msg = fz_caught_message(c);
        if (msg) {
            std::strncpy(errorMsg, msg, sizeof(errorMsg) - 1);
        }
    }

    // The frame is popped by here, so throwing is safe again. The callable's
    // own exception takes precedence: it is the original fault, and a MuPDF
    // error cannot have been raised on a path that threw before reaching one.
    if (cppError) {
        std::rethrow_exception(cppError);
    }
    if (hasError) {
        throw MuPdfException(std::string("MuPDF error: ") + errorMsg);
    }
}

void MuPdfContext::releaseThreadLocalContext()
{
    // Drop every clone the calling thread established (for any instance) and
    // clear the map. The thread_local destructor would also do this at thread
    // exit; this lets a thread reclaim eagerly before exiting.
    for (auto& entry : g_threadClones.clones) {
        if (entry.second) {
            fz_drop_context(entry.second);
            g_liveCloneCount.fetch_sub(1, std::memory_order_relaxed);
        }
    }
    g_threadClones.clones.clear();
}

size_t MuPdfContext::liveThreadCloneCount() noexcept
{
    return g_liveCloneCount.load(std::memory_order_relaxed);
}

} // namespace pdfe::core
