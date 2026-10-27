#include "RedactionManager.h"
#include "RedactionScrubber.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/QpdfWrapper.h"
// core/AtomicFileReplace.h is deliberately NOT included any more (WIN-87
// site #3, phase-57). This file used to reach for atomicReplaceFileWithRetry
// and, briefly, for openDenyWriteReadStream/StagingFile directly. The whole
// staging lifecycle now lives in Document::saveThroughExternalRewrite, and
// leaving the header included would keep the path-based primitive exactly one
// already-present #include away from being reached for again.
#include "core/Error.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <cstdio>
#include <stdexcept>
#include <filesystem>
#include <system_error>
#include <set>

namespace pdfe::redaction {

// Helper: Get the nth annotation from a page (only Redact type)
static pdf_annot* getNthRedactAnnot(fz_context* ctx, pdf_page* page, int targetIndex) {
    pdf_annot* annot = pdf_first_annot(ctx, page);
    int idx = 0;
    while (annot) {
        if (idx == targetIndex) {
            if (pdf_annot_type(ctx, annot) != PDF_ANNOT_REDACT) {
                return nullptr;  // Index exists but isn't a redaction
            }
            return annot;
        }
        annot = pdf_next_annot(ctx, annot);
        idx++;
    }
    return nullptr;
}

// Helper: Get any annotation by index
static pdf_annot* getNthAnnot(fz_context* ctx, pdf_page* page, int targetIndex) {
    pdf_annot* annot = pdf_first_annot(ctx, page);
    for (int i = 0; i < targetIndex && annot; i++) {
        annot = pdf_next_annot(ctx, annot);
    }
    return annot;
}

// Helper: Read redaction data from a MuPDF redaction annotation
static RedactionData readRedactionData(fz_context* ctx, pdf_annot* annot, int pageIndex, int annotIndex) {
    RedactionData data;
    data.pageIndex = pageIndex;
    data.annotIndex = annotIndex;

    // Bounding rectangle
    fz_rect r = pdf_bound_annot(ctx, annot);
    data.rect = QRectF(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);

    // Interior color (fill color for redaction box)
    // Note: MuPDF's pdf_annot_has_interior_color() throws for Redact annotations,
    // so we access the IC dict entry directly.
    pdf_obj* annotObjIC = pdf_annot_obj(ctx, annot);
    pdf_obj* icObj = pdf_dict_get(ctx, annotObjIC, PDF_NAME(IC));
    if (icObj && pdf_is_array(ctx, icObj)) {
        int n = pdf_array_len(ctx, icObj);
        if (n >= 3) {
            float red   = pdf_array_get_real(ctx, icObj, 0);
            float green = pdf_array_get_real(ctx, icObj, 1);
            float blue  = pdf_array_get_real(ctx, icObj, 2);
            data.fillColor = QColor::fromRgbF(red, green, blue);
        }
    }

    // Overlay text via dict-level access
    pdf_obj* annotObj = pdf_annot_obj(ctx, annot);
    pdf_obj* overlayObj = pdf_dict_gets(ctx, annotObj, "OverlayText");
    if (overlayObj) {
        const char* text = pdf_to_text_string(ctx, overlayObj);
        if (text) {
            data.overlayText = text;
        }
    }

    return data;
}

// Helper: Convert our enum to MuPDF enum
static int toMuPdfImageMethod(RedactionImageMethod method) {
    switch (method) {
        case RedactionImageMethod::None:                 return PDF_REDACT_IMAGE_NONE;
        case RedactionImageMethod::Remove:               return PDF_REDACT_IMAGE_REMOVE;
        case RedactionImageMethod::Pixels:               return PDF_REDACT_IMAGE_PIXELS;
        case RedactionImageMethod::RemoveUnlessInvisible: return PDF_REDACT_IMAGE_REMOVE_UNLESS_INVISIBLE;
        default:                                         return PDF_REDACT_IMAGE_REMOVE;
    }
}

static int toMuPdfLineArt(RedactionLineArtMethod method) {
    switch (method) {
        case RedactionLineArtMethod::None:            return PDF_REDACT_LINE_ART_NONE;
        case RedactionLineArtMethod::RemoveIfCovered: return PDF_REDACT_LINE_ART_REMOVE_IF_COVERED;
        case RedactionLineArtMethod::RemoveIfTouched: return PDF_REDACT_LINE_ART_REMOVE_IF_TOUCHED;
        default:                                      return PDF_REDACT_LINE_ART_REMOVE_IF_COVERED;
    }
}

static int toMuPdfText(RedactionTextMethod method) {
    switch (method) {
        case RedactionTextMethod::Remove: return PDF_REDACT_TEXT_REMOVE;
        case RedactionTextMethod::None:   return PDF_REDACT_TEXT_NONE;
        default:                          return PDF_REDACT_TEXT_REMOVE;
    }
}

// Pimpl implementation
class RedactionManager::Impl {
public:
    std::shared_ptr<pdfe::core::MuPdfContext> context;

