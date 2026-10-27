# -*- coding: utf-8 -*-
"""Render oracle — pixel-level assertions over the live PDFE canvas.

Pipeline e7d3c2a1 (Phase 2). PDFE's ~3940 GoogleTests assert the *model*,
*byte*, and *extraction* layers; none assert that an edit actually shows up as
*pixels on the rendered canvas*. That gap is exactly what let the
"Invoice → Quote goes blank" (#1) and "Ctrl-Z does not revert" defects ship.
This module closes it: it captures the real PdfCanvas bitmap and answers four
questions — did anything change, is the region blank, does it match a committed
golden, and (for proof tests) does a known defect reproduce.

Design constraints
==================

* **PyAutoGUI for pixels, pywinauto (UIA) only for geometry.** We never call
  raw UIA ``FindAll`` on a modal dialog (GAP-A14: that hangs indefinitely on
  the Windows-Server-2019 / RDP CI session). Geometry resolution reuses the
  canvas auto_id lookup in :mod:`canvas_click`, which is a single
  ``child_window(...).rectangle()`` — not a tree enumeration.
* **Importable headless.** All of PyAutoGUI / Pillow / pywinauto are imported
  under ``try`` guards so this module imports on a machine with no display and
  no deps; the capture functions raise a clear ``RuntimeError`` only when
  actually called.
* **No orphan processes.** :class:`RenderOracle` kills ``pdfe.exe`` two ways on
  exit (``app.kill()`` + ``taskkill``), same contract as
  :class:`PdfeAppFixture`.

Public API (exact signatures required by the pipeline acceptance criteria)
==========================================================================

``capture_canvas(hwnd) -> PIL.Image``
``images_differ(a, b, threshold=0.02) -> bool``
``region_is_blank(img, blank_threshold=0.98) -> bool``
``matches_golden(img, golden_path, tolerance=0.05, seed_if_missing=None) -> bool``
``save_golden(img, golden_path)``
``seed_golden(img, golden_path) -> Path`` — explicit, announced baseline seeding
``RenderOracle`` — context manager wrapping launch/capture/teardown.
"""
from __future__ import annotations

import subprocess
import time
from pathlib import Path
from typing import Optional, Union

# --- Guarded optional deps -------------------------------------------------
try:
    import pyautogui  # type: ignore
    pyautogui.FAILSAFE = False  # CI cursor starts at (0,0); see canvas_click
    _PYAUTOGUI = True
except Exception:  # noqa: BLE001 — ImportError or display-less init failure
    _PYAUTOGUI = False

try:
    from PIL import Image, ImageChops  # type: ignore
    _PIL = True
except Exception:  # noqa: BLE001
    _PIL = False

# Win32 PrintWindow capture — the mechanism the whole proven UI suite uses. On
# this headless/RDP Server-2019 rig a PyAutoGUI desktop screen-grab reads a
# stale/misaligned framebuffer (it sees the cursor move but NOT the window's
# actual re-render), so a real edit looks like "no change". PrintWindow grabs
# the window's own device context and always reflects the live render.
try:
    from tests.visual.capture import capture_hwnd as _capture_hwnd  # type: ignore
    _PRINTWINDOW = True
except Exception:  # noqa: BLE001
    _capture_hwnd = None  # type: ignore
    _PRINTWINDOW = False

# Mark this process PER_MONITOR_AWARE_V2 before any geometry query — this rig
# runs ~162% non-standard scaling and without it pywinauto rectangles and the
# Win32 capture/cursor APIs disagree. Idempotent; render_drive does this too.
try:
    import ctypes as _ctypes_dpi
    try:
        _ctypes_dpi.windll.user32.SetProcessDpiAwarenessContext(-4)
    except Exception:  # noqa: BLE001
        try:
            _ctypes_dpi.windll.shcore.SetProcessDpiAwareness(2)
        except Exception:  # noqa: BLE001
            _ctypes_dpi.windll.user32.SetProcessDPIAware()
except Exception:  # noqa: BLE001
    pass

# Reuse the canvas geometry + diff helpers already proven on this rig.
try:
    from tests.ui_automation.harness.canvas_click import (
        canvas_region as _canvas_region,
        pixel_diff_ratio as _pixel_diff_ratio,
    )
except Exception:  # noqa: BLE001 — allow flat import too (sys.path on harness)
    try:
        from canvas_click import (  # type: ignore
            canvas_region as _canvas_region,
            pixel_diff_ratio as _pixel_diff_ratio,
        )
    except Exception:  # noqa: BLE001
        _canvas_region = None  # type: ignore
        _pixel_diff_ratio = None  # type: ignore


# Proven structural-similarity + pixel-fraction metrics (tests/visual/diff.py).
# The whole UI suite calls "changed" when SSIM < 0.95 OR pixel-fraction >=
# 0.0001 — a thin ink stroke barely moves the pixel fraction but clearly drops
# SSIM, so a coarse pixel-only threshold misses it.
try:
    from tests.visual.diff import ssim as _ssim, pixel_diff as _pixel_diff
    _DIFF_METRICS = True
except Exception:  # noqa: BLE001
    _ssim = None  # type: ignore
    _pixel_diff = None  # type: ignore
    _DIFF_METRICS = False

#: Proven change-detection constants (mirrors test_ui_annotations.py).
SSIM_CHANGED_THRESHOLD = 0.95
PIXEL_DIFF_THRESHOLD = 0.0001


