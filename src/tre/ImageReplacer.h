#pragma once

#include "tre/TreTypes.h"
#include "tre/BitmapCompositor.h"  // FontRun
#include "tre/EnrichedAtlas.h"
#include <QtGlobal>  // qint64
#include <QImage>
#include <QRect>
#include <memory>
#include <string>
#include <vector>

namespace pdfe::core {
class MuPdfContext;
class Document;
class Page;
} // namespace pdfe::core

namespace pdfe::tre {

// Forward declarations
class OcrPipeline;
class CoordMapper;
class BgColorDetector;
class FontMatcher;
class ImageMasker;
class BitmapCompositor;
class PdfContentWriter;

struct ReconstructionResult; // from RegionReconstructor.h

/// Orchestrates the image replacement pipeline (Approach C).
///
/// ImageReplacer coordinates 7 components to:
/// 1. OCR a page and extract a text region
/// 2. Render the page to an image
/// 3. Detect background color/gradient
/// 4. Match fonts from OCR properties
/// 5. Render edited text to a bitmap with the matched font
/// 6. Composite the text bitmap over the cleaned background
/// 7. Replace the page's background image with the composited result
///
/// This is the fallback approach (Approach C) when font matching quality
/// is too low for hybrid reconstruction (Approach B). It always proceeds
/// without quality gates, as it's designed to be the safest fallback.
class ImageReplacer {
public:
    /// Construct an ImageReplacer with references to all pipeline components.
    ///
    /// @param ocrPipeline       OCR engine for extracting text (non-owning).
    /// @param coordMapper       Coordinate mapper for pixel/PDF conversions (non-owning).
    /// @param bgDetector        Background color detector (non-owning).
    /// @param fontMatcher       Font matching engine (non-owning).
    /// @param imageMasker       Image masking engine (non-owning).
    /// @param bitmapCompositor  Text bitmap renderer (non-owning).
    /// @param contentWriter     PDF content writer (non-owning).
    ImageReplacer(
        OcrPipeline* ocrPipeline,
        CoordMapper* coordMapper,
        BgColorDetector* bgDetector,
        FontMatcher* fontMatcher,
        ImageMasker* imageMasker,
        BitmapCompositor* bitmapCompositor,
        PdfContentWriter* contentWriter);

    ~ImageReplacer();

    // Non-copyable, movable
    ImageReplacer(const ImageReplacer&) = delete;
    ImageReplacer& operator=(const ImageReplacer&) = delete;
    ImageReplacer(ImageReplacer&&) noexcept;
    ImageReplacer& operator=(ImageReplacer&&) noexcept;

    /// Execute the full Approach C pipeline: analyze region and composite bitmap.
    ///
    /// This performs 7 steps:
    /// 1. OCR the page and extract the region (snap-to-line boundaries)
    /// 2. Render the page to an image
    /// 3. Detect background color/gradient
    /// 4. Match fonts based on OCR properties
    /// 5. Render edited text to a transparent bitmap
    /// 6. Composite text bitmap over cleaned background
    /// 7. Replace the page's background image
    ///
    /// No quality gate is applied - Approach C always proceeds as the fallback.
    ///
    /// @param page          Page object to modify (non-owning).
    /// @param document      Document object (non-owning).
    /// @param pageIndex     Zero-based page index.
    /// @param selectionPdf  Region selection in PDF coordinates.
    /// @param editedText    Text to render (if empty, uses OCR text).
    /// @param regionId      Unique region identifier.
    /// @param snapToLines   If true (default), snap selection to HOCR line boundaries
    ///                      for full-region replacement. If false, use the exact
    ///                      selection rectangle for inline span edits (prevents
    ///                      adjacent text from being erased).
    /// @param fontRuns      Optional per-character-range font overrides for multi-font
    ///                      rendering. When non-empty, renderMultiFontText() is used
    ///                      instead of the default single-font render path.
    /// @param fontSizeOverridePt  If > 0, override FontMatcher's detected font size
    ///                            with this value. Used when the span has a real embedded
    ///                            font with a known-correct size from the PDF text matrix.
    /// @param useGlyphAtlas       If true and char-level HOCR data is available, extract
    ///                            character images from the scanned page and compose text
    ///                            from the "glyph atlas" instead of FreeType rendering.
    ///                            Falls back to FreeType if atlas coverage is too low.
    /// @param fontMatchOverride   If non-null, skip FontMatcher (step 4) and use this
    ///                            pre-computed FontMatch instead. Useful when the span
    ///                            has a real embedded font and we already know the
    ///                            correct system font from the known-pair table.
    /// @param enrichedAtlas       If non-null and useGlyphAtlas is true, use this
    ///                            pre-built EnrichedAtlas (from overlay) instead of
    ///                            building a fresh atlas. Provides WYSIWYG: the same
    ///                            atlas+synthesis used for overlay is used for save.
    /// @return Result with success flag, region data, and error message on failure.
    ReconstructionResult replace(
        pdfe::core::Page* page,
        pdfe::core::Document* document,
        int pageIndex,
        const PdfRect& selectionPdf,
        const std::string& editedText,
        RegionId regionId,
        bool snapToLines = true,
        const std::vector<FontRun>& fontRuns = {},
        float fontSizeOverridePt = 0.0f,
        bool useGlyphAtlas = false,
        const FontMatch* fontMatchOverride = nullptr,
        const std::shared_ptr<EnrichedAtlas>& enrichedAtlas = nullptr);