    explicit Impl(std::shared_ptr<pdfe::core::MuPdfContext> ctx)
        : context(std::move(ctx)) {}

    pdf_page* getPdfPage(const pdfe::core::Page* page) {
        fz_page* fzPage = page->fzPage();
        return context->executeReturn<pdf_page*>([fzPage](fz_context* ctx) {
            return pdf_page_from_fz_page(ctx, fzPage);
        });
    }

    pdf_document* getPdfDoc(fz_document* fzDoc) {
        return context->executeReturn<pdf_document*>([fzDoc](fz_context* ctx) {
            return pdf_document_from_fz_document(ctx, fzDoc);
        });
    }
};

// --- RedactionManager public methods ---

RedactionManager::RedactionManager(std::shared_ptr<pdfe::core::MuPdfContext> ctx)
    : pImpl(std::make_unique<Impl>(std::move(ctx))) {}

RedactionManager::~RedactionManager() = default;

RedactionManager::RedactionManager(RedactionManager&&) noexcept = default;
RedactionManager& RedactionManager::operator=(RedactionManager&&) noexcept = default;

RedactionData RedactionManager::createRedactionMark(
    const pdfe::core::Page* page,
    QRectF rect
) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);
    int pageIndex = page->index();
    int newAnnotIndex = 0;

    pImpl->context->execute([&](fz_context* ctx) {
        // Create a Redact annotation
        pdf_annot* annot = pdf_create_annot(
            ctx, pdfPage, PDF_ANNOT_REDACT
        );

        // Set bounding rectangle
        fz_rect r = {
            static_cast<float>(rect.left()),
            static_cast<float>(rect.top()),
            static_cast<float>(rect.right()),
            static_cast<float>(rect.bottom())
        };
        pdf_set_annot_rect(ctx, annot, r);

        // Set default interior color (black fill) via dict-level IC entry
        // (pdf_set_annot_interior_color rejects Redact annotations)
        pdf_obj* annotObj = pdf_annot_obj(ctx, annot);
        pdf_obj* icArr = pdf_new_array(ctx, pdfPage->doc, 3);
        pdf_array_push_real(ctx, icArr, 0.0f);
        pdf_array_push_real(ctx, icArr, 0.0f);
        pdf_array_push_real(ctx, icArr, 0.0f);
        pdf_dict_put(ctx, annotObj, PDF_NAME(IC), icArr);

        // Set the annotation border color to red (visual indicator for marks)
        float red[3] = {1.0f, 0.0f, 0.0f};
        pdf_set_annot_color(ctx, annot, 3, red);

        pdf_update_annot(ctx, annot);

        // Manually generate appearance stream with filled rectangle.
        // MuPDF does not auto-generate filled appearance for Redact annotations.
        fz_rect annotRect = pdf_annot_rect(ctx, annot);
        float w = annotRect.x1 - annotRect.x0;
        float h = annotRect.y1 - annotRect.y0;

        // Build appearance stream: black fill + red border
        char apBuf[256];
        snprintf(apBuf, sizeof(apBuf),
            "q 0 0 0 rg 0 0 %g %g re f "   // black filled rect
            "1 0 0 RG 1 w 0 0 %g %g re S "  // red border
            "Q",
            static_cast<double>(w), static_cast<double>(h),
            static_cast<double>(w), static_cast<double>(h));

        fz_buffer* apBufObj = fz_new_buffer_from_copied_data(ctx,
            reinterpret_cast<const unsigned char*>(apBuf),
            strlen(apBuf));

        fz_rect apBbox = {0, 0, w, h};
        pdf_set_annot_appearance(ctx, annot, "N", nullptr,
            fz_make_matrix(1, 0, 0, 1, annotRect.x0, annotRect.y0),
            apBbox, nullptr, apBufObj);
        fz_drop_buffer(ctx, apBufObj);

        // Find the index of the newly created annotation
        pdf_annot* current = pdf_first_annot(ctx, pdfPage);
        int index = 0;
        while (current) {
            if (current == annot) {
                newAnnotIndex = index;
                break;
            }
            current = pdf_next_annot(ctx, current);
            index++;
        }
    });

    return getRedactionMark(page, newAnnotIndex);
}

