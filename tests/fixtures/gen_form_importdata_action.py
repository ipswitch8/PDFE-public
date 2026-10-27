# -*- coding: utf-8 -*-
"""Generate tests/fixtures/form_importdata_action.pdf (+ its FDF payload).

PDF-49 (meta-audit-2026-07 gap register): nothing in the tree covers the
``/ImportData`` action.  It is the read-side counterpart of ``/SubmitForm``:
the PDF names a local file and asks the viewer to load form data out of it.
A viewer that honours it can be pointed at any path the user can read, and
the loaded values then become exfiltratable through any of the submit
routes.  PDFE does not implement ``/ImportData`` at all -- but that is an
absence, and an absence with no test in front of it is one commit deep.

WHAT THIS FIXTURE HAS TO GET RIGHT, and the reason it emits two files:

The obvious version of this test builds a PDF whose ``/ImportData`` names an
FDF that does not exist, opens it, and observes that no data was imported.
That test passes on a viewer that implements ``/ImportData`` perfectly,
because the import failed for want of a file.  So the FDF is generated too,
it is real, it is valid, and the consuming test copies BOTH into its working
directory so the relative reference resolves.  The only reason nothing gets
imported must be that PDFE declines to import.

The payload value is a sentinel -- ``PDFE_IMPORTDATA_EXECUTED`` -- chosen so
that the assertion can be "this string appears in no field", which is a
statement about the whole form rather than about one field the test
remembered to check.

Three action placements, because "ignored" has to mean ignored everywhere a
viewer might look:

  * the document ``/OpenAction``          -- fires on open, no click needed
  * a Link annotation's ``/A``            -- fires on click
  * the field's ``/AA /Fo`` (on-focus)    -- fires on tab-in

Regenerate with:  python tests/fixtures/gen_form_importdata_action.py
"""
from __future__ import annotations

import io
import sys
from pathlib import Path

import pikepdf
from pikepdf import Array, Dictionary, Name, Pdf, String

THIS_DIR = Path(__file__).resolve().parent

PDF_OUT = THIS_DIR / "form_importdata_action.pdf"
FDF_OUT = THIS_DIR / "form_importdata_payload.fdf"

# The sentinel a successful import would deposit into the form.
SENTINEL = "PDFE_IMPORTDATA_EXECUTED"

# Field names must match between the FDF and the PDF or the import is a
# no-op even on a viewer that performs it -- another way this fixture could
# quietly stop testing anything.
FIELD_NAMES = ("ImportTarget", "SecondTarget")


def build_fdf() -> bytes:
    """A minimal but genuinely valid FDF carrying the sentinel."""
    fields = "".join(
        "<< /T ({name}) /V ({value}) >>\n".format(name=name, value=SENTINEL)
        for name in FIELD_NAMES
    )
    body = (
        "%FDF-1.2\n"
        "1 0 obj\n"
        "<< /FDF << /Fields [\n" + fields + "] /F (form_importdata_action.pdf) >> >>\n"
        "endobj\n"
        "trailer\n"
        "<< /Root 1 0 R >>\n"
        "%%EOF\n"
    )
    return body.encode("utf-8")


def build_pdf() -> Pdf:
    pdf = Pdf.new()
    page = pdf.add_blank_page(page_size=(612, 792))

    font = pdf.make_indirect(
        Dictionary(
            Type=Name.Font,
            Subtype=Name.Type1,
            BaseFont=Name.Helvetica,
        )
    )

    # The /ImportData action, referenced from three places below.
    import_action = pdf.make_indirect(
        Dictionary(
            S=Name.ImportData,
            F=String("form_importdata_payload.fdf"),
        )
    )

    widgets = []
    for i, name in enumerate(FIELD_NAMES):
        top = 700 - i * 60
        widget = pdf.make_indirect(
            Dictionary(
                Type=Name.Annot,
                Subtype=Name.Widget,
                FT=Name.Tx,
                T=String(name),
                V=String(""),          # starts EMPTY -- the whole assertion
                DV=String(""),
                Rect=Array([100, top, 400, top + 30]),
                DA=String("/Helv 12 Tf 0 g"),
                F=4,
                P=page.obj,
                # On-focus placement: fires when the field is tabbed into.
                AA=Dictionary(Fo=import_action),
            )
        )
        widgets.append(widget)

    # Link-annotation placement: fires on click.
    link = pdf.make_indirect(
        Dictionary(
            Type=Name.Annot,
            Subtype=Name.Link,
            Rect=Array([100, 500, 400, 530]),
            P=page.obj,
            A=import_action,
            Border=Array([0, 0, 0]),
        )
    )

    page.obj[Name.Annots] = Array(widgets + [link])

    pdf.Root[Name.AcroForm] = Dictionary(
        Fields=Array(widgets),
        DA=String("/Helv 12 Tf 0 g"),
        DR=Dictionary(Font=Dictionary(Helv=font)),
        NeedAppearances=True,
    )

    # Document-open placement: fires with no user interaction at all.
    pdf.Root[Name.OpenAction] = import_action

    return pdf


def main() -> int:
    fdf_bytes = build_fdf()
    FDF_OUT.write_bytes(fdf_bytes)

    pdf = build_pdf()
    pdf.save(PDF_OUT)

    # Read back and assert the three placements survived the save. A
    # generator that silently drops one produces a fixture that tests less
    # than its docstring claims, which is the failure mode this project
    # keeps meeting.
    with Pdf.open(PDF_OUT) as check:
        assert Name.OpenAction in check.Root, "OpenAction placement lost"
        annots = check.pages[0].obj[Name.Annots]
        subtypes = [str(a[Name.Subtype]) for a in annots]
        assert "/Link" in subtypes, "Link placement lost"
        assert sum(1 for s in subtypes if s == "/Widget") == len(FIELD_NAMES), \
            "widget count changed"
        assert any(
            Name.AA in a and Name.Fo in a[Name.AA] for a in annots
        ), "on-focus placement lost"

    assert SENTINEL.encode("ascii") in fdf_bytes, "FDF lost its sentinel"
    assert SENTINEL.encode("ascii") not in PDF_OUT.read_bytes(), (
        "the sentinel leaked into the PDF itself; a test asserting the "
        "sentinel is absent from the document would then be vacuous"
    )

    sys.stdout.write(
        "wrote {pdf} ({pn} bytes) and {fdf} ({fn} bytes)\n".format(
            pdf=PDF_OUT.name,
            pn=PDF_OUT.stat().st_size,
            fdf=FDF_OUT.name,
            fn=FDF_OUT.stat().st_size,
        )
    )
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main())
