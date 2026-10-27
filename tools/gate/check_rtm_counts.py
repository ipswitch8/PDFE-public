#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""GitHub #208 ratchet — rtm-coverage.md's Counts block must match its own table.

Why this exists
---------------
`docs/audit/rtm-coverage.md` carries a `## Counts` block summarising the
requirements matrix that sits above it in the same file. Those two drifted for
roughly fourteen months: the block read ~72 rows with 1 UNCOVERED while the
matrix beneath it said 77 with 0. Nothing noticed, because nothing compared
them.

Phase-5 of pipeline p87-open-issues re-derived the block by hand-running a
parser once. That corrected the numbers and guarded nothing — the next edit
would desynchronise them again, which is precisely the failure #208 was filed
about. The phase-5 `claim-check` gate failed the phase for exactly that gap:
DCA 0080 listed a "programmatic check" as a standing control when only a
one-time derivation had been run. This file is that control.

The parsing rule, and the trap in it
------------------------------------
Take the `## RTM — requirement → test → coverage class` section, **from that
heading to the next `## ` heading** — located by text, never by line number,
because any edit above the matrix shifts every line citation in the file. Take
every table row that is not the header or the `|---|` separator, and classify
its **last** cell in the order:

    HAPPY-PATH-ONLY → UNCOVERED → N/A → COVERED

**The order is load-bearing.** `HAPPY-PATH-ONLY` and `UNCOVERED` both contain
the substring `COVERED`, so a naive `"COVERED" in cell` misclassifies all eight
non-covered rows as covered — and lands on a total that looks plausible. That
is how the drift stayed invisible.

Exit 0 = the block matches the table. Exit 1 = they disagree, or the document's
shape changed enough that the derivation cannot be performed (which fails loud
rather than silently counting zero).
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
RTM = Path("docs/audit/rtm-coverage.md")

MATRIX_HEADING_HINT = "RTM"
COUNTS_HEADING = "## Counts"

# Order matters -- see the module docstring.
CLASS_ORDER = ["HAPPY-PATH-ONLY", "UNCOVERED", "N/A", "COVERED"]


def classify(cell: str) -> str | None:
    """Return the coverage class named in `cell`, or None."""
    up = cell.upper()
    for name in CLASS_ORDER:
        if name in up:
            return name
    return None


def section(text: str, heading_predicate) -> str | None:
    """Return the body of the first `## ` section whose heading matches."""
    lines = text.splitlines()
    start = None
    for i, line in enumerate(lines):
        if line.startswith("## ") and heading_predicate(line):
            start = i + 1
            break
    if start is None:
        return None
    for j in range(start, len(lines)):
        if lines[j].startswith("## "):
            return "\n".join(lines[start:j])
    return "\n".join(lines[start:])


def derive_from_matrix(text: str) -> dict[str, int] | None:
    body = section(
        text,
        lambda h: MATRIX_HEADING_HINT in h and "coverage class" in h.lower(),
    )
    if body is None:
        return None
    counts = {name: 0 for name in CLASS_ORDER}
    total = 0
    for line in body.splitlines():
        s = line.strip()
        if not s.startswith("|"):
            continue
        if set(s) <= set("|- :"):          # the |---|---| separator
            continue
        cells = [c.strip() for c in s.strip("|").split("|")]
        if not cells:
            continue
        kind = classify(cells[-1])
        if kind is None:                    # header row, or a prose row
            continue
        counts[kind] += 1
        total += 1
    counts["TOTAL"] = total
    return counts


def read_counts_block(text: str) -> dict[str, int] | None:
    body = section(text, lambda h: h.strip() == COUNTS_HEADING)
    if body is None:
        return None
    stated: dict[str, int] = {}
    for line in body.splitlines():
        s = line.strip()
        if not s.startswith("|"):
            continue
        cells = [c.strip().strip("*").strip() for c in s.strip("|").split("|")]
        if len(cells) < 2:
            continue
        label, value = cells[0], cells[1]
        if not re.fullmatch(r"\d+", value):
            continue
        if label.upper().startswith("TOTAL"):
            stated["TOTAL"] = int(value)
        else:
            kind = classify(label)
            if kind is not None:
                stated[kind] = int(value)
    return stated or None


