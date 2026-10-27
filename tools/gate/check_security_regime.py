#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Ratchet the security regime's WIRING against what `docs/security-testing.md` claims.

WHY THIS EXISTS (GitHub #201).

`docs/audit/audit-of-audits-2026-08.md` §4 said "a whole security-testing regime
was never wired". GitHub #145 fixed part of it by adding `secrets-audit.yml` and
`dependency-audit.yml`. #201 is the residual, and it is a DIFFERENT shape: the
scripts exist, the docs describe them in the present tense, and nothing runs
them. `scripts/security/run_sast.ps1` is invoked by nothing at all;
`pdfe_security_nightly` is invoked by zero workflows; both were described by
prose that read as coverage.

The lesson this repository keeps re-learning is that a true sentence with no
mechanism behind it rots exactly the way the false one did (see the header of
`.github/workflows/windows-validation.yml`, which says so about itself). So the
disposition of every security dispatcher is not left as prose: it is a table in
`docs/security-testing.md` that THIS CHECKER compares against the tree.

It fails in BOTH directions, which is the point:

  * a dispatcher with no table row              -> FAIL (new script, undisclosed)
  * a row claiming an invoker that does not     -> FAIL (prose says covered,
    exist                                          nothing runs it)
  * a row saying NONE for a script that IS      -> FAIL (stale pessimism; the
    wired                                          reader under-trusts a real
                                                   control and the table stops
                                                   being read at all)
  * a NONE row with no reason text              -> FAIL
  * a frozen-baseline list that does not match  -> FAIL (a baseline added or
    tests/security/regression_baselines/           removed without disclosure)

WHAT IT DOES NOT DO. It does not run any scanner, does not judge whether a lane
is a GOOD idea, and does not assert that a dispatch-only lane has ever been
dispatched. It asserts only that the written disposition matches the wiring.

Usage:
    python tools/gate/check_security_regime.py --check
    python tools/gate/check_security_regime.py --self-test

Exit codes: 0 = disposition table matches the tree, 1 = it does not,
2 = the table could not be located or parsed (itself a failure to report).
"""
from __future__ import annotations

import argparse
import io
import re
import sys
import tempfile
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

REPO_ROOT = Path(__file__).resolve().parents[2]

DOC_REL = "docs/security-testing.md"
SCRIPT_DIR_REL = "scripts/security"
WORKFLOW_DIR_REL = ".github/workflows"
SECURITY_CMAKE_REL = "tests/security/CMakeLists.txt"
BASELINE_DIR_REL = "tests/security/regression_baselines"
LOCAL_GATE_REL = "tools/gate/run_security_regression_check.py"
AUTO_WORKFLOW = "fast-checks.yml"

TABLE_BEGIN = "<!-- BEGIN security-regime-dispositions -->"
TABLE_END = "<!-- END security-regime-dispositions -->"
FROZEN_BEGIN = "<!-- BEGIN frozen-baselines -->"
FROZEN_END = "<!-- END frozen-baselines -->"

# The vocabulary a row may use. Each maps to a computed fact, so a row cannot
# claim something this checker is unable to verify.
STATUS_AUTO = "AUTO"            # reachable from the one auto-triggered workflow
STATUS_DISPATCH = "DISPATCH"    # reachable only from a workflow_dispatch lane
STATUS_CTEST = "CTEST"          # registered as a CTest entry
STATUS_CMAKE = "CMAKE-TARGET"   # driven by a custom CMake target
STATUS_LOCAL = "LOCAL-GATE"     # driven by a gate script the local sweep runs
STATUS_NONE = "NONE"            # nothing invokes it
ALL_STATUSES = {STATUS_AUTO, STATUS_DISPATCH, STATUS_CTEST, STATUS_CMAKE,
                STATUS_LOCAL, STATUS_NONE}


def _read(path: Path) -> str:
    try:
        return io.open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return ""


def _strip_comments(text: str, marker: str = "#") -> str:
    """Drop whole-line comments.

    LOAD-BEARING. `secrets-audit.yml` names `check_regressions.ps1` in its
    header comment precisely to say it is deliberately NOT wired there, and
    `dependency-audit.yml` names `run_fuzz.ps1` the same way. A reference
    counter that reads comments concludes both are wired to a lane that does
    not run them -- inventing exactly the false coverage this file exists to
    detect. Verified: with comments counted, check_regressions.ps1 computes as
    DISPATCH(secrets-audit.yml); with them stripped, it computes as CTEST.
    """
    out = []
    for line in text.splitlines():
        s = line.lstrip()
        if s.startswith(marker):
            continue
        out.append(line)
    return "\n".join(out)


def _balanced(text: str, open_idx: int) -> str:
    """The text between `text[open_idx] == '('` and its matching ')'.

    A fixed-size window was the first implementation and was WRONG: the
    `add_test(security_regression_check ...)` call sits ~700 characters above
    `add_custom_target(pdfe_security_nightly ... run_all.ps1 ...)` in
    tests/security/CMakeLists.txt, so a 1200-char window reported run_all.ps1
    as CTEST+CMAKE-TARGET -- crediting a script with a CTest registration it
    does not have. Exactly the over-claim this file exists to catch, committed
    by the checker itself.
    """
    depth = 0
    for i in range(open_idx, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return text[open_idx:i + 1]
    return text[open_idx:]


def discover_scripts(root: Path) -> list[str]:
    """Every PowerShell dispatcher under scripts/security, repo-relative.

    Discovery, not a frozen list: a dispatcher added tomorrow with no table row
    fails this checker rather than joining the set of things nobody runs.
    """
    d = root / SCRIPT_DIR_REL
    if not d.is_dir():
        return []
    return sorted(f"{SCRIPT_DIR_REL}/{p.name}" for p in d.glob("*.ps1"))


def compute_status(root: Path, script_rel: str) -> tuple[set[str], list[str]]:
    """(statuses, human-readable invokers) for one dispatcher."""
    name = Path(script_rel).name
    statuses: set[str] = set()
    invokers: list[str] = []

    wf_dir = root / WORKFLOW_DIR_REL
    if wf_dir.is_dir():
        for wf in sorted(wf_dir.glob("*.yml")):
            if name not in _strip_comments(_read(wf)):
                continue
            invokers.append(f"{WORKFLOW_DIR_REL}/{wf.name}")
            statuses.add(STATUS_AUTO if wf.name == AUTO_WORKFLOW
                         else STATUS_DISPATCH)

    cmake = root / SECURITY_CMAKE_REL
    cmake_text = _strip_comments(_read(cmake))
    if name in cmake_text:
        invokers.append(SECURITY_CMAKE_REL)
        # add_test(...) vs add_custom_target(...): both are real invokers but
        # they are reached differently, and the table says which.
        for block, status in ((r"add_test\s*\(", STATUS_CTEST),
                              (r"add_custom_target\s*\(", STATUS_CMAKE)):
            for m in re.finditer(block, cmake_text):
                body = _balanced(cmake_text, m.end() - 1)
                if name in body:
                    statuses.add(status)

    local_gate = root / LOCAL_GATE_REL
    if local_gate.exists() and name in _strip_comments(_read(local_gate)):
        invokers.append(LOCAL_GATE_REL)
        statuses.add(STATUS_LOCAL)

    if not statuses:
        statuses.add(STATUS_NONE)
    return statuses, invokers


_ROW_RE = re.compile(
    r"^\|\s*`([^`]+)`\s*\|\s*([A-Za-z0-9 ,+-]+?)\s*\|\s*(.*?)\s*\|\s*$")


def parse_table(doc_text: str) -> tuple[dict[str, tuple[set[str], str]], str]:
    """Rows of the disposition table: {script_rel: (statuses, reason)}."""
    if TABLE_BEGIN not in doc_text or TABLE_END not in doc_text:
        return {}, (f"{DOC_REL} has no {TABLE_BEGIN} / {TABLE_END} block; the "
                    f"disposition table this checker ratchets is missing")
    body = doc_text.split(TABLE_BEGIN, 1)[1].split(TABLE_END, 1)[0]
    rows: dict[str, tuple[set[str], str]] = {}
    for line in body.splitlines():
        m = _ROW_RE.match(line.strip())
        if not m:
            continue
        target, status_text, reason = m.group(1), m.group(2), m.group(3)
        statuses = {s.strip() for s in status_text.split("+") if s.strip()}
        rows[target] = (statuses, reason.strip())
    if not rows:
        return {}, (f"{DOC_REL} disposition block parsed to zero rows; a table "
                    f"nothing can be read out of ratchets nothing")
    return rows, ""


def parse_frozen(doc_text: str) -> tuple[set[str], str]:
    if FROZEN_BEGIN not in doc_text or FROZEN_END not in doc_text:
        return set(), (f"{DOC_REL} has no {FROZEN_BEGIN} / {FROZEN_END} block; "
                       f"the frozen-baseline disclosure is missing")
    body = doc_text.split(FROZEN_BEGIN, 1)[1].split(FROZEN_END, 1)[0]
    return set(re.findall(r"`([^`]+\.json)`", body)), ""


def check(root: Path) -> tuple[int, list[str]]:
    failures: list[str] = []
    doc = root / DOC_REL
    if not doc.exists():
        return 2, [f"{DOC_REL} not found under {root}"]
    doc_text = _read(doc)

    rows, err = parse_table(doc_text)
    if err:
        return 2, [err]

    scripts = discover_scripts(root)
    if not scripts:
        return 2, [f"no *.ps1 found under {SCRIPT_DIR_REL}; a checker that "
                   f"inspects nothing must not report success"]

    for rel in scripts:
        if rel not in rows:
            failures.append(
                f"{rel} has no row in the {DOC_REL} disposition table. Every "
                f"security dispatcher must be dispositioned -- wired, or "
                f"deliberately not wired with a written reason.")
            continue
        declared, reason = rows[rel]
        bad = declared - ALL_STATUSES
        if bad:
            failures.append(f"{rel}: unknown status {sorted(bad)}; allowed: "
                            f"{sorted(ALL_STATUSES)}")
            continue
        computed, invokers = compute_status(root, rel)
        if declared != computed:
            failures.append(
                f"{rel}: table says {'+'.join(sorted(declared))}, tree says "
                f"{'+'.join(sorted(computed))}"
                + (f" (invokers: {', '.join(invokers)})" if invokers
                   else " (no invoker found anywhere)"))
        if declared == {STATUS_NONE} and len(reason) < 20:
            failures.append(
                f"{rel}: declared NONE with no reason. 'Nothing runs it' is a "
                f"decision, and an undecided one is how #201 happened.")

    # Rows that name a file which no longer exists: the table must not accrete
    # ghosts, or it stops being read.
    for target in rows:
        if target.startswith(SCRIPT_DIR_REL + "/") and target not in scripts:
            failures.append(f"{target}: table row names a file that does not "
                            f"exist in the tree")

    # --- frozen baselines -------------------------------------------------
    frozen, err = parse_frozen(doc_text)
    if err:
        failures.append(err)
    else:
        bdir = root / BASELINE_DIR_REL
        on_disk = {p.name for p in bdir.glob("*.json")} if bdir.is_dir() else set()
        declared_names = {Path(f).name for f in frozen}
        missing = sorted(on_disk - declared_names)
        extra = sorted(declared_names - on_disk)
        if missing:
            failures.append(
                f"{BASELINE_DIR_REL}: {', '.join(missing)} present on disk but "
                f"not named in the frozen-baseline disclosure. An undisclosed "
                f"baseline reads as a live control (#201 item 4).")
        if extra:
            failures.append(
                f"frozen-baseline disclosure names {', '.join(extra)}, which "
                f"is not in {BASELINE_DIR_REL}")

    return (1 if failures else 0), failures


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------

_SCRIPT_STUB = "# stub dispatcher\nWrite-Host 'stub'\n"


def _mk_tree(tmp: Path, *, table_rows: str, frozen: str,
             wire_sast_to: str | None = None) -> Path:
    root = tmp
    (root / SCRIPT_DIR_REL).mkdir(parents=True, exist_ok=True)
    (root / WORKFLOW_DIR_REL).mkdir(parents=True, exist_ok=True)
    (root / "tests/security").mkdir(parents=True, exist_ok=True)
    (root / BASELINE_DIR_REL).mkdir(parents=True, exist_ok=True)
    (root / "docs").mkdir(parents=True, exist_ok=True)

    for n in ("run_sast.ps1", "check_regressions.ps1"):
        io.open(root / SCRIPT_DIR_REL / n, "w", encoding="utf-8").write(_SCRIPT_STUB)
    io.open(root / SECURITY_CMAKE_REL, "w", encoding="utf-8").write(
        'add_test(NAME security_regression_check COMMAND pwsh -File\n'
        '  "${CMAKE_SOURCE_DIR}/scripts/security/check_regressions.ps1")\n')
    # A comment-only mention, which must NOT count as an invoker.
    io.open(root / WORKFLOW_DIR_REL / "secrets-audit.yml", "w",
            encoding="utf-8").write(
        "# check_regressions.ps1 is deliberately NOT wired here.\n"
        "on:\n  workflow_dispatch:\n")
    if wire_sast_to:
        io.open(root / WORKFLOW_DIR_REL / wire_sast_to, "w",
                encoding="utf-8").write(
            "on:\n  workflow_dispatch:\njobs:\n  j:\n    steps:\n"
            "      - run: pwsh -File scripts/security/run_sast.ps1\n")
    io.open(root / BASELINE_DIR_REL / "sast_baseline.json", "w",
            encoding="utf-8").write("{}\n")
    io.open(root / DOC_REL, "w", encoding="utf-8").write(
        "# doc\n\n" + TABLE_BEGIN + "\n\n| Entry point | Status | Reason |\n"
        "|---|---|---|\n" + table_rows + "\n" + TABLE_END + "\n\n"
        + FROZEN_BEGIN + "\n" + frozen + "\n" + FROZEN_END + "\n")
    return root


_GOOD_ROWS = (
    "| `scripts/security/run_sast.ps1` | NONE | No lane; SAST needs clang-cl "
    "and a build tree this project will not spend a runner slot on. |\n"
    "| `scripts/security/check_regressions.ps1` | CTEST | Registered as CTest "
    "security_regression_check. |")
_GOOD_FROZEN = "- `sast_baseline.json`\n"


def self_test() -> int:
    """Drive every failure branch AND the positive case.

    A checker only ever exercised on a tree that passes is indistinguishable
    from `exit 0`.
    """
    failures: list[str] = []

    def expect(label: str, root: Path, want_rc: int, want_sub: str = "") -> None:
        rc, msgs = check(root)
        blob = " | ".join(msgs)
        if rc != want_rc:
            failures.append(f"{label}: expected rc={want_rc}, got {rc} ({blob})")
        elif want_sub and want_sub not in blob:
            failures.append(f"{label}: expected a message containing "
                            f"{want_sub!r}, got: {blob}")

    with tempfile.TemporaryDirectory() as td:
        base = Path(td)

        # 1. POSITIVE: a table that matches the tree passes.
        expect("clean tree", _mk_tree(base / "ok", table_rows=_GOOD_ROWS,
                                      frozen=_GOOD_FROZEN), 0)

        # 2. A dispatcher with no row.
        expect("missing row",
               _mk_tree(base / "missing",
                        table_rows=_GOOD_ROWS.splitlines()[0]
                        + "\n" + _GOOD_ROWS.splitlines()[1],
                        frozen=_GOOD_FROZEN), 0)  # both rows present -> passes
        root = _mk_tree(base / "missing2", table_rows=_GOOD_ROWS,
                        frozen=_GOOD_FROZEN)
        io.open(root / SCRIPT_DIR_REL / "run_brand_new.ps1", "w",
                encoding="utf-8").write(_SCRIPT_STUB)
        expect("undisclosed new dispatcher", root, 1, "has no row")

        # 3. Prose claims coverage that does not exist.
        expect("overclaimed invoker",
               _mk_tree(base / "over",
                        table_rows=_GOOD_ROWS.replace(
                            "| `scripts/security/run_sast.ps1` | NONE |",
                            "| `scripts/security/run_sast.ps1` | DISPATCH |"),
                        frozen=_GOOD_FROZEN),
               1, "no invoker found anywhere")

        # 4. Stale pessimism: the script IS wired, the table still says NONE.
        expect("underclaimed invoker",
               _mk_tree(base / "under", table_rows=_GOOD_ROWS,
                        frozen=_GOOD_FROZEN, wire_sast_to="sast-lane.yml"),
               1, "tree says DISPATCH")

        # 5. A comment-only mention must not count as wiring. If it did, the
        #    CTEST row for check_regressions.ps1 would compute as
        #    CTEST+DISPATCH and the clean tree in (1) would already be red --
        #    so (1) passing IS this assertion. Pin it explicitly anyway.
        root = _mk_tree(base / "comment", table_rows=_GOOD_ROWS,
                        frozen=_GOOD_FROZEN)
        computed, _ = compute_status(root, "scripts/security/check_regressions.ps1")
        if computed != {STATUS_CTEST}:
            failures.append(f"comment-only mention counted as an invoker: "
                            f"computed {sorted(computed)}")

        # 6. NONE with no written reason.
        expect("NONE without a reason",
               _mk_tree(base / "noreason",
                        table_rows=_GOOD_ROWS.replace(
                            "No lane; SAST needs clang-cl and a build tree "
                            "this project will not spend a runner slot on.",
                            "n/a"),
                        frozen=_GOOD_FROZEN),
               1, "declared NONE with no reason")

        # 7. An undisclosed baseline.
        root = _mk_tree(base / "baseline", table_rows=_GOOD_ROWS,
                        frozen=_GOOD_FROZEN)
        io.open(root / BASELINE_DIR_REL / "deps_baseline.json", "w",
                encoding="utf-8").write("{}\n")
        expect("undisclosed baseline", root, 1, "deps_baseline.json")

        # 8. No table at all is rc=2, not a quiet pass.
        root = _mk_tree(base / "notable", table_rows=_GOOD_ROWS,
                        frozen=_GOOD_FROZEN)
        io.open(root / DOC_REL, "w", encoding="utf-8").write("# doc\n")
        expect("no disposition block", root, 2, "disposition table")

    if failures:
        for f in failures:
            print(f"SELF-TEST FAIL: {f}")
        return 1
    print("SELF-TEST PASS: 8 branches exercised (positive, undisclosed script, "
          "over-claimed invoker, under-claimed invoker, comment-only mention, "
          "reasonless NONE, undisclosed baseline, missing table).")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--root", default=str(REPO_ROOT))
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    rc, failures = check(Path(args.root))
    if rc == 0:
        scripts = discover_scripts(Path(args.root))
        print(f"check_security_regime: PASS - {len(scripts)} security "
              f"dispatcher(s) dispositioned in {DOC_REL}, every declared "
              f"status matches the tree, and the frozen-baseline disclosure "
              f"names every file in {BASELINE_DIR_REL}.")
        return 0
    print(f"check_security_regime: FAIL ({len(failures)} finding(s))")
    for f in failures:
        print(f"  - {f}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
