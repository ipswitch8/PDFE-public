// Tantivy C ABI bridge for PDFE.
//
// Exposes a minimal C API that the PDFE C++ core can call to build and
// query a per-document full-text index. All functions are `extern "C"`
// and use primitive types + opaque handles so the C++ side only needs
// to know about a few forward-declared pointer types.
//
// Thread-safety: each `TantivyIndexHandle` is single-threaded from the
// caller's perspective. The C++ wrapper owns the handle and serializes
// calls via the same discipline the rest of PDFE uses (document mutex).

use std::collections::HashMap;
use std::ffi::{c_char, CStr, CString};
use std::path::Path;
use std::ptr;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{LazyLock, Mutex};

use tantivy::collector::TopDocs;
use tantivy::query::{
    AllQuery, BooleanQuery, Occur, PhraseQuery, Query, QueryParser, RegexQuery,
    TermQuery,
};
use tantivy::schema::{Field, Schema, Value, STORED, STRING, TEXT};
use tantivy::{doc, Index, IndexReader, IndexWriter, ReloadPolicy, TantivyDocument, Term};

const WRITER_HEAP_BYTES: usize = 32_000_000;

#[repr(C)]
pub struct TantivyIndexHandle {
    index: Index,
    reader: IndexReader,
    writer: Mutex<IndexWriter>,
    page_field: Field,
    text_field: Field,
}

#[repr(C)]
pub struct TantivyHit {
    pub page_idx: i32,
    pub score: f32,
    /// Owned C string — freed when the enclosing TantivyResults is freed.
    pub snippet: *const c_char,
}

#[repr(C)]
pub struct TantivyResults {
    pub hits: *mut TantivyHit,
    pub count: i32,
    /// Internal: capacity of the `hits` allocation. Used by free.
    pub capacity: i32,
}

// ---------- helpers ----------

/// Detect a simple trailing-asterisk prefix query: single token, ends with
/// `*`, contains no spaces/quotes/operators. Anything more complex falls
/// through to the full QueryParser.
fn is_simple_prefix(q: &str) -> bool {
    if !q.ends_with('*') || q.len() < 2 {
        return false;
    }
    let body = &q[..q.len() - 1];
    !body
        .chars()
        .any(|c| c.is_whitespace() || matches!(c, '"' | '+' | '-' | ':' | '(' | ')' | '*'))
}

/// Regex-escape special characters so a user's prefix is matched literally.
fn regex_escape(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for c in s.chars() {
        match c {
            '.' | '+' | '*' | '?' | '(' | ')' | '[' | ']' | '{' | '}' | '|'
            | '^' | '$' | '\\' => {
                out.push('\\');
                out.push(c);
            }
            _ => out.push(c),
        }
    }
    out
}

fn c_str_to_str<'a>(s: *const c_char) -> Option<&'a str> {
    if s.is_null() {
        return None;
    }
    unsafe { CStr::from_ptr(s).to_str().ok() }
}

fn to_c_string(s: &str) -> *mut c_char {
    match CString::new(s) {
        Ok(cs) => cs.into_raw(),
        Err(_) => {
            // `CString::new` fails only on an interior NUL. Now that page text
            // is indexed with an explicit length, a document CAN put a U+0000
            // in indexed text and therefore in a snippet -- and returning NULL
            // here would mean the crafted page is found but shows no snippet,
            // i.e. the document still degrades the result it appears in.
            // Substitute the replacement character and carry on.
            let cleaned: String = s
                .chars()
                .map(|c| if c == '\0' { '\u{FFFD}' } else { c })
                .collect();
            match CString::new(cleaned) {
                Ok(cs) => cs.into_raw(),
                Err(_) => ptr::null_mut(),
            }
        }
    }
}

fn build_schema() -> (Schema, Field, Field) {
    let mut builder = Schema::builder();
    let page_field = builder.add_text_field("page", STRING | STORED);
    let text_field = builder.add_text_field("text", TEXT | STORED);
    (builder.build(), page_field, text_field)
}

fn open_or_create(path: &Path, schema: Schema) -> tantivy::Result<Index> {
    if path.exists() {
        match Index::open_in_dir(path) {
            Ok(ix) => return Ok(ix),
            Err(_) => {
                // Existing dir but not a valid index — fall through and
                // recreate. This matches the "rebuild stale index" story.
            }
        }
    }
    std::fs::create_dir_all(path).ok();
    Index::create_in_dir(path, schema)
}

