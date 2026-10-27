# -*- coding: utf-8 -*-
"""Signature subsystem helpers for the PDFE UI test harness.

Provides high-level operations for signing, certifying, and verifying digital
signatures in PDFE.  All functions accept a ``PDFESession`` instance (from
``tests.ui_harness.core.launch``) as their first argument and delegate
low-level widget location, canvas interaction, and screenshot capture to the
shared ``core`` helpers.

Discovered objectNames (from src/ui/MainWindow.cpp / SignDialog.cpp /
SignaturePanel.cpp)
------------------------------------------------------------------------
MainWindow QAction objectNames:
- signDocumentAction       — QAction "Sign Document..."
                             menu: Security->&Sign Document...
                             shortcut: Ctrl+Shift+S
- verifySignaturesAction   — QAction "Verify Signatures"
                             menu: Security->&Verify Signatures
                             shortcut: Ctrl+Shift+V

MainWindow dock widget objectNames:
- signaturePanelDock       — QDockWidget, accessible name "Signatures Panel"
- signaturePanel           — QWidget (SignaturePanel content), accessible name
                             "Signature Panel"

SignaturePanel widget objectNames (from SignaturePanel.cpp):
- signatureTreeWidget      — QTreeWidget, accessible name "Signatures List"
                             columns: [0] Field Name, [1] Signer, [2] Page,
                             [3] Status  (text: "Signed" | "Unsigned")
- verifyAllButton          — QPushButton "Verify All", accessible name
                             "Verify All Signatures"
- signSelectedButton       — QPushButton "Sign Selected"
- viewDetailsButton        — QPushButton "View Details"
- signatureSummaryLabel    — QLabel showing "N of M signed"

SignDialog widget objectNames (from SignDialog.cpp):
- signDialog               — QDialog objectName
- pfxPathEdit              — QLineEdit, PFX certificate file path
- browseButton             — QPushButton "Browse..."
- pfxPasswordEdit          — QLineEdit, certificate password (echo=Password)
- reasonEdit               — QLineEdit, reason metadata
- locationEdit             — QLineEdit, location metadata
- invisibleCheck           — QCheckBox "Invisible signature"
                             accessible name "Invisible signature"
- certificationCombo       — QComboBox, items:
                               index 0 = "Regular signature (no certification)"
                               index 1 = "Certify — no changes allowed"
                               index 2 = "Certify — form fill-in + signing"
                               index 3 = "Certify — annotations + form + signing"
                             accessible name "Certification level"
- tsaCheckBox              — QCheckBox "Include TSA Timestamp"
- tsaUrlEdit               — QLineEdit, TSA URL
- okButton                 — QPushButton "Sign" (default, accept)
- cancelButton             — QPushButton "Cancel"

Sign flow (from MainWindow::onSignDocument):
- If no unsigned signature field exists, PDFE creates one automatically.
- SignDialog opens for user input.
- A QFileDialog ("Save Signed PDF") prompts for the output path.
- Signing occurs; the document is reopened; signaturePanelDock is shown.

Verify flow (from MainWindow::onVerifySignatures):
- verifyAllSignatures() is called; signaturePanel is updated and shown.
- A QMessageBox::information dialog appears summarising the result.

Menu paths (fallback):
- "Security->&Sign Document..."
- "Security->&Verify Signatures"
"""

import time

import pyautogui

from tests.ui_harness.core.canvas import wait_stable
from tests.ui_harness.core.locate import (
    by_auto_id_or_name,
    by_name,
    invoke,
)
from tests.ui_harness.core.screenshot import capture_canvas

__all__ = [
    "sign_document",
    "validate_signature",
    "certify_document",
]

# ---------------------------------------------------------------------------
# Internal constants — objectNames / accessible names from source files
# ---------------------------------------------------------------------------

# MainWindow QAction objectNames.
_SIGN_DOCUMENT_ACTION_ID = "signDocumentAction"
_SIGN_DOCUMENT_ACTION_NAME = "Sign Document..."

