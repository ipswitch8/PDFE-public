# -*- coding: utf-8 -*-
"""Shared drive helpers for the Phase 3 category render-verification suites
(pipeline e7d3c2a1).

Every ``test_render_*.py`` category file imports :mod:`render_oracle` for the
pixel assertions and this module for the *drive* primitives: launch/teardown,
deterministic action/click/type seams, golden management, and the small set of
readback pipes (active-tool, page-count, zoom, OCR text). Keeping the drive
logic here means each category file stays a thin list of
``pre-capture → act → post-capture → assert`` tests rather than re-deriving the
seam contracts.

Design rules carried over from the Phase 2 proof harness:

* **Deterministic seams over PyAutoGUI clicks.** All UI drive goes through the
  ``PDFE_TEST_PIPES`` QLineEdit seams (``testToolActivatePipe``,
  ``testClickByPdfCoordPipe``, ``testDoubleClickByPdfCoordPipe``,
  ``testCanvasDragPipe``, ``testAddRedactionMarkPipe``, ``testGoToPagePipe`` …)
  — NEVER ``win.menu_select`` / raw UIA ``FindAll`` (GAP-A14 hangs the
  Server-2019 CI session). PyAutoGUI is used *only* for pixel capture, inside
  :mod:`render_oracle`.
* **Release binary.** ``PDFE_BUILD_CONFIG`` is forced to Release before
  importing the fixture so the launched binary is
  ``build/vs2022/bin/Release/pdfe.exe`` (the binary the pipeline targets).
* **No orphans.** Every helper that launches also offers a teardown; the
  :func:`app_session` context manager guarantees it.
"""
from __future__ import annotations

import contextlib
import os
import pathlib
import sys
import time

import pytest

# Force the Release binary + test pipes BEFORE importing the fixture (it
# resolves PDFE_EXE and the pipe env at import / launch time).
os.environ.setdefault("PDFE_BUILD_CONFIG", "Release")
os.environ.setdefault("PDFE_TEST_PIPES", "1")
# Bypass the AtlasComparisonDialog first-click popup on scanned-region edits.
# That dialog is an app-modal Qt window; under UI automation it blocks UIA
# enumeration (GAP-A14), so a subsequent write_pipe()'s UIA FindAll hangs
# uninterruptibly and HARD-KILLS the whole pytest process (one scanned-edit
# test was aborting the entire render suite). The product ships a deterministic
# seam for exactly this: PDFE_SKIP_ATLAS_DIALOG=1 auto-accepts the persisted
# atlas mode and never shows the modal (MainWindow.cpp onTextEdit path).
os.environ.setdefault("PDFE_SKIP_ATLAS_DIALOG", "1")

# Guarded UI deps — skip the importing module cleanly on a headless box.
try:
    from pywinauto import Application  # noqa: F401
except Exception:  # noqa: BLE001
    pytest.skip("pywinauto not installed", allow_module_level=True)
try:
    import pyautogui  # noqa: F401
except Exception:  # noqa: BLE001
    pytest.skip("pyautogui not installed", allow_module_level=True)
try:
    from PIL import Image  # noqa: F401
except Exception:  # noqa: BLE001
    pytest.skip("Pillow not installed", allow_module_level=True)

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.ui_automation.harness.pdfe_app_fixture import (  # noqa: E402
    launch_pdfe, kill_pdfe_processes, write_pipe, read_result_pipe,
    invoke_action, PDFE_EXE,
)
from tests.ui_automation.harness.canvas_click import (  # noqa: E402
    canvas_region, pdf_point_to_screen,
)
from tests.ui_automation.harness import render_oracle as ro  # noqa: E402
# Phase F25 / gap WIN-45: reachability is a RECORDED NON-PASS, never a skip.
from tests.ui_automation.conftest import (  # noqa: E402
    current_test_id,
    record_matrix_result,
)

# Real OS-level foreground promotion — annotation/shape creation needs a genuine
# mouse-drag event sequence delivered to a foreground window; the synthetic
# QApplication::sendEvent path (testCanvasDragPipe) does NOT create annotations.
try:
    from tests.helpers.keys import ensure_foreground, press_to  # noqa: E402
except Exception:  # noqa: BLE001
    def ensure_foreground(_hwnd, timeout=2.0, poll=0.05):  # type: ignore
        return False

    def press_to(_hwnd, _key, focus_timeout=2.0):  # type: ignore
        raise RuntimeError("press_to unavailable")

# CRITICAL: mark THIS Python process PER_MONITOR_AWARE_V2 *before* any pywinauto
# geometry query. This rig runs non-standard (~162%) display scaling; without
# DPI awareness Windows virtualizes the coordinates pywinauto reports while
# SetCursorPos/mouse_event use raw physical pixels — so a real-mouse drag lands
# far off the page and silently creates no annotation. The proven
# tests/ui_harness launcher does this; the render layer must too.
try:
    from tests.ui_harness.core.launch import _set_process_dpi_aware  # noqa: E402
    _set_process_dpi_aware()
except Exception:  # noqa: BLE001
    import ctypes as _ctypes
    try:
        _ctypes.windll.user32.SetProcessDpiAwarenessContext(-4)
    except Exception:  # noqa: BLE001
        pass

