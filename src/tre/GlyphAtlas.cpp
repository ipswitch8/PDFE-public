#include "core/TempFileSafety.h"
#include "tre/GlyphAtlas.h"
#include "tre/ImageSimilarity.h"
#include "tre/LineFontVerifier.h"

#include <QDebug>
#include <QFile>
#include <QPainter>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <vector>

namespace pdfe::tre {

namespace {

/// Does an isolated glyph mask contain anything that is actually ink?
///
/// GitHub #266. `isolateAlpha` scores each pixel by where it falls between the
/// text and background colours, so alpha 255 means "this pixel IS the text
/// colour" and alpha 128 means "equidistant from both". A mask whose peak alpha
/// does not exceed 128 therefore contains no pixel that is more text-like than
/// background-like, and calling it a letter is unjustified whatever its shape.
///
/// The bound is the discriminator's own midpoint rather than a tuned constant:
/// a tuned threshold drifts, and nobody can later say what it meant.
bool maskCarriesInk(const QImage& mask)
{
    if (mask.isNull()) return false;
    const QImage m = mask.convertToFormat(QImage::Format_ARGB32);
    int peak = 0;
    for (int y = 0; y < m.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(m.constScanLine(y));
        for (int x = 0; x < m.width(); ++x) {
            const int a = qAlpha(row[x]);
            if (a > peak) {
                peak = a;
                if (peak > 128) return true;   // decided; stop scanning
            }
        }
    }
    return false;
}

/// Crop an ARGB32_Premultiplied image HORIZONTALLY only.
/// Trims left/right transparent columns for tight character spacing.
/// Preserves full vertical extent (y=0 to height) — critical because all
/// glyphs share a common line-height vertical coordinate system.
/// Returns null QImage if no visible pixels exist.
QImage cropHorizontalOnly(const QImage& img, int alphaThreshold = 0)
{
    if (img.isNull())
        return {};

    int left = img.width(), right = -1;
    bool hasVisible = false;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            if (qAlpha(line[x]) > alphaThreshold) {
                hasVisible = true;
                if (x < left) left = x;
                if (x > right) right = x;
            }
        }
    }
    if (!hasVisible)
        return {};
    return img.copy(left, 0, right - left + 1, img.height());
}

/// Clip an alpha-isolated glyph at significant vertical gaps on both sides.
/// Tesseract char bboxes sometimes extend into adjacent characters. After
/// alpha isolation, these adjacent pixels remain visible. We find contiguous
/// groups of visible columns separated by gaps, and keep the largest group
/// (the actual character), discarding contamination from neighbors.
///
/// Uses density-based gap detection: columns with pixel count below 10% of
/// the densest column are treated as gaps. This handles the common case where
/// contamination has a thin bridge of faint pixels connecting it to the main
/// character body (no fully-transparent gap exists).
///
/// Returns the input image unchanged if only one group is found or the glyph
/// is very narrow (likely a single character like 'l' or 'i').
/// Build contiguous groups at a given sparse threshold.
struct ColGroup { int startCol; int endCol; int totalPixels; };
static std::vector<ColGroup> buildColGroups(const std::vector<int>& colCounts,
                                            int sparseThreshold)
{
    std::vector<ColGroup> groups;
    int groupStart = -1;
    int groupPixels = 0;
    for (int x = 0; x < static_cast<int>(colCounts.size()); ++x) {
        if (colCounts[x] >= sparseThreshold) {
            if (groupStart < 0) groupStart = x;
            groupPixels += colCounts[x];
        } else {
            if (groupStart >= 0) {
                groups.push_back({groupStart, x, groupPixels});
                groupStart = -1;
                groupPixels = 0;
            }
        }
    }
    if (groupStart >= 0)
        groups.push_back({groupStart, static_cast<int>(colCounts.size()), groupPixels});
    return groups;
}

QImage clipAtVerticalGap(const QImage& img, int alphaThreshold = 20,
                         bool gentleMode = false)
{
    if (img.isNull() || img.width() < 4)
        return img;

    // Count visible pixels per column
    std::vector<int> colCounts(img.width(), 0);
    for (int y = 0; y < img.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            if (qAlpha(line[x]) > alphaThreshold)
                ++colCounts[x];
        }
    }

    int maxColCount = *std::max_element(colCounts.begin(), colCounts.end());
    if (maxColCount == 0)
        return img;

    // Progressive thresholding: try increasingly aggressive sparse thresholds.
    // At /10 (10% of peak), only truly empty columns are gaps — safe for all chars.
    // At /4 (25% of peak), thin ink bridges between adjacent characters are
    // treated as gaps — this catches contamination where characters physically
    // touch in the scan (e.g., 'r' + partial 'o' in "from"), but also destroys
    // thin strokes within characters (D's right curve, M's diagonals, r's hook).
    //
    // gentleMode=true: only /10 (safe). Used when templates handle contamination.
    // gentleMode=false: /10 then /4 (aggressive). Used without templates.
    const int kGentleDivisors[] = {10};
    const int kFullDivisors[] = {10, 4};
    const int* divisors = gentleMode ? kGentleDivisors : kFullDivisors;
    int numDivisors = gentleMode ? 1 : 2;
    int minBody = std::max(3, img.width() / 5);

    for (int i = 0; i < numDivisors; ++i) {
        int sparseThreshold = std::max(1, maxColCount / divisors[i]);
        auto groups = buildColGroups(colCounts, sparseThreshold);

        if (groups.size() <= 1)
            continue;  // No gap found at this threshold, try more aggressive

        // Find the group with the most total visible pixels (the main character)
        auto mainIt = std::max_element(groups.begin(), groups.end(),
            [](const ColGroup& a, const ColGroup& b) {
                return a.totalPixels < b.totalPixels;
            });

        int mainWidth = mainIt->endCol - mainIt->startCol;
        if (mainWidth >= minBody)
            return img.copy(mainIt->startCol, 0, mainWidth, img.height());
    }

    return img;  // No valid split found at any threshold
}

/// Composite an alpha-isolated glyph onto a white background.
/// binarizedNCC operates on grayscale values and breaks when both text (gray~30)
/// and transparent background (gray=0 after premultiplied→grayscale conversion)
/// are very dark — Otsu picks threshold=0, producing a uniform binary image.
/// Compositing onto white gives the expected dark-on-white pattern.
QImage compositeOnWhite(const QImage& img)
{
    if (img.isNull())
        return {};
    QImage result(img.size(), QImage::Format_RGB32);
    if (result.isNull())
        return {};
    result.fill(Qt::white);
    QPainter p(&result);
    p.drawImage(0, 0, img);
    p.end();
    return result;
}

