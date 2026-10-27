#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Apply declared mutations, build, run tests, and record what actually died.

WHY THIS EXISTS
---------------
"Mutation-verified" asserted in prose is not evidence. This repo has a recorded
failure for exactly that (`docs/audit/dca/` — a patch that silently failed to
apply is indistinguishable from a surviving mutant), and a gate review of
pipeline p94 phase-1 rejected four such claims because nothing on disk
substantiated them: no patch, no log, no before/after record.

So this script does the runs and writes the table. Every row is produced by a
build and a test invocation that happened, and the marker is grepped out of the
tree BEFORE the build so a mutation that failed to apply is reported as an
error rather than silently scored as a surviving mutant.

WHAT IT GUARANTEES
------------------
* The mutation text is found EXACTLY ONCE in the target file, or the run aborts.
  A pattern that matches zero or several places is a broken spec, not a result.
* The marker is verified present in the tree after writing and absent after
  restoration, so "the mutation was applied" and "the tree is clean again" are
  both measured.
* Restoration is by copy from a backup taken before the edit. Never
  `git checkout --`, which has destroyed work in this repo before.
* The generated report names tests and verdicts only — never absolute paths,
  which carry the build account name and must not reach a tracked file.

USAGE
-----
    python tests/scripts/run_mutation_check.py --spec tests/scripts/mutations/p94_phase1.json \\
        --report docs/audit/p94-phase1-mutation-log.md \\
        --work-dir tests/_runtime/p94-mutations

A spec is {"pipeline": str, "mutations": [ ... ]}; see the p94 spec for the shape.
Each mutation declares the file, the exact `find` text, its `replace`, a unique
`marker` string that must appear in the replacement, the build `targets`, the
gtest `filter` to run, and `expect_fail` — the tests that MUST fail. A mutation
that kills tests outside `expect_fail`, or fails to kill one inside it, is
reported as a discrepancy. That is the whole point: an oracle that reddens for
the wrong reason is as much a defect as one that cannot redden at all.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent

CMAKE = Path(
    "C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/"
    "CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
)


def _repo_relative(path_str: str) -> str:
    """Repo-relative POSIX form, whether the caller passed absolute or relative.

    Absolute paths must never reach the generated report: they carry the build
    account name, and a tracked file containing it is the leak this project has
    already had to rewrite history for twice.
    """
    p = Path(path_str)
    if p.is_absolute():
        try:
            return p.relative_to(REPO_ROOT).as_posix()
        except ValueError:
            return p.name
    return p.as_posix()


def log(msg: str) -> None:
    print(f"[mutation-check] {msg}", flush=True)


def build(targets: list[str], logfile: Path) -> bool:
    cmd = [str(CMAKE), "--build", "build/vs2022", "--config", "Debug"]
    for t in targets:
        cmd += ["--target", t]
    proc = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True, text=True,
                          errors="replace")
    logfile.parent.mkdir(parents=True, exist_ok=True)
    logfile.write_text(proc.stdout + "\n" + proc.stderr, encoding="utf-8")
    # MSBuild can exit 0 with errors in rare configurations, so look at both.
    had_error = re.search(r"error (C|LNK|MSB)\d+", proc.stdout) is not None
    return proc.returncode == 0 and not had_error


def run_gtest(filter_expr: str, xml_out: Path, logfile: Path) -> dict[str, str]:
    """Run the filtered suite; return {test_id: 'pass'|'fail'}."""
    exe = REPO_ROOT / "build/vs2022/bin/Debug/pdfe_tests.exe"
    xml_out.parent.mkdir(parents=True, exist_ok=True)
    if xml_out.exists():
        xml_out.unlink()
    proc = subprocess.run(
        [str(exe), f"--gtest_filter={filter_expr}",
         f"--gtest_output=xml:{xml_out}"],
        cwd=REPO_ROOT, capture_output=True, text=True, errors="replace",
        timeout=3600,
    )
    logfile.write_text(proc.stdout + "\n" + proc.stderr, encoding="utf-8")

    results: dict[str, str] = {}
    if not xml_out.exists():
        return results
    root = ET.parse(xml_out).getroot()
    for tc in root.iter("testcase"):
        test_id = f"{tc.get('classname')}.{tc.get('name')}"
        failed = any(True for _ in tc.iter("failure")) or \
            any(True for _ in tc.iter("error"))
        results[test_id] = "fail" if failed else "pass"
    return results


