#pragma once

#include "tre/TreTypes.h"
#include <memory>
#include <string>

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
class QualityAssessor;
class ImageMasker;
class PdfContentWriter;

/// Result of a region reconstruction operation.
struct ReconstructionResult {
    bool success = false;
    ReconstructedRegion region;     // from TreTypes.h
    QualityVerdict qualityVerdict;  // from TreTypes.h
    std::string fontResourceName;
    std::string errorMessage;
};

/// Orchestrates the hybrid reconstruction pipeline (Approach B).
///
/// RegionReconstructor coordinates 7 components to:
/// 1. OCR a page and extract a text region
/// 2. Render the page to an image
/// 3. Detect background color/gradient
/// 4. Match fonts from OCR properties
/// 5. Assess reconstruction quality
/// 6. Mask the region in the page image (replace background)
/// 7. Write reconstructed text to the PDF
///
/// The `reconstruct()` method performs the full pipeline and modifies the PDF.
/// The `analyze()` method runs steps 1-5 without modifying the PDF, useful
/// for previewing quality before committing changes.
class RegionReconstructor {
public:
    /// Construct a RegionReconstructor with references to all pipeline components.
    ///
    /// @param ocrPipeline     OCR engine for extracting text (non-owning).
    /// @param coordMapper     Coordinate mapper for pixel/PDF conversions (non-owning).
    /// @param bgDetector      Background color detector (non-owning).
    /// @param fontMatcher     Font matching engine (non-owning).
    /// @param qualityAssessor Quality evaluation engine (non-owning).
    /// @param imageMasker     Image masking engine (non-owning).
    /// @param contentWriter   PDF content writer (non-owning).
    RegionReconstructor(
        OcrPipeline* ocrPipeline,
        CoordMapper* coordMapper,
        BgColorDetector* bgDetector,
        FontMatcher* fontMatcher,
        QualityAssessor* qualityAssessor,
        ImageMasker* imageMasker,
        PdfContentWriter* contentWriter);

    ~RegionReconstructor();

    // Non-copyable, movable
    RegionReconstructor(const RegionReconstructor&) = delete;
    RegionReconstructor& operator=(const RegionReconstructor&) = delete;
    RegionReconstructor(RegionReconstructor&&) noexcept;
    RegionReconstructor& operator=(RegionReconstructor&&) noexcept;

    /// Execute the full Approach B pipeline: analyze region and write to PDF.
    ///
    /// This performs 7 steps:
    /// 1. OCR the page and extract the region (snap-to-line boundaries)
    /// 2. Render the page to an image
    /// 3. Detect background color/gradient
    /// 4. Match fonts based on OCR properties
    /// 5. Assess quality (returns error if quality too low)
    /// 6. Mask the region in the page image
    /// 7. Write the reconstructed text block to the PDF
    ///
    /// @param page          Page object to modify (non-owning).
    /// @param document      Document object (non-owning).
    /// @param pageIndex     Zero-based page index.
    /// @param selectionPdf  Region selection in PDF coordinates.
    /// @param editedText    Text to render (if empty, uses OCR text).
    /// @param regionId      Unique region identifier.
    /// @return Result with success flag, region data, and error message on failure.
    ReconstructionResult reconstruct(
        pdfe::core::Page* page,
        pdfe::core::Document* document,
        int pageIndex,
        const PdfRect& selectionPdf,
        const std::string& editedText,
        RegionId regionId);

    /// Analyze a region without modifying the PDF (steps 1-5 only).
    ///
    /// This is useful for previewing quality assessment before committing
    /// to the full reconstruction pipeline.
    ///
    /// @param pageIndex     Zero-based page index.
    /// @param selectionPdf  Region selection in PDF coordinates.
    /// @return Result with quality verdict and region data (no PDF modification).
    ReconstructionResult analyze(int pageIndex, const PdfRect& selectionPdf);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl_;
};

} // namespace pdfe::tre
