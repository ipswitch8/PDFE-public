#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate test_link_types.pdf — a 3-page PDF with four link
annotations on page 0, one for each action type PDFE supports:

  Rect (50, 50)..(150, 100)   /A /S /URI https://example.test/follow-link-uri
  Rect (200, 50)..(300, 100)  /A /S /GoTo (go to page 2)
  Rect (50, 150)..(150, 200)  /A /S /Named /N NextPage
  Rect (200, 150)..(300, 200) /A /S /Launch (external file: notes.txt)

Coordinates are in PDF /Rect convention (Y-up origin bottom-left).
Page size: US Letter (612x792). Link rectangles are positioned in
the upper-left quadrant so pywinauto click coordinates are easy to
derive from page-to-screen mapping.

This is a hand-rolled minimal PDF to avoid external PDF library
dependencies at fixture-generation time.
"""

from __future__ import annotations

from pathlib import Path


def pdf_str(s: str) -> bytes:
    """Encode a string as a PDF literal with basic escaping."""
    return b"(" + s.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)").encode() + b")"


def make_pdf() -> bytes:
    # Object layout:
    #  1 Catalog   /Pages 2 0 R
    #  2 Pages     [3 0 R, 10 0 R, 11 0 R]
    #  3 Page 0    /Annots [4 0 R, 5 0 R, 6 0 R, 7 0 R]
    #  4 Link URI
    #  5 Link GoTo (→ page 2, object 11)
    #  6 Link Named NextPage
    #  7 Link Launch
    #  8 (spare)
    #  9 (spare)
    # 10 Page 1 (blank)
    # 11 Page 2 (blank)
    objs: dict[int, bytes] = {}

    objs[1] = b"<< /Type /Catalog /Pages 2 0 R >>"
    objs[2] = (
        b"<< /Type /Pages /Kids [3 0 R 10 0 R 11 0 R] /Count 3 >>"
    )
    objs[3] = (
        b"<< /Type /Page /Parent 2 0 R "
        b"/MediaBox [0 0 612 792] /Resources << >> "
        b"/Annots [4 0 R 5 0 R 6 0 R 7 0 R] >>"
    )
    objs[4] = (
        b"<< /Type /Annot /Subtype /Link /P 3 0 R /F 4 "
        b"/Rect [50 692 150 742] "
        b"/A << /S /URI /URI "
        + pdf_str("https://example.test/follow-link-uri")
        + b" >> >>"
    )
    objs[5] = (
        b"<< /Type /Annot /Subtype /Link /P 3 0 R /F 4 "
        b"/Rect [200 692 300 742] "
        b"/A << /S /GoTo /D [11 0 R /Fit] >> >>"
    )
    objs[6] = (
        b"<< /Type /Annot /Subtype /Link /P 3 0 R /F 4 "
        b"/Rect [50 592 150 642] "
        b"/A << /S /Named /N /NextPage >> >>"
    )
    objs[7] = (
        b"<< /Type /Annot /Subtype /Link /P 3 0 R /F 4 "
        b"/Rect [200 592 300 642] "
        b"/A << /S /Launch /F << /Type /Filespec "
        b"/F " + pdf_str("notes.txt") + b" "
        b"/UF " + pdf_str("notes.txt") + b" >> >> >>"
    )
    objs[8] = b"null"
    objs[9] = b"null"
    objs[10] = (
        b"<< /Type /Page /Parent 2 0 R "
        b"/MediaBox [0 0 612 792] /Resources << >> >>"
    )
    objs[11] = (
        b"<< /Type /Page /Parent 2 0 R "
        b"/MediaBox [0 0 612 792] /Resources << >> >>"
    )

    buf = bytearray(b"%PDF-1.5\n%\xff\xff\xff\xff\n")
    offsets = [0]
    for n in sorted(objs):
        offsets.append(len(buf))
        buf += f"{n} 0 obj\n".encode() + objs[n] + b"\nendobj\n"
    xref_off = len(buf)
    size = max(objs) + 1
    buf += f"xref\n0 {size}\n".encode()
    buf += b"0000000000 65535 f \n"
    for off in offsets[1:]:
        buf += f"{off:010d} 00000 n \n".encode()
    buf += (
        b"trailer\n<< /Size " + str(size).encode()
        + b" /Root 1 0 R >>\nstartxref\n"
        + str(xref_off).encode() + b"\n%%EOF\n"
    )
    return bytes(buf)


def main() -> None:
    out = Path(__file__).parent / "test_link_types.pdf"
    data = make_pdf()
    out.write_bytes(data)
    print(f"wrote {out} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
