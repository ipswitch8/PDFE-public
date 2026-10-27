# -*- coding: utf-8 -*-
"""Screenshot capture helpers.

Captures a region of the screen (or the whole canvas widget) and saves the
actual screenshot image to tests/_runtime/baselines_actual/<subsystem>/<test>.png
(GAP-45: gitignored live artifacts, not source-controlled fixtures).

The source-controlled baseline reference images live in
tests/fixtures/ui_screenshots/ and are READ-ONLY at test time.
The manifest at tests/fixtures/ui_screenshots/manifest.json is also
READ-ONLY; a parallel runtime manifest is written to tests/_runtime/.

Usage
-----
    from tests.ui_harness.core.screenshot import capture_canvas, capture_region

    with PDFESession(pdf_path=...) as s:
        img_path = capture_canvas(s.app, subsystem="text_edit", test="baseline")
"""

import json
import os
import time
from pathlib import Path

import pyautogui

# ---------------------------------------------------------------------------
# Path constants
# ---------------------------------------------------------------------------

_HARNESS_DIR = Path(__file__).parent.parent
_REPO_ROOT = _HARNESS_DIR.parent.parent

# Source-controlled reference baseline tree (READ-ONLY at test time).
SCREENSHOTS_DIR = _REPO_ROOT / "tests" / "fixtures" / "ui_screenshots"
MANIFEST_PATH = SCREENSHOTS_DIR / "manifest.json"

# Live per-run artifact tree (gitignored, written by tests).
_RUNTIME_DIR = _REPO_ROOT / "tests" / "_runtime"
_RUNTIME_ACTUAL_DIR = _RUNTIME_DIR / "baselines_actual"
_RUNTIME_MANIFEST_PATH = _RUNTIME_DIR / "screenshot_manifest.json"


def _ensure_dir(path: Path) -> Path:
    path.mkdir(parents=True, exist_ok=True)
    return path


def _load_manifest() -> dict:
    """Load the runtime manifest (tests/_runtime/screenshot_manifest.json)."""
    if _RUNTIME_MANIFEST_PATH.exists():
        with open(str(_RUNTIME_MANIFEST_PATH), encoding="utf-8") as f:
            return json.load(f)
    return {}


