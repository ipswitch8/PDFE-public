#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <functional>
#include <cstring>
#include <exception>  // std::exception_ptr — see executeReturn (GitHub #287)

#include "Error.h"

// MuPDF headers (have their own extern "C" guards)
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

namespace pdfe::core {

/// RAII wrapper for MuPDF's fz_context*.
///
/// MuPDF uses setjmp/longjmp for error handling. C++ RAII objects (smart pointers,
/// std::string, etc.) MUST NOT be on the stack within fz_try/fz_catch blocks because
/// longjmp does not unwind C++ destructors.
///
/// This class provides:
/// - Safe creation/destruction of fz_context
/// - Thread-local context cloning (MuPDF requires one context per thread)
/// - Error translation from fz_try/fz_catch to C++ exceptions
class MuPdfContext {
public:
    /// Create the main context with given memory/glyph cache limits.
    /// @param maxStore Maximum memory for resource cache (default 256MB)
    explicit MuPdfContext(size_t maxStore = 256 * 1024 * 1024);

    ~MuPdfContext();

    // Non-copyable
    MuPdfContext(const MuPdfContext&) = delete;
    MuPdfContext& operator=(const MuPdfContext&) = delete;

    // Movable
    MuPdfContext(MuPdfContext&& other) noexcept;
    MuPdfContext& operator=(MuPdfContext&& other) noexcept;

    /// Get the raw fz_context pointer. Use with care.
    [[nodiscard]] fz_context* get() const noexcept { return ctx_; }

    /// Get a thread-local clone of this context.
    /// Each worker thread must use its own context clone.
    /// The clone shares the store/font cache with the parent.
    [[nodiscard]] fz_context* threadLocalClone();

    /// Execute a MuPDF operation with proper error translation.
    /// The callable receives fz_context* and must NOT create C++ RAII objects
    /// that would be on the stack when fz_try/fz_catch fires.
    ///
    /// Usage:
    ///   ctx.execute([](fz_context* ctx) {
    ///       fz_document* doc = fz_open_document(ctx, "file.pdf");
    ///       // ... use doc ...
    ///   });
    ///
    /// If MuPDF throws (via fz_throw), this translates to MuPdfException.
    void execute(const std::function<void(fz_context*)>& operation) const;

    /// Execute a MuPDF operation that returns a value.
    /// Same setjmp/longjmp safety rules as execute().
    template <typename T>
    T executeReturn(const std::function<T(fz_context*)>& operation) const;

    /// Release thread-local clones for the calling thread.
    /// Call this before a worker thread exits.
    static void releaseThreadLocalContext();

    /// This instance's monotonic id — the key under which its per-thread clones
    /// are cached. Never reused for the lifetime of the process, so a destroyed
    /// instance's cache entries can never be found by a later instance (GitHub
    /// #154). Zero means "moved-from" (no clones can be cached for it).
    [[nodiscard]] std::uint64_t instanceId() const noexcept { return instanceId_; }

    /// Number of live per-thread context clones across all threads.
    /// Legitimate production instrumentation: incremented when a clone is
    /// created (threadLocalClone) and decremented when it is dropped (thread
    /// exit or releaseThreadLocalContext). A healthy pool returns this to its
    /// baseline once all worker threads have exited; a persistently rising
    /// value indicates leaked clones (each clone also pins the parent store).
    [[nodiscard]] static size_t liveThreadCloneCount() noexcept;

private:
    fz_context* ctx_ = nullptr;

    // Monotonic, never-reused instance id. Assigned at construction; carried by
    // move; zeroed in the moved-from object. See instanceId().
    std::uint64_t instanceId_ = 0;

    // Thread-local context management. Each worker thread caches a DISTINCT
    // fz_context clone per MuPdfContext instance in a thread_local map whose
    // destructor drops every clone at thread exit (R2-06). currentThreadClone()
    // returns THIS instance's clone for the calling thread, or the main ctx_ if
    // none has been established — never another instance's clone (R2-07).
    // Definition + the thread_local map live in MuPdfContext.cpp.
    //
    // GitHub #154: the map is keyed on instanceId_, NOT on the raw ctx_ pointer.
    // Keying on ctx_ aliased across instance lifetimes — destroy a context and
    // let the allocator hand the same address to the next fz_new_context(), and
    // the dead instance's cached clone was returned for the NEW instance (whose
    // store it does not share). Instance ids are never reused, so the stale
    // entry is now simply unfindable. dropThreadCloneForInstance() additionally
    // erases (and drops) the calling thread's entry at destruction so the common
    // single-threaded case reclaims immediately.
    [[nodiscard]] fz_context* currentThreadClone() const noexcept;
    static void dropThreadCloneForInstance(std::uint64_t instanceId) noexcept;
    static std::mutex cloneMutex_;

    // MuPDF lock infrastructure for multi-threaded context cloning.
    // fz_clone_context requires that the parent context was created with
    // a valid fz_locks_context. These statics provide FZ_LOCK_MAX mutexes
    // and the C-style lock/unlock callbacks that MuPDF expects.
    static std::array<std::mutex, FZ_LOCK_MAX> fzLockMutexes_;
    static void lockMutex(void* user, int lock);
    static void unlockMutex(void* user, int lock);
    static fz_locks_context fzLocksContext_;
};

// Template implementation
template <typename T>
T MuPdfContext::executeReturn(const std::function<T(fz_context*)>& operation) const
{
    // Use this instance's thread-local clone if available (worker threads),
    // otherwise the main context.
    fz_context* c = currentThreadClone();
    T result{};
    // Declare result storage outside fz_try scope
    bool hasError = false;
    char errorMsg[256] = {0};

    // GitHub #287 — a C++ exception must not unwind through fz_try, or the
    // error frame fz_try pushed is never popped and fitz aborts the process
    // later (on context drop or thread exit) with
    //   Assertion failed: ctx->error.top == ctx->error.stack_base
    // One such abort killed a whole 16-shard-run shard, costing 408 tests.
    // See the long note on MuPdfContext::execute in the .cpp for the full
    // reasoning, including why the exception is captured rather than rethrown
    // in place and why the exception_ptr is declared before fz_try.
    std::exception_ptr cppError;

    fz_try(c)
    {
        try {
            result = operation(c);
        } catch (...) {
            cppError = std::current_exception();
        }
    }
    fz_catch(c)
    {
        hasError = true;
        // Copy error message while still in fz_catch scope
        const char* msg = fz_caught_message(c);
        if (msg) {
            std::strncpy(errorMsg, msg, sizeof(errorMsg) - 1);
        }
    }

    if (cppError) {
        std::rethrow_exception(cppError);
    }
    if (hasError) {
        throw MuPdfException(std::string("MuPDF error: ") + errorMsg);
    }

    return result;
}

} // namespace pdfe::core
