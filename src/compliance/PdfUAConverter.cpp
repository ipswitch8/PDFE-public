#include "PdfUAConverter.h"
#include "XmpMetadataWriter.h"
#include "IccProfileManager.h"
#include "StructureTreeBuilder.h"
#include "AutoTagger.h"
#include "ContentStreamTagger.h"
#include "PageFurnitureDetector.h"
#include "ProhibitedFeatureScanner.h"
#include "../core/MuPdfContext.h"
#include "../core/Document.h"
#include "../core/QpdfWrapper.h"
#include "../core/Error.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>

namespace pdfe::compliance {

class PdfUAConverter::Impl {
public:
    explicit Impl(std::shared_ptr<pdfe::core::MuPdfContext> context)
        : context_(std::move(context))
    {}

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
};

PdfUAConverter::PdfUAConverter(std::shared_ptr<pdfe::core::MuPdfContext> context)
    : impl_(std::make_unique<Impl>(std::move(context)))
{}

PdfUAConverter::~PdfUAConverter() = default;

ComplianceResult PdfUAConverter::convertAndSave(
    pdfe::core::Document* doc,
    const std::filesystem::path& outputPath,
    const std::string& language)
{
    ComplianceResult result;
    result.targetUALevel = PdfUALevel::PdfUA_1;

    if (!doc) {
        result.success = false;
        result.errorMessage = "Document is null";
        return result;
    }

    try {
        // 1. Set document language
        StructureTreeBuilder treeBuilder(impl_->context_);
        treeBuilder.setDocumentLanguage(doc, language);

        // 2a. GitHub #173 — decide what is PAGE FURNITURE before anything
        //     assigns an MCID. Running heads, running feet and folios are
        //     artifacts (PDF 32000-1 14.8.2.2); leaving them as content is what
        //     makes a screen reader announce "Confidential Draft" at the top of
        //     every page. Detection has to happen HERE and not after tagging:
        //     an artifact carries no /MCID, so a post-pass would have to unpick
        //     ids that were never legitimate.
        AutoTagger autoTagger(impl_->context_);
        std::vector<FurnitureRegion> furniture =
            autoTagger.detectPageFurniture(doc);

        //     ...and the SECOND, independent furniture signal: a rotated,
        //     centred watermark. It is not a widening of the band-plus-repeat
        //     rule above and cannot be: a watermark sits at the page centre and
        //     is routinely the only page-furniture item on a single-page
        //     document, where the repeat test returns empty by construction.
        //     Concatenated here so everything downstream — tagDocument,
        //     artifactRegionsForPage, markArtifacts — is unchanged.
        {
            const std::vector<FurnitureRegion> watermarks =
                autoTagger.detectWatermarks(doc);
            furniture.insert(furniture.end(), watermarks.begin(),
                             watermarks.end());
        }

        // 2b. Auto-tag the document, with the furniture excluded from the
        //     structure stream.
        auto root = autoTagger.tagDocument(doc, furniture);

        // 3. Build structure tree
        treeBuilder.buildTree(doc, root);

        // 4. Inject marked content into page streams
        //    IMPORTANT: Reuse MCIDs from the structure tree (root's children),
        //    NOT from a second tagPage() call which could produce different MCIDs.
        ContentStreamTagger streamTagger(impl_->context_);
        int pageCount = doc->pageCount();
        // Group structure nodes by page index
        std::map<int, std::vector<MarkedContentRegion>> pageRegions;
        for (const auto& child : root.children) {
            // An artifact node is NOT structure content: it gets no MCID, no
            // /StructElem and no ParentTree slot, so it must not be fed to
            // tagPageContent either. It is marked separately below.
            if (child->tag == StructureTag::Artifact) continue;
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

        // 4b. GitHub #173 — THE CALL SITE. Emit the detected furniture as
        //     "/Artifact <</Type /Pagination /Subtype /Header|/Footer
        //     /BBox [...]>> BDC ... EMC". markArtifacts APPENDS, so it runs
        //     after tagPageContent (which replaces the stream) rather than
        //     before, and returns a count so a no-op cannot read as success.
        int artifactsMarked = 0;
        for (int i = 0; i < pageCount; ++i) {
            const auto pageArtifacts = artifactRegionsForPage(furniture, i);
            if (pageArtifacts.empty()) continue;
            artifactsMarked += streamTagger.markArtifacts(doc, i, pageArtifacts);
        }
        const bool artifactMarkingFailed =
            !furniture.empty() && artifactsMarked == 0;

        // 5. Write XMP with pdfuaid
        XmpMetadataWriter xmpWriter(impl_->context_);
        xmpWriter.writePdfUAIdentification(doc, PdfUALevel::PdfUA_1);

        // 6. Add OutputIntent if missing
        IccProfileManager iccMgr(impl_->context_);
        if (!iccMgr.hasOutputIntent(doc)) {
            iccMgr.addSrgbOutputIntent(doc);
        }

        // 7/8. Bake and structurally rewrite through EXCLUSIVE HANDLES —
        // WIN-87 phase-59. Identical defect and identical fix to
        // PdfAConverter::convert; see the long comment there. In short: MuPDF
        // wrote "<outputPath>.pdfe_pdfua_tmp" BY NAME, QPDF REOPENED IT BY
        // NAME, and QPDFWriter then wrote DIRECTLY over outputPath — the WIN-87
        // substitution class plus an R2-04 data-loss window, in one sequence.
        //
        // PDF/UA always generates object streams (there is no PDF/A-1-style
        // level that forbids them), so disableObjectStreams is false here.
        doc->saveAsThroughExternalRewrite(
            outputPath,
            [](std::FILE* in, std::FILE* out) {
                struct OutGuard {
                    std::FILE* f;
                    ~OutGuard() { if (f) std::fclose(f); }
                } outGuard{out};

                core::QpdfWrapper qpdf;
                qpdf.openFromStream(in, "<staged PDF/UA bake handle>");
                std::FILE* owned = outGuard.f;
                outGuard.f = nullptr;
                qpdf.savePdfAToStream(owned, /*disableObjectStreams=*/false);
            });

        // 9. Verify
        auto verifyDoc = pdfe::core::Document::open(impl_->context_, outputPath);
        if (verifyDoc) {
            result = validatePdfUA(verifyDoc.get());
        } else {
            result.success = false;
            result.errorMessage = "Failed to open converted document for verification";
        }

        // markArtifacts returns a COUNT precisely so that "marked nothing"
        // cannot pass for success. Detecting furniture and then emitting none of
        // it is a silent accessibility regression, so it is reported.
        if (artifactMarkingFailed) {
            result.issues.push_back({
                IssueSeverity::Warning,
                // 7.1 General: content is either tagged real content or marked
                // as an artifact. Furniture that is neither violates that.
                "7.1",
                "Page furniture was detected but no artifact marking was "
                "emitted; running heads/footers remain tagged as content",
                -1, ""
            });
            result.success = !result.hasErrors();
        }

    } catch (const std::exception& e) {
        // Nothing to clean up: the staging files die with the save.
        result.success = false;
        result.errorMessage = std::string("PDF/UA conversion failed: ") + e.what();
    }

    return result;
}

// ── GitHub #172: reading /StructTreeRoot back into StructureNodes ─────────
namespace {

// A tagged PDF's structure tree is arbitrarily deep and, in a hostile file,
// cyclic. The cap bounds both.
constexpr int kMaxStructDepth = 64;

std::shared_ptr<StructureNode> readStructElem(
    fz_context* ctx, pdf_document* pdfDoc, pdf_obj* elem, int depth);

void readStructKids(fz_context* ctx, pdf_document* pdfDoc, pdf_obj* k,
                    StructureNode& parent, int depth)
{
    if (!k || depth > kMaxStructDepth) return;

    auto addIfElem = [&](pdf_obj* candidate) {
        if (!pdf_is_dict(ctx, candidate)) return;
        // A /K entry may also be a marked-content reference (/Type /MCR) or an
        // object reference (/OBJR); only a structure element carries /S.
        if (!pdf_dict_get(ctx, candidate, PDF_NAME(S))) return;
        parent.children.push_back(
            readStructElem(ctx, pdfDoc, candidate, depth));
    };

    if (pdf_is_array(ctx, k)) {
        const int n = pdf_array_len(ctx, k);
        for (int i = 0; i < n; ++i) addIfElem(pdf_array_get(ctx, k, i));
    } else {
        addIfElem(k);
    }
}

std::shared_ptr<StructureNode> readStructElem(
    fz_context* ctx, pdf_document* pdfDoc, pdf_obj* elem, int depth)
{
    auto node = std::make_shared<StructureNode>();

    const char* s = pdf_to_name(ctx, pdf_dict_get(ctx, elem, PDF_NAME(S)));
    node->tag = nameToStructureTag(s);
    if (s && *s && std::string(structureTagToName(node->tag)) != s) {
        node->customTag = s;
    }

    // /Alt is the alternative description. /ActualText is a replacement for the
    // glyphs themselves; PDF/UA accepts either as a description of a figure, so
    // a figure carrying only /ActualText must NOT be reported — reporting it
    // would make the rule fire on compliant documents, which is worse than not
    // having the rule at all.
    if (pdf_obj* altObj = pdf_dict_get(ctx, elem, PDF_NAME(Alt))) {
        const char* alt = pdf_to_text_string(ctx, altObj);
        if (alt && *alt) node->altText = alt;
    }
    if (node->altText.empty()) {
        if (pdf_obj* actObj = pdf_dict_gets(ctx, elem, "ActualText")) {
            const char* actual = pdf_to_text_string(ctx, actObj);
            if (actual && *actual) node->altText = actual;
        }
    }

    if (pdf_obj* pg = pdf_dict_gets(ctx, elem, "Pg")) {
        node->pageIndex = pdf_lookup_page_number(ctx, pdfDoc, pg);
    }

    pdf_obj* k = pdf_dict_get(ctx, elem, PDF_NAME(K));
    if (pdf_is_int(ctx, k)) {
        node->mcid = pdf_to_int(ctx, k);
    } else if (k) {
        readStructKids(ctx, pdfDoc, k, *node, depth + 1);
    }

    // The flag AutoTagger raises on a figure it cannot describe, raised again
    // here on ingestion. One field, two producers, one consumer
    // (figuresMissingAltText) — which is what "nothing reads needsAltText"
    // was describing the absence of.
    node->needsAltText =
        (node->tag == StructureTag::Figure) && node->altText.empty();

    return node;
}

void collectFiguresMissingAlt(const StructureNode& node,
                              std::vector<const StructureNode*>& out)
{
    if (node.tag == StructureTag::Figure && node.needsAltText &&
        node.altText.empty()) {
        out.push_back(&node);
    }
    for (const auto& child : node.children) {
        if (child) collectFiguresMissingAlt(*child, out);
    }
}

} // namespace

StructureNode PdfUAConverter::readStructureTree(pdfe::core::Document* doc) {
    StructureNode root;
    root.tag = StructureTag::Document;
    if (!doc) return root;

    impl_->context_->execute([&](fz_context* ctx) {
        pdf_document* pdfDoc =
            pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdfDoc) return;

        pdf_obj* catalog = pdf_dict_get(ctx, pdf_trailer(ctx, pdfDoc), PDF_NAME(Root));
        if (!catalog) return;

        pdf_obj* treeRoot = pdf_dict_get(ctx, catalog, PDF_NAME(StructTreeRoot));
        if (!treeRoot) return;

        readStructKids(ctx, pdfDoc, pdf_dict_get(ctx, treeRoot, PDF_NAME(K)),
                       root, 0);
    });

    return root;
}

std::vector<const StructureNode*> PdfUAConverter::figuresMissingAltText(
    const StructureNode& root)
{
    std::vector<const StructureNode*> out;
    collectFiguresMissingAlt(root, out);
    return out;
}

ComplianceResult PdfUAConverter::validatePdfUA(pdfe::core::Document* doc) {
    ComplianceResult result;
    result.targetUALevel = PdfUALevel::PdfUA_1;

    if (!doc) {
        result.success = false;
        result.errorMessage = "Document is null";
        return result;
    }

    // Check structure tree
    StructureTreeBuilder treeBuilder(impl_->context_);
    if (!treeBuilder.hasStructureTree(doc)) {
        result.issues.push_back({
            IssueSeverity::Error,
            "7.1",
            "No structure tree found (required for PDF/UA)",
            -1, ""
        });
    }

    // Check document language
    impl_->context_->execute([&](fz_context* ctx) {
        pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdfDoc) return;

        pdf_obj* catalog = pdf_dict_get(ctx, pdf_trailer(ctx, pdfDoc), PDF_NAME(Root));
        if (!catalog) return;

        const pdf_obj* lang = pdf_dict_get(ctx, catalog, PDF_NAME(Lang));
        if (!lang) {
            result.issues.push_back({
                IssueSeverity::Error,
                "7.2",
                "No document language set (required for PDF/UA)",
                -1, ""
            });
        }
    });

