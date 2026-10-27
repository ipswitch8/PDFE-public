# -*- coding: utf-8 -*-
"""Every self-hosted job must be satisfiable by a runner that actually exists.

WHY THIS EXISTS. GitHub routes work by label matching alone: a job runs on any
idle runner whose label set is a SUPERSET of its `runs-on` list. So a `runs-on`
naming a label no registered runner carries does not error, does not warn, and
does not fail. The job QUEUES — and after the timeout it is reported
`cancelled`, a status indistinguishable from a deliberate cancellation and one
that alerts nobody. This lane has already lost eight runs to statuses nobody
read (see docs/audit/audit-of-audits).

That makes an unsatisfiable `runs-on` uniquely nasty: it is introduced by an
ordinary-looking edit, it is invisible in review, and its symptom is silence.
Adding role labels (`pdfe-builder`) multiplies the ways to introduce one.

WHAT THIS CHECKS. For every job in every workflow whose `runs-on` is a list
containing `self-hosted`, the list must be a subset of some registered runner's
labels. Windows and Linux fleets are checked against their own runners, so a
Windows job is never judged satisfiable by a Linux runner that happens to share
`self-hosted`.

EVIDENCE, NOT ASSUMPTION. The runner inventory comes from a committed capture
(`docs/audit/runner-labels.json`, produced by `gh api
repos/<owner>/<repo>/actions/runners`) so this runs offline and in CI, and the
capture is refreshable with `--refresh`. `--live` queries the API directly. A
capture is a point-in-time fact and is dated in the file; when it disagrees with
reality the answer is to refresh it, never to relax the check.

Run:
    python tools/gate/check_runner_label_coverage.py [--self-test|--live|--refresh]
"""
from __future__ import annotations

import copy
import io
import json
import os
import subprocess
import sys

import yaml  # type: ignore

WORKFLOW_DIR = os.path.join(".github", "workflows")
EVIDENCE = os.path.join("docs", "audit", "runner-labels.json")
REPO = "ipswitch8/PDFE"

# `gh` is not on PATH by default on this host -- verified: a workflow step once
# failed with "term 'gh' not recognized". Try PATH first, then the known
# location, rather than assuming either.
def _gh_candidates() -> tuple:
    """Where to look for gh, most authoritative first.

    The previous version was a 3-tuple with two HARDCODED absolute paths under
    a `dev` user. On a machine whose account is not `dev` all three entries
    fail: bare "gh" cannot be exec'd by `subprocess` without a shell when what
    is on PATH is a .cmd SHIM (WinError 2, "The system cannot find the file
    specified"), and the two literals name a profile that does not exist.

    Measured when `--refresh --live` was run during the October 2026 runner
    move: it died with exactly that WinError 2, after the registration had
    already succeeded, so the capture was left stale at the one moment it most
    needed refreshing.

    The fix is to stop guessing a profile. PDFE_GH lets a caller that has
    ALREADY resolved gh hand the path straight over -- which is what
    tools/ops/Move-PdfeRunnerRegistration.ps1 now does -- and the remaining
    candidates are built from environment variables, so they work on any
    account without naming one. The two historical literals are kept last so
    nothing that used to resolve stops resolving.
    """
    out = []
    override = os.environ.get("PDFE_GH")
    if override:
        out.append(override)
    out.append("gh")
    local_app = os.environ.get("LOCALAPPDATA")
    if local_app:
        out.append(os.path.join(local_app, "Programs", "gh", "bin", "gh.exe"))
    profile = os.environ.get("USERPROFILE")
    if profile:
        out.append(os.path.join(profile, "bin", "gh.cmd"))
        out.append(os.path.join(profile, "bin", "gh.exe"))
    program_files = os.environ.get("ProgramFiles")
    if program_files:
        out.append(os.path.join(program_files, "GitHub CLI", "gh.exe"))
    out.append(r"C:\Users\dev\bin\gh.exe")
    out.append("/c/Users/dev/bin/gh")
    return tuple(out)


