# -*- coding: utf-8 -*-
"""GitHub #77 (WIN-19): a REAL Explorer-to-PDFE drag-drop opens the document.

WHY A SYNTHETIC QDropEvent IS NOT ENOUGH
----------------------------------------
``tests/test_ui_clipboard_dragdrop.cpp`` posts a hand-built ``QDropEvent``
straight at ``MainWindow``. That is a good test of ``dropEvent``'s branching --
it can manufacture payload shapes a live drag cannot reliably produce -- but it
starts INSIDE Qt. Everything between the shell and ``dropEvent`` is skipped:
Explorer's ``IDropSource``, OLE's ``DoDragDrop`` modal loop, Qt's Windows
``IDropTarget`` registration on the ``MainWindow`` HWND, the
``CF_HDROP`` -> ``text/uri-list`` conversion, and the fact that
``setAcceptDrops(true)`` was actually called on a window that is actually
registered with OLE. A regression in any of those leaves the synthetic test
green and the product broken for every user who drags a file out of Explorer.

So this test drives the OS: a live Explorer window, a physical
``SendInput`` press-move-release over the real item, and an oracle that asks
the application whether a DOCUMENT is now open -- not whether the process
survived.

NO SILENT FALLBACK
------------------
If the input desktop is not reachable (RDP disconnected -- see
docs memory ``rdp_disconnect_desktop_measured``) this test FAILS. It must not
degrade into a synthetic QDropEvent, because that is precisely the weaker
instrument it exists to replace: a green synthetic run would then be reported
as coverage of the OLE path it never touched.

COORDINATES
-----------
The press point is the item's own UIA rectangle, read from the live Explorer
window -- not a guess. The release point is the centre of PDFE's window rect.
Neither is a canvas-page coordinate, so ``canvas_geometry.point_on_page`` does
not apply here: the drop target is the MainWindow itself (``setAcceptDrops`` at
MainWindow.cpp:539), and where inside it the pointer lands is irrelevant to the
drop.

Run:  python -m pytest tests/test_ui_explorer_drop.py -v -s
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
REPO_ROOT = _HERE.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

FIXTURES = REPO_ROOT / "tests" / "fixtures"
RUNTIME = REPO_ROOT / "tests" / "_runtime"
CORPUS = FIXTURES / "test_3page.pdf"
DROP_DIR_NAME = "explorer_drop"
DROP_FILE_NAME = "explorer_drop_target.pdf"
EXPECTED_PAGES = 3


def _has(mod: str) -> bool:
    try:
        __import__(mod)
        return True
    except Exception:  # noqa: BLE001
        return False


def _has_pdfe() -> bool:
    try:
        from tests.helpers.pdfe_session import PDFE_EXE
        return Path(PDFE_EXE).exists()
    except Exception:  # noqa: BLE001
        return False


def _is_interactive() -> bool:
    if os.environ.get("PDFE_UI_FORCE_HEADLESS") == "1":
        return False
    if sys.platform != "win32":
        return False
    return bool(os.environ.get("SESSIONNAME"))


pytestmark = [
    pytest.mark.skipif(not _has_pdfe(), reason="pdfe.exe not built"),
    pytest.mark.skipif(not _is_interactive(),
                       reason="a live drag needs an interactive Windows desktop"),
    pytest.mark.skipif(not _has("pywinauto"), reason="pywinauto not installed"),
    pytest.mark.skipif(not _has("psutil"), reason="psutil not installed"),
    pytest.mark.skipif(not CORPUS.exists(),
                       reason="tests/fixtures/test_3page.pdf missing"),
]


# ---------------------------------------------------------------------------
# Win32 plumbing
# ---------------------------------------------------------------------------

_user32 = ctypes.WinDLL("user32", use_last_error=True)
_WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

SWP_NOACTIVATE = 0x0010
SWP_FRAMECHANGED = 0x0020
WM_CLOSE = 0x0010


def _input_desktop_reachable(timeout: float = 30.0) -> bool:
    """True when this session owns a usable INPUT desktop.

    ``OpenInputDesktop`` is the measured-correct probe (memory:
    rdp_disconnect_desktop_measured). A WTS "Active" state is reported up to
    11 s before input actually works, so it is not used here.

    Polled rather than sampled once: the handle is momentarily unavailable
    while the session switches desktops (a screensaver arming, a lock screen
    appearing and going away, an RDP reconnect settling). One such blip was
    observed mid-run between two passing executions. A blip is not an outage --
    but a real outage still ends here, as a FAILURE, never a skip.
    """
    DESKTOP_SWITCHDESKTOP = 0x0100
    deadline = time.time() + timeout
    while True:
        h = _user32.OpenInputDesktop(0, False, DESKTOP_SWITCHDESKTOP)
        if h:
            _user32.CloseDesktop(h)
            return True
        if time.time() >= deadline:
            return False
        time.sleep(1.0)


def _class_name(hwnd: int) -> str:
    buf = ctypes.create_unicode_buffer(256)
    _user32.GetClassNameW(wt.HWND(hwnd), buf, 256)
    return buf.value


def _window_title(hwnd: int) -> str:
    buf = ctypes.create_unicode_buffer(512)
    _user32.GetWindowTextW(wt.HWND(hwnd), buf, 512)
    return buf.value


def _window_rect(hwnd: int):
    r = wt.RECT()
    _user32.GetWindowRect(wt.HWND(hwnd), ctypes.byref(r))
    return r.left, r.top, r.right, r.bottom


def _move(hwnd: int, x: int, y: int, w: int, h: int) -> None:
    _user32.SetWindowPos(wt.HWND(hwnd), 0, x, y, w, h,
                         SWP_NOACTIVATE | SWP_FRAMECHANGED)
    time.sleep(0.4)


def _find_explorer_window(folder_title: str, timeout: float = 25.0) -> int:
    """HWND of a visible Explorer window whose caption is *folder_title*.

    Matched by window CLASS (``CabinetWClass``) rather than owning process:
    ``explorer.exe <dir>`` usually hands the request to the already-running
    shell process, so a PID-scoped search finds nothing.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        found = []

        def _cb(hwnd, _lp):
            if not _user32.IsWindowVisible(hwnd):
                return True
            if _class_name(int(hwnd)) != "CabinetWClass":
                return True
            if folder_title.lower() in _window_title(int(hwnd)).lower():
                found.append(int(hwnd))
                return False
            return True

        _user32.EnumWindows(_WNDENUMPROC(_cb), 0)
        if found:
            return found[0]
        time.sleep(0.4)
    return 0


