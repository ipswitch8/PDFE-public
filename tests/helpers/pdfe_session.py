# -*- coding: utf-8 -*-
"""Reusable PDFE session helper for end-to-end UI tests (Pipeline a7c2e541
phase-5).

Wraps the existing ``tests.ui_harness.core.launch.PDFESession`` deterministic
launcher and adds the phase-5 conveniences:

    * ``find(auto_id=...)`` — find a Qt widget by ``objectName`` exposed as
      UIA ``AutomationId``; transparently handles the Qt full-path-prefix
      quirk.
    * ``hwnd`` — Win32 HWND of the main PDFE window for screenshot capture.
    * ``wait_for_idle()`` — element-readiness polling (no naked ``time.sleep``).
    * ``orphans()`` — return ``True`` when no ``pdfe.exe`` processes remain;
      used by ``close()`` and by individual tests to assert clean teardown.

Phase-4 captured screenshot baselines via the same launcher; phase-5 builds
on that foundation.
"""

from __future__ import annotations

import ctypes
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Iterable, List, Optional

# ---------------------------------------------------------------------------
# Project-root resolution so this module imports cleanly regardless of cwd.
# ---------------------------------------------------------------------------

_HERE = Path(__file__).resolve().parent
REPO_ROOT = _HERE.parent.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

# ---------------------------------------------------------------------------
# Lazy imports so the module imports on non-Windows CI even though tests skip
# ---------------------------------------------------------------------------

def _import_pywinauto():
    import pywinauto  # noqa: F401  pylint: disable=import-outside-toplevel
    return pywinauto


def _import_psutil():
    import psutil  # noqa: F401  pylint: disable=import-outside-toplevel
    return psutil


# ---------------------------------------------------------------------------
# Public re-exports from the existing harness
# ---------------------------------------------------------------------------

from tests.ui_harness.core.launch import (  # noqa: E402
    PDFE_EXE as _PDFE_EXE,
    PDFESession as _BasePDFESession,
    no_orphan_pdfe as _no_orphan_pdfe,
    _kill_orphan_pdfe as _kill_orphan_pdfe,  # private but stable
)
from tests.ui_harness.core.locate import (  # noqa: E402
    by_auto_id as _by_auto_id,
    by_name as _by_name,
)


# ---------------------------------------------------------------------------
# Public constants
# ---------------------------------------------------------------------------

PDFE_EXE: str = _PDFE_EXE
DEFAULT_TIMEOUT_S: float = 12.0
"""Generous default for UIA element-existence polling.  Some Qt panels
(e.g. SignaturePanel, BookmarkPanel) lazy-load on first activation."""


# ---------------------------------------------------------------------------
# Convenience: idempotent orphan kill
# ---------------------------------------------------------------------------

def kill_pdfe_orphans() -> None:
    """Kill any orphaned ``pdfe.exe`` processes; safe to call repeatedly."""
    _kill_orphan_pdfe()


def force_kill_pdfe_now() -> None:
    """Force-kill every running pdfe.exe and wait for the kill to settle.

    Used by ``PDFESession.__exit__`` as a hard backstop when the graceful
    quit path is blocked by an unsaved-changes modal dialog.  Loops up
    to 5 seconds calling ``kill_pdfe_orphans`` until ``no_orphans()``
    reports clean.
    """
    kill_pdfe_orphans()
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        if no_orphans():
            return
        kill_pdfe_orphans()
        time.sleep(0.3)


def no_orphans() -> bool:
    """``True`` when no ``pdfe.exe`` is currently running."""
    return _no_orphan_pdfe()


def list_pdfe_pids() -> List[int]:
    """Return the PIDs of every running ``pdfe.exe`` process."""
    psutil = _import_psutil()
    pids: List[int] = []
    for proc in psutil.process_iter(["name", "pid"]):
        try:
            if proc.info["name"] and proc.info["name"].lower() == "pdfe.exe":
                pids.append(int(proc.info["pid"]))
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass
    return pids


# ---------------------------------------------------------------------------
# PDFESession — ergonomic phase-5 wrapper
# ---------------------------------------------------------------------------

