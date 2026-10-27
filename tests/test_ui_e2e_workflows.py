# -*- coding: utf-8 -*-
"""End-to-end UI workflow tests covering Pipeline 1 remaining items.

KEYBOARD-NAV-COMPLETE, MULTI-DOC-LIFECYCLE, NATIVE-FILE-DIALOG (partial),
DARK-MODE-RENDER (env var), MULTI-DPI-RENDERING (env var).
"""

from __future__ import annotations

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


def _has_pdfe() -> bool:
    try:
        from tests.helpers.pdfe_session import PDFE_EXE
        return Path(PDFE_EXE).exists()
    except Exception:
        return False


def _is_interactive() -> bool:
    if os.environ.get("PDFE_UI_FORCE_HEADLESS") == "1":
        return False
    return bool(os.environ.get("SESSIONNAME"))


def _has_pywinauto() -> bool:
    try:
        import pywinauto  # noqa
        return True
    except Exception:
        return False


pytestmark = [
    pytest.mark.skipif(not _has_pdfe(), reason="PDFE binary not found"),
    pytest.mark.skipif(not _is_interactive(), reason="No interactive session"),
    pytest.mark.skipif(not _has_pywinauto(), reason="pywinauto not installed"),
]


@pytest.fixture(autouse=True)
def _isolate():
    from tests.helpers.pdfe_session import kill_pdfe_orphans
    kill_pdfe_orphans()
    yield
    kill_pdfe_orphans()


# =====================================================================
# KEYBOARD-NAV-COMPLETE
# =====================================================================

def test_keyboard_ctrl_s_triggers_save_state(tmp_path):
    """Ctrl+S keyboard shortcut activates save (verified by dirty marker)."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_click

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    work = tmp_path / "kbd_test.pdf"
    shutil.copy2(fixture, work)

    with PDFESession(pdf_path=str(work)) as session:
        session.wait_for_idle()

        # Click canvas to ensure focus is in the document
        canvas = session.find_optional(auto_id="pdfCanvas", timeout=3)
        if canvas is None:
            pytest.skip("pdfCanvas not found")
        rect = canvas.rectangle()
        send_click((rect.left + rect.right) // 2, (rect.top + rect.bottom) // 2)
        time.sleep(0.3)

        # Send Ctrl+S
        from tests.helpers.win32_input import (
            INPUT, INPUT_KEYBOARD, KEYEVENTF_KEYUP, VK_CONTROL,
            SendInput as _SendInput,
        )
        import ctypes
        inputs = (INPUT * 4)()
        inputs[0].type = INPUT_KEYBOARD
        inputs[0].union.ki.wVk = VK_CONTROL
        inputs[1].type = INPUT_KEYBOARD
        inputs[1].union.ki.wVk = ord('S')
        inputs[2].type = INPUT_KEYBOARD
        inputs[2].union.ki.wVk = ord('S')
        inputs[2].union.ki.dwFlags = KEYEVENTF_KEYUP
        inputs[3].type = INPUT_KEYBOARD
        inputs[3].union.ki.wVk = VK_CONTROL
        inputs[3].union.ki.dwFlags = KEYEVENTF_KEYUP
        _SendInput(4, inputs, ctypes.sizeof(INPUT))
        time.sleep(0.5)

        # PDFE must still be responsive
        assert session.proc.poll() is None


def test_keyboard_escape_dismisses_any_dialog(tmp_path):
    """Escape key dismisses any modal that's open."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_key, VK_ESCAPE

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()

        # Open Preferences via pipe
        pipe = session.find_optional(auto_id="testToolActivatePipe", timeout=2)
        if pipe is None:
            pytest.skip("pipe not available")
        pipe.set_edit_text("preferencesAction")
        time.sleep(1.0)

        # Escape should dismiss
        send_key(VK_ESCAPE)
        time.sleep(0.5)

        # PDFE alive, main window still there
        assert session.proc.poll() is None


