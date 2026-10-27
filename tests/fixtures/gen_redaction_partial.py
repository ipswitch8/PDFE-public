# -*- coding: utf-8 -*-
"""
Generator for tests/fixtures/redaction_partial_overlap.pdf

A page with a single visible word "SECRET" used to prove the RedactionVerifier
partial-overlap security fix (Register M-c / pipeline e5b9c231 Phase 2). The old
verifier used FULL CONTAINMENT (a glyph counted as "still present in the redacted
region" only if its box was entirely inside the region), so leftover text that
merely straddled the region boundary passed verification -- a false-negative.
The test builds a region that intersects every glyph but fully contains none.

Regenerate with:  python tests/fixtures/gen_redaction_partial.py
"""
import io, os

W, H = 612, 792
CONTENT = b"BT /F1 24 Tf 0 0 0 rg 100 700 Td (SECRET) Tj ET\n"


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
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] "
        b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>" % (W, H),
        b"<< /Length %d >>\nstream\n" % len(CONTENT) + CONTENT + b"endstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "redaction_partial_overlap.pdf")
    with open(path, "wb") as f:
        f.write(_build(objs))
    print("wrote", path)


if __name__ == "__main__":
    main()
