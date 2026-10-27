# -*- coding: utf-8 -*-
"""GitHub #85 (WIN-54) and #86 (WIN-55): window geometry, dock arrangement and
maximized state must survive a relaunch.

WHAT WAS MISSING
----------------
MainWindow saves ``saveGeometry()`` / ``saveState()`` into AppSettings in
``closeEvent()`` and restores both in the constructor, but nothing anywhere
launched PDFE twice and checked that the second process came up where the
first one left off. The C++ suite cannot: the property is a CROSS-PROCESS
round trip through the settings store, so both halves have to be real
processes sharing one INI.

HOW THIS TESTS IT
-----------------
Every test pins ``PDFE_SETTINGS_PATH`` (src/app/AppSettings.cpp) to a fresh
temp INI, launches pdfe.exe, changes something, closes the window CLEANLY
(``WM_CLOSE`` — a taskkill would skip ``closeEvent()`` and save nothing),
waits for the process to exit, then relaunches against the same INI and
asserts what came back.

Geometry is read with Win32 ``GetWindowRect`` and the maximized flag with
``IsZoomed``, exactly as the two issues specify. Dock arrangement is read
through the ``testDockStateQueryPipe`` seam (src/ui/MainWindow.cpp), which
reports ``objectName:shown:area:raised`` per QDockWidget: UIA cannot answer
this, because a dock tabbed BEHIND another is still a mapped, "visible"
pane in the UIA tree — and raised-vs-merely-shown is precisely what
"the same docks are visible/raised" means.

THE PAGES DOCK IS DELIBERATELY EXCLUDED
---------------------------------------
``MainWindow::ensurePagesDockDefault()`` runs AFTER ``restoreState()`` on
every launch and forces Pages visible and raised (and Layers/Bookmarks
hidden) by design — see bugfix-3e91a7c4 #3 and
tests/test_ui_structure_bugs.cpp. Asserting that a saved Pages state
survives would be asserting against intended behaviour, so
``pagePanelDock`` is dropped from the comparison and the arrangement these
tests establish only touches right-area docks.

NON-VACUITY
-----------
Each test first records the arrangement/geometry the app comes up with on a
PRISTINE INI, and asserts the state it then establishes DIFFERS from it.
Without that, "instance 2 matches instance 1" would pass just as well on a
build that restored nothing and defaulted both times.
"""
from __future__ import annotations

import contextlib
import ctypes
import ctypes.wintypes as wt
import pathlib
import subprocess
import sys
import tempfile
import time
from typing import Dict, Optional

import pytest

if sys.platform != "win32":
    pytest.skip("Windows-only UI test", allow_module_level=True)

try:
    import pywinauto  # noqa: F401
except ImportError:  # pragma: no cover - environment guard
    pytest.skip("pywinauto not installed", allow_module_level=True)

from tests.helpers.qinvoke import _find_pipe, _write_to_pipe  # noqa: E402
from tests.helpers.modal_dialog import list_process_windows  # noqa: E402
from tests.ui_harness.core.launch import (  # noqa: E402
    PDFE_EXE,
    _kill_orphan_pdfe,
    _qt_env_for_determinism,
)

_user32 = ctypes.WinDLL("user32", use_last_error=True)
_user32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
_user32.GetWindowRect.restype = wt.BOOL
_user32.MoveWindow.argtypes = [wt.HWND, ctypes.c_int, ctypes.c_int,
                               ctypes.c_int, ctypes.c_int, wt.BOOL]
_user32.MoveWindow.restype = wt.BOOL
_user32.ShowWindow.argtypes = [wt.HWND, ctypes.c_int]
_user32.ShowWindow.restype = wt.BOOL
_user32.IsZoomed.argtypes = [wt.HWND]
_user32.IsZoomed.restype = wt.BOOL
_user32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
_user32.PostMessageW.restype = wt.BOOL

_SW_RESTORE = 9
_SW_MAXIMIZE = 3
_WM_CLOSE = 0x0010

#: Distinctive, safely on-screen normal geometry (left, top, width, height).
_TARGET_RECT = (140, 96, 1024, 700)

#: GetWindowRect tolerance. Qt restores the saved FRAME rect, so the round
#: trip is normally exact; a couple of pixels of slack absorbs any DWM frame
#: adjustment without coming close to the >200 px gap to the default rect.
_RECT_TOL = 6

