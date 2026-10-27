#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Assert every reachability-converted live-UI test file is cited by the RTM.

Why this exists
---------------
`phase-25-dispositions.md` recorded the finding that F25 converted 248
reachability sites across 37 files and exactly ONE requirement row in
`docs/audit/rtm-coverage.md` cited any of them:

    ... nothing in the RTM would have dimmed if the entire live-UI suite had
    started skipping every assertion.

F29 wires those files into the Test(s) column. This checker keeps them wired.

THE TRAP THIS CHECKER IS BUILT TO AVOID
---------------------------------------
A substring checker over `rtm-coverage.md` passes TODAY, before any citation is
added, because the prose paragraph that DOCUMENTS the gap names the very files
it says are uncited:

    `test_ui_annotations.py` (55 converted sites), `test_ui_redaction.py`
    (44), `test_ui_stress.py` (25) and `test_ui_forms.py` (18) are cited by
    **no requirement row at all**

`grep -n "test_ui_annotations.py" docs/audit/rtm-coverage.md` therefore returns
a hit and a naive checker reports the defect as fixed by the sentence describing
it. So this checker NEVER does substring presence over the document. It parses
the markdown tables structurally, keeps only tables whose header row is the RTM
requirement header (`| Requirement | Source | Test(s) | Class |`), and looks
only inside the `Test(s)` cell of those tables' body rows.

Two discriminating controls, not one
------------------------------------
The obvious control -- plant a new uncited `tests/test_ui_planted.py` and require
a FAIL -- does NOT catch the substring bug, because a planted file has no prose
mention either, so a substring checker fails on it too and looks correct. The
second control is the one that discriminates: a file mentioned ONLY in prose
must still be reported UNCITED. Both live in
`tools/gate/test_check_rtm_live_ui_citations.py`.

Scope
-----
"Reachability-converted" is derived mechanically, not from a hand-maintained
list: a file is in scope when it contains the `_reach = (` marker that F11/F25's
conversion produced and that `ReachabilitySkipGuard.ConvertedSitesStillRecord\
BlockedBeforeFailing` in `tests/test_doc_drift_guards.cpp` counts. Default glob
is `tests/test_ui_*.py`; `--recursive` widens it to `tests/**/test_ui_*.py`.

Usage
-----
    python tools/gate/check_rtm_live_ui_citations.py
    python tools/gate/check_rtm_live_ui_citations.py --recursive --json

Exit codes: 0 = every in-scope file is cited, 1 = at least one is not,
2 = usage error.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Set

RTM_RELPATH = "docs/audit/rtm-coverage.md"
CONVERSION_MARKER = "_reach = ("
REQUIREMENT_HEADER = ("requirement", "source", "test(s)", "class")
TEST_COLUMN = 2


def _read_text(path: Path) -> str:
    """Read a tracked text file with CRLF normalised.

    `core.autocrlf=true` in this repo: a tracked file is CRLF on disk in a fresh
    checkout and may be LF in the session that wrote it. Any line-shaped parsing
    that does not normalise passes for its author and fails for everyone else --
    hazard H3, the exact defect F20 found in F19DocClaims.
    """
    with open(path, "r", encoding="utf-8", newline="") as handle:
        return handle.read().replace("\r\n", "\n").replace("\r", "\n")


def _split_row(line: str) -> Optional[List[str]]:
    """Split a markdown table row into cells, or None if it is not a table row."""
    stripped = line.strip()
    if not stripped.startswith("|"):
        return None
    body = stripped[1:]
    if body.endswith("|"):
        body = body[:-1]
    return [cell.strip() for cell in body.split("|")]


def _is_separator(cells: Sequence[str]) -> bool:
    return bool(cells) and all(re.fullmatch(r":?-{2,}:?", c) for c in cells if c != "")