def canvas_changed(before, after,
                   ssim_threshold: float = SSIM_CHANGED_THRESHOLD,
                   pixel_threshold: float = PIXEL_DIFF_THRESHOLD) -> bool:
    """True if *after* visibly differs from *before* using the proven OR rule:
    structural SSIM < *ssim_threshold* OR pixel-diff fraction >= *pixel_threshold*.

    Falls back to the coarse pixel-diff ratio when the metric module is
    unavailable. PrintWindow captures are cursor-free, so the low pixel
    threshold does not false-positive on a static canvas."""
    _require(_PIL, "Pillow")
    if _DIFF_METRICS and _ssim is not None and _pixel_diff is not None:
        s = _ssim(before, after)
        frac, _vis = _pixel_diff(before, after, threshold_per_pixel=10)
        return (s < ssim_threshold) or (frac >= pixel_threshold)
    return images_differ(before, after, threshold=pixel_threshold)


def _require(flag: bool, what: str) -> None:
    if not flag:
        raise RuntimeError(
            f"{what} is required for render-oracle capture "
            f"(this machine is missing it or is headless)"
        )


# --------------------------------------------------------------------------
# Geometry
# --------------------------------------------------------------------------
def _window_rect_from_hwnd(hwnd: int):
    """Return ``(x, y, w, h)`` for a raw Win32 HWND, or ``None``.

    Uses ``GetWindowRect`` directly — no UIA, so GAP-A14-safe. Only used as the
    fallback when an integer HWND (rather than a pywinauto wrapper) is passed to
    :func:`capture_canvas`.
    """
    try:
        import ctypes
        from ctypes import wintypes
        rect = wintypes.RECT()
        ok = ctypes.windll.user32.GetWindowRect(
            wintypes.HWND(hwnd), ctypes.byref(rect))
        if not ok:
            return None
        return (rect.left, rect.top,
                rect.right - rect.left, rect.bottom - rect.top)
    except Exception:  # noqa: BLE001
        return None


def _resolve_pdfe_hwnd(target) -> int:
    """Resolve the real PDFE main-window HWND from *target* (pywinauto wrapper
    or int), enumerating top-level windows as a fallback."""
    hwnd = 0
    if isinstance(target, int):
        hwnd = target
    else:
        try:
            hwnd = int(target.handle)
        except Exception:  # noqa: BLE001
            hwnd = 0
    if hwnd and hwnd > 0:
        return int(hwnd)
    # Enumerate top-level visible windows titled "PDFE" not owned by us.
    try:
        import ctypes
        from ctypes import wintypes
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        proto = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_long)
        import os
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
    except Exception:  # noqa: BLE001
        return 0


def _window_outer_rect(target, hwnd: int):
    """Return the window's outer rect ``(x, y, w, h)`` in screen pixels.

    Prefers the pywinauto wrapper's ``rectangle()`` (matches how the canvas
    rect is resolved), falling back to ``GetWindowRect`` on the HWND."""
    if not isinstance(target, int):
        try:
            r = target.rectangle()
            return (r.left, r.top, r.right - r.left, r.bottom - r.top)
        except Exception:  # noqa: BLE001
            pass
    return _window_rect_from_hwnd(hwnd)


def _capture_origin_screen(target, hwnd: int):
    """Screen coords of the origin of what ``capture_hwnd`` returns.

    That is the CLIENT origin, because ``capture_hwnd`` passes PW_CLIENTONLY
    (GitHub #279). Resolved with ``ClientToScreen(hwnd, (0, 0))``, which asks
    Windows directly rather than deriving it from the window rect plus an
    assumed border and title-bar height -- the hardcoded-32px approach that had
    to be removed from ``_capture_dialog_client`` for the same reason.

    Falls back to the window outer rect only if ``ClientToScreen`` is
    unavailable, and that fallback is WRONG by the non-client offset; it exists
    so a missing pywin32 degrades to the old behaviour rather than crashing,
    and the caller's screen-grab fallback covers the rest.
    """
    try:
        import win32gui
        cx, cy = win32gui.ClientToScreen(int(hwnd), (0, 0))
        return (int(cx), int(cy))
    except Exception:  # noqa: BLE001
        rect = _window_outer_rect(target, hwnd)
        return None if rect is None else (rect[0], rect[1])


def _resolve_region(target):
    """Resolve the canvas screen rect ``(x, y, w, h)`` from *target*.

    *target* may be a pywinauto main-window wrapper (preferred — we read the
    PdfCanvas child rect via :func:`canvas_click.canvas_region`) or a raw int
    HWND (we fall back to that window's full rect). Returns ``None`` if neither
    path resolves.
    """
    # pywinauto wrapper path: get the precise canvas child rect.
    if _canvas_region is not None and not isinstance(target, int):
        region = _canvas_region(target)
        if region is not None:
            return region
    # Raw HWND path.
    if isinstance(target, int):
        return _window_rect_from_hwnd(target)
    # Wrapper but canvas lookup failed — try its own rectangle().
    try:
        r = target.rectangle()
        return (r.left, r.top, r.right - r.left, r.bottom - r.top)
    except Exception:  # noqa: BLE001
        return None


