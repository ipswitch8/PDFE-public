# -*- coding: utf-8 -*-
"""Fail if a TRACKED file carries a real account name or an un-placeholdered home path.

WHY THIS EXISTS, AND WHY F73 DID NOT ALREADY COVER IT.

F73 / DCA 0042 closed the PRODUCER: `run_ui_suite.py` scrubs everything it
writes. That is the right fix for the artifacts it produces and it is still
intact. It cannot cover a raw `--gtest_output=xml` capture that a person copies
into `docs/` by hand, because those bytes never pass through it.

That is exactly what happened in 3778b3b: 50 JUnit captures landed in the tree
carrying the account name and absolute home paths, on a repository that had been
history-scrubbed and RENAMED to remove precisely this. `git grep` for the account
name went from 0 to 50 and nothing said a word.

The same shape as F48's sixth emission point: five producers were each closed
correctly, and the completeness claim around them was what was wrong. A
per-producer control cannot answer "is the tree clean" -- only the tree can.
**The tree is what ships, so the tree is what this checks.**

WHY THE REAL ACCOUNT NAME IS NOT WRITTEN DOWN HERE.

Hard-coding it would put the very string this guard exists to keep out of the
repository INTO the repository, in a file guaranteed to be read. So the name is
taken from the environment at run time, and home-directory paths are matched
structurally, with the substitution placeholders the 2026-08-22 history rewrite
established allowed by name.

That placeholder allowance is load-bearing and deliberate: 141 tracked files
legitimately contain `C:\\Users\\dev\\...`, including the scrub's own write-up
and the DCA records that quote it. A guard that reddens on correct content is
worse than no guard, because it trains people to ignore it.

Usage:
    python tools/gate/check_tracked_path_hygiene.py
    python tools/gate/check_tracked_path_hygiene.py --self-test

Exit 0 = clean, 1 = a violation, 2 = usage/IO error.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import re
import subprocess
import sys

#: Home-directory owner components that are KNOWN PLACEHOLDERS, not real
#: accounts. `dev` is what the 2026-08-22 history rewrite substituted; the
#: others are CI service accounts that are public by construction and identify
#: no person.
PLACEHOLDER_OWNERS = {"dev", "runner", "runneradmin", "actions", "administrator",
                      "user", "username", "youruser", "public", "default"}

#: Synthetic owners that appear in TEST DATA and in prose examples, verified one
#: by one against the tree rather than allowlisted in a batch to make a red
#: guard go green:
#:   somedev   -- fixture in run_ui_suite.py's and scrub_artifacts.py's own
#:                self-tests. The scrubber's tests necessarily contain the
#:                unscrubbed shape; that is what they are testing.
#:   someone   -- test_ui_suite_path_hygiene.cpp, illustrating the JSON-escaped
#:                form that DCA 0042 records a detector having missed.
#:   me        -- test_path_security.cpp's is_unc_or_remote() case.
#:   x         -- ConformanceChecker.cpp / test_crash_handler.cpp path fixtures.
#: A guard that reddens on correct content is worse than no guard, because it
#: teaches people to ignore it -- so these are allowed BY NAME, and any other
#: owner still fails.
SYNTHETIC_OWNERS = {"somedev", "someone", "me", "x"}

#: `C:\Users\<owner>`, `C:/Users/<owner>`, `/home/<owner>`, `/Users/<owner>`.
#: TWO patterns, with DIFFERENT case rules, because the two path families have
#: different truths and one regex for both is wrong in one direction or the
#: other.
#:
#: Windows -- IGNORECASE, and it is load-bearing rather than tidiness. The
#: filesystem is case-insensitive and captures contain `C:\Users\`, `c:\users\`
#: and every mixture; a case-SENSITIVE `Users` let `c:\users\<account>\...`
#: through entirely, which this module's own parametrised spelling test caught.
_WIN_HOME_RE = re.compile(
    r"[A-Za-z]:[\\/]{1,2}Users[\\/]{1,2}([A-Za-z0-9_.\-]+)", re.IGNORECASE)

#: POSIX -- case-SENSITIVE, and NOT preceded by a word character or a dot.
#:
#: Making this one case-insensitive too was tried and was a mistake: it matched
#: `PgUp/Home/End` in keyboard-shortcut prose, reporting "end" and "pgup" as
#: account names. And the lookbehind is what keeps `https://api.github.com/
#: users/ipswitch8` -- a public API URL in a CI fixture, not a filesystem path --
#: out of the report. A real POSIX home path follows whitespace, a quote or an
#: `=`, never a letter or a dot.
_POSIX_HOME_RE = re.compile(r"(?<![A-Za-z0-9.])(?:/home/|/Users/)([A-Za-z0-9_.\-]+)")

TEXT_EXT = {".md", ".yml", ".yaml", ".py", ".cpp", ".h", ".hpp", ".txt", ".json",
            ".xml", ".log", ".ps1", ".sh", ".cmake", ".in", ".cfg", ".ini",
            ".toml", ".csv", ".bat", ".cmd"}

MAX_BYTES = 32 * 1024 * 1024

#: This detector and its tests necessarily CONTAIN the shapes they detect --
#: `C:\Users\carol\x`, `/home/bob/build` -- exactly as `scrub_artifacts.py`'s
#: self-test must contain an unscrubbed path. Without this they flag themselves,
#: which is how this guard was first pushed RED.
#:
#: The exemption is deliberately PARTIAL, not a skip. Only the synthetic-owner
#: half is waived; these files are still scanned for THIS MACHINE'S ACCOUNT
#: NAME, which is the half that matters and the half a real leak would use. A
#: blanket skip would make the detector the one place in the tree where a leak
#: could hide, which is a poor trade for tidiness.
SELF_EXEMPT_OWNER_CHECK = {
    "tools/gate/check_tracked_path_hygiene.py",
    "tools/gate/test_check_tracked_path_hygiene.py",
}
#: NOT widened for `tools/gate/scan_binary_path_hygiene.py` (GitHub #189, p87
#: phase-4), even though that file is this detector's binary counterpart and its
#: self-tests must exercise the same shapes. `test_the_self_exemption_is_partial
#: _and_narrow` pins this set to exactly two entries, and that pin is correct:
#: every addition makes the detector's blind spot larger. That scanner instead
#: ASSEMBLES its fixture paths at run time from parts, so no complete home-path
#: literal exists in its source and it needs no exemption at all. Prefer that
#: route for the next one too.


def _norm(path):
    """Forward-slashed form, so the self-exemption matches on Windows too."""
    return str(path).replace("\\", "/")


#: Shared/service accounts that identify a MACHINE ROLE, not a person. Using one
#: of these as the needle produces pure noise: on the GitHub ubuntu runner
#: USER=runner, and this repository has a tools/runner-deployment package, a
#: self-hosted runner, runner_watchdog.ps1 and a runner-requirements manifest --
#: so "runner" matched 365 tracked files, including this workflow itself, and
#: fast-checks stayed red across five consecutive commits with the real signal
#: buried underneath. A guard that reddens on correct code is worse than no
#: guard; this one had also taken every other check in the job down with it.
GENERIC_ACCOUNTS = frozenset({
    "runner", "runneradmin", "root", "ubuntu", "admin", "administrator",
    "user", "vsts", "vssadministrator", "circleci", "travis", "jenkins",
    "build", "builder", "buildagent", "azureuser", "ec2-user", "docker",
    "github", "actions", "nobody", "default", "guest", "svc", "service",
})


def current_account_names():
    """Account names for THIS machine. Never written to disk by this script.

    Generic service accounts are excluded -- see GENERIC_ACCOUNTS. They cannot
    be the thing this check protects: nobody leaks "root" into a commit and
    harms themselves by it, and searching for one guarantees false positives in
    any repository that discusses CI.
    """
    names = set()
    for var in ("USERNAME", "USER", "LOGNAME"):
        value = os.environ.get(var, "").strip()
        # Two chars or fewer is too short to search for without drowning in
        # false positives; such a name is reported as unsupported rather than
        # silently skipped.
        if len(value) >= 3 and value.lower() not in GENERIC_ACCOUNTS:
            names.add(value.lower())
    return names


def running_in_ci():
    """True on a hosted CI runner, where there is no personal account to leak."""
    return any(os.environ.get(v) for v in
               ("GITHUB_ACTIONS", "CI", "TF_BUILD", "JENKINS_URL"))


def tracked_text_files():
    """Every text file this check should look at: tracked, plus STAGED.

    `git ls-files` alone has a one-commit blind spot, and it was not
    hypothetical. A file that is being ADDED is untracked right up until the
    commit lands, so a leak inside a brand-new file is invisible to every
    tracked-file scan until the commit AFTER the one that introduced it. That
    is exactly how tests/fixtures/test_coverage_inventory_scrub.py -- a file
    written to prevent account-name leaks -- was committed containing the
    account name in a docstring, with a fully green evidence run.

    Unioning in the index closes it: when nothing is staged this yields
    precisely what it always did, so there are no new false positives, and
    when a commit is being prepared the new files are checked BEFORE they
    become history rather than one commit later.

    UNTRACKED FILES ARE NOW IN SCOPE (GitHub #160), REVERSING WHAT THIS
    DOCSTRING USED TO SAY.

    The old reasoning was: "deliberately NOT `git grep --untracked`, which would
    also sweep in every unignored working-tree leftover that is intentionally
    not committed -- 21 of them here." That reasoning was measured against the
    wrong population, and the 21 leftovers were the evidence AGAINST it, not
    for it: every one of them was a raw gtest JUnit capture carrying the build
    account name in its absolute `file=` paths -- 6,190 occurrences between
    them -- sitting un-ignored in `docs/`, one `git add -A` from being
    committed. Meanwhile this guard printed "OK: no real account name in any
    tracked file", which was true and useless. It was clean about a set that
    excluded the leak.

    So the population is now tracked | staged | untracked-and-not-ignored,
    which is exactly what `git add -A` would commit. That is the set the guard
    is supposed to protect.

    IGNORED FILES STAY OUT, and that is the load-bearing distinction rather
    than tracked-vs-untracked. `--exclude-standard` honours `.gitignore`, so
    `scratchpad/`, build trees and `docs/audit/**/rerun/` (deliberately ignored
    BECAUSE raw gtest shards carry the account name ~380x per file) do not
    redden anything. A file that cannot be committed cannot leak.
    """
    seen = set()

    def emit(paths):
        for path in paths:
            if not path or path in seen:
                continue
            seen.add(path)
            if os.path.splitext(path)[1].lower() in TEXT_EXT:
                yield path

    tracked = subprocess.run(["git", "ls-files"], capture_output=True,
                             text=True, encoding="utf-8",
                             errors="replace").stdout.splitlines()
    yield from emit(tracked)

    # Staged additions/modifications. --diff-filter excludes deletions, which
    # have no content left to scan.
    staged = subprocess.run(
        ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR"],
        capture_output=True, text=True, encoding="utf-8",
        errors="replace").stdout.splitlines()
    yield from emit(staged)

    yield from emit(untracked_paths())


def untracked_paths():
    """Untracked, NON-IGNORED working-tree files -- what `git add -A` commits.

    Matching is done in Python, never with `grep -iF`: in this environment
    `grep -i` combined with `-F` aborts with exit 134, and inside an `if` or a
    `&&` chain that reads as "no match". It produced a false CLEAN during the
    very investigation that found GitHub #160.
    """
    out = subprocess.run(
        ["git", "ls-files", "--others", "--exclude-standard"],
        capture_output=True, text=True, encoding="utf-8",
        errors="replace")
    return out.stdout.splitlines()


def tracked_only_paths():
    """Tracked | staged paths, for checks that cannot apply to untracked files.

    The host-name RATCHET is one: it accepts a per-path occurrence count from
    tools/gate/hostname_baseline.json, and there is no sensible baseline entry
    for a file that is not in the repository. Keeping the ratchet on tracked
    files only avoids the DCA 0047 shape -- a guard that reddens on correct
    local state, drowning the real signal. The ABSOLUTE half (account name,
    un-placeholdered home path) applies to everything.
    """
    seen = set()
    for args in (["git", "ls-files"],
                 ["git", "diff", "--cached", "--name-only",
                  "--diff-filter=ACMR"]):
        out = subprocess.run(args, capture_output=True, text=True,
                             encoding="utf-8", errors="replace")
        for path in out.stdout.splitlines():
            if path:
                seen.add(_norm(path))
    return seen


def scan_text(text, accounts):
    """Return (account_hits, home_owner_hits) for one file's content."""
    lowered = text.lower()
    account_hits = sorted(a for a in accounts if a in lowered)
    owners = {m.group(1).lower() for m in _WIN_HOME_RE.finditer(text)}
    owners |= {m.group(1).lower() for m in _POSIX_HOME_RE.finditer(text)}
    # A run of dots is an ELISION in prose ("C:\Users\...\actions-runner") or a
    # traversal segment in a path-security fixture ("C:\Users\..\Windows"). It
    # is not an account name and never identifies anyone.
    owners = {o for o in owners if o.strip(".")}
    return account_hits, sorted(owners - PLACEHOLDER_OWNERS - SYNTHETIC_OWNERS)


def _self_test():
    accounts = {"alice"}
    hits, owners = scan_text(r"C:\Users\alice\source\PDFE", accounts)
    assert hits == ["alice"], hits
    assert owners == ["alice"], owners

    hits, owners = scan_text(r"C:\Users\dev\source\PDFE", accounts)
    assert hits == [], hits
    assert owners == [], "the rewrite placeholder must not red: %r" % owners

    # An unknown owner is flagged even when it is not THIS machine's account:
    # a leak from another host is still a leak.
    hits, owners = scan_text("/home/bob/build", accounts)
    assert hits == [] and owners == ["bob"], (hits, owners)

    # Forward-slash Windows form, as gtest emits in GetParam() strings.
    _, owners = scan_text("C:/Users/carol/x", accounts)
    assert owners == ["carol"], owners

    # Doubled backslashes, the JSON/C-escaped form. DCA 0042 records that a
    # detector which missed this was the reason a leak read as clean.
    _, owners = scan_text(r"C:\\Users\\dave\\x", accounts)
    assert owners == ["dave"], owners

    # Case-insensitivity on the account, since Windows paths vary in case.
    hits, _ = scan_text(r"c:\users\ALICE\x", {"alice"})
    assert hits == ["alice"], hits

    # Elisions and traversal segments are not names.
    _, owners = scan_text(r"C:\Users\...\actions-runner and C:\Users\..\Windows",
                          accounts)
    assert owners == [], owners

    # The synthetic fixtures the tree really uses must not red...
    _, owners = scan_text(r"C:\Users\somedev\source\PDFE  c:/Users/me/doc.pdf",
                          accounts)
    assert owners == [], owners

    # ...but a name that merely LOOKS synthetic is not exempt. The allowlist is
    # by name, deliberately, so it cannot quietly widen.
    _, owners = scan_text(r"C:\Users\somedev2\x", accounts)
    assert owners == ["somedev2"], owners

    print("[path-hygiene] self-test OK (9 checks)")
    return 0


# ---------------------------------------------------------------------------
# HOST-NAME RATCHET (GitHub #146)
#
# The account-name half above is absolute: the name must appear NOWHERE. The
# host name is deliberately weaker -- a ratchet. Existing occurrences are
# accepted and enumerated in tools/gate/hostname_baseline.json; new ones fail.
# The reasoning for that asymmetry (a host name is not a person; ~25 of the
# occurrences are historical audit narrative; the real risk is machine-generated
# GROWTH) is recorded in the baseline file itself, so the next reader does not
# have to re-derive it.
#
# THE NAMES COME FROM THE BASELINE FILE, NOT FROM THE ENVIRONMENT. Deriving them
# from COMPUTERNAME would repeat DCA 0047 exactly: the account half took its
# needle from USER at run time, matched `runner` on the hosted CI box, flagged
# 365 files, and hid four real failures for five commits. The CI host is not
# this host, so an ambient host name is the wrong needle by construction.
# ---------------------------------------------------------------------------

# HOSTNAME_SELF_EXEMPT IS GONE -- removed 2026-10-24, and its removal is the
# point of that change.
#
# It used to exempt this checker, its baseline and its tests, because all three
# had to NAME the host to police it. That was true of a PLAINTEXT needle, and it
# made those three the only tracked files still carrying the machine name --
# correct for a private repository, and a disclosure the moment the repository
# is published (GitHub #200, now live).
#
# The needle is now HASHED, exactly as `scrub_needles.json` has always stored
# the account name, for the reason stated there: "a guard that names what it
# forbids reintroduces it". No tracked file names the host, so nothing needs
# exempting, and the self-match that bit three guards in this repo
# (lint_unc_gate, CiSupplyChain's comment scan, and this one) is now impossible
# by construction rather than by an allowlist.

HOSTNAME_BASELINE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 "hostname_baseline.json")

