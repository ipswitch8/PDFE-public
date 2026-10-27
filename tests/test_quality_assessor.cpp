#include <gtest/gtest.h>
#include "tre/QualityAssessor.h"
#include "tre/TreTypes.h"
#include <algorithm>

using namespace pdfe::tre;

// ---------------------------------------------------------------------------
// Helper Functions
// ---------------------------------------------------------------------------

static RegionTextData makeRegionData(const std::vector<int>& wordConfidences) {
    RegionTextData data;
    data.pdfBbox = PdfRect{0, 0, 100, 50};
    data.pixelBbox = PixelRect{0, 0, 100, 50};

    HocrLine line;
    line.bbox = PixelRect{0, 0, 100, 20};
    for (int conf : wordConfidences) {
        HocrWord word;
        word.text = "word";
        word.bbox = PixelRect{0, 0, 50, 20};
        word.confidence = conf;
        word.fontSizePx = 14.0f;
        line.words.push_back(word);
    }
    data.lines.push_back(line);
    data.plainText = "test text";
    return data;
}

static BackgroundInfo makeUniformBg() {
    BackgroundInfo bg;
    bg.dominantColor = RgbColor{255, 255, 255};
    bg.isUniform = true;
    bg.hasGradient = false;
    bg.isComplex = false;
    bg.confidence = 0.9f;
    return bg;
}

static BackgroundInfo makeComplexBg() {
    BackgroundInfo bg;
    bg.dominantColor = RgbColor{128, 128, 128};
    bg.isUniform = false;
    bg.hasGradient = false;
    bg.isComplex = true;
    bg.confidence = 0.5f;
    return bg;
}

static BackgroundInfo makeGradientBg() {
    BackgroundInfo bg;
    bg.dominantColor = RgbColor{255, 255, 255};
    bg.isUniform = false;
    bg.hasGradient = true;
    bg.isComplex = false;
    bg.confidence = 0.8f;
    return bg;
}

static bool hasWarning(const QualityVerdict& v, QualityWarningType type) {
    return std::any_of(v.warnings.begin(), v.warnings.end(),
        [type](const QualityWarning& w) { return w.type == type; });
}

// ---------------------------------------------------------------------------
// Test Suite
// ---------------------------------------------------------------------------

TEST(QualityAssessorTest, HighQualitySelectsApproachB) {
    // Arrange
    QualityAssessor assessor;
    RegionTextData data = makeRegionData({95, 92, 98});
    BackgroundInfo bg = makeUniformBg();
    float fontConf = 0.9f;

    // Act
    QualityVerdict verdict = assessor.evaluate(data, bg, fontConf);

    // Assert
    EXPECT_EQ(verdict.recommendedApproach, Approach::B_HybridReconstruction);
    EXPECT_GT(verdict.overallScore, 0.65f);
    EXPECT_GE(verdict.overallScore, 0.85f); // Should be quite high with these inputs
}

TEST(QualityAssessorTest, LowConfidenceSelectsApproachC) {
    // Arrange
    QualityAssessor assessor;
    RegionTextData data = makeRegionData({40, 35, 30});
    BackgroundInfo bg = makeUniformBg();
    float fontConf = 0.9f;

    // Act
    QualityVerdict verdict = assessor.evaluate(data, bg, fontConf);

    // Assert
    EXPECT_EQ(verdict.recommendedApproach, Approach::C_ImageReplacement);
    EXPECT_TRUE(hasWarning(verdict, QualityWarningType::LowConfidence));
    EXPECT_TRUE(hasWarning(verdict, QualityWarningType::MinConfidenceTooLow));
}

TEST(QualityAssessorTest, ComplexBgSelectsApproachC) {
    // Arrange
    QualityAssessor assessor;
    RegionTextData data = makeRegionData({70});
    BackgroundInfo bg = makeComplexBg();
    float fontConf = 0.3f;

    // Act
    QualityVerdict verdict = assessor.evaluate(data, bg, fontConf);

    // Assert
    EXPECT_EQ(verdict.recommendedApproach, Approach::C_ImageReplacement);
    EXPECT_TRUE(hasWarning(verdict, QualityWarningType::ComplexBackground));
    // Complex background contributes 0.0 to bgScore (weight 0.20), dragging overall down
    EXPECT_LT(verdict.overallScore, 0.65f);
}

