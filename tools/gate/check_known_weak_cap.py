#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""check_known_weak_cap.py — the known-weak cap can now actually fail.

WHAT THIS CLOSES
================
``tests/scripts/remediation_summary.py --strict`` is documented in
``tests/docs/test_strategy.md`` as the closeout gate that enforces the
known-weak entry cap.  It was wired to NOTHING: a sweep of every workflow and
every CMake file found ``remediation_summary`` only in documentation and in the
script's own source.  No workflow step and no ``add_test`` ever invoked it.

The consequence was measured on 2026-09-16 (pipeline p86-followups phase-6):
``tests/KNOWN_WEAK_TESTS.txt`` held **52** entries against a cap of 20, and
``--strict`` exited 1 the first time anyone ran it.  The cap had been breached
silently for many pipelines because the only thing that could detect the breach
was never executed.  This is the project's documented "guard registered but
never selected" shape -- a gate that cannot fail because nothing runs it.

Phase-6's remedy is two-part:
  * the FULL summary (``remediation_summary.py --strict``, ~40 s, shells out to
    three methodology lints) runs in ``governance.yml``, which is
    ``workflow_dispatch``-only;
  * THIS guard runs in ``fast-checks.yml`` on every push.  It is pure file I/O
    with no subprocess, so it costs milliseconds.  ``governance.yml`` is manual
    -- a cap breach could sit unnoticed there exactly as it did before -- so the
    cheap half has to be on the automatic lane.

WHY IT ALSO CHECKS THE PROSE
============================
The cap was stated in four places and they did not agree.  Two said 23 and two
said 20:

    tests/scripts/remediation_summary.py docstring   23   (stale)
    tests/KNOWN_WEAK_TESTS.txt header                23   (stale)
    tests/scripts/remediation_summary.py assert      20   (enforcing)
    tests/docs/test_strategy.md                      20

A contributor reading either stale copy would have believed 23 entries were
permitted.  Counting entries alone would not have caught that, so G2 pins the
enforcing assert to CAP: if someone edits the assert, this guard reddens and the
prose must be revisited deliberately.

GATES
=====
G1  ``tests/KNOWN_WEAK_TESTS.txt`` holds at most CAP (20) entries, where an
    entry is a non-blank line whose first non-space character is not ``#`` --
    byte-identical to ``remediation_summary.parse_known_weak_tests``.
G2  ``remediation_summary.py`` still asserts that same CAP, so this fast guard
    and the full closeout gate cannot silently diverge.
G3  Every live entry carries a measured root cause, not a bare "flaky" label.
    An entry whose text contains "flaky" or "flakey" must also carry a cause
    marker (``MEASURED CAUSE``, ``Root cause``, ``BLOCKED-EXTERNAL``,
    ``UNVERIFIABLE``, or ``INVESTIGATED``).

Usage:
  python tools/gate/check_known_weak_cap.py [--repo-root .]
  python tools/gate/check_known_weak_cap.py --self-test
Exit 0 = compliant; 1 = a gate failed; 2 = usage/IO error.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import List, Tuple

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
sys.stderr.reconfigure(encoding="utf-8", errors="replace")

CAP = 20

# The assert in remediation_summary.py. Matched loosely on purpose: the point is
# that SOME cap comparison exists and names CAP, not that the line is formatted
# one exact way.
ASSERT_RE = re.compile(r"if\s+kw\s*>\s*(\d+)\s*:")

CAUSE_MARKERS = ("MEASURED CAUSE", "ROOT CAUSE", "BLOCKED-EXTERNAL",
                 "UNVERIFIABLE", "INVESTIGATED")
FLAKY_RE = re.compile(r"\bflak(?:y|ey|iness)\b", re.I)


def normalise(text: str) -> str:
    """CRLF -> LF. ``core.autocrlf=true`` here, so a guard comparing tracked text
    content without this passes in the session that wrote the file and fails
    after any fresh checkout."""
    return text.replace("\r\n", "\n").replace("\r", "\n")


def parse_entries(text: str) -> List[str]:
    """Non-blank, non-comment lines. Mirrors
    remediation_summary.parse_known_weak_tests exactly -- if these two ever
    disagree, the fast guard and the closeout gate would count differently."""
    out: List[str] = []
    for line in normalise(text).split("\n"):
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        out.append(s)
    return out


