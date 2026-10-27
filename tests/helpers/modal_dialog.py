# -*- coding: utf-8 -*-
"""Detect a modal Qt dialog without going through UIA.

WHY THIS EXISTS
---------------
pywinauto's UIA backend cannot reliably see a modal ``QDialog::exec()`` window
belonging to the application under test. Both process-scoped
(``Application.windows()`` / ``top_window()``) and desktop-scoped
(``Desktop(backend="uia").windows()``) enumeration came back without it, for
the entire time the dialog was up.

That is not a hypothesis. The application was instrumented and logged:

    16:42:40.650  calling exec()
    16:46:38.358  exec() returned 0

i.e. the dialog was open and modal for ~4 minutes -- the whole duration of a
240 s UIA polling loop that reported "no dialog appeared" -- and only closed
when the test's teardown dismissed it.

Every earlier diagnosis of that symptom was wrong, and each was ruled out by
direct experiment before landing here:

  * the RDP session being disconnected  (re-ran connected, cursor verified
    landing at the target -- still "no dialog")
  * the input API             (``win32_input.send_drag`` behaves the same as
    PyAutoGUI here)
  * the drag missing the page (the app logged the region it actually received;
    once it was fully on-page the OCR text came back correct -- and the UIA
    poll STILL reported no dialog)

So the product was working the whole time and the harness could not observe it.

WHAT THIS DOES INSTEAD
----------------------
Raw Win32 ``EnumWindows`` + ``WM_GETTEXT``. A Qt modal loop keeps pumping
messages, so ``WM_GETTEXT`` is answered normally; there is no dependence on the
UIA provider, which is what was failing.
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import time
from typing import List, Optional, Tuple

_user32 = ctypes.WinDLL("user32", use_last_error=True)

_WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

_user32.EnumWindows.argtypes = [_WNDENUMPROC, wt.LPARAM]
_user32.EnumWindows.restype = wt.BOOL
_user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
_user32.GetWindowThreadProcessId.restype = wt.DWORD
_user32.IsWindowVisible.argtypes = [wt.HWND]
_user32.IsWindowVisible.restype = wt.BOOL
_user32.SendMessageTimeoutW.restype = wt.LPARAM

WM_GETTEXT = 0x000D
WM_CLOSE = 0x0010
SMTO_ABORTIFHUNG = 0x0002


def _window_text(hwnd: int, timeout_ms: int = 1500) -> str:
    """Title via WM_GETTEXT with a timeout, so a wedged window cannot hang us."""
    buf = ctypes.create_unicode_buffer(512)
    res = wt.LPARAM()
    ok = _user32.SendMessageTimeoutW(
        wt.HWND(hwnd), wt.UINT(WM_GETTEXT), wt.WPARAM(512),
        ctypes.cast(buf, ctypes.c_void_p), wt.UINT(SMTO_ABORTIFHUNG),
        wt.UINT(timeout_ms), ctypes.byref(res),
    )
    return buf.value if ok else ""


def list_process_windows(pid: int) -> List[Tuple[int, str]]:
    """Every visible top-level window owned by *pid*, as (hwnd, title)."""
    out: List[Tuple[int, str]] = []

    def _cb(hwnd, _lparam):
        owner = wt.DWORD()
        _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and _user32.IsWindowVisible(hwnd):
            out.append((int(hwnd), _window_text(int(hwnd))))
        return True

    _user32.EnumWindows(_WNDENUMPROC(_cb), 0)
    return out


def wait_for_window(pid: int, title_substr: str,
                    timeout: float = 120.0,
                    poll: float = 1.0) -> Optional[int]:
    """Wait for a visible window of *pid* whose title contains *title_substr*.

    Returns its hwnd, or None on timeout.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        for hwnd, title in list_process_windows(pid):
            if title_substr in title:
                return hwnd
        time.sleep(poll)
    return None


def close_window(hwnd: int) -> None:
    """Post WM_CLOSE. Posted, not sent: a modal dialog must not block us."""
    _user32.PostMessageW(wt.HWND(hwnd), wt.UINT(WM_CLOSE), 0, 0)


def uia_dialog(hwnd: int):
    """Wrap an already-known modal ``hwnd`` for UIA-based INTERACTION.

    Detection and INTERACTION are two different problems and only the first
    one is broken. What UIA cannot do is *enumerate* an app-modal Qt dialog
    -- neither ``Application.windows()`` nor ``Desktop(backend="uia").
    windows()`` nor a ``descendants(control_type="Window")`` walk from the
    main window returns it. Handed the HWND directly (from
    :func:`wait_for_window`), UIA resolves the element and its subtree
    normally, because no enumeration is involved.

    Measured on the QColorDialog opened by ``AnnotationPropertiesWidget::
    onColorButtonClicked`` (2026-09-12, GitHub #136): ``wait_for_window``
    returned the hwnd 0.22 s after the button click, and
    ``Desktop(backend="uia").window(handle=hwnd).descendants()`` then listed
    all 32 children including ``qt_colorname_lineedit`` and the OK button.

    So: find the dialog with Win32, drive it with UIA-by-handle. Never
    search for it with UIA.
    """
    from pywinauto import Desktop  # lazy: keep the win32 path import-free
    return Desktop(backend="uia").window(handle=hwnd)
