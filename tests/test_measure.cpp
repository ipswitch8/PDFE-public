// -*- coding: utf-8 -*-
// Phase 5a: MeasureCalc + MeasureTool tests.
// Covers distance/perimeter/area math, unit conversion correctness,
// scale factor (linear for distance/perimeter, squared for area),
// tagged-content build/parse, and round-trip through annotation save/reload.
#include <gtest/gtest.h>
#include "measure/MeasureCalc.h"
#include "measure/MeasureTool.h"
#include "measure/MeasureTypes.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationData.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QPointF>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::measure;
using namespace pdfe::annotations;
using namespace pdfe::core;

// -------- Distance --------

TEST(MeasureCalcDistance, TwoHorizontalPoints)
{
    EXPECT_NEAR(MeasureCalc::distance({0, 0}, {100, 0}), 100.0f, 1e-4);
}

TEST(MeasureCalcDistance, TwoDiagonalPoints_ThreeFourFive)
{
    // 3-4-5 right triangle → distance is 5.
    EXPECT_NEAR(MeasureCalc::distance({0, 0}, {3, 4}), 5.0f, 1e-4);
}

TEST(MeasureCalcDistance, NegativeCoordinates)
{
    EXPECT_NEAR(MeasureCalc::distance({-10, -10}, {10, 10}),
                std::sqrt(800.0f), 1e-3);
}

// -------- Perimeter (polyline) --------

TEST(MeasureCalcPerimeter, ZeroOrOnePointReturnsZero)
{
    EXPECT_EQ(MeasureCalc::perimeter({}), 0.0f);
    EXPECT_EQ(MeasureCalc::perimeter({{1, 1}}), 0.0f);
}

TEST(MeasureCalcPerimeter, TwoPointsIsSameAsDistance)
{
    std::vector<QPointF> pts{{0, 0}, {3, 4}};
    EXPECT_NEAR(MeasureCalc::perimeter(pts), 5.0f, 1e-4);
}

