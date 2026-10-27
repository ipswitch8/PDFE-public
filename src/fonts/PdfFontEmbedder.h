#pragma once

#include <memory>
#include <optional>
#include <string>

namespace pdfe::core {
class MuPdfContext;
class Page;
class Document;
}

namespace pdfe::fonts {

struct EmbeddedFontInfo {
    std::string resourceName;    // e.g., "Fsub1" -- the /Font dict key
    void* pdfFontObj = nullptr;  // Opaque pdf_obj* (indirect ref to font in PDF)
    void* fzFont = nullptr;      // Opaque fz_font* (for rendering reference)
};

class PdfFontEmbedder {
public:
    explicit PdfFontEmbedder(pdfe::core::MuPdfContext* context);
    ~PdfFontEmbedder();

    // Embed a system font file into the PDF document and register
    // it in the page's /Font resource dictionary.
    std::optional<EmbeddedFontInfo> embedFont(
        pdfe::core::Page* page,
        pdfe::core::Document* document,
        const std::string& fontFilePath
    );

    PdfFontEmbedder(const PdfFontEmbedder&) = delete;
    PdfFontEmbedder& operator=(const PdfFontEmbedder&) = delete;

private:
    pdfe::core::MuPdfContext* context_;
};

} // namespace pdfe::fonts
