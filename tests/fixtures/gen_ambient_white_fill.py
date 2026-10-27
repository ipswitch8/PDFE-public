# -*- coding: utf-8 -*-
"""
Generator for tests/fixtures/ambient_white_fill.pdf

Regression fixture for the INBOUND fill-colour leak in AlignmentWriter
(Register H1 / docs/content-stream-state-continuity-audit.md, pipeline e5b9c231
Phase 2). AlignmentInjector appends the writer's fragment as a SEPARATE content
stream in the page's /Contents array. PDF concatenates the array's streams into
one content stream with NO graphics-state reset between them, so ambient state
left by the first stream is inherited by the appended fragment.

AlignmentWriter emits Tf/Tw/Tm/Tj inside a q...Q but never sets a fill colour,
so on a page whose ambient fill is non-black the aligned text is drawn in that
colour. This fixture leaves the ambient fill WHITE (1 1 1 rg) at top level, so an
un-fixed writer renders the aligned text white -> invisible.

Regenerate with:  python tests/fixtures/gen_ambient_white_fill.py
"""
import io, os

W, H = 612, 792
# Top-level white fill, never reset, then a filled rectangle so the white fill
# is unmistakably the ambient graphics state when the appended fragment runs.
CONTENT = (
    b"0 0 0 rg\n"                        # black
    b"BT /F1 12 Tf 72 750 Td (NEIGHBOR) Tj ET\n"  # pre-existing black neighbor text
    b"1 1 1 rg\n"                        # ambient WHITE fill, never reset
    b"50 50 100 100 re f\n"
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
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ambient_white_fill.pdf")
    with open(path, "wb") as f:
        f.write(_build(objs))
    print("wrote", path)


if __name__ == "__main__":
    main()
