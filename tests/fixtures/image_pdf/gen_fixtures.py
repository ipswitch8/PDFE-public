# -*- coding: utf-8 -*-
"""
Deterministic fixture generator for image-based PDF edit pipeline tests.

Produces synthetic scanned-looking PDFs that target the six risks
documented in docs/image-pdf-risks.md. Each fixture is designed so
that a test can assert DPI, background classification, cache
behavior, or width-measurement correctness against a known ground
truth.

The generator uses Pillow to rasterize pages at arbitrary DPI, then
PyMuPDF (fitz) to wrap the raster in a PDF page sized to match the
implied physical dimensions. The key invariant is:

    image_pixel_width  / page_width_in_inches  == fixture_dpi
    image_pixel_height / page_height_in_inches == fixture_dpi

This matches how `Page::sourceImageDpi()` will infer native DPI once
A3 is fixed.

Usage:
    python gen_fixtures.py              # generate all fixtures
    python gen_fixtures.py --risk A3    # only A3 fixtures
    python gen_fixtures.py --clean      # remove existing fixtures first

Deterministic: seeded RNG, no timestamps in output, same bytes on rerun.
"""
from __future__ import annotations

import argparse
import io
import math
import random
import sys
from pathlib import Path
from typing import Tuple

import fitz  # pymupdf
from PIL import Image, ImageDraw, ImageFilter, ImageFont

SCRIPT_DIR = Path(__file__).resolve().parent
RNG_SEED = 0xC0FFEE

# Page sizes in inches (width, height)
PAGE_SIZES_IN = {
    "letter": (8.5, 11.0),
    "legal": (8.5, 14.0),
    "tabloid": (11.0, 17.0),
    "a4": (8.27, 11.69),
}


def find_font(preferred: list[str], size: int) -> ImageFont.FreeTypeFont:
    for name in preferred:
        try:
            return ImageFont.truetype(name, size)
        except (IOError, OSError):
            continue
    return ImageFont.load_default()


def latin_font(size: int) -> ImageFont.FreeTypeFont:
    return find_font(
        ["arial.ttf", "Arial.ttf", "DejaVuSans.ttf", "FreeSans.ttf"], size
    )


def latin_serif(size: int) -> ImageFont.FreeTypeFont:
    return find_font(
        ["times.ttf", "Times.ttf", "DejaVuSerif.ttf", "FreeSerif.ttf"], size
    )


def latin_mono(size: int) -> ImageFont.FreeTypeFont:
    return find_font(
        ["cour.ttf", "Courier.ttf", "DejaVuSansMono.ttf", "FreeMono.ttf"], size
    )


def make_page(
    dpi: int,
    size_in: Tuple[float, float],
    bg: Tuple[int, int, int] = (255, 255, 255),
) -> Image.Image:
    w_px = int(round(size_in[0] * dpi))
    h_px = int(round(size_in[1] * dpi))
    return Image.new("RGB", (w_px, h_px), bg)


