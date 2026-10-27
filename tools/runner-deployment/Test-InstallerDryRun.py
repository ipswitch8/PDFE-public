# -*- coding: utf-8 -*-
"""Prove the installer's dry run actually exercises every requirement.

WHY THIS EXISTS. `Install-PdfeRunner.ps1 -WhatIf` used to check ShouldProcess
BEFORE the per-kind switch, so it skipped every branch: it proved the loop
iterated and nothing else. Only 1 of the 11 requirements was ever executed by
any test, and a requirement with a missing url, an unknown kind or a malformed
portable_fallback was indistinguishable from a correct one.

The check now runs each branch and gates only the effectful calls. That is a
claim, and a claim about a guard is worth nothing until a mutation reddens it.
So: run the control (must pass, and must resolve every requirement), then feed
the script deliberately broken requirement files and require each to FAIL.

The whole thing runs against a COPY in a temp dir -- the script reads its
requirements from $PSScriptRoot, so mutating in place would edit the shipped
file, and a crash mid-run would leave it corrupt.

A MUTATION MUST BE KILLED FOR THE RIGHT REASON. This harness was first wired
into the ubuntu-hosted fast-checks lane, where Install-PdfeRunner.ps1 cannot run
at all: WindowsIdentity::GetCurrent() throws "not supported on this platform"
before the requirement loop is ever reached. Every mutation duly exited
non-zero, and the harness reported 6/6 killed -- six fake kills, on a platform
where not one line of the code under test executed. Only the control case
noticed. So each mutation now also asserts the SPECIFIC message it should
provoke, and the harness refuses to run off-Windows rather than emit a
comfortable green.

Run (Windows only):
    python tools/runner-deployment/Test-InstallerDryRun.py
"""
from __future__ import annotations

import io
import os
import sys
import json
import copy
import shutil
import tempfile
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = "Install-PdfeRunner.ps1"
REQS = "runner-requirements.json"
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))


PROFILES = ("build", "consume")


def run_dry(workdir: str, profile: str = "build") -> tuple[int, str]:
    p = subprocess.run(
        ["pwsh", "-NoProfile", "-ExecutionPolicy", "Bypass",
         "-File", os.path.join(workdir, SCRIPT),
         "-RepoRoot", REPO_ROOT, "-RunnerProfile", profile, "-WhatIf"],
        capture_output=True, text=True, encoding="utf-8", errors="replace",
        cwd=workdir)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def in_profile(req: dict, profile: str) -> bool:
    return profile in (req.get("profiles") or [])


def stage(spec: dict) -> str:
    d = tempfile.mkdtemp(prefix="pdfe-installer-dryrun-")
    shutil.copy2(os.path.join(HERE, SCRIPT), os.path.join(d, SCRIPT))
    with io.open(os.path.join(d, REQS), "w", encoding="utf-8") as fh:
        json.dump(spec, fh, indent=2)
    return d


def first_of_kind(spec: dict, kind: str) -> dict:
    for r in spec["requirements"]:
        inst = r.get("install")
        if inst and inst.get("kind") == kind:
            return inst
    raise SystemExit(f"no requirement of kind {kind!r} to mutate")