def _explorer_item_rect(explorer_hwnd: int, stem: str, timeout: float = 20.0):
    """Screen rect of the list item named *stem* inside the Explorer window.

    Explorer is an ordinary UIA provider (the modal-blindness problem is
    specific to a Qt ``QDialog::exec()``), so its item tree is enumerable. If
    the item cannot be found we FAIL rather than guessing an offset into the
    file pane -- a guessed press would start a rubber-band selection instead of
    a drag and the test would report the wrong reason.
    """
    from pywinauto import Desktop
    deadline = time.time() + timeout
    last_seen = []
    while time.time() < deadline:
        try:
            win = Desktop(backend="uia").window(handle=explorer_hwnd)
            items = win.descendants(control_type="ListItem")
            last_seen = []
            for it in items:
                try:
                    name = it.window_text() or ""
                except Exception:  # noqa: BLE001
                    continue
                last_seen.append(name)
                # Explorer hides known extensions by default, so match the stem.
                if name == stem or name.split(".")[0] == stem:
                    r = it.rectangle()
                    if r.right > r.left and r.bottom > r.top:
                        return r
        except Exception:  # noqa: BLE001
            pass
        time.sleep(0.5)
    raise AssertionError(
        "Explorer item %r never appeared in the file pane (items seen: %r). "
        "Without the item's own rectangle there is no honest press point."
        % (stem, last_seen)
    )


# ---------------------------------------------------------------------------
# The drag itself
# ---------------------------------------------------------------------------

