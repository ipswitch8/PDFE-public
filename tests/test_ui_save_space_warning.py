# -*- coding: utf-8 -*-
"""GitHub #100 / WIN-78: the low-disk-space warning dialog ACTUALLY appears.

WHY THIS FILE EXISTS RATHER THAN MORE GTEST
--------------------------------------------
#100 was closed once and reopened, because only the pre-check half had been
built. The pre-check is unobservable on a healthy machine: there is always
enough disk, ``SaveSpaceEstimate::insufficient`` is never true, the warning
handler is never called, and the QMessageBox that ``main.cpp`` installs is
never constructed. Every test that existed could assert the ARITHMETIC ran.
None could assert the user was told anything, which is the entire behaviour the
issue asks for.

Two things were needed and both are here:

1. **A seam at the volume boundary** — ``pdfe::core::setFreeSpaceProbeForTesting``
   replaces the single OS reading (``std::filesystem::space().available``, the
   portable spelling of ``GetDiskFreeSpaceExW``'s
   ``lpFreeBytesAvailableToCaller``) and nothing else. Reached from here
   through the ``testSimulateFreeSpacePipe`` hidden QLineEdit, gated on
   ``PDFE_TEST_PIPES``.
2. **An assertion on the DIALOG**, not on the pre-check's return value. The
   estimate, the comparison, the handler dispatch and the QMessageBox all run
   unmodified against the simulated figure, so what this observes is the real
   warning path.

HOW THE DIALOG IS DETECTED — DO NOT CHANGE THIS TO UIA
-------------------------------------------------------
``QMessageBox::warning`` is app-modal and runs its own event loop on the main
thread. pywinauto's UIA backend CANNOT enumerate such a window: measured, a Qt
modal sat open four minutes while a 240 s UIA poll reported nothing
(``tests/helpers/modal_dialog.py``). Detection here is raw Win32
``EnumWindows`` + ``WM_GETTEXT``, which a Qt modal loop answers normally and
which finds the window in well under a second.

A corollary that matters for reading a failure: because the modal blocks the
main thread, the save that triggered it has NOT completed while the dialog is
up. The dialog must be dismissed before anything about the saved file is
asserted.

Run with::

    python -m pytest tests/test_ui_save_space_warning.py -v
"""

from __future__ import annotations

import os
import shutil
import sys
import time
from pathlib import Path
from typing import List, Optional

import pytest

_HERE = Path(__file__).resolve().parent
REPO_ROOT = _HERE.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.helpers.pdfe_session import (  # noqa: E402
    PDFESession,
    PDFE_EXE,
    kill_pdfe_orphans,
    no_orphans,
)
from tests.helpers.modal_dialog import (  # noqa: E402
    close_window,
    list_process_windows,
    wait_for_window,
)
from tests.helpers.keys import hotkey_to  # noqa: E402

FIXTURES = REPO_ROOT / "tests" / "fixtures"
FIX_SINGLE = FIXTURES / "test_single.pdf"

# The warning's window title, from main.cpp's QMessageBox::warning call:
#   QObject::tr("Low disk space")
WARNING_TITLE = "Low disk space"

# The pipe that installs the simulated free-space figure (MainWindow.cpp,
# gated on PDFE_TEST_PIPES).
SIMULATE_PIPE = "testSimulateFreeSpacePipe"

# One byte free. Any real save estimate exceeds it, and it is unmistakably a
# simulated figure rather than a plausible reading, so a warning raised with
# these numbers cannot be a coincidence of the real volume.
SIMULATED_FREE_BYTES = "1"


def _has_pdfe_binary() -> bool:
    return Path(PDFE_EXE).exists()


def _has_pywinauto() -> bool:
    try:
        import pywinauto  # noqa: F401,PLC0415
        return True
    except Exception:  # noqa: BLE001
        return False


def _is_interactive_session() -> bool:
    if os.environ.get("PDFE_UI_FORCE_HEADLESS") == "1":
        return False
    return bool(os.environ.get("SESSIONNAME"))


REQUIRES_LIVE_UI = pytest.mark.skipif(
    not (_has_pdfe_binary() and _has_pywinauto() and _is_interactive_session()),
    reason="Live pdfe.exe + interactive Windows desktop session required.",
)


@pytest.fixture(autouse=True)
def _isolate_pdfe_processes():
    kill_pdfe_orphans()
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline and not no_orphans():
        kill_pdfe_orphans()
        time.sleep(0.3)
    time.sleep(1.0)
    yield
    kill_pdfe_orphans()
    deadline = time.monotonic() + 8.0
    while time.monotonic() < deadline and not no_orphans():
        kill_pdfe_orphans()
        time.sleep(0.3)
    assert no_orphans(), "pdfe.exe still running at teardown — orphan leak."


def _settle(seconds: float = 0.6) -> None:
    time.sleep(seconds)


def _write_simulate_pipe(session: PDFESession, payload: str) -> bool:
    """Set the simulated free-space figure, or ``"off"`` to clear it."""
    from tests.helpers.qinvoke import _write_to_pipe_with_retry  # noqa: PLC0415

    return _write_to_pipe_with_retry(session, SIMULATE_PIPE, payload)


def _main_hwnd(session: PDFESession) -> int:
    for hwnd, title in list_process_windows(session.pid):
        if title and WARNING_TITLE not in title:
            return hwnd
    return int(session.hwnd or 0)


def _titles(session: PDFESession) -> List[str]:
    return [t for _h, t in list_process_windows(session.pid)]


def _dismiss(hwnd: Optional[int]) -> None:
    if hwnd:
        close_window(hwnd)
        _settle(0.8)


