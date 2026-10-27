#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate 3 synthetic "scanned" PDFs for OCR testing:
    test_ocr_clean.pdf     — 300 DPI, clean single-column page.
    test_ocr_skewed.pdf    — 200 DPI, rotated 2° + Gaussian noise.
    test_ocr_multicol.pdf  — two-column newspaper-style layout, 300 DPI.

Each has a sibling .expected.txt containing the source text verbatim so
OCR tests can compute recall against a known ground truth.

Scans are synthetic, not photographed. That's allowed by the Phase-10
acceptance criterion ("test-synthetic or appropriately licensed") and
keeps recall numbers stable across runs.
"""

from pathlib import Path
import sys
import io

try:
    from PIL import Image, ImageDraw, ImageFilter, ImageFont
    import fitz  # PyMuPDF
except ImportError:
    print("Install Pillow and pymupdf first")
    sys.exit(1)


FIXTURE_DIR = Path(__file__).parent

CLEAN_TEXT = (
    "The quick brown fox jumps over the lazy dog. "
    "Pack my box with five dozen liquor jugs. "
    "Sphinx of black quartz, judge my vow."
)

SKEWED_TEXT = (
    "Rotated and noisy page. "
    "Tesseract should still extract most of this. "
    "Line two continues the content."
)

MULTICOL_TEXT_LEFT = (
    "Left column paragraph. "
    "Contains several sentences of ordinary English. "
    "Wraps within the narrow column width."
)
MULTICOL_TEXT_RIGHT = (
    "Right column paragraph. "
    "Independent content next to the left. "
    "Layout tests column-detection."
)


def _default_font(size: int) -> ImageFont.ImageFont:
    """Return a usable truetype font or fall back to the bitmap default."""
    candidates = [
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/times.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    ]
    for path in candidates:
        try:
            return ImageFont.truetype(path, size)
        except Exception:
            continue
    return ImageFont.load_default()


def _render_wrapped(draw, font, text, box, fill=0):
    """Word-wrap `text` inside `box` (x, y, w). Returns total lines drawn."""
    x, y, w = box
    words = text.split()
    line = ""
    for word in words:
        candidate = (line + " " + word).strip()
        if draw.textlength(candidate, font=font) <= w or not line:
            line = candidate
        else:
            draw.text((x, y), line, font=font, fill=fill)
            line_height = font.size + 4
            y += line_height
            line = word
    if line:
        draw.text((x, y), line, font=font, fill=fill)


def _image_to_pdf(img: Image.Image, pdf_path: Path):
    """Wrap a PIL image into a single-page PDF via PyMuPDF."""
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    buf.seek(0)
    pix = fitz.Pixmap(buf.getvalue())
    doc = fitz.open()
    page = doc.new_page(width=pix.width, height=pix.height)
    page.insert_image(page.rect, pixmap=pix)
    doc.save(pdf_path.as_posix())
    doc.close()


def make_clean():
    w, h = 2550, 3300  # 8.5" x 11" at 300 DPI
    img = Image.new("RGB", (w, h), "white")
    draw = ImageDraw.Draw(img)
    font = _default_font(size=48)
    _render_wrapped(draw, font, CLEAN_TEXT, box=(300, 400, w - 600))
    out = FIXTURE_DIR / "test_ocr_clean.pdf"
    _image_to_pdf(img, out)
    (FIXTURE_DIR / "test_ocr_clean.expected.txt").write_text(
        CLEAN_TEXT, encoding="utf-8")
    print(f"[ok] {out.name}")


def make_skewed():
    w, h = 1700, 2200  # 200 DPI letter
    img = Image.new("RGB", (w, h), "white")
    draw = ImageDraw.Draw(img)
    font = _default_font(size=44)
    _render_wrapped(draw, font, SKEWED_TEXT, box=(200, 400, w - 400))
    # Rotate 2° and re-expand the canvas so no text is clipped.
    img = img.rotate(2.0, resample=Image.BICUBIC, fillcolor="white",
                     expand=True)
    # Light Gaussian noise via a blur of a salt-and-pepper mask.
    import random
    random.seed(0xC0FFEE)  # deterministic
    noise = Image.new("L", img.size, 0)
    npix = noise.load()
    for y in range(0, img.height, 3):
        for x in range(0, img.width, 3):
            npix[x, y] = random.randint(0, 20)
    noise = noise.filter(ImageFilter.GaussianBlur(1.0))
    img = Image.composite(img, Image.new("RGB", img.size, "white"),
                          noise.point(lambda v: 255 - v))
    out = FIXTURE_DIR / "test_ocr_skewed.pdf"
    _image_to_pdf(img, out)
    (FIXTURE_DIR / "test_ocr_skewed.expected.txt").write_text(
        SKEWED_TEXT, encoding="utf-8")
    print(f"[ok] {out.name}")


def make_multicol():
    w, h = 2550, 3300
    img = Image.new("RGB", (w, h), "white")
    draw = ImageDraw.Draw(img)
    font = _default_font(size=42)
    gutter = 150
    col_w = (w - 600 - gutter) // 2
    _render_wrapped(draw, font, MULTICOL_TEXT_LEFT,
                    box=(300, 400, col_w))
    _render_wrapped(draw, font, MULTICOL_TEXT_RIGHT,
                    box=(300 + col_w + gutter, 400, col_w))
    out = FIXTURE_DIR / "test_ocr_multicol.pdf"
    _image_to_pdf(img, out)
    (FIXTURE_DIR / "test_ocr_multicol.expected.txt").write_text(
        MULTICOL_TEXT_LEFT + "\n\n" + MULTICOL_TEXT_RIGHT,
        encoding="utf-8")
    print(f"[ok] {out.name}")


if __name__ == "__main__":
    make_clean()
    make_skewed()
    make_multicol()
