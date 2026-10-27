// -*- coding: utf-8 -*-
//
// GitHub #21 / gap-register PDF-57 — the READ side of ISO 32000-1 §12.9
// measurement properties.
//
// WHAT THE CRITERION ASKS, and why each half needs its own oracle:
//
//   "the read side of /Measure (/R ratio, /X and /D number-format arrays)
//    DRIVES THE DISPLAYED VALUE and ROUND-TRIPS through save/reopen"
//
// Two independent properties, and a test can be green on one while the other
// is broken:
//
//   * DRIVES. It is not enough that /C lands in a struct. The number a user
//     reads — MeasureResult::displayedValue, computed by the same
//     MeasureCalc::format() the interactive ruler has always used — must be
//     the one the document's /Measure implies. So the assertions are on
//     displayedValue, unitSuffix and ratioLabel, against constants derived
//     from the fixture's stated scale rather than from the product.
//   * ROUND-TRIPS. The values must still be there after PDFE writes the file
//     and reads it back. Asserted on the WHOLE dictionary — including the
//     presentational /F, /RD, /RT, /PS, /SS and /O, which the arithmetic
//     never touches — because a round-trip oracle that compared only the
//     three entries the maths reads would be green on a save that dropped
//     everything else.
//
// THE FIXTURE'S SCALE IS AN INPUT, NEVER ANOTHER OUTPUT.
// tests/fixtures/annot_measure.pdf states "1 in = 10 ft" and carries
// /C 0.138889 (pikepdf writes reals at six decimals; the raw bytes were
// checked, not assumed). One inch is 72 default user-space units, so a 72pt
// segment is 72 * 0.138889 = 10.000008 ft, and the /D 100 precision the
// document also states rounds that to exactly 10.00. Both numbers come from
// the document, not from PDFE.

#include <gtest/gtest.h>

#include "measure/MeasureDictReader.h"
#include "measure/MeasureTool.h"
#include "measure/MeasureCalc.h"
#include "measure/MeasureTypes.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using pdfe::measure::MeasureCalc;
using pdfe::measure::MeasureDict;
using pdfe::measure::MeasureDictReader;
using pdfe::measure::MeasureMode;
using pdfe::measure::MeasureResult;
using pdfe::measure::NumberFormat;
using pdfe::measure::Scale;
using pdfe::measure::Unit;
using pdfe::measure::Viewport;

namespace {

// ── Constants taken from the fixture, restated here so the oracle does not
//    have to ask the product what the document says. ───────────────────────

// Each is written through `float` DELIBERATELY. MuPDF's `pdf_to_real` returns
// a float, so the value the reader reports is `(double)(float)0.138889`, which
// is NOT equal to the double literal `0.138889`. Comparing against the literal
// would fail for a reason that has nothing to do with the product — the same
// class of self-inflicted oracle bug as an `sscanf` that matched the wrong
// line. The float round-trip is stated here rather than papered over with a
// tolerance, so the assertion stays exact.

/// The /C the generator wrote for /X and /D of viewport 0, as it appears in
/// the raw bytes (pikepdf writes reals at six decimals).
constexpr double kFtPerUnit = static_cast<double>(0.138889f);
/// The /C for /A of viewport 0.
constexpr double kSqFtPerSqUnit = static_cast<double>(0.01929f);
/// Viewport 1's linear /C.
constexpr double kMPerUnit = static_cast<double>(0.027778f);

/// One inch, in default user space units.
constexpr float kOneInch = 72.0f;

class MeasureAnnotationTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        fixture_ = std::filesystem::path(TEST_FIXTURES_DIR) / "annot_measure.pdf";
        ASSERT_TRUE(std::filesystem::exists(fixture_))
            << "annot_measure.pdf fixture missing at " << fixture_
            << " — regenerate with tests/fixtures/gen_annot_measure.py";

        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tempDir_ = std::filesystem::temp_directory_path()
                   / "pdfe_measure_dict" / info->name();
        std::filesystem::remove_all(tempDir_);
        std::filesystem::create_directories(tempDir_);
    }

    void TearDown() override { ctx_.reset(); }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::filesystem::path fixture_;
    std::filesystem::path tempDir_;
};