# --------------------------------------------------------------------------
# Core oracle functions
# --------------------------------------------------------------------------
def capture_canvas(hwnd):
    """Capture the PdfCanvas region and return a ``PIL.Image`` (RGB).

    :param hwnd: a pywinauto main-window wrapper (preferred) or a raw int HWND.
    :raises RuntimeError: if Pillow is unavailable or the canvas region cannot
        be resolved.

    Primary path is Win32 **PrintWindow** over the PDFE window DC (cropped to
    the canvas child rect), because a desktop screen-grab is unreliable on this
    headless/RDP rig — it reads a stale framebuffer that does not reflect the
    window's live re-render, so genuine edits read as "no pixel change". The
    PyAutoGUI desktop grab is kept only as a last-resort fallback.
    """
    _require(_PIL, "Pillow")
    region = _resolve_region(hwnd)
    if region is None:
        raise RuntimeError(
            "could not resolve PdfCanvas region for capture "
            "(pass the pywinauto main-window wrapper or a valid HWND)")
    x, y, w, h = region
    if w <= 0 or h <= 0:
        raise RuntimeError(f"degenerate canvas region {region}")

    # --- Primary: PrintWindow over the window DC, cropped to the canvas. -----
    if _PRINTWINDOW and _capture_hwnd is not None:
        real_hwnd = _resolve_pdfe_hwnd(hwnd)
        origin = _capture_origin_screen(hwnd, real_hwnd)
        if real_hwnd > 0 and origin is not None:
            try:
                full = _capture_hwnd(real_hwnd).convert("RGB")
                fw, fh = full.size
                # CLIENT origin, not the window outer rect (GitHub #279).
                # `capture_hwnd` now passes PW_CLIENTONLY, so its image starts
                # at the CLIENT origin; subtracting the window origin here
                # would leave every crop short by the non-client offset
                # (measured dx=8 dy=31 on this rig). This site previously
                # subtracted the window rect and was CORRECT then, because the
                # capture really did start at the window origin -- it was
                # compensating for the bug, so fixing the bug without fixing
                # this would move the error rather than remove it.
                cx = max(0, x - origin[0])
                cy = max(0, y - origin[1])
                cw = max(1, min(w, fw - cx))
                ch = max(1, min(h, fh - cy))
                if cw >= 50 and ch >= 50:
                    return full.crop((cx, cy, cx + cw, cy + ch))
            except Exception:  # noqa: BLE001
                pass  # fall through to the screen-grab fallback

    # --- Fallback: PyAutoGUI desktop screen-grab. ----------------------------
    _require(_PYAUTOGUI, "pyautogui")
    img = pyautogui.screenshot(region=(x, y, w, h))
    return img.convert("RGB")


def images_differ(a, b, threshold: float = 0.02) -> bool:
    """True when the fraction of differing pixels between *a* and *b* exceeds
    *threshold* (default 2%). Different-sized images count as differing.
    """
    _require(_PIL, "Pillow")
    if _pixel_diff_ratio is not None:
        return _pixel_diff_ratio(a, b) > threshold
    # Fallback if canvas_click was unavailable.
    if a.size != b.size:
        return True
    diff = ImageChops.difference(a.convert("RGB"), b.convert("RGB"))
    bbox = diff.getbbox()
    if bbox is None:
        return False
    total = a.size[0] * a.size[1]
    changed = sum(1 for px in diff.crop(bbox).getdata()
                  if px[0] or px[1] or px[2])
    return (changed / total) > threshold


def region_is_blank(img, blank_threshold: float = 0.98) -> bool:
    """True when *img* is essentially a flat fill (e.g. a white/empty region).

    "Blank" = at least *blank_threshold* of pixels are within a small tolerance
    of the single most-common colour. This catches the #1 defect signature: an
    edited heading that renders to nothing leaves the region a near-uniform
    background. A normal page region (text + lines) has well under 98% of its
    pixels on one colour, so it is *not* blank.
    """
    _require(_PIL, "Pillow")
    rgb = img.convert("RGB")
    pixels = list(rgb.getdata())
    total = len(pixels)
    if total == 0:
        return True
    # Most-common exact colour (the presumed background).
    from collections import Counter
    bg, _bg_count = Counter(pixels).most_common(1)[0]
    tol = 12  # per-channel tolerance to absorb anti-aliasing / JPEG noise
    near = 0
    for (r, g, b) in pixels:
        if abs(r - bg[0]) <= tol and abs(g - bg[1]) <= tol and \
                abs(b - bg[2]) <= tol:
            near += 1
    return (near / total) >= blank_threshold


def save_golden(img, golden_path: Union[str, Path]) -> None:
    """Persist *img* as the golden PNG at *golden_path* (creating parents)."""
    _require(_PIL, "Pillow")
    p = Path(golden_path)
    p.parent.mkdir(parents=True, exist_ok=True)
    img.convert("RGB").save(str(p), format="PNG")


# --------------------------------------------------------------------------
# Golden seeding policy (gap A1-68 / UI-ORACLE-02)
# --------------------------------------------------------------------------
# HISTORY — the defect this replaces: ``matches_golden`` used to do
#
#     if not p.exists():
#         save_golden(img, p)
#         return True
#
# i.e. on a missing golden it silently wrote whatever it had just captured and
# reported PASS *having compared nothing*. Two consequences, both observed in
# practice: (1) a broken first capture (black rectangle, wrong window, a page
# that failed to render) becomes the committed baseline that every later run
# defends; (2) a test whose golden was never committed is permanently green
# because run #1 seeds it and run #2 compares it against itself.
#
# The policy now: a missing golden is an ERROR by default. Seeding is an
# explicit, deliberate act — either pass ``seed_if_missing=True`` from a
# seeding entry point, call :func:`seed_golden` directly, or set the
# environment variable ``PDFE_SEED_GOLDENS=1`` for a whole seeding run. Every
# seed is announced on stderr AND appended to :data:`SEEDED_GOLDENS` so a run
# that seeded can never be mistaken for a run that verified.

