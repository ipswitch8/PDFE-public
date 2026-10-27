#include "ComplianceManager.h"
#include "PdfAValidator.h"
#include "PdfAConverter.h"
#include "PdfUAConverter.h"
#include "XmpMetadataWriter.h"
#include "IccProfileManager.h"
#include "AutoTagger.h"
#include "StructureTreeBuilder.h"
#include "ContentStreamTagger.h"
#include "../core/MuPdfContext.h"
#include "../core/Document.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <algorithm>
#include <cstring>
#include <map>

namespace pdfe::compliance {

class ComplianceManager::Impl {
public:
    explicit Impl(std::shared_ptr<pdfe::core::MuPdfContext> context)
        : context_(std::move(context))
    {}

    std::shared_ptr<pdfe::core::MuPdfContext> context_;

    // Pipeline E phase-12: standalone JavaScript stripper. Mirrors
    // PdfAConverter::Impl::removeJavaScript so fixIssue can apply it
    // without going through full PDF/A conversion.
    void stripJavaScript(pdfe::core::Document* doc) {
        context_->execute([doc](fz_context* ctx) {
            pdf_document* pdfDoc =
                pdf_document_from_fz_document(ctx, doc->fzDocument());
            if (!pdfDoc) return;

            pdf_obj* catalog =
                pdf_dict_get(ctx, pdf_trailer(ctx, pdfDoc), PDF_NAME(Root));
            if (!catalog) return;

            pdf_obj* names = pdf_dict_get(ctx, catalog, PDF_NAME(Names));
            if (names) {
                pdf_dict_dels(ctx, names, "JavaScript");
            }

            pdf_obj* openAction = pdf_dict_gets(ctx, catalog, "OpenAction");
            if (openAction) {
                pdf_obj* s = pdf_dict_get(ctx, openAction, PDF_NAME(S));
                if (s && std::strcmp(pdf_to_name(ctx, s), "JavaScript") == 0) {
                    pdf_dict_dels(ctx, catalog, "OpenAction");
                }
            }

            pdf_dict_del(ctx, catalog, PDF_NAME(AA));

            const int pageCount = pdf_count_pages(ctx, pdfDoc);
            for (int i = 0; i < pageCount; ++i) {
                pdf_obj* pageObj = pdf_lookup_page_obj(ctx, pdfDoc, i);
                if (!pageObj) continue;
                pdf_dict_del(ctx, pageObj, PDF_NAME(AA));

                pdf_obj* annots = pdf_dict_get(ctx, pageObj, PDF_NAME(Annots));
                if (!annots) continue;
                int annotLen = pdf_array_len(ctx, annots);
                for (int j = 0; j < annotLen; ++j) {
                    pdf_obj* annot = pdf_array_get(ctx, annots, j);
                    pdf_dict_del(ctx, annot, PDF_NAME(AA));
                    pdf_obj* a = pdf_dict_get(ctx, annot, PDF_NAME(A));
                    if (a) {
                        pdf_obj* s = pdf_dict_get(ctx, a, PDF_NAME(S));
                        if (s && std::strcmp(pdf_to_name(ctx, s), "JavaScript") == 0) {
                            pdf_dict_del(ctx, annot, PDF_NAME(A));
                        }
                    }
                }
            }
        });
    }
};

ComplianceManager::ComplianceManager(std::shared_ptr<pdfe::core::MuPdfContext> context)
    : impl_(std::make_unique<Impl>(std::move(context)))
{}

ComplianceManager::~ComplianceManager() = default;

ComplianceResult ComplianceManager::validatePdfA(pdfe::core::Document* doc, PdfALevel level) {
    PdfAValidator validator(impl_->context_);
    return validator.validate(doc, level);
}

ComplianceResult ComplianceManager::validatePdfUA(pdfe::core::Document* doc) {
    PdfUAConverter converter(impl_->context_);
    return converter.validatePdfUA(doc);
}

PdfALevel ComplianceManager::detectPdfALevel(pdfe::core::Document* doc) {
    XmpMetadataWriter writer(impl_->context_);
    return writer.detectPdfALevel(doc);
}

ComplianceResult ComplianceManager::convertToPdfA(
    pdfe::core::Document* doc,
    PdfALevel level,
    const std::filesystem::path& outputPath)
{
    // For PDF/A-2a, add tagging before standard conversion
    if (levelRequiresTagging(level)) {
        AutoTagger autoTagger(impl_->context_);
        auto root = autoTagger.tagDocument(doc);

        StructureTreeBuilder treeBuilder(impl_->context_);
        treeBuilder.buildTree(doc, root);
        treeBuilder.setDocumentLanguage(doc, "en-US");

        // Inject marked content using MCIDs from the structure tree
        ContentStreamTagger streamTagger(impl_->context_);
        int pageCount = doc->pageCount();
        std::map<int, std::vector<MarkedContentRegion>> pageRegions;
        for (const auto& child : root.children) {
            MarkedContentRegion r;
            r.tag = child->tag;
            r.mcid = child->mcid;
            pageRegions[child->pageIndex].push_back(r);
        }
        for (int i = 0; i < pageCount; ++i) {
            auto it = pageRegions.find(i);
            if (it != pageRegions.end() && !it->second.empty()) {
                streamTagger.tagPageContent(doc, i, it->second);
            }
        }
    }

    PdfAConverter converter(impl_->context_);
    return converter.convertAndSave(doc, level, outputPath);
}

ComplianceResult ComplianceManager::convertToPdfUA(
    pdfe::core::Document* doc,
    const std::filesystem::path& outputPath,
    const std::string& language)
{
    PdfUAConverter converter(impl_->context_);
    return converter.convertAndSave(doc, outputPath, language);
}

ComplianceResult ComplianceManager::convertToPdfA2aAndUA(
    pdfe::core::Document* doc,
    const std::filesystem::path& outputPath,
    const std::string& language)
{
    // Combined conversion: PDF/A-2a + PDF/UA-1
    // First add tagging (shared between both standards)
    AutoTagger autoTagger(impl_->context_);
    auto root = autoTagger.tagDocument(doc);

    StructureTreeBuilder treeBuilder(impl_->context_);
    treeBuilder.buildTree(doc, root);
    treeBuilder.setDocumentLanguage(doc, language);

    // Inject marked content using MCIDs from the structure tree
    ContentStreamTagger streamTagger(impl_->context_);
    int pageCount = doc->pageCount();
    std::map<int, std::vector<MarkedContentRegion>> pageRegions;
    for (const auto& child : root.children) {
        MarkedContentRegion r;
        r.tag = child->tag;
        r.mcid = child->mcid;
        pageRegions[child->pageIndex].push_back(r);
    }
    for (int i = 0; i < pageCount; ++i) {
        auto it = pageRegions.find(i);
        if (it != pageRegions.end() && !it->second.empty()) {
            streamTagger.tagPageContent(doc, i, it->second);
        }
    }

    // Write both XMP identifications
    XmpMetadataWriter xmpWriter(impl_->context_);
    xmpWriter.writePdfAIdentification(doc, PdfALevel::PdfA_2a);
    xmpWriter.writePdfUAIdentification(doc, PdfUALevel::PdfUA_1);

    // Now do the standard PDF/A conversion pipeline
    PdfAConverter converter(impl_->context_);
    return converter.convertAndSave(doc, PdfALevel::PdfA_2a, outputPath);
}

ComplianceResult ComplianceManager::analyzeForConversion(
    pdfe::core::Document* doc,
    PdfALevel level)
{
    PdfAConverter converter(impl_->context_);
    return converter.analyzeForConversion(doc, level);
}

// ----- Pipeline E phase-12: Fix-All workflow -----

namespace {

// Resolve the type/strategy on an issue, falling back to rule-based
// classification when the validator hasn't tagged it yet.
ComplianceIssue resolved(const ComplianceIssue& in) {
    ComplianceIssue out = in;
    if (out.type == IssueType::Unknown) {
        out.type = classifyIssueByRule(in.rule);
    }
    if (out.fixStrategy == FixStrategy::Manual
        && defaultFixStrategy(out.type) == FixStrategy::Auto) {
        out.fixStrategy = FixStrategy::Auto;
    }
    return out;
}

} // namespace

std::vector<ComplianceIssue>
ComplianceManager::autoFixableIssues(
    const std::vector<ComplianceIssue>& issues)
{
    std::vector<ComplianceIssue> out;
    out.reserve(issues.size());
    for (const auto& i : issues) {
        ComplianceIssue r = resolved(i);
        if (r.fixStrategy == FixStrategy::Auto) {
            out.push_back(std::move(r));
        }
    }
    return out;
}

ComplianceManager::FixResult
ComplianceManager::fixIssue(pdfe::core::Document* doc,
                            const ComplianceIssue& issue)
{
    // Back-compat overload — defaults to PDF/A-2b. New callers should
    // supply an explicit level via the 3-arg form.
    return fixIssue(doc, issue, PdfALevel::PdfA_2b);
}

ComplianceManager::FixResult
ComplianceManager::fixIssue(pdfe::core::Document* doc,
                            const ComplianceIssue& issue,
                            PdfALevel level)
{
    FixResult r;
    if (!doc) {
        r.errorMessage = "Document is null";
        return r;
    }
    // PDF/A compliance fixes rewrite document structure (XMP, fonts, color);
    // force a full-rewrite save (pipeline b2e47d91, Tier 1). Covers the 2-arg
    // overload, which delegates here.
    doc->markRequiresFullRewrite();
    ComplianceIssue resolvedIssue = resolved(issue);
    if (resolvedIssue.fixStrategy == FixStrategy::Manual) {
        r.errorMessage = "Issue '" + issueTypeName(resolvedIssue.type)
                         + "' requires manual fix";
        return r;
    }
    try {
        switch (resolvedIssue.type) {
        case IssueType::MissingXmpMetadata: {
            XmpMetadataWriter writer(impl_->context_);
            // Pipeline F phase-1: stamp the XMP with the active
            // target level so re-validation against PDF/A-1b doesn't
            // surface a level-mismatch warning. Falls back to 2b if
            // None is passed (defensive, shouldn't happen via fixAll).
            PdfALevel target = (level == PdfALevel::None)
                ? PdfALevel::PdfA_2b
                : level;
            writer.writePdfAIdentification(doc, target);
            r.ok = true;
            return r;
        }
        case IssueType::MissingOutputIntent: {
            IccProfileManager iccMgr(impl_->context_);
            iccMgr.addSrgbOutputIntent(doc);
            r.ok = true;
            return r;
        }
        case IssueType::ContainsJavaScript: {
            impl_->stripJavaScript(doc);
            r.ok = true;
            return r;
        }
        default:
            r.errorMessage = "No fixer registered for type "
                             + issueTypeName(resolvedIssue.type);
            return r;
        }
    } catch (const std::exception& ex) {
        r.errorMessage = std::string("Fixer threw: ") + ex.what();
        return r;
    } catch (...) {
        r.errorMessage = "Fixer threw unknown exception";
        return r;
    }
}

ComplianceManager::FixAllResult
ComplianceManager::fixAll(pdfe::core::Document* doc,
                          const std::vector<ComplianceIssue>& issues,
                          PdfALevel level)
{
    FixAllResult agg;
    if (!doc) {
        agg.errorMessages.push_back("Document is null");
        return agg;
    }
    // PDF/A compliance fixes rewrite document structure; force a full-rewrite
    // save (pipeline b2e47d91, Tier 1).
    doc->markRequiresFullRewrite();
    // Pipeline F phase-1: surface wrong-level callers at runtime.
    // PdfALevel::None is meaningless for fix-all (the XMP fixer would
    // silently fall back to PDF/A-2b). If a real caller ever passes
    // None, that is a bug in the caller, not something to paper over.
    if (level == PdfALevel::None) {
        agg.errorMessages.push_back(
            "fixAll requires an explicit PdfALevel target (got None)");
        return agg;
    }

    // De-duplicate by issue type — multiple "Missing XMP" findings
    // are the same physical fix, applying it once is sufficient and
    // running the fixer twice should still be safe (idempotent).
    std::vector<IssueType> typesApplied;
    auto alreadyApplied = [&](IssueType t) {
        return std::find(typesApplied.begin(), typesApplied.end(), t)
               != typesApplied.end();
    };

    for (const auto& issue : issues) {
        ComplianceIssue r = resolved(issue);
        if (r.fixStrategy != FixStrategy::Auto) continue;
        if (alreadyApplied(r.type)) continue;

        agg.attempted++;
        // Pipeline F phase-1: forward the batch-level target so the
        // XMP fixer (and any future level-sensitive fixer) stamps the
        // correct pdfaid:part instead of always 2b.
        FixResult fr = fixIssue(doc, r, level);
        if (fr.ok) {
            agg.succeeded++;
            typesApplied.push_back(r.type);
        } else {
            agg.failed++;
            agg.errorMessages.push_back(fr.errorMessage);
        }
    }

    // Re-validate to populate stillRemaining. Manual issues are
    // surfaced even if we never tried to fix them — the AC explicitly
    // requires manual-only issues NOT to be silently dropped.
    ComplianceResult after = validatePdfA(doc, level);
    for (auto& post : after.issues) {
        agg.stillRemaining.push_back(resolved(post));
    }
    return agg;
}

} // namespace pdfe::compliance
