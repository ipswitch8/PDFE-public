#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_asan_cadence.py — the ASan lane's detection window must stay bounded.

Meta-audit follow-up **F30**, closing the ``deferred-backlog.md`` §3 row
*"ASan cadence leaves a 7-day window"* filed 2026-08-01 by the F4
``security-audit`` gate.

Why this exists
---------------
Defects #47 (SelectTool stale ``page_``) and #48 (TextMarkupTool self-delete
UAF) were **ASan-only** finds — no non-ASan run of the suite could see them.
F4 put ``windows-sanitizer.yml`` back on an automatic trigger, but at
``0 18 * * 0`` (weekly), which means a regression on that surface can live up
to **seven days** before anything looks at it. The gate's accepted
recommendation: a faster cadence (2-3 days), **or** an ASan trigger on PRs
touching codec/render paths. This check makes that bound *enforced* rather
than *remembered*.

What it actually asserts
------------------------
The workflow passes if EITHER:

* **cadence** — the union of every ``schedule.cron`` entry fires such that the
  maximum gap between consecutive firings is ``<= --max-gap-days``; or
* **path-filtered PR trigger** — a ``pull_request`` trigger exists carrying a
  NON-EMPTY ``paths`` list that covers every configured source area
  (default: ``src/image`` and ``src/rendering`` — "codec/render").

Three deliberate anti-cheat properties, because a checker that trivially
passes any YAML is worse than no checker (F29's lesson: the obvious control
did not catch the actual failure mode):

1. **The gap is COMPUTED, not pattern-matched.** Every cron is expanded into
   real firing datetimes over a multi-year span and consecutive deltas are
   measured. ``0 18 * * 0,1`` (Sun+Mon) *looks* like a two-day cadence to any
   regex and is really a 6-day window; it fails here. Day-of-month stepping
   (``0 18 */3 * *``) is evaluated against real month lengths rather than
   assumed.
2. **A parseable file is not a passing file.** No schedule and no qualifying
   PR trigger is a FAIL, not a silent pass. An unparseable cron field is a
   FAIL, never "unknown, assume fine".
3. **A bare ``pull_request:`` does NOT qualify.** ``windows-sanitizer.yml``'s
   header records that the push/PR auto-trigger was removed 2026-07-20 so a
   6-hour ASan build could not be kicked off by every commit and monopolise
   the single self-hosted ``pdfe-windows`` runner. An unfiltered PR trigger
   would re-create exactly that, so it is rejected: only a genuinely
   path-scoped list counts.

GitHub Actions gotcha handled here: PyYAML parses the unquoted workflow key
``on:`` as the **boolean** ``True`` (YAML 1.1 truthy), not the string
``"on"``. Looking up ``doc["on"]`` therefore reports a false absence and would
make this check pass vacuously on every real workflow. ``extract_on_block``
accepts ``True``, ``"on"`` and ``"On"``/``"ON"``.

Cron semantics follow Vixie/GitHub: when BOTH day-of-month and day-of-week are
restricted the day matches if EITHER matches; when only one is restricted that
one governs. Day-of-week 7 is normalised to 0 (Sunday).

The pure decision core is ``evaluate`` (no I/O) for direct unit testing.

Usage:
  python tools/gate/check_asan_cadence.py \
      --workflow .github/workflows/windows-sanitizer.yml \
      [--max-gap-days 3] [--source-area src/image --source-area src/rendering]
