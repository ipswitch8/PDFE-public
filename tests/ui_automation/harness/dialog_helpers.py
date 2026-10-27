# -*- coding: utf-8 -*-
"""Dialog lookup and widget search helpers.

Qt's UIA backend exposes ``QObject::objectName()`` as the UIA
``AutomationId`` — but the id is **hierarchical**: the name on the
widget is prefixed by the chain of ancestor auto_ids up to the
application. So a widget whose ``objectName`` is ``okButton`` may
be reachable via auto_id
``QApplication.mainWindow.aboutDialog.okButton`` or
``QApplication.mainWindow.encryptionDialog.buttonBox.okButton``
depending on where it lives.

Test code should therefore match by **suffix**, not exact string.
This module provides:

- :func:`find_by_suffix` — walk descendants of a root element and
  return the first control whose automation_id ends with the
  requested suffix (or equals it exactly).
- :func:`find_dialog_window` — find a top-level dialog window whose
  auto_id matches a target dialog name (e.g. ``"aboutDialog"``).
- :func:`wait_for_dialog` — block until a dialog window appears.
- :func:`assert_widget_exists` — convenience for reachability tests
  that just need a pass/fail against a suffix.
"""
from __future__ import annotations

import time
from typing import Optional


def find_by_suffix(root, object_name: str):
    """Find the first descendant whose auto_id ends with a suffix.

    Qt UIA auto_ids are hierarchical (``.``-separated). Matching by
    ``endswith("." + object_name)`` or exact equality reliably
    picks the widget regardless of its ancestor path.

    :param root: Any pywinauto wrapper that supports ``descendants()``.
    :param object_name: The Qt object name to find, without prefix.
    :returns: The first matching descendant, or ``None``.
    """
    suffix = "." + object_name

    # Qt's UIA bridge exposes most widgets' AutomationId from objectName, but
    # ITEM VIEWS expose it from the accessible Name instead (GAP-A15, and see
    # tests/ui_harness/core/locate.py for the measured table).
    #
    # That used to be invisible here because those views had their
    # accessibleName SET EQUAL to their objectName, so the `nm == object_name`
    # arm below matched. GitHub #185 made those names user-facing -- a screen
    # reader announcing "currentList" is a real defect -- so the alias is now
    # the only thing that resolves them. Taken from the one shared table
    # rather than duplicated.
    try:
        from tests.ui_harness.core.locate import item_view_alias
        alias = item_view_alias(object_name)
    except Exception:  # noqa: BLE001
        alias = None

    # Two passes rather than one. A single first-match-wins loop returns the
    # view's ROW, whose AutomationId genuinely does end in ".availableList",
    # in preference to the view itself -- callers then get a ListItem where
    # they asked for a List, and an EMPTY list matches nothing at all, which
    # is exactly how `currentList` came back "missing" while on screen.
    fallback = None
    for d in root.descendants():
        try:
            info = d.element_info
            aid = info.automation_id or ""
        except Exception:  # noqa: BLE001
            aid, info = "", None
        if aid == object_name:
            return d
        if alias and aid == alias:
            return d
        try:
            nm = (info.name or "") if info is not None else ""
        except Exception:  # noqa: BLE001
            nm = ""
        if alias and nm == alias and not aid:
            return d
        if fallback is None and (aid.endswith(suffix)
                                 or nm == object_name
                                 or nm.endswith(suffix)):
            fallback = d
    return fallback


def find_dialog_window(app, dialog_name: str, timeout_s: float = 3.0):
    """Find a top-level dialog window across an application.

    Polls ``app.windows()`` until one with a matching auto_id
    suffix appears, or the timeout expires.

    :param app: pywinauto ``Application`` instance.
    :param dialog_name: Expected object name, e.g. ``"aboutDialog"``.
    :param timeout_s: How long to poll before giving up.
    :returns: The matching window wrapper, or ``None`` on timeout.
    """
    deadline = time.time() + timeout_s
    suffix = "." + dialog_name
    while time.time() < deadline:
        try:
            for w in app.windows():
                try:
                    aid = w.element_info.automation_id or ""
                except Exception:  # noqa: BLE001
                    continue
                if aid == dialog_name or aid.endswith(suffix):
                    return w
        except Exception:  # noqa: BLE001
            pass
        time.sleep(0.1)
    return None


