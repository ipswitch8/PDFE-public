#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_needle_inventory.py — a tool that scans for scrubbed identifiers must
load the WHOLE inventory, not half of it.

WHY THIS EXISTS
---------------
The needle inventory is SPLIT across two files, for a historical reason:

  * ``tools/gate/scrub_needles.json``    — 8 needles: the build account name,
    the operator's real name, a company name, and the customer-derived
    identifiers. Hashed from the start.
  * ``tools/gate/hostname_baseline.json`` — the build host, under
    ``hostname_needles``. Built separately, and PLAINTEXT until 2026-10-24.

Nothing made a consumer load both. Measured 2026-10-24, two did not, and both
reported success over text that named the build host:

  * ``scan_tracker_text.py`` printed "OK: no scrubbed identifier found in any
    scanned issue title/body or release note" across 324 live tracker items.
    Re-run with the full inventory: **3 hits in 2 issue bodies**
    (GitHub #277, open; GitHub #146, closed).
  * ``redact_tracker_text.py`` — the tool that actually removes them — redacted
    GitHub #146 for ``build-account`` and GitHub #277 for ``customer-a``, both
    recorded in ``tracker_redactions.json``, and left the host name in THOSE
    SAME TWO BODIES.

Neither was a procedural failure: each was complete with respect to the needle
set it loaded. **The inventory was incomplete, and nothing checked that.** This
is the check.

That distinction is the whole point. Two fresh-repo migrations and four history
rewrites were each sound for the surface they targeted, and each was followed by
the discovery of a surface, or a needle, it had not covered. A ratchet on
procedure would have caught none of them. A ratchet on COVERAGE catches this
class.

WHAT IT ENFORCES
----------------
Any tracked Python file that loads the scrub needle set must also reach the host
needle — in practice by calling ``load_all_needles()`` rather than
``load_needles()`` — or carry an explicit in-file exemption::

    # needle-inventory: exempt -- <reason>

The implication runs one way only, and deliberately. A host-ONLY consumer is
not the failure mode: those tools are narrowly named for what they do
(``verify_sixth_rewrite.py``, ``sanitize_gtest_xml.py``) and claim nothing about
the other needles. The failure mode is a general-purpose scanner that answers
"is any scrubbed identifier present?" while knowing about only some of them.

Also enforced: ``load_all_needles`` must exist, must consult both files, and
must fail closed on a plaintext baseline — otherwise this guard polices calls to
a function that no longer does the thing.

Usage:
  python tools/gate/check_needle_inventory.py [--repo-root .] [--self-test]
Exit 0 = every consumer reaches the whole inventory; 1 = at least one does not.
"""
from __future__ import annotations

import argparse
import importlib.util
import io
import os
import re
import subprocess
import sys

for _n in ("stdout", "stderr"):
    _s = getattr(sys, _n, None)
    if _s is not None and hasattr(_s, "reconfigure"):
        _s.reconfigure(encoding="utf-8", errors="replace")

SCRUB_FILE = os.path.join("tools", "gate", "scrub_needles.json")
HOST_FILE = os.path.join("tools", "gate", "hostname_baseline.json")

#: The reason must be on the SAME LINE as the marker.
#:
#: This was first written with ``\s*(.+)``, and a reasonless exemption passed:
#: ``\s`` matches a newline, so after ``-- `` the greedy ``\s*`` crossed onto
#: the next line and ``(.+)`` captured the following line of CODE as the
#: "reason". The escape hatch therefore accepted ``# needle-inventory: exempt --``
#: with nothing after it -- exactly the waiver-without-justification this guard
#: is supposed to refuse. Caught by probing the reasonless case rather than
#: only the happy path. ``[^\S\n]`` is horizontal whitespace only.
EXEMPT_RE = re.compile(r"needle-inventory:[^\S\n]*exempt[^\S\n]*--[^\S\n]*"
                       r"([^\n]*)")

#: A reason must say something. Three non-space characters is a low bar chosen
#: to reject "", " ", "-" and "x" without inviting an argument about prose.
MIN_REASON_CHARS = 3

#: Reaching the scrub set: either the file by name, or the loader.
SCRUB_RE = re.compile(r"scrub_needles\.json|load_needles|load_all_needles")
#: Reaching the host needle: the file, the key, or the combined loader.
HOST_RE = re.compile(r"hostname_baseline\.json|hostname_needles|"
                     r"load_all_needles|HOSTNAME_FILE")

#: The guard module itself defines both and is the thing being single-sourced.
DEFINER = os.path.join("tools", "gate", "check_scrub_regressions.py")


def _norm(p: str) -> str:
    return p.replace("\\", "/")


def tracked_py(repo_root: str):
    r = subprocess.run(["git", "-C", repo_root, "ls-files", "*.py"],
                       capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if r.returncode != 0:
        return None
    return [p for p in r.stdout.split() if p]


def classify(text: str, path: str):
    """Pure decision logic: ('ok'|'blind'|'exempt'|'n/a', reason)."""
    if _norm(path) == _norm(DEFINER):
        return "ok", "defines the inventory"
    scrub = bool(SCRUB_RE.search(text))
    host = bool(HOST_RE.search(text))
    if not scrub:
        return "n/a", "does not load the scrub needle set"
    if host:
        return "ok", "reaches both halves"
    m = EXEMPT_RE.search(text)
    if m and len(m.group(1).strip()) >= MIN_REASON_CHARS:
        return "exempt", m.group(1).strip()
    if m:
        return "blind", ("exemption marker present but gives no reason on the "
                         "same line")
    return "blind", ("loads the scrub needles but never reaches the host "
                     "needle; call load_all_needles()")


def check_loader(repo_root: str) -> list[str]:
    """The function this guard points everyone at must actually do the job."""
    problems = []
    path = os.path.join(repo_root, DEFINER)
    spec = importlib.util.spec_from_file_location("_csr_inv", path)
    try:
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
    except Exception as exc:
        return ["could not import %s: %s" % (DEFINER, exc)]

    if not hasattr(mod, "load_all_needles"):
        return ["%s does not define load_all_needles()" % DEFINER]

    merged, err = mod.load_all_needles()
    if err:
        problems.append("load_all_needles() failed on the real tree: %s" % err)
        return problems
    base, berr = mod.load_needles()
    if berr:
        problems.append("load_needles() failed on the real tree: %s" % berr)
        return problems
    if len(merged) <= len(base):
        problems.append(
            "load_all_needles() returned %d needles, load_needles() returned "
            "%d -- it is not actually adding the host needle"
            % (len(merged), len(base)))

    # Must fail CLOSED on a plaintext baseline, not scan a subset.
    import json
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        bad = os.path.join(td, "plaintext.json")
        io.open(bad, "w", encoding="utf-8").write(
            json.dumps({"hostnames": ["some-host"]}))
        _m, e = mod.load_all_needles(hostname_path=bad)
        if not e:
            problems.append("load_all_needles() accepted a PLAINTEXT "
                            "`hostnames` baseline instead of failing closed")
        missing = os.path.join(td, "nope.json")
        _m, e = mod.load_all_needles(hostname_path=missing)
        if not e:
            problems.append("load_all_needles() accepted a MISSING hostname "
                            "baseline instead of failing closed")
        empty = os.path.join(td, "empty.json")
        io.open(empty, "w", encoding="utf-8").write(
            json.dumps({"hostname_needles": []}))
        _m, e = mod.load_all_needles(hostname_path=empty)
        if not e:
            problems.append("load_all_needles() accepted an EMPTY needle list "
                            "instead of failing closed")
    return problems


def self_test() -> int:
    """Decision logic only -- no I/O, no repo required."""
    cases = [
        ("x = load_all_needles()", "a.py", "ok"),
        ("n, e = guard.load_needles()", "a.py", "blind"),
        ("open('tools/gate/scrub_needles.json')", "a.py", "blind"),
        ("n = load_needles()\n# needle-inventory: exempt -- narrow tool",
         "a.py", "exempt"),
        # Reasonless exemptions, in every shape that reached this guard. The
        # second and third are the ones that originally ESCAPED, because `\s*`
        # crossed the newline and captured the next line as the reason.
        ("n = load_needles()\n# needle-inventory: exempt --  ",
         "a.py", "blind"),
        ("# needle-inventory: exempt --   \nn = load_needles()",
         "a.py", "blind"),
        ("# needle-inventory: exempt --\nreason_on_next_line = 1\n"
         "n = load_needles()", "a.py", "blind"),
        ("# needle-inventory: exempt -- x\nn = load_needles()",
         "a.py", "blind"),
        ("open('tools/gate/hostname_baseline.json')", "a.py", "n/a"),
        ("nothing relevant here", "a.py", "n/a"),
        ("n = load_needles()\nopen('hostname_baseline.json')", "a.py", "ok"),
        ("whatever", DEFINER, "ok"),
    ]
    bad = 0
    for text, path, want in cases:
        got, _why = classify(text, path)
        if got != want:
            bad += 1
            print("  self-test FAIL: %-44r -> %s, wanted %s"
                  % (text[:44], got, want))
    # A guard whose self-test only ever passes proves nothing; confirm the
    # classifier really can say 'blind'.
    if classify("n = load_needles()", "z.py")[0] != "blind":
        bad += 1
        print("  self-test FAIL: classifier cannot report 'blind' at all")
    print("self-test: %d case(s), %d failure(s)" % (len(cases), bad))
    return 0 if bad == 0 else 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo-root", default=".")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    root = os.path.abspath(args.repo_root)
    for rel in (SCRUB_FILE, HOST_FILE):
        if not os.path.exists(os.path.join(root, rel)):
            print("FAIL: %s is missing -- the inventory is not what this guard "
                  "thinks it is" % rel)
            return 1

    loader_problems = check_loader(root)
    if loader_problems:
        print("FAIL: the shared loader does not hold up:")
        for p in loader_problems:
            print("   %s" % p)
        return 1
    print("load_all_needles(): present, adds the host needle, and fails closed "
          "on a missing / empty / plaintext baseline")

    files = tracked_py(root)
    if files is None:
        print("FAIL: could not list tracked files")
        return 1
    if not files:
        print("FAIL: 0 tracked .py files -- refusing to report success after "
              "scanning nothing")
        return 1

    blind, exempt, ok = [], [], 0
    for rel in files:
        full = os.path.join(root, rel)
        try:
            text = io.open(full, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        verdict, why = classify(text, rel)
        if verdict == "blind":
            blind.append((rel, why))
        elif verdict == "exempt":
            exempt.append((rel, why))
        elif verdict == "ok":
            ok += 1

    print("tracked .py files scanned      : %d" % len(files))
    print("consumers reaching BOTH halves : %d" % ok)
    print("consumers exempted, with reason: %d" % len(exempt))
    for rel, why in exempt:
        print("   %s -- %s" % (rel, why))
    print("consumers BLIND to the host    : %d" % len(blind))
    for rel, why in blind:
        print("   %s" % rel)
        print("      %s" % why)

    if blind:
        print()
        print("FAIL: a tool that answers \"is any scrubbed identifier "
              "present?\" must load the whole inventory. Call "
              "check_scrub_regressions.load_all_needles(), or add")
        print("      # needle-inventory: exempt -- <reason>")
        print("      stating why that tool legitimately needs only a subset.")
        return 1
    print()
    print("OK: every scrub-needle consumer reaches the host needle too.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
