# -*- coding: utf-8 -*-
"""End-to-end: drag a region on a scanned page and get the OCR editor.

This is the flow a user performs on a scan with no text layer -- press
"Edit Text", drag a region, read the recognised text -- and it had NO
automated coverage. It was reported broken from the field, and the first
several attempts to reproduce it in a test all failed for reasons that turned
out to have nothing to do with the product.

WHAT MADE THIS HARD, so the next person does not repeat it
----------------------------------------------------------
Three plausible causes were investigated and each was DISPROVED by experiment:

1. The RDP session was disconnected during the failing runs (it genuinely was:
   disconnected 01:15:59, reconnected 13:29:29, and every attempt fell inside
   that window). Re-running while connected, with the cursor position read
   back to prove the input landed, still failed. Not the cause.

2. The input API. ``win32_input.send_drag`` -- which exists specifically to
   cross Qt6's event-spontaneity barrier -- behaved identically to PyAutoGUI.
   Not the cause.

3. The drag missing the page. This one was REAL but not the whole story: the
   page does not fill the canvas, and both a screenshot measurement (off by
   ~145px, a DPI mismatch against the UIA rect) and a fraction-of-the-canvas
   guess put the press at PDF x = -30.7, i.e. off the left edge. Hence
   ``page_rect_on_canvas`` below -- the test ASKS the app where the page is
   rather than guessing, via a seam that calls the same getPageScreenRect()
   the canvas itself uses.

The actual blocker was the last one standing: **pywinauto's UIA backend cannot
see a modal QDialog::exec() window.** Instrumenting the app proved the dialog
was open for four minutes while a 240 s UIA poll reported nothing, closing only
when teardown dismissed it. tests/helpers/modal_dialog.py replaces that with
raw Win32 EnumWindows -- which finds it in under a second.

So the product was correct throughout; the harness could not observe it.

MUTATION EVIDENCE, including one gap stated rather than papered over
--------------------------------------------------------------------
* Replacing the Win32 detection with the old UIA polling makes this test FAIL
  ("neither the editor nor the failure dialog appeared within 240s"). The
  modal_dialog helper is load-bearing and proved so.

* Replacing ``page_rect_on_canvas`` with the old fraction-of-the-canvas guess
  makes ``test_region_ocr_reads_the_block_you_selected`` FAIL. Getting there
  took two failed attempts at the FIXTURE before the real problem turned out
  to be the ORACLE, and both dead ends are recorded because the reasons are
  the useful part:

    - A physically smaller page does NOT help. PDFE FITS THE PAGE TO THE
      VIEWPORT: measured, a 180x240pt page and a 612x792pt page both render at
      ~58-60% of canvas width. Page size cannot change on-canvas coverage.
    - Zooming to 25% does not help either. Both the guess and the correct
      computation are CENTRED, and TreRegionTool takes the bounding rect of
      the drag -- so an over-large region still fully covers a centred page,
      OCR still succeeds, and the editor still opens.

  What both attempts shared is the assumption that the fixture was at fault.
  It was not. "A dialog opened with text in it" is insensitive to WHERE the
  region was, so NO fixture and NO zoom could ever make it discriminate: a
  displaced region still recognises *something*, and the oracle cannot tell
  the difference between something and the right thing.

  The fix is an oracle that reads WHAT was recognised. ``scan_two_blocks.pdf``
  carries two widely separated blocks -- ALPHA BRAVO in the upper third,
  CHARLIE DELTA in the lower third -- and the test selects only the upper one
  and asserts the recognised text contains ALPHA/BRAVO and NOT CHARLIE/DELTA.
  That is sensitive to region POSITION, which is exactly what the seam is
  responsible for.

  Reading text out of a modal needs a seam of its own: the dialog-introspection
  registry records which widgets EXIST, not what they SAY. TestDialogSeam.h's
  dialog-child pipe gained a ``text:<objectName>`` command for this, which
  works while the modal's exec() loop is running -- exactly where UIA cannot
  follow.
"""
import os
import sys
import time

import pytest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from tests.helpers.pdfe_session import PDFESession, list_pdfe_pids  # noqa: E402
from tests.helpers.qinvoke import (  # noqa: E402
    activate_tool_by_name, _write_to_pipe, _find_pipe, dialog_widget_text,
)
from tests.helpers.modal_dialog import wait_for_window, close_window  # noqa: E402
from tests.helpers.canvas_geometry import (  # noqa: E402
    canvas_rect, page_rect_on_canvas,
)
from tests.helpers.win32_input import send_drag  # noqa: E402

FIXTURES = os.path.join(os.path.dirname(__file__), "fixtures")
SCAN_PDF = os.path.join(FIXTURES, "test_scanned.pdf")
# A page that occupies only a SMALL fraction of the canvas. test_scanned.pdf is
# US Letter and fills most of it, so a careless canvas-fraction guess still
# lands on the page and the page-rect seam is not discriminated. At 180x240pt
# this one leaves wide margins, so a guess lands in them.
SMALL_SCAN_PDF = os.path.join(FIXTURES, "scan_small_page.pdf")