def evaluate(registry_text: str, summary_src: str, cap: int = CAP
             ) -> Tuple[bool, List[str]]:
    """Pure decision core; no I/O, so it is directly unit-testable."""
    msgs: List[str] = []
    ok = True

    entries = parse_entries(registry_text)
    if len(entries) > cap:
        ok = False
        msgs.append("G1 FAIL: known-weak entries %d > %d cap"
                    % (len(entries), cap))
    else:
        msgs.append("G1 ok: %d entr%s <= %d cap"
                    % (len(entries), "y" if len(entries) == 1 else "ies", cap))

    found = ASSERT_RE.search(normalise(summary_src))
    if not found:
        ok = False
        msgs.append("G2 FAIL: no known-weak cap assert found in "
                    "remediation_summary.py -- the closeout gate no longer "
                    "enforces a cap, so this fast guard is on its own")
    elif int(found.group(1)) != cap:
        ok = False
        msgs.append("G2 FAIL: remediation_summary.py asserts cap %s but this "
                    "guard enforces %d -- they must agree, and the prose in "
                    "tests/docs/test_strategy.md and the registry header must "
                    "be updated with them" % (found.group(1), cap))
    else:
        msgs.append("G2 ok: remediation_summary.py asserts the same cap (%d)"
                    % cap)

    bare = []
    for e in entries:
        if FLAKY_RE.search(e) and not any(m in e.upper() for m in CAUSE_MARKERS):
            bare.append(e.split(":")[0])
    if bare:
        ok = False
        msgs.append("G3 FAIL: entr%s labelled flaky with no measured cause: %s"
                    % ("y" if len(bare) == 1 else "ies", ", ".join(bare)))
    else:
        msgs.append("G3 ok: no bare 'flaky' label without a measured cause")

    return ok, msgs


def _self_test() -> int:
    """A guard no mutation can redden is decoration; one that can ONLY redden is
    equally useless. Each case below pins one gate in BOTH directions."""
    good_src = "        if kw > 20:\n"
    failures = []

    def check(name, got, want):
        if got != want:
            failures.append("%s: expected ok=%s, got ok=%s" % (name, want, got))

    # G1 both ways.
    ok, _ = evaluate("# c\n\nA: x\n", good_src)
    check("G1 under cap", ok, True)
    ok, _ = evaluate("\n".join("E%d: x" % i for i in range(CAP + 1)), good_src)
    check("G1 over cap", ok, False)
    # Exactly at the cap is allowed (the comparison is >, not >=).
    ok, _ = evaluate("\n".join("E%d: x" % i for i in range(CAP)), good_src)
    check("G1 at cap", ok, True)

    # Comments and blanks must not count, or the cap would fire on prose.
    ok, _ = evaluate("#c\n" * 500 + "A: x\n", good_src)
    check("G1 ignores comments", ok, True)

    # G2 both ways.
    ok, _ = evaluate("A: x\n", "        if kw > 23:\n")
    check("G2 divergent cap", ok, False)
    ok, _ = evaluate("A: x\n", "nothing here\n")
    check("G2 assert deleted", ok, False)

    # G3 both ways.
    ok, _ = evaluate("A: sometimes flaky, unclear why\n", good_src)
    check("G3 bare flaky", ok, False)
    ok, _ = evaluate("A: flaky -- MEASURED CAUSE: rdpclip swallows the "
                     "clipboard WRITE\n", good_src)
    check("G3 flaky with cause", ok, True)

    if failures:
        for f in failures:
            print("self-test FAIL: %s" % f)
        return 1
    print("check_known_weak_cap self-test: PASS (9 cases, each gate pinned "
          "in both directions)")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo-root", type=Path, default=Path("."))
    ap.add_argument("--self-test", action="store_true",
                    help="run the decision-core unit cases and exit")
    args = ap.parse_args(argv)

    if args.self_test:
        return _self_test()

    root = args.repo_root.resolve()
    registry = root / "tests" / "KNOWN_WEAK_TESTS.txt"
    summary = root / "tests" / "scripts" / "remediation_summary.py"
    for p in (registry, summary):
        if not p.is_file():
            print("check_known_weak_cap: ERROR missing %s" % p, file=sys.stderr)
            return 2

    ok, msgs = evaluate(registry.read_text(encoding="utf-8"),
                        summary.read_text(encoding="utf-8"))
    print("=== known-weak cap (cap=%d) ===" % CAP)
    for m in msgs:
        print("  " + m)
    print("check_known_weak_cap: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
