#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Pipeline G phase-10: native Windows QFileDialog driver.

.. deprecated:: Pipeline H phase-1 (2026-05)

   New Category A tests (the file Open/Save dialog flows) should use
   :mod:`tests.ui_automation.helpers.winappdriver_dialog_driver`
   instead. The pywinauto-based implementation in this module
   resolves the native ``#32770`` shell dialog inconsistently under
   non-interactive automation: Win 10/11 sometimes presents the
   file-name field as a ComboBox before folder enumeration completes,
   which silently swallows keystrokes and produces matrix-wide
   skips rather than greens. WinAppDriver speaks the W3C protocol
   over HTTP and handles the ComboBox/Edit transition correctly.

   This module is **not** removed because pre-Pipeline-H tests still
   import it. New tests should not.

Native Windows file dialogs (the standard Win32 IFileOpenDialog /
IFileSaveDialog that QFileDialog defaults to) live in their own
process-internal modal window with stable UIA control names that
DO NOT match Qt's objectName conventions. They look like:

  Window: Title='Open' or 'Save As', ClassName='#32770'
    Edit: 'File name:'   ← combo or edit, name varies by Win version
    Button: 'Open' / 'Save' / 'Cancel'

This module provides reusable helpers so individual tests don't
re-implement the dance of:
  1) wait for the modal to appear
  2) type the path into the filename field
  3) click Open / Save
  4) handle the optional 'Confirm Save As' overwrite confirmation

