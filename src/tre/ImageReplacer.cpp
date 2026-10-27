#include "tre/ImageReplacer.h"
#include "tre/LanczosResize.h"
#include "tre/RegionReconstructor.h"
#include "tre/OcrPipeline.h"
#include "tre/CoordMapper.h"
#include "tre/BgColorDetector.h"
#include "tre/FontMatcher.h"
#include "tre/ImageMasker.h"
#include "tre/BitmapCompositor.h"
#include "tre/PdfContentWriter.h"
#include "tre/HocrParser.h"
#include "tre/GlyphAtlas.h"
#include "tre/EnrichedAtlas.h"
#include "tre/GlyphAtlasCompositor.h"
#include "core/Page.h"
#include "core/Document.h"

#include "tre/TreTypes.h"

#include <QDebug>
#include <QDir>
#include <QPainter>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
#include <unordered_map>

namespace pdfe::tre {

namespace {

// ---------------------------------------------------------------------------
// A3 — per-page working DPI helpers
//
// The TRE image-edit pipeline operates in a consistent pixel coordinate
// space defined by a working DPI. Historically that DPI was always
// OcrConfig::ocrDpi (usually 300). When the source image XObject has a
// different native DPI, rendering at ocrDpi and then upscaling to the
// image's real dimensions costs crispness.
//
// The fix uses Page::sourceImageDpi() to infer native DPI per page,
// clamps it to [150, 600] for memory/quality tradeoffs, and re-renders
// the page image at that working DPI for the edit path only. HOCR
// data is kept at the original ocrDpi and its pixel coordinates are
// scaled by workingDpi/ocrDpi (Option b) rather than re-running OCR.
// ---------------------------------------------------------------------------

constexpr int kWorkingDpiMin = 150;
constexpr int kWorkingDpiMax = 600;

/// Compute per-page working DPI from the page's source image DPI,
/// clamped to [kWorkingDpiMin, kWorkingDpiMax]. Falls back to
/// ocrDpi when the page has no image XObject or detection fails.
int computeWorkingDpi(double sourceImageDpi, int ocrDpi) {
    if (sourceImageDpi <= 0.0) {
        return ocrDpi;
    }
    int rounded = static_cast<int>(std::round(sourceImageDpi));
    return std::clamp(rounded, kWorkingDpiMin, kWorkingDpiMax);
}

/// Scale a PixelRect in place by a scalar factor.
void scalePixelRect(PixelRect& r, double scale) {
    r.x0 *= scale;
    r.y0 *= scale;
    r.x1 *= scale;
    r.y1 *= scale;
}

/// Deep-scale a HocrLine (bbox + all word/char bboxes + pixel scalars).
void scaleHocrLine(HocrLine& line, double scale) {
    scalePixelRect(line.bbox, scale);
    line.baselineOffset = static_cast<float>(line.baselineOffset * scale);
    line.xSizePx = static_cast<float>(line.xSizePx * scale);
    line.descendersPx = static_cast<float>(line.descendersPx * scale);
    line.ascendersPx = static_cast<float>(line.ascendersPx * scale);
    // baselineSlope is a ratio, not a pixel value — do not scale.
    for (auto& w : line.words) {
        scalePixelRect(w.bbox, scale);
        w.fontSizePx = static_cast<float>(w.fontSizePx * scale);
        for (auto& c : w.chars) {
            scalePixelRect(c.bbox, scale);
        }
    }
}

/// Scale all pixel-space data in a RegionTextData by `scale`.
/// pdfBbox is in PDF points and is *not* scaled. Returns the mutated
/// input for chaining.
RegionTextData scaleRegionData(RegionTextData data, double scale) {
    if (scale == 1.0) return data;
    scalePixelRect(data.pixelBbox, scale);
    for (auto& line : data.lines) {
        scaleHocrLine(line, scale);
    }
    return data;
}

} // anonymous namespace


class ImageReplacer::Impl {
public:
    OcrPipeline* ocrPipeline_ = nullptr;
    CoordMapper* coordMapper_ = nullptr;
    BgColorDetector* bgDetector_ = nullptr;
    FontMatcher* fontMatcher_ = nullptr;
    ImageMasker* imageMasker_ = nullptr;
    BitmapCompositor* bitmapCompositor_ = nullptr;
    PdfContentWriter* contentWriter_ = nullptr;

    // C2: Memory-budgeted LRU cache of ORIGINAL pre-edit page images.
    //
    // Prevents quality degradation from repeated JPEG recompression:
    // without this, each edit re-reads the already-degraded image from
    // the PDF (written by the previous edit's replacePageImage), compresses
    // it again, and compounds the loss. The cache preserves the pristine
    // original for all subsequent edits on the same page.
    //
    // Eviction is driven by a configurable byte budget (default 256 MB)
    // rather than a hardcoded page count. Large high-DPI scans naturally
    // use more of the budget; small pages use less. This replaces the
    // old kMaxCachedPages=3 limit which silently defeated the
    // recompression fix on any non-sequential editing pattern.
    //
    // See docs/image-pdf-risks.md (C2).
    qint64 cacheBudgetBytes_ = ImageReplacer::kDefaultCacheBudgetBytes;
    qint64 cacheBytes_ = 0;  ///< Current total size of cached QImages in bytes.
    std::unordered_map<int, QImage> originalImageCache_;
    std::vector<int> cacheAccessOrder_;  // LRU: most recent at back

    // Stats counters
    qint64 cacheHits_ = 0;
    qint64 cacheMisses_ = 0;
    qint64 cacheEvictions_ = 0;

    /// Byte cost of a QImage in the cache, matching its in-memory footprint.
    static qint64 imageBytes(const QImage& img) {
        if (img.isNull()) return 0;
        // QImage::sizeInBytes returns the total pixel buffer size including
        // row padding; this is the authoritative cost metric.
        return static_cast<qint64>(img.sizeInBytes());
    }

    /// Evict LRU entries until cacheBytes_ fits within cacheBudgetBytes_,
    /// OR until the cache is empty. Does NOT evict the page just inserted
    /// when called from getOriginalPageImage — caller must append to LRU
    /// after this call so the new entry is protected.
    void evictUntilFits(qint64 budget) {
        while (cacheBytes_ > budget && !cacheAccessOrder_.empty()) {
            int evictIdx = cacheAccessOrder_.front();
            cacheAccessOrder_.erase(cacheAccessOrder_.begin());
            auto it = originalImageCache_.find(evictIdx);
            if (it != originalImageCache_.end()) {
                cacheBytes_ -= imageBytes(it->second);
                if (cacheBytes_ < 0) cacheBytes_ = 0;
                originalImageCache_.erase(it);
                ++cacheEvictions_;
                qDebug() << "[ImageReplacer] Evicted cached image for page"
                         << evictIdx << "(budget="
                         << cacheBudgetBytes_ << "bytes, now"
                         << cacheBytes_ << "used,"
                         << static_cast<int>(originalImageCache_.size())
                         << "entries)";
            }
        }
    }

    /// Resolve which image XObject an edit over `regionFitz` must be written
    /// into, and whether that is the page's main (largest) image.
    ///
    /// GitHub #262. Both write-back sites need exactly this, and when only one
    /// of them had it the defect simply reopened through the other door.
    struct WriteTarget {
        PageImageInfo info;
        bool isMain = false;
        bool resolved = false;
    };