TEST(QualityAssessorTest, PoorFontMatchWarning) {
    // Arrange
    QualityAssessor assessor;
    RegionTextData data = makeRegionData({90});
    BackgroundInfo bg = makeUniformBg();
    float fontConf = 0.3f;

    // Act
    QualityVerdict verdict = assessor.evaluate(data, bg, fontConf);

    // Assert
    EXPECT_TRUE(hasWarning(verdict, QualityWarningType::PoorFontMatch));
}

TEST(QualityAssessorTest, GradientBgReducesScore) {
    // Arrange
    QualityAssessor assessor;
    RegionTextData data = makeRegionData({90});
    BackgroundInfo uniformBg = makeUniformBg();
    BackgroundInfo gradientBg = makeGradientBg();
    float fontConf = 0.8f;

    // Act
    QualityVerdict uniformVerdict = assessor.evaluate(data, uniformBg, fontConf);
    QualityVerdict gradientVerdict = assessor.evaluate(data, gradientBg, fontConf);

    // Assert
    EXPECT_LT(gradientVerdict.overallScore, uniformVerdict.overallScore);
    EXPECT_TRUE(hasWarning(gradientVerdict, QualityWarningType::GradientBackground));
    // Gradient bg contributes 0.6 vs uniform bg 1.0 (weight 0.20 = 0.08 score difference)
}

