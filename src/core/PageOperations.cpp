#include "PageOperations.h"
#include "Document.h"
#include "PageArtifactGraft.h"
#include "MuPdfContext.h"
#include "core/FormXObjectDepthGuard.h"
#include "Error.h"

#include <algorithm>
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

namespace pdfe::core {

// Pipeline E phase-6: CropBox helpers.

QRectF getPageCropBox(Document* doc, int pageIndex)
{
    if (!doc || !doc->fzDocument()) return {};
    if (pageIndex < 0 || pageIndex >= doc->pageCount()) return {};

    auto ctx = doc->context();
    fz_document* fzDoc = doc->fzDocument();

    return ctx->executeReturn<QRectF>([&](fz_context* c) -> QRectF {
        pdf_document* pdoc = pdf_document_from_fz_document(c, fzDoc);
        if (!pdoc) return {};
        pdf_page* page = safeLoadPdfPage(c, pdoc, pageIndex);
        if (!page) return {};
        QRectF result;
        fz_try(c) {
            // Read the page's raw /CropBox dict entry, in PDF user
            // space (Y-up). MuPDF's fz_bound_page_box(FZ_CROP_BOX)
            // returns post-crop fitz-space bounds (always rooted at
            // 0,0), which is not what callers want for a get/set
            // round-trip; reading the dict gives back exactly what
            // was stored.
            pdf_obj* pageObj = page->obj;
            pdf_obj* cropArr = pageObj
                ? pdf_dict_get_inheritable(c, pageObj, PDF_NAME(CropBox))
                : nullptr;
            // Fall back to /MediaBox when /CropBox is absent (PDF spec
            // default).
            if (!cropArr) {
                cropArr = pageObj
                    ? pdf_dict_get_inheritable(c, pageObj, PDF_NAME(MediaBox))
                    : nullptr;
            }
            if (cropArr && pdf_is_array(c, cropArr) &&
                pdf_array_len(c, cropArr) == 4) {
                const float x0 = pdf_to_real(c, pdf_array_get(c, cropArr, 0));
                const float y0 = pdf_to_real(c, pdf_array_get(c, cropArr, 1));
                const float x1 = pdf_to_real(c, pdf_array_get(c, cropArr, 2));
                const float y1 = pdf_to_real(c, pdf_array_get(c, cropArr, 3));
                const float left   = std::min(x0, x1);
                const float bottom = std::min(y0, y1);
                const float right  = std::max(x0, x1);
                const float top    = std::max(y0, y1);
                result = QRectF(left, bottom, right - left, top - bottom);
            }
        } fz_always(c) {
            fz_drop_page(c, &page->super);
        } fz_catch(c) {
            // Swallow and return empty; caller treats empty as "unknown".
        }
        return result;
    });
}

void setPageCropBox(Document* doc, int pageIndex, const QRectF& cropBox)
{
    if (!doc || !doc->fzDocument()) return;
    if (pageIndex < 0 || pageIndex >= doc->pageCount()) return;
    if (cropBox.isEmpty()) return;

    auto ctx = doc->context();
    fz_document* fzDoc = doc->fzDocument();

    // Write /CropBox directly to the page's pdf_obj dict in PDF user
    // space (Y-up, four floats: x0 y0 x1 y1), matching what
    // getPageCropBox reads. We treat the input QRectF as those four
    // raw floats — the get/set pair forms a stable round-trip without
    // dragging fitz-space conversion through every caller. Writing
    // via pdf_set_page_box would expect fitz-space input which makes
    // round-trips diverge by the page's translation.
    ctx->execute([&](fz_context* c) {
        pdf_document* pdoc = pdf_document_from_fz_document(c, fzDoc);
        if (!pdoc) return;
        pdf_page* page = safeLoadPdfPage(c, pdoc, pageIndex);
        if (!page) return;
        fz_try(c) {
            pdf_obj* pageObj = page->obj;
            if (!pageObj) {
                fz_throw(c, FZ_ERROR_GENERIC, "Page object is null");
            }
            pdf_obj* arr = pdf_new_array(c, pdoc, 4);
            pdf_array_push_real(c, arr,
                static_cast<float>(cropBox.left()));
            pdf_array_push_real(c, arr,
                static_cast<float>(cropBox.top()));
            pdf_array_push_real(c, arr,
                static_cast<float>(cropBox.right()));
            pdf_array_push_real(c, arr,
                static_cast<float>(cropBox.bottom()));
            pdf_dict_put_drop(c, pageObj, PDF_NAME(CropBox), arr);
        } fz_always(c) {
            fz_drop_page(c, &page->super);
        } fz_catch(c) {
            fz_rethrow(c);
        }
    });
    doc->markPageDirty(pageIndex);
}

void extractPagesToFile(
    std::shared_ptr<MuPdfContext> context,
    Document* sourceDoc,
    const std::vector<int>& pageIndices,
    const std::filesystem::path& outputPath
)
{
    // Validate inputs
    if (!context) {
        throw PdfeException("MuPdfContext is null");
    }
    if (!sourceDoc) {
        throw PdfeException("Source document is null");
    }
    if (!sourceDoc->isPdf()) {
        throw PdfeException("Source document is not a PDF");
    }
    if (pageIndices.empty()) {
        throw PdfeException("No pages specified for extraction");
    }

    // Validate all page indices are in range
    const int pageCount = sourceDoc->pageCount();
    auto badIdx = std::find_if(pageIndices.begin(), pageIndices.end(),
        [pageCount](int idx) { return idx < 0 || idx >= pageCount; });
    if (badIdx != pageIndices.end()) {
        throw PdfeException(
            "Page index out of range: " + std::to_string(*badIdx) +
            " (document has " + std::to_string(pageCount) + " pages)"
        );
    }

    // Get the source pdf_document* via friend access to fzDocument()
    fz_document* sourceFzDoc = sourceDoc->fzDocument();
    pdf_document* sourcePdfDoc = context->executeReturn<pdf_document*>(
        [sourceFzDoc](fz_context* ctx) -> pdf_document* {
            pdf_document* pdfDoc = pdf_specifics(ctx, sourceFzDoc);
            if (!pdfDoc) {
                // fz_throw, NOT a C++ throw: this lambda body runs inside
                // MuPdfContext::executeReturn's fz_try, which is setjmp-based
                // and not unwind-aware. A C++ exception thrown here leaves
                // MuPDF's error frame pushed. executeReturn's fz_catch turns
                // this into a MuPdfException for the caller either way.
                fz_throw(ctx, FZ_ERROR_ARGUMENT,
                         "Failed to get PDF specifics from source document");
            }
            return pdfDoc;
        }
    );

    // Create new PDF document and graft pages
    std::string outputPathStr = outputPath.string();
    // Tier 2 (pipeline b2e47d91): the single graft map already de-duplicates
    // resources copied into the extract, so the cheap default GC level
    // suffices for a clean source; effectiveGarbageLevel() escalates to 3 only
    // if the source session has a pending dedup-worthy op.
    const int gcLevel = sourceDoc->effectiveGarbageLevel();
    context->execute([context, sourcePdfDoc, &pageIndices, &outputPathStr, gcLevel](fz_context* ctx) {
        pdf_document* newDoc = nullptr;

        // Create new empty PDF
        newDoc = pdf_create_document(ctx);
        if (!newDoc) {
            // fz_throw, not a C++ throw -- see the note in the executeReturn
            // lambda above; this body runs inside MuPdfContext::execute's
            // fz_try.
            fz_throw(ctx, FZ_ERROR_GENERIC, "Failed to create new PDF document");
        }

        // newDoc used to be dropped only on the success path. Every call in
        // the guarded block below reports failure by longjmp, so a failed
        // graft or save leaked the whole document.
        //
        // fz_var is not strictly required here -- newDoc is assigned before
        // fz_try and never reassigned inside it, so its value cannot be lost
        // to the longjmp. It is kept because MuPDF's own rule is that any
        // local read from fz_always/fz_catch is fz_var'd, and because a later
        // edit that moves the assignment inside the block would otherwise
        // introduce a silent, optimisation-dependent bug.
        fz_var(newDoc);
        fz_try(ctx) {
            // F6 / PDF-88: pdf_graft_mapped_page copies only Contents,
            // Resources, the boxes, /Rotate and /UserUnit -- no annotations
            // and no catalog entry at all, so an extract used to lose the
            // outline, page labels, form fields and their VALUES, layers, the
            // structure tree, embedded files, named destinations and every
            // annotation. Graft the pages AND those artifacts, pruning
            // anything that pointed at a page that was not extracted.
            graftPagesWithArtifacts(ctx, newDoc, sourcePdfDoc, pageIndices, 0);

            // Save the new document with compression
            pdf_write_options opts{};
            opts.do_garbage = gcLevel;     // Tier 2 conditional GC (see above)
            opts.do_compress = 1;          // Compress streams
            // Out of scope for the pipeline 3f8a1c72 per-object recompression
            // review: same MuPDF /Filter guard applies (filterless-only Flate,
            // no lossy image re-encode, no font re-subset), and this is a fresh
            // grafted document where compressing newly-copied streams is the
            // intended behaviour.
            opts.do_compress_images = 1;   // Compress images
            opts.do_compress_fonts = 1;    // Compress fonts
            opts.do_preserve_metadata = 1; // Preserve document metadata

            pdf_save_document(ctx, newDoc, outputPathStr.c_str(), &opts);
        }
        fz_always(ctx) {
            // pdf_drop_document tolerates NULL and is the only owner of
            // newDoc, so this is the single release point for both paths.
            pdf_drop_document(ctx, newDoc);
        }
        fz_catch(ctx) {
            fz_rethrow(ctx);
        }
    });
}

} // namespace pdfe::core
