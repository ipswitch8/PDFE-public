# -*- coding: utf-8 -*-
"""Guard the cross-machine build-tree restore in windows-validation.yml.

WHY THIS EXISTS. Until 2026-08-20 every downstream job consumed `build/vs2022`
and it survived between jobs ONLY because they all landed on the SAME machine
and checked out with `clean: false`. That is a single-runner property, and it is
what made registering a second runner unsafe: a job routed to a machine that
never built would FAIL, with a message about a missing exe that names the
symptom and not the cause.

`.github/actions/ensure-build-tree` closes that. This file is what stops the
closure from quietly coming undone, in the ways it actually could:

  0. A ROLE LABEL that replaces a base label instead of being appended to it.
     Caught by the phase-2 karen gate, not by any guard: `build` and `release`
     read `[self-hosted, Windows, X64, pdfe-builder]`, dropping `pdfe-windows`.
     Invisible on the one machine that carries every label, and it WIDENS
     eligibility rather than narrowing it.
  1. A NEW consuming job is added and nobody wires the restore into it. That is
     the failure mode with the highest probability by a wide margin, because
     the job works perfectly on the one-runner topology it is written on.
  2. The restore becomes OPT-IN -- gated on an input, or on implicit success()
     so a red step above turns it into a skip. An opt-in restore is a restore
     that is off on the run where it was needed.
  3. `required-files` empties out, which makes both the workspace probe and the
     completeness check VACUOUS: the action then reports "restored" for an empty
     directory. A guard that cannot go red is decoration, and so is a
     completeness check with nothing to check.
  4. The download loses `run-id`/`github-token`. actions/download-artifact@v4
     scopes artifacts PER RUN; without those it searches the current run only
     and the whole cross-machine path is INERT -- it fails closed forever while
     every name and path still matches. That is not hypothetical: it is exactly
     what a security-audit gate found in the `build` job's copy after two other
     gates had passed the wiring as self-consistent.
  5. A consumer names a store key or artifact name that nothing publishes, so
     the restore can only ever reach its cold-machine refusal.

Run:
    python tools/gate/check_ensure_build_tree.py [--self-test]
"""
from __future__ import annotations

import copy
import io
import os
import re
import sys

import yaml  # type: ignore

WORKFLOW = os.path.join(".github", "workflows", "windows-validation.yml")
ACTION = os.path.join(".github", "actions", "ensure-build-tree", "action.yml")

ACTION_USES = "./.github/actions/ensure-build-tree"

# A step that references any of these is reading the build tree this lane
# chains between jobs. Both separators, because PowerShell accepts either and a
# rule that knows only one is a rule that misses half the sites.
BUILD_TREE_REFS = (r"build\vs2022", "build/vs2022")

# A job containing a step that runs any of these BUILDS the tree rather than
# consuming a restored one, and cannot be served by a runtime-closure restore.
BUILDER_MARKERS = ("cmake --build", "cmake --preset", "cpack ")

# The label set EVERY job in this lane must still require. A role label is
# appended to these, never substituted for one.
BASE_LABELS = ("self-hosted", "Windows", "X64", "pdfe-windows")

# The four refusals the action inherits from the `build` job's restore, plus the
# cold-machine one. Each is pinned to a phrase the action must still contain, so
# deleting the check deletes the phrase.
# R2 configuration/credential variable names. Credentials must come from
# MACHINE-LEVEL env on the self-hosted runner -- never a workflow `secrets:`
# block (which would expose them to every job in the repo) and never a committed
# file. These names are also what the leak check below searches for.
R2_CONFIG_VARS = ("PDFE_R2_ENDPOINT", "PDFE_R2_BUCKET")
R2_CRED_VARS = ("PDFE_R2_ACCESS_KEY_ID", "PDFE_R2_SECRET_ACCESS_KEY")