RedactionData RedactionManager::deleteRedactionMark(
    const pdfe::core::Page* page,
    int annotIndex
) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    // Get data before deletion (for undo)
    RedactionData data = getRedactionMark(page, annotIndex);

    pImpl->context->execute([&](fz_context* ctx) {
        pdf_annot* annot = getNthAnnot(ctx, pdfPage, annotIndex);
        if (!annot) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Annotation index out of range");
        }
        if (pdf_annot_type(ctx, annot) != PDF_ANNOT_REDACT) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Annotation is not a redaction mark");
        }
        pdf_delete_annot(ctx, pdfPage, annot);
    });

    return data;
}

int RedactionManager::restoreRedactionMark(
    const pdfe::core::Page* page,
    const RedactionData& data
) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);
    int newAnnotIndex = 0;

    pImpl->context->execute([&](fz_context* ctx) {
        // Re-create the redaction annotation
        pdf_annot* annot = pdf_create_annot(
            ctx, pdfPage, PDF_ANNOT_REDACT
        );

        // Set bounding rectangle
        fz_rect r = {
            static_cast<float>(data.rect.left()),
            static_cast<float>(data.rect.top()),
            static_cast<float>(data.rect.right()),
            static_cast<float>(data.rect.bottom())
        };
        pdf_set_annot_rect(ctx, annot, r);

        // Set interior color (fill color) via dict-level IC entry
        pdf_obj* restoreAnnotObj = pdf_annot_obj(ctx, annot);
        pdf_obj* restoreIcArr = pdf_new_array(ctx, pdfPage->doc, 3);
        pdf_array_push_real(ctx, restoreIcArr, static_cast<float>(data.fillColor.redF()));
        pdf_array_push_real(ctx, restoreIcArr, static_cast<float>(data.fillColor.greenF()));
        pdf_array_push_real(ctx, restoreIcArr, static_cast<float>(data.fillColor.blueF()));
        pdf_dict_put(ctx, restoreAnnotObj, PDF_NAME(IC), restoreIcArr);

        // Set border color to red (visual indicator)
        float red[3] = {1.0f, 0.0f, 0.0f};
        pdf_set_annot_color(ctx, annot, 3, red);

        // Set overlay text if present
        if (!data.overlayText.empty()) {
            pdf_obj* annotObj = pdf_annot_obj(ctx, annot);
            pdf_dict_puts(ctx, annotObj, "OverlayText",
                pdf_new_text_string(ctx, data.overlayText.c_str()));
        }

        pdf_update_annot(ctx, annot);

        // Find the index
        pdf_annot* current = pdf_first_annot(ctx, pdfPage);
        int index = 0;
        while (current) {
            if (current == annot) {
                newAnnotIndex = index;
                break;
            }
            current = pdf_next_annot(ctx, current);
            index++;
        }
    });

    return newAnnotIndex;
}

void RedactionManager::setFillColor(
    const pdfe::core::Page* page,
    int annotIndex,
    QColor color
) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    pImpl->context->execute([&](fz_context* ctx) {
        pdf_annot* annot = getNthAnnot(ctx, pdfPage, annotIndex);
        if (!annot) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Annotation index out of range");
        }

        // Set IC via dict-level access (pdf_set_annot_interior_color rejects Redact annotations)
        pdf_obj* colorAnnotObj = pdf_annot_obj(ctx, annot);
        pdf_obj* colorIcArr = pdf_new_array(ctx, pdfPage->doc, 3);
        pdf_array_push_real(ctx, colorIcArr, static_cast<float>(color.redF()));
        pdf_array_push_real(ctx, colorIcArr, static_cast<float>(color.greenF()));
        pdf_array_push_real(ctx, colorIcArr, static_cast<float>(color.blueF()));
        pdf_dict_put(ctx, colorAnnotObj, PDF_NAME(IC), colorIcArr);
        pdf_update_annot(ctx, annot);
    });
}

