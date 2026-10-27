# -*- coding: utf-8 -*-
"""Pywinauto UIA-backed widget locator.

Locates Qt widgets in the live PDFE window by objectName (exposed as
AutomationId in the UIA tree) or by accessibleName (exposed as Name).

All public functions accept a pywinauto WindowSpecification as their
first argument and return a pywinauto control wrapper.

Usage
-----
    from tests.ui_harness.core.locate import by_auto_id, by_name, invoke

    with PDFESession(pdf_path=...) as s:
        btn = by_auto_id(s.window, "searchButton")
        invoke(btn)

Notes
-----
- Qt objectName → UIA AutomationId (accessible via auto_id= in pywinauto).
- Qt accessibleName → UIA Name property (accessible via title= in pywinauto).
- Qt accessibleDescription → UIA HelpText property.
- Timeout defaults allow for lazy-loading panels that only appear after
  being activated (e.g. SignaturePanel renders on first open).
"""

import time
import pywinauto

DEFAULT_TIMEOUT = 10  # seconds


# ---------------------------------------------------------------------------
# Qt ITEM VIEWS DO NOT PUBLISH objectName AS AutomationId (GitHub #185)
# ---------------------------------------------------------------------------
#
# The module docstring's rule -- objectName becomes AutomationId -- holds for
# every widget class in this application EXCEPT the item views. MEASURED on the
# running app, Qt 6.10, 2026-09-15:
#
#   QComboBox     id='QApplication.ComplianceDialog.LevelCombo'   name='Target'
#   QPushButton   id='...ComplianceDialog.AnalyzeButton'          name='Analyze'
#   QLabel        id='...ComplianceDialog.TargetLabel'            name='Target:'
#   QListView     id='Available actions'                          name='Available actions'
#   QTreeWidget   id='Tag Structure Tree'                         name='Tag Structure Tree'
#   QTableWidget  id='Compliance Issues'                          name='Compliance Issues'
#
# For the first three the AutomationId is the objectName chain and the
# accessibleName is irrelevant to it. For the last three the AutomationId IS
# the accessibleName and the objectName does not appear at all -- though the
# view's CHILD ITEMS still carry the objectName chain
# (``...customizeToolbarsDialog.availableList``), which is why a descendant
# walk can look like it found the view when it found a row.
#
# This is not new behaviour and not a consequence of #185. QTreeWidget and
# QTableWidget above have always had differing names, and a comment in
# ``test_ui_compliance_accessibility.py`` recorded the same thing for
# QListWidget. What #185 changed is that four item views whose accessibleName
# had been set EQUAL to their objectName -- which made the quirk invisible --
# now carry proper user-facing text, so their AutomationId moved with it.
#
# Issue #185 states that "every one of these widgets already sets objectName
# correctly on the adjacent line, so automation stability is not at risk".
# That is true for the nine non-item-view sites and NOT true for these four.
# The correct resolution is not to re-break the accessible name: a screen
# reader announcing "availableList" is a real defect and an AutomationId is a
# test-only concern. It is to resolve the alias here, once, where the quirk is
# documented, instead of scattering display strings through the suite.
#
# Keep this table in step with the accessibleName of any ITEM VIEW that tests
# locate by objectName.
_ITEM_VIEW_ID_ALIASES = {
    # objectName                 -> accessibleName, which Qt publishes as the
    #                               item view's AutomationId
    "availableList":   "Available actions",          # CustomizeToolbarsDialog
    "currentList":     "Current toolbar contents",   # CustomizeToolbarsDialog
    "fileList":        "Files to combine",           # CombineFilesWizard
    "IssueTable":      "Compliance Issues",          # ComplianceDialog
    "tagStructureTree": "Tag Structure Tree",        # ComplianceDialog
}


def item_view_alias(auto_id):
    """The AutomationId Qt actually publishes for an item view, or None."""
    return _ITEM_VIEW_ID_ALIASES.get(auto_id)


def by_auto_id(window, auto_id, control_type=None, timeout=DEFAULT_TIMEOUT):
    """Find a widget by its objectName (UIA AutomationId).

    Qt's UIA bridge exposes the objectName as the AutomationId, but it
    prepends the full parent-path, e.g. "QApplication.mainWindow.fileMenu".
    This function accepts either the short name ("fileMenu") or the full
    path and tries both strategies.

    Parameters
    ----------
    window      : pywinauto WindowSpecification
    auto_id     : str   The Qt objectName (short or full path)
    control_type: str or None  e.g. "Button", "Edit", "ComboBox"
    timeout     : int   Seconds to wait for the widget to appear.

    Returns
    -------
    pywinauto control wrapper
    """
    def _try_find(aid):
        kwargs = {"auto_id": aid, "found_index": 0}
        if control_type:
            kwargs["control_type"] = control_type
        ctrl = window.child_window(**kwargs)
        ctrl.wait("exists", timeout=min(timeout, 2))
        return ctrl

    # Strategy 1: exact match (works for full-path IDs or if Qt exposes short names)
    try:
        return _try_find(auto_id)
    except Exception:
        pass

    # Strategy 1b: item-view alias (GitHub #185 -- see _ITEM_VIEW_ID_ALIASES).
    # Tried BEFORE the descendant walk on purpose: the walk's suffix match
    # would otherwise hit one of the view's ROWS, whose AutomationId does end
    # in ".availableList", and hand back a ListItem where a List was asked for.
    alias = item_view_alias(auto_id)
    if alias:
        try:
            return _try_find(alias)
        except Exception:
            pass

    # Strategy 2: suffix match — search all descendants for one whose auto_id
    # ends with ".<auto_id>" (handles the Qt full-path prefix)
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            suffix = "." + auto_id
            all_ctrls = window.descendants()
            for ctrl in all_ctrls:
                try:
                    aid = ctrl.automation_id()
                    if aid == auto_id or (aid and aid.endswith(suffix)):
                        if control_type is None or ctrl.friendly_class_name() == control_type:
                            return ctrl
                except Exception:
                    continue
        except Exception:
            pass
        time.sleep(0.2)

    raise TimeoutError(
        f"Widget with auto_id={auto_id!r} not found after {timeout}s."
    )


