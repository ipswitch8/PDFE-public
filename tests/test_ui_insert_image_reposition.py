# -*- coding: utf-8 -*-
"""GitHub #329 — an inserted image is manipulable WITHOUT a manual click.

THE DEFECT
----------
Inserting an image activated the image tool but left nothing selected, so no
resize/rotate/crop handles were drawn and the image-toolbar actions stayed
disabled until the user separately clicked the image. The fix makes
``onInsertImage()`` auto-select the just-inserted image by its XObject identity
(``ImageTool::selectImage``), so the handles are live immediately.

WHAT THIS TEST PROVES, END TO END
---------------------------------
Driving the REAL insert flow (no direct call to the new selection API — that
would defeat the point, which is that INSERT auto-selects):

  1. push the image path to ``testFileDialogPathPipe`` so the native file
     picker is bypassed (the shell #32770 modal cannot be driven on the CI
     session — see TestDialogSeam.h);
  2. pre-enqueue an ``accept`` action for the modal Insert Image options dialog
     via ``testDialogActionPipe`` — a Qt app-modal ``exec()`` is INVISIBLE to
     UIA (see tests/helpers/modal_dialog.py), so it is driven through its own
     registered seam rather than enumerated;
  3. trigger ``insertImageAction``;
  4. WITHOUT any canvas click, read ``imageSelectionDiag`` and assert the image
     tool reports a live selection — this is the #329 fix;
  5. drive an actual resize by dragging the selection's bottom-right handle via
     the ``testCanvasDragPipe`` seam, and assert the placement actually changed.

WHY testCanvasDragPipe RATHER THAN A SCREEN-PIXEL point_on_page DRAG
--------------------------------------------------------------------
The PdfCanvas render area exposes no UIA tree, so a handle cannot be located or
clicked through UIA. ``tests/helpers/canvas_geometry`` embodies the project
rule "ask the app where the page is, never guess a canvas offset"; we honour it
by asking ``page_rect_on_canvas`` to confirm the page is placed (and FAIL if the
seam does not answer). The resize gesture itself is then driven by
``testCanvasDragPipe`` → ``PdfCanvas::dragByPdfCoord``, which synthesises a real
press/move/release through the SAME event path a mouse drag takes, in the EXACT
image coordinate space the application reports the selection bbox in
(``imageSelectionDiag``). Every coordinate used is therefore reported BY THE APP
— nothing is guessed — which is the strongest form of the canvas_geometry rule
for a drag that must land on a specific image handle rather than a page
fraction.

On-disk persistence of the resized geometry (and undo revert) is asserted by the
GoogleTest ``ImageCommandsTest.ResizeImage_ChangesGeometryAndUndoReverts``; here
we assert the user-observable effect: the selection the handle drag produced is a
larger box than before.
"""

from __future__ import annotations

import os
import sys
import time
from pathlib import Path
from typing import Optional, Tuple

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
FIXTURES_DIR = REPO_ROOT / "tests" / "fixtures"

sys.path.insert(0, str(REPO_ROOT))


def _has_pywinauto() -> bool:
    try:
        import pywinauto  # noqa: F401
        return True
    except Exception:
        return False


def _pdfe_exe() -> Path:
    config = os.environ.get("PDFE_BUILD_CONFIG", "Debug")
    return REPO_ROOT / "build" / "vs2022" / "bin" / config / "pdfe.exe"


def _has_pdfe_binary() -> bool:
    return _pdfe_exe().exists()


def _is_interactive_session() -> bool:
    if os.environ.get("PDFE_UI_FORCE_HEADLESS") == "1":
        return False
    return bool(os.environ.get("SESSIONNAME"))


REQUIRES_LIVE_UI = pytest.mark.skipif(
    not (_has_pywinauto() and _has_pdfe_binary() and _is_interactive_session()),
    reason="Live PDFE.exe + interactive Windows desktop required.",
)

_IMG_SEL_QUERY_PIPE = "testImageSelectionQueryPipe"
_IMG_SEL_RESULT_PIPE = "testImageSelectionResultPipe"


def _read_image_selection(session, settle: float = 0.3) -> Optional[str]:
    """Return the raw imageSelectionDiag string, or None if unreachable."""
    from tests.helpers.qinvoke import _write_to_pipe, _find_pipe  # noqa: PLC0415

    if not _write_to_pipe(session, _IMG_SEL_QUERY_PIPE, "1", settle=settle):
        return None
    el = _find_pipe(session, _IMG_SEL_RESULT_PIPE)
    if el is None:
        return None
    try:
        return (el.get_value() or "").strip()
    except Exception:  # noqa: BLE001
        return None


