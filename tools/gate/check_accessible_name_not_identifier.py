#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Fail when a widget's ``accessibleName`` is its ``objectName`` (GitHub #185).

WHY THIS EXISTS
---------------
``objectName`` and ``accessibleName`` are two different properties with two
different audiences:

* ``objectName`` is the AUTOMATION identity. Qt's Windows UIA bridge exposes it
  as ``AutomationId``; pywinauto reads it as ``auto_id``. It is never spoken.
* ``accessibleName`` is what assistive technology SPEAKS. It should be the same
  text a sighted user reads.

Thirteen sites across seven files had set them to the same developer
identifier, so a screen-reader user heard ``"ComplianceDialog"`` where a sighted
user read "PDF Standards Compliance", and ``"batchProgressBar"`` for a progress
bar. That is not a style nit: ``accessibleName`` OVERRIDES the window title in
the announcement, so the good title one line earlier was being suppressed by the
bad accessible name one line later.

It happened because the project's automation guidance asks for stable
identifiers on widgets and these sites satisfied it with the wrong property --
several with a source comment asserting that Qt's bridge took ``AutomationId``
from ``accessibleName``. It does not; the live-UI suite has always located
widgets whose two properties DIFFER (``auto_id="AnalyzeButton"`` against
``accessibleName("Analyze")``), and
``tests/test_ui_accessible_names.py`` re-measures that against the running app.

A one-time correction does not stop the next one arriving, and the shape is
mechanically detectable, so it is checked rather than remembered.

WHAT IS FLAGGED
---------------
A ``setAccessibleName("X")`` whose literal is exactly the ``"X"`` of a
``setObjectName("X")`` applied to the same receiver within a short window of
lines. Both the receiver expression and the literal must match, so two unrelated
widgets that happen to sit near each other cannot collide.

Deliberately narrow. It does NOT flag:

* an identifier-looking accessible name that has no matching ``setObjectName``
  (e.g. the ``atlas-comparison-*`` lookup keys in AltTextDialog's sibling
  dialogs) -- that is a different defect with a different fix, and guessing at
  "looks like an identifier" is how a guard starts reddening correct code;
* ``setAccessibleName(tr("X"))`` -- a translated string is by construction
  user-facing text, even if the English happens to equal the objectName;
* anything under ``tests/``, which sets both properties on throwaway fixtures.

ESCAPE HATCH: a ``// accessible-name-lint: exempt -- <reason>`` comment on the
``setAccessibleName`` line or the line above it. It must carry a reason; an
empty one does not count.

Exit codes: 0 = clean, 1 = violations found, 2 = bad usage.
Self-tests: ``python tools/gate/check_accessible_name_not_identifier.py --self-test``
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import List, NamedTuple

REPO_ROOT = Path(__file__).resolve().parents[2]

SCAN_DIRS = ("src",)
SCAN_SUFFIXES = (".cpp", ".cxx", ".cc")

# How far back from a setAccessibleName we look for the matching setObjectName.
# The observed sites put them 1-9 lines apart (a comment block sits between them
# in four cases). 12 covers that with margin and is still far too short to pair
# widgets from different construction blocks.
LOOKBACK_LINES = 12

_OBJECT_NAME_RE = re.compile(
    r'^\s*(?P<recv>[A-Za-z_][A-Za-z0-9_\->\.\(\)]*)?'
    r'(?:->|\.)?setObjectName\(\s*"(?P<lit>[^"]*)"\s*\)\s*;')
_ACCESSIBLE_NAME_RE = re.compile(
    r'^\s*(?P<recv>[A-Za-z_][A-Za-z0-9_\->\.\(\)]*)?'
    r'(?:->|\.)?setAccessibleName\(\s*"(?P<lit>[^"]*)"\s*\)\s*;')
_EXEMPT_RE = re.compile(r'//\s*accessible-name-lint:\s*exempt\s*--\s*(?P<why>\S.*)')


class Violation(NamedTuple):
    path: str
    line: int
    receiver: str
    literal: str
    object_name_line: int


def _receiver_of(match: "re.Match[str]") -> str:
    """The widget the call is applied to; "" for a bare `setX(...)` on `this`."""
    return (match.group("recv") or "").strip()


