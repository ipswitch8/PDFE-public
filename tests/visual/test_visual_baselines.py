# -*- coding: utf-8 -*-
"""Visual-regression demonstration tests (Pipeline a7c2e541 phase-4).

Five baselines are captured against the running PDFE.exe and compared
to committed PNGs under tests/fixtures/screenshots/.

The five required baselines:
    1. ``startup_no_document``      blank canvas, no document loaded
    2. ``document_open_page1``      test_single.pdf page 1, fit-page
    3. ``zoom_in_200pct``           same document at 200% zoom
    4. ``annotations_visible``      test_with_freetext_annot.pdf canvas
    5. ``encryption_dialog_open``   the EncryptionDialog modal

Each test:
    * skips when system DPI is not in the supported set
    * skips when PDFE.exe is missing
    * launches PDFE via the deterministic ``PDFESession`` harness
    * drives to the target state
    * captures the relevant region
    * compares to the committed baseline at SSIM >= 0.95

On failure, a side-by-side ``actual | baseline | diff`` PNG is written
to tests/visual/_failure_artifacts/ to make triage easy.

The capture functions are also exposed via ``BASELINE_PLAN`` so the
companion ``update_baselines.py`` script uses *the same* capture path
that the tests assert against.
"""

from __future__ import annotations

import os
import sys
import time
from pathlib import Path
from typing import Callable, List, Tuple

import pytest
from PIL import Image

# Repo root: tests/visual/ is two levels under it.
_HERE = Path(__file__).resolve().parent
REPO_ROOT = _HERE.parent.parent
sys.path.insert(0, str(REPO_ROOT))

# Local modules
from tests.visual.capture import (  # noqa: E402
    capture_hwnd,
    capture_screen_region,
    detect_system_dpi,
)
from tests.visual.diff import compare, save_diff_visualization  # noqa: E402
from tests.notrun import format_reason, skip_not_run  # noqa: E402

# Existing harness
from tests.ui_harness.core.launch import PDFESession, PDFE_EXE  # noqa: E402
from tests.ui_harness.core.locate import by_auto_id  # noqa: E402


# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

SUPPORTED_DPIS = {96, 120, 144, 192}

# GitHub #279 — these were SSIM_THRESHOLD = 0.95 and PIXEL_THRESHOLD = 0.05
# ("tolerate up to 5% per-pixel deltas (fonts, AA)"). That pairing could not
# see a defacement of the document:
#
#   a SOLID RED BLOCK over 2% of the page gave ssim=0.9721 pf=0.010171 -> PASS
#   the same over 0.5% of the page gave     ssim=0.9930 pf=0.002543 -> PASS
#
# so the oracle tolerated having a fifth of a page-width square painted over it.
#
# The 5% figure was never measured against anything. The actual run-to-run
# variation of these captures, measured with
# `tests/visual/probe_capture_noise_floor.py` (3 consecutive captures of the
# same state, then each against the committed baseline):
#
#   capture1 vs capture2   ssim=1.000000  pixel_fraction=0.000000
#   capture1 vs capture3   ssim=1.000000  pixel_fraction=0.000000
#   capture2 vs capture3   ssim=1.000000  pixel_fraction=0.000000
#   each vs baseline       ssim=1.000000  pixel_fraction=0.000000
#
# The noise floor is **exactly zero** — the capture is deterministic at a fixed
# geometry, so every differing pixel is signal and the old tolerance was pure
# slack. It existed to paper over the geometry and theme coupling that #279 is
# about (the superseded baselines were captured at an 846px-wide canvas; this
# rig produces 1111px), not to absorb measured noise.
#
# Set just above the floor rather than at it, because the floor was measured in
# one session on one rig and cross-DPI resampling is real.
#
# The first attempt at this used 0.99 / 0.001, and
# `test_oracle_thresholds_kill_a_small_in_page_change` immediately failed it:
# a 0.2%-of-page block measures pf=0.000977, which slips under 0.001. That is
# the test doing its job on the person writing it. Tightened to the values
# below, which kill 0.2% (31x31px here) decisively.
#
# RESOLUTION LIMIT, stated rather than implied: this oracle detects a solid
# in-page change of roughly 0.2% of page area and upwards. Something smaller
# than that is below its resolution, and no threshold short of exact equality
# would catch it. Exact equality is tempting given a measured floor of zero, but
# the floor comes from one rig in one session, so the margin stays.
SSIM_THRESHOLD = 0.998
PIXEL_THRESHOLD = 0.0005

