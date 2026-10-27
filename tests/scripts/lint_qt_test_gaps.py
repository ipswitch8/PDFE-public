# -*- coding: utf-8 -*-
"""Pipeline a7c2e541 phase-16 testing-methodology lint: enforce that
every finding in tests/docs/qt_test_gap_audit.md is explicitly
resolved.

Background
----------
Pipeline f3b82c51 phase-1 audited the Qt-Test-using subset of the
suite for seven methodology anti-patterns (A-G; see audit doc header).
32 findings were recorded across 18 files. Each finding describes a
real testing-methodology weakness: vacuous-assertion-on-nullptr,
loose contains() with OR-fallback, signal-self-emit, etc.

This lint enforces that NO finding remains in an unresolved state.
Each numbered finding in the audit must be marked with one of:

    - **Status**: FIXED <git-ref-or-commit-or-PR>
    - **Status**: ALLOWLISTED -- <one-line justification>

Allowlist budget is capped at 5 entries (per phase-16 acceptance
criterion AC4). Any unresolved finding OR an allowlist that exceeds
the cap is a lint failure (exit 1).

Output format mirrors lint_save_without_verify.py.

Usage
-----
    python tests/scripts/lint_qt_test_gaps.py tests/

The script takes a path argument for parity with the other lints;
the path is used only to locate the audit doc (resolved as
<path>/docs/qt_test_gap_audit.md).

Phase-16 acceptance criterion AC2: this lint reports 0 findings and
exits 0.
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


# ─── Audit-doc parsing ────────────────────────────────────────────────

# Each numbered finding starts with `### NN. <file>:<lines> -- <name>`.
FINDING_HEADER_RE = re.compile(r"^###\s+(\d+)\.\s+(.+?)\s*$")

# Status line within a finding's body.
STATUS_RE = re.compile(
    r"^- \*\*Status\*\*:\s*(FIXED|ALLOWLISTED)\b\s*(.*?)\s*$"
)

ALLOWLIST_BUDGET = 5


@dataclass
class Finding:
    number: int
    header: str
    status: str | None = None  # "FIXED" or "ALLOWLISTED" or None
    status_detail: str = ""


def parse_audit(audit_path: Path) -> list[Finding]:
    text = audit_path.read_text(encoding="utf-8")
    lines = text.splitlines()
    findings: list[Finding] = []
    current: Finding | None = None
    for line in lines:
        m = FINDING_HEADER_RE.match(line)
        if m:
            if current is not None:
                findings.append(current)
            current = Finding(
                number=int(m.group(1)),
                header=m.group(2).strip(),
            )
            continue
        if current is not None:
            sm = STATUS_RE.match(line)
            if sm:
                current.status = sm.group(1)
                current.status_detail = sm.group(2).strip()
    if current is not None:
        findings.append(current)
    return findings


def format_unresolved(f: Finding) -> str:
    return (f"qt_test_gap_audit.md  #{f.number}  "
            f"{f.header[:120]}  status=UNRESOLVED  "
            f"hint=add '- **Status**: FIXED <ref>' or "
            f"'- **Status**: ALLOWLISTED -- <reason>' under the finding")


def main(argv: Iterable[str]) -> int:
    ap = argparse.ArgumentParser(
        description=("Enforce that every finding in "
                     "qt_test_gap_audit.md is FIXED or ALLOWLISTED."))
    ap.add_argument("paths", nargs="+", type=Path,
                    help="Tests root (used to locate docs/qt_test_gap_audit.md).")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(list(argv))

    audit_path = None
    for p in args.paths:
        candidate = p / "docs" / "qt_test_gap_audit.md"
        if candidate.exists():
            audit_path = candidate
            break
    if audit_path is None:
        print(f"ERROR: qt_test_gap_audit.md not found under any of "
              f"{[str(p) for p in args.paths]}", file=sys.stderr)
        return 2

    findings = parse_audit(audit_path)
    if not findings:
        print("ERROR: parsed 0 findings from audit doc — audit-doc "
              "format may have drifted from the lint regex.",
              file=sys.stderr)
        return 2

    unresolved = [f for f in findings if f.status is None]
    allowlisted = [f for f in findings if f.status == "ALLOWLISTED"]
    fixed = [f for f in findings if f.status == "FIXED"]

    failures = list(unresolved)

    over_budget = len(allowlisted) > ALLOWLIST_BUDGET

    if not args.quiet:
        for f in unresolved:
            print(format_unresolved(f))
        if over_budget:
            print(f"qt_test_gap_audit.md  allowlist size "
                  f"{len(allowlisted)} > budget {ALLOWLIST_BUDGET}")

    print(f"\n[lint_qt_test_gaps] {len(findings)} finding(s) total, "
          f"{len(fixed)} FIXED, {len(allowlisted)} ALLOWLISTED, "
          f"{len(unresolved)} UNRESOLVED")

    if failures or over_budget:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
