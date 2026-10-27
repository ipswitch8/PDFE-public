# -*- coding: utf-8 -*-
"""Render-verification: annotation create / modify / undo.

Pipeline e7d3c2a1 Phase 3 — category suite for the gap report's G4 (annotation
tools, ~55 rows). Each test activates a real annotation tool via the
deterministic action seam, drives a click/drag on the canvas, and asserts the
rendered pixels changed (and that undo visibly reverts). Expected to PASS —
annotation create/draw is already a render-asserted path; this layer pins it at
the live-canvas level.
"""
from __future__ import annotations

import sys

import pytest

from tests.ui_automation.harness import render_oracle as ro  # noqa: F401
from tests.ui_automation.harness import render_drive as d


@pytest.mark.timeout(120)
def test_sticky_note_create_changes_canvas():
    with d.app_session(d.MULTISPAN) as win:
        before = d.capture(win)
        d.activate(win, "stickyNoteToolAction")
        d.click_pdf(win, 300, 300)
        after = d.capture(win)
        d.assert_changed(before, after, what="sticky-note canvas")


@pytest.mark.timeout(120)
def test_ink_draw_changes_canvas():
    with d.app_session(d.MULTISPAN) as win:
        before = d.capture(win)
        d.activate(win, "inkToolAction")
        d.drag_pdf(win, 150, 300, 400, 380)
        after = d.capture(win)
        d.assert_changed(before, after, what="ink canvas")


@pytest.mark.timeout(120)
def test_rectangle_shape_changes_canvas():
    with d.app_session(d.MULTISPAN) as win:
        # capture_stable, not capture: a plain capture() can grab a MID-render
        # frame while the machine is busy (its own docstring says so), and the
        # golden below is captured by the same call -- which is how a golden
        # can end up encoding a half-painted frame.
        before = d.capture_stable(win)
        d.activate(win, "rectangleToolAction")
        d.drag_pdf(win, 160, 300, 420, 420)
        after = d.capture_stable(win)
        d.assert_changed(before, after, what="rectangle canvas")
        # D8-2: crop to the page rect so the golden is geometry-stable across rigs.
        d.assert_matches_golden(d.crop_to_page_content(after),
                                "annot_rectangle_multispan.png")


@pytest.mark.timeout(120)
def test_freetext_create_changes_canvas():
    with d.app_session(d.MULTISPAN) as win:
        before = d.capture(win)
        d.activate(win, "freeTextToolAction")
        d.click_pdf(win, 200, 320)
        d.type_text(win, "Note")
        d.commit(win)
        after = d.capture(win)
        d.assert_changed(before, after, what="free-text canvas")


@pytest.mark.timeout(120)
def test_highlight_markup_changes_canvas():
    """Drag the highlight tool across a line of vector text."""
    with d.app_session(d.MULTISPAN) as win:
        before = d.capture(win)
        d.activate(win, "highlightToolAction")
        d.drag_pdf(win, 80, 110, 360, 110)
        after = d.capture(win)
        d.assert_changed(before, after, what="highlight canvas")


@pytest.mark.timeout(120)
def test_annotation_undo_reverts_canvas():
    """Create a shape, then undo — the canvas must revert toward the original."""
    with d.app_session(d.MULTISPAN) as win:
        reference = d.capture(win)
        d.activate(win, "rectangleToolAction")
        d.drag_pdf(win, 160, 300, 420, 420)
        created = d.capture(win)
        d.assert_changed(reference, created, what="shape pre-undo")
        d.undo(win)
        reverted = d.capture(win)
        assert not ro.images_differ(reference, reverted, threshold=0.02), (
            "canvas did not revert to the pre-annotation appearance after undo")


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
