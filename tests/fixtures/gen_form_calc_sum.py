# -*- coding: utf-8 -*-
"""
Generator for an AcroForm calculate-JavaScript fixture, for a form-JS
calculation oracle.

Output: form_calc_sum.pdf (1 page). AcroForm with THREE text fields:

  * field1  (FT=Tx)  -- addend
  * field2  (FT=Tx)  -- addend
  * field3  (FT=Tx)  -- result; carries a /AA /C (calculate) JavaScript action
                        that computes field1 + field2.

The calculate JS on field3 (exact source):

    var a=this.getField("field1").value;
    var b=this.getField("field2").value;
    event.value = (a==""?0:Number(a))+(b==""?0:Number(b));

The AcroForm /CO (calculation-order) array is set to [field3], and
/NeedAppearances is true so a viewer regenerates appearances after recompute.

Expected-sum contract: with field1="2" and field2="3", running the calculate
action sets field3 to 5.

Raw-pikepdf field-dict pattern, mirroring make_doc_js_fixture.py (build the
widget/field dicts, wire AcroForm.Fields, the /AA/C action, and /CO).

Regenerate with:  python tests/fixtures/gen_form_calc_sum.py
"""
from pathlib import Path

import pikepdf
from pikepdf import Pdf, Dictionary, Array, Name, String


HERE = Path(__file__).parent
OUT = HERE / "form_calc_sum.pdf"

CALC_JS = (
    'var a=this.getField("field1").value; '
    'var b=this.getField("field2").value; '
    'event.value = (a==""?0:Number(a))+(b==""?0:Number(b));'
)


def _text_field(pdf, name, rect):
    """Build one indirect text-field/widget dict (merged field+widget)."""
    return pdf.make_indirect(Dictionary(
        FT=Name.Tx,
        T=String(name),
        V=String(""),
        Ff=0,
        Type=Name.Annot,
        Subtype=Name.Widget,
        Rect=Array(rect),
    ))


def main() -> None:
    pdf = Pdf.new()
    pdf.add_blank_page(page_size=(612, 792))
    page = pdf.pages[0]

    field1 = _text_field(pdf, "field1", [72, 700, 220, 720])
    field2 = _text_field(pdf, "field2", [72, 670, 220, 690])
    field3 = _text_field(pdf, "field3", [72, 640, 220, 660])

    # Parent-link each widget to the page, and attach the calculate action to
    # field3 via /AA /C (additional-actions calculate).
    for fld in (field1, field2, field3):
        fld.P = page.obj
    field3.AA = Dictionary(
        C=Dictionary(
            S=Name.JavaScript,
            JS=String(CALC_JS),
        )
    )

    page.Annots = Array([field1, field2, field3])

    pdf.Root.AcroForm = Dictionary(
        Fields=Array([field1, field2, field3]),
        CO=Array([field3]),           # calculation order: field3 recomputes
        NeedAppearances=True,
    )

    pdf.save(str(OUT))
    print("wrote", OUT)


if __name__ == "__main__":
    main()