#: Same tokenising rule as check_scrub_regressions.py. Kept identical on
#: purpose; test_the_two_halves_tokenise_identically pins it.
_HOST_TOKEN_RE = re.compile(r"[A-Za-z0-9]+")


def load_hostname_baseline(path=HOSTNAME_BASELINE):
    """Return (hashed_needles, accepted_files), or (None, None) if unreadable.

    FAILS CLOSED on a baseline that still carries the plaintext `hostnames`
    key: that file was converted to hashes on 2026-10-24, so its reappearance
    means either a bad merge or a revert, and silently scanning for nothing is
    the one outcome worth refusing. A guard that cannot find its needle must
    not report a clean tree.
    """
    try:
        with io.open(path, encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, ValueError):
        return None, None
    if data.get("hostnames"):
        print("[path-hygiene] FAIL: %s carries a plaintext 'hostnames' key. "
              "The needle is hashed as of 2026-10-24; a plaintext needle in a "
              "tracked file is the exposure this change removed."
              % HOSTNAME_BASELINE)
        return None, None
    needles = [n for n in data.get("hostname_needles", [])
               if isinstance(n, dict) and n.get("sha256")]
    accepted = {_norm(k): int(v) for k, v in data.get("accepted_files", {}).items()}
    return needles, accepted


