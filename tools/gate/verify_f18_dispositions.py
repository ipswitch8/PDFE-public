# -*- coding: utf-8 -*-
"""Machine-check the F18 (meta-audit follow-up phase-18) disposition table.

The gap ids assigned to F18 are dispositioned in
``docs/audit/meta-audit-2026-07/followup/phase-18-dispositions.md``. This script
re-derives every checkable claim in that table FROM THE TREE, so no row is
believed on prose. It is deliberately non-circular: nothing it asserts is read
out of the document it is validating.

  * the id set in the table equals the id set assigned to F18 in
    ``docs/audit/meta-audit-2026-07/followup-pipeline-plan.md`` AND in the
    phase-18 entry of the pipeline's own ``pipeline.json``, and both are 32;
  * both counts (32 in the table, 32 assigned) are QUOTED in the document;
  * every CLOSED row cites a test FILE and a test NAME;
  * the cited file exists;
  * the cited file appears verbatim in ``tests/CMakeLists.txt`` (an unregistered
    test file silently never runs here -- the list is explicit, not a glob);
  * the cited test NAME is declared in that file, is not ``DISABLED_``-prefixed
    and its body is not opened by an unconditional ``GTEST_SKIP()``;
  * the STALE-OPEN ids enumerated in the document equal the set of F18 ids whose
    row in ``gap-register.md`` carries ``[STALE-OPEN]`` -- derived from the
    REGISTER, not from the dispositions file -- and every one of them carries a
    re-verification note;
  * every non-CLOSED row carries a substantive rationale (not a stub);
  * the B1 and B2 regression locks named in the document are declared in
    ``tests/test_generative_invariants.cpp`` and are NOT ``DISABLED_``-prefixed.

Exit 0 only when there are zero failures. Writes the JSON evidence artifact to
the path given by --out.

Usage:
  python tools/gate/verify_f18_dispositions.py \
      --out .claude/pipelines/meta-audit-followup/gate-artifacts/phase-18/closed-test-verification.json
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DISPOSITIONS = (REPO / "docs" / "audit" / "meta-audit-2026-07" / "followup"
                / "phase-18-dispositions.md")
PLAN = (REPO / "docs" / "audit" / "meta-audit-2026-07"
        / "followup-pipeline-plan.md")
REGISTER = (REPO / "docs" / "audit" / "meta-audit-2026-07" / "gap-register.md")
PIPELINE_JSON = (REPO / ".claude" / "pipelines" / "meta-audit-followup"
                 / "pipeline.json")
CMAKE = REPO / "tests" / "CMakeLists.txt"
GENERATIVE = REPO / "tests" / "test_generative_invariants.cpp"

EXPECTED_COUNT = 32

# An F18 id is either the tracker item #39 or an A3-NN id.
ID_RE = re.compile(r"(#39|A3-\d+)")
ROW_RE = re.compile(r"^\|\s*(#39|A3-\d+)\s*\|(.*)$")

B1_B2_TESTS = [
    "GenerativeInvariants.B1_RotateThenEditTextUndoRestoresText",
    "GenerativeInvariants.B1_RotateThenEditTextUndoRestoresText_180",
    "GenerativeInvariants.B1_RotateThenEditTextUndoRestoresText_270",
    "GenerativeInvariants.B2_PageStructUndoRedoSaveAsNoCrash",
]


def parse_table(text: str):
    """Yield one record per disposition-table row."""
    rows = []
    for line in text.splitlines():
        m = ROW_RE.match(line.strip())
        if not m:
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        while len(cells) < 5:
            cells.append("")
        rows.append({
            "id": m.group(1),
            "disposition": cells[1].replace("**", "").strip(),
            "test_file": cells[2].strip().strip("`"),
            "test_name": cells[3].strip().strip("`"),
            "rationale": cells[4].strip(),
        })
    return rows


def f18_ids_from_plan() -> set:
    text = PLAN.read_text(encoding="utf-8")
    m = re.search(r"\*\*Gap ids closed \(32\):\*\*(.+)", text)
    if not m:
        return set()
    return set(ID_RE.findall(m.group(1)))


def f18_ids_from_pipeline() -> set:
    if not PIPELINE_JSON.exists():
        return set()
    data = json.loads(PIPELINE_JSON.read_text(encoding="utf-8"))
    for ph in data.get("phases", []):
        if str(ph.get("id")) == "phase-18":
            desc = ph.get("description", "")
            # The description's "Gap ids closed (32):" list is the assignment.
            m = re.search(r"Gap ids closed \(32\):(.+)", desc, re.S)
            return set(ID_RE.findall(m.group(1) if m else desc))
    return set()


def stale_open_ids_from_register(assigned: set) -> set:
    """F18 ids whose gap-register row is marked [STALE-OPEN]. Read from the
    REGISTER so the dispositions file cannot vouch for itself."""
    out = set()
    for line in REGISTER.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if not cells:
            continue
        gid = cells[0].strip("`")
        if gid in assigned and "[STALE-OPEN]" in line:
            out.add(gid)
    return out


def stale_open_ids_declared(text: str) -> set:
    """The explicitly enumerated STALE-OPEN list in the dispositions file."""
    m = re.search(r"^>\s*\*\*((?:#39|A3-\d+)(?:,\s*(?:#39|A3-\d+))*)\*\*.*?count\s*=\s*\*\*(\d+)\*\*",
                  text, re.M | re.S)
    if not m:
        return set(), None
    return set(ID_RE.findall(m.group(1))), int(m.group(2))


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
    body = src[m.end():m.end() + 400]
    blanket = bool(re.match(r"\s*\{\s*GTEST_SKIP\(\)", body))
    return True, disabled, blanket


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()

    failures = []
    text = DISPOSITIONS.read_text(encoding="utf-8")
    rows = parse_table(text)
    table_ids = [r["id"] for r in rows]

    dupes = {i for i in table_ids if table_ids.count(i) > 1}
    if dupes:
        failures.append({"check": "duplicate-ids", "detail": sorted(dupes)})

    plan_ids = f18_ids_from_plan()
    pipe_ids = f18_ids_from_pipeline()
    assigned = plan_ids or pipe_ids
    if plan_ids and pipe_ids and plan_ids != pipe_ids:
        failures.append({"check": "plan-vs-pipeline-id-set",
                         "detail": sorted(plan_ids ^ pipe_ids)})
    if set(table_ids) != assigned:
        failures.append({
            "check": "table-id-set-equals-f18-assignment",
            "missing_from_table": sorted(assigned - set(table_ids)),
            "extra_in_table": sorted(set(table_ids) - assigned)})
    if len(table_ids) != EXPECTED_COUNT:
        failures.append({"check": "table-row-count", "detail": len(table_ids)})
    if len(assigned) != EXPECTED_COUNT:
        failures.append({"check": "assigned-id-count", "detail": len(assigned)})

    # Both counts must be QUOTED in the file (acceptance criterion 4).
    for phrase, label in ((r"ids assigned to F18 in the register / plan \| \*\*32\*\*",
                           "assigned-count-quoted"),
                          (r"rows in the disposition table below \| \*\*32\*\*",
                           "table-count-quoted")):
        if not re.search(phrase, text):
            failures.append({"check": label,
                             "detail": "phrase not found in dispositions file"})

    # STALE-OPEN enumeration, derived from the REGISTER.
    register_stale = stale_open_ids_from_register(assigned)
    declared_stale, declared_len = stale_open_ids_declared(text)
    if declared_stale != register_stale:
        failures.append({
            "check": "stale-open-enumeration-matches-register",
            "declared": sorted(declared_stale),
            "from_register": sorted(register_stale)})
    if declared_len is None or declared_len != len(register_stale):
        failures.append({"check": "stale-open-count-quoted",
                         "declared": declared_len,
                         "from_register": len(register_stale)})

    # Every STALE-OPEN row must carry a re-verification note.
    by_id = {r["id"]: r for r in rows}
    for gid in sorted(register_stale):
        r = by_id.get(gid)
        if not r:
            failures.append({"check": "stale-open-row-present", "id": gid})
            continue
        if "STALE-OPEN re-verified" not in r["rationale"]:
            failures.append({"check": "stale-open-row-has-reverification-note",
                             "id": gid})

    cmake = CMAKE.read_text(encoding="utf-8")
    checked = []
    for r in rows:
        rec = dict(r)
        disp = r["disposition"].upper()
        if disp.startswith("CLOSED"):
            rec["kind"] = "CLOSED"
            if not r["test_file"] or not r["test_name"]:
                failures.append({"check": "closed-row-has-citation", "id": r["id"]})
                rec["ok"] = False
                checked.append(rec)
                continue
            path = REPO / r["test_file"]
            rec["file_exists"] = path.exists()
            rec["registered_in_cmake"] = Path(r["test_file"]).name in cmake
            if path.exists():
                declared, disabled, blanket = test_declared(path, r["test_name"])
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
            rec["kind"] = disp if disp else "UNSPECIFIED"
            if rec["kind"] not in ("OPEN", "WONT-FIX", "UNVERIFIABLE", "DEFERRED"):
                failures.append({"check": "disposition-is-a-known-verdict",
                                 "id": r["id"], "detail": r["disposition"]})
            rec["ok"] = len(r["rationale"]) >= 120
            if not rec["ok"]:
                failures.append({"check": "non-closed-row-has-substantive-rationale",
                                 "id": r["id"],
                                 "rationale_len": len(r["rationale"])})
        checked.append(rec)

    # B1 / B2: the locks the document says were removed must really be enabled.
    b1b2 = []
    for name in B1_B2_TESTS:
        declared, disabled, blanket = test_declared(GENERATIVE, name)
        ok = declared and not disabled and not blanket
        b1b2.append({"test": name, "declared": declared, "disabled": disabled,
                     "blanket_skip_guarded": blanket, "ok": ok})
        if not ok:
            failures.append({"check": "b1b2-lock-removed-and-enabled",
                             "test": name})

    closed = [c for c in checked if c["kind"] == "CLOSED"]
    open_rows = [c for c in checked if c["kind"] == "OPEN"]
    unverifiable = [c for c in checked if c["kind"] == "UNVERIFIABLE"]

    artifact = {
        "phase": "phase-18",
        "generator": "tools/gate/verify_f18_dispositions.py",
        "dispositions_file": str(DISPOSITIONS.relative_to(REPO)).replace("\\", "/"),
        "counts": {
            "ids_in_table": len(table_ids),
            "ids_assigned_to_f18": len(assigned),
            "closed": len(closed),
            "open": len(open_rows),
            "unverifiable": len(unverifiable),
            "stale_open_from_register": len(register_stale),
        },
        "stale_open_ids": sorted(register_stale),
        "b1_b2_locks": b1b2,
        "failures": failures,
        "rows": checked,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(artifact, indent=2), encoding="utf-8")

    print("ids in table: %d  assigned to F18: %d" % (len(table_ids), len(assigned)))
    print("CLOSED: %d  OPEN: %d  UNVERIFIABLE: %d"
          % (len(closed), len(open_rows), len(unverifiable)))
    print("STALE-OPEN (from register): %d %s"
          % (len(register_stale), sorted(register_stale)))
    print("failures: %d" % len(failures))
    for f in failures:
        print("  FAIL", json.dumps(f)[:300])
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
