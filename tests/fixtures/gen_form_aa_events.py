# -*- coding: utf-8 -*-
"""Generate tests/fixtures/form_aa_events.pdf.

PDF-46 (meta-audit-2026-07 gap register) / GitHub issue #17: the suite fired
``/AA`` Format and Keystroke events but never asserted a SEMANTIC outcome.
Every existing oracle for those two events was satisfiable by a MuJS bridge
that never executed a line of script:

  * ``fireFormatEvent`` was only ever checked for determinism ("call it twice,
    get the same answer") or for not crashing on hostile input.  A bridge
    returning the raw ``/V`` unchanged — or the empty string — passed.
  * ``fireKeystrokeEvent`` was only ever checked for "returns a decision" and
    "does not throw".  A bridge hard-wired to accept everything passed.

This fixture AUTHORS real Format (``/AA /F``) and Keystroke (``/AA /K``)
actions whose outcomes are knowable without running the product, and — the
load-bearing property — whose outputs are NECESSARILY DIFFERENT from their
inputs.  That is what makes a working bridge distinguishable from a no-op.

Four text-field widgets, in this ``/Annots`` order:

  index 0  ``Amount``          ``/AA /F`` currency formatter, pure string ops
                               (no locale, no float, no Acrobat AF* helpers —
                               MuJS has none of those).  "1234567.5" must come
                               back as "$1,234,567.50": it gains a currency
                               sign, thousands separators AND a forced second
                               decimal, so NO substring of the raw value can
                               be mistaken for the formatted one.
  index 1  ``NoFormatAction``  no ``/AA`` at all.  ``pdf_field_event_format``
                               returns NULL, so the display value is empty.
                               The negative control: it pins that the rewrite
                               comes from the AUTHORED action and not from
                               PDFE formatting every field it sees.
  index 2  ``DigitsOnly``      ``/AA /K``: ``event.rc`` false unless every
                               character of ``event.change`` is 0-9.  Rejects
                               "a", accepts "3" — both directions, so neither
                               an accept-everything nor a reject-everything
                               bridge survives.  The empty change that MuPDF
                               sends on the willCommit pass is accepted, which
                               is what lets a valid value actually commit.
  index 3  ``UpperCase``       ``/AA /K``: ``event.change = event.change
                               .toUpperCase()``.  Committing "abc" through the
                               keystroke path must store "ABC".  A no-op
                               bridge cannot produce that value from that
                               input by any route.

Every field carries ``/DA`` and the AcroForm carries ``/DR`` Helvetica so
appearances can be generated; ``/V`` starts empty for all four.

Regenerate with:  python tests/fixtures/gen_form_aa_events.py
"""
from __future__ import annotations

import os

import pikepdf
from pikepdf import Array, Dictionary, Name, String

HERE = os.path.dirname(os.path.abspath(__file__))
OUTPUT = os.path.join(HERE, "form_aa_events.pdf")

PAGE_W, PAGE_H = 612, 792

# /AA /F — currency formatter.  Deliberately written with string operations
# only: no parseFloat (binary rounding), no toLocaleString (locale), no
# AFNumber_Format (an Acrobat host function MuJS does not provide).  The
# transformation is therefore exactly reproducible in the test's expectation.
FORMAT_CURRENCY_JS = (
    "var v = String(event.value);"
    " var neg = false;"
    " if (v.charAt(0) == '-') { neg = true; v = v.substring(1); }"
    " var parts = v.split('.');"
    " var whole = parts[0];"
    " if (whole == '') { whole = '0'; }"
    " var frac = (parts.length > 1) ? parts[1] : '';"
    " while (frac.length < 2) { frac = frac + '0'; }"
    " frac = frac.substring(0, 2);"
    " var grouped = '';"
    " while (whole.length > 3) {"
    "   grouped = ',' + whole.substring(whole.length - 3) + grouped;"
    "   whole = whole.substring(0, whole.length - 3);"
    " }"
    " grouped = whole + grouped;"
    " event.value = (neg ? '-$' : '$') + grouped + '.' + frac;"
)

# /AA /K — reject any change containing a non-digit.  An empty change (the
# willCommit pass MuPDF makes after the per-keystroke pass) is accepted.
KEYSTROKE_DIGITS_ONLY_JS = (
    "var ok = true;"
    " var c = String(event.change);"
    " for (var i = 0; i < c.length; i++) {"
    "   var ch = c.charAt(i);"
    "   if (ch < '0' || ch > '9') { ok = false; }"
    " }"
    " event.rc = ok;"
)

# /AA /K — rewrite the change rather than judging it.  Acceptance alone is
# unfalsifiable; a REWRITE is not.
KEYSTROKE_UPPERCASE_JS = "event.change = String(event.change).toUpperCase();"

# (field name, rect, additional-actions dict key or None, script)
FIELDS = [
    ("Amount", [100, 700, 300, 720], "F", FORMAT_CURRENCY_JS),
    ("NoFormatAction", [100, 660, 300, 680], None, None),
    ("DigitsOnly", [100, 620, 300, 640], "K", KEYSTROKE_DIGITS_ONLY_JS),
    ("UpperCase", [100, 580, 300, 600], "K", KEYSTROKE_UPPERCASE_JS),
]


def main() -> int:
    pdf = pikepdf.new()
    pdf.add_blank_page(page_size=(PAGE_W, PAGE_H))
    page = pdf.pages[0]

    helv = pdf.make_indirect(
        Dictionary(
            Type=Name.Font,
            Subtype=Name.Type1,
            BaseFont=Name.Helvetica,
            Encoding=Name.WinAnsiEncoding,
        )
    )

    refs = []
    for name, rect, aa_key, js in FIELDS:
        field = Dictionary(
            Type=Name.Annot,
            Subtype=Name.Widget,
            FT=Name.Tx,
            T=String(name),
            Ff=0,
            V=String(""),
            DA=String("/Helv 12 Tf 0 g"),
            F=4,  # Print
            Rect=Array(rect),
        )
        if aa_key is not None:
            field[Name("/AA")] = Dictionary(
                **{aa_key: Dictionary(S=Name.JavaScript, JS=String(js))}
            )
        refs.append(pdf.make_indirect(field))

    page.Annots = Array(refs)
    pdf.Root.AcroForm = Dictionary(
        Fields=Array(refs),
        DA=String("/Helv 12 Tf 0 g"),
        DR=Dictionary(Font=Dictionary(Helv=helv)),
    )

    pdf.save(OUTPUT)
    print("wrote", OUTPUT, os.path.getsize(OUTPUT), "bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