def scan_text(text: str, rel_path: str) -> List[Violation]:
    lines = text.splitlines()
    out: List[Violation] = []
    for i, line in enumerate(lines):
        acc = _ACCESSIBLE_NAME_RE.match(line)
        if not acc:
            continue

        exempt = _EXEMPT_RE.search(line)
        if not exempt and i > 0:
            exempt = _EXEMPT_RE.search(lines[i - 1])
        if exempt:
            continue

        recv = _receiver_of(acc)
        literal = acc.group("lit")

        lo = max(0, i - LOOKBACK_LINES)
        for j in range(i - 1, lo - 1, -1):
            obj = _OBJECT_NAME_RE.match(lines[j])
            if not obj:
                continue
            if _receiver_of(obj) != recv:
                continue
            if obj.group("lit") == literal:
                out.append(Violation(rel_path, i + 1, recv or "this",
                                     literal, j + 1))
            # First setObjectName on this receiver decides; a second, further
            # back, belongs to an earlier widget of the same name.
            break
    return out


def scan_repo(repo_root: Path) -> List[Violation]:
    out: List[Violation] = []
    for d in SCAN_DIRS:
        base = repo_root / d
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix.lower() not in SCAN_SUFFIXES:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            rel = path.relative_to(repo_root).as_posix()
            out.extend(scan_text(text, rel))
    return out


# ---------------------------------------------------------------------------
# Self-tests -- the guard must be shown to RED as well as to GREEN. A check
# that can only ever pass is decoration.
# ---------------------------------------------------------------------------

_SELF_TESTS = [
    # (name, source, expected violation count)
    ("the #185 shape is caught", '''
    progressBar_ = new QProgressBar(this);
    progressBar_->setObjectName("batchProgressBar");
    progressBar_->setAccessibleName("batchProgressBar");
''', 1),
    ("a comment block between the two does not hide it", '''
    fileList_->setObjectName("fileList");
    // some
    // intervening
    // rationale
    fileList_->setAccessibleName("fileList");
''', 1),
    ("the corrected form is clean", '''
    progressBar_->setObjectName("batchProgressBar");
    progressBar_->setAccessibleName(tr("Batch progress"));
''', 0),
    ("a differing literal is clean", '''
    analyzeButton_->setObjectName("AnalyzeButton");
    analyzeButton_->setAccessibleName("Analyze");
''', 0),
    ("this-receiver (a dialog) is caught", '''
    setObjectName("ComplianceDialog");
    setAccessibleName("ComplianceDialog");
''', 1),
    ("two different widgets with matching-but-crossed names are clean", '''
    a_->setObjectName("alpha");
    b_->setAccessibleName("alpha");
''', 0),
    ("an accessibleName with no objectName at all is not our business", '''
    label->setAccessibleName("atlas-comparison-font-charset");
''', 0),
    ("a reasoned exemption is honoured", '''
    w_->setObjectName("keepme");
    // accessible-name-lint: exempt -- deliberate, see GitHub #999
    w_->setAccessibleName("keepme");
''', 0),
    ("an exemption with no reason is NOT honoured", '''
    w_->setObjectName("keepme");
    // accessible-name-lint: exempt --
    w_->setAccessibleName("keepme");
''', 1),
    ("distance beyond the lookback window is not paired", '''
    w_->setObjectName("faraway");
''' + "    // filler\n" * 15 + '''    w_->setAccessibleName("faraway");
''', 0),
]


def run_self_tests() -> int:
    failures = 0
    for name, src, expected in _SELF_TESTS:
        got = len(scan_text(src, "<self-test>"))
        status = "ok  " if got == expected else "FAIL"
        if got != expected:
            failures += 1
        print(f"  [{status}] {name}: expected {expected}, got {got}")
    if failures:
        print(f"\nself-test: {failures} case(s) FAILED")
        return 1
    print(f"\nself-test: all {len(_SELF_TESTS)} cases passed")
    return 0


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo-root", default=str(REPO_ROOT))
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return run_self_tests()

    root = Path(args.repo_root).resolve()
    violations = scan_repo(root)
    if not violations:
        print("accessible-name lint: clean "
              "(no setAccessibleName literal equals its widget's objectName)")
        return 0

    print("accessible-name lint: FAILED (GitHub #185)\n")
    for v in violations:
        print(f"  {v.path}:{v.line}: setAccessibleName(\"{v.literal}\") on "
              f"`{v.receiver}` repeats the objectName set at line "
              f"{v.object_name_line}.")
    print(f"\n{len(violations)} site(s). accessibleName is SPOKEN by a screen "
          "reader; objectName is the automation id read as pywinauto "
          "`auto_id`. Set accessibleName to the text a sighted user sees "
          "(usually via tr(...)) and leave objectName alone -- no automation "
          "lookup depends on the two being equal.")
    return 1


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main(sys.argv[1:]))
