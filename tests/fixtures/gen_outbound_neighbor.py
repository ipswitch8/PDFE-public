# -*- coding: utf-8 -*-
"""
Generator for tests/fixtures/outbound_neighbor.pdf

REFUTATION fixture (pipeline e5b9c231 Phase 2). The audit predicted an OUTBOUND
state leak in ContentStreamEditor ("no q...Q around the injected BT...ET -> its
emitted state persists past ET and rescales/recolors a LATER text object").

This fixture is the adversarial witness for that scenario: two INDEPENDENT text
objects. Object 1 sets an ambient "-100 Tz" inside its BT and draws the editable
"EDITME" span; object 2 draws "NEIGHBOR" and deliberately does NOT set its own Tz
-- it relies on inheriting the ambient horizontal scale. If the editor's injected
"100 Tz" leaked past its ET into object 2, "NEIGHBOR" would render mirrored.

Empirically it does NOT: MuPDF's pdf_filter_page_contents wraps the injected
BT...ET in its own q...Q AND re-materialises the active text state (e.g. re-emits
"-100 Tz") at the start of every following text object, so the outbound path is
neutralised by the save pipeline. test_content_stream_state_leak.cpp asserts the
neighbour stays upright after the edit, making the refutation a committed,
reproducible fact rather than a code comment.

Regenerate with:  python tests/fixtures/gen_outbound_neighbor.py
"""
import io, os

H = 200
# Two independent text objects. Object 1 establishes the ambient -100 Tz; object
# 2 inherits it (sets its own -20 Tf so a leaked Tf is irrelevant, but NO Tz).
CONTENT = (
    b"q\n"
    b"1 0 0 -1 0 %d cm\n"        # Y-flip CTM
    b"BT\n"
    b"-100 Tz\n"                  # ambient horizontal scale, inside BT #1
    b"/F1 -20 Tf\n"
    b"0 0 0 rg\n"
    b"180 100 Td\n"
    b"(EDITME) Tj\n"            # OBJECT 1: the edited span
    b"ET\n"
    b"BT\n"                       # OBJECT 2: independent, UNEDITED neighbour
    b"/F1 -20 Tf\n"             # own font; NO Tz -> inherits ambient -100 Tz
    b"0 0 0 rg\n"
    b"180 40 Td\n"
    b"(NEIGHBOR) Tj\n"
    b"ET\n"
    b"Q\n" % H
)


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
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "outbound_neighbor.pdf")
    with open(path, "wb") as f:
        f.write(_build(objs))
    print("wrote", path)


if __name__ == "__main__":
    main()
