#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Run the security regression check for real, and refuse to call a vacuous run a pass.

WHY THIS EXISTS (GitHub #201).

`scripts/security/check_regressions.ps1` is the one piece of the security regime
that is cheap, offline, and runnable on a developer box today. It is registered
as CTest `security_regression_check` under the `security` label -- but the only
lane that runs `ctest -L security` is `windows-validation.yml`, which is
dispatch-only and, per #201, has zero runs. So the check has an invoker on paper
and produces no evidence in practice. That is the same shape as #187.

This driver closes the local half of that: it runs the check, parses the JSON it
emits, and applies an oracle the PowerShell script does not apply to itself.

THE ORACLE, AND WHY IT IS THE WHOLE POINT. `check_regressions.ps1` reports a
phase as `skipped` when no fresh output is present, and a run in which every
phase skipped still exits 0 with `overall_verdict: pass`. That is defensible for
the script -- it is reporting honestly that no comparison was possible -- but a
CALLER that treats it as a pass has learned nothing and believes it has. This
project has a name for that shape: "could not check" is not "clean" (see
`fz_try_raii_scan.py`, GitHub #187). So:

  * every phase whose fresh input EXISTS AND PARSES must come back with a
    verdict other than `skipped` -- if it comes back `skipped` anyway, the
    comparison silently degraded and that is a FAILURE here;
  * a run in which no phase compared anything is reported as NO EVIDENCE, not
    as a pass, whenever at least one fresh input was present;
  * a missing or stale `regression_check.json` is a failure -- the record is
    the evidence, and an unwritten record is an unperformed run.

WHEN NO INPUT EXISTS AT ALL (a fresh clone, or CI) the script legitimately has
nothing to compare and this driver reports SKIPPED WITH A REASON and exits 0.
That is also why this is NOT in `fast-checks.yml`: on a GitHub-hosted runner
every fresh input is gitignored and absent, so the step would be vacuous by
construction on every push -- and a guard that is structurally unable to fail in
the lane it runs in is decoration. It is wired into
`tools/gate/run_fast_checks_locally.py` instead, where the inputs exist.

Usage:
    python tools/gate/run_security_regression_check.py
    python tools/gate/run_security_regression_check.py --record <path.md>
    python tools/gate/run_security_regression_check.py --self-test

Exit codes: 0 = a real comparison happened and found no regression (or there was
nothing to compare and it said so), 1 = regression, degraded comparison, or a
run that produced no record.
"""
from __future__ import annotations

import argparse
import datetime as _dt
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

REPO_ROOT = Path(__file__).resolve().parents[2]
PS1_REL = "scripts/security/check_regressions.ps1"
REPORT_JSON_REL = "tests/security/reports/regression_check.json"
REPORT_MD_REL = "tests/security/reports/regression_check.md"
BASELINE_DIR_REL = "tests/security/regression_baselines"


def _load_json(path: Path):
    try:
        return json.loads(io.open(path, encoding="utf-8", errors="replace").read())
    except (OSError, ValueError):
        return None


def _latest_dir(parent: Path, prefix: str) -> Path | None:
    if not parent.is_dir():
        return None
    cands = [p for p in parent.iterdir() if p.is_dir() and p.name.startswith(prefix)]
    if not cands:
        return None
    return max(cands, key=lambda p: p.stat().st_mtime)


def fresh_inputs(root: Path) -> dict[str, bool]:
    """Per phase: does a fresh input exist AND parse as JSON?

    Mirrors the selection rule inside check_regressions.ps1 -- newest
    `corpus_run_*.json`, newest `fuzz/run_*/summary.json`, newest
    `sanitizers/run_*/findings.json`, and the fixed paths for sast/deps/secrets.
    Mirroring rather than guessing matters: if this function looked at a
    DIFFERENT file than the script does, the oracle below would fire on
    disagreements that are its own fault.
    """
    reports = root / "tests/security/reports"
    out: dict[str, bool] = {}

    corpus = sorted(reports.glob("corpus_run_*.json"),
                    key=lambda p: p.stat().st_mtime) if reports.is_dir() else []
    out["corpus"] = bool(corpus) and _load_json(corpus[-1]) is not None

    fz = _latest_dir(reports / "fuzz", "run_")
    out["fuzz"] = fz is not None and _load_json(fz / "summary.json") is not None

    out["sast"] = _load_json(reports / "sast" / "findings.json") is not None
    out["deps"] = _load_json(reports / "deps" / "cves.json") is not None

    sa = _latest_dir(reports / "sanitizers", "run_")
    out["sanitizers"] = sa is not None and _load_json(sa / "findings.json") is not None

    out["secrets"] = _load_json(reports / "secrets" / "findings.json") is not None
    return out


def judge(summary: dict, inputs: dict[str, bool]) -> tuple[int, list[str], list[str]]:
    """(rc, failures, notes) for one parsed regression_check.json."""
    failures: list[str] = []
    notes: list[str] = []

    results = summary.get("results") or []
    if not results:
        return 1, ["regression_check.json carries no per-phase results; a "
                   "record with nothing in it is not evidence of a run"], notes

    verdicts = {r.get("phase"): (r.get("verdict") or "") for r in results}
    compared = [p for p, v in verdicts.items() if v in ("pass", "fail")]

    for phase, present in inputs.items():
        v = verdicts.get(phase)
        if v is None:
            failures.append(f"{phase}: no result row at all -- the check did "
                            f"not consider this phase")
        elif present and v == "skipped":
            failures.append(
                f"{phase}: a fresh input exists and parses, but the check "
                f"reported 'skipped' "
                f"({[r.get('note') for r in results if r.get('phase') == phase]}). "
                f"A degraded comparison must not read as a clean one.")
        elif not present and v == "skipped":
            notes.append(f"{phase}: skipped, no fresh input on this box")

    failed = [r for r in results if r.get("verdict") == "fail"]
    if failed:
        for r in failed:
            failures.append(f"{r.get('phase')}: REGRESSION vs baseline -- "
                            f"{r.get('note')}")

    if summary.get("overall_verdict") == "fail" and not failed:
        failures.append("overall_verdict is 'fail' but no phase row says so; "
                        "the record contradicts itself")

    if not compared:
        if any(inputs.values()):
            failures.append(
                "no phase compared anything, yet fresh inputs are present. "
                "This is a vacuous run reported as a pass.")
        else:
            notes.append("NO EVIDENCE: no fresh scanner output exists on this "
                         "box, so nothing could be compared. Not a pass.")

    return (1 if failures else 0), failures, notes


def run(root: Path, record: Path | None) -> int:
    ps1 = root / PS1_REL
    if not ps1.exists():
        print(f"FAIL: {PS1_REL} not found")
        return 1

    pwsh = shutil.which("pwsh") or shutil.which("powershell")
    if not pwsh:
        print("SKIPPED: neither pwsh nor powershell is on PATH, so the "
              "security regression check cannot run here. This is a skip with "
              "a reason, not a pass.")
        return 0

    inputs = fresh_inputs(root)
    if not any(inputs.values()):
        print("SKIPPED: no fresh scanner output exists under "
              "tests/security/reports/, so check_regressions.ps1 would report "
              "every phase 'skipped' and compare nothing. Run a phase "
              "dispatcher first (see docs/security-testing.md). Not a pass.")
        return 0

    report = root / REPORT_JSON_REL
    before = report.stat().st_mtime if report.exists() else 0.0

    proc = subprocess.run(
        [pwsh, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(ps1)],
        cwd=str(root), capture_output=True, text=True,
        encoding="utf-8", errors="replace", timeout=900)

    if not report.exists():
        print(f"FAIL: {PS1_REL} exited {proc.returncode} and wrote no "
              f"{REPORT_JSON_REL}. An unwritten record is an unperformed run.")
        print((proc.stderr or proc.stdout or "").strip()[:800])
        return 1
    if report.stat().st_mtime <= before:
        print(f"FAIL: {REPORT_JSON_REL} was not rewritten by this run; the "
              f"record on disk is stale and would be read as fresh evidence.")
        return 1

    summary = _load_json(report)
    if summary is None:
        print(f"FAIL: {REPORT_JSON_REL} is not parseable JSON")
        return 1

    rc, failures, notes = judge(summary, inputs)
    # The script's own exit code still governs: it exits 1 on a regression it
    # detected and 2 on a preflight failure, and neither may be swallowed here.
    if proc.returncode != 0 and rc == 0:
        failures.append(f"{PS1_REL} exited {proc.returncode} while the record "
                        f"it wrote reads clean; trust the exit code")
        rc = 1

    phases = {r.get("phase"): r.get("verdict") for r in summary.get("results", [])}
    print(f"[security-regression] overall={summary.get('overall_verdict')} "
          f"stamp={summary.get('stamp')}")
    for p, v in phases.items():
        print(f"    {p:<12} {v}")
    for n in notes:
        print(f"    note: {n}")

    if record is not None:
        _write_record(root, record, summary, inputs, proc.returncode, rc)
        print(f"[security-regression] record written: "
              f"{record.relative_to(root) if record.is_absolute() else record}")

    if failures:
        print(f"[security-regression] FAIL ({len(failures)} finding(s))")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("[security-regression] OK: a real comparison was performed and no "
          "phase regressed against its committed baseline.")
    return 0


def _write_record(root: Path, record: Path, summary: dict,
                  inputs: dict[str, bool], ps_rc: int, rc: int) -> None:
    """A durable, SANITIZED run record.

    Deliberately not a copy of tests/security/reports/regression_check.md:
    that file is gitignored (tests/security/reports/* is excluded wholesale),
    so it cannot be the durable evidence, and the reports the run reads carry
    absolute build paths. Nothing here interpolates a filesystem path.
    """
    record.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# Security regression check - real run record",
        "",
        f"Produced by `python tools/gate/run_security_regression_check.py "
        f"--record <this file>` at `{summary.get('stamp')}` "
        f"(recorded {_dt.datetime.now(_dt.timezone.utc):%Y-%m-%dT%H:%M:%SZ}).",
        "",
        f"- driver verdict: **{'PASS' if rc == 0 else 'FAIL'}**",
        f"- `check_regressions.ps1` exit code: {ps_rc}",
        f"- `overall_verdict` in the emitted record: "
        f"`{summary.get('overall_verdict')}`",
        "",
        "| Phase | Verdict | Fresh input present & parseable | Note |",
        "|---|---|---|---|",
    ]
    for r in summary.get("results", []):
        phase = r.get("phase")
        lines.append(f"| {phase} | {r.get('verdict')} | "
                     f"{'yes' if inputs.get(phase) else 'no'} | "
                     f"{r.get('note') or ''} |")
    lines += [
        "",
        "Generated files (both gitignored, hence this tracked record):",
        "",
        f"- `{REPORT_JSON_REL}`",
        f"- `{REPORT_MD_REL}`",
        "",
    ]
    io.open(record, "w", encoding="utf-8", newline="\n").write("\n".join(lines))


# ---------------------------------------------------------------------------
# self-test -- drives the judgement, which is the part that can be wrong
# ---------------------------------------------------------------------------

def self_test() -> int:
    failures: list[str] = []

    def expect(label: str, summary: dict, inputs: dict, want_rc: int,
               want_sub: str = "") -> None:
        rc, msgs, notes = judge(summary, inputs)
        blob = " | ".join(msgs + notes)
        if rc != want_rc:
            failures.append(f"{label}: expected rc={want_rc}, got {rc} ({blob})")
        elif want_sub and want_sub not in blob:
            failures.append(f"{label}: expected {want_sub!r} in: {blob}")

    phases = ["corpus", "fuzz", "sast", "deps", "sanitizers", "secrets"]

    def summary_of(**verdicts) -> dict:
        return {"overall_verdict":
                "fail" if "fail" in verdicts.values() else "pass",
                "stamp": "1970-01-01T00:00:00Z",
                "results": [{"phase": p, "verdict": verdicts.get(p, "skipped"),
                             "note": "", "new_items": []} for p in phases]}

    all_in = {p: True for p in phases}
    no_in = {p: False for p in phases}

    # 1. POSITIVE: every phase has an input and every phase compared.
    expect("all compared", summary_of(**{p: "pass" for p in phases}),
           all_in, 0)

    # 2. THE ONE THAT MATTERS: an input exists, the check skipped it anyway.
    expect("degraded comparison",
           summary_of(**{p: "pass" for p in phases if p != "sast"}),
           all_in, 1, "reported 'skipped'")

    # 3. Everything skipped WITH inputs present = vacuous, must not pass.
    expect("vacuous with inputs", summary_of(), all_in, 1, "vacuous run")

    # 4. Everything skipped with NO inputs = honest skip, rc 0, and it must
    #    say NO EVIDENCE rather than implying a clean tree.
    expect("honest skip", summary_of(), no_in, 0, "NO EVIDENCE")

    # 5. A real regression fails.
    s = summary_of(**{p: "pass" for p in phases})
    s["results"][2]["verdict"] = "fail"
    s["results"][2]["note"] = "3 new high-sev SAST findings"
    s["overall_verdict"] = "fail"
    expect("regression", s, all_in, 1, "REGRESSION vs baseline")

    # 6. A record that contradicts itself.
    s = summary_of(**{p: "pass" for p in phases})
    s["overall_verdict"] = "fail"
    expect("self-contradicting record", s, all_in, 1, "contradicts itself")

    # 7. A phase missing from the record entirely.
    s = summary_of(**{p: "pass" for p in phases})
    s["results"] = [r for r in s["results"] if r["phase"] != "deps"]
    expect("missing phase row", s, all_in, 1, "no result row at all")

    # 8. An empty record is not a pass.
    expect("empty record", {"overall_verdict": "pass", "results": []},
           all_in, 1, "not evidence of a run")

    # 9. The input probe must actually look at the tree: on this repo it must
    #    return one entry per phase, or the oracle silently covers nothing.
    probe = fresh_inputs(REPO_ROOT)
    if sorted(probe) != sorted(phases):
        failures.append(f"fresh_inputs() probed {sorted(probe)}, expected "
                        f"{sorted(phases)}")

    # 10. And it must be capable of returning False -- a probe hardwired to
    #     True would make branch 2 unreachable.
    with tempfile.TemporaryDirectory() as td:
        empty = fresh_inputs(Path(td))
        if any(empty.values()):
            failures.append("fresh_inputs() reported a fresh input in an empty "
                            "directory tree")

    if failures:
        for f in failures:
            print(f"SELF-TEST FAIL: {f}")
        return 1
    print("SELF-TEST PASS: 10 branches exercised (all-compared, degraded "
          "comparison, vacuous-with-inputs, honest skip, regression, "
          "self-contradicting record, missing phase row, empty record, probe "
          "coverage, probe can say no).")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--record", default=None,
                    help="write a sanitized markdown run record here")
    ap.add_argument("--root", default=str(REPO_ROOT))
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    root = Path(args.root)
    record = None
    if args.record:
        r = Path(args.record)
        record = r if r.is_absolute() else (root / r)
    return run(root, record)


if __name__ == "__main__":
    sys.exit(main())
