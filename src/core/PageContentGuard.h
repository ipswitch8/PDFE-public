#pragma once

// Append a page-content fragment that assumes an IDENTITY CTM, isolating it from
// any ambient transform the existing page content left active.
//
// Image placement ops (insert/resize/rotate/crop, image watermark) emit an
// absolute "cm" that positions/scales the image assuming the CTM is identity
// when the fragment runs. But a page whose content leaves an ambient transform
// active at the top level (the "N-up / imposed" pattern, or a malformed
// unbalanced `cm`) makes the appended fragment inherit that transform, so the
// image is drawn half-size / shifted / flipped (Register H1 /
// docs/content-stream-state-continuity-audit.md, pipeline e5b9c231 Phase 2).
//
// A `cm` cannot RESET the CTM (it multiplies), so the fix brackets the existing
// content in `q ... Q`: the outer `q` (emitted as the very first content
// operator) saves the page-initial identity CTM, and the outer `Q` restores it
// unconditionally -- even if the bracketed content is internally unbalanced --
// so every appended fragment starts at identity.
//
// Idempotent: a sentinel comment on the leading `q` stream marks a page as
// already guarded, so repeated edits append their fragment after the existing
// `Q` (which already left the CTM at identity) instead of nesting another
// bracket. Each fragment is itself balanced (`q ... Q`), so the CTM stays at
// identity across any number of appended fragments.

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <cstring>
#include <string>

namespace pdfe::core {

// Sentinel marking the CTM-guard `q` stream. The trailing comment is ignored by
// PDF content parsers but lets us detect prior guarding.
inline const char* kCtmGuardStream() { return "q %PDFE-CTMGUARD\n"; }

inline bool streamStartsWithCtmGuard(fz_context* ctx, pdf_obj* streamObj)
{
    if (!streamObj) return false;
    const char* guard = kCtmGuardStream();
    const size_t glen = std::strlen(guard);
    fz_buffer* b = nullptr;
    bool match = false;
    fz_try(ctx) {
        b = pdf_load_stream(ctx, streamObj);
        if (b) {
            unsigned char* d = nullptr;
            size_t l = fz_buffer_storage(ctx, b, &d);
            if (d && l >= glen && std::memcmp(d, guard, glen) == 0) match = true;
        }
    }
    fz_catch(ctx) { match = false; }
    if (b) fz_drop_buffer(ctx, b);
    return match;
}

// Append `ops` (length `len`) to the page's /Contents so it runs at identity
// CTM regardless of ambient page state. `pageObj` is the page dictionary.
inline void appendContentIsolatingAmbientCtm(
    fz_context* ctx, pdf_document* doc, pdf_obj* pageObj,
    const char* ops, size_t len)
{
    // The fragment stream.
    fz_buffer* fbuf = fz_new_buffer_from_copied_data(
        ctx, reinterpret_cast<const unsigned char*>(ops), len);
    pdf_obj* frag = pdf_add_stream(ctx, doc, fbuf, nullptr, 0);
    fz_drop_buffer(ctx, fbuf);

    pdf_obj* contents = pdf_dict_get(ctx, pageObj, PDF_NAME(Contents));

    // No existing content: the page already starts at identity, so a bare
    // append is safe. Still normalise to an array for a consistent shape.
    if (!contents) {
        pdf_dict_put(ctx, pageObj, PDF_NAME(Contents), frag);
        return;
    }

    // Normalise to an array we can inspect/extend.
    pdf_obj* arr = contents;
    if (!pdf_is_array(ctx, contents)) {
        arr = pdf_new_array(ctx, doc, 4);
        pdf_array_push(ctx, arr, contents);
        pdf_dict_put(ctx, pageObj, PDF_NAME(Contents), arr);
    }

    const bool guarded =
        pdf_array_len(ctx, arr) > 0 &&
        streamStartsWithCtmGuard(ctx, pdf_array_get(ctx, arr, 0));

    if (guarded) {
        // The leading `q` guard + its matching `Q` already restore identity
        // before any appended fragment, and prior fragments are balanced, so
        // just append after them.
        pdf_array_push(ctx, arr, frag);
        return;
    }

    // First guard: bracket the existing content in `q ... Q`, then the fragment.
    const char* guard = kCtmGuardStream();
    fz_buffer* qbuf = fz_new_buffer_from_copied_data(
        ctx, reinterpret_cast<const unsigned char*>(guard), std::strlen(guard));
    pdf_obj* qStream = pdf_add_stream(ctx, doc, qbuf, nullptr, 0);
    fz_drop_buffer(ctx, qbuf);

    fz_buffer* QbufEnd = fz_new_buffer_from_copied_data(
        ctx, reinterpret_cast<const unsigned char*>("Q\n"), 2);
    pdf_obj* QStream = pdf_add_stream(ctx, doc, QbufEnd, nullptr, 0);
    fz_drop_buffer(ctx, QbufEnd);

    const int n = pdf_array_len(ctx, arr);
    pdf_obj* newArr = pdf_new_array(ctx, doc, n + 3);
    pdf_array_push(ctx, newArr, qStream);              // leading identity save
    for (int i = 0; i < n; ++i)
        pdf_array_push(ctx, newArr, pdf_array_get(ctx, arr, i));
    pdf_array_push(ctx, newArr, QStream);              // restore identity
    pdf_array_push(ctx, newArr, frag);                 // fragment at identity
    pdf_dict_put(ctx, pageObj, PDF_NAME(Contents), newArr);
}

} // namespace pdfe::core
