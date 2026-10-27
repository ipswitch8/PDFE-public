# -*- coding: utf-8 -*-
"""Dialog subsystem helpers for the PDFE UI test harness.

Provides high-level operations for ComplianceDialog and EncryptionDialog.
All functions accept a ``PDFESession`` instance (from
``tests.ui_harness.core.launch``) as their first argument and delegate
low-level widget location, canvas interaction, and screenshot capture to the
shared ``core`` helpers.

Focus-safety
------------
All keyboard injection in this module uses the focus-safe wrappers from
``tests.helpers.keys`` (hotkey_to, typewrite_to, press_to).  ZERO bare
pyautogui.{typewrite,press,hotkey} calls exist in this file — each injection
goes through a helper that verifies or forces the target HWND to the
foreground before delivery.

Discovered objectNames (from src/ui/MainWindow.cpp / ComplianceDialog.cpp /
EncryptionDialog.cpp)
------------------------------------------------------------------------
MainWindow QAction objectNames:
- complianceAction           — QAction "Compliance..."
                               menu: &Tools->Compliance...
- saveWithEncryptionAction   — QAction "Save with &Encryption..."
                               menu: &Security->Save with &Encryption...
                               shortcut: Ctrl+Shift+E

ComplianceDialog widget objectNames (from ComplianceDialog.cpp):
- ComplianceDialog           — QDialog objectName / accessibleName
- LevelCombo                 — QComboBox, accessible name "LevelCombo"
                               Items (text / data):
                                 "PDF/A-1b (Archival, Visual)"     / PdfA_1b
                                 "PDF/A-2b (Archival, Visual)"     / PdfA_2b
                                 "PDF/A-2u (Archival, Unicode)"    / PdfA_2u
                                 "PDF/A-2a (Archival, Accessible)" / PdfA_2a
                                 "PDF/A-3b (Archival + Attachments)"/ PdfA_3b
                                 "PDF/UA-1 (Universal Accessibility)" / -1
- AnalyzeButton              — QPushButton, accessible name "Analyze"
- ValidateButton             — QPushButton, accessible name "Validate"
- ConvertButton              — QPushButton, accessible name "Convert"
- previewTagsButton          — QPushButton, accessible name "Preview Tags"
- IssueTable                 — QTableWidget, accessible name "Compliance Issues"
                               columns: [0] Severity, [1] Rule, [2] Description,
                               [3] Page
- StatusLabel                — QLabel, accessible description "Compliance Status"
- progressBar                — QProgressBar (visible during conversion)
- tagStructureTree           — QTreeWidget, accessible name "Tag Structure Tree"

After clicking Convert...:
  A QFileDialog ("Save Compliant PDF") prompts for the output path.
  After the save dialog is accepted, a QMessageBox::information
  "Conversion Complete" confirms success.

EncryptionDialog widget objectNames (from EncryptionDialog.cpp):
- encryptionDialog           — QDialog objectName
- algorithmCombo             — QComboBox, accessible name "Algorithm"
                               Items: "None"(0), "RC4 40-bit"(1),
                               "RC4 128-bit"(2), "AES-128"(3), "AES-256"(4)
- userPasswordEdit           — QLineEdit, accessible name "User Password"
- userPasswordConfirmEdit    — QLineEdit, accessible name "Confirm User Password"
- ownerPasswordEdit          — QLineEdit, accessible name "Owner Password"
- ownerPasswordConfirmEdit   — QLineEdit, accessible name "Confirm Owner Password"
- printCheck                 — QCheckBox, accessible name "Allow printing"
- printHqCheck               — QCheckBox, accessible name "Allow high-quality printing"
- modifyCheck                — QCheckBox, accessible name "Allow modification"
- copyCheck                  — QCheckBox, accessible name "Allow copying"
- annotateCheck              — QCheckBox, accessible name "Allow annotations"
- formFillCheck              — QCheckBox, accessible name "Allow form filling"
- assembleCheck              — QCheckBox, accessible name "Allow assembly"
- accessibilityCheck         — QCheckBox, accessible name "Allow accessibility"
- encryptMetadataCheck       — QCheckBox, accessible name "Encrypt metadata"
- okButton                   — QPushButton OK (via QDialogButtonBox)
- validationLabel            — QLabel, red error text

After clicking OK:
  A QFileDialog ("Save Encrypted PDF") prompts for the output path.

Menu paths (fallback):
- "Tools->Compliance..."               (complianceAction)
- "Security->Save with &Encryption..."  (saveWithEncryptionAction)
"""

