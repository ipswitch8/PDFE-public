#pragma once

#include "tre/EnrichedAtlas.h"
#include "tre/LineFontVerifier.h"
#include "tre/TreTypes.h"
#include <QImage>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pdfe::tre {

class OcrPipeline;

/// Orchestrates construction of a font-verified, enriched glyph atlas
/// from a scanned page, with optional cross-page harvesting.
///
/// Build strategy:
///   1. Pick the reference line (longest line on the target page)
///   2. Harvest all chars from the reference line
///   3. For each other line on the page, verify font compatibility via
///      LineFontVerifier, then harvest compatible lines
///   4. If atlas coverage is below threshold, attempt cross-page harvesting
///   5. Build GlyphSynthesizer profile and pre-synthesize common characters
///   6. Return an EnrichedAtlas ready for both overlay and save paths
class AtlasBuilder {
public:
    struct Options {
        float minCoverageForCrossPage = 0.70f;  ///< Below this coverage, try other pages
        int maxCrossPages = 2;                   ///< Max adjacent pages to harvest from
        LineFontVerifier::Thresholds verifyThresholds;  ///< Font verification thresholds
        std::u32string presynthText;             ///< Text to pre-synthesize for
        std::string fontFilePath;                ///< Path to matched font for template scoring
        int fontFaceIndex = 0;                   ///< Face index in the font file
        int clickedLineIdx = -1;                 ///< HOCR line index for reference (-1 = auto longest)
    };

    struct BuildResult {
        std::shared_ptr<EnrichedAtlas> enrichedAtlas;
        int samePageDonorLines = 0;        ///< Lines that passed font verification
        int samePageRejectedLines = 0;     ///< Lines that failed font verification
        int crossPageDonorLines = 0;       ///< Cross-page lines harvested
        int crossPagesScanned = 0;         ///< Pages scanned for cross-page harvesting
        int atlasGlyphCount = 0;           ///< Glyphs from scan
        int synthesizedGlyphCount = 0;     ///< Glyphs from synthesis
    };

    /// Build a font-verified enriched atlas for a page.
    ///
    /// @param pageImage     Full scanned page image.
    /// @param allLines      All HOCR lines on the page (from char-level OCR).
    /// @param textColor     Detected text color.
    /// @param bgColor       Detected background color.
    /// @param ocrPipeline   OCR pipeline for cross-page harvesting (null to skip).
    /// @param renderPage    Callback to render a page by index (null to skip).
    /// @param pageIndex     Current page index (for cross-page adjacency).
    /// @param totalPages    Total pages in document.
    /// @param options       Build options.
    /// @return BuildResult with the enriched atlas and diagnostics.
    static BuildResult build(
        const QImage& pageImage,
        const std::vector<HocrLine>& allLines,
        const RgbColor& textColor,
        const RgbColor& bgColor,
        OcrPipeline* ocrPipeline,
        std::function<QImage(int pageIndex)> renderPage,
        int pageIndex,
        int totalPages,
        const Options& options = defaultOptions());

private:
    /// Select the reference line (longest by character count).
    static int selectReferenceLine(const std::vector<HocrLine>& lines);

    /// Returns Options with default values. clang-cl rejects using
    /// nested-type default member initializers in default arguments
    /// declared inside the same enclosing class; calling a function
    /// from the default arg lets us defer that evaluation.
    static Options defaultOptions();
};

} // namespace pdfe::tre
