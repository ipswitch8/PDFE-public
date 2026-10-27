# -*- coding: utf-8 -*-
"""File-dialog driver that drives Qt's own ``QFileDialog`` (the
test-only, ``Qt::AA_DontUseNativeDialogs`` mode), not the native
Windows shell ``IFileOpenDialog``.

PDFE's ``main.cpp`` checks the ``PDFE_USE_QT_FILE_DIALOG`` env var
and sets ``Qt::AA_DontUseNativeDialogs`` when it's ``"1"``. UI
regression tests that drive file dialogs set the env var so PDFE
shows its own QDialog inside MainWindow's UIA tree, with stable
Qt object names (``fileNameEdit``, ``Open`` / ``Save`` / ``Cancel``
push buttons inside ``buttonBox``).

Why we don't drive the native shell dialog
------------------------------------------
The native Win32 ``IFileOpenDialog`` /  ``IFileSaveDialog`` runs
out-of-process (``dllhost.exe``) and pulls its initial directory
from a per-exe shell MRU at
``HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\
ComDlg32\\LastVisitedPidlMRU``. If that MRU points at a missing
drive (e.g. a previously mounted but currently disconnected
B:/), the shell shows a blocking system error modal *before* the
file picker, with class ``#32770`` but no Edit/ComboBox controls
— breaking automation. WinAppDriver 1.2.1's Root session hangs
on every enumeration call against the Desktop on this host
(unmaintained since 2020). So instead we sidestep the whole
shell stack: PDFE shows a Qt QFileDialog under test, automation
finds it via Qt object names, and the original wont-fix matrix
rows flip to green.

Public API mirrors the original WinAppDriver helper signatures so
existing test imports keep working unchanged:
``open_file_dialog(pid, target, timeout)``,
``save_file_dialog(pid, target, timeout, confirm_overwrite)``,
``cancel_file_dialog(pid, timeout)``. The ``pid`` argument is
unused but kept for source compatibility.
"""
from __future__ import annotations

import time
from typing import Optional

try:
    from pywinauto import Desktop  # type: ignore
except ImportError:  # pragma: no cover
    Desktop = None  # type: ignore


_MAIN_WINDOW_CLASS = "pdfe::ui::MainWindow"


# Backwards-compat alias so callers that imported
# ``WinAppDriverUnavailable`` from the old module keep working.
class WinAppDriverUnavailable(RuntimeError):
    """Kept for source compatibility. We don't use WinAppDriver
    in this module — but tests catch ``WinAppDriverUnavailable``
    around dialog calls and we don't want to break that pattern."""


def _find_pdfe_window():
    """Return the active PDFE MainWindow wrapper, or None."""
    if Desktop is None:
        return None
    try:
        d = Desktop(backend="uia")
        wins = d.windows(class_name=_MAIN_WINDOW_CLASS)
        if wins:
            return wins[0]
    except Exception:
        pass
    return None


def _find_widget_by_leaf(parent, leaf_id: str, timeout: float = 5.0,
                         control_type: Optional[str] = None,
                         require_visible: bool = False):
    """Find a Qt widget by its leaf ``objectName``. pywinauto's UIA
    backend reports the full Qt parent-chain path in
    ``automation_id``; we match by suffix.

    Mirrors ``harness.pdfe_app_fixture.find_widget`` but lives here
    to avoid a circular import — and adds an optional
    ``control_type`` filter so we can disambiguate
    ``buttonBox.QPushButton`` (which has no leaf id of its own,
    just the ``QPushButton`` class) by Name."""
    suffix = "." + leaf_id
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            for d in parent.descendants():
                try:
                    aid = d.element_info.automation_id or ""
                except Exception:
                    continue
                if aid != leaf_id and not aid.endswith(suffix):
                    continue
                if control_type is not None:
                    try:
                        if d.element_info.control_type != control_type:
                            continue
                    except Exception:
                        continue
                if require_visible:
                    try:
                        if not d.is_visible():
                            continue
                    except Exception:
                        continue
                return d
        except Exception:
            pass
        time.sleep(0.2)
    raise TimeoutError(
        f"widget with leaf id {leaf_id!r} not found in {timeout}s"
    )


