// -*- coding: utf-8 -*-
// Phase-4 fuzz harness: AcroForm tree walk.
//
// Exercises FormManager::listFieldNames + per-page getFormFields.
// Targets the form-field cross-reference path (parent/child links,
// kids arrays, terminal field detection) which is where PDFE's
// MuPDF wrappers had to add null-checks for malformed acroforms.
#include "fuzz_common.h"

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "forms/FormManager.h"

#include <memory>

extern "C" void pdfe_fuzz_one_input(const std::uint8_t* data,
                                    std::size_t size)
{
    if (size == 0 || size > 4 * 1024 * 1024) return;
    pdfe::fuzz::TempPdfFile tmp(data, size);
    if (!tmp.valid()) return;

    try {
        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        auto doc = pdfe::core::Document::open(ctx, tmp.path());
        if (!doc) return;
        pdfe::forms::FormManager mgr(ctx);
        auto names = mgr.listFieldNames(doc.get());
        (void)names;
        const int n = doc->pageCount();
        for (int i = 0; i < n && i < 4; ++i) {
            auto p = doc->page(i);
            if (!p) continue;
            auto fields = mgr.getFormFields(p.get());
            (void)fields;
        }
    } catch (...) {
    }
}
