#pragma once

// Document-level artifact preservation for cross-document page grafting.
//
// `pdf_graft_mapped_page` deliberately copies "as few key/value pairs as we
// can" -- Contents, Resources, the boxes, Rotate and UserUnit -- and nothing
// else. It copies no /Annots, no /StructParents, and touches no catalog entry
// at all. Any feature built directly on it therefore produces an output that
// has lost every document-level artifact of the source:
//
//   * page annotations (including form-field widgets),
//   * the outline (/Outlines),
//   * page labels (/PageLabels),
//   * interactive form fields and their VALUES (/AcroForm),
//   * optional content / layers (/OCProperties),
//   * the tagged-PDF structure tree (/StructTreeRoot),
//   * embedded files and document JavaScript (/Names),
//   * named destinations (/Names /Dests).
//
// `graftPagesWithArtifacts` grafts the requested pages AND carries those
// artifacts across, pruning or remapping anything that referenced a page that
// was not grafted. That pruning is the point: a blind catalog copy produces
// dangling destinations, orphaned structure elements and form fields whose
// widgets live on pages that no longer exist, which is its own defect.
//
// Used by `extractPagesToFile` (F6/PDF-88) and by
// `Document::graftPagesPreservingArtifacts`, which backs Combine Files
// (F6/PDF-89).

#include <vector>

// Forward declarations for MuPDF C types (global scope, C linkage).
extern "C" {
struct fz_context;
struct pdf_document;
}

namespace pdfe::core {

/// Graft `srcPageIndices` (0-based, in the given order) from `src` into `dst`,
/// inserting at `dstInsertAt`, carrying page annotations, form fields and
/// document-level catalog artifacts with them.
///
/// Behaviour of the artifact carry-over:
///  - Outline items whose destination page was not grafted are dropped, unless
///    they still have surviving descendants, in which case the item is kept and
///    its dangling destination removed.
///  - Named destinations (/Names /Dests) pointing at non-grafted pages are
///    dropped. On a merge, a name that already exists in `dst` is imported under
///    a `_2`, `_3`, ... suffix and link annotations from `src` are rewritten to
///    the new name, so no reference is left ambiguous.
///  - Form fields are rebuilt from the widgets that were actually grafted, so
///    no field keeps a /Kids entry for a widget on a dropped page. Field VALUES
///    (/V, /DV) are carried. Signature values are NOT carried: a signature over
///    a different page set can never be valid.
///  - The structure tree is copied with subtrees orphaned to dropped pages
///    pruned. It is attached only when `dst` has no /StructTreeRoot of its own;
///    merging two structure trees additionally requires renumbering both
///    /ParentTree number trees and every page's /StructParents key, which is
///    not attempted here (see docs/audit/meta-audit-2026-07/followup/
///    phase-6-dispositions.md).
///
/// Must be called from inside a MuPdfContext::execute()/executeReturn() lambda.
/// Throws via `fz_throw` only (never a C++ exception) so it is longjmp-safe.
void graftPagesWithArtifacts(::fz_context* ctx,
                             ::pdf_document* dst,
                             ::pdf_document* src,
                             const std::vector<int>& srcPageIndices,
                             int dstInsertAt);

} // namespace pdfe::core
