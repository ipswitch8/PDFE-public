# -*- coding: utf-8 -*-
"""Native-UI visual proof of the per-line font-size fix (pipeline c4a9e271).

Phase 1 wired the CLICKED line's own measured size through
``FontMatcher::clickedLineSizePt()`` → ``TextEditTool::setOverlayFontSize()`` so a
large heading line and a small body line in the same scanned region get their
OWN size in the text-edit overlay, instead of every line sharing the single
region-average size (the inert ``perLineFontSizePt`` bug).

This test proves the fix is live in the real Release app — not just in the unit
test — by opening a generated mixed-size *scanned* PDF (heading glyphs ~3x the
body glyphs), activating the text-edit tool, clicking the heading line, then a
body line, and verifying on TWO independent axes that the overlay differs:

  1. DETERMINISTIC readback — ``textEditSelectionDiag()`` now reports
     ``;ovsz=<pt>``: the exact value ``setOverlayFontSize()`` received for the
     clicked line. heading ovsz / body ovsz must be >= 1.5.
       * Under the OLD bug both clicks report the same region-average size
         (ratio ~1.0) → this assertion FAILS, so it genuinely discriminates the
         fix from the regression, not merely "heading is bigger than body".
  2. VISUAL — the live active text-edit overlay box is located in the canvas
     screenshot by its distinct light-blue selection fill (toolbar band
     excluded), and the BLACK GLYPH INK extent rendered inside it is measured.
     heading ink height must be >= 1.5x body ink height. The box is detected by
     colour (NOT by pdf_point_to_screen, which mis-maps under this rig's ~162%
     display scaling) and its height tracks the overlay FONT SIZE, so under the
     old single-region-size bug both boxes would be ~equal (ratio ~1.0). Golden
     crops of each rendered overlay are stored under goldens/per_line_font_size/.

Harness rules (CLAUDE.md / GAP-A14): all UI drive goes through the
``PDFE_TEST_PIPES`` seams in :mod:`render_drive` (testToolActivatePipe,
testClickByPdfCoordPipe) — never raw UIA ``menu_select``/``FindAll`` (hangs the
Server-2019 session). ``app_session`` guarantees no orphaned pdfe.exe. Once the
Release binary launches and the page opens, missing readiness sentinels are
hard FAILURES (SKIP=FAIL), not skips — they would mask a real product break.
"""
from __future__ import annotations

import os
import pathlib
import re
import time

import pytest

from tests.ui_automation.harness import render_drive as d
from tests.ui_automation.conftest import record_matrix_result

# ---------------------------------------------------------------------------
# Fixture + landmarks (see tests/fixtures/make_mixed_size_scan.py).
# Click points are in fitz/PDF space (top-left origin, points); they are mapped
# to the canvas in C++ (canvasRectFromPdf), NOT via the Python coordinate
# helper, so the ~162% display-scaling mismatch does not affect the clicks.
# ---------------------------------------------------------------------------
MIXED = d.FIXTURES / "mixed_size_scan.pdf"
GOLDEN_DIR = d.GOLDENS / "per_line_font_size"

HEADING_CLICK_PT = (96.0, 72.0)    # 96px bold heading (span y ~40-103 pt)
BODY_CLICK_PT = (96.0, 233.0)      # body line 2 (span y ~221-241 pt, 30px glyphs)

STATUS_FILE = pathlib.Path(os.environ.get("TEMP", "/tmp")) / "pdfe_ui_status.txt"

MIN_RATIO = 1.5            # heading must be >= 1.5x body on both axes
TOOLBAR_EXCLUDE_PX = 50   # text-edit toolbar band at the top of the capture
BLUE_ROW_MIN = 20         # min light-blue px in a row to count as edit-box body
INK_LUMA = 80             # below this on all channels == black glyph stroke


# ---------------------------------------------------------------------------
# Status-file helpers
# ---------------------------------------------------------------------------
def _clear_status() -> None:
    try:
        STATUS_FILE.unlink(missing_ok=True)
    except Exception:  # noqa: BLE001
        pass


