# -*- coding: utf-8 -*-
"""Generate i18n_vertical_cjk.pdf — a REAL vertical-writing (tategaki) CJK page.

WHY THIS FIXTURE EXISTS  (gap PDF-71 / GitHub #22)
--------------------------------------------------
Vertical writing was only ever tested through a SYNTHETIC in-memory
``core::TextLine`` with ``writingMode`` assigned by the test itself
(``tests/test_writing_mode_autoplumb.cpp``). That test cannot fail if MuPDF
stops reporting ``fz_stext_line::wmode`` altogether, because no PDF is involved:
the whole extraction edge — content stream -> ``pdf_load_font`` wmode ->
``fz_stext_line::wmode`` -> ``core::TextLine::writingMode`` — is unexercised.

This fixture closes that hole with an on-disk document whose vertical run is
produced by the PDF machinery itself, not by hand-placing each glyph: ONE
``Tj`` with three CIDs, and the composite font's ``/Encoding /Identity-V``
plus ``/DW2`` are what stack the glyphs downward. If the vertical path breaks,
the glyphs do not move — so the geometry oracle is a real oracle.

THE DIFFERENTIAL (what makes the oracle non-vacuous)
----------------------------------------------------
The page carries TWO runs of the SAME three characters, drawn with TWO Type0
fonts that share ONE descendant CIDFont, ONE /FontDescriptor and ONE embedded
font program. The only difference between the two font dictionaries is:

    /FV  ->  /Encoding /Identity-V     (vertical, WMode 1)
    /FH  ->  /Encoding /Identity-H     (horizontal, WMode 0)

So any difference the oracle measures between the two runs is attributable to
the writing mode and to nothing else. A "vertical" assertion that would also
pass on the horizontal run is caught, because the horizontal run is asserted to
give the OPPOSITE answer with the same code.

THE FONT IS FULLY SYNTHETIC
---------------------------
Built here with fontTools' FontBuilder rather than subsetted from a system CJK
face, so the committed fixture carries no third-party font bytes and the
generator does not depend on a particular Windows font being installed. Three
glyphs are mapped, chosen because their meanings label their own shapes:

    U+4E0A 上 "above"  -> a filled bar across the TOP of the em box
    U+4E2D 中 "middle" -> a filled bar across the MIDDLE of the em box
    U+4E0B 下 "below"  -> a filled bar across the BOTTOM of the em box

Each is full-width (1000/1000 em) so the vertical advance is a clean square.

AUTHORED GROUND TRUTH (mirrored verbatim in the C++ oracle)
-----------------------------------------------------------
  page 0, MediaBox [0 0 400 400]
  vertical run   : /FV 32 Tf, Tm (200, 340), CIDs <0001 0002 0003>
                   -> 上 then 中 then 下, stacking DOWNWARD from y=340
  horizontal run : /FH 32 Tf, Tm ( 60,  60), CIDs <0001 0002 0003>
                   -> 上 then 中 then 下, marching RIGHTWARD from x=60
  Both runs extract the codepoints U+4E0A, U+4E2D, U+4E0B in that logical order.

Regenerate with:  python tests/fixtures/gen_vertical_cjk.py
"""
from __future__ import annotations

import io
import os

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "i18n_vertical_cjk.pdf")

UPEM = 1000
ASCENT = 880
DESCENT = -120

# (codepoint, glyph name, bar y-range in font units) — the bar's position in the
# em box is the glyph's identity, so a mis-mapped CID is visible in the render.
GLYPHS = [
    (0x4E0A, "uni4E0A", (700, 860)),   # 上 "above"  — bar at the top
    (0x4E2D, "uni4E2D", (400, 560)),   # 中 "middle" — bar in the middle
    (0x4E0B, "uni4E0B", (60, 220)),    # 下 "below"  — bar at the bottom
]


# ---------------------------------------------------------------------------
# Minimal raw-PDF writer (byte offsets exact, output deterministic).
# ---------------------------------------------------------------------------
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


