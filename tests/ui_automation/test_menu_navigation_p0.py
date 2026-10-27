# -*- coding: utf-8 -*-
"""Phase-2: P0 Menu Navigation tests.

One parameterized test case per P0 Menu row in the matrix CSV. Each
case:

1. Launches a fresh pdfe.exe with the row's fixture preloaded.
2. Clicks the menu path via ``click_menu_path`` (title-based, not
   auto_id, because Qt shares ``QMenuBar.QAction`` auto_ids).
3. Waits briefly for any modal to appear and dismisses it with ESC.
4. Asserts no crash — the pdfe process is still alive and the main
   window is still visible.
5. Tears down with double-kill (``app.kill`` + ``taskkill /F``).

Matrix rows whose ``element`` is mapped to ``None`` in
:mod:`tests.ui_automation.menu_path_map` are recorded as ``blocked``
without launching pdfe — those features don't exist in the current
build, so a test would be vacuous. Blocked rows are still collected
so matrix coverage (55+ P0 menu rows) is satisfied.

Each row's outcome is recorded via ``record_matrix_result`` which
feeds the conftest session-finish hook that writes back to the CSV.

The test docstring indirectly references the harness contract — see
``tests/ui_automation/harness/menu_nav.py`` for menu-click plumbing.
"""
from __future__ import annotations

import csv
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import List, Tuple

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.ui_automation.harness.pdfe_app_fixture import (  # noqa: E402
    PDFE_EXE,
    PdfeAppFixture,
    assert_no_orphan_pdfe,
    kill_pdfe_processes,
)
from tests.ui_automation.harness.menu_nav import (  # noqa: E402
    click_menu_path,
)
from tests.ui_automation.conftest import record_matrix_result  # noqa: E402
from tests.ui_automation.menu_path_map import MENU_MAP  # noqa: E402

MATRIX_CSV = Path(__file__).parent / "matrix" / "ui_test_matrix.csv"
FIXTURES_DIR = REPO_ROOT / "tests" / "fixtures"


def _load_p0_menu_rows() -> List[dict]:
    if not MATRIX_CSV.exists():
        return []
    with MATRIX_CSV.open("r", encoding="utf-8", newline="") as f:
        return [
            row for row in csv.DictReader(f)
            if row.get("priority") == "P0" and row.get("category") == "Menu"
        ]


P0_MENU_ROWS = _load_p0_menu_rows()


def _param_id(row: dict) -> str:
    return row.get("test_id") or row.get("element", "unknown")


def _fixture_args(row: dict, tmpdir: Path) -> List[str]:
    """Return CLI args for launching pdfe with this row's fixture.

    The fixture is copied into ``tmpdir`` first so any destructive
    menu action (File > Save, File > SaveAs, Forms > Reset, redact
    apply, etc.) mutates the copy, not the repo fixture.

    An empty list means launch with no preopened document. The matrix
    column ``fixture`` is an on-disk PDF under ``tests/fixtures/`` or
    a placeholder like ``(various)``.
    """
    fixture = (row.get("fixture") or "").strip()
    if not fixture or fixture.startswith("("):
        return []
    src = FIXTURES_DIR / fixture
    if not src.exists():
        return []
    dst = tmpdir / fixture
    shutil.copy2(src, dst)
    return [str(dst)]


def _dismiss_any_modal(main_window) -> None:
    """Best-effort ESC-dismiss for whatever modal the menu action
    opened. Idempotent; safe to call when no modal is present."""
    try:
        main_window.type_keys("{ESC}")
        time.sleep(0.2)
    except Exception:  # noqa: BLE001
        pass


def _pdfe_still_alive() -> bool:
    """Poll ``tasklist`` for pdfe.exe. Returns True if still running."""
    try:
        result = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq pdfe.exe"],
            capture_output=True,
            text=True,
            timeout=10,
        )
        return "pdfe.exe" in result.stdout
    except (subprocess.TimeoutExpired, FileNotFoundError, OSError):
        return False


# ---------------------------------------------------------------------------
# Parameterized test
# ---------------------------------------------------------------------------

