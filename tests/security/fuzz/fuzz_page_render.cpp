// -*- coding: utf-8 -*-
// Phase-4 fuzz harness: page render path.
//
// Exercises Document::open -> Page::render. Targets MuPDF's content-
// stream interpreter and the integration with PDFE's render hooks
// (fz_run_page -> pixmap). Page 0 only -- multi-page coverage comes
// from corpus diversity, not per-input loops.
#include "fuzz_common.h"

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QImage>
#include <QRectF>

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
        if (!doc || doc->pageCount() <= 0) return;
        auto page = doc->page(0);
        if (!page) return;
        // Tiny scale to keep allocation small; page parser still
        // walks the full content stream regardless.
        QImage img = page->render(QRectF(), 0.25f);
        (void)img;
    } catch (...) {
    }
}
