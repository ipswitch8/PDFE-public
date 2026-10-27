# -*- coding: utf-8 -*-
"""Generate scan_two_blocks.pdf — a scan whose OCR result reveals WHERE you looked.

WHY THIS FIXTURE EXISTS (read before changing it)
--------------------------------------------------
The region-OCR UI test could not discriminate `page_rect_on_canvas` from a
crude fraction-of-the-canvas guess. Two earlier attempts to fix that by
changing the FIXTURE both failed, and the reasons are the useful part:

  * A physically smaller page does not help. PDFE FITS THE PAGE TO THE
    VIEWPORT — measured, a 180x240pt page and a 612x792pt page both render at
    ~58-60% of canvas width. Page size cannot change on-canvas coverage.
  * Zooming out does not help either. Both the guess and the correct
    computation are CENTRED, and the region tool takes the drag's bounding
    rect, so an over-large region still fully covers a centred page.

The conclusion was about the ORACLE, not the fixture: "a dialog opened with
text in it" is insensitive to WHERE the region was, so it can never separate a
correct region from a displaced or over-large one — both recognise *something*.

So this page carries TWO widely separated, unmistakably different text blocks:

    upper third   ->  ALPHA BRAVO
    lower third   ->  CHARLIE DELTA

with a deliberately empty middle band between them. A test selects only the
UPPER block and asserts the recognised text contains ALPHA/BRAVO and does NOT
contain CHARLIE/DELTA. That assertion is sensitive to region POSITION, which is
exactly the property `page_rect_on_canvas` is responsible for:

  * correct mapping  -> the drag lands on the upper third -> ALPHA BRAVO
  * canvas-fraction guess -> the drag is displaced (and on the real reported
    manual landed at PDF x = -30.7, off the page entirely) -> either the wrong
    block, both blocks, or no text at all

The words are chosen to be OCR-friendly and mutually non-confusable: all caps,
common letterforms, no shared prefixes, and no word that is a substring of the
other pair. "ALPHA" vs "CHARLIE" cannot be confused by a single-character OCR
slip in the way that, say, "TOP" vs "TAP" could.

The bands are placed at 15-30% and 70-85% of page height, leaving a 40% empty
gutter. That gutter is the tolerance: the drag would have to be displaced by
more than a third of the page height before it started reading the wrong block,
which is far larger than any plausible rounding difference and far smaller than
the displacement a canvas-fraction guess actually produces.

Regenerate with:

    python tests/fixtures/make_two_block_scan.py

Image-only by construction: the text is RASTERISED, never drawn as PDF text
operators, so the page has no text layer and the app routes it to OCR. If it
had a text layer the region tool would not even be selected (see
test_edit_text_on_a_scan_routes_to_the_region_tool).
"""
import os

import fitz

OUT = os.path.dirname(os.path.abspath(__file__))

PAGE_W, PAGE_H = 612, 792

UPPER_TEXT = "ALPHA BRAVO"
LOWER_TEXT = "CHARLIE DELTA"

# Fractions of page height. The empty middle band is the tolerance; see above.
UPPER_BAND = (0.15, 0.30)
LOWER_BAND = (0.70, 0.85)


def _render_text_band(text, width_px, height_px):
    """Rasterise `text` centred on a white band, returned as PNG bytes.

    Rendered via a throwaway one-page PDF rather than a font library so the
    result depends only on PyMuPDF, which the other fixture generators here
    already require. The intermediate page IS drawn with text operators, but it
    is then rasterised to a pixmap, so what lands in the fixture is pixels.
    """
    tmp = fitz.open()
    page = tmp.new_page(width=width_px, height=height_px)
    # Large, plain, high-contrast: this has to survive OCR, not look good.
    fontsize = int(height_px * 0.45)
    tw = fitz.get_text_length(text, fontname="helv", fontsize=fontsize)
    while tw > width_px * 0.92 and fontsize > 8:
        fontsize -= 2
        tw = fitz.get_text_length(text, fontname="helv", fontsize=fontsize)
    x = (width_px - tw) / 2.0
    y = height_px / 2.0 + fontsize * 0.35
    page.insert_text((x, y), text, fontname="helv", fontsize=fontsize,
                     color=(0, 0, 0))
    # 2x supersample: OCR accuracy is sensitive to stroke thickness at small
    # raster sizes, and a fixture that only sometimes OCRs is worse than none.
    pix = page.get_pixmap(matrix=fitz.Matrix(2, 2))
    data = pix.tobytes("png")
    tmp.close()
    return data


def make_two_block_scan(path):
    doc = fitz.open()
    page = doc.new_page(width=PAGE_W, height=PAGE_H)

    # White background across the whole page: a scan is a photograph of paper,
    # and a transparent page would leave the canvas colour showing through and
    # change what OCR sees at the edges.
    page.draw_rect(fitz.Rect(0, 0, PAGE_W, PAGE_H), color=None,
                   fill=(1, 1, 1))

    margin = PAGE_W * 0.10
    band_w = PAGE_W - 2 * margin

    for text, (top_f, bot_f) in ((UPPER_TEXT, UPPER_BAND),
                                 (LOWER_TEXT, LOWER_BAND)):
        y0 = PAGE_H * top_f
        y1 = PAGE_H * bot_f
        rect = fitz.Rect(margin, y0, margin + band_w, y1)
        png = _render_text_band(text, int(band_w), int(y1 - y0))
        page.insert_image(rect, stream=png)

    doc.save(path, deflate=True)
    doc.close()


def report(path):
    """Confirm the fixture really is image-only and the bands are where we said."""
    doc = fitz.open(path)
    page = doc[0]
    text_layer = page.get_text().strip()
    images = page.get_images(full=True)
    print("  %s" % os.path.basename(path))
    print("    page          : %.0f x %.0f pt" % (page.rect.width,
                                                  page.rect.height))
    print("    images        : %d" % len(images))
    print("    text layer    : %r  (MUST be empty -- otherwise the app will "
          "route to the span tool, not OCR)" % text_layer)
    for b in page.get_text("dict", flags=fitz.TEXTFLAGS_DICT
                           | fitz.TEXT_PRESERVE_IMAGES)["blocks"]:
        if b["type"] == 1:
            x0, y0, x1, y1 = b["bbox"]
            print("    image block   : y %.0f..%.0f  (%.2f..%.2f of page "
                  "height)" % (y0, y1, y0 / page.rect.height,
                               y1 / page.rect.height))
    doc.close()


if __name__ == "__main__":
    out = os.path.join(OUT, "scan_two_blocks.pdf")
    make_two_block_scan(out)
    print("wrote:")
    report(out)
