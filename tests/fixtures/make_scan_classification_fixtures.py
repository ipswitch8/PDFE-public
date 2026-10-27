# -*- coding: utf-8 -*-
"""Generate the two fixtures that pin Page::isImageDominated()'s failure modes.

Committed as a GENERATOR, not just as two opaque PDFs, so the shape each file is
supposed to have is stated rather than having to be reverse-engineered from
bytes later. Re-run to regenerate:

    python tests/fixtures/make_scan_classification_fixtures.py

Both files reproduce defects measured on a real 16-page scanned manual; see
docs/audit/field-report-2026-08-28-scanned-manual.md.

  scan_offpage_image.pdf
      No text. One normally-placed image plus one placed ENTIRELY OFF the right
      edge of the page. MuPDF clips the far edge of such a block to the page
      boundary but leaves the near edge outside it, yielding an INVERTED bbox
      (x0 > x1). Measured on the reported manual, page 2:

          page rect x1 = 297.6      block bbox = (336.0, 242.8, 297.6, 356.2)

      The old code summed SIGNED areas, so that block SUBTRACTED 38.4 x 113.4
      of coverage. Enough of them and the total goes NEGATIVE -- three pages of
      the manual did -- at which point no amount of real image content can pass
      a `> 0.5` ratio test.

      NOTE, because it is the thing that was assumed and was wrong: this is NOT
      caused by a mirrored/negative-determinant CTM. MuPDF normalises those. A
      synthetic mirror was built first and reproduced NOTHING; the mechanism is
      off-page clipping.

  scan_low_coverage.pdf
      No text at all, one upright image covering well under half the page. A
      page with nothing on it but pixels is a scan whatever fraction they cover;
      the old `imageRatio > 0.5` alone refused to classify it, so OCR never ran.

Deliberately NOT derived from the reported manual: that file is third-party
material and does not belong in this repository. These are synthetic and carry
the same two structural properties.
"""
import os

import fitz

OUT = os.path.dirname(os.path.abspath(__file__))


def _ink(width=400, height=200):
    """A small opaque raster standing in for scanned content."""
    pix = fitz.Pixmap(fitz.csRGB, fitz.IRect(0, 0, width, height))
    pix.set_rect(pix.irect, (255, 255, 255))
    pix.set_rect(fitz.IRect(20, 20, width - 20, height - 20), (10, 10, 10))
    return pix.tobytes("png")


def make_offpage(path):
    doc = fitz.open()
    page = doc.new_page(width=612, height=792)
    img = _ink()
    # Upright, on-page.
    page.insert_image(fitz.Rect(60, 80, 400, 250), stream=img)
    # Placed wholly beyond the right edge (page x1 = 612). insert_image will not
    # position outside the page, so the placement is written as a raw CTM.
    page.insert_image(fitz.Rect(60, 300, 400, 470), stream=img, overlay=True)
    xrefs = page.get_contents()
    mirror_cm = "\nq\n340 0 0 170 700 300 cm\n/fzImg1 Do\nQ\n"
    doc.update_stream(xrefs[-1], mirror_cm.encode("latin-1"))
    tmp = path + ".tmp"
    doc.save(tmp, deflate=True)
    doc.close()
    if os.path.exists(path):
        os.remove(path)
    os.rename(tmp, path)


def make_low_coverage(path):
    doc = fitz.open()
    page = doc.new_page(width=612, height=792)
    # ~28% of the page: unambiguously below the old 0.5 threshold.
    page.insert_image(fitz.Rect(80, 120, 530, 420), stream=_ink())
    doc.save(path, deflate=True)
    doc.close()