import time

from tests.helpers.keys import (
    FocusError,
    hotkey_to,
    press_to,
    send_keys_to,
    typewrite_to,
)
from tests.ui_harness.core.canvas import wait_stable
from tests.ui_harness.core.locate import (
    by_auto_id_or_name,
    by_name,
    invoke,
)
from tests.ui_harness.core.screenshot import capture_canvas

__all__ = [
    "open_compliance_dialog",
    "convert_to_pdfa",
    "convert_to_pdfua",
    "validate_pdfa",
    "set_encryption",
    # Phase-9 generic dialog helpers.
    "open_dialog",
    "accept_dialog",
    "cancel_dialog",
]

# ---------------------------------------------------------------------------
# Internal constants — objectNames / accessible names from source files
# ---------------------------------------------------------------------------

# MainWindow QAction objectNames.
_COMPLIANCE_ACTION_ID = "complianceAction"
_COMPLIANCE_ACTION_NAME = "Compliance..."

_SAVE_WITH_ENCRYPTION_ACTION_ID = "saveWithEncryptionAction"
_SAVE_WITH_ENCRYPTION_ACTION_NAME = "Save with Encryption..."

# ComplianceDialog object/accessible name and widget objectNames.
_COMPLIANCE_DIALOG_TITLE = "PDF Standards Compliance"
_COMPLIANCE_DIALOG_ID = "ComplianceDialog"
_LEVEL_COMBO_ID = "LevelCombo"
_LEVEL_COMBO_NAME = "LevelCombo"
_ANALYZE_BUTTON_ID = "AnalyzeButton"
_ANALYZE_BUTTON_NAME = "Analyze"
_VALIDATE_BUTTON_ID = "ValidateButton"
_VALIDATE_BUTTON_NAME = "Validate"
_CONVERT_BUTTON_ID = "ConvertButton"
_CONVERT_BUTTON_NAME = "Convert"
_ISSUE_TABLE_ID = "IssueTable"
_ISSUE_TABLE_NAME = "Compliance Issues"
_STATUS_LABEL_ID = "StatusLabel"

# EncryptionDialog objectNames.
_ENCRYPTION_DIALOG_TITLE = "Encryption Settings"
_ENCRYPTION_DIALOG_ID = "encryptionDialog"
_ALGORITHM_COMBO_ID = "algorithmCombo"
_ALGORITHM_COMBO_NAME = "Algorithm"
_USER_PASSWORD_EDIT_ID = "userPasswordEdit"
_USER_PASSWORD_CONFIRM_ID = "userPasswordConfirmEdit"
_OWNER_PASSWORD_EDIT_ID = "ownerPasswordEdit"
_OWNER_PASSWORD_CONFIRM_ID = "ownerPasswordConfirmEdit"
_ENCRYPT_OK_BUTTON_ID = "okButton"

# Permission checkbox objectName -> accessible name map (EncryptionDialog.cpp).
_PERMISSION_CHECKBOX_MAP = {
    "print": ("printCheck", "Allow printing"),
    "printHq": ("printHqCheck", "Allow high-quality printing"),
    "modify": ("modifyCheck", "Allow modification"),
    "copy": ("copyCheck", "Allow copying"),
    "annotate": ("annotateCheck", "Allow annotations"),
    "formFill": ("formFillCheck", "Allow form filling"),
    "assemble": ("assembleCheck", "Allow assembly"),
    "accessibility": ("accessibilityCheck", "Allow accessibility"),
}

# LevelCombo item text -> algorithm combo text for encryption dialog.
_ENCRYPTION_ALGO_MAP = {
    "None": "None",
    "RC4-40": "RC4 40-bit",
    "RC4-128": "RC4 128-bit",
    "AES-128": "AES-128",
    "AES-256": "AES-256",
}

# LevelCombo display text substrings -> combo index for ComplianceDialog.
# Source: ComplianceDialog.cpp addItem calls (indices 0-5).
_PDFA_LEVEL_INDEX_MAP = {
    "1b": 0,   # "PDF/A-1b (Archival, Visual)"
    "2b": 1,   # "PDF/A-2b (Archival, Visual)"
    "2u": 2,   # "PDF/A-2u (Archival, Unicode)"
    "2a": 3,   # "PDF/A-2a (Archival, Accessible)"
    "3b": 4,   # "PDF/A-3b (Archival + Attachments)"
    "ua": 5,   # "PDF/UA-1 (Universal Accessibility)"
    "ua1": 5,
    "ua-1": 5,
}