#: Excluded from every dock comparison — see module docstring.
_PAGES_DOCK = "pagePanelDock"


# ─────────────────────────── Win32 helpers ────────────────────────────

def _rect(hwnd: int):
    r = wt.RECT()
    if not _user32.GetWindowRect(wt.HWND(hwnd), ctypes.byref(r)):
        raise AssertionError(f"GetWindowRect failed for hwnd {hwnd}")
    return (int(r.left), int(r.top), int(r.right - r.left),
            int(r.bottom - r.top))


def _assert_rect_close(actual, expected, tol: int, what: str):
    for i, axis in enumerate(("left", "top", "width", "height")):
        assert abs(actual[i] - expected[i]) <= tol, (
            f"{what}: {axis} was {actual[i]}, expected ~{expected[i]} "
            f"(tolerance {tol}). full rects actual={actual} expected={expected}"
        )


def _set_normal_rect(hwnd: int, rect) -> None:
    _user32.ShowWindow(wt.HWND(hwnd), _SW_RESTORE)
    time.sleep(0.3)
    assert _user32.MoveWindow(wt.HWND(hwnd), rect[0], rect[1], rect[2],
                              rect[3], True), "MoveWindow failed"
    time.sleep(0.6)


# ─────────────────────────── dock-state seam ───────────────────────────

def _dock_state(session, settle: float = 0.5) -> Dict[str, str]:
    """``{objectName: "shown:area:raised"}`` from the running instance.

    Raises rather than returning a default: an unanswered seam must fail the
    test, never silently degrade the oracle.
    """
    assert _write_to_pipe(session, "testDockStateQueryPipe", "?"), (
        "testDockStateQueryPipe unreachable — is pdfe.exe built from a tree "
        "containing the WIN-54 seam, and launched with PDFE_TEST_PIPES=1?"
    )
    time.sleep(settle)
    el = _find_pipe(session, "testDockStateResultPipe")
    assert el is not None, "testDockStateResultPipe not found"
    raw = (el.get_value() or "").strip()
    assert raw, "testDockStateResultPipe answered empty"
    out: Dict[str, str] = {}
    for record in raw.split(";"):
        name, _, rest = record.partition(":")
        assert rest, f"malformed dock record {record!r}"
        out[name] = rest
    assert len(out) > 3, f"implausibly few docks reported: {out}"
    return out


def _comparable(state: Dict[str, str]) -> Dict[str, str]:
    return {k: v for k, v in state.items() if k != _PAGES_DOCK}


def _set_dock(session, name: str, visible: bool,
              area: Optional[int] = None) -> None:
    request = f"{name}:{1 if visible else 0}"
    if area is not None:
        request += f":{area}"
    assert _write_to_pipe(session, "testDockSetPipe", request), (
        f"testDockSetPipe unreachable (request {request!r})")
    time.sleep(0.4)


def _arrange_docks(session) -> None:
    """Establish the distinctive right-area arrangement both #85 and #86 use.

    Nothing here touches Pages / Layers / Bookmarks, the three docks
    ensurePagesDockDefault() re-asserts on every launch.
    """
    _set_dock(session, "signaturePanelDock", True)     # hidden by default
    _set_dock(session, "commentPanelDock", False)      # shown by default
    _set_dock(session, "searchPanelDock", True)        # raise it in its group


# ───────────────────────────── launch/close ────────────────────────────

class _Session:
    """The minimum surface ``tests.helpers.qinvoke`` needs from a session.

    Deliberately NOT ``tests.helpers.pdfe_session.PDFESession``: that
    launcher moves and resizes the window to 1600x1000 at (0, 0) right after
    every launch (``_resize_window``, for render determinism). That would
    overwrite the restored geometry before this test could read it — the
    harness would be destroying the very property under test.
    """

    def __init__(self, proc, app, window):
        self.proc = proc
        self.app = app
        self.window = window

    @property
    def pid(self) -> int:
        return int(self.proc.pid)

    @property
    def hwnd(self) -> int:
        try:
            return int(self.window.handle)
        except Exception:  # noqa: BLE001
            for hwnd, title in list_process_windows(self.pid):
                if "PDFE" in (title or ""):
                    return hwnd
            raise AssertionError(f"no PDFE top-level window for pid {self.pid}")

    def find_optional(self, auto_id: Optional[str] = None,
                      timeout: float = 2.0, **_kw):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                for d in self.window.descendants():
                    try:
                        aid = d.element_info.automation_id or ""
                    except Exception:  # noqa: BLE001
                        continue
                    if aid.rsplit(".", 1)[-1] == auto_id or aid == auto_id:
                        return d
            except Exception:  # noqa: BLE001
                pass
            time.sleep(0.2)
        return None