    WriteTarget resolveWriteTarget(pdfe::core::Page* page,
                                   pdfe::core::Document* document,
                                   const QRectF& regionFitz) {
        WriteTarget t;
        auto mainInfo = contentWriter_->findPageImageXObject(page, document);
        auto targetInfo =
            contentWriter_->findImageXObjectForRegion(page, document, regionFitz);
        if (!targetInfo.has_value()) {
            // No region-aware answer (no overlap, or two resources sharing
            // pixel dimensions). Fall back to the historical behaviour rather
            // than refuse the edit — the post-write visibility check is what
            // stops a wrong fallback being reported as success.
            targetInfo = mainInfo;
        }
        if (!targetInfo.has_value()) return t;
        t.info = *targetInfo;
        t.isMain = mainInfo.has_value() &&
                   mainInfo->xobjectName == targetInfo->xobjectName;
        t.resolved = true;
        return t;
    }

    /// Compose the image to write back: the target XObject's pristine pixels
    /// with the edited region painted into its correct place in that image's
    /// OWN pixel grid.
    ///
    /// GitHub #262. The previous arithmetic scaled the edit bbox by
    /// origImage/composited and used the result directly, which assumes the
    /// target image spans the whole page. It need not: fzImg0 in
    /// mixed_dpi_page.pdf covers Rect(51,51,561,651) of a 612x792 page, so even
    /// an edit over the photo landed offset. Where a scan's single image does
    /// cover the page, placement == page rect and this reduces to exactly the
    /// old arithmetic.
    ///
    /// Returns a null QImage when nothing could be composed; the caller then
    /// writes `compositedImage` unchanged, as before.
    QImage buildWriteBackImage(pdfe::core::Page* page,
                               pdfe::core::Document* document,
                               int pageIndex,
                               const QImage& compositedImage,
                               const PixelRect& editBbox,
                               const WriteTarget& target) {
        if (!target.resolved || compositedImage.isNull() ||
            compositedImage.width() <= 0 || compositedImage.height() <= 0) {
            return {};
        }

        // Pristine pixels of the TARGET image. The LRU cache is keyed by page
        // index alone, so it can only describe the page's main image; for any
        // other target we decode uncached. That costs one decode per edit and
        // buys correctness — a cache returning the wrong image would be worse
        // than no cache at all.
        QImage origImage =
            target.isMain
                ? getOriginalPageImage(page, document, pageIndex)
                : contentWriter_->getOriginalImage(page, document, target.info);
        if (origImage.isNull()) return {};

        const QRectF pageRect = page->boundingBox();
        QRectF placement = target.info.placementFitz;
        if (placement.isEmpty() || pageRect.isEmpty()) {
            placement = pageRect;  // placement unknown -> old page-spanning assumption
        }
        if (placement.isEmpty()) return {};

        QRect ocrRect(
            static_cast<int>(editBbox.x0), static_cast<int>(editBbox.y0),
            static_cast<int>(editBbox.width()), static_cast<int>(editBbox.height()));
        ocrRect = ocrRect.intersected(compositedImage.rect());
        if (ocrRect.isEmpty()) return {};

        const double ptPerPxX =
            pageRect.width() / static_cast<double>(compositedImage.width());
        const double ptPerPxY =
            pageRect.height() / static_cast<double>(compositedImage.height());
        const double imgPxPerPtX =
            static_cast<double>(origImage.width()) / placement.width();
        const double imgPxPerPtY =
            static_cast<double>(origImage.height()) / placement.height();

        // Edit rect in page points, then relative to the placement.
        const double editPtX = pageRect.x() + ocrRect.x() * ptPerPxX;
        const double editPtY = pageRect.y() + ocrRect.y() * ptPerPxY;
        const double editPtW = ocrRect.width() * ptPerPxX;
        const double editPtH = ocrRect.height() * ptPerPxY;

        const int hiResW = std::max(1, static_cast<int>(editPtW * imgPxPerPtX));
        const int hiResH = std::max(1, static_cast<int>(editPtH * imgPxPerPtY));
        const int hiResX = static_cast<int>((editPtX - placement.x()) * imgPxPerPtX);
        const int hiResY = static_cast<int>((editPtY - placement.y()) * imgPxPerPtY);

        QImage editedRegion = compositedImage.copy(ocrRect);
        // C1: Lanczos-3 for >=1.5x upscales, bilinear below.
        QImage scaledRegion =
            LanczosResize::resizeBest(editedRegion, QSize(hiResW, hiResH));
        QImage finalImage = origImage.copy();  // deep copy to avoid modifying cache
        QPainter hiResPainter(&finalImage);
        hiResPainter.drawImage(hiResX, hiResY, scaledRegion);
        hiResPainter.end();

        qDebug() << "[ImageReplacer] Quality preservation: target="
                 << QString::fromStdString(target.info.xobjectName)
                 << (target.isMain ? "(main)" : "(region-selected)")
                 << "original=" << origImage.width() << "x" << origImage.height()
                 << "placement=" << placement
                 << "composited=" << compositedImage.width() << "x" << compositedImage.height()
                 << "editRect=" << ocrRect
                 << "-> dst" << QRect(hiResX, hiResY, hiResW, hiResH);
        return finalImage;
    }

    /// Whether a write into `target` can be seen at `regionFitz` (GitHub #262).
    ///
    /// Returns true when the target is NOT the image drawn on top at that
    /// region, i.e. when anything written into it will be painted over. Checked
    /// BEFORE the write, so an edit that cannot possibly show is reported as a
    /// failure instead of a terminal RECOMPOSITE_DONE.
    ///
    /// Region-aware selection already prefers the topmost covering image, so in
    /// practice this only fires on the fallback path — where the dimension match
    /// was ambiguous and the largest image was used instead. That is exactly the
    /// case where the old code silently wrote into an occluded image.
    bool targetIsOccludedAt(pdfe::core::Page* page,
                            pdfe::core::Document* document,
                            const PageImageInfo& target,
                            const QRectF& regionFitz) {
        const auto top =
            contentWriter_->topmostImageOverRegion(page, document, regionFitz);
        if (!top.has_value()) return false;  // nothing covers it; no claim to make

        // Identity by dimensions AND placement. Dimensions alone are ambiguous
        // on a page carrying two same-sized images -- the very situation that
        // sends us down the fallback path.
        const bool sameSize = top->width == target.width &&
                              top->height == target.height;
        if (!sameSize) return true;
        if (target.placementFitz.isEmpty()) {
            // Placement unknown (the fallback does not record it). Dimensions
            // match, so treat it as the same image rather than guess occlusion.
            return false;
        }
        const QRectF a = top->placementFitz;
        const QRectF b = target.placementFitz;
        const double tol = 0.5;
        return std::abs(a.x() - b.x()) > tol || std::abs(a.y() - b.y()) > tol ||
               std::abs(a.width() - b.width()) > tol ||
               std::abs(a.height() - b.height()) > tol;
    }

