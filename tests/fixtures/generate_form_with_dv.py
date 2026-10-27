# -*- coding: utf-8 -*-
"""Generate test_form_with_dv.pdf — a form PDF with explicit /DV entries.

Copies test_form.pdf and adds a /DV (default value) entry to each of
the 5 value-carrying form field types. After ``Forms > Reset Form``,
MuPDF's ``pdf_reset_form`` restores each field's /V to its /DV.

Field → /DV assignments (per docs/form-fixture-debt.md):
  FirstName (Text)       → "Default Name"
  AgreeTerms (Checkbox)  → "/Yes"
  Gender (RadioButton)   → "Female"
  Country (ComboBox)     → "USA"
  Interests (ListBox)    → "Programming"
"""
from __future__ import annotations

import sys
from pathlib import Path

import pikepdf
from pikepdf import Name, String

FIXTURES = Path(__file__).parent
INPUT = FIXTURES / "test_form.pdf"
OUTPUT = FIXTURES / "test_form_with_dv.pdf"

DV_MAP = {
    "FirstName": String("Default Name"),
    "AgreeTerms": Name("/Yes"),
    "Gender": String("Female"),
    "Country": String("USA"),
    "Interests": String("Programming"),
}


def main() -> int:
    if not INPUT.exists():
        print(f"ERROR: {INPUT} not found", file=sys.stderr)
        return 1

    pdf = pikepdf.open(INPUT)

    acroform = pdf.Root.get("/AcroForm")
    if acroform is None:
        print("ERROR: source PDF has no AcroForm", file=sys.stderr)
        return 1

    fields = acroform.get("/Fields")
    if fields is None:
        print("ERROR: AcroForm has no /Fields", file=sys.stderr)
        return 1

    updated = 0
    for field in fields:
        field_obj = field if isinstance(field, pikepdf.Dictionary) else pdf.get_object(field)
        name = str(field_obj.get("/T", ""))
        if name in DV_MAP:
            field_obj[Name("/DV")] = DV_MAP[name]
            updated += 1
            print(f"  {name}: /DV = {DV_MAP[name]}")

    if updated != len(DV_MAP):
        missing = set(DV_MAP) - {str(f.get("/T", "")) for f in fields}
        print(f"WARNING: missing fields: {missing}", file=sys.stderr)

    pdf.save(OUTPUT)
    print(f"Saved: {OUTPUT} ({OUTPUT.stat().st_size} bytes, {updated} /DV entries)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
