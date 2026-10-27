# -*- coding: utf-8 -*-
"""PLANTED CONTROL (F35, criterion 5) — the WIN-45 shape. MUST be flagged.

Not collected by pytest: `pytest.ini` sets `testpaths = tests`, and this file
lives under `tools/gate/fixtures/`. It exists only to be handed to
`check_reachability_skip_shape.py` by `tools/gate/test_f35_tranche_controls.py`.

Every guard below probes the PRODUCT through a live UI session and then leaves
the test without failing it. That is the false green WIN-45 names: the suite
reports green for a feature it never exercised.
"""

import pytest

from tests.helpers.pdfe_session import PDFESession  # pragma: no cover


def _open_customize_toolbars_dialog(session):  # pragma: no cover - fixture only
    return session.invoke("customizeToolbarsAction")


def _wait_for_dialog_widget(session, aid, timeout=3.0):  # pragma: no cover
    return session.find_widget(aid, timeout=timeout)


def test_win45_inline_session_probe():  # pragma: no cover - fixture only
    with PDFESession(pdf_path="x.pdf") as s:
        if not _open_customize_toolbars_dialog(s):
            pytest.skip("CustomizeToolbarsDialog could not be opened.")
        assert True


def test_win45_probe_through_local_assignment():  # pragma: no cover
    with PDFESession(pdf_path="x.pdf") as s:
        combo = _wait_for_dialog_widget(s, "toolbarCombo", timeout=3.0)
        if combo is None:
            pytest.skip("toolbarCombo not reachable.")
        assert combo.item_count() >= 2


def test_win45_split_record_then_skip():  # pragma: no cover
    """The third WIN-45 recurrence: bookkeeping call + an innocuous skip."""
    with PDFESession(pdf_path="x.pdf") as s:
        hwnd = s.resolve_hwnd()
        if hwnd <= 0:
            _note = "main HWND not resolvable"
            s.record("blocked", _note)
            pytest.skip("environment")
        assert hwnd > 0


def test_win45_probe_through_a_conditional_rebinding():  # pragma: no cover
    """GitHub #305, TENTH correction -- the selector/detector drift control.

    `select_f35_tranche._provenance_calls` claimed to mirror the detector's
    binding chase and had stopped doing so: it still took ONE binding, the
    textually last, after the detector moved to reaching definitions. On this
    shape the detector flagged the guard while the selector saw no session
    probe and classified it `S3:no-session-probe`, so Rule R35 would have
    EXCLUDED a genuine live-session WIN-45 site from a tranche.

    Nothing caught that, because no fixture here rebound a name. This one does.
    """
    with PDFESession(pdf_path="x.pdf") as s:
        if _open_customize_toolbars_dialog(s):
            combo = _wait_for_dialog_widget(s, "toolbarCombo", timeout=3.0)
        else:
            combo = None
        if combo is None:
            pytest.skip("toolbarCombo not reachable.")
        assert combo.item_count() >= 2