@pytest.mark.skipif(
    not PDFE_EXE.exists(),
    reason=f"pdfe.exe not built at {PDFE_EXE}",
)
@pytest.mark.parametrize(
    "row",
    P0_MENU_ROWS,
    ids=[_param_id(r) for r in P0_MENU_ROWS],
)
def test_p0_menu_item_no_crash(row: dict) -> None:
    """Click a P0 menu item and assert pdfe.exe does not crash.

    The oracle is minimal — "no crash + main window still visible" —
    because P0 menu coverage is about blast radius, not feature
    correctness. Deeper assertions belong to phases 4 (critical flows)
    and later.
    """
    test_id = row["test_id"]
    element = row["element"]
    menu_path = MENU_MAP.get(element)

    # Case 1: element is mapped to None — known-missing feature.
    if menu_path is None:
        record_matrix_result(
            test_id,
            "blocked",
            "menu item not present in current build",
        )
        pytest.skip(f"{element}: not present in current pdfe build")

    path_arrow = " -> ".join(menu_path)

    # Case 2: element is mapped — exercise the menu.
    kill_pdfe_processes()
    time.sleep(0.2)

    # Per-test tmpdir so destructive menu actions (Save, SaveAs,
    # Reset Form, Apply Redactions, ...) don't mutate the repo
    # fixtures. The tmpdir is cleaned up in the finally block.
    tmp_dir = Path(tempfile.mkdtemp(prefix="pdfe_phase2_"))
    fx = PdfeAppFixture()
    fx.launch_args = _fixture_args(row, tmp_dir)

    crashed_reason: str = ""
    try:
        try:
            fx.setUp()
        except Exception as e:  # noqa: BLE001
            crashed_reason = f"launch failed: {e}"
            record_matrix_result(test_id, "red", crashed_reason)
            pytest.fail(crashed_reason)

        assert fx.main_window is not None

        # Exit is a special case: clicking it terminates the process.
        # We verify the action landed by checking tasklist afterwards.
        is_exit = element == "File > Exit"

        try:
            click_menu_path(fx.main_window, path_arrow)
        except Exception as e:  # noqa: BLE001
            crashed_reason = (
                f"click_menu_path({path_arrow!r}) failed: {e}"
            )
            record_matrix_result(test_id, "red", crashed_reason)
            pytest.fail(crashed_reason)

        # Give dialogs time to appear, then ESC them.
        time.sleep(0.4)
        if not is_exit:
            _dismiss_any_modal(fx.main_window)

        # Crash oracle
        if is_exit:
            # Allow a generous window for clean shutdown.
            deadline = time.time() + 5.0
            while time.time() < deadline and _pdfe_still_alive():
                time.sleep(0.2)
            if _pdfe_still_alive():
                crashed_reason = "File > Exit did not terminate pdfe.exe"
                record_matrix_result(test_id, "red", crashed_reason)
                pytest.fail(crashed_reason)
        else:
            if not _pdfe_still_alive():
                crashed_reason = (
                    f"{element}: pdfe.exe died after menu click"
                )
                record_matrix_result(test_id, "red", crashed_reason)
                pytest.fail(crashed_reason)
            # Main window still present?
            try:
                fx.main_window.wait("visible", timeout=2.0)
            except Exception as e:  # noqa: BLE001
                crashed_reason = (
                    f"{element}: main window not visible after click ({e})"
                )
                record_matrix_result(test_id, "red", crashed_reason)
                pytest.fail(crashed_reason)

        record_matrix_result(test_id, "green", "no-crash smoke")

    finally:
        try:
            fx.tearDown()
        except Exception:  # noqa: BLE001
            pass
        # Post-condition: no orphan.
        try:
            assert_no_orphan_pdfe()
        except RuntimeError:
            kill_pdfe_processes()
        # Scrub the per-test tmpdir. Best-effort; a leaked fixture
        # copy is noise, not a test failure.
        try:
            shutil.rmtree(tmp_dir, ignore_errors=True)
        except Exception:  # noqa: BLE001
            pass