    /// Execute the Approach C pipeline with pre-computed font match and background.
    ///
    /// This skips steps 1-4 (OCR, rendering, background detection, font matching)
    /// and jumps directly to steps 5-7 (bitmap rendering, compositing, image replacement).
    /// Useful when analysis has already been performed by RegionReconstructor.
    ///
    /// @param page          Page object to modify (non-owning).
    /// @param document      Document object (non-owning).
    /// @param pageIndex     Zero-based page index.
    /// @param selectionPdf  Region selection in PDF coordinates.
    /// @param editedText    Text to render.
    /// @param fontMatch     Pre-computed font match from analysis.
    /// @param bgInfo        Pre-computed background info from analysis.
    /// @param regionId      Unique region identifier.
    /// @param fontRuns      Optional per-character-range font overrides.
    /// @param originalText  Optional original text for width expansion calculation.
    ///                      When provided and editedText is longer, the bitmap width
    ///                      is expanded proportionally to prevent truncation.
    /// @return Result with success flag, region data, and error message on failure.
    ReconstructionResult replaceWithFontMatch(
        pdfe::core::Page* page,
        pdfe::core::Document* document,
        int pageIndex,
        const PdfRect& selectionPdf,
        const std::string& editedText,
        const FontMatch& fontMatch,
        const BackgroundInfo& bgInfo,
        RegionId regionId,
        const std::vector<FontRun>& fontRuns = {},
        const std::string& originalText = {});

    /// Invalidate the cached original image for a specific page.
    /// Must be called when undo/redo changes the page's XObject so that
    /// the next edit re-reads the current (restored) image from the PDF.
    void invalidateOriginalCache(int pageIndex);

    /// Clear all cached original images (e.g., on page reindex or structure change).
    void clearOriginalCache();

    /// Populate the original-image cache for a page without running the
    /// full edit pipeline. Used by tests to exercise eviction logic and
    /// by any caller that wants to pre-warm the cache (e.g. a prefetch
    /// hint before the user starts editing a nearby page).
    /// Returns true if the page image was decoded (the cache may still
    /// refuse to store it if the image is larger than the total budget).
    bool primeOriginalCache(pdfe::core::Page* page,
                            pdfe::core::Document* document,
                            int pageIndex);

    /// C2: Memory-budgeted LRU cache for pre-edit original page images.
    /// Prevents repeated JPEG recompression on non-sequential editing
    /// patterns that defeated the old hardcoded 3-page cache.
    /// See docs/image-pdf-risks.md (C2).
    struct CacheStats {
        qint64 currentBytes = 0;  ///< Bytes held by the cache right now.
        qint64 maxBytes = 0;      ///< Eviction threshold.
        int entries = 0;          ///< Pages cached right now.
        qint64 hits = 0;          ///< Times a page was served from the cache.
        qint64 misses = 0;        ///< Times a page had to be re-decoded.
        qint64 evictions = 0;     ///< Total pages dropped for budget or invalidation.
    };

