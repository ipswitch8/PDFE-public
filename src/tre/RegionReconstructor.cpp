#include "tre/RegionReconstructor.h"
#include "tre/OcrPipeline.h"
#include "tre/CoordMapper.h"
#include "tre/BgColorDetector.h"
#include "tre/FontMatcher.h"
#include "tre/QualityAssessor.h"
#include "tre/ImageMasker.h"
#include "tre/PdfContentWriter.h"
#include "tre/HocrParser.h"
#include "core/Page.h"
#include "core/Document.h"
#include "core/Error.h"

#include <sstream>

namespace pdfe::tre {

// -----------------------------------------------------------------------------
// PIMPL Implementation
// -----------------------------------------------------------------------------

class RegionReconstructor::Impl {
public:
    Impl(OcrPipeline* ocrPipeline,
         CoordMapper* coordMapper,
         BgColorDetector* bgDetector,
         FontMatcher* fontMatcher,
         QualityAssessor* qualityAssessor,
         ImageMasker* imageMasker,
         PdfContentWriter* contentWriter)
        : ocrPipeline_(ocrPipeline)
        , coordMapper_(coordMapper)
        , bgDetector_(bgDetector)
        , fontMatcher_(fontMatcher)
        , qualityAssessor_(qualityAssessor)
        , imageMasker_(imageMasker)
        , contentWriter_(contentWriter)
    {
    }

