#include "GrayscaleConverter.h"

#include "core/Document.h"
#include "core/FormXObjectDepthGuard.h"
#include "core/MuPdfContext.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include <mupdf/pdf/recolor.h>

#include <cstddef>
#include <memory>
#include <vector>

namespace pdfe::optimize {

namespace {

// GitHub #345 — safety oracle for the grayscale-blackening bug.
//
// Renders `page` to a small DeviceGray raster and returns the fraction of
// pixels that are LIGHT (gray value > 200), i.e. 0.0 .. 1.0. A negative
// return means the render itself failed (which, post-recolor, is itself a
// corruption signal).
//
// Why light-pixel fraction: a legitimate grayscale conversion preserves
// luminance — a white/light-paper background stays white, colored text/vectors
// become mid-grays. The #345 corruption instead collapses NON-image content to
// solid black, so a page that was mostly light loses essentially all of its
// light area. Comparing this fraction before and after the recolor therefore
// distinguishes "converted correctly" from "blackened", independently of the
// exact MuPDF recolor internals that trip on an inserted image's SMask/alpha
// or awkward colorspace.
//
// Pure fz (no C++ objects crossing the fz_try, no ctx->execute re-entry) — it
// is called with the raw fz_context already held by the enclosing execute().
double measureLightFraction(fz_context* c, fz_page* page)
{
    double frac = -1.0;
    fz_pixmap* pix = nullptr;
    fz_var(pix);
    fz_try(c) {
        fz_rect b = fz_bound_page(c, page);
        float w = b.x1 - b.x0;
        float h = b.y1 - b.y0;
        float maxdim = (w > h) ? w : h;
        float scale = (maxdim > 0.0f) ? (150.0f / maxdim) : 1.0f;
        if (scale > 1.0f) scale = 1.0f;
        fz_matrix m = fz_scale(scale, scale);
        // alpha = 0 → opaque pixmap cleared to white, so a transparent/erased
        // background reads as light (as it appears on screen over paper).
        pix = fz_new_pixmap_from_page(c, page, m, fz_device_gray(c), 0);
        if (pix && pix->w > 0 && pix->h > 0) {
            const int n = pix->n;
            const int stride = pix->stride;
            const unsigned char* s = pix->samples;
            long long total = static_cast<long long>(pix->w) * pix->h;
            long long light = 0;
            for (int y = 0; y < pix->h; ++y) {
                const unsigned char* row = s + static_cast<std::size_t>(y) * stride;
                for (int x = 0; x < pix->w; ++x) {
                    if (row[static_cast<std::size_t>(x) * n] > 200) {
                        ++light;
                    }
                }
            }
            frac = (total > 0)
                ? static_cast<double>(light) / static_cast<double>(total)
                : -1.0;
        }
    }
    fz_always(c)
        fz_drop_pixmap(c, pix);
    fz_catch(c)
        frac = -1.0;
    return frac;
}

// GitHub #355 — teardown-safe snapshot of a page's original /Contents and
// /Resources. Deep-copies both into its OWN standalone pdf_document so the
// holder survives a full-rewrite save (drop+reopen) or saveAs (GC-renumber) of
// the live document, and pins the MuPdfContext so release needs no live
// Document (defect B2). Mirrors Document::RetainedPageHolder /
// ImageManager::RetainedImageObjHolder.
struct GrayContentHolder {
    pdf_document* doc = nullptr;      // standalone holder document
    pdf_obj* contents = nullptr;      // deep copy in `doc` (may be null)
    pdf_obj* resources = nullptr;     // deep copy in `doc` (may be null)
    std::shared_ptr<pdfe::core::MuPdfContext> context;  // pins ctx for B2 drop
};

} // namespace

std::vector<void*> GrayscaleConverter::snapshotPageContent(
    pdfe::core::Document* doc)
{
    std::vector<void*> out;
    if (!doc || !doc->fzDocument()) return out;
    const int n = doc->pageCount();
    if (n <= 0) return out;

    auto ctxShared = doc->context();
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        void* holderPtr = nullptr;
        ctxShared->execute([&](fz_context* c) {
            pdf_document* pdoc =
                pdf_document_from_fz_document(c, doc->fzDocument());
            if (!pdoc) return;

            pdf_page* ppage = nullptr;
            pdf_document* holderDoc = nullptr;
            pdf_obj* cCopy = nullptr;
            pdf_obj* rCopy = nullptr;
            bool ok = false;
            fz_var(ppage);
            fz_var(holderDoc);
            fz_var(cCopy);
            fz_var(rCopy);
            fz_var(ok);

            fz_try(c) {
                ppage = pdfe::core::safeLoadPdfPage(c, pdoc, i);  // #159: guard page load
                pdf_obj* contents =
                    pdf_dict_get(c, ppage->obj, PDF_NAME(Contents));
                pdf_obj* resources =
                    pdf_dict_get(c, ppage->obj, PDF_NAME(Resources));
                holderDoc = pdf_create_document(c);
                if (!holderDoc) {
                    fz_throw(c, FZ_ERROR_GENERIC,
                             "pdf_create_document failed");
                }
                if (contents)  cCopy = pdf_graft_object(c, holderDoc, contents);
                if (resources) rCopy = pdf_graft_object(c, holderDoc, resources);
                ok = true;
            }
            fz_always(c) {
                if (ppage) fz_drop_page(c, &ppage->super);
            }
            fz_catch(c) {
                if (holderDoc) { pdf_drop_document(c, holderDoc); holderDoc = nullptr; }
                ok = false;
            }

            // Build the C++ holder OUTSIDE the setjmp region (no C++ object
            // construction may be skipped by a longjmp).
            if (ok && holderDoc) {
                auto* h = new GrayContentHolder{};
                h->doc = holderDoc;
                h->contents = cCopy;
                h->resources = rCopy;
                h->context = ctxShared;
                holderPtr = h;
            }
        });
        out.push_back(holderPtr);
    }
    return out;
}