#: Per-process memo for the host-name scan, keyed by (path, size, mtime_ns).
#
# WHY IT EXISTS, measured rather than assumed. Hashed matching cannot use a
# single C-level `str.count`, so it costs 13.8 s over the real 129.7 MB / 3,596
# file corpus where the old plaintext substring scan cost ~1 s. One guard run
# absorbing that is immaterial next to a 15-minute CI job budget. THREE runs in
# one pytest process are not: `test_check_tracked_path_hygiene.py` calls
# `main([])` three times to exercise different environment branches of the
# ACCOUNT half, each one re-paying the whole host-name cost, and that pushed the
# local sweep's 900 s pytest batch to a TIMEOUT (exit 124).
#
# Keyed on content identity, not on path alone, so a file edited between runs is
# rescanned. A stale cache in a leak detector would be the worst kind of bug, so
# the key includes size and mtime_ns and the memo is per-process only -- never
# written to disk.
_HOST_SCAN_MEMO: dict = {}


def scan_hostnames_cached(path, text, needles):
    """`scan_hostnames`, memoised on (path, size, mtime_ns)."""
    try:
        st = os.stat(path)
        key = (path, st.st_size, st.st_mtime_ns)
    except OSError:
        return scan_hostnames(text, needles)
    hit = _HOST_SCAN_MEMO.get(key)
    if hit is None:
        hit = scan_hostnames(text, needles)
        _HOST_SCAN_MEMO[key] = hit
    return hit