void RedactionManager::setOverlayText(
    const pdfe::core::Page* page,
    int annotIndex,
    const std::string& text
) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    pImpl->context->execute([&](fz_context* ctx) {
        pdf_annot* annot = getNthAnnot(ctx, pdfPage, annotIndex);
        if (!annot) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Annotation index out of range");
        }

        pdf_obj* annotObj = pdf_annot_obj(ctx, annot);
        if (text.empty()) {
            pdf_dict_dels(ctx, annotObj, "OverlayText");
        } else {
            pdf_dict_puts(ctx, annotObj, "OverlayText",
                pdf_new_text_string(ctx, text.c_str()));
        }
        pdf_update_annot(ctx, annot);
    });
}

void RedactionManager::setRect(
    const pdfe::core::Page* page,
    int annotIndex,
    QRectF rect
) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);

    pImpl->context->execute([&](fz_context* ctx) {
        pdf_annot* annot = getNthAnnot(ctx, pdfPage, annotIndex);
        if (!annot) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Annotation index out of range");
        }

        fz_rect r = {
            static_cast<float>(rect.left()),
            static_cast<float>(rect.top()),
            static_cast<float>(rect.right()),
            static_cast<float>(rect.bottom())
        };
        pdf_set_annot_rect(ctx, annot, r);
        pdf_update_annot(ctx, annot);
    });
}

std::vector<RedactionData> RedactionManager::getRedactionMarks(const pdfe::core::Page* page) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);
    int pageIndex = page->index();
    std::vector<RedactionData> marks;

    pImpl->context->execute([&](fz_context* ctx) {
        pdf_annot* annot = pdf_first_annot(ctx, pdfPage);
        int idx = 0;
        while (annot) {
            if (pdf_annot_type(ctx, annot) == PDF_ANNOT_REDACT) {
                marks.push_back(readRedactionData(ctx, annot, pageIndex, idx));
            }
            annot = pdf_next_annot(ctx, annot);
            idx++;
        }
    });

    return marks;
}

RedactionData RedactionManager::getRedactionMark(const pdfe::core::Page* page, int annotIndex) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);
    int pageIndex = page->index();
    RedactionData data;

    pImpl->context->execute([&](fz_context* ctx) {
        pdf_annot* annot = getNthAnnot(ctx, pdfPage, annotIndex);
        if (!annot) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Annotation index out of range");
        }
        if (pdf_annot_type(ctx, annot) != PDF_ANNOT_REDACT) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Annotation is not a redaction mark");
        }
        data = readRedactionData(ctx, annot, pageIndex, annotIndex);
    });

    return data;
}

std::vector<RedactionData> RedactionManager::getAllRedactionMarks(pdfe::core::Document* document) {
    if (!document) {
        throw std::invalid_argument("Document cannot be null");
    }

    std::vector<RedactionData> allMarks;
    int pageCount = document->pageCount();

    for (int i = 0; i < pageCount; i++) {
        auto page = document->page(i);
        auto marks = getRedactionMarks(page.get());
        allMarks.insert(allMarks.end(), marks.begin(), marks.end());
    }

    return allMarks;
}

int RedactionManager::pendingMarkCount(pdfe::core::Document* document) {
    if (!document) {
        throw std::invalid_argument("Document cannot be null");
    }

    int count = 0;
    int pageCount = document->pageCount();

    for (int i = 0; i < pageCount; i++) {
        auto page = document->page(i);
        auto marks = getRedactionMarks(page.get());
        count += static_cast<int>(marks.size());
    }

    return count;
}

