// -*- coding: utf-8 -*-
#pragma once

#include <algorithm>

namespace pdfe::core {

/// Area of an image block's bounding box after NORMALISING inverted
/// coordinates and CLAMPING to the page. Returns 0 for a block that lies
/// wholly off the page.
///
/// Extracted from Page::isImageDominated so it can be tested as what it is: a
/// pure geometric function. Two previous attempts to cover this logic tried to
/// drive it through a whole PDF, and both failed on the same rock — measured
/// and recorded in tests/test_ocr_routing_fix.cpp:
///
///     PyMuPDF on the tuned fixture : signed 0.444 / clamped 0.995
///     instrumented C++, same file  : ratio=3.6297   <- POSITIVE, not 0.444
///
/// Synthetic `cm` placements do not produce inverted bboxes in MuPDF's stext
/// path at all; they ADD area. An inverted bbox is trivial to construct as
/// DATA and (so far) impossible to construct as a PDF, so the test takes the
/// data route. The document-level reproduction remains unachieved.
///
/// Takes plain doubles rather than an fz_rect deliberately: `fz_rect` is an
/// anonymous typedef and CANNOT be forward-declared (see CLAUDE.md), so a
/// signature using it would drag MuPDF headers into every test translation
/// unit that wants to check this arithmetic.
///
/// WHY CLAMPING RATHER THAN JUST abs(): an image hanging off the edge of the
/// page is not coverage a reader can see. Normalising alone would turn a
/// wholly-off-page block into a large POSITIVE contribution — wrong in the
/// opposite direction from the original bug, and invisible to any test that
/// only asserts "not negative".
inline double clampedCoverageArea(double bx0, double by0, double bx1, double by1,
                                  double px0, double py0, double px1, double py1)
{
    const double x0 = std::max(std::min(bx0, bx1), px0);
    const double y0 = std::max(std::min(by0, by1), py0);
    const double x1 = std::min(std::max(bx0, bx1), px1);
    const double y1 = std::min(std::max(by0, by1), py1);
    if (x1 > x0 && y1 > y0) {
        return (x1 - x0) * (y1 - y0);
    }
    return 0.0;
}

} // namespace pdfe::core