#: Environment variable enabling whole-run golden seeding.
SEED_ENV_VAR = "PDFE_SEED_GOLDENS"

#: Paths seeded (not verified) during this process, in order. A caller/CI step
#: can assert this is empty to prove a run VERIFIED rather than SEEDED.
SEEDED_GOLDENS: list = []


class GoldenMissingError(RuntimeError):
    """Raised when a golden does not exist and seeding was not requested.

    This is deliberately an error rather than a ``False`` return: "no baseline
    exists" is a different fact from "the capture does not match the baseline",
    and collapsing the two is how the auto-seed defect hid.
    """


def seeding_enabled() -> bool:
    """True when the ``PDFE_SEED_GOLDENS`` environment variable is truthy."""
    import os as _os
    return _os.environ.get(SEED_ENV_VAR, "").strip().lower() in (
        "1", "true", "yes", "on")


def seed_golden(img, golden_path: Union[str, Path]) -> Path:
    """Explicit seeding entry point: write *img* as the golden and REPORT it.

    Returns the path written. Records the path in :data:`SEEDED_GOLDENS` and
    prints a loud stderr notice — seeding is never silent, because a seeded
    golden has been *asserted by nobody*.
    """
    import sys as _sys
    p = Path(golden_path)
    save_golden(img, p)
    SEEDED_GOLDENS.append(p)
    print(f"[render_oracle] SEEDED GOLDEN (NOT VERIFIED): {p} — this capture "
          f"was written as the new baseline and compared against nothing. "
          f"Review it by eye before committing.", file=_sys.stderr)
    return p


#: Golden match is structural: a relaunched app re-renders the same page with
#: sub-pixel/anti-alias differences that blow past a raw per-pixel tolerance on
#: text-heavy pages (the established test_ui_* suite uses SSIM for the same
#: reason). A golden "matches" when SSIM is high OR the raw pixel-diff is within
#: tolerance — either is sufficient evidence of structural sameness.
GOLDEN_SSIM_MIN = 0.85


def matches_golden(img, golden_path: Union[str, Path],
                   tolerance: float = 0.05,
                   seed_if_missing: Optional[bool] = None) -> bool:
    """Compare *img* against a committed golden (structural + pixel).

    Returns True when the capture is structurally the same as the golden —
    SSIM >= GOLDEN_SSIM_MIN OR raw pixel-diff <= *tolerance*. SSIM is robust to
    the sub-pixel shift an app relaunch introduces; the pixel path is the
    fallback when the metric module is unavailable.

    **Missing golden (gap A1-68).** By default a missing golden raises
    :class:`GoldenMissingError` — it does NOT silently self-seed and report
    PASS, because that made a broken first capture the defended baseline and
    returned True having compared nothing. Seeding is explicit:

    * ``seed_if_missing=True`` (per call), or
    * ``PDFE_SEED_GOLDENS=1`` in the environment (whole seeding run), or
    * call :func:`seed_golden` directly from a seeding script.

    When seeding does happen the write is announced on stderr and recorded in
    :data:`SEEDED_GOLDENS`; the call then returns True for a *seeded* — never
    verified — golden. ``seed_if_missing=False`` forces the error even when the
    environment variable is set.
    """
    _require(_PIL, "Pillow")
    p = Path(golden_path)
    if not p.exists():
        seed = seeding_enabled() if seed_if_missing is None else bool(
            seed_if_missing)
        if not seed:
            raise GoldenMissingError(
                f"golden image does not exist: {p}\n"
                f"A missing golden is NOT a pass — nothing was compared. "
                f"If this baseline is genuinely new, seed it deliberately: "
                f"call render_oracle.seed_golden(img, path), pass "
                f"seed_if_missing=True, or re-run with {SEED_ENV_VAR}=1, then "
                f"REVIEW the seeded PNG by eye before committing it.")
        seed_golden(img, p)
        return True
    golden = Image.open(str(p)).convert("RGB")
    rgb = img.convert("RGB")
    if _DIFF_METRICS and _ssim is not None:
        if _ssim(rgb, golden) >= GOLDEN_SSIM_MIN:
            return True
    if _pixel_diff_ratio is not None:
        return _pixel_diff_ratio(rgb, golden) <= tolerance
    return not images_differ(img, golden, threshold=tolerance)