GH_CANDIDATES = _gh_candidates()


def _redact_runner_names(doc: dict) -> dict:
    """Replace each runner's NAME with its index before the capture is written.

    A self-hosted runner's name is routinely derived from the machine name --
    GitHub's own `config.cmd` defaults it to exactly that -- so a faithful
    capture of the runners API drops the HOST NAME into a tracked file.

    Measured during the October 2026 runner move: the registration was
    recreated, `config.cmd` defaulted the name to the machine name, and the
    very next `--refresh` made `check_tracked_path_hygiene.py` fail with

        docs/audit/runner-labels.json -- 1 occurrence(s), baseline allows 0

    The previous capture was clean only by accident: it was two months stale
    and still held a generic name from an earlier registration.

    That guard's instruction is explicit -- "if a GENERATED artifact grew it,
    fix the producer; baselining a fresh addition makes the ratchet a rubber
    stamp" -- and the precedent it cites is
    tests/scripts/sanitize_gtest_xml.py stripping the JUnit `hostname`
    attribute. This is the same fix in the same spirit.

    Nothing is lost: every check in this file reasons about LABELS and
    `status`. The name is diagnostic only, and `runner[0]`/`runner[1]` is as
    diagnostic as a hostname for a single-runner fleet.
    """
    out = copy.deepcopy(doc)
    for i, runner in enumerate(out.get("runners") or []):
        if isinstance(runner, dict) and "name" in runner:
            runner["name"] = "runner[%d]" % i
    out["_names_redacted"] = (
        "Runner names are replaced with runner[N] by the producer: a runner "
        "name is usually the machine name, and check_tracked_path_hygiene.py "
        "ratchets the host name out of tracked files. Labels and status are "
        "verbatim."
    )
    return out


def fetch_live() -> dict:
    last = None
    for gh in GH_CANDIDATES:
        try:
            out = subprocess.run(
                [gh, "api", "repos/%s/actions/runners" % REPO],
                capture_output=True, text=True, encoding="utf-8", errors="replace")
        except (OSError, ValueError) as exc:
            last = exc
            continue
        if out.returncode == 0:
            return json.loads(out.stdout)
        last = out.stderr.strip()
    raise SystemExit("could not query the runners API (%s). Use the committed "
                     "capture, or refresh it from a shell where `gh` is "
                     "authenticated." % last)


def runners_from(doc: dict) -> list[dict]:
    """[{name, status, labels:set}] from a runners-API payload."""
    out = []
    for r in doc.get("runners") or []:
        out.append({
            "name": r.get("name", "?"),
            "status": r.get("status", "?"),
            "labels": {l["name"] for l in (r.get("labels") or [])},
        })
    return out


def self_hosted_jobs(workflow_dir: str = WORKFLOW_DIR) -> list[tuple]:
    """[(workflow, job, [labels])] for every self-hosted job."""
    jobs = []
    for fn in sorted(os.listdir(workflow_dir)):
        if not fn.endswith((".yml", ".yaml")):
            continue
        path = os.path.join(workflow_dir, fn)
        with io.open(path, encoding="utf-8") as fh:
            try:
                doc = yaml.safe_load(fh.read())
            except yaml.YAMLError as exc:
                raise SystemExit("%s does not parse: %s" % (path, exc))
        if not isinstance(doc, dict):
            continue
        for name, job in (doc.get("jobs") or {}).items():
            if not isinstance(job, dict):
                continue
            runs_on = job.get("runs-on")
            if isinstance(runs_on, list) and "self-hosted" in runs_on:
                jobs.append((fn, name, list(runs_on)))
    return jobs


