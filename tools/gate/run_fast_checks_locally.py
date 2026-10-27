#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Run the gate checks that `fast-checks.yml` runs, locally, in one command.

WHY THIS EXISTS (DCA 0051, and DCA 0047 before it).

Twice now, commits have been pushed whose messages said "the guards are green".
They were green *locally* -- meaning the two or three guards that happened to be
run by hand. Nothing established the rest. Both times fast-checks was already
red and stayed red for many consecutive pushes:

  * DCA 0047 -- red for five commits; four real failures hid under a noisy one.
  * DCA 0051 -- red for TEN consecutive pushes across an entire 18-phase
    pipeline, hiding a reintroduced WIN-87 publish-shape defect in shipped code.

The lesson ("NO GATE EXECUTES CI") had been written down twice and did not
change the outcome, because a written lesson is not a mechanism. This is the
mechanism: it makes "the guards are green" a claim that can actually be
CHECKED before pushing, instead of assumed.

DISCOVERY, NOT A HARDCODED LIST. The invocations are parsed out of
`.github/workflows/fast-checks.yml` itself, so a guard added to CI is picked up
here without anyone remembering to update a second list -- the drift that makes
mirror-scripts worthless within a month.

WHAT IT CANNOT RUN, IT SAYS. Some steps need GitHub run context (a base/head
SHA pair, a run id, a token). Those are REPORTED AS SKIPPED WITH A REASON, never
silently dropped: a runner that quietly covered 30 of 39 checks and printed
"PASS" would be a worse lie than the one this replaces.

Usage:
    python tools/gate/run_fast_checks_locally.py
    python tools/gate/run_fast_checks_locally.py --list
    python tools/gate/run_fast_checks_locally.py --self-test