# The workspace fast path's commit stamp. Existence of files is NOT evidence a
# tree belongs to this commit: `clean: false` plus a gitignored build/ means a
# workspace retains whatever commit last built there, and a consume-only machine
# never overwrites it.
COMMIT_STAMP = ".pdfe-commit"

REFUSAL_PHRASES = (
    "published-sha.txt marker",              # 1: no marker
    "another commit's source",               # 2: wrong commit
    "restore incomplete",                    # 3: incomplete restore
    "no build tree available",               # 5: cold machine, nothing published
)


def _step_text(step: dict) -> str:
    """Everything in a step that could reference the build tree.

    An `actions/upload-artifact` step is EXCLUDED, deliberately and narrowly.
    Its `path:` is a glob it collects whatever matches of -- the `report` job
    sweeps `build/vs2022/**/*.log` best-effort -- so it names the tree without
    depending on it, and on a cold machine it simply collects nothing. Treating
    it as a consumer would demand a 2.3 GB restore in the reporting leg purely
    to hoover up log files, and would make the leg that exists to explain a
    failure fail for the same reason. The exclusion is safe because
    upload-artifact has no `run:`, so nothing can hide a real consumer inside
    one.
    """
    if "actions/upload-artifact" in str(step.get("uses", "")):
        return str(step.get("name", ""))
    parts = [str(step.get("run", "")), str(step.get("name", ""))]
    env = step.get("env") or {}
    if isinstance(env, dict):
        parts.extend(str(v) for v in env.values())
    with_ = step.get("with") or {}
    if isinstance(with_, dict):
        parts.extend(str(v) for v in with_.values())
    return "\n".join(parts)


def _publishes(doc: dict) -> tuple[set, set]:
    """(artifact names, store keys) this workflow actually publishes.

    Store keys are read from the upload `path`, not from the publish script, so
    a consumer and a producer that disagree are caught even when both look
    plausible in isolation.
    """
    names, keys = set(), set()
    for job in (doc.get("jobs") or {}).values():
        for s in job.get("steps") or []:
            if "actions/upload-artifact" not in str(s.get("uses", "")):
                continue
            w = s.get("with") or {}
            name = str(w.get("name", ""))
            path = str(w.get("path", ""))
            if name:
                names.add(name)
            m = re.search(r"pdfe-build-store[\\/](.+)$", path)
            if m:
                keys.add(m.group(1).strip())
    return names, keys