# EPHEMERAL FLEETS. A label set served by a runner that is PROVISIONED ON DEMAND
# and deleted afterwards is legitimately absent from a resting inventory, and
# flagging it would train the reader to ignore this check -- which is the only
# way a real unsatisfiable `runs-on` gets through.
#
# This is an allowlist of label SETS with a stated reason, not a switch that
# turns the rule off. A set is exempt only if it is exactly one of these; a job
# that adds one label to an exempt set is NOT exempt, because that added label
# is precisely the kind of typo this file exists to catch.
EPHEMERAL_LABEL_SETS = {
    frozenset({"self-hosted", "linux", "hetzner", "sanitizer"}):
        "Hetzner sanitizer box. Created per run and ALWAYS deleted afterwards "
        "(create -> test -> `hcloud server delete`), so it is correctly absent "
        "from a resting inventory. Its absence here says nothing about whether "
        "the lane works; see the linux/macOS lanes' own status.",
}


def check(jobs: list[tuple], runners: list[dict]) -> list[str]:
    problems: list[str] = []
    if not runners:
        # Fail CLOSED. An empty inventory would make every subset test vacuously
        # false-negative-free by having nothing to compare against, and a check
        # that passes because it had no data is the worst kind.
        return ["no runners in the inventory at all -- refusing to report "
                "coverage against an empty fleet. Refresh %s." % EVIDENCE]
    exempt = 0
    for wf, job, labels in jobs:
        want = set(labels)
        if frozenset(want) in EPHEMERAL_LABEL_SETS:
            exempt += 1
            continue
        matches = [r for r in runners if want <= r["labels"]]
        if not matches:
            # Report the CLOSEST runner and exactly what it lacks. "No runner
            # matches" sends someone reading five label lists by eye; naming the
            # missing label is the difference between a two-minute fix and an
            # afternoon.
            best, missing = None, None
            for r in runners:
                lack = want - r["labels"]
                if missing is None or len(lack) < len(missing):
                    best, missing = r, lack
            problems.append(
                "%s: job %r requires %s, which NO registered runner satisfies. "
                "Closest is %r (%s), missing %s. GitHub does not error on this "
                "-- the job QUEUES and is later reported `cancelled`, which "
                "alerts nobody."
                % (os.path.join(WORKFLOW_DIR, wf), job, sorted(want),
                   best["name"], best["status"], sorted(missing)))
        elif not any(r["status"] == "online" for r in matches):
            problems.append(
                "%s: job %r is satisfiable only by OFFLINE runner(s) %s. It "
                "would queue indefinitely."
                % (os.path.join(WORKFLOW_DIR, wf), job,
                   sorted(r["name"] for r in matches)))
    if exempt:
        # NEVER silent. A check that quietly skips work reads as a check that
        # covered it -- the failure shape this repo has hit repeatedly.
        print("[runner-labels] %d job(s) exempt as EPHEMERAL fleets (see "
              "EPHEMERAL_LABEL_SETS): they are provisioned per run and deleted "
              "afterwards, so a resting inventory correctly does not list them."
              % exempt)
    return problems


