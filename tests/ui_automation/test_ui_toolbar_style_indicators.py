# -*- coding: utf-8 -*-
"""Native-UI proof: toolbar Bold/Italic indicators reflect descriptor style.

Pipeline d4e7f921 phase 2. Phase 1 made TextChar/TextSpan carry
``isItalic``/``isBold`` read from the PDF /FontDescriptor (independent of the
font NAME). This phase wired those fields into
``TextEditTool::emitSelectionFormatting`` (via the pure helper
``resolveToolbarStyle``) so the formatting toolbar's Bold and Italic buttons
light up for the selected span's REAL style — even when the style is encoded
only in the descriptor and the font name is neutral.

This test proves the wiring is live in the Release app: it opens the phase-1
fixture ``embedded_descriptor_style.pdf`` — whose BaseFont name is
style-neutral ("ADRXQZ+Sutpenia Regular") but whose /FontDescriptor forces
Italic + ForceBold (/Flags 0x40062, /ItalicAngle -12) — activates the
text-edit tool, clicks into the span, and asserts the toolbar
``boldButton``/``italicButton`` UIA toggle_state are BOTH checked. Under the
pre-fix behaviour (name-parse at emit time) a neutral name yields unchecked
buttons, so this assertion genuinely discriminates the fix.

Harness rules (CLAUDE.md / GAP-A14): launch + tool-activate + span-click all go
through the ``PDFE_TEST_PIPES`` seams in :mod:`render_drive` (scaling-safe — the
~162% display-scaling mismatch breaks coordinate clicks). Only the shallow
toolbar-button read uses UIA (``find_by_suffix`` + ``get_toggle_state``), the
same proven read the phase-9 toolbar suite uses. ``app_session`` guarantees no
orphaned pdfe.exe. A missing span selection after the click is a hard FAILURE
(SKIP=FAIL), not a skip — it would mask a real wiring break.
"""
from __future__ import annotations

import time

import pytest

from tests.ui_automation.harness import render_drive as d
from tests.ui_automation.harness.dialog_helpers import find_by_suffix
from tests.ui_automation.conftest import record_matrix_result

# The phase-1 descriptor fixture: neutral BaseFont, descriptor-forced Italic+Bold.
FIXTURE = d.FIXTURES / "embedded_descriptor_style.pdf"

# Span landmark in fitz/PDF space (top-left origin, points). The fixture draws a
# single text line; gtest readback reports its bbox as
# QRectF(60, 115.877, 343.299 x 33.495) → centre ~ (231, 133).
SPAN_CLICK_PT = (231.0, 133.0)


def _toggle_state(win, suffix: str):
    """Return the UIA toggle_state (0/1/2) of a toolbar button, or None."""
    btn = find_by_suffix(win, suffix)
    if btn is None:
        return None
    try:
        return btn.get_toggle_state()
    except Exception as e:  # noqa: BLE001
        pytest.fail(f"{suffix}: get_toggle_state failed: {e}")
    return None


def test_toolbar_bold_italic_reflect_descriptor_style():
    """Clicking a descriptor-italic+bold span checks BOTH toolbar buttons."""
    d.require_binary()
    if not FIXTURE.exists():
        pytest.skip(f"fixture missing: {FIXTURE.name} "
                    f"(run tests/fixtures/make_descriptor_style_font.py)")

    with d.app_session(FIXTURE, settle_s=2.5) as win:
        # Reveal the formatting toolbar by activating the text-edit tool.
        d.activate(win, "textEditToolAction", settle_s=1.5)

        # The toolbar Bold/Italic buttons must be reachable post-activation.
        assert find_by_suffix(win, "boldButton") is not None, (
            "boldButton not in the UIA tree after activating the text-edit tool "
            "— the formatting toolbar did not appear")
        assert find_by_suffix(win, "italicButton") is not None, (
            "italicButton not in the UIA tree after activating the text-edit tool")

        # Before any span is selected the buttons default to unchecked.
        pre_bold = _toggle_state(win, "boldButton")
        pre_italic = _toggle_state(win, "italicButton")

        # Click into the span via the scaling-safe PDF-coord pipe, then confirm a
        # span is actually selected (a missed hit-test would make the toolbar
        # read meaningless — fail loudly rather than skip).
        d.click_pdf(win, SPAN_CLICK_PT[0], SPAN_CLICK_PT[1], settle_s=1.2)
        deadline = time.time() + 8.0
        selected = False
        while time.time() < deadline:
            if d.span_selected(d.selection_diag(win)):
                selected = True
                break
            time.sleep(0.3)
        assert selected, (
            f"no text span selected after clicking {SPAN_CLICK_PT} — the span "
            f"hit-test missed; cannot evaluate the toolbar indicators "
            f"(diag: {d.selection_diag(win)!r})")

        # The toolbar must now reflect the span's descriptor-sourced style.
        bold_state = _toggle_state(win, "boldButton")
        italic_state = _toggle_state(win, "italicButton")
        assert bold_state == 1, (
            f"Bold button toggle_state={bold_state} (want 1). The span's "
            f"descriptor ForceBold did not reach the toolbar — "
            f"emitSelectionFormatting/resolveToolbarStyle wiring is broken. "
            f"(pre-click state was {pre_bold})")
        assert italic_state == 1, (
            f"Italic button toggle_state={italic_state} (want 1). The span's "
            f"descriptor Italic did not reach the toolbar — the toolbar is "
            f"still defaulting to unchecked for a neutral-named italic font. "
            f"(pre-click state was {pre_italic})")

        print(f"[toolbar-style] pre(bold={pre_bold},italic={pre_italic}) "
              f"post(bold={bold_state},italic={italic_state})")

        record_matrix_result(
            "txt_toolbar_style_indicators", "green",
            f"descriptor-italic+bold span lights both toolbar buttons: "
            f"bold={bold_state}, italic={italic_state} (>=1)")
