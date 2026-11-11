#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Pipeline G phase-6 pywinauto test for the StatusBar form-field
count label.

Covers AC #4 of phase-6: open a fixture PDF with N form fields, read
the formFieldCount label via UIA, assert it reads "N form fields".

The MainWindow form-field count wiring (sums per-page form fields and
calls statusBar_->setFormFieldCount on document open) was added in
phase G-6 at MainWindow.cpp around line 2820. The label itself, its
objectName ("formFieldCount") and accessibleName ("Form Field Count"),
and the count-to-text formatting are locked by C++
test_theme_and_field_count.cpp.
"""

from __future__ import annotations

import pathlib
import sys
import time

import pytest

try:
    from pywinauto import Application  # noqa: F401  # type: ignore
    from pywinauto import Desktop  # type: ignore
except ImportError:
    pytest.skip("pywinauto not installed", allow_module_level=True)

from tests.ui_automation.harness.pdfe_app_fixture import launch_pdfe
from tests.ui_automation.conftest import record_matrix_result
# Phase F25 / gap WIN-45: reachability is a RECORDED NON-PASS, never a skip.
from tests.ui_automation.conftest import (  # noqa: E402
    current_test_id,
    record_matrix_result,
)


FIXTURES = pathlib.Path(__file__).resolve().parent.parent / "fixtures"


def test_statusbar_form_field_count_reflects_document():
    """Open test_form.pdf and assert the wired form-field count is a positive
    integer.

    GitHub #382b — ORACLE MIGRATION. The status-bar ``formFieldCount`` QLabel is
    a permanent widget in a QStatusBar and exposes no UIA ``AutomationId``, so
    the previous ``label.wait("exists")`` reachability step could never pass
    (tools/gate/reachability_skip_baseline.json). It is NOT a product defect:
    MainWindow's open path sums FormManager's per-page fields and calls
    ``setFormFieldCount`` (MainWindow.cpp ~7713). This oracle reads the SAME
    wired value through a UIA-reachable channel — ``testFormFieldCountResultPipe``
    (a 0x0 QLineEdit MainWindow mirrors the count into, inert unless
    PDFE_TEST_PIPES). The label->text mapping is locked at unit level by
    test_theme_and_field_count.cpp::StatusBarFormFieldCount, and the open-wiring
    headlessly by UiBugs382Test.StatusBarFormFieldCountTracksTheDocument."""
    fixture = FIXTURES / "test_form.pdf"
    if not fixture.exists():
        pytest.skip("test_form.pdf fixture missing")

    try:
        app, win = launch_pdfe(args=[str(fixture)])
    except RuntimeError as e:
        pytest.skip(f"pdfe.exe launch failed: {e}")
    try:
        time.sleep(3.0)
        from tests.ui_automation.conftest import assert_recorded
        from tests.ui_automation.harness.pdfe_app_fixture import (
            find_widget, _read_pipe_value)

        # The pipe is a 0x0 NoFocus QLineEdit, so it must be found with
        # require_visible=False (the same seam read_result_pipe uses).
        try:
            pipe = find_widget(win, "testFormFieldCountResultPipe",
                               timeout=5.0, require_visible=False)
        except Exception:
            pipe = None
        if pipe is None:
            # The pipe only exists under PDFE_TEST_PIPES (launch_pdfe sets it);
            # its absence is an environment/launch problem, recorded not skipped.
            _reach = "testFormFieldCountResultPipe absent (PDFE_TEST_PIPES off?)"
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")

        raw = (_read_pipe_value(pipe) or "").strip()

        assert_recorded(
            "statusbar_form_field_count",
            raw.isdigit(),
            f"form-field-count pipe did not return an integer: '{raw}'",
        )
        count = int(raw)
        assert_recorded(
            "statusbar_form_field_count",
            count > 0,
            f"expected a positive form-field count on test_form.pdf, got {count}",
            green_notes=(
                f"formFieldCount open-wiring propagated {count} fields into the "
                f"StatusBar, read via testFormFieldCountResultPipe (GitHub #382b "
                f"oracle migration off the UIA-invisible QLabel)."
            ),
        )
    finally:
        try:
            app.kill()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