/// Every field of a number format, compared one at a time so a failure names
/// the entry that was lost rather than "the dictionaries differ".
void ExpectNumberFormatsEqual(const NumberFormat& a, const NumberFormat& b,
                              const std::string& where)
{
    EXPECT_EQ(a.unitLabel, b.unitLabel) << where << ": /U";
    EXPECT_EQ(a.hasConversionFactor, b.hasConversionFactor)
        << where << ": /C presence";
    EXPECT_DOUBLE_EQ(a.conversionFactor, b.conversionFactor) << where << ": /C";
    EXPECT_EQ(a.fractionMode, b.fractionMode) << where << ": /F";
    EXPECT_DOUBLE_EQ(a.precisionDenominator, b.precisionDenominator)
        << where << ": /D";
    EXPECT_EQ(a.decimalSeparator, b.decimalSeparator) << where << ": /RD";
    EXPECT_EQ(a.thousandsSeparator, b.thousandsSeparator) << where << ": /RT";
    EXPECT_EQ(a.prefix, b.prefix) << where << ": /PS";
    EXPECT_EQ(a.suffix, b.suffix) << where << ": /SS";
    EXPECT_EQ(a.labelPosition, b.labelPosition) << where << ": /O";
}

void ExpectMeasuresEqual(const MeasureDict& a, const MeasureDict& b,
                         const std::string& where)
{
    ASSERT_EQ(a.present, b.present) << where << ": /Measure presence";
    if (!a.present) return;
    EXPECT_EQ(a.subtype, b.subtype) << where << ": /Subtype";
    EXPECT_EQ(a.ratio, b.ratio) << where << ": /R";
    EXPECT_EQ(a.valid, b.valid) << where << ": validity";
    ASSERT_EQ(a.x.size(), b.x.size()) << where << ": /X length";
    ASSERT_EQ(a.d.size(), b.d.size()) << where << ": /D length";
    ASSERT_EQ(a.a.size(), b.a.size()) << where << ": /A length";
    for (std::size_t i = 0; i < a.x.size(); ++i)
        ExpectNumberFormatsEqual(a.x[i], b.x[i], where + " /X[" + std::to_string(i) + "]");
    for (std::size_t i = 0; i < a.d.size(); ++i)
        ExpectNumberFormatsEqual(a.d[i], b.d[i], where + " /D[" + std::to_string(i) + "]");
    for (std::size_t i = 0; i < a.a.size(); ++i)
        ExpectNumberFormatsEqual(a.a[i], b.a[i], where + " /A[" + std::to_string(i) + "]");
}

