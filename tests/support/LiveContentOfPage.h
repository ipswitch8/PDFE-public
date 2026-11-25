// -*- coding: utf-8 -*-
#pragma once
//
// Shared helper: the content-stream bytes of one page of a LIVE document.
//
// Single definition (previously duplicated in test_image_scan_background.cpp
// and test_image_background_select.cpp; see
// tools/gate/check_duplicate_test_definitions.py).

#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <cstddef>
#include <string>

namespace pdfe::testsupport {

// The page's content stream bytes (every stream of a /Contents array, each
// followed by a newline), read from a recovery copy of the live document
// (Document::writeRecoveryCopy is const and leaves the document as it was)
// with an independent MuPDF handle.
inline std::string liveContentOfPage(pdfe::core::Document* doc, const std::string& file,
                                     int index)
{
    doc->writeRecoveryCopy(file);
    std::string out;
    doc->context()->execute([&](fz_context* ctx) {
        pdf_document* pdf = pdf_open_document(ctx, file.c_str());
        pdf_page* pp = pdf_load_page(ctx, pdf, index);
        pdf_obj* c = pdf_dict_get(ctx, pp->obj, PDF_NAME(Contents));
        auto add = [&](pdf_obj* st) {
            fz_buffer* b = pdf_load_stream(ctx, st);
            unsigned char* d = nullptr;
            const size_t n = fz_buffer_storage(ctx, b, &d);
            if (d && n) out.append(reinterpret_cast<const char*>(d), n);
            out += '\n';
            fz_drop_buffer(ctx, b);
        };
        if (pdf_is_array(ctx, c)) {
            for (int i = 0, n = pdf_array_len(ctx, c); i < n; ++i) add(pdf_array_get(ctx, c, i));
        } else if (c) {
            add(c);
        }
        fz_drop_page(ctx, &pp->super);
        pdf_drop_document(ctx, pdf);
    });
    return out;
}

}  // namespace pdfe::testsupport