# --------------------------------------------------------------------------
# RenderOracle context manager
# --------------------------------------------------------------------------
class RenderOracle:
    """Launch pdfe.exe (optionally on a document) and expose canvas captures.

    Usage::

        with RenderOracle(doc="tests/fixtures/invoice_vector_heading.pdf") as ro:
            before = ro.capture()
            ...act...
            after = ro.capture()
            assert ro.changed(before, after)

    Teardown always kills pdfe.exe (two ways). ``app`` and ``win`` are the
    pywinauto Application and main-window wrapper, identical to
    :func:`pdfe_app_fixture.launch_pdfe`'s return.
    """

    def __init__(self, doc: Optional[Union[str, Path]] = None,
                 settle_s: float = 1.5):
        self.doc = str(doc) if doc is not None else None
        self.settle_s = settle_s
        self.app = None
        self.win = None

    def __enter__(self) -> "RenderOracle":
        # Imported lazily so this module stays importable without pywinauto.
        from tests.ui_automation.harness.pdfe_app_fixture import launch_pdfe
        args = [self.doc] if self.doc else None
        self.app, self.win = launch_pdfe(args=args)
        # Let the first page render before any capture.
        time.sleep(self.settle_s)
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        if self.app is not None:
            try:
                self.app.kill()
            except Exception:  # noqa: BLE001
                pass
        try:
            subprocess.run(["taskkill", "/F", "/IM", "pdfe.exe"],
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=10)
        except Exception:  # noqa: BLE001
            pass
        self.app = None
        self.win = None

    # -- capture / assertion conveniences ---------------------------------
    def capture(self):
        """Capture the current canvas as a ``PIL.Image``."""
        return capture_canvas(self.win)

    def changed(self, before, after, threshold: float = 0.02) -> bool:
        return images_differ(before, after, threshold=threshold)

    def is_blank(self, img, blank_threshold: float = 0.98) -> bool:
        return region_is_blank(img, blank_threshold=blank_threshold)

    def matches(self, img, golden_path, tolerance: float = 0.05,
                seed_if_missing: Optional[bool] = None) -> bool:
        return matches_golden(img, golden_path, tolerance=tolerance,
                              seed_if_missing=seed_if_missing)


# ==========================================================================
# Font-identity pixel oracles (pipeline b91f4e83, phase 6/7)
# ==========================================================================
# These close the gap that let the SpecimenA text-edit defects ship: the model
# layer said "serif / italic" but nothing asserted the CANVAS actually painted
# serif, slanted glyphs. Three primitives, each validated against live captures
# of the embedded_italic_serif_subset fixture (serif italic) AND a live sans
# control (invoice_vector_heading, Arial-Bold):
#
#   assert_slant_matches(img, expected_italic, threshold)   -> directional
#   assert_overlay_font_class(img, expected_class, ref_text)-> serif vs sans
#   assert_overlay_alignment(overlay, underlay, max_offset) -> geometry match
#
# Design notes from calibration (see scratchpad analysis):
# * SLANT is measured by the shear angle that maximises the vertical
#   ink-projection sharpness (standard OCR de-slanting). Robust at every zoom:
#   live serif-italic read -10..-11 deg, live/PIL upright read -1..-3 deg.
# * SERIF vs SANS could NOT be separated by any single scalar (hv-edge ratio,
#   stroke run-length CV, serif-foot ratio) — every scalar is confounded by
#   slant, weight, text content, resolution, and the Qt-vs-PIL renderer gap
#   (e.g. live bold sans "Invoice" out-scored live serif on hv-edge ratio). The
#   robust method is TEMPLATE CORRELATION: render the KNOWN text in several
#   serif and several sans reference faces, de-slant + size-normalise both the
#   references and the live capture, and classify by which class the live shape
#   correlates with more strongly. Validated: serif lines 0.36/0.53 vs sans
#   0.26/0.30; the sans control 0.03 (serif) vs 0.13 (sans); an isolated
#   fallback 'z' glyph 0.054 (serif) vs -0.026 (sans).

#: |de-slant angle| >= this (deg) reads as italic; below it reads as upright.
ITALIC_SLANT_THRESHOLD_DEG = 6.0

# --- RIG_DISPLAY_SCALE: zoom / display-scaling dependency -------------------
# The CI/dev rig renders the PDFE canvas under a combined display + app zoom of
# roughly 1.62 (the "~162%" figure quoted throughout the b91f4e83 calibration
# notes). Only ONE of the three pixel oracles below is actually sensitive to
# that scale:
#
#   * assert_overlay_alignment   -> SCALE-DEPENDENT. Its tolerance is in screen
#     pixels, so a fixed PDF-point registration error grows linearly with zoom.
#     It multiplies max_offset_px by scale_factor.
#   * assert_slant_matches       -> SCALE-INVARIANT. Keys on a shear ANGLE
#     (degrees); zoom does not change an angle. scale_factor is a no-op.
#   * assert_overlay_font_class  -> SCALE-INVARIANT. Size-normalises live + ref
#     shapes to a fixed (H, W) before correlating; zoom cancels. scale_factor is
#     a no-op.
#
# RIG_DISPLAY_SCALE is the reference scale max_offset_px values were calibrated
# at. Callers that detect a different live zoom via
# render_drive.get_app_zoom_factor() pass the RATIO (live / reference) as
# scale_factor so the same physical misalignment yields the same verdict at any
# zoom. The two scale-invariant oracles accept scale_factor only for a uniform
# signature — they deliberately ignore it (faking a scale-dependent angle or
# normalised-correlation threshold would make them incorrect, not robust). The
# real cross-zoom proof is test_render_oracle_zoom.py, which runs all three at
# two distinct zoom levels and requires PASS at both.
RIG_DISPLAY_SCALE = 1.62

#: Reference faces for the serif/sans template classifier. Restricted to faces
#: that ship with every supported Windows image (Server-2019 CI included).
SERIF_REF_FONTS = (
    r"C:\Windows\Fonts\palabi.ttf",   # Palatino Linotype Bold Italic
    r"C:\Windows\Fonts\timesbi.ttf",  # Times New Roman Bold Italic
    r"C:\Windows\Fonts\georgiaz.ttf",  # Georgia Bold Italic
    r"C:\Windows\Fonts\times.ttf",    # Times New Roman (upright fallback)
    r"C:\Windows\Fonts\georgia.ttf",  # Georgia (upright fallback)
)
SANS_REF_FONTS = (
    r"C:\Windows\Fonts\arialbd.ttf",   # Arial Bold
    r"C:\Windows\Fonts\ariblk.ttf",    # Arial Black
    r"C:\Windows\Fonts\tahomabd.ttf",  # Tahoma Bold
    r"C:\Windows\Fonts\arial.ttf",     # Arial (upright fallback)
    r"C:\Windows\Fonts\verdana.ttf",   # Verdana (upright fallback)
)


