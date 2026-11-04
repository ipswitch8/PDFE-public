# -*- coding: utf-8 -*-
"""Pipeline ui-report-netopen phase-2: the Help -> Report Issue chooser dialog.

WHAT THIS PROVES
----------------
``MainWindow::onReportIssue`` used to open a GitHub URL directly. It now shows
an app-modal chooser offering TWO reporting channels -- GitHub (the public
mirror) and Email (pdfe@itwerx.net) -- plus Cancel. This test drives the real
``reportIssueAction`` QAction and asserts the real dialog carries both action
buttons with their stable objectNames.

WHY INTROSPECTION, NOT UIA ENUMERATION
--------------------------------------
The dialog is a Qt app-modal ``QDialog::exec()``. pywinauto's UIA backend
cannot enumerate a Qt modal while its loop runs (see
tests/helpers/modal_dialog.py -- a dialog sat open four minutes while a 240s
UIA poll reported nothing). So we do NOT look for the dialog through UIA.
Instead:

  * ``registerDialogIntrospection(&dialog)`` records the dialog's child-widget
    objectName inventory into a process-static registry THE MOMENT the dialog
    opens (before any action replay), and that record PERSISTS after it closes.
  * We pre-queue a ``reject`` action script keyed by the dialog's objectName,
    so when the modal opens it closes itself immediately -- without opening a
    browser or mail client -- and ``exec()`` returns. Choosing reject (not a
    button click) is deliberate: it exercises the Cancel path and launches
    nothing external from the CI session.
  * After the modal closes (MainWindow non-modal again, so its introspection
    query/result pipes receive UIA events) we read the recorded inventory and
    assert both channel buttons are present.

The GitHub URL and mailto VALUE assertions live in the headless GoogleTest
``test_report_issue_targets.cpp`` -- this test's job is the UI surface: that
the chooser exists and genuinely offers both legs.

Run:  python -m pytest tests/test_ui_report_issue_chooser.py -v -s
"""
from __future__ import annotations

import os
import sys
import time
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
REPO_ROOT = _HERE.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

DIALOG_OBJECT_NAME = "reportIssueChooserDialog"
GITHUB_BUTTON = "reportIssueGithubButton"
EMAIL_BUTTON = "reportIssueEmailButton"
CANCEL_BUTTON = "reportIssueCancelButton"
PROMPT_LABEL = "reportIssuePromptLabel"


def _has(mod: str) -> bool:
    try:
        __import__(mod)
        return True
    except Exception:  # noqa: BLE001
        return False


def _has_pdfe() -> bool:
    try:
        from tests.helpers.pdfe_session import PDFE_EXE
        return Path(PDFE_EXE).exists()
    except Exception:  # noqa: BLE001
        return False


def _is_interactive() -> bool:
    if os.environ.get("PDFE_UI_FORCE_HEADLESS") == "1":
        return False
    if sys.platform != "win32":
        return False
    return bool(os.environ.get("SESSIONNAME"))


pytestmark = [
    pytest.mark.skipif(not _has_pdfe(), reason="pdfe.exe not built"),
    pytest.mark.skipif(not _is_interactive(),
                       reason="UIA requires an interactive Windows desktop"),
    pytest.mark.skipif(not _has("pywinauto"), reason="pywinauto not installed"),
    pytest.mark.skipif(not _has("psutil"), reason="psutil not installed"),
]


def test_report_issue_chooser_offers_github_and_email():
    from tests.helpers.pdfe_session import PDFESession
    from tests.helpers import qinvoke

    with PDFESession() as s:
        assert s.wait_for_window_title("PDFE"), "PDFE window never appeared"
        s.wait_for_idle()
        time.sleep(0.8)

        # Pre-queue a reject so the modal closes itself the instant it opens
        # (no browser / mail client launched), while its inventory is recorded
        # first. Enqueue while MainWindow is still non-modal.
        assert qinvoke.enqueue_dialog_action(
            s, "%s|reject" % DIALOG_OBJECT_NAME), \
            "could not queue the dialog reject script"

        # Drive the REAL Help menu action. activateToolByObjectName defers the
        # trigger via singleShot, so this returns before the modal blocks.
        assert qinvoke.activate_tool_by_name(s, "reportIssueAction"), \
            "failed to trigger the reportIssueAction QAction"

        # Let the modal open, record its inventory, replay the reject, return.
        time.sleep(1.5)

        record = qinvoke.read_dialog_introspection(s, DIALOG_OBJECT_NAME)
        assert record and record != "__null__", (
            "no introspection record for %r -- the chooser dialog either never "
            "opened or did not call registerDialogIntrospection. Record was: %r"
            % (DIALOG_OBJECT_NAME, record)
        )

        # The record is "<obj>|name1,name2,...[||combos=...]". Assert both
        # reporting channels are offered, plus the prompt and a cancel path.
        names_part = record.split("||combos=", 1)[0]
        inventory = names_part.split("|", 1)[1] if "|" in names_part else ""
        names = set(n for n in inventory.split(",") if n)

        assert GITHUB_BUTTON in names, (
            "the chooser does not offer a GitHub button (%r). A report surface "
            "with no GitHub leg is the regression this guards. Inventory: %r"
            % (GITHUB_BUTTON, sorted(names))
        )
        assert EMAIL_BUTTON in names, (
            "the chooser does not offer an Email button (%r) -- the email "
            "reporting channel (pdfe@itwerx.net) is missing. Inventory: %r"
            % (EMAIL_BUTTON, sorted(names))
        )
        # Supporting controls: a prompt and an explicit cancel path.
        assert PROMPT_LABEL in names, (
            "the chooser is missing its prompt label %r. Inventory: %r"
            % (PROMPT_LABEL, sorted(names))
        )
        assert CANCEL_BUTTON in names, (
            "the chooser is missing a Cancel button %r. Inventory: %r"
            % (CANCEL_BUTTON, sorted(names))
        )
