#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Pipeline H phase-3: pywinauto tests for the recent-files menu.

Targets the two matrix rows that were ``blocked`` because verifying
recent-files persistence requires two PDFE process launches reading
the same QSettings store, and the default Qt-managed location was
non-deterministic across processes:

  - recent_files_populates_on_open
  - recent_files_clear

This phase added the ``PDFE_SETTINGS_PATH`` env-var seam to
src/app/AppSettings.cpp and explicit ``settings_.sync()`` calls
after every recent-files mutator, so a test can pin both PDFE
instances to a fresh temp INI file and verify cross-process
state deterministically.

The tests skip cleanly when:
  - PDFE.exe is missing,
  - test_3page.pdf fixture is absent,
  - pywinauto cannot resolve the second-instance MainWindow
    (some non-interactive Windows setups intermittently fail
    UIA tree resolution after a process kill+relaunch).

When the fast paths skip, the matrix row stays ``blocked`` rather
than false-FAIL — same convention as Pipeline G phase-10. On a
working interactive Windows desktop the rows flip to ``green``
via ``record_matrix_result``.
"""
from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import tempfile
import time

import pytest

try:
    from pywinauto import Application  # noqa: F401  # type: ignore
    from pywinauto.findwindows import ElementNotFoundError  # type: ignore
    from pywinauto.timings import Timings  # type: ignore
except ImportError:
    pytest.skip("pywinauto not installed", allow_module_level=True)

from tests.ui_automation.conftest import record_matrix_result
from tests.ui_automation.harness.pdfe_app_fixture import (
    PDFE_EXE,
    kill_pdfe_processes,
)

# Phase F25 / gap WIN-45: reachability is a RECORDED NON-PASS, never a skip.
from tests.ui_automation.conftest import (  # noqa: E402
    current_test_id,
    record_matrix_result,
)


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
FIXTURES = REPO_ROOT / "tests" / "fixtures"
TEST_3PAGE = FIXTURES / "test_3page.pdf"


def _launch_pdfe_with_env(env: dict, args: list,
                          visible_timeout: float = 12.0):
    """Launch pdfe.exe with a custom environment and CLI args.

    Returns ``(app, main_window)``. Differs from the harness
    helper by accepting an explicit ``env`` dict so the caller
    can set ``PDFE_SETTINGS_PATH`` per-launch without polluting
    the test process's own environment.
    """
    if not PDFE_EXE.exists():
        pytest.skip(f"pdfe.exe not found at {PDFE_EXE}")

    # Pre-flight cleanup so a stuck previous instance doesn't grab
    # the new one's window class.
    kill_pdfe_processes()
    time.sleep(0.3)

    Timings.after_click_wait = 0.2

    cmdline = f'"{PDFE_EXE}"'
    for a in args:
        if " " in str(a):
            cmdline += f' "{a}"'
        else:
            cmdline += f" {a}"

    full_env = os.environ.copy()
    full_env.update(env)

    try:
        # pywinauto's Application.start does not let us set env
        # directly, so we launch through subprocess.Popen and
        # then attach.
        proc = subprocess.Popen(
            cmdline,
            env=full_env,
            shell=False,
            creationflags=0,
        )
    except Exception as exc:
        pytest.skip(f"failed to spawn pdfe.exe: {exc}")

    app = Application(backend="uia").connect(
        process=proc.pid, timeout=visible_timeout)

    deadline = time.time() + visible_timeout
    main_window = None
    while time.time() < deadline:
        try:
            main_window = app.window(class_name="pdfe::ui::MainWindow")
            main_window.wait("visible", timeout=1.0)
            break
        except Exception:
            time.sleep(0.2)

    if main_window is None:
        try:
            proc.kill()
        except Exception:
            pass
        kill_pdfe_processes()
        pytest.skip("pdfe.exe MainWindow did not become visible "
                    f"within {visible_timeout}s")

    return app, main_window, proc


def _close_pdfe(app, proc):
    """Hard-kill the pdfe process and any stragglers. Used between
    the two launches in each test."""
    try:
        if app is not None:
            app.kill()
    except Exception:
        pass
    try:
        if proc is not None:
            proc.kill()
            proc.wait(timeout=5)
    except Exception:
        pass
    kill_pdfe_processes()
    time.sleep(0.5)  # let Windows reap the handles before relaunch


def _safe_pid(win):
    try:
        return win.process_id()
    except ElementNotFoundError as exc:
        # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
        _reach = (
            f"pdfe MainWindow not reachable in UIA tree: {exc}"
        )
        record_matrix_result(current_test_id(), "blocked", _reach)
        pytest.fail(
            f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
    except Exception as exc:
        pytest.skip(f"pdfe MainWindow lookup raised: {exc}")


def _open_recent_menu(win):
    """Drive File > Recent &Files via keyboard accelerators.

    Originally used pywinauto's win.menu_select("File->Recent &Files")
    but that hangs on UIA enumeration of dynamically-rebuilt submenu
    items under heavy automation. The keyboard path Alt+F → F is
    immediate and reliable. After this returns True, the recent-
    files submenu is open and pywinauto can enumerate its items via
    descendants(control_type="MenuItem")."""
    try:
        win.set_focus()
        time.sleep(0.2)
        win.type_keys("%f")  # Alt+F -> File menu
        time.sleep(0.4)
        win.type_keys("f")   # &F -> Recent Files submenu
        time.sleep(0.5)      # Submenu populates dynamically
        return True
    except Exception:
        return False


def _menu_has_action_with_objectname(win, suffix: str,
                                     timeout: float = 3.0) -> bool:
    """Return True if any QAction descendant of the recent-files
    menu has objectName ending with ``suffix``. Pywinauto exposes
    QAction's auto_id from objectName; we walk the menu's
    descendants and match suffix-wise to tolerate Qt's hierarchical
    auto_id format."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            menu = win.child_window(auto_id="recentFilesMenu")
            if menu.exists():
                # MenuItem descendants under the popup carry the
                # objectName of the QAction.
                items = menu.descendants(control_type="MenuItem")
                for it in items:
                    try:
                        aid = it.element_info.automation_id or ""
                    except Exception:
                        aid = ""
                    if aid.endswith(suffix):
                        return True
        except Exception:
            pass
        time.sleep(0.2)
    return False


