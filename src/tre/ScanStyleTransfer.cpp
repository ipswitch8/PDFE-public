#include "tre/ScanStyleTransfer.h"
#include "tre/GlyphAtlas.h"
#include "tre/GlyphSynthesizer.h"

#include <QDebug>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace pdfe::tre {

// ---------------------------------------------------------------------------
// LCG PRNG + Box-Muller for deterministic Gaussian noise
// ---------------------------------------------------------------------------

static uint32_t lcgNext(uint32_t& state)
{
    state = state * 1664525u + 1013904223u;
    return state;
}

static float lcgUniform01(uint32_t& state)
{
    return static_cast<float>(lcgNext(state) & 0x00FFFFFF) / 16777216.0f;
}

/// Box-Muller transform: two uniform → two Gaussian samples.
/// Returns one sample; the other is discarded for simplicity.
static float gaussianSample(uint32_t& state, float sigma)
{
    float u1 = std::max(1e-7f, lcgUniform01(state));
    float u2 = lcgUniform01(state);
    float z = std::sqrt(-2.0f * std::log(u1)) * std::cos(6.2831853f * u2);
    return z * sigma;
}

// ---------------------------------------------------------------------------
// Measurement utilities
// ---------------------------------------------------------------------------

float ScanStyleTransfer::measureEdgeRoughness(const QImage& glyph)
{
    if (glyph.isNull())
        return 0.0f;

    QImage img = glyph.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    std::vector<float> edgeAlphas;
    edgeAlphas.reserve(static_cast<size_t>(img.width() * img.height() / 4));

    for (int y = 0; y < img.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            int a = qAlpha(row[x]);
            if (a >= 20 && a <= 240)
                edgeAlphas.push_back(static_cast<float>(a));
        }
    }

    if (edgeAlphas.size() < 4)
        return 0.0f;

    float mean = std::accumulate(edgeAlphas.begin(), edgeAlphas.end(), 0.0f)
                 / static_cast<float>(edgeAlphas.size());
    float variance = std::accumulate(edgeAlphas.begin(), edgeAlphas.end(), 0.0f,
        [mean](float acc, float a) { return acc + (a - mean) * (a - mean); });
    variance /= static_cast<float>(edgeAlphas.size());

    return std::sqrt(variance);
}

float ScanStyleTransfer::measureForegroundNoise(const QImage& glyph)
{
    if (glyph.isNull())
        return 0.0f;

    QImage img = glyph.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    std::vector<float> rVals, gVals, bVals;
    rVals.reserve(static_cast<size_t>(img.width() * img.height() / 2));
    gVals.reserve(rVals.capacity());
    bVals.reserve(rVals.capacity());

    for (int y = 0; y < img.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            int a = qAlpha(row[x]);
            if (a <= 240)
                continue;
            // Un-premultiply to get actual RGB
            float af = static_cast<float>(a);
            rVals.push_back(static_cast<float>(qRed(row[x])) * 255.0f / af);
            gVals.push_back(static_cast<float>(qGreen(row[x])) * 255.0f / af);
            bVals.push_back(static_cast<float>(qBlue(row[x])) * 255.0f / af);
        }
    }

    if (rVals.size() < 4)
        return 0.0f;

    auto stddev = [](const std::vector<float>& v) {
        float mean = std::accumulate(v.begin(), v.end(), 0.0f) / static_cast<float>(v.size());
        float var = std::accumulate(v.begin(), v.end(), 0.0f,
            [mean](float acc, float x) { return acc + (x - mean) * (x - mean); });
        return std::sqrt(var / static_cast<float>(v.size()));
    };

    return (stddev(rVals) + stddev(gVals) + stddev(bVals)) / 3.0f;
}

static inline int luminance601(QRgb rgb) {
    // ITU-R BT.601 luminance from un-premultiplied RGB. Caller must
    // pass already un-premultiplied pixels or guarantee the alpha
    // is sufficiently opaque that premultiplication rounding is
    // negligible.
    return (299 * qRed(rgb) + 587 * qGreen(rgb) + 114 * qBlue(rgb)) / 1000;
}