def save_as_pdf(img: Image.Image, path: Path, dpi: int) -> None:
    """Wrap a PIL raster in a PDF page whose MediaBox matches the DPI.

    PyMuPDF accepts raw pixmap bytes; we use insert_image for a page
    of the right physical size so (pixel width / page width in pt) * 72
    == dpi.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    w_px, h_px = img.size
    w_pt = w_px * 72.0 / dpi
    h_pt = h_px * 72.0 / dpi
    doc = fitz.open()
    page = doc.new_page(width=w_pt, height=h_pt)
    buf = io.BytesIO()
    # Save as PNG for lossless fixtures (B3/A3 care about exact pixels);
    # downstream tests that want JPEG-compressed inputs rasterize
    # separately.
    img.save(buf, format="PNG")
    page.insert_image(page.rect, stream=buf.getvalue())
    doc.save(str(path), deflate=True, garbage=3)
    doc.close()


def save_as_pdf_jpeg(img: Image.Image, path: Path, dpi: int, quality: int = 75) -> None:
    """Same as save_as_pdf but embeds a JPEG stream, to exercise the
    JPEG recompression cache path in C2 tests."""
    path.parent.mkdir(parents=True, exist_ok=True)
    w_px, h_px = img.size
    w_pt = w_px * 72.0 / dpi
    h_pt = h_px * 72.0 / dpi
    doc = fitz.open()
    page = doc.new_page(width=w_pt, height=h_pt)
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=quality, optimize=False)
    page.insert_image(page.rect, stream=buf.getvalue())
    doc.save(str(path), deflate=True, garbage=3)
    doc.close()


def save_multipage_pdf(
    imgs: list[Image.Image], path: Path, dpis: list[int], jpeg: bool = True
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    doc = fitz.open()
    for img, dpi in zip(imgs, dpis):
        w_px, h_px = img.size
        w_pt = w_px * 72.0 / dpi
        h_pt = h_px * 72.0 / dpi
        page = doc.new_page(width=w_pt, height=h_pt)
        buf = io.BytesIO()
        if jpeg:
            img.save(buf, format="JPEG", quality=75, optimize=False)
        else:
            img.save(buf, format="PNG")
        page.insert_image(page.rect, stream=buf.getvalue())
    doc.save(str(path), deflate=True, garbage=3)
    doc.close()


# --------------------------------------------------------------------------
# A3 — source image DPI fixtures
# --------------------------------------------------------------------------


def _draw_a3_sample(img: Image.Image, dpi: int, label: str) -> None:
    draw = ImageDraw.Draw(img)
    w, h = img.size
    margin = int(dpi * 0.5)
    # Title in 24pt-equivalent
    font_big = latin_font(int(dpi * 24 / 72))
    font_body = latin_font(int(dpi * 12 / 72))
    draw.text(
        (margin, margin),
        f"A3 fixture — {label}",
        fill=(0, 0, 0),
        font=font_big,
    )
    lines = [
        "The quick brown fox jumps over the lazy dog.",
        "Pack my box with five dozen liquor jugs.",
        "How vexingly quick daft zebras jump!",
        "Sphinx of black quartz, judge my vow.",
        "Waltz, bad nymph, for quick jigs vex.",
    ]
    y = margin + int(dpi * 0.6)
    for line in lines:
        draw.text((margin, y), line, fill=(0, 0, 0), font=font_body)
        y += int(dpi * 0.25)


def gen_a3() -> list[Path]:
    out_dir = SCRIPT_DIR
    generated: list[Path] = []

    specs = [
        ("a3_72dpi_web_letter.pdf", 72, "letter"),
        ("a3_150dpi_a4_photo.pdf", 150, "a4"),
        ("a3_200dpi_fax_legal.pdf", 200, "legal"),
        ("a3_300dpi_office_letter.pdf", 300, "letter"),
        ("a3_600dpi_archival_a4.pdf", 600, "a4"),
        ("a3_1200dpi_ocr_letter.pdf", 1200, "letter"),
    ]
    for fname, dpi, size_key in specs:
        img = make_page(dpi, PAGE_SIZES_IN[size_key])
        _draw_a3_sample(img, dpi, f"{dpi} DPI")
        path = out_dir / fname
        save_as_pdf(img, path, dpi)
        generated.append(path)

    # Mixed-DPI multipage
    mixed_dpis = [72, 150, 300, 600, 1200]
    imgs = []
    for dpi in mixed_dpis:
        img = make_page(dpi, PAGE_SIZES_IN["letter"])
        _draw_a3_sample(img, dpi, f"page at {dpi} DPI")
        imgs.append(img)
    path = out_dir / "a3_mixed_dpi_5page.pdf"
    save_multipage_pdf(imgs, path, mixed_dpis, jpeg=False)
    generated.append(path)

    return generated


# --------------------------------------------------------------------------
# C2 — LRU cache fixtures
# --------------------------------------------------------------------------


def _draw_c2_sample(img: Image.Image, dpi: int, page_num: int) -> None:
    draw = ImageDraw.Draw(img)
    margin = int(dpi * 0.5)
    font_big = latin_font(int(dpi * 28 / 72))
    font_body = latin_font(int(dpi * 11 / 72))
    draw.text(
        (margin, margin),
        f"Page {page_num}",
        fill=(0, 0, 0),
        font=font_big,
    )
    # Dense body text to bulk up the image size
    rng = random.Random(RNG_SEED + page_num)
    words = [
        "alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf",
        "hotel", "india", "juliet", "kilo", "lima", "mike", "november",
    ]
    y = margin + int(dpi * 0.7)
    for _ in range(40):
        line = " ".join(rng.choice(words) for _ in range(10))
        draw.text((margin, y), line, fill=(0, 0, 0), font=font_body)
        y += int(dpi * 0.18)
        if y > img.size[1] - margin:
            break


def gen_c2() -> list[Path]:
    out_dir = SCRIPT_DIR
    generated: list[Path] = []

    # 6 pages at 300 DPI letter
    imgs = [make_page(300, PAGE_SIZES_IN["letter"]) for _ in range(6)]
    for i, img in enumerate(imgs):
        _draw_c2_sample(img, 300, i + 1)
    path = out_dir / "c2_small_6page_300dpi.pdf"
    save_multipage_pdf(imgs, path, [300] * 6, jpeg=True)
    generated.append(path)

    # 12 pages at 600 DPI letter — large
    imgs = [make_page(600, PAGE_SIZES_IN["letter"]) for _ in range(12)]
    for i, img in enumerate(imgs):
        _draw_c2_sample(img, 600, i + 1)
    path = out_dir / "c2_large_12page_600dpi.pdf"
    save_multipage_pdf(imgs, path, [600] * 12, jpeg=True)
    generated.append(path)

    # 20 pages at 72 DPI letter — tiny
    imgs = [make_page(72, PAGE_SIZES_IN["letter"]) for _ in range(20)]
    for i, img in enumerate(imgs):
        _draw_c2_sample(img, 72, i + 1)
    path = out_dir / "c2_tiny_20page_72dpi.pdf"
    save_multipage_pdf(imgs, path, [72] * 20, jpeg=True)
    generated.append(path)

    # 8-page mixed DPI
    mixed_dpis = [72, 150, 300, 300, 300, 600, 600, 1200]
    imgs = []
    for i, dpi in enumerate(mixed_dpis):
        img = make_page(dpi, PAGE_SIZES_IN["letter"])
        _draw_c2_sample(img, dpi, i + 1)
        imgs.append(img)
    path = out_dir / "c2_mixed_8page.pdf"
    save_multipage_pdf(imgs, path, mixed_dpis, jpeg=True)
    generated.append(path)

    # 10-page color ledger (blue-lined paper) at 300 DPI legal
    imgs = []
    for i in range(10):
        img = make_page(300, PAGE_SIZES_IN["legal"], bg=(250, 252, 255))
        draw = ImageDraw.Draw(img)
        w, h = img.size
        # Draw blue horizontal rules
        for y in range(int(300 * 0.5), h, int(300 * 0.25)):
            draw.line([(0, y), (w, y)], fill=(180, 200, 230), width=2)
        _draw_c2_sample(img, 300, i + 1)
        imgs.append(img)
    path = out_dir / "c2_color_ledger_10page.pdf"
    save_multipage_pdf(imgs, path, [300] * 10, jpeg=True)
    generated.append(path)

    return generated


# --------------------------------------------------------------------------
# B3 — background classification fixtures
# --------------------------------------------------------------------------


def _halftone_pattern(width: int, height: int, dpi: int, density: float = 0.5) -> Image.Image:
    """Create a halftone-dithered gray background.

    Dot spacing scales with DPI so the pattern represents a real-world
    halftone screen at a consistent ~85 LPI equivalent."""
    img = Image.new("L", (width, height), 255)
    draw = ImageDraw.Draw(img)
    spacing = max(3, int(dpi / 85))  # ~85 line halftone
    dot_radius = max(1, int(spacing * math.sqrt(density) * 0.55))
    for y in range(0, height, spacing):
        offset = (spacing // 2) if (y // spacing) % 2 == 0 else 0
        for x in range(-spacing, width + spacing, spacing):
            xc = x + offset
            draw.ellipse(
                [xc - dot_radius, y - dot_radius, xc + dot_radius, y + dot_radius],
                fill=0,
            )
    return img.convert("RGB")


def _draw_b3_text(img: Image.Image, dpi: int, text: str = "Sample Edit Target") -> None:
    draw = ImageDraw.Draw(img)
    margin = int(dpi * 0.75)
    font = latin_serif(int(dpi * 14 / 72))
    lines = [
        text,
        "The detector must classify this background correctly.",
        "Replacement fill should preserve the pattern.",
    ]
    y = margin
    for line in lines:
        draw.text((margin, y), line, fill=(20, 20, 20), font=font)
        y += int(dpi * 0.35)


def gen_b3() -> list[Path]:
    out_dir = SCRIPT_DIR
    generated: list[Path] = []
    rng = random.Random(RNG_SEED)

    # b3_uniform_white_300dpi.pdf
    img = make_page(300, PAGE_SIZES_IN["letter"])
    _draw_b3_text(img, 300, "Uniform white")
    p = out_dir / "b3_uniform_white_300dpi.pdf"
    save_as_pdf(img, p, 300)
    generated.append(p)

    # b3_yellowed_news_300dpi.pdf
    img = make_page(300, PAGE_SIZES_IN["letter"], bg=(245, 232, 200))
    _draw_b3_text(img, 300, "Yellowed news (uniform color)")
    p = out_dir / "b3_yellowed_news_300dpi.pdf"
    save_as_pdf(img, p, 300)
    generated.append(p)

    # b3_halftone_news_200dpi.pdf
    w_px = int(8.5 * 200)
    h_px = int(11.0 * 200)
    img = _halftone_pattern(w_px, h_px, 200, density=0.45)
    _draw_b3_text(img, 200, "Halftone 200 DPI")
    p = out_dir / "b3_halftone_news_200dpi.pdf"
    save_as_pdf(img, p, 200)
    generated.append(p)

    # b3_halftone_news_600dpi.pdf — same pattern at higher DPI
    w_px = int(8.5 * 600)
    h_px = int(11.0 * 600)
    img = _halftone_pattern(w_px, h_px, 600, density=0.45)
    _draw_b3_text(img, 600, "Halftone 600 DPI")
    p = out_dir / "b3_halftone_news_600dpi.pdf"
    save_as_pdf(img, p, 600)
    generated.append(p)

    # b3_blue_ledger_300dpi.pdf
    img = make_page(300, PAGE_SIZES_IN["legal"], bg=(252, 253, 255))
    draw = ImageDraw.Draw(img)
    w, h = img.size
    for y in range(int(300 * 0.5), h, int(300 * 0.3)):
        draw.line([(0, y), (w, y)], fill=(160, 180, 215), width=3)
    # Vertical red margin line
    draw.line(
        [(int(300 * 1.25), 0), (int(300 * 1.25), h)],
        fill=(200, 120, 130),
        width=3,
    )
    _draw_b3_text(img, 300, "Blue ledger")
    p = out_dir / "b3_blue_ledger_300dpi.pdf"
    save_as_pdf(img, p, 300)
    generated.append(p)

    # b3_textured_gray_300dpi.pdf — noise background
    w_px = int(8.5 * 300)
    h_px = int(11.0 * 300)
    # Salt-and-pepper style
    noise = Image.new("L", (w_px, h_px), 0)
    px = noise.load()
    for y in range(h_px):
        for x in range(w_px):
            px[x, y] = 200 + rng.randint(-30, 30)
    img = noise.convert("RGB")
    _draw_b3_text(img, 300, "Speckled gray texture")
    p = out_dir / "b3_textured_gray_300dpi.pdf"
    save_as_pdf(img, p, 300)
    generated.append(p)

    # b3_gradient_shaded_300dpi.pdf — page shadow
    img = make_page(300, PAGE_SIZES_IN["letter"])
    w, h = img.size
    px = img.load()
    for y in range(h):
        for x in range(w):
            # Linear gradient dark bottom-right
            t = (x / w * 0.5 + y / h * 0.5)
            v = int(255 - t * 60)
            px[x, y] = (v, v, v)
    _draw_b3_text(img, 300, "Shaded gradient")
    p = out_dir / "b3_gradient_shaded_300dpi.pdf"
    save_as_pdf(img, p, 300)
    generated.append(p)

    # b3_highlighted_yellow_300dpi.pdf
    img = make_page(300, PAGE_SIZES_IN["letter"])
    draw = ImageDraw.Draw(img)
    # Yellow highlight rectangle
    draw.rectangle(
        [int(300 * 0.7), int(300 * 1.0), int(300 * 7.5), int(300 * 1.6)],
        fill=(255, 240, 100),
    )
    _draw_b3_text(img, 300, "Highlighted")
    p = out_dir / "b3_highlighted_yellow_300dpi.pdf"
    save_as_pdf(img, p, 300)
    generated.append(p)

    # b3_jpeg_blocky_200dpi.pdf — re-encode through low-quality JPEG
    img = make_page(200, PAGE_SIZES_IN["letter"], bg=(240, 240, 240))
    _draw_b3_text(img, 200, "JPEG blocky")
    # Force through low-Q JPEG
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=25)
    buf.seek(0)
    blocky = Image.open(buf).convert("RGB")
    p = out_dir / "b3_jpeg_blocky_200dpi.pdf"
    save_as_pdf_jpeg(blocky, p, 200, quality=25)
    generated.append(p)

    return generated


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------


def clean(out_dir: Path) -> None:
    for p in out_dir.glob("*.pdf"):
        if p.is_file():
            p.unlink()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--risk", choices=["A3", "C2", "B3", "all"], default="all")
    ap.add_argument("--clean", action="store_true")
    args = ap.parse_args()

    random.seed(RNG_SEED)
    out_dir = SCRIPT_DIR
    if args.clean:
        clean(out_dir)

    all_generated: list[Path] = []
    if args.risk in ("A3", "all"):
        print("=== A3 fixtures ===")
        all_generated += gen_a3()
    if args.risk in ("C2", "all"):
        print("=== C2 fixtures ===")
        all_generated += gen_c2()
    if args.risk in ("B3", "all"):
        print("=== B3 fixtures ===")
        all_generated += gen_b3()

    for p in all_generated:
        size_kb = p.stat().st_size / 1024
        print(f"  {p.name}  ({size_kb:.1f} KB)")
    print(f"\nGenerated {len(all_generated)} fixtures in {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
