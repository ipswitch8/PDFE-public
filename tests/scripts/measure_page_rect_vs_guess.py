# -*- coding: utf-8 -*-
"""Measure the REAL page-to-canvas mapping against the canvas-fraction guess.

This is the diagnostic that settled why `tests/test_ui_tre_region_ocr.py` could
not discriminate `page_rect_on_canvas` from a crude guess. It is committed
because the evidence document cites it, and cited evidence that lives in
gitignored `scratchpad/` is evidence that disappears.

WHAT IT ANSWERS. For a given zoom, it prints where a fixed drag lands
*expressed as a fraction of the real page* under two hypotheses:

  REAL  -- the page rect the app reports through testPageRectQueryPipe
  GUESS -- the pre-fix assumption that the page simply fills the canvas

If both land on the same part of the page, no test using that drag can tell a
correct mapping from a broken one. That is exactly what happens at 100% zoom,
and it is why two earlier attempts at a discriminating test failed:

    zoom=100 canvas=(w=1048 h=885) page=(209, 10, 629, 815)
       REAL  drag covers real-page y 0.100..0.400
       GUESS drag covers real-page y 0.096..0.422     <- same verdict
    zoom=25  canvas=(w=1048 h=885) page=(447, 10, 153, 198)
       REAL  drag covers real-page y 0.100..0.400
       GUESS drag covers real-page y 0.396..1.737     <- different block

At 100% the page nearly fills the canvas vertically (10..825 of 885), so the
guess is nearly right by accident. Zooming out shrinks the page within the
canvas and opens the gap.

Run from the repo root (needs a build of pdfe.exe and no other instance
running):

    python tests/scripts/measure_page_rect_vs_guess.py

See docs/audit/page-rect-seam-mutation-evidence-2026-08-31.md.
"""
import os
import sys
import time

_REPO_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, _REPO_ROOT)

from tests.helpers.pdfe_session import PDFESession, list_pdfe_pids  # noqa: E402
from tests.helpers.qinvoke import activate_tool_by_name, _write_to_pipe  # noqa: E402
from tests.helpers.canvas_geometry import (  # noqa: E402
    canvas_rect, page_rect_on_canvas,
)

FIXTURE = os.path.join(_REPO_ROOT, "tests", "fixtures", "scan_two_blocks.pdf")

# The drag the region-OCR test performs, as fractions of the assumed page box.
DRAG_TOP, DRAG_BOTTOM = 0.10, 0.40
# Where the fixture's two text blocks actually are.
UPPER_BAND = (0.15, 0.30)
LOWER_BAND = (0.70, 0.85)


def _covers(band, y0, y1):
    return not (y1 < band[0] or y0 > band[1])


def report(zoom):
    with PDFESession(pdf_path=FIXTURE, kill_existing=False) as s:
        time.sleep(5)
        if zoom:
            if not _write_to_pipe(s, "testSetZoomPipe", str(zoom)):
                raise SystemExit("zoom seam unavailable -- is this a "
                                 "PDFE_TEST_PIPES build?")
            time.sleep(2.5)
        activate_tool_by_name(s, "textEditToolAction")
        time.sleep(20)   # activation OCRs the whole page on a scan

        canvas = canvas_rect(s)
        page = page_rect_on_canvas(s, 0)
        if canvas is None or page is None:
            raise SystemExit("canvas or page rect unavailable -- the point of "
                             "this script is to avoid guessing, so it stops")
        px, py, pw, ph = page
        print("zoom=%-4s canvas=(w=%d h=%d) page=%r"
              % (zoom or 100, canvas.width(), canvas.height(), page))

        for name, (ry, rh) in (("REAL ", (py, ph)),
                               ("GUESS", (0, canvas.height()))):
            y0 = (ry + rh * DRAG_TOP - py) / ph
            y1 = (ry + rh * DRAG_BOTTOM - py) / ph
            hits = []
            if _covers(UPPER_BAND, y0, y1):
                hits.append("UPPER")
            if _covers(LOWER_BAND, y0, y1):
                hits.append("LOWER")
            print("   %s drag covers real-page y %.3f..%.3f  -> %s"
                  % (name, y0, y1, "+".join(hits) or "NEITHER"))


if __name__ == "__main__":
    if list_pdfe_pids():
        raise SystemExit("pdfe.exe already running; refusing to disturb it")
    report(None)
    time.sleep(3)
    report(25)