float ScanStyleTransfer::measureEdgeRoughnessFromLuminance(const QImage& glyph)
{
    // Uses the alpha channel only as a region selector — pixels in
    // the band [20, 240] are where the glyph edge lives. Then
    // measures std-dev of RGB luminance at those pixels, which is
    // authentic scan content and not affected by C4's change to how
    // isolateAlpha() computes the mask.
    if (glyph.isNull()) return 0.0f;

    QImage img = glyph.convertToFormat(QImage::Format_ARGB32);
    std::vector<float> edgeLums;
    edgeLums.reserve(static_cast<size_t>(img.width() * img.height() / 4));

    for (int y = 0; y < img.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            int a = qAlpha(row[x]);
            if (a < 20 || a > 240) continue;
            // Un-premultiply is automatic because we converted to
            // non-premultiplied ARGB32 above.
            int lum = luminance601(row[x]);
            edgeLums.push_back(static_cast<float>(lum));
        }
    }

    if (edgeLums.size() < 4) return 0.0f;

    float mean = std::accumulate(edgeLums.begin(), edgeLums.end(), 0.0f)
                 / static_cast<float>(edgeLums.size());
    float variance = std::accumulate(edgeLums.begin(), edgeLums.end(), 0.0f,
        [mean](float acc, float l) { return acc + (l - mean) * (l - mean); });
    variance /= static_cast<float>(edgeLums.size());
    return std::sqrt(variance);
}

float ScanStyleTransfer::measureEdgeTransitionWidthFromLuminance(const QImage& glyph)
{
    // Scan horizontal rows for luminance transitions from bright
    // background (lum > 200) to dark ink (lum < 60) or the inverse.
    // Count the pixels between the two thresholds for each
    // transition. This measures the actual scan blur directly from
    // the RGB content and is unaffected by how the alpha mask was
    // computed.
    if (glyph.isNull() || glyph.width() < 4 || glyph.height() < 4)
        return 1.0f;

    QImage img = glyph.convertToFormat(QImage::Format_ARGB32);
    std::vector<float> widths;
    const int scanRows[] = {
        img.height() / 4, img.height() / 2, img.height() * 3 / 4
    };

    for (int y : scanRows) {
        if (y < 0 || y >= img.height()) continue;
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        int x = 0;
        while (x < img.width() - 1) {
            int lum = luminance601(row[x]);
            // Entering a dark region (bright > 200 → next < bright).
            if (lum > 200 && luminance601(row[x + 1]) <= 200) {
                int start = x + 1;
                int end = start;
                // Count intermediate pixels where luminance is
                // between the two thresholds.
                while (end < img.width() && luminance601(row[end]) >= 60
                       && luminance601(row[end]) <= 200) {
                    ++end;
                }
                if (end < img.width() && luminance601(row[end]) < 60) {
                    // Reached the dark plateau. Width is the count
                    // of intermediate pixels. Crisp (end==start)
                    // transitions are recorded as width 1 — a
                    // conservative minimum that still signals a
                    // transition happened.
                    float width = static_cast<float>(
                        std::max(1, end - start));
                    widths.push_back(width);
                }
                x = end;
                continue;
            }
            // Exiting a dark region (< 60 → eventually > 200).
            if (lum < 60 && luminance601(row[x + 1]) >= 60) {
                int start = x + 1;
                int end = start;
                while (end < img.width() && luminance601(row[end]) <= 200
                       && luminance601(row[end]) >= 60) {
                    ++end;
                }
                if (end < img.width() && luminance601(row[end]) > 200) {
                    float width = static_cast<float>(
                        std::max(1, end - start));
                    widths.push_back(width);
                }
                x = end;
                continue;
            }
            ++x;
        }
    }

    if (widths.empty()) return 1.0f;
    return std::accumulate(widths.begin(), widths.end(), 0.0f)
           / static_cast<float>(widths.size());
}

bool ScanStyleTransfer::wouldSignificantlyDegrade(
    float blurThreshold, float noiseThreshold) const
{
    // Not calibrated → assume default FreeType-like rendering,
    // which is not degraded. Return false so callers can use LCD
    // subpixel freely.
    if (!style_.valid) return false;
    if (style_.blurSigma > blurThreshold) return true;
    if (style_.fgNoiseLevel > noiseThreshold) return true;
    return false;
}

