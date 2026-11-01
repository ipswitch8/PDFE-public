# -*- coding: utf-8 -*-
"""
Generate tests/fixtures/shared_basefont_collision.pdf

Phase-3 RED-FIRST fixture for the ContentStreamEditor font-resource-key
collision (sweep Finding 1).

One Letter page (612x792) with TWO distinct Type1 font resources, /F0 and /F1,
that BOTH declare /BaseFont /Helvetica. Because they share one BaseFont name,
ContentStreamEditor's psNameToResKey map (keyed by BaseFont) collides: the
last-iterated resource (/F1, since F0 sorts first) wins. The content draws two
runs, (RUN ZERO) under /F0 and (RUN ONE) under /F1. An edit on the /F0 run must
be re-emitted under /F0 -- the bug emits it under the colliding alias /F1.

Writes hand-built raw PDF bytes (no large dependency). Probes the result with
PyMuPDF/fitz to confirm structure.
"""

import os
import sys

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "shared_basefont_collision.pdf")


def build_pdf() -> bytes:
    # Object bodies (1-indexed; index 0 is the free head of the xref table).
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
         b"/Resources << /Font << /F0 4 0 R /F1 5 0 R >> >> "
         b"/Contents 6 0 R >>"),
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        None,  # content stream, filled below
    ]

    content = (b"BT /F0 24 Tf 72 700 Td (RUN ZERO) Tj ET\n"
               b"BT /F1 24 Tf 72 650 Td (RUN ONE) Tj ET\n")
    objects[5] = (b"<< /Length " + str(len(content)).encode("ascii") +
                  b" >>\nstream\n" + content + b"endstream")

    out = bytearray()
    out += b"%PDF-1.7\n"
    out += b"%\xe2\xe3\xcf\xd3\n"  # binary marker

    offsets = [0] * (len(objects) + 1)
    for i, body in enumerate(objects, start=1):
        offsets[i] = len(out)
        out += str(i).encode("ascii") + b" 0 obj\n" + body + b"\nendobj\n"

    xref_pos = len(out)
    n = len(objects) + 1  # includes free object 0
    out += b"xref\n"
    out += b"0 " + str(n).encode("ascii") + b"\n"
    out += b"0000000000 65535 f \n"
    for i in range(1, n):
        out += ("%010d 00000 n \n" % offsets[i]).encode("ascii")

    out += b"trailer\n"
    out += (b"<< /Size " + str(n).encode("ascii") + b" /Root 1 0 R >>\n")
    out += b"startxref\n"
    out += str(xref_pos).encode("ascii") + b"\n"
    out += b"%%EOF\n"
    return bytes(out)


def probe(path: str) -> None:
    import fitz  # PyMuPDF
    doc = fitz.open(path)
    assert doc.page_count == 1, f"expected 1 page, got {doc.page_count}"
    page = doc[0]

    # Font dict: F0 and F1 both Helvetica.
    fonts = page.get_fonts(full=True)
    # get_fonts returns tuples: (xref, ext, type, basefont, name, encoding, ...)
    by_resname = {t[4]: t[3] for t in fonts}
    print("page fonts (resname -> basefont):", by_resname)
    assert "F0" in by_resname, "page Font dict missing /F0"
    assert "F1" in by_resname, "page Font dict missing /F1"
    assert "Helvetica" in by_resname["F0"], f"F0 basefont = {by_resname['F0']}"
    assert "Helvetica" in by_resname["F1"], f"F1 basefont = {by_resname['F1']}"

    # Content stream: /F0 ... (RUN ZERO) and /F1 ... (RUN ONE).
    raw = b"".join(doc.xref_stream(x) for x in page.get_contents())
    text = raw.decode("latin-1")
    print("content stream:\n" + text)
    assert "/F0" in text and "RUN ZERO" in text, "content missing /F0 RUN ZERO"
    assert "/F1" in text and "RUN ONE" in text, "content missing /F1 RUN ONE"
    i0 = text.index("/F0")
    i1 = text.index("/F1")
    assert i0 < i1, "/F0 must sort/appear before /F1"
    doc.close()
    print("PROBE OK: F0+F1 both Helvetica; runs drawn under /F0 and /F1.")


def main() -> int:
    data = build_pdf()
    with open(OUT, "wb") as f:
        f.write(data)
    print("wrote", OUT, "(", len(data), "bytes )")
    probe(OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
