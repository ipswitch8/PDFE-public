#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate test_dest_integrity.pdf — a 5-page PDF carrying KNOWN
destinations for the R3-01 (destination integrity across structural
page edits) class oracle.

Each page draws a UNIQUE text marker ("PDFEPAGEMARK0" .. "PDFEPAGEMARK4")
so a test can resolve "which page a destination now points at" BY CONTENT
— an assertion that survives page renumbering, unlike a raw object number.

Destinations embedded:
  * /Outlines (bookmarks): three items whose /Dest points at
      page index 1 (title "BM_MARK1"),
      page index 2 (title "BM_MARK2"),
      page index 3 (title "BM_MARK3").
  * Link annotations on page 0: two /Link annots with
      /A /GoTo /D [<page-ref> /Fit] targeting
      page index 2 and page index 3.

After a page delete / reorder + full-rewrite save + reopen, every
surviving destination must still resolve to the SAME marker; a
destination whose target page was deleted must be gone/cleared (not
dangling to a wrong page).

Hand-rolled minimal PDF — no external PDF library dependency at
fixture-generation time (mirrors generate_link_types_pdf.py).
"""

from __future__ import annotations

from pathlib import Path

NUM_PAGES = 5


def pdf_str(s: str) -> bytes:
    return b"(" + s.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)").encode() + b")"


def make_pdf() -> bytes:
    # Fixed object-number layout.
    #  1 Catalog
    #  2 Pages
    #  3 Font (Helvetica)
    #  Page P_i  = 5 + 2*i    (5, 7, 9, 11, 13)
    #  Content C_i = 6 + 2*i   (6, 8, 10, 12, 14)
    # 15 Outlines
    # 16 Outline item -> page index 1  (title BM_MARK1)
    # 17 Outline item -> page index 2  (title BM_MARK2)
    # 18 Outline item -> page index 3  (title BM_MARK3)
    # 19 Link on page0 -> page index 2 (GoTo)
    # 20 Link on page0 -> page index 3 (GoTo)
    def page_obj(i: int) -> int:
        return 5 + 2 * i

    def content_obj(i: int) -> int:
        return 6 + 2 * i

    objs: dict[int, bytes] = {}

    objs[1] = b"<< /Type /Catalog /Pages 2 0 R /Outlines 15 0 R >>"

    kids = b" ".join(f"{page_obj(i)} 0 R".encode() for i in range(NUM_PAGES))
    objs[2] = b"<< /Type /Pages /Kids [" + kids + b"] /Count " + str(NUM_PAGES).encode() + b" >>"

    objs[3] = b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"

    for i in range(NUM_PAGES):
        annots = b""
        if i == 0:
            annots = b" /Annots [19 0 R 20 0 R]"
        objs[page_obj(i)] = (
            b"<< /Type /Page /Parent 2 0 R "
            b"/MediaBox [0 0 612 792] "
            b"/Resources << /Font << /F1 3 0 R >> >> "
            b"/Contents " + f"{content_obj(i)} 0 R".encode() + annots + b" >>"
        )
        stream = (
            b"BT /F1 24 Tf 72 700 Td " + pdf_str(f"PDFEPAGEMARK{i}") + b" Tj ET"
        )
        objs[content_obj(i)] = (
            b"<< /Length " + str(len(stream)).encode() + b" >>\nstream\n"
            + stream + b"\nendstream"
        )

    # Outlines dictionary + three items (linked list).
    objs[15] = b"<< /Type /Outlines /First 16 0 R /Last 18 0 R /Count 3 >>"
    # item -> page index 1 (page obj 7)
    objs[16] = (
        b"<< /Title " + pdf_str("BM_MARK1") + b" /Parent 15 0 R "
        b"/Next 17 0 R /Dest [7 0 R /Fit] >>"
    )
    # item -> page index 2 (page obj 9)
    objs[17] = (
        b"<< /Title " + pdf_str("BM_MARK2") + b" /Parent 15 0 R "
        b"/Prev 16 0 R /Next 18 0 R /Dest [9 0 R /Fit] >>"
    )
    # item -> page index 3 (page obj 11)
    objs[18] = (
        b"<< /Title " + pdf_str("BM_MARK3") + b" /Parent 15 0 R "
        b"/Prev 17 0 R /Dest [11 0 R /Fit] >>"
    )

    # Link annotations on page 0 -> pages 2 and 3.
    objs[19] = (
        b"<< /Type /Annot /Subtype /Link /P 5 0 R /F 4 "
        b"/Rect [50 692 150 742] "
        b"/A << /S /GoTo /D [9 0 R /Fit] >> >>"
    )
    objs[20] = (
        b"<< /Type /Annot /Subtype /Link /P 5 0 R /F 4 "
        b"/Rect [200 692 300 742] "
        b"/A << /S /GoTo /D [11 0 R /Fit] >> >>"
    )

    buf = bytearray(b"%PDF-1.5\n%\xff\xff\xff\xff\n")
    offsets: dict[int, int] = {}
    for n in sorted(objs):
        offsets[n] = len(buf)
        buf += f"{n} 0 obj\n".encode() + objs[n] + b"\nendobj\n"
    xref_off = len(buf)
    size = max(objs) + 1
    buf += f"xref\n0 {size}\n".encode()
    buf += b"0000000000 65535 f \n"
    for n in range(1, size):
        off = offsets.get(n, 0)
        if n in offsets:
            buf += f"{off:010d} 00000 n \n".encode()
        else:
            buf += b"0000000000 65535 f \n"
    buf += (
        b"trailer\n<< /Size " + str(size).encode()
        + b" /Root 1 0 R >>\nstartxref\n"
        + str(xref_off).encode() + b"\n%%EOF\n"
    )
    return bytes(buf)


def main() -> None:
    out = Path(__file__).parent / "test_dest_integrity.pdf"
    data = make_pdf()
    out.write_bytes(data)
    print(f"wrote {out} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
