# -*- coding: utf-8 -*-
"""Render-verification: redaction mark draws / reverts on the canvas.

Pipeline e7d3c2a1 Phase 3 — category suite for the gap report's #11 (redaction
box actually paints on the canvas; previously model/round-trip-asserted only).
Uses the testAddRedactionMarkPipe seam to add a mark over a fitz-space rect and
asserts the mark overlay paints, and that undo removes it. Apply-redactions is
deliberately NOT exercised here (it opens a native save dialog — covered by the
model/round-trip redaction suites); this suite proves the *mark* renders.
"""
from __future__ import annotations

import sys

import pytest

from tests.ui_automation.harness import render_oracle as ro  # noqa: F401
from tests.ui_automation.harness import render_drive as d

# A rect over the vector text band of test_multispan.pdf (fitz x,y,w,h).
MARK_RECT = (80.0, 95.0, 300.0, 40.0)


@pytest.mark.timeout(120)
def test_redaction_mark_paints_on_canvas():
    with d.app_session(d.MULTISPAN) as win:
        d.activate(win, "redactToolAction")
        # capture_stable, not capture: a plain capture() can grab a MID-render
        # frame while the machine is busy (its own docstring says so), and the
        # golden below is captured by the same call -- which is how a golden
        # can end up encoding a half-painted frame.
        before = d.capture_stable(win)
        d.add_redaction_mark(win, *MARK_RECT)
        after = d.capture_stable(win)
        d.assert_changed(before, after, what="redaction mark overlay")
        # D8-2: crop to the page rect so the golden is geometry-stable across rigs.
        d.assert_matches_golden(d.crop_to_page_content(after),
                                "redaction_mark_multispan.png")


@pytest.mark.timeout(120)
def test_redaction_mark_undo_reverts_canvas():
    with d.app_session(d.MULTISPAN) as win:
        d.activate(win, "redactToolAction")
        reference = d.capture(win)
        d.add_redaction_mark(win, *MARK_RECT)
        marked = d.capture(win)
        d.assert_changed(reference, marked, what="redaction mark pre-undo")
        d.undo(win)
        reverted = d.capture(win)
        assert not ro.images_differ(reference, reverted, threshold=0.02), (
            "canvas did not revert after undoing the redaction mark")


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