# ---------------------------------------------------------------------------
# Locations
# ---------------------------------------------------------------------------
FIXTURES = REPO_ROOT / "tests" / "fixtures"
GOLDENS = REPO_ROOT / "tests" / "ui_automation" / "goldens"

#: The render-oracle fixture corpus (created in Phase 2) + the pre-existing
#: vector / scanned / form fixtures the category suites reuse.
INVOICE = FIXTURES / "invoice_vector_heading.pdf"
MIXED_DPI = FIXTURES / "mixed_dpi_page.pdf"
MULTILINGUAL = FIXTURES / "multilingual_latin_arabic.pdf"
MIXED_CONTENT = FIXTURES / "mixed_content_page.pdf"
NAV_SELFCHECK = FIXTURES / "nav_selfcheck.pdf"
MULTISPAN = FIXTURES / "test_multispan.pdf"
SCANNED = FIXTURES / "test_scanned.pdf"
SCANNED_OCR = FIXTURES / "test_scanned_with_ocr.pdf"
FORM = FIXTURES / "test_form.pdf"

LETTER = (612.0, 792.0)

# Common PDF-coordinate landmarks on the fixtures (fitz space: top-left origin).
INVOICE_HEADING_BBOX = (247.0, 73.0, 350.0, 106.0)
INVOICE_HEADING_CENTER = (298.7, 89.5)
MIXED_CONTENT_VECTOR_CENTER = (200.0, 120.0)   # live vector paragraph
MIXED_CONTENT_SCANNED_CENTER = (297.0, 435.0)  # rasterised "scanned" block
THUMB_REGION_BBOX = (80.0, 470.0, 224.0, 614.0)
THUMB_SPAN_CENTER = (135.0, 488.0)


# ---------------------------------------------------------------------------
# Launch / teardown
# ---------------------------------------------------------------------------
def require_binary() -> None:
    """Skip the test if the Release pdfe.exe has not been built."""
    if not PDFE_EXE.exists():
        pytest.skip(f"pdfe.exe (Release) not built at {PDFE_EXE}")