fn make_snippet(full: &str, max_len: usize) -> String {
    if full.len() <= max_len {
        full.to_string()
    } else {
        let mut end = max_len;
        while end > 0 && !full.is_char_boundary(end) {
            end -= 1;
        }
        format!("{}…", &full[..end])
    }
}

// ---------- results-allocation registry ----------
//
// `TantivyResults` carries `count` and `capacity` across the FFI boundary, and
// the free path used to rebuild the allocation with
// `Vec::from_raw_parts(rs.hits, rs.count, rs.capacity)` -- i.e. it handed the
// global allocator a size and a capacity that had made a round trip through
// C++ memory the C++ side is free to write to. A wrong capacity is not a wrong
// answer, it is a wrong deallocation: the allocator is told the block is a
// different size than it is. A negative `count` is worse, because `as usize`
// turns it into an enormous length and the loop walks off the end freeing
// arbitrary pointers as CStrings.
//
// Validating the incoming numbers would only narrow the window. Instead the
// numbers are not trusted at all: Rust records what it actually allocated,
// keyed by the `TantivyResults` pointer it handed out, and the free path uses
// ITS OWN record. The struct's `count`/`capacity` become descriptive fields for
// the C++ reader and carry no authority over deallocation.
//
// This also makes two other failures safe rather than fatal:
//   * double free -- the second call finds no record and returns without
//     dereferencing the (already freed) pointer at all;
//   * a forged or stale pointer -- likewise refused, not dereferenced.
//
// Every refusal is COUNTED and the count is readable over the FFI
// (`pdfe_tantivy_free_results_anomalies`). Silently doing the right thing is
// not enough: if the protective behaviour leaves no observable trace, no test
// can tell "the guard worked" from "the guard was never needed", and the guard
// is decoration.

struct ResultAlloc {
    hits: usize, // *mut TantivyHit as usize
    len: usize,
    cap: usize,
}

static RESULTS_REGISTRY: LazyLock<Mutex<HashMap<usize, ResultAlloc>>> =
    LazyLock::new(|| Mutex::new(HashMap::new()));

static FREE_ANOMALIES: AtomicU64 = AtomicU64::new(0);

fn note_anomaly() {
    FREE_ANOMALIES.fetch_add(1, Ordering::SeqCst);
}

fn register_results(key: *mut TantivyResults, hits: *mut TantivyHit, len: usize, cap: usize) {
    if let Ok(mut m) = RESULTS_REGISTRY.lock() {
        m.insert(
            key as usize,
            ResultAlloc {
                hits: hits as usize,
                len,
                cap,
            },
        );
    } else {
        // Poisoned registry: the allocation stays live and is leaked rather
        // than being freed on numbers we cannot vouch for. Leaking is the safe
        // direction; it is recorded so it cannot pass unnoticed.
        note_anomaly();
    }
}

/// Which query path a given query string would take, WITHOUT running it.
/// 1 = the `RegexQuery` prefix fast path, 0 = the `QueryParser`.
///
/// This exists because the interesting security property of the prefix path is
/// invisible from the outside: a caller sees a result set either way and cannot
/// tell which engine produced it.
///
/// The property is that a raw user string NEVER reaches `RegexQuery` as a
/// regex. Two independent things ensure that, and this entry point is how a
/// test can check the first of them:
///   * `is_simple_prefix` refuses anything containing whitespace or
///     `" + - : ( ) *` before the trailing `*`, which excludes every classic
///     catastrophic-backtracking construction; and
///   * whatever survives is `regex_escape`d, so it is matched LITERALLY -- the
///     only regex construct in the final pattern is the `.*` this code appends
///     itself.
///
/// (The third reason a ReDoS cannot occur here is not testable from any API:
/// no backtracking engine is on this path at all. `RegexQuery` compiles the
/// pattern to a DFA and intersects it with the term dictionary's FST, so the
/// work is bounded by the dictionary rather than by the input.
///
/// Stating the dependency edge exactly, because an earlier draft of this
/// comment did not and the phase's own gate caught it: `Cargo.lock` shows
/// `tantivy 0.26.1 -> [regex 1.12.3, tantivy-fst 0.5.0]`,
/// `tantivy-fst -> [regex-syntax]`, and
/// `regex 1.12.3 -> [regex-automata 0.4.14, regex-syntax]`. There is NO direct
/// `tantivy -> regex-automata` edge. It does not change the conclusion: both
/// candidate engines are non-backtracking by construction -- `tantivy-fst`
/// builds its own automaton from a `regex-syntax` AST, and the Rust `regex`
/// crate guarantees linear-time matching with no backtracking mode.)
#[no_mangle]
pub extern "C" fn pdfe_tantivy_query_uses_regex_path(query_str: *const c_char) -> i32 {
    let q = match c_str_to_str(query_str) {
        Some(s) => s,
        None => return 0,
    };
    let trimmed = q.trim();
    if trimmed.is_empty() {
        return 0;
    }
    if is_simple_prefix(trimmed) {
        1
    } else {
        0
    }
}

