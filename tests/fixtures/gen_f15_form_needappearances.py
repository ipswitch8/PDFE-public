# -*- coding: utf-8 -*-
"""
Meta-audit follow-up F15 / gaps PDF-41 + PDF-42 — /NeedAppearances and
widget appearance-stream regeneration.

Output: f15_form_needappearances.pdf — one page carrying an AcroForm with

    /NeedAppearances true          <- authored TRUE, so a reader that trusts
                                      the flag knows appearances are missing
    /DR << /Font << /Helv ... >> >>   (so an appearance CAN be generated)
    /DA (/Helv 12 Tf 0 g)

and ONE text field:

    /T (acct)   /FT /Tx   /V ()    and deliberately NO /AP entry at all.

AUTHORED LITERALS used by the oracle:

    FIELD_NAME     = "acct"
    INITIAL_VALUE  = ""            (empty)
    NEED_APPEARANCES = True

The gap: PDFE never read or wrote /NeedAppearances and nothing asserted that
filling a field produced an appearance stream, so a form filled by PDFE could
render BLANK in Acrobat/Chrome while every existing test stayed green.

Regenerate with:  python tests/fixtures/gen_f15_form_needappearances.py
"""
from pathlib import Path

import pikepdf
from pikepdf import Pdf, Dictionary, Array, Name, String

HERE = Path(__file__).parent
OUT = HERE / "f15_form_needappearances.pdf"

FIELD_NAME = "acct"
INITIAL_VALUE = ""
NEED_APPEARANCES = True
FIELD_RECT = [72, 700, 372, 724]


def main() -> None:
    pdf = Pdf.new()
    content = pdf.make_stream(b"1 1 1 rg\n0 0 612 792 re f\n")
    page = pdf.make_indirect(Dictionary(
        Type=Name.Page,
        MediaBox=Array([0, 0, 612, 792]),
        Contents=content,
        Resources=Dictionary(),
    ))

    helv = pdf.make_indirect(Dictionary(
        Type=Name.Font,
        Subtype=Name.Type1,
        BaseFont=Name.Helvetica,
        Encoding=Name.WinAnsiEncoding,
    ))

    field = pdf.make_indirect(Dictionary(
        FT=Name.Tx,
        T=String(FIELD_NAME),
        V=String(INITIAL_VALUE),
        DA=String("/Helv 12 Tf 0 g"),
        Ff=0,
        Type=Name.Annot,
        Subtype=Name.Widget,
        Rect=Array(FIELD_RECT),
        P=page,
        # NOTE: no /AP — that is the whole point of this fixture.
    ))
    page.Annots = Array([field])
    pdf.pages.append(pikepdf.Page(page))

    pdf.Root.AcroForm = pdf.make_indirect(Dictionary(
        Fields=Array([field]),
        NeedAppearances=NEED_APPEARANCES,
        DA=String("/Helv 12 Tf 0 g"),
        DR=Dictionary(Font=Dictionary(Helv=helv)),
    ))

    pdf.save(OUT, linearize=False)
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes), field={FIELD_NAME!r}, "
          f"NeedAppearances={NEED_APPEARANCES}")


if __name__ == "__main__":
    main()