def _save_manifest(data: dict):
    """Save to the runtime manifest (gitignored, never touches fixtures/)."""
    _ensure_dir(_RUNTIME_DIR)
    with open(str(_RUNTIME_MANIFEST_PATH), "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)


def _record_screenshot(subsystem: str, test: str, path: str):
    """Update the runtime manifest with a new screenshot entry."""
    data = _load_manifest()
    data.setdefault(subsystem, {})[test] = path
    _save_manifest(data)


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def capture_region(left, top, width, height, subsystem, test, wait=0.0):
    """Capture an arbitrary screen region and save to the runtime directory.

    Actual screenshots are written to tests/_runtime/baselines_actual/
    (GAP-45: gitignored live artifacts).  The source-controlled reference
    baselines under tests/fixtures/ui_screenshots/ are never modified by
    this function.  Use golden.regen_golden() explicitly when you intend
    to update a committed baseline.

    Parameters
    ----------
    left, top       : int  screen coordinates of the capture origin
    width, height   : int  capture dimensions in pixels
    subsystem       : str  e.g. "text_edit", "annotations"
    test            : str  e.g. "baseline", "after_bold"
    wait            : float  seconds to sleep before capturing (allows Qt to repaint)

    Returns
    -------
    str  absolute path to the saved PNG file (under tests/_runtime/)
    """
    if wait > 0:
        time.sleep(wait)

    # Write actuals to the gitignored runtime directory, not fixtures/.
    subdir = _ensure_dir(_RUNTIME_ACTUAL_DIR / subsystem)
    fname = f"{test}.png"
    path = subdir / fname

    try:
        img = pyautogui.screenshot(region=(left, top, width, height))
    except OSError as exc:
        # MEASURED, 2026-09-04, on a real RDP disconnect. Exact timings, since
        # the first write-up of this rounded them into a claim that was wrong:
        #
        #   01:45:52.0   TerminalServices event 24, DISCONNECTED
        #   01:45:54.2   first `OSError: screen grab failed`  (+2.2 s)
        #   01:47:19.0   TerminalServices event 25, RECONNECTED
        #   01:47:32.5   last OSError; grabs work again       (+13.5 s AFTER
        #                                                      the reconnect)
        #
        # 50 consecutive failing samples, 98.2 s. It does NOT return black and
        # does NOT return a frozen frame -- both were plausible, and both would
        # have been far worse, because they hand an oracle a plausible-looking
        # image to reason about.
        #
        # NOTE THE TRAILING 13.5 s. Recovery lags the reconnect, exactly as
        # OpenInputDesktop does (#132 measured 11 s there). Two independent
        # capabilities, same shape: the session reports Active well before it
        # can actually be used. Do not gate a retry on the reconnect event.
        #
        # This IS a screen grab, so it needs an attached display. Win32
        # PrintWindow (tests/visual/capture.py, and the _capture_canvas helpers
        # in the test_ui_* families) asks the WINDOW to render itself instead
        # and keeps working throughout. Evidence: two runs of
        # test_font_size_change_via_toolbar_spinner OVERLAPPING the outage
        # passed with real change detection -- iteration 4 spent 48 of its 50 s
        # inside the failure window, iteration 5 52 of its 60. Neither ran
        # ENTIRELY inside it, which is what the first version of this comment
        # claimed; the overlap is decisive but the wording was not accurate.
        #
        # So the bare OSError is honest, just uninformative. Anyone who hits it
        # deserves to know it is almost never a code defect.
        raise OSError(
            "%s\n\n"
            "CAPTURE DIAGNOSIS: this is pyautogui.screenshot(), a GDI SCREEN "
            "grab, which needs an attached display. Measured cause: an RDP "
            "disconnect (first failure ~2s after the session drops, persisting "
            "until reconnect); a locked or non-interactive session does the "
            "same. It is almost never a defect in the code under test.\n"
            "If this test does not actually need the SCREEN, capture the window "
            "instead -- Win32 PrintWindow keeps working while disconnected. See "
            "tests/visual/capture.py and docs/audit/issue-133/." % exc
        ) from exc
    img.save(str(path))

    _record_screenshot(subsystem, test, str(path))
    return str(path)


def capture_canvas(app, subsystem, test, wait=0.2):
    """Capture the PdfCanvas widget area.

    Parameters
    ----------
    app       : pywinauto.Application
    subsystem : str
    test      : str
    wait      : float  seconds to wait before capture

    Returns
    -------
    str  absolute path to the saved PNG file
    """
    from tests.ui_harness.core.canvas import canvas_rect  # local import to avoid cycles

    r = canvas_rect(app)
    width = r.right - r.left
    height = r.bottom - r.top
    return capture_region(r.left, r.top, width, height, subsystem, test, wait=wait)


def capture_window(app, subsystem, test, wait=0.2):
    """Capture the full PDFE main window.

    Parameters
    ----------
    app       : pywinauto.Application
    subsystem : str
    test      : str
    wait      : float

    Returns
    -------
    str  absolute path to the saved PNG file
    """
    win = app.window(title_re=r".*PDFE.*")
    r = win.rectangle()
    width = r.right - r.left
    height = r.bottom - r.top
    return capture_region(r.left, r.top, width, height, subsystem, test, wait=wait)


def list_screenshots(subsystem=None):
    """Return all screenshot paths recorded in the manifest.

    Parameters
    ----------
    subsystem : str or None  filter by subsystem; None returns everything

    Returns
    -------
    list of str  absolute paths
    """
    data = _load_manifest()
    if subsystem:
        return list(data.get(subsystem, {}).values())
    paths = []
    for sub in data.values():
        paths.extend(sub.values())
    return paths
