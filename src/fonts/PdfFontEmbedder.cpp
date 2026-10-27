#include "fonts/PdfFontEmbedder.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/Document.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace pdfe::fonts {

PdfFontEmbedder::PdfFontEmbedder(pdfe::core::MuPdfContext* context)
    : context_(context)
{
}

PdfFontEmbedder::~PdfFontEmbedder()
{
}

std::optional<EmbeddedFontInfo> PdfFontEmbedder::embedFont(
    pdfe::core::Page* page,
    pdfe::core::Document* document,
    const std::string& fontFilePath)
{
    // Null check
    if (!page || !document || !context_ || fontFilePath.empty())
        return std::nullopt;

    // Embedding a font program mutates the document's object graph in a way
    // the full-rewrite save path is expected to garbage-collect/dedup (see
    // SAVE-08, test_font_resource_persistence.cpp). markNeedsFullGc both blocks
    // the incremental-append shortcut (Tier 1) AND keeps do_garbage=3 (Tier 2)
    // so the SAVE-08 dedup contract is exercised (pipeline b2e47d91).
    document->markNeedsFullGc();

    try {
        return context_->executeReturn<EmbeddedFontInfo>([&](fz_context* ctx) -> EmbeddedFontInfo {
            // Get handles via friend access
            fz_page* fzPage = page->fzPage();
            fz_document* fzDoc = document->fzDocument();
            pdf_page* pdfPage = pdf_page_from_fz_page(ctx, fzPage);
            pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, fzDoc);

            if (!pdfPage || !pdfDoc) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "Invalid page or document");
            }

            // Load font from file
            fz_font* font = fz_new_font_from_file(ctx, nullptr, fontFilePath.c_str(), 0, 0);
            if (!font) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "Failed to load font from file");
            }

            // Create PDF font object (CID for full Unicode)
            pdf_obj* fontRef = pdf_add_cid_font(ctx, pdfDoc, font);
            if (!fontRef) {
                fz_drop_font(ctx, font);
                fz_throw(ctx, FZ_ERROR_GENERIC, "Failed to create CID font");
            }

            // Get page /Resources/Font dict (create if absent)
            pdf_obj* resources = pdf_page_resources(ctx, pdfPage);
            if (!resources) {
                fz_drop_font(ctx, font);
                fz_throw(ctx, FZ_ERROR_GENERIC, "Failed to get page resources");
            }

            pdf_obj* fontDict = pdf_dict_get(ctx, resources, PDF_NAME(Font));
            if (!fontDict) {
                fontDict = pdf_new_dict(ctx, pdfDoc, 4);
                pdf_dict_put(ctx, resources, PDF_NAME(Font), fontDict);
            }

            // Pick unique name: scan fontDict for "Fsub1", "Fsub2", etc.
            std::set<std::string> existingNames;
            int dictLen = pdf_dict_len(ctx, fontDict);
            for (int i = 0; i < dictLen; ++i) {
                pdf_obj* key = pdf_dict_get_key(ctx, fontDict, i);
                if (key) {
                    const char* keyStr = pdf_to_name(ctx, key);
                    if (keyStr) {
                        existingNames.insert(keyStr);
                    }
                }
            }

            // Find next available "FsubN" name
            std::string resourceName;
            int nameIndex = 1;
            while (true) {
                resourceName = "Fsub" + std::to_string(nameIndex);
                if (existingNames.find(resourceName) == existingNames.end()) {
                    break;
                }
                ++nameIndex;
            }

            // Register: pdf_dict_puts(ctx, fontDict, name.c_str(), fontRef)
            pdf_dict_puts(ctx, fontDict, resourceName.c_str(), fontRef);

            // Return EmbeddedFontInfo
            EmbeddedFontInfo info;
            info.resourceName = resourceName;
            info.pdfFontObj = fontRef;
            info.fzFont = font;

            return info;
        });
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace pdfe::fonts
