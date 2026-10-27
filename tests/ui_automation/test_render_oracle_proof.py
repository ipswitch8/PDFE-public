# -*- coding: utf-8 -*-
"""Render-oracle proof cases — pipeline e7d3c2a1 Phase 2.

These tests drive the REAL pdfe.exe (Release) and assert on the *rendered
canvas pixels*, the layer ~3940 GoogleTests never touch. They exist to
reproduce four systemic-defect signatures at the render level so that the
Phase 5/6 fixes have an executable definition of "done":

* ``test_vector_text_edit_visible``      — #1: editing the big bold "Invoice"
  heading on a vector page renders blank (Tr=3 forced via the page-wide
  ImageDominated misclassification).  XFAIL until Phase 5.
* ``test_ctrl_z_reverts_canvas``         — #1: Ctrl-Z after that edit does not
  visibly restore the heading.  XFAIL until Phase 5.
* ``test_mixed_dpi_region_not_smeared``  — #2: replacing the 150-DPI region
  upsamples it to the page-max 300 DPI → blur.  XFAIL until Phase 6.
* ``test_mixed_content_scanned_region_routable`` — #4: clicking the scanned
  block routes to the per-span text tool, not the region tool, because
  ``hasTextContent()`` is a page-wide boolean.  XFAIL until Phase 6.

* ``test_oracle_self_check``             — a NON-xfail control: page navigation
  must change the canvas.  Proves the oracle itself detects real changes
  (so the xfails above are meaningful, not the oracle failing to capture).

Drive model
===========
Every action is driven through PDFE's deterministic ``PDFE_TEST_PIPES`` seams
(``testToolActivatePipe``, ``testDoubleClickByPdfCoordPipe``,
``testTextEditTypePipe``, ``testGoToPagePipe``, ``testActiveToolQueryPipe``,
``testTextEditDiagTriggerPipe``) — NOT PyAutoGUI clicks and NOT UIA menu walks
(GAP-A14).  PyAutoGUI is used only for pixel capture.  This keeps each test
deterministic enough to reach its discriminating assertion rather than erroring
in setup.

Run against Release:  PDFE_BUILD_CONFIG is forced to Release below so the
fixture launches build/vs2022/bin/Release/pdfe.exe (the binary the pipeline
targets).
"""
from __future__ import annotations

import os
import pathlib
import tempfile
import sys
import time

import pytest

# Drive the Release binary the pipeline targets. Must be set BEFORE importing
# pdfe_app_fixture (which resolves PDFE_EXE at import time).
os.environ.setdefault("PDFE_BUILD_CONFIG", "Release")
os.environ.setdefault("PDFE_TEST_PIPES", "1")

try:
    from pywinauto import Application  # noqa: F401
except ImportError:
    pytest.skip("pywinauto not installed", allow_module_level=True)
try:
    import pyautogui  # noqa: F401
except ImportError:
    pytest.skip("pyautogui not installed", allow_module_level=True)
try:
    from PIL import Image  # noqa: F401
except ImportError:
    pytest.skip("Pillow not installed", allow_module_level=True)

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.ui_automation.harness.pdfe_app_fixture import (  # noqa: E402
    launch_pdfe, kill_pdfe_processes, write_pipe, read_result_pipe, PDFE_EXE,
)
from tests.ui_automation.harness.canvas_click import (  # noqa: E402
    canvas_region, pdf_point_to_screen,
)
from tests.ui_automation.harness import render_oracle as ro  # noqa: E402
# GitHub #218: this file has its own thin drive helpers, but the
# recomposite-completion seam lives in render_drive so all three affected
# probes wait on ONE implementation -- three copies of a wait loop is how they
# drift apart and one of them quietly goes back to sleeping.
from tests.ui_automation.harness import render_drive as rd  # noqa: E402
# Phase F25 / gap WIN-45: reachability is a RECORDED NON-PASS, never a skip.
from tests.ui_automation.conftest import (  # noqa: E402
    current_test_id,
    record_matrix_result,
)

