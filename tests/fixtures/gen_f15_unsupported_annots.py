# -*- coding: utf-8 -*-
"""
Meta-audit follow-up F15 / gaps PDF-56 + PDF-59 — annotation subtypes PDFE
does not model.

Output: f15_unsupported_annots.pdf — one page carrying TWELVE annotations
whose /Subtype PDFE maps to AnnotationType::Unknown:

  PDF-56 (standard PDF 1.7 markup, absent from src/annotations/):
      Polygon, PolyLine
  PDF-59 (rarely used, explicitly out of scope):
      Caret, Sound, Movie, Screen, Watermark, 3D, PrinterMark, TrapNet,
      RichMedia, Projection

AUTHORED LITERALS (the oracle compares against these, never against product
output):

    SUBTYPES  — the twelve names above, in this order
    /Contents on annotation i is exactly  "F15-<Subtype>-marker"
    /Rect     on annotation i is exactly  [50, 50+20*i, 150, 70+20*i]
    /NM       on annotation i is exactly  "f15-annot-<i>"

The gap: nothing proved such an annotation SURVIVES a PDFE save. An editor
that silently drops annotations it cannot model destroys third-party comment
data, and the suite was green either way.

Regenerate with:  python tests/fixtures/gen_f15_unsupported_annots.py
"""
from pathlib import Path

import pikepdf
from pikepdf import Pdf, Dictionary, Array, Name, String

HERE = Path(__file__).parent
OUT = HERE / "f15_unsupported_annots.pdf"

SUBTYPES = [
    "Polygon", "PolyLine",                       # PDF-56
    "Caret", "Sound", "Movie", "Screen",         # PDF-59
    "Watermark", "3D", "PrinterMark", "TrapNet",
    "RichMedia", "Projection",
]


def main() -> None:
    pdf = Pdf.new()
    content = pdf.make_stream(b"1 1 1 rg\n0 0 612 792 re f\n")
    page = pdf.make_indirect(Dictionary(
        Type=Name.Page,
        MediaBox=Array([0, 0, 612, 792]),
        Contents=content,
        Resources=Dictionary(),
    ))

    annots = []
    for i, sub in enumerate(SUBTYPES):
        d = Dictionary(
            Type=Name.Annot,
            Subtype=Name("/" + sub),
            Rect=Array([50, 50 + 20 * i, 150, 70 + 20 * i]),
            Contents=String(f"F15-{sub}-marker"),
            NM=String(f"f15-annot-{i}"),
            F=4,  # Print
            P=page,
        )
        if sub in ("Polygon", "PolyLine"):
            # Vertices are required for these two subtypes.
            d.Vertices = Array([50, 50 + 20 * i, 150, 50 + 20 * i,
                                150, 70 + 20 * i])
        annots.append(pdf.make_indirect(d))

    page.Annots = Array(annots)
    pdf.pages.append(pikepdf.Page(page))
    pdf.save(OUT, linearize=False)
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes), "
          f"{len(SUBTYPES)} annots: {SUBTYPES}")


if __name__ == "__main__":
    main()