def _send_mouse(flags: int, x: int | None = None, y: int | None = None) -> None:
    from tests.helpers.win32_input import (
        INPUT, INPUT_MOUSE, MOUSEEVENTF_ABSOLUTE, SendInput, _abs_coords,
    )
    inp = (INPUT * 1)()
    inp[0].type = INPUT_MOUSE
    if x is not None:
        ax, ay = _abs_coords(x, y)
        inp[0].union.mi.dx = ax
        inp[0].union.mi.dy = ay
        flags |= MOUSEEVENTF_ABSOLUTE
    inp[0].union.mi.dwFlags = flags
    SendInput(1, inp, ctypes.sizeof(INPUT))


def _activate(hwnd: int, timeout: float = 3.0) -> bool:
    """Bring *hwnd* to the foreground and confirm it got there.

    THIS IS LOAD-BEARING, and it cost an afternoon to find. A press on an item
    in a BACKGROUND Explorer window selects the item and then goes nowhere: the
    shell treats the activating click as an activation, not as the start of a
    drag, so ``DoDragDrop`` is never entered and the release is just a click.
    The symptom is indistinguishable from "PDFE refused the drop" -- the drag
    silently does not exist. Measured: identical press-move-release sequences,
    background Explorer -> nothing at all; foreground Explorer -> the document
    opens, and an Explorer-to-Explorer control drag moves the file.

    Note also that the drag's existence CANNOT be probed with
    ``GetCursorInfo``: during a shell drag the reported cursor handle stays the
    plain arrow, so a cursor-change check reports "no drag" for a drag that is
    demonstrably running. Use an effect (the file moved / the document opened).
    """
    _user32.SetForegroundWindow(wt.HWND(hwnd))
    deadline = time.time() + timeout
    while time.time() < deadline:
        if int(_user32.GetForegroundWindow() or 0) == hwnd:
            time.sleep(0.4)
            return True
        time.sleep(0.15)
        _user32.SetForegroundWindow(wt.HWND(hwnd))
    return False


def _ole_drag(x1: int, y1: int, x2: int, y2: int) -> None:
    """A press-move-release slow enough for OLE's modal drag loop to track.

    ``DoDragDrop`` runs its own message loop on the source thread and only
    begins once the pointer has moved past ``SM_CXDRAG`` with the button down.
    A fast interpolation (the 10-step / 0.3 s default in
    ``win32_input.send_drag``) can put the release inside the same input burst
    as the press, so the shell treats the whole thing as a click. Hence ~40
    tracked steps, and a dwell at the target before the release so the drop
    target's ``dragEnterEvent`` / ``dragMoveEvent`` have run and settled on an
    accepted action.
    """
    from tests.helpers.win32_input import (
        MOUSEEVENTF_MOVE, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP,
    )
    _send_mouse(MOUSEEVENTF_MOVE, x1, y1)
    time.sleep(0.4)
    _send_mouse(MOUSEEVENTF_LEFTDOWN, x1, y1)
    time.sleep(0.1)

    steps = 40
    for i in range(1, steps + 1):
        t = i / steps
        _send_mouse(MOUSEEVENTF_MOVE,
                    int(x1 + (x2 - x1) * t), int(y1 + (y2 - y1) * t))
        time.sleep(0.04)

    for _ in range(8):
        _send_mouse(MOUSEEVENTF_MOVE, x2, y2)
        time.sleep(0.06)

    _send_mouse(MOUSEEVENTF_LEFTUP, x2, y2)
    time.sleep(0.5)


# ---------------------------------------------------------------------------
# Oracles
# ---------------------------------------------------------------------------

def _page_count(session) -> int:
    """Pages in the open document; -1 when the seam did not answer."""
    from tests.helpers import qinvoke
    if not qinvoke._write_to_pipe(session, "testPageCountQueryPipe", "1",
                                  settle=0.25):
        return -1
    raw = qinvoke._read_result_pipe(session, "testPageCountResultPipe")
    try:
        return int((raw or "").strip())
    except (TypeError, ValueError):
        return -1


# ---------------------------------------------------------------------------

