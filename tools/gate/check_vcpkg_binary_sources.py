#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Every workflow's vcpkg binary-cache STORE SET is the reviewed one (GitHub #177).

READ THIS FIRST -- WHAT THIS DOES AND DOES NOT ASSERT
-----------------------------------------------------
GitHub #177 says "`VCPKG_BINARY_SOURCES` integrity is unasserted". It is easy to
write a check here that sounds like it fixes that and does not. So, plainly:

  **ASSERTED:** the *configuration* of the binary cache. The set of stores each
  workflow reads from and writes to is exactly the set that has been reviewed
  and recorded in `APPROVED_STORES` below. A commit cannot silently add a
  third-party or network-reachable store (`nuget`, `http`, `azblob`, `s3`,
  `gcs`, ...), point the local store at a different directory, or drop the
  leading `clear` that excludes vcpkg's implicit user-wide default cache.

  **NOT ASSERTED:** that the *contents* of those stores are authentic. Nothing
  here hashes a cached package, compares it against a from-source build, or
  verifies a signature. A poisoned entry inside an approved store passes this
  check. That remains an accepted risk, and the reasoning for accepting it is
  in `docs/security/supply-chain-scope-decision.md` -- this checker does not
  supersede that record, it narrows it.

Why the narrow property is still worth enforcing: the two cheapest ways to
subvert a binary cache do not require touching a store at all. They are
(a) adding an attacker-controlled store to the chain, and (b) removing `clear`
so an unreviewed machine-local default cache silently rejoins it. Both are
one-line edits to a workflow, both are invisible in review unless someone is
diffing an env string, and both are exactly what this rejects -- on the commit
that makes the change, not in the audit that finds it later.

What it buys is therefore *blast-radius bounding*, not integrity. Said once, in
the code, so the distinction cannot be lost on the way to a summary.

THE `clear` RULE IS NOT COSMETIC
---------------------------------
`VCPKG_BINARY_SOURCES` is an ordered chain, and vcpkg starts it with an implicit
`default` store (the user-wide archive cache under `%LOCALAPPDATA%\\vcpkg\\archives`
or `$XDG_CACHE_HOME/vcpkg/archives`) unless the chain begins with `clear`. On a
self-hosted runner that directory persists across jobs, across branches, and
across repositories, and nothing in this repo reviews it. A chain missing its
leading `clear` is therefore a *wider* trust boundary than the one the decision
record analysed. Dropping it is a one-word edit; this is the thing that notices.

FAIL CLOSED
-----------
Unreadable YAML, a workflow directory with no workflows, zero
`VCPKG_BINARY_SOURCES` settings found anywhere, or a disagreement between the
two independent ways this file locates those settings -- all non-zero. "I could
not check" is never exit 0. That failure mode (a scanner exiting 0 having
scanned nothing) is the one p85 phase 1 was filed over; it is not repeated here.

USAGE
    python tools/gate/check_vcpkg_binary_sources.py
    python tools/gate/check_vcpkg_binary_sources.py --self-test