_VERIFY_SIGNATURES_ACTION_ID = "verifySignaturesAction"
_VERIFY_SIGNATURES_ACTION_NAME = "Verify Signatures"

# SignaturePanel dock.
_SIGNATURE_PANEL_DOCK_ID = "signaturePanelDock"
_SIGNATURE_PANEL_DOCK_NAME = "Signatures Panel"

# SignaturePanel widgets.
_SIGNATURE_TREE_ID = "signatureTreeWidget"
_SIGNATURE_TREE_NAME = "Signatures List"
_VERIFY_ALL_BUTTON_ID = "verifyAllButton"
_VERIFY_ALL_BUTTON_NAME = "Verify All Signatures"

# SignDialog window title (exact string from setWindowTitle).
_SIGN_DIALOG_TITLE = "Sign Document"

# SignDialog widget objectNames.
_PFX_PATH_EDIT_ID = "pfxPathEdit"
_PFX_PASSWORD_EDIT_ID = "pfxPasswordEdit"
_REASON_EDIT_ID = "reasonEdit"
_LOCATION_EDIT_ID = "locationEdit"
_INVISIBLE_CHECK_ID = "invisibleCheckBox"
_INVISIBLE_CHECK_NAME = "Invisible signature"
_CERTIFICATION_COMBO_ID = "certificationLevelCombo"
_CERTIFICATION_COMBO_NAME = "Certification level"
_SIGN_OK_BUTTON_ID = "okButton"
_CANCEL_BUTTON_ID = "cancelButton"

# QFileDialog title for save path after signing.
_SAVE_SIGNED_DIALOG_TITLE_RE = r"Save Signed PDF"

# Verification result QMessageBox titles.
_VERIFY_RESULT_TITLE_RE = r"Signature Verification|Verify Signatures"

# Certification combo item map: perms keyword -> combo index.
# Source: SignDialog.cpp certificationCombo_ addItem calls.
_CERTIFICATION_COMBO_MAP = {
    "NoChanges": 1,           # "Certify — no changes allowed"
    "FormFillOnly": 2,        # "Certify — form fill-in + signing"
    "AnnotationsAllowed": 3,  # "Certify — annotations + form + signing"
}

# Menu fallback paths (from createMenus in MainWindow.cpp).
_MENU_SIGN_DOCUMENT = "Security->&Sign Document..."
_MENU_VERIFY_SIGNATURES = "Security->&Verify Signatures"

# Timeouts.
_DIALOG_TIMEOUT = 20
_PANEL_TIMEOUT = 10
_ACTION_SETTLE = 0.5
_SIGN_SETTLE = 2.0


# ---------------------------------------------------------------------------
# Public helpers
# ---------------------------------------------------------------------------

