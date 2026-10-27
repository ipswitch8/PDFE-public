# -*- coding: utf-8 -*-
"""Phase-1 harness smoke test.

The single test required by phase-1's acceptance criteria:
launches pdfe.exe through :class:`PdfeAppFixture`, asserts the main
window becomes visible, walks descendants to verify at least one
known-named widget is reachable, and exits cleanly with no
orphaned ``pdfe.exe`` process. Covers the whole harness stack end
to end in one ~5-second run.

Run directly:

    python tests/ui_automation/harness/smoke_test.py

Or as a pytest case (the file name ``smoke_test.py`` doesn't match
pytest's default ``test_*`` discovery, so pytest will not collect
this automatically — intentional, to keep harness code separate
from the matrix-driven test files under ``tests/ui_automation/``).
"""
from __future__ import annotations

import sys
import time

# When run as a script from anywhere, adjust sys.path so the
# harness package is importable without a pytest invocation.
from pathlib import Path
REPO_ROOT = Path(__file__).resolve().parents[3]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.ui_automation.harness.pdfe_app_fixture import (  # noqa: E402
    PdfeAppFixture,
    assert_no_orphan_pdfe,
    kill_pdfe_processes,
)
from tests.ui_automation.harness.dialog_helpers import (  # noqa: E402
    find_by_suffix,
)
from tests.ui_automation.harness.menu_nav import (  # noqa: E402
    list_top_menus,
)


def smoke_test_harness() -> int:
    """Run the phase-1 smoke test. Returns 0 on success, 1 on failure."""
    errors: list = []

    # Pre-clean any stragglers from earlier runs.
    kill_pdfe_processes()
    time.sleep(0.2)

    print("[smoke] launching pdfe.exe via PdfeAppFixture")
    fx = PdfeAppFixture()
    try:
        fx.setUp()

        # Assert the main window is reachable and has the expected
        # class name. setUp already waited for visible, so this
        # should be instant.
        if fx.main_window is None:
            errors.append("main_window is None after setUp")
        else:
            try:
                cls = fx.main_window.class_name()
                print(f"[smoke] main window class={cls!r}")
                if "MainWindow" not in cls:
                    errors.append(
                        f"main_window class_name {cls!r} "
                        f"does not contain 'MainWindow'")
            except Exception as e:  # noqa: BLE001
                errors.append(f"main_window.class_name() raised: {e}")

        # Assert the canvas widget is reachable via suffix match —
        # this exercises dialog_helpers.find_by_suffix and confirms
        # our hierarchical auto_id handling works.
        if fx.main_window is not None:
            canvas = find_by_suffix(fx.main_window, "pdfCanvas")
            if canvas is None:
                errors.append("pdfCanvas widget not reachable by suffix")
            else:
                print(f"[smoke] pdfCanvas found, auto_id="
                      f"{canvas.element_info.automation_id!r}")

        # Assert menu navigation enumerates all 9 top-level menus.
        # Karen phase-1 review Medium finding: the original
        # assertion only checked that {"File", "Help"} were
        # present, which would pass even if 7 of the 9 menus were
        # missing. Tightened to require the full expected set.
        if fx.main_window is not None:
            try:
                menus = list_top_menus(fx.main_window)
                print(f"[smoke] top menus: {menus}")
                expected_menus = {
                    "File", "Edit", "Pages", "Forms", "Security",
                    "Image", "Tools", "View", "Help",
                }
                missing = expected_menus - set(menus)
                if missing:
                    errors.append(
                        f"top menus missing: {sorted(missing)} "
                        f"(found {sorted(menus)})")
                if len(menus) != 9:
                    errors.append(
                        f"expected exactly 9 top-level menus, "
                        f"got {len(menus)}: {menus}")
            except Exception as e:  # noqa: BLE001
                errors.append(f"list_top_menus raised: {e}")

    finally:
        fx.tearDown()

    # Post-condition: no orphan pdfe.exe
    try:
        assert_no_orphan_pdfe()
        print("[smoke] no orphan pdfe.exe processes")
    except RuntimeError as e:
        errors.append(str(e))

    if errors:
        print("[smoke] FAILED")
        for err in errors:
            print(f"  - {err}")
        return 1
    print("[smoke] PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(smoke_test_harness())
