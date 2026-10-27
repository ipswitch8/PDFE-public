# -*- coding: utf-8 -*-
# Generates tests/fixtures/smalltext_edit.pdf — one page with four single-word
# text lines at descending font sizes (18 / 11 / 9 / 7 pt), each a distinct token.
# Used by tests/test_smalltext_edit_render.cpp to prove the content-stream text
# edit lands for LARGE text but silently no-ops for SMALL text (bugfix-3e91a7c4
# #4/#5). Root cause: ContentStreamEditor's glyph matcher builds the glyph box via
# `pageHeight - y` (discarding the page's coordinate ORIGIN) while the span box is
# in MuPDF's CropBox-normalized space, so on a page with a NONZERO MediaBox origin
# the two diverge by a constant Δ = origin.y0. Small spans (half-height < Δ) can't
# absorb Δ -> zero glyphs match -> /Contents unchanged while the overlay shows the
# edit. So the fixture MUST replicate the real SpecimenA doc's box structure:
#   MediaBox [0 7.9 594 745.9]  (y-origin 7.9)  +  CropBox [0 0 594 738]
# With Δ≈7.9, edits fail below ~1.6·Δ≈12.6pt (11/9/7pt fail; 18pt works). Distinct
# tokens make the read-back oracle unambiguous.
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "smalltext_edit.pdf")

# (font size pt, baseline y, token). Widely-spaced y (within the CropBox 0..738)
# so each is its own line/span.
LINES = [
    (18.0, 700.0, "HEADINGBIG"),
    (11.0, 640.0, "BODYELEVEN"),
    (9.0,  590.0, "SMALLNINE"),
    (7.0,  540.0, "TINYSEVEN"),
]

# Each line is its own BT/ET with an absolute text matrix (Tm).
content = b""
for size, y, word in LINES:
    content += (b"BT /F1 %s Tf 1 0 0 1 72 %s Tm (%s) Tj ET\n"
                % (("%g" % size).encode(), ("%g" % y).encode(), word.encode()))

objs = [
    b"<< /Type /Catalog /Pages 2 0 R >>",
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 7.9 594 745.9] /CropBox [0 0 594 738] "
    b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
    b"<< /Length %d >>\nstream\n%s\nendstream" % (len(content), content),
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
]

pdf = b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n"
offsets = []
for i, body in enumerate(objs, start=1):
    offsets.append(len(pdf))
    pdf += b"%d 0 obj\n%s\nendobj\n" % (i, body)
xref_pos = len(pdf)
n = len(objs) + 1
pdf += b"xref\n0 %d\n" % n
pdf += b"0000000000 65535 f \n"
for off in offsets:
    pdf += b"%010d 00000 n \n" % off
pdf += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (n, xref_pos)

with open(OUT, "wb") as f:
    f.write(pdf)
print("wrote", OUT, len(pdf), "bytes")