BASELINE_DIR = REPO_ROOT / "tests" / "fixtures" / "screenshots"
# GitHub #289: run-scoped, so a later run cannot overwrite the evidence an
# investigation is citing. See output_paths.visual_failure_dir().
from tests.helpers.output_paths import visual_failure_dir as _vfd
FAILURE_DIR = _vfd()

FIXTURE_SINGLE = REPO_ROOT / "tests" / "fixtures" / "test_single.pdf"
FIXTURE_FREETEXT = REPO_ROOT / "tests" / "fixtures" / "test_with_freetext_annot.pdf"


# ---------------------------------------------------------------------------
# Skip-conditions (shared by all tests)
# ---------------------------------------------------------------------------

def _has_pdfe_binary() -> bool:
    return Path(PDFE_EXE).exists()


def _is_interactive_session() -> bool:
    """UIA needs a real interactive Windows desktop session."""
    if os.environ.get("PDFE_UI_FORCE_HEADLESS") == "1":
        return False
    return bool(os.environ.get("SESSIONNAME"))


def _supported_dpi_or_skip() -> int:
    """Gap A1-66: an unsupported DPI means the test did NOT RUN — it is not a
    pass. Routed through :mod:`tests.notrun` so it lands in the NOT-RUN
    report instead of blending into the green skip count."""
    dpi = detect_system_dpi()
    if dpi not in SUPPORTED_DPIS:
        skip_not_run(
            "unsupported-dpi",
            f"system DPI {dpi} is not in the supported set "
            f"{sorted(SUPPORTED_DPIS)}; comparing at this scale would produce "
            f"false-fail diffs",
            remedy=(f"run on a host at one of {sorted(SUPPORTED_DPIS)} DPI, or "
                    f"re-baseline for this scale via update_baselines.py"))
    return dpi


#: Gap A1-66 / A1-83: the reason string is stamped NOT-RUN so a headless lane
#: reports "5 tests produced no evidence" rather than a silent green. The whole
#: module is live-UI-only by construction — these are photographs of the
#: running application and cannot be synthesised. The headlessly-checkable part
#: of the visual set is asserted for real in test_baseline_inventory.py.
REQUIRES_LIVE_UI = pytest.mark.skipif(
    not (_has_pdfe_binary() and _is_interactive_session()),
    reason=format_reason(
        "no-live-ui",
        "visual-baseline capture needs a built PDFE.exe AND an interactive "
        "Windows desktop session (present: "
        f"pdfe.exe={_has_pdfe_binary()}, interactive={_is_interactive_session()})",
        remedy="build PDFE and run from an interactive desktop session, not "
               "Session 0 / a service context",
    ),
)


# ---------------------------------------------------------------------------
# Capture helpers
# ---------------------------------------------------------------------------

def _settle(seconds: float = 0.6) -> None:
    """Sleep so Qt has time to repaint before capture."""
    time.sleep(seconds)


def _window_hwnd(session: PDFESession) -> int:
    return int(session.window.handle)


def _canvas_screen_rect(session: PDFESession) -> Tuple[int, int, int, int]:
    """Return (x, y, w, h) in absolute screen coords for the PdfCanvas widget.

    Falls back to the central area of the main window when the canvas
    cannot be located via UIA (e.g. if the user has hidden it).
    """
    try:
        canvas = by_auto_id(session.window, "pdfCanvas",
                            control_type=None, timeout=5)
        r = canvas.rectangle()
        return r.left, r.top, r.right - r.left, r.bottom - r.top
    except Exception:  # noqa: BLE001
        r = session.window.rectangle()
        # Crop to the central 70% of the main window — heuristic but
        # stable enough for the baseline if the canvas widget id is
        # ever renamed.
        x = r.left + int((r.right - r.left) * 0.15)
        y = r.top + int((r.bottom - r.top) * 0.20)
        w = int((r.right - r.left) * 0.70)
        h = int((r.bottom - r.top) * 0.65)
        return x, y, w, h