def _wait_status(substr: str, timeout: float) -> str:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if STATUS_FILE.exists():
            try:
                st = STATUS_FILE.read_text(encoding="utf-8").strip()
            except Exception:  # noqa: BLE001
                st = ""
            if substr in st:
                return st
        time.sleep(0.4)
    return ""


def _parse_ovsz(diag: str) -> float:
    """Extract the ``;ovsz=<pt>`` overlay-size readback from the diag string."""
    m = re.search(r";ovsz=([0-9]+(?:\.[0-9]+)?)", diag or "")
    return float(m.group(1)) if m else -1.0


# ---------------------------------------------------------------------------
# Visual measurement: locate the active edit box, measure rendered glyph ink
# ---------------------------------------------------------------------------
def _active_overlay_metrics(img):
    """Find the live active text-edit box in a full canvas capture by its
    light-blue selection fill (excluding the top toolbar band) and return
    ``(box_height_px, black_ink_extent_px, box_bbox)``.

    Both the box height and the black glyph ink inside it scale with the
    overlay font size set for the clicked line — so heading >> body proves the
    per-line size is applied to the LIVE render. Returns ``None`` if no box is
    found (no active overlay).
    """
    rgb = img.convert("RGB")
    w, h = rgb.size
    px = rgb.load()
    blue_per_row = [0] * h
    minx = [w] * h
    maxx = [-1] * h
    for y in range(TOOLBAR_EXCLUDE_PX, h):
        cnt = 0
        for x in range(w):
            r, g, b = px[x, y]
            if b > 180 and b - r > 18 and b - g > 8 and g > 150:  # light blue
                cnt += 1
                if x < minx[y]:
                    minx[y] = x
                if x > maxx[y]:
                    maxx[y] = x
        blue_per_row[y] = cnt
    rows = [y for y in range(TOOLBAR_EXCLUDE_PX, h) if blue_per_row[y] >= BLUE_ROW_MIN]
    if not rows:
        return None
    y0, y1 = rows[0], rows[-1]
    x0 = min(minx[y] for y in rows)
    x1 = max(maxx[y] for y in rows)
    # Black glyph ink extent within the detected box.
    ink_rows = []
    for y in range(y0, y1 + 1):
        c = 0
        for x in range(max(0, x0), min(w, x1 + 1)):
            r, g, b = px[x, y]
            if r < INK_LUMA and g < INK_LUMA and b < INK_LUMA:
                c += 1
                if c >= 3:
                    break
        if c >= 3:
            ink_rows.append(y)
    ink_ext = (ink_rows[-1] - ink_rows[0] + 1) if ink_rows else 0
    return (y1 - y0 + 1), ink_ext, (x0, y0, x1, y1)


def _click_line_and_capture(win, click_pt):
    """Single-click a line, wait for its per-line atlas to (re)build, then return
    ``(overlay_size_pt, full_canvas_capture)``.

    The spanActivated handler runs setOverlayFontSize() then rebuilds the atlas
    for the clicked line; it writes a transient ``SPAN_CLICKED`` overwritten by
    ``ATLAS_READY|<refLine>|<font>`` within ms, so we gate on ATLAS_READY
    (emitted for EVERY clicked line). By that point ``;ovsz=`` in the diag holds
    the clicked line's per-line size.
    """
    _clear_status()
    d.click_pdf(win, click_pt[0], click_pt[1], settle_s=1.0)
    if not _wait_status("ATLAS_READY", timeout=90.0):
        pytest.fail(f"ATLAS_READY not reported within 90s of clicking {click_pt} "
                    f"— span hit-test missed or atlas build failed silently "
                    f"(last status: {_wait_status('', 0.0)!r})")
    time.sleep(1.0)  # let the overlay repaint at the new per-line size
    ovsz = _parse_ovsz(d.selection_diag(win))
    return ovsz, d.capture(win)


