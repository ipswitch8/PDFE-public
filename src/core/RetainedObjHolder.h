#pragma once
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
namespace pdfe::core {
// Standalone holder for a retained pdf_obj across a full-rewrite save. The object
// is deep-copied into its OWN pdf_document so it is document-independent and
// survives Document::save() (drop+reopen) / saveAs() (GC-renumber). Mirrors
// Document::RetainedPageHolder (R2-01). Graft a COPY back into the live doc at
// restore time; drop the whole holder to release.
struct RetainedObjHolder { pdf_document* doc = nullptr; pdf_obj* obj = nullptr; };
inline void* makeRetainedObjHolder(fz_context* ctx, pdf_obj* src) {
    if (!src) return nullptr;
    pdf_document* holder = pdf_create_document(ctx);
    if (!holder) fz_throw(ctx, FZ_ERROR_GENERIC, "pdf_create_document failed");
    pdf_obj* copy = nullptr;
    fz_try(ctx) { copy = pdf_graft_object(ctx, holder, src); }
    fz_catch(ctx) { pdf_drop_document(ctx, holder); fz_rethrow(ctx); }
    return new RetainedObjHolder{holder, copy};
}
// Graft a COPY of the held obj into dst (live doc). Caller OWNS the returned ref
// and must pdf_dict_put it then pdf_drop_obj the local. NON-consuming.
inline pdf_obj* graftRetainedInto(fz_context* ctx, pdf_document* dst, void* holderVoid) {
    auto* h = static_cast<RetainedObjHolder*>(holderVoid);
    if (!h || !h->obj) return nullptr;
    return pdf_graft_object(ctx, dst, h->obj);
}
inline void dropRetainedObjHolder(fz_context* ctx, void* holderVoid) {
    if (!holderVoid) return;
    auto* h = static_cast<RetainedObjHolder*>(holderVoid);
    if (h->obj) pdf_drop_obj(ctx, h->obj);
    if (h->doc) pdf_drop_document(ctx, h->doc);
    delete h;
}
} // namespace pdfe::core
