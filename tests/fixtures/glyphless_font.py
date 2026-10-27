#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""A generated glyphless TrueType font, shared by the OCR-layer fixtures.

GitHub #250/#218. PDFE decides whether a text span belongs to an OCR
invisible-text layer with ``TextSpan::isOcrInvisibleLayer()``, which is
``isGlyphlessOcrFontName(fontName)`` in ``src/textedit/TextEditModel.h``: strip a
canonical ``ABCDEF+`` subset prefix, lowercase, then test for the substring
``glyphless``. A layer drawn in Tr=3 with an ordinary font (``/Helvetica``, as
the committed fixtures had) is NOT recognised, so the recomposite path those
fixtures exist to exercise answered ``RECOMPOSITE_SKIPPED|span-not-ocr-invisible``
and never ran.

Tesseract's own PDF writer emits ``/GlyphLessFont``, but it needs ``pdf.ttf``
from tessdata, which vcpkg does not install -- on this tree it exists only
inside a buildtrees scratch directory that no clean checkout or CI runner has.
Generating the font here keeps fixture regeneration reproducible anywhere
fontTools is available, and adds no vendored binary to the repository.

The glyphs are genuinely empty -- this font draws nothing, which is what
"glyphless" means -- but they carry ordinary advance widths so that span
bounding boxes stay non-degenerate and hit-testing still works. A zero-advance
font would collapse every span to zero width and silently break the very
hit-testing these fixtures support.

ONE definition, imported by both generators. Implementing this twice invites
DCA 0052: a rule implemented in two places, one of which gets fixed.
"""
from __future__ import annotations

import io

GLYPHLESS_FONT_NAME = "GlyphLessFont"
FIRST_CHAR, LAST_CHAR = 32, 126
ADVANCE = 500  # units per em = 1000
ASCENT, DESCENT = 800, -200


def build_glyphless_ttf() -> bytes:
    """Return TrueType bytes for a font whose glyphs draw nothing."""
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.ttGlyphPen import TTGlyphPen

    codes = list(range(FIRST_CHAR, LAST_CHAR + 1))
    names = [".notdef"] + ["g%02X" % c for c in codes]
    cmap = {c: "g%02X" % c for c in codes}

    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap(cmap)
    empty = TTGlyphPen(None).glyph()  # never asked to draw
    fb.setupGlyf({n: empty for n in names})
    fb.setupHorizontalMetrics({n: (ADVANCE, 0) for n in names})
    fb.setupHorizontalHeader(ascent=ASCENT, descent=DESCENT)
    fb.setupNameTable({
        "familyName": GLYPHLESS_FONT_NAME,
        "styleName": "Regular",
        "fullName": GLYPHLESS_FONT_NAME,
        "psName": GLYPHLESS_FONT_NAME,
        "version": "1.0",
    })
    fb.setupOS2(sTypoAscender=ASCENT, sTypoDescender=DESCENT,
                usWinAscent=ASCENT, usWinDescent=-DESCENT)
    fb.setupPost()
    buf = io.BytesIO()
    fb.save(buf)
    return buf.getvalue()


def write_glyphless_ttf(path) -> str:
    """Write the font to ``path`` (for APIs that need a file) and return it."""
    import pathlib
    p = pathlib.Path(path)
    p.write_bytes(build_glyphless_ttf())
    return str(p)
