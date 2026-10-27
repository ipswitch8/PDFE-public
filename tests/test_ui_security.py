# -*- coding: utf-8 -*-
"""GAP-25: Security tests verifying test-pipe QLineEdits are absent from the
UIA tree when PDFE_TEST_PIPES is not set (production-simulation mode).

These tests launch PDFE.exe directly — without the harness launcher — so that
PDFE_TEST_PIPES and PDFE_TEST_ANNOT_PIPES are absent from the environment,
exactly as they would be in a real user installation.

Test 1: No test pipe widgets appear in the main-window UIA tree at startup.
Test 2: Open the Customize Toolbars dialog (via View menu UIA navigation),
        then re-verify the dialog subtree also contains no test pipe widgets.
"""

import os
import subprocess
import time
from pathlib import Path

import psutil
import pytest
import pywinauto

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

# Resolve exe relative to this file's location (tests/).
_HERE = Path(__file__).parent
PDFE_EXE = (_HERE.parent / "build" / "vs2022" / "bin" / "Debug" / "pdfe.exe").resolve()

# How long to wait for the main window to appear after launch.
_LAUNCH_TIMEOUT = 30.0

# How long to wait for the Customize Toolbars dialog widgets to appear.
_DIALOG_TIMEOUT = 10.0

# Minimal Qt env vars: DPI stability + suppress the unsaved-prompt modal so
# teardown is clean.  Everything else (PDFE_TEST_PIPES etc.) is intentionally
# absent to simulate production.
_QT_ENV_STABLE = {
    "QT_SCALE_FACTOR": "1",
    "QT_AUTO_SCREEN_SCALE_FACTOR": "0",
    "QT_SCREEN_SCALE_FACTORS": "1",
    "QT_ENABLE_HIGHDPI_SCALING": "0",
    "PDFE_TEST_SKIP_UNSAVED_PROMPT": "1",
    "PDFE_SKIP_ATLAS_DIALOG": "1",
}


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _build_prod_env() -> dict:
    """Return an env dict that simulates a production install.

    Copies os.environ, removes PDFE_TEST_PIPES and PDFE_TEST_ANNOT_PIPES so
    that no test-seam widgets are created, then applies minimal Qt stability
    vars.
    """
    env = os.environ.copy()
    env.pop("PDFE_TEST_PIPES", None)
    env.pop("PDFE_TEST_ANNOT_PIPES", None)
    env.update(_QT_ENV_STABLE)
    return env


def _kill_orphan_pdfe() -> None:
    """Kill any lingering pdfe.exe processes before launching a new one."""
    for proc in psutil.process_iter(["name", "pid"]):
        try:
            if proc.info["name"] and proc.info["name"].lower() == "pdfe.exe":
                proc.kill()
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass
    time.sleep(0.3)


def _wait_for_main_window(pid: int, timeout: float) -> pywinauto.Application:
    """Connect pywinauto to the PDFE process and wait for the main window."""
    app = pywinauto.Application(backend="uia")
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            app.connect(process=pid, timeout=1.0)
            win = app.window(title_re=r".*PDFE.*")
            win.wait("visible", timeout=3)
            return app
        except Exception:  # noqa: BLE001
            time.sleep(0.5)
    raise TimeoutError(f"PDFE main window did not appear within {timeout}s.")


def _collect_leaked_pipes(descendants) -> list:
    """Return descriptive strings for any test-pipe widgets found.

    A widget is flagged when its UIA Name or AutomationId starts with "test"
    (case-sensitive) AND contains "Pipe".  This pattern matches every
    objectName used by PDFE's test-seam QLineEdits:
      testToolActivatePipe, testClickByPdfCoordPipe, testFormField*Pipe,
      testCustomizeAddActionPipe, testCustomizeRemoveActionPipe, etc.
    """
    leaked = []
    for d in descendants:
        try:
            name = (d.element_info.name or "").strip()
            auto_id = (d.element_info.automation_id or "").strip()
        except Exception:  # noqa: BLE001
            continue

        def _is_pipe_id(s: str) -> bool:
            return s.startswith("test") and "Pipe" in s

        if _is_pipe_id(name) or _is_pipe_id(auto_id):
            leaked.append(f"name={name!r} auto_id={auto_id!r}")

    return leaked


