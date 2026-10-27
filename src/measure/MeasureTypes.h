#pragma once

#include <QPointF>
#include <vector>
#include <string>
#include <cstdint>

namespace pdfe::measure {

enum class MeasureMode {
    Distance,   // two points → line length
    Perimeter,  // N points → sum of segment lengths
    Area        // N points → polygon area via shoelace formula
};

enum class Unit {
    Points,      // PDF native unit (1pt = 1/72 inch)
    Inches,
    Millimeters,
    Centimeters
};

/// Scale factor: "<pageUnits> on page = <realUnits> in reality".
/// e.g. 1 inch on page = 10 feet in reality → pageValue=1, realValue=10,
/// pageUnit=Inches, realUnit=Feet (but we don't model Feet — users can set
/// realValue to a scalar with whichever base unit they pick).
/// The simpler model used here: final displayed value = raw_pt_value *
/// conversionFactor * scaleFactor. A value of 1.0 means 1:1 (no scaling).
struct Scale {
    Unit displayUnit = Unit::Points;
    float scaleFactor = 1.0f;   // multiplier applied after unit conversion

    // ── ISO 32000-1 §12.9 /Measure read side (GitHub #21 / gap PDF-57) ──
    //
    // Every field below is INERT at its default, so a Scale built the way the
    // interactive tool has always built one behaves exactly as it did before.
    // They are populated only by MeasureDictReader::scaleFrom(), which turns a
    // page /Viewport's /Measure dictionary into the Scale that drives the
    // readout — that is the mechanism by which the parsed /R, /X and /D reach
    // the DISPLAYED value rather than merely landing in a struct.

    /// From a number format dictionary's /U (Table 266). When non-empty this
    /// REPLACES the derived "pt"/"in"/… suffix: the measurement is in the
    /// document author's real-world unit ("ft", "m", "mi"), which has no
    /// entry in the Unit enum and must not be forced into one.
    std::string unitLabel;

    /// From the measure dictionary's /R — the human-readable scale ratio,
    /// e.g. "1 in = 10 ft". Display-only by specification ("a text string
    /// expressing the scale ratio of the drawing"), so it is carried through
    /// to MeasureResult rather than participating in the arithmetic.
    std::string ratioLabel;

    /// From a number format dictionary's /D — the precision, expressed as the
    /// DENOMINATOR of the smallest displayed fraction (100 => hundredths).
    /// 0 means "no rounding", which is the pre-#21 behaviour.
    double precisionDenominator = 0.0;

    /// True when `scaleFactor` is ALREADY an area factor.
    ///
    /// The interactive tool's `scaleFactor` is a LINEAR ratio, so
    /// `MeasureCalc::format()` squares it for an area — 1 unit on page = 10
    /// real units means 1 unit² = 100 real units². A `/Measure` `/A` number
    /// format's `/C`, by contrast, converts SQUARE default-user-space units
    /// straight into `/U`, so squaring it again would give the fourth power
    /// of the real factor and an answer wrong by five orders of magnitude on
    /// a typical drawing. Set only by
    /// `MeasureDictReader::scaleFrom(m, MeasureMode::Area)`.
    bool scaleFactorIsAreal = false;
};

/// A single measurement record, ready for display and persistence.
struct MeasureResult {
    MeasureMode mode;
    float rawValuePoints;    // before unit conversion / scale
    float displayedValue;    // after unit conversion + scale
    Unit displayUnit;
    std::string unitSuffix;  // "pt", "in", "mm", "cm", or a /Measure /U label
    /// Copied from Scale::ratioLabel — empty unless a /Measure drove this.
    std::string ratioLabel;
};

} // namespace pdfe::measure
