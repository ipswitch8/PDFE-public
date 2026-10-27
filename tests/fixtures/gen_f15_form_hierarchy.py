# -*- coding: utf-8 -*-
"""
Meta-audit follow-up F15 / gap PDF-45 — AcroForm field hierarchies and
inherited attributes.

Output: f15_form_hierarchy.pdf — one page whose AcroForm has a NESTED field
tree, the shape real government/enterprise forms use:

    /Fields [ grp ]
        grp        /T (grp)   /FT /Tx   /DA (/Helv 12 Tf 0 g)   <- inherited
            kid0   /T (name)      (widget; NO /FT, NO /DA)
            kid1   /T (city)      (widget; NO /FT, NO /DA)

AUTHORED LITERALS used by the oracle:

    PARENT      = "grp"
    CHILDREN    = ["name", "city"]
    QUALIFIED   = ["grp.name", "grp.city"]
    both children inherit /FT /Tx and the parent's /DA.

The gap: `pdf_dict_get_inheritable` had ZERO hits in src/forms/ and no test
used a fully-qualified name, so a flat name lookup could write the wrong field
or miss it entirely with the suite green.

Regenerate with:  python tests/fixtures/gen_f15_form_hierarchy.py
"""
from pathlib import Path

import pikepdf
from pikepdf import Pdf, Dictionary, Array, Name, String

HERE = Path(__file__).parent
OUT = HERE / "f15_form_hierarchy.pdf"

PARENT = "grp"
CHILDREN = ["name", "city"]
QUALIFIED = [f"{PARENT}.{c}" for c in CHILDREN]
RECTS = [[72, 700, 372, 724], [72, 660, 372, 684]]


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
        Type=Name.Font, Subtype=Name.Type1, BaseFont=Name.Helvetica,
        Encoding=Name.WinAnsiEncoding,
    ))

    parent = pdf.make_indirect(Dictionary(
        T=String(PARENT),
        FT=Name.Tx,                      # inherited by both kids
        DA=String("/Helv 12 Tf 0 g"),    # inherited by both kids
    ))

    kids = []
    for i, child in enumerate(CHILDREN):
        kid = pdf.make_indirect(Dictionary(
            T=String(child),
            Parent=parent,
            Type=Name.Annot,
            Subtype=Name.Widget,
            Rect=Array(RECTS[i]),
            V=String(""),
            P=page,
            # deliberately NO /FT and NO /DA — they must resolve through /Parent
        ))
        kids.append(kid)
    parent.Kids = Array(kids)

    page.Annots = Array(kids)
    pdf.pages.append(pikepdf.Page(page))

    pdf.Root.AcroForm = pdf.make_indirect(Dictionary(
        Fields=Array([parent]),
        DA=String("/Helv 12 Tf 0 g"),
        DR=Dictionary(Font=Dictionary(Helv=helv)),
    ))

    pdf.save(OUT, linearize=False)
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes), qualified={QUALIFIED}")


if __name__ == "__main__":
    main()