def open_doc(doc: pathlib.Path, settle_s: float = 2.0):
    """Launch pdfe on *doc*, maximize, settle, and return ``(app, win)``.

    Skips (never errors) when the fixture is missing or the PdfCanvas does not
    appear in the UIA tree — both are environment conditions, not product
    failures.
    """
    require_binary()
    if not doc.exists():
        pytest.skip(f"fixture missing: {doc.name}")
    app, win = launch_pdfe(args=[str(doc)])
    try:
        win.maximize()
    except Exception:  # noqa: BLE001
        pass
    time.sleep(settle_s)
    if canvas_region(win) is None:
        teardown(app)
        # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
        _reach = (
            "PdfCanvas absent from UIA tree"
        )
        record_matrix_result(current_test_id(), "blocked", _reach)
        pytest.fail(
            f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
    return app, win


def teardown(app) -> None:
    """Kill the app two ways (``app.kill`` + ``taskkill``). Never raises."""
    if app is not None:
        try:
            app.kill()
        except Exception:  # noqa: BLE001
            pass
    kill_pdfe_processes()


@contextlib.contextmanager
def app_session(doc: pathlib.Path, settle_s: float = 2.0):
    """Context manager: ``with app_session(DOC) as win:`` — guarantees teardown
    (no orphaned pdfe.exe) even if the test body raises."""
    app, win = open_doc(doc, settle_s=settle_s)
    try:
        yield win
    finally:
        teardown(app)


# ---------------------------------------------------------------------------
# Capture
# ---------------------------------------------------------------------------
def capture(win):
    """Capture the PdfCanvas pixels as a ``PIL.Image`` (RGB)."""
    return ro.capture_canvas(win)


def capture_stable(win, tries: int = 6, interval: float = 0.4,
                   threshold: float = 0.005):
    """Capture the canvas only once it has stopped changing between frames.

    Captures repeatedly until two consecutive captures differ by at most
    *threshold* (default 0.5%) or *tries* is exhausted, then returns the last
    capture. This removes a render RACE that bites only under load: a plain
    ``capture()`` issued right after launch/settle can grab a MID-render frame
    when the machine is busy (e.g. during the full render_*.py sweep). A
    later second capture then catches the settled frame, so a
    "did the canvas change?" assertion sees a spurious change that does not
    reproduce in isolation. Gating the capture on inter-frame stability makes
    the measured ``before``/``after`` frames deterministic regardless of host
    load. Use it for any test whose verdict turns on whether the canvas
    changed (especially the change-detection / no-change xfail probes)."""
    prev = ro.capture_canvas(win)
    for _ in range(max(1, tries - 1)):
        time.sleep(interval)
        cur = ro.capture_canvas(win)
        if not ro.images_differ(prev, cur, threshold=threshold):
            return cur
        prev = cur
    return prev


def crop_to_page_content(img, white_thresh: int = 230,
                         min_run_frac: float = 0.15, pad: int = 2,
                         min_keep: float = 0.05):
    """Crop a full-canvas capture down to the rendered *page* rectangle.

    Why this exists (D8-2, pipeline c4a9e271 Phase 10). The five render goldens
    ``page_ops_rotate_cw`` / ``view_nav_rotate_cw`` / ``annot_rectangle`` /
    ``redaction_mark`` / ``coord_rotation`` were captured over the WHOLE captured
    frame — the top toolbar strip plus the gray PdfCanvas viewport with the white
    page centred in it. The viewport margin shade (#666 vs #999), the fit-zoom
    level, and the window geometry all differ from host to host, so the *margin*
    (most of the frame at fit-zoom) dominated the SSIM/pixel diff and the goldens
    drifted to false-reds on a new rig even though the page content was identical
    (discovery_run_phase8.md, Cluster A).

    The fix the discovery doc recommends is "crop to the page rect before
    diffing so goldens are not geometry-sensitive." The PDF page renders as a
    large near-WHITE block that is cleanly distinct from the gray viewport margin
    (~#6a) and the lighter toolbar chrome (~#d4); the only other near-white
    element (the small zoom-combo box in the toolbar) is tiny. So we build a
    near-white mask and keep only rows/columns whose near-white run is at least
    ``min_run_frac`` of the frame dimension — that isolates the big page block and
    rejects the combo box and any stray bright speck. The result is the page
    rectangle regardless of margin shade, fit-zoom, window geometry, page size,
    or page rotation (a rotated page is still a large white block). ``diff.py``
    then resizes the two page crops to common dimensions, so the residual
    fit-zoom difference becomes a pure scale that SSIM tolerates.

    Falls back to the original image when no qualifying page block is found
    (e.g. a non-white/scanned page or the page filling the frame) — never worse
    than the pre-crop behaviour. ``min_keep`` rejects a degenerate tiny crop.
    """
    rgb = img.convert("RGB")
    w, h = rgb.size
    if w == 0 or h == 0:
        return rgb
    import numpy as _np
    arr = _np.asarray(rgb, dtype=_np.int16)
    # Near-white = every channel at/above the threshold. White page (255) passes;
    # gray margin (~106) and toolbar chrome (~212) do not.
    white = (arr >= white_thresh).all(axis=-1)
    if not white.any():
        return rgb
    # A column/row "belongs to the page" if its near-white run clears the
    # fraction threshold. The page span is first..last such column/row AFTER
    # dropping thin isolated runs: a 1px near-white frame line at x=0 / y=0 (a
    # capture/border artifact) would otherwise drag the bbox back across the gray
    # margin, while a mid-page feature (the drawn rectangle, a redaction bar)
    # only briefly drops the run and must be BRIDGED so the whole page — feature
    # included — is kept. Spanning first..last over the surviving (>= min_seg)
    # runs does both: it ignores the thin border artifacts and bridges the
    # feature gaps. The toolbar's zoom-combo box never qualifies (too short to
    # clear the fraction threshold), so it is excluded for free.
    min_seg = 3
    col_span = _content_span(white.sum(axis=0) >= min_run_frac * h, min_seg)
    row_span = _content_span(white.sum(axis=1) >= min_run_frac * w, min_seg)
    if col_span is None or row_span is None:
        return rgb
    x0 = max(0, col_span[0] - pad)
    x1 = min(w, col_span[1] + pad)
    y0 = max(0, row_span[0] - pad)
    y1 = min(h, row_span[1] + pad)
    if (x1 - x0) * (y1 - y0) < min_keep * w * h:
        return rgb
    return rgb.crop((x0, y0, x1, y1))


def _content_span(flags, min_seg: int):
    """Return ``(first_start, last_end_exclusive)`` spanning every run of
    consecutive True values in *flags* whose length is at least *min_seg*, or
    ``None`` if no run qualifies. Runs shorter than *min_seg* (thin isolated
    border artifacts) are ignored; gaps BETWEEN surviving runs are bridged."""
    import numpy as _np
    b = _np.asarray(flags, dtype=_np.int8)
    if not b.any():
        return None
    edges = _np.diff(_np.concatenate([[0], b, [0]]))
    starts = _np.flatnonzero(edges == 1)
    ends = _np.flatnonzero(edges == -1)
    keep = (ends - starts) >= min_seg
    if not keep.any():
        return None
    return int(starts[keep][0]), int(ends[keep][-1])


def locate_active_edit_box(img, top_skip: int = 45):
    """Return the screen-rect ``(x0, y0, x1, y1)`` of the text-edit overlay's
    active-edit box within a full-canvas capture *img*, or ``None``.

    The in-place editor draws a solid blue border (and a light-blue fill) around
    the span being edited (see the per-line font-size UI proof, which keys on the
    same blue box). We detect the strong-blue BORDER pixels — restricted to below
    the toolbar strip (``top_skip``) so a blue toolbar button can't be mistaken
    for the box — and return their bounding rectangle. This is the deterministic
    locator the render-font tests crop to, because ``crop_pdf_bbox`` mis-maps by
    ~78px on this ~162%-scaled rig.
    """
    import numpy as _np
    a = _np.asarray(img.convert("RGB"), dtype=_np.int16)
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    border = (b > 150) & (r < 120) & (g < 160) & ((b - r) > 60)
    border[:top_skip, :] = False
    ys, xs = _np.where(border)
    if ys.size < 20:
        return None
    return (int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max()))