class PDFESession:
    """Phase-5 ergonomic wrapper around the deterministic harness.

    Usage::

        with PDFESession(pdf_path=str(fixture)) as s:
            btn = s.find(auto_id="openAction")
            btn.invoke()

    The underlying launcher (``tests.ui_harness.core.launch.PDFESession``)
    handles DPI awareness, Qt env-var pinning, deterministic window size,
    and orphan-process cleanup before launch.  This wrapper layers a few
    quality-of-life methods on top.
    """

    def __init__(
        self,
        pdf_path: Optional[str] = None,
        kill_existing: bool = True,
        extra_args: Optional[Iterable[str]] = None,
    ) -> None:
        self._pdf_path = pdf_path
        self._kill_existing = kill_existing
        self._extra_args = list(extra_args) if extra_args else []
        self._inner: Optional[_BasePDFESession] = None

    # ---- context manager -------------------------------------------------

    def __enter__(self) -> "PDFESession":
        if self._extra_args:
            # The base launcher does not accept extra args; for that case
            # we replicate its launch sequence.
            self._inner = _LaunchWithExtraArgs(
                self._pdf_path, self._kill_existing, self._extra_args,
            )
            self._inner.__enter__()  # type: ignore[attr-defined]
        else:
            self._inner = _BasePDFESession(
                pdf_path=self._pdf_path,
                kill_existing=self._kill_existing,
            )
            self._inner.__enter__()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> bool:
        try:
            if self._inner is not None:
                self._inner.__exit__(exc_type, exc_val, exc_tb)
        finally:
            # Belt-and-suspenders: orphans wedge the next test.
            kill_pdfe_orphans()
        return False  # do not suppress exceptions

    def close(self) -> None:
        """Idempotent close — equivalent to ``__exit__(None, None, None)``."""
        if self._inner is None:
            return
        try:
            self._inner.__exit__(None, None, None)
        finally:
            self._inner = None
            kill_pdfe_orphans()

    # ---- accessors -------------------------------------------------------

    @property
    def app(self):
        """The pywinauto Application bound to the running PDFE process."""
        assert self._inner is not None, "session not entered"
        return self._inner.app

    @property
    def proc(self) -> subprocess.Popen:
        """The subprocess.Popen handle for PDFE.exe."""
        assert self._inner is not None, "session not entered"
        return self._inner.proc

    @property
    def pid(self) -> int:
        """Process ID of the running PDFE.exe."""
        assert self._inner is not None, "session not entered"
        return int(self._inner.pid)

    @property
    def window(self):
        """The pywinauto WindowSpecification for the main PDFE window."""
        assert self._inner is not None, "session not entered"
        return self._inner.window

    @property
    def hwnd(self) -> int:
        """Win32 HWND of the main PDFE window (for screenshot capture).

        NOTE — do NOT memoise this. Reading it DOES cost a pywinauto/UIA window
        resolution (find_element -> FindAll, ~150 ms), so callers must not put
        it inside a tight loop: resolve it ONCE into a local and reuse that (see
        test_ui_stress, which hoists it out of its 1000-iteration undo/redo
        loops). An earlier attempt to memoise it here to fix that one test broke
        tests/ui_automation/test_form_fields_p1.py (all 10 XFDF export/fill
        cases started failing with "export failed"), because ~120 test files
        rely on this resolving dynamically. Fix the hot caller, not this
        property.
        """
        try:
            return int(self.window.handle)
        except Exception:  # noqa: BLE001
            # Fall back to enumerating top-level windows for our pid.
            return _find_top_window_hwnd(self.pid)

    # ---- ergonomic locators ---------------------------------------------

    def find(
        self,
        auto_id: Optional[str] = None,
        name: Optional[str] = None,
        control_type: Optional[str] = None,
        timeout: float = DEFAULT_TIMEOUT_S,
    ):
        """Find a widget by Qt ``objectName`` (preferred) or ``accessibleName``.

        At least one of ``auto_id`` / ``name`` must be provided.  ``auto_id``
        is preferred because it is more stable across translations.
        """
        if auto_id:
            return _by_auto_id(self.window, auto_id, control_type=control_type,
                               timeout=int(max(1, timeout)))
        if name:
            return _by_name(self.window, name, control_type=control_type,
                            timeout=int(max(1, timeout)))
        raise ValueError("PDFESession.find: must specify auto_id or name")

    def find_optional(self, auto_id: Optional[str] = None,
                      name: Optional[str] = None,
                      control_type: Optional[str] = None,
                      timeout: float = 2.0):
        """Best-effort variant of ``find`` that returns ``None`` on miss."""
        try:
            return self.find(auto_id=auto_id, name=name,
                             control_type=control_type, timeout=timeout)
        except Exception:  # noqa: BLE001
            return None

    # ---- waits / synchronization ---------------------------------------

    def wait_for_idle(self, timeout: float = 5.0,
                      poll: float = 0.1) -> None:
        """Block until the message queue is empty.

        Uses pywinauto's UIA waiting where possible; falls back to a short
        Win32 ``WaitForInputIdle``-style poll.  This is the recommended
        replacement for ad-hoc ``time.sleep`` in tests.
        """
        # pywinauto exposes wait_cpu_usage_lower for Win32 backend; with UIA
        # the cheapest reliable signal is window.is_visible() with a poll.
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                if self.window.is_visible():
                    # Issue a no-op to drain the message queue.
                    _ = self.window.element_info
                    return
            except Exception:  # noqa: BLE001
                pass
            time.sleep(poll)

    def wait_for_window_title(self, substr: str,
                              timeout: float = DEFAULT_TIMEOUT_S,
                              poll: float = 0.15) -> bool:
        """Poll until the main-window title contains ``substr``.

        Returns ``True`` on match, ``False`` on timeout.
        """
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                title = self.window.window_text() or ""
                if substr in title:
                    return True
            except Exception:  # noqa: BLE001
                pass
            time.sleep(poll)
        return False

    def wait_for_top_level_window(self, title_substr: str,
                                  timeout: float = DEFAULT_TIMEOUT_S,
                                  poll: float = 0.15) -> int:
        """Wait for any visible top-level window whose title contains
        ``title_substr`` and return its HWND.

        Used for modal dialogs that pywinauto's UIA backend sometimes
        does not enumerate via ``Application.windows()`` on Qt6 builds
        (e.g. EncryptionDialog, PasswordDialog).  Returns 0 on timeout.
        """
        deadline = time.monotonic() + timeout
        pid = self.pid
        while time.monotonic() < deadline:
            hwnd = _find_top_window_hwnd(pid, title_substr=title_substr)
            if hwnd:
                return hwnd
            time.sleep(poll)
        return 0


