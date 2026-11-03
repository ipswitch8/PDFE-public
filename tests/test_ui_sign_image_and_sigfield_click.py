# -*- coding: utf-8 -*-
"""UI tests for ui-fixes-2026-11 phase-3 (GitHub #327 and #328).

#327 — SignDialog gains an image-of-signature picker. This drives the new
       image controls (``imagePathEdit`` / ``imageBrowseButton`` /
       ``imagePreviewLabel``) in the live Sign dialog.

#328 — clicking a placed signature field must open the Sign dialog and sign
       THAT field. This places a signature field via the deterministic
       ``testFieldCreatePipe`` seam, activates the Form tool, clicks the field
       via ``clickByPdfCoord`` (at the field's exact fitz coordinates — never a
       guessed canvas offset), and asserts the Sign dialog modal appears.

Established-way compliance (see C:/src/PDFE/CLAUDE.md "Writing a UI test"):

* The Sign dialog is an app-modal ``QDialog::exec()`` — UIA CANNOT enumerate
  it. We detect it with ``tests/helpers/modal_dialog.wait_for_window`` (raw
  Win32 ``EnumWindows``), and INTERACT with it only by handle via
  ``modal_dialog.uia_dialog(hwnd)`` — never by UIA enumeration.
* Canvas coordinates are NOT guessed. The signature field is placed at a known
  fitz rect via the create pipe, and the click targets that rect's exact
  centre through ``clickByPdfCoord`` (PDF-space), the most deterministic real
  seam — strictly better than a screen-fraction guess.

Run with::

    python -m pytest tests/test_ui_sign_image_and_sigfield_click.py -v
"""
from __future__ import annotations

import os
import sys
import time
from pathlib import Path
from typing import Optional

import pytest

_HERE = Path(__file__).resolve().parent
REPO_ROOT = _HERE.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.helpers.pdfe_session import (  # noqa: E402
    PDFESession,
    PDFE_EXE,
    kill_pdfe_orphans,
    no_orphans,
)
from tests.helpers import modal_dialog  # noqa: E402
from tests.helpers.qinvoke import (  # noqa: E402
    activate_tool_by_name,
    click_by_pdf_coord,
    _write_to_pipe,
)

FIXTURES = REPO_ROOT / "tests" / "fixtures"
FIX_SINGLE = FIXTURES / "test_single.pdf"
FIX_3PAGE = FIXTURES / "test_3page.pdf"


# ---------------------------------------------------------------------------
# Skip conditions (ENVIRONMENTAL only — product/state mismatches FAIL)
# ---------------------------------------------------------------------------

def _has_pdfe_binary() -> bool:
    return Path(PDFE_EXE).exists()


def _is_interactive_session() -> bool:
    if os.environ.get("PDFE_UI_FORCE_HEADLESS") == "1":
        return False
    return bool(os.environ.get("SESSIONNAME"))


def _has_pywinauto() -> bool:
    try:
        import pywinauto  # noqa: F401
        return True
    except Exception:  # noqa: BLE001
        return False


REQUIRES_LIVE_UI = pytest.mark.skipif(
    not (_has_pdfe_binary() and _has_pywinauto() and _is_interactive_session()),
    reason="Live PDFE.exe + interactive Windows desktop session required.",
)


@pytest.fixture(autouse=True)
def _isolate_pdfe_processes():
    """Kill orphan pdfe.exe before and after each test."""
    kill_pdfe_orphans()
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        if no_orphans():
            break
        kill_pdfe_orphans()
        time.sleep(0.3)
    time.sleep(1.0)
    yield
    kill_pdfe_orphans()
    deadline = time.monotonic() + 8.0
    while time.monotonic() < deadline:
        if no_orphans():
            break
        kill_pdfe_orphans()
        time.sleep(0.3)
    assert no_orphans(), "PDFE.exe still running at teardown — orphan leak."


def _require_fixture(p: Path) -> None:
    if not p.exists():
        pytest.skip(f"Fixture missing: {p}")


def _settle(seconds: float = 0.6) -> None:
    time.sleep(seconds)


