# -*- coding: utf-8 -*-
"""GitHub #185: what a screen reader ANNOUNCES for the 13 corrected widgets.

WHAT THIS ASSERTS, AND WHY IT IS NOT THE SAME AS THE C++ TEST
-------------------------------------------------------------
``tests/test_compliance_dialog.cpp`` can assert ``widget->accessibleName()``.
That proves the property was set. It does NOT prove the property is the one
Windows hands to assistive technology, and #185 is entirely about the latter:
the defect was that ``accessibleName`` OVERRIDES the window title in the UIA
``Name``, which is invisible from inside Qt.

So this reads ``Name`` and ``AutomationId`` back out of the real UIA tree of a
running ``pdfe.exe``, which is the same channel Narrator and NVDA read, and
asserts:

1. every corrected widget's UIA ``Name`` is the USER-FACING text, and
2. its UIA ``AutomationId`` is still the unchanged ``objectName``.

Point 2 is the load-bearing half. Four of the corrected sites carried a source
comment claiming Qt's UIA bridge takes ``AutomationId`` from ``accessibleName``
and that the two therefore had to be kept equal for the tests to find the
widget. If that were true, #185 could not be fixed without breaking automation.
It is not true, and asserting BOTH properties on the SAME element in the SAME
pass is what settles it — an assertion on ``Name`` alone would leave the claim
standing and the next person would re-align them.

HOW THE DIALOG IS FOUND — READ THIS BEFORE CHANGING IT
-------------------------------------------------------
Never by UIA. pywinauto's UIA backend cannot ENUMERATE a Qt app-modal
``QDialog::exec()`` window; a dialog has sat open for four minutes while a 240 s
UIA poll reported nothing (see ``tests/helpers/modal_dialog.py``). The window is
located with raw Win32 ``EnumWindows`` and only THEN handed to UIA by HWND,
which resolves its subtree normally because no enumeration is involved.

Run with::

    python -m pytest tests/test_ui_accessible_names.py -v
"""

from __future__ import annotations

import os
import sys
import time
from pathlib import Path
from typing import Dict, List, Optional, Tuple

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
from tests.helpers.modal_dialog import (  # noqa: E402
    close_window,
    list_process_windows,
    uia_dialog,
    wait_for_window,
)
from tests.helpers.keys import hotkey_to, press_to  # noqa: E402

FIXTURES = REPO_ROOT / "tests" / "fixtures"
FIX_SINGLE = FIXTURES / "test_single.pdf"


# ---------------------------------------------------------------------------
# Skip conditions (environmental only — a missing desktop is not a pass)
# ---------------------------------------------------------------------------

def _has_pdfe_binary() -> bool:
    return Path(PDFE_EXE).exists()


def _has_pywinauto() -> bool:
    try:
        import pywinauto  # noqa: F401,PLC0415
        return True
    except Exception:  # noqa: BLE001
        return False


def _is_interactive_session() -> bool:
    if os.environ.get("PDFE_UI_FORCE_HEADLESS") == "1":
        return False
    return bool(os.environ.get("SESSIONNAME"))


REQUIRES_LIVE_UI = pytest.mark.skipif(
    not (_has_pdfe_binary() and _has_pywinauto() and _is_interactive_session()),
    reason="Live pdfe.exe + interactive Windows desktop session required.",
)


# ---------------------------------------------------------------------------
# The contract under test.
#
# Each entry: the dialog's window-title substring (for the Win32 lookup), the
# menu path that opens it, and {objectName: expected spoken Name}.
#
# The expected strings are the VISIBLE text a sighted user reads, taken from
# the QLabel / window title beside each widget — which is the whole point of
# #185. They are written out literally rather than derived from the source,
# because a test that recomputed the product's own rule would agree with the
# product no matter what the rule became.
# ---------------------------------------------------------------------------

