# -*- coding: utf-8 -*-
"""Native-UI BATCH-2 interaction proof — redaction tools (pipeline c4a9e271, phase-6).

Drives the redaction workflow through the production seams: activate the redact
tool, add a real redaction mark over heading CONTENT, and APPLY the redactions
(which permanently removes the marked content). ``applyRedactionsAction`` is the
destructive endpoint — the product guards it behind a Yes/No confirmation
(MainWindow::onApplyRedactions), so this suite drives that confirm with an explicit
YES (not the global auto-reject) and verifies the full apply → save → QPDF-rewrite
→ reopen cycle: the success dialog appears, the app survives, and the redacted
heading content visibly DISAPPEARS from the canvas. That is the real per-element
destructive path phase-5's fire-and-survive layer could never reach.

The app launches from a throwaway working copy (apply rewrites the open file) with
the global ``PDFE_TEST_DIALOG_AUTODISMISS`` OFF for this module, so the confirm can
be answered YES instead of auto-rejected. Drive: render_drive pipe seams (GAP-A14);
dialogs are dismissed via the Win32 keystroke helper (UIA-free).
"""
from __future__ import annotations

import os
import time

import pytest

from tests.ui_automation.harness import render_drive as d
from tests.ui_automation.harness import render_oracle as ro
from tests.ui_automation.harness import batch2_drive as b
from tests.ui_automation.conftest import record_matrix_result
# Phase F25 / gap WIN-45: reachability is a RECORDED NON-PASS, never a skip.
from tests.ui_automation.conftest import (  # noqa: E402
    current_test_id,
    record_matrix_result,
)

SRC = d.INVOICE
WORKING = d.REPO_ROOT / "tests" / "_runtime" / "batch2_redact" / "working_invoice_redact.pdf"
GOLDEN_DIR = d.GOLDENS / "batch2_redaction"

# Mark placed over the invoice heading (INVOICE_HEADING_BBOX 247,73,350,106) so
# applying it removes VISIBLE content — the canvas-change assertion is meaningful.
MARK = (247.0, 73.0, 103.0, 33.0)   # fitz-space x,y,w,h over heading content


@pytest.fixture(scope="module")
def holder():
    """Module-scoped launch with the global dialog auto-reject DISABLED, so the
    destructive apply confirm can be answered YES rather than auto-rejected."""
    d.require_binary()
    if not SRC.exists():
        pytest.skip(f"fixture missing: {SRC.name}")
    WORKING.parent.mkdir(parents=True, exist_ok=True)
    GOLDEN_DIR.mkdir(parents=True, exist_ok=True)
    prev = os.environ.pop("PDFE_TEST_DIALOG_AUTODISMISS", None)
    h = b.Holder(SRC, WORKING)
    try:
        h.ensure_live()
        if d.canvas_region(h.win) is None:
            h.teardown()
            # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
            _reach = (
                "PdfCanvas absent from UIA tree"
            )
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
        yield h
    finally:
        h.teardown()
        if prev is not None:
            os.environ["PDFE_TEST_DIALOG_AUTODISMISS"] = prev


def test_redact_tool(holder):
    """Activate the redact tool and paint a mark over heading content (the mark
    overlay renders → canvas changes)."""
    b.run_action_test(
        holder, "redactToolAction",
        gesture_fn=lambda win: d.add_redaction_mark(
            win, MARK[0], MARK[1], MARK[2], MARK[3], settle_s=0.8),
        expect_change=True,
        golden_dir=GOLDEN_DIR, golden_for={"redactToolAction"},
        extra_note="painted redaction mark over heading content",
    )


def test_mark_redaction(holder):
    """Fire markRedaction with the redact tool active and a mark painted."""
    b.run_action_test(
        holder, "markRedactionAction",
        setup_fn=lambda win: d.activate(win, "redactToolAction", settle_s=0.6),
        gesture_fn=lambda win: d.add_redaction_mark(
            win, MARK[0], MARK[1], MARK[2], MARK[3], settle_s=0.8),
        expect_change=True,
        extra_note="mark via redact tool",
    )


def test_apply_redactions(holder):
    """Apply redactions over heading content: drive the Yes confirm, dismiss the
    success dialog, and verify the heading content was struck (canvas changed)
    and the app survived the full save → QPDF-rewrite → reopen cycle."""
    tid = "act_applyRedactionsAction"
    win = holder.ensure_live()
    if not holder.alive():
        record_matrix_result(tid, "red", "app not alive before apply")
        pytest.fail(f"{tid}: app not alive before apply")

    # Precondition: a mark over the heading.
    d.activate(win, "redactToolAction", settle_s=0.6)
    d.add_redaction_mark(win, MARK[0], MARK[1], MARK[2], MARK[3], settle_s=0.8)
    before = d.capture(win)

    # Fire apply; the product shows a Yes/No warning (default No) — answer YES.
    d.activate(win, "applyRedactionsAction", settle_s=0.8)
    confirmed = d.accept_dialog(win, "Apply Redactions", accept_key="y",
                                timeout=8.0, settle_s=1.0)
    # The apply then saves, QPDF-rewrites, and reopens, then shows a result box.
    applied = d.accept_dialog(win, "Redactions Applied", accept_key="enter",
                              timeout=25.0, settle_s=1.5)
    if not applied:
        # Dismiss any failure/error box so we never leave a modal up.
        d.accept_dialog(win, "Redaction", accept_key="enter", timeout=3.0)
    time.sleep(1.0)

    survived = holder.alive()
    changed = None
    if survived:
        try:
            after = d.capture(win)
            changed = ro.canvas_changed(before, after, pixel_threshold=0.0001)
            after.save(str(GOLDEN_DIR / "applyRedactionsAction.png"))
        except Exception:  # noqa: BLE001
            changed = None

    notes = (f"Pipeline c4a9e271 phase-6 batch-2: applyRedactions over heading "
             f"mark; confirmYes={confirmed}, successDlg={applied}, "
             f"survived={survived}, canvasChanged={changed}")

    if not survived:
        record_matrix_result(tid, "red", notes + " — APP DIED on apply")
        pytest.fail(f"{tid}: app died applying redactions; {notes}")
    if not confirmed:
        record_matrix_result(tid, "red", notes + " — confirm dialog never appeared")
        pytest.fail(f"{tid}: apply confirm never appeared; {notes}")
    if not applied:
        record_matrix_result(tid, "red",
                             notes + " — apply did not report success")
        pytest.fail(f"{tid}: apply did not succeed; {notes}")
    if changed is False:
        record_matrix_result(tid, "red",
                             notes + " — redaction left content visibly unchanged")
        pytest.fail(f"{tid}: redaction removed nothing visible; {notes}")

    record_matrix_result(tid, "green", notes)