def _capture_window_image(session: PDFESession) -> Image.Image:
    """Full PDFE main window."""
    hwnd = _window_hwnd(session)
    return capture_hwnd(hwnd)


def _capture_canvas_image(session: PDFESession) -> Image.Image:
    """The PdfCanvas widget area."""
    x, y, w, h = _canvas_screen_rect(session)
    return capture_screen_region(x, y, w, h)


def _open_pdf(session: PDFESession, path: Path) -> None:
    """Open a PDF via Ctrl+O.

    The PDFESession launcher already supports passing a fixture at
    construction time; that path is preferred where available because
    it avoids the file-open dialog.  This helper is a fallback for
    tests that need to switch documents mid-session.
    """
    import pyautogui
    pyautogui.hotkey("ctrl", "o")
    _settle(0.4)
    pyautogui.typewrite(str(path).replace("/", "\\"), interval=0.01)
    _settle(0.1)
    pyautogui.press("enter")
    _settle(1.0)


def _zoom_to_200(session: PDFESession) -> None:
    """Drive zoom to ~200% via repeated Ctrl+= (zoom-in) keystrokes.

    The default zoom step in PDFE is 25%; six Ctrl+= presses from the
    fit-page baseline lands us above 200%.  We then issue Ctrl+0
    + 8 Ctrl+= to make the result deterministic regardless of the
    starting zoom.
    """
    import pyautogui
    # Reset to fit-width via Ctrl+1 (Actual size = 100%) for determinism.
    try:
        pyautogui.hotkey("ctrl", "1")
        _settle(0.3)
    except Exception:  # noqa: BLE001
        pass
    # Now four Ctrl+= presses: 100% -> 125 -> 150 -> 175 -> 200
    for _ in range(4):
        pyautogui.hotkey("ctrl", "=")
        _settle(0.15)
    _settle(0.5)


def _find_pdfe_top_window_by_title(pid: int, title_substr: str) -> int:
    """Return the first visible top-level HWND in process ``pid`` whose
    window title contains ``title_substr``.

    Returns 0 if not found.  Uses raw EnumWindows because Qt modal
    dialogs do not always appear in the pywinauto Application.windows()
    list under UIA on this Qt6 build.
    """
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    EnumWindows = user32.EnumWindows
    EnumWindows.argtypes = [ctypes.c_void_p, wintypes.LPARAM]
    GetWindowThreadProcessId = user32.GetWindowThreadProcessId
    GetWindowText = user32.GetWindowTextW
    IsWindowVisible = user32.IsWindowVisible

    enum_proto = ctypes.WINFUNCTYPE(
        wintypes.BOOL, wintypes.HWND, wintypes.LPARAM,
    )
    found = [0]

    def _cb(hwnd, _lp):
        win_pid = wintypes.DWORD()
        GetWindowThreadProcessId(hwnd, ctypes.byref(win_pid))
        if win_pid.value == pid and IsWindowVisible(hwnd):
            buf = ctypes.create_unicode_buffer(512)
            GetWindowText(hwnd, buf, 512)
            if title_substr in (buf.value or ""):
                found[0] = int(hwnd)
                return False
        return True

    EnumWindows(enum_proto(_cb), 0)
    return found[0]


