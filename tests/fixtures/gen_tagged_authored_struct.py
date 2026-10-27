#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate ``tests/fixtures/test_tagged_authored_struct.pdf`` — a tagged PDF
whose structure tree carries **authored** information that no auto-tagger could
re-derive from the page content.

GitHub #15 / meta-audit gap-register row **PDF-20**
(``PdfA2aConversionPreservesExistingStructTree``).

Why this fixture exists alongside ``test_tagged_multipage.pdf``
--------------------------------------------------------------
``test_tagged_multipage.pdf`` is tagged, but its tree is the *trivial* shape —
``/Document`` over one ``/P`` per page. A converter that threw the authored tree
away and ran the auto-tagger over the same three pages could plausibly land on
exactly that shape again, so an oracle built on it could not tell "preserved"
from "rebuilt, coincidentally identically". That is the whole distinction
GitHub #15 turns on: the criterion is *preserves*, not *produces a valid one*.

So this fixture is authored to be **un-rederivable**, in four independent ways:

1. ``/Alt`` — alternate descriptions ("A cartographer's note ...") that appear
   NOWHERE in the page content stream. There is no source an auto-tagger could
   read them out of.
2. ``/ID`` — author-chosen structure-element identifiers
   (``pdfe-authored-elem-N``). Arbitrary opaque byte strings.
3. ``/T`` — human titles on the grouping elements.
4. **Nesting an auto-tagger would not produce**: ``/Document`` → ``/Part`` →
   two ``/Sect`` groups → heading/paragraph/list content, including a real
   ``/L`` → ``/LI`` → ``/LBody`` list on page 2 whose items are ordinary text
   runs. A geometry-driven tagger sees three pages of flat text; it has no way
   to know page 1 and page 2 belong to one section and page 3 to another.

The document is otherwise a normal, valid tagged PDF: ``/MarkInfo /Marked
true``, ``/Lang``, real ``BDC``/``EMC`` marked content per page with matching
``/StructParents`` and a live ``/ParentTree``, so it round-trips through a
PDF/A-2a conversion the way a real tagged document would.