def sign_document(
    session,
    cert_path,
    password,
    output_path=None,
    location=None,
    reason=None,
    contact=None,
):
    """Sign the open document using *cert_path* (.pfx) and *password*.

    Invokes ``signDocumentAction`` which opens ``SignDialog``.  Fills the PFX
    path, password, and optional metadata fields (location, reason), then
    clicks Sign.  A QFileDialog then prompts for the save path; this helper
    types *output_path* and accepts (or accepts the current-file default when
    *output_path* is None).  After signing, PDFE reopens the document and
    shows the signature panel; the helper waits for that panel to appear.

    Parameters
    ----------
    session     : PDFESession
    cert_path   : str         Absolute path to the .pfx / .p12 certificate.
    password    : str         Certificate password.
    output_path : str | None  Path for the signed PDF.  None accepts the
                              QFileDialog default (current file path).
    location    : str | None  Optional location metadata (locationEdit).
    reason      : str | None  Optional reason metadata (reasonEdit).
    contact     : str | None  Ignored — PDFE's SignDialog has no contact
                              field; kept for API symmetry.

    Raises
    ------
    RuntimeError
        If the Sign Document action cannot be invoked, or SignDialog does not
        appear within ``_DIALOG_TIMEOUT`` seconds.
    """
    _invoke_sign_action(session)

    dlg = _wait_for_dialog(session, title_re=_SIGN_DIALOG_TITLE, timeout=_DIALOG_TIMEOUT)

    # Fill PFX path — this also triggers validateInput() which enables okButton.
    _fill_edit(dlg, _PFX_PATH_EDIT_ID, cert_path, timeout=8)

    # Fill password.
    _fill_edit(dlg, _PFX_PASSWORD_EDIT_ID, password, timeout=5)

    # Optional metadata.
    if location is not None:
        _fill_edit(dlg, _LOCATION_EDIT_ID, location, timeout=5)
    if reason is not None:
        _fill_edit(dlg, _REASON_EDIT_ID, reason, timeout=5)

    wait_stable(0.2)  # Let validateInput enable the Sign button.

    # Click Sign (okButton).
    _click_ok(dlg, _SIGN_OK_BUTTON_ID, fallback_key="enter")
    wait_stable(_ACTION_SETTLE)

    # Handle the "Save Signed PDF" QFileDialog.
    _handle_save_dialog(session, _SAVE_SIGNED_DIALOG_TITLE_RE, output_path)

    # Wait for the signature panel to appear (PDFE shows it after signing).
    _wait_for_signature_panel(session, timeout=_DIALOG_TIMEOUT)
    wait_stable(_SIGN_SETTLE)


def validate_signature(session, idx):
    """Verify all signatures and return the status string for row *idx*.

    Invokes ``verifySignaturesAction`` (which calls
    ``SignatureManager::verifyAllSignatures`` and populates
    ``signatureTreeWidget``), dismisses the result summary
    QMessageBox::information, then reads column 3 ("Status") of the tree row
    at zero-based *idx*.

    Parameters
    ----------
    session : PDFESession
    idx     : int   Zero-based row index into the Signatures List tree.

    Returns
    -------
    str
        Text of column 3 at row *idx*.  Typical values from SignaturePanel.cpp
        ``setSignatures()``: ``"Signed"`` or ``"Unsigned"``.

    Raises
    ------
    RuntimeError
        If the Verify action cannot be invoked, the panel does not appear, or
        the row at *idx* is out of range.
    """
    _invoke_verify_action(session)

    # Dismiss the QMessageBox::information that always appears.
    _dismiss_dialog_if_present(session, title_re=_VERIFY_RESULT_TITLE_RE, timeout=_DIALOG_TIMEOUT)

    # Ensure the dock is visible before reading the tree.
    _wait_for_signature_panel(session, timeout=_PANEL_TIMEOUT)
    wait_stable(_ACTION_SETTLE)

    return _read_tree_status(session, idx)


