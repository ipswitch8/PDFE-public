# -*- coding: utf-8 -*-
"""
Meta-audit follow-up F15 / gap PDF-40 — FDF (Forms Data Format) import.

Output: f15_form_data.fdf — an Acrobat-shaped FDF whose /Fields target the
field names in tests/fixtures/test_form.pdf.

AUTHORED LITERALS (the oracle asserts exactly these values land in the form;
nothing is read back from product output to define "expected"):

    FirstName  -> "SpecimenA"
    LastName   -> "Examplecorp"
    Comments   -> "paren ) and backslash \\ survive"
    Country    -> "Canada"

The FDF also carries, deliberately:

  * an /FDF /JavaScript dictionary — PDFE must NEVER execute it (an FDF is
    untrusted input; the importer reads /Fields only),
  * an /FDF /F file-specification pointing at a bogus path — PDFE must never
    follow it,
  * a hierarchical field (/T (grp) with /Kids [ /T (child) ]) so the
    fully-qualified-name join ("grp.child") is exercised.

The file is written by hand (pikepdf writes PDFs, not FDFs) with a real
cross-reference table so it parses as ordinary PDF syntax.

Regenerate with:  python tests/fixtures/gen_f15_form_data_fdf.py
"""
from pathlib import Path

HERE = Path(__file__).parent
OUT = HERE / "f15_form_data.fdf"

VALUES = {
    "FirstName": "SpecimenA",
    "LastName": "Examplecorp",
    "Comments": "paren ) and backslash \\ survive",
    "Country": "Canada",
}
HIER_PARENT = "grp"
HIER_CHILD = "child"
HIER_VALUE = "nested-value"
# Must never run, must never be read:
EVIL_JS = "app.alert('f15-fdf-js-must-not-run');"


def esc(s: str) -> str:
    """Escape a Python str for a PDF literal string body."""
    out = []
    for ch in s:
        if ch in "\\()":
            out.append("\\" + ch)
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        else:
            out.append(ch)
    return "".join(out)


def main() -> None:
    fields = []
    for name, value in VALUES.items():
        fields.append(f"<< /T ({esc(name)}) /V ({esc(value)}) >>")
    fields.append(
        f"<< /T ({HIER_PARENT}) /Kids [ << /T ({HIER_CHILD}) "
        f"/V ({esc(HIER_VALUE)}) >> ] >>"
    )
    fields_str = "\n".join(fields)

    obj1 = (
        "1 0 obj\n"
        "<< /FDF << /Fields [\n"
        f"{fields_str}\n"
        "]\n"
        f"/JavaScript << /Before ({esc(EVIL_JS)}) /After ({esc(EVIL_JS)}) >>\n"
        "/F (C:\\\\does-not-exist\\\\f15.pdf)\n"
        ">> >>\n"
        "endobj\n"
    )

    header = "%FDF-1.2\n"
    body = header + obj1
    xref_offset = len(body.encode("latin-1"))
    obj1_offset = len(header.encode("latin-1"))

    xref = (
        "xref\n"
        "0 2\n"
        "0000000000 65535 f \n"
        f"{obj1_offset:010d} 00000 n \n"
        "trailer\n"
        "<< /Size 2 /Root 1 0 R >>\n"
        "startxref\n"
        f"{xref_offset}\n"
        "%%EOF\n"
    )

    OUT.write_bytes((body + xref).encode("latin-1"))
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes); values={VALUES}, "
          f"hierarchical={HIER_PARENT}.{HIER_CHILD}={HIER_VALUE!r}")


if __name__ == "__main__":
    main()