float ScanStyleTransfer::measureEdgeTransitionWidth(const QImage& glyph)
{
    if (glyph.isNull() || glyph.width() < 4 || glyph.height() < 4)
        return 1.0f;

    QImage img = glyph.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    // Scan horizontal rows at 25%, 50%, 75% height
    std::vector<float> transitionWidths;
    const int scanRows[] = { img.height() / 4, img.height() / 2, img.height() * 3 / 4 };

    for (int y : scanRows) {
        if (y < 0 || y >= img.height()) continue;
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));

        // Find left-edge transitions: alpha going from <20 to >240
        for (int x = 0; x < img.width() - 1; ++x) {
            int a0 = qAlpha(row[x]);
            int a1 = qAlpha(row[x + 1]);

            // Detect entering a transition zone
            if (a0 < 20 && a1 >= 20) {
                // Count pixels until alpha > 240
                int transStart = x + 1;
                int transEnd = transStart;
                while (transEnd < img.width() && qAlpha(row[transEnd]) <= 240)
                    ++transEnd;
                if (transEnd < img.width()) {
                    transitionWidths.push_back(static_cast<float>(transEnd - transStart));
                }
            }
            // Also detect exiting: alpha going from >240 to <=240
            if (a0 > 240 && a1 <= 240) {
                int transStart = x + 1;
                int transEnd = transStart;
                while (transEnd < img.width() && qAlpha(row[transEnd]) >= 20)
                    ++transEnd;
                // Only count complete transitions (reaching alpha < 20)
                if (transEnd < img.width()) {
                    transitionWidths.push_back(static_cast<float>(transEnd - transStart));
                }
            }
        }
    }

    if (transitionWidths.empty())
        return 1.0f;

    return std::accumulate(transitionWidths.begin(), transitionWidths.end(), 0.0f)
           / static_cast<float>(transitionWidths.size());
}

// ---------------------------------------------------------------------------
// Calibration
// ---------------------------------------------------------------------------

void ScanStyleTransfer::calibrate(const GlyphAtlas& atlas)
{
    style_ = ScanStyle{};

    auto cps = atlas.codepoints();
    if (cps.empty())
        return;

    std::vector<float> roughnesses, noises, transitions, stemWidths;

    for (char32_t cp : cps) {
        QImage g = atlas.glyph(cp);
        if (g.isNull() || g.width() < 4 || g.height() < 6)
            continue;

        // NOTE on C4 coupling: atlas glyphs store textColor*alpha/255
        // as their RGB (synthetic monochrome from isolateAlpha),
        // not authentic scan RGB. Luminance on such pixels is
        // linearly related to alpha, so a luminance-based
        // measurement is NOT actually decoupled from
        // isolateAlpha's choice of distance function.
        //
        // The C4 change (Lab ΔE76 + morphological close) produces
        // slightly cleaner alpha on antialiased edges, which
        // shifts these measurements by a few percent. The shift
        // is accepted rather than worked around because:
        //   1. Atlas and fallback glyphs use the same calibrated
        //      targets, so the whole system moves together — no
        //      mismatch between atlas-rendered and fallback-
        //      rendered glyphs.
        //   2. A "correct" decoupling would require calibrating
        //      from raw pre-isolation crops, which is a larger
        //      architectural change than C4 itself.
        //
        // The luminance-based measurement helpers
        // (measureEdgeRoughnessFromLuminance /
        // measureEdgeTransitionWidthFromLuminance) are retained
        // as public utilities for any future calibration path
        // that operates on authentic scan RGB instead of atlas
        // monochrome.
        roughnesses.push_back(measureEdgeRoughness(g));
        noises.push_back(measureForegroundNoise(g));
        transitions.push_back(measureEdgeTransitionWidth(g));

        float sw = GlyphSynthesizer::measureStemWidth(g);
        if (sw >= 1.0f)
            stemWidths.push_back(sw);
    }

    int n = static_cast<int>(roughnesses.size());
    if (n < 1)
        return;

    // Average roughness and noise
    style_.edgeRoughness = std::accumulate(roughnesses.begin(), roughnesses.end(), 0.0f)
                           / static_cast<float>(n);
    style_.fgNoiseLevel = std::accumulate(noises.begin(), noises.end(), 0.0f)
                          / static_cast<float>(n);
    style_.edgeTransitionWidth = std::accumulate(transitions.begin(), transitions.end(), 0.0f)
                                 / static_cast<float>(n);

    // Median stem width
    if (!stemWidths.empty()) {
        std::sort(stemWidths.begin(), stemWidths.end());
        style_.avgStemWidth = stemWidths[stemWidths.size() / 2];
    }

    // Derive blur sigma from edge transition width
    // FreeType native anti-aliasing produces ~1.5 pixel transitions
    style_.blurSigma = std::max(0.0f, (style_.edgeTransitionWidth - 1.5f) * 0.4f);

    // Measure scan foreground/background contrast from atlas glyphs.
    // Atlas glyphs have opaque backgrounds (from scan paper) — measure the
    // grayscale histogram to find typical ink and paper brightness values.
    {
        int histogram[256] = {};
        int totalPx = 0;
        for (char32_t cp : cps) {
            QImage g = atlas.glyph(cp);
            if (g.isNull()) continue;
            QImage gray = g.convertToFormat(QImage::Format_Grayscale8);
            for (int y = 0; y < gray.height(); ++y) {
                const uchar* row = gray.constScanLine(y);
                for (int x = 0; x < gray.width(); ++x) {
                    histogram[row[x]]++;
                    totalPx++;
                }
            }
        }
        if (totalPx > 0) {
            int cum = 0;
            int p5 = 0, p95 = 255;
            bool p5Found = false;
            for (int i = 0; i < 256; ++i) {
                cum += histogram[i];
                if (!p5Found && cum * 100 / totalPx >= 5) { p5 = i; p5Found = true; }
                if (cum * 100 / totalPx >= 95) { p95 = i; break; }
            }
            style_.scanFgVal = std::clamp(p5, 20, 100);
            style_.scanBgVal = std::clamp(p95, 160, 230);
            if (style_.scanFgVal >= style_.scanBgVal - 40) {
                style_.scanFgVal = 50;
                style_.scanBgVal = 210;
            }
        }
    }

    style_.sampleCount = n;
    style_.valid = true;

    qDebug() << "[ScanStyleTransfer] calibrated from" << n << "glyphs:"
             << "edgeRoughness=" << style_.edgeRoughness
             << "fgNoise=" << style_.fgNoiseLevel
             << "edgeTransition=" << style_.edgeTransitionWidth
             << "blurSigma=" << style_.blurSigma
             << "stemWidth=" << style_.avgStemWidth
             << "scanFgVal=" << style_.scanFgVal
             << "scanBgVal=" << style_.scanBgVal;
}