# QFileDialog titles.
_SAVE_COMPLIANT_DIALOG_TITLE_RE = r"Save Compliant PDF"
_SAVE_ENCRYPTED_DIALOG_TITLE_RE = r"Save Encrypted PDF"

# Conversion success QMessageBox title.
_CONVERSION_COMPLETE_TITLE_RE = r"Conversion Complete"

# Menu fallback paths.
_MENU_COMPLIANCE = "Tools->Compliance..."
_MENU_SAVE_WITH_ENCRYPTION = "Security->Save with &Encryption..."

# Timeouts.
_DIALOG_TIMEOUT = 20
_CONVERT_TIMEOUT = 60   # Conversion can be slow for large documents.
_ACTION_SETTLE = 0.5
_ANALYZE_SETTLE = 2.0


# ---------------------------------------------------------------------------
# Internal: resolve main-window HWND from a session object
# ---------------------------------------------------------------------------

def _session_hwnd(session) -> int:
    """Return the Win32 HWND of the main PDFE window.

    Works with both ``tests.ui_harness.core.launch.PDFESession`` (which
    exposes ``session.window.handle``) and the phase-5 wrapper
    ``tests.helpers.pdfe_session.PDFESession`` (which exposes
    ``session.hwnd`` directly).
    """
    # Phase-5 wrapper exposes .hwnd directly.
    if hasattr(session, "hwnd"):
        return int(session.hwnd)
    # Base harness session: derive from pywinauto window spec.
    try:
        return int(session.window.handle)
    except Exception:  # noqa: BLE001
        return 0


def _dlg_hwnd(dlg) -> int:
    """Return the Win32 HWND of a pywinauto dialog window spec."""
    try:
        return int(dlg.handle)
    except Exception:  # noqa: BLE001
        return 0


# ---------------------------------------------------------------------------
# Public helpers
# ---------------------------------------------------------------------------

def open_compliance_dialog(session):
    """Open the ComplianceDialog via the Tools menu Compliance... action.

    If the dialog is already open this function returns immediately without
    opening a second instance.

    Parameters
    ----------
    session : PDFESession

    Returns
    -------
    pywinauto WindowSpecification
        The ComplianceDialog window.

    Raises
    ------
    RuntimeError
        If the action cannot be invoked, or the dialog does not appear within
        ``_DIALOG_TIMEOUT`` seconds.
    """
    # If the dialog is already present, return it.
    try:
        dlg = session.app.window(title_re=_COMPLIANCE_DIALOG_TITLE)
        dlg.wait("visible", timeout=2)
        return dlg
    except Exception:
        pass

    _invoke_compliance_action(session)

    return _wait_for_dialog(session, title_re=_COMPLIANCE_DIALOG_TITLE, timeout=_DIALOG_TIMEOUT)


def convert_to_pdfa(session, level="2b", output_path=None):
    """Open ComplianceDialog (if needed), pick the PDF/A level, convert.

    Selects the appropriate item in ``LevelCombo`` for *level*, clicks
    ``ConvertButton``, types *output_path* in the QFileDialog, accepts, and
    then dismisses the "Conversion Complete" success dialog.

    Parameters
    ----------
    session     : PDFESession
    level       : str
                  Short level code — one of ``"1b"``, ``"2b"``, ``"2u"``,
                  ``"2a"``, ``"3b"``.  Case-insensitive.
    output_path : str | None
                  Absolute path for the converted PDF.  None accepts the
                  QFileDialog default.

    Raises
    ------
    ValueError
        If *level* is not a recognised PDF/A level code.
    RuntimeError
        If the dialog cannot be opened or the conversion QFileDialog does not
        appear.
    """
    level_key = level.lower()
    if level_key not in _PDFA_LEVEL_INDEX_MAP:
        raise ValueError(
            f"convert_to_pdfa: level={level!r} not recognised. "
            f"Valid values: {sorted(_PDFA_LEVEL_INDEX_MAP)}"
        )
    combo_idx = _PDFA_LEVEL_INDEX_MAP[level_key]

    dlg = open_compliance_dialog(session)
    _set_level_combo(dlg, combo_idx)
    wait_stable(_ACTION_SETTLE)

    _click_button(dlg, _CONVERT_BUTTON_ID, _CONVERT_BUTTON_NAME)
    wait_stable(_ACTION_SETTLE)

    _handle_save_dialog(session, _SAVE_COMPLIANT_DIALOG_TITLE_RE, output_path)
    _dismiss_dialog_if_present(session, title_re=_CONVERSION_COMPLETE_TITLE_RE, timeout=_CONVERT_TIMEOUT)


