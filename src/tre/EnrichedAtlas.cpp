#include "core/TempFileSafety.h"
#include "tre/EnrichedAtlas.h"
#include "tre/BitmapCompositor.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QPainter>
#include <QTextStream>
#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace pdfe::tre {

namespace {

/// Crop an ARGB32 image to its visible (alpha > 0) bounding box.
QImage cropToContent(const QImage& img) {
    if (img.isNull())
        return {};
    QImage src = img.format() == QImage::Format_ARGB32_Premultiplied
                     ? img : img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    int top = src.height(), bottom = -1, left = src.width(), right = -1;
    for (int y = 0; y < src.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        for (int x = 0; x < src.width(); ++x) {
            if (qAlpha(line[x]) > 0) {
                if (y < top) top = y;
                if (y > bottom) bottom = y;
                if (x < left) left = x;
                if (x > right) right = x;
            }
        }
    }
    if (top > bottom || left > right)
        return {};
    return src.copy(left, top, right - left + 1, bottom - top + 1);
}

/// Find top-most and bottom-most rows with visible alpha (> threshold).
/// Returns {topRow, bottomRow} or {-1, -1} if no ink found.
std::pair<int, int> findAlphaInkBounds(const QImage& img, int alphaThreshold = 10) {
    if (img.isNull())
        return {-1, -1};
    QImage src = img.format() == QImage::Format_ARGB32_Premultiplied
                     ? img : img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    int top = -1, bottom = -1;
    for (int y = 0; y < src.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        for (int x = 0; x < src.width(); ++x) {
            if (qAlpha(row[x]) > alphaThreshold) {
                if (top < 0) top = y;
                bottom = y;
                break;
            }
        }
    }
    return {top, bottom};
}

} // anonymous namespace

void EnrichedAtlas::setFreeTypeFallback(BitmapCompositor* compositor,
                                         const FontMatch& fontMatch, int dpi)
{
    ftCompositor_ = compositor;
    ftFontMatch_ = fontMatch;
    ftDpi_ = dpi;
    ftCache_.clear();
    ftMetricsValid_ = false;  // Re-compute metrics for new font/size/DPI

    if (compositor && atlas_.size() > 0) {
        scanStyle_.calibrate(atlas_);
    }

    qDebug() << "[EnrichedAtlas] FreeType fallback configured:"
             << "font=" << QString::fromStdString(fontMatch.fontFilePath)
             << "sizePt=" << fontMatch.sizePt
             << "dpi=" << dpi
             << "scanStyle.valid=" << scanStyle_.style().valid;
}

void EnrichedAtlas::setFreeTypeOutputScale(double scale) const
{
    // Reject nonsense rather than silently rendering a 1-pixel or 40x glyph.
    // The clamp bounds are deliberately wide: a mixed-DPI page can legitimately
    // ask for 0.5 (300 -> 150) or 2.0 (150 -> 300), and nothing sane asks for
    // more than an order of magnitude either way.
    if (!(scale > 0.0) || !std::isfinite(scale))
        scale = 1.0;
    scale = std::clamp(scale, 0.1, 10.0);

    if (quantiseScale(scale) == quantiseScale(ftOutputScale_))
        return;  // no-op; do NOT invalidate metrics for a non-change

    ftOutputScale_ = scale;

    // Metrics (ftCanvasH_, ftBaseline_, ftCapH_) are measured from a reference
    // 'H' rendered at the EFFECTIVE dpi, so they are scale-specific and must be
    // recomputed. The glyph cache is NOT cleared: it is keyed by
    // (codepoint, scale), so entries for the other scale stay valid and a page
    // that alternates between two region DPIs re-renders each glyph once per
    // scale rather than once per switch.
    ftMetricsValid_ = false;

    qDebug() << "[EnrichedAtlas] FreeType output scale ->" << scale
             << "(effective dpi" << effectiveFtDpi() << "); cached glyphs kept:"
             << ftCache_.size();
}

int EnrichedAtlas::effectiveFtDpi() const
{
    // GitHub #268. THIS is the fix: the fallback rasterises at the dpi the
    // output will actually be shown at, instead of rasterising at the scan dpi
    // and letting the compositor resample. BitmapCompositor::renderText sizes
    // the glyph from (sizePt, dpi) -- the canvas height only bounds it -- so
    // scaling the dpi is what changes the rasterisation grid.
    const int d = static_cast<int>(std::lround(ftDpi_ * ftOutputScale_));
    return std::max(1, d);
}