/// Clip a crop image to the template's expected width, removing adjacent
/// character contamination from overlapping HOCR bounding boxes.
/// Scales the template width proportionally when crop and template have
/// different heights (different line heights on the page).
/// Uses a sliding window to find the densest maxWidth-wide region,
/// preserving asymmetric character features (e.g., the foot of 'L').
QImage clipToTemplateWidth(const QImage& crop, const QImage& tmpl, float tolerance = 1.25f)
{
    if (crop.isNull() || tmpl.isNull() || crop.height() <= 0 ||
        tmpl.width() <= 0 || tmpl.height() <= 0)
        return crop;

    // Scale template width to match the crop's line height.
    // Templates are rendered at the reference line height, but crops come
    // from lines with potentially different heights. Without scaling, the
    // template width is proportionally too wide for shorter-line crops,
    // and the clip barely removes any contamination.
    float heightRatio = static_cast<float>(crop.height()) /
                        static_cast<float>(tmpl.height());
    int scaledTmplW = std::max(1, static_cast<int>(tmpl.width() * heightRatio));
    int maxWidth = static_cast<int>(scaledTmplW * tolerance);
    if (crop.width() <= maxWidth)
        return crop;  // Already fits

    // Find the densest maxWidth-wide window using a sliding window over
    // column alpha sums. This naturally keeps the part of the crop with
    // the most ink (the main character body, including asymmetric features
    // like the foot of 'L') while excluding thin contamination at the edges.
    std::vector<double> colAlpha(crop.width(), 0.0);
    for (int y = 0; y < crop.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(crop.constScanLine(y));
        for (int x = 0; x < crop.width(); ++x)
            colAlpha[x] += qAlpha(row[x]);
    }

    // Initial window sum
    double windowSum = 0.0;
    int windowW = std::min(maxWidth, crop.width());
    for (int x = 0; x < windowW; ++x)
        windowSum += colAlpha[x];

    double bestSum = windowSum;
    int bestLeft = 0;

    // Slide the window rightward
    for (int left = 1; left + windowW <= crop.width(); ++left) {
        windowSum -= colAlpha[left - 1];
        windowSum += colAlpha[left + windowW - 1];
        if (windowSum > bestSum) {
            bestSum = windowSum;
            bestLeft = left;
        }
    }

    return crop.copy(bestLeft, 0, windowW, crop.height());
}

/// Pad a narrow scan crop to match the template's expected width.
/// Tesseract char bboxes are ink-tight (no side-bearings), so crops are
/// narrower than a properly-set character. After NCC confirms identity,
/// the template width (scaled for line height) gives the correct character
/// advance. We center the ink within a wider transparent canvas.
/// Only pads if crop is more than 20% narrower than the scaled template width.
QImage padToTemplateWidth(const QImage& crop, const QImage& tmpl)
{
    if (crop.isNull() || tmpl.isNull() || crop.height() <= 0 ||
        tmpl.width() <= 0 || tmpl.height() <= 0)
        return crop;

    float heightRatio = static_cast<float>(crop.height()) /
                        static_cast<float>(tmpl.height());
    int targetW = std::max(1, static_cast<int>(tmpl.width() * heightRatio));

    // Only pad if crop is notably narrower (>20% deficit)
    if (crop.width() >= static_cast<int>(targetW * 0.8f))
        return crop;

    if (targetW <= crop.width())
        return crop;

    // Center the ink in the wider canvas.
    // Crop must be 32-bit (from isolateAlpha) for the QRgb memcpy.
    if (crop.depth() != 32)
        return crop;
    QImage padded(targetW, crop.height(), QImage::Format_ARGB32_Premultiplied);
    padded.fill(Qt::transparent);
    int xOffset = (targetW - crop.width()) / 2;

    for (int y = 0; y < crop.height(); ++y) {
        const QRgb* srcRow = reinterpret_cast<const QRgb*>(crop.constScanLine(y));
        QRgb* dstRow = reinterpret_cast<QRgb*>(padded.scanLine(y));
        std::memcpy(dstRow + xOffset, srcRow, crop.width() * sizeof(QRgb));
    }

    return padded;
}

} // anonymous namespace