    // ── 7.3 Graphics (GitHub #172) ───────────────────────────────────────
    //
    // ISO 14289-1 7.3 requires alternative text on every /Figure. PDFE already
    // KNEW which figures lacked it — AutoTagger sets StructureNode::needsAltText
    // and deliberately refuses to fabricate a placeholder — but nothing read the
    // flag, so a document with an undescribed image validated clean. It no
    // longer does.
    {
        const StructureNode structRoot = readStructureTree(doc);
        for (const StructureNode* fig : figuresMissingAltText(structRoot)) {
            result.issues.push_back({
                IssueSeverity::Error,
                kRuleGraphicsAltText,
                "Figure has no alternative text (/Alt or /ActualText); a "
                "screen-reader user is told nothing about this image",
                fig->pageIndex, ""
            });
        }
    }

    // ── 7.16 Security ────────────────────────────────────────────────────
    //
    // RENUMBERED (GitHub #172). This check used to be filed under "7.3",
    // which is ISO 14289-1's *Graphics* clause — the clause that governs
    // figures and alternative text, not security. It now sits on 7.16
    // (*Security*), the clause that actually requires encryption not to block
    // assistive technology, freeing 7.3 for the rule above.
    //
    // SEVERITY DECIDED (GitHub #194). The issue set out two defensible
    // readings and asked for an explicit choice. The choice is reading (2),
    // made permission-aware rather than a blanket flip.
    //
    // WHAT THIS REASONING RESTS ON, stated here rather than only in the audit
    // note, because the next person to change this will read the code and not
    // that note. **No verbatim ISO 14289-1 clause 7.16 text exists anywhere in
    // this repository, and none was consulted.** The argument below is an
    // INFERENCE from three things we do have: the clause title (*Security*,
    // established when GitHub #172 renumbered this check off 7.3), ISO
    // 32000-1's definition of Table 22 bit 10, and the framing in #194 itself.
    // It is not a quotation and should not be cited as one. If a copy of
    // ISO 14289-1 ever becomes available and its 7.16 text contradicts any of
    // this, the spec wins and this block is what to revisit first.
    // Full disposition: docs/audit/p87-open-issues/phase-2/notes.md.
    //
    //   * On this reading, 7.16 does not forbid encryption. It forbids a security
    //     handler configured so that assistive technology cannot get at the
    //     content. ISO 32000-1's Table 22 bit 10 — PDF_PERM_ACCESSIBILITY,
    //     1<<9, "extract text and graphics ... for use by assistive
    //     technology" — is the single bit that expresses that. So the clause
    //     is about the BIT, not about /Encrypt being present.
    //
    //   * Encrypted with accessibility extraction PERMITTED -> Warning.
    //     Reading (1) of the issue is right for this case: the document is
    //     conformant, and failing it would be a false positive on every
    //     legitimately password-protected PDF/UA file. The warning stays
    //     because AT that cannot supply the password still cannot read it —
    //     that is a real caveat, but it is not a 7.16 violation.
    //
    //   * Encrypted with accessibility extraction DENIED -> Error.
    //     This is the case the clause exists to prevent, and it is exactly
    //     the case the old blanket Warning let through: ComplianceResult::
    //     success is !hasErrors(), so validatePdfUA() returned "valid" for a
    //     document ISO 14289-1 forbids and a screen-reader user cannot open.
    //
    // Document::extractForAccessibilityAllowed() reads the LIVE permission
    // word of the document as opened (an owner-password open would grant
    // everything, which is why the denied-permission test writes an empty
    // user password). It returns true for an unencrypted document, so it is
    // only consulted once encryption has been established. QPDF refuses to
    // clear bit 10 above R3 because PDF 2.0 deprecates it, so in practice
    // only R2..R4 documents can reach the Error branch — which is correct:
    // above R3 the permission genuinely is always granted.
    ProhibitedFeatureScanner scanner(impl_->context_);
    if (scanner.hasEncryption(doc)) {
        if (!doc->extractForAccessibilityAllowed()) {
            result.issues.push_back({
                IssueSeverity::Error,
                kRuleSecurity,
                "Document is encrypted with the accessibility-extraction "
                "permission denied (ISO 32000-1 Table 22 bit 10); assistive "
                "technology is not permitted to read the content",
                -1, ""
            });
        } else {
            result.issues.push_back({
                IssueSeverity::Warning,
                kRuleSecurity,
                "Document is encrypted (may restrict accessibility)",
                -1, ""
            });
        }
    }

    result.success = !result.hasErrors();
    return result;
}

} // namespace pdfe::compliance
