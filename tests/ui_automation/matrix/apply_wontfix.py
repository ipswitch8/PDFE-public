# -*- coding: utf-8 -*-
"""Apply the wont-fix bucket from blocked_triage.csv to ui_test_matrix.csv.

For every matrix row whose test_id is in the wont-fix bucket:
  - change status from "blocked" to "wont-fix"
  - replace notes with the triage reason + a backlog marker

Leaves other buckets alone (they're handled in later phases).
"""
from __future__ import annotations
import csv
from pathlib import Path

TRIAGE = Path("tests/ui_automation/matrix/blocked_triage.csv")
MATRIX = Path("tests/ui_automation/matrix/ui_test_matrix.csv")


def main() -> int:
    # Load triage — skip comment lines.
    with TRIAGE.open("r", encoding="utf-8") as f:
        lines = [l for l in f if not l.startswith("#")]
    triage_rows = list(csv.DictReader(lines))
    wontfix = {r["test_id"]: r["reason"]
               for r in triage_rows if r["bucket"] == "wont-fix"}
    print(f"wont-fix entries in triage: {len(wontfix)}")

    # Load matrix
    with MATRIX.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        rows = list(reader)
        fieldnames = reader.fieldnames

    updated = 0
    for r in rows:
        if r["status"] != "blocked":
            continue
        tid = r["test_id"]
        if tid not in wontfix:
            continue
        r["status"] = "wont-fix"
        reason = wontfix[tid]
        existing = (r.get("notes") or "").strip()
        # Preserve the existing notes but prepend the wont-fix reason
        r["notes"] = f"[wont-fix] {reason}; {existing}"[:500]
        updated += 1

    print(f"Rows updated to wont-fix: {updated}")

    if updated != len(wontfix):
        missing = set(wontfix) - {r["test_id"] for r in rows}
        print(f"Missing in matrix: {missing}")

    with MATRIX.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        for r in rows:
            w.writerow(r)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
