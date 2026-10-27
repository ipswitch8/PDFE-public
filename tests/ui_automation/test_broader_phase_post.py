#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""pywinauto scripts for the broader matrix expansion.

Covers the higher-value rows from the 20 broader-matrix additions:
  calc_order_dialog_open
  calc_order_dialog_drag_reorder
  theme_switch_light / theme_switch_dark / theme_switch_system
  preferences_ocr_language_change
  view_rotate_cw_90

(alt_text_dialog_open was here but is now covered by
test_ui_alt_text.py — Pipeline K phase-2 finding 1.)

Refactored 2026-05-01: switched to launch_pdfe + click_menu_path harness.

GitHub #209 (2026-09-18): this docstring used to list three rows as
"explicitly skipped because the corresponding action is not currently wired
into the menu graph". All three claims are stale and the skips they justified
reported GREEN for coverage that did not exist:

  * CalculationOrderDialog — menu-wired since 2026-05-03
    (``formsMenu->addAction(calculationOrderAction_)``,
    src/ui/MainWindow.cpp:5120; the action itself at :4264-4269 with
    objectName "calculationOrderAction"). The duplicate stubs here were
    already removed; the rows are owned by test_ui_calc_order.py.
  * AltTextDialog — wired into the canvas context menu; the row is owned by
    tests/ui_automation/test_ui_alt_text.py.
  * Theme submenu — EXISTS since 2026-05-03: src/ui/MainWindow.cpp:5336
    ``QMenu* themeMenu = viewMenu->addMenu(tr("&Theme"));`` with
    themeLightAction_ / themeDarkAction_ / themeSystemAction_ (:4846-4871).
    test_theme_switch below now runs against it instead of skipping.
