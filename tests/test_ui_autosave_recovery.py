# -*- coding: utf-8 -*-
"""Native-UI end-to-end tests for the PDFE crash-recovery FLOW (pywinauto).

autosave-coverage Phase-5.  These exercise the *real* startup recover/discard
prompt and *live* autosave sidecar writing through the shipping UI — the layer
GoogleTest (which drives AutosaveManager headlessly) cannot reach.

Why every test is ``@pytest.mark.autosave``
-------------------------------------------
``tests/conftest.py::_clean_autosave_sidecars`` is an autouse fixture that wipes
``%APPDATA%\\PDFE\\PDFE\\recovery`` after every test so a force-killed dirty test
cannot leave a sidecar that fires the modal recovery prompt on the next launch.
These tests DEPEND on that sidecar surviving between the setup launch and the
relaunch, so they opt out of that fixture via the ``autosave`` marker (registered
in pytest.ini) and instead clean the recovery dir themselves in teardown.

How the deterministic DIRTY edit + crash are produced
-----------------------------------------------------
* Dirty edit: fire the ``rotatePageCWAction`` QAction via the hidden
  ``testToolActivatePipe`` seam (PDFE_TEST_PIPES).  ``onRotatePageCW`` falls back
  to the current page when no page is selected and pushes a ``RotatePageCommand``
  onto the undo stack — so the document becomes dirty (undo not clean) with no
  coordinate math or annotation-tool dance.  This is the simplest reliable edit.
* Autosave: PDFE's autosave timer defaults to 30 s.  A test-only, production-inert
  seam ``PDFE_AUTOSAVE_INTERVAL_MS`` (read at the ``AutosaveManager::start()`` call
  site in MainWindow) shortens it so the sidecar appears within a bounded wait
  instead of 30 s.  See the report / MainWindow.cpp for the seam rationale.
* Crash: ``taskkill /F`` the process (NOT a clean close), so PDFE's clean-close
  ``clearRecovery()`` never runs and the sidecar survives to be recovered.

Server-2019 UIA note: the recover/discard prompt is a normal top-level
``QMessageBox`` window, located by window title + button objectName — it does NOT
use the ``menu_select`` / QMenu-popup ``FindAll`` walk that deadlocks under
GAP-A14, so no menu navigation is involved.

Run:  python -m pytest tests/test_ui_autosave_recovery.py -v
"""

from __future__ import annotations

import ctypes
import os
import shutil
import subprocess
import sys
import time
from ctypes import wintypes
from pathlib import Path

import pytest

# --- Make the repo importable regardless of pytest's launch cwd. ------------
_TESTS_DIR = Path(__file__).resolve().parent
_REPO_ROOT = _TESTS_DIR.parent
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

# pywinauto / psutil are only present on the Windows UI rig.  Import lazily so
# collection does not hard-fail on a headless box (the tests skip instead).
pywinauto = pytest.importorskip("pywinauto")
psutil = pytest.importorskip("psutil")

from pywinauto import Desktop  # noqa: E402
from tests.ui_harness.core import launch as _launch  # noqa: E402
from tests.ui_harness.core.locate import by_auto_id, invoke  # noqa: E402
from tests.helpers.autosave_cleanup import (  # noqa: E402
    SIDECAR_SUFFIX,
    discard_recovery_sidecars,
    recovery_dir,
)

FIXTURE = _TESTS_DIR / "fixtures" / "test_3page.pdf"

# Short autosave interval for the live tests (ms).  Comfortably above the 250 ms
# floor enforced by the C++ seam, small enough that a sidecar lands quickly.
AUTOSAVE_INTERVAL_MS = "1500"

TITLE_RE = r".*PDFE.*"
RECOVERY_DIALOG_TITLE = "Recover unsaved changes"


# ---------------------------------------------------------------------------
# Launch / teardown helpers (thin wrappers over the shared launch harness so
# we can control the env per-launch — the standard PDFESession always sets
# PDFE_TEST_SKIP_UNSAVED_PROMPT=1, which suppresses the prompt we test).
# ---------------------------------------------------------------------------

def _launch_custom(pdf_path=None, remove_keys=(), extra_env=None):
    """Launch pdfe.exe with a customised determinism env and connect via UIA."""
    _launch._set_process_dpi_aware()
    _launch._kill_orphan_pdfe()

    env = _launch._qt_env_for_determinism()
    for k in remove_keys:
        env.pop(k, None)
    if extra_env:
        env.update(extra_env)

    cmd = [_launch.PDFE_EXE]
    if pdf_path:
        cmd.append(os.path.abspath(str(pdf_path)))

    proc = subprocess.Popen(cmd, env=env)

    app = pywinauto.Application(backend="uia")
    deadline = time.time() + _launch.LAUNCH_TIMEOUT
    while time.time() < deadline:
        try:
            app.connect(process=proc.pid, timeout=_launch.POLL_INTERVAL)
            win = app.window(title_re=TITLE_RE)
            win.wait("visible", timeout=5)
            break
        except Exception:  # noqa: BLE001
            time.sleep(_launch.POLL_INTERVAL)
    else:
        proc.kill()
        raise TimeoutError(
            f"PDFE main window did not appear within {_launch.LAUNCH_TIMEOUT}s."
        )
    return proc, app