def build_font() -> bytes:
    """A 4-glyph TrueType font (.notdef + the three bars). GIDs 1,2,3 are the
    three mapped characters in GLYPHS order, which is what the PDF's CIDs mean
    under /CIDToGIDMap /Identity."""
    order = [".notdef"] + [name for _, name, _ in GLYPHS]

    def bar(y0: int, y1: int):
        pen = TTGlyphPen(None)
        pen.moveTo((80, y0))
        pen.lineTo((920, y0))
        pen.lineTo((920, y1))
        pen.lineTo((80, y1))
        pen.closePath()
        return pen.glyph()

    def blank():
        return TTGlyphPen(None).glyph()

    fb = FontBuilder(UPEM, isTTF=True)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap({cp: name for cp, name, _ in GLYPHS})
    glyphs = {".notdef": blank()}
    for _, name, (y0, y1) in GLYPHS:
        glyphs[name] = bar(y0, y1)
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics({name: (UPEM, 80 if name != ".notdef" else 0)
                               for name in order})
    fb.setupHorizontalHeader(ascent=ASCENT, descent=DESCENT)
    fb.setupNameTable({
        "familyName": "PDFE Tategaki Test",
        "styleName": "Regular",
        "psName": "PDFETategakiTest-Regular",
        "fullName": "PDFE Tategaki Test Regular",
        "version": "1.000",
        "uniqueFontIdentifier": "PDFE;Tategaki;1.000",
    })
    fb.setupOS2(sTypoAscender=ASCENT, sTypoDescender=DESCENT,
                usWinAscent=ASCENT, usWinDescent=-DESCENT,
                achVendID="PDFE")
    fb.setupPost(isFixedPitch=1)
    out = io.BytesIO()
    fb.save(out)
    return out.getvalue()


def build_pdf(fontdata: bytes) -> bytes:
    pdf = RawPdf()
    cids = b"".join(b"%04X" % (i + 1) for i in range(len(GLYPHS)))

    pdf.add(1, b"<< /Type /Catalog /Pages 2 0 R >>")
    pdf.add(2, b"<< /Type /Pages /Count 1 /Kids [3 0 R] >>")
    pdf.add(3, b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 400]"
               b" /Resources << /Font << /FV 5 0 R /FH 10 0 R >> >>"
               b" /Contents 4 0 R >>")
    content = (b"BT\n/FV 32 Tf\n1 0 0 1 200 340 Tm\n<" + cids + b"> Tj\nET\n"
               b"BT\n/FH 32 Tf\n1 0 0 1 60 60 Tm\n<" + cids + b"> Tj\nET\n")
    pdf.add_stream(4, b"<< >>", content)

    # The ONLY difference between objects 5 and 10 is /Encoding.
    pdf.add(5, b"<< /Type /Font /Subtype /Type0 /BaseFont /PDFETategakiTest-Regular"
               b" /Encoding /Identity-V /DescendantFonts [6 0 R] /ToUnicode 9 0 R >>")
    pdf.add(10, b"<< /Type /Font /Subtype /Type0 /BaseFont /PDFETategakiTest-Regular"
                b" /Encoding /Identity-H /DescendantFonts [6 0 R] /ToUnicode 9 0 R >>")

    # /DW2 [880 -1000]: vertical origin 880 above the baseline, advance -1000
    # (one full em DOWN per glyph) — this, not the content stream, is what makes
    # the vertical run stack.
    pdf.add(6, b"<< /Type /Font /Subtype /CIDFontType2"
               b" /BaseFont /PDFETategakiTest-Regular"
               b" /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity)"
               b" /Supplement 0 >>"
               b" /FontDescriptor 7 0 R /DW 1000 /DW2 [880 -1000]"
               b" /W [1 [1000 1000 1000]] /CIDToGIDMap /Identity >>")
    pdf.add(7, b"<< /Type /FontDescriptor /FontName /PDFETategakiTest-Regular"
               b" /Flags 4 /FontBBox [0 %d 1000 %d] /ItalicAngle 0"
               b" /Ascent %d /Descent %d /CapHeight %d /StemV 80"
               b" /FontFile2 8 0 R >>" % (DESCENT, ASCENT, ASCENT, DESCENT, ASCENT))
    pdf.add_stream(8, b"<< /Length1 %d >>" % len(fontdata), fontdata)

    bfchar = b"".join(b"<%04X> <%04X>\n" % (i + 1, cp)
                      for i, (cp, _, _) in enumerate(GLYPHS))
    tounicode = (b"/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
                 b"/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS)"
                 b" /Supplement 0 >> def\n"
                 b"/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
                 b"1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n"
                 b"%d beginbfchar\n" % len(GLYPHS) + bfchar +
                 b"endbfchar\nendcmap\n"
                 b"CMapName currentdict /CMap defineresource pop\nend\nend\n")
    pdf.add_stream(9, b"<< >>", tounicode)

    return pdf.finish(root=1, size=11)


def main() -> None:
    data = build_pdf(build_font())
    with open(OUT, "wb") as fh:
        fh.write(data)
    print("wrote %s (%d bytes)" % (os.path.basename(OUT), len(data)))


if __name__ == "__main__":
    main()