void GlyphAtlas::harvestLine(const QImage& pageImage, const HocrLine& line,
                              const RgbColor& textColor, const RgbColor& bgColor,
                              bool addMissingOnly,
                              const TemplateMap* templates,
                              float minNccScore)
{
    const int imgW = pageImage.width();
    const int imgH = pageImage.height();

    // LINE-BASED VERTICAL NORMALIZATION:
    // All characters from the same line are cropped using the LINE's Y range
    // (not individual char Y ranges). This puts every character into the same
    // vertical coordinate system: 'D' visible pixels appear near the top
    // (ascender), 'e' starts lower (x-height), 'g' extends to the bottom
    // (descender). A single scale factor (screenBboxH / lineH) then correctly
    // renders all characters with proper baseline alignment.
    int lineY0 = std::max(0, static_cast<int>(line.bbox.y0));
    int lineY1 = std::min(imgH, static_cast<int>(line.bbox.y1));
    int lineH = lineY1 - lineY0;
    if (lineH <= 0) return;

    // When templates are provided, collect ALL valid candidate crops per
    // codepoint, then score each against the reference template via NCC.
    // This lets us pick the cleanest instance (highest similarity to the
    // expected character shape) rather than relying solely on confidence.
    struct Candidate {
        QImage image;
        int confidence;
    };
    std::unordered_map<char32_t, std::vector<Candidate>> candidates;

    for (const auto& word : line.words) {
        for (size_t ci = 0; ci < word.chars.size(); ++ci) {
            const auto& ch = word.chars[ci];
            if (ch.codepoint == 0)
                continue;

            // Word-final characters have higher contamination risk (HOCR bbox
            // extends into adjacent space). Reduce their effective confidence
            // so word-internal instances of the same character (which are
            // clipped by next-char bbox and thus cleaner) take priority.
            bool isWordFinal = (ci + 1 >= word.chars.size());
            int effectiveConf = ch.confidence - (isWordFinal ? 1 : 0);

            // addMissingOnly (donor lines): when templates are available, still
            // collect candidates so template scoring can compare NCC against the
            // existing atlas entry (lines 500-508). This lets a cleaner crop from
            // a later donor line replace a contaminated one from an earlier line.
            // Without templates: strict skip (no quality comparison possible).
            if (addMissingOnly && glyphs_.find(ch.codepoint) != glyphs_.end()) {
                if (!templates)
                    continue;
                // With templates: fall through to collect as candidate
            }

            // Without templates: skip if existing glyph has equal or higher confidence
            // (original behavior). With templates: still skip early for clearly worse
            // confidence, but collect all candidates at the same or better confidence
            // level for NCC scoring later.
            if (!templates) {
                auto existIt = glyphs_.find(ch.codepoint);
                if (existIt != glyphs_.end() && existIt->second.confidence >= effectiveConf)
                    continue;
            }

            // Horizontal bounds from char bbox, clamped to image
            int x0 = std::max(0, static_cast<int>(ch.bbox.x0));
            int x1 = std::min(imgW, static_cast<int>(ch.bbox.x1));

            // Limit RIGHT edge to prevent right-side contamination.
            // Without templates: clip tightly to next char's left edge or word bbox.
            // With templates: use the char's own bbox right edge — template NCC
            // scoring + gentle clipAtVerticalGap handle contamination. Tight
            // clipping to next-char bbox destroys characters whose ink extends
            // past the HOCR bbox boundary (D's right curve, P's bowl, etc.).
            if (!templates) {
                if (ci + 1 < word.chars.size()) {
                    int nextX0 = static_cast<int>(word.chars[ci + 1].bbox.x0);
                    if (nextX0 > x0 + 2)
                        x1 = std::min(x1, nextX0);
                } else {
                    int wordX1 = static_cast<int>(word.bbox.x1);
                    if (wordX1 > x0 + 2)
                        x1 = std::min(x1, wordX1);
                }
            }

            int w = x1 - x0;
            if (w <= 0)
                continue;

            // Crop using char X range but LINE Y range
            QImage crop = pageImage.copy(x0, lineY0, w, lineH);
            if (crop.isNull())
                continue;

            // Apply alpha isolation
            QImage isolated = isolateAlpha(crop, textColor, bgColor);

            // Alpha floor: zero out pixels with very low alpha.
            // Contamination from adjacent characters (overlapping HOCR bboxes)
            // produces low-alpha pixels in the gap between characters. Removing
            // these prevents ghost pixels from rendering at wrong positions.
            // Threshold kept low (20) to preserve anti-aliased edges on curves
            // (D's right side, a's bowl, r's hook) which typically have alpha 30-80.
            constexpr int kAlphaFloor = 20;
            for (int y = 0; y < isolated.height(); ++y) {
                QRgb* row = reinterpret_cast<QRgb*>(isolated.scanLine(y));
                for (int x = 0; x < isolated.width(); ++x) {
                    if (qAlpha(row[x]) > 0 && qAlpha(row[x]) < kAlphaFloor)
                        row[x] = qRgba(0, 0, 0, 0);
                }
            }

            // Remove horizontal contamination from overlapping bboxes.
            // Finds contiguous groups of visible columns, keeps the largest.
            // Always uses gentle mode (/10 only) to preserve thin internal
            // strokes in round characters (o, a, b, d, p). The aggressive /4
            // mode destroyed these structures. Width constraint check below
            // handles remaining contamination for the non-template path.
            isolated = clipAtVerticalGap(isolated, 20, /*gentleMode=*/true);

            // Trim horizontal transparent borders only. Full vertical extent
            // (lineH) is preserved — it encodes each character's position
            // within the line (critical for the overlay's top-aligned rendering).
            isolated = cropHorizontalOnly(isolated);
            if (isolated.isNull())
                continue;

            // GitHub #266 -- refuse a crop in which nothing is actually ink.
            //
            // isolateAlpha() scores each pixel by where it falls between the
            // text and background colours: alpha = 255 * distToBg / (distToText
            // + distToBg). It reaches 255 only where a pixel IS the text
            // colour, and 128 exactly where a pixel is equidistant. So a mask
            // whose PEAK is at or below 128 says something specific and fatal:
            // no pixel in this crop is more text-like than background-like.
            // Whatever was cropped, it is not ink.
            //
            // The threshold is the discriminator's own midpoint, not a tuned
            // constant -- which matters, because a tuned one would drift and
            // nobody could say what it meant.
            //
            // MEASURED on mixed_dpi_page.pdf (2026-09-29): the harvested 'T'
            // came back as a featureless grey BAR, 30x58, alphaMin=0
            // alphaMax=127 alphaMean=76.6, ink over 60% of its box. The atlas
            // accepted it and, being the highest-fidelity source, it won over
            // every fallback -- so editing THUMBNAIL to EDIT drew "EDI" plus a
            // grey block. E, D and I were absent from the source string, fell
            // through to synthesis and FreeType, and came out correct. The
            // character with the BEST available source produced the worst
            // output.
            //
            // Rejecting here is not a loss: glyphFromScanImpl's chain
            // (pre-synth -> on-demand synthesis -> FreeType + ScanStyleTransfer)
            // is exactly what already renders the other three correctly. An
            // unusable harvest should fall to a path that works rather than
            // outrank it.
            if (!maskCarriesInk(isolated))
                continue;

            // Width constraint: if the processed glyph is much wider than the
            // HOCR char bbox, the excess is contamination from adjacent chars.
            // Use a sliding-window to find the densest maxAllowedW-wide strip.
            // This preserves round character bowls (which have most ink density)
            // while trimming edge contamination from overlapping HOCR bboxes.
            // Skip when templates are available — NCC scoring + clipToTemplateWidth
            // handle contamination without destroying character shapes.
            if (!templates) {
                int charBboxW = static_cast<int>(ch.bbox.x1 - ch.bbox.x0);
                int maxAllowedW = std::max(4, static_cast<int>(charBboxW * 1.5f));
                if (isolated.width() > maxAllowedW) {
                    std::vector<int> colAlpha(isolated.width(), 0);
                    for (int y2 = 0; y2 < isolated.height(); ++y2) {
                        const QRgb* row = reinterpret_cast<const QRgb*>(
                            isolated.constScanLine(y2));
                        for (int x2 = 0; x2 < isolated.width(); ++x2)
                            colAlpha[x2] += qAlpha(row[x2]);
                    }
                    long long windowSum = 0;
                    for (int x2 = 0; x2 < maxAllowedW; ++x2)
                        windowSum += colAlpha[x2];
                    long long bestSum = windowSum;
                    int bestStart = 0;
                    for (int x2 = maxAllowedW; x2 < isolated.width(); ++x2) {
                        windowSum += colAlpha[x2] - colAlpha[x2 - maxAllowedW];
                        if (windowSum > bestSum) {
                            bestSum = windowSum;
                            bestStart = x2 - maxAllowedW + 1;
                        }
                    }
                    isolated = isolated.copy(bestStart, 0, maxAllowedW, isolated.height());
                }
            }

            if (templates) {
                // Collect candidate for NCC scoring after processing all chars.
                // Cap at 20 per codepoint to bound memory (each holds a QImage).
                constexpr int kMaxCandidates = 20;
                auto& candVec = candidates[ch.codepoint];
                if (static_cast<int>(candVec.size()) < kMaxCandidates)
                    candVec.push_back({std::move(isolated), effectiveConf});
            } else {
                // Original behavior: store immediately if confidence is best
                auto it = glyphs_.find(ch.codepoint);
                if (it != glyphs_.end() && it->second.confidence >= effectiveConf)
                    continue;

                GlyphEntry entry;
                entry.image = std::move(isolated);
                entry.confidence = effectiveConf;
                glyphs_[ch.codepoint] = std::move(entry);
            }
        }
    }

    // Template-based selection: score each candidate against the reference
    // template and pick the one with the highest NCC score.
    if (templates && !candidates.empty()) {
        int templateSelected = 0, confFallback = 0;
        for (auto& [cp, candList] : candidates) {
            if (candList.empty())
                continue;

            // When templates are available, allow donor lines to compete with
            // existing atlas entries via NCC comparison (lines 500-508 below).
            // The best-NCC crop wins, replacing contaminated earlier harvests.

            auto tmplIt = templates->find(cp);
            if (tmplIt != templates->end() && !tmplIt->second.isNull()) {
                // Score each candidate against the template AFTER clipping.
                // Contaminated crops (adjacent char ink from HOCR bbox overlap)
                // get poor NCC against clean templates. By clipping first, we
                // remove contamination before scoring, so the NCC reflects the
                // actual character quality rather than the contamination level.
                QImage tmplWhite = compositeOnWhite(tmplIt->second);

                float bestRawNcc = -1.0f;
                float bestClipNcc = -1.0f;
                float bestFinalNcc = -1.0f;
                int bestIdx = 0;
                bool bestUsedClipped = false;
                QImage bestClippedImage;

                for (int i = 0; i < static_cast<int>(candList.size()); ++i) {
                    QImage candWhite = compositeOnWhite(candList[i].image);
                    float rawNcc = ImageSimilarity::binarizedNCC(candWhite, tmplWhite);

                    // Clip to template width to remove adjacent-char contamination.
                    // Skip for very small templates (punctuation) where the NCC
                    // sliding window is unreliable.
                    float clipHR = static_cast<float>(candList[i].image.height()) /
                        static_cast<float>(std::max(1, tmplIt->second.height()));
                    int scaledTmplW = static_cast<int>(tmplIt->second.width() * clipHR);
                    QImage clipped;
                    float clipNcc = rawNcc;
                    if (scaledTmplW >= 4) {
                        clipped = clipToTemplateWidth(candList[i].image, tmplIt->second);
                        QImage clipWhite = compositeOnWhite(clipped);
                        clipNcc = ImageSimilarity::binarizedNCC(clipWhite, tmplWhite);
                    }

                    // Use whichever version (raw or clipped) scored better
                    float finalNcc = std::max(rawNcc, clipNcc);
                    if (finalNcc > bestFinalNcc) {
                        bestRawNcc = rawNcc;
                        bestClipNcc = clipNcc;
                        bestFinalNcc = finalNcc;
                        bestIdx = i;
                        bestUsedClipped = (clipNcc >= rawNcc) && !clipped.isNull();
                        bestClippedImage = bestUsedClipped ? std::move(clipped) : QImage();
                    }
                }

                // Only reject truly garbage crops (NCC near 0). The per-glyph
                // "replace-if-better" comparison (lines below) already ensures
                // the highest-quality crop wins across all donor lines, so the
                // threshold only needs to catch empty/noise images.
                constexpr float kMinGlyphNcc = 0.10f;
                if (bestFinalNcc < kMinGlyphNcc) {
                    qDebug() << "[GlyphAtlas] Template REJECT:"
                             << QString::fromUcs4(&cp, 1)
                             << "U+" << QString::number(cp, 16)
                             << "bestNcc=" << bestFinalNcc
                             << "(need >=" << kMinGlyphNcc << ")"
                             << "rawNcc=" << bestRawNcc
                             << "clipNcc=" << bestClipNcc
                             << "cropW=" << candList[bestIdx].image.width()
                             << "cropH=" << candList[bestIdx].image.height()
                             << "tmplW=" << tmplIt->second.width()
                             << "tmplH=" << tmplIt->second.height()
                             << "cands=" << candList.size();
                    continue;
                }

                // Pick the image that produced the better NCC score
                const QImage& bestImage = bestUsedClipped
                    ? bestClippedImage : candList[bestIdx].image;

                // Compare against existing atlas entry
                auto existIt = glyphs_.find(cp);
                if (existIt != glyphs_.end()) {
                    QImage existWhite = compositeOnWhite(existIt->second.image);
                    float existNcc = ImageSimilarity::binarizedNCC(
                        existWhite, tmplWhite);
                    if (existNcc >= bestFinalNcc)
                        continue;  // Existing is better
                }

                // Pad narrow crops to match template-derived character width.
                QImage padded = padToTemplateWidth(bestImage, tmplIt->second);

                qDebug() << "[GlyphAtlas] Template pick:"
                         << QString::fromUcs4(&cp, 1)
                         << "U+" << QString::number(cp, 16)
                         << "rawNcc=" << bestRawNcc
                         << "clipNcc=" << bestClipNcc
                         << "finalNcc=" << bestFinalNcc
                         << "usedClipped=" << bestUsedClipped
                         << "cropW=" << candList[bestIdx].image.width()
                         << "tmplW=" << tmplIt->second.width()
                         << "paddedW=" << padded.width()
                         << "cands=" << candList.size();

                GlyphEntry entry;
                entry.image = std::move(padded);
                entry.confidence = candList[bestIdx].confidence;
                entry.nccScore = bestFinalNcc;
                glyphs_[cp] = std::move(entry);
                ++templateSelected;
            } else {
                // No template for this codepoint: fall back to highest confidence
                int bestConf = -1;
                int bestIdx = 0;
                for (int i = 0; i < static_cast<int>(candList.size()); ++i) {
                    if (candList[i].confidence > bestConf) {
                        bestConf = candList[i].confidence;
                        bestIdx = i;
                    }
                }

                auto existIt = glyphs_.find(cp);
                if (existIt != glyphs_.end() && existIt->second.confidence >= bestConf)
                    continue;

                GlyphEntry entry;
                entry.image = std::move(candList[bestIdx].image);
                entry.confidence = bestConf;
                glyphs_[cp] = std::move(entry);
                ++confFallback;
            }
        }

        if (templateSelected > 0 || confFallback > 0) {
            qDebug() << "[GlyphAtlas] Template scoring:"
                     << templateSelected << "template-selected,"
                     << confFallback << "confidence-fallback,"
                     << "from" << candidates.size() << "codepoints";
        }
    }
}