    /// Get the original (pre-edit) page image, caching on first access.
    /// Returns the cached original on subsequent calls for the same page,
    /// avoiding JPEG recompression quality loss.
    QImage getOriginalPageImage(pdfe::core::Page* page,
                                pdfe::core::Document* document,
                                int pageIndex) {
        auto it = originalImageCache_.find(pageIndex);
        if (it != originalImageCache_.end()) {
            // Move to back of LRU
            cacheAccessOrder_.erase(
                std::remove(cacheAccessOrder_.begin(), cacheAccessOrder_.end(), pageIndex),
                cacheAccessOrder_.end());
            cacheAccessOrder_.push_back(pageIndex);
            ++cacheHits_;
            return it->second;
        }

        QImage orig = contentWriter_->getOriginalImage(page, document);
        if (orig.isNull()) {
            return orig;
        }
        ++cacheMisses_;

        // Cache disabled (budget == 0) — return without storing.
        if (cacheBudgetBytes_ <= 0) {
            return orig;
        }

        qint64 cost = imageBytes(orig);
        // If this single image exceeds the budget, return it without
        // caching (a hot re-edit will re-decode each time, but that is
        // the price of a small budget; we never evict everything and
        // still fail to fit). Record as eviction-miss for stats.
        if (cost > cacheBudgetBytes_) {
            qDebug() << "[ImageReplacer] Image for page" << pageIndex
                     << "(" << cost << "bytes) exceeds cache budget"
                     << cacheBudgetBytes_ << "— not cached";
            return orig;
        }

        // Evict LRU entries until we can fit the new one.
        evictUntilFits(cacheBudgetBytes_ - cost);

        // Store and update LRU order.
        originalImageCache_.emplace(pageIndex, orig);
        cacheAccessOrder_.push_back(pageIndex);
        cacheBytes_ += cost;
        qDebug() << "[ImageReplacer] Cached original image for page"
                 << pageIndex << ":" << orig.width() << "x" << orig.height()
                 << "(" << cost << "bytes, cache now"
                 << cacheBytes_ << "/" << cacheBudgetBytes_ << ")";
        return orig;
    }

    Impl(
        OcrPipeline* ocrPipeline,
        CoordMapper* coordMapper,
        BgColorDetector* bgDetector,
        FontMatcher* fontMatcher,
        ImageMasker* imageMasker,
        BitmapCompositor* bitmapCompositor,
        PdfContentWriter* contentWriter)
        : ocrPipeline_(ocrPipeline)
        , coordMapper_(coordMapper)
        , bgDetector_(bgDetector)
        , fontMatcher_(fontMatcher)
        , imageMasker_(imageMasker)
        , bitmapCompositor_(bitmapCompositor)
        , contentWriter_(contentWriter) {}
};

ImageReplacer::ImageReplacer(
    OcrPipeline* ocrPipeline,
    CoordMapper* coordMapper,
    BgColorDetector* bgDetector,
    FontMatcher* fontMatcher,
    ImageMasker* imageMasker,
    BitmapCompositor* bitmapCompositor,
    PdfContentWriter* contentWriter)
    : pImpl_(std::make_unique<Impl>(
          ocrPipeline, coordMapper, bgDetector, fontMatcher,
          imageMasker, bitmapCompositor, contentWriter)) {}

ImageReplacer::~ImageReplacer() = default;

ImageReplacer::ImageReplacer(ImageReplacer&&) noexcept = default;
ImageReplacer& ImageReplacer::operator=(ImageReplacer&&) noexcept = default;

void ImageReplacer::invalidateOriginalCache(int pageIndex) {
    auto it = pImpl_->originalImageCache_.find(pageIndex);
    if (it != pImpl_->originalImageCache_.end()) {
        pImpl_->cacheBytes_ -= Impl::imageBytes(it->second);
        if (pImpl_->cacheBytes_ < 0) pImpl_->cacheBytes_ = 0;
        pImpl_->originalImageCache_.erase(it);
        ++pImpl_->cacheEvictions_;
    }
    pImpl_->cacheAccessOrder_.erase(
        std::remove(pImpl_->cacheAccessOrder_.begin(),
                    pImpl_->cacheAccessOrder_.end(), pageIndex),
        pImpl_->cacheAccessOrder_.end());
}

void ImageReplacer::clearOriginalCache() {
    pImpl_->cacheEvictions_ += static_cast<qint64>(pImpl_->originalImageCache_.size());
    pImpl_->originalImageCache_.clear();
    pImpl_->cacheAccessOrder_.clear();
    pImpl_->cacheBytes_ = 0;
}

bool ImageReplacer::primeOriginalCache(pdfe::core::Page* page,
                                       pdfe::core::Document* document,
                                       int pageIndex) {
    if (!page || !document) return false;
    QImage img = pImpl_->getOriginalPageImage(page, document, pageIndex);
    return !img.isNull();
}

void ImageReplacer::setCacheBudgetBytes(qint64 bytes) {
    pImpl_->cacheBudgetBytes_ = std::max<qint64>(0, bytes);
    // Apply the new budget immediately. Special-case budget==0: drop all.
    if (pImpl_->cacheBudgetBytes_ == 0) {
        clearOriginalCache();
    } else {
        pImpl_->evictUntilFits(pImpl_->cacheBudgetBytes_);
    }
}

qint64 ImageReplacer::cacheBudgetBytes() const {
    return pImpl_->cacheBudgetBytes_;
}

ImageReplacer::CacheStats ImageReplacer::cacheStats() const {
    CacheStats s;
    s.currentBytes = pImpl_->cacheBytes_;
    s.maxBytes = pImpl_->cacheBudgetBytes_;
    s.entries = static_cast<int>(pImpl_->originalImageCache_.size());
    s.hits = pImpl_->cacheHits_;
    s.misses = pImpl_->cacheMisses_;
    s.evictions = pImpl_->cacheEvictions_;
    return s;
}

int ImageReplacer::computeExpandedWidth(
    int basePixelWidth,
    const std::string& editedText,
    const std::string& originalText)
{
    if (originalText.empty() || editedText.empty() || editedText == originalText)
        return basePixelWidth;

    std::u32string editU32 = utf8ToU32(editedText);
    std::u32string origU32 = utf8ToU32(originalText);

    if (origU32.empty() || editU32.size() <= origU32.size())
        return basePixelWidth;

    float ratio = static_cast<float>(editU32.size())
                  / static_cast<float>(origU32.size());
    ratio *= 1.2f;  // 20% safety margin for variable-width chars
    return std::max(basePixelWidth,
        static_cast<int>(std::ceil(basePixelWidth * ratio)));
}

int ImageReplacer::computeExpandedWidthMeasured(
    int basePixelWidth,
    const std::string& editedText,
    const std::string& originalText,
    BitmapCompositor* compositor,
    const FontMatch& fontMatch,
    int dpi)
{
    // Degenerate cases delegate to the character-count fallback.
    if (!compositor ||
        fontMatch.fontFilePath.empty() ||
        fontMatch.sizePt <= 0.0f ||
        originalText.empty() ||
        editedText.empty() ||
        editedText == originalText) {
        return computeExpandedWidth(basePixelWidth, editedText, originalText);
    }

    std::u32string editU32 = utf8ToU32(editedText);
    std::u32string origU32 = utf8ToU32(originalText);
    if (origU32.empty() || editU32.empty()) {
        return computeExpandedWidth(basePixelWidth, editedText, originalText);
    }

    // Measure actual advance widths of both strings via the shaper.
    int editWidthPx = compositor->measureTextWidth(
        editU32, fontMatch.fontFilePath, fontMatch.sizePt, dpi);
    int origWidthPx = compositor->measureTextWidth(
        origU32, fontMatch.fontFilePath, fontMatch.sizePt, dpi);

    // If either measurement failed, fall back.
    if (editWidthPx <= 0 || origWidthPx <= 0) {
        qDebug() << "[ImageReplacer] computeExpandedWidthMeasured: shaper failed, "
                 << "falling back to char-count ratio";
        return computeExpandedWidth(basePixelWidth, editedText, originalText);
    }

    qDebug() << "[ImageReplacer] B2 measured widths:"
             << "orig=" << origWidthPx << "px"
             << "edit=" << editWidthPx << "px"
             << "base=" << basePixelWidth << "px";

    // If the edit is no wider than the original, keep the original bbox.
    if (editWidthPx <= origWidthPx) {
        return basePixelWidth;
    }

    // Ratio of measured widths × 20% safety margin for metric-to-bitmap
    // rounding (NOT a translation guess).
    double ratio = static_cast<double>(editWidthPx)
                   / static_cast<double>(origWidthPx);
    ratio *= 1.2;
    int expanded = static_cast<int>(std::ceil(basePixelWidth * ratio));
    return std::max(basePixelWidth, expanded);
}

bool ImageReplacer::shouldSkipStyledRuns(
    const std::string& editedText,
    const std::string& originalText)
{
    return !editedText.empty() && editedText != originalText;
}

namespace {

/// True when a string carries no visible characters at all.
/// An OCR pass over blank paper yields "" here, but a pass that picked up
/// only layout whitespace yields " \n" — both mean "nothing to cover".
bool isBlankText(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
}

/// Bounding box of every pixel in `bitmap` that will put ink on the page.
///
/// The text bitmap is ARGB32_Premultiplied cleared to Qt::transparent and is
/// drawn with SourceOver, so alpha is exactly "does this pixel change the
/// page". The threshold is 0 rather than something higher deliberately: the
/// outermost ring of an antialiased glyph has very low alpha, and excluding it
/// would put the fill boundary through the middle of the glyph edge.
///
/// Returns false when there is no such pixel, leaving `out` untouched.
bool measureInkExtent(const QImage& bitmap, QRect* out) {
    if (bitmap.isNull() || bitmap.width() <= 0 || bitmap.height() <= 0) {
        return false;
    }
    QImage img = bitmap;
    if (img.format() != QImage::Format_ARGB32 &&
        img.format() != QImage::Format_ARGB32_Premultiplied) {
        img = img.convertToFormat(QImage::Format_ARGB32);
    }
    int minX = img.width(), minY = img.height(), maxX = -1, maxY = -1;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            if (qAlpha(row[x]) == 0) continue;
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
            if (y < minY) minY = y;
            if (y > maxY) maxY = y;
        }
    }
    if (maxX < 0 || maxY < 0) {
        return false;  // fully transparent — no ink
    }
    *out = QRect(QPoint(minX, minY), QPoint(maxX, maxY));
    return true;
}

} // namespace