/// The regex pattern the prefix fast path would build for `query_str`, written
/// into `out` (NUL-terminated, truncated to fit). Returns the number of bytes
/// the pattern needs including the terminator, or 0 if this query does not take
/// the regex path.
///
/// Lets a test assert the ESCAPING rather than infer it: that the pattern
/// contains the user's metacharacters backslash-escaped and exactly one
/// unescaped regex construct, the trailing `.*`.
#[no_mangle]
pub extern "C" fn pdfe_tantivy_prefix_pattern(
    query_str: *const c_char,
    out: *mut c_char,
    out_len: usize,
) -> usize {
    let q = match c_str_to_str(query_str) {
        Some(s) => s,
        None => return 0,
    };
    let trimmed = q.trim();
    if !is_simple_prefix(trimmed) {
        return 0;
    }
    let prefix = &trimmed[..trimmed.len() - 1];
    let pattern = format!("{}.*", regex_escape(&prefix.to_lowercase()));
    let needed = pattern.len() + 1;
    if out.is_null() || out_len == 0 {
        return needed;
    }
    let copy = std::cmp::min(pattern.len(), out_len - 1);
    unsafe {
        std::ptr::copy_nonoverlapping(pattern.as_ptr(), out as *mut u8, copy);
        *out.add(copy) = 0;
    }
    needed
}

/// Number of times the free path has refused to trust what it was given:
/// an unknown/double-freed pointer, or a `TantivyResults` whose fields no
/// longer describe the allocation Rust made. Monotonic for the process.
#[no_mangle]
pub extern "C" fn pdfe_tantivy_free_results_anomalies() -> u64 {
    FREE_ANOMALIES.load(Ordering::SeqCst)
}

// ---------- public C API ----------

/// Open or create an index at `index_dir` (UTF-8). Returns NULL on failure.
#[no_mangle]
pub extern "C" fn pdfe_tantivy_open(index_dir: *const c_char) -> *mut TantivyIndexHandle {
    let dir = match c_str_to_str(index_dir) {
        Some(s) => s,
        None => return ptr::null_mut(),
    };
    let (schema, page_field, text_field) = build_schema();
    let index = match open_or_create(Path::new(dir), schema) {
        Ok(ix) => ix,
        Err(_) => return ptr::null_mut(),
    };
    let writer = match index.writer(WRITER_HEAP_BYTES) {
        Ok(w) => w,
        Err(_) => return ptr::null_mut(),
    };
    let reader = match index
        .reader_builder()
        .reload_policy(ReloadPolicy::OnCommitWithDelay)
        .try_into()
    {
        Ok(r) => r,
        Err(_) => return ptr::null_mut(),
    };

    let handle = Box::new(TantivyIndexHandle {
        index,
        reader,
        writer: Mutex::new(writer),
        page_field,
        text_field,
    });
    Box::into_raw(handle)
}

/// Close an index. Commits any pending writes and drops the handle.
/// Safe to call with NULL.
#[no_mangle]
pub extern "C" fn pdfe_tantivy_close(h: *mut TantivyIndexHandle) {
    if h.is_null() {
        return;
    }
    unsafe {
        let h = Box::from_raw(h);
        if let Ok(mut w) = h.writer.lock() {
            let _ = w.commit();
        }
        drop(h.reader);
        drop(h.index);
    }
}

/// Shared body of both upsert entry points. `text_str` is already a `&str`,
/// so the NUL question has been settled by the caller.
fn upsert_page_impl(h: *mut TantivyIndexHandle, page_idx: i32, text_str: &str) -> i32 {
    if h.is_null() {
        return -1;
    }
    let handle = unsafe { &mut *h };
    let page_key = page_idx.to_string();

    let writer = match handle.writer.lock() {
        Ok(w) => w,
        Err(_) => return -2,
    };

    // Remove any existing docs for this page.
    let term = Term::from_field_text(handle.page_field, &page_key);
    writer.delete_term(term);

    let res = writer.add_document(doc!(
        handle.page_field => page_key,
        handle.text_field => text_str
    ));
    if res.is_err() {
        return -3;
    }
    0
}