void GlyphAtlas::build(const QImage& pageImage,
                        const std::vector<HocrLine>& lines,
                        const RgbColor& textColor,
                        const RgbColor& bgColor)
{
    glyphs_.clear();

    if (pageImage.isNull())
        return;

    for (const auto& line : lines)
        harvestLine(pageImage, line, textColor, bgColor);

    qDebug() << "[GlyphAtlas::build] glyphs=" << size()
             << "from" << lines.size() << "lines";
}


int GlyphAtlas::buildVerified(const QImage& pageImage,
                               const HocrLine& referenceLine,
                               const std::vector<HocrLine>& otherLines,
                               const RgbColor& textColor,
                               const RgbColor& bgColor,
                               const TemplateMap* templates)
{
    glyphs_.clear();

    if (pageImage.isNull())
        return 0;

    // Step 1: Harvest from reference line unconditionally
    harvestLine(pageImage, referenceLine, textColor, bgColor, false, templates);
    int refLineH = std::max(1, static_cast<int>(referenceLine.bbox.y1 - referenceLine.bbox.y0));

    // Step 1b: Remove NCC outliers from the reference line harvest.
    // After harvesting, some chars may have much lower NCC than the majority
    // (e.g. damaged/smudged/misrecognized glyphs). Remove them so they can be
    // filled by donors or synthesis instead of polluting the atlas.
    // Uses IQR (interquartile range) fence: discard if ncc < Q1 - 1.5*IQR.
    int refOutliers = 0;
    if (templates) {
        std::vector<float> refNccs;
        for (const auto& [cp, entry] : glyphs_) {
            if (entry.nccScore > 0.0f)
                refNccs.push_back(entry.nccScore);
        }
        if (refNccs.size() >= 4) {
            std::sort(refNccs.begin(), refNccs.end());
            float q1 = refNccs[refNccs.size() / 4];
            float q3 = refNccs[refNccs.size() * 3 / 4];
            float iqr = q3 - q1;
            float fence = q1 - 1.5f * iqr;

            // Erase glyphs below the fence
            for (auto it = glyphs_.begin(); it != glyphs_.end(); ) {
                if (it->second.nccScore > 0.0f && it->second.nccScore < fence) {
                    qDebug() << "[GlyphAtlas::buildVerified] Removing ref outlier:"
                             << QString::fromUcs4(&it->first, 1)
                             << "ncc=" << it->second.nccScore
                             << "fence=" << fence;
                    it = glyphs_.erase(it);
                    ++refOutliers;
                } else {
                    ++it;
                }
            }
        }
    }
    int refGlyphs = size();

    // Compute reference NCC floor from reference line glyphs (after outlier removal).
    // Donor chars must meet this threshold to ensure atlas quality is
    // at least as good as the reference line. Uses P25 (25th percentile)
    // as a generous floor that filters junk without being too strict.
    float refNccFloor = 0.0f;
    if (templates) {
        std::vector<float> refNccs;
        for (const auto& [cp, entry] : glyphs_) {
            if (entry.nccScore > 0.0f)
                refNccs.push_back(entry.nccScore);
        }
        if (!refNccs.empty()) {
            std::sort(refNccs.begin(), refNccs.end());
            refNccFloor = refNccs[refNccs.size() / 4];
            qDebug() << "[GlyphAtlas::buildVerified] refNccFloor=" << refNccFloor
                     << "from" << refNccs.size() << "scored glyphs"
                     << "outliers=" << refOutliers
                     << "(min=" << refNccs.front()
                     << "median=" << refNccs[refNccs.size() / 2]
                     << "max=" << refNccs.back() << ")";
        }
    }

    // Step 2: Verify and harvest from other lines
    int acceptedLines = 0;
    int rejectedLines = 0;
    int rejectedByVerifier = 0;
    int rejectedByHeight = 0;

    // File-based diagnostic logging for atlas build debugging
    // WIN-87 wider class (phase-60), acceptance criterion 2. This was
    // QStringLiteral("debug_log.txt") -- RELATIVE, so it landed in the
    // PROCESS CWD, which for a shell-launched PDFE is whatever directory
    // the user happened to be standing in: their source tree, a network
    // share, or C:\Windows\System32 for an elevated launch. It is now
    // anchored to the application-controlled per-process diagnostics root.
    const QString diagDir = QString::fromStdWString(
        pdfe::core::diagnosticsSubdir("pdfe_glyph_atlas").wstring());
    QFile diagFile(diagDir.isEmpty() ? QString() : diagDir + "/debug_log.txt");
    bool diagOpen = diagFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
    QTextStream diag(&diagFile);
    if (diagOpen) {
        diag << "=== GlyphAtlas::buildVerified DIAGNOSTICS ===\n";
        diag << "Reference line: bbox=("
             << referenceLine.bbox.x0 << "," << referenceLine.bbox.y0 << ","
             << referenceLine.bbox.x1 << "," << referenceLine.bbox.y1 << ")"
             << " height=" << refLineH << "\n";

        // Log reference line text
        QString refText;
        for (const auto& w : referenceLine.words) {
            for (const auto& ch : w.chars)
                refText += QString::fromUcs4(&ch.codepoint, 1);
            refText += ' ';
        }
        diag << "Reference line text: " << refText << "\n";
        diag << "Reference line xSizePx=" << referenceLine.xSizePx
             << " ascendersPx=" << referenceLine.ascendersPx
             << " descendersPx=" << referenceLine.descendersPx << "\n";
        diag << "Reference glyphs after harvest: " << size() << "\n";

        // Log reference glyph codepoints
        diag << "Reference codepoints: ";
        for (const auto& [cp, entry] : glyphs_)
            diag << QString::fromUcs4(&cp, 1) << "(U+" << QString::number(cp, 16) << ") ";
        diag << "\n";
        diag << "Templates: " << (templates ? "YES" : "NO") << "\n";
        diag << "refNccFloor=" << refNccFloor << "\n";
        diag << "Other lines to check: " << otherLines.size() << "\n\n";
    }

    for (int lineIdx = 0; lineIdx < static_cast<int>(otherLines.size()); ++lineIdx) {
        const auto& line = otherLines[lineIdx];
        auto result = LineFontVerifier::verify(pageImage, referenceLine, line);

        // Log every line's verification result
        int donorLineH = static_cast<int>(line.bbox.y1 - line.bbox.y0);
        float heightRatio = (refLineH > 0)
            ? static_cast<float>(donorLineH) / static_cast<float>(refLineH)
            : 1.0f;

        if (diagOpen) {
            QString lineText;
            for (const auto& w : line.words) {
                for (const auto& ch : w.chars)
                    lineText += QString::fromUcs4(&ch.codepoint, 1);
                lineText += ' ';
            }
            diag << "Line " << lineIdx << ": \"" << lineText.trimmed() << "\"\n";
            diag << "  bbox=(" << line.bbox.x0 << "," << line.bbox.y0 << ","
                 << line.bbox.x1 << "," << line.bbox.y1 << ")"
                 << " height=" << donorLineH
                 << " heightRatio=" << heightRatio << "\n";
            diag << "  xSizePx=" << line.xSizePx
                 << " ascendersPx=" << line.ascendersPx
                 << " descendersPx=" << line.descendersPx << "\n";
            diag << "  metricScore=" << result.metricScore
                 << " nccScore=" << result.nccScore
                 << " confidence=" << result.confidence
                 << " sharedChars=" << result.sharedChars
                 << " compatible=" << result.compatible << "\n";
        }

        if (result.compatible) {
            if (heightRatio < 0.75f || heightRatio > 1.35f) {
                ++rejectedLines;
                ++rejectedByHeight;
                if (diagOpen)
                    diag << "  -> REJECTED: height ratio " << heightRatio
                         << " outside [0.75, 1.35]\n\n";
                qDebug() << "[GlyphAtlas::buildVerified] Rejected line:"
                         << "heightRatio=" << heightRatio << "(need 0.75-1.35)";
                continue;
            }

            int beforeSize = size();
            harvestLine(pageImage, line, textColor, bgColor, /*addMissingOnly=*/true,
                        templates, refNccFloor);
            int newGlyphs = size() - beforeSize;
            ++acceptedLines;
            if (diagOpen)
                diag << "  -> ACCEPTED: +" << newGlyphs << " new glyphs\n\n";
            qDebug() << "[GlyphAtlas::buildVerified] Accepted line:"
                     << "+" << newGlyphs << "glyphs"
                     << "ncc=" << result.nccScore
                     << "metric=" << result.metricScore
                     << "shared=" << result.sharedChars;
        } else {
            ++rejectedLines;
            ++rejectedByVerifier;
            if (diagOpen)
                diag << "  -> REJECTED by LineFontVerifier: combined="
                     << result.confidence << " (need >= 0.65)\n\n";
        }
    }

    if (diagOpen) {
        diag << "\n=== SUMMARY ===\n";
        diag << "Total atlas glyphs: " << size() << "\n";
        diag << "Reference glyphs: " << refGlyphs << "\n";
        diag << "Accepted donor lines: " << acceptedLines << "\n";
        diag << "Rejected by LineFontVerifier: " << rejectedByVerifier << "\n";
        diag << "Rejected by height ratio: " << rejectedByHeight << "\n";
        diag << "Total rejected: " << rejectedLines << "\n";

        // Log all codepoints in the final atlas
        diag << "\nFinal atlas codepoints (" << size() << "): ";
        for (const auto& [cp, entry] : glyphs_)
            diag << QString::fromUcs4(&cp, 1) << "(ncc=" << entry.nccScore << ") ";
        diag << "\n";

        // Log all unique chars across ALL accepted lines (for comparison)
        std::set<char32_t> allPageChars;
        for (const auto& w : referenceLine.words)
            for (const auto& ch : w.chars)
                if (ch.codepoint != 0 && ch.codepoint != U' ')
                    allPageChars.insert(ch.codepoint);
        for (const auto& line : otherLines)
            for (const auto& w : line.words)
                for (const auto& ch : w.chars)
                    if (ch.codepoint != 0 && ch.codepoint != U' ')
                        allPageChars.insert(ch.codepoint);
        diag << "All page chars (" << allPageChars.size() << "): ";
        for (char32_t cp : allPageChars) {
            bool inAtlas = glyphs_.find(cp) != glyphs_.end();
            diag << QString::fromUcs4(&cp, 1) << (inAtlas ? "+" : "-") << " ";
        }
        diag << "\n";

        // Log missing chars that exist on accepted lines
        diag << "\nMissing chars from accepted lines:\n";
        for (char32_t cp : allPageChars) {
            if (glyphs_.find(cp) != glyphs_.end()) continue;
            // Find which lines have this char
            QStringList lines;
            for (int li = 0; li < static_cast<int>(otherLines.size()); ++li) {
                for (const auto& w : otherLines[li].words)
                    for (const auto& ch : w.chars)
                        if (ch.codepoint == cp) {
                            lines << QString::number(li);
                            goto nextLine;
                        }
                nextLine:;
            }
            if (!lines.isEmpty())
                diag << "  " << QString::fromUcs4(&cp, 1)
                     << " (U+" << QString::number(cp, 16) << ")"
                     << " on lines: " << lines.join(", ") << "\n";
        }

        diagFile.close();
    }

    // Step 3 removed: no fallback harvest from rejected lines.
    // Missing characters are filled by GlyphSynthesizer in EnrichedAtlas::build().
    // Synthesis produces geometrically correct glyphs from recipes; rejected-line
    // crops produce wrong-font or blurry glyphs that degrade visual quality.

    // Step 4: Baseline-aligned normalization to reference lineH.
    // Glyphs from donor/fallback lines have different heights (different line
    // bbox Y ranges) and different baseline positions. Simply scaling to refLineH
    // would stretch visible content to wrong vertical positions. Instead:
    // - Compute reference baseline (mode of visible-bottom-from-image-bottom)
    // - For smaller glyphs: pad to refLineH, aligning baselines
    // - For larger glyphs: scale down to refLineH
    if (refLineH > 1) {
        // Compute reference baseline from reference-height glyphs.
        // The "visible bottom from image bottom" for non-descender characters
        // corresponds to the descender space. The mode captures the baseline
        // position, since most characters sit on the baseline.
        std::map<int, int> baselineHist;
        for (const auto& [cp, entry] : glyphs_) {
            if (entry.image.isNull() || entry.image.height() != refLineH)
                continue;
            for (int y = entry.image.height() - 1; y >= 0; --y) {
                const QRgb* row = reinterpret_cast<const QRgb*>(
                    entry.image.constScanLine(y));
                bool hasVisible = false;
                for (int x = 0; x < entry.image.width(); ++x) {
                    if (qAlpha(row[x]) > 20) { hasVisible = true; break; }
                }
                if (hasVisible) {
                    baselineHist[entry.image.height() - 1 - y]++;
                    break;
                }
            }
        }
        int refBaselineFromBottom = refLineH / 6;  // fallback
        int maxBLCount = 0;
        for (const auto& [fb, cnt] : baselineHist) {
            if (cnt > maxBLCount) {
                maxBLCount = cnt;
                refBaselineFromBottom = fb;
            }
        }

        qDebug() << "[GlyphAtlas::buildVerified] refBaselineFromBottom="
                 << refBaselineFromBottom << "from" << baselineHist.size() << "ref glyphs";

        // Normalize non-refLineH glyphs
        for (auto& [cp, entry] : glyphs_) {
            if (entry.image.isNull() || entry.image.height() == refLineH)
                continue;

            if (entry.image.height() > refLineH) {
                // Larger than reference: scale down
                entry.image = entry.image.scaledToHeight(
                    refLineH, Qt::SmoothTransformation);
                continue;
            }

            // Smaller than reference: baseline-aligned padding.
            // Find this glyph's visible bottom position.
            int srcBaselineFromBottom = entry.image.height() / 6;
            for (int y = entry.image.height() - 1; y >= 0; --y) {
                const QRgb* row = reinterpret_cast<const QRgb*>(
                    entry.image.constScanLine(y));
                bool hasVisible = false;
                for (int x = 0; x < entry.image.width(); ++x) {
                    if (qAlpha(row[x]) > 20) { hasVisible = true; break; }
                }
                if (hasVisible) {
                    srcBaselineFromBottom = entry.image.height() - 1 - y;
                    break;
                }
            }

            // Place glyph in refLineH canvas so baselines align.
            // bottomPad = how much transparent space below the glyph
            int bottomPad = std::max(0, refBaselineFromBottom - srcBaselineFromBottom);
            int yOffset = refLineH - entry.image.height() - bottomPad;
            yOffset = std::max(0, yOffset);

            QImage canvas(entry.image.width(), refLineH,
                          QImage::Format_ARGB32_Premultiplied);
            canvas.fill(Qt::transparent);
            for (int y = 0; y < entry.image.height(); ++y) {
                int destY = yOffset + y;
                if (destY >= 0 && destY < refLineH) {
                    memcpy(canvas.scanLine(destY),
                           entry.image.constScanLine(y),
                           entry.image.bytesPerLine());
                }
            }
            entry.image = canvas;
        }
    }

    qDebug() << "[GlyphAtlas::buildVerified] total=" << size()
             << "refGlyphs=" << refGlyphs
             << "accepted=" << acceptedLines
             << "rejected=" << rejectedLines
             << "refLineH=" << refLineH;

    return acceptedLines;
}