ImageReplacer::FillRegion ImageReplacer::computeFillRegion(
    const PixelRect& candidate,
    const QImage& textBitmap,
    const std::string& originalText,
    int padPx)
{
    FillRegion decision;
    decision.bbox = candidate;
    decision.bitmapCrop = textBitmap.isNull() ? QRect() : textBitmap.rect();
    decision.shrunkToInk = false;

    // Replacement: the fill MUST still cover the original text's extent, or
    // the scanned glyphs underneath show through the new rendering. Shrinking
    // here would reintroduce exactly the ghosting the caller's union prevents.
    if (!isBlankText(originalText)) {
        return decision;
    }

    QRect ink;
    if (!measureInkExtent(textBitmap, &ink)) {
        return decision;  // nothing rendered, or no visible ink: fill as before
    }

    const int pad = std::max(0, padPx);
    ink.adjust(-pad, -pad, pad, pad);
    ink = ink.intersected(textBitmap.rect());
    if (ink.width() <= 0 || ink.height() <= 0) {
        return decision;
    }

    // The bitmap's origin sits at the candidate's top-left, so an offset
    // inside the bitmap is that same offset in page pixels.
    decision.bbox.x0 = candidate.x0 + ink.left();
    decision.bbox.y0 = candidate.y0 + ink.top();
    decision.bbox.x1 = decision.bbox.x0 + ink.width();
    decision.bbox.y1 = decision.bbox.y0 + ink.height();
    decision.bitmapCrop = ink;
    decision.shrunkToInk = true;
    return decision;
}

