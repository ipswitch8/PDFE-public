#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_scrub_regressions.py — stop scrubbed identifiers coming back.

WHY THIS EXISTS.

pub-readiness removed several identifying strings from this repository: an
operator's personal name, a company domain, and three customer-derived names.
The `claim-check` gate then failed the work for a reason worth quoting in
substance: *the scrub had no standing control.* `check_tracked_path_hygiene.py`
polices the build ACCOUNT name and un-placeholdered home paths and nothing
else, so if any scrubbed string returned in a later commit, nothing would
notice. A one-time sweep is not a mechanism.

THE DESIGN PROBLEM, AND WHY THE NEEDLES ARE HASHED.

A guard that names what it forbids reintroduces it. That is not hypothetical
here: `check_tracked_path_hygiene.py` once printed the account name it had just
found into a CI log, and `hostname_baseline.json` has to carry a
`HOSTNAME_SELF_EXEMPT` entry for itself because it must hold its own needle.
Writing a scrubbed company name or a person's surname into this file would make
this file the leak -- and an earlier draft of this very paragraph did exactly
that, naming one of them as an illustration. It went undetected because this
file had exempted itself from its own scan.

So the needles are stored as **SHA-256 of the lower-cased string**, never in
plaintext. The checker tokenises each file, hashes the tokens, and compares.
A hash tells a reader nothing, cannot be grepped for, and is safe to publish —
but it still matches exactly when the real string comes back.

This is deliberately NOT the approach used for the account name, which reads
its needle from `USERNAME`/`USER`/`LOGNAME` at run time. That works there
because the account name is ambient. These strings are not ambient: there is no
environment variable that yields a scrubbed company name, and DCA 0047 is the
record of what happens when you invent an ambient needle that is wrong (the
hosted runner's `USER=runner` matched 365 files and hid four real failures).

WHAT IT CANNOT DO, STATED PLAINLY.

* It matches whole tokens and adjacent token pairs, not arbitrary substrings.
  A needle welded into the middle of a longer word is not caught -- if the
  needle were `acme`, then `PrefixAcmeSuffix` passes. That is the price of
  hashing; the account-name half still does substring matching because it can
  hold its needle in the clear. (The example is deliberately a made-up needle:
  an earlier draft of this docstring illustrated the point using a REAL
  scrubbed string welded into a word, which made this file a leak and tripped
  the sibling checker. Illustrate limitations with fictional needles.)
* It cannot tell you WHAT matched. The message names the file, the line and the
  needle's label, never the value. That is the point.
* It only scans tracked and staged text files. Scrubbing history is a separate
  operation and this guard says nothing about it.

Usage:
  python tools/gate/check_scrub_regressions.py
  python tools/gate/check_scrub_regressions.py --self-test
  python tools/gate/check_scrub_regressions.py --add-needle   (interactive; see below)

Exit 0 = clean; 1 = a scrubbed identifier is back; 2 = the guard could not run
(missing or unreadable needle file — fails closed, never silently clean).
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

# THREE dirnames: this file is tools/gate/<name>.py, so two would stop at
# tools/ and `git ls-files` would then list only what is under tools/. That is
# not a loud failure -- it reports OK over a 7% sample. Caught by comparing the
# scanned count against the sibling hygiene checker's (245 vs 3,548), which is
# the only reason it did not ship.
REPO_ROOT = os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NEEDLE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "scrub_needles.json")

#: DELIBERATELY EMPTY, and that is the point.
#:
#: This set used to exempt this checker, its needle file and its tests, by
#: analogy with `HOSTNAME_SELF_EXEMPT` in check_tracked_path_hygiene.py. The
#: analogy was wrong. That checker needs an exemption because its baseline
#: holds the host name IN THE CLEAR; this one stores only SHA-256, so none of
#: its files contains a needle and none of them needs forgiving.
#:
#: The exemption was not merely unnecessary, it was actively harmful: it hid a
#: real leak. An early draft of this file's own docstring named a scrubbed
#: company name as an illustration, and the scan skipped the file, so nothing
#: reported it. A guard that excuses itself cannot police itself.
#:
#: Entries may be added back, but each needs a reason stating why that file
#: must contain a needle in plaintext -- and for a hash-based checker, there
#: should never be such a file.
SELF_EXEMPT: set[str] = set()