void GlyphAtlas::mergeFrom(const GlyphAtlas& donor)
{
    for (const auto& [cp, entry] : donor.glyphs_) {
        // try_emplace inserts only if the key isn't already present;
        // saves the explicit find() lookup.
        glyphs_.try_emplace(cp, entry);
    }
}


void GlyphAtlas::addGlyph(char32_t codepoint, const QImage& isolatedImage, int confidence)
{
    if (isolatedImage.isNull() || codepoint == 0)
        return;

    auto it = glyphs_.find(codepoint);
    if (it != glyphs_.end() && it->second.confidence >= confidence)
        return;  // Existing entry has equal or higher confidence

    GlyphEntry entry;
    entry.image = isolatedImage;
    entry.confidence = confidence;
    glyphs_[codepoint] = std::move(entry);
}

void GlyphAtlas::replaceGlyphImage(char32_t codepoint, QImage image)
{
    auto it = glyphs_.find(codepoint);
    if (it != glyphs_.end())
        it->second.image = std::move(image);
}


QImage GlyphAtlas::glyph(char32_t codepoint) const
{
    auto it = glyphs_.find(codepoint);
    if (it != glyphs_.end())
        return it->second.image;
    return {};
}

bool GlyphAtlas::contains(char32_t codepoint) const
{
    return glyphs_.count(codepoint) > 0;
}