def _open_encryption_dialog(session: PDFESession) -> int:
    """Open the EncryptionDialog by sending the Ctrl+Shift+E shortcut.

    Returns the HWND of the dialog window (not a pywinauto wrapper —
    Qt modal dialogs don't always appear in the UIA windows() list on
    this Qt6 build, so we capture by HWND directly).

    The action is enabled only after a document is loaded — the caller
    must open a fixture first.

    The cpp source confirms (src/ui/EncryptionDialog.cpp):
        setWindowTitle("Encryption Settings")
        setObjectName("encryptionDialog")
    """
    from pywinauto.keyboard import send_keys

    # Make sure the main window has focus so the shortcut reaches it.
    try:
        session.window.set_focus()
        _settle(0.3)
    except Exception:  # noqa: BLE001
        pass

    # Send Ctrl+Shift+E.  pywinauto's send_keys delivers via SendInput
    # which is more reliable in remote-desktop sessions than pyautogui.
    send_keys("^+e")
    _settle(1.5)

    # Locate the dialog by its title.
    deadline = time.monotonic() + 8.0
    while time.monotonic() < deadline:
        hwnd = _find_pdfe_top_window_by_title(session.pid, "Encryption Settings")
        if hwnd:
            return hwnd
        time.sleep(0.25)

    raise RuntimeError(
        "EncryptionDialog ('Encryption Settings' window) did not appear "
        "within 8s of Ctrl+Shift+E."
    )


def _close_dialog_by_hwnd(hwnd: int) -> None:
    """Send WM_CLOSE to a top-level window."""
    if not hwnd:
        return
    import ctypes
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    WM_CLOSE = 0x0010
    try:
        user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
    except Exception:  # noqa: BLE001
        pass


# ---------------------------------------------------------------------------
# Capture-function library
# ---------------------------------------------------------------------------
#
# Each entry returns a PIL.Image representing what should be compared
# to the committed baseline of the same name.  These functions are used
# by *both* the pytest tests and the update_baselines.py script.
#
# Implementations launch and tear down PDFE.exe themselves so the script
# can refresh baselines individually without holding a session across
# calls.
# ---------------------------------------------------------------------------

def capture_startup_no_document() -> Image.Image:
    with PDFESession(pdf_path=None) as session:
        _settle(1.5)
        return _capture_window_image(session)


def capture_document_open_page1() -> Image.Image:
    with PDFESession(pdf_path=str(FIXTURE_SINGLE)) as session:
        _settle(2.0)
        return _capture_canvas_image(session)


def capture_zoom_in_200pct() -> Image.Image:
    with PDFESession(pdf_path=str(FIXTURE_SINGLE)) as session:
        _settle(2.0)
        _zoom_to_200(session)
        _settle(1.0)
        return _capture_canvas_image(session)


def capture_annotations_visible() -> Image.Image:
    if not FIXTURE_FREETEXT.exists():
        pytest.skip(f"freetext fixture missing: {FIXTURE_FREETEXT}")
    with PDFESession(pdf_path=str(FIXTURE_FREETEXT)) as session:
        _settle(2.0)
        return _capture_canvas_image(session)


def capture_encryption_dialog_open() -> Image.Image:
    """Open EncryptionDialog (requires a document) and capture it."""
    import ctypes
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    with PDFESession(pdf_path=str(FIXTURE_SINGLE)) as session:
        _settle(1.8)
        dlg_hwnd = _open_encryption_dialog(session)
        _settle(0.6)
        try:
            # Get window screen rect for capture.
            from ctypes import wintypes
            class RECT(ctypes.Structure):
                _fields_ = [
                    ("left", wintypes.LONG), ("top", wintypes.LONG),
                    ("right", wintypes.LONG), ("bottom", wintypes.LONG),
                ]
            r = RECT()
            user32.GetWindowRect(dlg_hwnd, ctypes.byref(r))
            w, h = r.right - r.left, r.bottom - r.top
            return capture_screen_region(r.left, r.top, w, h)
        finally:
            # Close the modal so the session teardown can exit cleanly.
            _close_dialog_by_hwnd(dlg_hwnd)
            _settle(0.5)


# ---------------------------------------------------------------------------
# Plan exposed to update_baselines.py
# ---------------------------------------------------------------------------

