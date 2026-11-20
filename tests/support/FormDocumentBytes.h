// -*- coding: utf-8 -*-
#pragma once
//
// Shared helper: serialise the whole document a form widget is bound to.
//
// Single definition (previously duplicated in test_form_reconcile.cpp and
// test_form_textfield_model.cpp; see tools/gate/check_duplicate_test_definitions.py).

#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "forms/FormManager.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <cstddef>
#include <string>

namespace pdfe::testsupport {

// The whole document serialised with MuPDF's default write options. A
// rejected setter call must leave this byte-identical. Returns an empty string
// when the page has no widget (or the widget is not bound to a document).
inline std::string documentBytes(pdfe::core::MuPdfContext* ctx, const pdfe::core::Page* page) {
    pdf_obj* w = static_cast<pdf_obj*>(pdfe::forms::FormManager::firstWidgetObjForPageSeam(ctx, page));
    if (!w) return std::string();
    struct Out {
        fz_buffer* buf = nullptr;
        const unsigned char* data = nullptr;
        size_t len = 0;
    };
    Out out;
    Out* o = &out;
    ctx->execute([w, o](fz_context* c) {
        pdf_document* doc = pdf_get_bound_document(c, w);
        if (!doc) return;
        o->buf = fz_new_buffer(c, 1 << 16);
        fz_output* os = fz_new_output_with_buffer(c, o->buf);
        fz_try(c) {
            pdf_write_options opts = pdf_default_write_options;
            pdf_write_document(c, doc, os, &opts);
            fz_close_output(c, os);
        }
        fz_always(c) {
            fz_drop_output(c, os);
        }
        fz_catch(c) {
            fz_rethrow(c);
        }
        unsigned char* d = nullptr;
        o->len = fz_buffer_storage(c, o->buf, &d);
        o->data = d;
    });
    std::string s;
    if (out.data && out.len) s.assign(reinterpret_cast<const char*>(out.data), out.len);
    if (out.buf) {
        fz_buffer* b = out.buf;
        ctx->execute([b](fz_context* c) { fz_drop_buffer(c, b); });
    }
    return s;
}

} // namespace pdfe::testsupport