ReconstructionResult ImageReplacer::replace(
    pdfe::core::Page* page,
    pdfe::core::Document* document,
    int pageIndex,
    const PdfRect& selectionPdf,
    const std::string& editedText,
    RegionId regionId,
    bool snapToLines,
    const std::vector<FontRun>& fontRuns,
    float fontSizeOverridePt,
    bool useGlyphAtlas,
    const FontMatch* fontMatchOverride,
    const std::shared_ptr<EnrichedAtlas>& enrichedAtlas) {

    ReconstructionResult result;
    result.success = false;

    // Validate inputs
    if (!page || !document) {
        result.errorMessage = "Null page or document pointer";
        return result;
    }

    if (pageIndex < 0 || pageIndex >= document->pageCount()) {
        result.errorMessage = "Page index out of range";
        return result;
    }

    try {
        // Step 1: OCR the page and extract region
        std::string hocrXml = pImpl_->ocrPipeline_->ocrPage(pageIndex);
        HocrDocument hocrDoc = HocrParser::parse(hocrXml);

        // Convert PDF selection to pixel coordinates (in ocrDpi space — the
        // coordMapper's transform is built from OcrPipeline::config().ocrDpi)
        const AffineTransform& transform = pImpl_->coordMapper_->getTransform(pageIndex);
        PixelRect pixelSelection = pImpl_->coordMapper_->pdfRectToPixel(selectionPdf, transform);

        qDebug() << "[ImageReplacer] selectionPdf:"
                 << selectionPdf.x0 << "," << selectionPdf.y0
                 << selectionPdf.x1 << "," << selectionPdf.y1
                 << "pixelSelection (ocrDpi):"
                 << pixelSelection.x0 << "," << pixelSelection.y0
                 << pixelSelection.x1 << "," << pixelSelection.y1
                 << "(" << pixelSelection.width() << "x" << pixelSelection.height() << ")";

        // A3: compute the per-page working DPI. Pixel coordinates above
        // are in ocrDpi space; everything below operates in workingDpi
        // space, rendered to match the source image's native DPI.
        int ocrDpi = pImpl_->ocrPipeline_->config().ocrDpi;
        // Defect #2: use the native DPI of the image actually under the edited
        // region, not the page maximum. A 150-DPI region is no longer rendered
        // at the page's 300-DPI photo resolution and upscaled.
        const QRectF selRectFitz(selectionPdf.x0, selectionPdf.y0,
                                 selectionPdf.x1 - selectionPdf.x0,
                                 selectionPdf.y1 - selectionPdf.y0);
        double sourceDpi = page->sourceImageDpiAt(selRectFitz);
        int workingDpi = computeWorkingDpi(sourceDpi, ocrDpi);
        double dpiScale = static_cast<double>(workingDpi) / static_cast<double>(ocrDpi);
        qDebug() << "[ImageReplacer] A3 working DPI: ocrDpi=" << ocrDpi
                 << "sourceDpi=" << sourceDpi
                 << "workingDpi=" << workingDpi
                 << "scale=" << dpiScale;

        if (dpiScale != 1.0) {
            scalePixelRect(pixelSelection, dpiScale);
            qDebug() << "[ImageReplacer] pixelSelection scaled to workingDpi:"
                     << pixelSelection.x0 << "," << pixelSelection.y0
                     << pixelSelection.x1 << "," << pixelSelection.y1;
        }

        // Extract region data at ocrDpi (HOCR native space), then scale
        // to workingDpi for downstream consumption. The original pixelSelection
        // is divided back down to ocrDpi for the HocrParser call.
        PixelRect pixelSelectionOcr = pixelSelection;
        if (dpiScale != 1.0) {
            scalePixelRect(pixelSelectionOcr, 1.0 / dpiScale);
        }
        RegionTextData regionData =
            scaleRegionData(HocrParser::extractRegion(hocrDoc, pixelSelectionOcr),
                            dpiScale);
        qDebug() << "[ImageReplacer] regionData:"
                 << "lines=" << regionData.lines.size()
                 << "pixelBbox=" << regionData.pixelBbox.x0 << "," << regionData.pixelBbox.y0
                 << regionData.pixelBbox.x1 << "," << regionData.pixelBbox.y1
                 << "(" << regionData.pixelBbox.width() << "x" << regionData.pixelBbox.height() << ")"
                 << "plainText=" << QString::fromStdString(regionData.plainText).left(50);

        // Log all HOCR lines for bbox comparison
        int lineIdx = 0;
        for (const auto& blk : hocrDoc.blocks) {
            for (const auto& par : blk.paragraphs) {
                for (const auto& ln : par.lines) {
                    bool overlaps = ln.bbox.intersects(pixelSelection);
                    qDebug() << "[ImageReplacer] HOCR line[" << lineIdx++ << "]:"
                             << ln.bbox.x0 << "," << ln.bbox.y0
                             << ln.bbox.x1 << "," << ln.bbox.y1
                             << "overlaps=" << overlaps;
                }
            }
        }

        // Step 2: Render page to image at the per-page working DPI (A3).
        QImage pageImage = pImpl_->ocrPipeline_->renderPageAtDpi(pageIndex, workingDpi);
        if (pageImage.isNull()) {
            result.errorMessage = "Failed to render page image";
            return result;
        }

        // Determine compositing region: snap to line boundaries for full-region
        // replacement (TRE tool), or use exact selection for inline span edits.
        PixelRect renderBbox;
        if (snapToLines) {
            renderBbox = regionData.pixelBbox;
            if (renderBbox.width() <= 0 || renderBbox.height() <= 0) {
                renderBbox = pixelSelection;
            }
        } else {
            // Inline span edit: union the PDF-derived pixelSelection with the
            // HOCR-derived regionData.pixelBbox. On image-dominated pages with
            // invisible OCR overlay, these two rects can be vertically misaligned
            // (e.g., PDF span at y=1085 vs HOCR line at y=1107). Without the
            // union, the background fill may not fully cover the original scanned
            // text, causing ghost text visible beneath the new rendering.
            renderBbox = pixelSelection;
            if (regionData.pixelBbox.width() > 0 && regionData.pixelBbox.height() > 0) {
                renderBbox.x0 = std::min(renderBbox.x0, regionData.pixelBbox.x0);
                renderBbox.y0 = std::min(renderBbox.y0, regionData.pixelBbox.y0);
                renderBbox.x1 = std::max(renderBbox.x1, regionData.pixelBbox.x1);
                renderBbox.y1 = std::max(renderBbox.y1, regionData.pixelBbox.y1);
            }
        }

        qDebug() << "[ImageReplacer] renderBbox:"
                 << renderBbox.x0 << "," << renderBbox.y0
                 << renderBbox.x1 << "," << renderBbox.y1
                 << "(" << renderBbox.width() << "x" << renderBbox.height() << ")"
                 << "snapToLines=" << snapToLines;

        // Step 3: Detect background color. B3: pass workingDpi so the
        // halftone/texture classifier can scale its kernel to the
        // source image's actual pixel density.
        BackgroundInfo bgInfo = snapToLines
            ? pImpl_->bgDetector_->detect(pageImage, renderBbox,
                                          regionData.lines, workingDpi)
            : pImpl_->bgDetector_->detect(pageImage, renderBbox, workingDpi);
        qDebug() << "[ImageReplacer] B3 bg classification="
                 << static_cast<int>(bgInfo.classification)
                 << "variance=" << bgInfo.localVariance
                 << "halftoneSpacingPx=" << bgInfo.halftoneSpacingPx;

        // Step 4: Match font from OCR properties (or use caller's override)
        // A3: use the per-page working DPI, not the raw ocrDpi.
        int dpi = workingDpi;
        FontMatch fontMatch;
        if (fontMatchOverride) {
            // Caller already knows the correct font (e.g., real embedded font
            // looked up via known-pair table). Skip OCR-based font matching.
            fontMatch = *fontMatchOverride;
            qDebug() << "[ImageReplacer] Using fontMatchOverride:"
                     << "family=" << QString::fromStdString(fontMatch.fontFamily)
                     << "sizePt=" << fontMatch.sizePt
                     << "filePath=" << QString::fromStdString(fontMatch.fontFilePath);
        } else {
            fontMatch = pImpl_->fontMatcher_->matchRegion(regionData, pageImage, dpi);
            qDebug() << "[ImageReplacer] FontMatch: family=" << QString::fromStdString(fontMatch.fontFamily)
                     << "sizePt=" << fontMatch.sizePt
                     << "filePath=" << QString::fromStdString(fontMatch.fontFilePath)
                     << "dpi=" << dpi
                     << "fontSizeOverridePt=" << fontSizeOverridePt;

            // Override FontMatcher's detected size when the caller provides a known-correct
            // font size (e.g., from a real embedded font's PDF text matrix).
            if (fontSizeOverridePt > 0.0f) {
                qDebug() << "[ImageReplacer] Overriding FontMatch sizePt from"
                         << fontMatch.sizePt << "to" << fontSizeOverridePt;
                fontMatch.sizePt = fontSizeOverridePt;
            }
        }

        // Determine text to render
        std::string textToRender = editedText.empty() ? regionData.plainText : editedText;
        if (textToRender.empty()) {
            result.errorMessage = "No text to render (OCR and edited text both empty)";
            return result;
        }

        // Step 5: Convert text to UTF-32 and render to bitmap
        std::u32string textU32 = utf8ToU32(textToRender);
        bool isEdited = shouldSkipStyledRuns(editedText, regionData.plainText);
        int basePixelWidth = std::max(1, static_cast<int>(renderBbox.width()));
        int pixelHeight = std::max(1, static_cast<int>(renderBbox.height()));

        // Expand bitmap width when edited text is longer than original to prevent
        // truncation. Includes 20% safety margin for variable-width characters.
        // B2: use measured advance widths instead of character-count ratio.
        int pixelWidth = computeExpandedWidthMeasured(
            basePixelWidth, editedText, regionData.plainText,
            pImpl_->bitmapCompositor_, fontMatch, dpi);
        if (pixelWidth != basePixelWidth) {
            qDebug() << "[ImageReplacer] Expanding bitmap width for longer edit:"
                     << basePixelWidth << "->" << pixelWidth;
            // CRITICAL: Expand renderBbox to match the widened bitmap. Without
            // this, compositeTextBitmap's background fill only covers the original
            // renderBbox width, but the textBitmap extends beyond it. Characters
            // past the fill overlap the original scanned text = ghost text.
            renderBbox.x1 = renderBbox.x0 + pixelWidth;
        }

        // Cap font size so text fits within the bitmap. The line height at
        // a given point size is approximately sizePt × dpi/72 × 1.2. To fit
        // within pixelHeight: sizePt ≤ pixelHeight × 72 / dpi / 1.2.
        float maxFontPt = static_cast<float>(pixelHeight) * 72.0f
                          / static_cast<float>(dpi) / 1.2f;
        qDebug() << "[ImageReplacer] Bitmap dims: pixelW=" << pixelWidth
                 << "pixelH=" << pixelHeight
                 << "fontMatch.sizePt=" << fontMatch.sizePt
                 << "maxFontPt=" << maxFontPt
                 << "renderBbox=" << renderBbox.x0 << "," << renderBbox.y0
                 << renderBbox.width() << "x" << renderBbox.height();
        if (fontMatch.sizePt > maxFontPt && maxFontPt > 0.0f) {
            qDebug() << "[ImageReplacer] CAPPING font size from" << fontMatch.sizePt << "to" << maxFontPt;
            fontMatch.sizePt = maxFontPt;
        }

        // Detect per-word styles and build styled runs for rendering.
        // Skip when text has been edited: OCR-based styled runs have character
        // indices computed from the original OCR text, which don't align with
        // the edited text and would apply wrong styles at wrong positions.
        std::vector<StyledRun> styledRuns;
        if (!isEdited) {
            auto styledLines = regionData.lines;
            FontMatcher::detectWordStyles(pageImage, styledLines);
            int charOff = 0;
            for (const auto& ln : styledLines) {
                for (const auto& w : ln.words) {
                    int wLen = static_cast<int>(utf8ToU32(w.text).size());
                    if (w.style.isBold || w.style.isItalic ||
                        w.style.isUnderlined || w.style.isStrikethrough) {
                        styledRuns.push_back({charOff, charOff + wLen, w.style});
                    }
                    charOff += wLen + 1; // +1 for space
                }
            }
        }

        QImage textBitmap;

        // WYSIWYG path: when an EnrichedAtlas is provided (from overlay), use it
        // directly instead of building a fresh atlas. This ensures identical glyph
        // selection between overlay and save, providing true WYSIWYG.
        if (useGlyphAtlas && enrichedAtlas && fontRuns.empty()) {
            try {
                GlyphAtlasCompositor atlasCompositor;
                textBitmap = atlasCompositor.compose(
                    textU32, *enrichedAtlas, fontMatch, bgInfo.dominantColor,
                    pixelWidth, pixelHeight, dpi, pImpl_->bitmapCompositor_);
                qDebug() << "[ImageReplacer] EnrichedAtlas WYSIWYG path:"
                         << "atlasSize=" << enrichedAtlas->size()
                         << "bitmap=" << (textBitmap.isNull() ? "NULL" : "OK");
            } catch (const std::exception& e) {
                qDebug() << "[ImageReplacer] EnrichedAtlas compose failed:" << e.what()
                         << "- falling back to fresh atlas";
            }
        }

        // Glyph atlas path: extract character images from original scan.
        // IMPORTANT: Only use atlas when text has NOT been edited. When the user
        // has edited the text (isEdited=true), the char-level HOCR cache contains
        // bounding boxes from the original scan, but on subsequent keystrokes the
        // page image has been modified by previous replacePageImage calls. Building
        // the atlas from the modified image at original-scan coordinates extracts
        // wrong glyphs, producing garbled output. FreeType rendering is the safe
        // fallback for edited text.
        if (textBitmap.isNull() && useGlyphAtlas && fontRuns.empty() && !isEdited) {
            try {
                // Run char-level OCR (separate cache from word-level)
                std::string charHocr = pImpl_->ocrPipeline_->ocrPageWithCharBoxes(pageIndex);
                HocrDocument charDoc = HocrParser::parse(charHocr);

                // Extract char-level region using the full line bbox from word-level
                // OCR, not the narrow span bbox. The word-level extractRegion already
                // found lines matching our selection; using that line bbox ensures the
                // char-level extraction finds the same lines and extracts all characters
                // from the entire line. This maximizes atlas coverage (more unique chars)
                // and avoids misses from coordinate misalignment between PDF span bbox
                // and HOCR line bbox.
                PixelRect charRegionBbox = regionData.pixelBbox;
                if (charRegionBbox.width() <= 0 || charRegionBbox.height() <= 0) {
                    charRegionBbox = renderBbox;
                }
                qDebug() << "[ImageReplacer] Glyph atlas char extraction region:"
                         << "x=" << charRegionBbox.x0 << "y=" << charRegionBbox.y0
                         << "w=" << charRegionBbox.width() << "h=" << charRegionBbox.height()
                         << "(renderBbox: x=" << renderBbox.x0 << "y=" << renderBbox.y0
                         << "w=" << renderBbox.width() << "h=" << renderBbox.height() << ")";
                RegionTextData charRegion = HocrParser::extractRegion(charDoc, charRegionBbox);

                // Build glyph atlas from scanned page
                GlyphAtlas atlas;
                const int totalChars = std::accumulate(
                    charRegion.lines.begin(), charRegion.lines.end(), 0,
                    [](int acc, const auto& ln) {
                        return acc + std::accumulate(
                            ln.words.begin(), ln.words.end(), 0,
                            [](int wAcc, const auto& w) {
                                return wAcc + static_cast<int>(w.chars.size());
                            });
                    });
                qDebug() << "[ImageReplacer] Glyph atlas input:"
                         << "charRegionLines=" << charRegion.lines.size()
                         << "totalCharsInRegion=" << totalChars
                         << "pageImageNull=" << pageImage.isNull()
                         << "pageImageW=" << pageImage.width()
                         << "pageImageH=" << pageImage.height()
                         << "textColor=(" << fontMatch.color.r << ","
                         << fontMatch.color.g << "," << fontMatch.color.b << ")"
                         << "bgColor=(" << bgInfo.dominantColor.r << ","
                         << bgInfo.dominantColor.g << "," << bgInfo.dominantColor.b << ")";
                atlas.build(pageImage, charRegion.lines, fontMatch.color, bgInfo.dominantColor);
                qDebug() << "[ImageReplacer] Glyph atlas built:"
                         << "atlasSize=" << atlas.size()
                         << "coverage=" << atlas.coverage(textU32);

                if (atlas.coverage(textU32) >= 0.8f) {
                    // Compose text from atlas with FreeType fallback for missing chars
                    GlyphAtlasCompositor atlasCompositor;
                    textBitmap = atlasCompositor.compose(
                        textU32, atlas, fontMatch, bgInfo.dominantColor,
                        pixelWidth, pixelHeight, dpi, pImpl_->bitmapCompositor_);
                    qDebug() << "[ImageReplacer] Glyph atlas path: coverage="
                             << atlas.coverage(textU32)
                             << "atlasSize=" << atlas.size()
                             << "bitmap=" << (textBitmap.isNull() ? "NULL" : "OK");
                } else {
                    qDebug() << "[ImageReplacer] Glyph atlas coverage too low ("
                             << atlas.coverage(textU32) << "< 0.8) - falling back to FreeType";
                }
            } catch (const std::exception& e) {
                qDebug() << "[ImageReplacer] Glyph atlas failed:" << e.what()
                         << "- falling back to FreeType";
            }
        }

        // FreeType rendering path (fallback or primary when atlas not used)
        if (textBitmap.isNull() && !fontRuns.empty()) {
            qDebug() << "[ImageReplacer] Rendering with" << fontRuns.size() << "fontRuns";
            for (size_t i = 0; i < fontRuns.size(); ++i) {
                qDebug() << "  fontRun[" << i << "]:"
                         << "chars=" << fontRuns[i].charStart << "-" << fontRuns[i].charEnd
                         << "sizePt=" << fontRuns[i].fontSizePt
                         << "path=" << QString::fromStdString(fontRuns[i].fontFilePath);
            }
            textBitmap = pImpl_->bitmapCompositor_->renderMultiFontText(
                textU32, fontMatch, fontRuns, pixelWidth, pixelHeight, dpi);
        } else if (textBitmap.isNull() && !styledRuns.empty()) {
            qDebug() << "[ImageReplacer] Rendering with" << styledRuns.size() << "styledRuns"
                     << "fontMatch.sizePt=" << fontMatch.sizePt;
            textBitmap = pImpl_->bitmapCompositor_->renderStyledText(
                textU32, fontMatch, styledRuns, pixelWidth, pixelHeight, dpi);
        } else if (textBitmap.isNull()) {
            qDebug() << "[ImageReplacer] Rendering with default font"
                     << "fontMatch.sizePt=" << fontMatch.sizePt;
            textBitmap = pImpl_->bitmapCompositor_->renderText(
                textU32, fontMatch, pixelWidth, pixelHeight, dpi);
        }

        if (textBitmap.isNull()) {
            result.errorMessage = "Failed to render text bitmap";
            return result;
        }

        // Fit the fill to the glyphs when there was nothing under the
        // selection to cover. Replacements are left alone -- see
        // computeFillRegion() for why shrinking them reintroduces ghosting.
        {
            const int padPx = std::max(1, dpi / 150);
            FillRegion fill = computeFillRegion(renderBbox, textBitmap,
                                                regionData.plainText, padPx);
            if (fill.shrunkToInk) {
                qDebug() << "[ImageReplacer] add-over-blank: fitting fill to ink"
                         << renderBbox.width() << "x" << renderBbox.height()
                         << "->" << fill.bbox.width() << "x" << fill.bbox.height();
                textBitmap = textBitmap.copy(fill.bitmapCrop);
                renderBbox = fill.bbox;
            }
        }

        qDebug() << "[ImageReplacer] Final render state:"
                 << "textToRender='" << QString::fromStdString(textToRender).left(50) << "'"
                 << "textBitmap=" << textBitmap.width() << "x" << textBitmap.height()
                 << "renderBbox=(" << renderBbox.x0 << "," << renderBbox.y0
                 << "," << renderBbox.x1 << "," << renderBbox.y1 << ")"
                 << "size=" << renderBbox.width() << "x" << renderBbox.height()
                 << "bgColor=(" << bgInfo.dominantColor.r << ","
                 << bgInfo.dominantColor.g << "," << bgInfo.dominantColor.b << ")"
                 << "atlasPath=" << (useGlyphAtlas ? "yes" : "no");

#ifndef NDEBUG
        // Save diagnostic images for ghost text debugging
        {
            QString debugDir = QStringLiteral("C:/Users/dev/pdfe_debug");
            QDir().mkpath(debugDir);
            // Crop pageImage around renderBbox for comparison
            QRect cropRect(
                static_cast<int>(renderBbox.x0), static_cast<int>(renderBbox.y0),
                static_cast<int>(renderBbox.width()), static_cast<int>(renderBbox.height()));
            cropRect = cropRect.intersected(pageImage.rect());
            if (!cropRect.isEmpty()) {
                pageImage.copy(cropRect).save(debugDir + "/1_pageImage_crop.png");
            }
            textBitmap.save(debugDir + "/2_textBitmap.png");
        }
#endif

        // Step 6: Composite text bitmap over cleaned background.
        // B3: pass full bgInfo so ImageMasker can use pattern-copy
        // for halftone/textured backgrounds instead of solid fill.
        QImage compositedImage = pImpl_->imageMasker_->compositeTextBitmap(
            pageImage, textBitmap, renderBbox, bgInfo);

        if (compositedImage.isNull()) {
            result.errorMessage = "Failed to composite text bitmap";
            return result;
        }

#ifndef NDEBUG
        {
            QString debugDir = QStringLiteral("C:/Users/dev/pdfe_debug");
            QRect cropRect(
                static_cast<int>(renderBbox.x0), static_cast<int>(renderBbox.y0),
                static_cast<int>(renderBbox.width()), static_cast<int>(renderBbox.height()));
            cropRect = cropRect.intersected(compositedImage.rect());
            if (!cropRect.isEmpty()) {
                compositedImage.copy(cropRect).save(debugDir + "/3_composited_crop.png");
            }
        }
#endif

        // Step 7: Replace page image.
        // ALWAYS overlay the edited region onto the CACHED original image.
        // This prevents quality degradation from repeated JPEG recompression:
        // - Without cache: each edit re-reads the degraded image from PDF,
        //   composites onto it, and re-encodes — compounding JPEG loss.
        // - With cache: every edit composites onto the pristine original,
        //   so only ONE JPEG encoding is applied (at write time).
        // The edited region is extracted from compositedImage (rendered at
        // OCR DPI) and scaled to match the original's resolution.
        // GitHub #262 — write back into the image UNDER the edit, not merely
        // the largest one on the page. Where a small image is drawn over a large
        // one, writing to the large one succeeds and is then painted over: the
        // edit is real, committed, and invisible.
        const auto target = pImpl_->resolveWriteTarget(page, document, selRectFitz);
        if (!target.resolved) {
            result.errorMessage = "No image XObject found for the edited region";
            return result;
        }

        QImage finalImage = pImpl_->buildWriteBackImage(
            page, document, pageIndex, compositedImage, renderBbox, target);
        if (finalImage.isNull()) {
            finalImage = compositedImage;  // nothing composable -> historical path
        }

        // Capture the target's own pixels and the region as rendered, BEFORE the
        // write, so "did anything change, and could the user see it?" can be
        // answered rather than assumed.
        // GitHub #262 -- refuse an edit that cannot be seen, BEFORE writing it.
        if (pImpl_->targetIsOccludedAt(page, document, target.info, selRectFitz)) {
            result.errorMessage =
                "Recomposite would commit no visible change to the edited region "
                "(target XObject '" + target.info.xobjectName +
                "' is drawn beneath another image that covers this region)";
            qWarning("ImageReplacer: %s", result.errorMessage.c_str());
            return result;  // success stays false -> caller publishes FAILED
        }

        bool replaceSuccess = pImpl_->contentWriter_->replacePageImage(
            page, document, finalImage, target.info);

        if (!replaceSuccess) {
            result.errorMessage = "Failed to replace page image in PDF";
            return result;
        }

        // The write reached the PDF. Whether the USER can see it is a separate
        // question, and the one #262 turns on: the old code answered only the
        // first and published RECOMPOSITE_DONE regardless.
        // Populate successful result
        result.success = true;
        result.region.id = regionId;
        result.region.pdfBbox = selectionPdf;
        result.region.pixelBbox = renderBbox;
        result.region.approach = Approach::C_ImageReplacement;
        result.region.originalText = regionData.plainText;
        result.region.editedText = textToRender;
        result.region.fontMatch = fontMatch;
        result.region.bgInfo = bgInfo;

        // Approach C doesn't use quality gates, so set a neutral verdict
        result.qualityVerdict.recommendedApproach = Approach::C_ImageReplacement;
        result.qualityVerdict.overallScore = 1.0f; // Always "acceptable" for fallback
        result.fontResourceName.clear(); // Not applicable for image replacement

    } catch (const std::exception& e) {
        result.errorMessage = std::string("Exception during image replacement: ") + e.what();
        return result;
    } catch (...) {
        result.errorMessage = "Unknown exception during image replacement";
        return result;
    }

    return result;
}

