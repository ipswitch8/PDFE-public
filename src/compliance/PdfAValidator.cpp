#include "PdfAValidator.h"
#include "XmpMetadataWriter.h"
#include "IccProfileManager.h"
#include "FontComplianceChecker.h"
#include "ProhibitedFeatureScanner.h"
#include "../core/MuPdfContext.h"
#include "../core/Document.h"

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

namespace pdfe::compliance {

class PdfAValidator::Impl {
public:
    explicit Impl(std::shared_ptr<pdfe::core::MuPdfContext> context)
        : context_(std::move(context))
    {}

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
};

PdfAValidator::PdfAValidator(std::shared_ptr<pdfe::core::MuPdfContext> context)
    : impl_(std::make_unique<Impl>(std::move(context)))
{}

PdfAValidator::~PdfAValidator() = default;

ComplianceResult PdfAValidator::validate(pdfe::core::Document* doc, PdfALevel level) {
    ComplianceResult result;
    result.targetLevel = level;

    if (!doc || level == PdfALevel::None) {
        result.success = false;
        result.errorMessage = "Invalid document or level";
        return result;
    }

    // 1. Check XMP metadata
    XmpMetadataWriter xmpWriter(impl_->context_);
    PdfALevel detected = xmpWriter.detectPdfALevel(doc);
    if (detected == PdfALevel::None) {
        result.issues.push_back({
            IssueSeverity::Error,
            "6.7.11",
            "XMP metadata missing PDF/A identification (pdfaid:part/conformance)",
            -1, ""
        });
    } else if (detected != level) {
        result.issues.push_back({
            IssueSeverity::Warning,
            "6.7.11",
            "XMP PDF/A identification (" + pdfaLevelName(detected) +
                ") does not match target level (" + pdfaLevelName(level) + ")",
            -1, ""
        });
    }

    // 2. Check OutputIntent
    IccProfileManager iccMgr(impl_->context_);
    if (!iccMgr.hasOutputIntent(doc)) {
        result.issues.push_back({
            IssueSeverity::Error,
            "6.2.2",
            "No OutputIntent with ICC profile found (required for PDF/A)",
            -1, ""
        });
    }

    // 3. Check font compliance
    FontComplianceChecker fontChecker(impl_->context_);
    bool requireUnicode = levelRequiresUnicode(level);
    auto fonts = fontChecker.checkFonts(doc, requireUnicode);
    for (const auto& font : fonts) {
        if (!font.isEmbedded) {
            result.issues.push_back({
                IssueSeverity::Error,
                "6.3.3",
                "Font '" + font.fontName + "' is not embedded",
                font.pageIndex, ""
            });
        }
        if (requireUnicode && !font.hasToUnicode && !font.isSymbolic) {
            result.issues.push_back({
                IssueSeverity::Error,
                "6.3.8",
                "Font '" + font.fontName + "' has no ToUnicode CMap (required for " +
                    pdfaLevelName(level) + ")",
                font.pageIndex, ""
            });
        }
    }

    // 4. Check prohibited features
    ProhibitedFeatureScanner scanner(impl_->context_);
    auto prohibitedIssues = scanner.scanAll(doc, level);
    result.issues.insert(result.issues.end(),
                         prohibitedIssues.begin(), prohibitedIssues.end());

    // 5. Check tagged PDF requirement (for PDF/A-2a, 3a)
    if (levelRequiresTagging(level)) {
        bool hasMarkInfo = false;
        impl_->context_->execute([&](fz_context* ctx) {
            pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
            if (!pdfDoc) return;

            pdf_obj* catalog = pdf_dict_get(ctx, pdf_trailer(ctx, pdfDoc), PDF_NAME(Root));
            if (!catalog) return;

            pdf_obj* markInfo = pdf_dict_gets(ctx, catalog, "MarkInfo");
            if (markInfo) {
                pdf_obj* marked = pdf_dict_gets(ctx, markInfo, "Marked");
                hasMarkInfo = marked && pdf_to_bool(ctx, marked);
            }

            // Also check for StructTreeRoot
            const pdf_obj* structTree = pdf_dict_gets(ctx, catalog, "StructTreeRoot");
            if (!structTree) hasMarkInfo = false;
        });

        if (!hasMarkInfo) {
            result.issues.push_back({
                IssueSeverity::Error,
                "6.8",
                "Document is not tagged (required for " + pdfaLevelName(level) + ")",
                -1, ""
            });
        }
    }

    result.success = !result.hasErrors();
    return result;
}

bool PdfAValidator::isCompliant(pdfe::core::Document* doc, PdfALevel level) {
    return validate(doc, level).success;
}

} // namespace pdfe::compliance
