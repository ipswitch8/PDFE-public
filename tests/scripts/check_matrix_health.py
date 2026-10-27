# -*- coding: utf-8 -*-
"""Matrix-health gate for the native-UI visual+interaction suite.

Pipeline c4a9e271 Phase 10. A full UI sweep writes a per-run status CSV to
``tests/_runtime/ui_test_matrix_<timestamp>.csv`` (see
``tests/ui_automation/conftest.py::_write_matrix_updates``). This script compares
that live run against the source-controlled *closeout* matrix
(``tests/ui_automation/matrix/ui_test_matrix.csv``) and FAILS CI on either of two
regressions:

  1. **Green-floor breach** — the number of ``green`` rows in the live run drops
     below ``closeout_green - floor_delta`` (default delta 5). A handful of green
     rows can legitimately flake red on a ~6 h pywinauto sweep, but a larger drop
     means real coverage was lost.

  2. **Planned-row ratchet (GitHub #142)** — the committed matrix carries more
     ``planned`` rows than ``--max-planned`` allows. Both checks above read
     only committed-``green`` rows, so a ``planned`` row was invisible to this
     gate rather than merely unsatisfied: the gate could ratify finished work
     and could say nothing whatever about work not yet started. The census is
     now printed unconditionally, and ``--fail-on-promotable`` turns a
     not-yet-done row that records ``green`` into a reported unexpected pass
     instead of a silent one.

  3. **Previously-green-now-red** — any row that is ``green`` in the committed
     closeout matrix records ``red`` in the live run. This is the GAP-A15-style
     false-green defense at the matrix layer: a single committed-green row going
     red fails the build regardless of the overall count.

Exit codes: 0 = healthy, 1 = a regression was detected, 2 = usage/IO error
(e.g. no runtime CSV found when one was required).

Usage
-----
    # CI: auto-discover the newest runtime CSV under tests/_runtime/.
    python tests/scripts/check_matrix_health.py --repo-root .

    # Explicit runtime CSV (e.g. a specific sweep artifact).
    python tests/scripts/check_matrix_health.py \
        --runtime-csv tests/_runtime/ui_test_matrix_20260625_011500.csv

    # Override the floor delta (how many green rows may regress before failing).
    python tests/scripts/check_matrix_health.py --floor-delta 5
"""
from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

DEFAULT_COMMITTED = Path("tests/ui_automation/matrix/ui_test_matrix.csv")
DEFAULT_RUNTIME_DIR = Path("tests/_runtime")
DEFAULT_FLOOR_DELTA = 5


def _load_status_by_id(csv_path: Path) -> Dict[str, str]:
    """Return ``{test_id: status}`` for every row in *csv_path*."""
    out: Dict[str, str] = {}
    with csv_path.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            tid = (row.get("test_id") or "").strip()
            if tid:
                out[tid] = (row.get("status") or "").strip()
    return out


def _green_count(status_by_id: Dict[str, str]) -> int:
    return sum(1 for s in status_by_id.values() if s == "green")


# Statuses that mean "this shard actually exercised the row". Anything else is
# the shard saying "not mine", which must never overwrite another shard's real
# result -- that is the whole hazard of merging a partitioned sweep.
_NOT_RUN = frozenset({"no-run", ""})


def merge_shard_status(paths: List[Path]) -> Tuple[Dict[str, str], List[str]]:
    """Merge per-shard runtime CSVs into one status map.

    WHY MERGING IS NOT A UNION. Under sharding each shard runs a SUBSET, and
    reports every row it did not run as `no-run` (or omits it). A naive
    last-writer-wins merge therefore lets shard 3's "no-run" erase shard 1's
    "green", and the aggregate looks like a mass regression -- or, worse, a
    later shard's "no-run" erases a "red" and the aggregate looks clean.

    So: a row's status comes from the shard that actually RAN it, and a row
    claimed by two shards with DIFFERENT real statuses is a conflict, reported
    rather than silently resolved. Picking a winner there would be inventing a
    result; the partition is supposed to make it impossible, so if it happens
    the partition is broken and that is the finding.
    """
    merged: Dict[str, str] = {}
    owner: Dict[str, Path] = {}
    conflicts: List[str] = []
    for p in paths:
        for tid, status in _load_status_by_id(p).items():
            if status in _NOT_RUN:
                merged.setdefault(tid, status)
                continue
            prev = merged.get(tid)
            if prev is not None and prev not in _NOT_RUN and prev != status:
                conflicts.append(
                    f"{tid}: {owner[tid].name} says {prev!r} but {p.name} says "
                    f"{status!r}. Two shards ran the same row and disagree; the "
                    f"partition is not disjoint, so neither result can be trusted."
                )
                continue
            merged[tid] = status
            owner[tid] = p
    return merged, conflicts


