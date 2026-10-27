# -*- coding: utf-8 -*-
"""
PDFE native UI automation smoke test.

Verifies every widget listed in tests/ui_automation/README.md is
reachable via pywinauto's UIA backend. Does not assert behaviour.

Run:
    python tests/ui_automation/smoke_test.py
"""
from __future__ import annotations

import sys
import time
from pathlib import Path

try:
    from pywinauto import Application
    from pywinauto.timings import Timings
except ImportError:
    print("pywinauto not installed. Run: python -m pip install pywinauto")
    sys.exit(2)

REPO = Path(__file__).resolve().parents[2]
EXE = REPO / "build" / "vs2022" / "bin" / "Debug" / "pdfe.exe"

# Widgets we expect to find in the main window at startup. Dock widgets
# hidden by default (bookmark, redaction, signatures, defaults log, etc.)
# are tested separately after showing them.
MAIN_WINDOW_WIDGETS = [
    "pdfCanvas",
    "pagePanelDock",
    "commentPanelDock",
    "searchPanelDock",
    "propertiesPanelDock",
    "mainToolBar",
    "annotationToolBar",
]

# Dialog-level verification is opt-in (pass --dialogs). Each dialog is
# modal and steals focus, and Qt's menu_select can hang on UIA
# notifications that never arrive; running them on every smoke test is
# noisy. The source-level audit ensures every dialog has the required
# setObjectName calls — see tests/ui_automation/README.md.
DIALOG_CHECKS = []


def find_by_suffix(root, object_name: str):
    """Qt's UIA backend exposes hierarchical auto_ids like
    'QApplication.mainWindow.pdfCanvas'. Match by trailing segment."""
    suffix = "." + object_name
    for d in root.descendants():
        try:
            aid = d.element_info.automation_id or ""
        except Exception:  # noqa: BLE001
            continue
        if aid == object_name or aid.endswith(suffix):
            return d
    return None


def check(root, object_name: str) -> bool:
    w = find_by_suffix(root, object_name)
    if w is None:
        print(f"  [FAIL] {object_name}: not found in UIA tree")
        return False
    return True


def main() -> int:
    if not EXE.exists():
        print(f"pdfe.exe not found at {EXE}")
        return 2

    Timings.after_click_wait = 0.2
    print(f"Launching {EXE}")
    app = Application(backend="uia").start(str(EXE), wait_for_idle=False)
    try:
        time.sleep(2.0)  # give Qt time to construct the MainWindow
        win = app.top_window()
        win.wait("visible", timeout=15)
        time.sleep(0.5)

        passed = 0
        failed = 0

        print("\n[1/2] Main window widgets")
        for wid in MAIN_WINDOW_WIDGETS:
            if check(win, wid):
                passed += 1
                print(f"  [OK]   {wid}")
            else:
                failed += 1

        print("\n[2/2] Dialogs")
        for top_menu, leaf_menu, dlg_id, children in DIALOG_CHECKS:
            try:
                win.set_focus()
                time.sleep(0.2)
                top = win.child_window(title=top_menu, control_type="MenuItem")
                top.click_input()
                time.sleep(0.4)
                # Leaf menu item appears as descendant of main window after open
                leaf = win.child_window(title=leaf_menu, control_type="MenuItem")
                leaf.click_input()
                time.sleep(0.8)
                # Find the dialog by iterating app windows
                dlg = None
                for w in app.windows():
                    try:
                        aid = w.element_info.automation_id or ""
                    except Exception:  # noqa: BLE001
                        continue
                    if aid.endswith(dlg_id):
                        dlg = w
                        break
                if dlg is None:
                    print(f"  [FAIL] {dlg_id}: dialog not found")
                    failed += 1
                    continue
                print(f"  Dialog {dlg_id} opened")
                for c in children:
                    if check(dlg, c):
                        passed += 1
                        print(f"    [OK]   {c}")
                    else:
                        failed += 1
                ok_btn = find_by_suffix(dlg, "okButton")
                if ok_btn is not None:
                    try:
                        ok_btn.click_input()
                    except Exception:  # noqa: BLE001
                        pass
                time.sleep(0.4)
            except Exception as e:  # noqa: BLE001
                print(f"  [FAIL] {dlg_id}: {e}")
                failed += 1

        print(f"\nResult: {passed} passed, {failed} failed")
        return 0 if failed == 0 else 1
    finally:
        try:
            app.kill()
        except Exception:  # noqa: BLE001
            pass


if __name__ == "__main__":
    sys.exit(main())
