#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate ``tests/fixtures/test_tagged_multipage.pdf`` — a genuinely
tagged (PDF/UA-shaped) 3-page PDF with REAL per-page marked content.

Unlike ``test_pdfua.pdf`` (single page, empty ``/ParentTree /Nums``, no
in-stream marked content), this fixture is built so the full structure
chain is exercisable across edit families (R3-03):

  * Catalog: ``/MarkInfo <</Marked true>>`` + ``/Lang (en-US)`` +
    ``/StructTreeRoot``.
  * Each of the 3 pages carries a real content stream whose text-show
    operators are wrapped in a marked-content sequence
    ``/P <</MCID 0>> BDC ... EMC`` and a ``/StructParents N`` key.
  * ``/StructTreeRoot`` → ``/Document`` → one ``/P`` ``/StructElem`` per
    page, each with ``/Pg`` = that page's object ref and ``/K`` = MCID 0.
  * ``/ParentTree`` is a real number tree: ``/Nums [0 [p0] 1 [p1] 2 [p2]]``
    keyed by each page's ``/StructParents`` value; the value at key N is an
    array indexed by MCID → the ``/StructElem`` that owns MCID 0 on page N.

So every page's ``/StructParents`` indexes a live ``/ParentTree`` entry,
and every ``/StructElem /Pg`` resolves to a live page — the invariants the
R3-03 oracle asserts survive a text edit, a page delete, and a redaction.

Text uses base-14 Helvetica (no embedding needed — MuPDF renders and
extracts standard fonts), so the text-edit family (ContentStreamEditor)
finds a real, editable span on each page.

Deterministic: no timestamps, fixed object numbering via pikepdf. Checked
into tests/fixtures/ so generation only runs once.
"""
from __future__ import annotations

from pathlib import Path
import sys

try:
    import pikepdf
    from pikepdf import Pdf, Dictionary, Array, Name, String
except ImportError:
    print("ERROR: pikepdf not installed.  pip install pikepdf", file=sys.stderr)
    sys.exit(1)


FIXTURE_DIR = Path(__file__).parent
OUTPUT = FIXTURE_DIR / "test_tagged_multipage.pdf"

PAGE_W = 612
PAGE_H = 792
NUM_PAGES = 3

PAGE_TEXT = [
    "Alpha heading on the first page",
    "Bravo heading on the second page",
    "Charlie heading on the third page",
]


def build() -> None:
    pdf = Pdf.new()

    # StructTreeRoot shell (filled in after pages exist so we can /Pg them).
    struct_root = pdf.make_indirect(Dictionary(Type=Name.StructTreeRoot))
    document_elem = pdf.make_indirect(
        Dictionary(Type=Name.StructElem, S=Name.Document, P=struct_root))

    page_refs = []
    struct_elems = []

    for i in range(NUM_PAGES):
        content = (
            "/P <</MCID 0>> BDC\n"
            "BT\n"
            "/F1 18 Tf\n"
            "72 700 Td\n"
            f"({PAGE_TEXT[i]}) Tj\n"
            "ET\n"
            "EMC\n"
        ).encode("latin-1")

        content_stream = pdf.make_stream(content)

        font = Dictionary(
            Type=Name.Font,
            Subtype=Name.Type1,
            BaseFont=Name.Helvetica,
        )
        resources = Dictionary(Font=Dictionary(F1=font))

        page = pdf.make_indirect(Dictionary(
            Type=Name.Page,
            MediaBox=Array([0, 0, PAGE_W, PAGE_H]),
            Contents=content_stream,
            Resources=resources,
            StructParents=i,
        ))
        pdf.pages.append(pikepdf.Page(page))
        # pages.append may wrap/copy; fetch the canonical ref back.
        page_refs.append(pdf.pages[i].obj)

        # One /P StructElem per page, owning MCID 0 on that page.
        se = pdf.make_indirect(Dictionary(
            Type=Name.StructElem,
            S=Name.P,
            P=document_elem,
            Pg=page_refs[i],
            K=0,
        ))
        struct_elems.append(se)

    # Wire the /Document element's kids.
    document_elem.K = Array(struct_elems)

    # ParentTree number tree: key = page /StructParents value, value =
    # array indexed by MCID -> owning StructElem.
    nums = Array()
    for i in range(NUM_PAGES):
        nums.append(i)
        nums.append(Array([struct_elems[i]]))
    parent_tree = pdf.make_indirect(Dictionary(Nums=nums))

    struct_root.K = document_elem
    struct_root.ParentTree = parent_tree
    struct_root.ParentTreeNextKey = NUM_PAGES

    # Catalog wiring: mark as tagged + language.
    pdf.Root.StructTreeRoot = struct_root
    pdf.Root.MarkInfo = Dictionary(Marked=True)
    pdf.Root.Lang = String("en-US")
    pdf.Root.ViewerPreferences = Dictionary(DisplayDocTitle=True)

    with pdf.open_metadata() as meta:
        meta["dc:title"] = "PDFE R3-03 Tagged Multipage Fixture"

    pdf.save(str(OUTPUT), deterministic_id=True)
    print(f"Wrote {OUTPUT} ({OUTPUT.stat().st_size} bytes)")


if __name__ == "__main__":
    build()
