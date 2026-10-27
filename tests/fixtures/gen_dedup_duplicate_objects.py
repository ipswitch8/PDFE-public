# -*- coding: utf-8 -*-
"""
Generator for tests/fixtures/dedup_duplicate_objects.pdf

A single-page PDF that deliberately carries THREE byte-identical, all-referenced
font objects (objects 5, 6, 7). Every font is used from the page content stream,
so a plain garbage collect (do_garbage=1) keeps all three; only the dedup sweep
(do_garbage=3) merges them into one and renumbers the xref, shrinking the object
count.

This backs the differential oracle for mutant M09
(effectiveGarbageLevel(): `needsFullGc_ ? 3 : ...` -> `? 1`): a full-rewrite
save with needsFullGc_ set must dedup (gc=3, fewer objects) versus the same
save without it (gc=1, all duplicates retained). The mutant downgrades the
escalated case to gc=1, so the two saves produce the SAME object count and the
strict-inequality oracle fails — killing the mutant.

Regenerate with:  python tests/fixtures/gen_dedup_duplicate_objects.py
"""
import os

W, H = 612, 792
# Uses /F1 /F2 /F3 so all three duplicate font objects are referenced ("used"),
# which is what forces gc=1 to keep them and lets gc=3 dedup demonstrate itself.
CONTENT = (
    b"BT /F1 12 Tf 72 700 Td (A) Tj ET\n"
    b"BT /F2 12 Tf 72 680 Td (B) Tj ET\n"
    b"BT /F3 12 Tf 72 660 Td (C) Tj ET\n"
)
# THREE byte-identical font dictionaries (the dedup targets).
FONT = b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"


def _obj(num, body):
    return b"%d 0 obj\n" % num + body + b"\nendobj\n"


def build():
    out = bytearray()
    out += b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n"
    offsets = {}
    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] "
         b"/Resources << /Font << /F1 5 0 R /F2 6 0 R /F3 7 0 R >> >> "
         b"/Contents 4 0 R >>" % (W, H)),
        b"<< /Length %d >>\nstream\n" % len(CONTENT) + CONTENT + b"endstream",
        FONT,
        FONT,
        FONT,
    ]
    for i, body in enumerate(objs, 1):
        offsets[i] = len(out)
        out += _obj(i, body)

    xref = len(out)
    n = len(objs) + 1
    out += b"xref\n0 %d\n" % n
    out += b"0000000000 65535 f \n"
    for k in range(1, n):
        out += b"%010d 00000 n \n" % offsets[k]
    out += (b"trailer\n<< /Size %d /Root 1 0 R >>\n"
            b"startxref\n%d\n%%%%EOF\n" % (n, xref))
    return bytes(out)


def main():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "dedup_duplicate_objects.pdf")
    with open(path, "wb") as f:
        f.write(build())
    print("wrote", path)


if __name__ == "__main__":
    main()
