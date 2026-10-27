# -*- coding: utf-8 -*-
"""Generate text_soft_hyphen.pdf — the U+00AD DISCRETIONARY HYPHEN corpus.

WHY THIS FIXTURE EXISTS  (gap PDF-73 / GitHub #24)
--------------------------------------------------
``HyphenationPreservesAuthoredHyphen`` pins the behaviour of an ordinary
HYPHEN-MINUS (U+002D) drawn at a line break. Nothing pinned the OTHER hyphen a
document can carry: U+00AD SOFT HYPHEN, the "discretionary" hyphen. It is a
different character with different semantics, it is what word processors emit
when they hyphenate, and a change of stext flags or of the encoding path could
silently start dropping it, folding it to U+002D, or emitting U+FFFD, with
nothing to notice.

AUTHORED, UNAMBIGUOUS ENCODING
------------------------------
The soft hyphen is not left to an encoding convention to imply. The font is
/Helvetica with an explicit /Encoding /Differences mapping code 0xAD to the
glyph /uni00AD, AND an explicit /ToUnicode CMap that states, for every code the
page draws, exactly which Unicode scalar it means — including <AD> -> <00AD>.
So "the file contains U+00AD" is a fact about the authored bytes, not an
inference about a reader.

(The explicit /ToUnicode also keeps this fixture out of the way of PDFE's
glyph-name recovery path, src/core/ToUnicodeRecovery.cpp, which only ever
touches a font that has NO /ToUnicode. Nothing this fixture measures can be
attributed to that code.)

AUTHORED GROUND TRUTH (mirrored verbatim in the C++ oracle)
-----------------------------------------------------------
  1 page, MediaBox [0 0 612 792], /Helvetica 14pt, uncompressed content.

  y=700  "hyphen" + U+00AD        <- a word broken at a DISCRETIONARY hyphen
  y=680  "ation"                  <- ...and its continuation on the next line
  y=640  "well-being"             <- HARD hyphen (U+002D) CONTROL
  y=600  "un" + U+00AD + "breakable"  <- discretionary hyphen MID-LINE

  The two hyphen kinds are therefore both present on one page, in one font, at
  known positions — so an oracle that cannot tell them apart is caught by the
  control rather than passing on it.

Regenerate with:  python tests/fixtures/gen_soft_hyphen.py
"""
from __future__ import annotations

import io
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "text_soft_hyphen.pdf")

SHY = 0xAD  # the character CODE used for U+00AD on this page

# (y, list of byte codes) — authored literally, no string escaping surprises.
LINES = [
    (700, list(b"hyphen") + [SHY]),
    (680, list(b"ation")),
    (640, list(b"well-being")),
    (600, list(b"un") + [SHY] + list(b"breakable")),
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


def _hex_string(codes: list[int]) -> bytes:
    """Show text as a HEX string so byte 0xAD needs no escaping and the authored
    codes are literally readable in the committed file."""
    return b"<" + b"".join(b"%02X" % c for c in codes) + b">"


def build() -> bytes:
    pdf = RawPdf()
    body = b"BT\n/F1 14 Tf\n"
    for y, codes in LINES:
        body += b"1 0 0 1 72 %d Tm\n" % y + _hex_string(codes) + b" Tj\n"
    body += b"ET\n"

    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Count 1 /Kids [3 0 R] >>")
    pdf.add(3, b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792]"
               b" /Resources << /Font << /F1 5 0 R >> >>"
               b" /Contents 4 0 R >>")
    pdf.add_stream(4, b"<< >>", body)
    pdf.add(5, b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica"
               b" /Encoding 6 0 R /ToUnicode 7 0 R >>")
    pdf.add(6, b"<< /Type /Encoding /BaseEncoding /WinAnsiEncoding"
               b" /Differences [%d /uni00AD] >>" % SHY)

    # Every code the page draws, stated explicitly.
    used = sorted({c for _, codes in LINES for c in codes})
    bfchar = b"".join(b"<%02X> <%04X>\n" % (c, c) for c in used)
    cmap = (b"/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
            b"/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS)"
            b" /Supplement 0 >> def\n"
            b"/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
            b"1 begincodespacerange\n<00> <FF>\nendcodespacerange\n"
            b"%d beginbfchar\n" % len(used) + bfchar +
            b"endbfchar\nendcmap\n"
            b"CMapName currentdict /CMap defineresource pop\nend\nend\n")
    pdf.add_stream(7, b"<< >>", cmap)
    return pdf.finish(root=1, size=8)


def main() -> None:
    data = build()
    with open(OUT, "wb") as fh:
        fh.write(data)
    print("wrote %s (%d bytes)" % (os.path.basename(OUT), len(data)))


if __name__ == "__main__":
    main()