TEST(QualityAssessorTest, BorderlineScore) {
    // Arrange
    QualityAssessor assessor;
    // Carefully craft inputs to hit exactly 0.65 score
    // Target: 0.30*confScore + 0.15*minConfScore + 0.20*bgScore + 0.15*fontConf + 0.20*pureText = 0.65
    // Using: meanConf=75 → confScore=0.5 → 0.15
    //        minConf=75 → minConfScore=1.0 → 0.15
    //        uniform bg → bgScore=1.0 → 0.20
    //        fontConf=0.5 → 0.075
    //        pureText=true → 0.20
    // Total = 0.15 + 0.15 + 0.20 + 0.075 + 0.20 = 0.775 (too high)

    // Better calculation:
    // meanConf=75 → (75-50)/50 = 0.5 → 0.30*0.5 = 0.15
    // minConf=75 → 1.0 → 0.15*1.0 = 0.15
    // uniform bg → 1.0 → 0.20*1.0 = 0.20
    // fontConf=0.5 → 0.15*0.5 = 0.075
    // pureText → 0.10*1.0 + 0.10*1.0 = 0.20
    // Total = 0.15 + 0.15 + 0.20 + 0.075 + 0.20 = 0.775

    // To get 0.65:
    // meanConf=70 → (70-50)/50 = 0.4 → 0.30*0.4 = 0.12
    // minConf=70 → 1.0 → 0.15*1.0 = 0.15
    // uniform bg → 1.0 → 0.20*1.0 = 0.20
    // fontConf=0.6 → 0.15*0.6 = 0.09
    // pureText → 0.20
    // Total = 0.12 + 0.15 + 0.20 + 0.09 + 0.20 = 0.76

    // To get closer to 0.65:
    // meanConf=65 → (65-50)/50 = 0.3 → 0.30*0.3 = 0.09
    // minConf=60 → 1.0 → 0.15
    // uniform bg → 1.0 → 0.20
    // fontConf=0.55 → 0.15*0.55 = 0.0825
    // pureText → 0.20
    // Total = 0.09 + 0.15 + 0.20 + 0.0825 + 0.20 = 0.7225

    // Let's use gradient bg instead:
    // meanConf=75 → 0.15
    // minConf=75 → 0.15
    // gradient bg → 0.6 → 0.20*0.6 = 0.12
    // fontConf=0.75 → 0.15*0.75 = 0.1125
    // pureText → 0.20
    // Total = 0.15 + 0.15 + 0.12 + 0.1125 + 0.20 = 0.7325

    // Try:
    // meanConf=70 → 0.12
    // minConf=70 → 0.15
    // gradient bg → 0.12
    // fontConf=0.65 → 0.15*0.65 = 0.0975
    // pureText → 0.20
    // Total = 0.12 + 0.15 + 0.12 + 0.0975 + 0.20 = 0.6875

    // Try:
    // meanConf=68 → (68-50)/50 = 0.36 → 0.108
    // minConf=68 → 0.15
    // gradient bg → 0.12
    // fontConf=0.62 → 0.093
    // pureText → 0.20
    // Total = 0.108 + 0.15 + 0.12 + 0.093 + 0.20 = 0.671

    // Very close, let's use meanConf=67:
    // meanConf=67 → (67-50)/50 = 0.34 → 0.102
    // minConf=67 → 0.15
    // gradient bg → 0.12
    // fontConf=0.64 → 0.096
    // pureText → 0.20
    // Total = 0.102 + 0.15 + 0.12 + 0.096 + 0.20 = 0.668

    // Try meanConf=66:
    // meanConf=66 → (66-50)/50 = 0.32 → 0.096
    // minConf=66 → 0.15
    // gradient bg → 0.12
    // fontConf=0.67 → 0.1005
    // pureText → 0.20
    // Total = 0.096 + 0.15 + 0.12 + 0.1005 + 0.20 = 0.6665

    // Very close! Let's use meanConf=65.5 (will be 65.5):
    // meanConf=65.5 → (65.5-50)/50 = 0.31 → 0.093
    // minConf=65 → 0.15
    // gradient bg → 0.12
    // fontConf=0.685 → 0.10275
    // pureText → 0.20
    // Total = 0.093 + 0.15 + 0.12 + 0.10275 + 0.20 = 0.66575

    // Let's just aim for very close to 0.65:
    RegionTextData data = makeRegionData({66, 66, 66});
    BackgroundInfo bg = makeGradientBg();
    float fontConf = 0.67f;

    // Act
    QualityVerdict verdict = assessor.evaluate(data, bg, fontConf);

    // Assert
    EXPECT_EQ(verdict.recommendedApproach, Approach::B_HybridReconstruction);
    EXPECT_GE(verdict.overallScore, 0.65f);
    EXPECT_LE(verdict.overallScore, 0.68f); // Should be close to threshold
}

TEST(QualityAssessorTest, CustomThresholdsRespected) {
    // Arrange
    QualityThresholds customThresholds;
    customThresholds.approachBMinScore = 0.80f; // Higher threshold
    customThresholds.minMeanConfidence = 80;
    customThresholds.minWordConfidence = 50;
    customThresholds.minBgUniformity = 0.7f;
    customThresholds.minFontMatchConf = 0.5f;

    QualityAssessor assessor(customThresholds);

    // Use same high-quality input as test 1, but with higher threshold
    RegionTextData data = makeRegionData({95, 92, 98});
    BackgroundInfo bg = makeUniformBg();
    float fontConf = 0.9f;

    // Act
    QualityVerdict verdict = assessor.evaluate(data, bg, fontConf);

    // Assert
    // With default threshold (0.65), this would be Approach B
    // With custom threshold (0.80), it should still be B (score is ~0.89)
    EXPECT_EQ(verdict.recommendedApproach, Approach::B_HybridReconstruction);
    EXPECT_GE(verdict.overallScore, 0.80f);

    // Now test with a case that would pass 0.65 but not 0.80
    RegionTextData mediumData = makeRegionData({65, 65, 65});
    QualityVerdict mediumVerdict = assessor.evaluate(mediumData, bg, fontConf);

    // This should fail the 0.80 threshold
    // Score: 0.30*(15/50) + 0.15*1.0 + 0.20*1.0 + 0.15*0.9 + 0.20*1.0 = 0.775
    EXPECT_LT(mediumVerdict.overallScore, 0.80f);
    EXPECT_EQ(mediumVerdict.recommendedApproach, Approach::C_ImageReplacement);
}

