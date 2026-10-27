# -*- coding: utf-8 -*-
# Generates tests/fixtures/actualtext_prefix_crash.pdf — a minimal, public-domain
# synthetic PDF whose single span DISPLAYS "ABC" inside a marked-content block
# with /ActualText (A). The /ActualText string is a proper PREFIX of the
# displayed glyphs, which triggers the MuPDF 1.26.10 extraction crash
# (do_extract_within_actualtext, source/fitz/stext-device.c:1142): the unsigned
# running length `z` underflows to 0 while the span still has items ->
# assert(z != 0) (Debug abort) / rune_index(actualtext, (size_t)-1) OOB read
# (Release "Access Violation: read address 0x0").
#
# Regression: tests/test_actualtext_extraction_crash.cpp. See
# tests/docs/bugfix-3e91a7c4-phase1-findings.md.
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "actualtext_prefix_crash.pdf")

# Content stream: display "ABC" inside /Span <</ActualText(A)>> BDC ... EMC
content = b"""BT
/F1 24 Tf
72 700 Td
/Span <</ActualText(A)>> BDC
(ABC) Tj
EMC
ET
"""

objs = [
    b"<< /Type /Catalog /Pages 2 0 R >>",
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
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