def by_name(window, name, control_type=None, timeout=DEFAULT_TIMEOUT):
    """Find a widget by its accessibleName (UIA Name property).

    Parameters
    ----------
    window      : pywinauto WindowSpecification
    name        : str   The Qt accessibleName set via widget->setAccessibleName(...)
    control_type: str or None
    timeout     : int

    Returns
    -------
    pywinauto control wrapper
    """
    kwargs = {"title": name, "found_index": 0}
    if control_type:
        kwargs["control_type"] = control_type
    ctrl = window.child_window(**kwargs)
    ctrl.wait("exists enabled visible", timeout=timeout)
    return ctrl


def by_auto_id_or_name(window, auto_id, name, control_type=None,
                       timeout=DEFAULT_TIMEOUT):
    """Try auto_id first; fall back to name if not found.

    Useful when automation IDs have not yet been added but accessible names exist.
    """
    try:
        return by_auto_id(window, auto_id, control_type, timeout=min(timeout, 2))
    except Exception:  # noqa: BLE001
        return by_name(window, name, control_type, timeout=timeout)


def invoke(ctrl):
    """Click a control via its InvokePattern (preferred) or mouse click fallback.

    Returns the control for method chaining.
    """
    try:
        ctrl.invoke()
    except Exception:  # noqa: BLE001
        ctrl.click_input()
    return ctrl


def type_into(ctrl, text, clear_first=True):
    """Set focus on ctrl and type text.

    Parameters
    ----------
    ctrl       : pywinauto control
    text       : str
    clear_first: bool  if True, Ctrl+A then Delete before typing
    """
    ctrl.set_focus()
    if clear_first:
        ctrl.type_keys("^a{DELETE}")
    ctrl.type_keys(text, with_spaces=True)
    return ctrl


def get_value(ctrl):
    """Return the current text / value of a control (best-effort)."""
    try:
        return ctrl.get_value()
    except Exception:  # noqa: BLE001
        pass
    try:
        return ctrl.window_text()
    except Exception:  # noqa: BLE001
        return None


def wait_for_auto_id(window, auto_id, timeout=DEFAULT_TIMEOUT):
    """Block until a widget with auto_id exists and is visible; return it."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            ctrl = window.child_window(auto_id=auto_id, found_index=0)
            ctrl.wait("exists visible", timeout=1)
            return ctrl
        except Exception:  # noqa: BLE001
            time.sleep(0.2)
    raise TimeoutError(
        f"Widget with auto_id={auto_id!r} not visible after {timeout}s."
    )


def is_enabled(ctrl):
    """Return True if the control is enabled (not greyed out)."""
    try:
        return ctrl.is_enabled()
    except Exception:  # noqa: BLE001
        return False


def is_checked(ctrl):
    """Return True if a checkbox/toggle control is checked."""
    try:
        return ctrl.get_toggle_state() == 1
    except Exception:  # noqa: BLE001
        return False


def invoke_action(window, action_object_name, settle_s=1.2,
                  timeout=DEFAULT_TIMEOUT):
    """Trigger a MainWindow QAction by objectName via the testToolActivatePipe
    seam — the GAP-A14-safe alternative to :func:`menu_path` / ``menu_select``.

    ``menu_select`` drives a UIA menu-bar walk (IUIAutomation FindAll over the
    QMenu popup tree) that DEADLOCKS on the Windows-Server-2019 CI session when
    PDFE's Win32 message pump is busy painting tiles. The test build instead
    exposes a hidden, zero-size ``testToolActivatePipe`` QLineEdit (gated by the
    ``PDFE_TEST_PIPES`` env var) whose ``textChanged`` handler calls
    ``MainWindow::activateToolByObjectName()``. That method triggers ANY QAction
    parented to the window — menu OR toolbar — by objectName, deferred via
    ``QTimer::singleShot(0)`` so a modal dialog opened by the action does not
    block the pipe write. One seam therefore covers every domain
    (e.g. ``"complianceAction"``, ``"saveWithEncryptionAction"``,
    ``"signDocumentAction"``, ``"toggleSearchPanelAction"``,
    ``"insertPagesFromFileAction"``).

    Prefer this over :func:`menu_path` for firing actions. Requires the binary
    to have been launched with ``PDFE_TEST_PIPES=1``.

    Returns True once the pipe write has been dispatched.
    """
    pipe = by_auto_id(window, "testToolActivatePipe", control_type="Edit",
                      timeout=timeout)
    pipe.set_edit_text(action_object_name)
    time.sleep(settle_s)
    return True


def menu_path(window, path):
    """Invoke a menu action by path string, e.g. 'File->Open'.

    GAP-A14: this uses pywinauto's ``menu_select``, which drives a UIA menu walk
    that can DEADLOCK on the Windows-Server-2019 CI session. It is retained only
    for (a) interactive/local hosts where the UIA menu walk is reliable, and
    (b) tests whose purpose IS to exercise the menu bar itself. For merely
    *firing* an action, prefer :func:`invoke_action`, which writes to the
    testToolActivatePipe seam and never enumerates the UIA menu tree.
    """
    window.menu_select(path)