BASELINE_PLAN: List[Tuple[str, Callable[[], Image.Image]]] = [
    ("startup_no_document",     capture_startup_no_document),
    ("document_open_page1",     capture_document_open_page1),
    ("zoom_in_200pct",          capture_zoom_in_200pct),
    ("annotations_visible",     capture_annotations_visible),
    ("encryption_dialog_open",  capture_encryption_dialog_open),
]


# ---------------------------------------------------------------------------
# Pytest test cases
# ---------------------------------------------------------------------------

def _validate_capture_not_blank(img: Image.Image, name: str) -> None:
    """Sanity-check the capture isn't a black or empty rectangle.

    karen (the Pipeline F gate-1 reality-check agent) explicitly looks
    for "black rectangles or wrong-window captures" — surface those as
    test failures rather than as a passed-but-wrong SSIM score.
    """
    if img.size[0] < 16 or img.size[1] < 16:
        pytest.fail(f"capture {name!r}: degenerate size {img.size}")
    # Sample-based black/uniform check
    grey = img.convert("L")
    extrema = grey.getextrema()
    if extrema[0] == extrema[1]:
        pytest.fail(
            f"capture {name!r}: uniform image (extrema={extrema}), "
            f"likely a wrong-window or black-rectangle capture."
        )


def _run_baseline_check(name: str, actual: Image.Image) -> None:
    """Compare ``actual`` to the committed baseline ``name``.

    Saves a diff visualization to FAILURE_DIR on failure.
    """
    _validate_capture_not_blank(actual, name)
    baseline_path = BASELINE_DIR / f"{name}.png"
    if not baseline_path.exists():
        # Gap A1-66/A1-68: an absent baseline is NOT a pass and must never be
        # self-seeded from the current capture — that promotes whatever this
        # run happened to render into the defended reference.
        skip_not_run(
            "baseline-not-seeded",
            f"no committed baseline at {baseline_path}; nothing was compared",
            remedy="capture it deliberately via tests/visual/update_baselines.py "
                   "and review the PNG before committing")
    baseline = Image.open(str(baseline_path)).convert("RGB")

    res = compare(
        actual, baseline,
        ssim_threshold=SSIM_THRESHOLD,
        pixel_threshold=PIXEL_THRESHOLD,
    )

    if not res["pass"]:
        FAILURE_DIR.mkdir(parents=True, exist_ok=True)
        diff_path = FAILURE_DIR / f"{name}_diff.png"
        save_diff_visualization(actual, baseline, diff_path)
        pytest.fail(
            f"Visual regression for {name!r}: "
            f"SSIM={res['ssim']:.4f} (>= {SSIM_THRESHOLD}) "
            f"pixel_fraction={res['pixel_fraction']:.4f} "
            f"(<= {PIXEL_THRESHOLD}); "
            f"diff -> {diff_path}"
        )

    # Always at least assert the SSIM threshold for explicit AC compliance.
    assert res["ssim"] >= SSIM_THRESHOLD, (
        f"SSIM {res['ssim']:.4f} < {SSIM_THRESHOLD} for {name!r}"
    )