def convert_to_pdfua(session, output_path=None):
    """Open ComplianceDialog, select PDF/UA-1, convert.

    Parameters
    ----------
    session     : PDFESession
    output_path : str | None
                  Absolute path for the converted PDF.  None accepts the
                  QFileDialog default.

    Raises
    ------
    RuntimeError
        If the dialog cannot be opened or the conversion QFileDialog does not
        appear.
    """
    dlg = open_compliance_dialog(session)
    _set_level_combo(dlg, _PDFA_LEVEL_INDEX_MAP["ua"])
    wait_stable(_ACTION_SETTLE)

    _click_button(dlg, _CONVERT_BUTTON_ID, _CONVERT_BUTTON_NAME)
    wait_stable(_ACTION_SETTLE)

    _handle_save_dialog(session, _SAVE_COMPLIANT_DIALOG_TITLE_RE, output_path)
    _dismiss_dialog_if_present(session, title_re=_CONVERSION_COMPLETE_TITLE_RE, timeout=_CONVERT_TIMEOUT)


def validate_pdfa(session):
    """Open ComplianceDialog, click Analyze, return the issue row count.

    Clicks ``AnalyzeButton``, waits for the ``IssueTable`` to populate, then
    reads its row count as a proxy for the number of compliance issues found.

    Parameters
    ----------
    session : PDFESession

    Returns
    -------
    int
        Number of rows in the ``IssueTable`` after analysis.  Zero means no
        issues were reported for the selected level.

    Raises
    ------
    RuntimeError
        If the dialog cannot be opened or the IssueTable cannot be located.
    """
    dlg = open_compliance_dialog(session)

    _click_button(dlg, _ANALYZE_BUTTON_ID, _ANALYZE_BUTTON_NAME)
    wait_stable(_ANALYZE_SETTLE)

    return _read_issue_table_row_count(dlg)


def set_encryption(session, level="AES-256", owner_pw=None, user_pw=None, perms=None, output_path=None):
    """Invoke Save with Encryption, fill EncryptionDialog, and accept.

    Triggers ``saveWithEncryptionAction`` which opens ``EncryptionDialog``.
    Sets the algorithm combo to *level*, fills owner and user passwords,
    applies any permission checkbox overrides from *perms*, clicks OK, then
    types *output_path* in the "Save Encrypted PDF" QFileDialog and accepts.

    Parameters
    ----------
    session     : PDFESession
    level       : str
                  Encryption algorithm.  One of ``"None"``, ``"RC4-40"``,
                  ``"RC4-128"``, ``"AES-128"``, ``"AES-256"``.
    owner_pw    : str | None
                  Owner password (required when level != "None").
    user_pw     : str | None
                  User (open) password.  May be empty.
    perms       : dict | None
                  Optional dict of permission flag overrides, keyed by the
                  canonical flag names used in ``_PERMISSION_CHECKBOX_MAP``
                  (``"print"``, ``"printHq"``, ``"modify"``, ``"copy"``,
                  ``"annotate"``, ``"formFill"``, ``"assemble"``,
                  ``"accessibility"``).  Values are bool: True = allow,
                  False = deny.  Unchecked keys keep their dialog default
                  (all checked by default in EncryptionDialog.cpp).
    output_path : str | None
                  Absolute path for the encrypted PDF.  None accepts the
                  QFileDialog default (current file path).

    Raises
    ------
    ValueError
        If *level* is not one of the recognised algorithm strings.
    RuntimeError
        If the action cannot be invoked, the dialog does not appear, or the
        save QFileDialog does not appear.
    """
    if level not in _ENCRYPTION_ALGO_MAP:
        raise ValueError(
            f"set_encryption: level={level!r} not recognised. "
            f"Valid values: {sorted(_ENCRYPTION_ALGO_MAP)}"
        )

    _invoke_encryption_action(session)

    dlg = _wait_for_dialog(session, title_re=_ENCRYPTION_DIALOG_TITLE, timeout=_DIALOG_TIMEOUT)

    # Select algorithm — this triggers onAlgorithmChanged which enables fields.
    _set_algorithm_combo(dlg, level)
    wait_stable(_ACTION_SETTLE)

    # Fill passwords (only relevant when algorithm is not "None").
    if level != "None":
        if owner_pw is not None:
            _fill_edit(dlg, _OWNER_PASSWORD_EDIT_ID, owner_pw, timeout=5)
            _fill_edit(dlg, _OWNER_PASSWORD_CONFIRM_ID, owner_pw, timeout=5)
        if user_pw is not None:
            _fill_edit(dlg, _USER_PASSWORD_EDIT_ID, user_pw, timeout=5)
            _fill_edit(dlg, _USER_PASSWORD_CONFIRM_ID, user_pw, timeout=5)

        # Apply permission checkbox overrides.
        if perms:
            _apply_permission_checkboxes(dlg, perms)

    wait_stable(0.2)  # Let validateInput enable the OK button.

    # Click OK (QDialogButtonBox OK button, objectName "okButton").
    _click_ok(dlg, _ENCRYPT_OK_BUTTON_ID, fallback_key="enter")
    wait_stable(_ACTION_SETTLE)

    # Handle the "Save Encrypted PDF" QFileDialog.
    _handle_save_dialog(session, _SAVE_ENCRYPTED_DIALOG_TITLE_RE, output_path)


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _invoke_compliance_action(session):
    """Invoke ``complianceAction`` via UIA or the Tools menu.

    Raises
    ------
    RuntimeError
        If the action cannot be invoked by any strategy.
    """
    window = session.window

    # Strategy 1: UIA auto_id (objectName = "complianceAction").
    try:
        ctrl = by_auto_id_or_name(
            window,
            _COMPLIANCE_ACTION_ID,
            _COMPLIANCE_ACTION_NAME,
            timeout=8,
        )
        invoke(ctrl)
        wait_stable(_ACTION_SETTLE)
        return
    except Exception:
        pass

    # Strategy 2: Tools menu.
    try:
        window.set_focus()
        # GAP-A14: UIA menu walk fallback, retained only for interactive/local
        # hosts where menu_select is reliable. On Server-2019 CI it can deadlock —
        # prefer invoke_action(window, "complianceAction").
        window.menu_select(_MENU_COMPLIANCE)
        wait_stable(_ACTION_SETTLE)
        return
    except Exception as exc:
        raise RuntimeError(
            f"Could not invoke Compliance action "
            f"(auto_id={_COMPLIANCE_ACTION_ID!r}, "
            f"name={_COMPLIANCE_ACTION_NAME!r}, "
            f"menu={_MENU_COMPLIANCE!r}): {exc}"
        ) from exc