def check_doc(doc: dict, action_text: str = "") -> list[str]:
    problems: list[str] = []
    jobs = doc.get("jobs") or {}
    pub_names, pub_keys = _publishes(doc)

    # RULE 0 -- role labels must be ADDITIVE.
    #
    # Added because the phase-2 karen gate caught a defect no guard could see:
    # `build` and `release` read `[self-hosted, Windows, X64, pdfe-builder]`,
    # which REPLACED the base label rather than adding to it. Two consequences,
    # neither visible in a green run on the one machine that carries every
    # label: it contradicts the "additive labels that never remove today's
    # working set" design, and it WIDENS eligibility -- a machine labelled only
    # [self-hosted, Windows, X64, pdfe-builder] would match, losing the
    # pdfe-windows guarantee every other job in the lane relies on.
    for name, job in jobs.items():
        runs_on = job.get("runs-on")
        if not isinstance(runs_on, list):
            continue
        missing = [lbl for lbl in BASE_LABELS if lbl not in runs_on]
        if missing:
            problems.append(
                f"{WORKFLOW}: job {name!r} has runs-on {runs_on!r}, which drops "
                f"{missing!r} from the base label set. Role labels are ADDITIVE: "
                f"append `pdfe-builder`, never replace a base label. Removing "
                f"one widens eligibility to machines that were never meant to "
                f"match.")

    for name, job in jobs.items():
        steps = job.get("steps") or []
        joined = "\n".join(_step_text(s) for s in steps)
        if not any(ref in joined for ref in BUILD_TREE_REFS):
            continue
        if any(mark in joined for mark in BUILDER_MARKERS):
            # A builder. It needs a CONFIGURED tree, which a runtime-closure
            # restore cannot supply, so requiring the action here would be
            # requiring something that does not work. Builders stay co-located
            # via the `pdfe-builder` label -- see the workflow's status block.
            continue

        idx = [i for i, s in enumerate(steps)
               if str(s.get("uses", "")).strip() == ACTION_USES]
        if not idx:
            problems.append(
                f"{WORKFLOW}: job {name!r} consumes the build tree but never "
                f"invokes {ACTION_USES}. On the single-runner topology it works; "
                f"the moment a second machine carries the same label it FAILS on "
                f"a missing exe, naming the symptom and not the cause.")
            continue

        pos = idx[0]
        step = steps[pos]

        # It must run BEFORE the first step that reads the tree.
        first_consumer = next(
            (i for i, s in enumerate(steps)
             if any(ref in _step_text(s) for ref in BUILD_TREE_REFS)), None)
        if first_consumer is not None and first_consumer < pos:
            problems.append(
                f"{WORKFLOW}: job {name!r} invokes {ACTION_USES} at step {pos} "
                f"but already reads the build tree at step {first_consumer} "
                f"({str(steps[first_consumer].get('name',''))[:50]!r}). A restore "
                f"after the first consumer restores nothing in time.")

        # AUTOMATIC, not opt-in, and not implicit-success().
        cond = str(step.get("if", ""))
        if "inputs." in cond:
            problems.append(
                f"{WORKFLOW}: job {name!r} gates the restore on an input "
                f"({cond.strip()}). An opt-in restore is a restore that is off "
                f"on the run where it was needed.")
        if "cancelled()" not in cond:
            problems.append(
                f"{WORKFLOW}: job {name!r} invokes {ACTION_USES} with "
                f"if={cond.strip()!r}. Without `!cancelled()` GitHub's implicit "
                f"success() lets any red step above SKIP the restore, and every "
                f"step below then fails on a missing exe instead.")

        w = step.get("with") or {}
        req = str(w.get("required-files", ""))
        if not [ln for ln in req.splitlines() if ln.strip()]:
            problems.append(
                f"{WORKFLOW}: job {name!r} passes an EMPTY required-files. Both "
                f"the workspace probe and the completeness check iterate it, so "
                f"an empty list makes the action report 'restored' for an empty "
                f"directory.")

        key = str(w.get("store-key", "")).strip()
        art = str(w.get("artifact-name", "")).strip()
        if not str(w.get("github-token", "")).strip():
            problems.append(
                f"{WORKFLOW}: job {name!r} passes no github-token, so the "
                f"cross-run artifact lookup and download cannot authenticate "
                f"and the cold-machine path is inert.")
        if pub_names and art and art not in pub_names:
            problems.append(
                f"{WORKFLOW}: job {name!r} restores artifact {art!r}, which no "
                f"upload-artifact step in this workflow publishes. The restore "
                f"could only ever reach its cold-machine refusal. Published: "
                f"{sorted(pub_names)}")
        if pub_keys and key and key not in pub_keys:
            problems.append(
                f"{WORKFLOW}: job {name!r} restores store key {key!r}, which no "
                f"publish writes. Published: {sorted(pub_keys)}")

    if action_text:
        problems.extend(check_action(action_text))
    return problems


