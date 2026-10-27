# -*- coding: utf-8 -*-
"""Non-UI unit test for the journey harness's target-selection / skip logic
(pipeline f4a82c1e Phase 2, AC2).

Verifies that the real "SpecimenA Green" variant is SKIPPED (simply absent from
the parametrization — not a hard failure) when the user's file is not present,
and included when it is. This runs UNCONDITIONALLY — no interactive desktop, no
pdfe.exe, no pywinauto — so the skip-when-absent behaviour is verified even on a
headless CI runner where the live journey itself is skipped. (The live journey
test in test_ui_journey_soak.py carries module-level skipifs for pdfe.exe /
interactive-desktop / pywinauto, which is why this AC2 check lives in its own
file with no such marks.)
"""
from __future__ import annotations

import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
REPO_ROOT = _HERE.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

import tests.test_ui_journey_soak as J  # noqa: E402  (import triggers no UI)


def test_specimen_a_absent_yields_corpus_only(monkeypatch):
    """When none of the SpecimenA candidate paths exist, _targets() yields the
    committed corpus ONLY — the real-file variant is cleanly absent, not a
    failing/erroring test item."""
    monkeypatch.setattr(J, "_SPECIMEN_A_CANDIDATES",
                        [Path(r"C:\__pdfe_nope__\missing_specimen_a_A.pdf"),
                         Path(r"C:\__pdfe_nope__\missing_specimen_a_B.pdf")])
    assert J._find_specimen_a() is None
    names = [t[0] for t in J._targets()]
    assert names == ["corpus"], \
        f"expected corpus-only when SpecimenA absent, got {names}"


def test_specimen_a_present_is_included(monkeypatch, tmp_path):
    """When a SpecimenA candidate path exists, the real-file variant is added to
    the parametrization alongside the always-present corpus."""
    fake = tmp_path / "specimen_a.pdf"
    fake.write_bytes(b"%PDF-1.7\n%%EOF\n")
    monkeypatch.setattr(J, "_SPECIMEN_A_CANDIDATES", [fake])
    assert J._find_specimen_a() == fake
    names = [t[0] for t in J._targets()]
    assert names == ["corpus", "specimen_a"], \
        f"expected specimen_a included when present, got {names}"