std::optional<int> RedactionManager::getRedactionMarkAt(
    const pdfe::core::Page* page,
    QPointF pdfPoint
) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);
    std::optional<int> result;

    pImpl->context->execute([&](fz_context* ctx) {
        pdf_annot* annot = pdf_first_annot(ctx, pdfPage);
        int idx = 0;
        while (annot) {
            if (pdf_annot_type(ctx, annot) == PDF_ANNOT_REDACT) {
                fz_rect r = pdf_bound_annot(ctx, annot);
                QRectF rect(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
                if (rect.contains(pdfPoint)) {
                    result = idx;
                    return;
                }
            }
            annot = pdf_next_annot(ctx, annot);
            idx++;
        }
    });

    return result;
}

bool RedactionManager::applyPageRedactions(
    const pdfe::core::Page* page,
    const RedactionApplyOptions& options
) {
    if (!page) {
        throw std::invalid_argument("Page cannot be null");
    }

    // Applying redactions is destructive intent: it MUST NOT be persisted via
    // an incremental-append save (which leaves the redacted bytes physically in
    // the file, defeating the redaction). Flag the owning document up front —
    // unconditionally, regardless of pdf_redact_page's return value — so the
    // next save takes the full-rewrite path that scrubs the orphaned objects.
    // markNeedsFullGc (Tier 2) escalates to do_garbage=3 so the redacted
    // objects are fully de-duplicated/collected, not merely unreferenced.
    // Tainting here (not just in applyAllRedactions) covers every caller,
    // including BatchEngine, which calls applyPageRedactions directly.
    if (auto* doc = page->ownerDocument()) {
        // markNeedsFullGc() sets BOTH needsFullGc_ (GC level -> do_garbage=3) AND
        // requiresFullRewrite_ (routing -> never saveIncremental) — see
        // Document::markNeedsFullGc(). So this single call already forces the
        // full-rewrite path that scrubs the redacted bytes; no separate
        // markRequiresFullRewrite() is needed.
        doc->markNeedsFullGc();
    }

    pdf_page* pdfPage = pImpl->getPdfPage(page);
    bool applied = false;

    // --- Confidentiality-surface scrubbing (gaps PDF-60..PDF-66) ------------
    // pdf_redact_page below only clears the page content stream, its images
    // and the redaction annotations. Everything else that can carry the
    // redacted text has to be scrubbed by us, and it has to happen BEFORE the
    // redaction destroys the evidence: the text under each mark is what the
    // document-scoped matching keys on, and the redaction annotations are what
    // identifies the overlapping annotations to remove.
    //
    // These containers are owned by THIS frame — never constructed inside the
    // execute() lambda, whose fz_try/fz_catch is setjmp/longjmp and would skip
    // their destructors.
    std::vector<fz_rect> redactRects;
    std::vector<std::string> redactedWords;
    ScrubStats scrubStats;

    const bool wantScrub = options.scrubPageSurfaces || options.scrubDocumentSurfaces
                        || options.fullSanitize;

    if (wantScrub) {
        pImpl->context->execute([&](fz_context* ctx) {
            for (pdf_annot* a = pdf_first_annot(ctx, pdfPage); a;
                 a = pdf_next_annot(ctx, a)) {
                if (pdf_annot_type(ctx, a) == PDF_ANNOT_REDACT) {
                    redactRects.push_back(pdf_bound_annot(ctx, a));
                }
            }
        });
    }

    if (!redactRects.empty()) {
        fz_page* fzPage = page->fzPage();
        pImpl->context->execute([&](fz_context* ctx) {
            collectRedactedWords(ctx, fzPage, redactRects, redactedWords);
        });

        if (options.scrubPageSurfaces) {
            pImpl->context->execute([&](fz_context* ctx) {
                scrubPageScopedSurfaces(ctx, pdfPage->doc, pdfPage, redactRects,
                                        redactedWords, scrubStats);
            });
        }
        if (options.scrubDocumentSurfaces || options.fullSanitize) {
            pImpl->context->execute([&](fz_context* ctx) {
                scrubDocumentScopedSurfaces(ctx, pdfPage->doc, redactedWords,
                                            options.fullSanitize, scrubStats);
            });
        }
    }

    pImpl->context->execute([&](fz_context* ctx) {
        pdf_document* pdfDoc = pdf_page_from_fz_page(ctx, page->fzPage())->doc;

        pdf_redact_options opts;
        opts.black_boxes = options.blackBoxes ? 1 : 0;
        opts.image_method = toMuPdfImageMethod(options.imageMethod);
        opts.line_art = toMuPdfLineArt(options.lineArtMethod);
        opts.text = toMuPdfText(options.textMethod);

        int result = pdf_redact_page(ctx, pdfDoc, pdfPage, &opts);
        applied = (result != 0);
    });

    // Residual page-content scrub (PDF-61, marked-content half): a redacted
    // word can survive the redaction as a marked-content property —
    // `/Span << /ActualText (secret) >> BDC` — which is not a text-showing
    // operator and so is not what pdf_redact_page removes, yet PDFE's own
    // extractor reads /ActualText. No-op when the content is already clean.
    if (applied && options.scrubPageSurfaces && !redactedWords.empty()) {
        pImpl->context->execute([&](fz_context* ctx) {
            scrubPageContentResiduals(ctx, pdfPage->doc, pdfPage, redactedWords,
                                      scrubStats);
        });
    }

    return applied;
}

