# -*- coding: utf-8 -*-
"""Generate the three programmatic fixtures for the render-oracle pipeline
(e7d3c2a1) Phase 2.

Produces, next to this script under ``tests/fixtures/``:

* ``mixed_dpi_page.pdf``      — one page carrying a 300-DPI photo region and a
                                150-DPI thumbnail region. Exercises defect #2
                                (``Page::sourceImageDpi()`` returns the single
                                page-maximum DPI, so the 150-DPI region gets
                                upscaled / smeared on replacement).
* ``multilingual_latin_arabic.pdf`` — one page with a Latin text block and an
                                Arabic text block. Exercises defect #3
                                (doc-wide ``OcrConfig::language`` baked at open
                                instead of per-region language).
* ``mixed_content_page.pdf``  — one page with live vector text AND a raster
                                "scanned" image block (rasterised text). Exercises
                                defect #4 (page-wide ``hasTextContent()`` tool
                                routing instead of per-region routing).

The fourth required fixture, ``invoice_vector_heading.pdf``, is a verbatim copy
of the user's real invoice and is committed directly (not generated here).

Run:  python tests/fixtures/generate_render_oracle_fixtures.py

Deterministic in CONTENT: no timestamps, no RNG seeds that vary between runs.

NOT byte-identical between runs, though, and this docstring claimed otherwise
until 2026-09-25. PyMuPDF writes a fresh random ``/ID`` pair into the trailer on
every save, so re-running this script rewrites all five fixtures with 56 bytes
of difference and nothing else. Measured: the only delta between a re-run and
the committed copy of ``multilingual_latin_arabic.pdf`` is the two ``/ID``
hex strings.

The practical consequence, if you regenerate to change ONE fixture: `git status`
will show all five as modified. Restore the ones you did not mean to change --
their content is identical and the churn is noise that hides the real diff.
"""
from __future__ import annotations

import io
import sys
from pathlib import Path

import fitz  # PyMuPDF
from PIL import Image, ImageDraw, ImageFont

# Before the sibling import: running as a script puts this directory on
# sys.path automatically, importing as a module does not.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from glyphless_font import write_glyphless_ttf  # noqa: E402

FIXTURES_DIR = Path(__file__).resolve().parent

# Letter page in points.
PAGE_W_PT = 612.0
PAGE_H_PT = 792.0

# Windows font paths (this rig). Arial and Tahoma both carry Arabic glyph
# coverage; Times is Latin-only and used for the vector text blocks.
ARIAL = r"C:\Windows\Fonts\arial.ttf"
TAHOMA = r"C:\Windows\Fonts\tahoma.ttf"
TIMES = r"C:\Windows\Fonts\times.ttf"


