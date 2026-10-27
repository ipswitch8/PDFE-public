# -*- coding: utf-8 -*-
"""The labels a deployment REGISTERS must cover the labels the workflows REQUIRE.

WHY THIS EXISTS, AND WHY `check_runner_label_coverage.py` DOES NOT COVER IT.
Those two checks sound alike and ask different questions:

  * `check_runner_label_coverage.py` asks **"can the fleet that exists right now
    serve this job?"** -- workflow `runs-on` vs the LIVE registered runners. It
    catches a fleet that has drifted, and it needs a runner inventory to say
    anything at all (it fails closed on an empty one).

  * THIS check asks **"would a machine provisioned by our own package be able to
    serve this job?"** -- workflow `runs-on` vs what the DEPLOYMENT PATH will
    register. It is a design-time check. It needs no inventory, so it is green or
    red on an empty fleet exactly as it is on a full one.

The second question went unasked, and the answer was no. Only two jobs in the
tree carry `pdfe-builder` -- `windows-validation.yml`'s `build` and `release` --
and the package registered runners without it. GitHub does not error on a
`runs-on` no runner satisfies: the job QUEUES and is then reported `cancelled`,
which alerts nobody. So a machine provisioned by the package looked healthy and
silently could not build, and the live-fleet check could not see it because there
was no fleet to check against.

Worse, the tree contradicted itself -- `docs/ci/repo-migration-2026-08-22.md` had
the right label list while the package had the wrong one. Two places that must
agree, disagreeing, with the wrong one in the automated path.

WHAT THIS CHECKS
  1. The three places that state the deployment label set AGREE with each other:
     `runner-requirements.json` (`target_labels` and `profiles.build.labels`),
     `Install-PdfeRunner.ps1`'s printed `--labels`, the package README's
     registration blocks, and `Register-PdfeRunner.ps1` -- which is the
     AUTOMATED path and therefore the one with the most teeth, since an operator
     running it never retypes the labels. Disagreement here IS the defect class.
  2. Every self-hosted `runs-on` in `.github/workflows/` is a SUBSET of that set,
     so a solo machine provisioned by the package can serve every job.

EXEMPTION, deliberately narrow: the ephemeral Hetzner linux set, matching the
allowlist in `check_runner_label_coverage.py`. It is a Linux box created per run
and always deleted afterwards; a Windows deployment package cannot and should not
cover it. NO other exemption exists, and a blanket one must never be added -- an
unsatisfiable `runs-on` whose symptom is silence is exactly what this prevents.

Run:
    python tools/gate/check_label_drift.py [--self-test]
Exit 0 clean, 1 on drift.
"""
from __future__ import annotations

import argparse
import io
import json
import os
import re
import sys

import yaml  # type: ignore

REQUIREMENTS = os.path.join("tools", "runner-deployment", "runner-requirements.json")
INSTALLER = os.path.join("tools", "runner-deployment", "Install-PdfeRunner.ps1")
PKG_README = os.path.join("tools", "runner-deployment", "README.md")
# Added 2026-09-02 with Register-PdfeRunner.ps1. It is now THE automated
# registration path, so its label set has more teeth than any of the printed
# ones -- an operator who runs it never retypes the labels at all. A source that
# nothing compares is exactly how the tree contradicted itself the first time.
REGISTER = os.path.join("tools", "runner-deployment", "Register-PdfeRunner.ps1")
WORKFLOW_DIR = os.path.join(".github", "workflows")

# Same set, and the same reasoning, as check_runner_label_coverage.py's
# EPHEMERAL_LABEL_SETS. Kept as its own literal rather than imported: these two
# checks are deliberately independent, and a shared constant would let one edit
# silently widen both.
EPHEMERAL_LABEL_SETS = {
    frozenset({"self-hosted", "linux", "hetzner", "sanitizer"}):
        "Hetzner sanitizer box: created per run, always `hcloud server delete`d "
        "afterwards, and Linux. A Windows deployment package neither can nor "
        "should provision it.",
}

