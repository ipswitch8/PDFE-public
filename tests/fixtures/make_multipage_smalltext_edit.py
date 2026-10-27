# -*- coding: utf-8 -*-
# Generates tests/fixtures/multipage_smalltext_edit.pdf — THREE pages, each with a
# large title (18pt, control) and a small body token (9pt) that repeats across all
# pages. Nonzero MediaBox origin [0 7.9 594 745.9] (like the real SpecimenA doc), so
# the small 9pt spans hit the ContentStreamEditor origin-offset no-op pre-fix.
# Used by tests/test_smalltext_edit_render.cpp for:
#   - editing a small span on PAGE 2+ (index 1) lands in the rendered/extracted content
#   - Replace All updating EVERY matched page (not just page 1) + the reported count
#     matching the actual number of landed replacements.
# bugfix-3e91a7c4 #4/#5.
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "multipage_smalltext_edit.pdf")

NPAGES = 3
# per page: (18pt title token, 9pt body token). Body token repeats -> Replace-All target.
def page_content(i):
    title = ("PAGEBIG%d" % i).encode()
    body = b"REPLNINE"  # same on every page -> Replace All matches once per page
    return (b"BT /F1 18 Tf 1 0 0 1 72 700 Tm (%s) Tj ET\n"
            b"BT /F1 9 Tf 1 0 0 1 72 640 Tm (%s) Tj ET\n" % (title, body))

objs = []
objs.append(b"<< /Type /Catalog /Pages 2 0 R >>")
kids = b" ".join(b"%d 0 R" % (3 + k) for k in range(NPAGES))
objs.append(b"<< /Type /Pages /Kids [%s] /Count %d >>" % (kids, NPAGES))
# page objects (obj 3..3+N-1), then their content streams, then the font.
content_obj_base = 3 + NPAGES
font_obj = content_obj_base + NPAGES
for i in range(NPAGES):
    objs.append(
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 7.9 594 745.9] /CropBox [0 0 594 738] "
        b"/Resources << /Font << /F1 %d 0 R >> >> /Contents %d 0 R >>"
        % (font_obj, content_obj_base + i))
for i in range(NPAGES):
    c = page_content(i)
    objs.append(b"<< /Length %d >>\nstream\n%s\nendstream" % (len(c), c))
objs.append(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")

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