def _force_kill(proc):
    """Simulate a CRASH: TerminateProcess the whole tree (no clean close, so
    MainWindow::closeEvent / clearRecovery never runs and the sidecar survives).
    """
    try:
        subprocess.run(
            ["taskkill", "/F", "/T", "/PID", str(proc.pid)],
            capture_output=True,
        )
    except Exception:  # noqa: BLE001
        pass
    try:
        proc.wait(timeout=5)
    except Exception:  # noqa: BLE001
        pass


def _fire_rotate(app):
    """Fire rotatePageCWAction through the testToolActivatePipe seam -> dirty."""
    win = app.window(title_re=TITLE_RE)
    pipe = by_auto_id(win, "testToolActivatePipe", control_type="Edit",
                      timeout=15)
    pipe.set_edit_text("rotatePageCWAction")
    time.sleep(1.0)  # let the queued QAction::trigger fire + undo push land


def _sidecars():
    d = recovery_dir()
    if not d or not d.is_dir():
        return []
    try:
        return sorted(d.glob("*" + SIDECAR_SUFFIX))
    except OSError:
        return []


def _wait_for_sidecar(timeout=25):
    deadline = time.time() + timeout
    while time.time() < deadline:
        scs = _sidecars()
        if scs:
            return scs
        time.sleep(0.5)
    return []


def _wait_for_no_sidecars(timeout=20):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not _sidecars():
            return True
        time.sleep(0.5)
    return not _sidecars()


def _find_window_hwnd_by_title(pid, title):
    """Return the top-level HWND of process ``pid`` whose window text equals
    ``title`` (exact), or None.

    Server-2019 UIA does NOT surface the modal recovery ``QMessageBox`` through
    ``Application.window(...)`` / ``Desktop().windows()`` enumeration — the
    dialog is invisible to a title/auto_id search even though it is on screen.
    Raw Win32 ``EnumWindows`` finds it reliably; once we have the HWND we wrap it
    with ``Desktop(backend="uia").window(handle=hwnd)`` and UIA then sees the
    dialog subtree (buttons included) correctly.
    """
    user32 = ctypes.windll.user32
    proc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
    found = []

    def _cb(hwnd, _lparam):
        pid_out = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid_out))
        if pid_out.value == pid and user32.IsWindowVisible(hwnd):
            buf = ctypes.create_unicode_buffer(512)
            user32.GetWindowTextW(hwnd, buf, 512)
            if buf.value == title:
                found.append(hwnd)
        return True

    user32.EnumWindows(proc(_cb), 0)
    return found[0] if found else None


