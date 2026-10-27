# -*- coding: utf-8 -*-
"""Phase-11: P0 test runner + summary report.

Executes every P0 test file in a single pytest invocation and
produces a summary JSON at ``tests/ui_automation/p0_summary.json``
plus a human-readable ``p0_summary.txt``. The runner returns a
non-zero exit code if any P0 test failed or if any planned/red row
in the matrix is missing from the execution report.

Usage:

    python tests/ui_automation/run_p0.py

Or with -v/--verbose for the pytest progress stream:

    python tests/ui_automation/run_p0.py -v

The runner is the gate for phase-11 acceptance criteria:
  * Single command runs all P0 tests
  * Summary file lists pass/fail counts + individual results
  * Matrix P0 rows all green/red/blocked with no 'planned'
  * Matrix green+red count matches the runner's executed count
  * No orphan pdfe.exe after teardown
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Dict, List

REPO_ROOT = Path(__file__).resolve().parents[2]
TESTS_DIR = REPO_ROOT / "tests" / "ui_automation"
MATRIX_CSV = TESTS_DIR / "matrix" / "ui_test_matrix.csv"
SUMMARY_JSON = TESTS_DIR / "p0_summary.json"
SUMMARY_TXT = TESTS_DIR / "p0_summary.txt"

# P0 test files, in execution order. Add new files to this list
# whenever a new phase implements P0 coverage.
P0_TEST_FILES = [
    "tests/ui_automation/test_menu_navigation_p0.py",     # phase-2
    "tests/ui_automation/test_dialog_reachability_p0.py", # phase-3
    "tests/ui_automation/test_critical_flows_p0.py",      # phase-4
]


def _count_matrix_rows() -> Dict[str, int]:
    """Return P0 matrix-row counts by status."""
    counts = {"green": 0, "red": 0, "blocked": 0, "planned": 0, "other": 0}
    by_phase: Dict[str, Dict[str, int]] = {}
    if not MATRIX_CSV.exists():
        return {"total": 0, "_error": "matrix missing", **counts}
    with MATRIX_CSV.open("r", encoding="utf-8", newline="") as f:
        rows = [r for r in csv.DictReader(f) if r.get("priority") == "P0"]
    for r in rows:
        status = r.get("status", "other") or "other"
        if status not in counts:
            status = "other"
        counts[status] += 1
        phase = r.get("owner_phase", "unknown") or "unknown"
        by_phase.setdefault(phase, {"green": 0, "red": 0, "blocked": 0,
                                    "planned": 0, "other": 0})
        by_phase[phase][status] += 1
    counts["total"] = len(rows)
    counts["by_phase"] = by_phase
    return counts


def _tasklist_pdfe_running() -> bool:
    try:
        r = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq pdfe.exe"],
            capture_output=True, text=True, timeout=10,
        )
        return "pdfe.exe" in r.stdout
    except Exception:
        return False


def _kill_pdfe_stragglers() -> None:
    try:
        subprocess.run(
            ["taskkill", "/F", "/IM", "pdfe.exe"],
            capture_output=True, timeout=10,
        )
    except Exception:
        pass


def _run_pytest(verbose: bool, junit_xml: Path) -> Dict[str, Any]:
    """Execute pytest on the P0 files, collect JUnit XML output."""
    args = [
        sys.executable, "-m", "pytest",
        *P0_TEST_FILES,
        "--tb=line",
        "-q",
        f"--junitxml={junit_xml}",
    ]
    if verbose:
        args.append("-v")

    start = time.monotonic()
    try:
        proc = subprocess.run(
            args,
            cwd=str(REPO_ROOT),
            capture_output=not verbose,
            text=True,
            timeout=3600,
        )
        exit_code = proc.returncode
        stdout = proc.stdout if not verbose else ""
        stderr = proc.stderr if not verbose else ""
    except subprocess.TimeoutExpired:
        exit_code = 124
        stdout = ""
        stderr = "pytest timeout (60 min)"

    duration_s = time.monotonic() - start
    return {
        "exit_code": exit_code,
        "duration_s": duration_s,
        "stdout_tail": stdout[-3000:] if stdout else "",
        "stderr_tail": stderr[-1000:] if stderr else "",
    }


def _parse_junit(junit_xml: Path) -> Dict[str, Any]:
    """Parse JUnit XML into per-case status dict.

    Returns a dict with keys:
      passed / skipped / failed / errors (counts)
      cases (list of dicts with test_id, status, duration, message)
    """
    if not junit_xml.exists():
        return {"_error": f"junit xml not at {junit_xml}"}

    import xml.etree.ElementTree as ET
    tree = ET.parse(junit_xml)
    root = tree.getroot()

    cases: List[Dict[str, Any]] = []
    passed = skipped = failed = errors = 0

    # JUnit XML from pytest: <testsuites><testsuite><testcase>
    for ts in root.iter("testsuite"):
        for tc in ts.iter("testcase"):
            name = tc.get("name") or ""
            classname = tc.get("classname") or ""
            duration = float(tc.get("time") or 0.0)

            failure = tc.find("failure")
            error = tc.find("error")
            skipped_el = tc.find("skipped")

            if failure is not None:
                status = "failed"
                msg = failure.get("message") or (failure.text or "")
                failed += 1
            elif error is not None:
                status = "error"
                msg = error.get("message") or (error.text or "")
                errors += 1
            elif skipped_el is not None:
                status = "skipped"
                msg = skipped_el.get("message") or (skipped_el.text or "")
                skipped += 1
            else:
                status = "passed"
                msg = ""
                passed += 1

            # Extract parametrized id from name like test_foo[param_id]
            bracket = name.find("[")
            test_id = name[bracket + 1:-1] if bracket >= 0 and name.endswith("]") else name

            cases.append({
                "test_id": test_id,
                "classname": classname,
                "test_function": name,
                "status": status,
                "duration_s": duration,
                "message": msg.strip()[:500],
            })

    return {
        "passed": passed,
        "skipped": skipped,
        "failed": failed,
        "errors": errors,
        "total": passed + skipped + failed + errors,
        "cases": cases,
    }


def _write_summary(summary: Dict[str, Any]) -> None:
    SUMMARY_JSON.write_text(
        json.dumps(summary, indent=2, default=str),
        encoding="utf-8",
    )

    # Human-readable .txt version
    lines: List[str] = []
    lines.append("=" * 72)
    lines.append("PDFE Phase-11 P0 Test Summary")
    lines.append("=" * 72)
    lines.append(f"Ran at: {summary.get('ran_at', '?')}")
    lines.append(f"Wall time: {summary.get('wall_time_s', 0):.1f}s")
    lines.append(f"Exit code: {summary.get('exit_code', '?')}")
    lines.append("")

    pytest_info = summary.get("pytest", {})
    lines.append("--- Pytest results ---")
    lines.append(f"  Total:   {pytest_info.get('total', 0)}")
    lines.append(f"  Passed:  {pytest_info.get('passed', 0)}")
    lines.append(f"  Skipped: {pytest_info.get('skipped', 0)}")
    lines.append(f"  Failed:  {pytest_info.get('failed', 0)}")
    lines.append(f"  Errors:  {pytest_info.get('errors', 0)}")
    lines.append("")

    matrix_info = summary.get("matrix", {})
    lines.append("--- Matrix P0 statuses ---")
    lines.append(f"  Total:   {matrix_info.get('total', 0)}")
    lines.append(f"  Green:   {matrix_info.get('green', 0)}")
    lines.append(f"  Red:     {matrix_info.get('red', 0)}")
    lines.append(f"  Blocked: {matrix_info.get('blocked', 0)}")
    lines.append(f"  Planned: {matrix_info.get('planned', 0)}")
    lines.append("")

    lines.append("--- By phase ---")
    for phase, counts in sorted(matrix_info.get("by_phase", {}).items()):
        green = counts.get("green", 0)
        red = counts.get("red", 0)
        blocked = counts.get("blocked", 0)
        lines.append(
            f"  {phase:8s}  green={green:3d}  red={red:3d}  blocked={blocked:3d}"
        )
    lines.append("")

    failures = [c for c in pytest_info.get("cases", []) if c["status"] in ("failed", "error")]
    if failures:
        lines.append("--- Failures ---")
        for c in failures:
            lines.append(f"  [{c['status']}] {c['test_function']}")
            if c.get("message"):
                lines.append(f"    {c['message'][:200]}")
        lines.append("")

    lines.append("--- Post-run checks ---")
    lines.append(f"  orphan pdfe.exe: {summary.get('orphan_pdfe', '?')}")
    lines.append(f"  matrix sync OK:  {summary.get('matrix_sync_ok', '?')}")
    lines.append("")

    SUMMARY_TXT.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="PDFE P0 runner")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    _kill_pdfe_stragglers()
    time.sleep(0.2)

    # Sanity: matrix P0 has no remaining 'planned' rows before we start.
    matrix_pre = _count_matrix_rows()
    pre_planned = matrix_pre.get("planned", 0)
    if pre_planned > 0:
        print(
            f"[run_p0] warning: {pre_planned} P0 rows have status 'planned' "
            "before the run starts. They will be updated during the run."
        )

    junit_xml = TESTS_DIR / "p0_junit.xml"
    try:
        junit_xml.unlink()
    except FileNotFoundError:
        pass

    print(f"[run_p0] executing P0 suite ({len(P0_TEST_FILES)} files)")
    for f in P0_TEST_FILES:
        print(f"         - {f}")
    run = _run_pytest(verbose=args.verbose, junit_xml=junit_xml)

    if not args.verbose:
        print(f"[run_p0] pytest exit code: {run['exit_code']}")
        print(f"[run_p0] wall time: {run['duration_s']:.1f}s")
        if run["stderr_tail"]:
            print(f"[run_p0] stderr tail:\n{run['stderr_tail']}")

    pytest_info = _parse_junit(junit_xml)
    matrix_post = _count_matrix_rows()

    orphan = _tasklist_pdfe_running()
    if orphan:
        print("[run_p0] orphan pdfe.exe detected, cleaning up")
        _kill_pdfe_stragglers()

    # Sync check: matrix green+red+blocked count must match pytest
    # cases. blocked matches pytest 'skipped' (phase conftest records
    # blocked on skip); green matches 'passed'; red matches 'failed'+'error'.
    matrix_green = matrix_post.get("green", 0)
    matrix_red = matrix_post.get("red", 0)
    matrix_blocked = matrix_post.get("blocked", 0)
    matrix_executed = matrix_green + matrix_red + matrix_blocked

    pytest_total = pytest_info.get("total", 0)
    matrix_sync_ok = (
        pytest_total == matrix_executed
        and matrix_post.get("planned", 0) == 0
    )

    summary = {
        "ran_at": time.strftime("%Y-%m-%d %H:%M:%S"),
        "wall_time_s": run["duration_s"],
        "exit_code": run["exit_code"],
        "pytest": pytest_info,
        "matrix": matrix_post,
        "orphan_pdfe": orphan,
        "matrix_sync_ok": matrix_sync_ok,
        "matrix_vs_pytest": {
            "matrix_executed": matrix_executed,
            "pytest_total": pytest_total,
            "delta": pytest_total - matrix_executed,
        },
    }
    _write_summary(summary)

    print(f"[run_p0] summary written:")
    print(f"         - {SUMMARY_JSON}")
    print(f"         - {SUMMARY_TXT}")
    print(f"[run_p0] matrix: green={matrix_green} red={matrix_red} "
          f"blocked={matrix_blocked} planned={matrix_post.get('planned',0)}")
    print(f"[run_p0] pytest: passed={pytest_info.get('passed',0)} "
          f"skipped={pytest_info.get('skipped',0)} "
          f"failed={pytest_info.get('failed',0)}")
    print(f"[run_p0] matrix sync OK: {matrix_sync_ok}")
    print(f"[run_p0] orphan pdfe.exe: {orphan}")

    # Exit non-zero if any P0 test failed OR planned rows remain OR orphans.
    if pytest_info.get("failed", 0) > 0 or pytest_info.get("errors", 0) > 0:
        return 1
    if matrix_post.get("planned", 0) > 0:
        return 2
    if orphan:
        return 3
    if not matrix_sync_ok:
        return 4
    return 0


if __name__ == "__main__":
    sys.exit(main())