def capture_overlay_over_underlay(win, pre_edit_full=None, inset: int = 5):
    """Capture the active text-edit overlay and the matching underlay crop.

    Call this AFTER the edit overlay is open (double-click on a span). It
    captures the current full canvas, locates the active-edit box
    (:func:`locate_active_edit_box`), and returns two PIL images cropped to that
    same box, inset by *inset* px to drop the blue border itself:

      * the **overlay** crop (the in-place editor's rendered glyphs), and
      * the **underlay** crop — the identical screen rectangle taken from
        *pre_edit_full*, the full-canvas page render captured BEFORE the edit
        began (so the page text that sits under the overlay).

    Returns ``(overlay_crop, underlay_crop)``; ``underlay_crop`` is ``None`` when
    *pre_edit_full* is not supplied. Raises if the overlay box can't be located.
    """
    overlay_full = capture(win)
    box = locate_active_edit_box(overlay_full)
    if box is None:
        raise AssertionError(
            "could not locate the active text-edit box (no blue editor border "
            "found below the toolbar) — is a span being edited?")
    x0, y0, x1, y1 = box
    rect = (x0 + inset, y0 + inset, x1 - inset, y1 - inset)
    overlay_crop = overlay_full.crop(rect)
    underlay_crop = pre_edit_full.crop(rect) if pre_edit_full is not None \
        else None
    return overlay_crop, underlay_crop


def save_as(win, out_path, timeout: float = 30.0) -> bool:
    """Save the open document to *out_path* via the testSaveAsPipe seam.

    Returns True once the file exists. Lets a probe inspect what the product
    actually WROTE, rather than inferring it from the canvas.
    """
    out = pathlib.Path(out_path)
    try:
        out.unlink()
    except OSError:
        pass
    write_pipe(win, "testSaveAsPipe", str(out))
    deadline = time.time() + timeout
    while time.time() < deadline:
        if out.exists() and out.stat().st_size > 0:
            return True
        time.sleep(0.25)
    return False


def image_dpi_under_region(pdf_path, bbox, page_index: int = 0):
    """Native DPI of the image drawn under *bbox*, from a SAVED document.

    GitHub #266. Returns ``(dpi, pixel_width, placement_width_pt)`` for the
    image whose placement contains the bbox centre, or ``None`` when no image
    does. This is the direct form of "was the region re-rendered at the page
    maximum?" -- the question defect #2 and this fixture are about -- and it is
    exact, where an edge-energy ratio over a screenshot is a proxy.
    """
    import fitz  # imported here: only probes that ask for this need PyMuPDF

    doc = fitz.open(str(pdf_path))
    try:
        page = doc[page_index]
        cx = (bbox[0] + bbox[2]) / 2.0
        cy = (bbox[1] + bbox[3]) / 2.0
        best = None
        for im in page.get_images(full=True):
            xref, px_w = im[0], im[2]
            for r in page.get_image_rects(xref):
                if r.x0 <= cx <= r.x1 and r.y0 <= cy <= r.y1:
                    width_pt = r.x1 - r.x0
                    if width_pt <= 0 or px_w <= 0:
                        continue
                    dpi = px_w / (width_pt / 72.0)
                    # Topmost/tightest wins, matching how the product picks the
                    # image a region belongs to.
                    if best is None or width_pt < best[2]:
                        best = (dpi, px_w, width_pt)
        return best
    finally:
        doc.close()


def page_rect_on_canvas(win, page_index: int = 0):
    """(x, y, w, h) of a page in canvas-widget pixels, straight from the app.

    Asks ``testPageRectQueryPipe`` / ``testPageRectResultPipe``, which report
    ``PdfCanvas::getPageScreenRect()``. Returns None when the seam does not
    answer — callers must FAIL on that rather than fall back to a computed
    guess, which is the whole point of asking.
    """
    raw = read_result_pipe(win, "testPageRectQueryPipe",
                           "testPageRectResultPipe", str(page_index))
    raw = (raw or "").strip()
    if not raw or raw == "none":
        return None
    try:
        x, y, w, h = (int(float(v)) for v in raw.split(","))
    except ValueError:
        return None
    if w <= 0 or h <= 0:
        return None
    return x, y, w, h


def crop_pdf_bbox(win, img, bbox, page=LETTER, pad=8):
    """Crop a full-canvas capture *img* to a PDF-coordinate bbox.

    ASKS THE APP WHERE THE PAGE IS. It used to map PDF points through
    ``pdf_point_to_screen()``, which calibrates a pixels-per-point ratio and
    re-derives PDFE's page centring in Python. That was measurably wrong, and
    wrong in the way CLAUDE.md warns about: the crop landed in the grey margin
    at the page's edge.

    The consequence was not a visible failure but a VACUOUS one. Measured
    2026-09-29 on mixed_dpi_page.pdf: the returned crop was 196x196 px of blank
    page and grey margin, containing none of the 150-DPI thumbnail it was
    supposed to frame. Its "sharpness" of 2.18 was the contrast step between
    margin and paper — which is why it read 2.18 before an edit and 2.18 after,
    and why the ``sharp_before > 0.0`` guard written to catch a blank crop
    passed happily. Two probes reported "the recomposite changes no pixels" for
    months on the strength of it, and the pixels they were watching were never
    going to change.

    The page rect now comes from ``PdfCanvas::getPageScreenRect()`` via a seam,
    so the mapping cannot drift from the widget's own layout, and a bbox is
    scaled by the page's ACTUAL on-canvas size rather than an inferred zoom.

    Raises AssertionError if the seam does not answer. The previous
    ``return img`` on failure handed back the entire canvas, which is a
    plausible-looking image that no caller could distinguish from a real crop.
    """
    rect = page_rect_on_canvas(win, 0)
    assert rect is not None, (
        "testPageRectQueryPipe did not answer, so the page's position on the "
        "canvas is unknown. Failing rather than cropping a guessed rectangle: "
        "a wrong crop does not error, it silently measures the wrong pixels.")
    px, py, pw, ph = rect

    sx = pw / float(page[0])
    sy = ph / float(page[1])
    x0 = px + bbox[0] * sx
    y0 = py + bbox[1] * sy
    x1 = px + bbox[2] * sx
    y1 = py + bbox[3] * sy

    cx0 = max(0, int(min(x0, x1) - pad))
    cy0 = max(0, int(min(y0, y1) - pad))
    cx1 = min(img.size[0], int(max(x0, x1) + pad))
    cy1 = min(img.size[1], int(max(y0, y1) + pad))
    assert cx1 > cx0 and cy1 > cy0, (
        f"PDF bbox {bbox} maps to an empty crop on a page drawn at {rect} "
        f"within a {img.size} capture")
    return img.crop((cx0, cy0, cx1, cy1))