def _wait_for_dialog_widget(session, auto_id: str, timeout: float = 6.0):
    """Poll until a modal-dialog descendant with ``auto_id`` is reachable.

    SignDialog registers ``registerDialogIntrospection()``, which makes its
    children reachable from the main-window UIA subtree via
    ``session.find_optional`` even while its modal ``exec()`` runs — the
    established way SignDialog is driven (see test_ui_encryption_signatures.py).
    Raw UIA enumeration of the modal top-level itself does NOT work (CLAUDE.md),
    which is why presence of the MODAL is detected with Win32 wait_for_window
    and its CHILDREN are found with this seam-backed lookup.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        w = session.find_optional(auto_id=auto_id, timeout=0.4)
        if w is not None:
            return w
        time.sleep(0.2)
    return None


# ---------------------------------------------------------------------------
# #327 — the Sign dialog carries a drivable image-of-signature picker
# ---------------------------------------------------------------------------

@REQUIRES_LIVE_UI
def test_sign_dialog_has_drivable_image_picker_issue327():
    """Open the Sign dialog (Security > Sign Document) and confirm the new
    image picker is present and drivable: the read-only path edit, the Browse
    button, and a preview label — and that clicking Browse opens the image
    file picker.
    """
    _require_fixture(FIX_SINGLE)
    with PDFESession(pdf_path=str(FIX_SINGLE)) as s:
        assert s.wait_for_window_title(FIX_SINGLE.name, timeout=15)
        _settle(1.0)

        # onSignDocument creates a signature field if none exists, then opens
        # the modal SignDialog. Fire it through the QAction seam (deferred
        # trigger, so the pipe write returns before exec() blocks).
        if not activate_tool_by_name(s, "signDocumentAction"):
            pytest.fail("signDocumentAction not reachable via tool-activate pipe.")

        hwnd = modal_dialog.wait_for_window(s.pid, "Sign Document", timeout=20)
        if hwnd is None:
            pytest.fail(
                "Sign Document modal never appeared (Win32 EnumWindows). "
                "The menu action did not open the dialog.")

        try:
            # Find the new #327 controls via the introspection seam.
            path_edit = _wait_for_dialog_widget(s, "imagePathEdit", timeout=6)
            browse_btn = _wait_for_dialog_widget(s, "imageBrowseButton", timeout=3)
            preview = _wait_for_dialog_widget(s, "imagePreviewLabel", timeout=3)

            assert path_edit is not None, \
                "imagePathEdit missing from Sign dialog — #327 control absent."
            assert browse_btn is not None, \
                "imageBrowseButton missing from Sign dialog — #327 control absent."
            assert preview is not None, \
                "imagePreviewLabel missing from Sign dialog — #327 control absent."

            # Drive the picker: clicking Browse must open the image file dialog.
            try:
                browse_btn.click_input()
            except Exception:  # noqa: BLE001
                try:
                    browse_btn.invoke()
                except Exception:  # noqa: BLE001
                    pass
            picker = modal_dialog.wait_for_window(
                s.pid, "Select Signature Image", timeout=8)
            assert picker is not None, \
                ("Browse button did not open the 'Select Signature Image' "
                 "file dialog — the image picker is not wired.")
            modal_dialog.close_window(picker)
            _settle(0.6)
        finally:
            # Dismiss the Sign dialog so teardown is clean.
            modal_dialog.close_window(hwnd)
            _settle(0.6)


# ---------------------------------------------------------------------------
# #328 — clicking a placed signature field opens the Sign dialog
# ---------------------------------------------------------------------------

@REQUIRES_LIVE_UI
def test_click_signature_field_opens_sign_dialog_issue328():
    """Place a signature field at a known fitz rect, activate the Form tool,
    click the field's exact centre via clickByPdfCoord, and confirm the Sign
    dialog modal opens — proving the canvas click now routes into signing
    (the missing FormTool switch arm that #328 fixes).
    """
    _require_fixture(FIX_SINGLE)
    with PDFESession(pdf_path=str(FIX_SINGLE)) as s:
        assert s.wait_for_window_title(FIX_SINGLE.name, timeout=15)
        _settle(1.0)

        # Place a Signature field at fitz rect (100,100,200,60) on page 0 via
        # the deterministic create seam ("type,page,x,y,w,h").
        if not _write_to_pipe(s, "testFieldCreatePipe", "Signature,0,100,100,200,60"):
            pytest.fail("testFieldCreatePipe not reachable — cannot place field.")
        _settle(1.0)

        # Activate the Form tool so the synthesised click dispatches through
        # FormTool's hit-test pipeline.
        if not activate_tool_by_name(s, "formToolAction"):
            pytest.fail("formToolAction not reachable.")
        _settle(0.5)

        # Click the field centre: fitz (100+100, 100+30) = (200, 130). The
        # create pipe used fitz-space coords, and clickByPdfCoord is fitz-space
        # too, so no Y-flip is needed.
        if not click_by_pdf_coord(s, 0, 200.0, 130.0, settle=0.8):
            pytest.fail("clickByPdfCoord dispatch failed (pipe absent).")

        hwnd = modal_dialog.wait_for_window(s.pid, "Sign Document", timeout=20)
        if hwnd is None:
            pytest.fail(
                "Clicking the placed signature field did not open the Sign "
                "dialog. The canvas click did not reach the signing flow "
                "(GitHub #328 regression).")

        # Clean up: close the modal.
        modal_dialog.close_window(hwnd)
        _settle(0.6)
