# -*- coding: utf-8 -*-
"""
Generator for the TJ numeric-offset word-boundary fidelity fixture.

Output: text_word_boundary_tj.pdf (1 page, uncompressed content stream).

WHY A HAND-WRITTEN PDF AND NOT reportlab
----------------------------------------
The property under test is that a word boundary can be pure GEOMETRY: in a real
PDF a justified or kerned line is very often emitted as a single ``TJ`` array
whose word gaps are NEGATIVE NUMERIC OFFSETS (thousandths of a text-space unit,
negative == advance further right) and which contains NO 0x20 space byte at all.
No high-level PDF writer will emit that shape on demand -- reportlab writes real
space characters -- so the content stream is authored byte-for-byte here.

The stream is deliberately written with NO whitespace inside the TJ arrays, e.g.

    [(Hello)-400(World)]TJ

so a consumer test can assert on the exact byte sequence and thereby PROVE, with
no PDF parsing at all, that the authored bytes carry no space between the two
words. The stream is left uncompressed (no /Filter) for the same reason.

WHAT IS DRAWN (Helvetica 24pt, one line each, top to bottom)
------------------------------------------------------------
1. GAP CASE -- ``[(Hello)-400(World)]TJ``
   -400/1000 * 24pt = 9.6pt of extra advance. Helvetica's space glyph is
   0.278em = 6.67pt at this size, so the gap is wider than a real space and a
   correct extractor must SYNTHESIZE a word boundary: "Hello World", never
   "HelloWorld".

2. TIGHT-KERNING CONTROL -- ``[(W)-20(a)-12(v)-18(e)-10(f)-14(r)-12(o)-16(n)-14(t)]TJ``
   The SAME MECHANISM (numeric offsets between TJ elements) at a much smaller
   magnitude: the largest is -20/1000 * 24pt = 0.48pt of inter-letter kerning.
   These MUST NOT produce word boundaries -- the word must extract as the single
   contiguous token "Wavefront".

   This control is the whole point of the fixture. Without it, an implementation
   that inserted a break at EVERY numeric offset would pass case 1 while
   shredding every kerned word in every real document.

3. WIDE GAP CASE -- ``[(Left)-700(Right)]TJ``
   -700/1000 * 24pt = 16.8pt. A second, independent instance of property 1 at a
   different magnitude, so the oracle does not rest on one threshold-adjacent
   measurement.

4. SECOND CONTROL -- ``[(Over)+15(lapping)]TJ``
   A POSITIVE offset pulls the second element slightly LEFT (tighter than the
   natural advance). A positive offset can never be a word gap, so this must
   also extract contiguously as "Overlapping".

MEASURED THRESHOLD (MuPDF 1.26.10, Helvetica 24pt, this fixture's geometry)
---------------------------------------------------------------------------
Sweeping a single offset between the two halves of one word, the synthesized
space appears between -100 (2.4pt, still contiguous) and -150 (3.6pt, split) --
i.e. the boundary sits at ~0.15 * font size, MuPDF's SPACE_DIST. The fixture's
values are therefore not threshold-adjacent in either direction:

  * gap case      -400  ->  9.6pt  == 2.7x ABOVE the boundary
  * wide gap case -700  -> 16.8pt  == 4.7x ABOVE the boundary
    (kept below the ~-900 point at which MuPDF starts a NEW LINE instead of
    inserting a space, so the oracle can assert on a contiguous "Left Right")
  * kerning control, largest offset -20 -> 0.48pt == 7.5x BELOW the boundary

Anchor tokens ("Hello", "World", "Wavefront", "Left", "Right", "Overlapping")
are each drawn exactly once so a consumer can assert on unique substrings.

Regenerate with:  python tests/fixtures/gen_word_boundary_corpus.py
"""
from __future__ import annotations

import os

FIXTURE_NAME = "text_word_boundary_tj.pdf"

# The exact TJ operator bytes the fixture is authored with. A consumer test
# asserts on these verbatim, which is what proves "no literal space bytes".
TJ_GAP = b"[(Hello)-400(World)]TJ"
TJ_KERN = b"[(W)-20(a)-12(v)-18(e)-10(f)-14(r)-12(o)-16(n)-14(t)]TJ"
TJ_WIDE_GAP = b"[(Left)-700(Right)]TJ"
TJ_POSITIVE = b"[(Over)+15(lapping)]TJ"

FONT_SIZE = 24


def build_content_stream() -> bytes:
    lines = [
        b"BT",
        b"/F1 %d Tf" % FONT_SIZE,
        b"72 700 Td",
        TJ_GAP,
        b"ET",
        b"BT",
        b"/F1 %d Tf" % FONT_SIZE,
        b"72 650 Td",
        TJ_KERN,
        b"ET",
        b"BT",
        b"/F1 %d Tf" % FONT_SIZE,
        b"72 600 Td",
        TJ_WIDE_GAP,
        b"ET",
        b"BT",
        b"/F1 %d Tf" % FONT_SIZE,
        b"72 550 Td",
        TJ_POSITIVE,
        b"ET",
    ]
    return b"\n".join(lines) + b"\n"


def build_pdf() -> bytes:
    content = build_content_stream()

    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
        b"<< /Length %d >>\nstream\n" % len(content) + content + b"endstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
        b"/Encoding /WinAnsiEncoding >>",
    ]

    out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for i, body in enumerate(objects, start=1):
        offsets.append(len(out))
        out += b"%d 0 obj\n" % i + body + b"\nendobj\n"

    xref_pos = len(out)
    n = len(objects) + 1
    out += b"xref\n0 %d\n" % n
    out += b"0000000000 65535 f \n"
    for off in offsets[1:]:
        out += b"%010d 00000 n \n" % off
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (
        n,
        xref_pos,
    )
    return bytes(out)


def main() -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    out_path = os.path.join(here, FIXTURE_NAME)
    data = build_pdf()

    # Self-check: the authored bytes must carry no space inside any drawn
    # literal. If this ever trips, the fixture would silently stop testing the
    # property it exists for.
    for tj in (TJ_GAP, TJ_KERN, TJ_WIDE_GAP, TJ_POSITIVE):
        assert tj in data, "authored TJ operator missing from output: %r" % tj
        assert b" " not in tj, "TJ operator contains a space byte: %r" % tj
    assert b"Hello World" not in data
    assert b"HelloWorld" not in data

    with open(out_path, "wb") as f:
        f.write(data)
    print("wrote", out_path, len(data), "bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