def _open_view_menu_and_click_customize(window) -> bool:
    """Expand the View menu and click customizeToolbarsAction via UIA.

    Mirrors the _expand_menu_and_invoke_action pattern from
    test_ui_preferences_chrome.py.  Returns True on success.
    """
    # Phase 1: find the "View" MenuItem among the window's descendants.
    target_menu = None
    deadline = time.monotonic() + 6.0
    while time.monotonic() < deadline:
        try:
            descendants = window.descendants()
        except Exception:  # noqa: BLE001
            descendants = []
        for d in descendants:
            try:
                ct = d.element_info.control_type or ""
                nm = (d.element_info.name or "").strip()
            except Exception:  # noqa: BLE001
                continue
            if ct == "MenuItem" and nm == "View":
                target_menu = d
                break
        if target_menu is not None:
            break
        time.sleep(0.2)

    if target_menu is None:
        return False

    # Phase 2: expand the View menu.
    expanded = False
    for fn in ("expand", "click_input", "invoke", "select"):
        try:
            getattr(target_menu, fn)()
            expanded = True
            break
        except Exception:  # noqa: BLE001
            continue
    if not expanded:
        return False

    time.sleep(0.6)

    # Phase 3: find the "customizeToolbarsAction" menu item by AutomationId.
    target = None
    deadline2 = time.monotonic() + 4.0
    while time.monotonic() < deadline2:
        try:
            descendants = window.descendants()
        except Exception:  # noqa: BLE001
            descendants = []
        for d in descendants:
            try:
                aid = d.element_info.automation_id or ""
            except Exception:  # noqa: BLE001
                continue
            if "customizeToolbarsAction" in aid:
                target = d
                break
        if target is not None:
            break
        time.sleep(0.2)

    if target is None:
        # Close the open menu before giving up.
        try:
            window.type_keys("{ESCAPE}")
        except Exception:  # noqa: BLE001
            pass
        return False

    # Phase 4: invoke the action.
    for fn in ("click_input", "invoke", "select"):
        try:
            getattr(target, fn)()
            time.sleep(0.6)
            return True
        except Exception:  # noqa: BLE001
            continue

    return False