def extract_requirement_test_cells(markdown: str) -> List[str]:
    """Every `Test(s)` cell of every RTM requirement table.

    Structural, not textual: a table is in scope only when its HEADER row reads
    `| Requirement | Source | Test(s) | Class |`. Prose paragraphs contribute
    nothing, and neither do the document's other tables (the F25 re-read table's
    header is `| row | before | after | why |`, so its `after` column -- which
    quotes `test_ui_view_navigation.py` -- is correctly excluded).
    """
    cells: List[str] = []
    in_requirement_table = False
    expect_separator = False

    for line in markdown.split("\n"):
        row = _split_row(line)
        if row is None:
            in_requirement_table = False
            expect_separator = False
            continue

        normalised = tuple(c.strip().lower() for c in row)
        if normalised == REQUIREMENT_HEADER:
            in_requirement_table = True
            expect_separator = True
            continue

        if not in_requirement_table:
            continue

        if expect_separator:
            expect_separator = False
            if _is_separator(row):
                continue
            # A header immediately followed by a non-separator is not a table.
            in_requirement_table = False
            continue

        if _is_separator(row):
            continue
        if len(row) > TEST_COLUMN:
            cells.append(row[TEST_COLUMN])

    return cells


def cited_filenames(markdown: str) -> Set[str]:
    """Every `*.py` filename appearing in an RTM requirement row's Test(s) cell."""
    found: Set[str] = set()
    for cell in extract_requirement_test_cells(markdown):
        for match in re.finditer(r"[A-Za-z0-9_./\\-]+\.py", cell):
            found.add(Path(match.group(0).replace("\\", "/")).name)
    return found


def converted_live_ui_files(repo_root: Path, recursive: bool) -> List[Path]:
    """Live-UI test files carrying F11/F25 reachability-conversion sites."""
    tests_dir = repo_root / "tests"
    pattern = "**/test_ui_*.py" if recursive else "test_ui_*.py"
    out: List[Path] = []
    # `_tmp` is pytest.ini's --basetemp and `_runtime` is generated scratch; both
    # are already in pytest.ini's norecursedirs. Without this, running the gate
    # self-tests before this checker makes it report its own fixture files as
    # uncited live-UI tests. Found by demonstration, not by reasoning.
    excluded = {"__pycache__", "_tmp", "_runtime"}
    for candidate in sorted(tests_dir.glob(pattern)):
        if excluded & set(candidate.relative_to(tests_dir).parts):
            continue
        if CONVERSION_MARKER in _read_text(candidate):
            out.append(candidate)
    return out


def evaluate(repo_root: Path, recursive: bool) -> Dict[str, object]:
    rtm_path = repo_root / RTM_RELPATH
    if not rtm_path.exists():
        raise FileNotFoundError(str(rtm_path))
    markdown = _read_text(rtm_path)
    cited = cited_filenames(markdown)
    in_scope = converted_live_ui_files(repo_root, recursive)

    uncited = [
        p.relative_to(repo_root).as_posix() for p in in_scope if p.name not in cited
    ]
    return {
        "rtm": RTM_RELPATH,
        "requirement_rows_scanned": len(extract_requirement_test_cells(markdown)),
        "in_scope": [p.relative_to(repo_root).as_posix() for p in in_scope],
        "in_scope_count": len(in_scope),
        "uncited": uncited,
    }


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=".", help="repo root (default: .)")
    parser.add_argument(
        "--recursive",
        action="store_true",
        help="widen scope from tests/test_ui_*.py to tests/**/test_ui_*.py",
    )
    parser.add_argument("--json", action="store_true", dest="as_json")
    args = parser.parse_args(argv)

    repo_root = Path(args.root).resolve()
    try:
        result = evaluate(repo_root, args.recursive)
    except FileNotFoundError as exc:
        sys.stderr.write("missing: %s\n" % exc)
        return 2

    if args.as_json:
        sys.stdout.write(json.dumps(result, indent=2, ensure_ascii=False) + "\n")
    else:
        sys.stdout.write(
            "%s: %d requirement rows scanned; %d reachability-converted live-UI "
            "file(s) in scope; %d uncited\n"
            % (
                result["rtm"],
                result["requirement_rows_scanned"],
                result["in_scope_count"],
                len(result["uncited"]),  # type: ignore[arg-type]
            )
        )
        for path in result["uncited"]:  # type: ignore[union-attr]
            sys.stdout.write("  UNCITED  %s\n" % path)

    if result["uncited"]:
        sys.stdout.write(
            "\nFAIL: the file(s) above carry reachability-converted sites and are cited by\n"
            "no requirement row's Test(s) column in %s. That is the F25 finding: the\n"
            "largest body of end-to-end evidence PDFE has is disconnected from the\n"
            "requirements it exercises, so the RTM cannot see whether it is healthy.\n"
            "Cite each file against a row it GENUINELY exercises -- read the file first.\n"
            "A mention in prose does not count and is not detected by this checker.\n"
            % RTM_RELPATH
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
