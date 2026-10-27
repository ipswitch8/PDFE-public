// test_high_contrast.cpp
// Unit tests for AccessibilityHelper

#include <gtest/gtest.h>
#include "ui/AccessibilityHelper.h"
#include <QColor>

using namespace pdfe::ui;

// Test fixture for AccessibilityHelper tests
class AccessibilityHelperTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup before each test if needed
    }

    void TearDown() override {
        // Cleanup after each test if needed
    }
};

// Test 1: Verify contrast ratio between black and white is approximately 21.0
TEST_F(AccessibilityHelperTest, ContrastRatio_BlackWhite) {
    QColor black(Qt::black);
    QColor white(Qt::white);

    double ratio = AccessibilityHelper::contrastRatio(black, white);

    EXPECT_NEAR(21.0, ratio, 0.1);
}

// Test 2: Verify contrast ratio of identical colors is 1.0
TEST_F(AccessibilityHelperTest, ContrastRatio_Identical) {
    QColor red(Qt::red);

    double ratio = AccessibilityHelper::contrastRatio(red, red);

    EXPECT_DOUBLE_EQ(1.0, ratio);
}

// Test 3: Verify contrast ratio is symmetric
TEST_F(AccessibilityHelperTest, ContrastRatio_Symmetric) {
    QColor color1(100, 150, 200);
    QColor color2(200, 100, 50);

    double ratio1 = AccessibilityHelper::contrastRatio(color1, color2);
    double ratio2 = AccessibilityHelper::contrastRatio(color2, color1);

    EXPECT_DOUBLE_EQ(ratio1, ratio2);

    // Test with different color pairs
    QColor blue(Qt::blue);
    QColor yellow(Qt::yellow);

    double ratioA = AccessibilityHelper::contrastRatio(blue, yellow);
    double ratioB = AccessibilityHelper::contrastRatio(yellow, blue);

    EXPECT_DOUBLE_EQ(ratioA, ratioB);
}

// Test 4: Verify contrast ratio is always within valid range [1.0, 21.0]
TEST_F(AccessibilityHelperTest, ContrastRatio_Range) {
    // Test various color combinations
    std::vector<std::pair<QColor, QColor>> colorPairs = {
        {QColor(Qt::black), QColor(Qt::white)},
        {QColor(Qt::red), QColor(Qt::blue)},
        {QColor(Qt::green), QColor(Qt::yellow)},
        {QColor(128, 128, 128), QColor(255, 255, 255)},
        {QColor(0, 0, 0), QColor(128, 128, 128)},
        {QColor(50, 100, 150), QColor(200, 150, 100)},
        {QColor(255, 0, 0), QColor(0, 255, 0)},
        {QColor(100, 100, 100), QColor(110, 110, 110)}
    };

    for (const auto& pair : colorPairs) {
        double ratio = AccessibilityHelper::contrastRatio(pair.first, pair.second);

        EXPECT_GE(ratio, 1.0) << "Contrast ratio should be >= 1.0 for colors ("
                              << pair.first.name().toStdString() << ", "
                              << pair.second.name().toStdString() << ")";

        EXPECT_LE(ratio, 21.0) << "Contrast ratio should be <= 21.0 for colors ("
                               << pair.first.name().toStdString() << ", "
                               << pair.second.name().toStdString() << ")";
    }
}

// Test 5: Verify relative luminance of black is 0.0
TEST_F(AccessibilityHelperTest, RelativeLuminance_Black) {
    QColor black(0, 0, 0);

    double luminance = AccessibilityHelper::relativeLuminance(black);

    EXPECT_DOUBLE_EQ(0.0, luminance);
}

// Test 6: Verify relative luminance of white is 1.0
TEST_F(AccessibilityHelperTest, RelativeLuminance_White) {
    QColor white(255, 255, 255);

    double luminance = AccessibilityHelper::relativeLuminance(white);

    EXPECT_DOUBLE_EQ(1.0, luminance);
}

