# -*- coding: utf-8 -*-
"""Machine-check the F19 (meta-audit follow-up phase-19) disposition table.

The gap ids assigned to F19 are dispositioned in
``docs/audit/meta-audit-2026-07/followup/phase-19-dispositions.md``. This script
re-derives every checkable claim in that table FROM THE TREE, so no row is
believed on prose. It is deliberately non-circular: nothing it asserts is read
out of the document it is validating.

  * the id set in the table equals the id set assigned to F19 in
    ``docs/audit/meta-audit-2026-07/followup-pipeline-plan.md`` AND in the
    phase-19 entry of the pipeline's own ``pipeline.json``, and both are 58;
  * both counts (58 in the table, 58 assigned) are QUOTED in the document;
  * the CLOSED / WONT-FIX / OPEN figures quoted in the summary table match the
    rows beneath it, and sum to 58 -- five phases of this pipeline have failed
    a gate on exactly that mismatch;
  * every CLOSED row cites an artifact: either a test FILE plus a test NAME, or
    a corrected document path;
  * a cited test file exists, appears verbatim in ``tests/CMakeLists.txt`` (an
    unregistered test file silently never runs here -- the list is explicit,
    not a glob), and the cited test NAME is declared in it, is not
    ``DISABLED_``-prefixed and does not open with an unconditional
    ``GTEST_SKIP()``;
  * a cited document path exists;
  * the A5 STALE-OPEN ids enumerated in the document equal the set of F19 A5
    ids whose row in ``gap-register.md`` carries ``[STALE-OPEN]`` -- derived
    from the REGISTER, not from the dispositions file -- and every STALE-OPEN
    row in F19's scope (including the A4 one) carries a re-verification note;
  * every non-CLOSED row carries a substantive rationale (not a stub);
  * the guards this phase added are declared in
    ``tests/test_f19_doc_claim_guards.cpp``, are not ``DISABLED_``-prefixed, and
    every guard named in the mutation-log table is a real one.

Exit 0 only when there are zero failures. Writes the JSON evidence artifact to
the path given by --out (and --mirror, if supplied).
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DISPOSITIONS = (REPO / "docs" / "audit" / "meta-audit-2026-07" / "followup"
                / "phase-19-dispositions.md")
PLAN = (REPO / "docs" / "audit" / "meta-audit-2026-07"
        / "followup-pipeline-plan.md")
REGISTER = (REPO / "docs" / "audit" / "meta-audit-2026-07" / "gap-register.md")
PIPELINE_JSON = (REPO / ".claude" / "pipelines" / "meta-audit-followup"
                 / "pipeline.json")
CMAKE = REPO / "tests" / "CMakeLists.txt"
GUARDS = REPO / "tests" / "test_f19_doc_claim_guards.cpp"

EXPECTED_COUNT = 58

ID_RE = re.compile(r"A[45]-\d+")
ROW_RE = re.compile(r"^\|\s*(A[45]-\d+)\s*\|(.*)$")


def parse_table(text: str):
    rows = []
    for line in text.splitlines():
        m = ROW_RE.match(line.strip())
        if not m:
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        while len(cells) < 5:
            cells.append("")
        # A rationale may legitimately contain '|' (grep alternations), so the
        # trailing cells are rejoined rather than truncated -- otherwise a long,
        # well-evidenced rationale is scored as a stub purely for quoting a grep.
        rows.append({
            "id": m.group(1),
            "disposition": cells[1].replace("**", "").strip(),
            "test_file": cells[2].strip().strip("`").strip(),
            "test_name": cells[3].strip().strip("`").strip(),
            "rationale": "|".join(cells[4:]).strip(),
        })
    return rows


def f19_ids_from_plan() -> set:
    text = PLAN.read_text(encoding="utf-8")
    m = re.search(r"\*\*Gap ids closed \(58\):\*\*(.+)", text)
    return set(ID_RE.findall(m.group(1))) if m else set()


def f19_ids_from_pipeline() -> set:
    if not PIPELINE_JSON.exists():
        return set()
    data = json.loads(PIPELINE_JSON.read_text(encoding="utf-8"))
    for ph in data.get("phases", []):
        if str(ph.get("id")) == "phase-19":
            desc = ph.get("description", "")
            m = re.search(r"Gap ids closed \(58\):(.+)", desc, re.S)
            return set(ID_RE.findall(m.group(1) if m else desc))
    return set()


def stale_open_from_register(assigned: set) -> set:
    """F19 ids whose gap-register row is marked [STALE-OPEN]. Read from the
    REGISTER so the dispositions file cannot vouch for itself. Only the id-row
    form `| A4-NN |` is considered -- the correction annex uses `| **A4-NN** |`
    and is commentary, not a row."""
    out = set()
    for line in REGISTER.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.match(r"^\|\s*(A[45]-\d+)\s*\|", line)
        if not m:
            continue
        if m.group(1) in assigned and "[STALE-OPEN]" in line:
            out.add(m.group(1))
    return out


def a5_stale_declared(text: str):
    """The explicitly enumerated A5 STALE-OPEN list (criterion 6)."""
    m = re.search(r"^>\s*\*\*((?:A5-\d+)(?:,\s*A5-\d+)*)\*\*.*?count\s*=\s*\*\*(\d+)\*\*",
                  text, re.M | re.S)
    if not m:
        return set(), None
    return set(ID_RE.findall(m.group(1))), int(m.group(2))


def test_declared(path: Path, name: str):
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
    ap.add_argument("--mirror", type=Path)
    args = ap.parse_args()

    failures = []
    text = DISPOSITIONS.read_text(encoding="utf-8")
    rows = parse_table(text)
    table_ids = [r["id"] for r in rows]

    dupes = {i for i in table_ids if table_ids.count(i) > 1}
    if dupes:
        failures.append({"check": "duplicate-ids", "detail": sorted(dupes)})

    plan_ids = f19_ids_from_plan()
    pipe_ids = f19_ids_from_pipeline()
    assigned = plan_ids or pipe_ids
    if plan_ids and pipe_ids and plan_ids != pipe_ids:
        failures.append({"check": "plan-vs-pipeline-id-set",
                         "detail": sorted(plan_ids ^ pipe_ids)})
    if set(table_ids) != assigned:
        failures.append({
            "check": "table-id-set-equals-f19-assignment",
            "missing_from_table": sorted(assigned - set(table_ids)),
            "extra_in_table": sorted(set(table_ids) - assigned)})
    if len(table_ids) != EXPECTED_COUNT:
        failures.append({"check": "table-row-count", "detail": len(table_ids)})
    if len(assigned) != EXPECTED_COUNT:
        failures.append({"check": "assigned-id-count", "detail": len(assigned)})

    # Every id in the table must also have a row in the register.
    reg = REGISTER.read_text(encoding="utf-8", errors="replace")
    reg_ids = set(re.findall(r"^\|\s*(A[45]-\d+)\s*\|", reg, re.M))
    absent = sorted(set(table_ids) - reg_ids)
    if absent:
        failures.append({"check": "every-id-has-a-register-row", "detail": absent})

    # Both counts QUOTED (criterion 3).
    for phrase, label in (
            (r"ids assigned to F19 in the register / plan \| \*\*58\*\*",
             "assigned-count-quoted"),
            (r"rows in the disposition table below \| \*\*58\*\*",
             "table-count-quoted")):
        if not re.search(phrase, text):
            failures.append({"check": label,
                             "detail": "phrase not found in dispositions file"})

    # Quoted split must match the rows beneath it -- the recurring gate failure.
    counted = {}
    for r in rows:
        counted[r["disposition"].upper()] = counted.get(
            r["disposition"].upper(), 0) + 1
    for label, key in (("CLOSED", "CLOSED"), ("WONT-FIX", "WONT-FIX"),
                       ("OPEN", "OPEN")):
        # The label may sit anywhere in the summary cell ("confirmed OPEN
        # (evidence given...)"), and the lookbehind stops "OPEN" matching
        # inside "STALE-OPEN" elsewhere in the document.
        m = re.search(r"\|[^|\n]*(?<![-\w])%s\b[^|\n]*\|\s*\*\*(\d+)\*\*\s*\|"
                      % re.escape(label), text)
        quoted = int(m.group(1)) if m else None
        actual = counted.get(key, 0)
        if quoted is None:
            failures.append({"check": "split-figure-quoted", "label": label})
        elif quoted != actual:
            failures.append({"check": "split-figure-matches-rows",
                             "label": label, "quoted": quoted, "rows": actual})
    if sum(counted.values()) != EXPECTED_COUNT:
        failures.append({"check": "split-sums-to-total",
                         "detail": counted})

    # A5 STALE-OPEN enumeration, derived from the REGISTER (criterion 6).
    register_stale = stale_open_from_register(assigned)
    register_stale_a5 = {i for i in register_stale if i.startswith("A5-")}
    declared, declared_len = a5_stale_declared(text)
    if declared != register_stale_a5:
        failures.append({
            "check": "a5-stale-open-enumeration-matches-register",
            "declared": sorted(declared),
            "from_register": sorted(register_stale_a5)})
    if declared_len is None or declared_len != len(register_stale_a5):
        failures.append({"check": "a5-stale-open-count-quoted",
                         "declared": declared_len,
                         "from_register": len(register_stale_a5)})

    # Every STALE-OPEN row in scope (A4 one included) needs a re-verification note.
    by_id = {r["id"]: r for r in rows}
    for gid in sorted(register_stale):
        r = by_id.get(gid)
        if not r:
            failures.append({"check": "stale-open-row-present", "id": gid})
            continue
        if "re-verified" not in r["rationale"].lower():
            failures.append({"check": "stale-open-row-has-reverification-note",
                             "id": gid})

    cmake = CMAKE.read_text(encoding="utf-8")
    checked = []
    for r in rows:
        rec = dict(r)
        disp = r["disposition"].upper()
        rec["kind"] = disp if disp else "UNSPECIFIED"
        if disp == "CLOSED":
            cited_doc = bool(r["test_file"]) and not r["test_file"].endswith(".cpp")
            if not r["test_file"] or r["test_file"] == "—":
                failures.append({"check": "closed-row-has-citation", "id": r["id"]})
                rec["ok"] = False
                checked.append(rec)
                continue
            path = REPO / r["test_file"]
            rec["artifact_exists"] = path.exists()
            if cited_doc:
                # CLOSED on a corrected document path.
                rec["citation_kind"] = "document"
                rec["ok"] = rec["artifact_exists"]
            else:
                rec["citation_kind"] = "test"
                rec["registered_in_cmake"] = Path(r["test_file"]).name in cmake
                if not r["test_name"] or r["test_name"] == "—":
                    failures.append({"check": "closed-on-test-needs-a-test-name",
                                     "id": r["id"]})
                    rec["ok"] = False
                    checked.append(rec)
                    continue
                if path.exists():
                    decl, dis, blank = test_declared(path, r["test_name"])
                else:
                    decl = dis = blank = False
                rec["test_declared"] = decl
                rec["disabled_prefixed"] = dis
                rec["blanket_skip_guarded"] = blank
                rec["ok"] = (rec["artifact_exists"] and rec["registered_in_cmake"]
                             and decl and not dis and not blank)
            if not rec["ok"]:
                failures.append({"check": "closed-citation-verifies",
                                 "id": r["id"], "record": rec})
        else:
            if rec["kind"] not in ("OPEN", "WONT-FIX", "UNVERIFIABLE", "DEFERRED"):
                failures.append({"check": "disposition-is-a-known-verdict",
                                 "id": r["id"], "detail": r["disposition"]})
            rec["ok"] = len(r["rationale"]) >= 200
            if not rec["ok"]:
                failures.append({"check": "non-closed-row-has-substantive-rationale",
                                 "id": r["id"],
                                 "rationale_len": len(r["rationale"])})
        checked.append(rec)

    # The guards this phase added must be real and enabled.
    guards = []
    if not GUARDS.exists():
        failures.append({"check": "guard-file-exists", "detail": str(GUARDS)})
    else:
        gsrc = GUARDS.read_text(encoding="utf-8", errors="replace")
        declared_guards = set(re.findall(r"\bTEST\(\s*F19DocClaims\s*,\s*(\w+)\s*\)", gsrc))
        if GUARDS.name not in cmake:
            failures.append({"check": "guard-file-registered-in-cmake"})
        for name in sorted(declared_guards):
            dis = name.startswith("DISABLED_")
            body_m = re.search(r"\bTEST\(\s*F19DocClaims\s*,\s*%s\s*\)" % name, gsrc)
            blank = bool(re.match(r"\s*\{\s*GTEST_SKIP\(\)",
                                  gsrc[body_m.end():body_m.end() + 400]))
            guards.append({"guard": name, "disabled": dis,
                           "blanket_skip_guarded": blank, "ok": not dis and not blank})
            if dis or blank:
                failures.append({"check": "guard-is-enabled", "guard": name})
        # Every guard named in the mutation-log table must exist.
        for named in set(re.findall(r"^\|\s*\d+\s*\|\s*`(\w+)`\s*\|", text, re.M)):
            if named not in declared_guards:
                failures.append({"check": "mutation-log-names-a-real-guard",
                                 "guard": named})

    artifact = {
        "phase": "phase-19",
        "generator": "tools/gate/verify_f19_dispositions.py",
        "dispositions_file": str(DISPOSITIONS.relative_to(REPO)).replace("\\", "/"),
        "counts": {
            "ids_in_table": len(table_ids),
            "ids_assigned_to_f19": len(assigned),
            "closed": counted.get("CLOSED", 0),
            "wont_fix": counted.get("WONT-FIX", 0),
            "open": counted.get("OPEN", 0),
            "stale_open_in_scope_from_register": len(register_stale),
            "stale_open_a5_from_register": len(register_stale_a5),
        },
        "stale_open_ids_in_scope": sorted(register_stale),
        "stale_open_a5_ids": sorted(register_stale_a5),
        "guards": guards,
        "failures": failures,
        "rows": checked,
    }
    for target in (args.out, args.mirror):
        if target:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(json.dumps(artifact, indent=2), encoding="utf-8")

    print("ids in table: %d  assigned to F19: %d" % (len(table_ids), len(assigned)))
    print("CLOSED: %d  WONT-FIX: %d  OPEN: %d"
          % (counted.get("CLOSED", 0), counted.get("WONT-FIX", 0),
             counted.get("OPEN", 0)))
    print("STALE-OPEN in scope: %d %s" % (len(register_stale), sorted(register_stale)))
    print("A5 STALE-OPEN: %d %s" % (len(register_stale_a5), sorted(register_stale_a5)))
    print("guards: %d" % len(guards))
    print("failures: %d" % len(failures))
    for f in failures:
        print("  FAIL", json.dumps(f)[:300])
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
