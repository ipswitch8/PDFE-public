#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_dca_presence.py — require a DCA record for defect-fix commits.

Pipeline e5b9c231 Phase 5, criterion 4 (closing the karen-flagged gap): the
companion check_dca.py validates DCA records that ALREADY exist, but nothing
fails a defect-fix commit that adds ZERO DCA records — so a future fix could skip
the escaped-defect -> DCA -> class-test loop entirely. This check closes that:
over a git range, if a commit reads as a *defect fix* AND touches product/oracle
logic, the range must add or modify at least one DCA record under
``docs/audit/dca/``.

Trigger is deliberately tight to avoid false positives on feature work:

  * the commit message matches a defect marker (fix/bug/defect/regression/
    hotfix/escaped), AND
  * the diff touches ``src/`` or a ``tests/*.cpp|*.py`` oracle.

Author escape hatch: put ``[skip-dca]`` in the commit message (ideally with a
one-line reason) for a legitimate non-defect "fix" (e.g. "fix: rename var
[skip-dca] cosmetic"). The default is enforcement; the escape hatch is explicit
and auditable in the git log.

Pure decision logic lives in ``requires_dca`` / ``has_dca`` (no git, no I/O) so
it is directly unit-testable — see tools/gate/test_check_dca_presence.py.

Usage:
  python tools/gate/check_dca_presence.py --base <sha> --head <sha>
  python tools/gate/check_dca_presence.py --base origin/master --head HEAD
Exit 0 = compliant (or not a defect-fix range); 1 = defect fix without a DCA;
2 = usage/git error.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from typing import List

DEFECT_MARKER = re.compile(r"\b(fix|fixes|fixed|bug|defect|regression|hotfix|escaped)\b",
                           re.IGNORECASE)
SKIP_TOKEN = "[skip-dca]"

_DCA_PREFIX = "docs/audit/dca/"
_DCA_EXCLUDED = {"README.md", "TEMPLATE.md"}


def _touches_logic(changed_files: List[str]) -> bool:
    """True if the diff touches product source or a test oracle."""
    for f in changed_files:
        f = f.replace("\\", "/")
        if f.startswith("src/"):
            return True
        if f.startswith("tests/") and (f.endswith(".cpp") or f.endswith(".py")):
            return True
    return False


def has_dca(changed_files: List[str]) -> bool:
    """True if the diff adds/modifies a real DCA record (not README/TEMPLATE)."""
    for f in changed_files:
        f = f.replace("\\", "/")
        if not f.startswith(_DCA_PREFIX) or not f.endswith(".md"):
            continue
        if f.rsplit("/", 1)[-1] in _DCA_EXCLUDED:
            continue
        return True
    return False


def requires_dca(commit_messages: List[str], changed_files: List[str]) -> bool:
    """Does this range require a DCA record?

    True iff some commit message reads as a defect fix (and is NOT escape-hatched
    with ``[skip-dca]``) AND the range touches product/oracle logic.
    """
    defect_fix = any(
        DEFECT_MARKER.search(m or "") and SKIP_TOKEN not in (m or "")
        for m in commit_messages
    )
    return defect_fix and _touches_logic(changed_files)


def _git(args: List[str]) -> str:
    proc = subprocess.run(["git"] + args, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)} failed: {proc.stderr.strip()}")
    return proc.stdout


def main(argv: List[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", required=True, help="Base ref (exclusive).")
    ap.add_argument("--head", default="HEAD", help="Head ref (inclusive).")
    args = ap.parse_args(argv)

    rng = f"{args.base}..{args.head}"
    try:
        # One message per commit (%B, blank-line separated, %x1e record sep).
        raw = _git(["log", "--format=%B%x1e", rng])
        messages = [m.strip() for m in raw.split("\x1e") if m.strip()]
        changed = [f for f in _git(["diff", "--name-only", rng]).splitlines() if f.strip()]
    except RuntimeError as exc:
        print(f"[check-dca-presence] {exc}", file=sys.stderr)
        return 2

    if not requires_dca(messages, changed):
        print(f"[check-dca-presence] OK: {rng} is not a defect-fix touching logic "
              f"(or is [skip-dca]-tagged) — no DCA required.")
        return 0

    if has_dca(changed):
        print(f"[check-dca-presence] OK: defect-fix range {rng} adds/updates a DCA "
              f"record.")
        return 0

    print(f"[check-dca-presence] FAIL: {rng} looks like a defect fix touching "
          f"product/oracle logic but adds no DCA record under {_DCA_PREFIX}.")
    print("  Add a Defect Causal Analysis record (see docs/audit/dca/README.md), "
          "or tag a non-defect fix with [skip-dca] and a one-line reason.")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