/// Add or replace the text for `page_idx`, given an explicit byte length.
///
/// PREFER THIS over `pdfe_tantivy_upsert_page`. Page text comes from
/// `Page::extractPlainText()`, i.e. from the document, and a PDF may map a
/// glyph to U+0000. Encoded as UTF-8 that is a 0x00 byte in the middle of an
/// otherwise ordinary string. A NUL-terminated entry point stops there, so a
/// single crafted codepoint near the top of a page silently removes the whole
/// remainder of that page from the index -- the document decides what is
/// findable. There is no error and no observable symptom: search simply does
/// not return a page it should. That is why the length is passed explicitly.
///
/// Invalid UTF-8 is replaced lossily (U+FFFD) rather than dropped. The
/// NUL-terminated entry point's `to_str().ok()` yields `None` on invalid UTF-8
/// and indexes the page as EMPTY, which is the same silent-loss failure by
/// another route.
///
/// `text` may be NULL only if `text_len` is 0. Returns 0 on success.
#[no_mangle]
pub extern "C" fn pdfe_tantivy_upsert_page_len(
    h: *mut TantivyIndexHandle,
    page_idx: i32,
    text: *const c_char,
    text_len: usize,
) -> i32 {
    if h.is_null() {
        return -1;
    }
    let owned: String;
    let text_str: &str = if text_len == 0 {
        ""
    } else if text.is_null() {
        // A non-zero length with a NULL pointer is a caller bug; refuse rather
        // than construct a slice from NULL (which is UB even to form).
        return -4;
    } else {
        let bytes = unsafe { std::slice::from_raw_parts(text as *const u8, text_len) };
        match std::str::from_utf8(bytes) {
            Ok(s) => s,
            Err(_) => {
                owned = String::from_utf8_lossy(bytes).into_owned();
                &owned
            }
        }
    };
    upsert_page_impl(h, page_idx, text_str)
}

/// Add or replace the text for `page_idx` from a NUL-terminated string.
///
/// RETAINED FOR ABI COMPATIBILITY, AND IT TRUNCATES. Text stops at the first
/// embedded NUL, and invalid UTF-8 indexes the page as empty. Callers that
/// have a length -- which in this codebase is all of them, since the text
/// arrives as a `std::string` -- must use `pdfe_tantivy_upsert_page_len`.
#[no_mangle]
pub extern "C" fn pdfe_tantivy_upsert_page(
    h: *mut TantivyIndexHandle,
    page_idx: i32,
    text: *const c_char,
) -> i32 {
    if h.is_null() {
        return -1;
    }
    let text_str = c_str_to_str(text).unwrap_or("");
    upsert_page_impl(h, page_idx, text_str)
}

/// Delete the document for `page_idx`. Returns 0 on success.
#[no_mangle]
pub extern "C" fn pdfe_tantivy_delete_page(h: *mut TantivyIndexHandle, page_idx: i32) -> i32 {
    if h.is_null() {
        return -1;
    }
    let handle = unsafe { &mut *h };
    let writer = match handle.writer.lock() {
        Ok(w) => w,
        Err(_) => return -2,
    };
    let term = Term::from_field_text(handle.page_field, &page_idx.to_string());
    writer.delete_term(term);
    0
}

/// Commit pending writes to disk and refresh the reader.
/// Returns 0 on success.
#[no_mangle]
pub extern "C" fn pdfe_tantivy_commit(h: *mut TantivyIndexHandle) -> i32 {
    if h.is_null() {
        return -1;
    }
    let handle = unsafe { &mut *h };
    let mut writer = match handle.writer.lock() {
        Ok(w) => w,
        Err(_) => return -2,
    };
    if writer.commit().is_err() {
        return -3;
    }
    if handle.reader.reload().is_err() {
        return -4;
    }
    0
}

/// Total number of documents currently visible to the reader.
#[no_mangle]
pub extern "C" fn pdfe_tantivy_num_docs(h: *mut TantivyIndexHandle) -> i64 {
    if h.is_null() {
        return 0;
    }
    let handle = unsafe { &*h };
    handle.reader.searcher().num_docs() as i64
}