_LABELS_RE = re.compile(r"--labels\s+([A-Za-z0-9,\-_]+)")

# A POSITIVE marker naming the canonical default-registration command.
#
# WHY NOT "take the widest `--labels` string in the file". That was this guard's
# first design and the `security-audit` gate FAILed phase-2 for it, correctly.
# `README.md` carries two `--labels` occurrences (the registration block and the
# Nth-runner walkthrough). Narrow ONE of them and `max(len)` still returns the
# other, so the guard stays green while an operator following the narrowed
# instruction registers a machine that silently cannot build -- the exact defect
# class this file exists to catch, reintroduced through the file's own
# redundancy. "Widest wins" also means any wider decoy string anywhere in the
# file -- a comment, a stale example -- re-inflates the declared set.
#
# Consistency-checking alone ("every occurrence must agree") is NOT sufficient
# either: a narrower `consume`-role example is legitimate documentation, and a
# same-set-everywhere rule would either false-positive on it or be scoped so
# loosely it reopens the hole. So the canonical command SAYS SO, and everything
# unmarked is ignored.
DEFAULT_MARKER = "PDFE-DEFAULT-LABELS"

# How far after the marker the command may sit. Generous enough for a fenced
# code block plus a couple of continuation lines, tight enough that the marker
# cannot accidentally claim an unrelated command further down the file.
_MARKER_WINDOW_LINES = 12


def _marked_label_set(text: str, where: str, problems: list):
    """The label set of the ONE command marked canonical in `text`.

    Returns None and appends a problem when the file does not name exactly one.
    Ambiguity is a failure, not something to resolve by picking.
    """
    lines = text.splitlines()
    marks = [i for i, ln in enumerate(lines) if DEFAULT_MARKER in ln]
    if not marks:
        problems.append(
            "%s carries no %s marker, so which of its `--labels` commands is the "
            "canonical default cannot be determined. Mark exactly one."
            % (where, DEFAULT_MARKER))
        return None
    if len(marks) > 1:
        problems.append(
            "%s carries %d %s markers (lines %s). Exactly one command may be the "
            "default; more than one is ambiguous and would let a narrowed "
            "default hide behind a wider sibling."
            % (where, len(marks), DEFAULT_MARKER,
               ", ".join(str(m + 1) for m in marks)))
        return None

    start = marks[0]
    window = "\n".join(lines[start:start + _MARKER_WINDOW_LINES])
    found = _LABELS_RE.findall(window)
    if not found:
        problems.append(
            "%s has a %s marker at line %d but no `--labels` command within the "
            "following %d lines. The marker must sit immediately above the "
            "command it names." % (where, DEFAULT_MARKER, start + 1,
                                   _MARKER_WINDOW_LINES))
        return None
    return set(found[0].split(","))