def run_pytest(target: str, logfile: Path) -> dict[str, str]:
    """Run a pytest file; return {nodeid_tail: 'pass'|'fail'}."""
    proc = subprocess.run(
        [sys.executable, "-m", "pytest", target, "-v", "--no-header", "-p",
         "no:cacheprovider"],
        cwd=REPO_ROOT, capture_output=True, text=True, errors="replace",
        timeout=3600,
    )
    logfile.write_text(proc.stdout + "\n" + proc.stderr, encoding="utf-8")
    results: dict[str, str] = {}
    for line in proc.stdout.splitlines():
        m = re.match(r"^\S+::(\S+)\s+(PASSED|FAILED|ERROR)", line)
        if m:
            results[m.group(1)] = "pass" if m.group(2) == "PASSED" else "fail"
    return results


def apply_mutation(spec: dict) -> Path:
    """Write the mutation; return the backup path. Aborts on an ambiguous spec."""
    target = REPO_ROOT / spec["file"]
    original = target.read_text(encoding="utf-8")
    occurrences = original.count(spec["find"])
    if occurrences != 1:
        raise SystemExit(
            f"spec error: '{spec['id']}' find-text occurs {occurrences} times in "
            f"{spec['file']}; it must occur exactly once"
        )
    if spec["marker"] not in spec["replace"]:
        raise SystemExit(
            f"spec error: '{spec['id']}' replacement does not contain its marker"
        )

    backup = target.with_suffix(target.suffix + ".mutation-backup")
    shutil.copy2(target, backup)
    target.write_text(original.replace(spec["find"], spec["replace"]),
                      encoding="utf-8", newline="\n")

    # Measured, not assumed: the marker must be in the tree before we build.
    if spec["marker"] not in target.read_text(encoding="utf-8"):
        raise SystemExit(f"'{spec['id']}': marker absent after write")
    log(f"{spec['id']}: applied, marker present in {spec['file']}")
    return backup