def wait_for_dialog(app, dialog_name: str, timeout_s: float = 5.0):
    """Block until a dialog window appears; raise on timeout."""
    dlg = find_dialog_window(app, dialog_name, timeout_s=timeout_s)
    if dlg is None:
        raise TimeoutError(
            f"Dialog {dialog_name!r} did not appear within "
            f"{timeout_s}s"
        )
    return dlg


def close_dialog_via_button(dlg, button_name: Optional[str] = None) -> None:
    """Click the OK / Close button of a dialog to dismiss it.

    Tries, in order:

    1. A child whose auto_id ends with the supplied ``button_name``,
       if it is enabled.
    2. ``okButton``, ``closeButton``, then ``cancelButton`` — each
       only if enabled.
    3. Escape key on the dialog.
    4. Escape key on the dialog's top-level window (Win32 fallback).

    Pipeline f3c91a2e phase-3 Cluster #1B: previously, a click was
    attempted on the first matching button regardless of enable state
    and the function returned without trying further options. Several
    dialogs (e.g. BatchDialog) leave ``cancelButton`` disabled until a
    run is active, so clicking it was a silent no-op and the dialog
    stayed open. Now we skip disabled buttons and always fall through
    to ESC if no enabled button works.

    :param dlg: pywinauto dialog window wrapper.
    :param button_name: Optional preferred button suffix.
    """
    candidates = []
    if button_name:
        candidates.append(button_name)
    for fallback in ("okButton", "closeButton", "cancelButton"):
        if fallback not in candidates:
            candidates.append(fallback)

    for name in candidates:
        btn = find_by_suffix(dlg, name)
        if btn is None:
            continue
        try:
            if not btn.is_enabled():
                continue
        except Exception:  # noqa: BLE001
            # If is_enabled() can't be queried, attempt the click anyway.
            pass
        try:
            btn.click_input()
            return
        except Exception:  # noqa: BLE001
            continue

    # Last resort: ESC on the dialog wrapper.
    try:
        dlg.set_focus()
    except Exception:  # noqa: BLE001
        pass
    try:
        dlg.type_keys("{ESC}")
        return
    except Exception:  # noqa: BLE001
        pass
    # Win32 fallback for cases where pywinauto's type_keys cannot
    # acquire focus on the dialog HWND (Qt6 modals).
    try:
        import ctypes
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        # VK_ESCAPE = 0x1B; KEYEVENTF_KEYUP = 0x0002
        user32.keybd_event(0x1B, 0, 0, 0)
        user32.keybd_event(0x1B, 0, 0x0002, 0)
    except Exception:  # noqa: BLE001
        pass


def assert_widget_exists(root, object_name: str) -> bool:
    """Return True iff a widget with the given object name is reachable.

    Used by phase-3 dialog reachability tests. Each test loops over
    the matrix's expected widget list and calls this helper for each.
    """
    return find_by_suffix(root, object_name) is not None


