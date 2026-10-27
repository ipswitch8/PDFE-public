# -*- coding: utf-8 -*-
"""Generate tests/fixtures/form_comb_maxlen.pdf.

PDF-44 (meta-audit-2026-07 gap register) / GitHub issue #16: nothing in the
suite asserted that a COMB text field is laid out as ``/MaxLen`` equal cells
with one character centred per cell.  A comb field that rendered as ordinary
left-aligned text was indistinguishable from a correct one, because every
existing oracle only checked that the characters were PRESENT.

The comb flag is **bit 25** of ``/Ff`` (PDF 32000-1 table 228), i.e. the
value ``1 << 24`` = 16777216.  Per 12.7.4.3 a comb field divides the field
width into ``/MaxLen`` equally spaced positions and places one character in
each.

Two text fields, in this ``/Annots`` order.  They are geometrically
IDENTICAL — same width, same height, same ``/MaxLen``, same ``/DA``, same
border — and differ in exactly one bit:

  index 0  ``CombSix``   ``/Ff`` = 1 << 24 (comb), ``/MaxLen`` 6
                         -> six 20pt cells across the 120pt width; character
                            *i* centred at x = (i + 0.5) * 20.
  index 1  ``PlainSix``  ``/Ff`` = 0, ``/MaxLen`` 6
                         -> the CONTROL. Same MaxLen, no comb bit, so the
                            value must be drawn as one ordinary left-aligned
                            run.  Without it, an implementation that combed
                            EVERY text field would satisfy the comb half
                            perfectly.

Both carry ``/MK /BC`` (border colour) and ``/BS /W 1`` because MuPDF only
draws the comb cell DIVIDERS when the widget has a visible border.  Font size
is pinned to 10 in ``/DA`` — auto-size (``0 Tf``) would make the cell
arithmetic depend on the string, and the point of the fixture is that the
geometry is knowable without running the product.

Regenerate with:  python tests/fixtures/gen_form_comb_maxlen.py
"""
from __future__ import annotations

import os

import pikepdf
from pikepdf import Array, Dictionary, Name, String

HERE = os.path.dirname(os.path.abspath(__file__))
OUTPUT = os.path.join(HERE, "form_comb_maxlen.pdf")

PAGE_W, PAGE_H = 612, 792

# Bit 25 of /Ff (PDF 32000-1 table 228, "Comb").
COMB_FLAG = 1 << 24

MAX_LEN = 6
FIELD_W = 120.0          # -> cell width 120 / 6 = 20.0 exactly
FIELD_H = 20.0
BORDER_W = 1

# (field name, y-bottom, /Ff)
FIELDS = [
    ("CombSix", 700.0, COMB_FLAG),
    ("PlainSix", 660.0, 0),
]


def main() -> int:
    pdf = pikepdf.new()
    pdf.add_blank_page(page_size=(PAGE_W, PAGE_H))
    page = pdf.pages[0]

    helv = pdf.make_indirect(
        Dictionary(
            Type=Name.Font,
            Subtype=Name.Type1,
            BaseFont=Name.Helvetica,
            Encoding=Name.WinAnsiEncoding,
        )
    )

    refs = []
    for name, y0, ff in FIELDS:
        field = Dictionary(
            Type=Name.Annot,
            Subtype=Name.Widget,
            FT=Name.Tx,
            T=String(name),
            Ff=ff,
            MaxLen=MAX_LEN,
            V=String(""),
            DA=String("/Helv 10 Tf 0 g"),
            F=4,  # Print
            Rect=Array([100.0, y0, 100.0 + FIELD_W, y0 + FIELD_H]),
            # A visible border: MuPDF draws the comb cell dividers only when
            # the widget has one (has_bc in pdf_write_tx_widget_appearance).
            MK=Dictionary(BC=Array([0, 0, 0])),
            BS=Dictionary(W=BORDER_W, S=Name.S),
        )
        refs.append(pdf.make_indirect(field))

    page.Annots = Array(refs)
    pdf.Root.AcroForm = Dictionary(
        Fields=Array(refs),
        DA=String("/Helv 10 Tf 0 g"),
        DR=Dictionary(Font=Dictionary(Helv=helv)),
    )

    pdf.save(OUTPUT)
    print("wrote", OUTPUT, os.path.getsize(OUTPUT), "bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