@REQUIRES_LIVE_UI
def test_low_disk_space_warning_dialog_appears_on_simulated_full_volume(tmp_path):
    """GitHub #100: with the volume simulated full, Ctrl+S raises the
    "Low disk space" warning dialog in the running application.

    This is the assertion the first close of #100 did not have. A green
    pre-check test proves the comparison happened; only this proves the user
    was told.
    """
    if not FIX_SINGLE.exists():
        pytest.skip(f"Fixture missing: {FIX_SINGLE}")

    working = tmp_path / "issue100_target.pdf"
    shutil.copyfile(FIX_SINGLE, working)

    with PDFESession(pdf_path=str(working)) as s:
        assert s.wait_for_window_title(working.name, timeout=15), (
            "PDFE never opened the working copy; nothing below is about "
            "disk space.")
        _settle(1.5)
        main = _main_hwnd(s)
        assert main, "could not resolve the PDFE main window HWND."

        # ---- CONTROL FIRST -------------------------------------------------
        # Save with the REAL volume reading. On a machine with free disk this
        # must NOT warn. Without this, the assertion below would be satisfied
        # by a build that warns on every save, which is a worse defect than
        # the one being fixed.
        hotkey_to(main, "ctrl", "s")
        spurious = wait_for_window(s.pid, WARNING_TITLE, timeout=6.0, poll=0.25)
        _dismiss(spurious)
        assert spurious is None, (
            f"PDFE warned about disk space for an ordinary save on a healthy "
            f"volume — the pre-check warns unconditionally. Windows seen: "
            f"{_titles(s)!r}")
        _settle(1.0)

        # ---- SIMULATE A FULL VOLUME ---------------------------------------
        assert _write_simulate_pipe(s, SIMULATED_FREE_BYTES), (
            f"the {SIMULATE_PIPE} seam is unreachable. Either the build "
            f"predates GitHub #100's seam or PDFE_TEST_PIPES is not set in "
            f"the launched process — in both cases a full volume cannot be "
            f"simulated and this test would be asserting nothing.")
        _settle(0.8)

        # Dirty the document so Ctrl+S has something to save. Rotating a page
        # is the cheapest structural edit reachable from the keyboard-free
        # action seam and does not depend on canvas coordinates.
        from tests.helpers.qinvoke import activate_tool_by_name  # noqa: PLC0415
        activate_tool_by_name(s, "rotatePageCWAction")
        _settle(1.0)

        hotkey_to(main, "ctrl", "s")
        warn_hwnd = wait_for_window(s.pid, WARNING_TITLE, timeout=25.0, poll=0.25)

        assert warn_hwnd is not None, (
            f"GitHub #100: free space was simulated at "
            f"{SIMULATED_FREE_BYTES} byte(s) and a save of a multi-kilobyte "
            f"document raised NO '{WARNING_TITLE}' dialog. The pre-check may "
            f"be computing 'insufficient' correctly while nothing reaches the "
            f"user — which is exactly the half that was missing when this "
            f"issue was closed the first time. Windows seen: {_titles(s)!r}")

        # The window found is the warning, named as the user sees it.
        titles = dict(list_process_windows(s.pid))
        assert WARNING_TITLE in (titles.get(warn_hwnd) or ""), (
            f"the HWND matched but its title is "
            f"{titles.get(warn_hwnd)!r}")

        _dismiss(warn_hwnd)

        # Clear the simulation so the session's own teardown save (if any)
        # behaves normally.
        _write_simulate_pipe(s, "off")
        _settle(0.5)

        # ADVISORY, NOT A GATE: the warning does not abort the save. The file
        # is still there and still readable. If this ever fails, the pre-check
        # has quietly become a refusal, which the header of SaveSpacePrecheck.h
        # argues at length it must never be.
        assert working.exists(), (
            "the working file disappeared after a warned save — the warning "
            "path is destructive")
        assert working.stat().st_size > 0


@REQUIRES_LIVE_UI
def test_simulated_sufficient_space_raises_no_warning(tmp_path):
    """The seam supplies a FIGURE, not a verdict.

    Same pipe, same save, a large simulated figure: no dialog. Without this,
    the test above could be satisfied by a pipe that simply asked for a
    warning to be shown — which would make the warning assertion a test of
    the test harness rather than of the product's comparison.
    """
    if not FIX_SINGLE.exists():
        pytest.skip(f"Fixture missing: {FIX_SINGLE}")

    working = tmp_path / "issue100_roomy.pdf"
    shutil.copyfile(FIX_SINGLE, working)

    with PDFESession(pdf_path=str(working)) as s:
        assert s.wait_for_window_title(working.name, timeout=15)
        _settle(1.5)
        main = _main_hwnd(s)
        assert main

        # 64 GB "free" — comfortably above any estimate this fixture produces.
        assert _write_simulate_pipe(s, str(64 * 1024 ** 3)), (
            f"{SIMULATE_PIPE} unreachable; see the sibling test.")
        _settle(0.8)

        from tests.helpers.qinvoke import activate_tool_by_name  # noqa: PLC0415
        activate_tool_by_name(s, "rotatePageCWAction")
        _settle(1.0)

        hotkey_to(main, "ctrl", "s")
        warn_hwnd = wait_for_window(s.pid, WARNING_TITLE, timeout=8.0, poll=0.25)
        _dismiss(warn_hwnd)
        assert warn_hwnd is None, (
            f"a simulated 64 GB of free space still raised the "
            f"'{WARNING_TITLE}' dialog — the seam is dictating the verdict "
            f"rather than supplying the free-space figure, so the sibling "
            f"test proves nothing about the product's comparison. Windows "
            f"seen: {_titles(s)!r}")

        _write_simulate_pipe(s, "off")
        _settle(0.5)
