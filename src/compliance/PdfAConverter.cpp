#include "PdfAConverter.h"
#include "XmpMetadataWriter.h"
#include "IccProfileManager.h"
#include "FontComplianceChecker.h"
#include "ProhibitedFeatureScanner.h"
#include "PdfAValidator.h"
#include "fonts/FontResolver.h"
#include "../core/MuPdfContext.h"
#include "../core/Document.h"
#include "../core/QpdfWrapper.h"
#include "../core/Error.h"

#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <unordered_map>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>

namespace pdfe::compliance {

namespace {

// The 14 standard PDF base fonts (ISO 32000-1, Annex D.5). A
// conforming PDF/A producer MUST embed these — leaving the reference
// unembedded is a rule 6.3.3 violation.
const std::set<std::string>& standardFourteen() {
    static const std::set<std::string> kSet = {
        "Helvetica", "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique",
        "Times-Roman", "Times-Bold", "Times-Italic", "Times-BoldItalic",
        "Courier", "Courier-Bold", "Courier-Oblique", "Courier-BoldOblique",
        "Symbol", "ZapfDingbats"
    };
    return kSet;
}

// Strip subset prefix "XXXXXX+" if present so we match the base name.
std::string stripSubsetPrefix(const std::string& name) {
    if (name.size() > 7 && name[6] == '+') {
        bool allUpper = true;
        for (size_t i = 0; i < 6; ++i) {
            if (name[i] < 'A' || name[i] > 'Z') { allUpper = false; break; }
        }
        if (allUpper) return name.substr(7);
    }
    return name;
}

// Resolve a standard-14 PDF font name to a TTF file path via
// FontResolver. FontResolver checks bundled fonts first (security:
// trusted source), then queries the OS via fontconfig (Linux/macOS)
// or DirectWrite (Windows), then walks the metric-compatible
// substitution chain (Arial → Liberation Sans → Arimo etc.).
// No hardcoded paths — locations come from the OS so they stay
// correct across distro / relocated system roots / user-configured
// font dirs / XDG_DATA_DIRS.
std::string resolveTtfReplacementImpl(const std::string& baseFontName) {
    std::string clean = stripSubsetPrefix(baseFontName);
    return pdfe::fonts::FontResolver::resolveStandard14(clean);
}

} // anonymous namespace

bool PdfAConverter::isStandardFourteen(const std::string& baseFontName) {
    return standardFourteen().count(stripSubsetPrefix(baseFontName)) > 0;
}

std::string PdfAConverter::resolveTtfReplacement(const std::string& baseFontName) {
    return resolveTtfReplacementImpl(baseFontName);
}

class PdfAConverter::Impl {
public:
    explicit Impl(std::shared_ptr<pdfe::core::MuPdfContext> context)
        : context_(std::move(context))
    {}

    std::shared_ptr<pdfe::core::MuPdfContext> context_;