def check(repo_root: Path) -> int:
    path = repo_root / RTM
    if not path.is_file():
        print(f"[rtm-counts] FAIL: {RTM.as_posix()} does not exist")
        return 1
    text = path.read_text(encoding="utf-8", errors="replace")

    derived = derive_from_matrix(text)
    if derived is None:
        print("[rtm-counts] FAIL: could not locate the RTM matrix section. "
              "The derivation is anchored on the heading text, so a renamed "
              "heading fails loud here rather than silently counting zero.")
        return 1
    if derived["TOTAL"] == 0:
        print("[rtm-counts] FAIL: the matrix section parsed to ZERO rows. "
              "A count of nothing must never read as agreement.")
        return 1

    stated = read_counts_block(text)
    if stated is None:
        print(f"[rtm-counts] FAIL: no '{COUNTS_HEADING}' table found")
        return 1

    problems = []
    for key in CLASS_ORDER + ["TOTAL"]:
        if key not in stated:
            problems.append(f"the Counts block does not state {key}")
        elif stated[key] != derived[key]:
            problems.append(
                f"{key}: block says {stated[key]}, the matrix has {derived[key]}"
            )

    if problems:
        print("[rtm-counts] FAIL: the Counts block disagrees with the matrix "
              "above it -- the GitHub #208 drift has returned.")
        for p in problems:
            print(f"  - {p}")
        print()
        print("  Re-derive rather than hand-tally: hand-tallying is what "
              "produced the superseded block.")
        return 1

    print("[rtm-counts] PASS: "
          + " / ".join(f"{k}={derived[k]}" for k in CLASS_ORDER)
          + f" / total={derived['TOTAL']}, matching the Counts block.")
    return 0


# ---------------------------------------------------------------------------
# Self-test. A guard nobody has proved can fail is decoration, and this one has
# a specific way of being wrong: get the classification order backwards and it
# reports every row COVERED, which is a plausible-looking number.
# ---------------------------------------------------------------------------

_DOC = """\
# RTM

## RTM — requirement → test → coverage class

| Requirement | Test | Class |
|---|---|---|
| A | t | COVERED |
| B | t | COVERED |
| C | t | HAPPY-PATH-ONLY |
| D | - | UNCOVERED |
| E | - | N/A |

## Counts

| Class | Rows |
|---|---|
| **COVERED** | **2** |
| **HAPPY-PATH-ONLY** | **1** |
| **N/A** | **1** |
| **UNCOVERED** | **1** |
| **Total data rows** | **5** |
"""


def _self_test() -> int:
    import tempfile

    def run(doc: str) -> int:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            p = root / RTM
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(doc, encoding="utf-8")
            return check(root)

    cases = [
        ("control: block matches the matrix", _DOC, 0),
        ("a count is wrong",
         _DOC.replace("| **COVERED** | **2** |", "| **COVERED** | **3** |"), 1),
        ("the total is wrong",
         _DOC.replace("**Total data rows** | **5**",
                      "**Total data rows** | **6**"), 1),
        ("a row changes class without the block following",
         _DOC.replace("| B | t | COVERED |", "| B | t | UNCOVERED |"), 1),
        ("a row is added without the block following",
         _DOC.replace("| E | - | N/A |", "| E | - | N/A |\n| F | t | COVERED |"),
         1),
        ("the matrix heading is renamed -- must fail loud, not count zero",
         _DOC.replace("## RTM — requirement → test → coverage class",
                      "## Requirements"), 1),
        ("the Counts block is missing",
         _DOC.split("## Counts")[0], 1),
    ]

    failures = 0
    for name, doc, want in cases:
        got = run(doc)
        ok = got == want
        if not ok:
            failures += 1
        print(f"  [{'ok  ' if ok else 'FAIL'}] {name}  (exit {got}, want {want})")

    # The classification-order trap, checked directly rather than via the file.
    if classify("HAPPY-PATH-ONLY") != "HAPPY-PATH-ONLY":
        print("  [FAIL] 'HAPPY-PATH-ONLY' misclassified -- the substring trap")
        failures += 1
    else:
        print("  [ok  ] 'HAPPY-PATH-ONLY' is not read as COVERED")
    if classify("UNCOVERED") != "UNCOVERED":
        print("  [FAIL] 'UNCOVERED' misclassified -- the substring trap")
        failures += 1
    else:
        print("  [ok  ] 'UNCOVERED' is not read as COVERED")

    print()
    if failures:
        print(f"self-test: {failures} case(s) failed")
        return 1
    print(f"self-test: {len(cases) + 2}/{len(cases) + 2} passed")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--repo-root", default=str(REPO_ROOT))
    args = ap.parse_args()
    if args.self_test:
        return _self_test()
    return check(Path(args.repo_root).resolve())


if __name__ == "__main__":
    sys.exit(main())
