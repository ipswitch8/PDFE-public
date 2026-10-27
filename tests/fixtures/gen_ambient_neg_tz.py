# -*- coding: utf-8 -*-
"""
Generator for tests/fixtures/ambient_neg_tz_page.pdf

A page that leaves an ambient "-100 Tz" (negative horizontal text scaling)
active at the top level of its content stream (as a real invoice does via the
GDI triple-flip idiom). Injectors that APPEND a raw content stream and don't
reset the horizontal text scale inherit this and draw their text mirrored.
Used by test_watermark_state_leak.cpp (Register H1 / pipeline e5b9c231 Phase 2).

Regenerate with:  python tests/fixtures/gen_ambient_neg_tz.py
"""
import io, os

W, H = 612, 792
# Top-level ambient -100 Tz, left active (a real page reaches this state via an
# unbalanced/idiomatic text-state set). A small black marker line is drawn so
# the page is non-empty.
CONTENT = (
    b"-100 Tz\n"
    b"BT /F1 10 Tf 0 0 0 rg 72 750 Td (PAGE) Tj ET\n"
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
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] "
        b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>" % (W, H),
        b"<< /Length %d >>\nstream\n" % len(CONTENT) + CONTENT + b"endstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ambient_neg_tz_page.pdf")
    with open(path, "wb") as f:
        f.write(_build(objs))
    print("wrote", path)


if __name__ == "__main__":
    main()