# ---------------------------------------------------------------------------
# Action / click / type seams
# ---------------------------------------------------------------------------
def activate(win, action_object_name: str, settle_s: float = 1.2) -> None:
    """Trigger a MainWindow QAction by objectName via testToolActivatePipe."""
    write_pipe(win, "testToolActivatePipe", action_object_name)
    time.sleep(settle_s)


# ---------------------------------------------------------------------------
# GitHub #218 -- ImageReplacer recomposite completion.
#
# Three render probes were xfail(strict=False) for one measured reason: the
# scanned-page recomposite engages only after the OCR -> atlas chain finishes,
# and nothing published that it had. The screenshot was therefore sampled after
# commit()'s blind 1.5 s settle -- a race dressed as a delay.
#
# MainWindow now writes a terminal RECOMPOSITE_DONE / _FAILED / _SKIPPED line to
# the UI status file with a MONOTONIC sequence number. The sequence is what
# makes the wait sound: without it a waiter sees the PREVIOUS edit's line and
# returns instantly, which is the same race with extra steps.
# ---------------------------------------------------------------------------
UI_STATUS_PATH = pathlib.Path(
    os.environ.get("TEMP", os.environ.get("TMP", "/tmp"))) / "pdfe_ui_status.txt"

_RECOMPOSITE_TERMINAL = ("RECOMPOSITE_DONE", "RECOMPOSITE_FAILED",
                         "RECOMPOSITE_SKIPPED")


def _read_recomposite_line():
    """Return ``(status, page, seq, detail)`` or ``None``.

    ``None`` covers three genuinely different situations -- no file, a file
    holding some other status, and an unparseable line -- and they are folded
    together deliberately: to a waiter they all mean "no terminal recomposite
    outcome is visible", and the caller's timeout message reports the raw line
    so the distinction is recoverable when it matters.
    """
    try:
        raw = UI_STATUS_PATH.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    line = raw.strip()
    if not line.startswith(_RECOMPOSITE_TERMINAL):
        return None
    parts = line.split("|")
    if len(parts) < 3:
        return None
    try:
        return parts[0], int(parts[1]), int(parts[2]), "|".join(parts[3:])
    except ValueError:
        return None


def recomposite_mark() -> int:
    """The recomposite sequence number as of NOW; 0 when none has occurred.

    Call this BEFORE the edit. ``wait_recomposite`` requires a strictly greater
    sequence, so a stale line left by an earlier test cannot satisfy the wait.
    """
    parsed = _read_recomposite_line()
    return parsed[2] if parsed else 0


