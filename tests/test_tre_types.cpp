#include <gtest/gtest.h>
#include "tre/TreTypes.h"

using namespace pdfe::tre;

// =============================================================================
// PixelRect Tests
// =============================================================================

TEST(TreTypes_PixelRect, Width) {
    PixelRect rect{10.0, 20.0, 50.0, 80.0};
    EXPECT_DOUBLE_EQ(40.0, rect.width());
}

TEST(TreTypes_PixelRect, Height) {
    PixelRect rect{10.0, 20.0, 50.0, 80.0};
    EXPECT_DOUBLE_EQ(60.0, rect.height());
}

TEST(TreTypes_PixelRect, Area) {
    PixelRect rect{10.0, 20.0, 50.0, 80.0};
    EXPECT_DOUBLE_EQ(2400.0, rect.area());
}

TEST(TreTypes_PixelRect, AreaZero) {
    PixelRect rect{10.0, 20.0, 10.0, 20.0};
    EXPECT_DOUBLE_EQ(0.0, rect.area());
}

TEST(TreTypes_PixelRect, Contains_PointInside) {
    PixelRect rect{10.0, 20.0, 50.0, 80.0};
    PixelPoint pt{30.0, 50.0};
    EXPECT_TRUE(rect.contains(pt));
}

TEST(TreTypes_PixelRect, Contains_PointOnEdge) {
    PixelRect rect{10.0, 20.0, 50.0, 80.0};
    PixelPoint pt{10.0, 20.0};
    EXPECT_TRUE(rect.contains(pt));
}

TEST(TreTypes_PixelRect, Contains_PointOutside) {
    PixelRect rect{10.0, 20.0, 50.0, 80.0};
    PixelPoint pt{5.0, 15.0};
    EXPECT_FALSE(rect.contains(pt));
}

TEST(TreTypes_PixelRect, Intersects_Overlapping) {
    PixelRect rect1{10.0, 20.0, 50.0, 80.0};
    PixelRect rect2{30.0, 40.0, 70.0, 100.0};
    EXPECT_TRUE(rect1.intersects(rect2));
    EXPECT_TRUE(rect2.intersects(rect1));
}

TEST(TreTypes_PixelRect, Intersects_NonOverlapping) {
    PixelRect rect1{10.0, 20.0, 50.0, 80.0};
    PixelRect rect2{60.0, 90.0, 100.0, 120.0};
    EXPECT_FALSE(rect1.intersects(rect2));
    EXPECT_FALSE(rect2.intersects(rect1));
}

TEST(TreTypes_PixelRect, Intersection_Overlapping) {
    PixelRect rect1{10.0, 20.0, 50.0, 80.0};
    PixelRect rect2{30.0, 40.0, 70.0, 100.0};
    PixelRect result = rect1.intersection(rect2);

    EXPECT_DOUBLE_EQ(30.0, result.x0);
    EXPECT_DOUBLE_EQ(40.0, result.y0);
    EXPECT_DOUBLE_EQ(50.0, result.x1);
    EXPECT_DOUBLE_EQ(80.0, result.y1);
}

TEST(TreTypes_PixelRect, Intersection_NonOverlapping) {
    PixelRect rect1{10.0, 20.0, 50.0, 80.0};
    PixelRect rect2{60.0, 90.0, 100.0, 120.0};
    PixelRect result = rect1.intersection(rect2);

    // Non-overlapping rects return zero-area rect
    EXPECT_DOUBLE_EQ(0.0, result.area());
}

TEST(TreTypes_PixelRect, Center) {
    PixelRect rect{10.0, 20.0, 50.0, 80.0};
    PixelPoint center = rect.center();

    EXPECT_DOUBLE_EQ(30.0, center.x);
    EXPECT_DOUBLE_EQ(50.0, center.y);
}

// =============================================================================
// PdfRect Tests
// =============================================================================

