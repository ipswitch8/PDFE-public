#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate ``tests/fixtures/pdf20_associated_files.pdf`` — a PDF 2.0 document
carrying **associated files** (ISO 32000-2 §14.13) at BOTH the catalog level and
the page level, each with a DISTINCT ``/AFRelationship``.

GitHub #2 / meta-audit gap-register row **PDF-04**.

Why the relationships differ from each other
--------------------------------------------
``/AFRelationship`` is the *only* thing that says what an associated file IS to
the document it hangs off: ``/Source`` is the file the PDF was produced from,
``/Data`` is the machine-readable payload a human-readable page renders,
``/Supplement`` is extra material, ``/Alternative`` is an alternate rendition.
A roundtrip that kept the ``/AF`` array but flattened every relationship to one
value — or dropped the key entirely — would preserve the attachments and destroy
their meaning. Four different values, on four different filespecs, make that
failure visible: a test can only pass by carrying each filespec's own name
across the save.

The catalog also declares the same filespecs through ``/Names /EmbeddedFiles``,
because that is the second, independent reachability path for an embedded file
(the union argument ``UnencryptedWrapperProbe`` and ``PdfSignatureProbe`` both
make). A writer that rebuilt only one of the two would leave the document
internally inconsistent.

Deterministic: fixed object order, ``deterministic_id=True``, no timestamps.
Checked into tests/fixtures/, so generation normally runs once.
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
OUTPUT = FIXTURE_DIR / "pdf20_associated_files.pdf"

PAGE_W = 612
PAGE_H = 792

# (filename, /AFRelationship, payload bytes, mime)
# The three CATALOG-level associated files.
CATALOG_AF = [
    ("invoice-source.xml",
     "Source",
     b"<?xml version='1.0' encoding='UTF-8'?>\n<invoice id='PDFE-AF-1'/>\n",
     "application/xml"),
    ("invoice-data.csv",
     "Data",
     b"id,amount\nPDFE-AF-2,42\n",
     "text/csv"),
    ("terms-supplement.txt",
     "Supplement",
     b"PDFE-AF-3 supplementary terms.\n",
     "text/plain"),
]

# The single PAGE-level associated file. Page-level /AF is a separate carrier
# from the catalog's and a writer can preserve one while losing the other.
PAGE_AF = ("page-alt-text.txt",
           "Alternative",
           b"PDFE-AF-4 alternate rendition of page 1.\n",
           "text/plain")


def make_filespec(pdf: Pdf, name: str, relationship: str, payload: bytes,
                  mime: str):
    """Build an indirect /Filespec with an /EF stream and an /AFRelationship."""
    ef_stream = pdf.make_stream(payload)
    ef_stream.Type = Name.EmbeddedFile
    # /Subtype on an /EmbeddedFile is a name whose '/' must be hex-escaped.
    ef_stream.Subtype = Name("/" + mime.replace("/", "#2F"))
    ef_stream.Params = Dictionary(Size=len(payload))

    return pdf.make_indirect(Dictionary(
        Type=Name.Filespec,
        F=String(name),
        UF=String(name),
        Desc=String(f"PDFE associated file, relationship {relationship}"),
        AFRelationship=Name("/" + relationship),
        EF=Dictionary(F=ef_stream, UF=ef_stream),
    ))


def build() -> None:
    pdf = Pdf.new()

    content = (
        "BT\n"
        "/F1 16 Tf\n"
        "72 700 Td\n"
        "(PDF 2.0 associated-files fixture) Tj\n"
        "0 -24 Td\n"
        "/F1 11 Tf\n"
        "(Four associated files: three on the catalog, one on this page.) Tj\n"
        "ET\n"
    ).encode("latin-1")

    page = pdf.make_indirect(Dictionary(
        Type=Name.Page,
        MediaBox=Array([0, 0, PAGE_W, PAGE_H]),
        Contents=pdf.make_stream(content),
        Resources=Dictionary(Font=Dictionary(F1=Dictionary(
            Type=Name.Font, Subtype=Name.Type1, BaseFont=Name.Helvetica))),
    ))
    pdf.pages.append(pikepdf.Page(page))
    page_ref = pdf.pages[0].obj

    catalog_specs = [make_filespec(pdf, *spec) for spec in CATALOG_AF]
    page_spec = make_filespec(pdf, *PAGE_AF)

    # Catalog-level /AF (ISO 32000-2 §14.13.2).
    pdf.Root.AF = Array(catalog_specs)

    # Page-level /AF (ISO 32000-2 §14.13.4) — the second carrier.
    page_ref.AF = Array([page_spec])

    # Second reachability path: the /EmbeddedFiles name tree. Every one of the
    # four filespecs appears here too, keyed by file name (sorted, as a name
    # tree requires).
    all_specs = list(zip([s[0] for s in CATALOG_AF] + [PAGE_AF[0]],
                         catalog_specs + [page_spec]))
    names = Array()
    for fname, spec in sorted(all_specs, key=lambda p: p[0]):
        names.append(String(fname))
        names.append(spec)
    pdf.Root.Names = Dictionary(EmbeddedFiles=Dictionary(Names=names))

    # PDF 2.0 header/version. pikepdf writes the header from min_version.
    pdf.Root.Version = Name("/2.0")

    with pdf.open_metadata() as meta:
        meta["dc:title"] = "PDFE PDF 2.0 Associated Files Fixture"

    pdf.save(str(OUTPUT), deterministic_id=True, min_version="2.0")
    print(f"Wrote {OUTPUT} ({OUTPUT.stat().st_size} bytes)")


if __name__ == "__main__":
    build()
