#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate ``tests/fixtures/test_tagged_rolemap.pdf`` — a tagged 2-page PDF
carrying the four things ``test_tagged_multipage.pdf`` deliberately does NOT:

  1. a ``/StructTreeRoot /RoleMap`` mapping two CUSTOM structure types
     (``/PullQuote`` -> ``/P``, ``/Sidebar`` -> ``/Sect``) to standard ones;
  2. a ``/Figure`` element with a **distinctive** ``/Alt`` string — not the
     generic "Image" placeholder, so a survival assertion can tell "the alt
     text came through" apart from "a default was re-emitted";
  3. **three** marked-content IDs on page 0 in a known order, so an
     intra-page reading-order permutation is detectable (a fixture with one
     MCID per page cannot see one);
  4. a real ``/Artifact <</Type /Pagination /Subtype /Footer>> BDC ... EMC``
     footer sequence, which is content that must NOT be in the structure tree.

``test_tagged_multipage.pdf`` is used by ``test_struct_tree_preservation.cpp``
AND ``test_accessibility_tree.cpp`` with exact structural counts, so it is left
untouched; this is a second, additive fixture.

The reading order the file is BUILT with — the INPUT any "order unchanged"
oracle must compare against, never a second read-back of the same output — is:

    (page 0, MCID 0)  H1
    (page 0, MCID 1)  PullQuote  (custom, role-mapped to P)
    (page 0, MCID 2)  Figure     (distinctive /Alt)
    (page 1, MCID 0)  P

Text uses base-14 Helvetica so ContentStreamEditor finds real editable spans.

Deterministic: fixed object construction order, ``deterministic_id=True``.
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
OUTPUT = FIXTURE_DIR / "test_tagged_rolemap.pdf"

PAGE_W = 612
PAGE_H = 792

# The distinctive alt text. Asserted verbatim by the survival tests; it must
# not be the AutoTagger placeholder ("Image") or a survival assertion cannot
# distinguish preservation from re-derivation.
FIGURE_ALT = "Red barn at dusk, Vermont, 1974 archival print"

PAGE0_CONTENT = (
    # MCID 0 -- H1
    "/H1 <</MCID 0>> BDC\n"
    "BT\n/F1 18 Tf\n72 700 Td\n(Alpha heading on the first page) Tj\nET\n"
    "EMC\n"
    # MCID 1 -- custom /PullQuote, role-mapped to /P
    "/PullQuote <</MCID 1>> BDC\n"
    "BT\n/F1 12 Tf\n72 650 Td\n(Quoted material set as a pull quote) Tj\nET\n"
    "EMC\n"
    # MCID 2 -- /Figure (a filled rectangle stands in for artwork)
    "/Figure <</MCID 2>> BDC\n"
    "q\n0.8 0.2 0.2 rg\n72 500 200 100 re\nf\nQ\n"
    "EMC\n"
    # Page furniture: an ARTIFACT, deliberately outside the structure tree.
    "/Artifact <</Type /Pagination /Subtype /Footer>> BDC\n"
    "BT\n/F1 9 Tf\n300 40 Td\n(Page 1 of 2) Tj\nET\n"
    "EMC\n"
).encode("latin-1")

PAGE1_CONTENT = (
    "/P <</MCID 0>> BDC\n"
    "BT\n/F1 12 Tf\n72 700 Td\n(Bravo body text on the second page) Tj\nET\n"
    "EMC\n"
    "/Artifact <</Type /Pagination /Subtype /Footer>> BDC\n"
    "BT\n/F1 9 Tf\n300 40 Td\n(Page 2 of 2) Tj\nET\n"
    "EMC\n"
).encode("latin-1")


def build() -> None:
    pdf = Pdf.new()

    struct_root = pdf.make_indirect(Dictionary(Type=Name.StructTreeRoot))
    document_elem = pdf.make_indirect(
        Dictionary(Type=Name.StructElem, S=Name.Document, P=struct_root))

    page_refs = []
    for i, content in enumerate((PAGE0_CONTENT, PAGE1_CONTENT)):
        content_stream = pdf.make_stream(content)
        resources = Dictionary(Font=Dictionary(F1=Dictionary(
            Type=Name.Font, Subtype=Name.Type1, BaseFont=Name.Helvetica)))
        page = pdf.make_indirect(Dictionary(
            Type=Name.Page,
            MediaBox=Array([0, 0, PAGE_W, PAGE_H]),
            Contents=content_stream,
            Resources=resources,
            StructParents=i,
        ))
        pdf.pages.append(pikepdf.Page(page))
        page_refs.append(pdf.pages[i].obj)

    # Page 0: three elements in reading order H1, PullQuote(custom), Figure.
    se_h1 = pdf.make_indirect(Dictionary(
        Type=Name.StructElem, S=Name.H1, P=document_elem, Pg=page_refs[0], K=0))
    se_pq = pdf.make_indirect(Dictionary(
        Type=Name.StructElem, S=Name.PullQuote, P=document_elem,
        Pg=page_refs[0], K=1))
    se_fig = pdf.make_indirect(Dictionary(
        Type=Name.StructElem, S=Name.Figure, P=document_elem,
        Pg=page_refs[0], K=2, Alt=String(FIGURE_ALT)))
    # Page 1: one paragraph.
    se_p1 = pdf.make_indirect(Dictionary(
        Type=Name.StructElem, S=Name.P, P=document_elem, Pg=page_refs[1], K=0))

    document_elem.K = Array([se_h1, se_pq, se_fig, se_p1])

    # ParentTree: key = page /StructParents, value = array indexed by MCID.
    nums = Array()
    nums.append(0)
    nums.append(Array([se_h1, se_pq, se_fig]))
    nums.append(1)
    nums.append(Array([se_p1]))
    parent_tree = pdf.make_indirect(Dictionary(Nums=nums))

    struct_root.K = document_elem
    struct_root.ParentTree = parent_tree
    struct_root.ParentTreeNextKey = 2
    # Two entries: one used (/PullQuote) and one declared-but-unused
    # (/Sidebar). Both must survive an edit -- a preservation path that
    # rebuilds only the mapping for types it currently SEES would silently
    # drop the second, and a single-entry fixture could not detect that.
    struct_root.RoleMap = Dictionary(PullQuote=Name.P, Sidebar=Name.Sect)

    pdf.Root.StructTreeRoot = struct_root
    pdf.Root.MarkInfo = Dictionary(Marked=True)
    pdf.Root.Lang = String("en-US")
    pdf.Root.ViewerPreferences = Dictionary(DisplayDocTitle=True)

    with pdf.open_metadata() as meta:
        meta["dc:title"] = "PDFE tagged RoleMap / artifact / reading-order fixture"

    pdf.save(str(OUTPUT), deterministic_id=True)
    print(f"Wrote {OUTPUT} ({OUTPUT.stat().st_size} bytes)")


if __name__ == "__main__":
    build()
