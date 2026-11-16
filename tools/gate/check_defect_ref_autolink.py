# -*- coding: utf-8 -*-
"""check_defect_ref_autolink.py — no internal defect reference GitHub will autolink.

GitHub #389. Two unrelated numbering schemes share ``#nn`` in this repository:
GitHub issue numbers and the internal "Defect" scheme (docs/TRACKING.md). The
old convention said to write the internal one as ``Defect #nn``. That
disambiguates for a human reader, but not for GitHub: any ``#nn`` in a commit
message, PR title or PR body is autolinked whatever word precedes it. So every
commit that followed the convention cross-referenced an unrelated GitHub issue.
PR GitHub #384 wrote the Linux port's internal number that way, and its commits
are now permanently listed on GitHub #42 (SEC-104, closed, unrelated).

The convention is now ``Defect-nn``, which GitHub does not autolink. This
checker fails on ``Defect #nn`` (any whitespace) wherever GitHub
would create a cross-reference from it:

* ``--base B --head H``: every commit message in the range ``B..H``;
* ``--event-file PATH``: a GitHub Actions event payload. A pull_request
  event's title and body are checked. Any other event carries nothing to check
  and passes;
* ``--paths P ...``: Markdown files. GitHub does not autolink ``#nn`` inside a
  rendered repository file, so this mode is about consistency, not
  cross-references. It keeps the live docs, which the commit messages tend to
  copy, on the new convention.

A reference inside an inline code span or a fenced code block is not
autolinked by GitHub and is not reported. That is how this file, TRACKING.md
and the tests can still name the old form.

Exit 0 = clean, 1 = at least one autolinkable internal reference, 2 = usage or
input error.
"""
from __future__ import annotations

import argparse
import io
import json
import re
import subprocess
import sys
from dataclasses import dataclass

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")

# "Defect", whitespace, "#", digits — the form GitHub turns into a link.
# Case-sensitive on purpose: the convention writes the scheme name capitalised,
# and prose such as "the defect #200 describes" is usually a GitHub reference
# that SHOULD link. The leading word boundary keeps identifiers such as
# "NoDefect #3" out, and the digit run must end at a non-word character, as
# GitHub's own reference parser requires.
PATTERN = re.compile(r"\bDefect\s+#(\d+)\b")

_FENCE = re.compile(r"^\s*(```|~~~)")
_INLINE_CODE = re.compile(r"(`+)(?:(?!\1).)+?\1")


@dataclass(frozen=True)
class Finding:
    where: str
    line: int
    text: str

    def __str__(self) -> str:
        return f"{self.where}:{self.line}: {self.text.strip()}"


def find_refs(text: str, where: str) -> list[Finding]:
    """Return every autolinkable ``Defect #nn`` outside code spans and fences."""
    out: list[Finding] = []
    in_fence = False
    for n, line in enumerate(text.splitlines(), 1):
        if _FENCE.match(line):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        visible = _INLINE_CODE.sub(lambda m: " " * len(m.group(0)), line)
        if PATTERN.search(visible):
            out.append(Finding(where, n, line))
    return out


def commit_messages(base: str, head: str) -> list[tuple[str, str]]:
    """``(sha, message)`` for every commit in ``base..head``."""
    raw = subprocess.run(
        ["git", "log", "--format=%H%x00%B%x1e", f"{base}..{head}"],
        check=True, capture_output=True, text=True, encoding="utf-8",
    ).stdout
    commits = []
    for rec in raw.split("\x1e"):
        rec = rec.strip("\n")
        if not rec:
            continue
        sha, _, msg = rec.partition("\x00")
        commits.append((sha, msg))
    return commits


def check_range(base: str, head: str) -> list[Finding]:
    found: list[Finding] = []
    for sha, msg in commit_messages(base, head):
        found += find_refs(msg, f"commit {sha[:12]}")
    return found


def check_event(path: str) -> list[Finding]:
    with io.open(path, encoding="utf-8") as f:
        event = json.load(f)
    pr = event.get("pull_request")
    if not isinstance(pr, dict):
        return []
    found = find_refs(pr.get("title") or "", "PR title")
    found += find_refs(pr.get("body") or "", "PR body")
    return found


def check_paths(paths: list[str]) -> list[Finding]:
    found: list[Finding] = []
    for p in paths:
        with io.open(p, encoding="utf-8") as f:
            found += find_refs(f.read(), p)
    return found


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--base")
    ap.add_argument("--head", default="HEAD")
    ap.add_argument("--event-file")
    ap.add_argument("--paths", nargs="+", default=[])
    args = ap.parse_args(argv)

    if not (args.base or args.event_file or args.paths):
        ap.error("nothing to check: give --base, --event-file and/or --paths")

    try:
        found: list[Finding] = []
        if args.base:
            found += check_range(args.base, args.head)
        if args.event_file:
            found += check_event(args.event_file)
        if args.paths:
            found += check_paths(args.paths)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"[defect-ref] ERROR: {exc}", file=sys.stderr)
        return 2

    if found:
        print(f"[defect-ref] FAIL: {len(found)} internal defect reference(s) "
              "GitHub would autolink to an unrelated issue:")
        for f in found:
            print(f"  {f}")
        print("Write the internal scheme as Defect-nn (no '#'); see "
              "docs/TRACKING.md and GitHub #389. A reference inside `code` "
              "is not autolinked and is allowed.")
        return 1
    print("[defect-ref] OK: no autolinkable internal defect references.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
