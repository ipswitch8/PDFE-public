#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Standing control for the four CI-only test failures triaged by F45 (phase-46).

WHAT THIS CLOSES
================
Four GoogleTest cases failed in ``windows-validation.yml``'s *"Run pdfe_tests"*
step, passed locally, were reproduced across two independent CI runs at
different HEADs, and were confirmed GENUINE (failed every rerun) rather than
flaky:

    F19DocClaims.ComparisonDocUiE2eCountsMatchTheSummaryArtifact
    FidelityR3_19_PdfaIndependent.ExternalPdfa_VeraPdfReference
    ConformanceIndependentTest.KnownGoodExternalPdfAIsAcceptedByChecker
    ConformanceIndependentTest.OwnValidatorAgreesWithIndependentChecker

They were fixed in phase-46 by committing the two gitignored INPUTS they
hard-assert on.  The risk this checker addresses is the NEXT sweep: a
green-again test carries no memory of having been red, so the next person to
see one of these names in a failure list has no way to know it was already
triaged, what the cause was, or that the ``.gitignore`` re-include keeping it
green is load-bearing.  Deleting the re-include reddens the test with an error
message that says nothing about F45.

So each of the four carries a marker in its own source, immediately above the
test macro::

    // PDFE-F45-CI-ONLY: <Suite>.<TestName>

and this checker FAILS if any of the four lacks it.

WHY A MARKER AND NOT ``flaky_quarantine.json``
==============================================
That registry has a narrow, different contract: ``run_gtest.py`` writes
``healed_flakes.json`` listing tests that FAILED and then PASSED on a
fresh-process rerun, and ``check_flaky_quarantine.py`` fails the build when a
HEALED test is unregistered.  A deterministic missing-input failure never heals,
so it never enters ``healed``, so the registry never sees it.  Nothing consumes
the ``xfail`` array to suppress anything either.  Registering these four there
would have been a marker that no mechanism reads.

WHAT THIS CHECKER DOES **NOT** CLAIM
====================================
It verifies that the triage record EXISTS and is attached to the right test.  It
does not verify the tests pass -- that is the suite's job -- and it does not
verify the ``.gitignore`` re-includes still work.  That second one IS checked,
separately and directly, by ``gate_inputs_are_tracked`` below, because a marker
describing a fix whose mechanism has been reverted is worse than no marker.

CRLF: ``core.autocrlf=true``.  Everything here is line-oriented and reads with
universal newlines, so no comparison depends on the byte form.

Exit: 0 all gates pass; 1 a gate failed; 2 usage/IO error.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

MARKER = "PDFE-F45-CI-ONLY"

# Fully-qualified test name -> repo-relative source file.
TRACKED = {
    "F19DocClaims.ComparisonDocUiE2eCountsMatchTheSummaryArtifact":
        "tests/test_f19_doc_claim_guards.cpp",
    "FidelityR3_19_PdfaIndependent.ExternalPdfa_VeraPdfReference":
        "tests/test_fidelity_r3_19_pdfa_independent.cpp",
    "ConformanceIndependentTest.KnownGoodExternalPdfAIsAcceptedByChecker":
        "tests/test_conformance_independent.cpp",
    "ConformanceIndependentTest.OwnValidatorAgreesWithIndependentChecker":
        "tests/test_conformance_independent.cpp",
}

# The two inputs phase-46 re-included.  If either stops being tracked, the four
# tests go red again in CI for the original reason.
REQUIRED_TRACKED_INPUTS = {
    "test_PDFs/crazyones-pdfa.pdf":
        "the known-good external PDF/A three of the four tests hard-assert on",
    "tests/_runtime/discovery_phase8/ui-suite-summary.json":
        "the UI-E2E summary the comparison document cites by name",
}

DISPOSITION_DOC = (
    "docs/audit/meta-audit-2026-07/followup/phase-46-dispositions.md")

# How far above the test macro the marker may sit.  Generous enough for a real
# explanation, bounded so a marker on some OTHER test cannot be credited here.
_LOOKBACK = 30

_TEST_MACRO_RE = re.compile(r"^\s*TEST(?:_F|_P)?\(\s*([A-Za-z_]\w*)\s*,\s*([A-Za-z_]\w*)\s*\)")


class Failure(Exception):
    pass


def _read(path: Path) -> str:
    try:
        with open(path, "r", encoding="utf-8", newline=None) as fh:
            return fh.read()
    except OSError as exc:
        raise Failure(f"cannot read {path}: {exc}")


def find_test(lines, suite: str, name: str):
    """Return the 0-based index of the `TEST*(suite, name)` line, or None."""
    for i, line in enumerate(lines):
        m = _TEST_MACRO_RE.match(line)
        if m and m.group(1) == suite and m.group(2) == name:
            return i
    return None


