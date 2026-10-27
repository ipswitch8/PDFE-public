#pragma once

#include "fonts/SubstituteFontSelector.h"
#include "tre/TreTypes.h"
#include <QImage>
#include <string>
#include <vector>

namespace pdfe::tre {

class BitmapCompositor;

/// Re-ranks font candidates by visual similarity to scanned text.
/// Uses BitmapCompositor to render diagnostic text with each candidate font,
/// then compares rendered bitmaps against the scanned region via NCC.
class VisualFontRanker {
public:
    struct RankedCandidate {
        pdfe::fonts::SubstituteResult candidate;
        float metricScore = 0.0f;     ///< Original distance score (lower = better)
        float visualScore = 0.0f;     ///< NCC similarity [0, 1] (higher = better)
        float combinedScore = 0.0f;   ///< Weighted combination (lower = better)
    };

    /// Rank font candidates by visual similarity to scanned text.
    /// @param scannedRegion Cropped image of the text region from the scanned page.
    /// @param candidates Font candidates from metric scoring (top N).
    /// @param diagnosticText Text to render for comparison (OCR plaintext).
    /// @param fontSizePt Detected font size in points.
    /// @param textColor Detected text color.
    /// @param compositor Optional BitmapCompositor to reuse; creates one internally if null.
    /// @param dpi Scan resolution.
    /// @return Candidates re-sorted by combined metric+visual score (best first).
    std::vector<RankedCandidate> rankByVisualSimilarity(
        const QImage& scannedRegion,
        const std::vector<pdfe::fonts::SubstituteResult>& candidates,
        const std::string& diagnosticText,
        float fontSizePt,
        const RgbColor& textColor,
        BitmapCompositor* compositor = nullptr,
        int dpi = 300
    ) const;
};

/// Move every candidate whose serif-ness DISAGREES with the query behind every
/// candidate that agrees, preserving each group's existing order.
///
/// WHY THIS IS A PARTITION AND NOT A BIGGER PENALTY (GitHub #249).
///
/// Serif-vs-sans is the most visually obvious property of a typeface, so a
/// match that inverts it reads as broken whatever the metrics say -- this was
/// reported from use, not found by a test. A penalty already existed
/// (`FontMetricProfile::distance()` adds 4.0) and was diluted twice over:
///
///   * `CNNFontRanker` normalises the metric by the LARGEST score in the
///     candidate set, so the penalty's influence on the final order is
///     `(4.0 / maxMetric) * metricWeight`. With a confident CNN
///     (metricWeight 0.3) and a maxMetric around 40 that is ~0.03 of combined
///     score, against a visual term spanning 0.7. A cosine-similarity
///     difference of 0.05 outvoted getting serif completely wrong.
///   * Because the divisor is the largest score IN THE SET, the same
///     (query, candidate) pair can rank differently depending only on what
///     else is in the list -- adding one distant candidate shrinks every
///     other candidate's metric contribution.
///
/// `FontMetricProfile.cpp` records this exact reasoning one layer down: the
/// categorical penalties were moved OUTSIDE the `sqrt` because being inside it
/// "dampened a 50-point script penalty to ~1.5 distance points". The same
/// dampening was happening again above it.
///
/// A partition rather than a larger number, because the property is
/// CATEGORICAL: there is no amount of visual similarity that should make a
/// serif face the right answer for sans-serif input. Ordering WITHIN each
/// group is untouched, so the existing combined score still decides which
/// serif-matching candidate wins.
///
/// @param ranked        Candidates, already sorted best-first. Reordered in place.
/// @param queryIsSerif  The scanned text's MEASURED serif-ness.
///
/// CALLERS MUST ONLY APPLY THIS WHEN THE QUERY'S SERIF-NESS WAS ACTUALLY
/// MEASURED. `FontMatcher::detectSerif()` returns false both for "measured as
/// sans" and for "could not measure" -- its own doc says "false if sans-serif
/// or indeterminate" -- so partitioning on an unmeasured value would silently
/// force sans-only results on any region whose serif-ness could not be
/// determined. See `ScannedTextProperties::serifDetermined`.
void demoteSerifMismatches(
    std::vector<VisualFontRanker::RankedCandidate>& ranked,
    bool queryIsSerif);

} // namespace pdfe::tre