FIXTURES = REPO_ROOT / "tests" / "fixtures"
INVOICE = FIXTURES / "invoice_vector_heading.pdf"
MIXED_DPI = FIXTURES / "mixed_dpi_page.pdf"
MIXED_CONTENT = FIXTURES / "mixed_content_page.pdf"
# Self-check control: two visually very different pages (78% pixel delta),
# unlike the existing multi-page fixtures (<0.05% page-to-page).
NAV_SELFCHECK = FIXTURES / "nav_selfcheck.pdf"

# Big bold "Invoice" heading on the real invoice (Arial-Bold 30pt).
INVOICE_HEADING_BBOX = (247.0, 73.0, 350.0, 106.0)
INVOICE_HEADING_CENTER = (298.7, 89.5)

# 150-DPI region on mixed_dpi_page.pdf (rasterised text image) and its centre.
THUMB_REGION_BBOX = (80.0, 470.0, 224.0, 614.0)
THUMB_SPAN_CENTER = (135.0, 488.0)

# Scanned image block on mixed_content_page.pdf (rasterised text, no live font).
SCANNED_BLOCK_CENTER = (297.0, 435.0)

LETTER = (612.0, 792.0)


def _require_binary():
    if not PDFE_EXE.exists():
        pytest.skip(f"pdfe.exe (Release) not built at {PDFE_EXE}")