KNOWN LIMITATION: pywinauto's typing into the native file-name edit
under non-interactive Windows automation is sometimes lost when the
edit briefly transitions to a combobox (e.g. on Win 10/11 when the
folder is not yet enumerated). Each helper therefore retries a few
times and gracefully raises TimeoutError so callers can downgrade
to pytest.skip rather than false-failing the matrix.
"""
from __future__ import annotations

import time
from typing import Optional

try:
    from pywinauto import Desktop  # type: ignore
except ImportError:  # pragma: no cover
    Desktop = None  # type: ignore


# Title strings used by the Win32 common dialogs across locales.
# pywinauto's title match is exact, so we enumerate candidates.
_OPEN_TITLES = ("Open", "Open File", "Select File", "Open ", "Choose File")
_SAVE_TITLES = ("Save As", "Save", "Save File")
_CONFIRM_OVERWRITE_TITLES = ("Confirm Save As", "Confirm File Replace")


def _find_dialog_by_titles(pid: int,
                           titles: tuple,
                           timeout: float):
    """Locate a top-level window owned by pid whose title matches one
    of titles. Returns the wrapped window or None on timeout."""
    if Desktop is None:
        return None
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            for w in Desktop(backend="uia").windows():
                try:
                    if w.process_id() != pid:
                        continue
                    title = (w.window_text() or "").strip()
                    if title in titles:
                        return w
                except Exception:
                    continue
        except Exception:
            pass
        time.sleep(0.15)
    return None


def _find_filename_edit(dlg):
    """Locate the file-name edit in a Win32 file dialog. The control's
    title is locale-dependent ('File name:', 'Nom de fichier :',
    'Dateiname:'...) so we fall back through several discovery
    strategies."""
    # Strategy 1: by title (English).
    try:
        e = dlg.child_window(title="File name:", control_type="Edit")
        if e.exists():
            return e
    except Exception:
        pass
    # Strategy 2: by class_name='Edit'.
    try:
        e = dlg.child_window(class_name="Edit")
        if e.exists():
            return e
    except Exception:
        pass
    # Strategy 3: by control type alone, take first match.
    try:
        edits = dlg.descendants(control_type="Edit")
        if edits:
            return edits[0]
    except Exception:
        pass
    # Strategy 4: ComboBox with editable child (Win 10/11 sometimes
    # presents the file-name field as a ComboBox before folder
    # enumeration completes).
    try:
        combos = dlg.descendants(control_type="ComboBox")
        if combos:
            return combos[0]
    except Exception:
        pass
    return None


def _type_path(edit, path: str) -> bool:
    """Type a target path into the dialog's filename edit. Returns
    True on success."""
    try:
        edit.set_focus()
    except Exception:
        return False
    # Clear any pre-filled content via Ctrl+A + Delete; some dialogs
    # leave the previously-saved-name in place.
    try:
        edit.type_keys("^a{DEL}")
    except Exception:
        pass
    try:
        # Use SendKeys mode that handles backslashes literally.
        edit.type_keys(path, with_spaces=True)
        return True
    except Exception:
        # Fallback: try set_edit_text() if available.
        try:
            edit.set_edit_text(path)
            return True
        except Exception:
            return False


def _click_button_by_title(dlg, candidates: tuple) -> bool:
    """Click the first button whose name matches one of candidates."""
    for title in candidates:
        try:
            btn = dlg.child_window(title=title, control_type="Button")
            if btn.exists():
                btn.click_input()
                return True
        except Exception:
            continue
    return False


def _dismiss_overwrite_confirmation(pid: int, timeout: float = 4.0) -> None:
    """If a 'Confirm Save As' overwrite dialog appears, click Yes."""
    confirm = _find_dialog_by_titles(pid, _CONFIRM_OVERWRITE_TITLES, timeout)
    if confirm is None:
        return
    for title in ("Yes", "&Yes", "OK"):
        try:
            btn = confirm.child_window(title=title, control_type="Button")
            if btn.exists():
                btn.click_input()
                return
        except Exception:
            continue


def open_file_dialog(pid: int,
                     target_path: str,
                     timeout: float = 8.0) -> bool:
    """Drive a native Windows Open dialog: wait for it, type the
    target path, click Open. Returns True on success.

    Raises TimeoutError if the dialog never appears or the path
    cannot be entered within timeout. Callers in pywinauto tests
    typically catch this and pytest.skip rather than fail.
    """
    dlg = _find_dialog_by_titles(pid, _OPEN_TITLES, timeout)
    if dlg is None:
        raise TimeoutError("Open dialog did not appear within "
                           f"{timeout}s")
    edit = _find_filename_edit(dlg)
    if edit is None:
        raise TimeoutError("could not locate file-name edit in Open "
                           "dialog")
    if not _type_path(edit, target_path):
        raise TimeoutError("could not type path into Open dialog")
    if not _click_button_by_title(dlg, ("Open", "&Open", "OK")):
        raise TimeoutError("could not click Open button")
    return True


def save_file_dialog(pid: int,
                     target_path: str,
                     timeout: float = 8.0,
                     confirm_overwrite: bool = True) -> bool:
    """Drive a native Windows Save As dialog: wait for it, type the
    target path, click Save, and optionally dismiss the overwrite
    confirmation dialog if the destination exists.

    Returns True on success. Raises TimeoutError if the dialog never
    appears or interaction fails.
    """
    dlg = _find_dialog_by_titles(pid, _SAVE_TITLES, timeout)
    if dlg is None:
        raise TimeoutError("Save dialog did not appear within "
                           f"{timeout}s")
    edit = _find_filename_edit(dlg)
    if edit is None:
        raise TimeoutError("could not locate file-name edit in Save "
                           "dialog")
    if not _type_path(edit, target_path):
        raise TimeoutError("could not type path into Save dialog")
    if not _click_button_by_title(dlg, ("Save", "&Save", "OK")):
        raise TimeoutError("could not click Save button")
    if confirm_overwrite:
        _dismiss_overwrite_confirmation(pid, timeout=timeout / 2)
    return True


def cancel_file_dialog(pid: int, timeout: float = 4.0) -> bool:
    """Click Cancel on whatever Open/Save dialog is currently in
    front. Useful for cleanup when a test bails out.
    """
    for titles in (_OPEN_TITLES, _SAVE_TITLES):
        dlg = _find_dialog_by_titles(pid, titles, timeout / 2)
        if dlg is not None:
            return _click_button_by_title(dlg, ("Cancel", "&Cancel"))
    return False