def _parse_bbox(diag: str) -> Optional[Tuple[str, float, float, float, float]]:
    """Parse "sel=1;xobj=<name>;bbox=x,y,w,h" -> (name, x, y, w, h) or None."""
    if not diag or not diag.startswith("sel=1"):
        return None
    name = ""
    bbox = None
    for part in diag.split(";"):
        if part.startswith("xobj="):
            name = part[len("xobj="):]
        elif part.startswith("bbox="):
            nums = part[len("bbox="):].split(",")
            if len(nums) == 4:
                try:
                    bbox = tuple(float(v) for v in nums)
                except ValueError:
                    return None
    if bbox is None:
        return None
    return (name, bbox[0], bbox[1], bbox[2], bbox[3])


@REQUIRES_LIVE_UI
def test_inserted_image_is_selected_and_resizable_without_a_click():
    from tests.helpers.pdfe_session import PDFESession  # noqa: PLC0415
    from tests.helpers.qinvoke import (  # noqa: PLC0415
        activate_tool_by_name,
        canvas_drag,
        _write_to_pipe,
    )
    from tests.helpers.canvas_geometry import page_rect_on_canvas  # noqa: PLC0415

    pdf_fixture = FIXTURES_DIR / "test_single.pdf"
    img_fixture = FIXTURES_DIR / "test_image_rgb.png"
    if not pdf_fixture.exists() or not img_fixture.exists():
        pytest.skip("fixtures missing")

    with PDFESession(pdf_path=str(pdf_fixture)) as session:
        # Precondition: nothing selected (no image tool / no selection yet).
        pre = _read_image_selection(session)
        assert pre is not None, (
            "imageSelectionDiag seam did not answer — the PDFE build is missing "
            "the #329 readback pipe (needs PDFE_TEST_PIPES and this build)."
        )
        assert pre == "none", f"expected no selection before insert, got {pre!r}"

        # 1) Bypass the native file picker: push the image path.
        assert _write_to_pipe(
            session, "testFileDialogPathPipe", str(img_fixture), settle=0.3
        ), "could not reach testFileDialogPathPipe"

        # 2) Pre-arm the modal Insert Image options dialog to accept its
        #    defaults (a Qt app-modal exec() is invisible to UIA; drive it via
        #    its own registered seam).
        assert _write_to_pipe(
            session, "testDialogActionPipe", "insertImageDialog|accept", settle=0.3
        ), "could not reach testDialogActionPipe"

        # 3) Trigger the real insert. The action trigger is deferred and opens
        #    the (now auto-accepting) modal; give it time to run to completion.
        assert activate_tool_by_name(session, "insertImageAction", settle=0.5), (
            "insertImageAction could not be triggered"
        )
        time.sleep(1.5)

        # 4) THE #329 ASSERTION: the just-inserted image is selected with NO
        #    manual canvas click.
        diag = _read_image_selection(session)
        parsed = _parse_bbox(diag or "")
        assert parsed is not None, (
            "GitHub #329: after the real insert flow the image tool reports "
            f"no live selection (diag={diag!r}); handles would not be drawn and "
            "the image-toolbar actions would stay disabled until a manual click."
        )
        name, x0, y0, w0, h0 = parsed
        assert w0 > 1.0 and h0 > 1.0, f"degenerate selection bbox: {parsed!r}"

        # Confirm the page geometry seam answers (ask, never guess); FAIL if not.
        page_rect = page_rect_on_canvas(session, 0)
        assert page_rect is not None, (
            "page_rect_on_canvas seam did not answer; refusing to guess canvas "
            "coordinates (project rule)."
        )

        # 5) Drive a real resize by dragging the bottom-right handle outward, in
        #    the same image coordinate space the app just reported the bbox in.
        #    (x0+w0, y0+h0) is the BottomRight handle centre; enlarge by ~40 pt.
        delta = 40.0
        from_x, from_y = x0 + w0, y0 + h0
        to_x, to_y = from_x + delta, from_y + delta
        assert canvas_drag(session, 0, from_x, from_y, to_x, to_y, settle=0.8), (
            "testCanvasDragPipe unreachable; could not drive the resize drag"
        )
        time.sleep(0.5)

        # The placement actually changed: the selection is now a larger box.
        after = _read_image_selection(session)
        parsed_after = _parse_bbox(after or "")
        assert parsed_after is not None, (
            f"selection lost after the resize drag (diag={after!r})"
        )
        _, _, _, w1, h1 = parsed_after
        assert w1 > w0 + 10.0, (
            f"width did not grow after the handle drag: {w0} -> {w1}"
        )
        assert h1 > h0 + 10.0, (
            f"height did not grow after the handle drag: {h0} -> {h1}"
        )
    # PDFESession.__exit__ closes the window and kills any orphan pdfe.exe.
