#pragma once

#include "MeasureTypes.h"
#include <QPointF>
#include <vector>

namespace pdfe::measure {

/// Pure math utilities for measurement computation. All inputs are in PDF
/// points; outputs can be converted to display units via formatValue().
class MeasureCalc {
public:
    /// Distance between two points (Euclidean).
    static float distance(QPointF a, QPointF b);

    /// Perimeter = sum of segment lengths along the polyline.
    /// Returns 0 if fewer than 2 points.
    static float perimeter(const std::vector<QPointF>& points);

    /// Polygon area via shoelace formula. Returns absolute value so
    /// winding order doesn't matter.
    /// Returns 0 if fewer than 3 points.
    static float area(const std::vector<QPointF>& points);

    /// Convert a value in PDF points to the target unit.
    /// 72 pt = 1 in; 1 in = 25.4 mm; 1 cm = 10 mm.
    static float convertFromPoints(float pts, Unit target);

    /// Short suffix for a unit ("pt", "in", "mm", "cm").
    static std::string unitSuffix(Unit u);

    /// Apply unit conversion and scale factor. For area, the conversion
    /// squares the linear factor (since area is length²).
    static MeasureResult format(MeasureMode mode, float rawPoints,
                                const Scale& scale);
};

} // namespace pdfe::measure
