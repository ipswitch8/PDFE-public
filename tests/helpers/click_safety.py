# -*- coding: utf-8 -*-
"""Global PyAutoGUI click-safety net.

WHY THIS EXISTS
---------------
UI tests drive PDFE with absolute-coordinate PyAutoGUI clicks. When the target
coordinates are computed from a stale / minimised / scrolled widget rect — or
when an unrelated window has stolen the foreground — those clicks land OUTSIDE
PDFE: on the desktop, on another application, or on the **Windows taskbar**,
which LAUNCHES whatever app icon sits under the cursor. That has happened
repeatedly on the self-hosted runner (it is someone's real desktop).

Per-call-site guards proved insufficient: click sites are scattered across
tests/, tests/ui_automation/ and tests/ui_harness/, and a newly-added one is
unguarded by default. An earlier guard also only rejected *off-screen* points —
but the taskbar is ON-screen, so it never blocked the actual failure mode.

WHAT THIS DOES
--------------
Monkey-patches PyAutoGUI's mouse entry points so EVERY click/drag in the test
suite is validated first, no matter which module issues it:

  1. HARD FLOOR — the point must be inside the desktop **work area**
     (SPI_GETWORKAREA), which EXCLUDES the taskbar. This is what actually stops
     the accidental app launches.
  2. WINDOW BOUNDS — if any PDFE-owned top-level window is visible, the point
     must fall inside one of them (main window, dialogs, popups — the common
     file dialog is hosted in-process, so it is PDFE-owned too). Anything else
     is a mis-computed coordinate about to hit another app.

If no PDFE window exists (e.g. before launch), only the work-area floor applies,
so non-PDFE fixtures still work.

Violations raise :class:`UnsafeClickError` — a loud, honest test failure instead
of a silent desktop mis-fire.

ESCAPE HATCH
------------
``with allow_unsafe_clicks():`` (or env ``PDFE_ALLOW_UNSAFE_CLICKS=1``) disables
the net for a test that legitimately clicks outside PDFE. Use sparingly.
"""
from __future__ import annotations

import contextlib
import ctypes
import os
import sys
import threading
import time
from ctypes import wintypes
from typing import List, Tuple

__all__ = [
    "UnsafeClickError",
    "assert_safe_click",
    "safe_set_cursor_pos",
    "install_pyautogui_click_guard",
    "allow_unsafe_clicks",
    "work_area",
    "pdfe_window_rects",
]


class UnsafeClickError(RuntimeError):
    """An absolute click target was outside PDFE / inside the taskbar."""


_IS_WIN = sys.platform == "win32"

# Thread-local so a guarded test can't be affected by another thread's bypass.
_bypass = threading.local()


def _bypassed() -> bool:
    if os.environ.get("PDFE_ALLOW_UNSAFE_CLICKS") == "1":
        return True
    return bool(getattr(_bypass, "on", False))


@contextlib.contextmanager
def allow_unsafe_clicks():
    """Temporarily disable the click guard (for clicks legitimately outside PDFE)."""
    prev = getattr(_bypass, "on", False)
    _bypass.on = True
    try:
        yield
    finally:
        _bypass.on = prev


# ---------------------------------------------------------------------------
# Win32
# ---------------------------------------------------------------------------
if _IS_WIN:
    _user32 = ctypes.WinDLL("user32", use_last_error=True)
    _SPI_GETWORKAREA = 0x0030

    _user32.IsWindowVisible.argtypes = [wintypes.HWND]
    _user32.IsWindowVisible.restype = wintypes.BOOL
    _user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    _user32.GetWindowRect.restype = wintypes.BOOL
    _user32.IsIconic.argtypes = [wintypes.HWND]
    _user32.IsIconic.restype = wintypes.BOOL

    _ENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)


def work_area() -> Tuple[int, int, int, int]:
    """Desktop work area ``(left, top, right, bottom)`` — EXCLUDES the taskbar."""
    if not _IS_WIN:
        return (0, 0, 1 << 30, 1 << 30)
    r = wintypes.RECT()
    if not _user32.SystemParametersInfoW(_SPI_GETWORKAREA, 0, ctypes.byref(r), 0):
        # Fall back to full virtual screen if the query fails.
        import pyautogui
        w, h = pyautogui.size()
        return (0, 0, int(w), int(h))
    return (int(r.left), int(r.top), int(r.right), int(r.bottom))


# Cache PDFE pids briefly — psutil.process_iter is far too slow to run per click.
_pid_cache: Tuple[float, set] = (0.0, set())
_PID_TTL_S = 2.0


def _pdfe_pids() -> set:
    global _pid_cache
    now = time.monotonic()
    ts, pids = _pid_cache
    if now - ts < _PID_TTL_S:
        return pids
    found = set()
    try:
        import psutil
        for p in psutil.process_iter(["name", "pid"]):
            nm = (p.info.get("name") or "").lower()
            if nm in ("pdfe.exe", "pdfe"):
                found.add(p.info["pid"])
    except Exception:  # noqa: BLE001
        pass
    _pid_cache = (now, found)
    return found


