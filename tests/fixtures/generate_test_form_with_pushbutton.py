# -*- coding: utf-8 -*-
"""Generate tests/fixtures/test_form_with_pushbutton.pdf -- GitHub #210.

Regenerate with:  python tests/fixtures/generate_test_form_with_pushbutton.py

WHY THIS FIXTURE IS SO SMALL, AND WHY IT MUST STAY SO

`tests/test_forms_phase5.cpp::PushButtonToggleNoCrash` reaches for it ONLY when
`test_form.pdf` has no pushbutton field -- and `test_form.pdf` has none, so this
is the file that test actually exercises. What it needs is exactly one `/Btn`
field with the pushbutton flag set, that `FormManager::toggleField` can be
dispatched against without throwing. It asserts nothing about the field's value
or appearance, and the committed binary reflects that: 649 bytes, one widget,
**no /AP, no /MK, no /BS, no /F, and an AcroForm carrying only /Fields** -- no
/DA, no /DR, no /NeedAppearances.

The absent keys are the specification, not an oversight. A pushbutton with no
/AP and no /MK is precisely the degenerate-but-legal shape a toggle dispatcher
has to survive; adding a helpful appearance stream would quietly retire the edge
case the fixture was built to cover.

AUTHORED GROUND TRUTH

  * 1 page, MediaBox [0 0 612 792], no /Rotate, 1 widget annotation.
  * AcroForm: /Fields only.
  * SubmitButton  /Btn  Ff 65536 (bit 17, Pushbutton)  Rect [72 700 172 730]
    -- no /V, no /DV, no /AP, no /MK, no /BS, no /F, no /DA.

Byte identity is not claimed; see generate_test_form.py's header for why, and
`verify_form_fixture_generators.py` for what is compared instead.
"""
from __future__ import annotations

import os
import sys
from pathlib import Path

import pikepdf
from pikepdf import Array, Dictionary, Name, String

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HERE = Path(__file__).resolve().parent
OUT = HERE / "test_form_with_pushbutton.pdf"

FF_PUSHBUTTON = 1 << 16  # 65536
assert FF_PUSHBUTTON == 65536


def main(out_path=None):
    out = Path(out_path) if out_path else OUT
    pdf = pikepdf.new()
    page = pdf.add_blank_page(page_size=(612, 792))
    page.obj[Name("/MediaBox")] = Array([0, 0, 612, 792])

    button = pdf.make_indirect(Dictionary(
        Type=Name("/Annot"), Subtype=Name("/Widget"),
        FT=Name("/Btn"), T=String("SubmitButton"), Ff=FF_PUSHBUTTON,
        Rect=Array([72, 700, 172, 730]), P=page.obj,
    ))
    page.obj[Name("/Annots")] = Array([button])
    pdf.Root[Name("/AcroForm")] = pdf.make_indirect(
        Dictionary(Fields=Array([button])))

    pdf.save(str(out))
    print("wrote", out, os.path.getsize(out),
          "bytes; 1 pushbutton field, no /AP or /MK (deliberate -- see "
          "docstring)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1] if len(sys.argv) > 1 else None))
