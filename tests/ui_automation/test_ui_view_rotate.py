#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Pipeline F phase-6 pywinauto test for View > Rotate > 180.

Covers PENDING row in the matrix:
  view_rotate_180

Phase 6 wires:
  * a new rotateView180Action_ QAction (objectName 'actionRotateView180',
    text 'Rotate View 180°') which calls PdfCanvas::rotateViewCW twice
    to land at 180°,
  * a new "Rotate" submenu under View (objectName 'rotateMenu') that
    groups the existing CW / CCW / Reset items and adds the new 180°
    item.

The test:
  1. Launches pdfe.exe with test_3page.pdf,
  2. Reads the canvas's UIA Description (PdfCanvasAccessible exposes
     the view rotation in the description string — phase-6 addition),
  3. Navigates View > Rotate > 180 by keyboard mnemonic
     (Alt+V, R, 1),
  4. Re-reads the description and confirms the rotation moved from
     0 to 180 degrees.
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
from tests.ui_automation.conftest import record_matrix_result


FIXTURES = pathlib.Path(__file__).resolve().parent.parent / "fixtures"


# Pipeline K phase-2 (finding 3): canvas rotation reader extracted
# to a shared harness helper so test_broader_phase_post.py can use
# the same oracle for view_rotate_cw_90.
from tests.ui_automation.harness.canvas_state import read_canvas_rotation as _read_canvas_rotation  # noqa: E402

# Phase F25 / gap WIN-45: reachability is a RECORDED NON-PASS, never a skip.
from tests.ui_automation.conftest import (  # noqa: E402
    current_test_id,
    record_matrix_result,
)


def test_view_rotate_180():
    """view_rotate_180 — View > Rotate > 180 sets canvas rotation
    to 180°."""
    fixture = FIXTURES / "test_3page.pdf"
    if not fixture.exists():
        pytest.skip(f"fixture missing: {fixture.name}")
    try:
        app, win = launch_pdfe(args=[str(fixture)])
    except RuntimeError as e:
        pytest.skip(f"pdfe.exe launch failed: {e}")
    try:
        time.sleep(3.0)
        before = _read_canvas_rotation(win)
        # If we can't read rotation through accessibility (older
        # pywinauto build), the test still attempts the menu
        # navigation and asserts the app survives.
        try:
            win.set_focus()
        except Exception:
            pass
        time.sleep(0.3)
        # Alt+V opens View menu, R opens Rotate submenu, 1 picks
        # "1&80°" mnemonic — wait, the action title is
        # "Rotate View 180°" with no ampersand so the mnemonic
        # may be auto-derived. Use the navigation-helper's
        # title-match path instead.
        from tests.ui_automation.harness.menu_nav import click_menu_path
        try:
            click_menu_path(win, "View -> Rotate -> Rotate View 180°")
        except Exception:
            # Try alternate display title
            try:
                click_menu_path(win, "View -> Rotate -> Rotate View 180")
            except Exception as e:
                # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
                _reach = (
                    f"View > Rotate > 180 not reachable: {e}"
                )
                record_matrix_result(current_test_id(), "blocked", _reach)
                pytest.fail(
                    f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
        time.sleep(0.5)
        after = _read_canvas_rotation(win)

        # Pipeline K phase-2 (finding 3): the rotation accessor must
        # be readable for this test to claim green. The previous
        # silent-fallback path (assert win.exists()) hid bugs where
        # the menu navigated but the canvas didn't actually rotate.
        if before < 0 or after < 0:
            record_matrix_result(
                "view_rotate_180", "red",
                "PdfCanvasAccessible.Description rotation not readable",
            )
            pytest.fail(
                "rotation accessor not readable; cannot verify rotate-180 "
                f"(before={before}, after={after})"
            )
        assert after == 180, (
            f"After View > Rotate > 180, canvas rotation should be "
            f"180; got {after} (was {before} before)"
        )
        record_matrix_result(
            "view_rotate_180", "green",
            f"Pipeline F phase-6: View > Rotate > 180 changes "
            f"canvas rotation {before} -> {after}",
        )
    finally:
        try:
            app.kill()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