def select_combo_item(combo, label_prefix: str,
                      parent=None, timeout_s: float = 3.0) -> bool:
    """Select a QComboBox item by clicking the matching ListItem.

    **Why this exists** (Pipeline I phase-3 finding): UIA's
    ``SelectionItem.Select`` pattern (what pywinauto ``combo.select()``
    uses) updates UIA selection state but does NOT update Qt's
    ``QComboBox::currentIndex``. The combo's UIA tree shows the
    selection as changed, but Qt continues to read the original
    default value when the connected slot fires. This produced
    quiet false-positives in compliance tests where selecting
    ``"PDF/A-2b (Archival, Visual)"`` left Qt thinking PDF/A-1b
    was selected, and the resulting output was PDF/A-1b regardless
    of what the test asked for.

    The reliable path: open the dropdown, find the ListItem whose
    Name starts with ``label_prefix``, and ``click_input()`` it.
    The mouse-down event goes through Qt's normal model-update
    path so ``currentIndex`` actually changes.

    :param combo: pywinauto wrapper for the QComboBox.
    :param label_prefix: Text the target item should *start with*
                         (so callers can pass a unique prefix
                         instead of the full display string).
    :param parent: Optional pywinauto wrapper to search for
                   ListItems. Some Qt UIA bridges expose the popup's
                   ListItems under the dialog or main window rather
                   than under the combo itself; pass the dialog
                   wrapper here as a fallback. Defaults to
                   ``combo``.
    :param timeout_s: Total time to wait for the popup ListItems
                      to appear after opening the dropdown.
    :returns: True on success, False otherwise.
    """
    try:
        combo.set_focus()
    except Exception:  # noqa: BLE001
        pass
    time.sleep(0.2)
    # Open popup. Try Alt+Down first; fall back to clicking the
    # combo (some Qt builds don't honor Alt+Down via UIA SendInput
    # when the focus race with set_focus() loses).
    opened = False
    for opener in ("%{DOWN}", None):
        try:
            if opener is None:
                combo.click_input()
            else:
                combo.type_keys(opener)
            opened = True
        except Exception:  # noqa: BLE001
            continue
        time.sleep(0.4)
        # Check if items appeared anywhere — if so, stop trying openers.
        try:
            if combo.descendants(control_type="ListItem"):
                break
        except Exception:  # noqa: BLE001
            pass
    if not opened:
        return False

    # The Qt combo popup is typically a separate top-level transient
    # window owned by the same process — NOT a descendant of either
    # the combo or the parent dialog. Enumerate Desktop top-levels
    # matching the same PID so we always have a chance to find it.
    desktop_pid = None
    try:
        from pywinauto import Desktop  # type: ignore
        desktop_pid = combo.process_id()
    except Exception:  # noqa: BLE001
        Desktop = None  # type: ignore

    deadline = time.time() + timeout_s
    while time.time() < deadline:
        search_roots = [combo]
        if parent is not None and parent is not combo:
            search_roots.append(parent)
        if Desktop is not None and desktop_pid is not None:
            try:
                for w in Desktop(backend="uia").windows():
                    try:
                        if w.process_id() != desktop_pid:
                            continue
                    except Exception:  # noqa: BLE001
                        continue
                    if w not in search_roots:
                        search_roots.append(w)
            except Exception:  # noqa: BLE001
                pass

        for root in search_roots:
            try:
                items = root.descendants(control_type="ListItem")
            except Exception:  # noqa: BLE001
                continue
            for it in items:
                try:
                    name = (it.element_info.name or "").strip()
                except Exception:  # noqa: BLE001
                    continue
                if name.startswith(label_prefix):
                    try:
                        it.click_input()
                        time.sleep(0.2)
                        return True
                    except Exception:  # noqa: BLE001
                        try:
                            it.invoke()
                            time.sleep(0.2)
                            return True
                        except Exception:  # noqa: BLE001
                            continue
        time.sleep(0.2)

    # Best-effort: close the popup if it stayed open.
    try:
        combo.type_keys("{ESC}")
    except Exception:  # noqa: BLE001
        pass
    return False


def xmp_pdfa_part(pdf_path) -> str:
    """Return ``pdfaid:part`` value embedded in the PDF's XMP, or ``""``.

    Uses pikepdf to bypass false matches in compressed object streams.
    Used by compliance tests to verify that PDF/A conversion actually
    produced the requested level (not just any PDF/A output).
    """
    try:
        import pikepdf  # type: ignore
    except ImportError:
        return ""
    try:
        with pikepdf.open(str(pdf_path)) as pdf:
            md = pdf.Root.get("/Metadata")
            if md is None:
                return ""
            xmp = bytes(md.read_bytes()).decode("utf-8", errors="replace")
        import re
        m = re.search(r"<pdfaid:part>\s*([^<\s]+)\s*</pdfaid:part>", xmp)
        return m.group(1) if m else ""
    except Exception:  # noqa: BLE001
        return ""


def xmp_pdfua_part(pdf_path) -> str:
    """Return ``pdfuaid:part`` value embedded in the PDF's XMP, or ``""``."""
    try:
        import pikepdf  # type: ignore
    except ImportError:
        return ""
    try:
        with pikepdf.open(str(pdf_path)) as pdf:
            md = pdf.Root.get("/Metadata")
            if md is None:
                return ""
            xmp = bytes(md.read_bytes()).decode("utf-8", errors="replace")
        import re
        m = re.search(r"<pdfuaid:part>\s*([^<\s]+)\s*</pdfuaid:part>", xmp)
        return m.group(1) if m else ""
    except Exception:  # noqa: BLE001
        return ""