TEST(MeasureCalcPerimeter, ThreeSegmentPolyline)
{
    // (0,0) → (10,0) → (10,10) → (0,10) is an open polyline:
    // 10 + 10 + 10 = 30 pt (not closed).
    std::vector<QPointF> pts{{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    EXPECT_NEAR(MeasureCalc::perimeter(pts), 30.0f, 1e-4);
}

// -------- Area (shoelace) --------

TEST(MeasureCalcArea, FewerThanThreeReturnsZero)
{
    EXPECT_EQ(MeasureCalc::area({{0, 0}, {1, 1}}), 0.0f);
}

TEST(MeasureCalcArea, UnitSquare)
{
    std::vector<QPointF> pts{{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    EXPECT_NEAR(MeasureCalc::area(pts), 100.0f, 1e-4);
}

TEST(MeasureCalcArea, RightTriangle)
{
    // Base 6, height 8 → area 24.
    std::vector<QPointF> pts{{0, 0}, {6, 0}, {0, 8}};
    EXPECT_NEAR(MeasureCalc::area(pts), 24.0f, 1e-4);
}

TEST(MeasureCalcArea, WindingOrderInvariant)
{
    std::vector<QPointF> ccw{{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    std::vector<QPointF> cw {{0, 0}, {0, 10}, {10, 10}, {10, 0}};
    EXPECT_NEAR(MeasureCalc::area(ccw), MeasureCalc::area(cw), 1e-4);
}

// -------- Unit conversion --------

TEST(MeasureCalcUnitConv, PointsToInches)
{
    // 72pt = 1in
    EXPECT_NEAR(MeasureCalc::convertFromPoints(72.0f, Unit::Inches), 1.0f, 1e-4);
    EXPECT_NEAR(MeasureCalc::convertFromPoints(144.0f, Unit::Inches), 2.0f, 1e-4);
}

TEST(MeasureCalcUnitConv, PointsToMillimeters)
{
    // 72pt = 25.4mm
    EXPECT_NEAR(MeasureCalc::convertFromPoints(72.0f, Unit::Millimeters), 25.4f, 1e-3);
}

TEST(MeasureCalcUnitConv, PointsToCentimeters)
{
    // 72pt = 2.54cm
    EXPECT_NEAR(MeasureCalc::convertFromPoints(72.0f, Unit::Centimeters), 2.54f, 1e-4);
}

TEST(MeasureCalcUnitConv, PointsToPointsIdentity)
{
    EXPECT_NEAR(MeasureCalc::convertFromPoints(123.45f, Unit::Points), 123.45f, 1e-4);
}

// -------- Format (unit + scale) --------

TEST(MeasureCalcFormat, DistanceWithInchesAndNoScale)
{
    Scale s;
    s.displayUnit = Unit::Inches;
    s.scaleFactor = 1.0f;
    auto r = MeasureCalc::format(MeasureMode::Distance, 144.0f, s);
    EXPECT_NEAR(r.displayedValue, 2.0f, 1e-4);
    EXPECT_EQ(r.unitSuffix, "in");
    EXPECT_EQ(r.mode, MeasureMode::Distance);
}

TEST(MeasureCalcFormat, DistanceWithScaleFactor)
{
    // 1 in on page = 10 ft in reality (display scalar 10)
    Scale s;
    s.displayUnit = Unit::Inches;
    s.scaleFactor = 10.0f;
    auto r = MeasureCalc::format(MeasureMode::Distance, 72.0f, s);
    // 72pt = 1in; × 10 scale → 10.
    EXPECT_NEAR(r.displayedValue, 10.0f, 1e-4);
}

TEST(MeasureCalcFormat, AreaUnitConversionSquared)
{
    // 100 pt² → inches². 1 pt = 1/72 in → 100 pt² = 100/72² in² ≈ 0.019290
    Scale s;
    s.displayUnit = Unit::Inches;
    s.scaleFactor = 1.0f;
    auto r = MeasureCalc::format(MeasureMode::Area, 100.0f, s);
    EXPECT_NEAR(r.displayedValue, 100.0f / (72.0f * 72.0f), 1e-5);
    EXPECT_EQ(r.unitSuffix, "in\xc2\xb2")  // "in²" UTF-8
        << "Area suffix should include the squared mark";
}

TEST(MeasureCalcFormat, AreaScaleFactorSquared)
{
    // scaleFactor applies squared for area: 2x scale → 4x result.
    Scale s;
    s.displayUnit = Unit::Points;
    s.scaleFactor = 2.0f;
    auto r = MeasureCalc::format(MeasureMode::Area, 100.0f, s);
    EXPECT_NEAR(r.displayedValue, 400.0f, 1e-4);
}

// -------- buildContents / parseContents --------

TEST(MeasureToolContents, BuildThenParseRoundTrip)
{
    MeasureResult r;
    r.mode = MeasureMode::Distance;
    r.displayedValue = 12.5f;
    r.rawValuePoints = 900.0f;
    r.displayUnit = Unit::Inches;
    r.unitSuffix = "in";

    std::string s = MeasureTool::buildContents(r);
    ASSERT_NE(s.find("[PDFE-MEASURE:"), std::string::npos);

    std::string mode, unit;
    float value = 0.0f;
    EXPECT_TRUE(MeasureTool::parseContents(s, mode, value, unit));
    EXPECT_EQ(mode, "distance");
    EXPECT_EQ(unit, "in");
    EXPECT_NEAR(value, 12.5f, 1e-3);
}

TEST(MeasureToolContents, ParseRejectsMissingTag)
{
    std::string mode, unit;
    float value = 0.0f;
    EXPECT_FALSE(MeasureTool::parseContents(
        "Just some free text", mode, value, unit));
}

TEST(MeasureToolContents, ParseReadsPerimeterAndArea)
{
    MeasureResult p;
    p.mode = MeasureMode::Perimeter;
    p.displayedValue = 30.0f;
    p.unitSuffix = "mm";
    std::string s = MeasureTool::buildContents(p);
    std::string mode, unit;
    float value = 0;
    ASSERT_TRUE(MeasureTool::parseContents(s, mode, value, unit));
    EXPECT_EQ(mode, "perimeter");
    EXPECT_EQ(unit, "mm");

    MeasureResult a;
    a.mode = MeasureMode::Area;
    a.displayedValue = 100.0f;
    a.unitSuffix = "in\xc2\xb2";
    s = MeasureTool::buildContents(a);
    mode.clear(); unit.clear(); value = 0;
    ASSERT_TRUE(MeasureTool::parseContents(s, mode, value, unit));
    EXPECT_EQ(mode, "area");
}

// -------- Round-trip via AnnotationManager --------

class MeasurePersistenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx = std::make_shared<MuPdfContext>();
        auto fixtures = std::filesystem::path(TEST_FIXTURES_DIR);
        src = fixtures / "test_single.pdf";
        if (!std::filesystem::exists(src)) {
            GTEST_SKIP() << "test_single.pdf not available";
        }
    }
    std::filesystem::path tempOut(const char* name) {
        auto d = std::filesystem::temp_directory_path() / "pdfe_measure_tests";
        std::filesystem::create_directories(d);
        return d / name;
    }

    std::shared_ptr<MuPdfContext> ctx;
    std::filesystem::path src;
};

TEST_F(MeasurePersistenceTest, DistanceMeasurementSurvivesSaveReload)
{
    auto doc = Document::open(ctx, src);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    AnnotationManager mgr(ctx);
    QRectF rect(100, 100, 200, 0);
    auto data = mgr.createAnnotation(page.get(),
                                     AnnotationType::Line, rect);

    MeasureResult r;
    r.mode = MeasureMode::Distance;
    r.displayedValue = 5.25f;
    r.unitSuffix = "in";
    const std::string contents = MeasureTool::buildContents(r);
    mgr.setContents(page.get(), data.annotIndex, contents);

    auto out = tempOut("distance_rt.pdf");
    std::filesystem::remove(out);
    doc->saveAs(out);
    ASSERT_TRUE(std::filesystem::exists(out));

    page.reset();
    doc.reset();

    auto doc2 = Document::open(ctx, out);
    ASSERT_NE(doc2, nullptr);
    auto page2 = doc2->page(0);
    ASSERT_NE(page2, nullptr);

    AnnotationManager mgr2(ctx);
    auto all = mgr2.getAnnotations(page2.get());
    ASSERT_FALSE(all.empty());

    // Locate any annotation carrying the measure tag.
    bool foundTag = false;
    std::string mode, unit;
    float value = 0.0f;
    for (const auto& a : all) {
        if (MeasureTool::parseContents(a.contents, mode, value, unit)) {
            foundTag = true;
            break;
        }
    }
    ASSERT_TRUE(foundTag) << "Measure tag did not survive save/reload";
    EXPECT_EQ(mode, "distance");
    EXPECT_EQ(unit, "in");
    EXPECT_NEAR(value, 5.25f, 1e-3);
}