Deterministic: fixed object order, ``deterministic_id=True``, no timestamps.
"""
from __future__ import annotations

from pathlib import Path
import sys

try:
    import pikepdf
    from pikepdf import Array, Dictionary, Name, Pdf, String
except ImportError:  # pragma: no cover
    print("ERROR: pikepdf not installed.  pip install pikepdf", file=sys.stderr)
    sys.exit(1)


FIXTURE_DIR = Path(__file__).resolve().parent
OUTPUT = FIXTURE_DIR / "test_tagged_authored_struct.pdf"

PAGE_W = 612
PAGE_H = 792

# Per page: a list of (structure type, visible text, authored /Alt).
# Each entry becomes one marked-content sequence with its own MCID and one
# /StructElem. The /Alt strings are deliberately unrelated to the visible text.
PAGES = [
    [   # page 0
        ("H1", "Coastline Survey",
         "A cartographer's note recorded off-page in 1974."),
        ("P", "The northern reach was surveyed first.",
         "Authored description with no counterpart in the content stream."),
    ],
    [   # page 1
        ("H2", "Instrument Log",
         "Authored heading description, page two."),
        ("LI_A", "Theodolite, serial 4471",
         "First authored list item description."),
        ("LI_B", "Chronometer, serial 902",
         "Second authored list item description."),
    ],
    [   # page 2
        ("H1", "Appendix",
         "Authored appendix description that the page never states."),
        ("P", "Raw readings are retained separately.",
         "Authored closing description."),
    ],
]


def build() -> None:
    pdf = Pdf.new()

    struct_root = pdf.make_indirect(Dictionary(Type=Name.StructTreeRoot))

    # /Document -> /Part -> two /Sect groups. None of this grouping is visible
    # in the page content; it is the authored document architecture.
    document_elem = pdf.make_indirect(Dictionary(
        Type=Name.StructElem, S=Name.Document, P=struct_root,
        T=String("Authored document root"),
        ID=String("pdfe-authored-elem-doc")))
    part_elem = pdf.make_indirect(Dictionary(
        Type=Name.StructElem, S=Name.Part, P=document_elem,
        T=String("Survey report"),
        ID=String("pdfe-authored-elem-part")))
    sect_a = pdf.make_indirect(Dictionary(
        Type=Name.StructElem, S=Name.Sect, P=part_elem,
        T=String("Section A spans pages one and two"),
        ID=String("pdfe-authored-elem-sect-a")))
    sect_b = pdf.make_indirect(Dictionary(
        Type=Name.StructElem, S=Name.Sect, P=part_elem,
        T=String("Section B"),
        ID=String("pdfe-authored-elem-sect-b")))

    page_refs = []
    # parent_tree_entries[page_index] = list indexed by MCID -> owning elem
    parent_tree_entries = []
    leaf_counter = 0

    # The list wrapper on page 1 is built lazily below.
    list_elem = None
    list_items = []

    sect_a_kids = []
    sect_b_kids = []

    for page_index, entries in enumerate(PAGES):
        chunks = []
        y = 700
        for mcid, (_kind, text, _alt) in enumerate(entries):
            chunks.append(
                f"/P <</MCID {mcid}>> BDC\n"
                "BT\n"
                "/F1 14 Tf\n"
                f"72 {y} Td\n"
                f"({text}) Tj\n"
                "ET\n"
                "EMC\n"
            )
            y -= 28
        content = "".join(chunks).encode("latin-1")

        page = pdf.make_indirect(Dictionary(
            Type=Name.Page,
            MediaBox=Array([0, 0, PAGE_W, PAGE_H]),
            Contents=pdf.make_stream(content),
            Resources=Dictionary(Font=Dictionary(F1=Dictionary(
                Type=Name.Font, Subtype=Name.Type1, BaseFont=Name.Helvetica))),
            StructParents=page_index,
        ))
        pdf.pages.append(pikepdf.Page(page))
        page_refs.append(pdf.pages[page_index].obj)

        by_mcid = []
        for mcid, (kind, _text, alt) in enumerate(entries):
            leaf_counter += 1
            elem_id = f"pdfe-authored-elem-{leaf_counter}"

            if kind.startswith("LI_"):
                # Build the /L wrapper once, on the first list item.
                if list_elem is None:
                    list_elem = pdf.make_indirect(Dictionary(
                        Type=Name.StructElem, S=Name.L, P=sect_a,
                        T=String("Authored instrument list"),
                        ID=String("pdfe-authored-elem-list")))
                    sect_a_kids.append(list_elem)
                li = pdf.make_indirect(Dictionary(
                    Type=Name.StructElem, S=Name.LI, P=list_elem,
                    T=String(f"List item {len(list_items) + 1}"),
                    ID=String(elem_id + "-li")))
                lbody = pdf.make_indirect(Dictionary(
                    Type=Name.StructElem, S=Name.LBody, P=li,
                    Alt=String(alt),
                    ID=String(elem_id),
                    Pg=page_refs[page_index],
                    K=mcid))
                li.K = Array([lbody])
                list_items.append(li)
                list_elem.K = Array(list_items)
                by_mcid.append(lbody)
                continue

            elem = pdf.make_indirect(Dictionary(
                Type=Name.StructElem, S=Name(f"/{kind}"),
                Alt=String(alt),
                ID=String(elem_id),
                Pg=page_refs[page_index],
                K=mcid))
            parent = sect_a if page_index < 2 else sect_b
            elem.P = parent
            (sect_a_kids if page_index < 2 else sect_b_kids).append(elem)
            by_mcid.append(elem)

        parent_tree_entries.append(by_mcid)

    sect_a.K = Array(sect_a_kids)
    sect_b.K = Array(sect_b_kids)
    part_elem.K = Array([sect_a, sect_b])
    document_elem.K = Array([part_elem])

    nums = Array()
    for page_index, owners in enumerate(parent_tree_entries):
        nums.append(page_index)
        nums.append(Array(owners))
    struct_root.ParentTree = pdf.make_indirect(Dictionary(Nums=nums))
    struct_root.ParentTreeNextKey = len(PAGES)
    struct_root.K = document_elem

    pdf.Root.StructTreeRoot = struct_root
    pdf.Root.MarkInfo = Dictionary(Marked=True)
    pdf.Root.Lang = String("en-US")
    pdf.Root.ViewerPreferences = Dictionary(DisplayDocTitle=True)

    with pdf.open_metadata() as meta:
        meta["dc:title"] = "PDFE Authored Structure Tree Fixture"

    pdf.save(str(OUTPUT), deterministic_id=True)
    print(f"Wrote {OUTPUT} ({OUTPUT.stat().st_size} bytes)")


if __name__ == "__main__":
    build()
