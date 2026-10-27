#pragma once

#include "tre/TreTypes.h"
#include <QImage>
#include <QRectF>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// fz_context is a named struct -> safe to forward-declare at global scope
// (see CLAUDE.md MuPDF gotchas). Lets the encodeGlyphIndicesHex() helper take a
// typed context without pulling <mupdf/fitz.h> into this header.
extern "C" {
struct fz_context;
}

namespace pdfe::core {
class MuPdfContext;
class Document;
class Page;
}

namespace pdfe::fonts {
class PdfFontEmbedder;
}

namespace pdfe::tre {

struct PageImageInfo {
    std::string xobjectName;  // e.g., "Im0"
    int width = 0;
    int height = 0;
    bool isJpeg = false;      // true if DCTDecode filter

    /// Where this image is drawn on the page, in fitz points (top-left origin,
    /// Y-down) — the same space as a selection rect.
    ///
    /// GitHub #262. Without this the writer treats the image it picked as if it
    /// WERE the page, so an edit is mapped into it with a page-derived scale.
    /// That is only true for a scan whose single image covers the whole page.
    /// An empty rect means "placement unknown"; callers must then fall back to
    /// the page-spanning assumption rather than compute a wrong offset.
    QRectF placementFitz;
};

class PdfContentWriter {
public:
    PdfContentWriter(pdfe::core::MuPdfContext* context,
                     pdfe::fonts::PdfFontEmbedder* fontEmbedder);
    ~PdfContentWriter();

    PdfContentWriter(const PdfContentWriter&) = delete;
    PdfContentWriter& operator=(const PdfContentWriter&) = delete;
    PdfContentWriter(PdfContentWriter&&) noexcept;
    PdfContentWriter& operator=(PdfContentWriter&&) noexcept;

    /// Find the largest image XObject on the page.
    /// Typically finds the scanned page background image.
    /// @return ImageInfo with xobjectName, dimensions, and filter type, or nullopt if none found
    std::optional<PageImageInfo> findPageImageXObject(
        pdfe::core::Page* page, pdfe::core::Document* document);

    /// Find the image XObject actually UNDER a region — not merely the biggest
    /// one on the page.
    ///
    /// GitHub #262. `findPageImageXObject()` ranks by Width*Height alone, so on
    /// a page carrying a small image drawn OVER a large one it always returns
    /// the large one. An edit over the small image is then written into the
    /// large image and painted over by the small one: the write succeeds and
    /// nothing visible changes.
    ///
    /// Selection matches `Page::sourceImageDpiAt()` — among images overlapping
    /// the region, prefer the one covering the largest fraction of it; on ties
    /// prefer the tightest bbox, which is also the one drawn last, i.e. on top.
    /// Keeping the two in step matters: they were already disagreeing about
    /// which image a region belongs to, one used for DPI and the other for
    /// write-back.
    ///
    /// Returns nullopt when no image overlaps the region, and ALSO when the
    /// chosen image cannot be tied to exactly one XObject entry (two resource
    /// images sharing pixel dimensions are indistinguishable here). Guessing
    /// would reintroduce the silent mis-write this exists to remove; the caller
    /// falls back to `findPageImageXObject()` and the post-write verification
    /// catches it if that fallback is wrong.
    ///
    /// @param regionFitz Selection rect in fitz points (top-left origin, Y-down)
    std::optional<PageImageInfo> findImageXObjectForRegion(
        pdfe::core::Page* page, pdfe::core::Document* document,
        const QRectF& regionFitz);

    /// The image drawn LAST among those covering a region — i.e. the one the
    /// user actually sees there.
    ///
    /// GitHub #262. This is the deterministic form of "will a write into this
    /// image be visible?". A write into any image that is not this one is
    /// painted over, so it changes the file and changes nothing the user sees.
    ///
    /// It deliberately answers structurally rather than by comparing rendered
    /// pixels before and after a write. That was tried first and does not work:
    /// re-compositing is not byte-idempotent (Lanczos resampling and re-encoding
    /// perturb the image on every pass) but it IS visually idempotent, so a
    /// later pass over an already-edited region legitimately produces changed
    /// image bytes and an identical render — indistinguishable, by that measure,
    /// from an occluded write. Measured on mixed_dpi_page.pdf: one user edit
    /// drives five recomposite passes, and passes 2-5 all have that shape.
    ///
    /// `xobjectName` is not populated: the caller needs identity, which
    /// dimensions plus placement supply, and resolving a name here would
    /// reintroduce the dimension ambiguity this is used to reason about.
    ///
    /// @param regionFitz Selection rect in fitz points (top-left origin, Y-down)
    /// @return The topmost covering image's dimensions and placement, or nullopt
    ///         when no image covers the region's centre.
    std::optional<PageImageInfo> topmostImageOverRegion(
        pdfe::core::Page* page, pdfe::core::Document* document,
        const QRectF& regionFitz);

    /// Replace the page's background image with a new image (typically cleaned background).
    /// @param newImage The new image to replace the existing one (will be converted to RGB if needed)
    /// @return true on success, false on failure
    bool replacePageImage(
        pdfe::core::Page* page, pdfe::core::Document* document,
        const QImage& newImage);

    /// Replace a SPECIFIC image XObject, rather than whichever one is biggest.
    ///
    /// GitHub #262. The two-argument form above resolves its own target and so
    /// cannot express "write into the image under the edit". `target` is
    /// normally the result of `findImageXObjectForRegion()`.
    /// @return true on success, false on failure
    bool replacePageImage(
        pdfe::core::Page* page, pdfe::core::Document* document,
        const QImage& newImage, const PageImageInfo& target);