def pdfe_window_rects() -> List[Tuple[int, int, int, int]]:
    """Rects of every visible, non-minimised top-level window owned by PDFE."""
    if not _IS_WIN:
        return []
    pids = _pdfe_pids()
    if not pids:
        return []
    rects: List[Tuple[int, int, int, int]] = []

    def _cb(hwnd, _lparam):
        try:
            if not _user32.IsWindowVisible(hwnd) or _user32.IsIconic(hwnd):
                return True
            pid = wintypes.DWORD(0)
            _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value not in pids:
                return True
            r = wintypes.RECT()
            if _user32.GetWindowRect(hwnd, ctypes.byref(r)):
                # Skip degenerate/off-screen rects (minimised windows report
                # (-32000,-32000); zero-size hidden pipes report empty rects).
                if r.right > r.left and r.bottom > r.top and r.right > 0 and r.bottom > 0:
                    rects.append((int(r.left), int(r.top), int(r.right), int(r.bottom)))
        except Exception:  # noqa: BLE001
            pass
        return True

    try:
        _user32.EnumWindows(_ENUMPROC(_cb), 0)
    except Exception:  # noqa: BLE001
        return []
    return rects


def _inside(x: int, y: int, rect: Tuple[int, int, int, int]) -> bool:
    left, top, right, bottom = rect
    return left <= x < right and top <= y < bottom


def assert_safe_click(x, y, op: str = "click") -> None:
    """Raise UnsafeClickError unless ``(x, y)`` is a safe PDFE click target."""
    if _bypassed() or not _IS_WIN:
        return
    try:
        xi, yi = int(x), int(y)
    except Exception:  # noqa: BLE001
        return  # Non-coordinate call (e.g. click() with no args) — nothing to check.

    wa = work_area()
    if not _inside(xi, yi, wa):
        raise UnsafeClickError(
            f"{op} at ({xi},{yi}) is OUTSIDE the desktop work area {wa} — this is "
            f"the taskbar or off-screen. Refusing: a click here can launch an "
            f"unrelated application. The source coordinates are almost certainly "
            f"computed from a stale/minimised/off-screen window rect."
        )

    rects = pdfe_window_rects()
    if rects and not any(_inside(xi, yi, r) for r in rects):
        raise UnsafeClickError(
            f"{op} at ({xi},{yi}) is not inside any PDFE window {rects} — refusing "
            f"to click another application/the desktop. If this click is "
            f"legitimately outside PDFE, wrap it in "
            f"tests.helpers.click_safety.allow_unsafe_clicks()."
        )


def safe_set_cursor_pos(x, y, op: str = "cursor-move") -> None:
    """Guarded ``SetCursorPos``: assert ``(x, y)`` is a safe PDFE target, then
    move the cursor there.

    Drop-in replacement for raw ``user32.SetCursorPos(x, y)`` in the win32 input
    drivers (SetCursorPos + mouse_event / wheel). Those raw calls are invisible
    to the PyAutoGUI monkey-patch, so without this a coordinate computed from a
    stale/gone widget rect silently moves the cursor onto the desktop/taskbar and
    the following mouse_event fires there — producing a WRONG click (and thus a
    false test result) with no error. Routing every SetCursorPos through here
    makes an out-of-bounds target raise UnsafeClickError loudly instead. Honors
    allow_unsafe_clicks() / PDFE_ALLOW_UNSAFE_CLICKS.
    """
    assert_safe_click(x, y, op=op)
    if _IS_WIN:
        ctypes.windll.user32.SetCursorPos(int(x), int(y))


_SPI_GETACTIVEWINDOWTRACKING = 0x1000
_SPI_SETACTIVEWINDOWTRACKING = 0x1001
_awt_saved = None  # prior value, so we restore exactly what we found


def get_active_window_tracking():
    """Return the current 'active window tracking' (X-Mouse) setting, or None.

    When enabled, Windows makes the window UNDER THE MOUSE CURSOR the active
    window — which is catastrophic for UI automation: whenever a test's cursor
    is not over the PDFE window (at rest in a corner, or after a click lands
    elsewhere), PDFE loses the foreground, so keystrokes leak and
    SetForegroundWindow (ensure_foreground / hotkey_to) fights the tracking and
    fails ("could not bring HWND to foreground within 2.0 s").
    """
    if not _IS_WIN:
        return None
    v = ctypes.c_int(-1)
    ok = _user32.SystemParametersInfoW(
        _SPI_GETACTIVEWINDOWTRACKING, 0, ctypes.byref(v), 0)
    return v.value if ok else None


def set_active_window_tracking(enabled: bool) -> bool:
    """Enable/disable active-window-tracking. Returns True on success."""
    if not _IS_WIN:
        return False
    # For SET actions pvParam carries the value BY VALUE (cast to void*).
    return bool(_user32.SystemParametersInfoW(
        _SPI_SETACTIVEWINDOWTRACKING, 0,
        ctypes.c_void_p(1 if enabled else 0), 0))