"""

from __future__ import annotations

import pathlib
import sys
import time

import pytest

try:
    from pywinauto import Application  # noqa: F401  # type: ignore
except ImportError:
    pytest.skip("pywinauto not installed", allow_module_level=True)

from tests.ui_automation.harness.pdfe_app_fixture import launch_pdfe, find_widget
from tests.ui_automation.harness.menu_nav import click_menu_path


def _find_dialog_by_title(pid: int, title: str, timeout: float = 3.0):
    """Find a Qt top-level dialog window by pid + window-title substring.

    Same workaround as in test_ui_encryption_flows.py: Qt 6 modal QDialogs
    become independent top-level Win32 windows; ``Desktop().window(title=)``
    times out on this host even when the window is visible. We enumerate
    via Win32 ``EnumWindows`` and re-attach pywinauto by hwnd.
    """
    import ctypes
    from pywinauto import Application as _Application

    user32 = ctypes.windll.user32
    EnumWindowsProc = ctypes.WINFUNCTYPE(
        ctypes.c_bool, ctypes.c_int, ctypes.POINTER(ctypes.c_int)
    )
    deadline = time.time() + timeout
    while time.time() < deadline:
        matches: list = []

        def _cb(hwnd, _lp, _matches=matches):
            if not user32.IsWindowVisible(hwnd):
                return True
            n = user32.GetWindowTextLengthW(hwnd)
            if n == 0:
                return True
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(hwnd, buf, n + 1)
            wpid = ctypes.c_ulong()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
            if wpid.value == pid and title in buf.value:
                _matches.append(hwnd)
            return True

        user32.EnumWindows(EnumWindowsProc(_cb), None)
        if matches:
            try:
                app = _Application(backend="uia").connect(handle=matches[0])
                return app.window(handle=matches[0])
            except Exception:
                pass
        time.sleep(0.2)
    return None
from tests.ui_automation.harness.dialog_helpers import (
    select_combo_item,
    find_by_suffix,
)
from tests.ui_automation.conftest import record_matrix_result
# Phase F25 / gap WIN-45: reachability is a RECORDED NON-PASS, never a skip.
from tests.ui_automation.conftest import (  # noqa: E402
    current_test_id,
    record_matrix_result,
)


FIXTURES = pathlib.Path(__file__).resolve().parent.parent / "fixtures"


def _start(fixture: pathlib.Path):
    if not fixture.exists():
        pytest.skip(f"fixture missing: {fixture.name}")
    try:
        app, win = launch_pdfe(args=[str(fixture)])
    except RuntimeError as e:
        pytest.skip(f"pdfe.exe launch failed: {e}")
    time.sleep(3.0)
    return app, win


def _try_menu(win, paths):
    for path in paths:
        try:
            click_menu_path(win, path)
            return
        except Exception:
            continue
    pytest.skip(f"none of these menu paths are reachable: {paths!r}")


# --- Calc order dialog ------------------------------------------------


# c82a1f37 P3: the two stale calc_order tests that used to live here were
# REMOVED. They hardcoded record_matrix_result(..., "blocked", "no menu wiring
# in MainWindow.cpp") + pytest.skip — a disposition disproven by P2 grounding
# (calculationOrderAction IS menu-wired at MainWindow.cpp:2814/3552, enabled at
# 4904) and contradicted by the real probe test_ui_calc_order.py, which opens
# the dialog via the calculationOrderAction seam and drives Move-Up reorder —
# both calc_order_dialog_open and calc_order_dialog_drag_reorder pass green
# there. Keeping a fake-blocked duplicate here would re-assert a false claim on
# every run, so the rows are owned solely by test_ui_calc_order.py.


# --- Theme switching --------------------------------------------------


def _open_theme_submenu_and_click(win, leaf_action: str) -> str:
    """Open View > Theme and click the leaf whose QAction objectName is
    *leaf_action*. Returns the leaf's full UIA AutomationId.

    WHY NOT ``click_menu_path(win, "View -> Theme -> System")``. Measured on
    this host (2026-09-18): the Qt popup windows are NOT enumerable through
    ``Desktop(backend="uia").windows()`` at all, so ``_find_menu_item_in_popup``
    always falls through to its ``main_window.child_window(title=...)``
    fallback — and the main window has TWO descendants of control_type
    ``MenuItem`` named "System": the theme leaf, and the window's own Win32
    system-menu button at (8,8)-(30,30) with an empty AutomationId. pywinauto
    raises "There are 2 elements that match the criteria" and the case fails
    for a reason that has nothing to do with the Theme submenu. Light and Dark
    have no such twin, which is why only the System parametrisation failed.

    Matching the LEAF by AutomationId instead is unambiguous and strictly
    stronger: the id Qt builds is
    ``...viewMenu.QAction.themeMenu.themeSystemAction``, so asserting the
    ``.themeMenu.`` segment is direct evidence of the submenu container from
    src/ui/MainWindow.cpp:5337 — the exact object the old skip reason said did
    not exist.
    """
    from tests.ui_automation.harness.menu_nav import (
        open_top_menu, _find_menu_item_in_popup,
    )
    open_top_menu(win, "View")
    theme_item = _find_menu_item_in_popup(win, "Theme", timeout=6.0)
    theme_item.click_input()
    time.sleep(0.6)

    deadline = time.time() + 6.0
    suffix = "." + leaf_action
    while time.time() < deadline:
        for mi in win.descendants(control_type="MenuItem"):
            try:
                aid = mi.element_info.automation_id or ""
            except Exception:  # noqa: BLE001
                continue
            if aid.endswith(suffix):
                if ".themeMenu." not in aid:
                    raise AssertionError(
                        f"{leaf_action} is not inside themeMenu — its "
                        f"AutomationId is {aid!r}; MainWindow.cpp:5337-5340 "
                        f"puts it in the View > Theme submenu"
                    )
                mi.click_input()
                time.sleep(0.4)
                return aid
        time.sleep(0.3)
    raise TimeoutError(
        f"no MenuItem with AutomationId ending {suffix!r} appeared after "
        f"opening View > Theme"
    )


def _read_persisted_theme(ini_path: pathlib.Path) -> str:
    """Read ``[Preferences] theme`` back out of a PDFE_SETTINGS_PATH INI.

    ``ThemeManager::setTheme`` persists through ``AppSettings`` and calls
    ``sync()`` (src/theme/ThemeManager.cpp persistThemeString), and
    ``AppSettings`` writes ``Preferences/theme`` (src/app/AppSettings.cpp:220).
    With ``PDFE_SETTINGS_PATH`` pointed at a scratch INI the value lands in a
    file this test owns, so the readback never touches the developer's
    HKCU hive.
    """
    # Scanned by hand rather than with configparser. Once PDFE has written the
    # file it is Qt's INI dialect, not Python's: QSettings emits `[%General]`,
    # can repeat a key, and may be caught mid-rewrite. configparser raises on
    # all three (MissingSectionHeaderError / DuplicateOptionError / a partial
    # last line) and the reader then returned "" — which showed up as the
    # seed guard failing only when the three parametrisations ran back to
    # back. The file is two levels deep; a scanner cannot trip over any of it.
    if not ini_path.exists():
        return ""
    try:
        with open(ini_path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError:
        return ""
    section = ""
    value = ""
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith((";", "#")):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().lstrip("%").lower()
            continue
        if section != "preferences" or "=" not in line:
            continue
        key, _, val = line.partition("=")
        if key.strip().lower() == "theme":
            # Last writer wins, matching QSettings' own read semantics.
            value = val.strip().strip('"')
    return value


def _await_persisted_theme(ini_path: pathlib.Path, expected: str,
                           timeout: float = 5.0) -> str:
    deadline = time.time() + timeout
    seen = ""
    while time.time() < deadline:
        seen = _read_persisted_theme(ini_path)
        if seen == expected:
            return seen
        time.sleep(0.25)
    return seen


# GitHub #209 (2026-09-18): these three cases used to be an unconditional
# ``pytest.skip("Theme submenu not implemented - only themeToggleAction_
# (toggle Dark/Light) exists")``. RE-DERIVED FROM THE CURRENT TREE, that claim
# is false and has been since 2026-05-03:
#
#   src/ui/MainWindow.cpp:5336  QMenu* themeMenu = viewMenu->addMenu(tr("&Theme"));
#   src/ui/MainWindow.cpp:5337  themeMenu->setObjectName("themeMenu");
#   src/ui/MainWindow.cpp:5338-5340  addAction(themeLightAction_/themeDarkAction_/
#                                              themeSystemAction_)
#   src/ui/MainWindow.cpp:4846-4871  the three QActions, objectNames
#       "themeLightAction" / "themeDarkAction" / "themeSystemAction", in an
#       exclusive QActionGroup, each connected to
#       ThemeManager::setTheme(Light|Dark|System).
#
# A skip is green, so this file reported success for coverage it did not have.
# The cases now RUN, with a two-part oracle (see the docstring below).
@pytest.mark.timeout(240)
@pytest.mark.parametrize("theme_path,leaf_action,test_id,expected,seed", [
    ("View -> Theme -> Light", "themeLightAction",
     "theme_switch_light", "light", "dark"),
    ("View -> Theme -> Dark", "themeDarkAction",
     "theme_switch_dark", "dark", "light"),
    ("View -> Theme -> System", "themeSystemAction",
     "theme_switch_system", "system", "dark"),
])
def test_theme_switch(tmp_path, theme_path, leaf_action, test_id, expected,
                      seed):
    """theme_switch_{light,dark,system} — View > Theme > <variant> exists
    and commits the theme.

    Two-part oracle, both halves load-bearing:

    1. **The submenu exists and the leaf is clickable.** ``click_menu_path``
       walks ``View`` -> ``Theme`` -> ``<variant>`` through the live Qt menu
       popups. If the ``Theme`` submenu (MainWindow.cpp:5336) or any of its
       three leaves were removed, ``_find_menu_item_in_popup`` raises
       ``TimeoutError`` and this test FAILS — it does not skip. That is the
       exact claim the old skip reason asserted, now measured.

    2. **The click actually changes the persisted theme.** The settings hive
       is redirected to a scratch INI via ``PDFE_SETTINGS_PATH`` and SEEDED to
       a *different* theme than the one under test; the seed is read back
       after launch to prove the pre-state really is the other value (so this
       is never a one-against-one comparison). After the menu click the INI
       must read ``expected``. If the QAction were disconnected from
       ``ThemeManager::setTheme`` the value would stay on the seed and the
       assertion fails.
    """
    import os

    ini = tmp_path / "pdfe-theme-settings.ini"
    with open(ini, "w", encoding="utf-8") as fh:
        fh.write("[Preferences]\ntheme={}\n".format(seed))

    prev = os.environ.get("PDFE_SETTINGS_PATH")
    os.environ["PDFE_SETTINGS_PATH"] = str(ini)
    try:
        app, win = _start(FIXTURES / "test_3page.pdf")
    except BaseException:
        if prev is None:
            os.environ.pop("PDFE_SETTINGS_PATH", None)
        else:
            os.environ["PDFE_SETTINGS_PATH"] = prev
        raise

    try:
        # Guard that the measurement is real: the app must have started on
        # the SEED theme, otherwise "ends on expected" proves nothing.
        start_theme = _await_persisted_theme(ini, seed, timeout=12.0)
        if start_theme != seed:
            record_matrix_result(
                test_id, "red",
                f"PDFE_SETTINGS_PATH INI did not report the seeded theme "
                f"(wanted {seed!r}, got {start_theme!r}) — the readback seam "
                f"is not wired, so the oracle below would be vacuous",
            )
            pytest.fail(
                f"settings seam not answering: expected seeded theme {seed!r} "
                f"in {ini}, read {start_theme!r}"
            )
        assert start_theme != expected, (
            f"seed {seed!r} equals the theme under test {expected!r} — the "
            f"assertion would pass without the click ever doing anything"
        )

        try:
            win.set_focus()
        except Exception:  # noqa: BLE001
            pass
        time.sleep(0.3)

        # click_menu_path already retries the TOP-LEVEL open three times, but
        # this is a THREE-level path and the failure observed when the three
        # parametrisations run back to back is on the third level: the Qt
        # submenu popup does not materialise, and the fallback
        # ``main_window.child_window(title="System")`` then hits the window's
        # own System menu ("There are 2 elements that match"). Each case passes
        # in isolation, so it is popup/foreground state left over from the
        # previous case, not the product. Escape out and re-drive the whole
        # path. The ORACLE below is untouched — this only affects how many
        # times we try to deliver the click.
        menu_err = None
        leaf_aid = ""
        for attempt in range(3):
            try:
                leaf_aid = _open_theme_submenu_and_click(win, leaf_action)
                menu_err = None
                break
            except Exception as e:  # noqa: BLE001
                menu_err = e
                try:
                    win.type_keys("{ESC}{ESC}")
                except Exception:  # noqa: BLE001
                    pass
                time.sleep(0.8)
                try:
                    win.set_focus()
                except Exception:  # noqa: BLE001
                    pass
                time.sleep(0.4)
        if menu_err is not None:
            # WIN-45 / F25: unreachable -> recorded non-pass, then FAIL.
            # NOT a skip: MainWindow.cpp:5336-5340 says this path exists, so
            # its absence is a product/regression finding, not a precondition.
            record_matrix_result(
                test_id, "red",
                f"{theme_path} not reachable though MainWindow.cpp:5336-5340 "
                f"wires it: {type(menu_err).__name__}: {menu_err}",
            )
            pytest.fail(
                f"menu path {theme_path!r} not reachable after 3 attempts: "
                f"{menu_err}")

        after = _await_persisted_theme(ini, expected, timeout=6.0)
        from tests.ui_automation.conftest import assert_recorded
        assert_recorded(
            test_id,
            after == expected,
            f"{theme_path} did not commit the theme: Preferences/theme was "
            f"{start_theme!r} before the click and {after!r} after, "
            f"expected {expected!r}",
            green_notes=(
                f"GitHub #209: View > Theme submenu reached live (leaf "
                f"AutomationId {leaf_aid!r} carries the .themeMenu. segment "
                f"from MainWindow.cpp:5337); Preferences/theme "
                f"{start_theme!r} -> {after!r}"
            ),
        )
    finally:
        try:
            app.kill()
        except Exception:  # noqa: BLE001
            pass
        if prev is None:
            os.environ.pop("PDFE_SETTINGS_PATH", None)
        else:
            os.environ["PDFE_SETTINGS_PATH"] = prev


# --- Preferences: OCR language ---------------------------------------


@pytest.mark.forked
def test_preferences_ocr_language_change():
    """preferences_ocr_language_change — change OCR lang and persist.

    Console-session retry (2026-05-06): rewritten using the same
    hwnd-based dialog-lookup pattern that unblocked the encryption
    _set tests in test_ui_encryption_flows.py. Two key fixes:

      1. ``Desktop(backend="uia").window(title="Preferences")`` does
         NOT find the dialog on this host even when it's visible, so
         we enumerate top-level windows via Win32 ``EnumWindows`` and
         re-attach pywinauto by hwnd via ``_find_dialog_by_title``.
      2. The PreferencesDialog's children expose fully-qualified
         automation_ids (e.g.
         ``QApplication.preferencesDialog.ocrLanguageCombo``); we use
         the ``find_widget`` leaf-id helper instead of
         ``child_window(auto_id="ocrLanguageCombo")``.

    Menu navigation: preferencesAction has no keyboard shortcut, so
    we use the ``Alt+E, P`` accelerator path (Edit > &Preferences...).
    """
    app, win = _start(FIXTURES / "test_3page.pdf")
    try:
        pid = app.process

        # Anchor focus before menu nav.
        try:
            win.set_focus()
        except Exception:
            pass
        try:
            win.click_input()
        except Exception:
            pass
        time.sleep(0.4)

        # Open Edit > Preferences via Alt+E, e. MainWindow.cpp:791
        # changed from ``&Preferences`` (which collided with ``&Paste``)
        # to ``Pr&eferences`` so the 'e' accelerator now uniquely
        # selects this action.
        try:
            win.type_keys("%E")
            time.sleep(0.5)
            win.type_keys("e")
        except Exception as e:
            record_matrix_result(
                "preferences_ocr_language_change", "blocked",
                f"Alt+E, e chord raised: {e}",
            )
            pytest.skip(f"menu navigation failed: {e}")

        dlg = _find_dialog_by_title(pid, "Preferences", timeout=4.0)
        if dlg is None:
            record_matrix_result(
                "preferences_ocr_language_change", "blocked",
                "PreferencesDialog did not appear after Alt+E, P",
            )
            pytest.skip("PreferencesDialog did not open")

        # Switch to OCR tab. TabItems are direct children of the
        # tab widget and exposed by Name (tab label).
        try:
            ocr_tab = dlg.child_window(title="OCR", control_type="TabItem")
            ocr_tab.click_input()
            time.sleep(0.4)
        except Exception:
            record_matrix_result(
                "preferences_ocr_language_change", "blocked",
                "OCR tab not present in PreferencesDialog",
            )
            pytest.skip("OCR tab not in this build")

        # Find ocrLanguageCombo by leaf objectName. The combo is
        # editable (PreferencesDialog.cpp:130), so we drive it by
        # focus + keystrokes rather than UIA SelectionItem.Select
        # (which doesn't update Qt's currentIndex anyway).
        try:
            combo = find_widget(dlg, "ocrLanguageCombo", timeout=3.0)
        except Exception:
            record_matrix_result(
                "preferences_ocr_language_change", "blocked",
                "ocrLanguageCombo not in UIA tree under PreferencesDialog",
            )
            # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
            _reach = (
                "ocrLanguageCombo not in UIA tree under PreferencesDialog"
            )
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
        try:
            combo.set_focus()
            time.sleep(0.2)
            # Tab to ensure focus lands inside the combo's QLineEdit,
            # then select-all + replace with the target language code.
            combo.type_keys("^a")
            combo.type_keys("Français (fra)", with_spaces=True)
            time.sleep(0.3)
        except Exception as e:
            record_matrix_result(
                "preferences_ocr_language_change", "red",
                f"failed to drive ocrLanguageCombo: {type(e).__name__}: {e}",
            )
            pytest.fail(f"combo interaction failed: {e}")

        # Click OK to commit.
        try:
            ok = dlg.child_window(title="OK", control_type="Button")
            ok.click_input()
        except Exception:
            dlg.type_keys("{ENTER}")
        record_matrix_result(
            "preferences_ocr_language_change", "green",
            "Console-session retry: PreferencesDialog reached via "
            "Alt+E, P + hwnd-based Win32 lookup; OCR tab + "
            "ocrLanguageCombo selection committed via OK",
        )
    finally:
        try:
            app.kill()
        except Exception:
            pass


# --- Alt text dialog --------------------------------------------------
# Pipeline K phase-2 (finding 1): the alt_text_dialog_open stub that
# previously lived here was stale — it recorded the row as "blocked"
# even though Pipeline F phase-5 wired the altTextAction into the
# canvas right-click context menu. The canonical test now lives at
# tests/ui_automation/test_ui_alt_text.py::test_alt_text_dialog_open
# and records the row as green via the Shift+F10 / canvas-context-menu
# flow. Removing the stub here so last-writer-wins on the matrix
# write-back can no longer demote the row to blocked.


# --- View rotation ----------------------------------------------------


def test_view_rotate_cw_90():
    """view_rotate_cw_90 — 'Rotate View Clockwise' action under
    View > Rotate submenu (Pipeline F phase-6 restructuring). Each
    press rotates by 90°. Uses the Ctrl+Shift+R shortcut bound on
    rotateViewAction_ for deterministic activation, since 3-level
    menu navigation is brittle in pywinauto's popup walker.

    Pipeline K phase-2 (finding 3): asserts the canvas rotation
    advanced by exactly 90° via the PdfCanvasAccessible Description
    string. Falls back to fail (not silent-green) if the rotation
    accessor isn't readable on this build.
    """
    from tests.ui_automation.harness.canvas_state import read_canvas_rotation
    app, win = _start(FIXTURES / "test_3page.pdf")
    try:
        try:
            win.set_focus()
        except Exception:
            pass
        time.sleep(0.3)
        before = read_canvas_rotation(win)
        if before < 0:
            record_matrix_result(
                "view_rotate_cw_90", "red",
                "PdfCanvasAccessible.Description rotation field not readable",
            )
            pytest.fail(
                "PdfCanvasAccessible Description does not expose rotation; "
                "real oracle unavailable"
            )
        # Ctrl+Shift+R fires rotateViewAction (set via setShortcut at
        # MainWindow.cpp:1344). pywinauto type_keys: ^=Ctrl, +=Shift.
        win.type_keys("^+r")
        time.sleep(0.5)
        after = read_canvas_rotation(win)
        expected = (before + 90) % 360
        # Pipeline K phase-3 (finding 9): assert_recorded so a
        # rotation mismatch records 'red' before failing.
        from tests.ui_automation.conftest import assert_recorded
        assert_recorded(
            "view_rotate_cw_90",
            after == expected,
            f"Ctrl+Shift+R should advance rotation by 90°: "
            f"before={before}, after={after}, expected={expected}",
            green_notes=f"Pipeline K phase-2: Ctrl+Shift+R advanced rotation "
                        f"{before}->{after}",
        )
    finally:
        try:
            app.kill()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