def gate_markers(problems, repo_root: Path):
    for fq, rel in sorted(TRACKED.items()):
        suite, name = fq.split(".", 1)
        path = repo_root / rel
        if not path.is_file():
            problems.append(
                f"M1 {rel} does not exist, but it is supposed to define {fq}. The "
                "triage record has rotted; re-derive it rather than deleting the row."
            )
            continue
        lines = _read(path).split("\n")
        idx = find_test(lines, suite, name)
        if idx is None:
            problems.append(
                f"M2 {rel}: no `TEST*({suite}, {name})` found, so {fq} is gone. "
                "The test F45 triaged was "
                "renamed or removed. If it was renamed, update TRACKED deliberately; a "
                "rename must not silently drop it out of this gate's scope."
            )
            continue

        # Only look back as far as the previous test macro, so a marker written
        # for the test above cannot be credited to this one.
        start = max(0, idx - _LOOKBACK)
        for j in range(idx - 1, start - 1, -1):
            if _TEST_MACRO_RE.match(lines[j]):
                start = j + 1
                break
        window = "\n".join(lines[start:idx])

        if MARKER not in window:
            problems.append(
                f"M3 {rel}:{idx + 1} test {fq} has no `{MARKER}` marker in the "
                f"{idx - start} line(s) above it. This test was a genuine, "
                "reproduced-across-two-CI-runs failure triaged by F45; without the "
                "marker the next sweep sees an untriaged name. Add the marker comment "
                f"above the test macro (see {DISPOSITION_DOC})."
            )
            continue

        if f"{MARKER}: {fq}" not in window:
            problems.append(
                f"M4 {rel}:{idx + 1} test {fq} has a `{MARKER}` marker, but it does not "
                f"name this test. The marker line must read exactly "
                f"`// {MARKER}: {fq}` so the record cannot drift onto the wrong test."
            )

        if DISPOSITION_DOC not in window:
            problems.append(
                f"M5 {rel}:{idx + 1} test {fq}'s marker does not reference "
                f"{DISPOSITION_DOC}. A marker that says 'this was triaged' without "
                "saying WHERE the triage is recorded sends the next reader nowhere."
            )


def _tracked_files(repo_root: Path):
    out = subprocess.run(
        ["git", "-C", str(repo_root), "ls-files"],
        capture_output=True, text=True, check=False,
    )
    if out.returncode != 0:
        raise Failure("`git ls-files` failed; this check must run inside the repo")
    return {p for p in out.stdout.replace("\\", "/").split("\n") if p}


def gate_inputs_are_tracked(problems, repo_root: Path):
    """The fix's MECHANISM, checked directly.

    `git check-ignore` is NOT used here on purpose: it exits 0 on a `!`
    re-include match, so it reports a re-included file as ignored. `git
    ls-files` reports what git actually tracks, which is the property that
    matters.
    """
    tracked = _tracked_files(repo_root)
    if DISPOSITION_DOC not in tracked:
        problems.append(
            f"M7 {DISPOSITION_DOC} is not tracked by git, yet all four markers point "
            "readers at it. A marker whose destination does not exist is a dead end, "
            "which is the failure mode the marker was added to prevent."
        )
    for rel, why in sorted(REQUIRED_TRACKED_INPUTS.items()):
        if rel not in tracked:
            problems.append(
                f"M6 {rel} is not tracked by git ({why}). F45's fix was to re-include "
                "it in .gitignore and commit it; without it the CI checkout "
                "(actions/checkout runs `git clean -ffdx`) has no such file and the "
                "tests it feeds go red again on a missing input. Re-add it rather "
                "than converting the assertions to skips."
            )


def run_all(repo_root: Path | None = None):
    root = repo_root or REPO_ROOT
    problems: list[str] = []
    gate_markers(problems, root)
    gate_inputs_are_tracked(problems, root)
    return problems


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):  # pragma: no cover
            pass

    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument(
        "--repo-root", default=None, metavar="PATH",
        help="check a snapshot tree instead of this repository. This is what makes "
             "the control discriminating: point it at a copy with a marker removed "
             "and it must FAIL. Note --markers-only is implied for a snapshot that "
             "is not a git repo.")
    ap.add_argument("--markers-only", action="store_true",
                    help="skip the git-tracked-input gate (M6)")
    args = ap.parse_args(argv)

    root = Path(args.repo_root).resolve() if args.repo_root else REPO_ROOT
    try:
        problems: list[str] = []
        gate_markers(problems, root)
        if not args.markers_only:
            gate_inputs_are_tracked(problems, root)
    except Failure as exc:
        print(f"[f45-markers] ERROR: {exc}", file=sys.stderr)
        return 2

    if problems:
        print(f"[f45-markers] FAIL — {len(problems)} problem(s):")
        for p in problems:
            print(f"  * {p}")
        return 1

    print(
        f"[f45-markers] OK — all {len(TRACKED)} tests triaged by F45 carry a "
        f"`{MARKER}` marker naming the test and the disposition record"
        + ("" if args.markers_only else
           f"; both re-included inputs are tracked by git.")
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