def scan_hostnames(text, needles):
    """Occurrence count of any baselined host name in one file's content.

    Hashed matching, token-based. The previous implementation counted a
    plaintext substring, chosen so the bare machine name would also catch the
    longer runner name built around it. That property is PRESERVED here by a
    different route: every maximal alphanumeric token and every space-joined
    adjacent PAIR is hashed, so a two-part machine name matches as a pair, and
    that pair is still present inside a longer runner name that embeds it.

    It is also strictly better on separators -- `X-Y`, `X_Y`, `X.Y` and `XY`
    all reduce to the same pair, where substring matching saw four strings.

    Single bare tokens are hashed too, so a one-word machine name still works;
    the baseline decides which forms are needles, and for a multi-part name it
    deliberately lists only the pairs, because a short component on its own is
    far too generic to be one.
    """
    by_hash = {n["sha256"] for n in needles}
    if not by_hash:
        return 0

    # LENGTH PRE-FILTER, and it is not an optimisation nicety -- without it this
    # function cost 66 s per run and timed out the fast-checks pytest batch
    # (exit 124), because it hashed all 9.9M tokens of 3,596 files. `n` is the
    # character length of each hashed string, so a token or pair of a different
    # length is skipped without hashing. Needles with no `n` recorded fall back
    # to being hashed always, so an older baseline still works rather than
    # silently matching nothing.
    single_n = {n["n"] for n in needles
                if n.get("form") != "adjacent-pair" and n.get("n")}
    pair_n = {n["n"] for n in needles
              if n.get("form") == "adjacent-pair" and n.get("n")}
    any_unsized = any(not n.get("n") for n in needles)
    want_single = bool(single_n) or any_unsized
    want_pair = bool(pair_n) or any_unsized

    sha = hashlib.sha256
    hits = 0
    for line in text.splitlines():
        toks = [t.lower() for t in _HOST_TOKEN_RE.findall(line)]
        for j, t in enumerate(toks):
            if want_single and (any_unsized or len(t) in single_n):
                if sha(t.encode("utf-8")).hexdigest() in by_hash:
                    hits += 1
            if want_pair and j + 1 < len(toks):
                nxt = toks[j + 1]
                if any_unsized or (len(t) + 1 + len(nxt)) in pair_n:
                    pair = t + " " + nxt
                    if sha(pair.encode("utf-8")).hexdigest() in by_hash:
                        hits += 1
    return hits