def _shard_csvs(shard_dir: Path) -> List[Path]:
    """Every per-shard runtime CSV in *shard_dir*, in stable order."""
    return sorted(shard_dir.glob("ui_test_matrix_*.csv"))


def _newest_runtime_csv(runtime_dir: Path) -> Optional[Path]:
    """Newest ``ui_test_matrix_*.csv`` under *runtime_dir* by filename.

    The filenames embed a sortable ``YYYYMMDD_HHMMSS`` stamp, so a lexical max is
    the newest; mtime is used only to break ties.
    """
    candidates = sorted(runtime_dir.glob("ui_test_matrix_*.csv"))
    if not candidates:
        return None
    return max(candidates, key=lambda p: (p.name, p.stat().st_mtime))


# Statuses that mean "asserted-pass this run". A committed-green row landing on
# anything else is a degradation (see _DEGRADED_STATUSES).
_PASS_STATUS = "green"

# A committed-green row whose runtime status is one of these DEGRADED this run.
# 'red' is the classic regression the original check already caught; 'skip',
# 'blocked', 'planned', 'no-run' and '' (row absent from the runtime CSV) are
# the H4 hole this phase closes — a row silently degrading from asserted-pass to
# not-asserted. 'no-run'/'' (out-of-scope in a partial leg) is only failed when
# --require-ran demands a full sweep; skip/blocked/planned/red always fail.
_ALWAYS_FAIL_DEGRADED = frozenset({"red", "skip", "blocked", "planned"})
_SCOPE_ONLY_DEGRADED = frozenset({"no-run", ""})

# ---------------------------------------------------------------------------
# GitHub #142 — THE GATE COULD ONLY EVER RATIFY.
#
# Everything above this line reads the committed matrix through one filter:
# `if s != _PASS_STATUS: continue`. So a row that is committed as `planned` —
# scaffolded from a UIA surface sweep and never given a driving test — is not
# merely tolerated by the gate, it is INVISIBLE to it. The gate reports on
# work already claimed done and is structurally incapable of saying anything
# about work not yet started. That is the same shape as the path-hygiene guard
# that matched 365 files and hid four real failures: the check was green
# because it was not looking.
#
# Three things are added, all of which are about VISIBILITY first:
#
#   1. A census of the committed matrix by status is printed on every run,
#      with the not-yet-done rows named. "26 rows are planned, here they are"
#      is the minimum a coverage gate owes its reader.
#   2. `--max-planned N` turns that census into a RATCHET. Without a pinned
#      ceiling, "add the row now, write the test later" has no cost and no
#      reviewer sees it; scaffolding accumulates and every new row is
#      invisible to the gate on arrival. The ceiling must be supplied
#      externally — deriving it from the committed matrix would compare the
#      file against itself and pass unconditionally.
#   3. `--fail-on-promotable` is the strict-xfail analogue. A row committed as
#      not-yet-done that records `green` in a live sweep is an UNEXPECTED
#      PASS. Swallowing it silently is how a row sits for months describing
#      work that is in fact finished — exactly what happened to the five
#      sign-dialog rows this issue names, which were promoted to `green` in
#      2026-07 while the known-issues row went on describing them as planned.
# ---------------------------------------------------------------------------

#: Committed statuses that mean "this row does not assert anything yet".
#: `wont-fix` is deliberately NOT here: it is a decision, not a backlog item,
#: so it is not backlog scaffolding and must not be ratcheted by --max-planned.
#: `skip`/`xfail` are also excluded — they have a driving test that runs.
_NOT_YET_DONE = frozenset({"planned", "blocked"})

#: Committed statuses from which an unexpected live GREEN is a FINDING.
#:
#: GITHUB #171 adds `wont-fix` to this set, and only to this set. A `wont-fix`
#: is a decision rather than a backlog item — which is why it stays out of
#: `_NOT_YET_DONE` above and out of the planned ratchet — but a decision can
#: EXPIRE, because the blocker it cites gets fixed by unrelated work. When that
#: happens the row goes on describing itself as blocked while the behaviour is
#: recoverable, and nothing can say so: this is precisely the "a tool that only
#: inspects the rows already claimed good can only ever ratify" shape that
#: GitHub #142 fixed for `planned` rows. The two now SHARE this mechanism
#: rather than growing separately, as #171 asks.
#:
#: This is only reachable because conftest's write-back stopped discarding a
#: live green on a `wont-fix` row (tests/ui_automation/matrix/runtime_status.py).
#: Before that fix the runtime CSV could never carry `green` for such a row, so
#: widening the set here alone would have been decoration.
_PROMOTABLE_FROM = _NOT_YET_DONE | frozenset({"wont-fix"})

