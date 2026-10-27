# -*- coding: utf-8 -*-
"""
Generator for the C-03 i18n RENDER corpus (coverage-expansion pipeline phase-2).

WHY THIS CORPUS EXISTS
----------------------
The harness-adversarial audit (docs/coverage-expansion/register.json, C-03) found
that RTL had an asserting consumer at the SHAPING layer (tests/test_textshaper_i18n
.cpp exercises TextShaper/BitmapCompositor directly) but CJK + combining-cluster
are ZERO-coverage at the RENDER level — i.e. a real on-disk PDF page carrying
i18n text, opened + rendered through PDFE's Document/Page path, with glyph-quad /
pixel oracles. Text-extraction round-trips are blind to shaping / direction /
mark-attachment, so "Arabic rendered unshaped or LTR", "CJK rendered tofu /
mis-positioned", and "combining marks detached" all slip through.

This produces three deterministic fixtures whose glyph geometry is baked into the
content stream by a REAL shaper (PyMuPDF `insert_htmlbox` -> HarfBuzz: bidi
reordering + Arabic GSUB joining + GPOS mark attachment) at generation time. Once
baked, the per-glyph positions are fixed in the PDF, so PDFE's own MuPDF 1.26.10
reader reproduces them regardless of reader version — the oracle reads the baked
quads via fz_stext and the rendered pixels via Page::render.

FIXTURES
--------
  i18n_rtl.pdf        Hebrew "shalom" + Arabic "marhaba" (each direction:rtl) and
                      a Latin "LTR" control, each on its own y-band.
                      Oracle (test_i18n_render.cpp):
                        * Hebrew: match each expected codepoint to its stext quad,
                          read x-centres in LOGICAL order -> STRICTLY DECREASING
                          (first logical char is rightmost = laid out RTL). The
                          Latin control's logical x-centres STRICTLY INCREASE
                          (same function, opposite result -> the oracle
                          discriminates direction; non-vacuous).
                        * Arabic: stext carries Arabic Presentation-Form-B
                          codepoints (U+FB50..U+FEFF) -> contextual JOINING was
                          applied (isolated U+06xx forms would mean NO shaping).
  i18n_cjk.pdf        Chinese "中文测试" (SimSun) on a known y-band + a deliberately
                      blank control band. Oracle: exact CJK codepoints preserved,
                      each glyph quad ~full-width square, real interior ink per
                      glyph (a dropped/tofu glyph collapses to ~0 ink), x ascending
                      (horizontal LTR); the blank control band reads ~0 ink so the
                      ink meter is proven non-vacuous.
  i18n_combining.pdf  "x" + U+0301 COMBINING ACUTE (attaches, zero-advance) vs
                      "x" + U+00B4 SPACING ACUTE (advances right, no overlap), each
                      followed by an "N" reference glyph. Oracle: the combining
                      mark's x-centre falls WITHIN the base glyph's x-span and the
                      following "N" is NOT pushed right by a mark advance; the
                      spacing accent sits to the RIGHT of the base (no overlap) and
                      DOES push its "N" further right. Differential -> non-vacuous.

FONTS (Windows dev + CI target)
  arial.ttf  — covers Latin, Hebrew, Arabic (with joining) and combining marks.
  simsun.ttc — covers CJK (SimSun). If a target lacks a CJK font the CJK fixture
               cannot be built; the generator FAILS LOUDLY (SKIP=FAIL) rather than
               emitting a Latin-fallback fixture that would silently pass.

Regenerate with:  python tests/fixtures/gen_i18n_render_corpus.py
"""
import os
import sys

import fitz  # PyMuPDF

HERE = os.path.dirname(os.path.abspath(__file__))

ARIAL = r"C:\Windows\Fonts\arial.ttf"
# CJK: SimSun (simplified Chinese). Fall back through common Windows CJK faces.
CJK_CANDIDATES = [
    r"C:\Windows\Fonts\simsun.ttc",
    r"C:\Windows\Fonts\msgothic.ttc",
    r"C:\Windows\Fonts\msyh.ttc",
    r"C:\Windows\Fonts\YuGothM.ttc",
]

W, H = 400, 300


def _first_existing(paths):
    for p in paths:
        if os.path.exists(p):
            return p
    return None


