#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_tracking_banners.py — every document that reads as a defect/work list
must say which list is canonical.

WHY THIS EXISTS
---------------
Until 2026-09-02 this repo had no declared source of truth for defects and about
two dozen documents that could each reasonably be mistaken for one: an
458-row ``gap-register.md``, a 392-row ``status-register.md``, ~52
``phase-NN-dispositions.md``, a second doc tree under ``tests/docs/``, and more.
Eleven ID namespaces, ~1,100 ids, and five GitHub cross-references in the whole
tree. ``docs/TRACKING.md`` fixed the ambiguity by declaring GitHub Issues
canonical for open work and banding every competing document.

Banding is a one-time act; drift is continuous. A tracker doc added next month
gets no banner, and the ambiguity grows back. This check is the ratchet.

WHAT IT ENFORCES
----------------
A markdown/text file under ``docs/`` or ``tests/`` that READS AS A WORK LIST must
carry either a tracking banner or an explicit, reasoned exemption.

"Reads as a work list" is scored, not guessed:

  * density of status tokens (``OPEN``, ``DEFERRED``, ``UNCOVERED``,
    ``HAPPY-PATH-ONLY``, ``TODO``, ``remaining work``, …), plus
  * table-row count / 5, capped — a long table of statuses is the shape that
    misleads, and

  * records of a SINGLE past investigation (evidence, closure, findings,
    dispositions, report, runbook) are discounted 3x, because "here is what I
    found on Tuesday" is not a work list even when it contains the word OPEN.

Below ``--threshold`` (default 12) a file is not confusable enough to require a
banner. The threshold is deliberately loose: a guard that reddens on correct
documents is worse than no guard, because it trains people to bypass it.

ESCAPE HATCH, auditable and in-file
-----------------------------------
Put this line anywhere in the document::

    <!-- tracking-banner: exempt -- <reason> -->

The reason is required and must be non-empty; an exemption without one fails the
same as a missing banner. It lives in the file rather than in a side list so it
travels with the document and shows up in review.

Pure decision logic (``score_text``, ``classify``) takes text and a path and does
no I/O, so it is directly unit-testable — see
tools/gate/test_check_tracking_banners.py.

Usage:
  python tools/gate/check_tracking_banners.py [--repo-root .] [--threshold 12]