def _np():
    import numpy as _n  # local import keeps the module importable headless
    return _n


def _ink_mask(img, lum_threshold: int = 130):
    """Boolean ink mask (True where darker than *lum_threshold*)."""
    _require(_PIL, "Pillow")
    np = _np()
    return np.asarray(img.convert("L"), dtype=np.int16) < lum_threshold


def _crop_to_ink(mask):
    """Tightly crop a boolean mask to its ink bounding box, or None if empty."""
    np = _np()
    rows = np.where(mask.any(axis=1))[0]
    cols = np.where(mask.any(axis=0))[0]
    if rows.size == 0 or cols.size == 0:
        return None
    return mask[rows[0]:rows[-1] + 1, cols[0]:cols[-1] + 1]


def estimate_slant_deg(img, max_deg: float = 22.0, step: float = 1.0) -> float:
    """Return the shear angle (degrees) that best DE-slants the glyphs in *img*.

    The estimate is the angle whose horizontal shear maximises the variance of
    the column ink-projection (vertical strokes line up into sharp peaks when
    correctly de-slanted). Positive magnitude with the italic sign means the
    text leans forward (italic); a magnitude near zero means upright. Robust to
    font, weight, and resolution because it keys on stroke alignment, not on
    any serif feature.
    """
    np = _np()
    mask = _crop_to_ink(_ink_mask(img))
    if mask is None:
        return 0.0
    h, w = mask.shape
    if h < 4 or w < 4:
        return 0.0
    mid = h / 2.0
    best_a, best_score = 0.0, -1.0
    a = -max_deg
    while a <= max_deg + 1e-6:
        shift = np.tan(np.radians(a))
        cols = np.zeros(w + 2 * h, dtype=np.float64)
        for y in range(h):
            dx = int(round(shift * (mid - y)))
            idx = np.where(mask[y])[0] + dx + h
            idx = idx[(idx >= 0) & (idx < w + 2 * h)]
            cols[idx] += 1
        score = float(np.var(cols))
        if score > best_score:
            best_score, best_a = score, a
        a += step
    return float(best_a)


def is_italic_render(img, threshold: float = ITALIC_SLANT_THRESHOLD_DEG) -> bool:
    """True when the rendered glyphs in *img* are slanted past *threshold* deg."""
    return abs(estimate_slant_deg(img)) >= threshold


def assert_slant_matches(img, expected_italic: bool,
                         threshold: float = ITALIC_SLANT_THRESHOLD_DEG,
                         what: str = "overlay",
                         scale_factor: float = 1.0) -> float:
    """Assert the rendered slant DIRECTION matches *expected_italic*.

    Directional on purpose: with ``expected_italic=True`` it fails when the
    render is upright (the overlay italic-drop defect 1b), and with
    ``expected_italic=False`` it fails when the render is unexpectedly slanted
    (a hypothetical wrong-italic regression). Returns the measured slant angle.

    *scale_factor* is accepted for API uniformity with the other oracles but is
    intentionally NOT applied to *threshold*: the slant metric is a shear ANGLE
    (degrees), which is dimensionless and scale-invariant by construction — a
    glyph italicised at 6 deg reads as 6 deg whether the page is rendered at 100%
    or 162% zoom (see RIG_DISPLAY_SCALE note above). Passing a different
    scale_factor here is a no-op on the decision; correctness is proved by the
    zoom-level regression test exercising this oracle at two zooms. Faking a
    scale-dependent angle threshold would make the oracle WRONG, not more robust.
    """
    _ = scale_factor  # see docstring: angle is scale-invariant; do not scale it
    angle = estimate_slant_deg(img)
    slanted = abs(angle) >= threshold
    if expected_italic and not slanted:
        raise AssertionError(
            f"{what}: expected ITALIC render but glyphs are upright "
            f"(|slant|={abs(angle):.1f} deg < {threshold} deg)")
    if not expected_italic and slanted:
        raise AssertionError(
            f"{what}: expected UPRIGHT render but glyphs are slanted "
            f"(|slant|={abs(angle):.1f} deg >= {threshold} deg)")
    return float(angle)


def _deslant_mask(mask, ang: float):
    """Apply the inverse shear so slanted glyphs become upright; re-crop."""
    np = _np()
    m = _crop_to_ink(mask)
    if m is None:
        return None
    h, w = m.shape
    shift = np.tan(np.radians(ang))
    mid = h / 2.0
    out = np.zeros((h, w + 2 * h), dtype=bool)
    for y in range(h):
        dx = int(round(shift * (mid - y)))
        idx = np.where(m[y])[0] + dx + h
        idx = idx[(idx >= 0) & (idx < w + 2 * h)]
        out[y, idx] = True
    return _crop_to_ink(out)


def _norm_shape(mask, H: int = 64, W: int = 512):
    """Resize a boolean ink mask to a fixed (H, W) mean-subtracted float image
    for correlation (mean subtraction makes correlation contrast-invariant)."""
    np = _np()
    from PIL import Image as _Image
    im = _Image.fromarray((mask * 255).astype("uint8")).resize(
        (W, H), _Image.LANCZOS)
    a = np.asarray(im, dtype=np.float64) / 255.0
    return a - a.mean()