# ITEM VIEWS ARE THE EXCEPTION, AND IT IS MEASURED, NOT ASSUMED.
#
# Qt 6.10 publishes an item view's AutomationId as its accessibleName, not as
# its objectName chain -- see tests/ui_harness/core/locate.py for the measured
# table. Issue #185 asserts "automation stability is not at risk" for all 13
# sites; that is true for the nine ordinary widgets and FALSE for the four item
# views, whose AutomationId moves with the accessible name. Naming them here
# means the test asserts the rule that actually holds rather than the one the
# issue assumed, and a future Qt that unified the two would REDDEN this rather
# than pass silently.
ITEM_VIEW_OBJECT_NAMES = {
    "availableList", "currentList", "fileList",
    "IssueTable", "tagStructureTree",
}

CORRECTED_WIDGETS: List[Dict[str, object]] = [
    {
        "dialog": "ComplianceDialog",
        "title_substr": "PDF Standards Compliance",
        "action": "complianceAction",
        # The dialog window itself: objectName "ComplianceDialog", spoken name
        # the title. This is the case #185 leads with.
        "window_name": "PDF Standards Compliance",
        "children": {
            "TargetLabel": "Target:",
            "LevelCombo": "Target",
            "IssueLabel": "Issues:",
        },
        # Controls: these widgets' two properties ALREADY differed before #185
        # and the live suite has always located them by auto_id. They are here
        # so that the AutomationId rule is asserted on sites the change did not
        # touch -- otherwise the conclusion would rest only on widgets edited in
        # the same commit as the assertion.
        "control_children": {
            "AnalyzeButton": "Analyze",
            "ValidateButton": "Validate",
            "IssueTable": "Compliance Issues",
        },
    },
    {
        "dialog": "customizeToolbarsDialog",
        "title_substr": "Customize Toolbars",
        "action": "customizeToolbarsAction",
        "window_name": None,
        "children": {
            "availableList": "Available actions",
            "currentList": "Current toolbar contents",
        },
        "control_children": {
            "addActionButton": "Add Action To Toolbar",
        },
    },
    {
        "dialog": "BatchDialog",
        "title_substr": "Batch Processing",
        "action": "batchAction",
        "window_name": None,
        "children": {
            "batchProgressBar": "Batch progress",
        },
        "control_children": {},
    },
    {
        "dialog": "CombineFilesWizard",
        "title_substr": "Combine Files",
        "action": "combineFilesAction",
        "window_name": None,
        "children": {
            "fileList": "Files to combine",
        },
        "control_children": {},
    },
]


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest.fixture(autouse=True)
def _isolate_pdfe_processes():
    """No orphan ``pdfe.exe`` before or after — one holds the machine-global
    single-instance mutex and wedges every later run."""
    kill_pdfe_orphans()
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline and not no_orphans():
        kill_pdfe_orphans()
        time.sleep(0.3)
    time.sleep(1.0)
    yield
    kill_pdfe_orphans()
    deadline = time.monotonic() + 8.0
    while time.monotonic() < deadline and not no_orphans():
        kill_pdfe_orphans()
        time.sleep(0.3)
    assert no_orphans(), "pdfe.exe still running at teardown — orphan leak."


def _settle(seconds: float = 0.6) -> None:
    time.sleep(seconds)


def _resolve_main_hwnd(session: PDFESession) -> int:
    for hwnd, title in list_process_windows(session.pid):
        if FIX_SINGLE.name in title or "PDFE" in title:
            return hwnd
    return int(session.hwnd or 0)


def _open_dialog_via_menu(session: PDFESession,
                          action_object_name: str,
                          title_substr: str,
                          timeout: float = 12.0) -> Optional[int]:
    """Open a dialog from the menu bar and return its HWND, or None.

    Uses the ``testToolActivatePipe`` seam to TRIGGER the action (GAP-A14: a
    UIA menu walk can hang the app), and raw Win32 to OBSERVE the result.
    """
    from tests.helpers.qinvoke import activate_tool_by_name  # noqa: PLC0415

    if not activate_tool_by_name(session, action_object_name):
        return None
    return wait_for_window(session.pid, title_substr, timeout=timeout, poll=0.3)


