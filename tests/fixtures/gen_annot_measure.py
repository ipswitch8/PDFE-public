# -*- coding: utf-8 -*-
"""Generate tests/fixtures/annot_measure.pdf.

PDF-57 (meta-audit-2026-07 gap register) / GitHub issue #21: PDFE had NO
read side for ISO 32000-1 §12.9 measurement properties at all -- a grep of
every ``.cpp``/``.h`` for ``/VP``, ``/Viewport`` or ``/Measure`` returned only
the English word "measured" in comments.  A drawing that states its own scale
was measured at 1:1 against default user space, silently.

This fixture states a scale that is *arithmetically knowable*, so the oracle
never has to trust the product's own answer:

  page /VP  ->  TWO viewports, deliberately OVERLAPPING.

    index 0  "SitePlan"   /BBox [0 0 612 792]   (the whole page)
             /Measure /R  "1 in = 10 ft"
                      /X  [ << /U (ft) /C 0.1388888888888889 /D 100 >> ]
                      /D  [ << /U (ft) /C 0.1388888888888889 /D 100 >> ]
                      /A  [ << /U (sq ft) /C 0.019290123456790123 /D 10 >> ]

             0.1388888888888889 == 10 / 72 exactly in the sense that matters:
             one inch on the page (72 default user space units) is ten feet,
             so ONE unit is 10/72 ft.  A 72pt distance must therefore display
             as 10.00 ft, and a 144pt distance as 20.00 ft -- values a test
             can assert without reimplementing the parser.
             The area factor is (10/72)**2 == 0.019290123456790123, so a
             10368 pt^2 region (one square inch is 5184 pt^2; two of them)
             displays as 200.0 sq ft.

    index 1  "DetailCallout"  /BBox [400 600 560 700]   (inside index 0)
             /Measure /R  "1 in = 2 m"
                      /X  [ << /U (m) /C 0.027777777777777776 /D 1000 >> ]
                      /D  [ << /U (m) /C 0.027777777777777776 /D 1000 >> ]
                      /A  [ << /U (sq m) /C 0.0007716049382716049 /D 100 >> ]

             2/72 = 0.027777777777777776.  It OVERLAPS viewport 0 on purpose:
             §12.9 says that when a point falls inside more than one viewport
             the one appearing LAST in the array shall be used, and scanning
             the array forwards gives the opposite answer.  A single-viewport
             fixture cannot tell a correct implementation from a wrong one
             here, so there are two, at different scales, with the smaller
             one last.

  index 2  "NoMeasure"  /BBox [0 0 100 100]
             A viewport with NO /Measure at all -- the negative control.
             ``MeasureDict::present`` must be false for it, and it must never
             be selected by ``viewportAt()``.  Without it, a reader that
             fabricated a default measure for every viewport would pass.

The number-format dictionaries also carry the presentational keys /F, /RD,
/RT, /PS, /SS and /O, with values that are NOT the specification defaults
(``/RD (,)``, ``/RT (.)``, ``/PS ( )``, ``/SS ( ft)``, ``/O /S``), so the
round-trip assertion covers the whole dictionary rather than only the three
entries the arithmetic reads.  A round-trip that compared only /U, /C and /D
would be green on a save that dropped everything else.

The page also carries one visible line, 72pt long, drawn between the two
points the test measures, so the fixture is inspectable by eye and a human
can confirm the "10 ft" claim with a ruler.

Regenerate with:  python tests/fixtures/gen_annot_measure.py
"""
from __future__ import annotations

import os

import pikepdf
from pikepdf import Array, Dictionary, Name, String

HERE = os.path.dirname(os.path.abspath(__file__))
OUTPUT = os.path.join(HERE, "annot_measure.pdf")

PAGE_W, PAGE_H = 612, 792

# One inch on the page is ten feet, so one default-user-space unit is 10/72 ft.
FT_PER_UNIT = 10.0 / 72.0
SQFT_PER_SQUNIT = FT_PER_UNIT * FT_PER_UNIT

# The detail callout: one inch is two metres.
M_PER_UNIT = 2.0 / 72.0
SQM_PER_SQUNIT = M_PER_UNIT * M_PER_UNIT


def number_format(unit: str, factor: float, precision: int,
                  suffix_label: str) -> Dictionary:
    """One number format dictionary -- ISO 32000-1 Table 266.

    Every presentational key is set to a NON-default value so a save that
    silently dropped it would be visible to the round-trip oracle.
    """
    return Dictionary(
        Type=Name.NumberFormat,
        U=String(unit),
        C=factor,
        F=Name.D,          # decimal fraction display
        D=precision,       # denominator of the smallest fraction shown
        RD=String(","),    # decimal separator (NOT the "." default)
        RT=String("."),    # thousands separator (NOT the "," default)
        PS=String(" "),    # prefix to the label
        SS=String(suffix_label),
        O=Name.S,          # label position: suffix
    )


def measure_dict(ratio: str, linear_unit: str, linear_factor: float,
                 linear_precision: int, area_unit: str, area_factor: float,
                 area_precision: int) -> Dictionary:
    """A rectilinear /Measure dictionary -- ISO 32000-1 Table 264."""
    linear = number_format(linear_unit, linear_factor, linear_precision,
                           " " + linear_unit)
    return Dictionary(
        Type=Name.Measure,
        Subtype=Name.RL,
        R=String(ratio),
        X=Array([linear]),
        D=Array([linear]),
        A=Array([number_format(area_unit, area_factor, area_precision,
                               " " + area_unit)]),
    )


def build() -> None:
    pdf = pikepdf.new()

    content = pdf.make_stream(
        b"q\n"
        b"1 w 0 0 0.6 RG\n"
        # The measured segment: (100, 400) -> (172, 400), exactly 72pt.
        b"100 400 m 172 400 l S\n"
        b"Q\n"
    )

    page = pdf.add_blank_page(page_size=(PAGE_W, PAGE_H))
    page.Contents = content

    site_plan = Dictionary(
        Type=Name.Viewport,
        BBox=Array([0, 0, PAGE_W, PAGE_H]),
        Name=String("SitePlan"),
        Measure=measure_dict("1 in = 10 ft", "ft", FT_PER_UNIT, 100,
                             "sq ft", SQFT_PER_SQUNIT, 10),
    )

    detail_callout = Dictionary(
        Type=Name.Viewport,
        BBox=Array([400, 600, 560, 700]),
        Name=String("DetailCallout"),
        Measure=measure_dict("1 in = 2 m", "m", M_PER_UNIT, 1000,
                             "sq m", SQM_PER_SQUNIT, 100),
    )

    no_measure = Dictionary(
        Type=Name.Viewport,
        BBox=Array([0, 0, 100, 100]),
        Name=String("NoMeasure"),
    )

    # ORDER MATTERS. DetailCallout is inside SitePlan and appears LATER, so
    # §12.9's "the one appearing last shall be used" is the rule under test.
    page.VP = Array([
        pdf.make_indirect(site_plan),
        pdf.make_indirect(detail_callout),
        pdf.make_indirect(no_measure),
    ])

    pdf.save(OUTPUT, static_id=True)
    print("wrote", OUTPUT)


if __name__ == "__main__":
    build()
