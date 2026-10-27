# -*- coding: utf-8 -*-
"""Native-UI BATCH-2 interaction proof — image-content tools (pipeline c4a9e271, phase-6).

Drives the image-editing workflow through the production seams on an
image-dominated scanned page: activate the image tool, CLICK to select the image
element, then fire each image operation (remove / rotate / crop / resize / strip-
EXIF) against the selection. ``insertImageAction`` is driven with a real image
file pre-seated into the file dialog. The hard gate is that the app SURVIVES the
real per-element interaction (image hit-test + apply-to-selection path) — the
defect class phase-5's fire-and-survive layer could not see. Pixel-level
correctness of the rendered image edits is covered by ``test_render_images.py``.

Core: :mod:`tests.ui_automation.harness.batch2_drive`. Launched from a throwaway
working copy (image ops modify the doc). Drive: render_drive pipe seams (GAP-A14).
"""
from __future__ import annotations

import pytest

from tests.ui_automation.harness import render_drive as d
from tests.ui_automation.harness import batch2_drive as b
from tests.ui_automation.harness.pdfe_app_fixture import inject_file_dialog_path

SRC = d.SCANNED                       # test_scanned.pdf — full-page raster image
WORKING = d.REPO_ROOT / "tests" / "_runtime" / "batch2_img" / "working_scanned_img.pdf"
GOLDEN_DIR = d.GOLDENS / "batch2_images"
INSERT_IMG = d.FIXTURES / "test_image_rgb.png"

IMG_CENTER = (306.0, 396.0)           # letter-page centre — the scanned image

holder = b.make_module_fixture(SRC, WORKING, GOLDEN_DIR)


def _select_image(win):
    d.activate(win, "imageToolAction", settle_s=0.8)
    d.click_pdf(win, IMG_CENTER[0], IMG_CENTER[1], settle_s=0.8)


# (objectName, needs_selected_image, gesture)
def _seed_insert(win):
    # Pre-seat a real PNG so the insert file dialog returns it without a modal.
    inject_file_dialog_path(win, str(INSERT_IMG))


def test_image_tool(holder):
    """Activate the image tool and click the image to select it."""
    b.run_action_test(
        holder, "imageToolAction",
        gesture_fn=lambda win: d.click_pdf(
            win, IMG_CENTER[0], IMG_CENTER[1], settle_s=0.8),
        expect_change=False,
        extra_note="clicked image to select",
    )


def test_insert_image(holder):
    """Insert a real PNG via the pre-seated file dialog."""
    b.run_action_test(
        holder, "insertImageAction",
        setup_fn=_seed_insert,
        gesture_fn=None,
        expect_change=False,
        extra_note=f"pre-seated {INSERT_IMG.name}",
    )


def test_strip_exif(holder):
    b.run_action_test(
        holder, "stripExifAction",
        setup_fn=_select_image, gesture_fn=None, expect_change=False,
        extra_note="strip EXIF on selected image",
    )


def test_image_remove(holder):
    b.run_action_test(
        holder, "imageRemoveAction",
        setup_fn=_select_image, gesture_fn=None, expect_change=False,
        extra_note="remove selected image",
    )


def test_image_rotate(holder):
    b.run_action_test(
        holder, "imageRotateAction",
        setup_fn=_select_image, gesture_fn=None, expect_change=False,
        extra_note="rotate selected image",
    )


def test_image_crop(holder):
    b.run_action_test(
        holder, "imageCropAction",
        setup_fn=_select_image, gesture_fn=None, expect_change=False,
        extra_note="crop selected image",
    )


def test_image_resize(holder):
    b.run_action_test(
        holder, "imageResizeAction",
        setup_fn=_select_image, gesture_fn=None, expect_change=False,
        extra_note="resize selected image",
    )


# --- page-level image-content operations (act_ fire-and-survive contract) -----
# convert-to-grayscale, crop tool, watermark and header/footer are document/page
# operations (dialog-driven or whole-page), not per-image-selection edits. Their
# matrix contract is the act_ floor — fire through the pipe seam, survive — so
# they are driven exactly like the other act_ rows. Dialogs are auto-dismissed by
# PDFE_TEST_DIALOG_AUTODISMISS (set in batch2_drive).
def test_convert_to_grayscale(holder):
    b.run_action_test(
        holder, "convertToGrayscaleAction",
        gesture_fn=None, expect_change=False,
        extra_note="convert page content to grayscale",
    )


def test_crop_tool(holder):
    b.run_action_test(
        holder, "cropToolAction",
        gesture_fn=None, expect_change=False,
        extra_note="activate crop tool",
    )


def test_watermark(holder):
    b.run_action_test(
        holder, "watermarkAction",
        gesture_fn=None, expect_change=False,
        extra_note="watermark dialog (auto-dismissed)",
    )


def test_header_footer(holder):
    b.run_action_test(
        holder, "headerFooterAction",
        gesture_fn=None, expect_change=False,
        extra_note="header/footer dialog (auto-dismissed)",
    )
