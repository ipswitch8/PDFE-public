# -*- coding: utf-8 -*-
"""Generate a deterministic mixed-size *scanned-with-OCR* PDF fixture for the
per-line font-size native-UI proof (pipeline c4a9e271 Phase 2).

The page is a full-page raster scan (so it is image-dominated → the text-edit
atlas path re-OCRs it and computes ``perLineFontSizePt``) WITH an invisible
(render_mode 3) text layer over it. The invisible layer matters for two reasons
the routing in MainWindow::activateTextEditForPage() requires:

  * ``page->hasTextContent()`` is true → the page routes to the per-span
    TextEditTool span-click flow (textEditToolAction_), NOT the drag-a-region
    TreRegionTool (treToolAction_). The per-line font-size fix
    (spanActivated → clickedLineSizePt → setOverlayFontSize) lives ONLY in the
    span-click flow.
  * ``page->extractText()`` returns spans for the user to click — a pure scan
    with no text layer yields no spans and spanActivated never fires.

This mirrors how the established test_scanned_with_ocr.pdf fixture works, but
with a KNOWN layout: ONE 96px-bold heading line and FOUR ~30px body lines, so
the heading glyph height is ~3x the body — well past the >=2x the phase needs —
and the click coordinates + expected size ratio are derived here, not guessed.

Letter @ 150 DPI => 1275 x 1650 px => 612 x 792 pt (px * 72/150 = px * 0.48).
fitz/PDF space is top-left origin, y down; insert_text() takes the glyph
BASELINE as its point. Coordinates the test clicks (PDF points):

    HEADING_CLICK_PT = (96.0,  72.0)   # inside the 96px heading band
    BODY_CLICK_PT    = (96.0, 233.0)   # inside body line 2 (~30px glyphs)

Run from the repo root:  python tests/fixtures/make_mixed_size_scan.py
Writes tests/fixtures/mixed_size_scan.pdf (committed).
"""
from __future__ import annotations

import io
import pathlib

import fitz  # PyMuPDF
from PIL import Image, ImageDraw, ImageFont

DPI = 150
PX_PER_PT = DPI / 72.0           # 2.0833...
PT_PER_PX = 72.0 / DPI           # 0.48
W_PX, H_PX = 1275, 1650          # US Letter @ 150 DPI
PAGE_W_PT, PAGE_H_PT = 612.0, 792.0
OUT = pathlib.Path(__file__).resolve().parent / "mixed_size_scan.pdf"

HEADING_FONT_PX = 96             # bold heading  (~3.2x the body)
BODY_FONT_PX = 30                # body lines

ARIAL_BOLD = r"C:\Windows\Fonts\arialbd.ttf"
ARIAL = r"C:\Windows\Fonts\arial.ttf"

LEFT_MARGIN_PX = 120
HEADING_TOP_PX = 110
BODY_TOP_PX = 420
BODY_PITCH_PX = 50

HEADING_TEXT = "ANNUAL REPORT"
BODY_LINES = [
    "Section one summarizes the operating results for the fiscal year.",
    "Revenue grew across every regional segment during the period.",
    "Operating margins improved on disciplined cost management.",
    "The board recommends a dividend consistent with prior guidance.",
]


def _render_scan_png() -> bytes:
    """Render the visible 'scanned' page bitmap to PNG bytes."""
    img = Image.new("RGB", (W_PX, H_PX), "white")
    draw = ImageDraw.Draw(img)
    heading_font = ImageFont.truetype(ARIAL_BOLD, HEADING_FONT_PX)
    body_font = ImageFont.truetype(ARIAL, BODY_FONT_PX)

    draw.text((LEFT_MARGIN_PX, HEADING_TOP_PX), HEADING_TEXT,
              font=heading_font, fill="black")
    y = BODY_TOP_PX
    for line in BODY_LINES:
        draw.text((LEFT_MARGIN_PX, y), line, font=body_font, fill="black")
        y += BODY_PITCH_PX

    buf = io.BytesIO()
    img.save(buf, "PNG", dpi=(DPI, DPI))
    return buf.getvalue()


def build() -> None:
    png = _render_scan_png()

    doc = fitz.open()
    page = doc.new_page(width=PAGE_W_PT, height=PAGE_H_PT)

    # Full-page raster scan => the page is image-dominated.
    page.insert_image(fitz.Rect(0, 0, PAGE_W_PT, PAGE_H_PT), stream=png)

    # Invisible OCR-style text layer (render_mode 3) matching the bitmap, so
    # hasTextContent() is true and extractText() yields clickable spans. Glyph
    # ascent ~= 0.8 * fontsize; baseline = top + ascent.
    x_pt = LEFT_MARGIN_PX * PT_PER_PX
    head_fs = HEADING_FONT_PX * PT_PER_PX          # ~46.1 pt
    head_baseline = (HEADING_TOP_PX * PT_PER_PX) + 0.8 * head_fs
    page.insert_text((x_pt, head_baseline), HEADING_TEXT,
                     fontsize=head_fs, fontname="helv", render_mode=3)

    body_fs = BODY_FONT_PX * PT_PER_PX             # ~14.4 pt
    for i, line in enumerate(BODY_LINES):
        top_px = BODY_TOP_PX + i * BODY_PITCH_PX
        baseline = (top_px * PT_PER_PX) + 0.8 * body_fs
        page.insert_text((x_pt, baseline), line,
                         fontsize=body_fs, fontname="helv", render_mode=3)

    doc.save(str(OUT), deflate=True, garbage=4)
    doc.close()

    print(f"wrote {OUT}  ({OUT.stat().st_size} bytes)")
    print(f"page = {PAGE_W_PT:.0f} x {PAGE_H_PT:.0f} pt; "
          f"heading baseline {head_baseline:.1f} pt @ {head_fs:.1f} pt, "
          f"body line2 baseline {(BODY_TOP_PX + 50) * PT_PER_PX + 0.8 * body_fs:.1f} pt")

    # Self-check: the embedded text layer must be extractable (hasTextContent).
    chk = fitz.open(str(OUT))
    txt = chk.load_page(0).get_text("text")
    chk.close()
    assert "ANNUAL REPORT" in txt, "heading text not extractable from fixture"
    assert "dividend" in txt, "body text not extractable from fixture"
    print("self-check: text layer extractable OK")


if __name__ == "__main__":
    build()
