# -*- coding: utf-8 -*-
"""Focused runner for the pipeline e7d3c2a1 render-verification suite.

Runs every ``tests/ui_automation/test_render_*.py`` file (the oracle-backed
canvas render checks) in its own pytest subprocess — the same per-file
isolation contract as :mod:`run_ui_suite` (a single hung pywinauto/UIA call only
kills its own file's pytest, not the whole session) — and emits a merged JUnit
XML + summary JSON. This is the runner Phase 4's triage executes to produce the
PASS/XFAIL/FAIL/ERROR matrix.

The general ``run_ui_suite.py`` already globs ``tests/ui_automation/test_*.py``
and therefore also runs these files in the full UI sweep; this script is the
*focused* entry point for the render layer alone (faster Phase-4 iteration and a
self-contained artifact set). The heavy lifting (subprocess isolation, JUnit
merge, failure extraction) is imported from run_ui_suite so the two stay in
lock-step.

Outputs (under --output-dir):
  render-junit-merged.xml   — merged JUnit XML
  render-suite-summary.json — per-file stats + failures + hidden-problem files
  per-file/*.xml / *.log    — individual junit + stdout per file

Exit code: always 0 (failure detail lives in the artifacts, like run_ui_suite).
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.scripts.run_ui_suite import (  # noqa: E402
    run_one, merge_junits, collect_failures, portable_path,
    DEFAULT_FILE_TIMEOUT_S,
)


def collect_render_test_files(repo_root: Path) -> list[Path]:
    """Every tests/ui_automation/test_render_*.py file, sorted."""
    ua = repo_root / "tests" / "ui_automation"
    if not ua.is_dir():
        return []
    return sorted(ua.glob("test_render_*.py"))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo-root", type=Path, default=REPO_ROOT)
    ap.add_argument("--output-dir", required=True, type=Path)
    ap.add_argument("--pdfe-exe", default="")
    ap.add_argument("--file-timeout", type=int, default=DEFAULT_FILE_TIMEOUT_S)
    args = ap.parse_args()

    repo_root = args.repo_root.resolve()
    out_dir = args.output_dir.resolve()
    per_file_dir = out_dir / "per-file"
    per_file_dir.mkdir(parents=True, exist_ok=True)

    # Default to the Release binary the render layer targets.
    pdfe_exe = args.pdfe_exe or str(
        repo_root / "build" / "vs2022" / "bin" / "Release" / "pdfe.exe")

    files = collect_render_test_files(repo_root)
    print(f"[render-suite] discovered {len(files)} render test files")
    if not files:
        print("[render-suite] none found — exiting", file=sys.stderr)
        return 0

    results = []
    junits: list[Path] = []
    suite_start = time.monotonic()
    for i, f in enumerate(files, 1):
        rel = f.relative_to(repo_root)
        slug = str(rel).replace("\\", "__").replace("/", "__").replace(".py", "")
        junit_path = per_file_dir / f"{slug}.xml"
        log_path = per_file_dir / f"{slug}.log"
        print(f"[render-suite] ({i}/{len(files)}) {rel}", flush=True)
        result = run_one(f, junit_path, log_path,
                         args.file_timeout, repo_root, pdfe_exe)
        marker = "TIMEOUT" if result["timed_out"] else f"rc={result['returncode']}"
        print(f"[render-suite]   {marker} in {result['elapsed_s']}s", flush=True)
        results.append(result)
        junits.append(junit_path)

    suite_elapsed = time.monotonic() - suite_start
    merged_path = out_dir / "render-junit-merged.xml"
    totals = merge_junits(junits, merged_path)
    # F48: pass the roots so failure messages are scrubbed of host paths here
    # too. run_one() (imported above) already emits portable per-file paths --
    # the two runners share one definition of the rule rather than each
    # carrying a copy that can drift.
    failures = collect_failures(junits, repo_root, out_dir)
    files_with_problems = [
        r for r in results
        if r["timed_out"] or (r["returncode"] != 0 and r["returncode"] is not None)
    ]
    summary = {
        "files_run": len(results),
        "wall_clock_s": round(suite_elapsed, 1),
        "totals": totals,
        "failure_count": len(failures),
        "per_file": results,
        "failures": failures,
        "files_with_problems_count": len(files_with_problems),
        "files_with_problems": [
            {"file": p["file"], "rc": p["returncode"],
             "timed_out": p["timed_out"], "elapsed_s": p["elapsed_s"]}
            for p in files_with_problems
        ],
    }
    summary_path = out_dir / "render-suite-summary.json"
    with open(summary_path, "w", encoding="utf-8") as fh:
        json.dump(summary, fh, indent=2)

    print()
    print(f"[render-suite] files run:  {summary['files_run']}")
    print(f"[render-suite] wall clock: {summary['wall_clock_s']}s")
    print(f"[render-suite] tests:      {totals['tests']}")
    print(f"[render-suite] failures:   {totals['failures']}")
    print(f"[render-suite] errors:     {totals['errors']}")
    print(f"[render-suite] skipped:    {totals['skipped']}")
    print(f"[render-suite] merged junit: "
          f"{portable_path(merged_path, repo_root, out_dir)}")
    print(f"[render-suite] summary json: "
          f"{portable_path(summary_path, repo_root, out_dir)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
