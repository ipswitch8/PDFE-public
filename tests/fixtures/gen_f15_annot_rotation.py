# -*- coding: utf-8 -*-
"""
Meta-audit follow-up F15 / gap PDF-52 — annotation placement under page
rotation.

Output: f15_annot_rotation.pdf — FOUR pages, identical in every respect
except their /Rotate value:

    page 0 -> /Rotate 0     (baseline)
    page 1 -> /Rotate 90
    page 2 -> /Rotate 180
    page 3 -> /Rotate 270

AUTHORED LITERALS (the test asserts against these, never against product
output):

    PAGE_WIDTH  = 400   (points, unrotated user space)
    PAGE_HEIGHT = 600
    ROTATIONS   = [0, 90, 180, 270]

Every page is blank white and carries ONE identical /Square annotation, filled
red, authored directly in USER space (never through the product):

    ANNOT_RECT = [40, 500, 160, 540]     (user space, y up)
    /C = /IC = [1 0 0]  (red)   /Contents = "F15-rot-marker"

Because the /Rect is identical on all four pages, the annotation's RENDERED
position must differ per page exactly as /Rotate dictates. That is the oracle:
the red block is the only ink on the page, so its centroid can be located by
pixel search and compared against the authored rotation transform of the
unrotated page's centroid.

Regenerate with:  python tests/fixtures/gen_f15_annot_rotation.py
"""
from pathlib import Path

import pikepdf
from pikepdf import Pdf, Dictionary, Array, Name, String

HERE = Path(__file__).parent
OUT = HERE / "f15_annot_rotation.pdf"

PAGE_WIDTH = 400
PAGE_HEIGHT = 600
ROTATIONS = [0, 90, 180, 270]
ANNOT_RECT = [40, 500, 160, 540]   # user space (y up), identical on every page
ANNOT_CONTENTS = "F15-rot-marker"


def main() -> None:
    pdf = Pdf.new()
    for rot in ROTATIONS:
        # Blank white page: an explicit white fill over the whole MediaBox so
        # the rendered background is deterministic rather than
        # renderer-default.
        content = (
            "1 1 1 rg\n"
            f"0 0 {PAGE_WIDTH} {PAGE_HEIGHT} re f\n"
        ).encode("ascii")
        stream = pdf.make_stream(content)
        page = Dictionary(
            Type=Name.Page,
            MediaBox=Array([0, 0, PAGE_WIDTH, PAGE_HEIGHT]),
            Rotate=rot,
            Contents=stream,
            Resources=Dictionary(),
        )
        pageObj = pdf.make_indirect(page)
        annot = pdf.make_indirect(Dictionary(
            Type=Name.Annot,
            Subtype=Name.Square,
            Rect=Array(ANNOT_RECT),
            C=Array([1, 0, 0]),
            IC=Array([1, 0, 0]),
            CA=1,
            F=4,                     # Print
            Contents=String(ANNOT_CONTENTS),
            NM=String(f"f15-rot-{rot}"),
            BS=Dictionary(W=2, S=Name.S),
            P=pageObj,
        ))
        pageObj.Annots = Array([annot])
        pdf.pages.append(pikepdf.Page(pageObj))

    pdf.save(OUT, linearize=False)
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes), rotations={ROTATIONS}, "
          f"annot rect={ANNOT_RECT}")


if __name__ == "__main__":
    main()