void EnrichedAtlas::calibrateStyleFrom(const GlyphAtlas& referenceAtlas)
{
    scanStyle_.calibrate(referenceAtlas);
    ftCache_.clear();
    qDebug() << "[EnrichedAtlas] calibrateStyleFrom: valid=" << scanStyle_.style().valid
             << "sampleCount=" << scanStyle_.style().sampleCount;
}

void EnrichedAtlas::setReferenceMetrics(int lineHeight, int baselineFromBottom)
{
    refLineH_ = std::max(0, lineHeight);
    refBaselineFromBottom_ = std::max(0, baselineFromBottom);
    // Note: refInkHeight_ stays 0 (no atlas glyphs to measure from).
    // FreeType ink-height scaling is disabled for FreeType-only atlases.
    ftCache_.clear();  // Invalidate cached glyphs rendered with old dimensions
    ftMetricsValid_ = false;  // Re-compute window for new refLineH_
    qDebug() << "[EnrichedAtlas] setReferenceMetrics: lineH=" << refLineH_
             << "baselineFromBottom=" << refBaselineFromBottom_;
}

void EnrichedAtlas::build(GlyphAtlas atlas, const std::u32string& presynthText)
{
    atlas_ = std::move(atlas);
    synthesized_.clear();

    // Determine reference line height: the most common glyph height.
    // All reference line glyphs share the same lineH. Donor glyphs may differ,
    // but reference line glyphs are the majority, so the mode = reference lineH.
    refLineH_ = 0;
    {
        std::map<int, int> heightCounts;
        for (char32_t cp : atlas_.codepoints()) {
            QImage g = atlas_.glyph(cp);
            if (!g.isNull())
                heightCounts[g.height()]++;
        }
        int maxCount = 0;
        for (const auto& [h, cnt] : heightCounts) {
            if (cnt > maxCount) {
                maxCount = cnt;
                refLineH_ = h;
            }
        }
        qDebug() << "[EnrichedAtlas::build] referenceLineHeight=" << refLineH_
                 << "from" << atlas_.size() << "glyphs";
        // Log individual glyph heights for debugging baseline alignment
        for (char32_t cp : atlas_.codepoints()) {
            QImage g = atlas_.glyph(cp);
            if (!g.isNull()) {
                qDebug() << "[EnrichedAtlas::build] glyph U+"
                         << QString::number(cp, 16)
                         << "'" << QString::fromUcs4(&cp, 1) << "'"
                         << "h=" << g.height() << "w=" << g.width();
            }
        }
    }

    // Use fallback baseline for the initial normalization pass.
    // The real baseline is computed AFTER normalization when all glyphs are
    // at refLineH_, since pre-normalization glyphs have varying heights and
    // few match refLineH_ exactly (causing refBaselineFromBottom_=0).
    refBaselineFromBottom_ = refLineH_ > 1 ? std::max(1, refLineH_ / 6) : 0;

    // Normalize all atlas glyphs to refLineH_ for consistent baseline alignment.
    // Donor line glyphs may have different heights (33px vs 40px etc.) from
    // different line bbox sizes. Without normalization, grid rendering assumes
    // all glyphs fit refLineH_ but they don't, causing baseline drift.
    if (refLineH_ > 1) {
        int normalizedCount = 0;
        for (char32_t cp : atlas_.codepoints()) {
            QImage g = atlas_.glyph(cp);
            if (g.isNull() || g.height() == refLineH_)
                continue;
            QImage norm = normalizeHeight(g);
            if (!norm.isNull()) {
                atlas_.replaceGlyphImage(cp, std::move(norm));
                ++normalizedCount;
            }
        }
        if (normalizedCount > 0)
            qDebug() << "[EnrichedAtlas::build] Normalized" << normalizedCount
                     << "glyphs to height" << refLineH_;
    }

    // Now compute baseline from the NORMALIZED glyphs (all at refLineH_).
    // This must happen after normalization because pre-normalization glyphs
    // have varying heights from different HOCR line bboxes, and very few
    // match refLineH_ exactly — causing the baseline mode to be 0.
    if (refLineH_ > 1) {
        std::map<int, int> baselineHist;
        for (char32_t cp : atlas_.codepoints()) {
            QImage g = atlas_.glyph(cp);
            if (g.isNull() || g.height() != refLineH_)
                continue;
            // Find the bottom-most visible row
            for (int y = g.height() - 1; y >= 0; --y) {
                const QRgb* row = reinterpret_cast<const QRgb*>(g.constScanLine(y));
                bool hasVisible = false;
                for (int x = 0; x < g.width(); ++x) {
                    if (qAlpha(row[x]) > 20) { hasVisible = true; break; }
                }
                if (hasVisible) {
                    baselineHist[g.height() - 1 - y]++;
                    break;
                }
            }
        }
        // Find mode, EXCLUDING fromBottom=0. Glyphs with ink at the very
        // bottom row (fromBottom=0) are tight-cropped chars that weren't
        // normalized (already at refLineH_). Their ink extends to the bottom
        // edge because the HOCR bbox was large enough to match refLineH_.
        // The TRUE baseline is revealed by NORMALIZED glyphs (originally
        // shorter, padded by normalizeHeight), which cluster at fromBottom≈5-6.
        int maxBLCount = 0;
        for (const auto& [fb, cnt] : baselineHist) {
            if (fb == 0)
                continue;  // skip tight-crop noise
            if (cnt > maxBLCount) {
                maxBLCount = cnt;
                refBaselineFromBottom_ = fb;
            }
        }
        // If no non-zero entries, fall back to initial estimate
        if (maxBLCount == 0)
            refBaselineFromBottom_ = refLineH_ > 1 ? std::max(1, refLineH_ / 6) : 0;
        qDebug() << "[EnrichedAtlas::build] refBaselineFromBottom="
                 << refBaselineFromBottom_ << "from" << baselineHist.size()
                 << "normalized glyphs";

        // File-based logging: dump baseline histogram and per-glyph ink bounds
        {
            // WIN-87 wider class (phase-60): under the per-process unpredictable
            // diagnostics root, not a fixed %TEMP% name anyone can pre-create.
            // An empty path means unavailable -- skip the dump rather than
            // falling back to a guessable one (or, as a bare concatenation
            // would, writing to the volume root).
            const QString dir = QString::fromStdWString(
                pdfe::core::diagnosticsSubdir("pdfe_glyph_debug").wstring());
            QFile logFile(dir.isEmpty() ? QString() : dir + "/baseline_hist.txt");
            if (logFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream ts(&logFile);
                ts << "=== Baseline histogram (fromBottom -> count) ===\n";
                for (const auto& [fb, cnt] : baselineHist) {
                    ts << "  fromBottom=" << fb << " count=" << cnt
                       << (fb == refBaselineFromBottom_ ? " <-- MODE" : "") << "\n";
                }
                ts << "\n=== Per-glyph ink bounds (after normalization) ===\n";
                for (char32_t cp : atlas_.codepoints()) {
                    QImage g = atlas_.glyph(cp);
                    if (g.isNull() || g.height() != refLineH_)
                        continue;
                    auto [inkTop, inkBot] = findAlphaInkBounds(g);
                    ts << "  U+" << QString::number(cp, 16)
                       << " '" << QString::fromUcs4(&cp, 1) << "'"
                       << " h=" << g.height() << " w=" << g.width()
                       << " inkTop=" << inkTop << " inkBot=" << inkBot
                       << " fromBottom=" << (inkBot >= 0 ? g.height() - 1 - inkBot : -1)
                       << "\n";
                }
                ts << "\nrefLineH=" << refLineH_
                   << " refBaselineFromBottom=" << refBaselineFromBottom_
                   << " refInkHeight=" << refInkHeight_ << "\n";
            }
        }
    }

    // Compute average ink height of non-descender reference-height glyphs.
    refInkHeight_ = 0;
    if (refLineH_ > 1) {
        int totalInkH = 0, inkHCount = 0;
        for (char32_t cp : atlas_.codepoints()) {
            QImage g = atlas_.glyph(cp);
            if (g.isNull() || g.height() != refLineH_)
                continue;
            int topVis = g.height(), botVis = -1;
            for (int y = 0; y < g.height(); ++y) {
                const QRgb* row = reinterpret_cast<const QRgb*>(g.constScanLine(y));
                for (int x = 0; x < g.width(); ++x) {
                    if (qAlpha(row[x]) > 20) {
                        topVis = std::min(topVis, y);
                        botVis = std::max(botVis, y);
                        break;
                    }
                }
            }
            if (topVis <= botVis) {
                int fromBottom = g.height() - 1 - botVis;
                // Non-descender: ink doesn't reach the last few rows
                if (fromBottom >= refBaselineFromBottom_ / 2) {
                    totalInkH += (botVis - topVis + 1);
                    ++inkHCount;
                }
            }
        }
        if (inkHCount > 0)
            refInkHeight_ = totalInkH / inkHCount;
        qDebug() << "[EnrichedAtlas::build] refInkHeight=" << refInkHeight_
                 << "from" << inkHCount << "non-descender glyphs";
    }

    // Profile the synthesizer from the atlas glyphs
    synthesizer_.buildProfile(atlas_);

    // Pre-synthesize missing characters for the given text
    if (!presynthText.empty()) {
        synthesized_ = synthesizer_.synthesizeAll(presynthText, atlas_);
        // Do NOT normalize synthesized glyphs. The synthesizer sets height
        // intentionally via ScaleToCapHeight/ScaleToXHeight transforms.
        // Normalizing would undo that (e.g., 'Z' from 'z' scaled back to 'z' height).
        if (!synthesized_.empty()) {
            qDebug() << "[EnrichedAtlas::build] pre-synthesized"
                     << synthesized_.size() << "glyphs (heights preserved)";
        }
    }
}