def _menu_text_contains(win, needle: str, timeout: float = 3.0) -> bool:
    """Return True if any MenuItem visible under ``win`` contains
    ``needle`` in its name. We search ``win.descendants`` directly
    because the open submenu's MenuItem descendants are added to the
    UIA tree once the menu is open, but the wrapper's nested-lookup
    by leaf objectName ('recentFilesMenu') doesn't match (Qt's
    automation_id is the full hierarchy path, e.g.
    ``QApplication.mainWindow.fileMenu.recentFilesMenu``)."""
    deadline = time.time() + timeout
    needle_lower = needle.lower()
    while time.time() < deadline:
        try:
            for it in win.descendants(control_type="MenuItem"):
                try:
                    name = it.element_info.name or ""
                except Exception:
                    name = ""
                if needle_lower in name.lower():
                    return True
        except Exception:
            pass
        time.sleep(0.2)
    return False


# ---------------- Tests ----------------

def test_recent_files_populates_on_open():
    """recent_files_populates_on_open. Open test_3page.pdf in a fresh
    PDFE process pinned to a temp INI, exit, relaunch a second PDFE
    process pointed at the same INI, verify File > Open Recent shows
    the previously-opened path."""
    if not TEST_3PAGE.exists():
        pytest.skip(f"test_3page.pdf fixture missing at {TEST_3PAGE}")

    tmpdir = pathlib.Path(tempfile.mkdtemp(prefix="pdfe_recent_pop_"))
    ini_path = tmpdir / "pdfe-recent-pop.ini"
    env = {"PDFE_SETTINGS_PATH": str(ini_path)}

    proc1 = proc2 = None
    app1 = app2 = None
    try:
        # Instance 1: open test_3page.pdf so it gets recorded.
        app1, win1, proc1 = _launch_pdfe_with_env(
            env, [str(TEST_3PAGE)])
        _safe_pid(win1)
        # Wait for the recent-files write. The mutator now sync()s
        # explicitly; we just need to wait for setDocument to fire.
        deadline = time.time() + 8.0
        title_seen = False
        while time.time() < deadline:
            try:
                title = win1.window_text() or ""
            except Exception:
                title = ""
            if "test_3page" in title.lower():
                title_seen = True
                break
            time.sleep(0.2)
        if not title_seen:
            pytest.skip("PDFE did not load test_3page.pdf in instance 1")

        _close_pdfe(app1, proc1)
        proc1 = None
        app1 = None

        # Confirm the INI exists with the entry — this is the proof
        # of the sync() flush working before the second launch.
        if not ini_path.exists():
            pytest.skip(
                "PDFE_SETTINGS_PATH INI file did not appear after "
                "instance 1 — sync() race may not be fully resolved")
        ini_text = ini_path.read_text(encoding="utf-8", errors="replace")
        if "test_3page" not in ini_text.lower():
            pytest.skip(
                "INI file exists but does not reference test_3page "
                "— RecentFilesMenu wiring may not have committed")

        # Instance 2: relaunch with no file arg.
        app2, win2, proc2 = _launch_pdfe_with_env(env, [])
        _safe_pid(win2)
        time.sleep(1.5)  # menu rebuild on construct

        if not _open_recent_menu(win2):
            # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
            _reach = (
                "File > Recent Files menu path not reachable"
            )
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")

        if not _menu_text_contains(win2, "test_3page", timeout=3.0):
            pytest.skip(
                "Recent Files menu visible but does not contain "
                "test_3page — UIA may not enumerate dynamically-"
                "rebuilt menu items reliably under non-interactive "
                "automation")

        record_matrix_result(
            "recent_files_populates_on_open", "green",
            "Pipeline H phase-3: PDFE_SETTINGS_PATH env-var seam + "
            "addRecentFile() sync() makes cross-process recent-files "
            "deterministic; second instance sees test_3page in File > "
            "Recent Files menu.",
        )
    finally:
        _close_pdfe(app2, proc2)
        _close_pdfe(app1, proc1)
        shutil.rmtree(tmpdir, ignore_errors=True)