# ---------------------------------------------------------------------------
# GitHub #279 — the blanket xfail is gone. Each baseline now carries its own
# MEASURED classification.
#
# The marker these replace read:
#
#     "baseline drift from rendering / Qt theme / OS DPI variations since
#      baselines were captured. SSIM 0.27-0.67 vs 0.95 threshold.
#      NOT a regression from any product pipeline."
#
# That last sentence was a CLAIM, not a measurement, and #279 exists because it
# is exactly the claim an SSIM number cannot support: `PdfCanvas::paintEvent`
# fills the viewport backdrop from `palette().color(QPalette::Dark)`, so a theme
# change and a rendering regression both collapse a full-frame SSIM.
#
# It was also `strict=False`, so a baseline that started matching produced an
# XPASS rather than a signal. These five could neither redden nor announce that
# they had come good: they asserted nothing in either direction.
#
# TELLING THEM APART takes two different measurements, because two different
# kinds of capture are involved (`audit_baseline_drift.py`):
#
#   * a CANVAS capture is judged on its page-content crop, compared separately
#     from the full frame. Page identical + frame different = stale chrome.
#   * a WINDOW or DIALOG capture is palette-derived end to end, so colour says
#     nothing. It is judged on STRUCTURE: thresholded Sobel edge maps, which are
#     invariant to a recolour (inverting every pixel leaves the edge map
#     IDENTICAL — measured 1.0000) but collapse when widgets move.
#
# Measured 2026-10-15 against a freshly-built pdfe.exe
# (`docs/audit/p97-phase8/i279-baseline-audit.json`):
#
#   document_open_page1     MATCHES               1.0000/0.0000, page 1.0000/0.0000
#   encryption_dialog_open  STALE-BASELINE        0.8951/0.0157, structure IoU 0.9518
#   startup_no_document     REGRESSION-CANDIDATE  0.5654/0.2850, structure IoU 0.0298
#   zoom_in_200pct          REGRESSION-CANDIDATE  0.7210/0.0088, page crop 31.3% wider
#   annotations_visible     REGRESSION-CANDIDATE  0.6854/0.1824, page 0.9775/0.0139
#
# TWO baselines were regenerated — `document_open_page1` and
# `encryption_dialog_open`, the two classified stale — and both have their xfail
# removed so the oracles actually assert. The remaining three keep a marker
# stating WHICH situation they are in and the number behind it.
#
# The two chrome captures SPLIT, which is why the structural measure was worth
# building: the dialog's layout is unchanged (0.9518, inside the palette-only
# band) so its baseline was simply captured under another theme; the main
# window's is not (0.0298 — as low as a deliberate 12px shift of the whole
# window), so that is a real change and must not be re-seeded.
#
# A NOTE ON WHAT THIS COMMENT USED TO SAY, because it is the same defect class
# the phase is about. An earlier version of this block listed both chrome
# captures as `CHROME-ONLY-ORACLE` and stated "only `document_open_page1` was
# regenerated". Both were false by the time it was read: the first is a verdict
# this classifier no longer has (phase-8's validator gate rejected it as a third
# bucket outside criterion 1's stale-or-regression dichotomy), and the second
# undercounted the regenerations by one. The code three lines below was correct
# throughout; only the prose was stale. The validator gate failed the phase for
# it, which is the right outcome — a false claim about a measurement, sitting in
# the deliverable, is exactly what this phase exists to remove.
# ---------------------------------------------------------------------------

#: A whole-window capture whose LAYOUT has genuinely changed, not just its
#: colours.
#:
#: This replaces an earlier `_CHROME_ONLY_ORACLE_XFAIL` which said these
#: captures "cannot be judged" because they are palette-derived end to end.
#: Phase-8's validator gate failed the phase for that: criterion 1 requires
#: every mismatch classified as stale-baseline OR real-regression, and
#: "unjudgeable" is a third bucket the criterion does not provide for. The gate
#: was right, and the question turned out to be answerable — just not from
#: colour.
#:
#: `audit_baseline_drift.py::structural_agreement` compares thresholded Sobel
#: edge maps, which are invariant to a recolour (inverting every pixel leaves
#: the edge map IDENTICAL — measured 1.0000) but collapse when widgets move.
#: Measured for this capture: **IoU 0.0298**, versus 0.9949+ for a pure palette
#: change and 0.0701 for a deliberate 12px shift of the whole window.
#:
#: So this is NOT theme drift. The main window's layout differs from its
#: baseline about as much as a wholesale shift would make it. Deliberately not
#: regenerated: re-seeding it would promote whatever the window looks like now
#: into the defended reference without anyone establishing that the change was
#: intended.
_WINDOW_LAYOUT_CHANGED_XFAIL = pytest.mark.xfail(
    reason=(
        "GitHub #279, measured: the main window's LAYOUT differs from this "
        "baseline, not merely its palette -- palette-invariant edge-map "
        "agreement is 0.0298, against 0.9949+ for a pure recolour and 0.0701 "
        "for a deliberate 12px shift of the whole window. So this is a real "
        "change in the window, not theme drift, and it must NOT be "
        "regenerated until the change is established as intended. Needs the "
        "difference identified."
    ),
    strict=False,
)

