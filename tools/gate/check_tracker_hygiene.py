#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_tracker_hygiene.py — GitHub-side TEXT is scanned on every push, not
by hand.

WHY THIS EXISTS
---------------
Every scrub guard before this one reasons about **git objects**. Issue bodies,
issue comments, release notes and Actions run logs live in GitHub's database,
and the controls that read them were hand-run migration scripts. Measured cost
of that, twice:

  * 2026-10-21: the first tracker scan ever run found 6 hits across 3 issue
    bodies — present on the source repository all along, untouched by four
    history rewrites, because issue text is not in git.
  * 2026-10-24: two live bodies (GitHub #277, #146) named the build host for
    three days after the needle that catches it was created, because the only
    scan was a script nobody was obliged to run (p100 phase-1 closed it).

`pub-readiness` also had to delete 283 leaking run logs from the previous
repository, so logs are a measured leak surface, not a hypothetical one.

This guard runs in `fast-checks.yml` on every push — the same standing,
GH_TOKEN-authenticated pattern `check_publication_block.py` proved — and in
`run_fast_checks_locally.py` via step discovery.

WHAT IT SCANS
-------------
`--tracker`: every issue/PR title+body, every issue comment, every release
name/body on the live repository, against the FULL needle inventory
(`load_all_needles()` — both files, fails closed; see GitHub #146/#277 for what
loading half the inventory cost).

`--logs N`: the logs of the N most recently created workflow runs. A bounded
sliding window rather than the full history, because the full set grows without
bound and each push re-scans; pushes are frequent relative to runs, so every
run's log sits inside the window for many consecutive pushes. The window size
is printed so a reader can judge the overlap. Runs whose logs have expired or
are not yet available are COUNTED AND NAMED, never silently skipped — and if
no log in the window is readable at all, that is a failure, not a clean pass.

WHAT IT REFUSES TO DO
---------------------
Report clean on any failed read. An API error, a 0-item tracker, a 0-run
window, or a failed sensitivity control all exit non-zero. "0 hits" from a
scanner that read nothing is the vacuous-pass shape this repository has been
burned by repeatedly; the header of check_scrub_regressions.py tells that
story.

TOKENS: authentication is whatever `gh` already holds (CI injects GH_TOKEN as
an environment variable; locally the gh keyring is used). Nothing here reads,
writes or logs a token.

Usage:
  python tools/gate/check_tracker_hygiene.py --tracker [--repo owner/name]
  python tools/gate/check_tracker_hygiene.py --logs 25 [--repo owner/name]
  python tools/gate/check_tracker_hygiene.py --self-test
Exit 0 = scanned and clean; 1 = a hit; 2 = could not scan (fails closed).
"""
from __future__ import annotations

import argparse
import importlib.util
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

for _n in ("stdout", "stderr"):
    _s = getattr(sys, _n, None)
    if _s is not None and hasattr(_s, "reconfigure"):
        _s.reconfigure(encoding="utf-8", errors="replace")

REPO_DEFAULT = "ipswitch8/PDFE"
_HERE = os.path.dirname(os.path.abspath(__file__))


def _load_guard():
    spec = importlib.util.spec_from_file_location(
        "csr", os.path.join(_HERE, "check_scrub_regressions.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _gh():
    return (os.environ.get("PDFE_GH")
            or shutil.which("gh.exe") or shutil.which("gh"))


def api_json(gh, path):
    """One paginated GET. Returns a flat list, or None on any failure."""
    r = subprocess.run([gh, "api", "-X", "GET", "--paginate", path],
                       capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if r.returncode != 0 or not r.stdout.strip():
        return None
    dec, s, i, flat = json.JSONDecoder(), r.stdout.strip(), 0, []
    while i < len(s):
        try:
            obj, i = dec.raw_decode(s, i)
        except ValueError:
            return None
        flat.extend(obj if isinstance(obj, list) else [obj])
        while i < len(s) and s[i] in " \r\n\t":
            i += 1
    return flat


# ---------------------------------------------------------------------------
# Pure decision logic — unit-tested by test_check_tracker_hygiene.py.
# ---------------------------------------------------------------------------

def scan_payload(payloads, needles, scan_text):
    """payloads: iterable of (where, text). Returns [(where, label), ...]."""
    hits = []
    for where, text in payloads:
        for _ln, lab in scan_text(text or "", needles):
            hits.append((where, lab))
    return hits


def controls_ok(scan_text, needles, live_value):
    """(sensitivity, specificity) — both must hold or every count is vacuous.

    `live_value` is the environment's own host string; it is matched, never
    printed.
    """
    sens = bool(live_value) and bool(scan_text("x %s y" % live_value, needles))
    spec = not scan_text("zzsynth qqq", needles)
    return sens, spec


def verdict(items_read, hits, unreadable=0, readable=None):
    """('clean'|'leak'|'refuse', reason). Refuses a 0-population pass, and a
    logs window where nothing was readable."""
    if items_read <= 0:
        return "refuse", "0 items read -- refusing to report clean on nothing"
    if readable is not None and readable <= 0:
        return "refuse", ("every log in the window was unreadable -- that is "
                          "not a clean result")
    if hits:
        return "leak", "%d hit(s)" % len(hits)
    return "clean", "0 hits over %d item(s)" % items_read


# ---------------------------------------------------------------------------


def run_controls(m, needles):
    live = (os.environ.get("COMPUTERNAME") or "").strip()
    if len(live) < 4:
        # CI runners always have COMPUTERNAME on Windows; a missing value means
        # the sensitivity control cannot run, and an uncontrolled scan is not
        # evidence. Hosted-Linux futures can revisit; today the runner is
        # Windows and this fails closed.
        print("FAIL: COMPUTERNAME absent/short -- sensitivity control cannot "
              "run, so a clean result would be unverifiable")
        return False
    sens, spec = controls_ok(m.scan_text, needles, live)
    print("controls: sensitivity %s, specificity %s"
          % ("PASS" if sens else "FAIL", "PASS" if spec else "FAIL"))
    return sens and spec


def scan_tracker(m, needles, gh, repo):
    issues = api_json(gh, "repos/%s/issues?state=all&per_page=100" % repo)
    comments = api_json(gh, "repos/%s/issues/comments?per_page=100" % repo)
    releases = api_json(gh, "repos/%s/releases?per_page=100" % repo)
    if issues is None or comments is None or releases is None:
        print("FAIL: could not read %s (issues=%s comments=%s releases=%s) -- "
              "refusing to report a clean tracker I could not read"
              % (repo, issues is not None, comments is not None,
                 releases is not None))
        return 2

    payloads = []
    for it in issues:
        payloads.append(("issue #%s title" % it.get("number"), it.get("title")))
        payloads.append(("issue #%s body" % it.get("number"), it.get("body")))
    for c in comments:
        payloads.append(("comment %s" % c.get("id"), c.get("body")))
    for r in releases:
        payloads.append(("release %s name" % r.get("tag_name"), r.get("name")))
        payloads.append(("release %s body" % r.get("tag_name"), r.get("body")))

    hits = scan_payload(payloads, needles, m.scan_text)
    print("tracker: %d issue/PR item(s), %d comment(s), %d release(s)"
          % (len(issues), len(comments), len(releases)))
    v, why = verdict(len(issues) + len(comments) + len(releases), hits)
    for where, lab in hits:
        print("   %s  needle=%s" % (where, lab))
    print("tracker verdict: %s (%s)" % (v.upper(), why))
    return {"clean": 0, "leak": 1, "refuse": 2}[v]


def scan_logs(m, needles, gh, repo, window):
    runs = api_json(gh, "repos/%s/actions/runs?per_page=100" % repo)
    if runs is None:
        print("FAIL: could not list workflow runs -- refusing to report clean")
        return 2
    # /actions/runs wraps the list; --paginate yields dicts with workflow_runs
    flat = []
    for r in runs:
        flat.extend(r.get("workflow_runs", []) if isinstance(r, dict)
                    and "workflow_runs" in r else [r])
    flat = [r for r in flat if isinstance(r, dict) and r.get("id")]
    flat.sort(key=lambda r: r.get("created_at") or "", reverse=True)
    recent = flat[:window]
    print("logs window: %d most recent of %d total run(s)"
          % (len(recent), len(flat)))
    if not recent:
        print("FAIL: 0 runs found -- that is wrong for this repository")
        return 2

    hits, readable, unreadable = [], 0, []
    tmp = tempfile.mkdtemp(prefix="pdfe-loghyg-")
    try:
        for r in recent:
            rid = r["id"]
            zp = os.path.join(tmp, "%d.zip" % rid)
            with io.open(zp, "wb") as fh:
                p = subprocess.run(
                    [gh, "api", "repos/%s/actions/runs/%d/logs" % (repo, rid)],
                    stdout=fh, stderr=subprocess.PIPE)
            if p.returncode != 0 or not os.path.getsize(zp):
                unreadable.append(rid)
                continue
            try:
                with zipfile.ZipFile(zp) as z:
                    for name in z.namelist():
                        if name.endswith("/"):
                            continue
                        text = z.read(name).decode("utf-8", "replace")
                        for _ln, lab in m.scan_text(text, needles):
                            hits.append(("run %d %s" % (rid, name[:60]), lab))
                readable += 1
            except zipfile.BadZipFile:
                unreadable.append(rid)
            finally:
                if os.path.exists(zp):
                    os.remove(zp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("logs: %d readable, %d unreadable/expired %s"
          % (readable, len(unreadable), unreadable[:10]))
    v, why = verdict(len(recent), hits, readable=readable)
    seen = set()
    for where, lab in hits:
        if (where, lab) in seen:
            continue
        seen.add((where, lab))
        print("   %s  needle=%s" % (where, lab))
    print("logs verdict: %s (%s)" % (v.upper(), why))
    return {"clean": 0, "leak": 1, "refuse": 2}[v]


def self_test():
    """Pure decision logic only — no network, no real needles."""
    import hashlib
    syn = hashlib.sha256(b"zzsynth host").hexdigest()
    needles = [{"label": "syn", "sha256": syn, "n": len("zzsynth host")}]

    m = _load_guard()
    bad = 0

    def case(name, ok):
        nonlocal bad
        print("  [%s] %s" % ("ok " if ok else "FAIL", name))
        bad += 0 if ok else 1

    hits = scan_payload([("a", "the zzsynth-host appears")], needles,
                        m.scan_text)
    case("a planted pair needle is found across a separator",
         hits == [("a", "syn")])
    case("an unrelated text does not hit",
         not scan_payload([("b", "nothing here")], needles, m.scan_text))
    case("a None body is tolerated",
         not scan_payload([("c", None)], needles, m.scan_text))
    sens, spec = controls_ok(m.scan_text, needles, "zzsynth host")
    case("controls: sensitivity fires on the live value", sens)
    case("controls: specificity stays silent", spec)
    sens2, _ = controls_ok(m.scan_text, needles, "")
    case("controls: an EMPTY live value cannot pass sensitivity", not sens2)
    case("verdict refuses a 0-item population",
         verdict(0, []) == ("refuse",
                            "0 items read -- refusing to report clean on "
                            "nothing"))
    case("verdict refuses an all-unreadable logs window",
         verdict(25, [], readable=0)[0] == "refuse")
    case("verdict reports a leak when there are hits",
         verdict(10, [("x", "syn")])[0] == "leak")
    case("verdict reports clean only with items and no hits",
         verdict(10, [])[0] == "clean")

    print("self-test: %d failure(s)" % bad)
    return 0 if bad == 0 else 1


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=REPO_DEFAULT)
    ap.add_argument("--tracker", action="store_true")
    ap.add_argument("--logs", type=int, default=0, metavar="N",
                    help="scan the N most recent run logs")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if not args.tracker and not args.logs:
        print("nothing to do: pass --tracker and/or --logs N")
        return 2

    gh = _gh()
    if not gh:
        print("FAIL: gh not found (set PDFE_GH) -- refusing to report clean")
        return 2

    m = _load_guard()
    needles, err = m.load_all_needles()
    if err:
        print("FAIL: could not load the needle inventory: %s" % err)
        return 2
    print("needles: %d (full inventory)" % len(needles))
    if not run_controls(m, needles):
        return 2

    rc = 0
    if args.tracker:
        rc = max(rc, scan_tracker(m, needles, gh, args.repo))
    if args.logs:
        rc = max(rc, scan_logs(m, needles, gh, args.repo, args.logs))
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
