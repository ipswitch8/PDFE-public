#include "ProhibitedFeatureScanner.h"
#include "../core/MuPdfContext.h"
#include "../core/Document.h"

#include <cstring>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

namespace pdfe::compliance {

class ProhibitedFeatureScanner::Impl {
public:
    explicit Impl(std::shared_ptr<pdfe::core::MuPdfContext> context)
        : context_(std::move(context))
    {}

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
};

ProhibitedFeatureScanner::ProhibitedFeatureScanner(
    std::shared_ptr<pdfe::core::MuPdfContext> context)
    : impl_(std::make_unique<Impl>(std::move(context)))
{}

ProhibitedFeatureScanner::~ProhibitedFeatureScanner() = default;

bool ProhibitedFeatureScanner::hasEncryption(pdfe::core::Document* doc) const {
    if (!doc) return false;

    return impl_->context_->executeReturn<bool>([doc](fz_context* ctx) -> bool {
        pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdfDoc) return false;

        pdf_obj* trailer = pdf_trailer(ctx, pdfDoc);
        const pdf_obj* encryptObj = pdf_dict_get(ctx, trailer, PDF_NAME(Encrypt));
        return encryptObj != nullptr;
    });
}

bool ProhibitedFeatureScanner::hasJavaScript(pdfe::core::Document* doc) const {
    if (!doc) return false;

    return impl_->context_->executeReturn<bool>([doc](fz_context* ctx) -> bool {
        pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdfDoc) return false;

        // Check catalog /Names/JavaScript
        pdf_obj* catalog = pdf_dict_get(ctx, pdf_trailer(ctx, pdfDoc), PDF_NAME(Root));
        if (!catalog) return false;

        pdf_obj* names = pdf_dict_get(ctx, catalog, PDF_NAME(Names));
        if (names) {
            const pdf_obj* jsNames = pdf_dict_gets(ctx, names, "JavaScript");
            if (jsNames) return true;
        }

        // Check catalog /OpenAction for JavaScript
        pdf_obj* openAction = pdf_dict_gets(ctx, catalog, "OpenAction");
        if (openAction) {
            pdf_obj* s = pdf_dict_get(ctx, openAction, PDF_NAME(S));
            if (s && std::strcmp(pdf_to_name(ctx, s), "JavaScript") == 0)
                return true;
        }

        // Check page-level /AA (Additional Actions) dictionaries
        int pageCount = pdf_count_pages(ctx, pdfDoc);
        for (int i = 0; i < pageCount; ++i) {
            pdf_obj* pageObj = pdf_lookup_page_obj(ctx, pdfDoc, i);
            if (!pageObj) continue;

            pdf_obj* aa = pdf_dict_get(ctx, pageObj, PDF_NAME(AA));
            if (aa) {
                // Check for JS in any AA trigger
                int aaLen = pdf_dict_len(ctx, aa);
                for (int j = 0; j < aaLen; ++j) {
                    pdf_obj* action = pdf_dict_get_val(ctx, aa, j);
                    pdf_obj* s = pdf_dict_get(ctx, action, PDF_NAME(S));
                    if (s && std::strcmp(pdf_to_name(ctx, s), "JavaScript") == 0)
                        return true;
                }
            }
        }

        return false;
    });
}

bool ProhibitedFeatureScanner::hasTransparency(pdfe::core::Document* doc) const {
    if (!doc) return false;

    return impl_->context_->executeReturn<bool>([doc](fz_context* ctx) -> bool {
        pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdfDoc) return false;

        int pageCount = pdf_count_pages(ctx, pdfDoc);
        for (int i = 0; i < pageCount; ++i) {
            pdf_obj* pageObj = pdf_lookup_page_obj(ctx, pdfDoc, i);
            if (!pageObj) continue;

            // Check page /Group with /S /Transparency
            pdf_obj* group = pdf_dict_get(ctx, pageObj, PDF_NAME(Group));
            if (group) {
                pdf_obj* s = pdf_dict_get(ctx, group, PDF_NAME(S));
                if (s && std::strcmp(pdf_to_name(ctx, s), "Transparency") == 0)
                    return true;
            }

            // Check ExtGState resources for blend modes and soft masks
            pdf_obj* resources = pdf_dict_get(ctx, pageObj, PDF_NAME(Resources));
            if (!resources) continue;

            pdf_obj* extGState = pdf_dict_get(ctx, resources, PDF_NAME(ExtGState));
            if (!extGState) continue;

            int gsLen = pdf_dict_len(ctx, extGState);
            for (int j = 0; j < gsLen; ++j) {
                pdf_obj* gs = pdf_dict_get_val(ctx, extGState, j);

                // Check for non-Normal blend mode
                pdf_obj* bm = pdf_dict_get(ctx, gs, PDF_NAME(BM));
                if (bm) {
                    const char* bmName = pdf_to_name(ctx, bm);
                    if (bmName && std::strcmp(bmName, "Normal") != 0 &&
                        std::strcmp(bmName, "Compatible") != 0)
                        return true;
                }

                // Check for soft mask
                pdf_obj* sMask = pdf_dict_get(ctx, gs, PDF_NAME(SMask));
                if (sMask && !pdf_is_name(ctx, sMask))
                    return true; // Dict means soft mask is present
                if (sMask && pdf_is_name(ctx, sMask)) {
                    const char* smName = pdf_to_name(ctx, sMask);
                    if (smName && std::strcmp(smName, "None") != 0)
                        return true;
                }
            }
        }

        return false;
    });
}