def self_test() -> int:
    checks = 0
    failures: list[str] = []

    def ck(label, got, want):
        nonlocal checks
        checks += 1
        if got != want:
            failures.append("%s: got %r, want %r" % (label, got, want))

    live = [{"name": "pdfe-runner-01", "status": "online",
             "labels": {"self-hosted", "Windows", "X64", "pdfe-windows",
                        "pdfe-builder"}}]
    jobs = [("windows-validation.yml", "build",
             ["self-hosted", "Windows", "X64", "pdfe-windows", "pdfe-builder"]),
            ("windows-validation.yml", "core-tests",
             ["self-hosted", "Windows", "X64", "pdfe-windows"])]
    ck("the live fleet satisfies the live jobs", check(jobs, live), [])

    # THE defect: a runs-on naming a label nothing carries.
    bad = jobs + [("windows-validation.yml", "typo",
                   ["self-hosted", "Windows", "X64", "pdfe-windwos"])]
    probs = check(bad, live)
    ck("an unsatisfiable runs-on is caught",
       any("NO registered runner" in p for p in probs), True)
    ck("...and it names the missing label",
       any("pdfe-windwos" in p for p in probs), True)

    # A role label nothing carries yet -- the shape a second machine introduces.
    bare = [{"name": "bare", "status": "online",
             "labels": {"self-hosted", "Windows", "X64", "pdfe-windows"}}]
    probs = check(jobs, bare)
    ck("a fleet with no builder cannot satisfy the build job",
       any("'build'" in p and "pdfe-builder" in p for p in probs), True)
    ck("...while the test job is still satisfiable",
       all("'core-tests'" not in p for p in probs), True)

    offline = copy.deepcopy(live)
    offline[0]["status"] = "offline"
    ck("an offline-only match is caught",
       any("OFFLINE" in p for p in check(jobs, offline)), True)

    ck("an empty fleet fails CLOSED", bool(check(jobs, [])), True)

    # A Windows job must not be satisfied by a Linux runner sharing self-hosted.
    linux = [{"name": "hetzner", "status": "online",
              "labels": {"self-hosted", "linux", "hetzner", "sanitizer"}}]
    ck("a Linux runner does not satisfy a Windows job",
       any("NO registered runner" in p for p in check(jobs, linux)), True)

    heth = [("linux-validation.yml", "build-and-test",
             ["self-hosted", "linux", "hetzner", "sanitizer"])]
    ck("an ephemeral fleet's exact label set is exempt", check(heth, live), [])

    # ...but ONE label more is NOT exempt. That extra label is exactly the typo
    # class this file exists for, and an exemption that swallowed it would make
    # the whole rule unreachable for the linux lane.
    heth_typo = [("linux-validation.yml", "build-and-test",
                  ["self-hosted", "linux", "hetzner", "sanitizer", "gpu"])]
    ck("a label ADDED to an exempt set is still caught",
       any("NO registered runner" in p for p in check(heth_typo, live)), True)

    if failures:
        print("[runner-labels] self-test FAILED:", file=sys.stderr)
        for f in failures:
            print("  - " + f, file=sys.stderr)
        return 1
    print("[runner-labels] self-test: %d checks passed" % checks)
    return 0


def main(argv: list[str]) -> int:
    if "--self-test" in argv:
        return self_test()

    if "--refresh" in argv:
        doc = fetch_live()
        # Stamp the capture. The runners API payload carries no timestamp, and
        # an undated point-in-time fact is one nobody can tell is stale -- the
        # docstring claimed the file was dated, so make that true rather than
        # softening the claim. Underscore-prefixed so it is visibly an addition
        # to the raw payload and not something GitHub returned.
        import datetime
        doc["_captured_utc"] = datetime.datetime.now(
            datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        doc["_captured_by"] = "tools/gate/check_runner_label_coverage.py --refresh"
        doc = _redact_runner_names(doc)
        os.makedirs(os.path.dirname(EVIDENCE), exist_ok=True)
        with io.open(EVIDENCE, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(doc, fh, indent=2, ensure_ascii=False)
            fh.write("\n")
        print("[runner-labels] refreshed %s" % EVIDENCE)

    if "--live" in argv:
        doc = fetch_live()
        source = "the live runners API"
    else:
        if not os.path.isfile(EVIDENCE):
            print("[runner-labels] no capture at %s; run with --refresh from a "
                  "shell where `gh` is authenticated." % EVIDENCE,
                  file=sys.stderr)
            return 1
        with io.open(EVIDENCE, encoding="utf-8") as fh:
            doc = json.load(fh)
        source = EVIDENCE

    runners = runners_from(doc)
    jobs = self_hosted_jobs()
    problems = check(jobs, runners)
    if problems:
        print("[runner-labels] FAILED — a self-hosted job cannot be scheduled:")
        for p in problems:
            print("  * " + p)
        return 1
    print("[runner-labels] %d self-hosted job(s) across %d workflow(s) are all "
          "satisfiable by the %d registered runner(s) in %s: %s"
          % (len(jobs), len({j[0] for j in jobs}), len(runners), source,
             ", ".join("%s%s [%s]" % (r["name"],
                                      "" if r["status"] == "online" else
                                      " (%s)" % r["status"],
                                      ",".join(sorted(r["labels"])))
                       for r in runners)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