ReconstructionResult ImageReplacer::replaceWithFontMatch(
    pdfe::core::Page* page,
    pdfe::core::Document* document,
    int pageIndex,
    const PdfRect& selectionPdf,
    const std::string& editedText,
    const FontMatch& fontMatch,
    const BackgroundInfo& bgInfo,
    RegionId regionId,
    const std::vector<FontRun>& fontRuns,
    const std::string& originalText) {

    ReconstructionResult result;
    result.success = false;

    // Validate inputs
    if (!page || !document) {
        result.errorMessage = "Null page or document pointer";
        return result;
    }

    if (pageIndex < 0 || pageIndex >= document->pageCount()) {
        result.errorMessage = "Page index out of range";
        return result;
    }

    if (editedText.empty()) {
        result.errorMessage = "Empty edited text in replaceWithFontMatch";
        return result;
    }

    try {
        // A3: compute working DPI before rendering so the page image
        // matches the source image's native DPI.
        int ocrDpi = pImpl_->ocrPipeline_->config().ocrDpi;
        // Defect #2: per-region native DPI (see replace() above).
        const QRectF selRectFitz(selectionPdf.x0, selectionPdf.y0,
                                 selectionPdf.x1 - selectionPdf.x0,
                                 selectionPdf.y1 - selectionPdf.y0);
        double sourceDpi = page->sourceImageDpiAt(selRectFitz);
        int workingDpi = computeWorkingDpi(sourceDpi, ocrDpi);
        double dpiScale = static_cast<double>(workingDpi) / static_cast<double>(ocrDpi);
        qDebug() << "[ImageReplacer::replaceWithFontMatch] A3 working DPI:"
                 << "ocrDpi=" << ocrDpi << "sourceDpi=" << sourceDpi
                 << "workingDpi=" << workingDpi << "scale=" << dpiScale;

        // Still need page image for compositing — at the working DPI.
        QImage pageImage = pImpl_->ocrPipeline_->renderPageAtDpi(pageIndex, workingDpi);
        if (pageImage.isNull()) {
            result.errorMessage = "Failed to render page image";
            return result;
        }

        // Convert PDF selection to pixel coordinates for dimensions (ocrDpi),
        // then rescale into workingDpi pixel space to match pageImage.
        const AffineTransform& transform = pImpl_->coordMapper_->getTransform(pageIndex);
        PixelRect pixelSelection = pImpl_->coordMapper_->pdfRectToPixel(selectionPdf, transform);
        if (dpiScale != 1.0) {
            scalePixelRect(pixelSelection, dpiScale);
        }

        int basePixelWidth = std::max(1, static_cast<int>(pixelSelection.width()));
        int pixelHeight = std::max(1, static_cast<int>(pixelSelection.height()));

        // Step 5: Convert text to UTF-32 and render to bitmap
        std::u32string textU32 = utf8ToU32(editedText);
        int dpi = workingDpi;

        // B2: use measured advance widths instead of character-count ratio.
        int pixelWidth = computeExpandedWidthMeasured(
            basePixelWidth, editedText, originalText,
            pImpl_->bitmapCompositor_, fontMatch, dpi);

        // Cap font size to fit within bitmap height (same guard as replace())
        FontMatch cappedMatch = fontMatch;
        float maxFontPt2 = static_cast<float>(pixelHeight) * 72.0f
                           / static_cast<float>(dpi) / 1.2f;
        if (cappedMatch.sizePt > maxFontPt2 && maxFontPt2 > 0.0f) {
            cappedMatch.sizePt = maxFontPt2;
        }

        QImage textBitmap;
        if (!fontRuns.empty()) {
            textBitmap = pImpl_->bitmapCompositor_->renderMultiFontText(
                textU32, cappedMatch, fontRuns, pixelWidth, pixelHeight, dpi);
        } else {
            textBitmap = pImpl_->bitmapCompositor_->renderText(
                textU32, cappedMatch, pixelWidth, pixelHeight, dpi);
        }

        if (textBitmap.isNull()) {
            result.errorMessage = "Failed to render text bitmap";
            return result;
        }

        // Same fit-to-ink decision as replace(). This path has to be fixed
        // too: it is the one taken when the caller already knows the font, so
        // leaving it alone would fix the defect on one route and leave the
        // other burning the whole selection box.
        PixelRect compositeBbox = pixelSelection;
        {
            const int padPx = std::max(1, dpi / 150);
            FillRegion fill = computeFillRegion(pixelSelection, textBitmap,
                                                originalText, padPx);
            if (fill.shrunkToInk) {
                qDebug() << "[ImageReplacer::replaceWithFontMatch]"
                         << "add-over-blank: fitting fill to ink"
                         << pixelSelection.width() << "x" << pixelSelection.height()
                         << "->" << fill.bbox.width() << "x" << fill.bbox.height();
                textBitmap = textBitmap.copy(fill.bitmapCrop);
                compositeBbox = fill.bbox;
            }
        }

        // Step 6: Composite text bitmap over cleaned background.
        // B3: pass full bgInfo so ImageMasker can use pattern-copy
        // for halftone/textured backgrounds instead of solid fill.
        QImage compositedImage = pImpl_->imageMasker_->compositeTextBitmap(
            pageImage, textBitmap, compositeBbox, bgInfo);

        if (compositedImage.isNull()) {
            result.errorMessage = "Failed to composite text bitmap";
            return result;
        }

        // Step 7: Replace page image (preserve quality using cached original)
        //
        // GitHub #262 — identical targeting and visibility rules to replace().
        // These two sites had the same write-back code duplicated, and fixing
        // only one would have reopened the defect through the other door. The
        // arithmetic now lives in ONE helper so they cannot drift apart again.
        // `compositeBbox` rather than the raw selection: writing back the raw
        // box would drag the untouched remainder through the resize and undo
        // the fit computed above.
        const auto target = pImpl_->resolveWriteTarget(page, document, selRectFitz);
        if (!target.resolved) {
            result.errorMessage = "No image XObject found for the edited region";
            return result;
        }

        QImage finalImage = pImpl_->buildWriteBackImage(
            page, document, pageIndex, compositedImage, compositeBbox, target);
        if (finalImage.isNull()) {
            finalImage = compositedImage;  // nothing composable -> historical path
        }

        // GitHub #262 -- refuse an edit that cannot be seen, BEFORE writing it.
        if (pImpl_->targetIsOccludedAt(page, document, target.info, selRectFitz)) {
            result.errorMessage =
                "Recomposite would commit no visible change to the edited region "
                "(target XObject '" + target.info.xobjectName +
                "' is drawn beneath another image that covers this region)";
            qWarning("ImageReplacer: %s", result.errorMessage.c_str());
            return result;  // success stays false -> caller publishes FAILED
        }

        bool replaceSuccess = pImpl_->contentWriter_->replacePageImage(
            page, document, finalImage, target.info);

        if (!replaceSuccess) {
            result.errorMessage = "Failed to replace page image in PDF";
            return result;
        }

        // Populate successful result
        result.success = true;
        result.region.id = regionId;
        result.region.pdfBbox = selectionPdf;
        result.region.pixelBbox = compositeBbox;  // what was actually rewritten
        result.region.approach = Approach::C_ImageReplacement;
        result.region.originalText.clear(); // Not available in this path
        result.region.editedText = editedText;
        result.region.fontMatch = fontMatch;
        result.region.bgInfo = bgInfo;

        // Approach C doesn't use quality gates
        result.qualityVerdict.recommendedApproach = Approach::C_ImageReplacement;
        result.qualityVerdict.overallScore = 1.0f;
        result.fontResourceName.clear();

    } catch (const std::exception& e) {
        result.errorMessage = std::string("Exception during image replacement: ") + e.what();
        return result;
    } catch (...) {
        result.errorMessage = "Unknown exception during image replacement";
        return result;
    }

    return result;
}

} // namespace pdfe::tre
