# -*- coding: utf-8 -*-
"""
Generator for tests/fixtures/neg_tz_grey_text.pdf

A minimal, customer-data-free reproduction of the two text-edit defects found in
a real Examplecorp RTV invoice (which is NOT committed -- it contains customer
data and this is a public repo). The fixture reproduces the exact triggering
traits:

  (a) ambient "-100 Tz" (negative horizontal text scaling) under a Y-flip CTM
      plus a NEGATIVE font size ("-20 Tf") -- the GDI-style triple-flip that
      renders upright, left-to-right text (stext dir == (1,0)) while leaving a
      negative horizontal text scale ambient for any subsequent edit. If the
      text editor does not neutralize that ambient -100 Tz, re-injected text is
      horizontally mirrored ("RTAV" -> "VATR").

  (b) a NON-black (grey #dbd0d0) text fill. If the editor does not capture and
      re-emit the original fill color, edited text turns black.

Regenerate with:  python tests/fixtures/gen_neg_tz_grey_text.py
"""
import io
import os

H = 200

# Content stream mirrors the real page's opening operators, including the exact
# structural detail that matters for the editor's code path: the page nests the
# text object in q/Q and sets "-100 Tz" *inside* the BT (after BT), so editing a
# Tj re-injects it under a still-active -100 Tz (the filter pass-through path).
# Putting -100 Tz before BT made the editor take a full-rebuild path that dropped
# the Tz and hid the mirror bug.
CONTENT = (
    b"q q\n"
    b"1 0 0 -1 0 %d cm\n"      # Y-flip CTM (device-space origin at top-left)
    b"BT\n"
    b"-100 Tz\n"                # negative horizontal text scaling (X mirror), INSIDE BT
    b"/F1 -20 Tf\n"             # NEGATIVE font size (Y flip within text space)
    b"0.118 0.114 0.114 rg\n"   # DARK ambient fill (like the real page)
    b"180 100 Td\n"
    b"0.859 0.816 0.816 rg\n"   # grey #dbd0d0 set LOCALLY for the SHIP TO span
    b"(SHIP TO) Tj\n"
    b"0.118 0.114 0.114 rg\n"   # back to dark
    b"0 60 Td (KEEP ME) Tj\n"   # SECOND span: keeps the editor on the filter pass-
    b"ET\n"                     # through path (only the edited Tj is swapped), so the
    b"Q Q\n" % H                # ambient -100 Tz survives -> mirror; and the span's
)                               # LOCAL grey is dropped -> color falls to dark ambient.


def _build(objs):
    out = io.BytesIO()
    out.write(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for i, obj in enumerate(objs, 1):
        offsets.append(out.tell())
        out.write(b"%d 0 obj\n" % i + obj + b"\nendobj\n")
    xref = out.tell()
    n = len(objs) + 1
    out.write(b"xref\n0 %d\n" % n + b"0000000000 65535 f \n")
    for k in range(1, n):
        out.write(b"%010d 00000 n \n" % offsets[k])
    out.write(b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (n, xref))
    return out.getvalue()


def main():
    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 %d] "
        b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>" % H,
        b"<< /Length %d >>\nstream\n" % len(CONTENT) + CONTENT + b"endstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "neg_tz_grey_text.pdf")
    with open(path, "wb") as f:
        f.write(_build(objs))
    print("wrote", path)


if __name__ == "__main__":
    main()
