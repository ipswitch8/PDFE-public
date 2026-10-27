#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""JUnit false-green defense tests (remediation Phase 5 — R-30 / GAP-A15).

Proves check_junit_xml.py is the pass/fail authority CI trusts instead of the
process exit code: the committed RED sentinel is rejected (exit 1) and the GREEN
sentinel is accepted (exit 0). This is the unit-level GAP-A15 wiring — a
deliberately-failing sentinel report fails the build via JUnit parse.

Run:
    python -m pytest tools/gate/test_check_junit_xml.py -q
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_junit_xml as cjx  # noqa: E402

FIX = Path(__file__).resolve().parent / "fixtures"


def test_red_sentinel_is_rejected():
    ok, msg = cjx.check(str(FIX / "sentinel_red.xml"))
    assert not ok, "the RED sentinel must NOT pass (GAP-A15 false-green defense)"
    assert "FAIL" in msg


def test_green_sentinel_is_accepted():
    ok, msg = cjx.check(str(FIX / "sentinel_green.xml"))
    assert ok, msg
    assert "PASS" in msg


def test_missing_report_fails():
    ok, msg = cjx.check(str(FIX / "no_such_report.xml"))
    assert not ok
    assert "not found" in msg


def test_min_tests_guards_suite_shrinkage():
    ok, _ = cjx.check(str(FIX / "sentinel_green.xml"), min_tests=100)
    assert not ok, "a suite far below the min-tests floor must fail"
