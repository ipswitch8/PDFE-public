// -*- coding: utf-8 -*-
// Backend wiring for the PDFE fuzz harnesses.
//
// libFuzzer / AFL++ unified entry point: each harness defines
// `void pdfe_fuzz_one_input(const uint8_t* data, size_t size);` -- the
// wiring below wraps that with the right backend depending on the
// PDFE_FUZZ_BACKEND macro (libfuzzer | aflpp).
//
// libFuzzer: clang-cl with -fsanitize=fuzzer; the binary calls
//   LLVMFuzzerTestOneInput per input. We forward to pdfe_fuzz_one_input.
//
// AFL++ persistent mode: shared-memory fuzzing harness. We use the
//   __AFL_LOOP / __AFL_FUZZ_TESTCASE_BUF / __AFL_FUZZ_TESTCASE_LEN
//   conventions documented in AFL++ persistent_mode.md.
//
// Why both: libFuzzer is the fast Windows path under clang-cl; AFL++ runs
// better in Linux CI and persistent-mode beats fork-mode on Windows where
// fork is unavailable.
//
// The backend-independent plumbing (TempPdfFile, the benign-throw triage
// classifier) lives in fuzz_support.h so it can also be reused from the
// GoogleTest smoke test without any backend macro / sanitizer runtime.
#pragma once

#include "fuzz_support.h"

#include <cstdint>
#include <cstddef>

extern "C" void pdfe_fuzz_one_input(const std::uint8_t* data, std::size_t size);


// ============================================================
// Backend wiring
// ============================================================

#if defined(PDFE_FUZZ_BACKEND_LIBFUZZER)

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size)
{
    pdfe_fuzz_one_input(data, size);
    return 0;
}

#elif defined(PDFE_FUZZ_BACKEND_AFLPP)

// AFL++ persistent mode -- the runtime sets up the shared-memory buffer via
// deferred-fork-server symbols. See AFL++ docs:
// https://aflplus.plus/docs/fuzzing_in_depth/#c-persistent-mode

extern "C" {
extern unsigned char* __AFL_FUZZ_TESTCASE_BUF;
extern unsigned int   __AFL_FUZZ_TESTCASE_LEN;
__attribute__((weak)) void __AFL_INIT(void);
__attribute__((weak)) int  __AFL_LOOP(unsigned int);
}

#  define __AFL_FUZZ_INIT() do {} while (0)

int main(int /*argc*/, char** /*argv*/)
{
    if (__AFL_INIT) __AFL_INIT();
    while (__AFL_LOOP && __AFL_LOOP(10000)) {
        std::size_t len = __AFL_FUZZ_TESTCASE_LEN;
        const std::uint8_t* buf = __AFL_FUZZ_TESTCASE_BUF;
        pdfe_fuzz_one_input(buf, len);
    }
    return 0;
}

#else
#  error "Define PDFE_FUZZ_BACKEND_LIBFUZZER or PDFE_FUZZ_BACKEND_AFLPP"
#endif