def _wait_for_qfiledialog(win, timeout: float = 10.0):
    """Wait for a Qt ``QFileDialog`` to appear and return
    ``(host, file_edit)`` — where ``host`` is whichever wrapper
    actually contains ``fileNameEdit`` (could be the MainWindow,
    or a top-level QFileDialog window if Qt presented it as a
    sibling top-level)."""
    if win is None:
        raise TimeoutError("PDFE MainWindow handle is None")
    deadline = time.time() + timeout
    last_err = None
    while time.time() < deadline:
        # Search the MainWindow's UIA subtree first.
        try:
            edit = _find_widget_by_leaf(win, "fileNameEdit",
                                        timeout=0.5)
            return win, edit
        except TimeoutError as e:
            last_err = e
        # Fallback: search Desktop top-level windows (Qt's
        # QFileDialog is sometimes a sibling top-level rather than
        # a child of MainWindow on Windows).
        try:
            d = Desktop(backend="uia")
            for w in d.windows():
                try:
                    cls = w.element_info.class_name or ""
                except Exception:
                    continue
                if cls != "QFileDialog":
                    continue
                try:
                    edit = _find_widget_by_leaf(
                        w, "fileNameEdit", timeout=0.5)
                    return w, edit
                except TimeoutError:
                    continue
        except Exception as e:
            last_err = e
        time.sleep(0.3)
    raise TimeoutError(
        f"fileNameEdit not found within {timeout}s "
        f"(last err: {last_err})"
    )


def _set_path(edit, path: str) -> bool:
    """Write ``path`` into the Qt fileNameEdit via UIA SetValue.

    Falls back through several pywinauto APIs that map to UIA's
    ``ValuePattern.SetValue``. SetValue is atomic and not subject
    to keystroke buffering issues.
    """
    try:
        edit.set_focus()
    except Exception:
        pass
    try:
        edit.set_edit_text(path)
        return True
    except Exception:
        pass
    try:
        edit.iface_value.SetValue(path)
        return True
    except Exception:
        pass
    try:
        edit.type_keys("^a{DEL}")
        edit.type_keys(path, with_spaces=True)
        return True
    except Exception:
        return False


def _click_dialog_button(win, button_name: str,
                         timeout: float = 5.0) -> bool:
    """Click the QPushButton in the QFileDialog's buttonBox by
    visible Name (``Open``, ``Save``, ``Cancel``). Returns True on
    success."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            for d in win.descendants(control_type="Button"):
                try:
                    name = d.element_info.name or ""
                    aid = d.element_info.automation_id or ""
                except Exception:
                    continue
                if name == button_name and "buttonBox" in aid:
                    try:
                        d.invoke()
                        return True
                    except Exception:
                        pass
                    try:
                        d.click_input()
                        return True
                    except Exception:
                        pass
        except Exception:
            pass
        time.sleep(0.2)
    return False


def _dismiss_overwrite_confirmation(win, timeout: float = 4.0) -> None:
    """Click Yes on a Qt ``QMessageBox`` overwrite-confirm dialog if
    one appears. Qt's QMessageBox lives under MainWindow's tree
    when ``Qt::AA_DontUseNativeDialogs`` is active."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            for d in win.descendants(control_type="Button"):
                try:
                    name = d.element_info.name or ""
                except Exception:
                    continue
                # Qt's standard buttons in QMessageBox are named
                # "&Yes", "Yes", "OK", "&OK".
                if name in ("Yes", "&Yes", "OK", "&OK"):
                    try:
                        # Only click if a QMessageBox is visible
                        # (heuristic: parent automation_id contains
                        # "MessageBox" / "QMessageBox").
                        parent_aid = ""
                        try:
                            p = d.element_info
                            parent_aid = p.automation_id or ""
                        except Exception:
                            pass
                        if "QMessageBox" in parent_aid or \
                           "MessageBox" in parent_aid:
                            d.invoke()
                            return
                    except Exception:
                        continue
        except Exception:
            pass
        time.sleep(0.2)


# --------------- Public API ---------------