bool RedactionManager::flattenAnnotations(pdfe::core::Document* document) {
    if (!document) {
        throw std::invalid_argument("Document cannot be null");
    }

    fz_document* fzDoc = document->fzDocument();
    if (!fzDoc) {
        return false;
    }

    bool baked = false;
    pImpl->context->execute([&](fz_context* ctx) {
        pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, fzDoc);
        if (!pdfDoc) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Not a PDF document");
        }
        // bake_annots = 1, bake_widgets = 0: annotations are flattened into
        // page content; form fields are left to FormManager::flattenForm.
        pdf_bake_document(ctx, pdfDoc, 1, 0);
        baked = true;
    });

    // Baking is a destructive structural change (annotation objects are
    // orphaned), so force the full-rewrite save path with do_garbage=3.
    document->markNeedsFullGc();

    return baked;
}

RedactionApplyResult RedactionManager::applyAllRedactions(
    pdfe::core::Document* document,
    const RedactionApplyOptions& options
) {
    if (!document) {
        throw std::invalid_argument("Document cannot be null");
    }

    RedactionApplyResult result;
    int pageCount = document->pageCount();
    std::set<int> affectedPages;

    // First, find all pages with redaction marks
    for (int i = 0; i < pageCount; i++) {
        auto page = document->page(i);
        auto marks = getRedactionMarks(page.get());
        if (!marks.empty()) {
            affectedPages.insert(i);
            result.marksApplied += static_cast<int>(marks.size());
        }
    }

    if (affectedPages.empty()) {
        result.success = true;
        return result;
    }

    // Destructive + dedup-worthy: full-rewrite with do_garbage=3 (pipeline
    // b2e47d91, Tiers 1+2). Belt-and-suspenders with the per-page taint in
    // applyPageRedactions.
    document->markNeedsFullGc();

    // Apply redactions page by page
    try {
        for (int pageIdx : affectedPages) {
            auto page = document->page(pageIdx);
            applyPageRedactions(page.get(), options);
        }
        result.pagesAffected = static_cast<int>(affectedPages.size());
        result.success = true;
    } catch (const std::exception& e) {
        result.success = false;
        result.errorMessage = e.what();
        return result;
    }

    // QPDF single-revision rewrite to eliminate data ghosts
    if (options.rewriteDocument && result.success) {
        // WIN-87 site #3, phase-57 — THERE IS NO LONGER A ".redact_tmp" OR A
        // ".redact_out", AND THE NAMES GOING AWAY *IS* THE FIX.
        //
        // This block used to be a THREE-STEP BY-PATH RELAY, and the dangerous
        // window in it was not the rename:
        //
        //     document->save("<doc>.redact_tmp")       MuPDF writes it by path
        //     ---- window A ----                       nothing holds it
        //     qpdf.open("<doc>.redact_tmp")            QPDF REOPENS IT BY NAME
        //     qpdf.save("<doc>.redact_out")            writes it by path
        //     ---- window B ----                       nothing holds it
        //     atomicReplaceFileWithRetry(out, filePath)
        //
        // Both names were derived directly from the user's own document path,
        // so an attacker who knew the document knew both. Window B is the one
        // an audit finds first; WINDOW A IS THE ONE THAT MATTERS MORE, because
        // the artifact published here IS the redaction — bytes stamped into the
        // intermediate are parsed by QPDF and republished as the document the
        // user believes had content removed from it. Making the final rename
        // atomic does not touch window A at all: the rename faithfully
        // publishes whatever QPDF produced.
        //
        // TWO THINGS THAT LOOKED LIKE THEY CLOSED WINDOW A AND DID NOT. Both
        // are recorded because each is a shape a later reader will reach for.
        //
        //   * "The reopened Document HOLDS the temp open" (task #46). It does —
        //     but MuPDF opens through the CRT in the default share mode, which
        //     permits other openers WRITE access. "Something has it open" is not
        //     "nobody else can write to it"; the share mode is the property, and
        //     it was never checked.
        //   * Publishing the bake and handing back a DUPLICATE of the staging
        //     handle (Document::saveAndRetainPublishedReader). MEASURED not to
        //     work: with commitOver having closed the original handle, a
        //     concurrent process still obtained GENERIC_WRITE on the published
        //     file. A surviving duplicate does not carry the share reservation
        //     the original open made. Tombstoned in AtomicFileReplace.h.
        //   * Pinning the published temp with openDenyWriteReadStream() the
        //     instant save() returned. That NARROWS window A to the interval
        //     between publication and the pin — and the hostile test measured
        //     the attacker winning that interval reliably, because
        //     Document::save() itself reopens the temp it just published.
        //     Narrowing a substitution window on a redaction publish path is
        //     not closing it.
        //
        // WHAT IT IS NOW: THE BAKE IS NEVER PUBLISHED UNDER A NAME AT ALL.
        // Document::saveThroughExternalRewrite full-rewrites into a StagingFile
        // (128-bit CSPRNG name, CREATE_NEW, exclusive handle held from creation
        // to destruction), hands QPDF a DUPLICATE of that still-held handle,
        // stages QPDF's output in a second such file, and publishes only that
        // one — by renaming THROUGH its handle. There is no instant at which
        // either artifact exists under a guessable name, and no instant at
        // which either is unheld, so there is no window to win rather than a
        // small one. Window B closed the same way, and the F10/WIN-85 data-loss
        // guarantee is unchanged: on any failure the original is byte-intact
        // and the caller learns the rewrite did not happen.
        //
        // WHAT THE NAMES GOING AWAY COST, PAID RATHER THAN SKIPPED. The DCA
        // 0007/0010 oracles named ".redact_tmp"/".redact_out" literally, so
        // after this they would pass VACUOUSLY — a check for a name nothing
        // produces reads as coverage while testing nothing. They are re-pointed
        // at the DIRECTORY (final listing == expected set exactly), which is
        // name-blind and catches residue whatever it is called. Same for the
        // Python sweeps in tests/conftest.py and tests/fixtures/.
        // Document::registerScratchTempForCleanup loses its only caller; see
        // the note at its declaration for why it is kept rather than deleted.
        try {
            document->saveThroughExternalRewrite(
                [](std::FILE* in, std::FILE* out) {
                    // Ownership of BOTH streams is ours on every path — a
                    // stream still open in THIS process blocks the publishing
                    // rename. QpdfWrapper takes each over as it receives it
                    // (close_file = true, on its throwing paths too), so the
                    // only gap to cover is `out` while openFromStream runs.
                    struct OutGuard {
                        std::FILE* f;
                        ~OutGuard() { if (f) std::fclose(f); }
                    } outGuard{out};

                    pdfe::core::QpdfWrapper qpdf;
                    qpdf.openFromStream(in, "<staged redaction bake handle>");
                    std::FILE* owned = outGuard.f;
                    outGuard.f = nullptr;
                    qpdf.saveToStream(owned, false);
                });
            result.documentRewritten = true;
        } catch (const std::exception& e) {
            result.documentRewritten = false;
            // Non-fatal: the redaction WAS applied in memory, it just was not
            // baked into a single-revision rewrite. The original file is
            // byte-intact, and the caller is told why — a silent false here
            // would leave the user believing previous revisions were
            // eliminated when they were not.
            if (result.errorMessage.empty()) {
                result.errorMessage =
                    std::string("Redactions applied but rewrite failed: ")
                    + e.what();
            }
        }
    }

    return result;
}

} // namespace pdfe::redaction