int GlyphAtlas::size() const
{
    return static_cast<int>(glyphs_.size());
}

float GlyphAtlas::coverage(const std::u32string& text) const
{
    if (text.empty())
        return 1.0f;

    // Count unique non-space characters needed
    std::set<char32_t> unique;
    for (char32_t c : text) {
        if (c != U' ' && c != U'\t' && c != U'\n' && c != U'\r')
            unique.insert(c);
    }

    if (unique.empty())
        return 1.0f;  // Only whitespace

    const int found = static_cast<int>(std::count_if(
        unique.begin(), unique.end(),
        [this](char32_t c) { return contains(c); }));

    return static_cast<float>(found) / static_cast<float>(unique.size());
}

std::vector<char32_t> GlyphAtlas::codepoints() const
{
    std::vector<char32_t> result;
    result.reserve(glyphs_.size());
    for (const auto& [cp, _] : glyphs_)
        result.push_back(cp);
    return result;
}

// ---------------------------------------------------------------------------
// C4: CIE Lab perceptual distance for alpha isolation
// ---------------------------------------------------------------------------
//
// Replaces Euclidean RGB distance with ΔE76 in CIE Lab color space.
// ΔE76 is perceptually more uniform than RGB: equal numeric
// differences correspond to approximately equal perceived color
// differences regardless of where in color space you are. For text
// isolation this gives cleaner alpha transitions on antialiased
// edges and on scans with colored paper, without requiring any
// per-region tuning.
//
// The conversion pipeline is sRGB → linear RGB → CIE XYZ (D65) →
// CIE Lab. Computed once per pixel. ~20 multiplications per pixel,
// so ~50 ns at typical glyph sizes — negligible compared to the
// outer loop cost.

