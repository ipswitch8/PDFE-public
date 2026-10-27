#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Coverage-floor gate tests (remediation Phase 5 — R-32).

Proves check_coverage_floor.py is a GATE: a below-floor report exits 1, an
at/above-floor report exits 0, and a missing/corrupt report is a HARD error
(exit 2) — never a silent pass.

Run:
    python -m pytest tools/gate/test_check_coverage_floor.py -q
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_coverage_floor as ccf  # noqa: E402


def _cobertura(tmp_path: Path, line_rate: float) -> Path:
    p = tmp_path / "cov.xml"
    p.write_text(
        f'<?xml version="1.0"?>\n<coverage line-rate="{line_rate}" '
        f'lines-covered="{int(line_rate*100)}" lines-valid="100"></coverage>\n',
        encoding="utf-8")
    return p


def test_below_floor_fails(tmp_path):
    code, msg = ccf.check(str(_cobertura(tmp_path, 0.42)), floor=0.60)
    assert code == 1, msg
    assert "FAIL" in msg


def test_at_or_above_floor_passes(tmp_path):
    code, msg = ccf.check(str(_cobertura(tmp_path, 0.73)), floor=0.60)
    assert code == 0, msg
    assert "PASS" in msg


def test_missing_report_is_hard_error(tmp_path):
    code, msg = ccf.check(str(tmp_path / "does_not_exist.xml"), floor=0.60)
    assert code == 2, msg
    assert "HARD ERROR" in msg


def test_corrupt_report_is_hard_error(tmp_path):
    p = tmp_path / "bad.xml"
    p.write_text("<coverage line-rate=", encoding="utf-8")  # truncated
    code, msg = ccf.check(str(p), floor=0.60)
    assert code == 2, msg
    assert "HARD ERROR" in msg


def test_derives_rate_from_lines_covered_when_no_line_rate(tmp_path):
    p = tmp_path / "cov.xml"
    p.write_text(
        '<?xml version="1.0"?>\n<coverage lines-covered="80" lines-valid="100">'
        '</coverage>\n', encoding="utf-8")
    code, msg = ccf.check(str(p), floor=0.60)
    assert code == 0, msg
    assert "80.00%" in msg


def test_derives_rate_from_line_hits_fallback(tmp_path):
    p = tmp_path / "cov.xml"
    p.write_text(
        '<?xml version="1.0"?>\n<coverage><packages><package><classes><class>'
        '<lines>'
        '<line number="1" hits="1"/><line number="2" hits="0"/>'
        '<line number="3" hits="1"/><line number="4" hits="1"/>'
        '</lines></class></classes></package></packages></coverage>\n',
        encoding="utf-8")
    # 3/4 = 0.75 covered.
    code, msg = ccf.check(str(p), floor=0.70)
    assert code == 0, msg
    code2, _ = ccf.check(str(p), floor=0.80)
    assert code2 == 1


# ---------------------------------------------------------------------------
# The PROCESS, not just the predicate. (GitHub #258)
#
# Every test above calls ccf.check() and asserts on its return value. That proves
# the predicate computes the right answer and says nothing about whether running
# the script reports it -- and those come apart in practice: a watchdog in this
# repo failed every heartbeat write and exited 0. For a gate, the exit code IS
# the contract, so it needs its own assertions.
# ---------------------------------------------------------------------------
import subprocess                                             # noqa: E402
import sys                                                    # noqa: E402
import os                                                     # noqa: E402

_GUARD = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "check_coverage_floor.py")


def _run(*args):
    return subprocess.run([sys.executable, "-u", _GUARD, *args],
                          capture_output=True, text=True,
                          encoding="utf-8", errors="replace")


def test_cli_exit_1_when_below_floor(tmp_path):
    r = _run(str(_cobertura(tmp_path, 0.42)), "--floor", "0.60")
    assert r.returncode == 1, (
        "below-floor coverage did not fail the PROCESS (exit %d). A gate that "
        "prints FAIL and exits 0 is not a gate.\n%s"
        % (r.returncode, r.stdout + r.stderr))
    assert "FAIL" in r.stdout


def test_cli_exit_0_when_at_or_above_floor(tmp_path):
    r = _run(str(_cobertura(tmp_path, 0.73)), "--floor", "0.60")
    assert r.returncode == 0, r.stdout + r.stderr
    assert "PASS" in r.stdout


def test_cli_exit_2_when_the_report_is_absent(tmp_path):
    """The shape that let a coverage collapse ship green before R-32: a runner
    without OpenCppCoverage produced no report, and nothing complained. A missing
    report must be a HARD error, distinguishable from both pass and below-floor."""
    r = _run(str(tmp_path / "absent.xml"))
    assert r.returncode == 2, (
        "a missing coverage report exited %d; 2 is reserved for "
        "missing-or-unusable precisely so it cannot be mistaken for either a "
        "pass (0) or a coverage regression (1).\n%s"
        % (r.returncode, r.stdout + r.stderr))


def test_cli_exit_2_when_the_report_has_no_coverage_data(tmp_path):
    p = tmp_path / "empty.xml"
    p.write_text('<?xml version="1.0"?><coverage></coverage>', encoding="utf-8")
    r = _run(str(p))
    assert r.returncode == 2, r.stdout + r.stderr


def test_cli_refuses_to_pass_with_no_arguments():
    """Without the argparse guard, adding --self-test made `xml` optional, and a
    no-argument invocation would have taken the check() path with xml=None. A
    gate invoked wrongly must fail, not succeed vacuously."""
    r = _run()
    assert r.returncode != 0, (
        "running the gate with no report path succeeded (exit 0), so a "
        "mis-wired CI step would read as a passing coverage gate.\n%s"
        % (r.stdout + r.stderr))


def test_self_test_passes_and_is_not_vacuous():
    r = _run("--self-test")
    assert r.returncode == 0, r.stdout + r.stderr
    # It must say how many cases ran. A self-test that silently exercised zero
    # cases would exit 0 and look identical to this.
    assert "case(s) passed" in r.stdout, r.stdout
    assert "all three exit codes" in r.stdout, r.stdout
