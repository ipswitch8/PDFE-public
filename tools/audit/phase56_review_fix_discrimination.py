#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Discrimination evidence for the phase-56 security-review fixes (F1/F2/F4).

WHY THIS EXISTS SEPARATELY FROM phase56_revert_and_observe.py
-------------------------------------------------------------
That script backs acceptance criterion 5: it reverts WIN-87 site #1 to the
path-based primitive and records that the site-#1 hostile cases go red. This one
backs the *other* discrimination claim made in this phase — that the five cases
added after the non-gate `security-code-reviewer` pass are witnesses rather than
decoration. The claim was originally made in prose with no artifact behind it,
and the phase-3 `validator` gate failed the phase for exactly that: two claims of
the same kind held to two different standards.

WHAT IT MUTATES
---------------
All three F-fixes in src/core/AtomicFileReplace.cpp at once, because they are
independent and one build is enough to tell which cases notice which:

  F1  ~StagingFile() unlinks THROUGH the handle (FileDispositionInfo) rather
      than by name after CloseHandle.
  F2  ERROR_SHARING_VIOLATION from the stale-name clear is classified as
      "another writer holds it" (back off and retry) rather than as an
      unclearable squatter (fail immediately).
  F4  The extended-length (\\?\) path form for a staging name that crosses
      MAX_PATH even though the destination does not.

WHY THE REBUILD IS PROVEN, NOT ASSUMED
--------------------------------------
Same control as the sibling script, and it is not ceremony: earlier in this
pipeline a revert-and-observe produced a COMPLETE false finding (F69) because
shutil.copy2 preserved the source mtime, MSBuild skipped the compile, and five
deterministic runs measured a binary that was never rebuilt. Every build here
force-touches the source (os.utime) and then requires BOTH that the object
file's mtime advanced AND that a compile line for the file appears in the build
log, or it aborts instead of reporting a measurement.