def _corr(a, b) -> float:
    np = _np()
    denom = np.sqrt((a * a).sum() * (b * b).sum()) + 1e-9
    return float((a * b).sum() / denom)


def _render_ref_shape(font_path: str, text: str, H: int, W: int,
                      px: int = 120):
    """De-slanted, normalised reference shape of *text* in *font_path*, or None
    if the font is unavailable."""
    if not Path(font_path).exists():
        return None
    try:
        from PIL import Image as _Image, ImageDraw, ImageFont
    except Exception:  # noqa: BLE001
        return None
    f = ImageFont.truetype(font_path, px)
    img = _Image.new("L", (int(px * max(1, len(text)) * 0.9) + 30,
                           int(px * 2)), 255)
    ImageDraw.Draw(img).text((10, 8), text, font=f, fill=0)
    mask = _ink_mask(img.convert("RGB"))
    desl = _deslant_mask(mask, estimate_slant_deg(img.convert("RGB")))
    if desl is None:
        return None
    return _norm_shape(desl, H, W)


def classify_font_class(img, ref_text: str, H: int = 64, W: int = 512):
    """Classify the glyphs in *img* as 'serif' or 'sans' by template
    correlation against reference renders of *ref_text*.

    Returns ``(verdict, corr_serif, corr_sans)`` where the correlations are the
    means over the available reference faces of each class. Raises RuntimeError
    if no reference font of either class is installed (so a missing-font
    environment is a hard error, never a silent pass).
    """
    np = _np()
    mask = _ink_mask(img)
    desl = _deslant_mask(mask, estimate_slant_deg(img))
    if desl is None:
        raise AssertionError("classify_font_class: no ink found in image")
    live = _norm_shape(desl, H, W)
    serif = [c for c in (_corr(live, _render_ref_shape(f, ref_text, H, W))
                         for f in SERIF_REF_FONTS
                         if _render_ref_shape(f, ref_text, H, W) is not None)]
    sans = [c for c in (_corr(live, _render_ref_shape(f, ref_text, H, W))
                        for f in SANS_REF_FONTS
                        if _render_ref_shape(f, ref_text, H, W) is not None)]
    if not serif or not sans:
        raise RuntimeError(
            "font-class classifier needs at least one serif and one sans "
            "reference font installed (looked under C:\\Windows\\Fonts)")
    # MAX over each class's faces: classify by the BEST-matching serif face vs
    # the BEST-matching sans face ("does it look like ANY serif / ANY sans").
    # Mean dilutes the signal with dissimilar faces and is unstable; max keys on
    # the closest face (for this fixture, Palatino Bold Italic) and is stable.
    cs, cn = float(np.max(serif)), float(np.max(sans))
    return ("serif" if cs >= cn else "sans"), cs, cn


def assert_overlay_font_class(img, expected_class: str, ref_text: str,
                              margin: float = 0.0,
                              what: str = "overlay",
                              scale_factor: float = 1.0) -> tuple:
    """Assert the rendered glyph class matches *expected_class* ('serif'|'sans').

    Classification is template correlation (see :func:`classify_font_class`):
    the live de-slanted shape must correlate more strongly with the
    *expected_class* reference faces than with the other class, by at least
    *margin*. *ref_text* is the text actually on screen (the test knows it), so
    the references match glyph-for-glyph. Returns ``(corr_serif, corr_sans)``.

    *scale_factor* is accepted for API uniformity with the other oracles but is
    intentionally NOT applied: both the live capture and every reference face are
    size-normalised to a fixed (H=64, W=512) shape in :func:`_norm_shape` before
    correlation, so absolute pixel size — and therefore zoom — cancels out
    entirely (see RIG_DISPLAY_SCALE note above). The correlation *margin* is a
    dimensionless ratio, not a pixel count, so scaling it would be meaningless.
    Robustness across zoom is proved by the zoom-level regression test, not by
    perturbing this threshold.
    """
    _ = scale_factor  # see docstring: normalised correlation is scale-invariant
    expected_class = expected_class.lower()
    if expected_class not in ("serif", "sans"):
        raise ValueError(f"expected_class must be 'serif'/'sans', got "
                         f"{expected_class!r}")
    _verdict, cs, cn = classify_font_class(img, ref_text)
    if expected_class == "serif":
        ok = cs > cn + margin
    else:
        ok = cn > cs + margin
    if not ok:
        raise AssertionError(
            f"{what}: expected {expected_class.upper()} font class but "
            f"correlation says otherwise (corr_serif={cs:.3f}, "
            f"corr_sans={cn:.3f}, margin={margin}); ref_text={ref_text!r}")
    return cs, cn


def _ink_bbox(img, lum_threshold: int = 130):
    """(x0, y0, x1, y1) ink bounding box within *img*, or None."""
    np = _np()
    mask = _ink_mask(img, lum_threshold)
    rows = np.where(mask.any(axis=1))[0]
    cols = np.where(mask.any(axis=0))[0]
    if rows.size == 0 or cols.size == 0:
        return None
    return (int(cols[0]), int(rows[0]), int(cols[-1]) + 1, int(rows[-1]) + 1)


