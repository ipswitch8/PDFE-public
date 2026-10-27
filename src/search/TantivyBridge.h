#pragma once
// Raw C ABI exported by the Rust tantivy_bridge staticlib. The RAII C++
// wrapper in TantivyIndex.h is what the rest of the codebase should use.

#include <cstddef>
#include <cstdint>

extern "C" {

struct TantivyIndexHandle;

struct TantivyHit {
    int32_t page_idx;
    float   score;
    const char* snippet;  // owned by the enclosing TantivyResults
};

struct TantivyResults {
    TantivyHit* hits;
    int32_t count;
    int32_t capacity;
};

TantivyIndexHandle* pdfe_tantivy_open(const char* index_dir);
void                pdfe_tantivy_close(TantivyIndexHandle* h);

/// NUL-terminated upsert. TRUNCATES at the first embedded NUL, and indexes the
/// page as empty on invalid UTF-8. Retained for ABI compatibility only —
/// use pdfe_tantivy_upsert_page_len.
int32_t pdfe_tantivy_upsert_page(TantivyIndexHandle* h, int32_t page_idx, const char* text);

/// Length-taking upsert. Page text is document-controlled, and a PDF may map a
/// glyph to U+0000 — a 0x00 byte in the middle of otherwise ordinary UTF-8. A
/// NUL-terminated entry point stops there, so one crafted codepoint near the
/// top of a page silently drops the rest of that page out of the index, with no
/// error and nothing observable except a search that fails to find text that is
/// plainly on the page. Pass the length.
int32_t pdfe_tantivy_upsert_page_len(TantivyIndexHandle* h, int32_t page_idx,
                                     const char* text, size_t text_len);
int32_t pdfe_tantivy_delete_page(TantivyIndexHandle* h, int32_t page_idx);
int32_t pdfe_tantivy_commit     (TantivyIndexHandle* h);
int64_t pdfe_tantivy_num_docs   (TantivyIndexHandle* h);

TantivyResults* pdfe_tantivy_search(TantivyIndexHandle* h,
                                    const char* query_str,
                                    int32_t max_hits);
void            pdfe_tantivy_free_results(TantivyResults* r);

/// How many times the free path has refused to trust the TantivyResults it was
/// handed — an unknown or already-freed pointer, or fields that no longer
/// describe the allocation Rust made. Monotonic for the process.
///
/// The free path deliberately does NOT use the struct's count/capacity to
/// rebuild the allocation (a tampered capacity is a wrong deallocation, not a
/// wrong answer); it uses Rust's own record. That correctness is invisible from
/// the outside, so it is counted here — otherwise no test can distinguish "the
/// guard held" from "the guard was never exercised".
uint64_t pdfe_tantivy_free_results_anomalies(void);

/// Which engine a query string would be routed to, without running it.
/// 1 = the RegexQuery prefix fast path, 0 = the QueryParser.
///
/// The routing is the security-relevant decision and is otherwise invisible —
/// a caller gets a result set either way. A raw user string never reaches
/// RegexQuery as a regex: is_simple_prefix refuses anything containing
/// whitespace or " + - : ( ) * before the trailing *, and whatever survives is
/// regex-escaped and therefore matched literally.
int32_t pdfe_tantivy_query_uses_regex_path(const char* query_str);

/// The regex pattern the prefix path would build for `query_str`, so a test can
/// assert the escaping directly instead of inferring it from a result set.
/// Returns bytes needed including the terminator, or 0 if this query does not
/// take the regex path.
size_t pdfe_tantivy_prefix_pattern(const char* query_str, char* out, size_t out_len);

/// Layout of the two shared structs as the RUST side computes it.
///
/// The structs above are a hand-written mirror of `#[repr(C)]` definitions in
/// `rust/tantivy_bridge/src/lib.rs`. `repr(C)` makes each side follow C layout
/// rules, but nothing makes the two DECLARATIONS agree: add a field on one
/// side, reorder two, widen an `int32_t`, and both sides still compile, still
/// link, and read each other's memory at the wrong offsets. There is no failing
/// assertion anywhere in that scenario — the search results are simply wrong.
///
/// So Rust reports what it believes and the C++ side checks it
/// (`SearchFfiLayout.*`). A one-sided edit fails a test instead of silently
/// corrupting results.
struct TantivyLayout {
    uint32_t hit_size;
    uint32_t hit_align;
    uint32_t hit_off_page_idx;
    uint32_t hit_off_score;
    uint32_t hit_off_snippet;
    uint32_t results_size;
    uint32_t results_align;
    uint32_t results_off_hits;
    uint32_t results_off_count;
    uint32_t results_off_capacity;
};

TantivyLayout pdfe_tantivy_layout(void);

} // extern "C"