def _find_recovery_dialog(pid, timeout=25):
    """Return a UIA WindowSpecification for the recovery prompt, located via its
    raw HWND (see _find_window_hwnd_by_title for why UIA search alone fails)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        hwnd = _find_window_hwnd_by_title(pid, RECOVERY_DIALOG_TITLE)
        if hwnd:
            dlg = Desktop(backend="uia").window(handle=hwnd)
            try:
                dlg.wait("exists", timeout=2)
                return dlg
            except Exception:  # noqa: BLE001
                pass
        time.sleep(0.5)
    raise TimeoutError("recovery prompt dialog HWND not found")


def _click_dialog_button(dlg, object_name, accessible_name):
    """Invoke a QMessageBox button.

    Qt's UIA bridge exposes the button objectName as the TAIL of a dotted
    AutomationId (e.g. "QApplication.recoveryPromptDialog.qt_msgbox_buttonbox.
    recoveryRecoverButton"), so an exact short-name match fails. Match on the
    objectName suffix among the dialog's Button descendants (keyed on the stable
    objectName, per project convention), falling back to the accessible name.
    """
    for btn in dlg.descendants(control_type="Button"):
        try:
            aid = btn.element_info.automation_id or ""
        except Exception:  # noqa: BLE001
            aid = ""
        if aid.endswith(object_name) or aid == object_name:
            invoke(btn)
            return
    # Fallback: accessible name (set via setAccessibleName in MainWindow).
    btn = dlg.child_window(title=accessible_name, control_type="Button")
    btn.wait("exists", timeout=5)
    invoke(btn)


def _wait_title_contains(app, needle, timeout=15):
    deadline = time.time() + timeout
    win = app.window(title_re=TITLE_RE)
    last = ""
    while time.time() < deadline:
        try:
            last = win.window_text()
            if needle in last:
                return last
        except Exception:  # noqa: BLE001
            pass
        time.sleep(0.5)
    return last


@pytest.fixture(autouse=True)
def _teardown_recovery():
    """Belt-and-suspenders cleanup: the autosave marker exempts these tests from
    the conftest sidecar wipe, so kill orphans + clean the recovery dir here."""
    discard_recovery_sidecars()  # start from a clean slate
    yield
    try:
        _launch._kill_orphan_pdfe()
    except Exception:  # noqa: BLE001
        pass
    discard_recovery_sidecars()


def _make_dirty_sidecar_then_crash(pdf_path):
    """Launch (determinism env + short interval), make a dirty edit, wait for a
    recovery sidecar, then CRASH-kill.  Returns the session id (recovery pdf
    stem) captured from the sidecar name."""
    proc, app = _launch_custom(
        pdf_path=pdf_path,
        extra_env={"PDFE_AUTOSAVE_INTERVAL_MS": AUTOSAVE_INTERVAL_MS},
    )
    try:
        _fire_rotate(app)
        scs = _wait_for_sidecar()
        assert scs, (
            "no recovery sidecar appeared after a dirty rotate edit + "
            f"{AUTOSAVE_INTERVAL_MS}ms autosave interval"
        )
        session_id = scs[0].name[: -len(SIDECAR_SUFFIX)]
    finally:
        _force_kill(proc)
    # Sidecar must survive the crash (clean-close clearRecovery never ran).
    assert _sidecars(), "recovery sidecar did not survive the crash kill"
    return session_id


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

@pytest.mark.autosave
def test_recover_path_restores_unsaved_edit(tmp_path):
    """Dirty edit -> autosave sidecar -> crash -> relaunch -> Recover opens the
    recovered copy (title switches to the recovery <uuid>.pdf)."""
    doc = tmp_path / "recover_doc.pdf"
    shutil.copy(FIXTURE, doc)

    session_id = _make_dirty_sidecar_then_crash(doc)

    # Relaunch WITHOUT the skip-prompt seam and with NO document, so the startup
    # recovery prompt is the only thing on screen.
    proc, app = _launch_custom(
        pdf_path=None,
        remove_keys=("PDFE_TEST_SKIP_UNSAVED_PROMPT",),
    )
    try:
        dlg = _find_recovery_dialog(proc.pid)
        _click_dialog_button(dlg, "recoveryRecoverButton",
                             "Recover unsaved changes")

        # The recovery copy is <session_id>.pdf; after Recover the window title
        # becomes "PDFE - [<...session_id...>.pdf]".  Proving that copy is open
        # proves the edit was recovered (the copy was written from the dirty
        # in-memory document).
        title = _wait_title_contains(app, session_id, timeout=15)
        assert session_id in title, (
            f"recovered document not open; window title was {title!r}, "
            f"expected it to contain recovery session id {session_id!r}"
        )
        assert title.strip() != "PDFE", "no document open after Recover"
    finally:
        _force_kill(proc)


@pytest.mark.autosave
def test_discard_path_removes_sidecars(tmp_path):
    """Dirty edit -> autosave sidecar -> crash -> relaunch -> Discard removes all
    recovery sidecars from the recovery dir."""
    doc = tmp_path / "discard_doc.pdf"
    shutil.copy(FIXTURE, doc)

    _make_dirty_sidecar_then_crash(doc)
    assert _sidecars(), "precondition: a sidecar must exist before Discard"

    proc, app = _launch_custom(
        pdf_path=None,
        remove_keys=("PDFE_TEST_SKIP_UNSAVED_PROMPT",),
    )
    try:
        dlg = _find_recovery_dialog(proc.pid)
        _click_dialog_button(dlg, "recoveryDiscardButton",
                             "Discard recovered changes")

        assert _wait_for_no_sidecars(timeout=15), (
            "recovery sidecars still present after clicking Discard: "
            f"{[p.name for p in _sidecars()]}"
        )
    finally:
        _force_kill(proc)


@pytest.mark.autosave
def test_live_autosave_writes_sidecar_during_edit(tmp_path):
    """While the app is RUNNING under a real dirty edit, a recovery sidecar
    (.pdfe_recovery.json + the .pdf copy) appears in the recovery dir within a
    bounded wait (autosave interval shortened via the test seam)."""
    doc = tmp_path / "live_doc.pdf"
    shutil.copy(FIXTURE, doc)

    proc, app = _launch_custom(
        pdf_path=doc,
        extra_env={"PDFE_AUTOSAVE_INTERVAL_MS": AUTOSAVE_INTERVAL_MS},
    )
    try:
        # Clean slate: startup discardAllRecoverable (skip-prompt seam) already
        # cleared any stale sidecar, so anything we see now is from THIS edit.
        _fire_rotate(app)

        scs = _wait_for_sidecar(timeout=25)
        assert scs, (
            "no recovery sidecar appeared while the app was live after a dirty "
            f"edit within a {AUTOSAVE_INTERVAL_MS}ms autosave interval"
        )
        # The companion recovery PDF copy must exist alongside the sidecar.
        session_id = scs[0].name[: -len(SIDECAR_SUFFIX)]
        copy_pdf = recovery_dir() / (session_id + ".pdf")
        assert copy_pdf.exists(), (
            f"sidecar present but recovery copy {copy_pdf.name} missing"
        )
        assert copy_pdf.stat().st_size > 0, "recovery copy is empty"
    finally:
        _force_kill(proc)