def _open(doc: pathlib.Path):
    """Launch pdfe on *doc*, maximize, settle. Returns (app, win)."""
    if not doc.exists():
        pytest.skip(f"fixture missing: {doc.name}")
    app, win = launch_pdfe(args=[str(doc)])
    try:
        win.maximize()
    except Exception:  # noqa: BLE001
        pass
    time.sleep(2.0)
    if canvas_region(win) is None:
        try:
            app.kill()
        except Exception:  # noqa: BLE001
            pass
        kill_pdfe_processes()
        # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
        _reach = (
            "PdfCanvas absent from UIA tree"
        )
        record_matrix_result(current_test_id(), "blocked", _reach)
        pytest.fail(
            f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
    return app, win


def _crop_pdf_bbox(win, img, bbox, page=LETTER, pad=8):
    """Crop *img* (a full-canvas capture) to a PDF-coordinate bbox.

    DELEGATES to the shared helper, which asks the application where the page
    is (``PdfCanvas::getPageScreenRect()`` via ``testPageRectQueryPipe``).

    This function used to be a verbatim copy of the old shared one, mapping PDF
    points through a Python re-derivation of PDFE's page centring and zoom. That
    copy landed in the GREY MARGIN: measured 2026-09-29, the crop it returned
    for the thumbnail was 196x196 px of blank page containing none of the
    region, and its "sharpness 2.18" -- cited in GitHub #262, in both probes'
    xfail reasons and in known_issues.md as evidence the recomposite changed no
    pixels -- was the contrast step between margin and paper.

    Fixing the shared helper did not fix this one, because it was a COPY. That
    is the defect shape this pipeline kept meeting: a fix applied at one door
    while the same code waits behind the next. There is now one implementation.
    """
    return rd.crop_pdf_bbox(win, img, bbox, page=page, pad=pad)


def _activate_text_edit(win):
    write_pipe(win, "testToolActivatePipe", "textEditToolAction")
    time.sleep(1.5)


def _double_click_pdf(win, x, y, page_index=0):
    write_pipe(win, "testDoubleClickByPdfCoordPipe",
               f"{page_index},{x},{y}")
    time.sleep(0.8)


def _type(win, text):
    write_pipe(win, "testTextEditTypePipe", text)
    time.sleep(1.0)


def _commit(win):
    """Commit the active text edit by switching to the select tool, which calls
    clearTextEditTool() -> deactivate() -> commitEdit() and re-renders. The
    committed re-render is where defect #1 (Tr=3-forced blank) becomes visible
    on the canvas; the live in-edit overlay shows the text correctly."""
    write_pipe(win, "testToolActivatePipe", "selectToolAction")
    time.sleep(1.5)


def _selection_diag(win):
    return read_result_pipe(win, "testTextEditDiagTriggerPipe",
                            "testTextEditDiagResultPipe", "1") or ""


def _span_hit(diag: str) -> bool:
    """True if the diag string reports an active span selection."""
    head = diag.split(";")[0].split("|")[0]
    parts = head.split(",")
    if len(parts) < 2:
        return False
    try:
        span_count = int(parts[0])
        active = int(parts[1])
    except ValueError:
        return False
    return span_count > 0 and active >= 0


def _ink_ratio(img, lum_threshold: int = 110,
               band: tuple = (0.45, 1.0)) -> float:
    """Fraction of 'ink' pixels (luminance below *lum_threshold*) in the
    vertical *band* (y0_frac, y1_frac) of *img*.

    Text glyphs are dark ink on a light background. A heading full of text has
    a high ink ratio; one whose text vanished (defect #1) collapses toward the
    background's ink ratio. The heading-bbox crop also catches the company logo,
    which sits ABOVE the "Invoice" glyphs and is CONSTANT across the edit — so we
    measure the LOWER band where the heading text lives, not the top half (which
    is logo-dominated and blind to the collapse). Verified on PrintWindow
    captures: top-half reads 0.063 before==after (no signal); the lower text
    band reads ~0.187 -> ~0.032 when the heading blanks. Pure Pillow, no numpy.
    """
    g = img.convert("L")
    w, h = g.size
    if w == 0 or h == 0:
        return 0.0
    y0 = max(0, int(h * band[0]))
    y1 = max(y0 + 1, int(h * band[1]))
    g = g.crop((0, y0, w, y1))
    data = list(g.getdata())
    if not data:
        return 0.0
    ink = sum(1 for v in data if v < lum_threshold)
    return ink / len(data)


def _sharpness(img) -> float:
    """Mean absolute Laplacian-ish edge energy of *img* (grayscale).

    A blurred/upsampled region has lower edge energy than a crisp one. Pure
    Pillow (no numpy dependency): sum of |pixel - right-neighbour| +
    |pixel - down-neighbour|, normalised by pixel count.
    """
    g = img.convert("L")
    w, h = g.size
    if w < 2 or h < 2:
        return 0.0
    px = g.load()
    total = 0
    n = 0
    # Sample every pixel for small crops; stride for large ones to stay fast.
    sx = max(1, w // 200)
    sy = max(1, h // 200)
    for y in range(0, h - 1, sy):
        for x in range(0, w - 1, sx):
            c = px[x, y]
            total += abs(c - px[x + 1, y]) + abs(c - px[x, y + 1])
            n += 1
    return (total / n) if n else 0.0


# ---------------------------------------------------------------------------
# #1 — vector text edit visibility
# ---------------------------------------------------------------------------
# Vertical band of the heading crop used for ink measurement — the LOWER band
# where the "Invoice" glyphs sit, excluding the constant company logo ABOVE it
# (the top half is logo-dominated and blind to the heading's ink collapse).
_HEAD_TEXT_BAND = (0.45, 1.0)
# A correctly-rendered edit retains >= this fraction of the heading's original
# text-band ink; the Tr=3 collapse drops it to ~17% (measured 0.187 -> 0.032).
_INK_KEEP = 0.6


@pytest.mark.timeout(120)
# Phase 5 (defects #1+#5) FIXED: applyText() now routes per-span via
# TextSpan::isOcrInvisibleLayer() instead of the page-wide ImageDominated
# verdict, so the real "Invoice" heading edits as visible vector text. The
# xfail mark is removed — this must now PASS. Deterministic oracle for the
# fix is tests/test_text_edit_span_provenance.cpp (gtest).
def test_vector_text_edit_visible():
    """Editing the bold 'Invoice' heading must leave VISIBLE text, not blank.

    Drives: activate text-edit -> double-click the heading span -> type
    'Quote' -> COMMIT (the committed re-render is where the Tr=3 blanking
    shows; the in-edit overlay renders correctly). The committed heading must
    retain most of its ink.
    """
    _require_binary()
    app, win = _open(INVOICE)
    try:
        _activate_text_edit(win)
        before = ro.capture_canvas(win)
        before_crop = _crop_pdf_bbox(win, before, INVOICE_HEADING_BBOX, pad=2)
        ink_before = _ink_ratio(before_crop, band=_HEAD_TEXT_BAND)

        _double_click_pdf(win, *INVOICE_HEADING_CENTER)
        diag = _selection_diag(win)
        _type(win, "Quote")
        _commit(win)

        after = ro.capture_canvas(win)
        after_crop = _crop_pdf_bbox(win, after, INVOICE_HEADING_BBOX, pad=2)
        ink_after = _ink_ratio(after_crop, band=_HEAD_TEXT_BAND)

        assert ink_after >= _INK_KEEP * ink_before, (
            f"heading text collapsed after edit: top-half ink "
            f"{ink_before:.3f} -> {ink_after:.3f} (kept "
            f"{ink_after / max(ink_before, 1e-9):.0%}, need "
            f">= {_INK_KEEP:.0%}); selection diag={diag!r}")
    finally:
        try:
            app.kill()
        except Exception:  # noqa: BLE001
            pass
        kill_pdfe_processes()


@pytest.mark.timeout(120)
# Phase 5 (defect #1) FIXED: EditTextCommand::undo() re-runs applyText(oldText_)
# which now routes the real heading through the visible-vector path (not Tr=3),
# so in-edit Ctrl-Z visibly restores the heading. xfail removed — must now PASS.
def test_ctrl_z_reverts_canvas():
    """Ctrl-Z after the heading edit must visibly restore the original text.

    This drives the user's exact scenario: edit while still IN edit mode, then
    undo WITHOUT first committing via a tool switch. (Undo after an explicit
    commit happens to restore correctly; the broken path is the in-edit undo,
    which re-runs applyText() invisibly.) The restored heading must regain its
    ink.
    """
    _require_binary()
    app, win = _open(INVOICE)
    try:
        _activate_text_edit(win)
        pre_edit = ro.capture_canvas(win)
        pre_crop = _crop_pdf_bbox(win, pre_edit, INVOICE_HEADING_BBOX, pad=2)
        ink_before = _ink_ratio(pre_crop, band=_HEAD_TEXT_BAND)

        _double_click_pdf(win, *INVOICE_HEADING_CENTER)
        diag = _selection_diag(win)
        _type(win, "Quote")

        # Undo via the deterministic action seam — NO commit first.
        write_pipe(win, "testToolActivatePipe", "undoAction")
        time.sleep(1.5)

        post_undo = ro.capture_canvas(win)
        post_crop = _crop_pdf_bbox(win, post_undo, INVOICE_HEADING_BBOX, pad=2)
        ink_after = _ink_ratio(post_crop, band=_HEAD_TEXT_BAND)

        assert ink_after >= _INK_KEEP * ink_before, (
            f"heading not restored after Ctrl-Z: top-half ink "
            f"{ink_before:.3f} -> {ink_after:.3f} (recovered "
            f"{ink_after / max(ink_before, 1e-9):.0%}, need "
            f">= {_INK_KEEP:.0%}); selection diag={diag!r}")
    finally:
        try:
            app.kill()
        except Exception:  # noqa: BLE001
            pass
        kill_pdfe_processes()


# ---------------------------------------------------------------------------
# #2 — mixed-DPI region not smeared
# ---------------------------------------------------------------------------
@pytest.mark.timeout(150)
# xfail removed (GitHub #262 fixed) — same state as the sibling probe in
# test_render_images.py, whose comment carries the full account of which of the
# three blockers were fixed and which merely stopped reproducing. In short: the
# write-back target was selected as the largest image XObject on the page rather
# than the one under the edit, so an edit over the 150-DPI thumbnail was
# committed into the photo beneath it and painted over. See
# docs/audit/dca/0105-a-success-reported-without-checking-the-thing-it-claims.md.
#
# Also FIXED, GitHub #266 -- and this probe no longer has an xfail marker.
# Two causes, both closed: atlas glyphs harvested from a page-wide 300-DPI
# render were composited into the 150-DPI region at native size (arriving at
# 2x), and the stored 'T' was a featureless grey bar whose peak alpha was 127 --
# isolateAlpha gives 128 to a pixel equidistant between text and background, so
# nothing in that crop was predominantly text, yet it outranked the fallbacks
# that render E, D and I correctly. See DCA 0107 and 0108.
#
# This file's own _crop_pdf_bbox carried the same margin-landing defect as the
# shared helper; it now delegates, so there is one implementation.
#
# (This comment said "STILL XFAILED ... cannot pass either way until #266 is
# fixed" after the marker had already been removed. The validator gate caught
# it. Prose describing a mechanism that has since moved is the defect this
# pipeline has spent its length finding in other people's files.)
def test_mixed_dpi_region_not_smeared():
    """Replacing the 150-DPI region must render the edit crisply (no upsample
    to the page-max 300 DPI).

    Now waits on the GitHub #218 recomposite-complete seam and asserts the
    outcome status, so the failure names a cause instead of expiring a timer.
    """
    _require_binary()
    app, win = _open(MIXED_DPI)
    try:
        _activate_text_edit(win)
        before = ro.capture_canvas(win)
        before_crop = _crop_pdf_bbox(win, before, THUMB_REGION_BBOX, pad=2)
        sharp_before = _sharpness(before_crop)
        assert sharp_before > 0.0, (
            "baseline sharpness is 0 -- the crop captured nothing, so the "
            "ratio comparison below would compare two blank images")

        # Edit the OCR'd text over the 150-DPI region → triggers ImageReplacer,
        # which (defect #2) re-renders the region at the page-max 300 DPI,
        # upsampling the 150-DPI source and blurring it.
        mark = rd.recomposite_mark()
        _double_click_pdf(win, *THUMB_SPAN_CENTER)
        diag = _selection_diag(win)
        _type(win, "EDIT")
        write_pipe(win, "testToolActivatePipe", "selectToolAction")
        status, page, seq, detail = rd.wait_recomposite(win, mark)
        assert status == "RECOMPOSITE_DONE", (
            f"recomposite did not succeed: {status} page={page} seq={seq} "
            f"detail={detail!r}; selection diag was {diag!r}")
        time.sleep(0.8)  # Qt repaint is async even once the recomposite lands.

        after = ro.capture_canvas(win)
        after_crop = _crop_pdf_bbox(win, after, THUMB_REGION_BBOX, pad=2)
        sharp_after = _sharpness(after_crop)

        # The edit must take effect, and the region must not have been
        # re-rendered at the page-maximum DPI.
        #
        # The old `sharp_after >= 0.85 * sharp_before` half is gone, for the
        # reasons set out at length in the sibling probe: that ratio was never
        # once evaluated against this region (the crop above was returning grey
        # margin), and it is confounded by TEXT LENGTH -- measured 2026-09-30,
        # replacing the 9-character "THUMBNAIL" with a 9-character word gives
        # 0.809 and with a 4-character word 0.605, a 0.2 swing from word length
        # alone. The property it stood in for is now measured exactly, below.
        changed = ro.images_differ(before_crop, after_crop, threshold=0.01)
        assert changed, (
            f"150-DPI region did not change: changed={changed}, "
            f"sharpness {sharp_before:.2f} -> {sharp_after:.2f}; diag={diag!r}")

        # The named property, measured on what was actually WRITTEN rather than
        # inferred from edge energy in a screenshot.
        saved = pathlib.Path(tempfile.gettempdir()) / "pdfe_mixed_dpi_oracle.pdf"
        assert rd.save_as(win, saved), (
            "testSaveAsPipe produced no file, so the written result cannot be "
            "inspected")
        measured = rd.image_dpi_under_region(saved, THUMB_REGION_BBOX)
        assert measured is not None, (
            "no image is drawn under the thumbnail region in the saved "
            "document -- the recomposite removed it rather than replacing it")
        dpi, px_w, width_pt = measured
        assert 140.0 <= dpi <= 160.0, (
            f"the 150-DPI region was re-rendered at {dpi:.1f} DPI "
            f"({px_w}px across {width_pt:.1f}pt) -- defect #2, the page-maximum "
            f"DPI applied to a region with its own. Observed sharpness "
            f"{sharp_before:.2f} -> {sharp_after:.2f}.")
    finally:
        try:
            app.kill()
        except Exception:  # noqa: BLE001
            pass
        kill_pdfe_processes()


# ---------------------------------------------------------------------------
# #4 — mixed-content scanned region routable to the region tool
# ---------------------------------------------------------------------------
@pytest.mark.timeout(120)
# Phase 6 (defect #4) FIXED: PdfCanvas routes a left-click that hits an image
# block with no live text span (Page::hasImageContentAt + TextEditTool::
# spanIndexAtPoint) to the region tool via regionToolRequested, instead of the
# page-wide hasTextContent() forcing the whole mixed page to the span tool.
# xfail removed — clicking the scanned block now activates treToolAction.
def test_mixed_content_scanned_region_routable():
    """Clicking the scanned block must activate the region tool, not span tool."""
    _require_binary()
    app, win = _open(MIXED_CONTENT)
    try:
        # Enter text-edit mode. Because the page also has live vector text,
        # the page-wide hasTextContent() routes the WHOLE page to the span
        # tool (defect #4).
        _activate_text_edit(win)
        # Click the scanned block specifically.
        write_pipe(win, "testClickByPdfCoordPipe",
                   f"0,{SCANNED_BLOCK_CENTER[0]},{SCANNED_BLOCK_CENTER[1]}")
        time.sleep(0.8)

        active = read_result_pipe(win, "testActiveToolQueryPipe",
                                  "testActiveToolResultPipe", "1")
        assert active == "treToolAction", (
            f"clicking the scanned block activated {active!r}, not the "
            f"region tool 'treToolAction'")
    finally:
        try:
            app.kill()
        except Exception:  # noqa: BLE001
            pass
        kill_pdfe_processes()


# ---------------------------------------------------------------------------
# Self-check — MUST PASS (proves the oracle detects real canvas changes)
# ---------------------------------------------------------------------------
@pytest.mark.timeout(120)
def test_oracle_self_check():
    """Page navigation changes the canvas — the oracle must detect it.

    This is the control: if it fails, the oracle is broken and the xfails
    above are not trustworthy.
    """
    _require_binary()
    app, win = _open(NAV_SELFCHECK)
    try:
        page0 = ro.capture_canvas(win)
        # Deterministic navigation to page index 1.
        write_pipe(win, "testGoToPagePipe", "1")
        time.sleep(1.2)
        page1 = ro.capture_canvas(win)
        assert ro.images_differ(page0, page1, threshold=0.02), (
            "canvas did not change after navigating to page 2 — the render "
            "oracle cannot see real changes")
    finally:
        try:
            app.kill()
        except Exception:  # noqa: BLE001
            pass
        kill_pdfe_processes()


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