# ---------------------------------------------------------------------------
# Test
# ---------------------------------------------------------------------------
def test_per_line_font_size_overlay_ui():
    """Heading-line overlay renders (and reports) >= 1.5x the body-line size."""
    d.require_binary()
    if not MIXED.exists():
        pytest.skip(f"fixture missing: {MIXED.name} "
                    f"(run tests/fixtures/make_mixed_size_scan.py)")
    GOLDEN_DIR.mkdir(parents=True, exist_ok=True)

    with d.app_session(MIXED, settle_s=2.5) as win:
        _clear_status()
        # Activate text-edit tool — fires onTextEditToolSelected → OCR pipeline.
        d.activate(win, "textEditToolAction", settle_s=1.5)
        ready = _wait_status("TEXT_EDIT_READY", timeout=120.0)
        if not ready:
            # Binary launched + page opened: OCR readiness is required. A missing
            # sentinel is a real break (SKIP=FAIL), not an environment skip.
            pytest.fail("OCR never reported TEXT_EDIT_READY within 120s on the "
                        "mixed-size scan (tessdata not deployed, or the span-click "
                        "OCR path regressed)")
        m = re.search(r"TEXT_EDIT_READY\|(\d+)", ready)
        assert m and int(m.group(1)) >= 2, (
            f"expected >= 2 OCR lines on the mixed-size page, got: {ready!r}")

        head_ovsz, head_cap = _click_line_and_capture(win, HEADING_CLICK_PT)
        body_ovsz, body_cap = _click_line_and_capture(win, BODY_CLICK_PT)

        head_metrics = _active_overlay_metrics(head_cap)
        body_metrics = _active_overlay_metrics(body_cap)
        assert head_metrics is not None, "no active edit box found after heading click"
        assert body_metrics is not None, "no active edit box found after body click"
        head_boxh, head_ink, head_box = head_metrics
        body_boxh, body_ink, body_box = body_metrics

        # Persist the locked visual reference: the rendered overlay box crops.
        head_cap.crop(head_box).save(GOLDEN_DIR / "heading_overlay.png")
        body_cap.crop(body_box).save(GOLDEN_DIR / "body_overlay.png")

        # --- Axis 1: deterministic overlay-size readback ---------------------
        assert head_ovsz > 0 and body_ovsz > 0, (
            f"overlay sizes not reported (head={head_ovsz}, body={body_ovsz}); "
            f"setOverlayFontSize() may not have run")
        size_ratio = head_ovsz / body_ovsz
        assert size_ratio >= MIN_RATIO, (
            f"per-line overlay size NOT applied: heading={head_ovsz:.2f}pt "
            f"body={body_ovsz:.2f}pt ratio={size_ratio:.2f} (< {MIN_RATIO}). "
            f"Both lines share the region average → the perLineFontSizePt bug "
            f"is NOT fixed in the live app.")

        # --- Axis 2: visual rendered glyph height in the LIVE overlay --------
        assert head_ink > 0 and body_ink > 0, (
            f"no rendered glyph ink in the active box (head={head_ink}px "
            f"body={body_ink}px); the overlay did not paint visible text")
        visual_ratio = head_ink / body_ink
        assert visual_ratio >= MIN_RATIO, (
            f"heading overlay did not render visibly taller: glyph ink "
            f"head={head_ink}px body={body_ink}px ratio={visual_ratio:.2f} "
            f"(< {MIN_RATIO}); box heights head={head_boxh}px body={body_boxh}px")

        print(f"[per-line-size] ovsz head={head_ovsz:.2f}pt body={body_ovsz:.2f}pt "
              f"ratio={size_ratio:.2f}; glyph-ink head={head_ink}px body={body_ink}px "
              f"ratio={visual_ratio:.2f}; box head={head_boxh}px body={body_boxh}px")

        # Pipeline c4a9e271 phase-6: this test IS criterion 2's per-line
        # heading-vs-body visual proof — record its matrix row green.
        record_matrix_result(
            "txt_per_line_font_size_ui", "green",
            f"per-line overlay verified: size ratio={size_ratio:.2f}, "
            f"glyph-ink ratio={visual_ratio:.2f} (>= {MIN_RATIO})")
