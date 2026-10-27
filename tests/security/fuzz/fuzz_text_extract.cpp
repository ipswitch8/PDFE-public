// -*- coding: utf-8 -*-
// Phase-4 fuzz harness: text-extraction pipeline.
//
// Exercises Page::extractPlainText, which goes through MuPDF's
// fz_stext_page builder. Targets glyph-table malformation, CID
// encoding issues, and PDFE's UTF-8 normalization code path.
#include "fuzz_common.h"

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <memory>
#include <string>

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
        // Walk a few pages -- multi-page fuzz coverage matters here
        // because content-stream variation is per-page.
        const int n = doc->pageCount();
        for (int i = 0; i < n && i < 4; ++i) {
            auto p = doc->page(i);
            if (!p) continue;
            std::string text = p->extractPlainText();
            (void)text;
        }
    } catch (...) {
    }
}