// Shared implementation for levels 1-3 (atlas + synthesis, no FreeType).
// Used by both glyph() and glyphFromScan() to avoid code duplication.
QImage EnrichedAtlas::glyphFromScanImpl(char32_t codepoint) const
{
    // 1. Atlas (highest fidelity -- cropped from scan)
    QImage img = atlas_.glyph(codepoint);
    if (!img.isNull())
        return img;

    // 2. Pre-synthesized or previously on-demand synthesized
    auto it = synthesized_.find(codepoint);
    if (it != synthesized_.end())
        return it->second;

    // 3. On-demand synthesis (lazy, cached, height preserved)
    auto result = synthesizer_.synthesize(codepoint, atlas_);
    if (result.success && !result.image.isNull()) {
        auto [insertIt, _] = synthesized_.emplace(codepoint, std::move(result.image));
        return insertIt->second;
    }

    return {};
}


QImage EnrichedAtlas::glyph(char32_t codepoint, GlyphSource* source) const
{
    // GitHub #268. Report WHICH level answered. A compositor that rescales
    // atlas glyphs from scan space into composite space must not also rescale a
    // FreeType glyph that setFreeTypeOutputScale() already produced at
    // composite scale, and the QImage carries no trace of its origin.
    if (source) *source = GlyphSource::None;

    // Levels 1-3: atlas + synthesis
    QImage img = glyphFromScanImpl(codepoint);
    if (!img.isNull()) {
        if (source) {
            *source = atlas_.contains(codepoint) ? GlyphSource::Atlas
                                                 : GlyphSource::Synthesized;
        }
        // Log which level provided the glyph (atlas vs synthesis)
        bool fromAtlas = atlas_.contains(codepoint);
        bool fromPreSynth = !fromAtlas && synthesized_.count(codepoint) > 0;
        // File-based log for first few calls
        static int scanLogCount = 0;
        if (scanLogCount < 50) {
            ++scanLogCount;
            // WIN-87 wider class (phase-60): under the per-process unpredictable
            // diagnostics root, not a fixed %TEMP% name anyone can pre-create.
            // An empty path means unavailable -- skip the dump rather than
            // falling back to a guessable one (or, as a bare concatenation
            // would, writing to the volume root).
            const QString dir = QString::fromStdWString(
                pdfe::core::diagnosticsSubdir("pdfe_glyph_debug").wstring());
            QFile logFile(dir.isEmpty() ? QString() : dir + "/ft_glyphs.txt");
            if (logFile.open(QIODevice::Append | QIODevice::Text)) {
                QTextStream ts(&logFile);
                // GitHub #266. Alpha stats, not just dimensions. The size of a
                // glyph says nothing about whether it is a LETTER: a uniform
                // mask and a crisp 'T' have identical bounds, and the log of
                // dimensions alone was read for a whole investigation as
                // evidence the glyph was fine. A near-constant alpha with a
                // near-100% coverage fraction is the signature of an isolation
                // that failed open -- every pixel scored as text.
                int nonZero = 0, aMin = 255, aMax = 0;
                long long aSum = 0;
                const QImage probe = img.convertToFormat(QImage::Format_ARGB32);
                for (int yy = 0; yy < probe.height(); ++yy) {
                    const QRgb* row =
                        reinterpret_cast<const QRgb*>(probe.constScanLine(yy));
                    for (int xx = 0; xx < probe.width(); ++xx) {
                        const int a = qAlpha(row[xx]);
                        if (a > 0) ++nonZero;
                        aMin = std::min(aMin, a);
                        aMax = std::max(aMax, a);
                        aSum += a;
                    }
                }
                const int total = std::max(1, probe.width() * probe.height());
                ts << "U+" << QString::number(codepoint, 16)
                   << " '" << QString::fromUcs4(&codepoint, 1) << "'"
                   << " from=" << (fromAtlas ? "atlas" : fromPreSynth ? "pre-synth" : "on-demand-synth")
                   << " h=" << img.height() << " w=" << img.width()
                   << " inkFrac=" << QString::number(100.0 * nonZero / total, 'f', 1) << "%"
                   << " alphaMin=" << aMin << " alphaMax=" << aMax
                   << " alphaMean=" << QString::number(double(aSum) / total, 'f', 1)
                   << "\n";
            }
            // The image itself, once per codepoint. Reading the pixels beat
            // three rounds of inference about them.
            if (!dir.isEmpty()) {
                const QString png =
                    dir + "/glyph_" + (fromAtlas ? "atlas" : "synth") + "_U+" +
                    QString::number(codepoint, 16) + ".png";
                if (!QFile::exists(png)) img.save(png);
            }
        }
        return img;
    }

    // 4. FreeType fallback with baseline-anchored rendering.
    //
    // Approach (same concept as AtlasComparisonDialog::renderFontGlyphWithScanStyle):
    //   a) Render reference 'H' to find the font's baseline and cap-height (once, cached).
    //   b) Render the target character on the same canvas.
    //   c) Crop to ink content (tight crop).
    //   d) Scale by atlasCapH / ftCapH so character size matches atlas.
    //   e) Place on a refLineH_ canvas at the correct baseline-aligned Y position.
    //   f) Apply ScanStyleTransfer for scan appearance.
    //
    // This approach separates sizing (step d) from positioning (step e), avoiding
    // the window-proportion issues that caused size mismatches.
    if (ftCompositor_) {
        // GitHub #268: keyed by (codepoint, output scale). A mixed-DPI page
        // asks the same atlas for the same character at two different target
        // scales, and one image per codepoint cannot hold both.
        const FtKey ftKey{codepoint, quantiseScale(ftOutputScale_)};
        auto ftIt = ftCache_.find(ftKey);
        if (ftIt != ftCache_.end()) {
            if (source) *source = GlyphSource::FreeType;
            return ftIt->second;
        }

        // Compute font metrics on first call (renders reference 'H' once)
        if (!ftMetricsValid_)
            ensureFreeTypeMetrics();

        std::u32string ch(1, codepoint);
        int charW = std::max(20, ftCanvasH_ * 3);

        // GitHub #268: effectiveFtDpi(), not ftDpi_. At scale 1.0 these are the
        // same value and this is the historic path byte for byte.
        QImage rendered = ftCompositor_->renderText(
            ch, ftFontMatch_.fontFilePath, ftFontMatch_.sizePt,
            ftFontMatch_.color, charW, ftCanvasH_, effectiveFtDpi());

        bool placed = false;
        if (!rendered.isNull() && ftBaseline_ >= 0 && ftCapH_ > 0 && atlasCapH_ > 0) {
            // Crop-scale-position approach: crop to ink, scale by cap-height
            // ratio, then position on a refLineH_ canvas aligned to the atlas
            // baseline row (derived from refBaselineFromBottom_).
            auto [charTop, charBot] = findAlphaInkBounds(rendered);
            if (charTop >= 0 && charBot > charTop) {
                int distAboveBaseline = ftBaseline_ - charTop;

                QImage cropped = cropToContent(rendered);
                if (!cropped.isNull()) {
                    // GitHub #268. The cap-match ratio is SCALE-INVARIANT and
                    // deliberately so: ftCapH_ is measured at effectiveFtDpi(),
                    // so it already grew by the output scale, and multiplying
                    // the atlas target by the same scale cancels it. The ratio
                    // therefore equals its historic value and the clamp below
                    // keeps its original meaning instead of clipping at S<1.
                    const double S = ftOutputScale_;
                    float scale = static_cast<float>(
                        (static_cast<double>(atlasCapH_) * S)
                        / static_cast<double>(ftCapH_));
                    scale = std::clamp(scale, 0.3f, 3.0f);
                    int scaledH = std::max(1, static_cast<int>(cropped.height() * scale));
                    int scaledW = std::max(1, static_cast<int>(cropped.width() * scale));
                    QImage scaled = cropped.scaled(scaledW, scaledH,
                                                   Qt::IgnoreAspectRatio,
                                                   Qt::SmoothTransformation);

                    QImage styled = scanStyle_.apply(scaled);

                    // Place on a canvas at the OUTPUT line height. At S == 1.0
                    // this is refLineH_ and every expression below reduces to
                    // the original.
                    const int outLineH = std::max(
                        1, static_cast<int>(std::lround(refLineH_ * S)));
                    const int outBaselineFromBottom = static_cast<int>(
                        std::lround(refBaselineFromBottom_ * S));
                    int atlasBaselineRow = outLineH - 1 - outBaselineFromBottom;
                    int drawY = atlasBaselineRow - static_cast<int>(distAboveBaseline * scale);

                    QImage canvas(styled.width(), outLineH, QImage::Format_ARGB32_Premultiplied);
                    canvas.fill(Qt::transparent);

                    int srcStartY = 0;
                    if (drawY < 0) {
                        srcStartY = -drawY;
                        drawY = 0;
                    }
                    int copyH = std::min(styled.height() - srcStartY, outLineH - drawY);
                    // (drawY/copyH are now in output space; see outLineH above.)
                    if (copyH > 0 && styled.width() > 0) {
                        QPainter painter(&canvas);
                        painter.setCompositionMode(QPainter::CompositionMode_Source);
                        painter.drawImage(0, drawY, styled, 0, srcStartY, styled.width(), copyH);
                        painter.end();
                    }

                    auto [it, _] = ftCache_.emplace(ftKey, std::move(canvas));
                    if (source) *source = GlyphSource::FreeType;
                    // (No `placed = true;` here — the return at the
                    //  end of this branch makes the fallback path at
                    //  line 444 unreachable.)

                    // File-based logging
                    {
                        // WIN-87 wider class (phase-60): under the per-process unpredictable
                        // diagnostics root, not a fixed %TEMP% name anyone can pre-create.
                        // An empty path means unavailable -- skip the dump rather than
                        // falling back to a guessable one (or, as a bare concatenation
                        // would, writing to the volume root).
                        const QString dir = QString::fromStdWString(
                            pdfe::core::diagnosticsSubdir("pdfe_glyph_debug").wstring());
                        QFile logFile(dir.isEmpty() ? QString() : dir + "/ft_glyphs.txt");
                        if (logFile.open(QIODevice::Append | QIODevice::Text)) {
                            QTextStream ts(&logFile);
                            ts << "U+" << QString::number(codepoint, 16)
                               << " '" << QString::fromUcs4(&codepoint, 1) << "'"
                               << " scale=" << scale
                               << " croppedH=" << cropped.height()
                               << " scaledH=" << scaledH
                               << " distAbove=" << distAboveBaseline
                               << " atlasBaselineRow=" << atlasBaselineRow
                               << " drawY=" << drawY
                               << " refBLBottom=" << refBaselineFromBottom_
                               << " resultH=" << it->second.height()
                               << "\n";
                        }
                    }
                    return it->second;
                }
            }
        }

        // Fallback: if baseline metrics unavailable, use cropToContent + normalizeHeight
        if (!placed && !rendered.isNull()) {
            QImage cropped = cropToContent(rendered);
            if (!cropped.isNull()) {
                QImage styled = scanStyle_.apply(cropped);
                QImage result = normalizeHeight(styled);
                auto [it, _] = ftCache_.emplace(ftKey, std::move(result));
                if (source) *source = GlyphSource::FreeType;
                return it->second;
            }
        }
    }

    // 5. Not available -- caller falls back to QFont/system fonts
    return {};
}