#: The page content itself differs, so this baseline must NOT be regenerated:
#: doing so would promote a possible rendering regression into the defended
#: reference. Classified by `tests/visual/audit_baseline_drift.py` and localised
#: by `tests/visual/probe_candidate_diff_shape.py`.
_PAGE_CONTENT_DIFFERS_XFAIL = pytest.mark.xfail(
    reason=(
        "GitHub #279, measured: the PAGE CONTENT differs, not just the "
        "palette-derived chrome, with the diff confined to a horizontal band "
        "(3.9%-7.0% of rows, spread across 35%-47% of columns) -- the "
        "signature of a text line rendering differently. Deliberately NOT "
        "regenerated: re-seeding a baseline whose document content differs "
        "would bake a possible regression into the reference. Needs the band "
        "identified before this can be resolved either way."
    ),
    strict=False,
)


@_WINDOW_LAYOUT_CHANGED_XFAIL
@REQUIRES_LIVE_UI
def test_baseline_startup_no_document():
    _supported_dpi_or_skip()
    img = capture_startup_no_document()
    _run_baseline_check("startup_no_document", img)


# NO xfail: classified STALE-BASELINE, regenerated 2026-10-15 from a
# freshly-built pdfe.exe, and therefore required to actually assert.
@REQUIRES_LIVE_UI
def test_baseline_document_open_page1():
    _supported_dpi_or_skip()
    if not FIXTURE_SINGLE.exists():
        pytest.skip(f"Fixture missing: {FIXTURE_SINGLE}")
    img = capture_document_open_page1()
    _run_baseline_check("document_open_page1", img)


@_PAGE_CONTENT_DIFFERS_XFAIL
@REQUIRES_LIVE_UI
def test_baseline_zoom_in_200pct():
    _supported_dpi_or_skip()
    if not FIXTURE_SINGLE.exists():
        pytest.skip(f"Fixture missing: {FIXTURE_SINGLE}")
    img = capture_zoom_in_200pct()
    _run_baseline_check("zoom_in_200pct", img)


@_PAGE_CONTENT_DIFFERS_XFAIL
@REQUIRES_LIVE_UI
def test_baseline_annotations_visible():
    _supported_dpi_or_skip()
    if not (FIXTURE_FREETEXT.exists() or FIXTURE_SINGLE.exists()):
        pytest.skip("No annotation fixture available.")
    img = capture_annotations_visible()
    _run_baseline_check("annotations_visible", img)


# NO xfail: classified STALE-BASELINE by structural agreement
# (palette-invariant edge-map IoU 0.9518 -- same dialog layout, different
# palette), regenerated 2026-10-15 from a freshly-built pdfe.exe, and
# therefore required to actually assert.
@REQUIRES_LIVE_UI
def test_baseline_encryption_dialog_open():
    _supported_dpi_or_skip()
    if not FIXTURE_SINGLE.exists():
        pytest.skip(f"Fixture missing: {FIXTURE_SINGLE}")
    img = capture_encryption_dialog_open()
    _run_baseline_check("encryption_dialog_open", img)


# ---------------------------------------------------------------------------
# Self-tests of the harness itself (run regardless of live-UI availability)
# ---------------------------------------------------------------------------

