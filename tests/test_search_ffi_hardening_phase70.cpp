// -*- coding: utf-8 -*-
//
// Phase-17 (follow-up phase-70) — two hazards on the Rust/Tantivy FFI boundary.
//
// 1. EMBEDDED NUL (criterion 7). Page text comes from
//    `Page::extractPlainText()` — i.e. from the document. A PDF may map a glyph
//    to U+0000, which encodes to a 0x00 byte sitting in the middle of otherwise
//    ordinary UTF-8. The upsert used to be handed `text.c_str()`, so indexing
//    stopped at that byte. One crafted codepoint near the top of a page removed
//    the rest of that page from the index: no error, no diagnostic, nothing
//    observable except a search that fails to find text plainly on the page.
//    The document decided what was findable.
//
// 2. THE FREE PATH TRUSTING ITS OWN ARGUMENT (criterion 5).
//    `pdfe_tantivy_free_results` used to rebuild the allocation with
//    `Vec::from_raw_parts(rs.hits, rs.count, rs.capacity)` — three values that
//    had made a round trip through memory the C++ side owns and may write. A
//    wrong capacity is not a wrong answer, it is a wrong deallocation. A
//    negative count is worse: `as usize` makes it enormous and the loop walks
//    off the end freeing arbitrary pointers as CStrings.
//
//    The fix does not validate those numbers — it stops using them. Rust
//    records what it allocated and the free path consults its own record.
//
// WHY THERE IS AN ANOMALY COUNTER. Doing the right thing silently is not
// enough here: if the protection leaves no observable trace, no test can tell
// "the guard held" from "the guard was never reached", and the guard is
// decoration. So each refusal is counted and the count is readable over the
// FFI. Every case below asserts a DELTA and is paired with a control that
// asserts the delta is zero on the ordinary path — without that pairing a
// counter that increments on everything would pass just as well.

#include <gtest/gtest.h>

#include "search/TantivyBridge.h"
#include "search/TantivyIndex.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

using namespace pdfe::search;