def _solid_photo(px_w: int, px_h: int, base=(70, 110, 170)) -> Image.Image:
    """A deterministic non-blank 'photo': a smooth diagonal gradient with a
    couple of shapes so it has real high-frequency content (not a flat fill
    that ``region_is_blank`` would treat as empty)."""
    img = Image.new("RGB", (px_w, px_h), base)
    px = img.load()
    for y in range(px_h):
        for x in range(px_w):
            r = (base[0] + (x * 120) // max(1, px_w)) % 256
            g = (base[1] + (y * 120) // max(1, px_h)) % 256
            b = (base[2] + ((x + y) * 60) // max(1, px_w + px_h)) % 256
            px[x, y] = (r, g, b)
    d = ImageDraw.Draw(img)
    d.ellipse([px_w * 0.2, px_h * 0.2, px_w * 0.6, px_h * 0.6],
              outline=(255, 255, 255), width=max(2, px_w // 80))
    d.rectangle([px_w * 0.5, px_h * 0.5, px_w * 0.85, px_h * 0.85],
                outline=(20, 20, 20), width=max(2, px_w // 80))
    return img


def _rasterized_text(px_w: int, px_h: int, lines, font_path=ARIAL,
                     font_px=28) -> Image.Image:
    """Render text lines to a white raster image — simulates a *scanned*
    block (pixels, no live font / text-show operators)."""
    img = Image.new("RGB", (px_w, px_h), (255, 255, 255))
    d = ImageDraw.Draw(img)
    try:
        font = ImageFont.truetype(font_path, font_px)
    except OSError:
        font = ImageFont.load_default()
    y = font_px // 2
    for line in lines:
        d.text((10, y), line, fill=(0, 0, 0), font=font)
        y += int(font_px * 1.5)
    return img


# GitHub #250/#218. mixed_dpi_page.pdf carried NO font resources at all
# (`get_fonts()` == [] on page 0), so there was no OCR span to edit and the
# recomposite path MainWindow gates on isOcrInvisibleLayer() could not engage --
# the measured blocker behind the one render probe still marked xfail(strict).
#
# The layer is placed DETERMINISTICALLY from the same coordinates
# _rasterized_text() drew the pixels at, rather than by running OCR over the
# rendered page: a fixture generator that depends on OCR output depends on
# Tesseract's version-to-version behaviour, and this text is already known
# exactly because we rasterised it ourselves.
def _insert_invisible_ocr_layer(page, rect, px_w: int, px_h: int, lines,
                                font_px: int, fontname: str) -> None:
    """Invisible (Tr=3) glyphless text over a rasterised region.

    Mirrors _rasterized_text()'s layout: first line top at ``font_px // 2``,
    x at 10, subsequent lines every ``font_px * 1.5`` -- all in IMAGE pixels,
    scaled into ``rect``. PIL positions text by its TOP edge and PDF places it
    by the BASELINE, hence the ascent offset.
    """
    sx = rect.width / px_w
    sy = rect.height / px_h
    y_px = font_px // 2
    for line in lines:
        baseline_px = y_px + font_px * 0.8  # approximate ascent
        page.insert_text(
            fitz.Point(rect.x0 + 10 * sx, rect.y0 + baseline_px * sy),
            line,
            fontname=fontname,
            fontsize=font_px * sy,
            render_mode=3,  # invisible
        )
        y_px += int(font_px * 1.5)


def _pil_to_pixmap(img: Image.Image) -> fitz.Pixmap:
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return fitz.Pixmap(buf.getvalue())


def make_mixed_dpi_page(out: Path) -> None:
    """Image-DOMINATED page carrying a 300-DPI photo and a 150-DPI region.

    DPI of an embedded raster = pixel_width / (rect_width_pt / 72).

    The page must be image-dominated (>50% of the page area covered by
    images) so that editing the invisible OCR text over a region routes
    through the TRE *image* path (ImageReplacer), which is where defect #2
    lives — ImageReplacer asks Page::sourceImageDpi() for ONE page-wide DPI
    (the 300-DPI maximum) and re-renders the 150-DPI region at it, upsampling
    and blurring it. On a vector-dominated page the image path never engages.

    Layout (Letter 612x792):
    * Big 300-DPI photo: 2125 px wide in a 510 pt (≈7.08 in) rect → 300 DPI,
      covering ~500x600 pt = ~62% of the page (image-dominated).
    * 150-DPI region: 300 px wide in a 144 pt (2.0 in) rect → 150 DPI, drawn
      ON TOP of the big photo in the lower-left. The #2 proof edits the
      invisible 'THUMB' span over THIS region.
    """
    doc = fitz.open()
    page = doc.new_page(width=PAGE_W_PT, height=PAGE_H_PT)

    # Big 300-DPI region carrying REAL rasterised text (black on white) so the
    # TRE OCR pipeline detects words and the image-text-edit path (ImageReplacer)
    # can engage. 2125 px wide in a 510 pt rect => 300 DPI; ~62% of the page =>
    # image-dominated.
    hi_rect = fitz.Rect(51, 51, 51 + 510, 51 + 600)
    hi_lines = ["PHOTO REGION", "rendered at 300 DPI", "high resolution block",
                "the quick brown fox", "jumps over the lazy dog"]
    hi = _rasterized_text(2125, 2500, hi_lines, font_path=ARIAL, font_px=120)
    page.insert_image(hi_rect, pixmap=_pil_to_pixmap(hi))

    # 150-DPI region with its own rasterised text, drawn over the big block,
    # lower-left. 300 px wide in a 144 pt rect => 150 DPI. The #2 proof edits
    # the OCR'd word in THIS region; the defect re-renders it at the page-max
    # 300 DPI (upsampled => blurred) instead of its native 150 DPI.
    lo_rect = fitz.Rect(80, 470, 80 + 144, 470 + 144)
    lo_lines = ["THUMBNAIL", "small region"]
    lo = _rasterized_text(
        300, 300, lo_lines, font_path=ARIAL, font_px=40)
    page.insert_image(lo_rect, pixmap=_pil_to_pixmap(lo))

    # Invisible glyphless OCR layer over BOTH regions. The #2 proof edits the
    # 'THUMB' span, which must land inside lo_rect -- that rect is exactly
    # THUMB_REGION_BBOX in the render harness.
    ttf = Path(out).parent / "_glyphless_tmp.ttf"
    write_glyphless_ttf(ttf)
    try:
        page.insert_font(fontname="GLF", fontfile=str(ttf))
        _insert_invisible_ocr_layer(page, hi_rect, 2125, 2500, hi_lines,
                                    120, "GLF")
        _insert_invisible_ocr_layer(page, lo_rect, 300, 300, lo_lines,
                                    40, "GLF")
        doc.save(str(out), garbage=1, deflate=True)
    finally:
        doc.close()
        ttf.unlink(missing_ok=True)


def make_occluded_write_target_page(out: Path) -> None:
    """Page where the recomposite's write-back target is DETERMINISTICALLY the
    wrong (occluded) image, so the post-write visibility check can be tested.

    GitHub #262 / DCA 0105. The DCA's primary standing control is: a recomposite
    that changes nothing the user can see must report FAILED, not DONE. Nothing
    could exercise that control, because on every existing fixture the
    region-aware target selection is correct and the write is therefore visible
    — a check that never fires is indistinguishable from a check that is absent.

    This page forces the selection to bail out and the FALLBACK to land on an
    occluded image, which is exactly the path the DCA says the check protects:

    * ``base``  1200x1200 px at Rect(51, 51, 561, 651) — by a wide margin the
      largest image by pixel area, so ``findPageImageXObject()`` picks it
      deterministically rather than by dictionary order.
    * ``decoy`` 300x300 px at Rect(400, 100, 472, 172) — nowhere near the edited
      region. Its only job is to share pixel dimensions with ``top``.
    * ``top``   300x300 px at Rect(80, 470, 224, 614) — drawn LAST, over
      ``base``, and carrying the invisible OCR span that gets edited.

    The sequence that results, and every step of it is forced rather than
    incidental:

    1. ``findImageXObjectForRegion()`` walks the stext image blocks and picks
       ``top`` — it covers the region and has the tightest bbox.
    2. It then maps that back to an XObject name by pixel dimensions, and
       300x300 matches BOTH ``top`` and ``decoy``. Ambiguous, so it returns
       nullopt rather than guessing.
    3. The caller falls back to ``findPageImageXObject()``, which returns
       ``base`` — strictly the largest, so no tie to break.
    4. The composite is written into ``base``, and ``top`` — drawn afterwards —
       paints straight over it. The write is committed and invisible.

    So ``ImageReplacer::replace()`` over the ``top`` region must report
    ``success == false``. Remove the visibility check and it reports success for
    a write the user cannot see, which is GitHub #262 restored.

    Note the shape of this fixture is an argument FOR the ambiguity bail-out,
    not against it: the bail-out is what makes the failure honest instead of a
    coin-flip between two same-sized images.
    """
    doc = fitz.open()
    page = doc.new_page(width=PAGE_W_PT, height=PAGE_H_PT)

    base_rect = fitz.Rect(51, 51, 51 + 510, 51 + 600)
    base_lines = ["BASE REGION", "this image is the fallback target",
                  "and it is covered over", "the quick brown fox",
                  "jumps over the lazy dog"]
    base = _rasterized_text(1200, 1200, base_lines, font_path=ARIAL, font_px=70)
    page.insert_image(base_rect, pixmap=_pil_to_pixmap(base))

    # Same pixel dimensions as `top`, deliberately, and placed clear of the
    # edited region so it can only ever contribute the dimension ambiguity.
    decoy_rect = fitz.Rect(400, 100, 472, 172)
    decoy = _rasterized_text(300, 300, ["DECOY"], font_path=ARIAL, font_px=40)
    page.insert_image(decoy_rect, pixmap=_pil_to_pixmap(decoy))

    top_rect = fitz.Rect(80, 470, 80 + 144, 470 + 144)
    top_lines = ["OCCLUDED", "top image"]
    top = _rasterized_text(300, 300, top_lines, font_path=ARIAL, font_px=40)
    page.insert_image(top_rect, pixmap=_pil_to_pixmap(top))

    ttf = Path(out).parent / "_glyphless_tmp.ttf"
    write_glyphless_ttf(ttf)
    try:
        page.insert_font(fontname="GLF", fontfile=str(ttf))
        _insert_invisible_ocr_layer(page, base_rect, 1200, 1200, base_lines,
                                    70, "GLF")
        _insert_invisible_ocr_layer(page, top_rect, 300, 300, top_lines,
                                    40, "GLF")
        doc.save(str(out), garbage=1, deflate=True)
    finally:
        doc.close()
        ttf.unlink(missing_ok=True)


def make_multilingual(out: Path) -> None:
    """SCANNED Latin block + SCANNED Arabic block on one Letter page.

    The blocks are RASTERISED text (black on white, embedded as images), not a
    live vector text layer. This is deliberate and required for the defect-#3
    OCR test: OCR (Tesseract) operates on the *rendered page image*, and a
    purely-vector text layer does not reach the OCR render path as recognisable
    pixels (PDFE's OcrPipeline::renderPage produced an empty hOCR on the old
    vector version — Tesseract saw nothing). A scanned page is also the correct
    input for the Phase-7 FontMatcher integration, which reconstructs fonts for
    OCR'd *scanned* regions, not live embedded fonts.

    The #3 signature: with the doc-wide OCR language baked to English at open,
    the Arabic block OCRs to Latin garbage with zero Arabic-script codepoints;
    the Latin block OCRs cleanly (the control that proves OCR ran). Arabic is
    drawn in isolated forms (PIL has no bidi/shaping), which is sufficient — the
    test only asserts presence/absence of Arabic-script codepoints in the OCR
    *output*, not shaped rendering.
    """
    doc = fitz.open()
    page = doc.new_page(width=PAGE_W_PT, height=PAGE_H_PT)

    # Latin block — large, clean raster so Tesseract reads it under eng (the
    # control that keeps the #3 xfail honest: OCR genuinely ran).
    latin_img = _rasterized_text(
        1400, 600,
        ["Latin block:",
         "The quick brown fox jumps over",
         "the lazy dog. Latin script body",
         "text for OCR language routing."],
        font_path=ARIAL, font_px=64)
    page.insert_image(fitz.Rect(72, 80, 72 + 468, 80 + 200),
                      pixmap=_pil_to_pixmap(latin_img))

    # Arabic block — rasterised genuine Arabic Unicode (U+0600..U+06FF) via a
    # font with Arabic coverage (Tahoma). Isolated forms; bidi not required.
    arabic_img = _rasterized_text(
        1400, 600,
        ["Arabic block:",
         "مرحبا بالعالم",
         "هذا نص عربي",
         "العربية النص"],
        font_path=TAHOMA, font_px=72)
    page.insert_image(fitz.Rect(72, 320, 72 + 468, 320 + 200),
                      pixmap=_pil_to_pixmap(arabic_img))
    doc.save(str(out), garbage=1, deflate=True)
    doc.close()


def make_mixed_content(out: Path) -> None:
    """Live vector text block + raster 'scanned' image block on one page.

    The vector block uses real text-show operators (a live font); the scanned
    block is a rasterised-text PNG (pixels only). Defect #4: page-wide
    ``hasTextContent()`` is true because of the vector block, so the whole page
    routes to the span tool and the scanned block can't be region-edited.
    """
    doc = fitz.open()
    page = doc.new_page(width=PAGE_W_PT, height=PAGE_H_PT)

    # Live vector text block (top half).
    page.insert_text((72, 80), "Live vector text block", fontsize=14,
                     fontfile=TIMES, fontname="times")
    page.insert_textbox(
        fitz.Rect(72, 100, 540, 240),
        "This paragraph is real vector text drawn with text-show operators. "
        "Editing here should route to the per-span text tool.",
        fontsize=12, fontfile=TIMES, fontname="times")

    # Scanned image block (bottom half): rasterised text, no live font.
    scan = _rasterized_text(
        900, 300,
        ["SCANNED IMAGE BLOCK", "(rasterised text - no live font)",
         "Editing here should route to the region tool."],
        font_path=ARIAL, font_px=34)
    scan_rect = fitz.Rect(72, 360, 72 + 450, 360 + 150)  # 900px/ (450/72)=144DPI
    page.insert_text((72, 350), "Scanned block:", fontsize=10,
                     fontfile=TIMES, fontname="times")
    page.insert_image(scan_rect, pixmap=_pil_to_pixmap(scan))
    doc.save(str(out), garbage=1, deflate=True)
    doc.close()


def make_nav_selfcheck(out: Path) -> None:
    """Two visually VERY different pages — the render-oracle self-check control.

    Page 0 is a large blue field with 'PAGE ONE'; page 1 is a large red field
    with 'PAGE TWO'. >50% of pixels differ between pages, so a successful
    navigation produces an unmistakable canvas change (the existing multi-page
    fixtures differ by <0.05% — far below any sane render threshold).
    """
    doc = fitz.open()
    for idx, (label, color) in enumerate(
            [("PAGE ONE", (0.15, 0.30, 0.75)),
             ("PAGE TWO", (0.80, 0.20, 0.15))]):
        page = doc.new_page(width=PAGE_W_PT, height=PAGE_H_PT)
        page.draw_rect(fitz.Rect(40, 40, PAGE_W_PT - 40, PAGE_H_PT - 40),
                       color=color, fill=color)
        page.insert_text((90, 420), label, fontsize=72,
                         fontfile=TIMES, fontname="times",
                         color=(1, 1, 1))
    doc.save(str(out), garbage=1, deflate=True)
    doc.close()


def make_coord_rotation_page(out: Path) -> None:
    """A /Rotate 90 page carrying a text run whose OWN text matrix (Tm) ALSO
    encodes a 90-degree rotation — the defect-#6 double-rotation trap.

    CoordMapper used to apply the page /Rotate as a blanket correction to every
    element; a text run that already carries its own quarter-turn would then be
    rotated twice and land off-screen. The per-run residual fix
    (CoordMapper::computeRunTransform) applies the page /Rotate only as the
    residual the run's Tm does not already account for, so the text maps to the
    correct on-screen location.

    Layout: insert_textbox(rotate=90) turns the text a quarter turn within its
    box (this is what writes the rotation into the run's Tm); page.set_rotation(90)
    sets the page-level /Rotate. The two rotations compose to render the text
    UPRIGHT and roughly centred — visible on the canvas, not clipped away. The
    authoritative unit proof is gtest test_coord_mapper_rotation.cpp; this fixture
    backs the render-oracle regression guard (test_render_coord_rotation.py).
    """
    doc = fitz.open()
    page = doc.new_page(width=PAGE_W_PT, height=PAGE_H_PT)
    # Centre-ish box; rotate=90 encodes the run's own rotation in its Tm.
    box = fitz.Rect(140, 230, 472, 560)
    page.insert_textbox(box, "ROTATED 90 ON ROTATE-90 PAGE",
                        fontsize=44, fontfile=TIMES, fontname="times",
                        rotate=90, align=1)
    page.set_rotation(90)
    doc.save(str(out), garbage=1, deflate=True)
    doc.close()


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    targets = [
        ("mixed_dpi_page.pdf", make_mixed_dpi_page),
        ("multilingual_latin_arabic.pdf", make_multilingual),
        ("mixed_content_page.pdf", make_mixed_content),
        ("nav_selfcheck.pdf", make_nav_selfcheck),
        ("coord_rotation_page.pdf", make_coord_rotation_page),
    ]
    for name, fn in targets:
        out = FIXTURES_DIR / name
        fn(out)
        size = out.stat().st_size
        print(f"  wrote {name}  ({size} bytes)")
    print("done.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
