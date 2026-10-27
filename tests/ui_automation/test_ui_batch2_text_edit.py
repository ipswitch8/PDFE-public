# -*- coding: utf-8 -*-
"""Native-UI BATCH-2 interaction proof — text-edit / TRE tools (pipeline c4a9e271, phase-6).

Drives the text-editing workflow through the production seams on a vector-text
document: activate the text-edit tool, double-click a word to ENTER edit mode with
a live selection, then fire each in-edit sub-action (cut / copy / paste / select-
all / bold / italic / find) and the atlas actions against that active edit. The
hard gate is that the app SURVIVES the real per-element interaction (double-click
hit-test → edit-overlay activation → sub-action on the selection) — the exact code
path the inert per-line-font-size bug lived in, and the defect class phase-5's
fire-and-survive layer could not see. Pixel-level edit rendering is covered by
``test_render_text_edit.py`` and ``test_per_line_font_size_ui.py``.

Core: :mod:`tests.ui_automation.harness.batch2_drive`. Launched from a throwaway
working copy. Drive: render_drive pipe seams (GAP-A14); the first-click atlas modal
is bypassed by PDFE_SKIP_ATLAS_DIALOG (set in render_drive).
"""
from __future__ import annotations

import pytest

from tests.ui_automation.harness import render_drive as d
from tests.ui_automation.harness import batch2_drive as b

SRC = d.INVOICE                       # invoice_vector_heading.pdf — real vector text
WORKING = d.REPO_ROOT / "tests" / "_runtime" / "batch2_text" / "working_invoice_text.pdf"
GOLDEN_DIR = d.GOLDENS / "batch2_text_edit"

WORD_AT = d.INVOICE_HEADING_CENTER    # (298.7, 89.5) — a word in the heading

holder = b.make_module_fixture(SRC, WORKING, GOLDEN_DIR)


def _enter_edit(win):
    """Activate the text-edit tool and double-click a word to open the edit
    overlay with a live selection (the precondition the sub-actions operate on)."""
    d.activate(win, "textEditToolAction", settle_s=1.2)
    d.double_click_pdf(win, WORD_AT[0], WORD_AT[1], settle_s=1.0)


def test_text_edit_tool(holder):
    """Activate the text-edit tool and double-click into a word (enter edit)."""
    b.run_action_test(
        holder, "textEditToolAction",
        gesture_fn=lambda win: d.double_click_pdf(
            win, WORD_AT[0], WORD_AT[1], settle_s=1.0),
        expect_change=False, extra_note="double-click into word",
    )


def test_tre_tool(holder):
    """Activate the TRE tool (scanned-text reconstruction routing)."""
    b.run_action_test(holder, "treToolAction", gesture_fn=None,
                      expect_change=False, extra_note="activate TRE routing")


def test_tre_action(holder):
    """Activate the TRE menu action (treAction) — a distinct objectName from
    treToolAction (the toolbar tool); the Tools>Text-Reconstruction entry point.
    act_ fire-and-survive contract."""
    b.run_action_test(holder, "treAction", gesture_fn=None,
                      expect_change=False, extra_note="activate TRE menu action")


# In-edit sub-actions: each operates on the active edit's selection.
_SUB_ACTIONS = [
    "textEditSelectAllAction",
    "textEditCopyAction",
    "textEditCutAction",
    "textEditPasteAction",
    "textEditBoldAction",
    "textEditItalicAction",
    "textEditFindAction",
    "textEditAtlasScanAction",
    "textEditAtlasFontAction",
    "textEditAtlasCompareAction",
]


@pytest.mark.parametrize("action", _SUB_ACTIONS)
def test_text_edit_sub_action(holder, action):
    """Enter edit mode on a word, then fire the in-edit sub-action against the
    live selection and verify the app survives."""
    b.run_action_test(
        holder, action,
        setup_fn=_enter_edit,
        gesture_fn=None,
        expect_change=False,
        extra_note="fired against active edit selection",
    )