// Test 7: Verify black and white meet WCAG AA standards
TEST_F(AccessibilityHelperTest, MeetsWcagAA_BlackWhite) {
    QColor black(Qt::black);
    QColor white(Qt::white);

    bool meetsStandard = AccessibilityHelper::meetsWcagAA(black, white);

    EXPECT_TRUE(meetsStandard);

    // Also test the reverse order
    bool meetsStandardReverse = AccessibilityHelper::meetsWcagAA(white, black);

    EXPECT_TRUE(meetsStandardReverse);
}

// Test 8: Verify similar colors do not meet WCAG AA standards
TEST_F(AccessibilityHelperTest, MeetsWcagAA_SimilarColors) {
    QColor lightGray(200, 200, 200);
    QColor lighterGray(220, 220, 220);

    bool meetsStandard = AccessibilityHelper::meetsWcagAA(lightGray, lighterGray);

    EXPECT_FALSE(meetsStandard) << "Similar colors should not meet WCAG AA standard";

    // Test other similar color pairs
    QColor darkBlue(0, 0, 100);
    QColor slightlyDarkerBlue(0, 0, 80);

    bool meetsStandard2 = AccessibilityHelper::meetsWcagAA(darkBlue, slightlyDarkerBlue);

    EXPECT_FALSE(meetsStandard2) << "Similar dark colors should not meet WCAG AA standard";
}

// Test 9: Verify focus indicator color is valid
TEST_F(AccessibilityHelperTest, FocusIndicatorColor_NotInvalid) {
    QColor focusColor = AccessibilityHelper::focusIndicatorColor();

    EXPECT_TRUE(focusColor.isValid()) << "Focus indicator color should be a valid QColor";

    // Additionally verify it's not a default-constructed invalid color
    EXPECT_NE(QColor(), focusColor);
}

// Additional test: Verify WCAG AA threshold (contrast ratio >= 4.5)
TEST_F(AccessibilityHelperTest, MeetsWcagAA_Threshold) {
    // Test a color pair that should be right at or near the threshold
    QColor darkGray(80, 80, 80);
    QColor white(255, 255, 255);

    double ratio = AccessibilityHelper::contrastRatio(darkGray, white);

    // This pair should have ratio > 4.5, thus meeting WCAG AA
    if (ratio >= 4.5) {
        EXPECT_TRUE(AccessibilityHelper::meetsWcagAA(darkGray, white));
    } else {
        EXPECT_FALSE(AccessibilityHelper::meetsWcagAA(darkGray, white));
    }
}

// Additional test: Verify relative luminance is in valid range [0.0, 1.0]
TEST_F(AccessibilityHelperTest, RelativeLuminance_Range) {
    std::vector<QColor> colors = {
        QColor(Qt::red),
        QColor(Qt::green),
        QColor(Qt::blue),
        QColor(Qt::yellow),
        QColor(Qt::cyan),
        QColor(Qt::magenta),
        QColor(128, 128, 128),
        QColor(50, 100, 150),
        QColor(200, 150, 100)
    };

    for (const auto& color : colors) {
        double luminance = AccessibilityHelper::relativeLuminance(color);

        EXPECT_GE(luminance, 0.0) << "Luminance should be >= 0.0 for color "
                                  << color.name().toStdString();

        EXPECT_LE(luminance, 1.0) << "Luminance should be <= 1.0 for color "
                                  << color.name().toStdString();
    }
}

// Additional test: Verify focus indicator has sufficient contrast with common backgrounds
TEST_F(AccessibilityHelperTest, FocusIndicatorColor_ContrastWithCommonBackgrounds) {
    QColor focusColor = AccessibilityHelper::focusIndicatorColor();

    // Test against white background
    double whiteContrast = AccessibilityHelper::contrastRatio(focusColor, Qt::white);
    EXPECT_GE(whiteContrast, 3.0) << "Focus indicator should have reasonable contrast with white";

    // Test against black background
    double blackContrast = AccessibilityHelper::contrastRatio(focusColor, Qt::black);
    EXPECT_GE(blackContrast, 3.0) << "Focus indicator should have reasonable contrast with black";
}
