#include "FontComplianceChecker.h"
#include "../core/MuPdfContext.h"
#include "../core/Document.h"
#include <algorithm>
#include <set>
#include <cstring>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

namespace pdfe::compliance {

class FontComplianceChecker::Impl {
public:
    explicit Impl(std::shared_ptr<pdfe::core::MuPdfContext> context)
        : context_(std::move(context))
    {}

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
};

FontComplianceChecker::FontComplianceChecker(std::shared_ptr<pdfe::core::MuPdfContext> context)
    : impl_(std::make_unique<Impl>(std::move(context)))
{}

FontComplianceChecker::~FontComplianceChecker() = default;

std::vector<FontComplianceInfo> FontComplianceChecker::checkPageFonts(
    pdfe::core::Document* doc,
    int pageIndex,
    bool requireUnicode)
{
    std::vector<FontComplianceInfo> results;
    if (!doc) return results;

    impl_->context_->execute([&](fz_context* ctx) {
        pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdfDoc) return;

        // Validate page index
        int count = pdf_count_pages(ctx, pdfDoc);
        if (pageIndex < 0 || pageIndex >= count) return;

        // Get the page object
        pdf_obj* pageObj = pdf_lookup_page_obj(ctx, pdfDoc, pageIndex);
        if (!pageObj) return;

        // Get the Resources dictionary
        pdf_obj* resources = pdf_dict_get(ctx, pageObj, PDF_NAME(Resources));
        if (!resources) return;

        // Get the Font dictionary from Resources
        pdf_obj* fontDict = pdf_dict_get(ctx, resources, PDF_NAME(Font));
        if (!fontDict) return;

        // Iterate through all fonts in the dictionary
        int len = pdf_dict_len(ctx, fontDict);
        for (int i = 0; i < len; ++i) {
            pdf_obj* fontRef = pdf_dict_get_val(ctx, fontDict, i);
            pdf_obj* fontKey = pdf_dict_get_key(ctx, fontDict, i);

            FontComplianceInfo info;
            info.pageIndex = pageIndex;

            // Get font name
            pdf_obj* baseFont = pdf_dict_get(ctx, fontRef, PDF_NAME(BaseFont));
            if (baseFont) {
                const char* baseFontName = pdf_to_name(ctx, baseFont);
                info.fontName = baseFontName ? baseFontName : "";
            } else {
                const char* keyName = pdf_to_name(ctx, fontKey);
                info.fontName = keyName ? keyName : "";
            }

            // Check font type
            pdf_obj* subtypeObj = pdf_dict_get(ctx, fontRef, PDF_NAME(Subtype));
            const char* subtype = subtypeObj ? pdf_to_name(ctx, subtypeObj) : "";
            bool isType0 = (std::strcmp(subtype, "Type0") == 0);
            bool isType3 = (std::strcmp(subtype, "Type3") == 0);

            // Check embedding
            pdf_obj* descriptor = pdf_dict_get(ctx, fontRef, PDF_NAME(FontDescriptor));
            if (descriptor) {
                info.isEmbedded = (
                    pdf_dict_get(ctx, descriptor, PDF_NAME(FontFile)) != nullptr ||
                    pdf_dict_get(ctx, descriptor, PDF_NAME(FontFile2)) != nullptr ||
                    pdf_dict_get(ctx, descriptor, PDF_NAME(FontFile3)) != nullptr
                );

                // Check symbolic flag (bit 3 of Flags)
                int flags = pdf_dict_get_int(ctx, descriptor, PDF_NAME(Flags));
                info.isSymbolic = (flags & 4) != 0;
            } else if (isType0) {
                // Type0 fonts: check descendant CIDFont for descriptor
                pdf_obj* descendants = pdf_dict_get(ctx, fontRef, PDF_NAME(DescendantFonts));
                if (descendants && pdf_array_len(ctx, descendants) > 0) {
                    pdf_obj* cidFont = pdf_array_get(ctx, descendants, 0);
                    pdf_obj* cidDescriptor = pdf_dict_get(ctx, cidFont, PDF_NAME(FontDescriptor));
                    if (cidDescriptor) {
                        info.isEmbedded = (
                            pdf_dict_get(ctx, cidDescriptor, PDF_NAME(FontFile)) != nullptr ||
                            pdf_dict_get(ctx, cidDescriptor, PDF_NAME(FontFile2)) != nullptr ||
                            pdf_dict_get(ctx, cidDescriptor, PDF_NAME(FontFile3)) != nullptr
                        );
                        int flags = pdf_dict_get_int(ctx, cidDescriptor, PDF_NAME(Flags));
                        info.isSymbolic = (flags & 4) != 0;
                    }
                }
            } else if (isType3) {
                // Type3 fonts have inline glyph definitions, considered "embedded"
                info.isEmbedded = true;
            }

            // Check ToUnicode CMap
            info.hasToUnicode = pdf_dict_get(ctx, fontRef, PDF_NAME(ToUnicode)) != nullptr;

            // Check glyph widths
            if (isType0) {
                // CIDFont: check /DW or /W
                pdf_obj* descendants = pdf_dict_get(ctx, fontRef, PDF_NAME(DescendantFonts));
                if (descendants && pdf_array_len(ctx, descendants) > 0) {
                    pdf_obj* cidFont = pdf_array_get(ctx, descendants, 0);
                    info.hasGlyphWidths = (
                        pdf_dict_get(ctx, cidFont, PDF_NAME(DW)) != nullptr ||
                        pdf_dict_get(ctx, cidFont, PDF_NAME(W)) != nullptr
                    );
                }
            } else {
                // Simple fonts: check /Widths array
                info.hasGlyphWidths = pdf_dict_get(ctx, fontRef, PDF_NAME(Widths)) != nullptr;
            }

            // Build issue list
            if (!info.isEmbedded) {
                info.issues.push_back("Font is not embedded");
            }
            if (!info.hasGlyphWidths && !isType3) {
                info.issues.push_back("Font has no glyph width definitions");
            }
            if (requireUnicode && !info.hasToUnicode && !info.isSymbolic) {
                info.issues.push_back("Font has no ToUnicode CMap (required for PDF/A-2u+)");
            }

            results.push_back(info);
        }
    });

    return results;
}

std::vector<FontComplianceInfo> FontComplianceChecker::checkFonts(
    pdfe::core::Document* doc,
    bool requireUnicode)
{
    std::vector<FontComplianceInfo> allFonts;
    if (!doc) return allFonts;

    std::set<std::string> seenFonts;

    int pageCount = doc->pageCount();
    for (int i = 0; i < pageCount; ++i) {
        auto pageFonts = checkPageFonts(doc, i, requireUnicode);

        for (const auto& font : pageFonts) {
            // Deduplicate fonts by name, keeping first occurrence.
            // insert() returns {iterator, bool}; the bool tells us
            // whether the entry was actually inserted, no separate
            // find() needed.
            if (seenFonts.insert(font.fontName).second) {
                allFonts.push_back(font);
            }
        }
    }

    return allFonts;
}

bool FontComplianceChecker::allFontsCompliant(
    pdfe::core::Document* doc,
    bool requireUnicode)
{
    auto fonts = checkFonts(doc, requireUnicode);
    return std::none_of(fonts.begin(), fonts.end(),
                        [](const auto& font) { return !font.issues.empty(); });
}

} // namespace pdfe::compliance