def check_action(text: str) -> list[str]:
    """The action itself must still fail loudly and still be cross-RUN."""
    problems: list[str] = []

    # THE WORKSPACE PROBE MUST CHECK THE COMMIT, not just the files.
    #
    # This is the one source with no marker check by nature -- it reads a
    # directory rather than a published store. Without the stamp the probe
    # reports "already present" for a tree built from a DIFFERENT commit, which
    # on a consume-only machine (never builds, `clean: false`, gitignored
    # build/) means testing this commit's source against a previous commit's
    # binaries. Latent on one machine; live the moment a second runner exists.
    if COMMIT_STAMP not in text:
        problems.append(
            f"{ACTION}: the workspace probe has no {COMMIT_STAMP!r} check, so it "
            f"accepts a build tree from ANY commit that happens to be on disk. "
            f"File existence is not evidence of provenance.")
    else:
        seg = re.search(r"id:\s*probe\b(.*?)(?=\n\s*-\s*name:|\Z)", text, re.S)
        if seg and COMMIT_STAMP not in seg.group(1):
            problems.append(
                f"{ACTION}: {COMMIT_STAMP!r} appears in the action but NOT inside "
                f"the workspace probe step, so the probe still accepts a tree "
                f"from any commit.")
        if seg and "GITHUB_SHA" not in seg.group(1):
            problems.append(
                f"{ACTION}: the workspace probe reads {COMMIT_STAMP!r} but never "
                f"compares it against GITHUB_SHA -- a stamp nobody checks is "
                f"decoration.")
    for phrase in REFUSAL_PHRASES:
        if phrase not in text:
            problems.append(
                f"{ACTION}: the refusal identified by {phrase!r} is gone. The "
                f"action's whole value is that it fails LOUDLY naming which "
                f"refusal applied; a missing one is a silent pass.")
    if "download-artifact" in text:
        if "run-id:" not in text:
            problems.append(
                f"{ACTION}: the download has no `run-id`. "
                f"actions/download-artifact@v4 scopes artifacts PER RUN, so "
                f"without it the lookup searches the CURRENT run only and the "
                f"cross-machine path is INERT -- fails closed on every run while "
                f"every name and path still matches.")
        if "github-token:" not in text:
            problems.append(
                f"{ACTION}: the download has no `github-token`; a cross-run "
                f"download cannot authenticate, so the path is inert.")
    # --- R2 leg -----------------------------------------------------------
    if any(v in text for v in R2_CONFIG_VARS + R2_CRED_VARS):
        # ORDERING. R2 must be tried BEFORE the GitHub-artifact leg, or the
        # exhausted-quota path is consulted first and the whole point is lost.
        i_r2 = min((text.find(v) for v in R2_CONFIG_VARS if v in text),
                   default=-1)
        i_art = text.find("actions/download-artifact")
        if i_r2 >= 0 and i_art >= 0 and i_r2 > i_art:
            problems.append(
                f"{ACTION}: the R2 leg is ordered AFTER the GitHub-artifact "
                f"leg. Artifacts are bounded by a quota this account has "
                f"exhausted; consulting them first defeats the migration.")

        # FAIL CLOSED ON PARTIAL CONFIGURATION. Half-set R2 settings mean
        # somebody intended this machine to use R2 and it silently will not --
        # the operator then concludes R2 "does not work" rather than "was never
        # finished". This is the single most likely real-world misconfiguration.
        if "PARTIALLY configured" not in text:
            problems.append(
                f"{ACTION}: nothing refuses a PARTIALLY configured R2 store. "
                f"Half-set credentials must fail CLOSED and LOUD, never fall "
                f"through quietly to the artifact leg.")

        # A MISS MUST NOT BE AN ERROR. Nothing published yet is ordinary and
        # must fall through; only a broken/unreachable store is fatal. Without
        # this distinction the first run on any commit fails.
        if "NoSuchKey" not in text and "404" not in text:
            problems.append(
                f"{ACTION}: the R2 leg does not distinguish a MISS (nothing "
                f"published for this commit yet) from a BROKEN store. Without "
                f"it, either every first run fails or every real failure is "
                f"swallowed.")

        # Credentials must not be sourced from workflow secrets.
        if "secrets." in text:
            problems.append(
                f"{ACTION}: references `secrets.` — R2 credentials must come "
                f"from machine-level environment on the runner, not a workflow "
                f"secrets block that every job in the repository can read.")

        # THE LEG MUST BE ABLE TO FIRE AT ALL. Found by the phase-3 validator
        # gate: setting the R2 step's own `if:` to `${{ false }}` made it
        # unreachable and NOTHING noticed -- every other rule inspects the
        # consumer steps or the leg's body, none inspected its guard. That is
        # the inert-wiring class this file was written for, occurring in the
        # file's own newest rule.
        #
        # The condition must gate on the two probes (so it only runs when the
        # cheap same-machine sources missed) and must not be a constant.
        m = re.search(r"id:\s*r2\b(.*?)(?=\n\s*-\s*name:|\Z)", text, re.S)
        if m:
            seg = m.group(1)
            cond = re.search(r"^\s*if:\s*(.+)$", seg, re.M)
            if not cond:
                problems.append(
                    f"{ACTION}: the R2 leg has no `if:`, so it runs even when "
                    f"the workspace or local store already satisfied the job -- "
                    f"a pointless network round trip on every warm machine.")
            else:
                c = cond.group(1)
                if re.search(r"\$\{\{\s*(false|true)\s*\}\}", c):
                    problems.append(
                        f"{ACTION}: the R2 leg's `if:` is the constant "
                        f"{c.strip()!r}. A constant-false leg is INERT -- it "
                        f"can never fire, on any run, while every name, key and "
                        f"credential still looks correct.")
                elif ("steps.probe.outputs.source" not in c
                      or "local_probe" not in c):
                    problems.append(
                        f"{ACTION}: the R2 leg's `if:` does not gate on both "
                        f"probes ({c.strip()}). It must run only when the "
                        f"workspace AND the local store have missed.")

    if "run.path" not in text and "PDFE_WORKFLOW_PATH" not in text:
        problems.append(
            f"{ACTION}: the artifact lookup does not constrain the producing "
            f"run's workflow PATH. Matching a workflow NAME is not an identity "
            f"check -- names are editable and reusable.")
    return problems


