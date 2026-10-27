#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Flaky-quarantine gate tests (remediation Phase 5 — R-37).

Proves check_flaky_quarantine.py stops a rerun from silently masking a flake:
an UNTRACKED healed test fails, a REGISTERED healed test is tolerated, and an
EXPIRED registry entry fails so a quarantine cannot live forever.

Run:
    python -m pytest tools/gate/test_check_flaky_quarantine.py -q
"""
from __future__ import annotations

import datetime as dt
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_flaky_quarantine as cfq  # noqa: E402

TODAY = dt.date(2026, 7, 28)


def _registry(entries):
    return {"schema": 1, "known_flaky": entries, "xfail": []}


def test_untracked_healed_flake_fails():
    reg = _registry([])
    ok, msgs = cfq.evaluate(reg, ["SomeSuite.SomethingFlaky"], TODAY)
    assert not ok
    assert any("NOT in the flaky quarantine registry" in m for m in msgs)


def test_registered_healed_flake_is_tolerated():
    reg = _registry([{"test": "MainWindowIntegrationTest.ZoomComboExists",
                      "reason": "cumulative state", "owner": "harness",
                      "expires": "2026-12-31"}])
    ok, msgs = cfq.evaluate(reg, ["MainWindowIntegrationTest.ZoomComboExists"], TODAY)
    assert ok, "\n".join(msgs)


def test_expired_entry_fails():
    reg = _registry([{"test": "T.Old", "reason": "x", "owner": "y",
                      "expires": "2025-01-01"}])
    ok, msgs = cfq.evaluate(reg, [], TODAY)
    assert not ok
    assert any("expires" in m for m in msgs)


def test_no_heals_is_clean():
    reg = _registry([{"test": "T.Known", "reason": "x", "owner": "y",
                      "expires": "2026-12-31"}])
    ok, msgs = cfq.evaluate(reg, [], TODAY)
    assert ok, "\n".join(msgs)


def test_the_committed_registry_is_valid_and_unexpired():
    """The real shipped registry must parse and hold no already-expired entry."""
    import json
    p = Path(__file__).resolve().parent / "flaky_quarantine.json"
    reg = json.loads(p.read_text(encoding="utf-8"))
    ok, msgs = cfq.evaluate(reg, [], TODAY)
    assert ok, "committed flaky_quarantine.json has an expired entry:\n" + "\n".join(msgs)
