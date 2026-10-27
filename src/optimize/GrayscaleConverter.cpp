#include "GrayscaleConverter.h"

#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include <mupdf/pdf/recolor.h>

namespace pdfe::optimize {

bool GrayscaleConverter::convertPageToGrayscale(pdfe::core::Document* doc,
                                                int pageIndex)
{
    if (!doc || !doc->fzDocument()) return false;
    if (pageIndex < 0 || pageIndex >= doc->pageCount()) return false;
    // Recoloring rewrites page content, orphaning the original color streams;
    // force a full-rewrite save (pipeline b2e47d91, Tier 1). This also covers
    // convertDocumentToGrayscale, which calls this per page.
    doc->markRequiresFullRewrite();

    auto ctx = doc->context();
    bool ok = false;
    ctx->execute([&](fz_context* c) {
        pdf_document* pdoc = pdf_document_from_fz_document(c, doc->fzDocument());
        if (!pdoc) return;
        pdf_recolor_options opts{};
        opts.num_comp = 1;  // DeviceGray
        fz_try(c) {
            pdf_recolor_page(c, pdoc, pageIndex, &opts);
            ok = true;
        } fz_catch(c) {
            // Swallow MuPDF errors; caller treats false as failure.
        }
    });
    if (ok) {
        doc->markPageDirty(pageIndex);
    }
    return ok;
}

GrayscaleConverter::Result GrayscaleConverter::convertDocumentToGrayscale(
    pdfe::core::Document* doc)
{
    Result r;
    if (!doc || !doc->fzDocument()) {
        r.errorMessage = "Document is null";
        return r;
    }
    const int n = doc->pageCount();
    if (n <= 0) {
        r.errorMessage = "Document has no pages";
        return r;
    }
    for (int i = 0; i < n; ++i) {
        if (convertPageToGrayscale(doc, i)) {
            ++r.pagesConverted;
        } else {
            ++r.pagesFailed;
        }
    }
    r.ok = (r.pagesFailed == 0);
    if (!r.ok) {
        r.errorMessage =
            "Some pages failed to recolor (" +
            std::to_string(r.pagesFailed) + " failed of " +
            std::to_string(n) + " total)";
    }
    return r;
}

} // namespace pdfe::optimize