QImage EnrichedAtlas::glyphFromScan(char32_t codepoint) const
{
    return glyphFromScanImpl(codepoint);
}


bool EnrichedAtlas::contains(char32_t codepoint) const
{
    if (atlas_.contains(codepoint))
        return true;
    if (synthesized_.count(codepoint))
        return true;
    // GitHub #268: the key now carries an output scale, so "is this codepoint
    // cached" is a question about any scale, not one lookup.
    for (const auto& [k, _] : ftCache_) {
        if (k.cp == codepoint)
            return true;
    }
    // Don't try on-demand synthesis or FreeType for contains() -- too expensive for iteration
    return false;
}


int EnrichedAtlas::size() const
{
    // These sets are disjoint by construction (glyph() checks atlas first,
    // then synthesized_, then ftCache_), so simple addition is correct ACROSS
    // the three levels.
    //
    // GitHub #268: within ftCache_ it no longer is. The key is now
    // (codepoint, output scale), so one character cached at two target scales
    // occupies two entries — and on a mixed-DPI page that is the normal case,
    // not an edge one. Counting entries would report a glyph count that rises
    // when a second region is composited at a different DPI, which is a
    // property of the cache rather than of the atlas's coverage. Count
    // DISTINCT codepoints instead, which is what every caller of size() means.
    std::set<char32_t> ftUnique;
    for (const auto& [k, _] : ftCache_)
        ftUnique.insert(k.cp);
    return atlas_.size() + static_cast<int>(synthesized_.size())
           + static_cast<int>(ftUnique.size());
}


