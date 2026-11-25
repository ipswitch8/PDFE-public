#pragma once

#include <QImage>
#include <QPointF>
#include <QRectF>
#include <memory>
#include <string>
#include <vector>

// Forward declarations for MuPDF C types (must be at global scope, with C linkage)
extern "C" {
struct fz_page;
struct pdf_page;
}

namespace pdfe::annotations {
class AnnotationManager;
class TextSelector;
}

namespace pdfe::forms {
class FormManager;
class JavaScriptEngine;
}

namespace pdfe::textedit {
class ContentStreamEditor;
}

namespace pdfe::fonts {
class EmbeddedFontAnalyzer;
class PdfFontEmbedder;
class VectorSubsetDetector;
}

namespace pdfe::tre {
class PdfContentWriter;
class SynthesizedType3FontBundler;
class AutoMissingGlyphResolver;
}

namespace pdfe::redaction {
class RedactionManager;
}

namespace pdfe::signatures {
class SignatureManager;
class LtvDssWriter;
}

namespace pdfe::measure {
// GitHub #21 / PDF-57 — the read side of ISO 32000-1 §12.9 needs the page
// DICTIONARY (`/VP`), which is reachable only through the MuPDF page handle.
class MeasureDictReader;
}

namespace pdfe::image {
class ImageManager;
class ImageContentEditor;
class ExifStripper;
}

namespace pdfe::core {

class MuPdfContext;
class Document;
struct TextBlock;

class Page {
public:
    ~Page();

    // Rendering
    /// The whole page as one RGBA raster at @p scale (72 dpi = 1.0); @p
    /// region is ignored. MuPDF draws straight into the returned image, so a
    /// render holds ONE raster at its peak (ui-tweaks-3 phase-2): bytes are
    /// exactly fz_new_pixmap_from_page's. A raster too large to allocate gives
    /// a null image; a MuPDF error mid-draw throws MuPdfException, as before.
    QImage render(const QRectF& region, float scale = 1.0f);

    /// TEST SEAM. The next render() (one-shot) raises a MuPDF error after its
    /// draw device is created, to exercise the error path's cleanup.
    static void failNextRenderForTest();

    // Text extraction
    std::vector<TextBlock> extractText();
    std::string extractPlainText();

    /// Returns true if the page contains native text (not image-only).
    /// Uses fz_stext_page traversal with short-circuit on first text block.
    bool hasTextContent() const;

    /// Returns true if the page is dominated by images with an OCR text layer.
    /// Detects scanned/OCR'd pages where image blocks cover >50% of page area
    /// AND text content exists (the invisible OCR text behind the image).
    bool isImageDominated();

    // Page properties
    QRectF boundingBox() const;

    /// The page's MediaBox (inherited if need be) in the SAME fitz space as
    /// boundingBox(): user space mapped by the page ctm that boundingBox()
    /// uses (pdf_page_transform: the CropBox frame, with /Rotate and
    /// /UserUnit applied). So boundingBox() lies inside it, and the two are
    /// equal when the page has no CropBox or its CropBox is its MediaBox.
    /// This is the page box the image tool clamps to and shades against, and
    /// the box the save-time image crop/clip cuts to (ui-tweaks-2 phase-1);
    /// the canvas still renders only the CropBox. Falls back to boundingBox()
    /// for a non-PDF page or an empty/degenerate MediaBox (MuPDF then
    /// substitutes its own default box, which is what boundingBox() reports).
    QRectF mediaBox() const;

    int rotation() const;
    int index() const;

    /// Infer the native DPI of the page's dominant image XObject.
    ///
    /// Walks the page's Resources/XObject dict, finds the largest image
    /// (by pixel area), and returns max(pxW/pageWidthIn, pxH/pageHeightIn).
    /// Returns 0.0 when no image XObject is present (vector-only pages).
    ///
    /// This is the source-of-truth for the per-page working DPI used by
    /// the TRE image-edit pipeline. See docs/image-pdf-risks.md (A3).
    double sourceImageDpi() const;

    /// Infer the native DPI of the image actually under a given region
    /// (defect #2). Walks the page's image blocks with their placement
    /// rectangles, finds the image overlapping @p regionFitz the most, and
    /// returns that image's native DPI (pixel dimensions / displayed size in
    /// inches). This is per-region, not per-page: a 150-DPI thumbnail and a
    /// 300-DPI photo on the same page report their own resolutions rather than
    /// the page maximum.
    ///
    /// @param regionFitz Region of interest in fitz-space PDF points (Y-down,
    ///        the same space as boundingBox()).
    /// @return The overlapping image's native DPI, or — when no image overlaps
    ///         the region — falls back to sourceImageDpi() (page maximum), and
    ///         0.0 when the page has no image XObject at all.
    double sourceImageDpiAt(const QRectF& regionFitz) const;

    /// True when an image block covers the given point (defect #4 per-region
    /// tool routing). Lets the editor route a click on a scanned image block to
    /// the region/TRE tool while a click on live vector text routes to the
    /// per-span text tool — on the SAME mixed-content page.
    ///
    /// @param fitzPoint Point in fitz-space PDF coordinates (Y-down, the same
    ///        space as boundingBox()).
    bool hasImageContentAt(const QPointF& fitzPoint) const;

    // Non-copyable
    Page(const Page&) = delete;
    Page& operator=(const Page&) = delete;

    // Movable
    Page(Page&&) noexcept;
    Page& operator=(Page&&) noexcept;

private:
    friend class Document;
    friend struct PageRenderTestAccess;  // tests: the fz_page for an independent reference render
    friend class TextExtractor;
    friend class SearchEngine;
    friend class pdfe::annotations::AnnotationManager;
    friend class pdfe::annotations::TextSelector;
    friend class pdfe::forms::FormManager;
    friend class pdfe::forms::JavaScriptEngine;
    friend class pdfe::textedit::ContentStreamEditor;
    friend class pdfe::fonts::EmbeddedFontAnalyzer;
    friend class pdfe::fonts::PdfFontEmbedder;
    friend class pdfe::fonts::VectorSubsetDetector;
    friend class pdfe::tre::PdfContentWriter;
    friend class pdfe::tre::SynthesizedType3FontBundler;
    friend class pdfe::tre::AutoMissingGlyphResolver;
    friend class pdfe::redaction::RedactionManager;
    friend class pdfe::signatures::SignatureManager;
    friend class pdfe::signatures::LtvDssWriter;
    friend class pdfe::measure::MeasureDictReader;
    friend class pdfe::image::ImageManager;
    friend class pdfe::image::ImageContentEditor;
    friend class pdfe::image::ExifStripper;

    Page(
        std::shared_ptr<MuPdfContext> context,
        ::fz_page* fzPage,
        ::pdf_page* pdfPage,
        int index,
        Document* ownerDoc = nullptr
    );

    /// Access the underlying MuPDF page handle (for internal use by friends).
    ::fz_page* fzPage() const;

    /// The Document that produced this page, or nullptr if unknown. Used by
    /// friend managers whose mutating methods receive only a Page (e.g.
    /// RedactionManager::applyPageRedactions, FormManager::flattenForm) to
    /// flag the owning document as requiring a full-rewrite save — see
    /// Document::markRequiresFullRewrite (pipeline b2e47d91, Tier 1).
    Document* ownerDocument() const;

    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pdfe::core