TRE_DIALOG_TITLE = "Scanned Text Editor"
TRE_FAILURE_TITLE = "Text Recognition"


@pytest.mark.timeout(900)
@pytest.mark.parametrize("fixture_name", ["test_scanned.pdf",
                                          "scan_small_page.pdf"])
def test_region_drag_on_a_scan_opens_the_ocr_editor(fixture_name):
    """Edit Text -> drag a region -> the Scanned Text Editor opens.

    Parametrised over a Letter-sized scan and a SMALL-page scan. The small one
    is what makes page_rect_on_canvas load-bearing: on Letter the page fills
    enough of the canvas that even a careless guess lands on it, so that case
    alone could not tell a correct helper from a broken one.
    """
    SCAN_PDF = os.path.join(FIXTURES, fixture_name)
    if not os.path.exists(SCAN_PDF):
        pytest.fail("missing committed fixture: %s" % SCAN_PDF)
    if list_pdfe_pids():
        pytest.fail("pdfe.exe already running; refusing to disturb it")

    with PDFESession(pdf_path=SCAN_PDF, kill_existing=False) as s:
        time.sleep(5)

        # Zoom out on the small-page case so the page covers only a fraction
        # of the canvas. PDFE FITS THE PAGE TO THE VIEWPORT -- measured, not
        # assumed: a 180x240pt page and a 612x792pt page both render at
        # ~58-60% of canvas width, so page SIZE cannot produce a page that a
        # canvas-fraction guess would miss. Zoom is the only lever that can,
        # and that is what makes page_rect_on_canvas load-bearing rather than
        # merely correct.
        if fixture_name == "scan_small_page.pdf":
            assert _write_to_pipe(s, "testSetZoomPipe", "25"), \
                "zoom seam unavailable"
            time.sleep(2.5)

        assert activate_tool_by_name(s, "textEditToolAction"), \
            "could not write the tool-activate pipe"
        # Activation OCRs the whole page on a scan; that is not instant.
        time.sleep(20)

        canvas = canvas_rect(s)
        assert canvas is not None, "pdfCanvas not found in the UIA tree"

        page = page_rect_on_canvas(s, 0)
        assert page is not None, \
            "testPageRectQueryPipe unavailable -- the test must not guess " \
            "where the page is; that is what broke earlier attempts"
        px, py, pw, ph = page
        assert pw > 0 and ph > 0, "page rect is degenerate: %r" % (page,)

        # Well inside the page, over the upper text block. Insetting by a
        # fifth keeps the press off the edge even if layout shifts a little.
        sx = canvas.left + px + int(pw * 0.20)
        sy = canvas.top + py + int(ph * 0.15)
        ex = canvas.left + px + int(pw * 0.80)
        ey = canvas.top + py + int(ph * 0.35)

        s.window.set_focus()
        time.sleep(1)
        send_drag(sx, sy, ex, ey, duration=0.6)

        # Win32, not UIA: UIA cannot see a modal QDialog (module docstring).
        hwnd = wait_for_window(s.pid, TRE_DIALOG_TITLE, timeout=240)
        if hwnd is None:
            failed = wait_for_window(s.pid, TRE_FAILURE_TITLE, timeout=5)
            if failed is not None:
                close_window(failed)
                pytest.fail(
                    "OCR reported no text in the region: PDFE showed the "
                    "'%s' failure dialog" % TRE_FAILURE_TITLE)
            pytest.fail(
                "neither the editor nor the failure dialog appeared within "
                "240s after the region drag")

        try:
            # Getting here means the whole chain worked: the drag reached
            # TreRegionTool, its regionSelected callback fired,
            # RegionReconstructor::analyze() succeeded, and the editor opened.
            assert hwnd != 0
        finally:
            close_window(hwnd)
            time.sleep(2)