def test_oracle_thresholds_kill_a_small_in_page_change():
    """GitHub #279 — the thresholds must be able to FAIL on a real change.

    This is the direction a visual oracle almost never checks about itself, and
    the one that matters: an oracle is only worth its runtime if some change
    reddens it. The previous pairing (SSIM 0.95 / pixel 0.05) tolerated a solid
    block over **2% of the page** — measured at ssim=0.9721, pf=0.010171, a
    comfortable pass.

    Headless by construction: it mutates the committed baseline PNG in memory
    and runs the same `compare()` the live oracle runs, so it executes on every
    lane rather than only where a desktop exists.
    """
    import numpy as np

    baseline_path = BASELINE_DIR / "document_open_page1.png"
    if not baseline_path.exists():
        pytest.skip(f"no committed baseline at {baseline_path}")

    baseline = Image.open(str(baseline_path)).convert("RGB")

    # A baseline must match itself, or everything below is meaningless.
    same = compare(baseline, baseline, ssim_threshold=SSIM_THRESHOLD,
                   pixel_threshold=PIXEL_THRESHOLD)
    assert same["pass"], (
        f"the committed baseline does not compare equal to itself "
        f"(ssim={same['ssim']:.6f} pf={same['pixel_fraction']:.6f}); the "
        f"comparison itself is broken")

    # Locate the page so the mutation lands on the document, not the margin --
    # a block painted into the grey surround would test nothing about the page.
    arr = np.asarray(baseline, dtype=np.int16)
    white = (arr >= 230).all(axis=-1)
    assert white.any(), "no near-white page region found in the baseline"
    ys, xs = np.nonzero(white)
    y0, y1, x0, x1 = int(ys.min()), int(ys.max()), int(xs.min()), int(xs.max())
    page_h, page_w = y1 - y0, x1 - x0
    assert page_h > 50 and page_w > 50, (
        f"implausible page region {page_w}x{page_h}; the mutation would not "
        f"land on the document")

    for frac in (0.02, 0.005, 0.002):
        mutant = np.array(baseline).copy()
        side = max(4, int((frac * page_h * page_w) ** 0.5))
        cy = y0 + (page_h - side) // 2
        cx = x0 + (page_w - side) // 2
        mutant[cy:cy + side, cx:cx + side] = (255, 0, 0)
        res = compare(Image.fromarray(mutant), baseline,
                      ssim_threshold=SSIM_THRESHOLD,
                      pixel_threshold=PIXEL_THRESHOLD)
        assert not res["pass"], (
            f"a solid block over {frac * 100:.1f}% of the page "
            f"({side}x{side}px) PASSED the oracle at ssim={res['ssim']:.4f} "
            f"pf={res['pixel_fraction']:.6f} against thresholds "
            f"ssim>={SSIM_THRESHOLD} pf<={PIXEL_THRESHOLD}. An oracle that "
            f"cannot see this is not protecting the rendering.")


def test_dpi_detection_returns_positive_integer():
    """Smoke test: detect_system_dpi() returns a sensible value."""
    dpi = detect_system_dpi()
    assert isinstance(dpi, int)
    assert 48 <= dpi <= 1200, f"Improbable DPI: {dpi}"


def test_compare_identical_images_passes():
    """compare() on identical images yields SSIM=1.0 and pass=True."""
    img = Image.new("RGB", (64, 64), color=(120, 80, 200))
    res = compare(img, img.copy())
    assert res["pass"] is True
    assert res["ssim"] >= 0.999
    assert res["pixel_fraction"] == 0.0


def test_compare_different_images_fails():
    """compare() on a black vs white image yields a clear failure."""
    a = Image.new("RGB", (64, 64), color=(0, 0, 0))
    b = Image.new("RGB", (64, 64), color=(255, 255, 255))
    res = compare(a, b, ssim_threshold=0.95, pixel_threshold=0.02)
    assert res["pass"] is False
    assert res["pixel_fraction"] > 0.5


def test_diff_visualization_writes_png(tmp_path):
    """save_diff_visualization writes a non-empty PNG."""
    a = Image.new("RGB", (32, 32), color=(0, 200, 0))
    b = Image.new("RGB", (32, 32), color=(0, 100, 0))
    out = tmp_path / "diff.png"
    save_diff_visualization(a, b, out)
    assert out.exists() and out.stat().st_size > 0
    loaded = Image.open(str(out))
    assert loaded.size[0] == 32 * 3  # three panels