TEXT_EXT = {
    ".py", ".cpp", ".h", ".hpp", ".c", ".md", ".txt", ".json", ".yml", ".yaml",
    ".ps1", ".sh", ".cmake", ".xml", ".fdf", ".cfg", ".ini", ".toml", ".bat",
    ".cmd", ".rc", ".qrc", ".ui", ".spdx",
}

MAX_BYTES = 4 * 1024 * 1024

#: Tokens are maximal runs of letters/digits. Splitting on everything else is
#: what makes `C:\Users\someone\x` and `someone@example.com` both yield the
#: bare token, so a needle is caught in a path, an email and prose alike.
_TOKEN_RE = re.compile(r"[A-Za-z0-9]+")


def sha(s: str) -> str:
    return hashlib.sha256(s.lower().encode("utf-8")).hexdigest()


def load_needles(path: str = NEEDLE_FILE):
    """Return (list_of_entries, None) or (None, reason)."""
    try:
        with io.open(path, encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, ValueError) as exc:
        return None, str(exc)
    needles = data.get("needles")
    if not isinstance(needles, list) or not needles:
        return None, "no needles listed"
    for n in needles:
        if not isinstance(n, dict) or "sha256" not in n or "label" not in n:
            return None, "a needle entry is missing sha256 or label"
    return needles, None


#: The second half of the needle inventory. The build host name is NOT in
#: `scrub_needles.json`; it lives in `hostname_baseline.json` under
#: `hostname_needles`, for the historical reason that the host ratchet was built
#: separately and used a plaintext needle until 2026-10-24.
HOSTNAME_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "hostname_baseline.json")


def load_all_needles(needle_path: str = NEEDLE_FILE,
                     hostname_path: str = HOSTNAME_FILE):
    """Return (every needle in the inventory, None) or (None, reason).

    USE THIS, NOT `load_needles`, in anything that scans a corpus and then
    reports whether a scrubbed identifier is present. `load_needles` returns
    only the eight scrub needles and is blind to the build host.

    WHY THIS EXISTS. The inventory is SPLIT across two files, and a tool that
    loaded one of them reported a clean result over text that named the host:

      * `scan_tracker_text.py` printed "OK: no scrubbed identifier found in any
        scanned issue title/body or release note" over a tracker in which TWO
        LIVE ISSUE BODIES named the build host.
      * `redact_tracker_text.py` -- the tool that actually removes them --
        redacted issue #146 for `build-account` and issue #277 for
        `customer-a`, both recorded in `tracker_redactions.json`, and left the
        host name in THOSE SAME TWO BODIES. It could not redact what it could
        not see.

    Neither was a procedural failure. Both were complete with respect to the
    needle set they loaded. The inventory was the thing that was incomplete,
    which is why this returns the whole of it or an error, and never a subset.

    Fails CLOSED: a missing, unreadable, empty or still-plaintext hostname
    baseline is an error, not a quieter scan.
    """
    needles, err = load_needles(needle_path)
    if err:
        return None, "scrub needles: %s" % err

    try:
        with io.open(hostname_path, encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, ValueError) as exc:
        return None, "hostname baseline: %s" % exc
    if "hostnames" in data:
        return None, ("hostname baseline still carries the PLAINTEXT "
                      "`hostnames` key; it was replaced by hashed "
                      "`hostname_needles`")
    host = data.get("hostname_needles")
    if not isinstance(host, list) or not host:
        return None, "hostname baseline has no `hostname_needles`"
    for n in host:
        if not isinstance(n, dict) or "sha256" not in n:
            return None, "a hostname needle entry is missing sha256"

    merged = list(needles) + [dict(n, label=n.get("label") or "build-host")
                              for n in host]
    return merged, None


def scan_text(text: str, needles):
    """Return [(line_no, label)] for every needle found.

    Checks single tokens and adjacent PAIRS, so a two-word personal name is
    caught without either word alone being a needle — which matters because a
    bare forename would false-positive on unrelated people.
    """
    by_hash = {n["sha256"]: n["label"] for n in needles}
    hits = []
    for i, line in enumerate(text.splitlines(), start=1):
        toks = [t.lower() for t in _TOKEN_RE.findall(line)]
        for j, t in enumerate(toks):
            h = sha(t)
            if h in by_hash:
                hits.append((i, by_hash[h]))
            if j + 1 < len(toks):
                h2 = sha(t + " " + toks[j + 1])
                if h2 in by_hash:
                    hits.append((i, by_hash[h2]))
    return hits