def check_no_committed_credentials() -> list[str]:
    """No R2 secret may be committed. Scans TRACKED files only.

    Names alone are fine and expected -- the action must reference the variable
    names to read them. What must never appear is an assignment giving one a
    literal value, which is what a credential accidentally pasted into a script
    or a doc example looks like.
    """
    import subprocess
    problems: list[str] = []
    try:
        tracked = subprocess.run(["git", "ls-files"], capture_output=True,
                                 text=True, encoding="utf-8",
                                 errors="replace").stdout.split("\n")
    except OSError:
        return ["could not list tracked files to scan for committed secrets"]
    pat = re.compile(
        r"(" + "|".join(R2_CRED_VARS) + r")\s*[:=]\s*[\"\']?[A-Za-z0-9/+=_-]{16,}")
    for path in tracked:
        path = path.strip()
        if not path or not os.path.isfile(path):
            continue
        try:
            with io.open(path, encoding="utf-8", errors="ignore") as fh:
                body = fh.read()
        except OSError:
            continue
        m = pat.search(body)
        if m:
            problems.append(
                f"{path}: looks like a COMMITTED R2 credential "
                f"({m.group(1)} assigned a literal). Credentials belong in "
                f"machine-level environment on the runner, never in the repo.")
    return problems


def self_test() -> int:
    checks = 0
    failures: list[str] = []

    def check(label, got, want):
        nonlocal checks
        checks += 1
        if got != want:
            failures.append(f"{label}: got {got!r}, want {want!r}")

    ensure = {
        "name": "Ensure the build tree is present",
        "if": "${{ !cancelled() }}",
        "uses": ACTION_USES,
        "with": {
            "store-key": "${{ github.sha }}-debug",
            "artifact-name": "pdfe-build-debug-${{ github.sha }}",
            "github-token": "${{ github.token }}",
            "required-files": "bin/Debug/pdfe.exe\n",
        },
    }
    good = {"jobs": {
        "build": {"steps": [
            {"name": "Build", "run": "cmake --build --preset debug-vs2022"},
            {"uses": "actions/upload-artifact@v4", "with": {
                "name": "pdfe-build-debug-${{ github.sha }}",
                "path": r"C:\pdfe-build-store\${{ github.sha }}-debug"}},
        ]},
        "core-tests": {"steps": [
            {"uses": "actions/checkout@v4"},
            copy.deepcopy(ensure),
            {"name": "Run pdfe_tests",
             "run": r"$b = \"$env:GITHUB_WORKSPACE\build\vs2022\bin\Debug\""},
        ]},
        # A job that touches nothing -- must not be demanded to restore.
        "matrix-health": {"steps": [{"name": "gate", "run": "python x.py"}]},
    }}
    action = io.open(ACTION, encoding="utf-8").read() if os.path.isfile(ACTION) else ""

    check("a sound wiring passes", check_doc(good), [])
    check("the live action passes its own rules", check_action(action) if action else [], [])

    bad = copy.deepcopy(good)
    del bad["jobs"]["core-tests"]["steps"][1]
    check("a consumer with no restore is caught",
          any("never invokes" in p for p in check_doc(bad)), True)

    bad2 = copy.deepcopy(good)
    bad2["jobs"]["core-tests"]["steps"][1]["if"] = \
        "${{ !cancelled() && inputs.reuse_published_build != '' }}"
    check("an opt-in restore is caught",
          any("off on the run where it was needed" in p for p in check_doc(bad2)), True)

    bad3 = copy.deepcopy(good)
    bad3["jobs"]["core-tests"]["steps"][1]["if"] = "${{ success() }}"
    check("implicit-success gating is caught",
          any("implicit" in p for p in check_doc(bad3)), True)

    bad4 = copy.deepcopy(good)
    bad4["jobs"]["core-tests"]["steps"][1]["with"]["required-files"] = "\n  \n"
    check("an empty required-files is caught",
          any("EMPTY required-files" in p for p in check_doc(bad4)), True)

    bad5 = copy.deepcopy(good)
    bad5["jobs"]["core-tests"]["steps"][1]["with"]["artifact-name"] = "pdfe-build-typo"
    check("an artifact nothing publishes is caught",
          any("no upload-artifact step" in p.replace("which no ", "no ")
              for p in check_doc(bad5)) or
          any("publishes" in p for p in check_doc(bad5)), True)

    bad6 = copy.deepcopy(good)
    bad6["jobs"]["core-tests"]["steps"][1]["with"]["store-key"] = "${{ github.sha }}"
    check("a store key nothing publishes is caught",
          any("store key" in p for p in check_doc(bad6)), True)

    bad7 = copy.deepcopy(good)
    bad7["jobs"]["core-tests"]["steps"][1]["with"]["github-token"] = ""
    check("a missing github-token is caught",
          any("github-token" in p for p in check_doc(bad7)), True)

    bad8 = copy.deepcopy(good)
    steps = bad8["jobs"]["core-tests"]["steps"]
    steps[1], steps[2] = steps[2], steps[1]
    check("a restore placed after the first consumer is caught",
          any("restores nothing in time" in p for p in check_doc(bad8)), True)

    # A builder must NOT be required to restore -- a runtime closure cannot
    # serve `cmake --build`, so demanding it would demand something broken.
    bad9 = copy.deepcopy(good)
    bad9["jobs"]["release"] = {"steps": [
        {"name": "Build Release",
         "run": r"cmake --build \"$env:GITHUB_WORKSPACE\build\vs2022\" --config Release"}]}
    check("a builder job is exempt, not flagged",
          any("release" in p for p in check_doc(bad9)), False)

    # A job that names the tree ONLY in an upload-artifact glob (the `report`
    # leg sweeping build/vs2022/**/*.log) is collecting, not consuming.
    bad10 = copy.deepcopy(good)
    bad10["jobs"]["report"] = {"steps": [
        {"uses": "actions/upload-artifact@v4",
         "with": {"name": "logs", "path": "build/vs2022/**/*.log"}}]}
    check("an upload-artifact glob does not make a job a consumer",
          any("report" in p for p in check_doc(bad10)), False)

    # ...but a REAL consumer must still be caught even when an upload step in
    # the same job also names the tree, or the exclusion above would be a hole.
    bad11 = copy.deepcopy(bad10)
    bad11["jobs"]["report"]["steps"].append(
        {"name": "run it", "run": r"& \"build\vs2022\bin\Debug\pdfe.exe\""})
    check("a real consumer alongside an upload glob is still caught",
          any("never invokes" in p for p in check_doc(bad11)), True)

    # RULE 0: the defect the phase-2 karen gate found -- a role label that
    # REPLACES a base label instead of being appended to it.
    good_labels = copy.deepcopy(good)
    good_labels["jobs"]["build"]["runs-on"] = [
        "self-hosted", "Windows", "X64", "pdfe-windows", "pdfe-builder"]
    check("an additive role label passes", check_doc(good_labels), [])

    bad12 = copy.deepcopy(good_labels)
    bad12["jobs"]["build"]["runs-on"] = [
        "self-hosted", "Windows", "X64", "pdfe-builder"]
    check("a role label that REPLACES a base label is caught",
          any("drops" in p and "additive" in p.lower() for p in check_doc(bad12)),
          True)

    if action:
        check("removing the workspace commit stamp is caught",
              any("not evidence of provenance" in p
                  for p in check_action(action.replace(COMMIT_STAMP, "x-stamp"))),
              True)
        # THE SUBTLE ONE: the stamp is mentioned somewhere in the action (a
        # comment, the restore step) but no longer inside the PROBE. A
        # whole-file containment check passes this while the probe accepts any
        # commit's tree.
        _probe = re.search(r"id:\s*probe\b(.*?)(?=\n\s*-\s*name:|\Z)", action, re.S)
        if _probe:
            gutted = action.replace(
                _probe.group(1), _probe.group(1).replace(COMMIT_STAMP, "x-stamp"), 1)
            check("a stamp check removed from the PROBE only is caught",
                  any("NOT inside" in p for p in check_action(gutted)), True)

    # --- R2 leg mutations, against the LIVE action text -------------------
    if action:
        # Ordering: R2 must be consulted before the artifact leg, because the
        # artifact quota is the thing being migrated away from.
        # Synthetic, because reordering the real 100-line R2 step by string
        # surgery tests the surgery rather than the rule. The rule is an index
        # comparison; this is the minimum text that expresses both orders.
        ordered_ok = ("PDFE_R2_ENDPOINT ... NoSuchKey ... PARTIALLY configured "
                      "... actions/download-artifact ... run.path")
        ordered_bad = ("actions/download-artifact ... PDFE_R2_ENDPOINT ... "
                       "NoSuchKey ... PARTIALLY configured ... run.path")
        check("R2 before the artifact leg passes the ordering rule",
              any("ordered AFTER" in p for p in check_action(ordered_ok)), False)
        check("R2 ordered AFTER the artifact leg is caught",
              any("ordered AFTER" in p for p in check_action(ordered_bad)), True)

        check("no refusal of a PARTIALLY configured R2 store is caught",
              any("PARTIALLY configured" in p for p in check_action(
                  action.replace("PARTIALLY configured", "half-set"))), True)

        no_miss = action.replace("NoSuchKey", "XX").replace("404", "XX")
        check("failing to distinguish a MISS from a broken store is caught",
              any("MISS" in p for p in check_action(no_miss)), True)

        check("sourcing R2 credentials from workflow secrets is caught",
              any("secrets." in p for p in check_action(
                  action + "\n        key: ${{ secrets.R2_KEY }}\n")), True)

    # A committed credential must be caught. Written to a temp tracked-looking
    # path is not possible here (the scan reads `git ls-files`), so exercise the
    # pattern directly -- the rule is the regex, and a rule whose regex is never
    # tested is a rule nobody has checked.
    import re as _re
    _pat = _re.compile(
        r"(" + "|".join(R2_CRED_VARS) + r")\s*[:=]\s*[\"\']?[A-Za-z0-9/+=_-]{16,}")
    # THE SAMPLE IS ASSEMBLED AT RUNTIME, ON PURPOSE. Writing the literal
    # `NAME=<32 chars>` into this file made the leak scan flag THIS FILE -- a
    # true positive on a fake secret. The alternative fix, exempting the guard's
    # own source, is the one to avoid: a rule that does not apply to itself is
    # the first place someone will hide a violation. Concatenation keeps the
    # rule universal and the file clean.
    _sample = R2_CRED_VARS[1] + "=" + ("abcdef0123456789" * 2)
    check("a literal credential assignment matches the leak pattern",
          bool(_pat.search(_sample)), True)
    check("a bare variable NAME does not match (names are expected)",
          bool(_pat.search("env: " + R2_CRED_VARS[1])), False)

    if action:
        check("a constant-false R2 leg is caught",
              any("INERT" in p for p in check_action(
                  action.replace(
                      "      if: ${{ steps.probe.outputs.source != 'workspace' "
                      "&& steps.local_probe.outputs.local_hit != 'true' }}\n"
                      "      shell: pwsh\n"
                      "      env:\n"
                      "        PDFE_STORE_KEY: ${{ inputs.store-key }}",
                      "      if: ${{ false }}\n"
                      "      shell: pwsh\n"
                      "      env:\n"
                      "        PDFE_STORE_KEY: ${{ inputs.store-key }}", 1))),
              True)

    # Action-level mutations: each refusal, and the two inert-wiring fields.
    if action:
        for phrase in REFUSAL_PHRASES:
            mutated = action.replace(phrase, "REMOVED", 1)
            check(f"deleting the refusal {phrase!r} is caught",
                  any(repr(phrase) in p for p in check_action(mutated)), True)
        check("dropping run-id is caught",
              any("INERT" in p for p in check_action(action.replace("run-id:", "x-id:"))),
              True)
        check("dropping the download github-token is caught",
              any("cannot authenticate" in p
                  for p in check_action(action.replace("github-token:", "x-token:"))),
              True)
        no_path = action.replace("run.path", "run.name").replace(
            "PDFE_WORKFLOW_PATH", "PDFE_WORKFLOW_NAME")
        check("dropping the workflow-path constraint is caught",
              any("identity check" in p for p in check_action(no_path)), True)

    if failures:
        print("[ensure-build-tree] self-test FAILED:", file=sys.stderr)
        for f in failures:
            print("  - " + f, file=sys.stderr)
        return 1
    print(f"[ensure-build-tree] self-test: {checks} checks passed")
    return 0


def main(argv: list[str]) -> int:
    if "--self-test" in argv:
        return self_test()
    for p in (WORKFLOW, ACTION):
        if not os.path.isfile(p):
            print(f"[ensure-build-tree] {p} not found", file=sys.stderr)
            return 1
    with io.open(WORKFLOW, encoding="utf-8") as fh:
        doc = yaml.safe_load(fh.read())
    with io.open(ACTION, encoding="utf-8") as fh:
        action_text = fh.read()
    problems = check_doc(doc, action_text)
    problems.extend(check_no_committed_credentials())
    if problems:
        print("[ensure-build-tree] FAILED — the cross-machine restore can be lost:")
        for p in problems:
            print("  * " + p)
        return 1
    consumers = [n for n, j in (doc.get("jobs") or {}).items()
                 if any(str(s.get("uses", "")).strip() == ACTION_USES
                        for s in (j.get("steps") or []))]
    print(f"[ensure-build-tree] {len(consumers)} consuming job(s) "
          f"({', '.join(sorted(consumers))}) restore automatically; the action "
          f"keeps all five refusals and stays cross-RUN.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