TEST(QualityAssessorTest, MultipleWarnings) {
    // Arrange
    QualityAssessor assessor;
    RegionTextData data = makeRegionData({40, 35, 30}); // Low confidence
    BackgroundInfo bg = makeComplexBg(); // Complex background
    float fontConf = 0.9f;

    // Act
    QualityVerdict verdict = assessor.evaluate(data, bg, fontConf);

    // Assert
    EXPECT_TRUE(hasWarning(verdict, QualityWarningType::LowConfidence));
    EXPECT_TRUE(hasWarning(verdict, QualityWarningType::ComplexBackground));
    EXPECT_TRUE(hasWarning(verdict, QualityWarningType::MinConfidenceTooLow));
    EXPECT_GE(verdict.warnings.size(), 3u);
}

TEST(QualityAssessorTest, PerfectScore) {
    // Arrange
    QualityAssessor assessor;
    // All maximal inputs: high confidence, uniform bg, perfect font match
    RegionTextData data = makeRegionData({100, 100, 100});
    BackgroundInfo bg = makeUniformBg();
    float fontConf = 1.0f;

    // Act
    QualityVerdict verdict = assessor.evaluate(data, bg, fontConf);

    // Assert
    EXPECT_EQ(verdict.recommendedApproach, Approach::B_HybridReconstruction);
    EXPECT_GE(verdict.overallScore, 0.95f); // Should be near perfect
    // Calculation:
    // meanConf=100 → (100-50)/50 = 1.0 → 0.30
    // minConf=100 → 1.0 → 0.15
    // uniform bg → 1.0 → 0.20
    // fontConf=1.0 → 0.15
    // pureText → 0.20
    // Total = 1.0
    EXPECT_FLOAT_EQ(verdict.overallScore, 1.0f);
    EXPECT_TRUE(verdict.warnings.empty()); // No warnings for perfect input
}

TEST(QualityAssessorTest, MinConfidenceWarning) {
    // Arrange
    QualityAssessor assessor;
    // High overall, but one word below threshold
    RegionTextData data = makeRegionData({90, 90, 40});
    BackgroundInfo bg = makeUniformBg();
    float fontConf = 0.9f;

    // Act
    QualityVerdict verdict = assessor.evaluate(data, bg, fontConf);

    // Assert
    EXPECT_TRUE(hasWarning(verdict, QualityWarningType::MinConfidenceTooLow));
    // Mean is (90+90+40)/3 = 73.33, which is below 80
    EXPECT_TRUE(hasWarning(verdict, QualityWarningType::LowConfidence));
}

TEST(QualityAssessorTest, NonTextOverlapWarning) {
    // Arrange
    QualityAssessor assessor;
    // Create data with zero-confidence word (non-text)
    RegionTextData data = makeRegionData({90, 0, 85});
    BackgroundInfo bg = makeUniformBg();
    float fontConf = 0.9f;

    // Act
    QualityVerdict verdict = assessor.evaluate(data, bg, fontConf);

    // Assert
    EXPECT_TRUE(hasWarning(verdict, QualityWarningType::NonTextOverlap));
    // containsOnlyText() will return false because one word has confidence <= 0
}

TEST(QualityAssessorTest, GetAndSetThresholds) {
    // Arrange
    QualityAssessor assessor;
    QualityThresholds customThresholds;
    customThresholds.approachBMinScore = 0.75f;
    customThresholds.minMeanConfidence = 85;

    // Act
    assessor.setThresholds(customThresholds);
    const QualityThresholds& retrieved = assessor.thresholds();

    // Assert
    EXPECT_FLOAT_EQ(retrieved.approachBMinScore, 0.75f);
    EXPECT_EQ(retrieved.minMeanConfidence, 85);
}