// ---------------------------------------------------------------------------
// Processing stages
// ---------------------------------------------------------------------------

QImage ScanStyleTransfer::adjustStrokeWeight(const QImage& glyph, float targetStemWidth)
{
    if (glyph.isNull() || targetStemWidth < 1.0f)
        return glyph;

    float currentStem = GlyphSynthesizer::measureStemWidth(glyph);
    if (currentStem < 1.0f)
        return glyph;

    float ratio = targetStemWidth / currentStem;
    if (std::abs(ratio - 1.0f) < 0.15f)
        return glyph;

    // Reuse morphological approach from GlyphSynthesizer::matchStrokeWeight()
    QImage src = glyph.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    int iterations = std::clamp(static_cast<int>(std::round(std::abs(ratio - 1.0f))), 1, 2);

    // Cache scanLine pointers for fast pixel access
    auto getRow = [](const QImage& img, int y) -> const QRgb* {
        return reinterpret_cast<const QRgb*>(img.constScanLine(y));
    };

    for (int iter = 0; iter < iterations; ++iter) {
        QImage result(src.size(), QImage::Format_ARGB32);
        result.fill(Qt::transparent);

        if (ratio > 1.0f) {
            // Dilation: spread alpha to 4-connected neighbors
            for (int y = 0; y < h; ++y) {
                QRgb* outRow = reinterpret_cast<QRgb*>(result.scanLine(y));
                for (int x = 0; x < w; ++x) {
                    int maxAlpha = 0;
                    QRgb bestPixel = qRgba(0, 0, 0, 0);
                    constexpr int dx[] = {0, -1, 1, 0, 0};
                    constexpr int dy[] = {0, 0, 0, -1, 1};
                    for (int d = 0; d < 5; ++d) {
                        int nx = x + dx[d], ny = y + dy[d];
                        if (nx >= 0 && nx < w && ny >= 0 && ny < h) {
                            QRgb p = getRow(src, ny)[nx];
                            if (qAlpha(p) > maxAlpha) {
                                maxAlpha = qAlpha(p);
                                bestPixel = p;
                            }
                        }
                    }
                    if (maxAlpha > 0)
                        outRow[x] = bestPixel;
                }
            }
        } else {
            // Erosion: keep pixel only if all 4 neighbors have alpha > 0
            for (int y = 0; y < h; ++y) {
                const QRgb* srcRow = getRow(src, y);
                QRgb* outRow = reinterpret_cast<QRgb*>(result.scanLine(y));
                for (int x = 0; x < w; ++x) {
                    if (qAlpha(srcRow[x]) == 0)
                        continue;
                    bool allFg = true;
                    constexpr int dx[] = {-1, 1, 0, 0};
                    constexpr int dy[] = {0, 0, -1, 1};
                    for (int d = 0; d < 4; ++d) {
                        int nx = x + dx[d], ny = y + dy[d];
                        if (nx < 0 || nx >= w || ny < 0 || ny >= h ||
                            qAlpha(getRow(src, ny)[nx]) == 0) {
                            allFg = false;
                            break;
                        }
                    }
                    if (allFg)
                        outRow[x] = srcRow[x];
                }
            }
        }
        src = result;
    }

    return src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

QImage ScanStyleTransfer::roughenEdges(const QImage& glyph, float roughness, uint32_t seed)
{
    if (glyph.isNull() || roughness < 0.5f)
        return glyph;

    QImage src = glyph.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    uint32_t rng = seed;

    for (int y = 0; y < h; ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(src.scanLine(y));
        for (int x = 0; x < w; ++x) {
            int a = qAlpha(row[x]);
            if (a < 20 || a > 240)
                continue;

            // Add Gaussian noise to alpha
            float noise = gaussianSample(rng, roughness);
            int newAlpha = std::clamp(a + static_cast<int>(std::round(noise)), 0, 255);
            row[x] = qRgba(qRed(row[x]), qGreen(row[x]), qBlue(row[x]), newAlpha);
        }
    }

    return src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

QImage ScanStyleTransfer::applyBlur(const QImage& glyph, float sigma)
{
    if (glyph.isNull() || sigma < 0.3f)
        return glyph;

    const int w = glyph.width();
    const int h = glyph.height();
    if (w < 3 || h < 3)
        return glyph;

    // Build 1D Gaussian kernel
    int radius = std::min(static_cast<int>(std::ceil(2.0f * sigma)),
                          std::min(w, h) / 2 - 1);
    radius = std::max(1, radius);
    int ksize = 2 * radius + 1;

    std::vector<float> kernel(static_cast<size_t>(ksize));
    float sum = 0.0f;
    for (int i = 0; i < ksize; ++i) {
        float x = static_cast<float>(i - radius);
        kernel[static_cast<size_t>(i)] = std::exp(-x * x / (2.0f * sigma * sigma));
        sum += kernel[static_cast<size_t>(i)];
    }
    std::transform(kernel.begin(), kernel.end(), kernel.begin(),
                   [sum](float k) { return k / sum; });

    // Work on premultiplied data directly — linear blur preserves premultiplication.
    // Extract float channels for precision.
    std::vector<float> rCh(static_cast<size_t>(w * h));
    std::vector<float> gCh(static_cast<size_t>(w * h));
    std::vector<float> bCh(static_cast<size_t>(w * h));
    std::vector<float> aCh(static_cast<size_t>(w * h));

    QImage src = glyph.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < h; ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            size_t idx = static_cast<size_t>(y * w + x);
            rCh[idx] = static_cast<float>(qRed(row[x]));
            gCh[idx] = static_cast<float>(qGreen(row[x]));
            bCh[idx] = static_cast<float>(qBlue(row[x]));
            aCh[idx] = static_cast<float>(qAlpha(row[x]));
        }
    }

    // Horizontal pass
    auto horizBlur = [&](std::vector<float>& ch) {
        std::vector<float> tmp(ch.size());
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float val = 0.0f;
                float wt = 0.0f;
                for (int k = -radius; k <= radius; ++k) {
                    int sx = x + k;
                    if (sx >= 0 && sx < w) {
                        float kw = kernel[static_cast<size_t>(k + radius)];
                        val += ch[static_cast<size_t>(y * w + sx)] * kw;
                        wt += kw;
                    }
                }
                tmp[static_cast<size_t>(y * w + x)] = val / wt;
            }
        }
        ch = std::move(tmp);
    };

    // Vertical pass
    auto vertBlur = [&](std::vector<float>& ch) {
        std::vector<float> tmp(ch.size());
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float val = 0.0f;
                float wt = 0.0f;
                for (int k = -radius; k <= radius; ++k) {
                    int sy = y + k;
                    if (sy >= 0 && sy < h) {
                        float kw = kernel[static_cast<size_t>(k + radius)];
                        val += ch[static_cast<size_t>(sy * w + x)] * kw;
                        wt += kw;
                    }
                }
                tmp[static_cast<size_t>(y * w + x)] = val / wt;
            }
        }
        ch = std::move(tmp);
    };

    horizBlur(rCh); horizBlur(gCh); horizBlur(bCh); horizBlur(aCh);
    vertBlur(rCh);  vertBlur(gCh);  vertBlur(bCh);  vertBlur(aCh);

    // Reconstruct image, clamping to ensure premultiplied invariant (R<=A, etc.)
    QImage result(w, h, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < h; ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < w; ++x) {
            size_t idx = static_cast<size_t>(y * w + x);
            int a = std::clamp(static_cast<int>(std::round(aCh[idx])), 0, 255);
            int r = std::clamp(static_cast<int>(std::round(rCh[idx])), 0, a);
            int g = std::clamp(static_cast<int>(std::round(gCh[idx])), 0, a);
            int b = std::clamp(static_cast<int>(std::round(bCh[idx])), 0, a);
            row[x] = qRgba(r, g, b, a);
        }
    }

    return result;
}