def _fixed_metadata(doc):
    # Deterministic, timestamp-free metadata so regeneration is reproducible.
    doc.set_metadata({
        "title": "PDFE i18n render fixture",
        "author": "PDFE test corpus",
        "producer": "gen_i18n_render_corpus.py",
        "creator": "gen_i18n_render_corpus.py",
        "creationDate": "",
        "modDate": "",
    })


def _save(doc, name):
    out = os.path.join(HERE, name)
    _fixed_metadata(doc)
    # no_new_id keeps the /ID stable; garbage+deflate compact the output.
    doc.save(out, garbage=4, deflate=True, no_new_id=True)
    doc.close()
    print("wrote", name)


def gen_rtl():
    if not os.path.exists(ARIAL):
        sys.exit("FATAL: arial.ttf not found (%s); required for the RTL fixture" % ARIAL)
    arch = fitz.Archive()
    arch.add(ARIAL, "arial.ttf")
    css = "@font-face{font-family:f;src:url(arial.ttf);}"

    doc = fitz.open()
    page = doc.new_page(width=W, height=H)

    # Hebrew "shalom": shin(05E9) lamed(05DC) vav(05D5) final-mem(05DD).
    hebrew = "שלום"
    page.insert_htmlbox(
        fitz.Rect(40, 30, 360, 80),
        '<div style="font-family:f;font-size:28px;direction:rtl">%s</div>' % hebrew,
        css=css, archive=arch)

    # Arabic "marhaba": meem(0645) reh(0631) hah(062D) beh(0628) alef(0627).
    arabic = "مرحبا"
    page.insert_htmlbox(
        fitz.Rect(40, 110, 360, 160),
        '<div style="font-family:f;font-size:28px;direction:rtl">%s</div>' % arabic,
        css=css, archive=arch)

    # Latin LTR control on its own band.
    page.insert_htmlbox(
        fitz.Rect(40, 190, 360, 240),
        '<div style="font-family:f;font-size:28px">LTR</div>',
        css=css, archive=arch)

    _save(doc, "i18n_rtl.pdf")


def gen_cjk():
    cjk_font = _first_existing(CJK_CANDIDATES)
    if not cjk_font:
        sys.exit("FATAL: no CJK font found among %s; cannot build i18n_cjk.pdf "
                 "(SKIP=FAIL)" % CJK_CANDIDATES)
    arch = fitz.Archive()
    arch.add(cjk_font, "cjk.ttc")
    css = "@font-face{font-family:c;src:url(cjk.ttc);}"

    doc = fitz.open()
    page = doc.new_page(width=W, height=H)

    # "zhong wen ce shi" (Chinese text test): 4E2D 6587 6D4B 8BD5.
    cjk = "中文测试"
    page.insert_htmlbox(
        fitz.Rect(40, 30, 360, 90),
        '<div style="font-family:c;font-size:28px">%s</div>' % cjk,
        css=css, archive=arch)
    # Lower band deliberately left BLANK — the ink-meter non-vacuity control.
    _save(doc, "i18n_cjk.pdf")


def gen_combining():
    if not os.path.exists(ARIAL):
        sys.exit("FATAL: arial.ttf not found; required for the combining fixture")
    arch = fitz.Archive()
    arch.add(ARIAL, "arial.ttf")
    css = "@font-face{font-family:f;src:url(arial.ttf);}"

    doc = fitz.open()
    page = doc.new_page(width=W, height=H)

    # Combining: base 'x' + U+0301 COMBINING ACUTE (x-acute has NO precomposed
    # codepoint, so HarfBuzz keeps two glyphs and attaches the mark) + 'N' ref.
    page.insert_htmlbox(
        fitz.Rect(40, 30, 360, 90),
        '<div style="font-family:f;font-size:40px">x́N</div>',
        css=css, archive=arch)

    # Spacing control: base 'x' + U+00B4 ACUTE ACCENT (a spacing glyph with its
    # own advance) + 'N' ref. This must NOT overlap the base.
    page.insert_htmlbox(
        fitz.Rect(40, 130, 360, 190),
        '<div style="font-family:f;font-size:40px">x´N</div>',
        css=css, archive=arch)

    _save(doc, "i18n_combining.pdf")


def main():
    gen_rtl()
    gen_cjk()
    gen_combining()


if __name__ == "__main__":
    main()
