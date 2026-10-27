#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Self-tests for the F35 tranche selector and its discriminating controls.

Registered in `.github/workflows/fast-checks.yml` ("Gate self-tests" step).
`pytest.ini` sets `testpaths = tests`, so a file under `tools/gate/` runs
NOWHERE unless it is named there explicitly.

What is pinned here
-------------------
1.  The **discriminating control pair** (phase-35 criterion 5):
    `check_reachability_skip_shape.py` FLAGS the planted WIN-45 fixture and
    PASSES the planted provenance-gated external-tool fixture. A checker that
    only satisfied the first would be a checker that flags everything.
2.  The checker's **documented limitation** — `acrobat = _find_acrobat()` is
    still flagged, and those sites are still in the baseline (criterion 4). If
    someone "fixes" the limitation by widening the environment allowlist to
    swallow arbitrary helper calls, this test goes red rather than the sweep
    silently getting quieter.
3.  **Rule R35** selects the live-session-probe shape and rejects the
    external-tool shape (criterion 1), and the tranche it converted is gone from
    the baseline while the excluded sites remain (criterion 3).
"""

from __future__ import annotations

import contextlib
import io
import json
import sys
from pathlib import Path

import pytest

GATE_DIR = Path(__file__).resolve().parent
REPO_ROOT = GATE_DIR.parent.parent
FIXTURES = GATE_DIR / "fixtures"

sys.path.insert(0, str(GATE_DIR))

import check_reachability_skip_shape as detector  # noqa: E402
import select_f35_tranche as selector  # noqa: E402

POSITIVE = FIXTURES / "win45_positive_reachability_skip.py"
NEGATIVE = FIXTURES / "win45_negative_external_tool_gate.py"
BASELINE = GATE_DIR / "reachability_skip_baseline.json"
TRANCHE_FILE = "tests/test_ui_preferences_chrome.py"


def _scan(path: Path):
    return detector.scan_path(path, REPO_ROOT)


def _baseline() -> dict:
    with open(BASELINE, "r", encoding="utf-8") as handle:
        return json.load(handle)


# --------------------------------------------------------------------------
# Criterion 5 — the discriminating control pair
# --------------------------------------------------------------------------


def test_positive_control_fixture_exists():
    assert POSITIVE.is_file(), "planted WIN-45 control fixture is missing"


def test_negative_control_fixture_exists():
    assert NEGATIVE.is_file(), "planted external-tool control fixture is missing"


def test_checker_flags_the_planted_win45_fixture():
    findings = _scan(POSITIVE)
    assert findings, "the WIN-45 control fixture was NOT flagged — the checker is blind"
    functions = {f.function for f in findings}
    assert functions == {
        "test_win45_inline_session_probe",
        "test_win45_probe_through_local_assignment",
        "test_win45_split_record_then_skip",
        # Added by GitHub #305's tenth correction, together with the
        # selector/detector agreement test below.
        "test_win45_probe_through_a_conditional_rebinding",
    }, functions


def _run_cli(*argv) -> tuple:
    """Run the checker CLI, capturing stdout.

    `capsys` is unusable here: this repo's `pytest.ini` disables capture, so the
    capture manager is not installed and the fixture errors out.
    """
    buffer = io.StringIO()
    with contextlib.redirect_stdout(buffer):
        rc = detector.main(list(argv))
    return rc, buffer.getvalue()


def test_checker_cli_exits_nonzero_on_the_planted_win45_fixture():
    rc, out = _run_cli("--paths", str(POSITIVE), "--root", str(REPO_ROOT))
    assert rc == 1
    # 4 since GitHub #305's tenth correction added the conditional-rebinding
    # control to the fixture. This figure is deliberately exact: a fixture that
    # grows without this number moving would mean the checker stopped seeing
    # one of the planted guards.
    assert "4 NEW" in out


def test_checker_passes_the_planted_external_tool_fixture():
    findings = _scan(NEGATIVE)
    assert findings == [], "legitimate environment gates were flagged: %r" % (
        [(f.function, f.condition) for f in findings]
    )


def test_checker_cli_exits_zero_on_the_planted_external_tool_fixture():
    rc, out = _run_cli("--paths", str(NEGATIVE), "--root", str(REPO_ROOT))
    assert rc == 0
    assert "0 NEW" in out


@pytest.mark.parametrize(
    "function_name",
    [
        "test_external_tool_absent_is_a_real_environment_gate",
        "test_optional_dependency_absent_is_a_real_environment_gate",
        "test_opt_in_env_var_is_a_real_environment_gate",
        "test_missing_fixture_file_is_a_real_environment_gate",
    ],
)
def test_each_negative_control_guard_is_individually_clean(function_name):
    """Not just "0 findings overall" — every guard in the file is exercised.

    A whole-file assertion would still pass if three of the four guards had been
    quietly deleted; this fails if any one of them stops being present.
    """
    source = NEGATIVE.read_text(encoding="utf-8")
    assert ("def %s(" % function_name) in source
    assert [f for f in _scan(NEGATIVE) if f.function == function_name] == []


# --------------------------------------------------------------------------
# Criterion 4 — the documented limitation, pinned
# --------------------------------------------------------------------------


def test_opaque_external_tool_helper_is_still_flagged():
    """`acrobat = _find_acrobat()` is invisible to AST provenance — by design.

    The checker cannot follow provenance into a called function, so this shape
    is indistinguishable from a product probe and MUST stay in the baseline
    rather than be swept by F35.
    """
    target = REPO_ROOT / "tests" / "test_print_acrobat_interop.py"
    findings = _scan(target)
    acrobat = [f for f in findings if "acrobat" in f.condition.lower()]
    assert acrobat, "expected the opaque _find_acrobat() gate to still be flagged"
    known = set(_baseline()["known_sites"])
    for finding in acrobat:
        assert finding.key() in known, (
            "%s was swept out of the baseline; F35 must not remove opaque "
            "external-tool gates" % finding.key()
        )


def test_rule_r35_does_not_select_opaque_external_tool_gates():
    rows = selector.classify_file(
        REPO_ROOT / "tests" / "test_print_acrobat_interop.py", REPO_ROOT
    )
    acrobat = [r for r in rows if "acrobat" in str(r["condition"]).lower()]
    assert acrobat
    for row in acrobat:
        assert row["selected"] is False
        assert "S3:no-session-probe" in row["excluded_by"]


# --------------------------------------------------------------------------
# Criterion 1 / 3 — the rule, and the shrink it produced
# --------------------------------------------------------------------------


def test_rule_r35_selects_the_live_session_probe_shape():
    rows = selector.classify_file(POSITIVE, REPO_ROOT)
    selected = {r["function"] for r in rows if r["selected"]}
    assert selected == {
        "test_win45_inline_session_probe",
        "test_win45_probe_through_local_assignment",
        "test_win45_split_record_then_skip",
        # GitHub #305, tenth correction. See the next test for why this one is
        # here; it is the shape on which the selector had silently stopped
        # agreeing with the detector.
        "test_win45_probe_through_a_conditional_rebinding",
    }, selected


def test_selector_and_detector_agree_on_a_conditionally_rebound_probe():
    """GitHub #305, TENTH correction -- the drift this file could not see.

    `_provenance_calls` documents itself as mirroring
    `detector._condition_provenance`'s traversal. After the ninth correction
    moved the detector to reaching definitions, that claim was FALSE: the
    selector still chased ONE binding, the textually last. The p98 phase-3
    claim-check gate measured the consequence -- the detector flags a
    conditionally-rebound session probe while the selector reported
    `S3:no-session-probe`, i.e. Rule R35 would EXCLUDE a real live-session
    WIN-45 site from a tranche, backwards from its purpose.

    A "mirrors X" docstring is a claim, and this is its test: both tools are
    asked about the same guard and must agree that it probes a live session.
    """
    rows = selector.classify_file(POSITIVE, REPO_ROOT)
    row = next(
        r for r in rows
        if r["function"] == "test_win45_probe_through_a_conditional_rebinding"
    )
    assert row["selected"], row
    assert row["session_probe"], (
        "the selector must see the session probe through the rebinding, or it "
        "has drifted from the detector again: " + repr(row)
    )
    # The other half of "agree": the detector must flag the same guard.
    findings = _scan(POSITIVE)
    assert any(f.line == row["line"] for f in findings), (
        "the detector no longer flags the guard the selector selected"
    )


def test_rule_r35_rejects_every_guard_in_the_negative_control():
    rows = selector.classify_file(NEGATIVE, REPO_ROOT)
    assert [r for r in rows if r["selected"]] == []


def test_baseline_count_matches_its_own_site_list():
    data = _baseline()
    assert data["count"] == len(data["known_sites"])
    assert len(set(data["known_sites"])) == len(data["known_sites"])


def test_tranche_file_has_no_selectable_sites_left():
    """Every R35-selected site in the tranche file was actually converted."""
    rows = selector.classify_file(REPO_ROOT / TRANCHE_FILE, REPO_ROOT)
    assert [r for r in rows if r["selected"]] == []


def test_tranche_file_sites_still_in_baseline_are_the_excluded_ones():
    rows = selector.classify_file(REPO_ROOT / TRANCHE_FILE, REPO_ROOT)
    known = {k for k in _baseline()["known_sites"] if k.startswith(TRANCHE_FILE + ":")}
    assert {str(r["site"]) for r in rows} == known
    reasons = {reason for r in rows for reason in r["excluded_by"]}
    assert reasons <= {"S2:exit_kind=continue", "S2:exit_kind=return", "S3:no-session-probe"}


def test_converted_sites_use_the_recorded_non_pass_shape():
    """A converted site must FAIL, not silently pass. Pin the whole triple."""
    source = (REPO_ROOT / TRANCHE_FILE).read_text(encoding="utf-8")
    assert source.count("# WIN-45 / F35:") == 20
    assert source.count(
        "WIN-45 reachability blocked (recorded non-pass, not skipped)"
    ) == source.count("record_matrix_result(current_test_id(), \"blocked\", _reach)")