def certify_document(session, perms="NoChanges"):
    """Open SignDialog, select the certification level, then cancel.

    This helper exercises the certification combo without actually signing
    (no certificate path is supplied, so the Sign button remains disabled).
    It is useful for verifying that the combo is populated correctly and that
    the desired permission level is selectable.

    To perform a real certified signature, call ``sign_document`` and set the
    ``certificationCombo`` via the internal helper, or use this function as a
    pre-flight check followed by a separate ``sign_document`` call.

    Parameters
    ----------
    session : PDFESession
    perms   : str
              One of ``"NoChanges"``, ``"FormFillOnly"``,
              ``"AnnotationsAllowed"``.  Mapped to ``certificationCombo``
              item index per SignDialog.cpp.

    Raises
    ------
    ValueError
        If *perms* is not one of the recognised values.
    RuntimeError
        If SignDialog cannot be opened.
    """
    if perms not in _CERTIFICATION_COMBO_MAP:
        raise ValueError(
            f"certify_document: perms={perms!r} is not valid. "
            f"Must be one of: {sorted(_CERTIFICATION_COMBO_MAP)}"
        )

    _invoke_sign_action(session)

    dlg = _wait_for_dialog(session, title_re=_SIGN_DIALOG_TITLE, timeout=_DIALOG_TIMEOUT)

    # Select the certification level on the combo.
    _set_certification_combo(dlg, perms)
    wait_stable(_ACTION_SETTLE)

    # Cancel — no certificate was supplied so Sign button is disabled anyway.
    _click_cancel(dlg)
    wait_stable(_ACTION_SETTLE)


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _invoke_sign_action(session):
    """Invoke ``signDocumentAction`` via UIA or the Security menu.

    Raises
    ------
    RuntimeError
        If the action cannot be invoked by any strategy.
    """
    window = session.window

    # Strategy 1: UIA auto_id (objectName = "signDocumentAction").
    try:
        ctrl = by_auto_id_or_name(
            window,
            _SIGN_DOCUMENT_ACTION_ID,
            _SIGN_DOCUMENT_ACTION_NAME,
            timeout=8,
        )
        invoke(ctrl)
        wait_stable(_ACTION_SETTLE)
        return
    except Exception:
        pass

    # Strategy 2: Security menu.
    try:
        # GAP-A14: prefer the deadlock-free pipe seam; fall through to the
        # UIA menu walk only if the pipe isn't available (non-test build).
        try:
            from tests.ui_harness.core.locate import invoke_action
            invoke_action(window, "signDocumentAction")
            return
        except Exception:  # noqa: BLE001
            pass
        window.set_focus()
        # GAP-A14: UIA menu walk fallback, retained only for interactive/local
        # hosts where menu_select is reliable. On Server-2019 CI it can deadlock —
        # prefer invoke_action(window, "signDocumentAction") (applied just above).
        window.menu_select(_MENU_SIGN_DOCUMENT)
        wait_stable(_ACTION_SETTLE)
        return
    except Exception as exc:
        raise RuntimeError(
            f"Could not invoke Sign Document action "
            f"(auto_id={_SIGN_DOCUMENT_ACTION_ID!r}, "
            f"name={_SIGN_DOCUMENT_ACTION_NAME!r}, "
            f"menu={_MENU_SIGN_DOCUMENT!r}): {exc}"
        ) from exc


def _invoke_verify_action(session):
    """Invoke ``verifySignaturesAction`` via UIA, panel button, or menu.

    Raises
    ------
    RuntimeError
        If the action cannot be invoked by any strategy.
    """
    window = session.window

    # Strategy 1: UIA auto_id (objectName = "verifySignaturesAction").
    try:
        ctrl = by_auto_id_or_name(
            window,
            _VERIFY_SIGNATURES_ACTION_ID,
            _VERIFY_SIGNATURES_ACTION_NAME,
            timeout=8,
        )
        invoke(ctrl)
        wait_stable(_ACTION_SETTLE)
        return
    except Exception:
        pass

    # Strategy 2: verifyAllButton in the SignaturePanel (already open).
    try:
        btn = by_auto_id_or_name(
            window,
            _VERIFY_ALL_BUTTON_ID,
            _VERIFY_ALL_BUTTON_NAME,
            timeout=4,
        )
        invoke(btn)
        wait_stable(_ACTION_SETTLE)
        return
    except Exception:
        pass

    # Strategy 3: Security menu.
    try:
        # GAP-A14: prefer the deadlock-free pipe seam; fall through to the
        # UIA menu walk only if the pipe isn't available (non-test build).
        try:
            from tests.ui_harness.core.locate import invoke_action
            invoke_action(window, "verifySignaturesAction")
            return
        except Exception:  # noqa: BLE001
            pass
        window.set_focus()
        # GAP-A14: UIA menu walk fallback, retained only for interactive/local
        # hosts where menu_select is reliable. On Server-2019 CI it can deadlock —
        # prefer invoke_action(window, "verifySignaturesAction") (applied just above).
        window.menu_select(_MENU_VERIFY_SIGNATURES)
        wait_stable(_ACTION_SETTLE)
        return
    except Exception as exc:
        raise RuntimeError(
            f"Could not invoke Verify Signatures action "
            f"(auto_id={_VERIFY_SIGNATURES_ACTION_ID!r}, "
            f"name={_VERIFY_SIGNATURES_ACTION_NAME!r}, "
            f"menu={_MENU_VERIFY_SIGNATURES!r}): {exc}"
        ) from exc