def declared_label_sets(root: str = ".", problems: list = None) -> dict:
    """Every place that states what a default deployment registers."""
    out: dict = {}
    if problems is None:
        problems = []

    req_path = os.path.join(root, REQUIREMENTS)
    with io.open(req_path, encoding="utf-8") as fh:
        req = json.load(fh)
    out["runner-requirements.json:target_labels"] = set(req.get("target_labels", []))
    profiles = req.get("profiles", {})
    for name, prof in profiles.items():
        if prof.get("is_default"):
            out["runner-requirements.json:profiles.%s(default)" % name] = set(
                prof.get("labels", []))

    # The PRINTED commands are what an operator actually runs, so they are the
    # ones with teeth -- but only the one that declares itself canonical.
    for key, path in (("Install-PdfeRunner.ps1", INSTALLER),
                      ("runner-deployment/README.md", PKG_README),
                      ("Register-PdfeRunner.ps1", REGISTER)):
        full = os.path.join(root, path)
        if not os.path.exists(full):
            continue
        with io.open(full, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        labels = _marked_label_set(text, key, problems)
        if labels is not None:
            out[key] = labels
    return out


def self_hosted_jobs(root: str = ".") -> list:
    """[(workflow, job, set(labels))] for every self-hosted `runs-on` list."""
    jobs = []
    wf_dir = os.path.join(root, WORKFLOW_DIR)
    for name in sorted(os.listdir(wf_dir)):
        if not name.endswith((".yml", ".yaml")):
            continue
        with io.open(os.path.join(wf_dir, name), encoding="utf-8",
                     errors="replace") as fh:
            try:
                doc = yaml.safe_load(fh)
            except yaml.YAMLError as exc:
                jobs.append((name, "<unparseable>", set()))
                continue
        if not isinstance(doc, dict):
            continue
        for job_name, job in (doc.get("jobs") or {}).items():
            if not isinstance(job, dict):
                continue
            runs_on = job.get("runs-on")
            if isinstance(runs_on, list) and "self-hosted" in runs_on:
                jobs.append((name, job_name, set(runs_on)))
    return jobs


def check(declared: dict, jobs: list) -> list:
    problems = []

    if not declared:
        return ["no declared deployment label set could be read at all -- "
                "refusing to report coverage against nothing."]

    # (1) The declarations must agree with each other.
    sets = {k: v for k, v in declared.items() if v}
    widest = max(sets.values(), key=len)
    for where, labels in sorted(sets.items()):
        missing = widest - labels
        if missing:
            problems.append(
                "%s declares %s, which is MISSING %s that another declaration "
                "carries. Two places that must agree are disagreeing, and the "
                "narrower one will provision a machine that cannot serve every "
                "job." % (where, sorted(labels), sorted(missing)))

    if not jobs:
        return problems + ["no self-hosted job found in %s at all -- an empty "
                           "model is not a pass." % WORKFLOW_DIR]

    # (2) Every self-hosted runs-on must be servable by that set.
    exempt = 0
    for wf, job, want in jobs:
        if frozenset(want) in EPHEMERAL_LABEL_SETS:
            exempt += 1
            continue
        missing = want - widest
        if missing:
            problems.append(
                "%s job '%s' needs %s, which a default deployment does NOT "
                "register. GitHub will not error: the job queues, then reports "
                "`cancelled`, which alerts nobody. Add the label to the "
                "deployment path (all of %s), or justify it here."
                % (wf, job, sorted(missing),
                   "runner-requirements.json + Install-PdfeRunner.ps1 + README"))
    if exempt:
        print("[label-drift] %d job(s) exempt as ephemeral fleets; a Windows "
              "deployment package cannot provision them." % exempt)
    return problems


# ---------------------------------------------------------------------------
# Self-test. A guard no mutation can redden is decoration.
# ---------------------------------------------------------------------------
def self_test() -> int:
    checks = 0
    failures = []

    def ck(label, got, want):
        nonlocal checks
        checks += 1
        if got != want:
            failures.append("%s: got %r want %r" % (label, got, want))

    FULL_LABELS = {"self-hosted", "Windows", "X64", "pdfe-windows",
                   "pdfe-builder"}
    full = set(FULL_LABELS)
    agree = {"runner-requirements.json:target_labels": set(full),
             "Install-PdfeRunner.ps1": set(full),
             "runner-deployment/README.md": set(full)}
    jobs_ok = [("windows-validation.yml", "build", set(full)),
               ("governance.yml", "governance",
                {"self-hosted", "Windows", "X64", "pdfe-windows"})]

    ck("agreeing declarations + servable jobs pass", check(agree, jobs_ok), [])

    # THE REAL DEFECT: the installer drops pdfe-builder while the json keeps it.
    drifted = dict(agree)
    drifted["Install-PdfeRunner.ps1"] = full - {"pdfe-builder"}
    probs = check(drifted, jobs_ok)
    ck("a declaration missing a label is caught",
       any("Install-PdfeRunner.ps1" in p and "pdfe-builder" in p for p in probs),
       True)

    # A job needing a label NO declaration carries.
    narrow = {k: (full - {"pdfe-builder"}) for k in agree}
    probs = check(narrow, jobs_ok)
    ck("a job needing an unregistered label is caught",
       any("build" in p and "pdfe-builder" in p for p in probs), True)

    # The ephemeral Linux set must be exempt, not a failure.
    heth = [("hetzner-sanitizer.yml", "sanitize",
             {"self-hosted", "linux", "hetzner", "sanitizer"})]
    ck("the ephemeral Hetzner set is exempt", check(agree, heth), [])

    # ...but a TYPO'd ephemeral set must NOT be silently exempt.
    heth_typo = [("hetzner-sanitizer.yml", "sanitize",
                  {"self-hosted", "linux", "hetzner", "sanitiser"})]
    ck("a typo'd ephemeral set is NOT exempt",
       any("sanitiser" in p for p in check(agree, heth_typo)), True)

    # An empty model must fail rather than pass vacuously.
    ck("no declarations at all fails", bool(check({}, jobs_ok)), True)
    ck("no self-hosted jobs at all fails", bool(check(agree, [])), True)

    # ---- marker anchoring: the hole security-audit FAILed phase-2 for --------
    marked = (
        "intro line\n"
        "# " + DEFAULT_MARKER + " canonical\n"
        "  config.cmd --labels self-hosted,Windows,X64,pdfe-windows,pdfe-builder\n"
        "later prose\n"
        "  an older example: --labels self-hosted,Windows,X64,pdfe-windows\n")
    probs: list = []
    ck("the MARKED command is taken, not the widest",
       _marked_label_set(marked, "f", probs), set(FULL_LABELS))
    ck("...and that is clean", probs, [])

    # THE REGRESSION THE FAIL WAS ABOUT: a narrowed canonical with a WIDER
    # unmarked sibling further down must still be reported as narrow.
    decoy = (
        "# " + DEFAULT_MARKER + " canonical\n"
        "  config.cmd --labels self-hosted,Windows,X64,pdfe-windows\n"
        "elsewhere, a wider stale example:\n"
        "  --labels self-hosted,Windows,X64,pdfe-windows,pdfe-builder\n")
    probs = []
    ck("a WIDER unmarked sibling cannot mask a narrowed canonical",
       _marked_label_set(decoy, "f", probs), FULL_LABELS - {"pdfe-builder"})

    probs = []
    ck("no marker is a failure",
       (_marked_label_set("  --labels a,b\n", "f", probs), bool(probs)),
       (None, True))

    probs = []
    two = ("# " + DEFAULT_MARKER + "\n  --labels a,b\n"
           "# " + DEFAULT_MARKER + "\n  --labels a,b,c\n")
    ck("two markers are ambiguous and fail",
       (_marked_label_set(two, "f", probs), bool(probs)), (None, True))

    probs = []
    far = "# " + DEFAULT_MARKER + "\n" + ("filler\n" * 20) + "  --labels a,b\n"
    ck("a marker with no command beneath it fails",
       (_marked_label_set(far, "f", probs), bool(probs)), (None, True))

    if failures:
        print("[label-drift] self-test FAILED:", file=sys.stderr)
        for f in failures:
            print("   " + f, file=sys.stderr)
        return 1
    print("[label-drift] self-test: %d checks passed" % checks)
    return 0


def main(argv: list) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--repo-root", default=".")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    # Extraction problems (missing/ambiguous marker) are drift failures in their
    # own right, not merely a reason the comparison could not run -- a file that
    # cannot say which command is canonical is exactly how the widest-wins hole
    # opened. So they are collected and reported alongside the comparison.
    problems: list = []
    declared = declared_label_sets(args.repo_root, problems)
    jobs = self_hosted_jobs(args.repo_root)
    problems += check(declared, jobs)

    if problems:
        print("[label-drift] FAILED -- the deployment path and the workflows "
              "disagree about labels:", file=sys.stderr)
        for p in problems:
            print("  * " + p, file=sys.stderr)
        return 1

    print("[label-drift] OK -- every self-hosted job is servable by a machine "
          "this package provisions (%d job(s) checked against %d declaration(s))."
          % (len(jobs), len(declared)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
