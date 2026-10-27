# -*- coding: utf-8 -*-
"""Generate tests/fixtures/cropbox_offset.pdf — hostile class 9 (A1-25):
a page whose /CropBox differs from its /MediaBox by a LARGE offset.

  MediaBox [0 0 1200 1600]      (1200 x 1600 pt, origin at 0,0)
  CropBox  [400 500 1000 1100]  (600 x 600 pt, origin offset by 400,500)

Everything coordinate-dependent in a PDF editor — redaction rects, extracted
text boxes, annotation /Rect, render output — must be expressed relative to
the CropBox, because MuPDF's "fitz space" puts the origin at the CropBox's
TOP-LEFT with Y running down:

    fitz_x = pdf_x - 400          pdf_x = fitz_x + 400
    fitz_y = 1100 - pdf_y         pdf_y = 1100 - fitz_y

A 400/500 offset is large enough that any code that forgets the CropBox
translation (or uses the MediaBox origin instead) lands on empty paper, which
is exactly what tests/test_cropbox_offset_coordinates.cpp detects.

AUTHORED GROUND TRUTH — all coordinates below are literals in this file and
are the ONLY oracle the C++ test is allowed to anchor to.

  token / mark   PDF user space (Y-up)              fitz space (Y-down)
  -------------  ---------------------------------  ---------------------------
  RED BLOCK      x 450..560,  y 1030..1070          x  50..160,  y  30.. 70
  "REDACTME"     baseline (450, 1000), Helv 24      baseline x 50, y 100
  "KEEPME"       baseline (450,  700), Helv 24      baseline x 50, y 400
  "OUTSIDEBOX"   baseline (100,  200), Helv 24      OUTSIDE the CropBox
                                                    (inside the MediaBox)

Vertical separation is deliberate: "REDACTME" (fitz y ~82..106) does not
overlap the red block (fitz y 30..70) and is 300 pt away from "KEEPME", so a
redaction sized to REDACTME cannot touch either by accident.

"OUTSIDEBOX" sits fully outside the CropBox but inside the MediaBox: it is the
control proving the fixture really carries out-of-crop content.

Text is drawn with the standard-14 Helvetica, so the advance widths are fixed
by the AFM and are themselves authored ground truth:
    "REDACTME" = R722 E667 D722 A667 C722 T611 M833 E667 = 5611/1000 * 24
               = 134.664 pt
    "KEEPME"   = K667 E667 E667 P667 M833 E667           = 4168/1000 * 24
               = 100.032 pt

The PDF is assembled as raw bytes (no pikepdf/reportlab dependency) with an
UNCOMPRESSED, directly greppable content stream, so the fixture is
deterministic and reproducible on any machine.

Regenerate with:  python tests/fixtures/gen_cropbox_offset.py
"""

import io
import os
import sys

# --- Authored geometry (PDF user space, Y-up) --------------------------------
MEDIABOX = (0, 0, 1200, 1600)
CROPBOX = (400, 500, 1000, 1100)

BLOCK = (450, 1030, 560, 1070)      # red block: x0 y0 x1 y1
REDACTME_ORIGIN = (450, 1000)       # text baseline origin
KEEPME_ORIGIN = (450, 700)
OUTSIDE_ORIGIN = (100, 200)
FONT_SIZE = 24

CONTENT = (
    # White paper across the whole MediaBox (not just the crop) so the
    # out-of-crop token sits on white, not on nothing.
    b"1 1 1 rg\n"
    b"%d %d %d %d re\nf\n" % (MEDIABOX[0], MEDIABOX[1],
                              MEDIABOX[2] - MEDIABOX[0],
                              MEDIABOX[3] - MEDIABOX[1])
    # Pure-red block at an exactly known absolute position inside the CropBox.
    + b"1 0 0 rg\n"
    + b"%d %d %d %d re\nf\n" % (BLOCK[0], BLOCK[1],
                                BLOCK[2] - BLOCK[0], BLOCK[3] - BLOCK[1])
    # "REDACTME" — the token the redaction test must destroy.
    + b"0 0 0 rg\n"
    + b"BT\n/F1 %d Tf\n%d %d Td\n(REDACTME) Tj\nET\n" % (
        FONT_SIZE, REDACTME_ORIGIN[0], REDACTME_ORIGIN[1])
    # "KEEPME" — the control that must SURVIVE the redaction.
    + b"BT\n/F1 %d Tf\n%d %d Td\n(KEEPME) Tj\nET\n" % (
        FONT_SIZE, KEEPME_ORIGIN[0], KEEPME_ORIGIN[1])
    # "OUTSIDEBOX" — inside the MediaBox, outside the CropBox.
    + b"BT\n/F1 %d Tf\n%d %d Td\n(OUTSIDEBOX) Tj\nET\n" % (
        FONT_SIZE, OUTSIDE_ORIGIN[0], OUTSIDE_ORIGIN[1])
)


def build_pdf():
    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R "
        b"/MediaBox [%d %d %d %d] "
        b"/CropBox [%d %d %d %d] "
        b"/Resources << /Font << /F1 5 0 R >> >> "
        b"/Contents 4 0 R >>" % (MEDIABOX + CROPBOX),
        b"<< /Length %d >>\nstream\n" % len(CONTENT) + CONTENT + b"endstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
        b"/Encoding /WinAnsiEncoding >>",
    ]

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
    out.write(
        b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n"
        % (n, xref)
    )
    return out.getvalue()


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    target = os.path.join(here, "cropbox_offset.pdf")
    data = build_pdf()
    with open(target, "wb") as f:
        f.write(data)
    sys.stdout.reconfigure(encoding="utf-8")
    print("wrote %s (%d bytes)" % (target, len(data)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