/// Execute a query. `query_str` uses Tantivy's QueryParser syntax:
///   plain:     words → implicit OR
///   boolean:   +must -must_not optional
///   phrase:    "exact phrase"
///   prefix:    foo*
///   field:     text:word
/// Returns a newly-allocated TantivyResults (caller must free with
/// pdfe_tantivy_free_results) or NULL on failure.
#[no_mangle]
pub extern "C" fn pdfe_tantivy_search(
    h: *mut TantivyIndexHandle,
    query_str: *const c_char,
    max_hits: i32,
) -> *mut TantivyResults {
    if h.is_null() {
        return ptr::null_mut();
    }
    let handle = unsafe { &*h };
    let q = c_str_to_str(query_str).unwrap_or("");
    let max = max_hits.max(1) as usize;

    let searcher = handle.reader.searcher();

    // Fast-path: empty query returns no hits.
    if q.trim().is_empty() {
        // NOTE: a NULL `hits` with capacity 0 used to be handed straight to
        // `Vec::from_raw_parts` on the free path. `from_raw_parts` requires a
        // pointer from the global allocator; NULL is not one, so that was UB by
        // contract even though a zero capacity frees nothing in practice. The
        // registry now carries the real numbers and the free path skips the
        // reconstruction entirely when there is no allocation.
        let results = Box::into_raw(Box::new(TantivyResults {
            hits: ptr::null_mut(),
            count: 0,
            capacity: 0,
        }));
        register_results(results, ptr::null_mut(), 0, 0);
        return results;
    }

    // Fast-path: single trailing-asterisk prefix like "run*" — the default
    // QueryParser in conjunctive mode doesn't handle `*` the way users expect,
    // so build a RegexQuery directly against the text field.
    let trimmed = q.trim();
    let query: Box<dyn Query> = if is_simple_prefix(trimmed) {
        let prefix = &trimmed[..trimmed.len() - 1];
        let pattern = format!("{}.*", regex_escape(&prefix.to_lowercase()));
        match RegexQuery::from_pattern(&pattern, handle.text_field) {
            Ok(r) => Box::new(r),
            Err(_) => return ptr::null_mut(),
        }
    } else {
        // Full parser: AND/OR/NOT, phrase, field-scoped, plus-minus operators.
        let mut parser = QueryParser::for_index(&handle.index, vec![handle.text_field]);
        parser.set_field_boost(handle.text_field, 1.0);
        parser.set_conjunction_by_default();
        match parser.parse_query(trimmed) {
            Ok(qq) => qq,
            Err(_) => {
                let _ = AllQuery; // suppress unused-import warning
                return ptr::null_mut();
            }
        }
    };

    // tantivy 0.26 split TopDocs from the Collector impl: the actual
    // Collector is produced by .order_by_score() (or the other builder
    // methods on TopDocs). The result type is unchanged: Vec<(Score, DocAddress)>.
    let collector = TopDocs::with_limit(max).order_by_score();
    let top_docs = match searcher.search(&*query, &collector) {
        Ok(docs) => docs,
        Err(_) => return ptr::null_mut(),
    };

    let mut hits: Vec<TantivyHit> = Vec::with_capacity(top_docs.len());
    for (score, doc_addr) in top_docs {
        let doc: TantivyDocument = match searcher.doc(doc_addr) {
            Ok(d) => d,
            Err(_) => continue,
        };

        let page_idx_str = doc
            .get_first(handle.page_field)
            .and_then(|v| v.as_str())
            .unwrap_or("-1");
        let page_idx: i32 = page_idx_str.parse().unwrap_or(-1);

        let text = doc
            .get_first(handle.text_field)
            .and_then(|v| v.as_str())
            .unwrap_or("");
        let snippet = make_snippet(text, 160);
        hits.push(TantivyHit {
            page_idx,
            score,
            snippet: to_c_string(&snippet),
        });
    }

    let len = hits.len();
    let cap = hits.capacity();
    let ptr = hits.as_mut_ptr();
    std::mem::forget(hits);

    let results = Box::into_raw(Box::new(TantivyResults {
        hits: ptr,
        count: len as i32,
        capacity: cap as i32,
    }));
    // The authoritative record. What the struct says about itself from here on
    // is the C++ side's business; the free path uses this.
    register_results(results, ptr, len, cap);
    results
}