Exit 0 = compliant; 1 = cadence too slow / no qualifying trigger; 2 = usage,
IO or parse error.
"""
from __future__ import annotations

import argparse
import datetime as _dt
import re
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Set, Tuple

try:
    import yaml
except ImportError:  # pragma: no cover - environment problem, not a policy result
    print("[asan-cadence] PyYAML is required (python -m pip install pyyaml)",
          file=sys.stderr)
    raise

# Defaults. "codec/render" per the backlog row's own wording.
DEFAULT_SOURCE_AREAS: Tuple[str, ...] = ("src/image", "src/rendering")
DEFAULT_MAX_GAP_DAYS = 3
# Span start/length for firing enumeration. 1500 days > 4 years, so it spans
# every month length, two Februaries of 28 days and the 29-day Feb 2028 — the
# cases a day-of-month step expression behaves differently across.
DEFAULT_SPAN_START = _dt.date(2026, 1, 1)
DEFAULT_SPAN_DAYS = 1500


class CronError(ValueError):
    """A cron expression that cannot be interpreted. Never treated as 'fine'."""


# ---------------------------------------------------------------------------
# cron parsing / expansion
# ---------------------------------------------------------------------------

def _parse_field(expr: str, lo: int, hi: int, field: str) -> Set[int]:
    """Expand one cron field into the set of values it matches.

    Supports ``*``, ``a``, ``a-b``, ``*/n``, ``a-b/n`` and comma lists.
    Anything else raises :class:`CronError` — an uninterpretable schedule must
    fail the check, not be waved through.
    """
    expr = (expr or "").strip()
    if not expr:
        raise CronError("empty %s field" % field)
    out: Set[int] = set()
    for part in expr.split(","):
        part = part.strip()
        if not part:
            raise CronError("empty element in %s field %r" % (field, expr))
        step = 1
        if "/" in part:
            base, _, step_s = part.partition("/")
            base = base.strip()
            try:
                step = int(step_s)
            except ValueError:
                raise CronError("bad step %r in %s field %r" % (step_s, field, expr))
            if step <= 0:
                raise CronError("non-positive step in %s field %r" % (field, expr))
        else:
            base = part
        if base == "*":
            start, end = lo, hi
        elif "-" in base.lstrip("-"):
            a_s, _, b_s = base.partition("-")
            try:
                start, end = int(a_s), int(b_s)
            except ValueError:
                raise CronError("bad range %r in %s field %r" % (base, field, expr))
        else:
            try:
                start = end = int(base)
            except ValueError:
                raise CronError("bad value %r in %s field %r" % (base, field, expr))
        if start < lo or end > hi or start > end:
            raise CronError("out-of-range %r in %s field %r (allowed %d-%d)"
                            % (base, field, expr, lo, hi))
        out.update(range(start, end + 1, step))
    if not out:
        raise CronError("%s field %r matches nothing" % (field, expr))
    return out


class CronSpec:
    """A parsed 5-field cron expression that can enumerate real firing times."""

    __slots__ = ("raw", "minutes", "hours", "doms", "months", "dows",
                 "dom_restricted", "dow_restricted")

    def __init__(self, raw: str) -> None:
        self.raw = raw
        fields = (raw or "").split()
        if len(fields) != 5:
            raise CronError("cron %r has %d fields, expected 5" % (raw, len(fields)))
        m, h, dom, mon, dow = fields
        self.minutes = _parse_field(m, 0, 59, "minute")
        self.hours = _parse_field(h, 0, 23, "hour")
        self.doms = _parse_field(dom, 1, 31, "day-of-month")
        self.months = _parse_field(mon, 1, 12, "month")
        # 0 and 7 both mean Sunday.
        self.dows = {0 if d == 7 else d for d in _parse_field(dow, 0, 7, "day-of-week")}
        self.dom_restricted = dom.strip() != "*"
        self.dow_restricted = dow.strip() != "*"

    def _day_matches(self, day: _dt.date) -> bool:
        if day.month not in self.months:
            return False
        # Python weekday(): Mon=0..Sun=6. Cron: Sun=0..Sat=6.
        cron_dow = (day.weekday() + 1) % 7
        dom_ok = day.day in self.doms
        dow_ok = cron_dow in self.dows
        if self.dom_restricted and self.dow_restricted:
            return dom_ok or dow_ok          # Vixie/GitHub OR semantics
        if self.dom_restricted:
            return dom_ok
        if self.dow_restricted:
            return dow_ok
        return True

    def firings(self, start: _dt.date, span_days: int) -> List[_dt.datetime]:
        out: List[_dt.datetime] = []
        for offset in range(span_days):
            day = start + _dt.timedelta(days=offset)
            if not self._day_matches(day):
                continue
            for hh in sorted(self.hours):
                for mm in sorted(self.minutes):
                    out.append(_dt.datetime(day.year, day.month, day.day, hh, mm))
        return out


def max_gap_days(crons: Sequence[str],
                 start: _dt.date = DEFAULT_SPAN_START,
                 span_days: int = DEFAULT_SPAN_DAYS) -> Tuple[Optional[float], Optional[float], int]:
    """Return ``(max_gap, min_gap, firing_count)`` in days over the span.

    Gaps are measured across the UNION of every cron entry, so two crons that
    are individually slow but interleave correctly are judged on their combined
    effect. ``(None, None, n)`` when fewer than two firings exist — an
    unbounded window, which the caller must treat as a failure.
    """
    fires: Set[_dt.datetime] = set()
    for raw in crons:
        fires.update(CronSpec(raw).firings(start, span_days))
    ordered = sorted(fires)
    if len(ordered) < 2:
        return None, None, len(ordered)
    deltas = [(b - a).total_seconds() / 86400.0
              for a, b in zip(ordered, ordered[1:])]
    return max(deltas), min(deltas), len(ordered)


# ---------------------------------------------------------------------------
# workflow inspection
# ---------------------------------------------------------------------------

def extract_on_block(doc) -> Optional[Dict]:
    """Return the workflow's trigger block, tolerating the ``on:`` -> True trap.

    PyYAML resolves the unquoted key ``on`` to the boolean ``True`` under YAML
    1.1 truthiness. ``doc.get("on")`` therefore returns ``None`` for every real
    GitHub workflow, which would make a naive checker report "no triggers" or,
    worse, pass vacuously.
    """
    if not isinstance(doc, dict):
        return None
    for key in (True, "on", "On", "ON"):
        if key in doc:
            block = doc[key]
            if isinstance(block, dict):
                return block
            # `on: push` / `on: [push, pull_request]` — legal YAML, but neither
            # form can carry a schedule or a paths filter.
            if block is None:
                return {}
            if isinstance(block, str):
                return {block: None}
            if isinstance(block, list):
                return {str(k): None for k in block}
            return {}
    return None


def schedule_crons(on_block: Dict) -> List[str]:
    sched = on_block.get("schedule") or []
    if isinstance(sched, dict):
        sched = [sched]
    out: List[str] = []
    for entry in sched:
        if isinstance(entry, dict) and entry.get("cron"):
            out.append(str(entry["cron"]))
    return out


def qualifying_pr_paths(on_block: Dict,
                        source_areas: Iterable[str]) -> Tuple[bool, List[str], List[str]]:
    """Does a path-FILTERED ``pull_request`` trigger cover every source area?

    Returns ``(qualifies, covered_areas, positive_globs)``.

    A bare ``pull_request:`` (no ``paths``) explicitly does NOT qualify: it is
    the unfiltered trigger deliberately removed on 2026-07-20 to stop a 6-hour
    ASan run being kicked off by every commit on the single-capacity
    self-hosted runner. Negated globs (``!...``) are exclusions and never count
    as coverage.
    """
    areas = [a.replace("\\", "/").rstrip("/") for a in source_areas]
    covered: List[str] = []
    positives: List[str] = []
    for trigger in ("pull_request", "pull_request_target"):
        block = on_block.get(trigger)
        if not isinstance(block, dict):
            continue
        paths = block.get("paths") or []
        if isinstance(paths, str):
            paths = [paths]
        for glob in paths:
            g = str(glob).replace("\\", "/").strip()
            if not g or g.startswith("!"):
                continue
            positives.append(g)
    for area in areas:
        if any(g == area or g.startswith(area + "/") or g.rstrip("/*") == area
               for g in positives):
            covered.append(area)
    return (bool(areas) and len(covered) == len(areas)), covered, positives



# ---------------------------------------------------------------------------
# The MANUAL-ONLY posture (2026-08-13).
#
# The owner's instruction is that this repository runs no scheduled jobs. That
# does not make the ASan detection window bounded -- it makes it UNBOUNDED, and
# this checker must keep saying so rather than being taught that unbounded is
# fine. What it accepts instead is that the cost has been WRITTEN DOWN: the
# posture document states, in terms, that a UAF on the sanitizer-only surface
# now lives until somebody dispatches the lane.
#
# Both conditions are required, and they fail in opposite directions on purpose:
#   * no workflow may still carry a `schedule:` key -- if one does, a cadence is
#     live and must be modelled, so an empty analysis means the derivation broke;
#   * the posture document must exist AND carry its marker -- so the state
#     cannot drift back to "nobody decided this" while the gate keeps passing.
#
# A path-filtered pull_request trigger still qualifies on its own merits above.
# It was considered for this repository and rejected as theatre (direct pushes
# to master mean it would fire approximately never); that reasoning lives in the
# posture document, not here.
# ---------------------------------------------------------------------------
POSTURE_DOC = "docs/audit/ci-manual-only-posture.md"
POSTURE_MARKER = "PDFE-CI-POSTURE: MANUAL-ONLY"


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _workflows_with_schedule() -> List[str]:
    """Every workflow file that still carries a top-level `schedule:` key."""
    root = _repo_root() / ".github" / "workflows"
    found: List[str] = []
    for f in sorted(root.glob("*.yml")):
        try:
            body = f.read_text(encoding="utf-8")
        except OSError:
            continue
        for line in body.splitlines():
            if re.match(r"^\s{0,4}schedule:\s*$", line):
                found.append(f.name)
                break
    return found


def manual_only_posture() -> Tuple[bool, str]:
    """(accepted, why). Accepted only when nothing is scheduled AND the posture
    document says so."""
    stray = _workflows_with_schedule()
    if stray:
        return False, ("%d workflow(s) still carry a `schedule:` trigger (%s), "
                       "so a cadence is live and must be modelled"
                       % (len(stray), ", ".join(stray)))
    doc = _repo_root() / POSTURE_DOC
    if not doc.exists():
        return False, ("nothing is scheduled, but %s does not exist -- an "
                       "undocumented posture is drift, not a decision"
                       % POSTURE_DOC)
    try:
        body = doc.read_text(encoding="utf-8")
    except OSError as exc:
        return False, "%s could not be read: %s" % (POSTURE_DOC, exc)
    if POSTURE_MARKER not in body:
        return False, ("%s exists but does not carry the %r marker"
                       % (POSTURE_DOC, POSTURE_MARKER))
    return True, ("no workflow carries a `schedule:` trigger and %s records the "
                  "manual-only posture and its cost" % POSTURE_DOC)

def evaluate(on_block: Optional[Dict],
             max_allowed_gap_days: int = DEFAULT_MAX_GAP_DAYS,
             source_areas: Sequence[str] = DEFAULT_SOURCE_AREAS,
             start: _dt.date = DEFAULT_SPAN_START,
             span_days: int = DEFAULT_SPAN_DAYS) -> Tuple[bool, List[str]]:
    """Pure decision core. Return ``(ok, messages)``."""
    msgs: List[str] = []
    if on_block is None:
        return False, ["FAIL: no trigger (`on:`) block found in the workflow — "
                       "note PyYAML parses the unquoted key `on:` as boolean "
                       "True, so a lookup of doc['on'] reports a false absence."]

    crons = schedule_crons(on_block)
    cadence_ok = False
    if crons:
        try:
            mx, mn, count = max_gap_days(crons, start, span_days)
        except CronError as exc:
            return False, ["FAIL: uninterpretable cron expression (%s). An "
                           "unparseable schedule is a failure, never an "
                           "assumed pass." % exc]
        if mx is None:
            msgs.append("FAIL: schedule %s fires %d time(s) in %d days — the "
                        "detection window is effectively unbounded."
                        % (crons, count, span_days))
        else:
            msgs.append("schedule %s -> %d firings over %d days from %s; "
                        "max gap %.2f d, min gap %.2f d (bound %d d)"
                        % (crons, count, span_days, start.isoformat(),
                           mx, mn, max_allowed_gap_days))
            if mx <= max_allowed_gap_days:
                cadence_ok = True
            else:
                msgs.append("FAIL(cadence): max gap %.2f d exceeds the %d d "
                            "bound." % (mx, max_allowed_gap_days))
    else:
        msgs.append("no `schedule:` cron entries found.")

    pr_ok, covered, positives = qualifying_pr_paths(on_block, source_areas)
    if pr_ok:
        msgs.append("qualifying path-filtered PR trigger covers %s (globs: %s)"
                    % (", ".join(covered), ", ".join(positives)))
    elif positives:
        missing = [a for a in source_areas if a not in covered]
        msgs.append("path-filtered PR trigger present but does NOT cover %s "
                    "(globs: %s)" % (", ".join(missing), ", ".join(positives)))
    elif any(isinstance(on_block.get(t), (dict, type(None))) and t in on_block
             for t in ("pull_request", "pull_request_target")):
        msgs.append("a `pull_request` trigger exists but carries no `paths` "
                    "filter — an UNFILTERED PR trigger does not qualify (it "
                    "re-creates the per-commit ASan cost removed 2026-07-20).")
    else:
        msgs.append("no path-filtered PR trigger.")

    ok = cadence_ok or pr_ok
    if ok:
        msgs.append("OK: ASan detection window is bounded (%s)."
                    % ("cadence" if cadence_ok else "path-filtered PR trigger"))
    else:
        msgs.append("FAIL: neither a cadence within %d day(s) nor a qualifying "
                    "path-filtered PR trigger over %s. The ASan lane's "
                    "detection window is not bounded."
                    % (max_allowed_gap_days, ", ".join(source_areas)))
    # NOTE: this function stays PURE, deliberately. The manual-only posture
    # (2026-08-13) is repository-global state, and consulting it HERE made every
    # synthetic negative case in the self-tests pass -- the controls that prove
    # this checker discriminates were silently disarmed by their own subject.
    # Caught by those self-tests going green-when-they-must-be-red. The posture
    # is applied by main(), against the live tree, where it belongs.
    return ok, msgs


def load_workflow(path: Path):
    """Load a workflow. Text is read with explicit UTF-8 and universal newlines
    so a CRLF working tree (core.autocrlf=true) parses identically to LF."""
    with open(path, "r", encoding="utf-8") as fh:
        return yaml.safe_load(fh)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--workflow", type=Path,
                    default=Path(".github/workflows/windows-sanitizer.yml"))
    ap.add_argument("--max-gap-days", type=int, default=DEFAULT_MAX_GAP_DAYS)
    ap.add_argument("--source-area", action="append", dest="source_areas",
                    default=None,
                    help="Repo-relative source area a qualifying PR paths "
                         "filter must cover (repeatable). Default: %s"
                         % ", ".join(DEFAULT_SOURCE_AREAS))
    ap.add_argument("--span-days", type=int, default=DEFAULT_SPAN_DAYS)
    args = ap.parse_args(argv)

    try:
        doc = load_workflow(args.workflow)
    except OSError as exc:
        print("[asan-cadence] cannot read workflow: %s" % exc, file=sys.stderr)
        return 2
    except yaml.YAMLError as exc:
        print("[asan-cadence] YAML parse error in %s: %s" % (args.workflow, exc),
              file=sys.stderr)
        return 2

    areas = tuple(args.source_areas) if args.source_areas else DEFAULT_SOURCE_AREAS
    ok, msgs = evaluate(extract_on_block(doc), args.max_gap_days, areas,
                        DEFAULT_SPAN_START, args.span_days)

    # The manual-only posture is applied HERE, against the live tree, and only
    # after the pure decision has already said "not bounded". Keeping it out of
    # evaluate() is not tidiness: when it lived in there, every synthetic
    # negative case in the self-tests started passing, because the repository's
    # own posture answered a question the fixtures were asking about themselves.
    if not ok:
        accepted, why = manual_only_posture()
        if accepted:
            ok = True
            msgs.append("PASS (manual-only posture): %s." % why)
            msgs.append("THE WINDOW IS UNBOUNDED, NOT BOUNDED. windows-sanitizer "
                        "is the only lane built with -fsanitize=address and "
                        "defects #47/#48 were ASan-ONLY finds, so a UAF on that "
                        "surface lives until somebody DISPATCHES the lane. This "
                        "passes on the cost being WRITTEN DOWN, not on the risk "
                        "being absent -- dispatch it after any change to %s."
                        % ", ".join(areas))
        else:
            msgs.append("manual-only posture NOT accepted: %s" % why)

    print("[asan-cadence] workflow: %s" % args.workflow)
    for m in msgs:
        print("[asan-cadence] " + m)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