def disable_active_window_tracking_for_session() -> None:
    """Disable X-Mouse for the test session, remembering the prior value.

    Idempotent. Pair with restore_active_window_tracking() at session end.
    """
    global _awt_saved
    if not _IS_WIN or _awt_saved is not None:
        return
    cur = get_active_window_tracking()
    _awt_saved = cur if cur is not None else 0
    if cur:
        set_active_window_tracking(False)


def restore_active_window_tracking() -> None:
    """Restore the active-window-tracking value captured at session start."""
    global _awt_saved
    if not _IS_WIN or _awt_saved is None:
        return
    if _awt_saved:
        set_active_window_tracking(True)
    _awt_saved = None


_pywinauto_installed = False


def install_pywinauto_click_guard() -> None:
    """Guard pywinauto's mouse input too — NOT just PyAutoGUI.

    An app was STILL being launched from the taskbar after the PyAutoGUI net
    went in, because most clicks in this suite never touch PyAutoGUI: they go
    through pywinauto (``wrapper.click_input()``, ``canvas.click_input(coords=)``,
    ``pywinauto.mouse.click(...)``), which uses its own SendInput path. If a
    control's rectangle is stale / minimised / off-screen, click_input() happily
    clicks the taskbar.

    ``pywinauto.mouse._perform_click_input(button, coords, ...)`` is the single
    funnel for EVERY pywinauto mouse action (click/double_click/right_click/
    move/press/release/scroll/wheel_click) and it receives ABSOLUTE screen
    coords — the ideal choke point.

    IMPORTANT: ``pywinauto.base_wrapper`` does ``from .mouse import
    _perform_click_input``, i.e. it holds its OWN module-level reference. Patching
    only ``pywinauto.mouse`` would therefore MISS every ``wrapper.click_input()``
    — the very calls that matter. So rebind the name in every already-imported
    pywinauto module that holds it.
    """
    global _pywinauto_installed
    if _pywinauto_installed or not _IS_WIN:
        return
    try:
        import pywinauto.mouse as _pwm
        # Force-load the modules that take their OWN reference to the funnel, so
        # they exist in sys.modules and can be rebound below. (pytest_configure
        # runs before the test modules import pywinauto, so base_wrapper would
        # otherwise not be loaded yet — and every wrapper.click_input() goes
        # through its private binding.) Modules imported LATER pick up the
        # already-guarded function from pywinauto.mouse automatically.
        import pywinauto.base_wrapper  # noqa: F401
    except Exception:  # noqa: BLE001
        return

    orig = getattr(_pwm, "_perform_click_input", None)
    if orig is None:
        return
    if getattr(orig, "_pdfe_click_guarded", False):
        _pywinauto_installed = True
        return

    def guarded(button="left", coords=(None, None), *args, **kwargs):
        try:
            x, y = coords
        except Exception:  # noqa: BLE001
            x = y = None
        if x is not None and y is not None:
            assert_safe_click(x, y, op="pywinauto click_input")
        return orig(button, coords, *args, **kwargs)

    guarded._pdfe_click_guarded = True  # noqa: SLF001
    guarded.__name__ = "_perform_click_input"

    # Rebind in the mouse module AND in every pywinauto module that imported the
    # symbol directly (base_wrapper, and any control wrappers that did the same).
    _pwm._perform_click_input = guarded
    for name, mod in list(sys.modules.items()):
        if not name.startswith("pywinauto") or mod is None:
            continue
        existing = getattr(mod, "_perform_click_input", None)
        if existing is not None and not getattr(existing, "_pdfe_click_guarded", False):
            try:
                setattr(mod, "_perform_click_input", guarded)
            except Exception:  # noqa: BLE001
                pass

    _pywinauto_installed = True


_installed = False


def install_pyautogui_click_guard() -> None:
    """Monkey-patch PyAutoGUI's mouse entry points to validate the target first.

    Idempotent. Only positional/keyword ``x``/``y`` targets are validated; calls
    with no coordinates (which act at the CURRENT cursor position, e.g.
    ``mouseDown()`` after a guarded ``moveTo``) are passed through — the guarded
    move that positioned the cursor is what matters.
    """
    global _installed
    if _installed or not _IS_WIN:
        return
    try:
        import pyautogui
    except Exception:  # noqa: BLE001
        return

    def _wrap(fn, name):
        def guarded(*args, **kwargs):
            x = kwargs.get("x", args[0] if len(args) >= 1 else None)
            y = kwargs.get("y", args[1] if len(args) >= 2 else None)
            if x is not None and y is not None and not isinstance(x, str):
                assert_safe_click(x, y, op=f"pyautogui.{name}")
            return fn(*args, **kwargs)
        guarded.__name__ = getattr(fn, "__name__", name)
        guarded.__doc__ = getattr(fn, "__doc__", None)
        guarded._pdfe_click_guarded = True  # noqa: SLF001
        return guarded

    for name in ("click", "doubleClick", "tripleClick", "rightClick",
                 "middleClick", "moveTo", "dragTo", "mouseDown", "mouseUp"):
        fn = getattr(pyautogui, name, None)
        if fn is not None and not getattr(fn, "_pdfe_click_guarded", False):
            setattr(pyautogui, name, _wrap(fn, name))

    _installed = True
