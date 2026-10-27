// -*- coding: utf-8 -*-
// Pipeline b3d7f942 phase-1 remediation (GAP-16 AC3)
//
// Thin header that exposes pdfe::forms internals to GoogleTest suites.
// Include ONLY from test files — never from production headers.
//
// The declarations here must stay in sync with the definitions in
// src/forms/FormManager.cpp.

#pragma once

#include "forms/FormManager.h"   // for FormManager::firstWidgetObjForPageSeam
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

namespace pdfe::forms::detail {

/// Per-widget Tx reset helper extracted from FormManager::resetForm.
///
/// Applies the /DV → /V copy for a single text-field annotation object.
///
/// @param ctx      Live fz_context* (must not be null).
/// @param pdfDoc   The pdf_document* that owns the field.  May be null when
///                 fieldObj is null — the function returns before using it.
/// @param fieldObj The result of pdf_annot_obj() for the widget being reset;
///                 may be null (the null guard fires and false is returned).
///
/// @return false if fieldObj was null (guard fired, widget was skipped);
///         true  if fieldObj was non-null and the widget body was entered.
///
/// Tests use the bool return to verify which branch was taken, making the
/// null guard mechanically observable without relying on a crash.
bool resetTxWidgetFromObj(fz_context* ctx, pdf_document* pdfDoc, pdf_obj* fieldObj);

} // namespace pdfe::forms::detail

/// Convenience wrapper: get the first widget pdf_obj* for a page using the
/// FormManager::firstWidgetObjForPageSeam member (which has friend access to
/// Page::fzPage).  Returns nullptr if the page has no widgets.
inline pdf_obj* getFirstWidgetObjForPage(
    pdfe::core::MuPdfContext* ctx,
    const pdfe::core::Page* page)
{
    return static_cast<pdf_obj*>(
        pdfe::forms::FormManager::firstWidgetObjForPageSeam(ctx, page));
}