    OcrPipeline* ocrPipeline_ = nullptr;
    CoordMapper* coordMapper_ = nullptr;
    BgColorDetector* bgDetector_ = nullptr;
    FontMatcher* fontMatcher_ = nullptr;
    QualityAssessor* qualityAssessor_ = nullptr;
    ImageMasker* imageMasker_ = nullptr;
    PdfContentWriter* contentWriter_ = nullptr;
};

// -----------------------------------------------------------------------------
// RegionReconstructor Public API
// -----------------------------------------------------------------------------

RegionReconstructor::RegionReconstructor(
    OcrPipeline* ocrPipeline,
    CoordMapper* coordMapper,
    BgColorDetector* bgDetector,
    FontMatcher* fontMatcher,
    QualityAssessor* qualityAssessor,
    ImageMasker* imageMasker,
    PdfContentWriter* contentWriter)
    : pImpl_(std::make_unique<Impl>(
          ocrPipeline, coordMapper, bgDetector, fontMatcher,
          qualityAssessor, imageMasker, contentWriter))
{
}

RegionReconstructor::~RegionReconstructor() = default;

RegionReconstructor::RegionReconstructor(RegionReconstructor&&) noexcept = default;
RegionReconstructor& RegionReconstructor::operator=(RegionReconstructor&&) noexcept = default;

// -----------------------------------------------------------------------------
// Main Pipeline: reconstruct()
// -----------------------------------------------------------------------------

ReconstructionResult RegionReconstructor::reconstruct(
    pdfe::core::Page* page,
    pdfe::core::Document* document,
    int pageIndex,
    const PdfRect& selectionPdf,
    const std::string& editedText,
    RegionId regionId)
{
    ReconstructionResult result;
    result.region.id = regionId;
    result.region.approach = Approach::B_HybridReconstruction;

    // Validate input parameters
    if (!page) {
        result.success = false;
        result.errorMessage = "Null page pointer";
        return result;
    }

    if (!document) {
        result.success = false;
        result.errorMessage = "Null document pointer";
        return result;
    }

    // Check page index range
    const int pageCount = document->pageCount();
    if (pageIndex < 0 || pageIndex >= pageCount) {
        result.success = false;
        std::ostringstream oss;
        oss << "Page index " << pageIndex << " out of range [0, " << pageCount << ")";
        result.errorMessage = oss.str();
        return result;
    }

    // Check selection area
    if (selectionPdf.area() <= 0.0) {
        result.success = false;
        result.errorMessage = "Selection rectangle has zero or negative area";
        return result;
    }

    try {
        // Step 1: OCR the page and extract the region
        const std::string hocrXml = pImpl_->ocrPipeline_->ocrPage(pageIndex);
        const HocrDocument hocrDoc = HocrParser::parse(hocrXml);

        // Convert PDF selection to pixel coordinates
        const AffineTransform& transform = pImpl_->coordMapper_->getTransform(pageIndex);
        const PixelRect pixelSelection = pImpl_->coordMapper_->pdfRectToPixel(selectionPdf, transform);

        // Extract region with snap-to-line boundaries
        RegionTextData regionData = HocrParser::extractRegion(hocrDoc, pixelSelection);
        regionData.pdfBbox = pImpl_->coordMapper_->pixelRectToPdf(regionData.pixelBbox, transform);

        // Store region data
        result.region.pdfBbox = regionData.pdfBbox;
        result.region.pixelBbox = regionData.pixelBbox;
        result.region.originalText = regionData.plainText;

        // Step 2: Render the page to an image
        const QImage pageImage = pImpl_->ocrPipeline_->renderPage(pageIndex);

        // Step 3: Detect background color/gradient
        const BackgroundInfo bgInfo = pImpl_->bgDetector_->detect(
            pageImage, regionData.pixelBbox, regionData.lines);
        result.region.bgInfo = bgInfo;

        // Step 4: Match fonts from OCR properties
        const int ocrDpi = pImpl_->ocrPipeline_->config().ocrDpi;
        const FontMatch fontMatch = pImpl_->fontMatcher_->matchRegion(
            regionData, pageImage, ocrDpi);
        result.region.fontMatch = fontMatch;

        // Step 5: Assess quality
        const QualityVerdict verdict = pImpl_->qualityAssessor_->evaluate(
            regionData, bgInfo, fontMatch.confidence);
        result.qualityVerdict = verdict;

        // Check if quality is sufficient for Approach B
        if (verdict.recommendedApproach != Approach::B_HybridReconstruction) {
            result.success = false;
            result.errorMessage = "Quality too low for Approach B (hybrid reconstruction)";
            return result;
        }

        // Step 6: Mask the region in the page image
        QImage maskedImage;
        if (bgInfo.hasGradient) {
            maskedImage = pImpl_->imageMasker_->maskRegionGradient(
                pageImage, regionData.pixelBbox, bgInfo.gradient);
        } else {
            maskedImage = pImpl_->imageMasker_->maskRegion(
                pageImage, regionData.pixelBbox, bgInfo.dominantColor);
        }

        // Replace the page background image with the masked version
        const bool replaceSuccess = pImpl_->contentWriter_->replacePageImage(
            page, document, maskedImage);
        if (!replaceSuccess) {
            result.success = false;
            result.errorMessage = "Failed to replace page background image";
            return result;
        }

        // Step 7: Write the reconstructed text block to the PDF
        const std::string textToWrite = editedText.empty() ? regionData.plainText : editedText;
        result.region.editedText = textToWrite;

        const std::string pdfOperators = pImpl_->contentWriter_->writeTextBlock(
            page, document, textToWrite, fontMatch, regionData.pdfBbox, regionData.lines);

        // Extract font resource name from the PDF operators (e.g., "/F1")
        // The operators typically start with "/F1 12 Tf" or similar
        const size_t slashPos = pdfOperators.find('/');
        const size_t spacePos = pdfOperators.find(' ', slashPos);
        if (slashPos != std::string::npos && spacePos != std::string::npos) {
            result.fontResourceName = pdfOperators.substr(slashPos, spacePos - slashPos);
        }

        result.success = true;
        return result;

    } catch (const pdfe::core::PdfeException& ex) {
        result.success = false;
        result.errorMessage = std::string("PDFE exception: ") + ex.what();
        return result;
    } catch (const std::exception& ex) {
        result.success = false;
        result.errorMessage = std::string("Exception: ") + ex.what();
        return result;
    } catch (...) {
        result.success = false;
        result.errorMessage = "Unknown exception during reconstruction";
        return result;
    }
}

// -----------------------------------------------------------------------------
// Analysis Only: analyze()
// -----------------------------------------------------------------------------

ReconstructionResult RegionReconstructor::analyze(
    int pageIndex,
    const PdfRect& selectionPdf)
{
    ReconstructionResult result;
    result.region.approach = Approach::B_HybridReconstruction;

    // Check selection area
    if (selectionPdf.area() <= 0.0) {
        result.success = false;
        result.errorMessage = "Selection rectangle has zero or negative area";
        return result;
    }

    try {
        // Step 1: OCR the page and extract the region
        const std::string hocrXml = pImpl_->ocrPipeline_->ocrPage(pageIndex);
        const HocrDocument hocrDoc = HocrParser::parse(hocrXml);

        // Convert PDF selection to pixel coordinates
        const AffineTransform& transform = pImpl_->coordMapper_->getTransform(pageIndex);
        const PixelRect pixelSelection = pImpl_->coordMapper_->pdfRectToPixel(selectionPdf, transform);

        // Extract region with snap-to-line boundaries
        RegionTextData regionData = HocrParser::extractRegion(hocrDoc, pixelSelection);
        regionData.pdfBbox = pImpl_->coordMapper_->pixelRectToPdf(regionData.pixelBbox, transform);

        // Store region data
        result.region.pdfBbox = regionData.pdfBbox;
        result.region.pixelBbox = regionData.pixelBbox;
        result.region.originalText = regionData.plainText;

        // Step 2: Render the page to an image
        const QImage pageImage = pImpl_->ocrPipeline_->renderPage(pageIndex);

        // Step 3: Detect background color/gradient
        const BackgroundInfo bgInfo = pImpl_->bgDetector_->detect(
            pageImage, regionData.pixelBbox, regionData.lines);
        result.region.bgInfo = bgInfo;

        // Step 4: Match fonts from OCR properties
        const int ocrDpi = pImpl_->ocrPipeline_->config().ocrDpi;
        const FontMatch fontMatch = pImpl_->fontMatcher_->matchRegion(
            regionData, pageImage, ocrDpi);
        result.region.fontMatch = fontMatch;

        // Step 5: Assess quality
        const QualityVerdict verdict = pImpl_->qualityAssessor_->evaluate(
            regionData, bgInfo, fontMatch.confidence);
        result.qualityVerdict = verdict;

        // Analysis complete (no PDF modification for steps 6-7)
        result.success = true;
        return result;

    } catch (const pdfe::core::PdfeException& ex) {
        result.success = false;
        result.errorMessage = std::string("PDFE exception: ") + ex.what();
        return result;
    } catch (const std::exception& ex) {
        result.success = false;
        result.errorMessage = std::string("Exception: ") + ex.what();
        return result;
    } catch (...) {
        result.success = false;
        result.errorMessage = "Unknown exception during analysis";
        return result;
    }
}

} // namespace pdfe::tre