namespace {

// sRGB gamma → linear conversion (IEC 61966-2-1).
inline float srgbToLinear(int c) {
    float cf = static_cast<float>(c) / 255.0f;
    return cf <= 0.04045f
               ? cf / 12.92f
               : std::pow((cf + 0.055f) / 1.055f, 2.4f);
}

struct LabColor {
    float L;
    float a;
    float b;
};

// sRGB → XYZ (D65 reference white).
inline LabColor rgbToLab(int r, int g, int b) {
    const float rl = srgbToLinear(r);
    const float gl = srgbToLinear(g);
    const float bl = srgbToLinear(b);

    // sRGB D65 → XYZ matrix (Bradford adapted).
    const float X = 0.4124564f * rl + 0.3575761f * gl + 0.1804375f * bl;
    const float Y = 0.2126729f * rl + 0.7151522f * gl + 0.0721750f * bl;
    const float Z = 0.0193339f * rl + 0.1191920f * gl + 0.9503041f * bl;

    // Normalize by D65 reference white.
    const float Xn = X / 0.95047f;
    const float Yn = Y / 1.00000f;
    const float Zn = Z / 1.08883f;

    auto f = [](float t) -> float {
        const float epsilon = 216.0f / 24389.0f;
        const float kappa = 24389.0f / 27.0f;
        return t > epsilon
                   ? std::cbrt(t)
                   : (kappa * t + 16.0f) / 116.0f;
    };

    const float fx = f(Xn);
    const float fy = f(Yn);
    const float fz = f(Zn);

    LabColor lab;
    lab.L = 116.0f * fy - 16.0f;
    lab.a = 500.0f * (fx - fy);
    lab.b = 200.0f * (fy - fz);
    return lab;
}

inline float deltaE76(const LabColor& a, const LabColor& b) {
    const float dL = a.L - b.L;
    const float da = a.a - b.a;
    const float db = a.b - b.b;
    return std::sqrt(dL * dL + da * da + db * db);
}

/// Morphological close on the alpha channel: dilate by 1 pixel
/// followed by erode by 1 pixel. Removes single-pixel noise gaps
/// in the glyph silhouette without changing the overall shape.
/// Operates on a standalone Grayscale8 mask to avoid disturbing
/// the color channels.
QImage morphologicalCloseAlpha(const QImage& src) {
    if (src.isNull() || src.width() < 3 || src.height() < 3) return src;
    const int W = src.width();
    const int H = src.height();

    // Extract alpha channel.
    std::vector<uint8_t> mask(static_cast<size_t>(W) * H, 0);
    for (int y = 0; y < H; ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        for (int x = 0; x < W; ++x) {
            mask[static_cast<size_t>(y) * W + x] =
                static_cast<uint8_t>(qAlpha(line[x]));
        }
    }

    // Dilate: max of 3x3 neighborhood.
    std::vector<uint8_t> dil(static_cast<size_t>(W) * H, 0);
    for (int y = 1; y < H - 1; ++y) {
        for (int x = 1; x < W - 1; ++x) {
            uint8_t m = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    uint8_t v = mask[static_cast<size_t>(y + dy) * W + (x + dx)];
                    if (v > m) m = v;
                }
            }
            dil[static_cast<size_t>(y) * W + x] = m;
        }
    }
    // Copy borders unchanged from original mask.
    for (int y = 0; y < H; ++y) {
        dil[static_cast<size_t>(y) * W + 0] = mask[static_cast<size_t>(y) * W + 0];
        dil[static_cast<size_t>(y) * W + (W - 1)] = mask[static_cast<size_t>(y) * W + (W - 1)];
    }
    for (int x = 0; x < W; ++x) {
        dil[x] = mask[x];
        dil[static_cast<size_t>(H - 1) * W + x] = mask[static_cast<size_t>(H - 1) * W + x];
    }

    // Erode: min of 3x3 neighborhood.
    std::vector<uint8_t> ero(static_cast<size_t>(W) * H, 0);
    for (int y = 1; y < H - 1; ++y) {
        for (int x = 1; x < W - 1; ++x) {
            uint8_t m = 255;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    uint8_t v = dil[static_cast<size_t>(y + dy) * W + (x + dx)];
                    if (v < m) m = v;
                }
            }
            ero[static_cast<size_t>(y) * W + x] = m;
        }
    }
    for (int y = 0; y < H; ++y) {
        ero[static_cast<size_t>(y) * W + 0] = dil[static_cast<size_t>(y) * W + 0];
        ero[static_cast<size_t>(y) * W + (W - 1)] = dil[static_cast<size_t>(y) * W + (W - 1)];
    }
    for (int x = 0; x < W; ++x) {
        ero[x] = dil[x];
        ero[static_cast<size_t>(H - 1) * W + x] = dil[static_cast<size_t>(H - 1) * W + x];
    }

    // Apply the closed mask back to the source, recomputing
    // premultiplied color channels from the RGB at the source
    // pixels (pre-isolation color survives).
    QImage out = src.copy();
    for (int y = 0; y < H; ++y) {
        QRgb* dst = reinterpret_cast<QRgb*>(out.scanLine(y));
        const QRgb* srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        for (int x = 0; x < W; ++x) {
            uint8_t newA = ero[static_cast<size_t>(y) * W + x];
            uint8_t oldA = qAlpha(srcLine[x]);
            if (newA == oldA) {
                dst[x] = srcLine[x];
                continue;
            }
            if (newA == 0) {
                dst[x] = qRgba(0, 0, 0, 0);
                continue;
            }
            // Un-premultiply from the old alpha, re-premultiply
            // with the new alpha. When oldA was 0 we fall back to
            // the neighbor's color, which we approximate as black
            // (the typical ink color) scaled by newA.
            if (oldA > 0) {
                int r = qRed(srcLine[x]) * 255 / oldA;
                int g = qGreen(srcLine[x]) * 255 / oldA;
                int b = qBlue(srcLine[x]) * 255 / oldA;
                dst[x] = qRgba(r * newA / 255, g * newA / 255, b * newA / 255, newA);
            } else {
                dst[x] = qRgba(0, 0, 0, newA);
            }
        }
    }
    return out;
}

} // anonymous namespace

