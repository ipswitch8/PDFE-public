# -*- coding: utf-8 -*-
"""
Generator for a ligature + hyphenation text-extraction fidelity fixture.

Output: text_ligature.pdf (1 page). Draws the words "office" and "flourish"
with a standard base-14 font (Helvetica) so that MuPDF stext extraction yields
plain ASCII: the "fi"/"fl" sequences come out as ASCII letters, NOT as the
precomposed Unicode ligature codepoints U+FB01 (fi) / U+FB03 (ffi) etc.

This pins the expected-extraction contract for a fidelity oracle:
  * ASCII "fi" and "fl" MUST be present in the extracted text.
  * The precomposed ligature codepoints U+FB01 / U+FB02 / U+FB03 MUST be absent.

A soft-hyphenated line break scenario is also included: the word "cooperation"
is split across two lines as "cooper-" / "ation" using a visible hyphen, so a
de-hyphenation oracle can observe the wrapped form and assert the intended
behaviour (either the raw "cooper-\nation" layout or a de-hyphenated
"cooperation", depending on the extractor policy under test).

Drawn strings (exact, top to bottom):
  line 1: "office"
  line 2: "flourish"
  line 3: "cooper-"
  line 4: "ation"

Uses reportlab (already used by neighbouring generators) for deterministic,
standard-font text. reportlab embeds no ligature glyphs for these ASCII strings,
so extraction is plain ASCII by construction.

Regenerate with:  python tests/fixtures/gen_ligature_fixture.py
"""
import os

from reportlab.pdfgen import canvas
from reportlab.lib.pagesizes import letter

# Exact strings drawn, in order. Kept as module constants so a test can import
# or mirror them for expected-value assertions.
WORD_OFFICE = "office"
WORD_FLOURISH = "flourish"
HYPHEN_LINE_1 = "cooper-"
HYPHEN_LINE_2 = "ation"

# Codepoints the extractor MUST NOT emit for the above ASCII strings.
FORBIDDEN_LIGATURES = ["ﬁ", "ﬂ", "ﬃ", "ﬄ", "ﬀ"]  # fi fl ffi ffl ff


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out_path = os.path.join(here, "text_ligature.pdf")

    c = canvas.Canvas(out_path, pagesize=letter)
    width, height = letter

    c.setFont("Helvetica", 24)
    # Deterministic absolute positions (PDF user space, origin bottom-left).
    c.drawString(72, height - 100, WORD_OFFICE)
    c.drawString(72, height - 140, WORD_FLOURISH)
    # Soft-hyphenated line break rendered as an explicit two-line split.
    c.drawString(72, height - 200, HYPHEN_LINE_1)
    c.drawString(72, height - 224, HYPHEN_LINE_2)

    c.showPage()
    c.save()
    print("wrote", out_path)


if __name__ == "__main__":
    main()
