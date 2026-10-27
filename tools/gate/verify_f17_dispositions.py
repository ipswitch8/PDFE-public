# -*- coding: utf-8 -*-
"""Machine-check the F17 (meta-audit follow-up phase-17) disposition table.

Gap A1-* ids assigned to F17 are dispositioned in
``docs/audit/meta-audit-2026-07/followup/phase-17-dispositions.md``. This script
re-derives every checkable claim in that table from the tree, so no row is
believed on prose:

  * the id set in the table equals the id set assigned to F17 in
    ``docs/audit/meta-audit-2026-07/followup-pipeline-plan.md`` (and the phase
    entry in the pipeline's own pipeline.json), and both are 50;
  * every CLOSED row cites a test FILE and a test NAME;
  * the cited file exists;
  * the cited file appears verbatim in ``tests/CMakeLists.txt`` (an
    unregistered test file silently never runs here -- the list is explicit,
    not a glob);
  * the cited test NAME is declared in that file, is not ``DISABLED_``-prefixed
    and its body is not opened by an unconditional ``GTEST_SKIP()``;
  * every WONT-FIX row carries a non-trivial rationale.

Exit 0 only when there are zero failures. Writes the JSON evidence artifact to
the path given by --out.

Usage:
  python tools/gate/verify_f17_dispositions.py \
      --out .claude/pipelines/meta-audit-followup/gate-artifacts/phase-17/closed-test-verification.json
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DISPOSITIONS = (REPO / "docs" / "audit" / "meta-audit-2026-07" / "followup"
                / "phase-17-dispositions.md")
PLAN = (REPO / "docs" / "audit" / "meta-audit-2026-07"
        / "followup-pipeline-plan.md")
PIPELINE_JSON = (REPO / ".claude" / "pipelines" / "meta-audit-followup"
                 / "pipeline.json")
CMAKE = REPO / "tests" / "CMakeLists.txt"

ROW_RE = re.compile(r"^\|\s*(A1-\d+)\s*\|(.*)$")


def parse_table(text: str):
    """Yield (id, disposition, test_file, test_name, rationale) per table row."""
    rows = []
    for line in text.splitlines():
        m = ROW_RE.match(line.strip())
        if not m:
            continue
        gid = m.group(1)
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        # id | disposition | test file | test name | evidence/rationale
        while len(cells) < 5:
            cells.append("")
        rows.append({
            "id": gid,
            "disposition": cells[1].replace("**", "").strip(),
            "test_file": cells[2].strip().strip("`"),
            "test_name": cells[3].strip().strip("`"),
            "rationale": cells[4].strip(),
        })
    return rows


def f17_ids_from_plan() -> set:
    text = PLAN.read_text(encoding="utf-8")
    m = re.search(r"\*\*Gap ids closed \(50\):\*\*(.+)", text)
    if not m:
        return set()
    return set(re.findall(r"A1-\d+", m.group(1)))


def f17_ids_from_pipeline() -> set:
    if not PIPELINE_JSON.exists():
        return set()
    data = json.loads(PIPELINE_JSON.read_text(encoding="utf-8"))
    for ph in data.get("phases", []):
        if str(ph.get("id")) == "phase-17":
            return set(re.findall(r"A1-\d+", ph.get("description", "")))
    return set()


def test_declared(path: Path, name: str):
    """Return (declared, disabled, blanket_skipped) for Suite.Test in path."""
    if "." not in name:
        return False, False, False
    suite, case = name.split(".", 1)
    src = path.read_text(encoding="utf-8", errors="replace")
    decl = re.compile(r"\bTEST(?:_F|_P)?\(\s*%s\s*,\s*%s\s*\)"
                      % (re.escape(suite), re.escape(case)))
    m = decl.search(src)
    if not m:
        return False, False, False
    disabled = case.startswith("DISABLED_") or suite.startswith("DISABLED_")
    # Unconditional GTEST_SKIP as the first statement of the body.
    body = src[m.end():m.end() + 400]
    blanket = bool(re.match(r"\s*\{\s*GTEST_SKIP\(\)", body))
    return True, disabled, blanket


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()

    failures = []
    rows = parse_table(DISPOSITIONS.read_text(encoding="utf-8"))
    table_ids = [r["id"] for r in rows]
    dupes = {i for i in table_ids if table_ids.count(i) > 1}
    if dupes:
        failures.append({"check": "duplicate-ids", "detail": sorted(dupes)})

    plan_ids = f17_ids_from_plan()
    pipe_ids = f17_ids_from_pipeline()
    assigned = plan_ids or pipe_ids
    if plan_ids and pipe_ids and plan_ids != pipe_ids:
        failures.append({"check": "plan-vs-pipeline-id-set",
                         "detail": sorted(plan_ids ^ pipe_ids)})
    if set(table_ids) != assigned:
        failures.append({
            "check": "table-id-set-equals-f17-assignment",
            "missing_from_table": sorted(assigned - set(table_ids)),
            "extra_in_table": sorted(set(table_ids) - assigned)})
    if len(table_ids) != 50:
        failures.append({"check": "table-row-count", "detail": len(table_ids)})
    if len(assigned) != 50:
        failures.append({"check": "assigned-id-count", "detail": len(assigned)})

    cmake = CMAKE.read_text(encoding="utf-8")
    checked = []
    for r in rows:
        rec = dict(r)
        disp = r["disposition"].upper()
        if disp.startswith("CLOSED"):
            rec["kind"] = "CLOSED"
            if not r["test_file"] or not r["test_name"]:
                failures.append({"check": "closed-row-has-citation",
                                 "id": r["id"]})
                rec["ok"] = False
                checked.append(rec)
                continue
            path = REPO / r["test_file"]
            rec["file_exists"] = path.exists()
            base = Path(r["test_file"]).name
            rec["registered_in_cmake"] = base in cmake
            if path.exists():
                declared, disabled, blanket = test_declared(path,
                                                            r["test_name"])
            else:
                declared = disabled = blanket = False
            rec["test_declared"] = declared
            rec["disabled_prefixed"] = disabled
            rec["blanket_skip_guarded"] = blanket
            rec["ok"] = (rec["file_exists"] and rec["registered_in_cmake"]
                         and declared and not disabled and not blanket)
            if not rec["ok"]:
                failures.append({"check": "closed-citation-verifies",
                                 "id": r["id"], "record": rec})
        else:
            rec["kind"] = "WONT-FIX"
            rec["ok"] = len(r["rationale"]) >= 80
            if not rec["ok"]:
                failures.append({"check": "wontfix-has-substantive-rationale",
                                 "id": r["id"],
                                 "rationale_len": len(r["rationale"])})
        checked.append(rec)

    closed = [c for c in checked if c["kind"] == "CLOSED"]
    wontfix = [c for c in checked if c["kind"] == "WONT-FIX"]

    artifact = {
        "phase": "phase-17",
        "generator": "tools/gate/verify_f17_dispositions.py",
        "dispositions_file": str(DISPOSITIONS.relative_to(REPO)).replace("\\", "/"),
        "counts": {
            "ids_in_table": len(table_ids),
            "ids_assigned_to_f17": len(assigned),
            "closed": len(closed),
            "wont_fix": len(wontfix),
        },
        "failures": failures,
        "rows": checked,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(artifact, indent=2), encoding="utf-8")

    print(f"ids in table: {len(table_ids)}  assigned to F17: {len(assigned)}")
    print(f"CLOSED: {len(closed)}  WONT-FIX: {len(wontfix)}")
    print(f"failures: {len(failures)}")
    for f in failures:
        print("  FAIL", json.dumps(f)[:300])
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