bool GlyphAtlas::maskCarriesInkForTesting(const QImage& mask)
{
    return maskCarriesInk(mask);
}

QImage GlyphAtlas::isolateAlpha(const QImage& crop,
                                 const RgbColor& textColor,
                                 const RgbColor& bgColor)
{
    QImage result(crop.size(), QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);

    const QImage src = crop.convertToFormat(QImage::Format_ARGB32);

    // Convert target colors to Lab ONCE outside the pixel loop.
    const LabColor textLab = rgbToLab(textColor.r, textColor.g, textColor.b);
    const LabColor bgLab = rgbToLab(bgColor.r, bgColor.g, bgColor.b);

    for (int y = 0; y < src.height(); ++y) {
        const QRgb* srcLine = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        QRgb* dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));

        for (int x = 0; x < src.width(); ++x) {
            int pr = qRed(srcLine[x]);
            int pg = qGreen(srcLine[x]);
            int pb = qBlue(srcLine[x]);

            // C4: perceptually uniform ΔE76 distance in CIE Lab
            // instead of Euclidean RGB. See docs/image-pdf-risks.md.
            const LabColor pixLab = rgbToLab(pr, pg, pb);
            const float distToText = deltaE76(pixLab, textLab);
            const float distToBg = deltaE76(pixLab, bgLab);

            float totalDist = distToText + distToBg;
            int alpha = 0;
            if (totalDist > 0.001f) {
                alpha = std::clamp(static_cast<int>(255.0f * distToBg / totalDist), 0, 255);
            }

            // Premultiplied alpha: color channels scaled by alpha/255
            if (alpha > 0) {
                int r = textColor.r * alpha / 255;
                int g = textColor.g * alpha / 255;
                int b = textColor.b * alpha / 255;
                dstLine[x] = qRgba(r, g, b, alpha);
            } else {
                dstLine[x] = qRgba(0, 0, 0, 0);
            }
        }
    }

    // C4: morphological close on the alpha channel smooths
    // single-pixel noise in the glyph silhouette without changing
    // the overall shape.
    return morphologicalCloseAlpha(result);
}

} // namespace pdfe::tre
