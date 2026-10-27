#include "tre/LineFontVerifier.h"
#include "tre/ImageSimilarity.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include <QDebug>
#include <QImage>

namespace pdfe::tre {

namespace {

/// Compare two positive float values as a ratio score [0, 1].
/// Returns 1.0 if both are zero (no info, don't penalize).
/// Returns 0.5 if exactly one is zero (uncertain).
/// Otherwise returns min/max, which is 1.0 when identical.
float ratioScore(float a, float b)
{
    if (a <= 0.0f && b <= 0.0f)
        return 1.0f;  // Both unknown -- no info, assume compatible
    if (a <= 0.0f || b <= 0.0f)
        return 0.5f;  // One unknown -- uncertain
    float mn = std::min(a, b);
    float mx = std::max(a, b);
    return mn / mx;  // [0, 1] where 1 = identical
}

/// Compute a sub-ratio score: ratio of (metric / base) for two lines.
/// If base is zero for either line, returns 0.5 (uncertain).
float subRatioScore(float metricA, float baseA, float metricB, float baseB)
{
    if (baseA <= 0.0f || baseB <= 0.0f)
        return 0.5f;
    float ratioA = metricA / baseA;
    float ratioB = metricB / baseB;
    // Both ratios are proportions -- compare their similarity.
    // Clamp to [0,1] to handle negative metrics (e.g. Tesseract negative descenders).
    float mn = std::min(ratioA, ratioB);
    float mx = std::max(ratioA, ratioB);
    if (mx <= 0.0001f)
        return 1.0f;  // Both near zero
    return std::clamp(mn / mx, 0.0f, 1.0f);
}

/// Compute average word fontSizePx for a line.
float avgWordFontSize(const HocrLine& line)
{
    float sum = 0.0f;
    int count = 0;
    for (const auto& w : line.words) {
        if (w.fontSizePx > 0.0f) {
            sum += w.fontSizePx;
            ++count;
        }
    }
    return count > 0 ? sum / static_cast<float>(count) : 0.0f;
}

} // anonymous namespace


float LineFontVerifier::compareMetrics(const HocrLine& refLine,
                                        const HocrLine& candidateLine)
{
    // Weight: xSize 0.4, ascenderRatio 0.2, descenderRatio 0.2, fontSize 0.2
    float xSizeScore = ratioScore(refLine.xSizePx, candidateLine.xSizePx);

    float ascScore = subRatioScore(refLine.ascendersPx, refLine.xSizePx,
                                    candidateLine.ascendersPx, candidateLine.xSizePx);

    float descScore = subRatioScore(refLine.descendersPx, refLine.xSizePx,
                                     candidateLine.descendersPx, candidateLine.xSizePx);

    float fontSizeScore = ratioScore(avgWordFontSize(refLine),
                                      avgWordFontSize(candidateLine));

    return 0.4f * xSizeScore + 0.2f * ascScore + 0.2f * descScore + 0.2f * fontSizeScore;
}


std::vector<char32_t> LineFontVerifier::findSharedCodepoints(
    const HocrLine& lineA, const HocrLine& lineB)
{
    // Build set of codepoints from lineA
    std::unordered_set<char32_t> setA;
    for (const auto& w : lineA.words)
        for (const auto& ch : w.chars)
            if (ch.codepoint != 0 && ch.codepoint != U' ')
                setA.insert(ch.codepoint);

    // Find intersection with lineB
    std::unordered_set<char32_t> shared;
    for (const auto& w : lineB.words)
        for (const auto& ch : w.chars)
            if (ch.codepoint != 0 && ch.codepoint != U' ' && setA.count(ch.codepoint))
                shared.insert(ch.codepoint);

    return {shared.begin(), shared.end()};
}


QImage LineFontVerifier::cropChar(const QImage& pageImage, const HocrChar& ch)
{
    int x0 = std::max(0, static_cast<int>(ch.bbox.x0));
    int y0 = std::max(0, static_cast<int>(ch.bbox.y0));
    int x1 = std::min(pageImage.width(), static_cast<int>(ch.bbox.x1));
    int y1 = std::min(pageImage.height(), static_cast<int>(ch.bbox.y1));

    int w = x1 - x0;
    int h = y1 - y0;
    if (w <= 0 || h <= 0)
        return {};

    return pageImage.copy(x0, y0, w, h);
}


const HocrChar* LineFontVerifier::findBestChar(const HocrLine& line, char32_t codepoint)
{
    const HocrChar* best = nullptr;
    for (const auto& w : line.words) {
        for (const auto& ch : w.chars) {
            if (ch.codepoint == codepoint) {
                if (!best || ch.confidence > best->confidence)
                    best = &ch;
            }
        }
    }
    return best;
}


/// Content-crop a grayscale image: trim surrounding whitespace to the ink
/// bounding box. Uses Otsu threshold to separate ink from background.
static QImage contentCrop(const QImage& img)
{
    QImage gray = img.format() == QImage::Format_Grayscale8
                      ? img : img.convertToFormat(QImage::Format_Grayscale8);
    if (gray.isNull())
        return img;

    int thresh = ImageSimilarity::otsuThreshold(gray);
    int top = gray.height(), bottom = -1, left = gray.width(), right = -1;
    for (int y = 0; y < gray.height(); ++y) {
        const uchar* row = gray.constScanLine(y);
        for (int x = 0; x < gray.width(); ++x) {
            if (row[x] < thresh) {  // ink pixel (dark)
                if (y < top) top = y;
                if (y > bottom) bottom = y;
                if (x < left) left = x;
                if (x > right) right = x;
            }
        }
    }
    if (top > bottom || left > right)
        return img;  // no ink found, return original

    return img.copy(left, top, right - left + 1, bottom - top + 1);
}

float LineFontVerifier::compareSharedGlyphs(
    const QImage& pageImage,
    const HocrLine& refLine,
    const HocrLine& candidateLine,
    const std::vector<char32_t>& sharedCodepoints)
{
    float totalNcc = 0.0f;
    int validPairs = 0;

    for (char32_t cp : sharedCodepoints) {
        const HocrChar* refCh = findBestChar(refLine, cp);
        const HocrChar* candCh = findBestChar(candidateLine, cp);
        if (!refCh || !candCh)
            continue;

        QImage cropA = cropChar(pageImage, *refCh);
        QImage cropB = cropChar(pageImage, *candCh);
        if (cropA.isNull() || cropB.isNull())
            continue;

        // Content-crop: trim whitespace to ink bounding box so NCC
        // compares character shapes rather than background padding.
        // This prevents size mismatches from different line heights
        // (e.g., 33px vs 43px lines) from dominating the comparison.
        cropA = contentCrop(cropA);
        cropB = contentCrop(cropB);

        // Skip very small crops (likely noise)
        if (cropA.width() < 3 || cropA.height() < 3 ||
            cropB.width() < 3 || cropB.height() < 3)
            continue;

        // Scale both to common height (the larger) to avoid
        // squishing the bigger crop into the smaller footprint.
        int targetH = std::max(cropA.height(), cropB.height());
        if (cropA.height() != targetH)
            cropA = cropA.scaledToHeight(targetH, Qt::SmoothTransformation);
        if (cropB.height() != targetH)
            cropB = cropB.scaledToHeight(targetH, Qt::SmoothTransformation);

        float ncc = ImageSimilarity::binarizedNCC(cropA, cropB);
        totalNcc += ncc;
        ++validPairs;
    }

    if (validPairs == 0)
        return -1.0f;  // No valid comparisons

    return totalNcc / static_cast<float>(validPairs);
}


LineFontVerifier::Thresholds LineFontVerifier::defaultThresholds() {
    return {};
}

FontVerifyResult LineFontVerifier::verify(
    const QImage& pageImage,
    const HocrLine& refLine,
    const HocrLine& candidateLine,
    const Thresholds& thresholds)
{
    FontVerifyResult result;

    // Step 1: Compare HOCR metrics (cheap)
    result.metricScore = compareMetrics(refLine, candidateLine);
    if (result.metricScore < thresholds.minMetricScore) {
        // Early reject -- metrics are too different
        result.confidence = result.metricScore;
        return result;
    }

    // Step 2: Find shared codepoints
    auto shared = findSharedCodepoints(refLine, candidateLine);
    result.sharedChars = static_cast<int>(shared.size());

    if (result.sharedChars < thresholds.minSharedCharsForNcc) {
        // Not enough shared chars for pixel comparison -- use metric score alone
        result.confidence = result.metricScore;
        result.compatible = result.confidence >= thresholds.minCombinedScore;
        return result;
    }

    // Step 3: Pixel-based NCC comparison of shared character glyphs
    result.nccScore = compareSharedGlyphs(pageImage, refLine, candidateLine, shared);
    if (result.nccScore < 0.0f) {
        // No valid comparisons (all crops were too small or null)
        result.confidence = result.metricScore;
        result.compatible = result.confidence >= thresholds.minCombinedScore;
        return result;
    }

    // Step 4: Combine scores.
    // When metrics agree well (>= 0.85), bias toward metric score (0.6/0.4).
    // Tesseract HOCR metrics (xSizePx, ascender/descender ratios) are reliable
    // font identity signals. NCC comparison is noisy when reference line and
    // candidate have different heights (e.g., 33px vs 41px) — the same character
    // at different sizes doesn't NCC-match well even after scaling.
    float mw = thresholds.metricWeight;
    float nw = thresholds.nccWeight;
    if (result.metricScore >= 0.85f) {
        mw = 0.6f;
        nw = 0.4f;
    }
    result.confidence = mw * result.metricScore + nw * result.nccScore;
    result.compatible = result.confidence >= thresholds.minCombinedScore;

    return result;
}

} // namespace pdfe::tre