def _norm(p: str) -> str:
    return p.replace("\\", "/")


def candidate_files():
    """Tracked, staged AND untracked-non-ignored text files.

    The untracked set is not optional, and it was missing from the first
    version. That version reported OK on a brand-new document which named two
    of the scrubbed strings outright, because an unstaged new file is in
    neither `git ls-files` nor `git diff --cached`. A guard that only sees a
    leak once someone stages it is a guard you can walk straight past.

    `--others --exclude-standard` is the same population the sibling hygiene
    checker uses ("tracked | staged | untracked non-ignored"), so the two
    checkers now agree on what "committable" means.
    """
    seen = []
    for args in (["git", "ls-files"],
                 ["git", "diff", "--cached", "--name-only"],
                 ["git", "ls-files", "--others", "--exclude-standard"]):
        out = subprocess.run(args, cwd=REPO_ROOT, capture_output=True,
                             text=True, encoding="utf-8",
                             errors="replace").stdout
        for rel in out.splitlines():
            rel = _norm(rel.strip())
            if not rel or rel in SELF_EXEMPT:
                continue
            if os.path.splitext(rel)[1].lower() not in TEXT_EXT:
                continue
            if rel not in seen:
                seen.append(rel)
    return seen


def self_test() -> int:
    """Both directions. A guard that cannot fail is not a guard."""
    test_needle = "zzqq-scrub-probe"
    needles = [{"sha256": sha(test_needle), "label": "test-probe"}]
    cases = [
        ("a line with nothing in it", 0, "clean text must not match"),
        ("mentions zzqq-scrub-probe here", 0,
         "hyphenated needle is NOT one token, so it must NOT match -- this "
         "records the documented limitation rather than pretending otherwise"),
    ]
    bad = 0
    for text, want, why in cases:
        got = len(scan_text(text, needles))
        ok = got == want
        bad += 0 if ok else 1
        print("  [%s] %s (want %d, got %d)" % ("ok  " if ok else "FAIL",
                                               why, want, got))

    # Single-token needle, the normal shape.
    n1 = [{"sha256": sha("examplecorpx"), "label": "probe-token"}]
    for text, want, why in (
        ("nothing to see", 0, "clean"),
        ("path C:/x/examplecorpx/y", 1, "needle inside a path is caught"),
        ("mail a@examplecorpx.com", 1, "needle inside an email is caught"),
        ("EXAMPLECORPX shouting", 1, "matching is case-insensitive"),
        ("examplecorpxy", 0, "a longer word is NOT a match (hashing is "
                             "token-exact; documented limitation)"),
    ):
        got = len(scan_text(text, n1))
        ok = got == want
        bad += 0 if ok else 1
        print("  [%s] %s (want %d, got %d)" % ("ok  " if ok else "FAIL",
                                               why, want, got))

    # Adjacent-pair needle, for a two-word personal name.
    n2 = [{"sha256": sha("ada lovelace"), "label": "probe-pair"}]
    for text, want, why in (
        ("ada lovelace wrote it", 1, "adjacent pair is caught"),
        ("ada byron wrote it", 0, "first word alone must not match"),
        ("lovelace alone", 0, "second word alone must not match"),
        ("Ada  Lovelace spaced", 1, "extra whitespace still pairs"),
    ):
        got = len(scan_text(text, n2))
        ok = got == want
        bad += 0 if ok else 1
        print("  [%s] %s (want %d, got %d)" % ("ok  " if ok else "FAIL",
                                               why, want, got))

    # The real needle file must load; a guard that cannot read it must fail.
    needles_real, err = load_needles()
    if needles_real is None:
        print("  [FAIL] the real needle file did not load: %s" % err)
        bad += 1
    else:
        print("  [ok  ] the real needle file loads (%d needles)"
              % len(needles_real))
    bad += 0 if load_needles("definitely/not/here.json")[0] is None else 1
    print("  [ok  ] a missing needle file reports failure rather than 'clean'")

    # THE POPULATION IS PART OF THE SPEC. The first version of this checker
    # resolved REPO_ROOT one directory short, listed only what is under
    # tools/, scanned 245 files instead of ~3,500 and reported OK. Nothing in
    # the output distinguished that from a real pass. These two assertions are
    # what make a repeat loud.
    if not os.path.isfile(os.path.join(REPO_ROOT, "CMakeLists.txt")):
        print("  [FAIL] REPO_ROOT=%s is not the repository root (no "
              "CMakeLists.txt); the scan would cover a subtree" % REPO_ROOT)
        bad += 1
    else:
        print("  [ok  ] REPO_ROOT is the repository root")

    n_files = len(candidate_files())
    if n_files < 2000:
        print("  [FAIL] only %d candidate file(s); the tracked text population "
              "is several thousand, so this scan is a sample, not a check"
              % n_files)
        bad += 1
    else:
        print("  [ok  ] candidate population is %d file(s)" % n_files)

    # THE NEEDLE COUNT IS ALSO PART OF THE SPEC.
    #
    # The needle set silently dropped from 8 to 7 and NOTHING went red: the
    # scan still reported OK, and this self-test still passed, because neither
    # checked how many needles it was scanning for. A guard with a needle
    # quietly removed is indistinguishable from a guard that is working -- the
    # same failure shape as the REPO_ROOT bug above, one level further in.
    #
    # The floor is deliberately a floor, not an equality: adding a needle is
    # normal and should not require editing a test. Removing one should.
    MIN_NEEDLES = 8
    n_needles = len(needles_real) if needles_real else 0
    if n_needles < MIN_NEEDLES:
        print("  [FAIL] only %d needle(s); at least %d are expected. A needle "
              "has been removed or lost -- check `git diff` on "
              "scrub_needles.json and whether the file was staged."
              % (n_needles, MIN_NEEDLES))
        bad += 1
    else:
        print("  [ok  ] needle count is %d (floor %d)" % (n_needles,
                                                          MIN_NEEDLES))

    # And the file that will actually be COMMITTED is the index copy, not the
    # working tree. Those diverged once: the working tree had 8 needles while
    # the index had 7, so everything reported green on a set that would not
    # have survived the commit.
    staged = subprocess.run(
        ["git", "show", ":tools/gate/scrub_needles.json"], cwd=REPO_ROOT,
        capture_output=True, text=True, encoding="utf-8",
        errors="replace")
    if staged.returncode == 0 and staged.stdout.strip():
        try:
            n_staged = len(json.loads(staged.stdout).get("needles", []))
        except ValueError:
            n_staged = -1
        if n_staged != n_needles:
            print("  [FAIL] the STAGED needle file has %d needle(s) but the "
                  "working tree has %d. The commit would carry the staged "
                  "one." % (n_staged, n_needles))
            bad += 1
        else:
            print("  [ok  ] staged and working-tree needle files agree (%d)"
                  % n_staged)
    else:
        print("  [ok  ] needle file is not staged (nothing to compare)")

    print("self-test: %s" % ("PASS" if bad == 0 else "%d FAILED" % bad))
    return 1 if bad else 0


