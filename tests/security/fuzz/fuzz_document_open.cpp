// -*- coding: utf-8 -*-
// Phase-4 fuzz harness: Document::open + dtor.
//
// Exercises the lifecycle path that PDFE invokes for every File>Open:
//   MuPdfContext ctor -> Document::open(ctx, path) -> ~Document
// MuPDF's PDF parser is the heavy lifting here -- this harness is
// the broadest of the six and catches stream-parser crashes.
#include "fuzz_common.h"

#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <memory>

extern "C" void pdfe_fuzz_one_input(const std::uint8_t* data,
                                    std::size_t size)
{
    if (size == 0 || size > 4 * 1024 * 1024) {
        // Bound input size; >4 MB is unlikely to find new edges in
        // 5-min smoke window and bloats fuzzer corpora.
        return;
    }
    pdfe::fuzz::TempPdfFile tmp(data, size);
    if (!tmp.valid()) return;

    try {
        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        auto doc = pdfe::core::Document::open(ctx, tmp.path());
        // Document goes out of scope -> destructor exercises
        // teardown which has historically been crash-prone for
        // malformed xref recovery.
        (void)doc;
    } catch (const std::exception& e) {
        // R2-17: triage, do NOT blanket-swallow. Genuine MuPDF/PDFE parse
        // refusals are the expected path and are swallowed; but a std::bad_alloc
        // (a corrupted length driving an absurd allocation) or any non-parse
        // exception is rethrown so libFuzzer/ASan records it. A true SEH
        // access-violation / heap-corruption is not a C++ exception under the
        // default /EH model, so it is not caught here at all -> it crashes and
        // is recorded, exactly as we want.
        pdfe::fuzz::triage_or_rethrow(e);
    }
    // NOTE: intentionally NO `catch (...)` — an unknown non-std C++ throw is
    // not part of the expected refuse path and must escape to be recorded.
}