    /// Configure the cache memory budget in bytes. Default is 256 MB.
    /// Pass 0 to disable caching entirely. The new budget is enforced
    /// immediately: if currentBytes exceeds the new budget, older
    /// entries are evicted until it fits.
    void setCacheBudgetBytes(qint64 bytes);

    /// Current cache budget in bytes.
    [[nodiscard]] qint64 cacheBudgetBytes() const;

    /// Snapshot of cache counters for tests and instrumentation.
    [[nodiscard]] CacheStats cacheStats() const;

    /// Default memory budget when no setCacheBudgetBytes call has been made.
    static constexpr qint64 kDefaultCacheBudgetBytes =
        static_cast<qint64>(256) * 1024 * 1024;  // 256 MB

    /// Compute expanded pixel width when edited text is longer than original.
    /// Returns basePixelWidth unchanged if no expansion is needed.
    /// Character-count-based fallback — ignores kerning and ligatures.
    /// Includes a 20% safety margin for variable-width characters.
    /// Public for testing.
    static int computeExpandedWidth(
        int basePixelWidth,
        const std::string& editedText,
        const std::string& originalText);

    /// B2: Compute expanded pixel width by MEASURING actual rendered
    /// advance widths via the BitmapCompositor's shaper. This respects
    /// kerning pairs, Latin ligatures, Arabic ligated final forms, and
    /// CJK fixed-width semantics where the char-count ratio fails.
    ///
    /// When any argument is empty or measurement fails, falls back to
    /// the static character-count version. The 20% safety margin is
    /// still applied to the measured width to cover metric-vs-bitmap
    /// rounding.
    ///
    /// See docs/image-pdf-risks.md (B2).
    static int computeExpandedWidthMeasured(
        int basePixelWidth,
        const std::string& editedText,
        const std::string& originalText,
        BitmapCompositor* compositor,
        const FontMatch& fontMatch,
        int dpi);

    /// Determine whether OCR-based styled runs should be skipped.
    /// Returns true when editedText differs from originalText (OCR char indices invalid).
    /// Public for testing.
    static bool shouldSkipStyledRuns(
        const std::string& editedText,
        const std::string& originalText);

    /// The page area that will actually be background-filled and drawn into.
    struct FillRegion {
        PixelRect bbox;            ///< Page-pixel-space rect to clear and draw into.
        QRect bitmapCrop;          ///< Sub-rect of the text bitmap to draw.
        bool shrunkToInk = false;  ///< True when fitted to the rendered glyphs.
    };

    /// Decide the fill/composite region for a text edit.
    ///
    /// The caller's `candidate` rect is the region the old code always used:
    /// the user's selection (unioned, on the inline path, with the OCR line
    /// bbox). Compositing over the whole of it is correct for a REPLACEMENT —
    /// the fill has to cover the original scanned text or it shows through as
    /// ghost text underneath the new rendering, which is exactly why that
    /// union exists.
    ///
    /// It is wrong for text ADDED over blank background. There is nothing to
    /// cover there, so filling the whole selection rewrites the page under
    /// every pixel the user happened to drag across — a single character in a
    /// large box repaints the entire box. Reported from the field on a scanned
    /// manual; see docs/audit/field-report-2026-08-28-scanned-manual.md (3).
    ///
    /// So the region is fitted to the ink extent of the rendered glyphs ONLY
    /// when there was no original text under the selection. It is never
    /// shrunk unconditionally.
    ///
    /// Every failure path returns the candidate unchanged (`shrunkToInk ==
    /// false`), i.e. the previous behaviour: a null bitmap, a bitmap with no
    /// visible ink at all, or a degenerate result all fall back to filling the
    /// whole candidate rather than to filling nothing.
    ///
    /// @param candidate    Region the caller would otherwise fill, page pixels.
    /// @param textBitmap   Rendered text, transparent where there is no glyph.
    ///                     Its origin is `candidate`'s top-left.
    /// @param originalText Text that was under the selection before the edit.
    ///                     Blank (empty or all-whitespace) means "adding over
    ///                     blank background" — the only case that shrinks.
    /// @param padPx        Pixels of margin kept around the ink so the
    ///                     antialiased glyph edges sit inside the filled area.
    /// Public for testing.
    static FillRegion computeFillRegion(
        const PixelRect& candidate,
        const QImage& textBitmap,
        const std::string& originalText,
        int padPx = 1);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl_;
};

} // namespace pdfe::tre