def _wait_for_dialog(session, title_re, timeout=_DIALOG_TIMEOUT):
    """Block until a top-level dialog matching *title_re* appears.

    Returns
    -------
    pywinauto WindowSpecification

    Raises
    ------
    RuntimeError
        If no matching dialog appears within *timeout* seconds.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            dlg = session.app.window(title_re=title_re)
            dlg.wait("visible", timeout=1)
            return dlg
        except Exception:
            time.sleep(0.3)

    raise RuntimeError(
        f"Dialog matching title_re={title_re!r} did not appear within {timeout}s."
    )


def _fill_edit(parent, auto_id, text, timeout=5):
    """Clear and type *text* into a QLineEdit identified by *auto_id*.

    Falls back to pyautogui typewrite if pywinauto cannot set the value
    directly.
    """
    # Strategy 1: pywinauto locate + type_keys.
    try:
        ctrl = by_auto_id_or_name(parent, auto_id, auto_id, timeout=timeout)
        try:
            ctrl.set_focus()
            ctrl.type_keys("^a{DELETE}", with_spaces=False)
        except Exception:
            pass
        pyautogui.typewrite(text, interval=0.02)
        return
    except Exception:
        pass

    # Strategy 2: locate by accessible name then pyautogui.
    try:
        ctrl = by_name(parent, auto_id, timeout=timeout)
        ctrl.set_focus()
        pyautogui.hotkey("ctrl", "a")
        pyautogui.press("delete")
        pyautogui.typewrite(text, interval=0.02)
    except Exception as exc:
        raise RuntimeError(
            f"Could not fill edit auto_id={auto_id!r}: {exc}"
        ) from exc


def _click_ok(parent, auto_id, fallback_key="enter"):
    """Click the OK/Sign button, falling back to a keypress."""
    try:
        btn = by_auto_id_or_name(parent, auto_id, auto_id, timeout=5)
        invoke(btn)
        return
    except Exception:
        pass
    pyautogui.press(fallback_key)


def _click_cancel(parent):
    """Click the Cancel button, falling back to Escape."""
    try:
        btn = by_auto_id_or_name(parent, _CANCEL_BUTTON_ID, "Cancel", timeout=5)
        invoke(btn)
        return
    except Exception:
        pass
    pyautogui.press("escape")


def _set_certification_combo(dlg, perms):
    """Select the certification combo item corresponding to *perms*.

    Parameters
    ----------
    dlg   : pywinauto WindowSpecification   The open SignDialog.
    perms : str   One of the keys in ``_CERTIFICATION_COMBO_MAP``.
    """
    target_idx = _CERTIFICATION_COMBO_MAP[perms]

    # Strategy 1: pywinauto .select(index).
    try:
        combo = by_auto_id_or_name(
            dlg, _CERTIFICATION_COMBO_ID, _CERTIFICATION_COMBO_NAME, timeout=5
        )
        combo.select(target_idx)
        return
    except Exception:
        pass

    # Strategy 2: focus combo and arrow-key to the target index.
    try:
        combo = by_auto_id_or_name(
            dlg, _CERTIFICATION_COMBO_ID, _CERTIFICATION_COMBO_NAME, timeout=5
        )
        combo.set_focus()
        pyautogui.hotkey("alt", "up")   # Collapse if open.
        pyautogui.press("home")
        for _ in range(target_idx):
            pyautogui.press("down")
    except Exception as exc:
        raise RuntimeError(
            f"Could not set certificationCombo to index {target_idx} "
            f"(perms={perms!r}): {exc}"
        ) from exc


def _handle_save_dialog(session, title_re, output_path):
    """Type *output_path* in the Save QFileDialog and accept.

    If *output_path* is None the dialog is accepted with its default filename.
    """
    deadline = time.time() + _DIALOG_TIMEOUT
    dlg = None
    while time.time() < deadline:
        try:
            dlg = session.app.window(title_re=title_re)
            dlg.wait("visible", timeout=1)
            break
        except Exception:
            time.sleep(0.3)

    if dlg is None:
        # Dialog did not appear — signing may have failed or been cancelled.
        return

    if output_path is not None:
        # Focus the filename field and type the path.
        try:
            fn_edit = by_name(dlg, "File name:", timeout=5)
            fn_edit.set_focus()
            fn_edit.type_keys("^a{DELETE}", with_spaces=False)
        except Exception:
            pyautogui.hotkey("ctrl", "a")
            pyautogui.press("delete")
        pyautogui.typewrite(output_path, interval=0.01)

    pyautogui.press("enter")
    wait_stable(_ACTION_SETTLE)


def _wait_for_signature_panel(session, timeout=_PANEL_TIMEOUT):
    """Wait until the signaturePanelDock is visible.

    Non-fatal: if the dock does not appear (e.g. no signatures), the function
    returns silently rather than raising.
    """
    window = session.window
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            dock = by_auto_id_or_name(
                window,
                _SIGNATURE_PANEL_DOCK_ID,
                _SIGNATURE_PANEL_DOCK_NAME,
                timeout=2,
            )
            if dock.is_visible():
                return
        except Exception:
            pass
        time.sleep(0.3)


def _read_tree_status(session, idx):
    """Read column 3 ("Status") from the signatureTreeWidget at row *idx*.

    SignaturePanel.cpp ``setSignatures()`` populates column 3 with
    ``sigData.isSigned ? "Signed" : "Unsigned"``.

    Parameters
    ----------
    session : PDFESession
    idx     : int   Zero-based row index.

    Returns
    -------
    str   Status text at row *idx*.

    Raises
    ------
    RuntimeError
        If the tree cannot be found or *idx* is out of range.
    """
    window = session.window

    try:
        tree = by_auto_id_or_name(
            window,
            _SIGNATURE_TREE_ID,
            _SIGNATURE_TREE_NAME,
            timeout=_PANEL_TIMEOUT,
        )
    except Exception as exc:
        raise RuntimeError(
            f"Could not locate signatureTreeWidget "
            f"(auto_id={_SIGNATURE_TREE_ID!r}): {exc}"
        ) from exc

    try:
        items = tree.children()
    except Exception as exc:
        raise RuntimeError(
            f"Could not enumerate signatureTreeWidget children: {exc}"
        ) from exc

    if idx >= len(items):
        raise RuntimeError(
            f"validate_signature: idx={idx} is out of range "
            f"(tree has {len(items)} rows)."
        )

    try:
        row_item = items[idx]
        # Attempt to read column 3 from child sub-items.
        sub_items = row_item.children()
        if len(sub_items) >= 4:
            return sub_items[3].window_text()
        # Fallback: parse the last whitespace-delimited token from the full row text
        # (the four columns are typically space-separated in the UIA name).
        row_text = row_item.window_text()
        parts = row_text.split()
        if parts:
            return parts[-1]
        return row_text
    except Exception as exc:
        raise RuntimeError(
            f"Could not read Status from signatureTreeWidget row {idx}: {exc}"
        ) from exc


def _dismiss_dialog_if_present(session, title_re, timeout=5):
    """Accept (press Enter) a dialog if it appears within *timeout* seconds.

    Used to dismiss the QMessageBox::information that ``onVerifySignatures``
    always shows.  Non-fatal if no dialog appears.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            dlg = session.app.window(title_re=title_re)
            dlg.wait("visible", timeout=1)
            try:
                ok_btn = by_name(dlg, "OK", timeout=2)
                invoke(ok_btn)
            except Exception:
                pyautogui.press("enter")
            wait_stable(0.2)
            return
        except Exception:
            time.sleep(0.3)