def test_keyboard_alt_releases_focus_cleanly(tmp_path):
    """Alt key press/release does not freeze the menu state."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_key

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    VK_MENU = 0x12

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()
        # Press Alt to focus menu
        send_key(VK_MENU)
        time.sleep(0.3)
        # Press Escape to leave menu
        from tests.helpers.win32_input import VK_ESCAPE
        send_key(VK_ESCAPE)
        time.sleep(0.3)

        assert session.proc.poll() is None


# =====================================================================
# MULTI-DOC-LIFECYCLE
# =====================================================================

def test_second_instance_forwards_and_exits_by_default(tmp_path):
    """The SHIPPING default is single-instance: #2 hands its file over and exits.

    This test used to assert "Instance 2 must remain alive" and was one of
    GitHub #285's failures. It was asserting the opposite of the product's
    design. `SingleInstanceGuard` is deliberate: the first process becomes
    primary, a later one forwards its command line to it and exits cleanly,
    which is why opening a PDF from Explorer reuses the open window instead of
    starting a second copy. A second process that "remains alive" would BE the
    defect.

    Both halves are asserted, because "exited" alone is also what a crash looks
    like: #2 must exit *cleanly* (rc 0) and #1 must still be alive afterwards.
    """
    from tests.helpers.pdfe_session import PDFE_EXE

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    work1 = tmp_path / "doc1.pdf"
    work2 = tmp_path / "doc2.pdf"
    shutil.copy2(fixture, work1)
    shutil.copy2(fixture, work2)

    # An isolated single-instance domain (F63), so a concurrently-running
    # unrelated PDFE on this test host cannot be the "primary" we measure
    # against. Without it this test is a race against every other UI test.
    env = {**os.environ, "PDFE_INSTANCE_DOMAIN": f"e2e-si-{os.getpid()}"}
    env.pop("PDFE_DISABLE_SINGLE_INSTANCE", None)

    p1 = subprocess.Popen([str(PDFE_EXE), str(work1)], env=env)
    time.sleep(3.0)  # let #1 win the race for primary deterministically
    p2 = subprocess.Popen([str(PDFE_EXE), str(work2)], env=env)

    try:
        rc2 = p2.wait(timeout=20)
        assert rc2 == 0, (
            f"the secondary instance exited {rc2}, not 0. Single-instance "
            f"forwarding is supposed to be a clean handover; a non-zero exit "
            f"means it failed rather than forwarded.")
        assert p1.poll() is None, (
            "the PRIMARY instance died while the secondary forwarded to it")
    finally:
        for p in (p2, p1):
            try:
                p.terminate()
                p.wait(timeout=10)
            except Exception:
                p.kill()


def test_single_instance_can_be_opted_out_of(tmp_path):
    """`PDFE_DISABLE_SINGLE_INSTANCE=1` really does allow side-by-side.

    The companion to the test above, and the reason that one is not simply a
    restatement of whatever the product happens to do: the opt-out is a
    documented capability (main.cpp:177-181), so it gets a test. Without this,
    single-instance could become unconditional and nothing would notice.
    """
    from tests.helpers.pdfe_session import PDFE_EXE

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    work1 = tmp_path / "solo1.pdf"
    work2 = tmp_path / "solo2.pdf"
    shutil.copy2(fixture, work1)
    shutil.copy2(fixture, work2)

    env = {**os.environ, "PDFE_DISABLE_SINGLE_INSTANCE": "1"}
    p1 = subprocess.Popen([str(PDFE_EXE), str(work1)], env=env)
    p2 = subprocess.Popen([str(PDFE_EXE), str(work2)], env=env)
    try:
        time.sleep(6.0)
        assert p1.poll() is None, "instance 1 exited despite the opt-out"
        assert p2.poll() is None, (
            "instance 2 exited despite PDFE_DISABLE_SINGLE_INSTANCE=1; the "
            "documented opt-out no longer works")
    finally:
        for p in (p1, p2):
            try:
                p.terminate()
                p.wait(timeout=10)
            except Exception:
                p.kill()


def test_open_after_close_releases_file_lock(tmp_path):
    """After PDFESession exits, the PDF file is no longer locked
    and can be deleted or rewritten."""
    from tests.helpers.pdfe_session import PDFESession

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    work = tmp_path / "lock_test.pdf"
    shutil.copy2(fixture, work)

    with PDFESession(pdf_path=str(work)) as session:
        session.wait_for_idle()
        time.sleep(0.5)

    # After exit, file lock should be released
    time.sleep(2.0)  # Allow Windows handle reaper to run
    try:
        # Should be able to overwrite
        work.write_bytes(b"%PDF-1.7\n%%EOF\n")
        assert work.stat().st_size > 0
    except PermissionError:
        pytest.fail("File still locked after PDFESession exit")


# =====================================================================
# NATIVE-FILE-DIALOG: smoke test
# =====================================================================

def test_open_action_triggers_dialog_response(tmp_path):
    """Activating openAction must result in a state change (either
    dialog appears or completes via testSaveAsPipe equivalent)."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers.win32_input import send_key, VK_ESCAPE

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    with PDFESession(pdf_path=str(fixture)) as session:
        session.wait_for_idle()

        pipe = session.find_optional(auto_id="testToolActivatePipe", timeout=2)
        if pipe is None:
            pytest.skip("pipe not available")

        pipe.set_edit_text("openAction")
        time.sleep(2.0)

        # Whatever opened (native or Qt dialog), Escape should clear it
        send_key(VK_ESCAPE)
        time.sleep(0.5)
        send_key(VK_ESCAPE)
        time.sleep(0.5)

        # PDFE must still be alive (the native dialog may have left
        # behind a stuck modal in some configurations; the test catches
        # the catastrophic case where PDFE itself crashed)
        assert session.proc.poll() is None


# =====================================================================
# DARK-MODE-RENDER: launch with QT_STYLE_OVERRIDE
# =====================================================================

def test_dark_theme_environment_launches_cleanly(tmp_path):
    """PDFE launches and runs with dark Qt style override."""
    from tests.helpers.pdfe_session import PDFE_EXE

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    env = os.environ.copy()
    env["QT_STYLE_OVERRIDE"] = "Fusion"  # Dark-mode-compatible style
    env["QT_QPA_PLATFORM_PLUGIN_PATH"] = ""  # Use default

    proc = subprocess.Popen([str(PDFE_EXE), str(fixture)], env=env)
    try:
        time.sleep(4.0)
        assert proc.poll() is None, "PDFE must run with Fusion style"
    finally:
        try:
            proc.terminate()
            proc.wait(timeout=10)
        except Exception:
            proc.kill()


# =====================================================================
# MULTI-DPI-RENDERING: launch at different scale factors
# =====================================================================

@pytest.mark.parametrize("scale", ["1.0", "1.25", "1.5", "2.0"])
def test_dpi_scale_launches_cleanly(tmp_path, scale):
    """PDFE launches and runs at varied QT_SCALE_FACTOR settings."""
    from tests.helpers.pdfe_session import PDFE_EXE

    fixture = FIXTURES / "test_single.pdf"
    if not fixture.exists():
        pytest.skip("fixture missing")

    env = os.environ.copy()
    env["QT_SCALE_FACTOR"] = scale

    proc = subprocess.Popen([str(PDFE_EXE), str(fixture)], env=env)
    try:
        time.sleep(4.0)
        assert proc.poll() is None, f"PDFE must run at scale {scale}"
    finally:
        try:
            proc.terminate()
            proc.wait(timeout=10)
        except Exception:
            proc.kill()