bool ProhibitedFeatureScanner::hasObjectStreams(pdfe::core::Document* doc) const {
    if (!doc) return false;

    return impl_->context_->executeReturn<bool>([doc](fz_context* ctx) -> bool {
        pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdfDoc) return false;

        // Check xref for object stream entries (type 2)
        int xrefLen = pdf_xref_len(ctx, pdfDoc);
        for (int i = 0; i < xrefLen; ++i) {
            const pdf_xref_entry* entry = pdf_get_xref_entry_no_null(ctx, pdfDoc, i);
            if (entry && entry->type == 'o') // 'o' = compressed (in object stream)
                return true;
        }

        return false;
    });
}

bool ProhibitedFeatureScanner::hasExternalReferences(pdfe::core::Document* doc) const {
    if (!doc) return false;

    return impl_->context_->executeReturn<bool>([doc](fz_context* ctx) -> bool {
        pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdfDoc) return false;

        int pageCount = pdf_count_pages(ctx, pdfDoc);
        for (int i = 0; i < pageCount; ++i) {
            pdf_obj* pageObj = pdf_lookup_page_obj(ctx, pdfDoc, i);
            if (!pageObj) continue;

            // Check annotations for URI/GoToR/Launch actions
            pdf_obj* annots = pdf_dict_get(ctx, pageObj, PDF_NAME(Annots));
            if (!annots) continue;

            int annotLen = pdf_array_len(ctx, annots);
            for (int j = 0; j < annotLen; ++j) {
                pdf_obj* annot = pdf_array_get(ctx, annots, j);
                pdf_obj* a = pdf_dict_get(ctx, annot, PDF_NAME(A));
                if (!a) continue;

                pdf_obj* s = pdf_dict_get(ctx, a, PDF_NAME(S));
                if (!s) continue;

                const char* sName = pdf_to_name(ctx, s);
                if (sName &&
                    (std::strcmp(sName, "Launch") == 0 ||
                     std::strcmp(sName, "Sound") == 0 ||
                     std::strcmp(sName, "Movie") == 0 ||
                     std::strcmp(sName, "GoToR") == 0))
                    return true;
            }
        }

        return false;
    });
}

std::vector<ComplianceIssue> ProhibitedFeatureScanner::scanAll(
    pdfe::core::Document* doc, PdfALevel level) const
{
    std::vector<ComplianceIssue> issues;

    if (hasEncryption(doc)) {
        issues.push_back({
            IssueSeverity::Error,
            "6.1.3",
            "Document is encrypted (prohibited in PDF/A)",
            -1, ""
        });
    }

    if (hasJavaScript(doc)) {
        issues.push_back({
            IssueSeverity::Error,
            "6.6.1",
            "Document contains JavaScript (prohibited in PDF/A)",
            -1, ""
        });
    }

    if (hasExternalReferences(doc)) {
        issues.push_back({
            IssueSeverity::Error,
            "6.6.3",
            "Document contains external references (Launch/Sound/Movie/GoToR actions)",
            -1, ""
        });
    }

    // Transparency is only prohibited in PDF/A-1
    if (!levelAllowsTransparency(level) && hasTransparency(doc)) {
        issues.push_back({
            IssueSeverity::Error,
            "6.4",
            "Document uses transparency (prohibited in PDF/A-1)",
            -1, ""
        });
    }

    // Object streams are only prohibited in PDF/A-1
    if (!levelAllowsObjStreams(level) && hasObjectStreams(doc)) {
        issues.push_back({
            IssueSeverity::Error,
            "6.1.7",
            "Document uses object streams (prohibited in PDF/A-1)",
            -1, ""
        });
    }

    return issues;
}

} // namespace pdfe::compliance