TEST(TreTypes_PdfRect, Width) {
    PdfRect rect{10.0, 20.0, 50.0, 80.0};
    EXPECT_DOUBLE_EQ(40.0, rect.width());
}

TEST(TreTypes_PdfRect, Height) {
    PdfRect rect{10.0, 20.0, 50.0, 80.0};
    EXPECT_DOUBLE_EQ(60.0, rect.height());
}

TEST(TreTypes_PdfRect, Area) {
    PdfRect rect{10.0, 20.0, 50.0, 80.0};
    EXPECT_DOUBLE_EQ(2400.0, rect.area());
}

TEST(TreTypes_PdfRect, AreaZero) {
    PdfRect rect{15.0, 25.0, 15.0, 25.0};
    EXPECT_DOUBLE_EQ(0.0, rect.area());
}

TEST(TreTypes_PdfRect, Contains_PointInside) {
    PdfRect rect{10.0, 20.0, 50.0, 80.0};
    PdfPoint pt{30.0, 50.0};
    EXPECT_TRUE(rect.contains(pt));
}

TEST(TreTypes_PdfRect, Contains_PointOutside) {
    PdfRect rect{10.0, 20.0, 50.0, 80.0};
    PdfPoint pt{100.0, 150.0};
    EXPECT_FALSE(rect.contains(pt));
}

TEST(TreTypes_PdfRect, Intersects_Overlapping) {
    PdfRect rect1{10.0, 20.0, 50.0, 80.0};
    PdfRect rect2{30.0, 40.0, 70.0, 100.0};
    EXPECT_TRUE(rect1.intersects(rect2));
}

TEST(TreTypes_PdfRect, Intersects_NonOverlapping) {
    PdfRect rect1{10.0, 20.0, 50.0, 80.0};
    PdfRect rect2{60.0, 90.0, 100.0, 120.0};
    EXPECT_FALSE(rect1.intersects(rect2));
}

TEST(TreTypes_PdfRect, Intersection_Overlapping) {
    PdfRect rect1{10.0, 20.0, 50.0, 80.0};
    PdfRect rect2{30.0, 40.0, 70.0, 100.0};
    PdfRect result = rect1.intersection(rect2);

    EXPECT_DOUBLE_EQ(30.0, result.x0);
    EXPECT_DOUBLE_EQ(40.0, result.y0);
    EXPECT_DOUBLE_EQ(50.0, result.x1);
    EXPECT_DOUBLE_EQ(80.0, result.y1);
}

TEST(TreTypes_PdfRect, Intersection_NonOverlapping) {
    PdfRect rect1{10.0, 20.0, 50.0, 80.0};
    PdfRect rect2{100.0, 150.0, 200.0, 250.0};
    PdfRect result = rect1.intersection(rect2);

    EXPECT_DOUBLE_EQ(0.0, result.area());
}

TEST(TreTypes_PdfRect, Center) {
    PdfRect rect{10.0, 20.0, 50.0, 80.0};
    PdfPoint center = rect.center();

    EXPECT_DOUBLE_EQ(30.0, center.x);
    EXPECT_DOUBLE_EQ(50.0, center.y);
}

// =============================================================================
// RegionTextData Tests
// =============================================================================

TEST(TreTypes_RegionTextData, MeanWordConfidence_EmptyLines) {
    RegionTextData data;
    EXPECT_FLOAT_EQ(0.0f, data.meanWordConfidence());
}

TEST(TreTypes_RegionTextData, MeanWordConfidence_SingleWord) {
    RegionTextData data;
    HocrLine line;
    HocrWord word;
    word.text = "Hello";
    word.confidence = 85;
    line.words.push_back(word);
    data.lines.push_back(line);

    EXPECT_FLOAT_EQ(85.0f, data.meanWordConfidence());
}