def restore(spec: dict, backup: Path) -> None:
    target = REPO_ROOT / spec["file"]

    # copy(), NOT copy2(), and then touch.
    #
    # copy2() preserves the backup's mtime, which sets the restored source back
    # to a time EARLIER than the object file just built from the mutant. MSBuild
    # then considers that object up to date and does not recompile it, so the
    # MUTANT OBJECT survives into every later mutation's build and links into the
    # test binary.
    #
    # That is not theoretical: it produced four bogus "discrepancies" in this
    # pipeline, each of which looked like an unrelated test being fragile. M2's
    # mutant (write only the first path) leaked into the M6 and M7 runs, where it
    # failed the forwarding test those mutations do not touch; M7's leaked into
    # M8, where it failed a drop test on a flag M8 never changes. Hours went into
    # chasing the innocent tests.
    shutil.copy(backup, target)
    os.utime(target, None)
    backup.unlink()
    remaining = target.read_text(encoding="utf-8").count(spec["marker"])
    if remaining:
        raise SystemExit(
            f"'{spec['id']}': marker STILL PRESENT after restore ({remaining}) — "
            f"the tree is mutated; fix {spec['file']} before doing anything else"
        )
    log(f"{spec['id']}: restored, marker absent")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--spec", required=True)
    ap.add_argument("--report", required=True)
    ap.add_argument("--work-dir", default="tests/_runtime/mutations")
    args = ap.parse_args()

    spec_doc = json.loads(Path(args.spec).read_text(encoding="utf-8"))
    work = REPO_ROOT / args.work_dir
    work.mkdir(parents=True, exist_ok=True)

    rows = []
    discrepancies = []

    for mut in spec_doc["mutations"]:
        mid = mut["id"]
        log(f"=== {mid}: {mut['what']} ===")
        backup = apply_mutation(mut)
        try:
            if not build(mut["targets"], work / f"{mid}-build.log"):
                raise SystemExit(
                    f"{mid}: BUILD FAILED. A mutation that does not compile "
                    f"proves nothing; see {mut['targets']} build log."
                )
            if mut.get("pytest"):
                results = run_pytest(mut["pytest"], work / f"{mid}-tests.log")
            else:
                results = run_gtest(mut["filter"], work / f"{mid}.xml",
                                    work / f"{mid}-tests.log")
        finally:
            restore(mut, backup)

        if not results:
            raise SystemExit(f"{mid}: no test results parsed — cannot score")

        failed = sorted(t for t, v in results.items() if v == "fail")
        expected = sorted(mut["expect_fail"])
        unexpected = [t for t in failed if t not in expected]
        missing = [t for t in expected if t not in failed]

        known_unguarded = mut.get("known_unguarded")
        if known_unguarded and not expected:
            # A mutation deliberately kept with NO oracle, so the gap stays
            # visible. Silently dropping it from the spec would make the report
            # read as full coverage; scoring it as a discrepancy would make a
            # clean run impossible. It gets its own section instead.
            if failed:
                discrepancies.append(
                    f"{mid}: declared unguarded but KILLED {failed} — the spec "
                    f"is out of date and the note should be removed")
        else:
            if unexpected:
                discrepancies.append(
                    f"{mid}: killed tests NOT in expect_fail: {unexpected}")
            if missing:
                discrepancies.append(
                    f"{mid}: SURVIVED against: {missing}")

        (work / f"{mid}-result.json").write_text(
            json.dumps({"id": mid, "what": mut["what"], "file": mut["file"],
                        "results": results}, indent=2),
            encoding="utf-8")
        rows.append({
            "id": mid, "what": mut["what"], "file": mut["file"],
            "total": len(results), "failed": failed,
            "expected": expected, "ok": not unexpected and not missing,
            "known_unguarded": mut.get("known_unguarded"),
        })
        log(f"{mid}: {len(failed)}/{len(results)} killed; "
            f"{'AS EXPECTED' if rows[-1]['ok'] else 'DISCREPANCY'}")

    # ---- leave the tree BUILT from the restored sources ----
    #
    # Every restore touches its file, so after the last mutation the sources are
    # newer than the objects compiled from the mutants. Leaving it there hands
    # the next person a build whose objects contain mutated code — and the
    # staleness preflight in run_gtest.py correctly REFUSES to run against it
    # (GitHub #245), which is how this was noticed. A verification tool must not
    # leave the tree in a state its own project's guards reject.
    # Always include BOTH binaries, not just the targets the spec mutated.
    #
    # run_gtest.py's staleness preflight checks the application binary as well as
    # the test binary (GitHub #245), and several suites launch pdfe.exe. A spec
    # that only mutates files in pdfe_tests leaves pdfe.exe un-relinked and older
    # than the restored sources, so the preflight correctly refuses to run and the
    # tree looks broken after a clean mutation sweep. Measured: exactly that, with
    # pdfe.exe reported 4.0h older than a restored source.
    all_targets = sorted({t for m in spec_doc["mutations"] for t in m["targets"]}
                         | {"pdfe", "pdfe_tests"})
    log(f"final rebuild from restored sources: {all_targets}")
    if not build(all_targets, work / "final-rebuild.log"):
        raise SystemExit(
            "final rebuild FAILED — the tree is restored but its objects still "
            "hold mutated code; rebuild before running anything"
        )

    # ---- report (no absolute paths: they carry the build account name) ----
    out = [
        f"# Mutation check — {spec_doc['pipeline']}",
        "",
        "**Generated by `tests/scripts/run_mutation_check.py` from real builds and",
        "real test runs.** Every row below is the recorded outcome of applying the",
        "mutation, compiling it, and running the named filter. The script verifies",
        "the mutation marker is present in the tree *before* building and absent",
        "after restoring, so a patch that failed to apply is reported as an error",
        "rather than scored as a surviving mutant.",
        "",
        "A mutation is only useful if it kills the RIGHT tests. `expected` is",
        "declared in the spec ahead of the run; killing anything outside it is a",
        "discrepancy, because an oracle that reddens for the wrong reason is as",
        "much a defect as one that cannot redden at all.",
        "",
        f"Spec: `{_repo_relative(args.spec)}`",
        "",
        "| Mutation | What it breaks | File | Tests run | Killed | As expected |",
        "|---|---|---|---|---|---|",
    ]
    for r in rows:
        out.append(
            f"| `{r['id']}` | {r['what']} | `{r['file']}` | {r['total']} | "
            f"{len(r['failed'])} | "
            f"{'n/a (unguarded)' if r.get('known_unguarded') else ('yes' if r['ok'] else '**NO**')} |"
        )
    unguarded = [r for r in rows if r.get("known_unguarded")]
    if unguarded:
        out += ["", "## Deliberately unguarded — recorded, not hidden", "",
                "These mutations survive because NO test currently detects them.",
                "They are kept in the spec so the gap stays visible: dropping them",
                "would make the table above read as full coverage.", ""]
        for r in unguarded:
            out.append(f"* **`{r['id']}`** — {r['what']}.")
            out.append(f"  {r['known_unguarded']}")
            out.append("")

    out += ["", "## Which tests each mutation killed", ""]
    for r in rows:
        out.append(f"### `{r['id']}`")
        out.append("")
        if r["failed"]:
            for t in r["failed"]:
                out.append(f"* `{t}`")
        elif r.get("known_unguarded"):
            out.append("* nothing — survives by design; see the unguarded section")
        else:
            out.append("* nothing — **the mutation SURVIVED**")
        out.append("")
    if discrepancies:
        out += ["## Discrepancies", ""]
        out += [f"* {d}" for d in discrepancies]
        out.append("")
    else:
        out += ["## Discrepancies", "", "None: every mutation killed exactly the "
                "tests declared for it, and no others.", ""]

    report = REPO_ROOT / args.report
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text("\n".join(out), encoding="utf-8", newline="\n")
    log(f"report -> {args.report}")

    if discrepancies:
        for d in discrepancies:
            log(f"DISCREPANCY: {d}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