def wait_recomposite(win, since: int, timeout: float = 90.0):
    """Block until a terminal recomposite outcome newer than ``since``.

    Returns ``(status, page, seq, detail)``. Raises ``AssertionError`` on
    timeout -- deliberately, rather than returning a sentinel: a caller that
    silently proceeds on "no signal" reproduces the blind-settle behaviour this
    replaces, and the resulting screenshot assertion would fail somewhere
    unrelated to the actual cause.

    NOTE ON WHAT THIS DOES AND DOES NOT GUARANTEE. The status is published
    immediately after ``document_->markPageDirty()``, so on return the page is
    certainly recomposited and flagged. Qt's repaint is still asynchronous, so
    a short settle for the PAINT remains necessary; what is removed is the
    unbounded, multi-second OCR/atlas/recomposite race. Claiming more than that
    would be overstating the seam.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        parsed = _read_recomposite_line()
        if parsed and parsed[2] > since:
            return parsed
        time.sleep(0.2)
    try:
        raw = UI_STATUS_PATH.read_text(encoding="utf-8", errors="replace").strip()
    except OSError as exc:
        raw = "<unreadable: %s>" % exc
    raise AssertionError(
        "no RECOMPOSITE_* outcome with sequence > %d appeared within %.0fs. "
        "Last status line: %r. An older sequence means the edit never reached "
        "refreshImageForScannedPage; no RECOMPOSITE line at all means the "
        "running pdfe.exe predates the GitHub #218 seam -- check that "
        "build/vs2022/bin/%s/pdfe.exe is current."
        % (since, timeout, raw, os.environ.get("PDFE_BUILD_CONFIG", "Debug")))


def activate_span(win, target="0", timeout: float = 10.0) -> str:
    """Activate a TRE text span through the production selectSpan() path.

    GitHub #219 / #218. ``target`` is a span index (``"0"``) or a PDF-space
    point (``"280,700"``). Returns the seam's verdict, ``"ok|<index>"`` or
    ``"err|<reason>"``; the caller asserts on it.

    This exists because a hard-coded canvas coordinate is a guess, and a guess
    that lands on the wrong span fails as a mysterious "nothing re-rendered"
    rather than as "I activated a span that is not an OCR layer". Asking for a
    span by index removes the coordinate from the question entirely.
    """
    from tests.ui_automation.harness.pdfe_app_fixture import (
        find_widget, _read_pipe_value,
    )
    write_pipe(win, "testSpanActivatePipe", str(target))
    result = find_widget(win, "testSpanActivateResultPipe", timeout=timeout,
                         require_visible=False)
    deadline = time.time() + timeout
    verdict = ""
    while time.time() < deadline:
        verdict = (_read_pipe_value(result) or "").strip()
        if verdict:
            break
        time.sleep(0.2)
    return verdict


def commit(win, settle_s: float = 1.5) -> None:
    """Commit an in-progress text edit by switching to the select tool, which
    runs clearTextEditTool() → deactivate() → commitEdit() → re-render."""
    write_pipe(win, "testToolActivatePipe", "selectToolAction")
    time.sleep(settle_s)


def commit_and_wait_recomposite(win, since: int, timeout: float = 90.0,
                                paint_settle_s: float = 0.8):
    """Commit a scanned-page edit and wait for its recomposite to complete.

    ``since`` must come from a ``recomposite_mark()`` taken BEFORE the edit.
    Returns the parsed outcome tuple so the caller can assert on the STATUS --
    a RECOMPOSITE_SKIPPED is a legitimate product answer ("that span was not an
    OCR invisible-text layer") and a test that treated it as success would be
    asserting a pixel diff that has no reason to exist.
    """
    write_pipe(win, "testToolActivatePipe", "selectToolAction")
    outcome = wait_recomposite(win, since, timeout=timeout)
    time.sleep(paint_settle_s)
    return outcome


def undo(win, settle_s: float = 1.5) -> None:
    write_pipe(win, "testToolActivatePipe", "undoAction")
    time.sleep(settle_s)


def redo(win, settle_s: float = 1.5) -> None:
    write_pipe(win, "testToolActivatePipe", "redoAction")
    time.sleep(settle_s)


def go_to_page(win, idx: int, settle_s: float = 1.2) -> None:
    write_pipe(win, "testGoToPagePipe", str(idx))
    time.sleep(settle_s)


def click_pdf(win, x, y, page_index: int = 0, settle_s: float = 0.8) -> None:
    write_pipe(win, "testClickByPdfCoordPipe", f"{page_index},{x},{y}")
    time.sleep(settle_s)


def double_click_pdf(win, x, y, page_index: int = 0,
                     settle_s: float = 0.8) -> None:
    write_pipe(win, "testDoubleClickByPdfCoordPipe", f"{page_index},{x},{y}")
    time.sleep(settle_s)


def _resolve_pdfe_hwnd(hwnd_hint: int) -> int:
    """Resolve a valid PDFE main-window HWND (mirrors the proven helper in
    tests/test_ui_annotations.py). Returns the hint when it's already valid,
    else enumerates top-level windows for a visible non-self window titled
    'PDFE'."""
    if hwnd_hint and hwnd_hint > 0:
        return int(hwnd_hint)
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    proto = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_long)
    my_pid = os.getpid()
    found = [0]

    def _cb(h, _lp):
        wpid = wintypes.DWORD(0)
        user32.GetWindowThreadProcessId(h, ctypes.byref(wpid))
        if wpid.value and wpid.value != my_pid and user32.IsWindowVisible(h):
            buf = ctypes.create_unicode_buffer(512)
            user32.GetWindowTextW(h, buf, 512)
            if "PDFE" in (buf.value or ""):
                found[0] = int(h)
                return False
        return True

    user32.EnumWindows(proto(_cb), 0)
    return found[0]


def _real_mouse_drag_screen(win, p0, p1, hold_steps: int = 8) -> None:
    """OS-level left-button drag from absolute screen point *p0* to *p1*.

    Ports the proven ``_drag_canvas`` mechanism (test_ui_annotations.py): force
    the PDFE window foreground, then issue a real LEFTDOWN → intermediate
    SetCursorPos moves → LEFTUP sequence so Qt receives a genuine drag (the
    only thing that makes ink/shape/signature tools commit an annotation)."""
    import ctypes
    hwnd_hint = 0
    try:
        hwnd_hint = int(win.handle)
    except Exception:  # noqa: BLE001
        hwnd_hint = 0
    real_hwnd = _resolve_pdfe_hwnd(hwnd_hint)
    if real_hwnd > 0:
        try:
            ensure_foreground(real_hwnd, timeout=2.0)
        except Exception:  # noqa: BLE001
            pass
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    cx1, cy1 = int(p0[0]), int(p0[1])
    cx2, cy2 = int(p1[0]), int(p1[1])
    user32.SetCursorPos(cx1, cy1)
    time.sleep(0.05)
    user32.mouse_event(0x0002, 0, 0, 0, 0)  # LEFTDOWN
    time.sleep(0.05)
    if hold_steps < 1:
        hold_steps = 1
    for i in range(1, hold_steps + 1):
        ix = int(cx1 + (cx2 - cx1) * (i / hold_steps))
        iy = int(cy1 + (cy2 - cy1) * (i / hold_steps))
        user32.SetCursorPos(ix, iy)
        time.sleep(0.03)
    user32.mouse_event(0x0004, 0, 0, 0, 0)  # LEFTUP
    time.sleep(0.4)


def drag_pdf(win, x0, y0, x1, y1, page_index: int = 0, page=LETTER,
             settle_s: float = 0.8) -> None:
    """Drag from fitz-space (x0,y0) to (x1,y1) with a REAL OS-level mouse drag.

    Annotation/shape/signature tools only commit on a genuine drag event
    sequence delivered to the foreground window — the synthetic
    ``testCanvasDragPipe`` (QApplication::sendEvent) path does NOT create
    annotations. Maps both fitz endpoints to absolute screen pixels via
    ``pdf_point_to_screen`` and drives the mouse directly. Falls back to the
    pipe drive only if the canvas geometry can't be resolved."""
    p0 = pdf_point_to_screen(win, x0, y0, page[0], page[1])
    p1 = pdf_point_to_screen(win, x1, y1, page[0], page[1])
    if p0 is None or p1 is None:
        write_pipe(win, "testCanvasDragPipe",
                   f"{page_index},{x0},{y0},{x1},{y1}")
        time.sleep(settle_s)
        return
    _real_mouse_drag_screen(win, p0, p1)
    time.sleep(settle_s)


def add_redaction_mark(win, x, y, w, h, page_index: int = 0,
                       settle_s: float = 0.8) -> None:
    """Add a redaction mark over the fitz-space rect (x,y,w,h)."""
    write_pipe(win, "testAddRedactionMarkPipe",
               f"{page_index},{x},{y},{w},{h}")
    time.sleep(settle_s)


def type_text(win, text: str, settle_s: float = 1.0) -> None:
    write_pipe(win, "testTextEditTypePipe", text)
    time.sleep(settle_s)


# ---------------------------------------------------------------------------
# Readback pipes
# ---------------------------------------------------------------------------
def active_tool(win) -> str:
    """Return the active editing tool ('treToolAction' / 'textEditToolAction'
    / a checked annotation action / 'none') via the activeToolDiag seam."""
    return read_result_pipe(win, "testActiveToolQueryPipe",
                            "testActiveToolResultPipe", "1") or ""


def selection_diag(win) -> str:
    return read_result_pipe(win, "testTextEditDiagTriggerPipe",
                            "testTextEditDiagResultPipe", "1") or ""


def span_selected(diag: str) -> bool:
    """True if a text-edit diag string reports an active span selection."""
    head = diag.split(";")[0].split("|")[0]
    parts = head.split(",")
    if len(parts) < 2:
        return False
    try:
        return int(parts[0]) > 0 and int(parts[1]) >= 0
    except ValueError:
        return False


def page_count(win) -> int:
    raw = read_result_pipe(win, "testPageCountQueryPipe",
                           "testPageCountResultPipe", "1")
    try:
        return int((raw or "").strip())
    except ValueError:
        return -1


def zoom_value(win) -> str:
    return read_result_pipe(win, "testZoomComboQueryPipe",
                            "testZoomComboResultPipe", "1") or ""


#: Reference display scale the render-oracle pixel thresholds were calibrated at
#: (mirrors render_oracle.RIG_DISPLAY_SCALE — the rig's combined display+app
#: zoom, "~162%"). get_app_zoom_factor() normalises the live zoom against this
#: so a scale_factor of 1.0 means "at the calibration scale".
RIG_REFERENCE_ZOOM = 1.62


def parse_zoom_to_float(raw: str) -> float:
    """Parse a zoom-combo readback string into an absolute zoom fraction.

    The testZoomComboResultPipe reports the app zoom as either a percentage
    string ("162%", "100 %") or a bare ratio ("1.62", "1"). Returns the value as
    a float fraction (162% -> 1.62, "1.0" -> 1.0). Raises ValueError on an
    unparseable string so a broken pipe never silently degrades to a wrong scale.
    """
    s = (raw or "").strip()
    if not s:
        raise ValueError("empty zoom readback")
    pct = s.endswith("%")
    if pct:
        s = s[:-1].strip()
    try:
        v = float(s)
    except ValueError as exc:
        raise ValueError(f"unparseable zoom readback {raw!r}") from exc
    if pct or v > 10.0:  # values like 162 are percentages even without the '%'
        v /= 100.0
    if v <= 0.0:
        raise ValueError(f"non-positive zoom {raw!r}")
    return v


def get_app_zoom_factor(win, reference: float = RIG_REFERENCE_ZOOM) -> float:
    """Return the live app zoom NORMALISED against the calibration *reference*.

    Reads the current zoom via :func:`zoom_value` (the testZoomComboQueryPipe ->
    testZoomComboResultPipe seam), parses it to an absolute fraction, and divides
    by *reference* so the result is the ``scale_factor`` to hand to the
    render-oracle pixel assertions: 1.0 at the rig's reference zoom, ~1.23 when
    the user has zoomed one step beyond it, etc. The render-oracle alignment
    tolerance multiplies by this; the slant/font-class oracles ignore it (they
    are scale-invariant — see render_oracle.RIG_DISPLAY_SCALE).
    """
    return parse_zoom_to_float(zoom_value(win)) / reference


def _find_window_by_title(title_substr: str, timeout: float = 6.0) -> int:
    """Return the HWND of a visible non-self top-level window whose title
    contains *title_substr* (case-insensitive), polling until *timeout*.
    UIA-free (raw EnumWindows) — GAP-A14-safe for modal QMessageBoxes that
    UIA FindAll would hang on."""
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    proto = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_long)
    my_pid = os.getpid()
    needle = title_substr.lower()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        found = [0]

        def _cb(h, _lp):
            wpid = wintypes.DWORD(0)
            user32.GetWindowThreadProcessId(h, ctypes.byref(wpid))
            if wpid.value and wpid.value != my_pid and user32.IsWindowVisible(h):
                buf = ctypes.create_unicode_buffer(512)
                user32.GetWindowTextW(h, buf, 512)
                if needle in (buf.value or "").lower():
                    found[0] = int(h)
                    return False
            return True

        user32.EnumWindows(proto(_cb), 0)
        if found[0]:
            return found[0]
        time.sleep(0.2)
    return 0


