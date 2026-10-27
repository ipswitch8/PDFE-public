#include "fonts/VectorSubsetDetector.h"

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <unordered_set>

namespace pdfe::fonts {

bool VectorSubsetDetector::hasSubsetPrefix(const std::string& baseFont)
{
    // PDF subset convention: six uppercase ASCII letters + '+' + actual name.
    // E.g. "AAAAAA+Alef" or "REFXWK+Liberation Serif".
    if (baseFont.size() < 8)
        return false;
    if (baseFont[6] != '+')
        return false;
    for (int i = 0; i < 6; ++i) {
        char c = baseFont[i];
        if (c < 'A' || c > 'Z')
            return false;
    }
    return true;
}

/// Walk the page's `/Resources/Font` dict and return every `BaseFont` name,
/// including `BaseFont` entries on `/DescendantFonts[0]` for Type0 wrappers
/// (since the embedded subset prefix lives on the descendant for CID fonts).
///
/// Runs entirely inside a single `MuPdfContext::execute` so the pdf_obj
/// pointers stay valid for the duration of the callback. Returns the full
/// list — caller filters by `hasSubsetPrefix` if it only wants subsetted
/// names.
std::vector<std::string> VectorSubsetDetector::collectBaseFontNames(
    pdfe::core::Page* page,
    pdfe::core::Document* document,
    pdfe::core::MuPdfContext* context)
{
    std::vector<std::string> names;
    if (!page || !document || !context)
        return names;

    try {
        context->execute([&](fz_context* ctx) {
            fz_page* fz_pg = page->fzPage();
            if (!fz_pg)
                return;
            pdf_page* pdf_pg = pdf_page_from_fz_page(ctx, fz_pg);
            if (!pdf_pg)
                return;

            pdf_obj* resources = pdf_page_resources(ctx, pdf_pg);
            if (!resources)
                return;

            pdf_obj* fontDict = pdf_dict_get(ctx, resources, PDF_NAME(Font));
            if (!fontDict)
                return;

            int n = pdf_dict_len(ctx, fontDict);
            std::unordered_set<std::string> seen;
            for (int i = 0; i < n; ++i) {
                pdf_obj* fontRef = pdf_dict_get_val(ctx, fontDict, i);
                if (!fontRef)
                    continue;
                fontRef = pdf_resolve_indirect(ctx, fontRef);
                if (!fontRef)
                    continue;

                // Top-level BaseFont (e.g. simple Type1/TrueType) — record it.
                pdf_obj* baseFont = pdf_dict_get(ctx, fontRef, PDF_NAME(BaseFont));
                if (baseFont) {
                    const char* nm = pdf_to_name(ctx, baseFont);
                    if (nm && *nm) {
                        std::string s(nm);
                        if (seen.insert(s).second)
                            names.push_back(std::move(s));
                    }
                }

                // Type0 → walk DescendantFonts[0] for the embedded BaseFont.
                pdf_obj* descendants =
                    pdf_dict_get(ctx, fontRef, PDF_NAME(DescendantFonts));
                if (descendants && pdf_array_len(ctx, descendants) > 0) {
                    pdf_obj* cidFont = pdf_resolve_indirect(
                        ctx, pdf_array_get(ctx, descendants, 0));
                    if (cidFont) {
                        pdf_obj* cidBase =
                            pdf_dict_get(ctx, cidFont, PDF_NAME(BaseFont));
                        if (cidBase) {
                            const char* nm = pdf_to_name(ctx, cidBase);
                            if (nm && *nm) {
                                std::string s(nm);
                                if (seen.insert(s).second)
                                    names.push_back(std::move(s));
                            }
                        }
                    }
                }
            }
        });
    } catch (...) {
        // Swallow MuPDF errors — return whatever we collected so far.
    }

    return names;
}

std::vector<std::string> VectorSubsetDetector::subsettedFontNames(
    pdfe::core::Page* page,
    pdfe::core::Document* document,
    pdfe::core::MuPdfContext* context)
{
    std::vector<std::string> all =
        VectorSubsetDetector::collectBaseFontNames(page, document, context);
    std::vector<std::string> subset;
    subset.reserve(all.size());
    for (auto& nm : all) {
        if (hasSubsetPrefix(nm))
            subset.push_back(std::move(nm));
    }
    return subset;
}

PageFontClass VectorSubsetDetector::classify(
    pdfe::core::Page* page,
    pdfe::core::Document* document,
    pdfe::core::MuPdfContext* context)
{
    if (!page)
        return PageFontClass::VectorComplete;

    // Image-dominated wins regardless of fonts on the page — the OCR
    // invisible-text layer typically ships with a placeholder font like
    // GlyphLessFont, which would otherwise misclassify as VectorComplete.
    if (page->isImageDominated())
        return PageFontClass::ImageDominated;

    auto subsetted =
        VectorSubsetDetector::subsettedFontNames(page, document, context);
    if (!subsetted.empty())
        return PageFontClass::VectorSubsetted;

    return PageFontClass::VectorComplete;
}

} // namespace pdfe::fonts