TEST_F(MeasureAnnotationTest, MeasureDictScaleRoundTripsAndDrivesDisplayedValue)
{
    MeasureDictReader reader(ctx_);

    // ═══ PART 1 — the read side parses what the document states ═══════════
    std::vector<Viewport> original;
    {
        auto doc = pdfe::core::Document::open(ctx_, fixture_);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);
        original = reader.readViewports(page.get());
    }

    ASSERT_EQ(original.size(), 3u)
        << "annot_measure.pdf states THREE viewports in /VP; reading a "
           "different number means the /VP walk, not the /Measure parse, is "
           "what this test is measuring";

    const Viewport& sitePlan = original[0];
    const Viewport& callout  = original[1];
    const Viewport& none     = original[2];

    EXPECT_EQ(sitePlan.name, "SitePlan");
    EXPECT_EQ(sitePlan.bbox, QRectF(QPointF(0, 0), QPointF(612, 792)));
    ASSERT_TRUE(sitePlan.measure.present);
    ASSERT_TRUE(sitePlan.measure.valid)
        << "the site plan's /Measure must be usable: /Subtype /RL, a non-empty "
           "/R, and a first /X and /D element with both /U and /C";
    EXPECT_EQ(sitePlan.measure.subtype, "RL");
    EXPECT_EQ(sitePlan.measure.ratio, "1 in = 10 ft") << "/R";

    ASSERT_EQ(sitePlan.measure.x.size(), 1u) << "/X";
    ASSERT_EQ(sitePlan.measure.d.size(), 1u) << "/D";
    ASSERT_EQ(sitePlan.measure.a.size(), 1u) << "/A";
    EXPECT_EQ(sitePlan.measure.x[0].unitLabel, "ft");
    EXPECT_TRUE(sitePlan.measure.x[0].hasConversionFactor);
    EXPECT_DOUBLE_EQ(sitePlan.measure.x[0].conversionFactor, kFtPerUnit);
    EXPECT_DOUBLE_EQ(sitePlan.measure.d[0].conversionFactor, kFtPerUnit);
    EXPECT_DOUBLE_EQ(sitePlan.measure.d[0].precisionDenominator, 100.0);
    EXPECT_EQ(sitePlan.measure.a[0].unitLabel, "sq ft");
    EXPECT_DOUBLE_EQ(sitePlan.measure.a[0].conversionFactor, kSqFtPerSqUnit);
    // The presentational entries were authored to NON-default values, so
    // reading a default here means the key was never read at all.
    EXPECT_EQ(sitePlan.measure.d[0].decimalSeparator, ",") << "/RD";
    EXPECT_EQ(sitePlan.measure.d[0].thousandsSeparator, ".") << "/RT";
    EXPECT_EQ(sitePlan.measure.d[0].prefix, " ") << "/PS";
    EXPECT_EQ(sitePlan.measure.d[0].suffix, " ft") << "/SS";
    EXPECT_EQ(sitePlan.measure.d[0].labelPosition, "S") << "/O";
    EXPECT_EQ(sitePlan.measure.d[0].fractionMode, "D") << "/F";

    EXPECT_EQ(callout.name, "DetailCallout");
    ASSERT_TRUE(callout.measure.valid);
    EXPECT_EQ(callout.measure.ratio, "1 in = 2 m");
    EXPECT_DOUBLE_EQ(callout.measure.d[0].conversionFactor, kMPerUnit);

    // NEGATIVE CONTROL. A reader that fabricated a default /Measure for every
    // viewport would satisfy every assertion above.
    EXPECT_EQ(none.name, "NoMeasure");
    EXPECT_FALSE(none.measure.present)
        << "the third viewport has NO /Measure; reporting one means the "
           "reader is inventing measurement properties the document does not "
           "state";
    EXPECT_FALSE(none.measure.valid);

    // ═══ PART 2 — those values DRIVE the displayed value ══════════════════
    //
    // The bridge under test is scaleFrom() -> MeasureCalc::format(), i.e. the
    // same path the interactive readout uses. Nothing here recomputes the
    // arithmetic: the expectations are stated from the fixture's own scale.

    const Scale distScale =
        MeasureDictReader::scaleFrom(sitePlan.measure, MeasureMode::Distance);
    EXPECT_EQ(distScale.displayUnit, Unit::Points)
        << "/C converts default user space units — which ARE points — "
           "straight to /U, so a second pt->unit conversion would "
           "double-convert";
    EXPECT_FLOAT_EQ(distScale.scaleFactor, static_cast<float>(kFtPerUnit));
    EXPECT_EQ(distScale.unitLabel, "ft");
    EXPECT_EQ(distScale.ratioLabel, "1 in = 10 ft");
    EXPECT_DOUBLE_EQ(distScale.precisionDenominator, 100.0);
    EXPECT_FALSE(distScale.scaleFactorIsAreal);

    // One inch on the page. The document says one inch is ten feet.
    {
        const MeasureResult r =
            MeasureCalc::format(MeasureMode::Distance, kOneInch, distScale);
        EXPECT_FLOAT_EQ(r.displayedValue, 10.0f)
            << "a 72pt (one inch) segment on a drawing whose /Measure states "
               "\"1 in = 10 ft\" must DISPLAY as 10 ft. Got "
            << r.displayedValue << " " << r.unitSuffix
            << " — the parsed /C is not reaching the displayed value.";
        EXPECT_EQ(r.unitSuffix, "ft")
            << "the unit shown must be the document's /U, not a derived "
               "\"pt\"/\"in\" — \"ft\" has no entry in the Unit enum and must "
               "not be coerced into one";
        EXPECT_EQ(r.ratioLabel, "1 in = 10 ft") << "/R must reach the readout";
        EXPECT_FLOAT_EQ(r.rawValuePoints, kOneInch);
    }
    // Two inches — twenty feet. A second point on the line, so a displayed
    // value that happened to be right by a constant offset cannot pass.
    {
        const MeasureResult r =
            MeasureCalc::format(MeasureMode::Distance, 2 * kOneInch, distScale);
        EXPECT_FLOAT_EQ(r.displayedValue, 20.0f)
            << "144pt must display as 20 ft at \"1 in = 10 ft\"";
    }
    // The /D 100 precision is doing real work: the raw product is
    // 72 * 0.138889 = 10.000008, and only the document's own stated precision
    // makes it read as 10.00.
    {
        Scale unrounded = distScale;
        unrounded.precisionDenominator = 0.0;
        const MeasureResult r =
            MeasureCalc::format(MeasureMode::Distance, kOneInch, unrounded);
        EXPECT_NE(r.displayedValue, 10.0f)
            << "SANITY: without the document's /D precision the value is "
               "10.000008, so the 10.00 asserted above is produced BY the "
               "rounding and not by a coincidence of the fixture's /C";
        EXPECT_NEAR(r.displayedValue, 10.000008f, 1e-4f);
    }

    // The detail callout is at a DIFFERENT scale, on the same page. The same
    // 72pt segment must display as 2 m there — so the displayed value is
    // driven by the viewport that was read, not by anything global.
    {
        const Scale mScale =
            MeasureDictReader::scaleFrom(callout.measure, MeasureMode::Distance);
        const MeasureResult r =
            MeasureCalc::format(MeasureMode::Distance, kOneInch, mScale);
        EXPECT_FLOAT_EQ(r.displayedValue, 2.0f)
            << "the same 72pt segment inside the \"1 in = 2 m\" callout must "
               "display as 2 m";
        EXPECT_EQ(r.unitSuffix, "m");
        EXPECT_EQ(r.ratioLabel, "1 in = 2 m");
    }

    // §12.9's LAST-WINS rule for overlapping viewports, which is the opposite
    // of the obvious scan direction. The point is inside both.
    {
        const QPointF inBoth(450, 650);
        ASSERT_TRUE(sitePlan.bbox.contains(inBoth));
        ASSERT_TRUE(callout.bbox.contains(inBoth));
        auto chosen = MeasureDictReader::viewportAt(original, inBoth);
        ASSERT_TRUE(chosen.has_value());
        EXPECT_EQ(chosen->name, "DetailCallout")
            << "ISO 32000-1 §12.9: when a point is inside more than one "
               "viewport the one appearing LAST in /VP shall be used";
        // A point inside only the site plan resolves to the site plan.
        auto outer = MeasureDictReader::viewportAt(original, QPointF(50, 400));
        ASSERT_TRUE(outer.has_value());
        EXPECT_EQ(outer->name, "SitePlan");
    }

    // Area: /A's /C is already an AREA factor. Two square inches is
    // 2 * 72 * 72 = 10368 pt^2, which at 0.01929 sq ft per sq unit is
    // 199.99872 sq ft, rounded by the document's /D 10 to 200.0.
    {
        const Scale areaScale =
            MeasureDictReader::scaleFrom(sitePlan.measure, MeasureMode::Area);
        EXPECT_TRUE(areaScale.scaleFactorIsAreal)
            << "an /A conversion factor must be marked areal, or format() "
               "squares it a second time";
        const MeasureResult r =
            MeasureCalc::format(MeasureMode::Area, 2 * kOneInch * kOneInch,
                                areaScale);
        EXPECT_FLOAT_EQ(r.displayedValue, 200.0f)
            << "two square inches at \"1 in = 10 ft\" is 200 sq ft; got "
            << r.displayedValue << " " << r.unitSuffix;
        EXPECT_EQ(r.unitSuffix, "sq ft")
            << "an /A /U already names an areal unit, so no \"\xc2\xb2\" is "
               "appended — \"sq ft\xc2\xb2\" would be an area of an area";
    }

    // A viewport with no /Measure must degrade to 1:1 with no label, not to
    // nonsense — the documented contract of scaleFrom() on an invalid dict.
    {
        const Scale s =
            MeasureDictReader::scaleFrom(none.measure, MeasureMode::Distance);
        EXPECT_FLOAT_EQ(s.scaleFactor, 1.0f);
        EXPECT_TRUE(s.unitLabel.empty());
        const MeasureResult r =
            MeasureCalc::format(MeasureMode::Distance, kOneInch, s);
        EXPECT_FLOAT_EQ(r.displayedValue, kOneInch);
        EXPECT_EQ(r.unitSuffix, "pt");
    }

    // ── The tool the USER drives must consult the page's /Measure ────────
    //
    // Everything above proves the reader and MeasureCalc agree. It would all
    // still be true of a reader nothing ever called — which is precisely the
    // defect PDF-57 records, since MeasureTool measured every document at
    // whatever scale the user had typed. So the same question is asked of
    // MeasureTool::scaleAt(), the seam the readout and the committed
    // annotation both go through.
    {
        pdfe::measure::MeasureTool tool(nullptr, MeasureMode::Distance);
        // A user-typed scale that is WRONG for this document. If the document
        // is not consulted, this is what comes out, and 72pt would read as
        // 72 * 3 = 216.
        Scale typedByUser;
        typedByUser.displayUnit = Unit::Points;
        typedByUser.scaleFactor = 3.0f;
        tool.setScale(typedByUser);

        // Before the page's measures are handed over, the user's scale rules —
        // the pre-#21 behaviour, kept as the fallback for the documents that
        // declare nothing.
        EXPECT_FLOAT_EQ(tool.scaleAt(QPointF(50, 400)).scaleFactor, 3.0f);

        tool.setPageMeasures(original);
        EXPECT_FLOAT_EQ(tool.scaleAt(QPointF(50, 400)).scaleFactor,
                        static_cast<float>(kFtPerUnit))
            << "with the page's /VP loaded, a point inside SitePlan must "
               "measure at the DOCUMENT's declared scale, not the user's";
        EXPECT_EQ(tool.scaleAt(QPointF(50, 400)).unitLabel, "ft");
        EXPECT_FLOAT_EQ(tool.scaleAt(QPointF(450, 650)).scaleFactor,
                        static_cast<float>(kMPerUnit))
            << "a point inside the later, overlapping DetailCallout must "
               "measure at ITS scale";
        EXPECT_EQ(tool.scaleAt(QPointF(450, 650)).unitLabel, "m");
    }

    // ═══ PART 3 — ROUND-TRIP through PDFE's own save and reopen ═══════════
    const std::filesystem::path out = tempDir_ / "annot_measure_roundtrip.pdf";
    {
        auto doc = pdfe::core::Document::open(ctx_, fixture_);
        ASSERT_NE(doc, nullptr);
        doc->saveAs(out);
    }
    ASSERT_TRUE(std::filesystem::exists(out));

    std::vector<Viewport> reread;
    {
        auto reopened = pdfe::core::Document::open(ctx_, out);
        ASSERT_NE(reopened, nullptr);
        auto page = reopened->page(0);
        ASSERT_NE(page, nullptr);
        reread = reader.readViewports(page.get());
    }

    ASSERT_EQ(reread.size(), original.size())
        << "the saved copy lost or gained a /VP entry";
    for (std::size_t i = 0; i < original.size(); ++i) {
        const std::string where = "viewport[" + std::to_string(i) + "]";
        EXPECT_EQ(reread[i].name, original[i].name) << where << ": /Name";
        EXPECT_EQ(reread[i].bbox, original[i].bbox) << where << ": /BBox";
        ExpectMeasuresEqual(original[i].measure, reread[i].measure, where);
    }

    // The round-trip is only worth anything if the reopened values still
    // DRIVE the same displayed value. Asserted on the reopened document's own
    // /Measure, not on the one read before the save.
    {
        ASSERT_TRUE(reread[0].measure.valid)
            << "the site plan's /Measure must still be USABLE after "
               "save/reopen, not merely present";
        const Scale s =
            MeasureDictReader::scaleFrom(reread[0].measure, MeasureMode::Distance);
        const MeasureResult r =
            MeasureCalc::format(MeasureMode::Distance, kOneInch, s);
        EXPECT_FLOAT_EQ(r.displayedValue, 10.0f)
            << "after save/reopen a 72pt segment must still display as 10 ft";
        EXPECT_EQ(r.unitSuffix, "ft");
        EXPECT_EQ(r.ratioLabel, "1 in = 10 ft");
    }
}

} // namespace