TEST(TreTypes_RegionTextData, MeanWordConfidence_MultipleWords) {
    RegionTextData data;
    HocrLine line1;
    HocrWord word1;
    word1.text = "Hello";
    word1.confidence = 90;
    HocrWord word2;
    word2.text = "World";
    word2.confidence = 80;
    line1.words.push_back(word1);
    line1.words.push_back(word2);

    HocrLine line2;
    HocrWord word3;
    word3.text = "Test";
    word3.confidence = 70;
    line2.words.push_back(word3);

    data.lines.push_back(line1);
    data.lines.push_back(line2);

    // (90 + 80 + 70) / 3 = 80.0
    EXPECT_FLOAT_EQ(80.0f, data.meanWordConfidence());
}

TEST(TreTypes_RegionTextData, MinWordConfidence_EmptyLines) {
    RegionTextData data;
    EXPECT_FLOAT_EQ(100.0f, data.minWordConfidence());
}

TEST(TreTypes_RegionTextData, MinWordConfidence_SingleWord) {
    RegionTextData data;
    HocrLine line;
    HocrWord word;
    word.text = "Hello";
    word.confidence = 75;
    line.words.push_back(word);
    data.lines.push_back(line);

    EXPECT_FLOAT_EQ(75.0f, data.minWordConfidence());
}

TEST(TreTypes_RegionTextData, MinWordConfidence_MultipleWords) {
    RegionTextData data;
    HocrLine line1;
    HocrWord word1;
    word1.text = "Hello";
    word1.confidence = 90;
    HocrWord word2;
    word2.text = "World";
    word2.confidence = 55;
    line1.words.push_back(word1);
    line1.words.push_back(word2);

    HocrLine line2;
    HocrWord word3;
    word3.text = "Test";
    word3.confidence = 70;
    line2.words.push_back(word3);

    data.lines.push_back(line1);
    data.lines.push_back(line2);

    EXPECT_FLOAT_EQ(55.0f, data.minWordConfidence());
}

TEST(TreTypes_RegionTextData, ContainsOnlyText_EmptyLines) {
    RegionTextData data;
    EXPECT_TRUE(data.containsOnlyText());
}

TEST(TreTypes_RegionTextData, ContainsOnlyText_AllPositiveConfidence) {
    RegionTextData data;
    HocrLine line;
    HocrWord word1;
    word1.text = "Hello";
    word1.confidence = 90;
    HocrWord word2;
    word2.text = "World";
    word2.confidence = 50;
    line.words.push_back(word1);
    line.words.push_back(word2);
    data.lines.push_back(line);

    EXPECT_TRUE(data.containsOnlyText());
}

TEST(TreTypes_RegionTextData, ContainsOnlyText_WithZeroConfidence) {
    RegionTextData data;
    HocrLine line;
    HocrWord word1;
    word1.text = "Hello";
    word1.confidence = 90;
    HocrWord word2;
    word2.text = "???";
    word2.confidence = 0;
    line.words.push_back(word1);
    line.words.push_back(word2);
    data.lines.push_back(line);

    EXPECT_FALSE(data.containsOnlyText());
}

TEST(TreTypes_RegionTextData, ContainsOnlyText_WithNegativeConfidence) {
    RegionTextData data;
    HocrLine line;
    HocrWord word1;
    word1.text = "Hello";
    word1.confidence = 90;
    HocrWord word2;
    word2.text = "Bad";
    word2.confidence = -5;
    line.words.push_back(word1);
    line.words.push_back(word2);
    data.lines.push_back(line);

    EXPECT_FALSE(data.containsOnlyText());
}

// =============================================================================
// Default Values Tests
// =============================================================================

TEST(TreTypes_DefaultValues, QualityThresholds) {
    QualityThresholds thresholds;
    EXPECT_FLOAT_EQ(0.65f, thresholds.approachBMinScore);
    EXPECT_EQ(80, thresholds.minMeanConfidence);
    EXPECT_EQ(50, thresholds.minWordConfidence);
    EXPECT_FLOAT_EQ(0.7f, thresholds.minBgUniformity);
    EXPECT_FLOAT_EQ(0.5f, thresholds.minFontMatchConf);
}

