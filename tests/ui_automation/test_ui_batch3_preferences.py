# -*- coding: utf-8 -*-
"""Pipeline c4a9e271 phase-7 — batch-3 preferences control-state proof.

Phase-7 acceptance criterion 3: "Preferences tests cover all QTabWidget
pages in PreferencesDialog (objectName preferencesTabs); each page has at
least 1 control-state assertion."

This drives the production ``testPref*`` dialog-child pipe seams (the same
GAP-A14, UIA-FindAll-free path the mature phase-14 suite uses) to:

  * confirm all five tab pages are present (General, View, Security, OCR,
    Advanced) via the widget-inventory ``||tabs=`` suffix, and
  * read back the live VALUE (control-state) of one representative control
    on each page, plus a set→get round-trip on the General page.

Scope note (reality-check for the gates): the DEEP preferences coverage —
every widget exercised + persistence to the QSettings INI + Customize
Toolbars + keyboard shortcuts + theme visual diffs — already exists and
passes in ``tests/test_ui_preferences_chrome.py`` (pipeline a7c2e541
phase-14). Batch-3 does not duplicate it; it adds the focused,
matrix-recording per-tab control-state rows the phase-7 batch layer owns,
reusing phase-14's public ``qinvoke`` helpers rather than re-implementing
the pipe protocol.

Driven through ``tests.helpers.qinvoke`` (PDFESession harness) because the
``testPrefGetValueQueryPipe`` seam reads a dialog-CHILD pipe via a
child-scope COM scan — never a ``descendants()`` walk of the open modal,
which hangs on the Server-2019 UIA bridge.

RUN-ISOLATION CONSTRAINT (important for CI / the gate)
------------------------------------------------------
This module MUST run in its OWN pytest process — do NOT bundle it into a
single ``pytest`` invocation behind the other ``test_ui_batch3_*`` modules.
The qinvoke child-pipe scan opens fresh ``Application().connect()`` handles,
and after ~30 prior ``launch_pdfe`` UIA sessions in the same Python process
the COM proxy tables accumulate the 0x80040155 (REGDB_E_CLASSNOTREG) state
that makes the first dialog-child scan miss the modal. Run standalone it is
deterministic (PreferencesDialog opens, all five tabs + per-tab control
state read in ~20 s). CI runs each UI file in its own subprocess, which
satisfies this; only an artificial ``pytest a.py b.py prefs.py`` bundle
trips it. See [[gap_a14_uia_menu_hang]] / the COM-accumulation note in
test_ui_batch1_actions.py.
"""
from __future__ import annotations

import os
import sys
import time
from pathlib import Path

import pytest

# testPref* pipes require PDFE_TEST_PIPES; drive the Release build. Set
# BEFORE the PDFESession launcher copies os.environ. NOTE: we deliberately
# do NOT set PDFE_TEST_DIALOG_AUTODISMISS — the PreferencesDialog must stay
# OPEN so its child pipes are reachable; we close it via testPrefRejectPipe.
os.environ.setdefault("PDFE_TEST_PIPES", "1")
os.environ.setdefault("PDFE_BUILD_CONFIG", "Release")

_HERE = Path(__file__).resolve().parent
REPO_ROOT = _HERE.parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.ui_automation.conftest import record_matrix_result  # noqa: E402
from tests.helpers.pdfe_session import (  # noqa: E402
    PDFESession,
    PDFE_EXE,
    kill_pdfe_orphans,
    no_orphans,
)
from tests.helpers.qinvoke import (  # noqa: E402
    activate_tool_by_name,
    preferences_dialog_is_open,
    preferences_tab_labels,
    preferences_get_widget_value,
    preferences_set_widget_value,
    preferences_reject,
)

FIXTURES = REPO_ROOT / "tests" / "fixtures"
FIX_3PAGE = FIXTURES / "test_3page.pdf"

# One representative control per tab page (objectNames from
# src/ui/PreferencesDialog.cpp). Each get-value call is a control-STATE
# assertion satisfying criterion 3 for that page.
TAB_CONTROLS = [
    ("General", "maxRecentFilesSpinBox"),
    ("View", "defaultZoomSpinBox"),
    ("Security", "javaScriptEnabledCheck"),
    ("OCR", "ocrLanguageCombo"),
    ("Advanced", "logLevelCombo"),
]
EXPECTED_TABS = {"General", "View", "Security", "OCR", "Advanced"}


@pytest.fixture(autouse=True)
def _isolate_pdfe_processes():
    """Kill orphan pdfe.exe before/after (the ui_automation conftest
    autouse killer opts batch-3 modules out, so this module brings its own
    — mirrors the phase-14 fixture)."""
    kill_pdfe_orphans()
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline and not no_orphans():
        kill_pdfe_orphans()
        time.sleep(0.3)
    time.sleep(0.8)
    yield
    kill_pdfe_orphans()
    deadline = time.monotonic() + 8.0
    while time.monotonic() < deadline and not no_orphans():
        kill_pdfe_orphans()
        time.sleep(0.3)
    assert no_orphans(), "PDFE.exe still running at teardown — orphan leak."