QImage ScanStyleTransfer::injectNoise(const QImage& glyph, float noiseLevel, uint32_t seed)
{
    if (glyph.isNull() || noiseLevel < 1.0f)
        return glyph;

    QImage src = glyph.convertToFormat(QImage::Format_ARGB32);
    const int w = src.width();
    const int h = src.height();
    uint32_t rng = seed;

    for (int y = 0; y < h; ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(src.scanLine(y));
        for (int x = 0; x < w; ++x) {
            int a = qAlpha(row[x]);
            if (a <= 200)
                continue;

            // Read straight RGB (ARGB32 is non-premultiplied)
            int r = qRed(row[x]);
            int g = qGreen(row[x]);
            int b = qBlue(row[x]);

            r = std::clamp(r + static_cast<int>(std::round(gaussianSample(rng, noiseLevel))), 0, 255);
            g = std::clamp(g + static_cast<int>(std::round(gaussianSample(rng, noiseLevel))), 0, 255);
            b = std::clamp(b + static_cast<int>(std::round(gaussianSample(rng, noiseLevel))), 0, 255);

            row[x] = qRgba(r, g, b, a);
        }
    }

    return src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

// ---------------------------------------------------------------------------
// Full pipeline
// ---------------------------------------------------------------------------

QImage ScanStyleTransfer::apply(const QImage& ftGlyph) const
{
    if (ftGlyph.isNull() || !style_.valid)
        return ftGlyph;

    // Seed from image dimensions and pixel content for deterministic per-glyph results
    uint32_t seed = 0x12345678u
                    ^ (static_cast<uint32_t>(ftGlyph.width()) * 7919u)
                    ^ (static_cast<uint32_t>(ftGlyph.height()) * 104729u);
    // Sample first, middle, and last rows for better differentiation
    const int sampleRows[] = { 0, ftGlyph.height() / 2, ftGlyph.height() - 1 };
    for (int row : sampleRows) {
        if (row >= 0 && row < ftGlyph.height()) {
            const QRgb* rowData = reinterpret_cast<const QRgb*>(ftGlyph.constScanLine(row));
            for (int x = 0; x < std::min(ftGlyph.width(), 4); ++x)
                seed ^= static_cast<uint32_t>(rowData[x]) * (static_cast<uint32_t>(row) + 1u);
        }
    }

    QImage result = ftGlyph.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    // Stage 1: Stroke weight matching
    if (style_.avgStemWidth >= 1.0f)
        result = adjustStrokeWeight(result, style_.avgStemWidth);

    // Stage 2: Edge roughening
    if (style_.edgeRoughness >= 0.5f)
        result = roughenEdges(result, style_.edgeRoughness, seed);

    // Stage 3: Gaussian blur
    if (style_.blurSigma >= 0.3f)
        result = applyBlur(result, style_.blurSigma);

    // Stage 4: Foreground noise injection
    if (style_.fgNoiseLevel >= 1.0f)
        result = injectNoise(result, style_.fgNoiseLevel, seed ^ 0xDEADBEEFu);

    return result;
}

QImage ScanStyleTransfer::applyOnWhiteBg(const QImage& whiteBgGlyph) const
{
    if (whiteBgGlyph.isNull() || !style_.valid)
        return whiteBgGlyph;

    QImage gray = whiteBgGlyph.convertToFormat(QImage::Format_Grayscale8);
    const int w = gray.width(), h = gray.height();
    if (w < 2 || h < 2)
        return gray;

    // Stage 1: Contrast remap (black ink → scanFgVal, white paper → scanBgVal)
    // with deterministic per-pixel noise for scan texture.
    float fg = static_cast<float>(style_.scanFgVal);
    float bg = static_cast<float>(style_.scanBgVal);

    auto fineNoise = [](int x, int y) -> int {
        unsigned hh = static_cast<unsigned>(x) * 374761393u
                    + static_cast<unsigned>(y) * 668265263u;
        hh = (hh ^ (hh >> 13)) * 1274126177u;
        return static_cast<int>((hh >> 16) & 0xFF) % 21 - 10;
    };
    auto paperTexture = [](int x, int y) -> int {
        unsigned bx = static_cast<unsigned>(x / 5);
        unsigned by = static_cast<unsigned>(y / 5);
        unsigned hh = bx * 2654435761u + by * 2246822519u;
        hh = (hh ^ (hh >> 15)) * 2246822519u;
        return static_cast<int>((hh >> 16) & 0xFF) % 13 - 6;
    };

    for (int y = 0; y < h; ++y) {
        uchar* row = gray.scanLine(y);
        for (int x = 0; x < w; ++x) {
            float t = static_cast<float>(row[x]) / 255.0f;
            int val = static_cast<int>(fg + t * (bg - fg));
            int noise = fineNoise(x, y);
            if (t > 0.5f)
                noise += paperTexture(x, y);
            row[x] = static_cast<uchar>(std::clamp(val + noise, 0, 255));
        }
    }

    // Stage 2: Gaussian blur (match scan softness)
    if (style_.blurSigma >= 0.3f) {
        int radius = std::max(1, static_cast<int>(std::ceil(style_.blurSigma * 2.0f)));
        int kernelSize = radius * 2 + 1;
        std::vector<float> kernel(static_cast<size_t>(kernelSize));
        float kSum = 0.0f;
        for (int i = 0; i < kernelSize; ++i) {
            float xf = static_cast<float>(i - radius);
            kernel[static_cast<size_t>(i)] = std::exp(-(xf * xf) / (2.0f * style_.blurSigma * style_.blurSigma));
            kSum += kernel[static_cast<size_t>(i)];
        }
        std::transform(kernel.begin(), kernel.end(), kernel.begin(),
                       [kSum](float k) { return k / kSum; });

        // Horizontal pass
        QImage temp(gray.size(), QImage::Format_Grayscale8);
        for (int y = 0; y < h; ++y) {
            const uchar* src = gray.constScanLine(y);
            uchar* dst = temp.scanLine(y);
            for (int x = 0; x < w; ++x) {
                float sum = 0.0f;
                for (int k = -radius; k <= radius; ++k) {
                    int sx = std::clamp(x + k, 0, w - 1);
                    sum += static_cast<float>(src[sx]) * kernel[static_cast<size_t>(k + radius)];
                }
                dst[x] = static_cast<uchar>(std::clamp(static_cast<int>(sum + 0.5f), 0, 255));
            }
        }
        // Vertical pass
        for (int x = 0; x < w; ++x) {
            for (int y = 0; y < h; ++y) {
                float sum = 0.0f;
                for (int k = -radius; k <= radius; ++k) {
                    int sy = std::clamp(y + k, 0, h - 1);
                    sum += static_cast<float>(temp.constScanLine(sy)[x]) * kernel[static_cast<size_t>(k + radius)];
                }
                gray.scanLine(y)[x] = static_cast<uchar>(std::clamp(static_cast<int>(sum + 0.5f), 0, 255));
            }
        }
    }

    return gray;
}

} // namespace pdfe::tre
