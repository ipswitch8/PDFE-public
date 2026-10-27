# -*- coding: utf-8 -*-
"""Native-UI BATCH-2 interaction proof — annotation tools (pipeline c4a9e271, phase-6).

Phase 5 fired the toolbar/menu QActions through ``testToolActivatePipe`` and proved
the window survived (the ``act_*`` fire-and-survive floor). That layer never *drove
the canvas* — so a tool that activates cleanly but crashes or hangs the moment the
user actually draws with it would still show green. That gap is exactly the kind of
hole the per-line-font-size bug slipped through.

This suite closes it for the annotation domain. For each annotation tool it
activates the tool (the act_ floor + a deterministic ``active_tool`` readback) and
then performs the REAL creating gesture through the production input path —
``drag_pdf`` (a genuine OS-level mouse drag, the only thing that commits a shape /
ink / text-markup annotation), ``click_pdf`` placement, or click + ``type_text`` +
``commit`` for free-text. It asserts the MainWindow survives the gesture and, for
the drawing tools whose commit is visibly rendered, that the canvas actually
CHANGED (discriminates "drew something" from "silently swallowed the gesture").

The uniform activate → gesture → survival → record core lives in
:mod:`tests.ui_automation.harness.batch2_drive`; this file is the declarative
annotation table. Drive goes through the :mod:`render_drive` pipe seams (GAP-A14);
the app launches from a throwaway working COPY so no save path mutates a fixture.
"""
from __future__ import annotations

import pytest

from tests.ui_automation.harness import render_drive as d
from tests.ui_automation.harness import batch2_drive as b
from tests.ui_automation.harness.pdfe_app_fixture import inject_file_dialog_path

# ---------------------------------------------------------------------------
# Fixture + landmarks (invoice_vector_heading.pdf has real vector text for the
# text-markup tools to attach to). Coords are fitz/PDF space (top-left origin).
# ---------------------------------------------------------------------------
SRC = d.INVOICE
WORKING = d.REPO_ROOT / "tests" / "_runtime" / "batch2_annot" / "working_invoice_annot.pdf"
ATTACH_SRC = d.FIXTURES / "test_form.pdf"
GOLDEN_DIR = d.GOLDENS / "batch2_annotations"

TEXT_X0, TEXT_X1, TEXT_Y = 250.0, 348.0, 90.0   # heading baseline for markup drags
SHAPE = (150.0, 200.0, 300.0, 300.0)            # blank-area drag box for shapes/ink
PLACE = (170.0, 430.0)                           # blank spot for sticky/stamp
FREETEXT_AT = (160.0, 470.0)

holder = b.make_module_fixture(SRC, WORKING, GOLDEN_DIR)

# (objectName, gesture-tag, expect_canvas_change)
ACTIONS = [
    ("selectToolAction",         "select",     False),
    ("stickyNoteToolAction",     "place",      False),
    ("highlightToolAction",      "drag_text",  True),
    ("underlineToolAction",      "drag_text",  True),
    ("strikeOutToolAction",      "drag_text",  True),
    ("inkToolAction",            "drag_shape", True),
    ("freeTextToolAction",       "freetext",   True),
    ("rectangleToolAction",      "drag_shape", True),
    ("circleToolAction",         "drag_shape", True),
    ("lineToolAction",           "drag_shape", True),
    ("stampToolAction",          "place",      False),
    ("fileAttachmentToolAction", "attach",     False),
]

_GOLDEN_FOR = {"rectangleToolAction", "inkToolAction", "highlightToolAction",
               "freeTextToolAction"}


def _gesture(tag: str):
    def fn(win):
        if tag == "select":
            d.click_pdf(win, TEXT_X0 + 10, TEXT_Y, settle_s=0.6)
        elif tag == "place":
            d.click_pdf(win, PLACE[0], PLACE[1], settle_s=0.8)
        elif tag == "drag_text":
            d.drag_pdf(win, TEXT_X0, TEXT_Y, TEXT_X1, TEXT_Y, settle_s=0.8)
        elif tag == "drag_shape":
            d.drag_pdf(win, SHAPE[0], SHAPE[1], SHAPE[2], SHAPE[3], settle_s=0.8)
        elif tag == "freetext":
            d.click_pdf(win, FREETEXT_AT[0], FREETEXT_AT[1], settle_s=0.8)
            d.type_text(win, "batch2 freetext", settle_s=0.8)
            d.commit(win, settle_s=1.0)
        elif tag == "attach":
            inject_file_dialog_path(win, str(ATTACH_SRC))
            d.click_pdf(win, PLACE[0], PLACE[1] + 40, settle_s=0.8)
        else:
            raise ValueError(f"unknown gesture tag {tag!r}")
    return fn


@pytest.mark.parametrize("action,gesture,expect_change",
                         ACTIONS, ids=[a[0] for a in ACTIONS])
def test_annotation_tool(holder, action, gesture, expect_change):
    """Activate the annotation tool, draw with it, verify the app survives the
    real per-element interaction (canvas change asserted for drawing tools)."""
    b.run_action_test(
        holder, action,
        gesture_fn=_gesture(gesture),
        expect_change=expect_change,
        golden_dir=GOLDEN_DIR,
        golden_for=_GOLDEN_FOR,
        extra_note=f"gesture={gesture}",
    )


# --- annotation-domain menu actions (act_ fire-and-survive contract) ---------
# These two annotation-domain QActions are not creating tools driven by a single
# canvas gesture; they are a view toggle and an image-stamp importer. Their
# matrix contract is the act_ floor ("action fires without crash; window stays
# alive"), so they are driven the same way the 98 other act_ rows are — fire the
# action through the pipe seam and assert survival — not with a bespoke gesture.
def test_toggle_sticky_notes(holder):
    """Toggle sticky-note visibility (view toggle) and verify survival."""
    b.run_action_test(
        holder, "toggleStickyNotesAction",
        gesture_fn=None, expect_change=False,
        extra_note="toggle sticky-note visibility",
    )


def test_import_image_stamp(holder):
    """Import an image as a stamp via the pre-seated file dialog."""
    b.run_action_test(
        holder, "importImageStampAction",
        setup_fn=lambda win: inject_file_dialog_path(
            win, str(d.FIXTURES / "test_image_rgb.png")),
        gesture_fn=None, expect_change=False,
        extra_note="import image stamp from pre-seated path",
    )