def main() -> int:
    if os.name != "nt":
        # NOT a skip. The script under test provisions a WINDOWS runner and uses
        # WindowsIdentity / Get-Process SessionId; off-Windows it dies before the
        # requirement loop, which makes every mutation "pass" for the wrong
        # reason. Exiting 0 here would put this lane's green back on a check that
        # verified nothing -- the exact defect this file exists to prevent.
        print("[installer-dryrun] REFUSING to run on %s: Install-PdfeRunner.ps1 "
              "cannot execute off-Windows, so any result here is meaningless. "
              "Run this on the Windows lane." % os.name, file=sys.stderr)
        return 2

    with io.open(os.path.join(HERE, REQS), encoding="utf-8") as fh:
        base = json.load(fh)

    results: list[tuple[str, bool, str]] = []

    def record(label, ok, detail=""):
        results.append((label, ok, detail))

    # --- control, ONCE PER PROFILE ------------------------------------------
    # Both profiles, because a profile that selects the wrong set fails in a way
    # a single-profile run cannot see: running only 'build' would pass with the
    # filter broken open (every requirement selected), and running only
    # 'consume' would pass with it stuck shut. The pair pins both directions.
    reached_by_profile: dict[str, set] = {}
    for profile in PROFILES:
        d = stage(base)
        try:
            rc, out = run_dry(d, profile)
            record("control[%s]: dry run exits 0" % profile, rc == 0,
                   out.strip()[-400:])
            record("control[%s]: says DRY RUN, not 'no failures'" % profile,
                   "DRY RUN" in out and
                   "completed with no reported failures" not in out,
                   out.strip()[-200:])
            record("control[%s]: names the profile it selected" % profile,
                   ("profile: %s --" % profile) in out, out.strip()[-200:])
            # Every installable requirement IN THIS PROFILE must have been
            # REACHED, not skipped.
            installable = [r["id"] for r in base["requirements"]
                           if r.get("install") and in_profile(r, profile)]
            reached = {i for i in installable
                       if (": %s." % i) in out or ("==> %s " % i) in out}
            reached_by_profile[profile] = reached
            record("control[%s]: every installable requirement is reached (%d)"
                   % (profile, len(installable)),
                   len(reached) == len(installable),
                   "missed: %s" % sorted(set(installable) - reached))
            # ...and nothing OUTSIDE it. A profile that quietly installs the
            # build toolchain on a consume-only box is the whole failure this
            # parameter exists to prevent, and it would otherwise be invisible:
            # every check above still passes.
            outside = [r["id"] for r in base["requirements"]
                       if r.get("install") and not in_profile(r, profile)]
            leaked = [i for i in outside
                      if (": %s." % i) in out or ("==> %s " % i) in out]
            record("control[%s]: reaches NOTHING outside the profile" % profile,
                   not leaked, "leaked: %s" % sorted(leaked))
            record("control[%s]: installs nothing (no 'downloading')" % profile,
                   "  downloading " not in out, out.strip()[-200:])
        finally:
            shutil.rmtree(d, ignore_errors=True)

    # The two profiles must actually DIFFER. If they resolved to the same set,
    # every check above would still pass while the parameter did nothing at all.
    if len(reached_by_profile) == len(PROFILES):
        b, c = reached_by_profile["build"], reached_by_profile["consume"]
        record("the two profiles select DIFFERENT requirement sets",
               b != c and c < b,
               "build=%s consume=%s" % (sorted(b), sorted(c)))

    # --- mutations: each must be CAUGHT (non-zero exit) ---------------------
    mutations = []

    m = copy.deepcopy(base)
    first_of_kind(m, "download").pop("url", None)
    mutations.append(("download entry with no url", m, "with no url"))

    m = copy.deepcopy(base)
    first_of_kind(m, "download")["kind"] = "sideload-from-vibes"
    mutations.append(("unknown install kind", m, "unknown install kind"))

    m = copy.deepcopy(base)
    for r in m["requirements"]:
        inst = r.get("install")
        if inst and "portable_fallback" in inst:
            inst["portable_fallback"].pop("dest", None)
            break
    mutations.append(("portable_fallback with no dest", m,
                      "portable_fallback with no dest"))

    m = copy.deepcopy(base)
    first_of_kind(m, "registry-set").pop("name", None)
    mutations.append(("registry-set with no name", m,
                      "registry-set with no path/name"))

    m = copy.deepcopy(base)
    first_of_kind(m, "git-clone-bootstrap").pop("bootstrap", None)
    mutations.append(("git-clone-bootstrap with no bootstrap", m,
                      "git-clone-bootstrap with no repo/bootstrap"))

    m = copy.deepcopy(base)
    first_of_kind(m, "pip-requirements")["path"] = "tests/nope/not-here.txt"
    mutations.append(("pip-requirements pointing at a missing file", m,
                      "requirements file not found"))

    # --- profile-selection mutations ---------------------------------------
    # The three ways profile selection can fail OPEN. Each must refuse, and each
    # must refuse with its OWN message: a shared "something went wrong" would let
    # any one of the three stand in for the other two.
    m = copy.deepcopy(base)
    m.pop("profiles", None)
    mutations.append(("the profiles block deleted", m, "has no 'profiles' block"))

    m = copy.deepcopy(base)
    for r in m["requirements"]:
        r.pop("profiles", None)
    mutations.append(("every requirement stripped of its profiles tag", m,
                      "selected ZERO requirements"))

    for label, spec, expect in mutations:
        d = stage(spec)
        try:
            rc, out = run_dry(d)
            # Non-zero is NOT enough -- see the module docstring. The message the
            # mutation was supposed to provoke must actually appear, or the kill
            # is somebody else's crash wearing this mutation's name.
            record("mutation KILLED for the right reason: " + label,
                   rc != 0 and expect in out,
                   "exit %d, expected %r in output:\n%s"
                   % (rc, expect, out.strip()[-300:]))
        finally:
            shutil.rmtree(d, ignore_errors=True)

    # A profile name that is not a profile. ValidateSet rejects this at
    # parameter binding, BEFORE the script body -- which is the desired
    # behaviour and also the reason it cannot be a requirements-file mutation.
    d = stage(base)
    try:
        rc, out = run_dry(d, "definitely-not-a-profile")
        record("an unknown profile name is refused, not defaulted",
               rc != 0 and "RunnerProfile" in out,
               "exit %d, output:\n%s" % (rc, out.strip()[-300:]))
    finally:
        shutil.rmtree(d, ignore_errors=True)

    print("")
    failed = 0
    for label, ok, detail in results:
        print("  %-58s %s" % (label, "PASS" if ok else "FAIL"))
        if not ok:
            failed += 1
            print("      " + detail.replace("\n", "\n      "))
    print("")
    if failed:
        print("RESULT: %d of %d FAILED" % (failed, len(results)))
        return 1
    print("RESULT: all %d checks pass (%d mutations killed)"
          % (len(results), len(mutations)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