def _wait_for_customize_dialog(app, timeout: float):
    """Wait for CustomizeToolbarsDialog to appear; return a UIA wrapper or None.

    The dialog is a Qt child QDialog so it shows up as a 'Window' element
    inside app.top_window().descendants(), NOT as a separate process window.
    We locate it by auto_id "QApplication.customizeToolbarsDialog" or by
    finding the toolbarCombo child element.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            top = app.top_window()
            # Fast path: find by the known dialog auto_id.
            for d in top.descendants():
                try:
                    aid = d.element_info.automation_id or ""
                    ct = d.element_info.control_type or ""
                    if ct == "Window" and "customizeToolbarsDialog" in aid:
                        return d
                except Exception:  # noqa: BLE001
                    continue
            # Fallback: find by toolbarCombo descendant.
            try:
                combo = top.child_window(auto_id="toolbarCombo", control_type="ComboBox")
                combo.wait("exists", timeout=0.5)
                # Return the parent Window element.
                parent = combo.parent()
                if parent is not None:
                    return parent
                return top  # use top_window as the container
            except Exception:  # noqa: BLE001
                pass
        except Exception:  # noqa: BLE001
            pass
        time.sleep(0.3)
    return None


# ---------------------------------------------------------------------------
# Fixture
# ---------------------------------------------------------------------------

@pytest.fixture()
def pdfe_proc_no_test_pipes():
    """Launch PDFE without PDFE_TEST_PIPES; yield (proc, app); teardown."""
    _kill_orphan_pdfe()

    env = _build_prod_env()
    proc = subprocess.Popen(
        [str(PDFE_EXE)],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    app = None
    try:
        app = _wait_for_main_window(proc.pid, _LAUNCH_TIMEOUT)
        yield proc, app
    finally:
        # Graceful close, then force-kill.
        if app is not None:
            try:
                win = app.window(title_re=r".*PDFE.*")
                win.close()
                proc.wait(timeout=8)
            except Exception:  # noqa: BLE001
                pass
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

@pytest.mark.skipif(not PDFE_EXE.exists(), reason="PDFE binary not built")
@pytest.mark.timeout(60)
def test_no_test_pipes_in_uia_tree_without_env_var(pdfe_proc_no_test_pipes):
    """GAP-25 Test 1: Main-window UIA tree contains no test-pipe widgets.

    Launches PDFE.exe without PDFE_TEST_PIPES or PDFE_TEST_ANNOT_PIPES.
    Enumerates all UIA descendants of the main window and asserts that no
    widget whose name/AutomationId starts with "test" and contains "Pipe"
    is present.
    """
    _proc, app = pdfe_proc_no_test_pipes

    main_window = app.top_window()

    # Allow the window to finish rendering all child widgets.
    time.sleep(1.0)

    try:
        descendants = main_window.descendants()
    except Exception as exc:  # noqa: BLE001
        pytest.fail(f"Could not enumerate UIA descendants: {exc}")

    leaked = _collect_leaked_pipes(descendants)
    assert not leaked, (
        "Test-pipe widgets leaked into UIA tree without PDFE_TEST_PIPES:\n"
        + "\n".join(f"  {entry}" for entry in leaked)
    )


@pytest.mark.skipif(not PDFE_EXE.exists(), reason="PDFE binary not built")
@pytest.mark.timeout(90)
def test_no_test_pipes_in_customize_toolbars_dialog_without_env_var(
    pdfe_proc_no_test_pipes,
):
    """GAP-25 Test 2: CustomizeToolbarsDialog UIA subtree has no test-pipe widgets.

    With PDFE_TEST_PIPES absent: opens the Customize Toolbars dialog via the
    View menu using UIA ExpandCollapse + AutomationId navigation (no pipe
    seam required), then enumerates the dialog's UIA descendants and asserts
    no test-pipe widgets are present.
    """
    _proc, app = pdfe_proc_no_test_pipes

    main_window = app.top_window()
    main_window.set_focus()
    time.sleep(1.5)  # extra settle: menus are sometimes unavailable briefly after focus

    # Open View > Customize Toolbars... via UIA.  Retry once on first failure
    # (cold-start menu race is common on Server 2019).
    opened = _open_view_menu_and_click_customize(main_window)
    if not opened:
        time.sleep(1.5)
        main_window.set_focus()
        time.sleep(0.3)
        opened = _open_view_menu_and_click_customize(main_window)
    if not opened:
        pytest.skip(
            "Could not open Customize Toolbars dialog via UIA menu navigation "
            "(environment may lack a visible PDFE window)."
        )

    # Wait for the dialog to become visible.
    dialog = _wait_for_customize_dialog(app, _DIALOG_TIMEOUT)
    if dialog is None:
        pytest.skip(
            f"Customize Toolbars dialog did not appear within {_DIALOG_TIMEOUT}s "
            "— cannot verify dialog pipe isolation."
        )

    time.sleep(0.5)  # let the dialog finish populating its widget tree

    try:
        dialog_descendants = dialog.descendants()
    except Exception as exc:  # noqa: BLE001
        pytest.fail(f"Could not enumerate dialog UIA descendants: {exc}")

    leaked = _collect_leaked_pipes(dialog_descendants)

    # Close the dialog before asserting to avoid leaving a blocking modal.
    closed = False
    try:
        for d in dialog.descendants():
            try:
                ct = d.element_info.control_type or ""
                nm = (d.element_info.name or "").strip()
                if ct == "Button" and nm in ("Cancel", "OK"):
                    d.click_input()
                    closed = True
                    break
            except Exception:  # noqa: BLE001
                continue
    except Exception:  # noqa: BLE001
        pass
    if not closed:
        try:
            dialog.close()
        except Exception:  # noqa: BLE001
            pass
    time.sleep(0.3)

    assert not leaked, (
        "Test-pipe widgets leaked into CustomizeToolbarsDialog UIA subtree "
        "without PDFE_TEST_PIPES:\n"
        + "\n".join(f"  {entry}" for entry in leaked)
    )