    void removeJavaScript(pdfe::core::Document* doc) {
        context_->execute([doc](fz_context* ctx) {
            pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
            if (!pdfDoc) return;

            pdf_obj* catalog = pdf_dict_get(ctx, pdf_trailer(ctx, pdfDoc), PDF_NAME(Root));
            if (!catalog) return;

            // Remove /Names/JavaScript from catalog
            pdf_obj* names = pdf_dict_get(ctx, catalog, PDF_NAME(Names));
            if (names) {
                pdf_dict_dels(ctx, names, "JavaScript");
            }

            // Remove /OpenAction if it's JavaScript
            pdf_obj* openAction = pdf_dict_gets(ctx, catalog, "OpenAction");
            if (openAction) {
                pdf_obj* s = pdf_dict_get(ctx, openAction, PDF_NAME(S));
                if (s && std::strcmp(pdf_to_name(ctx, s), "JavaScript") == 0) {
                    pdf_dict_dels(ctx, catalog, "OpenAction");
                }
            }

            // Remove JS from page-level /AA dictionaries
            int pageCount = pdf_count_pages(ctx, pdfDoc);
            for (int i = 0; i < pageCount; ++i) {
                pdf_obj* pageObj = pdf_lookup_page_obj(ctx, pdfDoc, i);
                if (!pageObj) continue;
                pdf_dict_del(ctx, pageObj, PDF_NAME(AA));
            }

            // Remove JS from annotation /A and /AA
            for (int i = 0; i < pageCount; ++i) {
                pdf_obj* pageObj = pdf_lookup_page_obj(ctx, pdfDoc, i);
                if (!pageObj) continue;

                pdf_obj* annots = pdf_dict_get(ctx, pageObj, PDF_NAME(Annots));
                if (!annots) continue;

                int annotLen = pdf_array_len(ctx, annots);
                for (int j = 0; j < annotLen; ++j) {
                    pdf_obj* annot = pdf_array_get(ctx, annots, j);

                    // Remove /AA from annotations
                    pdf_dict_del(ctx, annot, PDF_NAME(AA));

                    // Remove /A if it's JavaScript
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

    void setMarkInfo(pdfe::core::Document* doc) {
        context_->execute([doc](fz_context* ctx) {
            pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
            if (!pdfDoc) return;

            pdf_obj* catalog = pdf_dict_get(ctx, pdf_trailer(ctx, pdfDoc), PDF_NAME(Root));
            if (!catalog) return;

            pdf_obj* markInfo = pdf_dict_gets(ctx, catalog, "MarkInfo");
            if (!markInfo) {
                markInfo = pdf_new_dict(ctx, pdfDoc, 1);
                pdf_dict_puts(ctx, catalog, "MarkInfo", markInfo);
                pdf_drop_obj(ctx, markInfo);
            }
            pdf_dict_puts(ctx, markInfo, "Marked", PDF_TRUE);
        });
    }

    void removeTransparencyGroups(pdfe::core::Document* doc) {
        context_->execute([doc](fz_context* ctx) {
            pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
            if (!pdfDoc) return;

            int pageCount = pdf_count_pages(ctx, pdfDoc);
            for (int i = 0; i < pageCount; ++i) {
                pdf_obj* pageObj = pdf_lookup_page_obj(ctx, pdfDoc, i);
                if (!pageObj) continue;

                pdf_obj* group = pdf_dict_get(ctx, pageObj, PDF_NAME(Group));
                if (group) {
                    pdf_obj* s = pdf_dict_get(ctx, group, PDF_NAME(S));
                    if (s && std::strcmp(pdf_to_name(ctx, s), "Transparency") == 0) {
                        pdf_dict_del(ctx, pageObj, PDF_NAME(Group));
                    }
                }
            }
        });
    }

    // Scan every page's /Resources/Font dict; for any font whose
    // /BaseFont is one of the standard-14 base fonts AND which is
    // not already embedded (no /FontDescriptor with FontFile/2/3),
    // replace the font resource with an embedded TrueType font built
    // from a system-font TTF (Arial for Helvetica, Times New Roman
    // for Times, etc.).
    //
    // We use pdf_add_simple_font with PDF_SIMPLE_ENCODING_LATIN so
    // the new font advertises WinAnsiEncoding — which is the default
    // encoding standard-14 Helvetica/Times/Courier already use, so
    // existing content-stream byte sequences continue to select the
    // correct glyphs.
    void substituteStandardFonts(pdfe::core::Document* doc) {
        context_->execute([doc](fz_context* ctx) {
            pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
            if (!pdfDoc) return;

            // Cache: TTF path -> pdf_obj* for the embedded font, so
            // we only embed each replacement once per document even
            // if many pages share the same standard-14 reference.
            std::unordered_map<std::string, pdf_obj*> embedCache;

            int pageCount = pdf_count_pages(ctx, pdfDoc);
            for (int i = 0; i < pageCount; ++i) {
                pdf_obj* pageObj = pdf_lookup_page_obj(ctx, pdfDoc, i);
                if (!pageObj) continue;

                pdf_obj* resources = pdf_dict_get(ctx, pageObj, PDF_NAME(Resources));
                if (!resources) continue;

                pdf_obj* fontDict = pdf_dict_get(ctx, resources, PDF_NAME(Font));
                if (!fontDict) continue;

                int len = pdf_dict_len(ctx, fontDict);
                for (int j = 0; j < len; ++j) {
                    pdf_obj* fontKey = pdf_dict_get_key(ctx, fontDict, j);
                    pdf_obj* fontRef = pdf_dict_get_val(ctx, fontDict, j);
                    if (!fontKey || !fontRef) continue;

                    pdf_obj* baseFont = pdf_dict_get(ctx, fontRef, PDF_NAME(BaseFont));
                    if (!baseFont) continue;

                    const char* baseFontName = pdf_to_name(ctx, baseFont);
                    if (!baseFontName) continue;

                    std::string nameStr = baseFontName;
                    if (!isStandardFourteen(nameStr)) continue;

                    // Already embedded? Check /FontDescriptor for FontFile*.
                    pdf_obj* descriptor = pdf_dict_get(ctx, fontRef, PDF_NAME(FontDescriptor));
                    bool alreadyEmbedded = false;
                    if (descriptor) {
                        alreadyEmbedded =
                            pdf_dict_get(ctx, descriptor, PDF_NAME(FontFile))  != nullptr ||
                            pdf_dict_get(ctx, descriptor, PDF_NAME(FontFile2)) != nullptr ||
                            pdf_dict_get(ctx, descriptor, PDF_NAME(FontFile3)) != nullptr;
                    }
                    if (alreadyEmbedded) continue;

                    // Resolve the replacement TTF on disk.
                    std::string ttfPath = resolveTtfReplacementImpl(nameStr);
                    if (ttfPath.empty()) {
                        // Nothing we can do — skip this font, validator
                        // will continue reporting 6.3.3 for it.
                        continue;
                    }

                    pdf_obj* embedded = nullptr;
                    auto cacheIt = embedCache.find(ttfPath);
                    if (cacheIt != embedCache.end()) {
                        embedded = cacheIt->second;
                    } else {
                        fz_font* ftFont = nullptr;
                        fz_try(ctx) {
                            ftFont = fz_new_font_from_file(
                                ctx, nullptr, ttfPath.c_str(), 0, 0);
                            // Standard-14 Helvetica/Times/Courier use
                            // WinAnsiEncoding by default — match it so
                            // existing content streams keep rendering
                            // the same glyphs.
                            embedded = pdf_add_simple_font(
                                ctx, pdfDoc, ftFont,
                                PDF_SIMPLE_ENCODING_LATIN);
                        }
                        fz_always(ctx) {
                            if (ftFont) fz_drop_font(ctx, ftFont);
                        }
                        fz_catch(ctx) {
                            // Failed to embed this one — leave it
                            // unembedded and continue with the next
                            // font reference rather than aborting
                            // the whole conversion.
                            embedded = nullptr;
                        }
                        if (embedded) {
                            embedCache[ttfPath] = embedded;
                        }
                    }

                    if (embedded) {
                        // Replace the font resource entry under the
                        // SAME key so existing content stream "/F1 Tf"
                        // (etc.) continues to resolve.
                        pdf_dict_put(ctx, fontDict, fontKey, embedded);
                    }
                }
            }

            // Note: embedCache holds the pdf_obj* returned by
            // pdf_add_simple_font. We do NOT drop these — matching
            // the convention established by src/fonts/PdfFontEmbedder
            // (pdf_add_cid_font return value is not dropped there
            // either). The dict-put increments the dict's internal
            // ref so the embedded font survives the cache going out
            // of scope.
        });
    }
};

PdfAConverter::PdfAConverter(std::shared_ptr<pdfe::core::MuPdfContext> context)
    : impl_(std::make_unique<Impl>(std::move(context)))
{}

PdfAConverter::~PdfAConverter() = default;

ComplianceResult PdfAConverter::analyzeForConversion(
    pdfe::core::Document* doc, PdfALevel level)
{
    ComplianceResult result;
    result.targetLevel = level;

    if (!doc || level == PdfALevel::None) {
        result.success = false;
        result.errorMessage = "Invalid document or level";
        return result;
    }

    ProhibitedFeatureScanner scanner(impl_->context_);

    // Check for issues we cannot auto-fix
    FontComplianceChecker fontChecker(impl_->context_);
    bool requireUnicode = levelRequiresUnicode(level);
    auto fonts = fontChecker.checkFonts(doc, requireUnicode);
    for (const auto& font : fonts) {
        if (!font.isEmbedded) {
            result.issues.push_back({
                IssueSeverity::Warning,
                "6.3.3",
                "Font '" + font.fontName + "' is not embedded (cannot auto-fix)",
                font.pageIndex, ""
            });
        }
    }

    // Check for features we CAN auto-fix (report as warnings)
    if (scanner.hasEncryption(doc)) {
        result.issues.push_back({
            IssueSeverity::Warning,
            "6.1.3",
            "Encryption will be removed during conversion",
            -1, ""
        });
    }

    if (scanner.hasJavaScript(doc)) {
        result.issues.push_back({
            IssueSeverity::Warning,
            "6.6.1",
            "JavaScript will be removed during conversion",
            -1, ""
        });
    }

    if (!levelAllowsTransparency(level) && scanner.hasTransparency(doc)) {
        result.issues.push_back({
            IssueSeverity::Warning,
            "6.4",
            "Transparency groups will be removed during conversion (PDF/A-1)",
            -1, ""
        });
    }

    result.success = true;
    return result;
}

ComplianceResult PdfAConverter::convertAndSave(
    pdfe::core::Document* doc,
    PdfALevel level,
    const std::filesystem::path& outputPath)
{
    ComplianceResult result;
    result.targetLevel = level;

    if (!doc || level == PdfALevel::None) {
        result.success = false;
        result.errorMessage = "Invalid document or level";
        return result;
    }

    try {
        // ================================================================
        // Stage 1: MuPDF in-memory modifications + save to temp
        // ================================================================

        // 1a. Write XMP metadata with PDF/A identification
        XmpMetadataWriter xmpWriter(impl_->context_);
        xmpWriter.writePdfAIdentification(doc, level);

        // 1b. Add sRGB OutputIntent if missing
        IccProfileManager iccMgr(impl_->context_);
        if (!iccMgr.hasOutputIntent(doc)) {
            iccMgr.addSrgbOutputIntent(doc);
        }

        // 1c. Set /MarkInfo /Marked true
        impl_->setMarkInfo(doc);

        // 1d. Remove JavaScript
        impl_->removeJavaScript(doc);

        // 1e. Remove transparency groups for PDF/A-1
        if (!levelAllowsTransparency(level)) {
            impl_->removeTransparencyGroups(doc);
        }

        // 1f. Embed standard-14 fonts (Helvetica, Times-Roman, etc.)
        // PDF/A requires every font to be embedded (rule 6.3.3). The
        // standard-14 base fonts get a free pass in plain PDF but
        // MUST be embedded for PDF/A conformance.
        impl_->substituteStandardFonts(doc);

        // 1g/2. Bake and structurally rewrite through EXCLUSIVE HANDLES —
        // WIN-87 phase-59.
        //
        // This used to be a three-step by-path relay: MuPDF wrote
        // "<outputPath>.pdfe_pdfa_tmp" BY NAME, nothing held it, QPDF REOPENED
        // IT BY NAME, and then QPDFWriter wrote DIRECTLY over outputPath. Two
        // separate defects. An attacker who knows where you are converting knows
        // that temp's name and can have PDFE republish their bytes as the
        // "PDF/A" output — the WIN-87 substitution class, site #3's exact shape.
        // And there was NO atomic publish at all, so a failure part-way through
        // the QPDF write left the user's output TRUNCATED: the R2-04 data-loss
        // shape, which every other publish path in this product had already had
        // removed.
        //
        // Both close together. The bake goes into a StagingFile behind an
        // exclusive handle at an unpredictable name, QPDF reads a DUPLICATE of
        // that handle and writes into a second one, and only that output is
        // published — by renaming through its handle. outputPath is never
        // written in place and never removed first.
        doc->saveAsThroughExternalRewrite(
            outputPath,
            [level](std::FILE* in, std::FILE* out) {
                // Ownership of BOTH streams is ours on every path; a stream
                // still open in THIS process blocks the publishing rename.
                struct OutGuard {
                    std::FILE* f;
                    ~OutGuard() { if (f) std::fclose(f); }
                } outGuard{out};

                core::QpdfWrapper qpdf;
                qpdf.openFromStream(in, "<staged PDF/A bake handle>");
                std::FILE* owned = outGuard.f;
                outGuard.f = nullptr;
                qpdf.savePdfAToStream(owned, !levelAllowsObjStreams(level));
            });

        // ================================================================
        // Stage 3: Post-save verification
        // ================================================================
        auto verifyDoc = pdfe::core::Document::open(impl_->context_, outputPath);
        if (!verifyDoc) {
            result.success = false;
            result.errorMessage = "Failed to open converted document for verification";
            return result;
        }

        PdfAValidator validator(impl_->context_);
        result = validator.validate(verifyDoc.get(), level);

    } catch (const std::exception& e) {
        // Nothing to clean up: both staging files are owned by
        // saveAsThroughExternalRewrite and die with it, and outputPath was
        // never written in place.
        result.success = false;
        result.errorMessage = std::string("Conversion failed: ") + e.what();
    }

    return result;
}

} // namespace pdfe::compliance