int EnrichedAtlas::scanSize() const
{
    // Atlas + synthesized only (no FreeType cache).
    return atlas_.size() + static_cast<int>(synthesized_.size());
}


float EnrichedAtlas::coverage(const std::u32string& text) const
{
    if (text.empty())
        return 1.0f;

    std::set<char32_t> unique;
    for (char32_t c : text) {
        if (c != U' ' && c != U'\t' && c != U'\n' && c != U'\r')
            unique.insert(c);
    }

    if (unique.empty())
        return 1.0f;

    // Check atlas and pre-synthesized cache (no on-demand)
    const int found = static_cast<int>(std::count_if(
        unique.begin(), unique.end(),
        [this](char32_t c) {
            return atlas_.contains(c) || synthesized_.count(c);
        }));

    return static_cast<float>(found) / static_cast<float>(unique.size());
}


std::vector<char32_t> EnrichedAtlas::codepoints() const
{
    // These sets are disjoint by construction, but use a set for safety
    std::set<char32_t> unique;
    for (char32_t cp : atlas_.codepoints())
        unique.insert(cp);
    for (const auto& [cp, _] : synthesized_)
        unique.insert(cp);
    for (const auto& [k, _] : ftCache_)
        unique.insert(k.cp);  // #268: key is (codepoint, scale)
    return {unique.begin(), unique.end()};
}