def accept_dialog(win, title_substr: str, accept_key: str = "y",
                  timeout: float = 6.0, settle_s: float = 0.8) -> bool:
    """Wait for a modal QMessageBox titled *title_substr* and accept it.

    Sends the accept mnemonic (default 'y' for Yes) to the dialog HWND, falling
    back to Enter (the default button). Mirrors the proven
    ``_accept_qmessagebox`` helper in test_ui_page_operations.py — bare
    ``QMessageBox::question`` calls (e.g. onDeletePages' confirm) are not
    covered by the testDialogActionPipe introspection seam, so they must be
    accepted via keystroke. Returns True if the dialog appeared and a key was
    dispatched."""
    hwnd = _find_window_by_title(title_substr, timeout=timeout)
    if hwnd <= 0:
        return False
    try:
        press_to(hwnd, accept_key)
    except Exception:  # noqa: BLE001
        try:
            press_to(hwnd, "enter")
        except Exception:  # noqa: BLE001
            return False
    time.sleep(settle_s)
    return True


def ocr_text(win, settle_s: float = 8.0) -> str:
    """Run OCR on the current page and return the recognized plain text.

    Drives the testOcrTextQueryPipe → testOcrTextResultPipe seam
    (MainWindow::currentPageOcrText). Tesseract can take several seconds, so
    the settle is generous. Returns "" when the pipe reports ``__null__`` or is
    unreachable.
    """
    raw = read_result_pipe(win, "testOcrTextQueryPipe",
                           "testOcrTextResultPipe", "1", settle_s=settle_s)
    if not raw or raw == "__null__":
        return ""
    return raw