#: The subset of _NOT_YET_DONE that `--max-planned` ratchets. `blocked` rows
#: have a real cause recorded elsewhere and can legitimately appear when an
#: environment regresses; `planned` rows are pure un-started scaffolding and
#: are the ones that must not accumulate unseen.
_RATCHETED_NOT_DONE = frozenset({"planned"})


def status_census(status_by_id: Dict[str, str]) -> Dict[str, int]:
    """``{status: count}`` over *status_by_id*, including empty statuses."""
    out: Dict[str, int] = {}
    for s in status_by_id.values():
        key = s or "(blank)"
        out[key] = out.get(key, 0) + 1
    return out


def not_yet_done_rows(committed: Dict[str, str]) -> List[Tuple[str, str]]:
    """Committed rows that assert nothing yet, sorted by (status, test_id)."""
    rows = [(tid, s) for tid, s in committed.items() if s in _NOT_YET_DONE]
    rows.sort(key=lambda kv: (kv[1], kv[0]))
    return rows


def promotable_rows(committed: Dict[str, str],
                    runtime: Dict[str, str]) -> List[Tuple[str, str]]:
    """Rows committed as not-yet-done OR ``wont-fix`` that recorded ``green``.

    An unexpected pass. The matrix is stale and the row should be promoted, or
    (for a ``wont-fix``) its stated reason re-justified — see _PROMOTABLE_FROM.
    """
    rows = [(tid, s) for tid, s in committed.items()
            if s in _PROMOTABLE_FROM and runtime.get(tid, "") == _PASS_STATUS]
    rows.sort()
    return rows


