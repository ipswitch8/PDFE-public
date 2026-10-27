# -*- coding: utf-8 -*-
"""One-shot: launch pdfe.exe and dump every top-level menu's leaves.

Used during phase-2 authoring to discover the actual on-screen menu
labels (Qt may render them differently from the element column in the
matrix CSV). Writes the result as JSON to stdout and to
tests/ui_automation/menu_ground_truth.json.
"""
from __future__ import annotations

import io
import json
import sys
import time
from pathlib import Path

if sys.platform == "win32":
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.ui_automation.harness.pdfe_app_fixture import (  # noqa: E402
    PdfeAppFixture,
    kill_pdfe_processes,
)
from tests.ui_automation.harness.menu_nav import (  # noqa: E402
    list_top_menus,
    list_menu_leaves,
)


def main() -> int:
    kill_pdfe_processes()
    time.sleep(0.2)

    fx = PdfeAppFixture()
    result: dict = {}
    try:
        fx.setUp()
        if fx.main_window is None:
            print("main_window is None", file=sys.stderr)
            return 1
        top = list_top_menus(fx.main_window)
        print(f"top menus: {top}")
        for title in top:
            try:
                leaves = list_menu_leaves(fx.main_window, title)
                result[title] = leaves
                print(f"  {title}: {leaves}")
            except Exception as e:  # noqa: BLE001
                result[title] = {"error": str(e)}
                print(f"  {title}: ERROR {e}")
    finally:
        fx.tearDown()

    out_path = Path(__file__).parent / "menu_ground_truth.json"
    out_path.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(f"\nSaved: {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