TEST(TreTypes_DefaultValues, QualityVerdict) {
    QualityVerdict verdict;
    EXPECT_EQ(Approach::C_ImageReplacement, verdict.recommendedApproach);
    EXPECT_FLOAT_EQ(0.0f, verdict.overallScore);
    EXPECT_TRUE(verdict.warnings.empty());
}

TEST(TreTypes_DefaultValues, BackgroundInfo) {
    BackgroundInfo bgInfo;
    EXPECT_FALSE(bgInfo.isUniform);
    EXPECT_FALSE(bgInfo.hasGradient);
    EXPECT_FALSE(bgInfo.isComplex);
    EXPECT_FLOAT_EQ(0.0f, bgInfo.confidence);
}

TEST(TreTypes_DefaultValues, OcrConfig) {
    OcrConfig config;
    EXPECT_EQ("eng", config.language);
    EXPECT_TRUE(config.tessdataPath.empty());
    EXPECT_EQ(300, config.ocrDpi);
    EXPECT_FALSE(config.useGpu);
    EXPECT_EQ(3, config.pageSegMode);
}

TEST(TreTypes_DefaultValues, ScannedTextProperties) {
    ScannedTextProperties props;
    EXPECT_FLOAT_EQ(0.0f, props.fontSizePt);
    EXPECT_EQ(FontWeight::Regular, props.weight);
    EXPECT_FALSE(props.isItalic);
    EXPECT_FALSE(props.isSerif);
    EXPECT_FLOAT_EQ(1.2f, props.lineSpacingRatio);
    EXPECT_FLOAT_EQ(0.0f, props.xHeightRatio);
    EXPECT_FLOAT_EQ(0.0f, props.ascenderRatio);
    EXPECT_FLOAT_EQ(0.0f, props.descenderRatio);
}

TEST(TreTypes_DefaultValues, FontMatch) {
    FontMatch match;
    EXPECT_TRUE(match.fontFamily.empty());
    EXPECT_TRUE(match.fontStyle.empty());
    EXPECT_TRUE(match.fontFilePath.empty());
    EXPECT_EQ(0, match.fontFaceIndex);
    EXPECT_FLOAT_EQ(0.0f, match.sizePt);
    EXPECT_FLOAT_EQ(1.2f, match.lineSpacingRatio);
    EXPECT_FLOAT_EQ(0.0f, match.confidence);
}

TEST(TreTypes_DefaultValues, RgbColor) {
    RgbColor color;
    EXPECT_EQ(0, color.r);
    EXPECT_EQ(0, color.g);
    EXPECT_EQ(0, color.b);
}

TEST(TreTypes_DefaultValues, RgbaColor) {
    RgbaColor color;
    EXPECT_EQ(0, color.r);
    EXPECT_EQ(0, color.g);
    EXPECT_EQ(0, color.b);
    EXPECT_EQ(255, color.a);
}

TEST(TreTypes_DefaultValues, HocrWord) {
    HocrWord word;
    EXPECT_TRUE(word.text.empty());
    EXPECT_EQ(0, word.confidence);
    EXPECT_FLOAT_EQ(0.0f, word.fontSizePx);
}

TEST(TreTypes_DefaultValues, AffineTransform) {
    AffineTransform transform;
    EXPECT_DOUBLE_EQ(1.0, transform.scaleX);
    EXPECT_DOUBLE_EQ(1.0, transform.scaleY);
    EXPECT_DOUBLE_EQ(0.0, transform.translateX);
    EXPECT_DOUBLE_EQ(0.0, transform.translateY);
    EXPECT_FALSE(transform.flipY);
    EXPECT_EQ(0, transform.rotation);
    EXPECT_DOUBLE_EQ(0.0, transform.imageWidth);
    EXPECT_DOUBLE_EQ(0.0, transform.imageHeight);
    EXPECT_DOUBLE_EQ(0.0, transform.pageWidth);
    EXPECT_DOUBLE_EQ(0.0, transform.pageHeight);
}
