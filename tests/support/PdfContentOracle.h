#pragma once
// Shared test-support truth oracles for PDF content-stream inspection.
//
// Used by redaction and injector tests that must assert a byte-level property
// (e.g. "the secret text operators are actually removed"). A raw ASCII grep of
// an on-disk PDF is blind to Flate-compressed streams -- these helpers
// DECOMPRESS the content before you search it.
//
// Header-only; include from a test .cpp (the tests target adds tests/ to its
// include path). Requires the MuPDF headers, which test .cpp files already pull
// in transitively via the linked pdfe_* libraries.

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <filesystem>
#include <string>

namespace pdfe::testsupport {

/// Concatenated, DECOMPRESSED bytes of a page's /Contents stream(s) from a PDF
/// on disk. Returns empty on any error.
inline std::string decompressedPageContent(const std::filesystem::path& pdf,
                                           int pageNum = 0) {
    std::string out;
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return out;
    fz_register_document_handlers(ctx);
    fz_document* fdoc = nullptr;
    fz_try(ctx) {
        fdoc = fz_open_document(ctx, pdf.string().c_str());
        pdf_document* doc = pdf_document_from_fz_document(ctx, fdoc);
        pdf_page* page = pdf_load_page(ctx, doc, pageNum);
        pdf_obj* contents = pdf_dict_get(ctx, page->obj, PDF_NAME(Contents));
        auto append = [&](pdf_obj* stm) {
            if (!stm) return;
            fz_buffer* b = pdf_load_stream(ctx, stm);
            if (!b) return;
            unsigned char* d = nullptr;
            size_t n = fz_buffer_storage(ctx, b, &d);
            if (d && n) out.append(reinterpret_cast<const char*>(d), n);
            fz_drop_buffer(ctx, b);
        };
        if (pdf_is_array(ctx, contents)) {
            int n = pdf_array_len(ctx, contents);
            for (int i = 0; i < n; ++i) append(pdf_array_get(ctx, contents, i));
        } else {
            append(contents);
        }
    }
    fz_always(ctx) { if (fdoc) fz_drop_document(ctx, fdoc); }
    fz_catch(ctx) {}
    fz_drop_context(ctx);
    return out;
}

/// True if `needle` appears in the decompressed content of the given page.
inline bool contentContains(const std::filesystem::path& pdf,
                            const std::string& needle, int pageNum = 0) {
    return decompressedPageContent(pdf, pageNum).find(needle) != std::string::npos;
}

} // namespace pdfe::testsupport