# ---------------------------------------------------------------------------
# Internal: launch with extra command-line args (the base launcher does not
# expose this, so we replicate its sequence here).
# ---------------------------------------------------------------------------

class _LaunchWithExtraArgs:
    """Mirror of ``tests.ui_harness.core.launch.PDFESession`` that supports
    appending extra command-line arguments (e.g. for the
    ``test_open_from_command_line`` test).
    """

    def __init__(self, pdf_path: Optional[str], kill_existing: bool,
                 extra_args: Iterable[str]) -> None:
        self._pdf_path = pdf_path
        self._kill_existing = kill_existing
        self._extra_args = list(extra_args)
        self.proc: Optional[subprocess.Popen] = None
        self.app = None

    @property
    def window(self):
        return self.app.window(title_re=r".*PDFE.*")

    @property
    def pid(self) -> int:
        return self.proc.pid if self.proc else 0

    def __enter__(self) -> "_LaunchWithExtraArgs":
        from tests.ui_harness.core.launch import (
            _set_process_dpi_aware,
            _qt_env_for_determinism,
            LAUNCH_TIMEOUT,
            POLL_INTERVAL,
            _resize_window,
        )

        _set_process_dpi_aware()
        if self._kill_existing:
            kill_pdfe_orphans()

        cmd = [PDFE_EXE]
        cmd.extend(self._extra_args)
        if self._pdf_path:
            cmd.append(os.path.abspath(self._pdf_path))

        env = _qt_env_for_determinism()
        self.proc = subprocess.Popen(cmd, env=env)

        pywinauto = _import_pywinauto()
        self.app = pywinauto.Application(backend="uia")
        deadline = time.time() + LAUNCH_TIMEOUT
        while time.time() < deadline:
            try:
                self.app.connect(process=self.proc.pid,
                                 timeout=POLL_INTERVAL)
                win = self.app.window(title_re=r".*PDFE.*")
                win.wait("visible", timeout=5)
                break
            except Exception:  # noqa: BLE001
                time.sleep(POLL_INTERVAL)
        else:
            self.proc.kill()
            raise TimeoutError(
                f"PDFE main window did not appear within {LAUNCH_TIMEOUT}s"
                f" (extra args={self._extra_args!r})."
            )
        _resize_window(self.app, self.proc.pid)
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> bool:
        """Teardown that never blocks on an unsaved-changes modal.

        Strategy: attempt graceful quit_pdfe in a worker thread with a
        short timeout, then unconditionally force-kill.  If the doc is
        dirty PDFE shows a "Save changes?" modal — graceful close blocks
        on that, so we cap it at 2.0s and fall through to force-kill.
        This makes ``with PDFESession(...) as s:`` safe even when the
        test intentionally dirties the document; no per-test
        dirty-safe wrapper is required.
        """
        from tests.ui_harness.core.launch import quit_pdfe
        import threading

        def _try_graceful():
            try:
                if self.proc is not None and self.app is not None:
                    quit_pdfe(self.proc, self.app)
            except Exception:  # noqa: BLE001
                pass

        try:
            if self.proc is not None and self.app is not None:
                t = threading.Thread(target=_try_graceful, daemon=True)
                t.start()
                t.join(timeout=2.0)
                # If graceful quit is still running (dialog blocked it),
                # force-kill the process directly so the thread can exit.
                if t.is_alive() and self.proc.poll() is None:
                    try:
                        self.proc.kill()
                    except Exception:  # noqa: BLE001
                        pass
        finally:
            # Backstop: force-kill any remaining pdfe.exe.  Cheap when
            # graceful quit already succeeded; essential when it didn't.
            force_kill_pdfe_now()
        return False