EXPECTED OUTCOME, STATED UP FRONT SO A SURPRISE IS VISIBLE
----------------------------------------------------------
Four of the five cases are expected to go red under the mutation. The fifth,
.TwoDocumentsSavingTheSameTargetDoNotCorruptIt, is expected to PASS: it is a
corruption guard, not an F2 witness, for the reason written at the case itself
(two in-process Documents cannot both publish over one target with or without
F2 — the winner's reopened CRT handle blocks the loser's rename). This script
records what actually happens; the expectation is here so that a DIFFERENT
result is read as news rather than as noise.
"""

import argparse
import io
import json
import os
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "src" / "core" / "AtomicFileReplace.cpp"
OBJ = (REPO / "build" / "vs2022" / "src" / "core" / "pdfe_core.dir" / "Debug"
       / "AtomicFileReplace.obj")
EXE = REPO / "build" / "vs2022" / "bin" / "Debug" / "pdfe_tests.exe"
CMAKE = Path(
    r"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE"
    r"\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)

CASES = [
    "FullRewriteStagingPrimitive.AbandonedStagingIsUnlinkedThroughTheHandle",
    "FullRewriteStagingPrimitive.ASecondWriterWaitsForTheFirstRatherThanFailing",
    "FullRewriteStagingPrimitive.AContendedNameEventuallyReportsASharingViolation",
    "FullRewriteSaveTocttou.TwoDocumentsSavingTheSameTargetDoNotCorruptIt",
    "SaveAsStagingTocttou.ADestinationOnlyTheStagingNameOverflowsStillSaves",
]

# Cases that are NOT expected to discriminate against this mutation, with the
# reason. Recorded here so the script's own report says which is which instead
# of leaving a reader to infer it from a tally.
NON_WITNESSES = {
    "FullRewriteSaveTocttou.TwoDocumentsSavingTheSameTargetDoNotCorruptIt":
        "corruption guard, not an F2 witness: the mutation makes the loser fail "
        "immediately but the winner still publishes a valid PDF. The stronger "
        "form (BOTH saves complete) was tried and fails 5/5 against the FIXED "
        "code too, because the winner's reopened CRT 'rb' handle blocks the "
        "loser's replace-existing rename in-process — a pre-existing limit "
        "unrelated to F2. .ASecondWriterWaitsForTheFirstRatherThanFailing is "
        "the F2 witness.",
}

# (find, replace). Each must match EXACTLY once or the script aborts — a
# silently unapplied hunk is how a mutation run measures the fixed code and then
# calls a real witness decoration.
MUTATIONS = [
    (
        # F1 — back to CloseHandle-then-remove-by-name.
        """        FILE_DISPOSITION_INFO disp{};
        disp.DeleteFile = TRUE;
        const bool markedForDelete =
            !committed_
            && ::SetFileInformationByHandle(h, FileDispositionInfo, &disp,
                                            sizeof(disp)) != FALSE;
        ::CloseHandle(h);
        handle_ = nullptr;
        if (markedForDelete) {
            return;  // already gone; never touch the name
        }""",
        """        ::CloseHandle(h);
        handle_ = nullptr;""",
    ),
    (
        # F4 — never take the extended-length form.
        "if (native.size() < 240",
        "if (native.size() < 100000",
    ),
    (
        # F2 — a sharing violation is an unclearable squatter again.
        """        if (ec.value() == ERROR_SHARING_VIOLATION) {
            inUseByAnotherWriter = true;
        }""",
        """        if (false) {
            inUseByAnotherWriter = true;
        }""",
    ),
]


def run(cmd, log_path=None, timeout=3600):
    proc = subprocess.run(cmd, cwd=str(REPO), capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=timeout)
    out = (proc.stdout or "") + (proc.stderr or "")
    if log_path:
        io.open(log_path, "w", encoding="utf-8").write(out)
    return proc.returncode, out


def build_and_verify(tag, outdir):
    """Build pdfe_tests and PROVE AtomicFileReplace.cpp was recompiled."""
    before = OBJ.stat().st_mtime if OBJ.exists() else 0.0
    now = time.time()
    os.utime(SRC, (now, now))
    log = outdir / f"build_{tag}.log"
    rc, out = run([str(CMAKE), "--build", str(REPO / "build" / "vs2022"),
                   "--config", "Debug", "--target", "pdfe_tests"], log)
    if rc != 0:
        sys.exit(f"BUILD FAILED ({tag}); see {log}")
    after = OBJ.stat().st_mtime if OBJ.exists() else 0.0
    compiled = "AtomicFileReplace.cpp" in out
    if not (after > before and compiled):
        sys.exit(
            f"ABORT: the {tag} build did not recompile AtomicFileReplace.cpp "
            f"(obj mtime {before} -> {after}, compile line seen: {compiled}). "
            "Measuring now would repeat the F69 false finding."
        )
    return {"obj_mtime_before": before, "obj_mtime_after": after,
            "compile_line_seen": compiled, "log": str(log)}


def run_cases(tag, outdir):
    xml = outdir / f"gtest_{tag}.xml"
    rc, _ = run([str(EXE), "--gtest_filter=" + ":".join(CASES),
                 f"--gtest_output=xml:{xml}"], outdir / f"tests_{tag}.log")
    root = ET.parse(str(xml)).getroot()
    per_case = {}
    for case in root.iter("testcase"):
        name = f"{case.get('classname')}.{case.get('name')}"
        per_case[name] = "FAIL" if case.find("failure") is not None else "PASS"
    missing = [c for c in CASES if c not in per_case]
    if missing:
        sys.exit(f"ABORT: {tag} run did not execute {missing} — a filter typo "
                 "would otherwise be reported as a clean result.")
    return {"exit_code": rc, "xml": str(xml), "per_case": per_case,
            "tests": int(root.get("tests", 0)),
            "failures": int(root.get("failures", 0)),
            "errors": int(root.get("errors", 0))}


def apply(text, mutate, crlf):
    for find, replace in MUTATIONS:
        a, b = (find, replace) if mutate else (replace, find)
        if crlf:
            a = a.replace("\n", "\r\n")
            b = b.replace("\n", "\r\n")
        n = text.count(a)
        if n != 1:
            sys.exit(f"ABORT: hunk matched {n} times, expected 1:\n{a[:120]}")
        text = text.replace(a, b)
    return text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--outdir",
                    default=str(REPO / "build" / "p56_review_discrimination"))
    args = ap.parse_args()
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    original = io.open(SRC, encoding="utf-8", newline="").read()
    io.open(outdir / "AtomicFileReplace.cpp.orig", "w", encoding="utf-8",
            newline="").write(original)
    crlf = "\r\n" in original

    result = {"cases": CASES, "mutations": ["F1", "F4", "F2"]}
    try:
        io.open(SRC, "w", encoding="utf-8", newline="").write(
            apply(original, True, crlf))
        result["mutated_build"] = build_and_verify("mutated", outdir)
        result["mutated_tests"] = run_cases("mutated", outdir)
    finally:
        io.open(SRC, "w", encoding="utf-8", newline="").write(original)

    result["restored_build"] = build_and_verify("restored", outdir)
    result["restored_tests"] = run_cases("restored", outdir)

    mut = result["mutated_tests"]["per_case"]
    res = result["restored_tests"]["per_case"]
    verdict = {}
    for c in CASES:
        if res[c] != "PASS":
            verdict[c] = "BROKEN — fails against the FIXED code"
        elif mut[c] == "FAIL":
            verdict[c] = "DISCRIMINATING — red under the mutation, green fixed"
        else:
            verdict[c] = ("NOT A WITNESS FOR THIS MUTATION — "
                          + NON_WITNESSES.get(c, "UNEXPLAINED; investigate"))
    result["verdict"] = verdict
    result["discriminating_count"] = sum(
        1 for v in verdict.values() if v.startswith("DISCRIMINATING"))
    result["all_green_when_fixed"] = all(v == "PASS" for v in res.values())
    result["unexplained_non_witnesses"] = [
        c for c, v in verdict.items() if "UNEXPLAINED" in v]

    io.open(outdir / "phase56_review_fix_discrimination.json", "w",
            encoding="utf-8").write(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))

    if not result["all_green_when_fixed"]:
        sys.exit("A case fails against the FIXED code — fix that first.")
    if result["unexplained_non_witnesses"]:
        sys.exit("A case failed to discriminate with no recorded reason.")


if __name__ == "__main__":
    os.environ.setdefault("PYTHONIOENCODING", "utf-8")
    main()