def evaluate(committed_csv: Path, runtime_csv: Optional[Path],
             floor_delta: int,
             explicit_floor: Optional[int] = None,
             require_ran: bool = False,
             runtime_status: Optional[Dict[str, str]] = None,
             max_planned: Optional[int] = None,
             fail_on_promotable: bool = False,
             ) -> Tuple[bool, List[str]]:
    """Compare the runtime sweep to the committed closeout matrix.

    Returns ``(healthy, messages)``. *healthy* is False if the green floor is
    breached, or any committed-green row DEGRADED this run — where degraded
    means it did not record ``green``:

    * to ``red`` / ``skip`` / ``blocked`` / ``planned``: ALWAYS a failure
      (a row that participated but stopped asserting-pass — the H4 defect);
    * to ``no-run`` / absent: a failure only when *require_ran* is set (a full
      sweep must exercise every committed-green row; a partial CI leg legitimately
      leaves out-of-scope rows unrun).

    ``require_ran`` is the full-sweep / closeout gate; leave it off for partial
    legs so out-of-scope rows are not mistaken for silent coverage loss.
    """
    committed = _load_status_by_id(committed_csv)
    # runtime_status lets a MERGED per-shard map be scored by exactly the same
    # code as a single-run CSV. Scoring merged results through a second path
    # would mean the thing CI runs is not the thing the equivalence test proves.
    runtime = (runtime_status if runtime_status is not None
               else _load_status_by_id(runtime_csv))

    closeout_green = _green_count(committed)
    live_green = _green_count(runtime)
    floor = explicit_floor if explicit_floor is not None \
        else closeout_green - floor_delta

    # Classify every committed-green row by what it recorded this run.
    degraded_in_place: List[Tuple[str, str]] = []   # participated, stopped passing
    degraded_no_run: List[Tuple[str, str]] = []     # did not run this sweep
    for tid, s in committed.items():
        if s != _PASS_STATUS:
            continue
        rt = runtime.get(tid, "")  # '' == row absent from the runtime CSV
        if rt == _PASS_STATUS:
            continue
        if rt in _ALWAYS_FAIL_DEGRADED:
            degraded_in_place.append((tid, rt or "red"))
        elif rt in _SCOPE_ONLY_DEGRADED:
            degraded_no_run.append((tid, rt or "absent"))
        else:
            # Any other unexpected status is treated as an in-place degradation
            # (fail-closed): an unknown status is not proof the row asserted-pass.
            degraded_in_place.append((tid, rt))
    degraded_in_place.sort()
    degraded_no_run.sort()

    msgs: List[str] = []
    msgs.append(f"committed closeout green: {closeout_green}")
    msgs.append(f"live runtime green:       {live_green}")
    msgs.append(f"green floor:              {floor} "
                f"(closeout {closeout_green} - delta {floor_delta})"
                if explicit_floor is None else
                f"green floor:              {floor} (explicit)")
    msgs.append(f"green->degraded (in place): {len(degraded_in_place)}")
    msgs.append(f"green->no-run/absent:       {len(degraded_no_run)} "
                f"(require_ran={require_ran})")

    # --- GitHub #142: make the not-yet-done rows VISIBLE -------------------
    census = status_census(committed)
    msgs.append("committed status census: " + ", ".join(
        f"{k}={census[k]}" for k in sorted(census)))
    pending = not_yet_done_rows(committed)
    planned_count = sum(1 for _, s in pending if s in _RATCHETED_NOT_DONE)
    msgs.append(f"NOT-YET-DONE rows:        {len(pending)} "
                f"(planned={planned_count})")
    for tid, s in pending:
        msgs.append(f"    . {tid} -> {s} (asserts nothing yet)")
    promotable = promotable_rows(committed, runtime)
    msgs.append(f"NOT-YET-DONE but GREEN this run: {len(promotable)}")
    for tid, s in promotable:
        msgs.append(f"    ^ {tid} committed '{s}' but recorded green "
                    f"— promote it")

    healthy = True
    if max_planned is not None and planned_count > max_planned:
        healthy = False
        msgs.append(
            f"FAIL: {planned_count} committed 'planned' rows exceed the "
            f"pinned ceiling of {max_planned}. A row added to the matrix "
            f"without a driving test is invisible to every other check in "
            f"this gate; raise the ceiling deliberately, or implement the "
            f"rows.")
    if promotable and fail_on_promotable:
        healthy = False
        msgs.append(
            "FAIL: rows committed as not-yet-done recorded green this run. "
            "An unexpected pass is a finding, not a nil result — update the "
            "committed matrix so the coverage is counted:")
        msgs.extend(f"    - {tid} ({s} -> green)" for tid, s in promotable)
    if live_green < floor:
        healthy = False
        msgs.append(
            f"FAIL: live green {live_green} < floor {floor} "
            f"({closeout_green - live_green} rows lost vs closeout)")
    if degraded_in_place:
        healthy = False
        msgs.append("FAIL: committed-green rows that stopped asserting-pass "
                    "this run (red/skip/blocked/planned):")
        msgs.extend(f"    - {tid} -> {rt}" for tid, rt in degraded_in_place)
    if degraded_no_run and require_ran:
        healthy = False
        msgs.append("FAIL: committed-green rows not exercised this run "
                    "(--require-ran / full sweep expected all to run):")
        msgs.extend(f"    - {tid} -> {rt}" for tid, rt in degraded_no_run)
    elif degraded_no_run:
        msgs.append(f"note: {len(degraded_no_run)} committed-green row(s) not run "
                    f"this leg (tolerated without --require-ran)")

    if healthy:
        msgs.append("OK: matrix health within tolerance (no floor breach, "
                    "no committed-green degradations)")
    return healthy, msgs


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo-root", type=Path, default=Path("."),
                    help="Repo root; relative defaults resolve against it.")
    ap.add_argument("--committed-csv", type=Path, default=None,
                    help="Source-controlled closeout matrix CSV.")
    ap.add_argument("--runtime-dir", type=Path, default=None,
                    help="Directory holding ui_test_matrix_<ts>.csv sweep "
                         "artifacts (newest is used).")
    ap.add_argument("--runtime-csv", type=Path, default=None,
                    help="Explicit runtime CSV; overrides --runtime-dir.")
    ap.add_argument("--runtime-shard-dir", type=Path, default=None,
                    help="Directory of PER-SHARD ui_test_matrix_*.csv files to "
                         "MERGE into one aggregate before scoring. Without this, "
                         "sharding the UI leg makes every shard look like it "
                         "dropped the committed-green rows it did not run.")
    ap.add_argument("--floor-delta", type=int, default=DEFAULT_FLOOR_DELTA,
                    help="How many green rows may regress before failing "
                         f"(default {DEFAULT_FLOOR_DELTA}).")
    ap.add_argument("--floor", type=int, default=None,
                    help="Absolute green floor; overrides --floor-delta math.")
    ap.add_argument("--allow-missing-runtime", action="store_true",
                    help="Exit 0 (skip) instead of failing when no runtime CSV "
                         "is found (useful for non-sweep CI legs).")
    ap.add_argument("--max-planned", type=int, default=None,
                    help="Ceiling on the number of committed 'planned' rows. "
                         "FAILS when exceeded. Must be supplied externally: a "
                         "ceiling derived from the committed matrix would "
                         "compare the file against itself and never fail.")
    ap.add_argument("--fail-on-promotable", action="store_true",
                    help="FAIL when a row committed as planned/blocked "
                         "recorded 'green' in the live run. An unexpected "
                         "pass is a finding; without this it is swallowed and "
                         "the matrix stays stale (GitHub #142).")
    ap.add_argument("--require-ran", action="store_true",
                    help="Full-sweep/closeout gate: also FAIL when a committed-"
                         "green row was not exercised this run (green->no-run/"
                         "absent). Off by default so partial legs tolerate "
                         "out-of-scope rows.")
    args = ap.parse_args(argv)

    root = args.repo_root.resolve()
    committed_csv = args.committed_csv or (root / DEFAULT_COMMITTED)
    if not committed_csv.is_absolute():
        committed_csv = root / committed_csv
    if not committed_csv.exists():
        print(f"[matrix-health] committed CSV not found: {committed_csv}",
              file=sys.stderr)
        return 2

    # MERGED-SHARD PATH. Takes precedence over --runtime-csv/--runtime-dir,
    # because asking for it and silently getting a single shard's view would be
    # the exact false-green this flag exists to prevent.
    if args.runtime_shard_dir is not None:
        shard_dir = args.runtime_shard_dir
        if not shard_dir.is_absolute():
            shard_dir = root / shard_dir
        shards = _shard_csvs(shard_dir) if shard_dir.exists() else []
        if not shards:
            if args.allow_missing_runtime:
                print(f"[matrix-health] no per-shard CSV under {shard_dir}; "
                      f"--allow-missing-runtime set, skipping.")
                return 0
            print(f"[matrix-health] no per-shard ui_test_matrix_*.csv under "
                  f"{shard_dir}.", file=sys.stderr)
            return 2
        merged, conflicts = merge_shard_status(shards)
        print(f"[matrix-health] committed: {committed_csv}")
        print(f"[matrix-health] merged {len(shards)} shard CSV(s) from "
              f"{shard_dir}: {len(merged)} row(s)")
        for s in shards:
            print(f"[matrix-health]   + {s.name}")
        if conflicts:
            # Fail rather than choose. See merge_shard_status.
            for c in conflicts:
                print(f"[matrix-health] CONFLICT {c}", file=sys.stderr)
            print("[matrix-health] refusing to score a merge whose shards "
                  "disagree.", file=sys.stderr)
            return 2
        healthy, msgs = evaluate(committed_csv, None,
                                 floor_delta=args.floor_delta,
                                 explicit_floor=args.floor,
                                 require_ran=args.require_ran,
                                 runtime_status=merged,
                                 max_planned=args.max_planned,
                                 fail_on_promotable=args.fail_on_promotable)
        for m in msgs:
            print(f"[matrix-health] {m}")
        return 0 if healthy else 1

    runtime_csv = args.runtime_csv
    if runtime_csv is not None and not runtime_csv.is_absolute():
        runtime_csv = root / runtime_csv
    if runtime_csv is None:
        runtime_dir = args.runtime_dir or (root / DEFAULT_RUNTIME_DIR)
        if not runtime_dir.is_absolute():
            runtime_dir = root / runtime_dir
        runtime_csv = _newest_runtime_csv(runtime_dir) if runtime_dir.exists() \
            else None

    if runtime_csv is None or not runtime_csv.exists():
        where = args.runtime_csv or (args.runtime_dir or DEFAULT_RUNTIME_DIR)
        if args.allow_missing_runtime:
            print(f"[matrix-health] no runtime CSV under {where}; "
                  f"--allow-missing-runtime set, skipping.")
            return 0
        print(f"[matrix-health] no runtime sweep CSV found ({where}). A UI "
              f"sweep must run first to produce ui_test_matrix_<ts>.csv.",
              file=sys.stderr)
        return 2

    print(f"[matrix-health] committed: {committed_csv}")
    print(f"[matrix-health] runtime:   {runtime_csv}")
    healthy, msgs = evaluate(committed_csv, runtime_csv,
                             floor_delta=args.floor_delta,
                             explicit_floor=args.floor,
                             require_ran=args.require_ran,
                             max_planned=args.max_planned,
                             fail_on_promotable=args.fail_on_promotable)
    for m in msgs:
        print(f"[matrix-health] {m}")
    return 0 if healthy else 1


if __name__ == "__main__":
    raise SystemExit(main())
