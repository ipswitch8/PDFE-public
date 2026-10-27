#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Pipeline F phase-5 pywinauto tests for the View > Theme submenu.

Covers 3 BLOCKED rows in the matrix:
  theme_switch_light
  theme_switch_dark
  theme_switch_system

Each test:
  1. Launches pdfe.exe with test_3page.pdf,
  2. Navigates View -> Theme -> {Light, Dark, System} via menu,
  3. Asserts the choice persisted to QSettings (the registry value
     under HKCU\\Software\\PDFE\\theme).

Reading the persisted theme value is the most robust oracle that
clicking the menu actually triggered the QAction's slot — UIA has
no first-class way to inspect ``QApplication::palette()`` directly,
and re-opening the menu to inspect the MenuItem toggle state is
fragile across Qt menu-popup lifetimes.

We also do a small C++ test (test_theme_manager.cpp) that asserts
``ThemeManager::setTheme(Dark)`` changes ``QApplication::palette()``
to the dark palette; the pywinauto layer then only needs to prove
that menu clicks reach the manager — not re-test the manager itself.
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

from tests.ui_automation.harness.pdfe_app_fixture import launch_pdfe
from tests.ui_automation.harness.menu_nav import click_menu_path
from tests.ui_automation.conftest import record_matrix_result


FIXTURES = pathlib.Path(__file__).resolve().parent.parent / "fixtures"


def _start():
    fixture = FIXTURES / "test_3page.pdf"
    if not fixture.exists():
        pytest.skip(f"fixture missing: {fixture.name}")
    try:
        app, win = launch_pdfe(args=[str(fixture)])
    except RuntimeError as e:
        pytest.skip(f"pdfe.exe launch failed: {e}")
    time.sleep(3.0)
    return app, win


def _read_persisted_theme() -> str:
    """Read HKCU\\Software\\PDFE\\theme via winreg.

    Qt's QSettings on Windows defaults to the registry under
    HKCU\\Software\\<OrganizationName>\\<ApplicationName>. PDFE
    uses 'PDFE' as both. Returns 'light', 'dark', 'system', or ''.
    """
    try:
        import winreg
    except ImportError:
        return ""
    # ThemeManager::setTheme writes via QSettings::setValue("Preferences/theme",
    # ...). Forward slashes in QSettings keys create registry subkeys, so the
    # value lives at HKCU\Software\PDFE\PDFE\Preferences\theme — not directly
    # under HKCU\Software\PDFE\PDFE. (Karen phase-5 review finding 1.)
    candidates = [
        r"Software\\PDFE\\PDFE\\Preferences",
        r"Software\\PDFE\\PDFE",
        r"Software\\PDFE",
    ]
    for sub in candidates:
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, sub) as key:
                try:
                    value, _ = winreg.QueryValueEx(key, "theme")
                    if value:
                        return str(value).strip().lower()
                except FileNotFoundError:
                    continue
                except OSError:
                    continue
        except FileNotFoundError:
            continue
        except OSError:
            continue
    return ""


def _click_theme_and_verify(leaf: str, expected: str,
                             matrix_id: str, label: str) -> None:
    app, win = _start()
    try:
        # Keyboard navigation by mnemonics is far more reliable for
        # 3-level menu paths than the desktop-popup walker. Mnemonics:
        # View=&View (Alt+V), Theme=&Theme (T), Light=&Light (L),
        # Dark=&Dark (D), System=&System (S).
        leaf_mnemonic = {"Light": "l", "Dark": "d", "System": "s"}[leaf]
        try:
            win.set_focus()
        except Exception:
            pass
        time.sleep(0.2)
        win.type_keys("%v")  # Alt+V opens View menu
        time.sleep(0.3)
        win.type_keys("t")   # Theme submenu
        time.sleep(0.3)
        win.type_keys(leaf_mnemonic)  # Light / Dark / System
        # Allow QSettings::sync() to flush. Qt syncs lazily on idle so
        # 0.5s is comfortably enough.
        time.sleep(0.6)
        persisted = _read_persisted_theme()
        # Phase F11 / gap WIN-38: the persisted-theme read is a HARD
        # precondition. It used to fall back to `win.exists()` — a liveness
        # assertion — and record the row GREEN when the settings read failed.
        # That made the test structurally incapable of failing for the defect
        # it targets: a build that opens the Theme menu but never persists the
        # choice recorded green forever. An unreadable settings key is now a
        # recorded non-pass, not a substitute pass. The key path is known and
        # documented in _read_persisted_theme() above.
        # Pipeline K phase-3 (finding 9): assert_recorded so theme
        # mismatches (or window crash) record red before pytest.fail.
        from tests.ui_automation.conftest import assert_recorded, record_matrix_result
        if not persisted:
            record_matrix_result(
                matrix_id, "blocked",
                "persisted theme could not be read after View > Theme > "
                f"{leaf}; the settings read is a precondition, not optional",
            )
            pytest.fail(
                f"WIN-38: {matrix_id}: persisted theme unreadable after "
                f"View > Theme > {leaf} — recorded blocked rather than "
                "substituting a liveness assertion"
            )
        assert_recorded(
            matrix_id,
            persisted == expected,
            f"After clicking View > Theme > {leaf}, persisted theme "
            f"should be {expected!r}; got {persisted!r}",
            green_notes=f"{label} (persisted={persisted})",
        )
    finally:
        try:
            app.kill()
        except Exception:
            pass


def test_theme_switch_light():
    """theme_switch_light — View > Theme > Light persists 'light'."""
    _click_theme_and_verify(
        "Light", "light",
        "theme_switch_light",
        "Pipeline F phase-5: View > Theme > Light reachable",
    )


def test_theme_switch_dark():
    """theme_switch_dark — View > Theme > Dark persists 'dark'."""
    _click_theme_and_verify(
        "Dark", "dark",
        "theme_switch_dark",
        "Pipeline F phase-5: View > Theme > Dark reachable",
    )


def test_theme_switch_system():
    """theme_switch_system — View > Theme > System persists 'system'."""
    _click_theme_and_verify(
        "System", "system",
        "theme_switch_system",
        "Pipeline F phase-5: View > Theme > System reachable",
    )


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