namespace {

std::filesystem::path freshDir(const char* name)
{
    auto dir = std::filesystem::temp_directory_path() / "pdfe_ffi_hardening" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

// "alphaword\0bravoword" — built so the NUL is genuinely interior and the
// tail is a distinct, searchable token.
std::string textWithInteriorNul()
{
    std::string s = "alphaword";
    s.push_back('\0');
    s += " bravoword";
    return s;
}

bool containsPage(const std::vector<SearchHit>& hits, int page)
{
    return std::any_of(hits.begin(), hits.end(),
                       [&](const SearchHit& h) { return h.pageIndex == page; });
}

// RAII holder so a failed assertion mid-test cannot leak the index handle.
struct RawIndex {
    TantivyIndexHandle* h = nullptr;
    explicit RawIndex(const std::filesystem::path& dir)
        : h(pdfe_tantivy_open(dir.string().c_str())) {}
    ~RawIndex() { if (h) pdfe_tantivy_close(h); }
    RawIndex(const RawIndex&) = delete;
    RawIndex& operator=(const RawIndex&) = delete;
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Criterion 7 — embedded NUL
// ─────────────────────────────────────────────────────────────────────────────

TEST(SearchFfiNulHandling, TextAfterAnEmbeddedNulIsStillIndexed)
{
    auto dir = freshDir("nul_indexed");
    TantivyIndex idx(dir);

    ASSERT_TRUE(idx.upsertPage(0, textWithInteriorNul()));
    ASSERT_TRUE(idx.commit());

    // The head is found either way — it precedes the NUL. Asserting it first
    // separates "the tail was lost" from "nothing was indexed at all", which
    // are different failures with different causes.
    EXPECT_TRUE(containsPage(idx.search("alphaword", 10), 0))
        << "text BEFORE the NUL is missing too — this is not the truncation "
           "defect, the page was not indexed at all";

    EXPECT_TRUE(containsPage(idx.search("bravoword", 10), 0))
        << "text AFTER the embedded NUL was not indexed. upsertPage is passing "
           "a NUL-terminated pointer again (text.c_str() instead of "
           "text.data()/text.size()), so a document can hide its own content "
           "from search by planting a U+0000 glyph.";
}

// CALIBRATION, NOT A REQUIREMENT.
//
// This asserts the hazard is real by exercising the legacy NUL-terminated
// entry point directly. Without it, the case above could pass for a reason
// having nothing to do with the fix — if tantivy happened to index past the
// NUL anyway, the oracle would be vacuous and would stay green even if
// upsertPage regressed to c_str().
//
// It pins OLD behaviour deliberately. If `pdfe_tantivy_upsert_page` is ever
// given a length of its own, DELETE this case — do not "fix" it. It is a
// measurement of the entry point this fix routes around, not a demand that the
// entry point stay broken.
TEST(SearchFfiNulHandling, CalibrationTheNulTerminatedEntryPointDoesLoseTheTail)
{
    auto dir = freshDir("nul_calibration");
    RawIndex ix(dir);
    ASSERT_NE(ix.h, nullptr);

    const std::string text = textWithInteriorNul();
    ASSERT_EQ(pdfe_tantivy_upsert_page(ix.h, 0, text.c_str()), 0);
    ASSERT_EQ(pdfe_tantivy_commit(ix.h), 0);

    TantivyResults* head = pdfe_tantivy_search(ix.h, "alphaword", 10);
    ASSERT_NE(head, nullptr);
    EXPECT_GT(head->count, 0) << "even the pre-NUL text is missing; this "
                                 "calibration is measuring the wrong thing";
    pdfe_tantivy_free_results(head);

    TantivyResults* tail = pdfe_tantivy_search(ix.h, "bravoword", 10);
    ASSERT_NE(tail, nullptr);
    EXPECT_EQ(tail->count, 0)
        << "the NUL-terminated entry point indexed past the embedded NUL. If "
           "that is now intentional, delete this calibration case — but check "
           "first that TextAfterAnEmbeddedNulIsStillIndexed can still fail.";
    pdfe_tantivy_free_results(tail);
}

TEST(SearchFfiNulHandling, InvalidUtf8IsIndexedLossilyRatherThanDroppingThePage)
{
    auto dir = freshDir("bad_utf8");
    RawIndex ix(dir);
    ASSERT_NE(ix.h, nullptr);

    // A lone 0x80 continuation byte is not valid UTF-8. The NUL-terminated
    // entry point's to_str().ok() yields None for this and indexes the page as
    // EMPTY — the same silent total loss as truncation, by another route.
    std::string text = "charlieword ";
    text.push_back(static_cast<char>(0x80));
    text += " deltaword";

    ASSERT_EQ(pdfe_tantivy_upsert_page_len(ix.h, 0, text.data(), text.size()), 0);
    ASSERT_EQ(pdfe_tantivy_commit(ix.h), 0);

    TantivyResults* r = pdfe_tantivy_search(ix.h, "deltaword", 10);
    ASSERT_NE(r, nullptr);
    EXPECT_GT(r->count, 0)
        << "a page containing one invalid byte was dropped from the index "
           "entirely; from_utf8_lossy should have preserved the valid text";
    pdfe_tantivy_free_results(r);
}

TEST(SearchFfiNulHandling, NullTextWithZeroLengthIsAcceptedAndNonZeroLengthRefused)
{
    auto dir = freshDir("nul_ptr_args");
    RawIndex ix(dir);
    ASSERT_NE(ix.h, nullptr);

    EXPECT_EQ(pdfe_tantivy_upsert_page_len(ix.h, 0, nullptr, 0), 0)
        << "an empty page is legitimate and must index as empty text";
    EXPECT_NE(pdfe_tantivy_upsert_page_len(ix.h, 1, nullptr, 16), 0)
        << "a NULL pointer with a non-zero length must be refused, not turned "
           "into a slice — forming that slice is UB before anything reads it";
}

// ─────────────────────────────────────────────────────────────────────────────
// Criterion 5 — the free path
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Produce a real result set with at least one hit, so the free path has an
// actual allocation (and an owned snippet) to reclaim.
TantivyResults* searchWithHits(TantivyIndexHandle* h)
{
    TantivyResults* r = pdfe_tantivy_search(h, "echoword", 10);
    return r;
}

void seed(TantivyIndexHandle* h)
{
    const std::string text = "echoword foxtrotword golfword";
    ASSERT_EQ(pdfe_tantivy_upsert_page_len(h, 0, text.data(), text.size()), 0);
    ASSERT_EQ(pdfe_tantivy_commit(h), 0);
}

} // namespace

// THE CONTROL. Every case below asserts the anomaly counter moved; this one
// asserts it does NOT move on the ordinary path. A counter that incremented
// unconditionally would satisfy the others and fail this.
TEST(SearchFfiFreeResults, OrdinaryFreeReportsNoAnomaly)
{
    auto dir = freshDir("free_control");
    RawIndex ix(dir);
    ASSERT_NE(ix.h, nullptr);
    seed(ix.h);

    TantivyResults* r = searchWithHits(ix.h);
    ASSERT_NE(r, nullptr);
    ASSERT_GT(r->count, 0) << "no hits — this case would not exercise the "
                              "allocation-reclaiming path at all";

    const uint64_t before = pdfe_tantivy_free_results_anomalies();
    pdfe_tantivy_free_results(r);
    EXPECT_EQ(pdfe_tantivy_free_results_anomalies(), before)
        << "the untampered free path reported an anomaly; the counter is "
           "firing on correct behaviour, which makes every other case here "
           "meaningless";
}

TEST(SearchFfiFreeResults, TamperedCapacityIsIgnoredAndReported)
{
    auto dir = freshDir("free_capacity");
    RawIndex ix(dir);
    ASSERT_NE(ix.h, nullptr);
    seed(ix.h);

    TantivyResults* r = searchWithHits(ix.h);
    ASSERT_NE(r, nullptr);
    ASSERT_GT(r->count, 0);

    // Exactly the write the old implementation would have obeyed: it passed
    // this straight to Vec::from_raw_parts, telling the allocator the block
    // was a size it is not.
    r->capacity = 0x7FFFFFFF;

    const uint64_t before = pdfe_tantivy_free_results_anomalies();
    pdfe_tantivy_free_results(r);
    EXPECT_EQ(pdfe_tantivy_free_results_anomalies(), before + 1)
        << "a capacity that does not describe the allocation was accepted "
           "without complaint — the free path is trusting its argument again";
}

TEST(SearchFfiFreeResults, TamperedNegativeCountIsIgnoredAndReported)
{
    auto dir = freshDir("free_count");
    RawIndex ix(dir);
    ASSERT_NE(ix.h, nullptr);
    seed(ix.h);

    TantivyResults* r = searchWithHits(ix.h);
    ASSERT_NE(r, nullptr);
    ASSERT_GT(r->count, 0);

    // `-1 as usize` is 18446744073709551615. Under the old implementation the
    // loop would have walked that many TantivyHit-sized steps off the end,
    // calling CString::from_raw on whatever it found.
    r->count = -1;

    const uint64_t before = pdfe_tantivy_free_results_anomalies();
    pdfe_tantivy_free_results(r);
    EXPECT_EQ(pdfe_tantivy_free_results_anomalies(), before + 1)
        << "a negative count was accepted; sign is not being checked and the "
           "struct is still authoritative over the free";
}

TEST(SearchFfiFreeResults, DoubleFreeIsRefusedAndReported)
{
    auto dir = freshDir("free_double");
    RawIndex ix(dir);
    ASSERT_NE(ix.h, nullptr);
    seed(ix.h);

    TantivyResults* r = searchWithHits(ix.h);
    ASSERT_NE(r, nullptr);

    pdfe_tantivy_free_results(r);

    // Deliberate second free of a now-dangling pointer. This is safe ONLY
    // because the registry is consulted before the pointer is dereferenced;
    // the old code read r->hits first, which is itself the use-after-free.
    const uint64_t before = pdfe_tantivy_free_results_anomalies();
    pdfe_tantivy_free_results(r);
    EXPECT_EQ(pdfe_tantivy_free_results_anomalies(), before + 1)
        << "the second free was not recognised as one; either the record was "
           "not removed on the first free, or the pointer is being "
           "dereferenced before it is looked up";
}

TEST(SearchFfiFreeResults, NullAndUnknownPointersAreHandledDistinctly)
{
    // NULL is a legitimate no-op — callers may free unconditionally — so it
    // must NOT be counted as an anomaly. An unknown non-NULL pointer must be.
    const uint64_t beforeNull = pdfe_tantivy_free_results_anomalies();
    pdfe_tantivy_free_results(nullptr);
    EXPECT_EQ(pdfe_tantivy_free_results_anomalies(), beforeNull)
        << "freeing NULL was counted as an anomaly; that is an ordinary no-op "
           "and counting it would bury the real signal in noise";

    // A pointer this crate never issued. Never dereferenced, so the address
    // itself is never read — only compared.
    auto* bogus = reinterpret_cast<TantivyResults*>(static_cast<uintptr_t>(0xDEADBEEF));
    const uint64_t beforeBogus = pdfe_tantivy_free_results_anomalies();
    pdfe_tantivy_free_results(bogus);
    EXPECT_EQ(pdfe_tantivy_free_results_anomalies(), beforeBogus + 1)
        << "a pointer that was never issued was accepted for freeing";
}

TEST(SearchFfiFreeResults, EmptyResultSetFreesCleanlyAndWithoutAnomaly)
{
    auto dir = freshDir("free_empty");
    RawIndex ix(dir);
    ASSERT_NE(ix.h, nullptr);
    seed(ix.h);

    // The empty-query fast path returns hits=NULL, count=0, capacity=0 — which
    // the old free path handed to Vec::from_raw_parts. NULL is not a pointer
    // from the global allocator, so that was UB by contract even though a zero
    // capacity deallocates nothing in practice.
    TantivyResults* r = pdfe_tantivy_search(ix.h, "   ", 10);
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(r->count, 0);
    ASSERT_EQ(r->hits, nullptr);

    const uint64_t before = pdfe_tantivy_free_results_anomalies();
    pdfe_tantivy_free_results(r);
    EXPECT_EQ(pdfe_tantivy_free_results_anomalies(), before)
        << "the legitimate empty result set was flagged; the registry is not "
           "recording the no-allocation case";
}

TEST(SearchFfiFreeResults, SearchResultsAreStillCorrectAfterTheHardening)
{
    // The guard above is only worth having if the ordinary path still works.
    auto dir = freshDir("free_still_works");
    TantivyIndex idx(dir);
    ASSERT_TRUE(idx.upsertPage(3, "hotelword indiaword julietword"));
    ASSERT_TRUE(idx.commit());

    auto hits = idx.search("indiaword", 10);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].pageIndex, 3);
    EXPECT_GT(hits[0].score, 0.0f);
    EXPECT_NE(hits[0].snippet.find("indiaword"), std::string::npos)
        << "the snippet no longer carries the matched text";
}

TEST(SearchFfiFreeResults, ASnippetContainingANulStillArrivesNonEmpty)
{
    // Now that a NUL survives into indexed text, it can reach a snippet.
    // CString::new rejects an interior NUL, and returning NULL there would
    // mean the crafted page is found but shows nothing — the document
    // degrading the result it appears in. The NUL is replaced instead.
    auto dir = freshDir("snippet_nul");
    TantivyIndex idx(dir);

    std::string text = "kiloword";
    text.push_back('\0');
    text += " limaword";
    ASSERT_TRUE(idx.upsertPage(0, text));
    ASSERT_TRUE(idx.commit());

    auto hits = idx.search("limaword", 10);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_FALSE(hits[0].snippet.empty())
        << "the snippet came back empty for a page whose text contains a NUL; "
           "to_c_string is returning NULL instead of substituting";
    EXPECT_NE(hits[0].snippet.find("kiloword"), std::string::npos);
}