def _launch(ini_path: pathlib.Path, timeout: float = 40.0) -> _Session:
    import pywinauto as _pwa

    assert PDFE_EXE and pathlib.Path(PDFE_EXE).exists(), (
        f"pdfe.exe not found at {PDFE_EXE} — build the pdfe target first")
    _kill_orphan_pdfe()
    env = _qt_env_for_determinism()
    env["PDFE_SETTINGS_PATH"] = str(ini_path)
    proc = subprocess.Popen([PDFE_EXE], env=env)

    app = _pwa.Application(backend="uia")
    deadline = time.time() + timeout
    window = None
    while time.time() < deadline:
        try:
            app.connect(process=proc.pid, timeout=1)
            w = app.window(title_re=r".*PDFE.*")
            w.wait("visible", timeout=1)
            window = w
            break
        except Exception:  # noqa: BLE001
            if proc.poll() is not None:
                raise AssertionError(
                    f"pdfe.exe exited with {proc.returncode} during launch")
            time.sleep(0.3)
    if window is None:
        try:
            proc.kill()
        finally:
            _kill_orphan_pdfe()
        raise AssertionError(
            f"pdfe.exe main window did not appear within {timeout}s")
    # Let the constructor's restoreGeometry()/restoreState() and the
    # deferred singleShot work settle before anything is measured.
    time.sleep(1.5)
    return _Session(proc, app, window)


@contextlib.contextmanager
def _pdfe(ini_path: pathlib.Path):
    """A pdfe.exe pinned to ``ini_path``. Closes cleanly on the way out."""
    session = _launch(ini_path)
    try:
        yield session
    finally:
        try:
            _clean_close(session)
        finally:
            try:
                if session.proc.poll() is None:
                    session.proc.kill()
            except Exception:  # noqa: BLE001
                pass
            _kill_orphan_pdfe()


def _clean_close(session, timeout: float = 25.0) -> None:
    """WM_CLOSE + wait for exit.

    A hard kill would bypass ``MainWindow::closeEvent()`` and therefore save
    no geometry at all — the test would then be measuring nothing.
    """
    proc = session.proc
    if proc.poll() is not None:
        return
    hwnd = session.hwnd
    assert hwnd, "no HWND to close"
    _user32.PostMessageW(wt.HWND(hwnd), _WM_CLOSE, 0, 0)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            time.sleep(0.4)   # let the INI hit the disk
            return
        time.sleep(0.2)
    raise AssertionError(
        "pdfe.exe did not exit within "
        f"{timeout}s of WM_CLOSE — geometry was never saved")


@pytest.fixture()
def ini_path():
    tmpdir = pathlib.Path(tempfile.mkdtemp(prefix="pdfe_win54_"))
    yield tmpdir / "pdfe-window-state.ini"


def _assert_saved(ini_path: pathlib.Path) -> None:
    assert ini_path.exists(), (
        f"{ini_path} was never written — the first instance did not run "
        "closeEvent(), so nothing downstream proves anything")
    text = ini_path.read_text(encoding="utf-8", errors="replace")
    assert "geometry" in text.lower(), (
        "INI exists but holds no window geometry key")


# ───────────────────────────────── #85 ─────────────────────────────────