def _leaf_id(automation_id: str) -> str:
    """The widget's own ``objectName`` out of Qt's dotted AutomationId.

    MEASURED, not assumed. Qt's Windows UIA bridge does not publish a bare
    objectName: the ComplianceDialog window comes back as
    ``"QApplication.ComplianceDialog"`` and its children as
    ``"QApplication.ComplianceDialog.LevelCombo"``. That is the "Qt
    full-path-prefix quirk" ``tests/helpers/pdfe_session.py`` already
    documents on ``find(auto_id=...)``.

    For ORDINARY widgets the AutomationId is an objectName chain and no
    segment of it is an accessibleName: ``LevelCombo``'s accessible name is
    "Target" and the string "Target" appears nowhere in its id. For ITEM VIEWS
    the opposite holds and the id IS the accessible name, with no dot to split
    on — see ``ITEM_VIEW_OBJECT_NAMES`` and the measured table in
    ``tests/ui_harness/core/locate.py``. Both cases are asserted below, keyed
    on which one the widget is, so neither can drift unnoticed.
    """
    return automation_id.rsplit(".", 1)[-1]


# {published AutomationId -> objectName} for the item views, so the property
# map can be keyed on the property the C++ source actually sets. Built from
# CORRECTED_WIDGETS rather than written twice.
_ALIAS_BY_PUBLISHED_ID: Dict[str, str] = {
    str(_txt): str(_obj)
    for _spec in CORRECTED_WIDGETS
    for _group in ("children", "control_children")
    for _obj, _txt in dict(_spec[_group]).items()  # type: ignore[arg-type]
    if _obj in ITEM_VIEW_OBJECT_NAMES
}


def _assert_automation_id_shape(object_name: str, full_id: str,
                                spoken: str) -> None:
    """Assert the AutomationId follows the rule for this widget's CLASS.

    Two rules, both measured against the running app:

    * ordinary widget -> the id is the objectName chain and ENDS in
      ``"." + objectName``. The accessibleName plays no part in it, which is
      the refutation of the f3c91a2e "keep them equal" rationale and the
      reason nine of the thirteen sites could be corrected with no cost at
      all.
    * item view       -> the id IS the accessibleName. The objectName appears
      nowhere in it, so correcting the accessible name DOES move the
      AutomationId. That is the part GitHub #185 gets wrong when it says
      "automation stability is not at risk"; it is resolved by the alias
      table in ``tests/ui_harness/core/locate.py``, not by re-breaking the
      announcement.

    Asserting the item-view case rather than exempting it is deliberate: an
    exemption would let a future Qt change it back unnoticed, and the alias
    table would then be silently wrong.
    """
    if object_name in ITEM_VIEW_OBJECT_NAMES:
        assert full_id == spoken, (
            f"{object_name!r} is an item view, whose AutomationId Qt "
            f"publishes as the accessibleName; got id={full_id!r} "
            f"name={spoken!r}. If these have diverged, Qt's item-view "
            f"behaviour changed and the locate.py alias table is now wrong.")
        assert object_name not in full_id, (
            f"{object_name!r} now appears in its own AutomationId "
            f"{full_id!r} — Qt started publishing objectName for item "
            f"views, which makes the locate.py alias table unnecessary.")
    else:
        assert full_id.endswith("." + object_name), (
            f"{object_name!r} resolved to AutomationId {full_id!r}, which is "
            f"not an objectName chain ending in {object_name!r}.")
        # The decisive comparison, and it is an EQUALITY on the last segment
        # rather than a substring test. A substring test is not sound here:
        # "Analyze" is a prefix of "AnalyzeButton", so `accessibleName not in
        # automation_id` fires on a widget that is behaving perfectly.
        assert _leaf_id(full_id) == object_name, (
            f"{object_name!r} resolved to AutomationId {full_id!r}, whose "
            f"last segment is {_leaf_id(full_id)!r}.")
        assert _leaf_id(full_id) != spoken or spoken == object_name, (
            f"the AutomationId's last segment equals the accessibleName "
            f"{spoken!r} rather than the objectName {object_name!r} — this "
            f"widget follows the ITEM VIEW rule and belongs in "
            f"ITEM_VIEW_OBJECT_NAMES.")


