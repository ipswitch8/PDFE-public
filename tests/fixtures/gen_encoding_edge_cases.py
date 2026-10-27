# -*- coding: utf-8 -*-
"""Generate encoding_edge_cases.pdf — the READ-SIDE simple-font encoding oracle.

WHY THIS FIXTURE EXISTS  (gap PDF-75 / GitHub #25)
--------------------------------------------------
``/Differences`` and WinAnsi appeared in the suite only on the WRITE side —
``test_type3_font_builder.cpp`` asserts that PDFE *emits* a /Differences array.
A round-trip through PDFE's own writer cannot catch a wrong mapping, because
both ends would share it: writer and reader would agree, and the test would be
green while every legacy document extracted as mojibake.

So this fixture is authored as RAW PDF BYTES. Nothing in PDFE produced it, and
the expected codepoints below come from PDF 32000-1 Annex D (WinAnsiEncoding)
and the Adobe Glyph List — not from observing what PDFE happens to do.

WHY 0x80..0x9F SPECIFICALLY
---------------------------
That band is exactly where WinAnsiEncoding diverges from Latin-1. In Latin-1
(and in a naive "the byte IS the codepoint" reader) 0x80..0x9F are C1 CONTROL
characters; in WinAnsi they are printable punctuation — the euro sign, the
curly quotes, the en/em dashes, the ellipsis. A reader that passes the raw byte
through produces invisible control characters that look like successful
extraction and match nothing. The whole point of the assertion is that band.

AUTHORED GROUND TRUTH (mirrored verbatim in the C++ oracle)
-----------------------------------------------------------
  1 page, MediaBox [0 0 612 792], uncompressed content, three fonts.

  /FW  /Helvetica, /Encoding /WinAnsiEncoding, no /ToUnicode
       y=700, codes 80 85 91 92 93 94 96 97 99
       -> U+20AC EURO, U+2026 ELLIPSIS, U+2018 / U+2019 single curly quotes,
          U+201C / U+201D double curly quotes, U+2013 EN DASH,
          U+2014 EM DASH, U+2122 TRADE MARK

  /FD  /Helvetica, /Encoding << /BaseEncoding /WinAnsiEncoding
                               /Differences [65 /bullet /Euro /trademark] >>
       y=660, codes 41 42 43  (i.e. the ASCII bytes for "ABC")
       -> U+2022 BULLET, U+20AC EURO, U+2122 TRADE MARK
       A reader that ignores /Differences extracts the literal "ABC" instead.

  /FC  /Helvetica, /Encoding /WinAnsiEncoding  (the CONTROL)
       y=620, codes 41 42 43
       -> "ABC"
       Same three bytes, same base encoding, no /Differences: proves the /FD
       result came from the override and not from the reader always saying
       bullet/euro/trademark.

  None of the three fonts carries a /ToUnicode, which is the realistic legacy
  shape and is also what forces the reader to go through the ENCODING rather
  than through a producer-supplied table.

Regenerate with:  python tests/fixtures/gen_encoding_edge_cases.py
"""
from __future__ import annotations

import io
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "encoding_edge_cases.pdf")

# WinAnsi codes in the 0x80..0x9F divergence band, with their Annex D meanings.
WINANSI_BAND = [
    (0x80, 0x20AC, "Euro"),
    (0x85, 0x2026, "ellipsis"),
    (0x91, 0x2018, "quoteleft"),
    (0x92, 0x2019, "quoteright"),
    (0x93, 0x201C, "quotedblleft"),
    (0x94, 0x201D, "quotedblright"),
    (0x96, 0x2013, "endash"),
    (0x97, 0x2014, "emdash"),
    (0x99, 0x2122, "trademark"),
]

# /Differences override on the ASCII codes for "ABC".
DIFFERENCES = [
    (0x41, 0x2022, "bullet"),
    (0x42, 0x20AC, "Euro"),
    (0x43, 0x2122, "trademark"),
]


class RawPdf:
    def __init__(self, header: bytes = b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n"):
        self.buf = io.BytesIO()
        self.buf.write(header)
        self.offsets: dict[int, int] = {}

    def add(self, num: int, body: bytes) -> None:
        self.offsets[num] = self.buf.tell()
        self.buf.write(b"%d 0 obj\n" % num + body + b"\nendobj\n")

    def add_stream(self, num: int, dict_body: bytes, data: bytes) -> None:
        d = dict_body.rstrip()
        assert d.endswith(b">>")
        d = d[:-2] + b" /Length %d >>" % len(data)
        self.offsets[num] = self.buf.tell()
        self.buf.write(b"%d 0 obj\n" % num + d + b"\nstream\n" + data
                       + b"\nendstream\nendobj\n")

    def finish(self, root: int, size: int) -> bytes:
        xref_at = self.buf.tell()
        lines = [b"xref\n", b"0 %d\n" % size, b"0000000000 65535 f \n"]
        for n in range(1, size):
            off = self.offsets.get(n)
            lines.append(b"%010d 00000 n \n" % off if off is not None
                         else b"0000000000 65535 f \n")
        self.buf.write(b"".join(lines))
        self.buf.write(b"trailer\n<< /Size %d /Root %d 0 R >>\nstartxref\n%d\n%%%%EOF\n"
                       % (size, root, xref_at))
        return self.buf.getvalue()


def _hex(codes) -> bytes:
    return b"<" + b"".join(b"%02X" % c for c in codes) + b">"


def build() -> bytes:
    pdf = RawPdf()
    band = [c for c, _, _ in WINANSI_BAND]
    abc = [c for c, _, _ in DIFFERENCES]

    body = (b"BT\n/FW 18 Tf\n1 0 0 1 72 700 Tm\n" + _hex(band) + b" Tj\nET\n"
            b"BT\n/FD 18 Tf\n1 0 0 1 72 660 Tm\n" + _hex(abc) + b" Tj\nET\n"
            b"BT\n/FC 18 Tf\n1 0 0 1 72 620 Tm\n" + _hex(abc) + b" Tj\nET\n")

    diff_names = b" ".join(b"/%s" % n.encode("ascii") for _, _, n in DIFFERENCES)

    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Count 1 /Kids [3 0 R] >>")
    pdf.add(3, b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792]"
               b" /Resources << /Font << /FW 5 0 R /FD 6 0 R /FC 8 0 R >> >>"
               b" /Contents 4 0 R >>")
    pdf.add_stream(4, b"<< >>", body)
    pdf.add(5, b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica"
               b" /Encoding /WinAnsiEncoding >>")
    pdf.add(6, b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica"
               b" /Encoding 7 0 R >>")
    pdf.add(7, b"<< /Type /Encoding /BaseEncoding /WinAnsiEncoding"
               b" /Differences [%d " % DIFFERENCES[0][0] + diff_names + b"] >>")
    pdf.add(8, b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica"
               b" /Encoding /WinAnsiEncoding >>")
    return pdf.finish(root=1, size=9)


def main() -> None:
    data = build()
    with open(OUT, "wb") as fh:
        fh.write(data)
    print("wrote %s (%d bytes)" % (os.path.basename(OUT), len(data)))


if __name__ == "__main__":
    main()
