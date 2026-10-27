// -*- coding: utf-8 -*-
#include "PdfSignatureProbe.h"

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include <set>

namespace pdfe::core {

namespace {

/// `/Kids` recursion cap, matching DocMdpEnforcer::collectFields.
constexpr int kMaxKidsDepth = 16;
/// `/Parent` chain cap, matching DocMdpEnforcer::fieldIsSignature.
constexpr int kMaxParentDepth = 32;

// ---------------------------------------------------------------------------
// EVERYTHING BELOW THIS LINE IS POD-ONLY BY CONSTRUCTION.
//
// These helpers run inside an fz_try frame whose setjmp target lives in
// countAppliedSignatures(). A longjmp out of them skips no destructor because
// they construct no object that has one — only raw pointers, ints and
// references to the caller's containers. The only non-trivial objects in this
// translation unit (the std::set and the int) are declared in the SAME frame as
// the fz_catch that receives the longjmp, so their destructors still run at
// normal scope exit.
// ---------------------------------------------------------------------------

/// Look `key` up on `node`, then up the `/Parent` chain. Depth-capped so a
/// cyclic chain (which a hostile file may author) terminates.
pdf_obj* inheritedGet(fz_context* ctx, pdf_obj* node, pdf_obj* key)
{
    for (int d = 0; node && d < kMaxParentDepth; ++d) {
        pdf_obj* v = pdf_dict_get(ctx, node, key);
        if (v) return v;
        node = pdf_dict_get(ctx, node, PDF_NAME(Parent));
    }
    return nullptr;
}

/// Does this `/V` carry an applied signature — a dictionary whose `/Contents`
/// is a NON-EMPTY string? An empty signature field has no `/ByteRange` and so
/// nothing a rewrite can invalidate.
bool valueIsAppliedSignature(fz_context* ctx, pdf_obj* v)
{
    if (!v) return false;
    pdf_obj* contents = pdf_dict_get(ctx, v, PDF_NAME(Contents));
    return contents != nullptr
        && pdf_is_string(ctx, contents)
        && pdf_to_str_len(ctx, contents) > 0;
}

/// De-duplication key: the object number of the `/V` signature value
/// dictionary. The AcroForm walk and the page-annotation walk reach the SAME
/// signature by two routes on a well-formed document, and it must be counted
/// once. Falls back to the field's own object number, and finally to 0 —
/// which the caller counts unconditionally, the over-warn direction.
int appliedIdentity(fz_context* ctx, pdf_obj* field, pdf_obj* v)
{
    const int vnum = pdf_to_num(ctx, v);
    if (vnum > 0) return vnum;
    return pdf_to_num(ctx, field);
}

void tallyField(fz_context* ctx, pdf_obj* field, std::set<int>& seen, int& anon)
{
    if (!pdf_is_dict(ctx, pdf_resolve_indirect(ctx, field))) return;
    pdf_obj* v = inheritedGet(ctx, field, PDF_NAME(V));
    if (!valueIsAppliedSignature(ctx, v)) return;
    const int id = appliedIdentity(ctx, field, v);
    if (id > 0) {
        seen.insert(id);
    } else {
        ++anon;
    }
}

void walkFieldTree(fz_context* ctx, pdf_obj* fields, int depth,
                   std::set<int>& seen, int& anon)
{
    if (!fields || !pdf_is_array(ctx, fields) || depth > kMaxKidsDepth) return;
    const int n = pdf_array_len(ctx, fields);
    for (int i = 0; i < n; ++i) {
        pdf_obj* field = pdf_array_get(ctx, fields, i);
        if (!field) continue;
        tallyField(ctx, field, seen, anon);
        walkFieldTree(ctx, pdf_dict_get(ctx, field, PDF_NAME(Kids)), depth + 1,
                      seen, anon);
    }
}

void walkPageAnnots(fz_context* ctx, pdf_obj* page, std::set<int>& seen,
                    int& anon)
{
    pdf_obj* annots = pdf_dict_get(ctx, page, PDF_NAME(Annots));
    if (!pdf_is_array(ctx, annots)) return;
    const int n = pdf_array_len(ctx, annots);
    for (int i = 0; i < n; ++i) {
        pdf_obj* aref = pdf_array_get(ctx, annots, i);
        pdf_obj* annot = pdf_resolve_indirect(ctx, aref);
        if (!pdf_is_dict(ctx, annot)) continue;
        if (!pdf_name_eq(ctx, pdf_dict_get(ctx, annot, PDF_NAME(Subtype)),
                         PDF_NAME(Widget))) {
            continue;
        }
        // Identity is taken from the REFERENCE (`aref`), not the resolved
        // dictionary, so it matches the number the AcroForm walk records for
        // the same merged field/widget object.
        tallyField(ctx, aref, seen, anon);
    }
}

} // namespace

bool pdfFieldIsSignature(fz_context* ctx, pdf_obj* field)
{
    pdf_obj* node = pdf_resolve_indirect(ctx, field);
    for (int depth = 0; node && depth < kMaxParentDepth; ++depth) {
        pdf_obj* ft = pdf_dict_get(ctx, node, PDF_NAME(FT));
        if (ft) return pdf_name_eq(ctx, ft, PDF_NAME(Sig)) != 0;
        node = pdf_dict_get(ctx, node, PDF_NAME(Parent));
    }
    return false;
}

int countAppliedSignatures(fz_context* ctx, pdf_document* doc)
{
    if (!ctx || !doc) return 0;

    // Declared HERE, in the frame that owns every fz_catch below, so no
    // longjmp can skip their destructors. See the POD note above.
    std::set<int> seen;
    int anon = 0;

    // --- Route 1: /Root /AcroForm /Fields, recursively through /Kids.
    fz_try(ctx) {
        pdf_obj* root =
            pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
        pdf_obj* acroForm = pdf_dict_get(ctx, root, PDF_NAME(AcroForm));
        walkFieldTree(ctx, pdf_dict_get(ctx, acroForm, PDF_NAME(Fields)), 0,
                      seen, anon);
    }
    fz_catch(ctx) {
        // A broken AcroForm must not suppress route 2 — that combination is
        // precisely the document this probe under-counted before.
    }

    // --- Route 2: every page's /Annots. Caught PER PAGE so one unparseable
    // page cannot lose the signatures on the others.
    int pageCount = 0;
    fz_try(ctx) {
        pageCount = pdf_count_pages(ctx, doc);
    }
    fz_catch(ctx) {
        pageCount = 0;
    }

    for (int p = 0; p < pageCount; ++p) {
        fz_try(ctx) {
            walkPageAnnots(ctx, pdf_lookup_page_obj(ctx, doc, p), seen, anon);
        }
        fz_catch(ctx) {
            // Next page.
        }
    }

    return static_cast<int>(seen.size()) + anon;
}

} // namespace pdfe::core