def shown_owners(owners, accounts, needle_hashes):
    """Decide which home-path owners may be PRINTED in a violation diagnostic.

    Pure, so test_check_tracked_path_hygiene.py can pin it directly (the live
    `main()` scans the whole tree and cannot be driven with a fixture owner).
    An owner is withheld if it matches an ambient account OR if its lower-cased
    sha256 is a known scrub needle -- the latter is the p101 phase-6 fix: on CI
    the leaked owner (the build account) was FOREIGN to the runner's account,
    so ambient-only redaction named it straight into the Actions log. Returns
    (shown_list, redacted_count).
    """
    shown = [o for o in owners
             if o not in accounts
             and hashlib.sha256(o.lower().encode("utf-8")).hexdigest()
             not in needle_hashes]
    return shown, len(owners) - len(shown)


def _load_scrub_needle_hashes():
    """sha256 set from tools/gate/scrub_needles.json, for redacting a
    diagnostic owner that is itself a forbidden identifier. Returns an empty
    set if the file is unreadable -- redaction is best-effort defence in depth
    on top of the ambient-account redaction, never the sole barrier."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "scrub_needles.json")
    try:
        with io.open(path, encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, ValueError):
        return set()
    return {n["sha256"] for n in data.get("needles", [])
            if isinstance(n, dict) and "sha256" in n}


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)
    if args.self_test:
        return _self_test()

    accounts = current_account_names()
    account_half_ran = True
    if not accounts:
        if running_in_ci():
            # A hosted runner has no personal account, so the ambient-name half
            # has nothing meaningful to search for. Run the half that still
            # works rather than reporting a failure nobody can act on -- but say
            # plainly which half did NOT run. A skip that announces itself is a
            # different thing from a vacuous pass.
            #
            # The loss is bounded: the leaks this check exists for looked like
            # `C:\Users\<owner>\...` and `/home/<owner>/...` in committed
            # evidence files, and the home-path half below catches exactly that
            # shape, name-independently, and is still armed here.
            account_half_ran = False
            print("[path-hygiene] NOTE: hosted CI -- no personal account in "
                  "USERNAME/USER/LOGNAME (only a service account), so the "
                  "ambient-account-name half CANNOT run here. The home-path "
                  "half IS running and catches the leak shape that actually "
                  "occurred. Run this locally for the account-name half.")
        else:
            # Fail closed and say so. "Could not determine the account" passing
            # vacuously is this repository's single most repeated defect shape.
            print("[path-hygiene] FAIL: no usable account name in USERNAME/USER/"
                  "LOGNAME, so the account-name half of this check cannot run. "
                  "Refusing to report clean on a check that did not happen.")
            return 1

    violations = []
    unreadable = []
    oversized = []
    scanned = 0
    listed = 0
    ratchet_scope = tracked_only_paths()

    hostnames, accepted_hosts = load_hostname_baseline()
    hostname_growth = []
    if hostnames is None:
        # Fail closed: a missing baseline must not read as "no host names to
        # check". That is the vacuous-pass shape this file already warns about.
        print("[path-hygiene] FAIL: %s is missing or unreadable, so the "
              "host-name ratchet cannot run. Refusing to report clean on a "
              "check that did not happen." % HOSTNAME_BASELINE)
        return 1
    for path in tracked_text_files():
        listed += 1
        try:
            if os.path.getsize(path) > MAX_BYTES:
                oversized.append((path, "larger than %d bytes" % MAX_BYTES))
                continue
            with io.open(path, encoding="utf-8", errors="replace") as fh:
                text = fh.read()
        except OSError as exc:
            unreadable.append((path, str(exc)))
            continue
        scanned += 1
        account_hits, owner_hits = scan_text(text, accounts)
        if _norm(path) in SELF_EXEMPT_OWNER_CHECK:
            owner_hits = []          # account-name half stays armed; see above
        if account_hits or owner_hits:
            violations.append((path, bool(account_hits), owner_hits))

        # Host-name ratchet. NO self-exemption any more: the needle is hashed,
        # so this checker, its baseline and its tests no longer name the host
        # and have nothing to exempt. Every file in scope is scanned, including
        # these three -- which is a strictly larger population than before.
        if hostnames and _norm(path) in ratchet_scope:
            n = scan_hostnames_cached(path, text, hostnames)
            allowed = accepted_hosts.get(_norm(path), 0)
            if n > allowed:
                hostname_growth.append((path, n, allowed))

    print("[path-hygiene] listed %d text file(s) (tracked | staged | untracked "
          "non-ignored); scanned %d" % (listed, scanned))
    if oversized:
        print("[path-hygiene] NOT scanned: %d file(s) over the size cap -- "
              "reported, not ignored" % len(oversized))
        for path, why in oversized[:5]:
            print("    %s (%s)" % (path, why))

    # READ == LISTED, OR FAIL (GitHub #160, trap 2). A second false CLEAN in the
    # investigation behind #160 came from a stale working directory: the file
    # list was non-empty, EVERY open failed, and the run still concluded "none
    # carry the account name". A check that opened nothing has not checked
    # anything, so a failure to read is a failure of the check -- not a note.
    if unreadable:
        print("[path-hygiene] FAIL: %d listed file(s) could not be READ, so "
              "this run did not scan what it enumerated. Reporting clean on "
              "the remainder would repeat the exact false-CLEAN that GitHub "
              "#160 records." % len(unreadable))
        for path, why in unreadable[:10]:
            print("    %s (%s)" % (path, why))
        return 1
    if listed and scanned + len(oversized) != listed:
        print("[path-hygiene] FAIL: listed %d file(s) but accounted for only "
              "%d. The scan lost files silently."
              % (listed, scanned + len(oversized)))
        return 1

    if hostname_growth:
        print("[path-hygiene] FAIL: %d tracked file(s) carry MORE occurrences of "
              "the self-hosted runner's host name than the ratchet accepts."
              % len(hostname_growth))
        for path, found, allowed in hostname_growth:
            print("    %s -- %d occurrence(s), baseline allows %d"
                  % (path, found, allowed))
        print("[path-hygiene] The host name is accepted where it already is "
              "(tools/gate/hostname_baseline.json) and must not spread. If a "
              "GENERATED artifact grew it, fix the producer -- "
              "tests/scripts/sanitize_gtest_xml.py strips the JUnit `hostname` "
              "attribute. Baselining a fresh addition makes the ratchet a "
              "rubber stamp.")
        return 1

    if not violations:
        if account_half_ran:
            print("[path-hygiene] OK: no real account name and no "
                  "un-placeholdered home path in any committable file (tracked, staged, or untracked non-ignored).")
        else:
            print("[path-hygiene] OK (home-path half only): no un-placeholdered "
                  "home path in any committable file (tracked, staged, or untracked non-ignored). The account-name half did "
                  "NOT run -- see the NOTE above.")
        return 0

    print("[path-hygiene] FAIL: %d committable file(s) carry host-identifying "
          "data." % len(violations))
    # The offending account name is deliberately NOT echoed: printing it into a
    # CI log that is itself a published artifact would defeat the point.
    #
    # That claim used to be only half true. `has_account` was suppressed, but
    # the home-path owner list was printed verbatim -- and when the leak is
    # this machine's own home directory (the common case, and the exact shape
    # of the #160 leak) the owner IS the account name. So the guard for the
    # account name printed the account name. Owners that match an ambient
    # account are now redacted; a FOREIGN owner is still named, because that is
    # the diagnostic and it identifies nobody on this machine.
    # Owners that are a KNOWN SCRUB NEEDLE must be redacted even when they are
    # foreign to this machine's ambient accounts. On the CI runner the leaked
    # home-path owner (the build account) was foreign to the runner's own
    # account, so the ambient-only redaction named it -- and that diagnostic
    # went straight into the Actions log, a scanned publication surface
    # (p101 phase-6: the --logs guard caught exactly this). A guard that
    # forbids the account name knows it as a hashed needle; it must never print
    # the value, whoever's machine it is running on.
    _needle_hashes = _load_scrub_needle_hashes()

    for path, has_account, owners in violations:
        parts = []
        if has_account:
            parts.append("this machine's account name")
        shown, redacted = shown_owners(owners, accounts, _needle_hashes)
        if shown:
            parts.append("home path owner(s): %s" % ", ".join(shown))
        if redacted:
            parts.append("%d home path owner(s) matching this machine's "
                         "account, redacted" % redacted)
        print("    %s -- %s" % (path, "; ".join(parts)))
    print("[path-hygiene] Fix with: python tools/gate/scrub_artifacts.py . <files>")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