void EnrichedAtlas::ensureFreeTypeMetrics() const
{
    ftMetricsValid_ = true;  // Mark computed even if it fails, to avoid retrying
    ftCanvasH_ = std::max(60, refLineH_ * 4);

    if (!ftCompositor_)
        return;

    // Render a reference capital letter to find the font's baseline and cap-height.
    // Same approach as AtlasComparisonDialog::renderFontGlyphWithScanStyle().
    for (char32_t refCp : {U'H', U'M', U'N', U'W', U'E', U'T', U'B', U'D'}) {
        QImage refGlyph = ftCompositor_->renderText(
            std::u32string{refCp}, ftFontMatch_.fontFilePath, ftFontMatch_.sizePt,
            ftFontMatch_.color, ftCanvasH_ * 3, ftCanvasH_, effectiveFtDpi());
    // GitHub #268. effectiveFtDpi(), NOT ftDpi_, and this line is the whole
    // reason the cap-match ratio in glyph() is scale-invariant. Measuring the
    // reference 'H' at the unscaled dpi while rendering the glyph at the scaled
    // one makes ftCapH_ too large by 1/scale, so the ratio comes out scaled
    // twice: at scale 0.5 the glyph arrived 6 px wide where 12 was correct,
    // which is how this was found. setFreeTypeOutputScale() clears
    // ftMetricsValid_ precisely so this re-measures.
        if (refGlyph.isNull())
            continue;

        auto [rTop, rBot] = findAlphaInkBounds(refGlyph);
        if (rTop >= 0 && rBot > rTop) {
            ftBaseline_ = rBot;
            ftCapH_ = rBot - rTop + 1;
            break;
        }
    }

    if (ftCapH_ <= 0) {
        qDebug() << "[EnrichedAtlas] FreeType metrics: failed to find reference char baseline";
        return;
    }

    // Atlas cap-height: use refInkHeight_ (average ink height of non-descender
    // atlas glyphs). We cannot use findAlphaInkBounds on individual atlas glyphs
    // because reference-line glyphs are tight-cropped (ink fills entire refLineH_
    // image), so their "ink height" equals refLineH_ — which is the LINE height,
    // not the cap-height. refInkHeight_ correctly measures just the ink portion.
    atlasCapH_ = refInkHeight_;

    qDebug() << "[EnrichedAtlas] FreeType metrics:"
             << "ftBaseline=" << ftBaseline_ << "ftCapH=" << ftCapH_
             << "atlasCapH=" << atlasCapH_ << "refLineH=" << refLineH_
             << "refBLBottom=" << refBaselineFromBottom_;

    // File-based logging for diagnosis
    {
        // WIN-87 wider class (phase-60): under the per-process unpredictable
        // diagnostics root, not a fixed %TEMP% name anyone can pre-create.
        // An empty path means unavailable -- skip the dump rather than
        // falling back to a guessable one (or, as a bare concatenation
        // would, writing to the volume root).
        const QString dir = QString::fromStdWString(
            pdfe::core::diagnosticsSubdir("pdfe_glyph_debug").wstring());
        QFile logFile(dir.isEmpty() ? QString() : dir + "/ft_metrics.txt");
        if (logFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&logFile);
            ts << "=== ensureFreeTypeMetrics ===\n"
               << "ftCanvasH=" << ftCanvasH_ << "\n"
               << "ftBaseline=" << ftBaseline_ << "\n"
               << "ftCapH=" << ftCapH_ << "\n"
               << "atlasCapH=" << atlasCapH_ << "\n"
               << "refLineH=" << refLineH_ << "\n"
               << "refBaselineFromBottom=" << refBaselineFromBottom_ << "\n"
               << "refInkHeight=" << refInkHeight_ << "\n"
               << "scale(atlasCapH/ftCapH)=" << (static_cast<float>(atlasCapH_) / ftCapH_) << "\n"
               << "scanStyle.valid=" << scanStyle_.style().valid << "\n"
               << "scanStyle.blurSigma=" << scanStyle_.style().blurSigma << "\n"
               << "scanStyle.edgeRough=" << scanStyle_.style().edgeRoughness << "\n"
               << "scanStyle.stemW=" << scanStyle_.style().avgStemWidth << "\n"
               << "scanStyle.fgNoise=" << scanStyle_.style().fgNoiseLevel << "\n";
        }
    }
}