def live_refs():
    """Branches and tags only.

    Deliberately NOT `--all`: that silently ignores an explicit ref list and
    pulls in whatever backup refs happen to be fetched locally. Pre-rewrite
    backups legitimately still contain every scrubbed string, so `--all` would
    report a leak that is not in the published repository. This trap has
    already produced false positives for two gates in this pipeline.
    """
    out = subprocess.run(
        ["git", "for-each-ref", "--format=%(refname)",
         "refs/heads/", "refs/tags/"],
        cwd=REPO_ROOT, capture_output=True, text=True,
        encoding="utf-8", errors="replace").stdout
    return [l.strip() for l in out.splitlines() if l.strip()]


def scan_history(needles):
    """Scan commit messages and every blob reachable from live refs.

    WHY THIS MODE EXISTS. The working-tree scan above says nothing about
    history, and the `claim-check` gate failed phase-5 for exactly that: the
    phase record deferred history to "phase-6", phase-6's acceptance criteria
    never mention these strings, and there is no phase-7. A deferral to a phase
    that does not require the work is a promise, not a mechanism.

    Returns (message_hits, blob_hits) as counts, plus a sample of refs.
    """
    refs = live_refs()
    if not refs:
        return None, None

    msgs = subprocess.run(["git", "log"] + refs + ["--format=%B"],
                          cwd=REPO_ROOT, capture_output=True, text=True,
                          encoding="utf-8", errors="replace").stdout
    msg_hits = len(scan_text(msgs, needles))

    # Blobs: list objects reachable from the live refs, keep the blobs, stream
    # their contents. Batched through cat-file rather than one process per
    # object, which on this repository is the difference between a minute and
    # an hour.
    objs = subprocess.run(["git", "rev-list", "--objects"] + refs,
                          cwd=REPO_ROOT, capture_output=True, text=True,
                          encoding="utf-8", errors="replace").stdout
    names = [l.split(" ", 1)[0] for l in objs.splitlines() if l.strip()]

    check = subprocess.run(
        ["git", "cat-file", "--batch-check=%(objectname) %(objecttype)"],
        cwd=REPO_ROOT, input="\n".join(names), capture_output=True, text=True,
        encoding="utf-8", errors="replace").stdout
    blobs = [l.split()[0] for l in check.splitlines()
             if l.endswith(" blob")]

    blob_hits = 0
    CHUNK = 500
    for i in range(0, len(blobs), CHUNK):
        part = blobs[i:i + CHUNK]
        body = subprocess.run(["git", "cat-file", "--batch"], cwd=REPO_ROOT,
                              input="\n".join(part), capture_output=True,
                              text=True, encoding="utf-8",
                              errors="replace").stdout
        blob_hits += len(scan_text(body, needles))

    return msg_hits, blob_hits


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--history", action="store_true",
                    help="also scan commit messages and every blob reachable "
                         "from refs/heads and refs/tags. Slow (minutes).")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    if args.history:
        # load_ALL_needles, deliberately. The history scan ran with only the
        # eight scrub needles until 2026-10-26, when p101 phase-4's claim-check
        # gate got a clean --history result WITH the pre-rewrite lineage still
        # reachable through a local safety branch -- possible only because the
        # scan was blind to the build-host needle in hostname_baseline.json.
        # The same split-inventory defect fixed everywhere else on 2026-10-24,
        # hiding in the one mode nobody re-pointed.
        needles, err = load_all_needles()
        if needles is None:
            print("[scrub-regressions] FAIL: cannot load the full needle "
                  "inventory (%s)." % err)
            return 2
        msg_hits, blob_hits = scan_history(needles)
        if msg_hits is None:
            print("[scrub-regressions] FAIL: no live refs found; refusing to "
                  "report clean on a history scan that covered nothing.")
            return 2
        print("[scrub-regressions] history scan over %d live ref(s): "
              "%d commit-message hit(s), %d blob hit(s)"
              % (len(live_refs()), msg_hits, blob_hits))
        if msg_hits or blob_hits:
            print("[scrub-regressions] FAIL: scrubbed identifiers remain in "
                  "HISTORY. The working tree being clean does not fix this -- "
                  "it needs a history rewrite. See "
                  "docs/audit/pub-readiness/ for the ones already performed.")
            return 1
        print("[scrub-regressions] OK: no scrubbed identifier in history.")
        return 0

    needles, err = load_needles()
    if needles is None:
        print("[scrub-regressions] FAIL: cannot read %s (%s). Refusing to "
              "report clean on a check that did not happen."
              % (NEEDLE_FILE, err))
        return 2

    violations = []
    scanned = 0
    for rel in candidate_files():
        full = os.path.join(REPO_ROOT, rel)
        try:
            if os.path.getsize(full) > MAX_BYTES:
                continue
            with io.open(full, encoding="utf-8", errors="replace") as fh:
                text = fh.read()
        except OSError:
            continue
        scanned += 1
        for line_no, label in scan_text(text, needles):
            violations.append((rel, line_no, label))

    print("[scrub-regressions] scanned %d tracked/staged text file(s) against "
          "%d hashed needle(s)" % (scanned, len(needles)))

    if not violations:
        print("[scrub-regressions] OK: no scrubbed identifier has returned.")
        return 0

    print("[scrub-regressions] FAIL: %d occurrence(s) of a scrubbed "
          "identifier are back." % len(violations))
    for rel, line_no, label in violations[:40]:
        # The VALUE is never printed -- only where it is and which class it is.
        print("    %s:%d -- scrubbed identifier (%s)" % (rel, line_no, label))
    if len(violations) > 40:
        print("    ... and %d more" % (len(violations) - 40))
    print("[scrub-regressions] These strings were removed deliberately (see "
          "docs/audit/pub-readiness/). Use the approved replacement, or if the "
          "occurrence is legitimate, add an exemption WITH a reason -- do not "
          "delete the needle.")
    return 1


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main())
