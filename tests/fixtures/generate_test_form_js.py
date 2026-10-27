# -*- coding: utf-8 -*-
"""Generate tests/fixtures/test_form_js.pdf -- GitHub #210 (register row A5-20).

Regenerate with:  python tests/fixtures/generate_test_form_js.py

THE NAME IS A LIE AND THE GENERATOR KEEPS IT THAT WAY ON PURPOSE.

The committed `test_form_js.pdf` contains **no JavaScript at all**: no `/AA` on
any field, no `/Names /JavaScript` in the catalog. Verified by re-extracting the
binary with `tests/fixtures/form_field_inventory.py`. The comment at
`tests/test_forms_phase5.cpp:448` ("carries calculation scripts but NO /AA /V
Validate action") describes an intent that the binary does not implement.

That matters for the one test that opens it.
`JavaScriptValidateEventReturnsBoolean` asserts `fireValidateEvent(...)` returns
**true**, which is spec-correct precisely BECAUSE no field carries a `/AA /V`
Validate action -- a field with no Validate script validates trivially. Adding
the implied Price x Quantity = Total calculate script would be a plausible
"improvement" that changes what the fixture means, and this generator exists to
make regeneration faithful, not to smuggle in a behaviour change. If the project
later wants a live calculate script, that is a deliberate fixture change with
its own test update, not a side effect of restoring reproducibility.

AUTHORED GROUND TRUTH

  * 1 page, MediaBox [0 0 612 792], /Rotate 0, 3 widget annotations.
  * AcroForm /DA "/Helv 0 Tf 0 g", /DR /Font /Helv. No /CO, no /NeedAppearances.
  * Three text fields, each Ff 0, MaxLen 100, /F 4,
    /DA "/Helv 10 Tf 0 0 0 rg", /MK << /BC [0 0 0] /BG [1 1 1] >>,
    /BS << /S /S /W 1 >>, and a single /N appearance stream:

        Price     V "100"  DV "100"  Rect [180 690 380 710]
        Quantity  V "2"    DV "2"    Rect [180 650 380 670]
        Total     V "200"  DV "200"  Rect [180 610 380 630]

    The 100 x 2 = 200 relationship is suggestive and inert: nothing in the
    file computes it.

Byte identity is not claimed; see generate_test_form.py's header for why, and
`verify_form_fixture_generators.py` for what is compared instead.
"""
from __future__ import annotations

import os
import sys
from pathlib import Path

import pikepdf
from pikepdf import Array, Dictionary, Name, String

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_test_form import _blank_stream, _bs, _mk, TEXT_DA  # noqa: E402

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HERE = Path(__file__).resolve().parent
OUT = HERE / "test_form_js.pdf"

FIELDS = [
    ("Price", [180, 690, 380, 710], "100"),
    ("Quantity", [180, 650, 380, 670], "2"),
    ("Total", [180, 610, 380, 630], "200"),
]


def main(out_path=None):
    out = Path(out_path) if out_path else OUT
    pdf = pikepdf.new()
    page = pdf.add_blank_page(page_size=(612, 792))
    page.obj[Name("/MediaBox")] = Array([0, 0, 612, 792])
    page.obj[Name("/Rotate")] = 0

    helv = pdf.make_indirect(Dictionary(
        Type=Name("/Font"), Subtype=Name("/Type1"),
        BaseFont=Name("/Helvetica"), Encoding=Name("/WinAnsiEncoding"),
    ))

    fields = []
    for name, rect, value in FIELDS:
        w = pdf.make_indirect(Dictionary(
            Type=Name("/Annot"), Subtype=Name("/Widget"),
            FT=Name("/Tx"), T=String(name), Ff=0,
            V=String(value), DV=String(value), MaxLen=100,
            Rect=Array(rect), F=4, DA=String(TEXT_DA),
            MK=_mk(), BS=_bs(), P=page.obj,
            AP=Dictionary(N=_blank_stream(pdf, rect)),
        ))
        fields.append(w)

    page.obj[Name("/Annots")] = Array(fields)
    pdf.Root[Name("/AcroForm")] = pdf.make_indirect(Dictionary(
        Fields=Array(fields),
        DA=String("/Helv 0 Tf 0 g"),
        DR=Dictionary(Font=Dictionary(Helv=helv)),
    ))
    # Deliberately absent: /AA on every field, /Names /JavaScript in the Root.
    # See the module docstring -- their absence is what makes
    # JavaScriptValidateEventReturnsBoolean's `true` the correct answer.

    pdf.save(str(out))
    print("wrote", out, os.path.getsize(out), "bytes;", len(fields),
          "text fields, 0 JavaScript actions (deliberate -- see docstring)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1] if len(sys.argv) > 1 else None))