def assert_overlay_alignment(overlay_img, underlay_img, max_offset_px: int,
                             what: str = "overlay",
                             scale_factor: float = 1.0) -> tuple:
    """Assert the overlay glyph band aligns with the underlay glyph band.

    Both images must be cropped to the SAME screen rectangle (see
    :func:`render_drive.capture_overlay_over_underlay`). The check compares the
    top-left corner of each image's ink bounding box; an offset larger than the
    effective tolerance on either axis means the editing overlay is painted out
    of register with the page text underneath it (defect 2, the baseline/geometry
    mismatch). Returns ``(dx, dy)`` in pixels.

    *scale_factor* genuinely matters here, UNLIKE the slant/font-class oracles:
    ``dx``/``dy`` are absolute screen PIXELS, so a fixed PDF-point registration
    error projects to MORE pixels the further the page is zoomed in. The caller's
    *max_offset_px* is calibrated for the rig's reference scale (see
    RIG_DISPLAY_SCALE note above, ~1.62); the effective tolerance is therefore
    ``max_offset_px * scale_factor`` so the same real-world misalignment passes or
    fails consistently at 100%, 162%, or any other zoom. Pass
    ``render_drive.get_app_zoom_factor(win)`` (normalised to the reference scale)
    as *scale_factor* for adaptive thresholding.
    """
    if scale_factor <= 0.0:
        raise ValueError(f"scale_factor must be > 0, got {scale_factor!r}")
    ob = _ink_bbox(overlay_img)
    ub = _ink_bbox(underlay_img)
    if ob is None or ub is None:
        raise AssertionError(
            f"{what}: could not locate glyph ink in "
            f"{'overlay' if ob is None else 'underlay'} crop for alignment")
    dx = abs(ob[0] - ub[0])
    dy = abs(ob[1] - ub[1])
    eff_offset = max_offset_px * scale_factor
    if dx > eff_offset or dy > eff_offset:
        raise AssertionError(
            f"{what}: overlay misaligned with underlay by (dx={dx}, dy={dy}) "
            f"px > {eff_offset:.1f} px (max_offset_px={max_offset_px} * "
            f"scale_factor={scale_factor:.3f}); overlay ink bbox {ob}, "
            f"underlay {ub}")
    return dx, dy


def word_segments(img, gap_frac: float = 0.28):
    """Return de-slanted word column-runs ``[(x0, x1), ...]`` for *img*.

    Segments the de-slanted ink mask into runs separated by inter-word blank
    gaps (>= *gap_frac* of the glyph height). Lets a test isolate a single
    word/glyph (e.g. a newly typed character) from a rendered line. Returns the
    segments AND the de-slanted mask so the caller can crop sub-regions
    consistently: ``(segments, deslanted_mask)``.

    *gap_frac* is the inter-word-gap threshold as a fraction of glyph height
    (default 0.28). It is rig-sensitive: the absolute inter-word gap a renderer
    produces varies with resolution/zoom/font, so a fixed threshold can MERGE
    adjacent words (too large) on some rigs. Callers that need a specific word
    count should sweep *gap_frac* downward until the words separate, rather than
    assume the default splits a given line correctly
    (CI-LIMITATION-WINDOWS-VALIDATION-UI-E2E-FLAKES item #4).
    """
    np = _np()
    mask = _ink_mask(img)
    desl = _deslant_mask(mask, estimate_slant_deg(img))
    if desl is None:
        return [], None
    h = desl.shape[0]
    nz = desl.sum(axis=0) > 0
    gap = max(3, int(h * gap_frac))
    segs = []
    start = None
    blank = 0
    for x, v in enumerate(nz):
        if v:
            if start is None:
                start = x
            blank = 0
        elif start is not None:
            blank += 1
            if blank >= gap:
                segs.append((start, x - blank + 1))
                start = None
                blank = 0
    if start is not None:
        segs.append((start, len(nz)))
    return [s for s in segs if s[1] - s[0] >= 4], desl


def assert_glyph_class(deslanted_mask, x0: int, x1: int, ref_glyph: str,
                       expected_class: str, what: str = "glyph") -> tuple:
    """Assert a single word/glyph column-slice [x0:x1] of a de-slanted mask is
    *expected_class*, correlating against *ref_glyph* references.

    Single-glyph correlation margins are thin (it is the weakest pixel signal —
    the authoritative per-glyph face check is the gtest
    test_textedit_font_identity.cpp), so this asserts only the correlation SIGN/
    ordering: the glyph must correlate more with the expected class. Returns
    ``(corr_serif, corr_sans)``.
    """
    np = _np()
    seg = _crop_to_ink(deslanted_mask[:, x0:x1])
    if seg is None:
        raise AssertionError(f"{what}: empty glyph slice [{x0}:{x1}]")
    H, W = 64, 64
    live = _norm_shape(seg, H, W)
    serif = [_corr(live, s) for s in
             (_render_ref_shape(f, ref_glyph, H, W) for f in SERIF_REF_FONTS)
             if s is not None]
    sans = [_corr(live, s) for s in
            (_render_ref_shape(f, ref_glyph, H, W) for f in SANS_REF_FONTS)
            if s is not None]
    if not serif or not sans:
        raise RuntimeError("glyph classifier needs serif+sans reference fonts")
    cs, cn = float(np.max(serif)), float(np.max(sans))
    expected_class = expected_class.lower()
    ok = cs > cn if expected_class == "serif" else cn > cs
    if not ok:
        raise AssertionError(
            f"{what}: expected {expected_class.upper()} glyph {ref_glyph!r} "
            f"but corr_serif={cs:.3f}, corr_sans={cn:.3f}")
    return cs, cn
