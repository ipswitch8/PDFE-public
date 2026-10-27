# -*- coding: utf-8 -*-
"""SEC-UI-101: JavaScript sandbox — hostile JS must not spawn processes.

Pipeline b9e14d07 phase-4.

Opens a PDF fixture containing hostile JavaScript (app.launchURL or
similar), triggers the form action, and asserts:
1. No new processes were spawned.
2. PDFE.exe remains responsive (window still findable).
"""

from __future__ import annotations

import os
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
FIX_DOC_JS = FIXTURES / "test_doc_js.pdf"


def _has_pdfe_binary() -> bool:
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
        import pywinauto  # noqa: F401
        return True
    except Exception:
        return False


def _get_process_names() -> set:
    """Snapshot of running process names (lowercase)."""
    try:
        import psutil
        return {p.name().lower() for p in psutil.process_iter(["name"])}
    except Exception:
        result = subprocess.run(
            ["tasklist", "/FO", "CSV", "/NH"],
            capture_output=True, text=True, encoding="utf-8"
        )
        names = set()
        for line in result.stdout.splitlines():
            parts = line.strip().strip('"').split('","')
            if parts:
                names.add(parts[0].lower())
        return names


pytestmark = [
    pytest.mark.skipif(not _has_pdfe_binary(), reason="PDFE binary not found"),
    pytest.mark.skipif(not _is_interactive(), reason="No interactive session"),
    pytest.mark.skipif(not _has_pywinauto(), reason="pywinauto not installed"),
]


@pytest.fixture(autouse=True)
def _isolate():
    """Kill orphan PDFE processes before and after each test."""
    from tests.helpers.pdfe_session import kill_pdfe_orphans
    kill_pdfe_orphans()
    yield
    kill_pdfe_orphans()


def test_hostile_js_no_process_spawn():
    """SEC-UI-101: Open hostile-JS fixture, trigger JS, verify no new
    processes and PDFE remains responsive."""
    if not FIX_DOC_JS.exists():
        pytest.skip(f"Fixture {FIX_DOC_JS} not found")

    from tests.helpers.pdfe_session import PDFESession

    # Snapshot process names before opening the hostile PDF.
    procs_before = _get_process_names()

    with PDFESession(pdf_path=str(FIX_DOC_JS)) as session:
        session.wait_for_idle(timeout=5)

        # The JS in test_doc_js.pdf has a willSave handler. Trigger it
        # by initiating a save (Ctrl+S). The JS engine should sandbox
        # any hostile API calls.
        from tests.helpers.keys import hotkey_to
        try:
            hotkey_to(session.window, "ctrl", "s")
        except Exception:
            pass  # Save may fail (no path set); we just want to trigger JS.

        time.sleep(2)  # Allow any JS to execute.

        # Snapshot processes after JS execution.
        procs_after = _get_process_names()

        # New processes (excluding system churn like svchost etc.)
        # should not include anything suspicious.
        new_procs = procs_after - procs_before
        suspicious = {"cmd.exe", "powershell.exe", "pwsh.exe",
                      "wscript.exe", "cscript.exe", "mshta.exe",
                      "calc.exe", "notepad.exe"}
        spawned = new_procs & suspicious
        assert not spawned, (
            f"SEC-UI-101: Hostile JS spawned suspicious processes: {spawned}"
        )

        # PDFE must still be responsive — window findable.
        assert session.window.exists(), (
            "SEC-UI-101: PDFE window must remain responsive after hostile JS"
        )


def test_js_sandbox_pdfe_does_not_crash():
    """SEC-UI-101 (supplement): PDFE must not crash when opening a PDF
    with JavaScript. Verify the process is still alive after open."""
    if not FIX_DOC_JS.exists():
        pytest.skip(f"Fixture {FIX_DOC_JS} not found")

    from tests.helpers.pdfe_session import PDFESession

    with PDFESession(pdf_path=str(FIX_DOC_JS)) as session:
        session.wait_for_idle(timeout=5)

        # Process must still be alive.
        assert session.window.exists(), (
            "SEC-UI-101: PDFE must not crash when opening JS-bearing PDF"
        )

        # Window title should contain the filename.
        title = session.window.window_text()
        assert title, "SEC-UI-101: PDFE window must have a title"