@pytest.mark.timeout(900)
def test_region_ocr_reads_the_block_you_selected():
    """Select the UPPER text block; the OCR result must be the UPPER block.

    THIS is the test that makes ``page_rect_on_canvas`` load-bearing. The
    other test above proves the chain runs end to end, but it asserts only
    that a dialog appeared -- which a displaced region also produces, because
    a displaced region still recognises *something*.

    ``scan_two_blocks.pdf`` carries ALPHA BRAVO in the upper third and
    CHARLIE DELTA in the lower third, separated by an empty 40% gutter. If the
    page-to-canvas mapping is right the drag lands on ALPHA BRAVO; if it is a
    canvas-fraction guess the drag is displaced and reads the wrong block,
    both blocks, or nothing. The assertion is therefore sensitive to region
    POSITION, which is the property the seam is responsible for.

    Both directions are asserted deliberately. Requiring ALPHA alone would
    still pass if the region grew to cover the whole page, and that is the
    exact over-large-region failure the previous oracle could not see.
    """
    two_block = os.path.join(FIXTURES, "scan_two_blocks.pdf")
    if not os.path.exists(two_block):
        pytest.fail(
            "missing committed fixture: %s -- regenerate with "
            "python tests/fixtures/make_two_block_scan.py" % two_block)
    if list_pdfe_pids():
        pytest.fail("pdfe.exe already running; refusing to disturb it")

    with PDFESession(pdf_path=two_block, kill_existing=False) as s:
        time.sleep(5)

        # BOTH the two-block fixture AND the zoom-out are required. Measured on
        # this fixture, expressing the drag as a fraction of the REAL page:
        #
        #   zoom 100%: canvas 1048x885, page (209,10,629,815)
        #              real  -> page y 0.100..0.400   (upper block: HIT)
        #              guess -> page y 0.096..0.422   (upper block: HIT)
        #   zoom  25%: canvas 1048x885, page (447,10,153,198)
        #              real  -> page y 0.100..0.400   (upper block: HIT)
        #              guess -> page y 0.396..1.737   (LOWER block, runs off page)
        #
        # At 100% the page nearly fills the canvas VERTICALLY (10..825 of 885),
        # so a canvas-as-page guess lands in almost the same place and the test
        # cannot tell them apart -- verified by actually running the mutant,
        # which PASSED. Zooming out shrinks the page within the canvas and opens
        # the gap. This is why the two earlier attempts failed: the fixture
        # alone gives a position-sensitive oracle with nothing to detect, and
        # the zoom alone gives a discrepancy with an oracle blind to it.
        assert _write_to_pipe(s, "testSetZoomPipe", "25"), \
            "zoom seam unavailable -- without it this test cannot discriminate"
        time.sleep(2.5)

        assert activate_tool_by_name(s, "textEditToolAction"), \
            "could not write the tool-activate pipe"
        time.sleep(20)   # activation OCRs the whole page on a scan

        canvas = canvas_rect(s)
        assert canvas is not None, "pdfCanvas not found in the UIA tree"

        page = page_rect_on_canvas(s, 0)
        assert page is not None, \
            "testPageRectQueryPipe unavailable -- the test must not guess " \
            "where the page is; that is what broke earlier attempts"
        px, py, pw, ph = page
        assert pw > 0 and ph > 0, "page rect is degenerate: %r" % (page,)

        # The upper block sits at 15-30% of page height. Drag 10%..40%: clear
        # of the block on both sides, and still ending well above the 70%
        # gutter edge where CHARLIE DELTA begins.
        sx = canvas.left + px + int(pw * 0.05)
        sy = canvas.top + py + int(ph * 0.10)
        ex = canvas.left + px + int(pw * 0.95)
        ey = canvas.top + py + int(ph * 0.40)

        s.window.set_focus()
        time.sleep(1)
        send_drag(sx, sy, ex, ey, duration=0.6)

        hwnd = wait_for_window(s.pid, TRE_DIALOG_TITLE, timeout=240)
        if hwnd is None:
            failed = wait_for_window(s.pid, TRE_FAILURE_TITLE, timeout=5)
            if failed is not None:
                close_window(failed)
                pytest.fail(
                    "OCR reported no text in the upper-block region: PDFE "
                    "showed the '%s' failure dialog. A correctly-mapped "
                    "region covers ALPHA BRAVO, so this is itself a "
                    "page-rect failure." % TRE_FAILURE_TITLE)
            pytest.fail(
                "neither the editor nor the failure dialog appeared within "
                "240s after the region drag")

        try:
            recognised = dialog_widget_text(s, "treTextEdit")
            upper = recognised.upper()
            assert ("ALPHA" in upper) or ("BRAVO" in upper), (
                "region did not read the UPPER block. Recognised %r, expected "
                "ALPHA BRAVO. The drag was mapped onto the wrong part of the "
                "page -- this is what a canvas-fraction guess does."
                % recognised)
            assert "CHARLIE" not in upper and "DELTA" not in upper, (
                "region ALSO read the LOWER block. Recognised %r. The selected "
                "region covered more of the page than it should have; an "
                "over-large region is the failure the old dialog-opened "
                "oracle could not detect." % recognised)
        finally:
            close_window(hwnd)
            time.sleep(2)


@pytest.mark.timeout(300)
def test_edit_text_on_a_scan_routes_to_the_region_tool():
    """A page with no text layer must get the REGION tool, not the span tool.

    Guards the routing decision in MainWindow::activateTextEditForPage, which
    keys on hasTextContent(). If that ever flips, the region drag above stops
    being reachable at all and its failure would look like an OCR problem
    rather than a routing one.
    """
    if not os.path.exists(SCAN_PDF):
        pytest.fail("missing committed fixture: %s" % SCAN_PDF)
    if list_pdfe_pids():
        pytest.fail("pdfe.exe already running; refusing to disturb it")

    with PDFESession(pdf_path=SCAN_PDF, kill_existing=False) as s:
        time.sleep(5)
        assert activate_tool_by_name(s, "textEditToolAction")
        time.sleep(20)

        assert _write_to_pipe(s, "testActiveToolQueryPipe", "?")
        time.sleep(1.5)
        el = _find_pipe(s, "testActiveToolResultPipe")
        assert el is not None, "active-tool result pipe not found"
        tool = (el.get_value() or "").strip()
        assert tool == "treToolAction", \
            "expected the region tool on a scanned page, got %r" % tool
