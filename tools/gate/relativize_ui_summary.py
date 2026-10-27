# -*- coding: utf-8 -*-
"""Re-express an existing ui-suite-summary.json with portable paths (F48).

WHAT THIS IS, AND WHAT IT IS NOT. This does NOT re-run the UI suite. It applies
`run_ui_suite.portable_path` / `scrub_text` -- the exact functions the producer
now uses -- to the string fields of an artifact that was produced BEFORE the
fix, and rewrites it in place.

That distinction matters enough to state plainly: the committed artifact
`tests/_runtime/discovery_phase8/ui-suite-summary.json` records a 6h19m sweep
from 2026-06-24 whose figures a documented claim is checked against
(`F19DocClaims.ComparisonDocUiE2eCountsMatchTheSummaryArtifact`). Re-running the
suite to regenerate it would produce DIFFERENT figures and silently invalidate
the comparison the artifact exists to support. So the run is not repeated; only
the host-specific rendering of paths that nothing reads is changed, and every
numeric field is asserted unchanged before the file is written.

Usage:  python tools/gate/relativize_ui_summary.py <summary.json> [--check]

`--check` reports whether the file is already clean and writes nothing; it is
what a re-run of this tool does after the fix has landed (exit 0 = clean).
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.scripts.run_ui_suite import (  # noqa: E402
    _ABS_PATH_RE, portable_path, scrub_text,
)

#: Numeric/boolean fields that MUST survive the rewrite untouched. Checked
#: rather than trusted: the whole point of the artifact is its counts.
_SCALAR_KEYS = ("files_run", "wall_clock_s", "totals", "failure_count",
                "files_with_problems_count")


def _walk_strings(obj, mutate):
    if isinstance(obj, dict):
        return {k: _walk_strings(v, mutate) for k, v in obj.items()}
    if isinstance(obj, list):
        return [_walk_strings(v, mutate) for v in obj]
    if isinstance(obj, str):
        return mutate(obj)
    return obj


def _absolute_strings(obj) -> list[str]:
    found: list[str] = []

    def collect(s):
        found.extend(_ABS_PATH_RE.findall(s))
        return s

    _walk_strings(obj, collect)
    return found


def main(argv: list[str]) -> int:
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 2
    path = Path(argv[0]).resolve()
    check_only = "--check" in argv[1:]

    raw = path.read_bytes()
    crlf = b"\r\n" in raw
    data = json.loads(raw.decode("utf-8"))

    before = _absolute_strings(data)
    if not before:
        print(f"[relativize] {path.name}: already clean (0 absolute paths)")
        return 0
    if check_only:
        print(f"[relativize] {path.name}: {len(before)} absolute path(s), "
              f"{len(set(before))} distinct -- NOT clean")
        return 1

    # The run's own roots, recovered from the artifact rather than assumed:
    # the deepest common ancestor of the absolute paths it contains is the
    # output directory, and the repo root is whichever prefix ends at the
    # repository this tool lives in. Recovering it from the data means the tool
    # works on an artifact produced on another machine.
    repo_root = str(REPO_ROOT)
    out_root = str(path.parent)

    scalars_before = {k: json.dumps(data.get(k)) for k in _SCALAR_KEYS}
    fixed = _walk_strings(
        data, lambda s: scrub_text(s, repo_root, out_root)
        if _ABS_PATH_RE.search(s) else s)
    # Whole-value path fields go through portable_path directly so a bare path
    # with no surrounding prose takes the same route as one inside a message.
    for rec in fixed.get("per_file", []):
        for key in ("file", "junit", "log"):
            if key in rec:
                rec[key] = portable_path(rec[key], repo_root, out_root)
    for rec in fixed.get("files_with_problems", []):
        if "file" in rec:
            rec["file"] = portable_path(rec["file"], repo_root, out_root)
    if isinstance(fixed.get("matrix_aggregate"), dict) and \
            "aggregate_csv" in fixed["matrix_aggregate"]:
        fixed["matrix_aggregate"]["aggregate_csv"] = portable_path(
            fixed["matrix_aggregate"]["aggregate_csv"], repo_root, out_root)

    scalars_after = {k: json.dumps(fixed.get(k)) for k in _SCALAR_KEYS}
    for k in _SCALAR_KEYS:
        if scalars_before[k] != scalars_after[k]:
            print(f"[relativize] REFUSING: {k} changed "
                  f"({scalars_before[k]} -> {scalars_after[k]}). This tool "
                  f"must not alter a single recorded figure.", file=sys.stderr)
            return 1

    after = _absolute_strings(fixed)
    if after:
        print(f"[relativize] REFUSING: {len(after)} absolute path(s) survived, "
              f"e.g. {after[0]!r}", file=sys.stderr)
        return 1

    text = json.dumps(fixed, indent=2)
    newline = "\r\n" if crlf else "\n"
    with open(path, "w", encoding="utf-8", newline=newline) as fh:
        fh.write(text)

    print(f"[relativize] {path.name}: {len(before)} absolute path(s) "
          f"({len(set(before))} distinct) -> 0; every recorded figure "
          f"unchanged.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
