// -*- coding: utf-8 -*-
//
// FormXObjectDepthGuard.h — PDF-85 / GitHub issues #32 and #159.
//
// WHY THIS EXISTS
// ---------------
// A Form XObject may draw another Form XObject. Nothing in ISO 32000 bounds
// how deep that goes, and MuPDF renders it by RECURSION. MuPDF does carry its
// own guard, and for moderate depths it works: an over-deep document raises a
// catchable "exception stack overflow!" that fz_catch turns into an ordinary
// PDFE error.
//
// It stops working before the depth does. Past roughly a thousand levels the
// NATIVE stack is exhausted before MuPDF's counter reaches its limit, and a
// native stack overflow is not catchable here: fz_try/fz_catch is
// setjmp/longjmp, which cannot recover a stack that no longer exists. The
// process dies. tests/test_malicious_pdf_advanced.cpp measured exactly this at
// depth 4096 — the first version of that sweep killed the whole gtest binary
// and took every remaining test with it, which is why it now renders in a
// child process. GitHub #159 recorded the defect; this file is the fix.
//
// THE SHAPE OF THE FIX. The depth is a STATIC property of the document: it can
// be measured by walking `/Resources /XObject` dictionaries, which is
// iterative and costs no stack. So PDFE measures before it renders, and
// refuses a document whose nesting exceeds the bound with an ordinary
// exception. The render that would have died is never started.
//
// A BOUND THAT ONLY TRIPS ON THE ALREADY-CATCHABLE PATH WOULD CHANGE NOTHING.
// That is the trap this fix has to avoid: MuPDF already survives 64 and 512,
// so a guard set at, say, 2048 would look like a fix, pass a monotonicity
// test, and leave the process-killing case exactly as it was. The bound is set
// well BELOW where the native stack gives out — see maxFormXObjectDepth().
//
// WHY THE LOADERS BELOW EXIST, AND WHY EVERY CALLER MUST USE THEM
// ---------------------------------------------------------------
// The FIRST version of this fix guarded `Document::page()` and
// `Document::createDisplayList()` and was reviewed, gated and reported as
// complete. It was not: `src/` had **thirteen** places that load a page by
// index, and eleven of them were still fatal. The worst,
// `Document::pageInfo()`, is reached simply by OPENING a document — the page
// panel builds a thumbnail for every page — so the exact threat model in
// GitHub #159 ("open a hostile file and the process dies") was still live.
//
// That is the recorded lesson that fixing one instance of a pattern is not
// fixing the pattern. The response is structural rather than another round of
// sprinkled checks: there is now ONE guarded loader per MuPDF page-loading
// entry point, every call site in `src/` uses it, and
// `tests/test_page_load_guard_sites.cpp` re-derives the call-site list from
// the source on every run so a new unguarded site reddens instead of shipping.
//
// The refusal is `fz_throw`, not a C++ exception, because every one of those
// call sites is already inside an fz_try frame: a C++ throw crossing a
// setjmp frame is the documented hazard this codebase keeps hitting.
// `MuPdfContext::execute` converts it to `MuPdfException` (a `PdfeException`)
// at the C++ boundary, so callers see an ordinary error.

#pragma once

extern "C" {
typedef struct fz_context fz_context;
typedef struct fz_document fz_document;
typedef struct fz_page fz_page;
typedef struct pdf_document pdf_document;
typedef struct pdf_obj pdf_obj;
typedef struct pdf_page pdf_page;
}

namespace pdfe::core {

/// The maximum Form XObject nesting depth PDFE will attempt to render.
///
/// 256 is far above anything a real producer emits (nesting past a handful is
/// already pathological — a logo inside a header inside a template is three)
/// and far below where the native stack gives out on the supported platforms,
/// which is the property that matters: the bound must refuse the document
/// BEFORE the recursion that cannot be trapped begins. Setting it above
/// MuPDF's own catchable limit would produce a guard that never runs on the
/// case it exists for.
///
/// Exposed as a function rather than a constant in a header so a test reads
/// the value actually compiled into the library instead of re-declaring it.
int maxFormXObjectDepth();

/// Maximum Form XObject nesting depth reachable from `resources`.
///
/// Depth 0 means "no Form XObject at all"; a page that draws one plain form is
/// depth 1. Traversal is ITERATIVE (an explicit work list), so measuring a
/// hostile document costs no native stack — measuring it recursively would
/// reproduce the very crash this exists to prevent.
///
/// Returns as soon as `limit` is exceeded, reporting `limit + 1`. The caller
/// only needs to know whether the bound was passed, and stopping early keeps a
/// deliberately wide, deeply-branching document from costing O(nodes^depth).
///
/// Cycle-safe: `/X0` may resolve to a form whose own resources name it again.
/// Object numbers already on the current path are not re-entered, so a cycle
/// terminates instead of counting to infinity.
///
/// Null-safe; a null or non-dictionary `resources` answers 0. MUST be called
/// from inside a MuPdfContext execute()/executeReturn() scope: it calls into
/// MuPDF, which reports failure with fz_throw.
int measureFormXObjectDepth(fz_context* ctx, pdf_obj* resources, int limit);

/// Would loading page `pageIndex` of `doc` recurse deeper than the bound?
///
/// Reads the page dictionary with `pdf_lookup_page_obj`, which does NOT load
/// the page — the whole point, since loading is the thing that crashes. Page
/// resources are INHERITABLE, so they are fetched that way; a page that
/// inherits them from an ancestor node would otherwise measure as depth 0 and
/// walk straight into the crash.
///
/// A pure query that refuses nothing. Use it when the right answer to an
/// over-deep page is something other than an error — `RedactionVerifier` is
/// the one such caller, because a verifier that cannot inspect a page must
/// report "not verified", not throw and not silently skip.
///
/// Answers false for a null document or an out-of-range index: those are the
/// caller's own errors and this function must not redefine them.
bool formXObjectDepthExceeded(fz_context* ctx, pdf_document* doc,
                              int pageIndex);

/// `fz_load_page`, refusing an over-deep page first.
///
/// **Use this instead of `fz_load_page` everywhere in `src/`.** A non-PDF
/// document has no Form XObjects and is loaded unchecked.
///
/// @throws (fz_throw, FZ_ERROR_LIMIT) when the page is over-deep.
fz_page* safeLoadPage(fz_context* ctx, fz_document* doc, int pageIndex);

/// `pdf_load_page`, refusing an over-deep page first.
///
/// **Use this instead of `pdf_load_page` everywhere in `src/`.**
///
/// @throws (fz_throw, FZ_ERROR_LIMIT) when the page is over-deep.
pdf_page* safeLoadPdfPage(fz_context* ctx, pdf_document* doc, int pageIndex);

}  // namespace pdfe::core
