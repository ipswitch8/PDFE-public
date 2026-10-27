// -*- coding: utf-8 -*-
// Phase-4 fuzz harness: annotation enumeration.
//
// Exercises AnnotationManager::getAnnotations on every page.
// Targets PDFE's annotation parsing wrappers around pdf_first_annot
// / pdf_next_annot, which historically had dangling-pointer issues
// with certain Popup / FileAttachment / 3D annotations.
#include "fuzz_common.h"

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "annotations/AnnotationManager.h"

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
        pdfe::annotations::AnnotationManager mgr(ctx);
        const int n = doc->pageCount();
        for (int i = 0; i < n && i < 4; ++i) {
            auto p = doc->page(i);
            if (!p) continue;
            auto annots = mgr.getAnnotations(p.get());
            (void)annots;
        }
    } catch (...) {
    }
}