Exit codes: 0 = every runnable check passed, 1 = at least one failed,
2 = the workflow could not be parsed (which is itself a failure to report).
"""
from __future__ import annotations

import argparse
import io
import os
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "fast-checks.yml"

# LOCAL-ONLY EXTRAS -- the deliberate exception to "discovery, not a list".
#
# Everything above is discovered from fast-checks.yml precisely so this file
# cannot drift. An entry here is a check that CANNOT be in that workflow and
# would otherwise run nowhere at all. Each one carries the reason it cannot,
# it is printed under its own heading so it is never mistaken for CI coverage,
# and the self-test fails if the script it names does not exist -- a hardcoded
# list that can rot silently is the thing this file was written against.
LOCAL_ONLY: list[tuple[str, str]] = [
    ("python tools/gate/check_staged_runtime_closure.py "
     "build/vs2022/bin/Debug/pdfe.exe build/vs2022/bin/Debug/pdfe_tests.exe",
     "GitHub #281 -- reads the PE IMPORT TABLES of the built images and "
     "requires every imported DLL to be staged beside them, resolvable as a "
     "system DLL, or an API set. It cannot live in fast-checks.yml for the "
     "same reason the perf floor cannot: there is no build there, and a "
     "closure check with no binaries to read would be structurally unable to "
     "fail. Its judgement half IS enforced on push, as --self-test in the "
     "pytest batch. Here it reads the images this machine actually built. "
     "Replaces the C-ENUM shape in tests/test_dll_dependencies.cpp, whose "
     "hand-typed roster misses three real staged dependencies (gtest.dll, "
     "qt6networkd.dll, qt6testd.dll) -- a dependency absent from a typed "
     "list is indistinguishable from one that does not exist."),

    ("python tools/gate/run_security_regression_check.py",
     "GitHub #201 -- runs scripts/security/check_regressions.ps1 for real and "
     "rejects a vacuous comparison. It cannot live in fast-checks.yml: every "
     "fresh scanner output it compares (sast/findings.json, deps/cves.json, "
     "secrets/findings.json, corpus_run_*.json) is gitignored, so on a "
     "GitHub-hosted runner it would compare nothing on every push -- a step "
     "structurally unable to fail. Its judgement half IS enforced on push, as "
     "--self-test. Here it compares whatever this box actually has."),
]

# OPT-IN LOCAL EXTRAS -- correct to run here, too slow to run every time.
#
# GitHub #258. These are guards that CAN be run on a developer box and cannot
# be run on push, but whose runtime would destroy the point of this script: it
# exists so "are the guards green?" is answerable in about two minutes, and a
# tool nobody runs because it takes an hour guards nothing.
#
# They are NOT silently dropped. When --slow is absent they are printed under
# their own heading with the measured runtime and the reason, because an unrun
# check reported as nothing is the failure mode this whole file was written
# against (DCA 0047). The entry states what it costs so the choice to skip it
# is an informed one rather than an accident.
#: Entries are (command, why, prepare) where `prepare` is a command that
#: PRODUCES the input the checker needs, or None.
#:
#: GitHub #258. Two of #187's seven guards take an artifact that only a build
#: produces -- a measured-timings JSON, a Cobertura report -- and a checker
#: pointed at a file that does not exist is either a hard error on every run or
#: a step structurally unable to fail. Neither is a guard. So an entry may bring
#: its own input: the prepare command runs first, and if it fails the entry is
#: reported as FAILED rather than skipped, because an input that could not be
#: produced is not evidence of a passing check.
# GitHub #292. `check_coverage_floor.py --self-test` USED TO SIT AT THE TOP OF
# THIS LIST, AND MUST NOT BE PUT BACK.
#
# It was added here and to fast-checks.yml in the SAME commit, and the two are
# contradictory: this list means "too slow or too environment-dependent for CI,
# so run it locally", while `fast-checks.yml:125` runs that exact invocation on
# every push. The entry's own rationale said "fast-checks.yml names only its
# companion test_check_coverage_floor.py" -- which that commit made false in the
# act of writing it. The self-test's whole point was to be runnable WITHOUT a
# coverage run, which is precisely why CI can have it and why it is not
# local-only.
#
# This module's own --self-test catches the collision (it would otherwise be
# counted twice and reported as local-only coverage that CI lacks), so the list
# was red at HEAD until the entry was removed.
#
# Nothing was lost with it: the reasoning that mattered -- that a companion test
# calling check() directly proves the PREDICATE computes and says nothing about
# whether the PROCESS reports it, the distinction a watchdog in this repo
# demonstrated by failing every heartbeat write and exiting 0 -- is preserved at
# the CI step that actually runs it, `fast-checks.yml:116-124`.
#
# The REAL coverage-floor gate remains `windows-validation.yml:1673`; it needs
# the Cobertura XML from OpenCppCoverage and so belongs in neither list.
SLOW_LOCAL_ONLY: list[tuple[str, str, str | None]] = [
    ("python tools/gate/page_load_guard_scan.py",
     "GitHub #258 -- one of #187's original seven, and dispatch-only until now: "
     "fast-checks.yml names only its companion test_page_load_guard_scan.py, "
     "which exercises the checker's logic on synthetic input and never scans "
     "the tree. It cannot go on push for two independent reasons, both "
     "measured 2026-09-29: it needs a CONFIGURED build tree "
     "(build/vs2022-asan/compile_commands.json, exit 2 without one), which a "
     "GitHub-hosted runner does not have; and a full clang AST parse of 244 "
     "translation units takes about 55 minutes. It earns its keep -- 9 of the "
     "14 call sites it finds are reached through MACRO EXPANSION, which the "
     "lexical scanner cannot see by construction. Opt in with --slow.",
     None),

    ("python tools/gate/check_perf_floor.py "
     "--baselines tools/gate/perf_baselines.json "
     "--measured tests/_runtime/perf-measured.json",
     "GitHub #258 -- one of #187's original seven, dispatch-only until now "
     "because its input is a measured-timings JSON that only a gtest run "
     "produces, and a floor compared against a missing file cannot fail. It "
     "now BRINGS ITS OWN INPUT: the prepare command runs the PerfBenchmark "
     "suite, which emits perf-measured.json, and the floor is then compared "
     "against real timings from this machine. Measured 2026-09-30: the suite "
     "is 5 tests and takes seconds, so the cost here is the build it needs, "
     "not the run. Still cannot go on push -- fast-checks.yml has no build and "
     "no gtest at all.",
     "python tests/scripts/run_perf_benchmark.py"),
]

# Placeholders that mean "this needs GitHub run context we do not have locally".
CONTEXT_TOKENS = ("${{", "$BASE", "$HEAD", "$GITHUB_", "$RUN_ID", "$GH_TOKEN")

# Invocations that are self-tests of a checker rather than the checker itself
# are still run -- they are cheap and they are exactly what caught a dead guard
# in DCA 0047.

# Three invocation SHAPES appear in this workflow, and the first version of this
# file matched only the first. It therefore discovered 16 of 64 referenced
# scripts and would have printed a confident PASS having run a quarter of the
# guards -- rebuilding, in the tool meant to fix silent under-coverage, exactly
# the silent under-coverage it exists to fix. Caught by comparing discovered
# scripts against every script the workflow mentions; that comparison is now the
# self-test below, so the next shape cannot slip through quietly either.
#
#   1. a block line:      `  python tools/gate/x.py --flag`
#   2. a folded `run:`:   `  run: python tools/gate/x.py --flag`
#   3. a pytest fan-out:  `  python -m pytest -q` followed by indented file
#                         paths on their own lines (YAML `>` folding)
_DIRECT_RE = re.compile(
    r"^\s*(?:run:\s*)?(python3?\s+(?:tools/gate|tests/scripts)/"
    r"[A-Za-z0-9_]+\.py[^\r\n]*)$")
_PYTEST_HEAD_RE = re.compile(r"^\s*(?:run:\s*)?(python3?\s+-m\s+pytest\b[^\r\n]*)$")
# Any repo-relative .py path, NOT just tools/gate and tests/scripts. The narrow
# version stopped absorbing continuation lines the moment it hit a
# `tests/ui_automation/...` entry mid-list, silently dropping the three paths
# BELOW it -- a scan that gives up quietly partway through a list is the same
# defect as one that never started.
_PATH_ONLY_RE = re.compile(r"^\s*((?:tools|tests)/[A-Za-z0-9_/]+\.py)\s*$")

# A folded `run: >` block puts a command's ARGUMENTS on the following lines.
# Dropping them does not merely lose coverage -- it runs the checker with the
# WRONG arguments and reports a red that is purely this tool's fault. Two guards
# (reachability-skip, magnitude) failed exactly that way in the first run, and a
# runner that reddens on correct code is the thing DCA 0047 says gets muted.
_ARG_CONT_RE = re.compile(r"^\s*(--?[A-Za-z0-9][^\r\n]*|[A-Za-z0-9_./-]+)\s*$")

# A step that deliberately expects a NON-ZERO exit (a negative control) inverts
# the code itself. Running the bare command here and calling non-zero a failure
# would invert the meaning of the check.
_NEGATIVE_CONTROL_MARKERS = ("set +e", "rc=$?")


# GitHub #260. This mirror models a step's COMMAND and not the JOB it sits in,
# so every step looks alike to it. CI does not work that way: each job installs
# its own dependencies, and a step placed in a job that never installed what it
# imports dies there with ModuleNotFoundError while passing perfectly here --
# where the developer's interpreter has everything. That is exactly how commit
# 4cd0e101 went red: a step needing PyYAML was added to `installer-dry-run`,
# whose pip line installs only pytest.
#
# Modelled from the workflow rather than hardcoded: which job each step is in,
# and what that job pip-installs.
_JOB_RE = re.compile(r"^  ([A-Za-z0-9][A-Za-z0-9_-]*):\s*$")
_PIP_RE = re.compile(r"pip\s+install\b([^\r\n]*)")
_PIP_FLAG_RE = re.compile(r"^--")

#: import name -> the pip distribution that provides it. Only third-party
#: modules a gate step actually imports; the stdlib needs no install.
MODULE_TO_PACKAGE = {
    "yaml": "pyyaml",
    "pikepdf": "pikepdf",
    "numpy": "numpy",
    "fitz": "pymupdf",
    "pymupdf": "pymupdf",
    "fontTools": "fonttools",
    "clang": "libclang",
    "pytest": "pytest",
    "cryptography": "cryptography",
}

def _required_imports(source: str) -> set:
    """Third-party modules a script imports UNCONDITIONALLY.

    An import inside `try:` (or guarded by importlib.util.find_spec) is an
    OPTIONAL dependency the script has chosen to degrade without -- flagging it
    would redden CI for behaviour that is working as designed.
    `check_crl_chain_fixture.py` is the live example: it imports `cryptography`
    in a try block and prints "regeneration check unavailable" when it is
    absent, which is exactly what CI shows today. Only a top-level import can
    turn into the ModuleNotFoundError this check exists to predict.
    """
    import ast
    try:
        tree = ast.parse(source)
    except SyntaxError:
        return set()

    guarded = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Try):
            for sub in ast.walk(node):
                guarded.add(id(sub))

    found = set()
    for node in ast.walk(tree):
        if id(node) in guarded:
            continue
        if isinstance(node, ast.Import):
            for a in node.names:
                found.add(a.name.split(".")[0])
        elif isinstance(node, ast.ImportFrom) and node.module:
            found.add(node.module.split(".")[0])
    return {m for m in found if m in MODULE_TO_PACKAGE}


def jobs_with_steps(workflow: Path) -> dict:
    """{job: {"installs": set[str], "commands": [str]}} from the workflow text.

    A text walk for the same reason `discover` is one: the thing being mirrored
    is the literal command a maintainer reads in review.
    """
    out: dict = {}
    if not workflow.exists():
        return out
    lines = io.open(workflow, encoding="utf-8", errors="replace").read().splitlines()

    job = None
    in_jobs = False
    for line in lines:
        if line.startswith("jobs:"):
            in_jobs = True
            continue
        if not in_jobs:
            continue
        m = _JOB_RE.match(line)
        if m:
            job = m.group(1)
            out.setdefault(job, {"installs": set(), "commands": []})
            continue
        if job is None:
            continue
        pm = _PIP_RE.search(line)
        if pm:
            for tok in pm.group(1).split():
                if _PIP_FLAG_RE.match(tok) or tok.startswith("-"):
                    continue
                out[job]["installs"].add(tok.lower())
        out[job]["commands"].append(line)
    return out


def _script_paths_in(command: str, repo_root: Path) -> list:
    """The .py files a command actually runs."""
    hits = []
    for tok in command.split():
        tok = tok.strip("'\"")
        if tok.endswith(".py"):
            p = repo_root / tok
            if p.exists():
                hits.append(p)
    return hits


def modules_required_by(command: str, repo_root: Path) -> set:
    """Third-party import names the command's scripts need."""
    needed = set()
    if re.search(r"\bpytest\b", command):
        needed.add("pytest")
    for p in _script_paths_in(command, repo_root):
        try:
            body = io.open(p, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        needed |= _required_imports(body)
    return needed


def missing_job_dependencies(workflow: Path, repo_root: Path) -> list:
    """[(job, command, module, package)] for steps their job cannot import."""
    problems = []
    for job, info in sorted(jobs_with_steps(workflow).items()):
        installed = info["installs"]
        for raw in info["commands"]:
            line = raw.strip()
            if not line.startswith(("python ", "- run:", "run:")) and \
                    "python " not in line:
                continue
            for mod in sorted(modules_required_by(line, repo_root)):
                pkg = MODULE_TO_PACKAGE[mod]
                if pkg.lower() not in installed:
                    problems.append((job, line, mod, pkg))
    return problems


def discover(workflow: Path) -> list[str]:
    """Every gate command line in the workflow, as a runnable command string.

    Deliberately a text scan of the `run:` blocks rather than a YAML walk: the
    thing being mirrored is the literal command a maintainer reads in review.
    """
    if not workflow.exists():
        return []
    text = io.open(workflow, encoding="utf-8", errors="replace").read()
    lines = text.splitlines()
    out: list[str] = []

    def add(cmd: str) -> None:
        cmd = re.sub(r"\s*\\$", "", cmd).strip()
        if cmd and cmd not in out:
            out.append(cmd)

    i = 0
    while i < len(lines):
        line = lines[i]

        m = _PYTEST_HEAD_RE.match(line)
        if m:
            # Absorb the folded continuation lines that are bare test paths.
            head = m.group(1).strip()
            files: list[str] = []
            j = i + 1
            while j < len(lines):
                # SKIP COMMENT LINES INSIDE THE FILE LIST, do not stop at them.
                #
                # Absorption used to break on the first line that was not a bare
                # path, so a `#` comment in the middle of the list silently
                # truncated it and every entry after the comment was invisible to
                # this mirror. Measured 2026-10-07: that was already hiding
                # test_canvas_crop_semantics.py and test_canvas_crop_coverage.py,
                # the second of which was added to fast-checks.yml specifically so
                # a guard would stop being unwired -- it runs on push, and had
                # never once run locally.
                #
                # This is the same failure family as #258: a check that is
                # referenced and still does not run. Here the reference was real
                # and the DISCOVERY was blind, which is harder to see, because
                # the file list looks right in the workflow.
                if lines[j].lstrip().startswith("#"):
                    j += 1
                    continue
                pm = _PATH_ONLY_RE.match(lines[j])
                if not pm:
                    break
                files.append(pm.group(1))
                j += 1
            # A pytest head with inline paths already carries its own files.
            add(head + ("" if not files else " " + " ".join(files)))
            i = j
            continue

        m = _DIRECT_RE.match(line)
        if m:
            cmd = m.group(1).strip()
            # Absorb folded-block arguments: more-indented continuation lines
            # that are flags or bare tokens, stopping at a blank line or at
            # anything that starts a new YAML key / list item.
            base_indent = len(line) - len(line.lstrip())
            j = i + 1
            while j < len(lines):
                nxt = lines[j]
                if not nxt.strip():
                    break
                indent = len(nxt) - len(nxt.lstrip())
                if indent < base_indent:
                    break
                s = nxt.strip()
                if s.startswith("- ") or s.startswith("#") or ":" in s.split()[0]:
                    break
                if not _ARG_CONT_RE.match(nxt):
                    break
                cmd += " " + s
                j += 1
            add(cmd)
            i = j
            continue
        i += 1

    return out


def negative_control_commands(workflow: Path) -> set[str]:
    """Commands inside a step that deliberately expects a non-zero exit.

    Detected by the step's own inversion machinery (`set +e` / `rc=$?`) rather
    than by a hardcoded list, so a new negative control is handled without
    anyone remembering this file exists.
    """
    if not workflow.exists():
        return set()
    text = io.open(workflow, encoding="utf-8", errors="replace").read()
    out: set[str] = set()
    # Split into steps on the `- name:` boundary and inspect each step's body.
    steps = re.split(r"\n\s+- name:", text)
    for step in steps:
        if not any(mk in step for mk in _NEGATIVE_CONTROL_MARKERS):
            continue
        for line in step.splitlines():
            m = _DIRECT_RE.match(line)
            if m:
                out.add(m.group(1).strip())
    return out


def _strip_yaml_comments(text: str) -> str:
    """Drop `#`-to-end-of-line, outside single or double quotes.

    Crude on purpose. A `#` inside a quoted string is stripped too, which can
    only ever make the resulting reference set SMALLER -- i.e. it can never
    invent a reference that is not there, only miss one. For a coverage
    yardstick that is the safe direction to be wrong in, and the
    non-vacuity test guards the other side.
    """
    out: list[str] = []
    for line in text.splitlines():
        in_s = in_d = False
        cut = len(line)
        for i, ch in enumerate(line):
            if ch == "'" and not in_d:
                in_s = not in_s
            elif ch == '"' and not in_s:
                in_d = not in_d
            elif ch == "#" and not in_s and not in_d:
                cut = i
                break
        out.append(line[:cut])
    return "\n".join(out)


def referenced_scripts(workflow: Path) -> set[str]:
    """Every gate script the workflow actually INVOKES -- the coverage yardstick.

    GitHub #311. This used to grep the whole file TEXT, so a script named only
    in a YAML COMMENT counted as referenced. `fast-checks.yml:292` says

        # tree is tests/scripts/remediation_summary.py, which builds an

    and that one comment made `test_every_referenced_gate_script_is_discovered`
    fail permanently: the runner cannot discover a command that does not exist,
    so the yardstick reported a gap that was prose. The workflow never invokes
    that script, and the real file is under `tests/scripts/`, not `tools/gate/`
    where the message implied.

    A guard red on `master` for a reason unrelated to any push is the condition
    under which a standing red stops being read -- the same complaint #305 was
    filed for.

    The fix is semantic rather than a cleverer regex: scrape only the `run:`
    blocks, which is what "referenced by the workflow" has to mean if the
    comparison against *discovered commands* is to say anything. A comment is
    not an invocation.

    Falls back to comment-stripped raw text if the YAML cannot be parsed, so a
    malformed workflow degrades to the old behaviour minus the comment bug
    rather than silently reporting nothing -- an empty yardstick would make the
    coverage check vacuous, which is worse than over-reporting.
    """
    if not workflow.exists():
        return set()
    text = io.open(workflow, encoding="utf-8", errors="replace").read()
    pattern = r"(?:tools/gate|tests/scripts)/([A-Za-z0-9_]+\.py)"

    try:
        import yaml  # noqa: PLC0415 -- optional, and only needed here
        doc = yaml.safe_load(text)
        runs: list[str] = []

        def walk(node):
            # `run:` can appear at any depth once reusable workflows and
            # composite actions are in play, so recurse rather than assume
            # jobs -> steps.
            if isinstance(node, dict):
                for k, v in node.items():
                    if k == "run" and isinstance(v, str):
                        runs.append(v)
                    else:
                        walk(v)
            elif isinstance(node, list):
                for item in node:
                    walk(item)

        walk(doc)
        if runs:
            # Strip comments INSIDE the run blocks too: a commented-out command
            # line is not an invocation either. The first version of this fix
            # missed that, and this module's own new test caught it.
            return set(re.findall(
                pattern, _strip_yaml_comments("\n".join(runs))))
        # Parsed but no run blocks at all: fall through rather than return an
        # empty set, which would quietly disable the coverage comparison.
    except Exception:
        pass

    # Fallback: the same shared comment stripper over the raw text.
    return set(re.findall(pattern, _strip_yaml_comments(text)))


def discovered_scripts(cmds: list[str]) -> set[str]:
    found: set[str] = set()
    for c in cmds:
        found.update(re.findall(
            r"(?:tools/gate|tests/scripts)/([A-Za-z0-9_]+\.py)", c))
    return found


def classify(cmd: str, negatives: set[str] | None = None) -> tuple[bool, str]:
    """(runnable, reason-if-not)."""
    for tok in CONTEXT_TOKENS:
        if tok in cmd:
            return False, f"needs GitHub run context ({tok})"
    if negatives and cmd in negatives:
        return False, ("negative control -- the step expects a NON-ZERO exit "
                       "and inverts it in-step; running it bare would report "
                       "correct behaviour as a failure")
    return True, ""


#: Per-command wall clock for an ordinary check. A CI-mirrored guard that runs
#: for a quarter of an hour locally is broken, so the cap is also a smoke alarm.
DEFAULT_TIMEOUT_S = 900

#: The SLOW tier's cap. GitHub #258: the first version of the slow tier reused
#: DEFAULT_TIMEOUT_S, and page_load_guard_scan.py -- whose measured runtime is
#: about 55 minutes -- was killed at 900s and reported `exit 124` every time. A
#: guard wired so it can only ever fail is as useless as one that can only ever
#: pass, and rather more confusing, because the red looks like a finding. Caught
#: by running the tier rather than by reasoning about it.
SLOW_TIMEOUT_S = 7200


def run_one(cmd: str, timeout_s: int = DEFAULT_TIMEOUT_S) -> tuple[int, str]:
    exe = sys.executable
    parts = cmd.split()
    if parts and parts[0].startswith("python"):
        parts = [exe] + parts[1:]
    try:
        p = subprocess.run(parts, cwd=str(REPO_ROOT), capture_output=True,
                           text=True, encoding="utf-8", errors="replace",
                           timeout=timeout_s)
    except subprocess.TimeoutExpired:
        return 124, f"TIMEOUT after {timeout_s}s"
    except Exception as e:  # noqa: BLE001
        return 125, f"could not launch: {e}"
    tail = (p.stdout or "").strip().splitlines()
    tail_err = (p.stderr or "").strip().splitlines()
    last = (tail[-1] if tail else (tail_err[-1] if tail_err else ""))
    return p.returncode, last[:200]


def self_test() -> int:
    """Prove the discovery half works and cannot silently find nothing."""
    failures = []
    cmds = discover(WORKFLOW)
    if not cmds:
        failures.append("discovered 0 invocations -- the scan is broken or the "
                        "workflow moved; a runner that finds nothing must not "
                        "be able to report PASS")
    # The WIN-87 sweep is the guard whose unread red motivated this file. If it
    # is not discovered, this runner would not have caught DCA 0051 either.
    if not any("check_win87_publish_shapes.py" in c for c in cmds):
        failures.append("check_win87_publish_shapes.py not discovered")
    if not any("check_dca_presence.py" in c for c in cmds):
        failures.append("check_dca_presence.py not discovered")
    # Context-needing invocations must be classified as such, not run blindly.
    negatives = negative_control_commands(WORKFLOW)
    ctx = [c for c in cmds if not classify(c, negatives)[0]]
    if not ctx:
        failures.append("no context-needing invocation was classified; "
                        "check_dca_presence.py takes --base/--head, so at "
                        "least one was expected")

    # THE COVERAGE ASSERTION. This is the one that matters: the first version of
    # discover() matched one invocation shape out of three and silently covered
    # a quarter of the guards while reporting PASS. Comparing what was
    # DISCOVERED against every script the workflow MENTIONS is what caught that,
    # so it is pinned here rather than left as a thing someone might re-check.
    ref = referenced_scripts(WORKFLOW)
    got = discovered_scripts(cmds)
    missing = sorted(ref - got)
    if missing:
        failures.append(
            f"{len(missing)} script(s) are referenced by the workflow but not "
            f"discovered, so this runner would report a pass it did not "
            f"establish: {', '.join(missing[:8])}"
            + (" …" if len(missing) > 8 else ""))

    # THE LOCAL-ONLY LIST MUST NOT ROT. It is the one hardcoded list in this
    # file, so it gets the treatment a hardcoded list needs: the script it
    # names must exist and must be runnable, and it must NOT be something
    # fast-checks.yml already runs (which would make the "local-only" label a
    # lie and double-count it).
    # Both hardcoded lists get the same treatment; SLOW_LOCAL_ONLY is no less
    # able to rot for being opt-in, and an opt-in entry naming a script that
    # has been renamed would fail only for whoever passed --slow.
    for label, table in (("LOCAL_ONLY", LOCAL_ONLY),
                         ("SLOW_LOCAL_ONLY", SLOW_LOCAL_ONLY)):
        for entry in table:
            cmd, why = entry[0], entry[1]
            prepare = entry[2] if len(entry) > 2 else None
            if prepare:
                # A prepare command that does not exist is a slow entry that can
                # never run, which is the same as not being wired at all.
                pscript = next(
                    (t for t in prepare.split() if t.endswith(".py")), "")
                if not pscript or not (REPO_ROOT / pscript).exists():
                    failures.append(
                        f"{label} entry {cmd!r} names prepare command "
                        f"{prepare!r}, whose script does not exist")
            script = next((tok for tok in cmd.split() if tok.endswith(".py")), "")
            if not script or not (REPO_ROOT / script).exists():
                failures.append(f"{label} names {script or cmd!r}, which does "
                                f"not exist in the tree")
            if len(why) < 40:
                failures.append(f"{label} entry {cmd!r} has no written reason "
                                f"for why it cannot be in fast-checks.yml")
            if cmd in cmds:
                failures.append(
                    f"{label} entry {cmd!r} is the SAME invocation "
                    f"fast-checks.yml already runs; it is not local-only and "
                    f"would be counted twice")
    # An entry in BOTH tables would run twice under --slow and be reported
    # under two headings.
    both = {c for c, _ in LOCAL_ONLY} & {e[0] for e in SLOW_LOCAL_ONLY}
    if both:
        failures.append(
            f"{len(both)} invocation(s) appear in both LOCAL_ONLY and "
            f"SLOW_LOCAL_ONLY: {', '.join(sorted(both))}")

    if failures:
        for f in failures:
            print(f"SELF-TEST FAIL: {f}")
        return 1
    print(f"SELF-TEST PASS: {len(cmds)} invocation(s) discovered, "
          f"{len(ctx)} correctly classified as needing CI context, "
          f"{len(LOCAL_ONLY)} local-only extra(s) verified present.")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true",
                    help="print what would run, and what would be skipped")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--slow", action="store_true",
                    help="also run SLOW_LOCAL_ONLY guards (minutes to an hour "
                         "each). Without it they are reported as not run, "
                         "never silently dropped.")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    cmds = discover(WORKFLOW)
    if not cmds:
        print(f"FAIL: no gate invocations discovered in {WORKFLOW}. "
              f"This runner cannot report a pass it did not establish.")
        return 2

    negatives = negative_control_commands(WORKFLOW)
    runnable = [(c, *classify(c, negatives)) for c in cmds]
    local_cmds = [c for c, _ in LOCAL_ONLY]
    runnable += [(c, True, "") for c in local_cmds if c not in cmds]

    slow_cmds = [e[0] for e in SLOW_LOCAL_ONLY]
    slow_prepare = {e[0]: (e[2] if len(e) > 2 else None)
                    for e in SLOW_LOCAL_ONLY}
    if args.slow:
        runnable += [(c, True, "") for c in slow_cmds if c not in cmds]

    if args.list:
        for cmd, ok, why in runnable:
            if cmd in slow_cmds:
                tag = "SLOW  "
            elif cmd in local_cmds:
                tag = "LOCAL "
            else:
                tag = "RUN   " if ok else "SKIP  "
            print(tag + cmd + ("" if ok else f"   [{why}]"))
        if not args.slow:
            for cmd in slow_cmds:
                print("SLOW- " + cmd + "   [omitted; pass --slow to include]")
        return 0

    # GitHub #260. Before running anything, check each step against the JOB it
    # lives in. Every step looks alike to a command-only mirror, and the
    # developer's interpreter has every package -- so a step in a job that never
    # installed what it imports passes here and dies on CI. This is the one
    # class of failure this tool structurally could not see.
    dep_problems = missing_job_dependencies(WORKFLOW, REPO_ROOT)
    if dep_problems:
        print("[fast-checks-local] JOB-DEPENDENCY MISMATCH — these steps would "
              "die on CI with ModuleNotFoundError and pass here:")
        for job, cmd, mod, pkg in dep_problems:
            print(f"  [{job}] imports {mod!r} (pip: {pkg}) but that job does "
                  f"not install it:\n        {cmd}")
        print("  Move the step to a job that installs it, or add the package "
              "to that job's pip line.")

    passed, failed, skipped = [], [], []
    for cmd, ok, why in runnable:
        if not ok:
            skipped.append((cmd, why))
            continue
        # GitHub #258. A slow entry may bring its own input. The prepare step
        # runs first, and a prepare FAILURE fails the entry rather than skipping
        # it: a checker whose input could not be produced has not passed, and
        # reporting it as anything other than red is the exact shape this file
        # exists to prevent.
        is_slow = cmd in slow_cmds
        budget = SLOW_TIMEOUT_S if is_slow else DEFAULT_TIMEOUT_S
        prep = slow_prepare.get(cmd)
        if prep:
            prc, plast = run_one(prep, budget)
            if prc != 0:
                failed.append((prep, prc, plast))
                print(f"  FAIL {prep}   (rc={prc}) {plast}")
                print(f"  FAIL {cmd}   (its input could not be produced)")
                failed.append((cmd, prc, "input not produced"))
                continue
            print(f"  ok   {prep}   (prepared input for the next check)")
        rc, last = run_one(cmd, budget)
        if rc == 0:
            passed.append(cmd)
            print(f"  ok   {cmd}")
        else:
            failed.append((cmd, rc, last))
            print(f"  FAIL {cmd}  (exit {rc})")
            if last:
                print(f"         {last}")

    print()
    print(f"[fast-checks-local] {len(passed)} passed, {len(failed)} failed, "
          f"{len(skipped)} skipped.")
    if local_cmds:
        # Named separately and always: counting a local-only check inside the
        # same total as the CI-mirrored ones would let "the guards are green"
        # quietly mean "including one that push never runs".
        print("[fast-checks-local] LOCAL-ONLY (included in the totals above; "
              "NOT run by fast-checks.yml on push):")
        for cmd, why in LOCAL_ONLY:
            print(f"    {cmd}")
            print(f"        {why}")
    if SLOW_LOCAL_ONLY:
        # Never silent, whether they ran or not. A guard omitted for cost is
        # still a guard that did not run, and the reader has to be told which
        # of the two happened -- reporting only the ones that ran is how a
        # partial sweep comes to read as a complete one.
        if args.slow:
            print("[fast-checks-local] SLOW LOCAL-ONLY (RAN, included in the "
                  "totals above; NOT run by fast-checks.yml on push):")
            for cmd, why in ((e[0], e[1]) for e in SLOW_LOCAL_ONLY):
                print(f"    {cmd}")
                print(f"        {why}")
        else:
            print("[fast-checks-local] SLOW LOCAL-ONLY -- NOT RUN (pass --slow "
                  "to include). These are NOT enforced on push either, so "
                  "nothing has checked them:")
            for cmd, why in ((e[0], e[1]) for e in SLOW_LOCAL_ONLY):
                print(f"    {cmd}")
                print(f"        {why}")
    if skipped:
        # Never silent: an unrun check is not a passing check.
        print("[fast-checks-local] NOT RUN LOCALLY (need CI context) -- these "
              "are still enforced on push:")
        for cmd, why in skipped:
            print(f"    {cmd}   [{why}]")
    if failed:
        print()
        print("[fast-checks-local] FAIL: the checks above are red HERE, which "
              "means they are red on the next push too. Fix them before "
              "claiming the guards are green.")
        return 1
    if dep_problems:
        print()
        print("[fast-checks-local] FAIL: every guard passed here, but the "
              "job-dependency mismatch above means the next push is red "
              "anyway. That gap is GitHub #260 and is the whole reason this "
              "check exists.")
        return 1
    print("[fast-checks-local] OK: every locally-runnable fast-checks guard "
          "passes on this tree.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
