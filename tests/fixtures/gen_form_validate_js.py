# -*- coding: utf-8 -*-
"""Generate tests/fixtures/form_validate_js.pdf.

A1-60 (meta-audit-2026-07 status register): the pre-existing
``JavaScriptValidateEventReturnsBoolean`` oracle threw the verdict away
(``(void)result; SUCCEED();``) so a ``fireValidateEvent`` that always
returned true — i.e. a form-validation bypass — passed.

This fixture AUTHORS validation verdicts that are knowable without
running the product, so the test can assert the exact boolean for each
case.  Four text-field widgets, in this ``/Annots`` order:

  index 0  ``ShortOnly``    ``/AA /V /JS``: ``event.rc = (event.value.length <= 5)``
                            -> accepts "abc" (3), rejects "abcdefghij" (10);
                               the length-5 / length-6 pair is the boundary.
  index 1  ``AlwaysReject``  ``/AA /V /JS``: ``event.rc = false``
                            -> rejects EVERY value. A validator wired to a
                               constant-true stub fails here first.
  index 2  ``InRange``       ``/AA /V /JS``: accepts integers in [10, 20]
                            -> 9 reject / 10 accept / 20 accept / 21 reject.
  index 3  ``NoValidateScript``  no ``/AA`` at all
                            -> PDF 32000-1 + MuPDF ``pdf_field_event_validate``:
                               a field with no Validate action is accepted, so
                               the verdict must be true.

Every field carries ``/DA`` and the AcroForm carries ``/DR`` Helvetica so
appearances can be generated; ``/V`` starts empty for all four.

Regenerate with:  python tests/fixtures/gen_form_validate_js.py
"""
from __future__ import annotations

import os

import pikepdf
from pikepdf import Array, Dictionary, Name, String

HERE = os.path.dirname(os.path.abspath(__file__))
OUTPUT = os.path.join(HERE, "form_validate_js.pdf")

PAGE_W, PAGE_H = 612, 792

# (field name, rect, validate JS or None)
FIELDS = [
    (
        "ShortOnly",
        [100, 700, 300, 720],
        "event.rc = (event.value.length <= 5);",
    ),
    (
        "AlwaysReject",
        [100, 660, 300, 680],
        "event.rc = false;",
    ),
    (
        "InRange",
        [100, 620, 300, 640],
        "var n = parseInt(event.value, 10);"
        " event.rc = (!isNaN(n) && n >= 10 && n <= 20);",
    ),
    (
        "NoValidateScript",
        [100, 580, 300, 600],
        None,
    ),
]


def main() -> int:
    pdf = pikepdf.new()
    page = pdf.add_blank_page(page_size=(PAGE_W, PAGE_H))

    helv = pdf.make_indirect(
        Dictionary(
            Type=Name.Font,
            Subtype=Name.Type1,
            BaseFont=Name.Helvetica,
            Encoding=Name.WinAnsiEncoding,
        )
    )

    refs = []
    for name, rect, js in FIELDS:
        field = Dictionary(
            Type=Name.Annot,
            Subtype=Name.Widget,
            FT=Name.Tx,
            T=String(name),
            Ff=0,
            V=String(""),
            DA=String("/Helv 12 Tf 0 g"),
            F=4,  # Print
            Rect=Array(rect),
        )
        if js is not None:
            field[Name("/AA")] = Dictionary(
                V=Dictionary(S=Name.JavaScript, JS=String(js))
            )
        refs.append(pdf.make_indirect(field))

    page.Annots = Array(refs)
    pdf.Root.AcroForm = Dictionary(
        Fields=Array(refs),
        DA=String("/Helv 12 Tf 0 g"),
        DR=Dictionary(Font=Dictionary(Helv=helv)),
    )

    pdf.save(OUTPUT)
    print("wrote", OUTPUT, os.path.getsize(OUTPUT), "bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
