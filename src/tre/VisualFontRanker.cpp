#include "tre/VisualFontRanker.h"
#include "tre/BitmapCompositor.h"
#include "tre/ImageSimilarity.h"
#include "tre/TreTypes.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace pdfe::tre {

std::vector<VisualFontRanker::RankedCandidate> VisualFontRanker::rankByVisualSimilarity(
    const QImage& scannedRegion,
    const std::vector<pdfe::fonts::SubstituteResult>& candidates,
    const std::string& diagnosticText,
    float fontSizePt,
    const RgbColor& textColor,
    BitmapCompositor* compositor,
    int dpi) const
{
    std::vector<RankedCandidate> ranked;

    if (candidates.empty() || scannedRegion.isNull())
        return ranked;

    // If no text to compare, return candidates as-is with metric scores only
    if (diagnosticText.empty()) {
        for (const auto& c : candidates) {
            RankedCandidate rc;
            rc.candidate = c;
            rc.metricScore = c.matchScore;
            rc.visualScore = 0.0f;
            rc.combinedScore = c.matchScore;
            ranked.push_back(std::move(rc));
        }
        return ranked;
    }

    // Convert diagnostic text to UTF-32 for BitmapCompositor
    const std::u32string u32Text = utf8ToU32(diagnosticText);
    if (u32Text.empty()) {
        for (const auto& c : candidates) {
            RankedCandidate rc;
            rc.candidate = c;
            rc.metricScore = c.matchScore;
            rc.visualScore = 0.0f;
            rc.combinedScore = c.matchScore;
            ranked.push_back(std::move(rc));
        }
        return ranked;
    }

    const int targetW = scannedRegion.width();
    const int targetH = scannedRegion.height();

    // Find max metric score for normalization
    float maxMetric = 0.0f;
    for (const auto& c : candidates) {
        if (c.matchScore > maxMetric)
            maxMetric = c.matchScore;
    }
    if (maxMetric < 1e-6f)
        maxMetric = 1.0f;  // Avoid division by zero

    // Render and compare each candidate
    // Use provided compositor or create a local fallback
    std::unique_ptr<BitmapCompositor> localCompositor;
    if (!compositor) {
        localCompositor = std::make_unique<BitmapCompositor>();
        compositor = localCompositor.get();
    }
    for (const auto& c : candidates) {
        RankedCandidate rc;
        rc.candidate = c;
        rc.metricScore = c.matchScore;

        // Render the diagnostic text with this candidate's font
        QImage rendered = compositor->renderText(
            u32Text,
            c.filePath,
            fontSizePt,
            textColor,
            targetW,
            targetH,
            dpi
        );

        if (!rendered.isNull()) {
            rc.visualScore = ImageSimilarity::binarizedNCC(scannedRegion, rendered);
        } else {
            rc.visualScore = 0.0f;
        }

        // Balanced scoring: metric distance provides structural correctness
        // (serif/weight/proportions), NCC provides visual similarity within
        // that structure.
        float metricNorm = c.matchScore / maxMetric;
        rc.combinedScore = 0.5f * (1.0f - rc.visualScore) + 0.5f * metricNorm;

        ranked.push_back(std::move(rc));
    }

    // Sort by combined score ascending (lower = better)
    std::sort(ranked.begin(), ranked.end(),
        [](const RankedCandidate& a, const RankedCandidate& b) {
            return a.combinedScore < b.combinedScore;
        });

    return ranked;
}

// ---------------------------------------------------------------------------
// demoteSerifMismatches — GitHub #249. See the header for why this is a
// partition rather than a penalty.
// ---------------------------------------------------------------------------

void demoteSerifMismatches(
    std::vector<VisualFontRanker::RankedCandidate>& ranked,
    bool queryIsSerif)
{
    // stable_partition, NOT sort or partition:
    //
    //   * it keeps the relative order of BOTH groups, and the input is already
    //     sorted best-first, so the existing combined score still decides the
    //     winner within each group -- the fix removes a bad answer without
    //     discarding the scoring that picks among the good ones;
    //   * plain std::partition gives no such guarantee and would scramble the
    //     ordering this function is supposed to preserve.
    std::stable_partition(
        ranked.begin(), ranked.end(),
        [queryIsSerif](const VisualFontRanker::RankedCandidate& r) {
            return r.candidate.profile.isSerif == queryIsSerif;
        });
}

} // namespace pdfe::tre
