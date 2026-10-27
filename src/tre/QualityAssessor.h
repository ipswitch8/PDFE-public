#pragma once
#include "tre/TreTypes.h"

namespace pdfe::tre {

/// Evaluates OCR quality for a text region and recommends reconstruction
/// approach: Approach B (real text objects) if quality is high enough,
/// or Approach C (bitmap replacement) as fallback.
class QualityAssessor {
public:
    QualityAssessor() = default;
    explicit QualityAssessor(const QualityThresholds& thresholds);

    /// Evaluate a region for reconstruction quality.
    /// @param regionData          OCR text data with confidence values.
    /// @param bgInfo              Detected background classification.
    /// @param fontMatchConfidence Font matching confidence (0.0-1.0).
    /// @return Verdict with recommended approach, score, and warnings.
    QualityVerdict evaluate(const RegionTextData& regionData,
                            const BackgroundInfo& bgInfo,
                            float fontMatchConfidence) const;

    [[nodiscard]] const QualityThresholds& thresholds() const;
    void setThresholds(const QualityThresholds& thresholds);

private:
    QualityThresholds thresholds_;
};

} // namespace pdfe::tre