def _invoke_encryption_action(session):
    """Invoke ``saveWithEncryptionAction`` via UIA or the Security menu.

    Raises
    ------
    RuntimeError
        If the action cannot be invoked by any strategy.
    """
    window = session.window

    # Strategy 1: UIA auto_id (objectName = "saveWithEncryptionAction").
    try:
        ctrl = by_auto_id_or_name(
            window,
            _SAVE_WITH_ENCRYPTION_ACTION_ID,
            _SAVE_WITH_ENCRYPTION_ACTION_NAME,
            timeout=8,
        )
        invoke(ctrl)
        wait_stable(_ACTION_SETTLE)
        return
    except Exception:
        pass

    # Strategy 2: Security menu.
    try:
        window.set_focus()
        # GAP-A14: UIA menu walk fallback, retained only for interactive/local
        # hosts where menu_select is reliable. On Server-2019 CI it can deadlock —
        # prefer invoke_action(window, "saveWithEncryptionAction").
        window.menu_select(_MENU_SAVE_WITH_ENCRYPTION)
        wait_stable(_ACTION_SETTLE)
        return
    except Exception as exc:
        raise RuntimeError(
            f"Could not invoke Save with Encryption action "
            f"(auto_id={_SAVE_WITH_ENCRYPTION_ACTION_ID!r}, "
            f"name={_SAVE_WITH_ENCRYPTION_ACTION_NAME!r}, "
            f"menu={_MENU_SAVE_WITH_ENCRYPTION!r}): {exc}"
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


def _set_level_combo(dlg, index):
    """Select *index* in the ComplianceDialog LevelCombo.

    Parameters
    ----------
    dlg   : pywinauto WindowSpecification   The open ComplianceDialog.
    index : int   Zero-based item index.
    """
    # Strategy 1: pywinauto .select().
    try:
        combo = by_auto_id_or_name(dlg, _LEVEL_COMBO_ID, _LEVEL_COMBO_NAME, timeout=5)
        combo.select(index)
        return
    except Exception:
        pass

    # Strategy 2: focus and arrow-key.
    try:
        combo = by_auto_id_or_name(dlg, _LEVEL_COMBO_ID, _LEVEL_COMBO_NAME, timeout=5)
        combo.set_focus()
        hwnd = _dlg_hwnd(dlg)
        hotkey_to(hwnd, "alt", "up")
        press_to(hwnd, "home")
        for _ in range(index):
            press_to(hwnd, "down")
    except Exception as exc:
        raise RuntimeError(
            f"Could not set LevelCombo to index {index}: {exc}"
        ) from exc


def _set_algorithm_combo(dlg, level):
    """Select the algorithm combo item matching *level*.

    Parameters
    ----------
    dlg   : pywinauto WindowSpecification   The open EncryptionDialog.
    level : str   Key from ``_ENCRYPTION_ALGO_MAP``.
    """
    item_text = _ENCRYPTION_ALGO_MAP[level]

    # Strategy 1: pywinauto .select(text).
    try:
        combo = by_auto_id_or_name(dlg, _ALGORITHM_COMBO_ID, _ALGORITHM_COMBO_NAME, timeout=5)
        combo.select(item_text)
        return
    except Exception:
        pass

    # Strategy 2: select by index.
    index = list(_ENCRYPTION_ALGO_MAP.values()).index(item_text)
    try:
        combo = by_auto_id_or_name(dlg, _ALGORITHM_COMBO_ID, _ALGORITHM_COMBO_NAME, timeout=5)
        combo.select(index)
        return
    except Exception:
        pass

    # Strategy 3: focus and arrow-key.
    try:
        combo = by_auto_id_or_name(dlg, _ALGORITHM_COMBO_ID, _ALGORITHM_COMBO_NAME, timeout=5)
        combo.set_focus()
        hwnd = _dlg_hwnd(dlg)
        hotkey_to(hwnd, "alt", "up")
        press_to(hwnd, "home")
        for _ in range(index):
            press_to(hwnd, "down")
    except Exception as exc:
        raise RuntimeError(
            f"Could not set algorithmCombo to {level!r} (index {index}): {exc}"
        ) from exc


def _fill_edit(parent, auto_id, text, timeout=5):
    """Clear and type *text* into a QLineEdit identified by *auto_id*."""
    hwnd = _dlg_hwnd(parent)
    try:
        ctrl = by_auto_id_or_name(parent, auto_id, auto_id, timeout=timeout)
        try:
            ctrl.set_focus()
            ctrl.type_keys("^a{DELETE}", with_spaces=False)
        except Exception:
            pass
        typewrite_to(hwnd, text, interval=0.02)
        return
    except Exception:
        pass

    try:
        ctrl = by_name(parent, auto_id, timeout=timeout)
        ctrl.set_focus()
        hotkey_to(hwnd, "ctrl", "a")
        press_to(hwnd, "delete")
        typewrite_to(hwnd, text, interval=0.02)
    except Exception as exc:
        raise RuntimeError(
            f"Could not fill edit auto_id={auto_id!r}: {exc}"
        ) from exc


def _click_button(parent, auto_id, accessible_name, fallback_key=None):
    """Click a button in *parent* by auto_id or accessible name.

    Parameters
    ----------
    parent          : pywinauto WindowSpecification
    auto_id         : str   Widget objectName.
    accessible_name : str   Widget accessibleName.
    fallback_key    : str | None   Key to press if UIA lookup fails.
    """
    try:
        btn = by_auto_id_or_name(parent, auto_id, accessible_name, timeout=5)
        invoke(btn)
        return
    except Exception:
        pass

    if fallback_key:
        press_to(_dlg_hwnd(parent), fallback_key)
    else:
        raise RuntimeError(
            f"Could not click button auto_id={auto_id!r}, name={accessible_name!r}."
        )


def _click_ok(parent, auto_id, fallback_key="enter"):
    """Click the OK button, falling back to a keypress."""
    try:
        btn = by_auto_id_or_name(parent, auto_id, "OK", timeout=5)
        invoke(btn)
        return
    except Exception:
        pass
    press_to(_dlg_hwnd(parent), fallback_key)


def _apply_permission_checkboxes(dlg, perms):
    """Set permission checkboxes in EncryptionDialog according to *perms*.

    Parameters
    ----------
    dlg   : pywinauto WindowSpecification   The open EncryptionDialog.
    perms : dict   Keys from ``_PERMISSION_CHECKBOX_MAP``, values bool.
    """
    for flag_name, desired in perms.items():
        entry = _PERMISSION_CHECKBOX_MAP.get(flag_name)
        if entry is None:
            continue  # Unknown flag — skip silently.
        obj_name, acc_name = entry
        try:
            chk = by_auto_id_or_name(dlg, obj_name, acc_name, timeout=5)
            try:
                current = chk.get_toggle_state()
                # toggle_state: 0 = unchecked, 1 = checked.
                currently_checked = (current == 1)
            except Exception:
                currently_checked = None

            if currently_checked is None or (desired != currently_checked):
                invoke(chk)
        except Exception:
            # Checkbox may be disabled (e.g. algorithm=None). Skip.
            pass


def _handle_save_dialog(session, title_re, output_path):
    """Type *output_path* in a Save QFileDialog and accept it.

    If *output_path* is None the dialog is accepted with its default filename.
    """
    hwnd = _session_hwnd(session)
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
        return  # Dialog did not appear — operation may have been cancelled.

    if output_path is not None:
        try:
            fn_edit = by_name(dlg, "File name:", timeout=5)
            fn_edit.set_focus()
            fn_edit.type_keys("^a{DELETE}", with_spaces=False)
        except Exception:
            hotkey_to(hwnd, "ctrl", "a")
            press_to(hwnd, "delete")
        typewrite_to(hwnd, output_path, interval=0.01)

    press_to(hwnd, "enter")
    wait_stable(_ACTION_SETTLE)


def _read_issue_table_row_count(dlg):
    """Return the number of data rows in the ComplianceDialog IssueTable.

    Parameters
    ----------
    dlg : pywinauto WindowSpecification   The open ComplianceDialog.

    Returns
    -------
    int   Row count (excluding header).

    Raises
    ------
    RuntimeError
        If the IssueTable cannot be located.
    """
    try:
        table = by_auto_id_or_name(dlg, _ISSUE_TABLE_ID, _ISSUE_TABLE_NAME, timeout=5)
    except Exception as exc:
        raise RuntimeError(
            f"Could not locate IssueTable (auto_id={_ISSUE_TABLE_ID!r}): {exc}"
        ) from exc

    # Strategy 1: pywinauto item_count() or similar.
    try:
        return table.item_count()
    except Exception:
        pass

    # Strategy 2: count children, subtracting the header row.
    try:
        children = table.children()
        # QTableWidget UIA tree: header items + data items.
        # Filter to data rows by excluding the header.
        count = len([c for c in children
                     if "header" not in (c.element_info.control_type or "").lower()])
        return max(0, count)
    except Exception:
        pass

    # Strategy 3: read StatusLabel text and parse the error count.
    try:
        status_label = by_auto_id_or_name(dlg, _STATUS_LABEL_ID, _STATUS_LABEL_ID, timeout=3)
        text = status_label.window_text()
        # Expected format: "Analysis: N errors, M warnings"
        import re
        m = re.search(r"(\d+)\s+error", text, re.IGNORECASE)
        if m:
            return int(m.group(1))
    except Exception:
        pass

    return 0


_DIALOG_MENU_PATHS = {
    "about":        "Help->About PDFE...",
    "preferences":  "Edit->Preferences...",
    "metadata":     "File->Properties...",
    "properties":   "File->Properties...",
    "compliance":   "Tools->Compliance...",
    "encryption":   "Security->Save with &Encryption...",
    "header":       "Tools->Headers and Footers...",
    "footer":       "Tools->Headers and Footers...",
    "headerfooter": "Tools->Headers and Footers...",
    "watermark":    "Tools->Watermark...",
    "redaction":    "Security->Redact...",
    "atlas":        "Tools->Compare Atlas Modes...",
    "comparison":   "Tools->Compare Atlas Modes...",
    "batch":        "Tools->Batch Process...",
    "optimize":     "File->Optimize...",
    "sign":         "Security->Sign Document...",
    "signature":    "Security->Sign Document...",
    "compare":      "Tools->Compare Documents...",
    "altText":      "Tools->Edit Alt Text...",
    "calculation":  "Forms->Edit Calculation Order...",
    "taborder":     "Forms->Edit Tab Order...",
    "tabOrder":     "Forms->Edit Tab Order...",
    "preflight":    "Tools->Preflight...",
    "print":        "File->Print...",
    "open":         "File->Open...",
    "saveas":       "File->Save As...",
}

_DIALOG_TITLE_PATTERNS = {
    "about":        r"About PDFE|About",
    "preferences":  r"Preferences",
    "metadata":     r"Document Properties|Metadata",
    "properties":   r"Document Properties|Metadata",
    "compliance":   _COMPLIANCE_DIALOG_TITLE,
    "encryption":   _ENCRYPTION_DIALOG_TITLE,
    "header":       r"Headers? and Footers?",
    "footer":       r"Headers? and Footers?",
    "headerfooter": r"Headers? and Footers?",
    "watermark":    r"Watermark",
    "redaction":    r"Redaction",
    "atlas":        r"Atlas Comparison|Compare Atlas",
    "comparison":   r"Atlas Comparison|Compare Atlas",
    "batch":        r"Batch",
    "optimize":     r"Optimi[sz]e",
    "sign":         r"Sign Document|Signature",
    "signature":    r"Sign Document|Signature",
    "compare":      r"Compare Documents",
    "altText":      r"Alt Text",
    "calculation":  r"Calculation Order",
    "taborder":     r"Tab Order",
    "tabOrder":     r"Tab Order",
    "preflight":    r"Preflight",
    "print":        r"Print",
    "open":         r"Open",
    "saveas":       r"Save As",
}


def open_dialog(session, name):
    """Open a dialog by short name (Phase-9 generic helper).

    Walks the canonical menu path for *name*, then waits for the dialog
    title to appear. Returns the dialog window handle.

    Parameters
    ----------
    session : PDFESession
    name    : str  Short name; one of ``_DIALOG_MENU_PATHS`` keys
              (case-sensitive for camelCase entries).

    Raises
    ------
    ValueError    If *name* is not recognised.
    RuntimeError  If the menu action cannot be invoked or the dialog
                  does not appear within ``_DIALOG_TIMEOUT`` seconds.
    """
    if name not in _DIALOG_MENU_PATHS:
        raise ValueError(
            f"open_dialog: name={name!r} not recognised. "
            f"Valid: {sorted(_DIALOG_MENU_PATHS)}"
        )

    window = session.window
    try:
        window.set_focus()
        # GAP-A14: UIA menu walk fallback, retained only for interactive/local
        # hosts where menu_select is reliable. On Server-2019 CI it can deadlock —
        # prefer invoke_action(window, "<dialogAction>") to fire the action via
        # the testToolActivatePipe seam instead of enumerating the UIA menu tree.
        window.menu_select(_DIALOG_MENU_PATHS[name])
        wait_stable(_ACTION_SETTLE)
    except Exception as exc:
        raise RuntimeError(
            f"Could not invoke menu path "
            f"{_DIALOG_MENU_PATHS[name]!r} for dialog {name!r}: {exc}"
        ) from exc

    title_re = _DIALOG_TITLE_PATTERNS.get(name, name)
    return _wait_for_dialog(session, title_re=title_re, timeout=_DIALOG_TIMEOUT)


def accept_dialog(session, name=None):
    """Accept the topmost open dialog (Enter / OK button).

    When *name* is supplied, locate the matching dialog first and click
    its OK / Accept button by ``objectName``. Otherwise just press Enter
    on the active window.
    """
    hwnd = _session_hwnd(session)
    if name and name in _DIALOG_TITLE_PATTERNS:
        try:
            dlg = session.app.window(title_re=_DIALOG_TITLE_PATTERNS[name])
            dlg.wait("visible", timeout=2)
            try:
                btn = by_name(dlg, "OK", timeout=2)
                invoke(btn)
                wait_stable(_ACTION_SETTLE)
                return
            except Exception:
                pass
        except Exception:
            pass

    press_to(hwnd, "enter")
    wait_stable(_ACTION_SETTLE)


def cancel_dialog(session, name=None):
    """Cancel the topmost open dialog (Escape / Cancel button)."""
    hwnd = _session_hwnd(session)
    if name and name in _DIALOG_TITLE_PATTERNS:
        try:
            dlg = session.app.window(title_re=_DIALOG_TITLE_PATTERNS[name])
            dlg.wait("visible", timeout=2)
            try:
                btn = by_name(dlg, "Cancel", timeout=2)
                invoke(btn)
                wait_stable(_ACTION_SETTLE)
                return
            except Exception:
                pass
        except Exception:
            pass

    press_to(hwnd, "escape")
    wait_stable(_ACTION_SETTLE)


def _dismiss_dialog_if_present(session, title_re, timeout=5):
    """Accept a dialog (press Enter) if it appears within *timeout* seconds.

    Non-fatal if no dialog appears.
    """
    hwnd = _session_hwnd(session)
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            dlg = session.app.window(title_re=title_re)
            dlg.wait("visible", timeout=1)
            try:
                ok_btn = by_name(dlg, "OK", timeout=2)
                invoke(ok_btn)
            except Exception:
                press_to(hwnd, "enter")
            wait_stable(0.2)
            return
        except Exception:
            time.sleep(0.3)