QImage EnrichedAtlas::normalizeHeight(const QImage& glyph) const
{
    if (glyph.isNull() || refLineH_ <= 1)
        return glyph;

    // Ensure consistent format for memcpy safety
    QImage src = glyph.format() == QImage::Format_ARGB32_Premultiplied
                     ? glyph
                     : glyph.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    if (src.height() == refLineH_)
        return src;

    // Taller than reference: scale down
    if (src.height() > refLineH_) {
        return src.scaledToHeight(refLineH_, Qt::SmoothTransformation);
    }

    // Shorter than reference: baseline-aligned padding.
    // Find the bottom-most visible row in the source glyph.
    int srcBaselineFromBottom = src.height() / 6;  // fallback
    for (int y = src.height() - 1; y >= 0; --y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        bool hasVisible = false;
        for (int x = 0; x < src.width(); ++x) {
            if (qAlpha(row[x]) > 20) { hasVisible = true; break; }
        }
        if (hasVisible) {
            srcBaselineFromBottom = src.height() - 1 - y;
            break;
        }
    }

    // Compute vertical offset to align baselines.
    // bottomPad aligns source baseline to reference baseline.
    int bottomPad = std::max(0, refBaselineFromBottom_ - srcBaselineFromBottom);
    int yOffset = refLineH_ - src.height() - bottomPad;
    yOffset = std::max(0, yOffset);

    QImage canvas(src.width(), refLineH_, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);

    for (int y = 0; y < src.height(); ++y) {
        int destY = yOffset + y;
        if (destY >= 0 && destY < refLineH_) {
            std::memcpy(canvas.scanLine(destY), src.constScanLine(y),
                        std::min(canvas.bytesPerLine(), src.bytesPerLine()));
        }
    }

    return canvas;
}

} // namespace pdfe::tre