# ---------------------------------------------------------------------------
# Assertions / golden management
# ---------------------------------------------------------------------------
def assert_changed(before, after, threshold: float = 0.01,
                   what: str = "canvas") -> None:
    """Assert the canvas visibly changed using the proven OR rule (structural
    SSIM < 0.95 OR pixel-diff fraction >= 0.0001) — the same sensitivity the
    established UI suite uses. *threshold* is retained for signature
    compatibility and used as the pixel-fraction floor."""
    assert ro.canvas_changed(before, after, pixel_threshold=min(threshold, 0.0001)), (
        f"{what} did not change (SSIM >= {ro.SSIM_CHANGED_THRESHOLD} and "
        f"pixel-diff < {ro.PIXEL_DIFF_THRESHOLD}) — the action produced no "
        f"visible render update")


def assert_not_blank(img, blank_threshold: float = 0.98,
                     what: str = "region") -> None:
    assert not ro.region_is_blank(img, blank_threshold=blank_threshold), (
        f"{what} rendered blank (>= {blank_threshold:.0%} one colour)")


def golden_path(name: str) -> pathlib.Path:
    return GOLDENS / name


def assert_matches_golden(img, name: str, tolerance: float = 0.06) -> None:
    """Assert *img* matches the COMMITTED golden ``name``.

    Goldens live under tests/ui_automation/goldens/. A generous default
    tolerance absorbs anti-aliasing / cursor-blink variance between runs.

    Gap A1-68: this no longer seeds a missing golden and reports success. If
    the golden is absent, :class:`render_oracle.GoldenMissingError` propagates
    and the test ERRORS — "no baseline" must never read as "matched baseline".
    Seed deliberately with ``seed_golden(img, name)`` or by re-running the
    capture with ``PDFE_SEED_GOLDENS=1``, then review the PNG before commit.
    """
    p = golden_path(name)
    assert ro.matches_golden(img, p, tolerance=tolerance), (
        f"capture diverged from committed golden {name} "
        f"(> {tolerance:.0%} pixels differ)")


def seed_golden(img, name: str):
    """Explicitly (re)seed the golden ``name`` from *img*. Announced, never
    silent — see :func:`render_oracle.seed_golden`."""
    return ro.seed_golden(img, golden_path(name))