Exit 0 = every setting matches the approved shape. Non-zero = a finding, or the
check could not run.
"""
import argparse
import glob
import io
import os
import re
import sys

try:
    import yaml
except ImportError:  # pragma: no cover
    sys.stderr.write("check_vcpkg_binary_sources: PyYAML is required "
                     "(pip install pyyaml)\n")
    sys.exit(2)

KEY = "VCPKG_BINARY_SOURCES"

# The ONLY local `files,` store this project uses: a directory on the single
# self-hosted Windows runner. Recorded here so pointing the cache somewhere else
# is a deliberate, reviewable edit to this list rather than a silent edit to a
# workflow env string.
APPROVED_FILES_PATHS = {
    r"c:\vcpkg-bincache",
}

# Store *kinds* that may appear at all. Everything else is denied by default --
# including every network-backed provider vcpkg supports. Adding one here is the
# decision point; there is deliberately no "allow anything that looks fine".
APPROVED_KINDS = {
    "clear",   # resets the chain, dropping vcpkg's implicit user-wide default
    "files",   # a local directory, constrained to APPROVED_FILES_PATHS
    "x-gha",   # the GitHub Actions cache, scoped to this repository
}

# Access modes vcpkg accepts on a store.
VALID_MODES = {"read", "write", "readwrite"}


def find_key_in_yaml(node, found=None):
    """Every value of KEY anywhere in a parsed workflow, at any nesting depth.

    Walks the parsed document rather than assuming `env:` sits at workflow or
    job level -- it legitimately appears at both, and at step level too.

    Note `on:` parses as the BOOLEAN True under YAML 1.1, so dict keys here are
    not all strings. Compare defensively rather than calling .lower() on a key.
    """
    if found is None:
        found = []
    if isinstance(node, dict):
        for k, v in node.items():
            if isinstance(k, str) and k == KEY:
                found.append(v)
            else:
                find_key_in_yaml(v, found)
    elif isinstance(node, list):
        for item in node:
            find_key_in_yaml(item, found)
    return found


_RAW_RE = re.compile(r"^\s*" + KEY + r"\s*:\s*(?P<v>\S.*?)\s*$", re.MULTILINE)


def find_key_in_text(text):
    """Every value of KEY found by raw text scan -- the independent second opinion.

    Exists so that a YAML shape this file's walker cannot follow (an anchor, a
    merge key, a flow mapping) is detected as a DISAGREEMENT rather than being
    silently skipped by the walker and reported as "nothing to check".
    """
    return [m.group("v") for m in _RAW_RE.finditer(text)]


def unquote(v):
    v = v.strip()
    if len(v) >= 2 and v[0] == v[-1] and v[0] in ("'", '"'):
        v = v[1:-1]
    return v.strip()


def check_value(value):
    """Findings (list of strings) for one VCPKG_BINARY_SOURCES value. [] == OK."""
    findings = []
    raw = unquote(str(value))
    if not raw:
        return ["[empty] %s is set to an empty value" % KEY]

    segments = [s.strip() for s in raw.split(";") if s.strip()]
    if not segments:
        return ["[empty] %s has no store segments: %r" % (KEY, raw)]

    # RULE 1 -- the chain must start by clearing vcpkg's implicit default store.
    if segments[0].lower() != "clear":
        findings.append(
            "[missing-clear] the chain does not begin with 'clear', so vcpkg's "
            "implicit user-wide default archive cache stays in the chain and "
            "participates unreviewed: %r" % raw)

    for seg in segments:
        parts = [p.strip() for p in seg.split(",")]
        kind = parts[0].lower()

        # RULE 2 -- default-deny on store kind.
        if kind not in APPROVED_KINDS:
            findings.append(
                "[unapproved-store] store kind %r is not in APPROVED_KINDS %s. "
                "If this is intended, add it to the allowlist deliberately and "
                "say why -- a network-backed store widens the supply-chain "
                "trust boundary: %r" % (parts[0], sorted(APPROVED_KINDS), raw))
            continue

        if kind == "clear":
            continue

        # RULE 3 -- a local store must point at an approved directory.
        if kind == "files":
            if len(parts) < 2 or not parts[1]:
                findings.append(
                    "[files-no-path] a 'files' store has no path: %r" % seg)
            elif parts[1].lower().rstrip("\\/") not in APPROVED_FILES_PATHS:
                findings.append(
                    "[unapproved-path] local cache path %r is not in "
                    "APPROVED_FILES_PATHS %s: %r"
                    % (parts[1], sorted(APPROVED_FILES_PATHS), raw))
            mode_parts = parts[2:]
        else:  # x-gha
            mode_parts = parts[1:]

        # RULE 4 -- the access mode must be one vcpkg actually understands. A
        # typo'd mode is not a harmless string: vcpkg's handling of an
        # unrecognised token is not something this repo should be relying on.
        for m in mode_parts:
            if m and m.lower() not in VALID_MODES:
                findings.append(
                    "[bad-mode] %r is not a valid access mode %s: %r"
                    % (m, sorted(VALID_MODES), seg))

    return findings


def scan(workflow_dir):
    """(findings, settings_checked, workflows_scanned). Fails closed."""
    findings = []
    paths = sorted(glob.glob(os.path.join(workflow_dir, "*.yml"))
                   + glob.glob(os.path.join(workflow_dir, "*.yaml")))
    if not paths:
        return (["[no-workflows] %s contains no .yml/.yaml files -- this check "
                 "cannot run, and that is not a pass" % workflow_dir], 0, 0)

    total_settings = 0
    for path in paths:
        name = os.path.basename(path)
        try:
            text = io.open(path, encoding="utf-8").read()
        except Exception as exc:
            findings.append("[unreadable] %s: %s" % (name, exc))
            continue
        try:
            doc = yaml.safe_load(text)
        except Exception as exc:
            findings.append("[unparseable] %s: %s" % (name, exc))
            continue

        from_yaml = find_key_in_yaml(doc)
        from_text = find_key_in_text(text)

        # The two locators must agree on HOW MANY settings exist. If they do
        # not, this file contains a shape one of them cannot see, and the safe
        # reading is "unknown", not "fine".
        if len(from_yaml) != len(from_text):
            findings.append(
                "[locator-disagreement] %s: the YAML walk found %d %s "
                "setting(s) and the text scan found %d. One of them cannot see "
                "a shape in this file, so the result is unknown -- failing "
                "closed rather than reporting the smaller number as complete."
                % (name, len(from_yaml), KEY, len(from_text)))
            continue

        for value in from_yaml:
            total_settings += 1
            for f in check_value(value):
                findings.append("%s: %s" % (name, f))

    # The failure this project keeps rediscovering: a scanner that exits 0
    # having measured nothing. This repo HAS these settings; finding none means
    # the locators broke, not that the risk went away.
    if total_settings == 0 and not findings:
        findings.append(
            "[scanned-nothing] %d workflow(s) were read but not a single %s "
            "setting was found. This repository does use a vcpkg binary cache, "
            "so zero means this check stopped working -- not that there is "
            "nothing to check." % (len(paths), KEY))

    return findings, total_settings, len(paths)


# ---------------------------------------------------------------------------
# Self-tests -- the checker must be able to say NO as well as YES.
# ---------------------------------------------------------------------------
def self_test():
    failures = 0

    def check(label, got, want):
        nonlocal failures
        ok = got == want
        if not ok:
            failures += 1
        print("  [%s] %-58s got=%r" % ("ok  " if ok else "FAIL", label, got))

    # --- the shapes actually in the tree must PASS -------------------------
    check("real windows-lane value passes",
          check_value("clear;files,C:\\vcpkg-bincache,readwrite;x-gha,readwrite"),
          [])
    check("real non-windows value passes",
          check_value("clear;x-gha,readwrite"), [])
    check("quoted value passes",
          check_value("'clear;x-gha,readwrite'"), [])

    # --- NEGATIVE CASES: each rule must redden on its own violation --------
    def kinds(v):
        return sorted({f[f.index("["):f.index("]") + 1] for f in check_value(v)})

    check("dropping the leading 'clear' is caught",
          kinds("x-gha,readwrite"), ["[missing-clear]"])
    check("a nuget store is caught",
          kinds("clear;nuget,https://evil.example/f.json,readwrite"),
          ["[unapproved-store]"])
    check("an http store is caught",
          kinds("clear;http,https://evil.example/{sha},readwrite"),
          ["[unapproved-store]"])
    check("an azblob store is caught",
          kinds("clear;azblob,https://x.blob.core.windows.net/c,readwrite"),
          ["[unapproved-store]"])
    check("a relocated local cache is caught",
          kinds("clear;files,C:\\somewhere-else,readwrite"),
          ["[unapproved-path]"])
    check("a files store with no path is caught",
          kinds("clear;files"), ["[files-no-path]"])
    check("a bogus access mode is caught",
          kinds("clear;x-gha,readwr1te"), ["[bad-mode]"])
    check("an empty value is caught", kinds(""), ["[empty]"])

    # A violation must not be masked by the rest of the chain being fine.
    check("a bad store alongside good ones is still caught",
          kinds("clear;x-gha,readwrite;nuget,https://evil.example/f.json,read"),
          ["[unapproved-store]"])

    # --- fail-closed plumbing ----------------------------------------------
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        f, _, n = scan(td)
        check("an empty workflow dir FAILS (not a silent pass)",
              (len(f) == 1 and "[no-workflows]" in f[0], n), (True, 0))

    with tempfile.TemporaryDirectory() as td:
        io.open(os.path.join(td, "w.yml"), "w", encoding="utf-8").write(
            "name: x\n\"on\":\n  push:\njobs:\n  a:\n    runs-on: x\n")
        f, s, n = scan(td)
        check("workflows present but no setting FAILS closed",
              (len(f) == 1 and "[scanned-nothing]" in f[0], s, n),
              (True, 0, 1))

    with tempfile.TemporaryDirectory() as td:
        io.open(os.path.join(td, "w.yml"), "w", encoding="utf-8").write(
            "name: x\n\"on\":\n  push:\nenv:\n  %s: 'clear;x-gha,readwrite'\n"
            % KEY)
        f, s, n = scan(td)
        check("a good workflow passes and is COUNTED", (f, s, n), ([], 1, 1))

    with tempfile.TemporaryDirectory() as td:
        io.open(os.path.join(td, "w.yml"), "w", encoding="utf-8").write(
            "name: x\n\"on\":\n  push:\nenv:\n"
            "  %s: 'clear;nuget,https://evil.example/f.json,readwrite'\n" % KEY)
        f, s, _ = scan(td)
        check("a bad workflow is reported with its filename",
              (len(f) == 1 and f[0].startswith("w.yml:")
               and "[unapproved-store]" in f[0], s), (True, 1))

    with tempfile.TemporaryDirectory() as td:
        io.open(os.path.join(td, "w.yml"), "w", encoding="utf-8").write(
            "name: x\n\"on\":\n  push:\njobs:\n  a:\n    runs-on: x\n"
            "    env:\n      %s: 'clear;x-gha,readwrite'\n" % KEY)
        f, s, _ = scan(td)
        check("a JOB-level setting is found too", (f, s), ([], 1))

    with tempfile.TemporaryDirectory() as td:
        io.open(os.path.join(td, "w.yml"), "w", encoding="utf-8").write(
            "name: x\n\"on\": [push]\nthis: is: not: yaml:\n")
        f, _, _ = scan(td)
        check("unparseable YAML FAILS",
              len(f) == 1 and "[unparseable]" in f[0], True)

    print("self-test: %s" % ("PASS" if failures == 0
                             else "%d FAILURE(S)" % failures))
    return 1 if failures else 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--self-test", action="store_true",
                    help="prove the checker can say NO; needs no repo state")
    ap.add_argument("--workflow-dir", default=os.path.join(".github", "workflows"))
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    findings, settings, workflows = scan(args.workflow_dir)
    if findings:
        print("check_vcpkg_binary_sources: FAIL — %d finding(s)" % len(findings))
        for f in findings:
            print("  " + f)
        print("\nThis checker asserts the binary-cache STORE SET only. It does "
              "NOT verify the contents of any store; see "
              "docs/security/supply-chain-scope-decision.md.")
        return 1

    print("check_vcpkg_binary_sources: PASS — %d %s setting(s) across %d "
          "workflow(s) match the approved store set (configuration only; store "
          "CONTENTS are not verified — see "
          "docs/security/supply-chain-scope-decision.md)."
          % (settings, KEY, workflows))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
