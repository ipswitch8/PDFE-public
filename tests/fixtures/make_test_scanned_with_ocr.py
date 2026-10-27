#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate test_scanned_with_ocr.pdf — same image as test_scanned.pdf
but with an invisible OCR text layer (Tr=3) over each detected word.

This fixture lets PDFE's TextEditTool find clickable spans on a
scanned page (since hasTextContent() returns true for the OCR layer
even though the layer is invisible). Required for the Group D atlas
tests:

  atlas_mode_combo_set_persist
  atlas_mode_visual_diff
  scan_style_toggle_persist
  glyph_synth_coverage
  font_match_panose_top_candidate

Run once to generate the fixture; the result is checked into git so
CI doesn't need Tesseract.

Usage::

    python tests/fixtures/make_test_scanned_with_ocr.py

    # check discovery without regenerating anything
    python tests/fixtures/make_test_scanned_with_ocr.py --print-tesseract

    # only if tesseract lives somewhere the glob cannot see
    python tests/fixtures/make_test_scanned_with_ocr.py --tesseract <path>

Requires pikepdf and PIL. Tesseract is found automatically anywhere under
``build/`` regardless of vcpkg triplet (see ``_TESSERACT_GLOBS``), then on
PATH; tessdata resolves to ``tests/fixtures/tessdata/``, which is committed.
No path needs to be supplied by hand on a normal checkout.
"""
from __future__ import annotations

import argparse
import io
import os
import pathlib
import subprocess
import sys

import pikepdf
from PIL import Image

# Before the sibling import: running this as a script puts this directory on
# sys.path automatically, but importing it as a module does not.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from glyphless_font import (ADVANCE as _ADVANCE, ASCENT, DESCENT,  # noqa: E402
                            FIRST_CHAR as _FIRST_CHAR,
                            GLYPHLESS_FONT_NAME,
                            LAST_CHAR as _LAST_CHAR,
                            build_glyphless_ttf)

REPO = pathlib.Path(__file__).resolve().parents[2]
SRC = REPO / "tests" / "fixtures" / "test_scanned.pdf"
DEST = REPO / "tests" / "fixtures" / "test_scanned_with_ocr.pdf"

# GitHub #250/#218. The lookup used to name ONE vcpkg triplet, `x64-windows`.
# This tree builds `x64-windows-repro`, so the binary sits at the SAME relative
# path under a different triplet directory and the search missed it entirely --
# then silently fell back to bare "tesseract" on PATH, where it is also absent.
# Globbing the triplet fixes it for every preset and every developer, which is
# what a repeatable step means; an explicit path passed by whoever happens to
# know it is not. `--tesseract` / TESSERACT_EXE stay available for a binary
# installed somewhere unusual.
#
# Likewise the tessdata search listed a build-output directory and Program
# Files but not `tests/fixtures/tessdata/`, which is where eng/ara/osd actually
# live in this repo.
_TESSERACT_GLOBS = [
    "build/*/vcpkg_installed/*/tools/tesseract/tesseract.exe",
    "build/*/installed/*/tools/tesseract/tesseract.exe",
    "build/*/vcpkg_scratch/packages/tesseract_*/tools/tesseract/tesseract.exe",
]
TESSDATA_CANDIDATES = [
    REPO / "tests" / "fixtures" / "tessdata",
    REPO / "build" / "vs2022" / "bin" / "Debug" / "tessdata",
    pathlib.Path(r"C:\Program Files\Tesseract-OCR\tessdata"),
]


def find_tesseract(explicit: str | None = None) -> str:
    """Locate tesseract.exe: explicit path, env var, globbed build tree, PATH."""
    if explicit:
        p = pathlib.Path(explicit)
        if not p.exists():
            raise SystemExit(f"--tesseract path does not exist: {p}")
        return str(p)

    env = os.environ.get("TESSERACT_EXE")
    if env and pathlib.Path(env).exists():
        return env

    for pattern in _TESSERACT_GLOBS:
        # Sorted so the choice is deterministic rather than filesystem-order.
        for hit in sorted(REPO.glob(pattern)):
            if hit.exists():
                return str(hit)

    legacy = pathlib.Path(r"C:\Program Files\Tesseract-OCR\tesseract.exe")
    if legacy.exists():
        return str(legacy)
    return "tesseract"  # rely on PATH


def find_tessdata() -> str:
    env = os.environ.get("TESSDATA_PREFIX")
    if env and pathlib.Path(env).exists():
        return env
    for p in TESSDATA_CANDIDATES:
        if p.exists():
            return str(p)
    return ""


# Set from --tesseract by main(); None means "discover it" (the normal path).
_EXPLICIT_TESSERACT: str | None = None

# GitHub #250/#218. This generator is named for an OCR invisible-text layer, but
# the layer it produced used /Helvetica -- a real font with real glyphs, merely
# drawn in Tr=3. PDFE decides "is this span an OCR layer?" with
# TextSpan::isOcrInvisibleLayer(), which is `isGlyphlessOcrFontName(fontName)`:
# a case-insensitive "glyphless" substring test after stripping a subset prefix.
# Helvetica fails it, so every span in the committed fixture answered
# RECOMPOSITE_SKIPPED|span-not-ocr-invisible and the recomposite path this
# fixture exists to exercise never ran once.
#
# Tesseract's own PDF writer emits /GlyphLessFont, but it needs `pdf.ttf` from
# tessdata, which vcpkg does not install (it exists only inside a buildtrees
# scratch directory that no clean checkout or CI runner has). So the font is
# GENERATED here instead: a real TrueType with empty outlines -- glyphless in
# fact, not just in name -- with ordinary advance widths so span bounding boxes
# stay non-degenerate and hit-testing still works.
def add_glyphless_font(pdf: "pikepdf.Pdf") -> "pikepdf.Object":
    """Embed the generated glyphless font and return the font dict."""
    ttf = build_glyphless_ttf()
    stream = pdf.make_stream(ttf)
    stream["/Length1"] = len(ttf)

    descriptor = pdf.make_indirect({
        "/Type": pikepdf.Name("/FontDescriptor"),
        "/FontName": pikepdf.Name("/" + GLYPHLESS_FONT_NAME),
        "/Flags": 4,  # symbolic
        "/FontBBox": pikepdf.Array([0, DESCENT, _ADVANCE, ASCENT]),
        "/ItalicAngle": 0,
        "/Ascent": ASCENT,
        "/Descent": DESCENT,
        "/CapHeight": 700,
        "/StemV": 80,
        "/FontFile2": stream,
    })
    return pdf.make_indirect({
        "/Type": pikepdf.Name("/Font"),
        "/Subtype": pikepdf.Name("/TrueType"),
        "/BaseFont": pikepdf.Name("/" + GLYPHLESS_FONT_NAME),
        "/FirstChar": _FIRST_CHAR,
        "/LastChar": _LAST_CHAR,
        "/Widths": pikepdf.Array([_ADVANCE] * (_LAST_CHAR - _FIRST_CHAR + 1)),
        "/Encoding": pikepdf.Name("/WinAnsiEncoding"),
        "/FontDescriptor": descriptor,
    })


def run_ocr(image_path: pathlib.Path) -> list:
    """Return a list of (word, x0, y0, x1, y1, conf) in image-pixel coords."""
    tesseract = find_tesseract(_EXPLICIT_TESSERACT)
    tessdata = find_tessdata()
    out_base = image_path.parent / "ocr_output"
    cmd = [tesseract, str(image_path), str(out_base), "-l", "eng",
           "-c", "tessedit_create_tsv=1"]
    env = os.environ.copy()
    if tessdata:
        env["TESSDATA_PREFIX"] = tessdata
    subprocess.run(cmd, check=True, env=env, capture_output=True)
    tsv_path = out_base.with_suffix(".tsv")
    words = []
    with tsv_path.open("r", encoding="utf-8") as f:
        next(f)  # header
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 12:
                continue
            try:
                conf = float(parts[10])
            except ValueError:
                continue
            text = parts[11].strip()
            if not text or conf < 30:
                continue
            x = int(parts[6]); y = int(parts[7])
            w = int(parts[8]); h = int(parts[9])
            words.append((text, x, y, x + w, y + h, conf))
    tsv_path.unlink(missing_ok=True)
    out_base.with_suffix(".txt").unlink(missing_ok=True)
    return words


def main(argv: list | None = None) -> int:
    global _EXPLICIT_TESSERACT
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--tesseract",
        default=None,
        help="path to tesseract.exe. Normally unnecessary -- the build tree is "
             "globbed across vcpkg triplets. Also reads TESSERACT_EXE.")
    ap.add_argument(
        "--print-tesseract", action="store_true",
        help="print the resolved tesseract path and exit (discovery check)")
    args = ap.parse_args(argv)
    _EXPLICIT_TESSERACT = args.tesseract

    if args.print_tesseract:
        print(find_tesseract(_EXPLICIT_TESSERACT))
        print(f"tessdata: {find_tessdata() or '<none found>'}")
        return 0

    if not SRC.exists():
        print(f"source missing: {SRC}", file=sys.stderr)
        return 1

    pdf = pikepdf.open(str(SRC))
    page = pdf.pages[0]
    img_obj = page.images["/Im0"]
    pi = pikepdf.PdfImage(img_obj)
    img_w, img_h = pi.width, pi.height

    # Page MediaBox is [0,0,612,792] — Letter portrait.
    media_box = [float(v) for v in page.MediaBox]
    page_w = media_box[2] - media_box[0]
    page_h = media_box[3] - media_box[1]
    print(f"image: {img_w}x{img_h}, page: {page_w}x{page_h}")

    # Render image and OCR.
    pil = pi.as_pil_image()
    tmp_png = SRC.parent / "_ocr_tmp.png"
    pil.save(tmp_png)
    try:
        words = run_ocr(tmp_png)
    finally:
        tmp_png.unlink(missing_ok=True)
    print(f"OCR found {len(words)} words")

    # Build invisible (Tr=3) text content stream.
    # PDF coords have origin bottom-left; y in PDF = page_h - y_image_top.
    sx = page_w / img_w
    sy = page_h / img_h

    lines: list = []
    lines.append(b"q")  # save graphics state
    lines.append(b"BT")  # begin text
    lines.append(b"3 Tr")  # invisible text rendering mode

    # /F1 is the generated glyphless font, added to page resources below.
    lines.append(b"/F1 12 Tf")  # set font + size (placeholder size, OCR uses scale matrix)

    for w_text, x0, y0, x1, y1, _conf in words:
        wpdf_x = x0 * sx
        wpdf_y = page_h - (y1 * sy)  # PDF y = page_h - bottom of word in image
        wpdf_h = (y1 - y0) * sy
        wpdf_w = (x1 - x0) * sx
        # Set font size to roughly match word height (Tr=3 invisible
        # so visual size doesn't matter, but the bbox drives PDFE's
        # span hit-testing).
        fs = max(wpdf_h * 0.95, 6.0)
        # Position via Tm (text matrix) for absolute placement.
        # Tm = [Sx 0 0 Sy Tx Ty] where Sx=Sy=1 for normal scaling.
        # We use (1,0,0,1, x, y).
        lines.append(f"1 0 0 1 {wpdf_x:.2f} {wpdf_y:.2f} Tm".encode())
        lines.append(f"/F1 {fs:.2f} Tf".encode())
        # Show string. Escape parens in the OCR text.
        text_escaped = w_text.replace("\\", "\\\\").replace("(", "\\(") \
                              .replace(")", "\\)")
        lines.append(f"({text_escaped}) Tj".encode())

    lines.append(b"ET")
    lines.append(b"Q")
    overlay = b"\n".join(lines) + b"\n"

    # Append overlay to page contents and add /F1 to resources.
    # /F1 is the GENERATED glyphless font, not Helvetica -- see the note above
    # build_glyphless_ttf(). This is what makes the layer read as an OCR
    # invisible layer to TextSpan::isOcrInvisibleLayer() rather than as ordinary
    # vector text that merely happens to be drawn in Tr=3.
    resources = page.get("/Resources") or pikepdf.Dictionary({})
    fonts = resources.get("/Font") or pikepdf.Dictionary({})
    fonts["/F1"] = add_glyphless_font(pdf)
    resources["/Font"] = fonts
    page["/Resources"] = resources

    # Append the overlay to contents.
    existing = page.Contents
    if isinstance(existing, pikepdf.Array):
        existing.append(pdf.make_stream(overlay))
    else:
        # Single stream — convert to array
        new_array = pikepdf.Array([existing, pdf.make_stream(overlay)])
        page["/Contents"] = new_array

    pdf.save(str(DEST))
    pdf.close()
    print(f"wrote {DEST}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