def test_normal_geometry_and_dock_arrangement_survive_relaunch(ini_path):
    """GitHub #85 / WIN-54.

    A non-maximized geometry and a right-area dock arrangement set in one
    process must both come back in the next process launched against the
    same settings INI.
    """
    with _pdfe(ini_path) as s1:
        hwnd1 = s1.hwnd
        default_docks = _comparable(_dock_state(s1))
        default_rect = _rect(hwnd1)

        _set_normal_rect(hwnd1, _TARGET_RECT)
        assert not _user32.IsZoomed(wt.HWND(hwnd1)), (
            "precondition: the window must be non-maximized for #85")

        _arrange_docks(s1)
        established_docks = _comparable(_dock_state(s1))
        # AFTER the dock changes: showing a dock can raise the layout's
        # minimum height, so the rect that will actually be saved is the one
        # standing at close time, not the one MoveWindow asked for.
        established_rect = _rect(hwnd1)

        # Non-vacuity: what we are about to persist must differ from what a
        # pristine launch produces, or "it matched" would prove nothing.
        assert established_docks != default_docks, (
            "the dock arrangement seam did not change anything; the "
            "comparison after relaunch would be vacuous.\n"
            f"before={default_docks}\nafter ={established_docks}")
        assert abs(established_rect[0] - default_rect[0]) > 20 \
            or abs(established_rect[2] - default_rect[2]) > 20, (
            f"target geometry {established_rect} is too close to the default "
            f"{default_rect} to prove restoration")

    _assert_saved(ini_path)

    with _pdfe(ini_path) as s2:
        hwnd2 = s2.hwnd
        assert not _user32.IsZoomed(wt.HWND(hwnd2)), (
            "the relaunched window came up maximized after a non-maximized "
            "session")
        _assert_rect_close(_rect(hwnd2), established_rect, _RECT_TOL,
                           "relaunched window geometry")

        restored_docks = _comparable(_dock_state(s2))
        assert restored_docks == established_docks, (
            "dock arrangement did not survive the relaunch (Pages excluded "
            "— ensurePagesDockDefault overrides it by design).\n"
            f"saved   ={established_docks}\nrestored={restored_docks}")


# ───────────────────────────────── #86 ─────────────────────────────────

def test_maximized_state_survives_relaunch(ini_path):
    """GitHub #86 / WIN-55, part 1: the MAXIMIZED FLAG round-trips.

    Distinct from part 2 below: this one asserts only ``IsZoomed``, and says
    nothing about what geometry a restore would produce.
    """
    with _pdfe(ini_path) as s1:
        hwnd1 = s1.hwnd
        _set_normal_rect(hwnd1, _TARGET_RECT)
        assert not _user32.IsZoomed(wt.HWND(hwnd1))
        _user32.ShowWindow(wt.HWND(hwnd1), _SW_MAXIMIZE)
        time.sleep(0.8)
        assert _user32.IsZoomed(wt.HWND(hwnd1)), (
            "precondition: ShowWindow(SW_MAXIMIZE) did not maximize")

    _assert_saved(ini_path)

    with _pdfe(ini_path) as s2:
        assert _user32.IsZoomed(wt.HWND(s2.hwnd)), (
            "the relaunched window is not maximized — a maximized session "
            "reopened restored")


def test_restore_after_relaunch_yields_pre_maximize_geometry(ini_path):
    """GitHub #86 / WIN-55, part 2: the PRE-MAXIMIZE geometry round-trips.

    Deliberately a different property from part 1. ``saveGeometry()`` stores
    the maximized flag and the normal (pre-maximize) rectangle as two
    separate pieces of state, so a build that persisted the flag while
    losing or defaulting the normal rectangle would pass part 1 and fail
    here: the user's window would reopen maximized and then jump somewhere
    else the moment they hit Restore.
    """
    with _pdfe(ini_path) as s1:
        hwnd1 = s1.hwnd
        _set_normal_rect(hwnd1, _TARGET_RECT)
        pre_maximize_rect = _rect(hwnd1)
        _user32.ShowWindow(wt.HWND(hwnd1), _SW_MAXIMIZE)
        time.sleep(0.8)
        maximized_rect = _rect(hwnd1)
        assert _user32.IsZoomed(wt.HWND(hwnd1))
        # Non-vacuity: the two rects must actually differ, otherwise
        # "restore gives the pre-maximize rect" is trivially true.
        assert maximized_rect != pre_maximize_rect, (
            "maximizing did not change the window rect; this rig cannot "
            "distinguish restored from maximized")

    _assert_saved(ini_path)

    with _pdfe(ini_path) as s2:
        hwnd2 = s2.hwnd
        assert _user32.IsZoomed(wt.HWND(hwnd2)), (
            "precondition for the restore check: instance 2 must reopen "
            "maximized")
        _user32.ShowWindow(wt.HWND(hwnd2), _SW_RESTORE)
        time.sleep(0.8)
        assert not _user32.IsZoomed(wt.HWND(hwnd2))
        _assert_rect_close(_rect(hwnd2), pre_maximize_rect, _RECT_TOL,
                           "geometry after restoring the relaunched window")