    /// Write a BT/ET text block to the page's content stream.
    /// Embeds the font if needed, then generates PDF text operators.
    /// @param utf8Text The text to render
    /// @param fontMatch Font properties (path, size, color, etc.)
    /// @param pdfRect Bounding box in fitz-space (top-left origin, Y-down)
    /// @param lines Optional HOCR line data for multi-line layout
    /// @return The PDF operators string written (for debugging/verification)
    std::string writeTextBlock(
        pdfe::core::Page* page, pdfe::core::Document* document,
        const std::string& utf8Text, const FontMatch& fontMatch,
        const PdfRect& pdfRect, const std::vector<HocrLine>& lines = {});

    /// Map Unicode codepoints to their GLYPH INDICES in `fzFont` (an opaque
    /// fz_font*) and return the concatenated hex CID show-string payload
    /// (four hex digits per glyph, no angle brackets).
    ///
    /// The embedded reconstruction font is a pdf_add_cid_font (Type0 /
    /// Identity-H), so the bytes of a `<...> Tj` show string ARE glyph indices,
    /// not character codes. Emitting raw Unicode codepoints (as the old glyph
    /// fallback did) therefore renders whatever glyph happens to sit at that
    /// index -- the wrong glyph for almost every font. Each codepoint is
    /// resolved via the font cmap (fz_encode_character); a codepoint genuinely
    /// absent from the font maps to GID 0 (.notdef), never to the raw
    /// codepoint. Per-character encoding is guarded so one unresolvable glyph
    /// cannot abort the whole run.
    static std::string encodeGlyphIndicesHex(
        fz_context* ctx, void* fzFont, const std::vector<uint32_t>& codepoints);

    /// Append raw PDF operators to the page's content stream.
    /// If the page has no content stream, creates one.
    /// If the page has a single content stream, converts to an array and appends.
    /// If the page already has a content stream array, appends to it.
    /// @param operators Raw PDF content stream operators string
    /// @return true on success, false on failure
    bool appendContentStream(
        pdfe::core::Page* page, pdfe::core::Document* document,
        const std::string& operators);

    /// Get raw binary data from the page's largest image XObject.
    /// Useful for debugging or extracting the background image.
    /// @return Binary image data, or empty vector if not found
    std::vector<uint8_t> getImageXObjectData(
        pdfe::core::Page* page, pdfe::core::Document* document);

    /// Decode the page's largest image XObject to a full-resolution QImage.
    /// Returns the image at its native resolution (no scaling).
    /// @return Decoded RGB888 QImage, or null if not found or decode fails
    QImage getOriginalImage(
        pdfe::core::Page* page, pdfe::core::Document* document);

    /// Decode a SPECIFIC image XObject, rather than whichever one is biggest.
    /// GitHub #262 — the recomposite needs the pristine pixels of the image it
    /// is about to write back into, which on a multi-image page is not the
    /// largest one.
    /// @return Decoded RGB888 QImage, or null if not found or decode fails
    QImage getOriginalImage(
        pdfe::core::Page* page, pdfe::core::Document* document,
        const PageImageInfo& target);

    /// Get raw binary data from the page's content stream(s).
    /// Concatenates multiple streams if the page has a content stream array.
    /// @return Binary content stream data, or empty vector if none
    std::vector<uint8_t> getContentStreamData(
        pdfe::core::Page* page, pdfe::core::Document* document);

    // -----------------------------------------------------------------
    // Undo support: save/restore PDF object references (opaque void*)
    // -----------------------------------------------------------------
    // Uses pdf_keep_obj()/pdf_drop_obj() pattern from Phase 4 PageCommands.
    // The caller stores the opaque void* and passes it back for restore/release.

    /// Save a reference to the page's largest image XObject for later undo.
    /// The returned pointer must be released via releaseImageXObjectRef()
    /// or restored via restoreImageXObjectRef().
    /// @return Opaque reference (pdf_obj*), or nullptr if no image found
    void* saveImageXObjectRef(
        pdfe::core::Page* page, pdfe::core::Document* document);

    /// Restore a previously saved image XObject reference into the page.
    /// @param savedRef Opaque reference from saveImageXObjectRef()
    /// @param xobjectName The XObject name (e.g., "Im0") to put it back under
    /// @return true on success
    bool restoreImageXObjectRef(
        pdfe::core::Page* page, const pdfe::core::Document* document,
        void* savedRef, const std::string& xobjectName);

    /// Release a saved image XObject reference without restoring.
    /// Call this in the command destructor if the ref was never restored.
    void releaseImageXObjectRef(void* ref);

    /// Save a reference to the page's content stream(s) for later undo.
    /// The returned pointer must be released via releaseContentsRef()
    /// or restored via restoreContentsRef().
    /// @return Opaque reference (pdf_obj*), or nullptr if no contents
    void* saveContentsRef(
        pdfe::core::Page* page, const pdfe::core::Document* document);

    /// Restore a previously saved content stream reference.
    /// @param savedRef Opaque reference from saveContentsRef() (nullptr removes Contents)
    /// @return true on success
    bool restoreContentsRef(
        pdfe::core::Page* page, const pdfe::core::Document* document,
        void* savedRef);

    /// Release a saved content stream reference without restoring.
    void releaseContentsRef(void* ref);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl_;
};

} // namespace pdfe::tre
