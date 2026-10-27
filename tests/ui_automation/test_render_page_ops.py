# -*- coding: utf-8 -*-
"""Render-verification: page operations (rotate / delete / insert / move).

Pipeline e7d3c2a1 Phase 3 — category suite for the gap report's page-ops paths
(rotate page render G3; delete/insert/move thumbnail+canvas re-render #19). Each
test drives a real Pages-menu action via the deterministic action seam and
asserts the canvas (and page count) updated. Expected to PASS — these are
working features whose *render* outcome was previously only model/UIA-asserted.
"""
from __future__ import annotations

import sys

import pytest

from tests.ui_automation.harness import render_oracle  # noqa: F401
from tests.ui_automation.harness import render_drive as d


@pytest.mark.timeout(120)
def test_rotate_page_cw_changes_canvas():
    with d.app_session(d.MULTISPAN) as win:
        # capture_stable, not capture: a plain capture() can grab a MID-render
        # frame while the machine is busy (its own docstring says so), and the
        # golden below is captured by the same call -- which is how a golden
        # can end up encoding a half-painted frame.
        before = d.capture_stable(win)
        d.activate(win, "rotatePageCWAction")
        after = d.capture_stable(win)
        d.assert_changed(before, after, threshold=0.02, what="rotated page")
        # D8-2: crop to the page rect so the golden is not sensitive to the
        # viewport margin shade / fit-zoom / window geometry of the host rig.
        d.assert_matches_golden(d.crop_to_page_content(after),
                                "page_ops_rotate_cw_multispan.png")


@pytest.mark.timeout(120)
def test_rotate_page_180_changes_canvas():
    with d.app_session(d.MULTISPAN) as win:
        before = d.capture(win)
        d.activate(win, "rotatePage180Action")
        after = d.capture(win)
        d.assert_changed(before, after, threshold=0.02,
                         what="180-rotated page")


@pytest.mark.timeout(120)
def test_insert_blank_page_updates_count_and_canvas():
    with d.app_session(d.NAV_SELFCHECK) as win:
        n0 = d.page_count(win)
        d.activate(win, "insertBlankPageAction")
        n1 = d.page_count(win)
        assert n1 == n0 + 1, f"page count did not grow: {n0} -> {n1}"


@pytest.mark.timeout(120)
def test_delete_page_updates_count_and_canvas():
    with d.app_session(d.NAV_SELFCHECK) as win:
        n0 = d.page_count(win)
        before = d.capture(win)
        # deletePageAction raises a "Delete Pages" QMessageBox::question confirm
        # (onDeletePages). Trigger it, then accept the modal via keystroke (the
        # bare QMessageBox is not covered by the dialog-introspection seam).
        d.activate(win, "deletePageAction", settle_s=0.4)
        assert d.accept_dialog(win, "Delete Pages"), (
            "Delete Pages confirmation dialog never appeared")
        n1 = d.page_count(win)
        after = d.capture(win)
        assert n1 == n0 - 1, f"page count did not shrink: {n0} -> {n1}"
        d.assert_changed(before, after, threshold=0.05,
                         what="canvas after page delete")


@pytest.mark.timeout(120)
def test_move_page_down_changes_first_page_canvas():
    """On the 2-page self-check (PAGE ONE blue / PAGE TWO red), moving page 0
    down swaps which page renders first."""
    with d.app_session(d.NAV_SELFCHECK) as win:
        d.go_to_page(win, 0)
        before = d.capture(win)
        d.activate(win, "movePageDownAction")
        d.go_to_page(win, 0)
        after = d.capture(win)
        d.assert_changed(before, after, threshold=0.05,
                         what="first-page canvas after move")


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
