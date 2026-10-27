#include "MeasureCalc.h"
#include <cmath>
#include <cstdlib>

namespace pdfe::measure {

float MeasureCalc::distance(QPointF a, QPointF b)
{
    const float dx = static_cast<float>(b.x() - a.x());
    const float dy = static_cast<float>(b.y() - a.y());
    return std::sqrt(dx * dx + dy * dy);
}

float MeasureCalc::perimeter(const std::vector<QPointF>& points)
{
    if (points.size() < 2) return 0.0f;
    float total = 0.0f;
    for (size_t i = 1; i < points.size(); ++i) {
        total += distance(points[i - 1], points[i]);
    }
    return total;
}

float MeasureCalc::area(const std::vector<QPointF>& points)
{
    const size_t n = points.size();
    if (n < 3) return 0.0f;
    // Shoelace formula: sum of (x_i * y_{i+1} - x_{i+1} * y_i) / 2
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const QPointF& cur = points[i];
        const QPointF& nxt = points[(i + 1) % n];
        acc += (cur.x() * nxt.y()) - (nxt.x() * cur.y());
    }
    return static_cast<float>(std::abs(acc) * 0.5);
}

float MeasureCalc::convertFromPoints(float pts, Unit target)
{
    switch (target) {
        case Unit::Points:      return pts;
        case Unit::Inches:      return pts / 72.0f;
        case Unit::Millimeters: return pts * 25.4f / 72.0f;
        case Unit::Centimeters: return pts * 2.54f / 72.0f;
    }
    return pts;
}

std::string MeasureCalc::unitSuffix(Unit u)
{
    switch (u) {
        case Unit::Points:      return "pt";
        case Unit::Inches:      return "in";
        case Unit::Millimeters: return "mm";
        case Unit::Centimeters: return "cm";
    }
    return "pt";
}

MeasureResult MeasureCalc::format(MeasureMode mode, float rawPoints,
                                  const Scale& scale)
{
    MeasureResult r;
    r.mode = mode;
    r.rawValuePoints = rawPoints;
    r.displayUnit = scale.displayUnit;
    r.unitSuffix = unitSuffix(scale.displayUnit);
    // GitHub #21: /R is display-only per ISO 32000-1 §12.9, so it rides
    // through rather than entering the arithmetic.
    r.ratioLabel = scale.ratioLabel;

    if (mode == MeasureMode::Area) {
        // Area is length² — the linear unit conversion factor squares.
        const float linearFactor = convertFromPoints(1.0f, scale.displayUnit);
        // GitHub #21: a /Measure /A factor is already areal and must not be
        // squared a second time. See Scale::scaleFactorIsAreal.
        const float areaFactor = scale.scaleFactorIsAreal
                                     ? scale.scaleFactor
                                     : scale.scaleFactor * scale.scaleFactor;
        r.displayedValue = rawPoints * linearFactor * linearFactor * areaFactor;
        if (scale.displayUnit != Unit::Points) {
            r.unitSuffix = unitSuffix(scale.displayUnit) + "²";
        } else {
            r.unitSuffix = "pt²";
        }
    } else {
        r.displayedValue = convertFromPoints(rawPoints, scale.displayUnit)
                           * scale.scaleFactor;
    }

    // ── GitHub #21 / PDF-57: the /Measure read side reaching the DISPLAY ──
    //
    // /U (unit label). A /Measure number format names its unit in the
    // document author's own vocabulary — "ft", "m", "mi" — which is not in
    // the Unit enum and must not be coerced into one. When present it
    // replaces the derived suffix outright.
    //
    // NO "²" IS APPENDED FOR AN AREA, deliberately: an /A number format's /U
    // already names an AREAL unit in the author's own words ("sq ft", "m²",
    // "acres"), so appending a superscript two would produce "sq ft²" — an
    // area of an area. The derived-suffix branch above still squares, because
    // there the label came from the Unit enum and really is linear.
    if (!scale.unitLabel.empty()) {
        r.unitSuffix = scale.unitLabel;
    }

    // /D (precision). Table 266: "the precision or denominator of a fractional
    // amount". For the decimal fraction mode this is the denominator of the
    // smallest displayed fraction, so 100 means hundredths. Applied LAST, to
    // the finished value, because that is the number a user reads.
    if (scale.precisionDenominator > 0.0) {
        const double p = scale.precisionDenominator;
        r.displayedValue = static_cast<float>(
            std::round(static_cast<double>(r.displayedValue) * p) / p);
    }

    return r;
}

} // namespace pdfe::measure