def make_signed_area_straddle(path):
    """A page whose verdict depends ONLY on the area-sign half of the fix.

    The pure-scan branch (`!hasText && ratio > 0.05`) must not be able to
    decide this page, or the test would pass for the wrong reason -- which is
    exactly how an earlier candidate for this slot passed against PRE-FIX code
    and had to be deleted. So the page carries REAL TEXT: hasText is true, the
    pure-scan branch is off, and only `imageRatio > 0.5` can decide it.

    Then the two ratios are made to straddle that threshold:

        signed  (pre-fix: inverted bbox SUBTRACTS)  ->  below 0.5
        clamped (post-fix: normalised + clamped)    ->  above 0.5

    One large on-page image supplies the coverage; one image placed beyond the
    right edge supplies the inversion that eats it. Sizes are deliberately
    generous rather than marginal, because the exact per-block bbox MuPDF
    reports is not worth predicting from Python -- the earlier attempt failed
    precisely by trusting such a model.
    """
    doc = fitz.open()
    page = doc.new_page(width=612, height=792)
    img = _ink(600, 780)  # page-ish aspect: insert_image fits, so 3:2 art
                          # would only cover 612x408 = 0.515 of the page

    # Real text: forces hasText true, so the pure-scan branch cannot decide.
    page.insert_text((40, 40), "AREA SIGN STRADDLE FIXTURE", fontsize=13)

    # Full-page: comfortably above the 0.5 gate on its own.
    page.insert_image(fitz.Rect(0, 0, 612, 792), stream=img)

    # Off the right edge: MuPDF clips the far edge to the page boundary and
    # leaves the near edge outside, producing an INVERTED bbox whose signed
    # area is negative -- enough to drag the coverage below 0.5.
    #
    # APPEND to the existing content stream, do not replace it: replacing
    # destroys the on-page image drawn above (and its /fzImg0 resource), which
    # is how the first attempt at this fixture produced coverage 0.000 and a
    # "cannot find XObject resource" error. The resource name is read from the
    # page rather than guessed -- a single insert_image yields fzImg0, not
    # fzImg1.
    name = page.get_images(full=True)[0][7]
    xrefs = page.get_contents()
    existing = doc.xref_stream(xrefs[-1]) or b""
    # ONE off-page block subtracts only ~0.07: MuPDF bounds the clipped bbox,
    # so enlarging the CTM changes nothing (measured -- 900x760 produced
    # exactly the same 0.069 as 420x380). The reported manual reached a
    # NEGATIVE total by carrying SIX such blocks on one page. Emit several for
    # the same reason: the subtraction accumulates per BLOCK, not per area.
    one = "\nq\n420 380 0 0 700 200 cm\n/%s Do\nQ\n" % name
    offpage = one * 8
    doc.update_stream(xrefs[-1], existing + offpage.encode("latin-1"))

    tmp = path + ".tmp"
    doc.save(tmp, deflate=True)
    doc.close()
    if os.path.exists(path):
        os.remove(path)
    os.rename(tmp, path)


def report(path):
    """Print what the OLD signed-area rule would have computed."""
    doc = fitz.open(path)
    page = doc[0]
    pa = page.rect.width * page.rect.height
    d = page.get_text("dict", flags=fitz.TEXTFLAGS_DICT | fitz.TEXT_PRESERVE_IMAGES)
    signed = clamped = 0.0
    inverted = 0
    has_text = False
    for b in d["blocks"]:
        if b["type"] != 1:
            for line in b.get("lines", []):
                for sp in line.get("spans", []):
                    if sp.get("text", "").strip():
                        has_text = True
            continue
        x0, y0, x1, y1 = b["bbox"]
        signed += (x1 - x0) * (y1 - y0)
        if x1 - x0 < 0 or y1 - y0 < 0:
            inverted += 1
        cx0, cy0 = max(min(x0, x1), page.rect.x0), max(min(y0, y1), page.rect.y0)
        cx1, cy1 = min(max(x0, x1), page.rect.x1), min(max(y0, y1), page.rect.y1)
        if cx1 > cx0 and cy1 > cy0:
            clamped += (cx1 - cx0) * (cy1 - cy0)
    print("  %-26s hasText=%-5s inverted=%d  signed=%.3f  clamped=%.3f  "
          "old_rule=%s  new_rule=%s"
          % (os.path.basename(path), has_text, inverted, signed / pa,
             clamped / pa,
             "dominated" if signed / pa > 0.5 else "NOT dominated",
             "dominated" if ((not has_text and clamped / pa > 0.05)
                             or clamped / pa > 0.5) else "NOT dominated"))
    doc.close()


if __name__ == "__main__":
    a = os.path.join(OUT, "scan_offpage_image.pdf")
    b = os.path.join(OUT, "scan_low_coverage.pdf")
    c = os.path.join(OUT, "scan_signed_area_straddle.pdf")
    make_offpage(a)
    make_low_coverage(b)
    make_signed_area_straddle(c)
    print("wrote:")
    report(a)
    report(b)
    report(c)
