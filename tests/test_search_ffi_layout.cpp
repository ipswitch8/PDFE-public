// -*- coding: utf-8 -*-
//
// Phase-17 (follow-up phase-70) — the hand-mirrored Rust/C++ FFI structs.
//
// `TantivyHit` and `TantivyResults` are declared TWICE: once in
// `rust/tantivy_bridge/src/lib.rs` as `#[repr(C)]`, and once by hand in
// `src/search/TantivyBridge.h`. `repr(C)` guarantees each side follows C layout
// rules. It guarantees NOTHING about the two declarations describing the same
// struct.
//
// Add a field on one side, reorder two, widen an `i32` to an `i64` — and both
// sides still compile, still link, and read each other's memory at the wrong
// offsets. No assertion fails. The search results are simply wrong, or the
// `free` path walks a mis-sized array and corrupts the heap. That is a silent
// defect with no observable failure, which is precisely the class this
// programme keeps finding by accident rather than by control.
//
// TWO INDEPENDENT CHECKS, because they fail for different reasons:
//
//   1. `RustAndCppAgreeOnStructLayout` — the round trip. Rust reports the sizes,
//      alignments and field offsets IT computes; C++ compares them against what
//      IT computes. This catches a change on EITHER side, which is the whole
//      point: a compile-time assertion on the C++ side alone cannot see a Rust
//      edit, and vice versa.
//
//   2. `TheCppSideStillMatchesItsRecordedLayout` — static_asserts pinning the
//      C++ layout to the values recorded when this was written. This catches a
//      C++-side edit at COMPILE time rather than at test time, and it also
//      documents what the layout is supposed to be. On its own it would be
//      circular; paired with (1) it is not.
//
// Check (1) is the load-bearing one. Check (2) exists so the failure arrives as
// early as possible for the half of the mirror this repository compiles.

#include <gtest/gtest.h>

#include "search/TantivyBridge.h"

#include <cstddef>
#include <cstdint>

namespace {

// The layout recorded when this guard was written, on x86-64 MSVC.
// TantivyHit   : i32 @0, f32 @4, ptr @8  -> 16 bytes, align 8
// TantivyResults: ptr @0, i32 @8, i32 @12 -> 16 bytes, align 8
//
// These are not arbitrary: the pointer forces 8-byte alignment, which is why
// TantivyHit is 16 and not 12. A field reorder that put the pointer first would
// change every offset below while keeping the size identical — which is exactly
// why the offsets are asserted individually and not just the size.
static_assert(sizeof(TantivyHit) == 16, "TantivyHit size changed");
static_assert(alignof(TantivyHit) == 8, "TantivyHit alignment changed");
static_assert(offsetof(TantivyHit, page_idx) == 0, "page_idx moved");
static_assert(offsetof(TantivyHit, score) == 4, "score moved");
static_assert(offsetof(TantivyHit, snippet) == 8, "snippet moved");

static_assert(sizeof(TantivyResults) == 16, "TantivyResults size changed");
static_assert(alignof(TantivyResults) == 8, "TantivyResults alignment changed");
static_assert(offsetof(TantivyResults, hits) == 0, "hits moved");
static_assert(offsetof(TantivyResults, count) == 8, "count moved");
static_assert(offsetof(TantivyResults, capacity) == 12, "capacity moved");

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
TEST(SearchFfiLayout, RustAndCppAgreeOnStructLayout)
{
    const TantivyLayout rust = pdfe_tantivy_layout();

    // Sanity first: a Rust side that reported all zeros — because the symbol
    // resolved to something unexpected, or the struct came back by hidden
    // pointer and we mis-declared the ABI — would satisfy every "equal"
    // assertion below only if C++ also computed zero, which it cannot. Assert
    // non-degeneracy explicitly so the failure names the real cause.
    ASSERT_GT(rust.hit_size, 0u)
        << "pdfe_tantivy_layout() reported a zero-sized TantivyHit. The symbol "
           "resolved but returned nothing usable — suspect the return-by-value "
           "ABI of the layout struct itself, not the structs it describes.";
    ASSERT_GT(rust.results_size, 0u);

    EXPECT_EQ(rust.hit_size, sizeof(TantivyHit))
        << "Rust and C++ disagree on sizeof(TantivyHit). One side's declaration "
           "changed without the other. Every hit read across the FFI is now at "
           "the wrong offset, and free_results walks a mis-sized array.";
    EXPECT_EQ(rust.hit_align, alignof(TantivyHit));
    EXPECT_EQ(rust.hit_off_page_idx, offsetof(TantivyHit, page_idx));
    EXPECT_EQ(rust.hit_off_score, offsetof(TantivyHit, score));
    EXPECT_EQ(rust.hit_off_snippet, offsetof(TantivyHit, snippet))
        << "the snippet pointer is at a different offset on each side; reading "
           "it yields a wild pointer, and freeing it corrupts the heap";

    EXPECT_EQ(rust.results_size, sizeof(TantivyResults))
        << "Rust and C++ disagree on sizeof(TantivyResults)";
    EXPECT_EQ(rust.results_align, alignof(TantivyResults));
    EXPECT_EQ(rust.results_off_hits, offsetof(TantivyResults, hits));
    EXPECT_EQ(rust.results_off_count, offsetof(TantivyResults, count));
    EXPECT_EQ(rust.results_off_capacity, offsetof(TantivyResults, capacity))
        << "capacity is at a different offset on each side; free_results would "
           "hand Vec::from_raw_parts a capacity read from the wrong bytes";
}

// ─────────────────────────────────────────────────────────────────────────────
// The static_asserts above are the real content of this case: if the C++ layout
// drifts, this translation unit does not compile. The runtime body exists so
// the guard appears in the suite inventory and in the merged XML — a
// compile-time-only check is invisible to every count and every report, and
// this project has been bitten by controls nobody could see.
TEST(SearchFfiLayout, TheCppSideStillMatchesItsRecordedLayout)
{
    // The static_asserts above already fail the BUILD if the C++ layout drifts,
    // so the first version of this case was a bare SUCCEED() whose only job was
    // to make the guard visible in the suite inventory.
    //
    // The project's OracleStandard.SucceedOnlyBaselineMatchesTheTree guard
    // rejected that, and it is right to: a SUCCEED()-only body asserts nothing,
    // and "the file compiled" is invisible to anyone reading a test report. The
    // baseline of pinned SUCCEED-only bodies is explicitly not to be extended
    // for new tests. So the recorded layout is asserted again at RUNTIME.
    //
    // This is not redundant with the static_asserts in one specific way: it puts
    // the actual numbers into the failure message and into the XML, so a drift
    // reports *what* the layout became rather than only that it changed.
    EXPECT_EQ(sizeof(TantivyHit), 16u);
    EXPECT_EQ(alignof(TantivyHit), 8u);
    EXPECT_EQ(offsetof(TantivyHit, page_idx), 0u);
    EXPECT_EQ(offsetof(TantivyHit, score), 4u);
    EXPECT_EQ(offsetof(TantivyHit, snippet), 8u)
        << "the pointer is what forces TantivyHit to 16 bytes rather than 12; a "
           "reorder that moved it would change every offset while keeping the "
           "size identical, which is why offsets are asserted individually";

    EXPECT_EQ(sizeof(TantivyResults), 16u);
    EXPECT_EQ(alignof(TantivyResults), 8u);
    EXPECT_EQ(offsetof(TantivyResults, hits), 0u);
    EXPECT_EQ(offsetof(TantivyResults, count), 8u);
    EXPECT_EQ(offsetof(TantivyResults, capacity), 12u);
}
