// -*- coding: utf-8 -*-
// Backend-independent scaffolding shared by every PDFE fuzz harness AND
// by the GoogleTest smoke test (tests/test_fuzz_harness_smoke.cpp).
//
// This header deliberately carries NO libFuzzer / AFL++ backend wiring and
// NO `#error "define a backend"` guard, so it can be included from an
// ordinary (non-fuzzer) translation unit — e.g. the gtest build — to reuse
// the temp-file plumbing and the benign-throw triage classifier without
// pulling in LLVMFuzzerTestOneInput or the sanitizer runtime.
//
// fuzz_common.h includes this file and then adds the backend entry points.
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>   // std::abort (pdfe::fuzz::fail)
#include <exception>
#include <filesystem>
#include <fstream>
#include <new>       // std::bad_alloc
#include <stdexcept>
#include <typeinfo>

#include "core/Error.h"   // pdfe::core::PdfeException / MuPdfException / ...

#if defined(_WIN32)
#  include <process.h>     // _getpid
#  define PDFE_FUZZ_GETPID() (::_getpid())
#else
#  include <unistd.h>      // getpid
#  define PDFE_FUZZ_GETPID() (::getpid())
#endif

namespace pdfe::fuzz {

// ============================================================
// Temp-file plumbing
// ============================================================
// PDFE's Document::open(...) takes a std::filesystem::path, not a memory
// buffer, so each harness writes the fuzz bytes to a per-iteration temp
// file and opens from there. The temp file is deleted at end of iteration.

// Write the input bytes to a unique temp file and return the path.
// Caller is responsible for unlinking. Returns an empty path on failure
// (e.g., disk full); the caller should treat that as a no-op input rather
// than crash the fuzzer.
inline std::filesystem::path write_temp_pdf(
    const std::uint8_t* data, std::size_t size)
{
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "pdfe_fuzz";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return {};

    static thread_local std::uint64_t counter = 0;
    char name[64];
    std::snprintf(name, sizeof(name), "in_%u_%llu.pdf",
        static_cast<unsigned>(PDFE_FUZZ_GETPID()),
        static_cast<unsigned long long>(++counter));
    fs::path path = dir / name;

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return {};
    if (size > 0) {
        f.write(reinterpret_cast<const char*>(data),
                static_cast<std::streamsize>(size));
    }
    f.close();
    if (!f) {
        std::error_code rmec;
        fs::remove(path, rmec);
        return {};
    }
    return path;
}

inline void unlink_temp_pdf(const std::filesystem::path& p)
{
    if (p.empty()) return;
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

// RAII guard: writes bytes on construction, unlinks on destruction.
class TempPdfFile {
public:
    TempPdfFile(const std::uint8_t* data, std::size_t size)
        : path_(write_temp_pdf(data, size)) {}
    ~TempPdfFile() { unlink_temp_pdf(path_); }
    TempPdfFile(const TempPdfFile&)            = delete;
    TempPdfFile& operator=(const TempPdfFile&) = delete;

    const std::filesystem::path& path() const noexcept { return path_; }
    bool valid() const noexcept { return !path_.empty(); }

private:
    std::filesystem::path path_;
};

// ============================================================
// Benign-throw triage (R2-17)
// ============================================================
// A fuzz harness must swallow the EXPECTED "refuse malformed input" path
// (MuPDF/PDFE parse errors) so the campaign keeps making progress, while
// letting anything that indicates a genuine defect propagate so libFuzzer
// / ASan records it as a crash.
//
// The historical hazard this classifier closes (R2-17): a bare `catch (...)`
// around Document::open swallows EVERYTHING — including the C++ exceptions a
// real memory fault can surface as (a std::bad_alloc from a corrupted length
// driving an absurd allocation, or an unknown non-std throw). Those are NOT
// benign and must crash. A true SEH access-violation / heap-corruption is not
// a C++ exception under the default /EH model, so it already escapes a
// `catch (const std::exception&)`; the job here is to make sure the C++
// exceptions that CAN mask a memory fault are never bucketed benign.

enum class ThrowClass {
    BenignExpected,  ///< Expected malformed-input refusal; safe to swallow.
    MustCrash        ///< Possible real defect; must propagate (fuzzer records).
};

// A tag exception used to SIMULATE a hard memory fault (SEGV / heap
// corruption) reaching the classifier from a unit test — the classifier must
// always route it to MustCrash. Also gives harnesses a way to convert a
// detected-but-recoverable memory-safety signal into a hard crash.
struct SimulatedMemoryFault : public std::exception {
    const char* what() const noexcept override {
        return "pdfe::fuzz::SimulatedMemoryFault";
    }
};

// Classify a caught std::exception from a PDFE / MuPDF integration call.
// Only genuine parse/refuse errors from the integration layer are benign.
inline ThrowClass classify_caught(const std::exception& e) noexcept
{
    // std::bad_alloc: a corrupted object length or count can drive an absurd
    // allocation whose failure is indistinguishable from a heap fault. Never
    // swallow it — a real out-of-memory in a 64 KB-bounded harness is itself
    // a bug worth surfacing.
    if (dynamic_cast<const std::bad_alloc*>(&e)) return ThrowClass::MustCrash;

    // Explicit simulated memory-fault marker: always a crash.
    if (dynamic_cast<const SimulatedMemoryFault*>(&e)) return ThrowClass::MustCrash;

    // The expected refuse path: MuPDF/PDFE integration errors (MuPdfException,
    // QpdfException, IoException, and PdfeException itself).
    if (dynamic_cast<const pdfe::core::PdfeException*>(&e))
        return ThrowClass::BenignExpected;

    // Plain std::runtime_error from the parser/loader layer is expected.
    if (dynamic_cast<const std::runtime_error*>(&e))
        return ThrowClass::BenignExpected;

    // A logic_error (bad index / bad invariant) or any other std::exception
    // subclass is NOT part of the expected refuse path — surface it.
    return ThrowClass::MustCrash;
}

// Swallow-or-rethrow helper. Harnesses call this from a
// `catch (const std::exception& e)` block instead of a bare `catch (...)`.
// It rethrows anything the classifier deems MustCrash. NOTE: harnesses must
// NOT add a following `catch (...)` — leaving non-std C++ throws uncaught is
// intentional, so they crash and get recorded.
[[noreturn]] inline void rethrow_if_not_benign_impl(const std::exception& e)
{
    // Only reached on MustCrash; rethrow the active exception unchanged.
    (void)e;
    throw;
}

inline void triage_or_rethrow(const std::exception& e)
{
    if (classify_caught(e) == ThrowClass::MustCrash)
        rethrow_if_not_benign_impl(e);
    // else: benign, return and let the caller continue.
}

// Report a violated OUTPUT INVARIANT and abort (phase-73).
//
// WHY ABORT AND NOT A RETURNED BOOL. A harness that merely notices a bad
// output and returns has performed no oracle at all: libFuzzer only records an
// input when the process dies, so a silently-ignored invariant makes the
// campaign a no-crash test with extra steps. abort() is what turns "the parser
// claimed success but produced garbage" into a saved artifact that can be
// replayed — the same reasoning as the fuzz_image_decode / fuzz_font_program
// self-checks, which abort when their gate is not reached.
//
// Deliberately NOT a C++ throw: the harnesses catch(...) around the call under
// test, and a throw would be swallowed by exactly the handler this is meant to
// escape.
[[noreturn]] inline void fail(const char* what) noexcept
{
    std::fprintf(stderr, "PDFE FUZZ INVARIANT VIOLATED: %s\n",
                 what ? what : "(null)");
    std::fflush(stderr);
    std::abort();
}

} // namespace pdfe::fuzz
