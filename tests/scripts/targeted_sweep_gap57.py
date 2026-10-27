# -*- coding: utf-8 -*-
"""Targeted sweep of files affected by GAP-57 (toolbar action button
exposure) and GAP-58 (page panel context-menu QAction exposure).

Re-runs only those test files after the C++ fix to confirm whether
the cascade resolves. Same subprocess + tree-kill safeguards as
sweep_runner.py.
"""
from __future__ import annotations

import datetime as _dt
import json
import sys
from pathlib import Path

# Reuse helpers from the main sweep runner.
sys.path.insert(0, str(Path(__file__).parent))
from sweep_runner import (  # noqa: E402
    KNOWN_HANGERS, kill_orphans, parse_summary, run_one_file, ROOT,
)
import sweep_runner  # noqa: E402

# Override SWEEP_DIR for this targeted run.
sweep_runner.SWEEP_DIR = (
    ROOT / "tests" / "_runtime" / "sweeps"
    / f"targeted_gap57_{_dt.datetime.now().strftime('%Y%m%dT%H%M%S')}"
)
sweep_runner.SWEEP_DIR.mkdir(parents=True, exist_ok=True)
SWEEP_DIR = sweep_runner.SWEEP_DIR

# GAP-57 affected files: 97 of 224 failures.
# GAP-58 affected files: ~10 failures (page panel reachability).
TARGET_FILES = [
    "tests/ui_automation/test_precondition_panels_phase4.py",  # 40 F (GAP-57)
    "tests/ui_automation/test_annotations_p1.py",               # 26 F (GAP-57)
    "tests/ui_automation/test_text_formatting_p2.py",           # 24 F (GAP-57)
    "tests/ui_automation/test_canvas_phase3.py",                #  7 F (GAP-57)
    "tests/ui_automation/test_dialog_reachability_p0.py",       # 30 F (mixed; GAP-58 contributes)
    "tests/ui_automation/test_page_operations_p1.py",           # 23 F (mixed; GAP-58 contributes)
]


def main() -> int:
    print(f"[targeted] sweep dir: {SWEEP_DIR}", flush=True)
    kill_orphans()
    results = []
    totals = {"passed": 0, "failed": 0, "skipped": 0, "errors": 0,
              "xfailed": 0, "xpassed": 0, "crashed_files": 0}
    start = _dt.datetime.now()
    for i, rel in enumerate(TARGET_FILES, 1):
        path = ROOT / rel
        elapsed = (_dt.datetime.now() - start).total_seconds()
        print(f"[targeted {i}/{len(TARGET_FILES)}] (t+{int(elapsed)}s) {rel}",
              flush=True)
        r = run_one_file(path)
        results.append(r)
        for k in ("passed", "failed", "skipped", "errors",
                  "xfailed", "xpassed"):
            totals[k] += r.get(k, 0)
        if r["crashed"]:
            totals["crashed_files"] += 1
        print(f"  -> P:{r['passed']} F:{r['failed']} S:{r['skipped']} "
              f"E:{r['errors']} exit:{r['exit_code']} {r['duration_s']}s",
              flush=True)
        (SWEEP_DIR / "_running.json").write_text(
            json.dumps({"totals": totals, "results": results}, indent=2),
            encoding="utf-8",
        )
    totals["wall_clock_s"] = round(
        (_dt.datetime.now() - start).total_seconds(), 1
    )
    (SWEEP_DIR / "_final.json").write_text(
        json.dumps({"totals": totals, "results": results}, indent=2),
        encoding="utf-8",
    )
    print("\n[targeted] TOTALS:", json.dumps(totals, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