# ---------------------------------------------------------------------------
# Win32 helpers
# ---------------------------------------------------------------------------

_USER32 = None


def _is_window(hwnd: int) -> bool:
    """Cheap Win32 validity check for a memoised HWND (no UIA involved).

    Used by PDFESession.hwnd to revalidate its memoised handle without paying
    for a pywinauto/UIA window resolution on every access.
    """
    global _USER32
    if _USER32 is None:
        _USER32 = ctypes.WinDLL("user32", use_last_error=True)
    return bool(_USER32.IsWindow(ctypes.c_void_p(int(hwnd))))


def _find_top_window_hwnd(pid: int, title_substr: Optional[str] = None) -> int:
    """Find the first visible top-level HWND owned by ``pid`` whose title
    contains ``title_substr`` (or any title when ``title_substr`` is None).
    """
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    EnumWindows = user32.EnumWindows
    GetWindowThreadProcessId = user32.GetWindowThreadProcessId
    GetWindowText = user32.GetWindowTextW
    IsWindowVisible = user32.IsWindowVisible

    enum_proto = ctypes.WINFUNCTYPE(
        ctypes.c_int, ctypes.c_void_p, ctypes.c_long,
    )
    found = [0]

    def _cb(hwnd, _lp):
        win_pid = ctypes.c_ulong()
        GetWindowThreadProcessId(hwnd, ctypes.byref(win_pid))
        if win_pid.value == pid and IsWindowVisible(hwnd):
            buf = ctypes.create_unicode_buffer(512)
            GetWindowText(hwnd, buf, 512)
            title = buf.value or ""
            if title_substr is None or title_substr in title:
                found[0] = int(hwnd)
                return False  # stop
        return True  # continue

    EnumWindows(enum_proto(_cb), 0)
    return found[0]


def close_window_by_hwnd(hwnd: int) -> None:
    """Send ``WM_CLOSE`` to a top-level window."""
    if not hwnd:
        return
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    WM_CLOSE = 0x0010
    try:
        user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
    except Exception:  # noqa: BLE001
        pass


def get_window_rect(hwnd: int):
    """Return ``(left, top, right, bottom)`` for a window in screen coords."""
    user32 = ctypes.WinDLL("user32", use_last_error=True)

    class _RECT(ctypes.Structure):
        _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                    ("right", ctypes.c_long), ("bottom", ctypes.c_long)]

    r = _RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    return r.left, r.top, r.right, r.bottom