def test_real_explorer_drag_drop_opens_the_document():
    from tests.helpers.click_safety import allow_unsafe_clicks
    from tests.helpers.pdfe_session import PDFESession, list_pdfe_pids

    assert _input_desktop_reachable(), (
        "OpenInputDesktop failed: this session has no input desktop (an RDP "
        "console disconnect does exactly this). A real drag cannot be driven "
        "here. FAILING rather than substituting a synthetic QDropEvent -- the "
        "synthetic form is the coverage this test exists to replace."
    )

    drop_dir = RUNTIME / DROP_DIR_NAME
    if drop_dir.exists():
        shutil.rmtree(drop_dir, ignore_errors=True)
    drop_dir.mkdir(parents=True, exist_ok=True)
    dropped = drop_dir / DROP_FILE_NAME
    shutil.copy2(CORPUS, dropped)

    explorer_hwnd = 0
    try:
        # 1. PDFE with NO document -- so "a document is open" afterwards can
        #    only have come from the drop.
        with PDFESession() as s:
            assert s.wait_for_window_title("PDFE"), "PDFE never appeared"
            s.wait_for_idle()
            time.sleep(1.0)

            title_before = s.window.window_text() or ""
            assert "[" not in title_before, (
                "expected an empty PDFE (title %r); a document was already "
                "open, so the drop oracle would be meaningless" % title_before)

            pdfe_hwnd = s.hwnd
            _move(pdfe_hwnd, 940, 0, 930, 1000)
            pl, pt, pr, pb = _window_rect(pdfe_hwnd)
            drop_x, drop_y = (pl + pr) // 2, (pt + pb) // 2

            # 2. A live Explorer window on the folder holding the file.
            subprocess.Popen(["explorer.exe", str(drop_dir)])
            explorer_hwnd = _find_explorer_window(DROP_DIR_NAME)
            assert explorer_hwnd, (
                "no Explorer window titled %r appeared -- there is no OLE drag "
                "source to drag from" % DROP_DIR_NAME)
            _move(explorer_hwnd, 0, 0, 880, 900)
            time.sleep(1.0)

            item = _explorer_item_rect(explorer_hwnd, Path(DROP_FILE_NAME).stem)
            # Near the icon/label end of the row rather than its far right: in
            # Details view the row rect spans the whole pane, and the right of
            # it is empty column space.
            press_x = min(item.left + 60, (item.left + item.right) // 2)
            press_y = (item.top + item.bottom) // 2

            # The shell will not start a drag from a background window.
            assert _activate(explorer_hwnd), (
                "could not bring the Explorer window to the foreground; a "
                "press on a background window is consumed as an activation "
                "click and no drag is ever started")

            assert press_x < pl, (
                "the Explorer item at x=%d overlaps the PDFE window starting "
                "at x=%d; the press would land on PDFE, not on the item"
                % (press_x, pl))

            # 3. The drag. The press is legitimately outside PDFE (it is on
            #    Explorer), which is exactly what allow_unsafe_clicks is for;
            #    both endpoints are computed from live window rects.
            with allow_unsafe_clicks():
                _ole_drag(press_x, press_y, drop_x, drop_y)

            # 4. Oracle: a DOCUMENT is open, and it is the one we dropped.
            opened = s.wait_for_window_title(Path(DROP_FILE_NAME).stem,
                                             timeout=25)
            title_after = s.window.window_text() or ""
            assert opened, (
                "the dropped document did not open: window title is still %r "
                "(was %r before the drag). The Explorer->PDFE OLE drop path "
                "did not deliver the file." % (title_after, title_before))
            pages = _page_count(s)
            assert pages == EXPECTED_PAGES, (
                "title changed to %r but the page-count seam reports %d, not "
                "%d -- the document did not actually load"
                % (title_after, pages, EXPECTED_PAGES))
            assert s.pid in list_pdfe_pids(), "PDFE died during the drop"
    finally:
        if explorer_hwnd:
            _user32.PostMessageW(wt.HWND(explorer_hwnd), wt.UINT(WM_CLOSE), 0, 0)
            time.sleep(0.8)
        from tests.helpers.pdfe_session import kill_pdfe_orphans
        kill_pdfe_orphans()