Exit 0 = every confusable document is labelled; 1 = at least one is not.
"""
from __future__ import annotations

import argparse
import os
import re
import sys

BANNER_MARK = "<!-- tracking-banner -->"
# The plain-text registries (KNOWN_WEAK_TESTS.txt) cannot carry an HTML comment
# and say it in prose instead.
BANNER_PROSE = "canonical tracker"
EXEMPT_RE = re.compile(r"<!--\s*tracking-banner:\s*exempt\s*--\s*(.+?)\s*-->",
                       re.I | re.S)

STATUS_RE = re.compile(
    r"\b(OPEN|STALE-OPEN|DEFERRED|UNVERIFIABLE|TODO|FIXME|UNCOVERED|"
    r"NOT COVERED|HAPPY-PATH-ONLY|PENDING|outstanding|remaining work|"
    r"still needed|not yet implemented)\b")
TABLE_ROW_RE = re.compile(r"^\|", re.M)

# A record of one investigation is not a work list, even when it says OPEN.
#
# "dispositions" was in this list and had to be REMOVED. An ad-hoc review
# demonstrated the hole: a synthetic phase-90-dispositions.md shaped like the
# real ones scored 10 after the discount and passed unbanded, and 21 of the 70
# real phase-NN-dispositions.md files score >=12 raw but land under the
# threshold only because of it. They carry banners today only because they were
# swept by hand -- so for the single LARGEST document class in the repo this
# guard was not a ratchet at all, which is the one thing its docstring promises.
# A discount that exempts the biggest confusable population is not a
# false-positive control, it is a hole.
SINGLE_RECORD_RE = re.compile(
    r"(mutation-evidence|-evidence-|closure|findings|report|"
    r"summary|record|runbook|architecture|setup|guide|methodology|plan-|"
    r"CHANGELOG|LICENSE|NOTICE)", re.I)

# "_tmp" and "_runtime" are pytest's basetemp and the gtest shard output. The
# basetemp is rooted at tests/_tmp/s<pid> by the repo-root conftest.py, one
# directory per session (GitHub #257); pytest.ini pinned every session to a
# single shared tests/_tmp until then. Both live INSIDE the repo and both hold
# throwaway fixtures that trip the scorer -- this guard flagged its own test
# leftovers the first time it was run for real, which is precisely the
# reddens-on-correct-state failure that makes people bypass a guard.
SKIP_DIRS = {".git", "build", "node_modules", "__pycache__", ".claude",
             ".vs", "vcpkg_installed", "_runtime", "_tmp", "out", "external"}


def normalise(text: str) -> str:
    """CRLF -> LF. ``core.autocrlf=true`` here, so a guard that compares tracked
    text content without this passes in the session that wrote the file and
    fails after any fresh checkout."""
    return text.replace("\r\n", "\n").replace("\r", "\n")


def score_text(text: str, relpath: str) -> int:
    """How much this document reads like a work list. Pure; no I/O."""
    text = normalise(text)
    status_hits = len(STATUS_RE.findall(text))
    if status_hits == 0:
        return 0
    table_rows = min(len(TABLE_ROW_RE.findall(text)), 200)
    score = status_hits + table_rows // 5
    if SINGLE_RECORD_RE.search(relpath.replace("\\", "/")):
        score //= 3
    return score


def classify(text: str, relpath: str, threshold: int) -> tuple[str, int]:
    """Return (verdict, score). Verdict is one of:
       'ok'      — banded, exempt with a reason, or not confusable
       'missing' — confusable and unlabelled
       'blank-exemption' — claims an exemption but gives no reason
    Pure; no I/O."""
    text_n = normalise(text)
    score = score_text(text_n, relpath)

    m = EXEMPT_RE.search(text_n)
    if m:
        return ("ok" if m.group(1).strip() else "blank-exemption", score)
    if BANNER_MARK in text_n or BANNER_PROSE in text_n:
        return ("ok", score)
    if score < threshold:
        return ("ok", score)
    return ("missing", score)


def iter_candidates(repo_root: str):
    for base in ("docs", "tests"):
        root = os.path.join(repo_root, base)
        if not os.path.isdir(root):
            continue
        for dirpath, dirs, files in os.walk(root):
            dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
            for f in files:
                if f.endswith(".md") or f.endswith(".txt"):
                    full = os.path.join(dirpath, f)
                    yield full, os.path.relpath(full, repo_root)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo-root", default=".")
    ap.add_argument("--threshold", type=int, default=12)
    ap.add_argument("--list-scores", action="store_true",
                    help="print every scored file, for tuning the threshold")
    args = ap.parse_args(argv)

    offenders, blank, scored = [], [], []
    for full, rel in iter_candidates(args.repo_root):
        try:
            text = open(full, encoding="utf-8", errors="ignore").read()
        except OSError:
            continue
        verdict, score = classify(text, rel, args.threshold)
        if score:
            scored.append((score, rel))
        if verdict == "missing":
            offenders.append((score, rel))
        elif verdict == "blank-exemption":
            blank.append((score, rel))

    if args.list_scores:
        for s, rel in sorted(scored, reverse=True)[:60]:
            print("%5d  %s" % (s, rel.replace("\\", "/")))

    if not offenders and not blank:
        print("check_tracking_banners: OK — every confusable document is labelled "
              "(%d scored, threshold %d)" % (len(scored), args.threshold))
        return 0

    if offenders:
        print("check_tracking_banners: FAIL — %d document(s) read as a work list "
              "but say nothing about which list is canonical:\n" % len(offenders))
        for s, rel in sorted(offenders, reverse=True):
            print("    score %-4d %s" % (s, rel.replace("\\", "/")))
    if blank:
        print("\ncheck_tracking_banners: FAIL — %d exemption(s) with no reason:\n"
              % len(blank))
        for s, rel in sorted(blank, reverse=True):
            print("    score %-4d %s" % (s, rel.replace("\\", "/")))

    print("""
Fix by adding ONE of the following to the top of each file.

  Historical evidence (an audit record; most documents):

    <!-- tracking-banner -->
    > **STATUS: HISTORICAL EVIDENCE — this is not a work list.**
    >
    > **GitHub Issues (`ipswitch8/PDFE`) is the canonical tracker for anything
    > still open.** Where this document and a GitHub issue disagree, the issue
    > wins. See [`docs/TRACKING.md`](docs/TRACKING.md).

  Live operational state (something CI, a generator or a drift guard READS):

    <!-- tracking-banner -->
    > **STATUS: LIVE OPERATIONAL STATE — keep maintaining this file.**
    >
    > Not a defect tracker and not superseded by GitHub Issues; a defect
    > recorded only here is still untracked. See [`docs/TRACKING.md`](docs/TRACKING.md).

  Genuinely neither, and the score is a false positive:

    <!-- tracking-banner: exempt -- why this is not a work list -->

Background: docs/TRACKING.md.""")
    return 1


if __name__ == "__main__":
    sys.exit(main())