void GrayscaleConverter::restorePageContent(
    pdfe::core::Document* doc, const std::vector<void*>& snapshot)
{
    if (!doc || !doc->fzDocument()) return;
    auto ctxShared = doc->context();
    const int n = doc->pageCount();
    const int count = static_cast<int>(snapshot.size());
    for (int i = 0; i < n && i < count; ++i) {
        auto* h = static_cast<GrayContentHolder*>(snapshot[static_cast<std::size_t>(i)]);
        if (!h) continue;
        ctxShared->execute([&](fz_context* c) {
            pdf_document* pdoc =
                pdf_document_from_fz_document(c, doc->fzDocument());
            if (!pdoc) return;
            pdf_page* ppage = nullptr;
            fz_var(ppage);
            fz_try(c) {
                ppage = pdfe::core::safeLoadPdfPage(c, pdoc, i);  // #159: guard page load
                // Graft a COPY of each held object into the LIVE document and
                // install it, then drop the local ref (the dict keeps its own).
                if (h->contents) {
                    pdf_obj* live = pdf_graft_object(c, pdoc, h->contents);
                    pdf_dict_put(c, ppage->obj, PDF_NAME(Contents), live);
                    pdf_drop_obj(c, live);
                }
                if (h->resources) {
                    pdf_obj* live = pdf_graft_object(c, pdoc, h->resources);
                    pdf_dict_put(c, ppage->obj, PDF_NAME(Resources), live);
                    pdf_drop_obj(c, live);
                }
            }
            fz_always(c) {
                if (ppage) fz_drop_page(c, &ppage->super);
            }
            fz_catch(c) {
                // Leave the page as-is on failure; the caller's oracle will
                // observe content that did not revert.
            }
        });
        doc->markPageDirty(i);
    }
    // Reinstating the original streams orphans the recolored ones; force a full
    // rewrite on the next save (same rationale as convertPageToGrayscale).
    doc->markRequiresFullRewrite();
}

void GrayscaleConverter::releaseSnapshot(std::vector<void*>& snapshot)
{
    for (void* p : snapshot) {
        auto* h = static_cast<GrayContentHolder*>(p);
        if (!h) continue;
        if (h->doc && h->context) {
            pdf_document* hd = h->doc;
            h->context->execute([hd](fz_context* c) {
                pdf_drop_document(c, hd);
            });
        }
        delete h;
    }
    snapshot.clear();
}

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

        pdf_page* ppage = nullptr;
        pdf_obj* savedContents = nullptr;
        pdf_obj* savedResources = nullptr;
        fz_var(ppage);
        fz_var(savedContents);
        fz_var(savedResources);

        fz_try(c) {
            ppage = pdfe::core::safeLoadPdfPage(c, pdoc, pageIndex);  // #159: guard page load

            // Snapshot the page's /Contents and /Resources BEFORE the recolor.
            // pdf_recolor_page → pdf_filter_page_contents always installs a
            // *new* /Contents object and a *new* /Resources dict (verified in
            // MuPDF 1.26.10 pdf-clean.c), leaving the originals intact, so
            // keeping references lets us put them back verbatim on rollback.
            savedContents  = pdf_keep_obj(c, pdf_dict_get(c, ppage->obj, PDF_NAME(Contents)));
            savedResources = pdf_keep_obj(c, pdf_dict_get(c, ppage->obj, PDF_NAME(Resources)));

            double lightBefore = measureLightFraction(c, &ppage->super);

            pdf_recolor_options opts{};
            opts.num_comp = 1;  // DeviceGray
            pdf_recolor_page(c, pdoc, pageIndex, &opts);

            double lightAfter = measureLightFraction(c, &ppage->super);

            // GitHub #345: content-blackening guard. If the page carried
            // substantial light area (white/light paper background) and that
            // light has collapsed — or the page can no longer even be rendered
            // — the blind recolor corrupted the original, non-image content to
            // solid black. Roll back to the untouched color content: grayscale
            // simply does not apply safely to this page. This is the
            // safety-critical requirement — a convert must never blacken the
            // original content. A page that is legitimately dark to begin with
            // (lightBefore low) skips the check, so a dark photo still converts.
            const bool hadLight    = (lightBefore > 0.10);
            const bool lostLight   = (lightAfter < 0.0) ||
                                     (lightAfter < lightBefore * 0.25);
            if (hadLight && lostLight) {
                pdf_dict_put(c, ppage->obj, PDF_NAME(Contents), savedContents);
                pdf_dict_put(c, ppage->obj, PDF_NAME(Resources), savedResources);
                ok = false;
            } else {
                ok = true;
            }
        }
        fz_always(c) {
            pdf_drop_obj(c, savedContents);
            pdf_drop_obj(c, savedResources);
            if (ppage) fz_drop_page(c, &ppage->super);
        }
        fz_catch(c) {
            // Swallow MuPDF errors; caller treats false as failure.
            ok = false;
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
