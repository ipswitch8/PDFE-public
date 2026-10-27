# -*- coding: utf-8 -*-
"""Generate scan_small_page.pdf — a scan whose page is SMALL on the canvas.

WHY THIS FIXTURE EXISTS
-----------------------
`tests/helpers/canvas_geometry.point_on_page()` asks the application where a
page actually is, instead of guessing from the canvas rectangle. That fix was
correct and prevented a real failure (a canvas-fraction guess put a press at
PDF x = -30.7, off the left edge of a 16-page manual), but it was NOT
DISCRIMINATED by any test: `tests/fixtures/test_scanned.pdf` is US Letter
(612 x 792 pt) and fills most of the canvas, so reverting the helper to a
careless guess left the test passing anyway.

A fixture is only load-bearing here if the page occupies a SMALL fraction of
the canvas, so that a canvas-relative guess lands in the grey margin while a
page-relative computation lands on the page. This page is 180 x 240 pt --
under a third of Letter width -- which puts a 0.12..0.88 canvas-fraction span
outside it at ordinary zoom.

Two properties are load-bearing and must be preserved if this is regenerated:

  * NO text layer. MainWindow::activateTextEditForPage routes on
    hasTextContent(); with a text layer the document would go to the per-span
    text tool and never reach the TRE region tool the test drives.
  * The image must carry LEGIBLE text, because the test asserts the region
    OCRs to something rather than merely that a dialog appeared.

Regenerate:  python tests/fixtures/make_small_page_scan.py
"""
import os

import fitz

OUT = os.path.dirname(os.path.abspath(__file__))

PAGE_W, PAGE_H = 180.0, 240.0
# Rendered at 4x the page size so the rasterised glyphs are comfortably above
# Tesseract's resolution floor once the small page is scaled up for OCR.
SCALE = 4


def build(path):
    # Render the text ONCE on a scratch document, then re-insert the result as
    # a raster. Drawing text directly on the target page would leave a text
    # layer and defeat the fixture's purpose.
    scratch = fitz.open()
    sp = scratch.new_page(width=PAGE_W, height=PAGE_H)
    sp.draw_rect(sp.rect, color=(1, 1, 1), fill=(1, 1, 1))
    sp.insert_text((18, 60), "SMALL PAGE", fontsize=20, fontname="helv",
                   color=(0, 0, 0))
    sp.insert_text((18, 95), "SCAN FIXTURE", fontsize=20, fontname="helv",
                   color=(0, 0, 0))
    sp.insert_text((18, 130), "REGION OCR", fontsize=20, fontname="helv",
                   color=(0, 0, 0))
    pix = sp.get_pixmap(matrix=fitz.Matrix(SCALE, SCALE))
    png = pix.tobytes("png")
    scratch.close()

    doc = fitz.open()
    page = doc.new_page(width=PAGE_W, height=PAGE_H)
    page.insert_image(page.rect, stream=png)
    doc.save(path, deflate=True)
    doc.close()


def report(path):
    doc = fitz.open(path)
    p = doc[0]
    text = p.get_text().strip()
    print("  %-24s %.0f x %.0f pt  images=%d  textlen=%d  (textlen MUST be 0)"
          % (os.path.basename(path), p.rect.width, p.rect.height,
             len(p.get_images(full=False)), len(text)))
    doc.close()


if __name__ == "__main__":
    target = os.path.join(OUT, "scan_small_page.pdf")
    build(target)
    print("wrote:")
    report(target)