def _open_preferences(s: PDFESession) -> bool:
    """Open PreferencesDialog via the testToolActivatePipe seam (no UIA
    menu walk) and confirm its child pipes are reachable.

    Retries up to three times: the very first activate after a fresh
    launch (especially right after another harness churned the desktop)
    can lose a one-time COM-warmup race in the dialog-child pipe scan, so
    we cancel any half-open modal and retry with a longer settle.
    """
    for attempt in range(3):
        try:
            activate_tool_by_name(s, "preferencesAction")
        except Exception:  # noqa: BLE001
            pass
        deadline = time.monotonic() + 7.0
        while time.monotonic() < deadline:
            if preferences_dialog_is_open(s, lookup_timeout=1.0):
                return True
            time.sleep(0.3)
        # Cancel any half-open modal (pipe-first, then nothing else needed
        # since the dialog is reject-on-cancel) and retry.
        preferences_reject(s, settle=0.3)
        time.sleep(1.5 if attempt == 0 else 0.6)
    return False


def test_preferences_tabs_and_control_state():
    """Criterion 3: all five PreferencesDialog tabs present, and one live
    control-state readback per tab + a General set→get round-trip.

    Records seven owned matrix rows: pref_tabs_present, pref_state_<tab>
    (x5), pref_set_roundtrip.
    """
    if not Path(PDFE_EXE).exists():
        # Environmental (binary not built) — record blocked, not green.
        for tid in (["pref_tabs_present", "pref_set_roundtrip"]
                    + [f"pref_state_{t.lower()}" for t, _ in TAB_CONTROLS]):
            record_matrix_result(tid, "blocked", f"Release binary missing: {PDFE_EXE}")
        pytest.fail(f"Release PDFE.exe not built: {PDFE_EXE}")
    if not FIX_3PAGE.exists():
        pytest.fail(f"fixture missing: {FIX_3PAGE}")

    failures: list[str] = []
    with PDFESession(pdf_path=str(FIX_3PAGE)) as s:
        assert s.wait_for_window_title(FIX_3PAGE.name, timeout=12)
        time.sleep(1.0)
        if not _open_preferences(s):
            for tid in (["pref_tabs_present", "pref_set_roundtrip"]
                        + [f"pref_state_{t.lower()}" for t, _ in TAB_CONTROLS]):
                record_matrix_result(
                    tid, "red",
                    "PreferencesDialog did not open via the activate seam")
            pytest.fail("PreferencesDialog did not open via the activate seam")

        # --- all five tabs present ---
        tabs = set(preferences_tab_labels(s))
        missing_tabs = EXPECTED_TABS - tabs
        if missing_tabs:
            record_matrix_result(
                "pref_tabs_present", "red",
                f"missing tabs {missing_tabs!r}; found {sorted(tabs)!r}")
            failures.append(f"pref_tabs_present: missing {missing_tabs}")
        else:
            record_matrix_result(
                "pref_tabs_present", "green",
                f"phase-7 batch-3: PreferencesDialog tabs={sorted(tabs)} "
                f"(preferencesTabs); via testPrefWidgetNamesResultPipe")

        # --- per-tab control-state readback ---
        for tab, widget in TAB_CONTROLS:
            tid = f"pref_state_{tab.lower()}"
            val = preferences_get_widget_value(s, widget)
            ok = val not in (None, "__notfound__", "")
            notes = (f"phase-7 batch-3: {tab} tab control-state "
                     f"{widget}='{val}' via testPrefGetValueResultPipe")
            if not ok:
                record_matrix_result(tid, "red", notes)
                failures.append(f"{tid}: {widget} value={val!r}")
            else:
                record_matrix_result(tid, "green", notes)

        # --- General set -> get round-trip (control-state mutation) ---
        set_ok = preferences_set_widget_value(s, "maxRecentFilesSpinBox", "17")
        rt = preferences_get_widget_value(s, "maxRecentFilesSpinBox")
        rt_ok = set_ok and rt is not None and "17" in str(rt)
        rt_notes = (f"phase-7 batch-3: set maxRecentFilesSpinBox=17 via "
                    f"testPrefSetValuePipe, readback='{rt}' (set_ok={set_ok}); "
                    f"Cancel — no QSettings persistence (that path is proven "
                    f"in test_ui_preferences_chrome.py phase-14)")
        if not rt_ok:
            record_matrix_result("pref_set_roundtrip", "red", rt_notes)
            failures.append(f"pref_set_roundtrip: readback={rt!r}")
        else:
            record_matrix_result("pref_set_roundtrip", "green", rt_notes)

        # Cancel so nothing persists to QSettings.
        preferences_reject(s, settle=0.5)
        time.sleep(0.4)

    if failures:
        pytest.fail("; ".join(failures))
