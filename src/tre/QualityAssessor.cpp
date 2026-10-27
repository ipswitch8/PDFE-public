#include "QualityAssessor.h"
#include <algorithm>
#include <cmath>

namespace pdfe::tre {

QualityAssessor::QualityAssessor(const QualityThresholds& thresholds)
    : thresholds_(thresholds) {
}

QualityVerdict QualityAssessor::evaluate(const RegionTextData& regionData,
                                         const BackgroundInfo& bgInfo,
                                         float fontMatchConfidence) const {
    QualityVerdict verdict;

    // Signal 1: Mean word confidence (weight 0.30)
    float meanConf = regionData.meanWordConfidence();
    float confScore = std::clamp((meanConf - 50.0f) / 50.0f, 0.0f, 1.0f);
    float score = 0.30f * confScore;

    // Signal 2: Min word confidence (weight 0.15)
    float minConf = regionData.minWordConfidence();
    float minConfScore = (minConf >= 50.0f) ? 1.0f : std::max(0.0f, minConf / 50.0f);
    score += 0.15f * minConfScore;

    // Signal 3: Background uniformity (weight 0.20)
    float bgScore = bgInfo.isComplex ? 0.0f : (bgInfo.hasGradient ? 0.6f : 1.0f);
    score += 0.20f * bgScore;

    // Signal 4: Font match confidence (weight 0.15)
    score += 0.15f * std::clamp(fontMatchConfidence, 0.0f, 1.0f);

    // Signal 5: No non-text overlap (weight 0.10)
    bool isPureText = regionData.containsOnlyText();
    score += 0.10f * (isPureText ? 1.0f : 0.0f);

    // Signal 6: Region purity (weight 0.10)
    score += 0.10f * (isPureText ? 1.0f : 0.0f);

    // Store overall score
    verdict.overallScore = score;

    // Determine recommended approach
    if (score >= thresholds_.approachBMinScore) {
        verdict.recommendedApproach = Approach::B_HybridReconstruction;
    } else {
        verdict.recommendedApproach = Approach::C_ImageReplacement;
    }

    // Generate warnings
    if (meanConf < thresholds_.minMeanConfidence) {
        verdict.warnings.push_back({
            QualityWarningType::LowConfidence,
            "Mean word confidence (" + std::to_string(static_cast<int>(meanConf)) +
            ") is below threshold (" + std::to_string(thresholds_.minMeanConfidence) + ")"
        });
    }

    if (bgInfo.isComplex) {
        verdict.warnings.push_back({
            QualityWarningType::ComplexBackground,
            "Background is complex, may affect text reconstruction quality"
        });
    }

    if (fontMatchConfidence < thresholds_.minFontMatchConf) {
        verdict.warnings.push_back({
            QualityWarningType::PoorFontMatch,
            "Font match confidence (" + std::to_string(fontMatchConfidence) +
            ") is below threshold (" + std::to_string(thresholds_.minFontMatchConf) + ")"
        });
    }

    if (minConf < thresholds_.minWordConfidence) {
        verdict.warnings.push_back({
            QualityWarningType::MinConfidenceTooLow,
            "Minimum word confidence (" + std::to_string(static_cast<int>(minConf)) +
            ") is below threshold (" + std::to_string(thresholds_.minWordConfidence) + ")"
        });
    }

    if (bgInfo.hasGradient) {
        verdict.warnings.push_back({
            QualityWarningType::GradientBackground,
            "Background has gradient, may complicate reconstruction"
        });
    }

    if (!isPureText) {
        verdict.warnings.push_back({
            QualityWarningType::NonTextOverlap,
            "Region contains non-text elements or low-confidence words"
        });
    }

    return verdict;
}

const QualityThresholds& QualityAssessor::thresholds() const {
    return thresholds_;
}

void QualityAssessor::setThresholds(const QualityThresholds& thresholds) {
    thresholds_ = thresholds;
}

} // namespace pdfe::tre