def _uia_property_map(hwnd: int) -> Dict[str, Tuple[str, str]]:
    """``{objectName: (full AutomationId, Name)}`` for the dialog's subtree.

    The dialog is resolved BY HANDLE (see module docstring). Elements with no
    AutomationId are dropped — they cannot be the subject of an assertion
    keyed on objectName.
    """
    dlg = uia_dialog(hwnd)
    out: Dict[str, Tuple[str, str]] = {}
    # Item-view entries are collected separately and merged LAST. An item
    # view's ROWS carry the objectName chain (``...availableList``) while the
    # view itself does not, so a single-pass map keyed on the last segment
    # ends up holding whichever row was enumerated last -- the first run of
    # this test reported availableList's Name as "Redo", which is a list item.
    alias_hits: Dict[str, Tuple[str, str]] = {}
    for el in dlg.descendants():
        try:
            info = el.element_info
            auto_id = info.automation_id or ""
            if not auto_id:
                continue
            entry = (auto_id, info.name or "")
            out.setdefault(_leaf_id(auto_id), entry)
            alias_of = _ALIAS_BY_PUBLISHED_ID.get(auto_id)
            if alias_of:
                alias_hits[alias_of] = entry
        except Exception:  # noqa: BLE001
            # A child that vanished mid-walk is not a finding.
            continue
    out.update(alias_hits)
    return out


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

@REQUIRES_LIVE_UI
@pytest.mark.parametrize(
    "spec", CORRECTED_WIDGETS,
    ids=[str(s["dialog"]) for s in CORRECTED_WIDGETS])
def test_accessible_names_are_user_facing(spec):
    """GitHub #185: the UIA ``Name`` of each corrected widget is the visible
    text, and its ``AutomationId`` is still the untouched ``objectName``.

    Failure modes this distinguishes, because they need different fixes:

    * Name == AutomationId  -> the defect is back: accessibleName was set to
      the developer identifier again.
    * Name == ""            -> accessibleName was REMOVED rather than
      corrected, so the announcement fell back to whatever Qt derives, which
      for a plain QListView is nothing.
    * AutomationId missing  -> objectName was changed while "fixing" the
      accessible name, which breaks every ``auto_id=`` lookup in the suite.
    """
    if not FIX_SINGLE.exists():
        pytest.skip(f"Fixture missing: {FIX_SINGLE}")

    action = spec["action"]
    expected: Dict[str, str] = dict(spec["children"])  # type: ignore[arg-type]
    controls: Dict[str, str] = dict(spec["control_children"])  # type: ignore[arg-type]

    with PDFESession(pdf_path=str(FIX_SINGLE)) as s:
        assert s.wait_for_window_title(FIX_SINGLE.name, timeout=15), (
            "PDFE main window never showed the fixture name; nothing below "
            "can be attributed to the dialog.")
        _settle(1.5)

        hwnd = _open_dialog_via_menu(
            s, str(action), str(spec["title_substr"]))
        assert hwnd, (
            f"{spec['dialog']} never appeared. Win32 EnumWindows saw: "
            f"{[t for _h, t in list_process_windows(s.pid)]!r}. This is a "
            f"harness failure, not a Name assertion — do NOT relax it into a "
            f"skip, the dialog is detectable in under a second when it opens.")

        try:
            props = _uia_property_map(hwnd)

            # The walk must have SEEN something. An empty map would make every
            # assertion below vacuous on a missing key, so it is checked as a
            # measurement-happened guard rather than left implicit.
            assert props, (
                f"{spec['dialog']} resolved by HWND but exposed no UIA "
                f"descendants carrying an AutomationId — the property map is "
                f"empty and nothing below would be measuring anything.")

            window_name = spec.get("window_name")
            if window_name:
                info = uia_dialog(hwnd).element_info
                assert info.name == window_name, (
                    f"GitHub #185: the {spec['dialog']} window announces "
                    f"{info.name!r}; a screen-reader user should hear "
                    f"{window_name!r} — the same string a sighted user reads "
                    f"in the title bar. accessibleName overrides the window "
                    f"title, so an identifier here silently suppresses a "
                    f"perfectly good title.")
                assert _leaf_id(info.automation_id or "") == str(spec["dialog"]), (
                    f"the {spec['dialog']} window's AutomationId is "
                    f"{info.automation_id!r}, whose last segment is not "
                    f"{spec['dialog']!r} — objectName was changed, which "
                    f"breaks auto_id lookups.")

            missing = [k for k in expected if k not in props]
            assert not missing, (
                f"{spec['dialog']}: no UIA element with AutomationId "
                f"{missing!r}. objectName is the AutomationId, so a missing "
                f"key means objectName changed. Present ids: "
                f"{sorted(props)!r}")

            for object_name, visible_text in expected.items():
                full_id, spoken = props[object_name]
                # Both properties asserted on the SAME element in the same
                # pass — that pairing is what settles the f3c91a2e claim, one
                # way or the other, for each widget class.
                _assert_automation_id_shape(object_name, full_id, spoken)
                assert spoken != object_name, (
                    f"GitHub #185 REGRESSED: the widget whose objectName is "
                    f"{object_name!r} announces its own objectName. A screen "
                    f"reader says {object_name!r} where a sighted user reads "
                    f"{visible_text!r}.")
                assert spoken, (
                    f"the widget {object_name!r} announces NOTHING. "
                    f"accessibleName was removed rather than corrected; the "
                    f"expected announcement is {visible_text!r}.")
                assert spoken == visible_text, (
                    f"the widget {object_name!r} announces {spoken!r}; the "
                    f"text a sighted user reads beside it is "
                    f"{visible_text!r}.")

            # The control sites — untouched by #185, asserted to prove the
            # AutomationId/objectName relationship independently of the edit.
            for object_name, visible_text in controls.items():
                assert object_name in props, (
                    f"control site {object_name!r} not found by AutomationId; "
                    f"present ids: {sorted(props)!r}")
                ctrl_full_id, ctrl_spoken = props[object_name]
                _assert_automation_id_shape(object_name, ctrl_full_id,
                                            ctrl_spoken)
                assert ctrl_spoken == visible_text, (
                    f"control site {object_name!r} announces "
                    f"{ctrl_spoken!r}, expected {visible_text!r}")
                assert ctrl_spoken != object_name, (
                    f"control site {object_name!r} is itself an instance of "
                    f"the #185 defect and should have been in scope")
        finally:
            close_window(hwnd)
            _settle(0.8)