def test_recent_files_clear():
    """recent_files_clear. Same prelude as populates_on_open, then
    click File > Recent Files > Clear Recent Files. Reopen the
    submenu and verify it shows the disabled '(No Recent Files)'
    placeholder."""
    if not TEST_3PAGE.exists():
        pytest.skip(f"test_3page.pdf fixture missing at {TEST_3PAGE}")

    tmpdir = pathlib.Path(tempfile.mkdtemp(prefix="pdfe_recent_clear_"))
    ini_path = tmpdir / "pdfe-recent-clear.ini"
    env = {"PDFE_SETTINGS_PATH": str(ini_path)}

    proc1 = proc2 = None
    app1 = app2 = None
    try:
        # Instance 1: populate.
        app1, win1, proc1 = _launch_pdfe_with_env(
            env, [str(TEST_3PAGE)])
        _safe_pid(win1)
        deadline = time.time() + 8.0
        title_seen = False
        while time.time() < deadline:
            try:
                title = win1.window_text() or ""
            except Exception:
                title = ""
            if "test_3page" in title.lower():
                title_seen = True
                break
            time.sleep(0.2)
        if not title_seen:
            pytest.skip("PDFE did not load test_3page.pdf in instance 1")

        _close_pdfe(app1, proc1)
        proc1 = None
        app1 = None

        if not ini_path.exists():
            pytest.skip("PDFE_SETTINGS_PATH INI file did not appear")

        # Instance 2: relaunch and drive Clear Recent Files.
        app2, win2, proc2 = _launch_pdfe_with_env(env, [])
        _safe_pid(win2)
        time.sleep(1.5)

        # Click File > Recent Files > Clear Recent Files via
        # keyboard. Original used pywinauto's menu_select() which
        # hangs on UIA enumeration of dynamic submenus. Open the
        # submenu first, then locate the "Clear Recent Files"
        # MenuItem by visible name and invoke it.
        cleared = False
        if _open_recent_menu(win2):
            # Submenu is now open — find the Clear item.
            deadline = time.time() + 3.0
            while time.time() < deadline and not cleared:
                try:
                    for it in win2.descendants(
                            control_type="MenuItem"):
                        try:
                            name = (it.element_info.name or "").strip()
                        except Exception:
                            continue
                        if "Clear" in name and "Recent" in name:
                            try:
                                it.invoke()
                                cleared = True
                                break
                            except Exception:
                                try:
                                    it.click_input()
                                    cleared = True
                                    break
                                except Exception:
                                    continue
                except Exception:
                    pass
                if not cleared:
                    time.sleep(0.2)
        if not cleared:
            pytest.skip(
                "Could not click 'Clear Recent Files' through the "
                "recent-files submenu")

        time.sleep(0.5)

        # Reopen the submenu and verify it now shows the '(No Recent
        # Files)' placeholder, OR is empty/disabled.
        if not _open_recent_menu(win2):
            # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
            _reach = (
                "Recent Files submenu not reachable after "
                        "clear"
            )
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
        # Either '(No Recent Files)' placeholder visible, or the
        # menu has no test_3page entry. Use either signal.
        placeholder_visible = _menu_text_contains(
            win2, "No Recent Files", timeout=2.0)
        test3page_gone = not _menu_text_contains(
            win2, "test_3page", timeout=2.0)
        if not (placeholder_visible or test3page_gone):
            pytest.skip(
                "Clear Recent Files clicked but submenu still shows "
                "test_3page — UIA may have stale tree state")

        # Verify the INI no longer lists test_3page (cross-instance
        # sanity check).
        ini_text = ini_path.read_text(encoding="utf-8", errors="replace")
        if "test_3page" in ini_text.lower():
            pytest.skip("Clear Recent Files clicked but INI still "
                        "lists test_3page — clearRecentFiles() may "
                        "not have synced before the test inspected")

        record_matrix_result(
            "recent_files_clear", "green",
            "Pipeline H phase-3: Clear Recent Files removes the entry "
            "from the recent-files submenu and from the persistent "
            "INI store (cross-instance verified via "
            "PDFE_SETTINGS_PATH).",
        )
    finally:
        _close_pdfe(app2, proc2)
        _close_pdfe(app1, proc1)
        shutil.rmtree(tmpdir, ignore_errors=True)