/// The layout of the FFI structs AS RUST COMPUTES IT.
///
/// `TantivyHit` and `TantivyResults` are hand-mirrored in
/// `src/search/TantivyBridge.h`. `#[repr(C)]` guarantees each side follows C
/// layout rules, but nothing guarantees the two DECLARATIONS still agree: add a
/// field here, reorder two there, change an `i32` to an `i64`, and both sides
/// still compile, still link, and silently read each other's memory at the
/// wrong offsets. That is a silent-corruption bug with no failing assertion
/// anywhere -- exactly the shape this project keeps finding.
///
/// So Rust reports what it believes, and the C++ side asserts the two agree.
/// A one-sided edit then fails a test instead of corrupting a search result.
#[repr(C)]
pub struct TantivyLayout {
    pub hit_size: u32,
    pub hit_align: u32,
    pub hit_off_page_idx: u32,
    pub hit_off_score: u32,
    pub hit_off_snippet: u32,
    pub results_size: u32,
    pub results_align: u32,
    pub results_off_hits: u32,
    pub results_off_count: u32,
    pub results_off_capacity: u32,
}

/// Report Rust's view of the shared struct layout. Pure; no allocation.
#[no_mangle]
pub extern "C" fn pdfe_tantivy_layout() -> TantivyLayout {
    use std::mem::{align_of, offset_of, size_of};
    TantivyLayout {
        hit_size: size_of::<TantivyHit>() as u32,
        hit_align: align_of::<TantivyHit>() as u32,
        hit_off_page_idx: offset_of!(TantivyHit, page_idx) as u32,
        hit_off_score: offset_of!(TantivyHit, score) as u32,
        hit_off_snippet: offset_of!(TantivyHit, snippet) as u32,
        results_size: size_of::<TantivyResults>() as u32,
        results_align: align_of::<TantivyResults>() as u32,
        results_off_hits: offset_of!(TantivyResults, hits) as u32,
        results_off_count: offset_of!(TantivyResults, count) as u32,
        results_off_capacity: offset_of!(TantivyResults, capacity) as u32,
    }
}

/// Free results produced by `pdfe_tantivy_search`.
///
/// Safe against a double free, a pointer this crate never handed out, and a
/// `TantivyResults` whose `count`/`capacity`/`hits` fields have been altered
/// after the fact: the allocation is reconstructed from Rust's own record, not
/// from the struct. Anything that does not match is refused and counted --
/// see `pdfe_tantivy_free_results_anomalies`.
#[no_mangle]
pub extern "C" fn pdfe_tantivy_free_results(r: *mut TantivyResults) {
    if r.is_null() {
        return;
    }

    // Look the pointer up BEFORE dereferencing it. On a double free the
    // pointee is already gone, so reading `rs.hits` first -- as the previous
    // implementation did -- is itself the use-after-free.
    let rec = match RESULTS_REGISTRY.lock() {
        Ok(mut m) => m.remove(&(r as usize)),
        Err(_) => {
            note_anomaly();
            return;
        }
    };
    let rec = match rec {
        Some(rec) => rec,
        None => {
            // Unknown pointer: already freed, never issued, or forged.
            note_anomaly();
            return;
        }
    };

    unsafe {
        let rs = Box::from_raw(r);
        // Descriptive-vs-authoritative: if these disagree, the struct was
        // written to after we handed it over. We free what we allocated and
        // record that it happened.
        if rs.hits as usize != rec.hits
            || rs.count < 0
            || rs.count as usize != rec.len
            || rs.capacity < 0
            || rs.capacity as usize != rec.cap
        {
            note_anomaly();
        }

        if rec.hits != 0 {
            let hits =
                Vec::from_raw_parts(rec.hits as *mut TantivyHit, rec.len, rec.cap);
            for h in hits {
                if !h.snippet.is_null() {
                    let _ = CString::from_raw(h.snippet as *mut c_char);
                }
            }
        }
        // rs itself is dropped here
    }
}

// Helper: used by tests to force building with phrase/prefix through the
// typed API rather than parser strings. Tests in Rust-land; not part of
// the C ABI but must exist in the crate.
#[allow(dead_code)]
pub(crate) fn phrase_query_for_test(
    field: Field,
    terms: Vec<&str>,
) -> Box<dyn Query> {
    let pterms: Vec<Term> = terms
        .into_iter()
        .map(|t| Term::from_field_text(field, t))
        .collect();
    if pterms.len() == 1 {
        Box::new(TermQuery::new(
            pterms.into_iter().next().unwrap(),
            tantivy::schema::IndexRecordOption::WithFreqsAndPositions,
        ))
    } else {
        Box::new(PhraseQuery::new(pterms))
    }
}

#[allow(dead_code)]
pub(crate) fn bool_must(queries: Vec<Box<dyn Query>>) -> Box<dyn Query> {
    Box::new(BooleanQuery::new(
        queries.into_iter().map(|q| (Occur::Must, q)).collect(),
    ))
}
