# -*- coding: utf-8 -*-
"""
Generator for tests/fixtures/hf_adversarial.pdf

Two adversarial traits for header/footer + watermark injectors
(pipeline e5b9c231 Phase 2 adjacent fixes):

  * NON-ZERO MediaBox origin [100 100 712 892] (612x792 page shifted by 100,100).
    Injectors that place at (pageWidth-...) / (pageHeight-...) assuming a (0,0)
    origin land offset by (100,100).

  * /F1 is a NON-Helvetica font (Courier Type1). An engine that hardcodes /F1 for
    its own Helvetica/WinAnsi text collides with this body font. (Courier here is
    still readable; the real hazard is a Type0/CID /F1 turning the bytes into
    wrong glyphs -- the fix avoids the collision entirely by using a fresh key.)

Regenerate with:  python tests/fixtures/gen_hf_adversarial.py
"""
import io, os

# MediaBox origin (100,100), size 612x792.
X0, Y0, X1, Y1 = 100, 100, 712, 892
CONTENT = b"BT /F1 10 Tf 0 0 0 rg 120 850 Td (BODY) Tj ET\n"


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
        b"<< /Type /Page /Parent 2 0 R /MediaBox [%d %d %d %d] "
        b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>" % (X0, Y0, X1, Y1),
        b"<< /Length %d >>\nstream\n" % len(CONTENT) + CONTENT + b"endstream",
        # /F1 is Courier (NOT Helvetica) -> collision hazard for the H/F engine.
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Courier >>",
    ]
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hf_adversarial.pdf")
    with open(path, "wb") as f:
        f.write(_build(objs))
    print("wrote", path)


if __name__ == "__main__":
    main()
