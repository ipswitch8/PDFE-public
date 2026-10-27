// -*- coding: utf-8 -*-
// Phase-73 fuzz harness: PKCS#7 / CMS SignedData verification path.
//
// THIS IS A PROMOTION, NOT A NEW HARNESS. tests/test_pkcs7_fuzz.cpp has carried
// an `LLVMFuzzerTestOneInput` since pipeline e4c71f3a phase-2, and its own file
// header said so plainly: "on Windows/MSVC the libFuzzer entry point compiles
// but is not linked as a fuzzer target". It was compiled into the GoogleTest
// binary, where nothing calls it, and no fuzz target linked it -- so for the
// whole of its existence it was a coverage-guided harness that had never been
// coverage-guided. The seed-replay gtest beside it was real and stayed real;
// the fuzzer was a declaration.
//
// See docs/audit/dca/0037 for why that shape survives review: an entry point
// with the right signature in a file with "fuzz" in its name reads as coverage
// to every reader who does not check the link line.
//
// The body lives in pkcs7_fuzz_core.h and is shared verbatim with the gtest, so
// the seed-replay oracle and this campaign cannot drift apart.
#include "fuzz_common.h"

#include "pkcs7_fuzz_core.h"

#include <cstddef>
#include <cstdint>

extern "C" void pdfe_fuzz_one_input(const std::uint8_t* data,
                                    std::size_t size)
{
    if (size == 0 || size > (1u << 20)) return;

    const auto r = pdfe::fuzz::pkcs7::probe(data, size);

    // OUTPUT INVARIANT (the same one the gtest asserts): a successfully-decoded
    // PKCS7 must carry a resolvable content-type OID. d2i must never hand back
    // a typeless object on success, because PKCS7_verify would dereference it.
    if (r.parsed && !r.hasContentType) {
        pdfe::fuzz::fail("d2i_PKCS7 succeeded but produced no content-type OID");
    }
}