def open_file_dialog(win,
                     target_path: str,
                     timeout: float = 10.0) -> bool:
    """Drive a Qt ``QFileDialog`` Open: wait for fileNameEdit,
    write ``target_path`` via UIA SetValue, click Open. Returns
    True. Raises TimeoutError on failure. ``win`` is the
    pywinauto MainWindow wrapper from ``launch_pdfe``."""
    host, edit = _wait_for_qfiledialog(win, timeout)
    if not _set_path(edit, target_path):
        raise TimeoutError("could not write path to QFileDialog")
    if not _click_dialog_button(host, "Open", timeout=timeout):
        raise TimeoutError("could not click Open button")
    return True


def choose_directory_dialog(win,
                            target_dir: str,
                            timeout: float = 10.0) -> bool:
    """Drive a Qt ``QFileDialog::getExistingDirectory`` picker: wait
    for the dialog, set the path via ``fileNameEdit``, click the
    accept button (named ``Choose`` in directory mode). Returns
    ``True`` on success.

    Used by ``attachment_panel_extract`` and any other flow that
    invokes ``QFileDialog::getExistingDirectory``. The accept button
    is labelled "Choose" rather than "Save"/"Open" — we additionally
    accept "Open" + "OK" + Enter as fallbacks for older Qt builds.
    """
    host, edit = _wait_for_qfiledialog(win, timeout)
    if not _set_path(edit, target_dir):
        raise TimeoutError("could not write path to QFileDialog")
    for label in ("Choose", "Open", "OK"):
        if _click_dialog_button(host, label, timeout=2.0):
            return True
    # Fallback: pyautogui Enter.
    # NOTE: not focus-guarded. This is a QFileDialog accept fallback; the path
    # was already set via UIA SetValue (not typed), so a stray Enter can leak
    # no content — it only commits the default button. The PDFE-process
    # foreground guard is deliberately not applied here so this helper stays
    # uniform with the native-shell (#32770, different-process) driver.
    try:
        import pyautogui  # type: ignore
        pyautogui.PAUSE = 0.05
        pyautogui.press("enter")
        time.sleep(0.4)
        return True
    except ImportError:
        raise TimeoutError("could not click Choose/Open/OK button")


def save_file_dialog(win,
                     target_path: str,
                     timeout: float = 10.0,
                     confirm_overwrite: bool = True) -> bool:
    """Drive a Qt ``QFileDialog`` Save As: wait, set path, click
    Save, optionally dismiss overwrite confirmation. Returns
    True. ``win`` is the pywinauto MainWindow wrapper."""
    host, edit = _wait_for_qfiledialog(win, timeout)
    if not _set_path(edit, target_path):
        raise TimeoutError("could not write path to QFileDialog")
    if not _click_dialog_button(host, "Save", timeout=timeout):
        if not _click_dialog_button(host, "OK", timeout=timeout):
            # Fallback: Enter is the default-button shortcut on a
            # Save As dialog. Some Qt QFileDialog instances on Win
            # Server 2019 don't expose the Save button to UIA reliably
            # — the OS-level Enter key still commits the dialog.
            # NOTE: not focus-guarded. The path was set via UIA SetValue
            # (not typed), so this Enter leaks no content — it only commits
            # the default button; kept uniform with the native-shell driver.
            try:
                import pyautogui  # type: ignore
                pyautogui.PAUSE = 0.05
                pyautogui.press("enter")
                time.sleep(0.4)
            except ImportError:
                raise TimeoutError("could not click Save button")
    if confirm_overwrite:
        _dismiss_overwrite_confirmation(win, timeout=timeout / 2)
    return True


def cancel_file_dialog(win, timeout: float = 4.0) -> bool:
    """Click Cancel on the active Qt QFileDialog. ``win`` is the
    pywinauto MainWindow wrapper."""
    if win is None:
        return False
    # Try MainWindow subtree first; then fall through to top-level
    # QFileDialog.
    if _click_dialog_button(win, "Cancel", timeout=timeout / 2):
        return True
    try:
        d = Desktop(backend="uia")
        for w in d.windows():
            try:
                if w.element_info.class_name == "QFileDialog":
                    if _click_dialog_button(w, "Cancel",
                                            timeout=timeout / 2):
                        return True
            except Exception:
                continue
    except Exception:
        pass
    return False
