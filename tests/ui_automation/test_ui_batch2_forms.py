# -*- coding: utf-8 -*-
"""Native-UI BATCH-2 interaction proof — form tools (pipeline c4a9e271, phase-6).

Drives the forms workflow through the production seams on a real form document
(test_form.pdf): activate the form tool, create each field type by dragging a
field rectangle on the canvas, exercise field-properties / tab-order / calc-order
(dialog-driven, auto-dismissed), and run the form data operations (export / import
/ reset / flatten / submit) with their file dialogs pre-seated. The hard gate is
that the app SURVIVES each real per-element interaction (field hit-test, create-
drag, apply-to-form path) — the defect class phase-5's fire-and-survive layer
could not see. Launched from a throwaway working copy so export/flatten/reset
never mutate a real fixture.

Core: :mod:`tests.ui_automation.harness.batch2_drive`. Drive: render_drive pipe
seams (GAP-A14). File dialogs are pre-seated (no native #32770 modal blocks).
"""
from __future__ import annotations

import pytest

from tests.ui_automation.harness import render_drive as d
from tests.ui_automation.harness import batch2_drive as b
from tests.ui_automation.harness.pdfe_app_fixture import inject_file_dialog_path

SRC = d.FORM                          # test_form.pdf — real AcroForm fields
WORKING = d.REPO_ROOT / "tests" / "_runtime" / "batch2_form" / "working_form.pdf"
_RT = d.REPO_ROOT / "tests" / "_runtime" / "batch2_form"
GOLDEN_DIR = d.GOLDENS / "batch2_forms"

CREATE_BOX = (150.0, 300.0, 270.0, 324.0)   # blank-area field-create drag
EXISTING_FIELD = (150.0, 150.0)             # near an existing field for properties

holder = b.make_module_fixture(SRC, WORKING, GOLDEN_DIR)


def _enter_form_mode(win):
    d.activate(win, "formToolAction", settle_s=0.8)


def _create_drag(win):
    d.drag_pdf(win, CREATE_BOX[0], CREATE_BOX[1], CREATE_BOX[2], CREATE_BOX[3],
               settle_s=0.8)


def _click_field(win):
    d.click_pdf(win, EXISTING_FIELD[0], EXISTING_FIELD[1], settle_s=0.8)


def _seed_save(name):
    def fn(win):
        _RT.mkdir(parents=True, exist_ok=True)
        inject_file_dialog_path(win, str(_RT / name))
    return fn


def _seed_open(path):
    def fn(win):
        inject_file_dialog_path(win, str(path))
    return fn


# --- the form tool + field-creation tools (drag to place a field) -----------
def test_form_tool(holder):
    b.run_action_test(holder, "formToolAction", gesture_fn=_create_drag,
                      expect_change=False, extra_note="form mode + create drag")


_FIELD_TOOLS = [
    "createFormFieldAction", "formTextFieldAction", "formCheckboxAction",
    "formRadioButtonAction", "formComboBoxAction", "formListBoxAction",
    "formPushButtonAction",
]


@pytest.mark.parametrize("action", _FIELD_TOOLS)
def test_form_field_create(holder, action):
    """Select the field type with the form tool active, then drag to create the
    field on the canvas."""
    b.run_action_test(
        holder, action,
        setup_fn=_enter_form_mode,
        gesture_fn=_create_drag,
        expect_change=False,
        extra_note="create-drag of field type",
    )


# --- dialog-driven properties / ordering ------------------------------------
def test_form_field_properties(holder):
    b.run_action_test(
        holder, "formFieldPropertiesAction",
        setup_fn=lambda win: (_enter_form_mode(win), _click_field(win)),
        gesture_fn=None, expect_change=False,
        extra_note="properties dialog (auto-dismissed)",
    )


def test_tab_order(holder):
    b.run_action_test(holder, "tabOrderAction", gesture_fn=None,
                      expect_change=False, extra_note="tab-order dialog")


def test_calculation_order(holder):
    b.run_action_test(holder, "calculationOrderAction", gesture_fn=None,
                      expect_change=False, extra_note="calc-order dialog")


# --- form data operations (file dialogs pre-seated) -------------------------
def test_export_form_data(holder):
    b.run_action_test(
        holder, "exportFormDataAction",
        setup_fn=_seed_save("export_out.fdf"), gesture_fn=None,
        expect_change=False, extra_note="export to pre-seated path",
    )


def test_import_form_data(holder):
    b.run_action_test(
        holder, "importFormDataAction",
        setup_fn=_seed_open(SRC), gesture_fn=None,
        expect_change=False, extra_note="import from pre-seated path",
    )


def test_reset_form(holder):
    b.run_action_test(holder, "resetFormAction", gesture_fn=None,
                      expect_change=False, extra_note="reset field values")


def test_flatten_form(holder):
    b.run_action_test(holder, "flattenFormAction", gesture_fn=None,
                      expect_change=False, extra_note="flatten (confirm auto-dismissed)")


def test_submit_form(holder):
    b.run_action_test(holder, "submitFormAction", gesture_fn=None,
                      expect_change=False, extra_note="submit (dialog auto-dismissed)")


def test_form_signature_field(holder):
    """Create a signature form field: enter form mode, then drag a field
    rectangle on the canvas (mirrors the other field-create tools)."""
    b.run_action_test(
        holder, "formSignatureAction",
        setup_fn=_enter_form_mode,
        gesture_fn=_create_drag,
        expect_change=False,
        extra_note="signature field create-drag",
    )