@REQUIRES_LIVE_UI
def test_automation_id_follows_object_name_not_accessible_name():
    """GitHub #185: the claim that forced the defect, measured directly.

    Four corrected sites carried a comment asserting Qt6's UIA bridge exposes
    ``AutomationId`` from ``accessibleName``, so the two had to be kept equal.
    This finds a widget whose two properties DIFFER and checks which one the
    AutomationId matches. If the comment were right, this test fails and #185
    is a genuine trade-off rather than a straight fix.
    """
    if not FIX_SINGLE.exists():
        pytest.skip(f"Fixture missing: {FIX_SINGLE}")

    with PDFESession(pdf_path=str(FIX_SINGLE)) as s:
        assert s.wait_for_window_title(FIX_SINGLE.name, timeout=15)
        _settle(1.5)

        hwnd = _open_dialog_via_menu(
            s, "complianceAction", "PDF Standards Compliance")
        assert hwnd, (
            f"ComplianceDialog never appeared; windows: "
            f"{[t for _h, t in list_process_windows(s.pid)]!r}")
        try:
            props = _uia_property_map(hwnd)
            assert "AnalyzeButton" in props, (
                f"AnalyzeButton (objectName) is not an AutomationId in the "
                f"dialog's UIA tree. Present: {sorted(props)!r}. If instead "
                f"'Analyze' appears as a key, the f3c91a2e comment was right "
                f"and AutomationId does come from accessibleName.")
            assert "Analyze" not in props, (
                f"'Analyze' (the accessibleName) appears as an AutomationId; "
                f"AutomationId is NOT purely objectName after all. Present: "
                f"{sorted(props)!r}")
            full_id, spoken = props["AnalyzeButton"]
            assert spoken == "Analyze", (
                f"AnalyzeButton announces {spoken!r}; the button's "
                f"accessibleName is 'Analyze'.")
            assert full_id.endswith("AnalyzeButton"), (
                f"AnalyzeButton's AutomationId is {full_id!r}")
            assert "Analyze" == spoken and full_id != spoken, (
                "the two properties are equal, so this measurement cannot "
                "distinguish which one AutomationId follows")
        finally:
            close_window(hwnd)
            _settle(0.8)
