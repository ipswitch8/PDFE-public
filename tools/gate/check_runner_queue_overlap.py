#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_runner_queue_overlap.py — the shared self-hosted runner's queue
overlap must stay inside the exposure this project has explicitly accepted.

Meta-audit follow-up **F38**, closing the deferred-backlog row
*"governance can queue behind a lane whose tail exceeds its own timeout"*
(the ``120 > 90`` arithmetic written into ``.github/workflows/governance.yml``
and ``.github/workflows/mutation-kill.yml`` by F30).

--------------------------------------------------------------------------
WHAT F38 ESTABLISHED FIRST, because it changes what needs enforcing
--------------------------------------------------------------------------
Both workflow headers recorded, and explicitly flagged as **UNVERIFIED**, the
claim that GitHub's ``timeout-minutes`` clock starts when a job begins
EXECUTING rather than when it is queued. F38 verified it, two ways:

* **GitHub documentation** — ``timeout-minutes`` applies to job execution;
  a job that cannot find a matching online runner stays queued and is subject
  to a separate limit, being cancelled after ~24 h in queue.
* **This repository's own run records** — run ``30959077894`` (`governance`,
  scheduled 2026-08-04T23:10:16Z) sat queued for **23 h 59 m** with
  ``timeout-minutes: 90`` and did **not** time out. It ended ``cancelled``
  with an EMPTY ``steps`` list — it never executed a single step. A 90-minute
  timeout that does not fire after 1439 minutes is a direct measurement that
  the clock does not run while queued.

**Consequence: the ``120 > 90`` hazard as originally described does not
exist.** A governance run cannot exhaust its ``timeout-minutes`` while
waiting. Raising that timeout would therefore have fixed nothing, and this
check deliberately does NOT gate on it.

What remains real is **lateness**: a governance run queued behind another
lane's tail executes late, and the only limit that can actually kill it is
the ~1440-minute queue-cancellation cliff. That is what is enforced here.

--------------------------------------------------------------------------
WHAT THIS CHECK ASSERTS
--------------------------------------------------------------------------
Two models, both re-derived from the LIVE ``schedule.cron`` and
``timeout-minutes`` values in the workflow files — never from a hard-coded
number, so an edit to any cron keeps the arithmetic honest.

**Model 1 — PAIRWISE TAIL (this is the gate).** For every other lane on the
shared runner, and every day both lanes fire, the residual tail the other
lane can still be occupying when the target lane fires:

    tail = (blocker_firing + blocker_timeout_minutes) - target_firing

The maximum over all pairs is the target lane's worst-case *direct* queue
delay. It must be ``<= --max-queue-minutes`` (the accepted exposure).

**Model 2 — SERIAL CASCADE (diagnostic) + a SUBSCRIPTION ratchet (gated).**
The pairwise model understates reality, because the runner is single-capacity
and a blocked lane blocks the next one in turn. Model 2 is a single-server
FIFO simulation of every lane, with each job taking its full
``timeout-minutes``.

Running it produced F38's second finding, which the pairwise figure alone had
kept invisible for three phases: **at worst-case durations this runner is
OVER-subscribed.** ``windows-fuzz`` (600) plus ``windows-validation`` (720)
is already 1320 of every 1440 minutes, and the full lane set totals ~1599
minutes of work per day — **111% of a day**. A queue offered more work than
it can serve has no steady state, so the simulated delay does not converge:
it grows without bound (~11,000 minutes by day 70) for *every* lane.

That has a sharp consequence for how this check may be written. **The
cascade delay is deliberately NOT gated.** A threshold on a divergent
quantity is red in every configuration, including every correct one, and a
gate that can never pass is not a gate — it is a broken build that teaches
people to ignore it. It would also be gating a scenario that does not occur:
``timeout-minutes`` are ceilings, not durations, and these lanes have
historically completed in a fraction of them.

What IS gated from Model 2 is the input that drives it and is perfectly
stable: the **daily worst-case subscription**, i.e. the sum of every lane's
firings times its timeout, per day. It is pinned at
``--max-daily-subscription-minutes`` so that adding a lane, lengthening a
timeout, or tightening a cadence cannot quietly push the runner further past
saturation without a decision. The current value is accepted, not endorsed —
see the risk acceptance in ``governance.yml``'s header. The real remedy is
runner capacity, which no schedule change can substitute for.

Anti-cheat properties, because a checker that cannot fail is decoration:

1. **Values are PARSED, not asserted.** The crons are expanded into real
   firing datetimes and the timeouts read from the jobs. Editing a comment
   cannot change the verdict; editing a cron does.
2. **A lane that cannot be interpreted is a FAILURE, never an assumed pass.**
   No schedule, an unparseable cron, or a missing ``timeout-minutes`` on a
   shared-runner job all fail loudly.
3. **Only jobs on the SHARED runner count.** Lane membership is decided by
   the ``runs-on`` labels containing ``pdfe-windows``, so moving a job off
   the shared host genuinely removes it from the arithmetic, and a job that
   is added to the shared host is picked up automatically.
4. **The bound is a ratchet, not a rubber stamp.** The committed fixture
   ``fixtures/governance-2200-prefix.yml`` is the real pre-F38 governance
   slot and it FAILS this check (120 > 60); the live slot passes at 0.

GitHub Actions gotcha handled here: PyYAML parses the unquoted workflow key
``on:`` as the **boolean** ``True`` under YAML 1.1 truthiness, not the string
``"on"``. ``doc["on"]`` therefore reports a false absence on every real
workflow and would make this check pass vacuously. ``extract_on_block`` is
reused from ``check_asan_cadence`` and handles it.

The pure decision core is ``evaluate`` (no I/O) for direct unit testing.

Usage:
  python tools/gate/check_runner_queue_overlap.py
      [--workflow PATH ...] [--target governance]
      [--max-queue-minutes 60] [--queue-cancel-minutes 1440]
Exit 0 = within the accepted exposure; 1 = exposure exceeded; 2 = usage, IO
or parse error.
"""
from __future__ import annotations

import argparse
import datetime as _dt
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

try:
    import yaml
except ImportError:  # pragma: no cover - environment problem, not a policy result
    print("[queue-overlap] PyYAML is required (python -m pip install pyyaml)",
          file=sys.stderr)
    raise

from check_asan_cadence import (  # noqa: E402  (path set above)
    CronError,
    CronSpec,
    extract_on_block,
    schedule_crons,
)

# The label that identifies the single shared self-hosted host.
SHARED_RUNNER_LABEL = "pdfe-windows"

# The lanes that live on it. Paths are repo-relative; the caller may override.
DEFAULT_WORKFLOWS: Tuple[str, ...] = (
    ".github/workflows/governance.yml",
    ".github/workflows/windows-sanitizer.yml",
    ".github/workflows/mutation-kill.yml",
    ".github/workflows/windows-fuzz.yml",
    ".github/workflows/windows-validation.yml",
)

DEFAULT_TARGET = "governance"

# ACCEPTED MAXIMUM EXPOSURE (F38). The target lane's worst-case *direct*
# queue delay behind any single other lane. 60 minutes, not 120: the live
# configuration achieves 0, so this is a ratchet with an hour of deliberate
# headroom rather than a bound drawn around whatever happens to be true.
DEFAULT_MAX_QUEUE_MINUTES = 60

# GitHub cancels a job that has been queued for a self-hosted runner for
# ~24 h. This is the only limit that can actually kill a queued run, now that
# `timeout-minutes` is verified not to run while queued. Reported for context;
# the cascade delay it would apply to is divergent and so is NOT gated.
DEFAULT_QUEUE_CANCEL_MINUTES = 1440

# ACCEPTED DAILY WORST-CASE SUBSCRIPTION (F38). Sum over lanes of
# firings x timeout-minutes, per day. The live value is 1599 min against a
# 1440-min day (111%) — the runner is over-subscribed at worst-case
# durations. 1600 pins it: this is accepted as the cost of the current lane
# set, and any change that pushes it higher must be a decision, not a drift.
DEFAULT_MAX_DAILY_SUBSCRIPTION_MINUTES = 1600

# Enumeration span. 70 days covers every weekday/weekend phase combination of
# the daily, Sun/Tue/Thu and Saturday lanes, starting on a Sunday so the
# weekly patterns are exercised from their first firing.
DEFAULT_SPAN_START = _dt.date(2026, 1, 4)   # a Sunday
DEFAULT_SPAN_DAYS = 70


class Lane:
    """One workflow's presence on the shared runner."""

    __slots__ = ("name", "crons", "timeout_minutes")

    def __init__(self, name: str, crons: Sequence[str], timeout_minutes: int) -> None:
        self.name = name
        self.crons = list(crons)
        self.timeout_minutes = timeout_minutes

    def firings(self, start: _dt.date, span_days: int) -> List[_dt.datetime]:
        out: List[_dt.datetime] = []
        for raw in self.crons:
            out.extend(CronSpec(raw).firings(start, span_days))
        return sorted(set(out))

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return "Lane(%r, %r, %d)" % (self.name, self.crons, self.timeout_minutes)


def _runs_on_labels(runs_on) -> List[str]:
    if runs_on is None:
        return []
    if isinstance(runs_on, str):
        return [runs_on]
    if isinstance(runs_on, list):
        return [str(x) for x in runs_on]
    if isinstance(runs_on, dict):
        # `runs-on: {group: ..., labels: [...]}`
        labels = runs_on.get("labels") or []
        if isinstance(labels, str):
            labels = [labels]
        out = [str(x) for x in labels]
        if runs_on.get("group"):
            out.append(str(runs_on["group"]))
        return out
    return [str(runs_on)]


def lane_from_document(name: str, doc,
                       shared_label: str = SHARED_RUNNER_LABEL
                       ) -> Tuple[Optional[Lane], List[str]]:
    """Build a :class:`Lane` from a parsed workflow, or explain why not.

    Returns ``(lane_or_None, problems)``. A workflow with no job on the shared
    runner yields ``(None, [])`` — legitimately not part of this arithmetic.
    A workflow that IS on the shared runner but cannot be interpreted yields
    ``(None, [problem, ...])``, which the caller must treat as a failure.
    """
    problems: List[str] = []
    if not isinstance(doc, dict):
        return None, ["%s: not a YAML mapping" % name]

    jobs = doc.get("jobs")
    if not isinstance(jobs, dict):
        return None, []

    shared_timeouts: List[int] = []
    on_shared = False
    for job_id, job in jobs.items():
        if not isinstance(job, dict):
            continue
        labels = [l.strip() for l in _runs_on_labels(job.get("runs-on"))]
        if shared_label not in labels:
            continue
        on_shared = True
        tmo = job.get("timeout-minutes")
        if tmo is None:
            problems.append(
                "%s: job %r runs on the shared %r runner but declares no "
                "`timeout-minutes` — its worst-case tail is unbounded and "
                "cannot be reasoned about." % (name, job_id, shared_label))
            continue
        try:
            shared_timeouts.append(int(tmo))
        except (TypeError, ValueError):
            problems.append("%s: job %r has a non-integer `timeout-minutes` "
                            "(%r)." % (name, job_id, tmo))

    if not on_shared:
        return None, []
    if problems:
        return None, problems

    on_block = extract_on_block(doc)
    if on_block is None:
        return None, ["%s: no trigger (`on:`) block found. Note PyYAML parses "
                      "the unquoted key `on:` as boolean True, so a lookup of "
                      "doc['on'] reports a false absence." % name]
    crons = schedule_crons(on_block)
    if not crons:
        # Dispatch-only lanes cannot contribute a *scheduled* collision.
        return None, []
    try:
        for raw in crons:
            CronSpec(raw)
    except CronError as exc:
        return None, ["%s: uninterpretable cron (%s). An unparseable schedule "
                      "is a failure, never an assumed pass." % (name, exc)]

    return Lane(name, crons, max(shared_timeouts)), []


# ---------------------------------------------------------------------------
# Model 1 — pairwise tail
# ---------------------------------------------------------------------------

def pairwise_worst_tails(lanes: Sequence[Lane], target: str,
                         start: _dt.date = DEFAULT_SPAN_START,
                         span_days: int = DEFAULT_SPAN_DAYS
                         ) -> Dict[str, Tuple[float, Optional[_dt.datetime]]]:
    """Worst residual tail each other lane can impose on ``target``.

    ``{blocker_name: (max_tail_minutes, target_firing_at_which_it_occurs)}``.
    A blocker that never overlaps yields ``(0.0, None)``.
    """
    tgt = next((l for l in lanes if l.name == target), None)
    if tgt is None:
        return {}
    tgt_firings = tgt.firings(start, span_days)
    out: Dict[str, Tuple[float, Optional[_dt.datetime]]] = {}
    for lane in lanes:
        if lane.name == target:
            continue
        best = 0.0
        best_at: Optional[_dt.datetime] = None
        ends = [f + _dt.timedelta(minutes=lane.timeout_minutes)
                for f in lane.firings(start, span_days)]
        for tf in tgt_firings:
            for bf, be in zip(lane.firings(start, span_days), ends):
                if bf <= tf < be:
                    tail = (be - tf).total_seconds() / 60.0
                    if tail > best:
                        best, best_at = tail, tf
        out[lane.name] = (best, best_at)
    return out


# ---------------------------------------------------------------------------
# Model 2 — serial single-capacity FIFO cascade
# ---------------------------------------------------------------------------

def daily_subscription_minutes(lanes: Sequence[Lane],
                               start: _dt.date = DEFAULT_SPAN_START,
                               span_days: int = DEFAULT_SPAN_DAYS) -> float:
    """Worst-case minutes of work offered to the runner per day.

    Sum over lanes of ``firings x timeout-minutes`` divided by the span. Above
    1440 the runner cannot serve its own schedule at worst-case durations.
    """
    if span_days <= 0:
        return 0.0
    total = sum(lane.timeout_minutes * len(lane.firings(start, span_days))
                for lane in lanes)
    return total / float(span_days)


def serial_cascade_delays(lanes: Sequence[Lane],
                          start: _dt.date = DEFAULT_SPAN_START,
                          span_days: int = DEFAULT_SPAN_DAYS
                          ) -> Dict[str, float]:
    """Worst-case queue delay per lane on a SINGLE-capacity runner, FIFO.

    Every job is assumed to take its full ``timeout-minutes``. Returns
    ``{lane_name: max_delay_minutes}``.
    """
    events: List[Tuple[_dt.datetime, str, int]] = []
    for lane in lanes:
        for f in lane.firings(start, span_days):
            events.append((f, lane.name, lane.timeout_minutes))
    events.sort(key=lambda e: (e[0], e[1]))

    free = _dt.datetime(start.year, start.month, start.day)
    worst: Dict[str, float] = {lane.name: 0.0 for lane in lanes}
    for enqueued, name, tmo in events:
        began = max(free, enqueued)
        delay = (began - enqueued).total_seconds() / 60.0
        if delay > worst[name]:
            worst[name] = delay
        free = began + _dt.timedelta(minutes=tmo)
    return worst


# ---------------------------------------------------------------------------
# decision core
# ---------------------------------------------------------------------------

def evaluate(lanes: Sequence[Lane],
             problems: Sequence[str] = (),
             target: str = DEFAULT_TARGET,
             max_queue_minutes: int = DEFAULT_MAX_QUEUE_MINUTES,
             queue_cancel_minutes: int = DEFAULT_QUEUE_CANCEL_MINUTES,
             max_daily_subscription_minutes: int = DEFAULT_MAX_DAILY_SUBSCRIPTION_MINUTES,
             start: _dt.date = DEFAULT_SPAN_START,
             span_days: int = DEFAULT_SPAN_DAYS) -> Tuple[bool, List[str]]:
    """Pure decision core. Return ``(ok, messages)``."""
    msgs: List[str] = []
    if problems:
        return False, ["FAIL: " + p for p in problems]

    if not lanes:
        # PURITY, deliberately: this core does NOT consult the repository's own
        # posture. When it did (2026-08-13, briefly), this project's own
        # "an empty model is not a pass" control started passing — the subject
        # of the question answered it. main() applies the manual-only posture
        # against the live tree instead.
        return False, ["FAIL: no lane on the shared %r runner could be "
                       "derived. An empty model is not a pass."
                       % SHARED_RUNNER_LABEL]
    if not any(l.name == target for l in lanes):
        return False, ["FAIL: target lane %r is not among the derived shared-"
                       "runner lanes (%s). Either it left the shared runner "
                       "or it lost its schedule; both need a decision, not a "
                       "silent pass." % (target, ", ".join(l.name for l in lanes))]

    msgs.append("shared runner %r — lanes derived from live values:"
                % SHARED_RUNNER_LABEL)
    for lane in sorted(lanes, key=lambda l: l.name):
        msgs.append("  %-20s cron=%-18s timeout=%4d min"
                    % (lane.name, ",".join(lane.crons), lane.timeout_minutes))

    ok = True

    # -- Model 1: the gate -------------------------------------------------
    tails = pairwise_worst_tails(lanes, target, start, span_days)
    msgs.append("MODEL 1 — worst-case PAIRWISE queue delay for %r "
                "(accepted bound %d min):" % (target, max_queue_minutes))
    worst_tail = 0.0
    worst_blocker = None
    for blocker in sorted(tails):
        tail, at = tails[blocker]
        msgs.append("  behind %-20s %6.0f min%s"
                    % (blocker, tail,
                       ("  (worst at %s)" % at.isoformat(sep=" ")) if at else "  (never overlaps)"))
        if tail > worst_tail:
            worst_tail, worst_blocker = tail, blocker
    msgs.append("  => worst pairwise delay %.0f min%s"
                % (worst_tail, (" (behind %s)" % worst_blocker) if worst_blocker else ""))
    if worst_tail > max_queue_minutes:
        ok = False
        msgs.append("FAIL(pairwise): %r can queue %.0f min behind %s, which "
                    "exceeds the accepted exposure of %d min."
                    % (target, worst_tail, worst_blocker, max_queue_minutes))

    # -- Model 2: cascade reported (divergent, NOT gated); subscription gated
    subscription = daily_subscription_minutes(lanes, start, span_days)
    cascade = serial_cascade_delays(lanes, start, span_days)
    msgs.append("MODEL 2 — single-capacity runner load:")
    msgs.append("  daily worst-case subscription: %.0f min of 1440 (%.0f%%) "
                "(accepted ceiling %d min)"
                % (subscription, 100.0 * subscription / 1440.0,
                   max_daily_subscription_minutes))
    saturated = subscription > 1440.0
    msgs.append("  serial-cascade delay over %d days (DIAGNOSTIC ONLY — %s):"
                % (span_days,
                   "DIVERGENT, the runner is over-subscribed so this grows "
                   "without bound and is not a gateable quantity" if saturated
                   else "convergent"))
    for name in sorted(cascade):
        flag = "  >= %d-min queue-cancellation cliff" % queue_cancel_minutes \
            if cascade[name] >= queue_cancel_minutes else ""
        msgs.append("    %-20s %7.0f min%s" % (name, cascade[name], flag))
    if saturated:
        msgs.append("  NOTE: worst-case subscription exceeds a 24-hour day. "
                    "At `timeout-minutes` ceilings this runner cannot serve "
                    "its own schedule; only added capacity fixes that, not a "
                    "slot change. Timeouts are ceilings rather than expected "
                    "durations, which is why this is a recorded exposure and "
                    "not a live outage — see the risk acceptance in "
                    "governance.yml's header.")
    if subscription > max_daily_subscription_minutes:
        ok = False
        msgs.append("FAIL(subscription): daily worst-case subscription %.0f "
                    "min exceeds the accepted ceiling of %d min. The shared "
                    "runner has been pushed further past saturation; that "
                    "needs a decision (capacity, or a lane leaving the host), "
                    "not a wider bound."
                    % (subscription, max_daily_subscription_minutes))

    if ok:
        msgs.append("OK: %r stays within the accepted %d-min pairwise exposure "
                    "and the daily worst-case subscription (%.0f min) is "
                    "within the accepted %d-min ceiling."
                    % (target, max_queue_minutes, subscription,
                       max_daily_subscription_minutes))
    return ok, msgs


def load_workflow(path: Path):
    """Load a workflow with explicit UTF-8 and universal newlines, so a CRLF
    working tree (``core.autocrlf=true``) parses identically to LF."""
    with open(path, "r", encoding="utf-8") as fh:
        return yaml.safe_load(fh)


def build_lanes(paths: Sequence[Path]) -> Tuple[List[Lane], List[str]]:
    lanes: List[Lane] = []
    problems: List[str] = []
    for p in paths:
        try:
            doc = load_workflow(p)
        except OSError as exc:
            problems.append("cannot read %s: %s" % (p, exc))
            continue
        except yaml.YAMLError as exc:
            problems.append("YAML parse error in %s: %s" % (p, exc))
            continue
        lane, probs = lane_from_document(p.stem, doc)
        problems.extend(probs)
        if lane is not None:
            lanes.append(lane)
    return lanes, problems



def _workflows_with_schedule():
    """Every workflow file that still carries a top-level `schedule:` key.

    Deliberately a TEXT scan of all of .github/workflows, not of the five lanes
    this check models: the question it answers is "can any timer still fire?",
    and a cron restored on a sixth workflow is exactly the case that must not
    slip through.
    """
    import re as _re
    root = Path(__file__).resolve().parents[2] / ".github" / "workflows"
    found = []
    for f in sorted(root.glob("*.yml")):
        try:
            body = f.read_text(encoding="utf-8")
        except OSError:
            continue
        for line in body.splitlines():
            if _re.match(r"^\s{0,4}schedule:\s*$", line):
                found.append(f.name)
                break
    return found

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--workflow", action="append", dest="workflows", default=None,
                    type=Path, help="Workflow file to include (repeatable). "
                                    "Default: the five shared-runner lanes.")
    ap.add_argument("--target", default=DEFAULT_TARGET,
                    help="Lane whose queue exposure is gated (default: %(default)s).")
    ap.add_argument("--max-queue-minutes", type=int,
                    default=DEFAULT_MAX_QUEUE_MINUTES)
    ap.add_argument("--queue-cancel-minutes", type=int,
                    default=DEFAULT_QUEUE_CANCEL_MINUTES)
    ap.add_argument("--max-daily-subscription-minutes", type=int,
                    default=DEFAULT_MAX_DAILY_SUBSCRIPTION_MINUTES)
    ap.add_argument("--span-days", type=int, default=DEFAULT_SPAN_DAYS)
    args = ap.parse_args(argv)

    paths = args.workflows if args.workflows else [Path(p) for p in DEFAULT_WORKFLOWS]
    lanes, problems = build_lanes(paths)
    ok, msgs = evaluate(lanes, problems, args.target, args.max_queue_minutes,
                        args.queue_cancel_minutes,
                        args.max_daily_subscription_minutes,
                        DEFAULT_SPAN_START, args.span_days)
    # The MANUAL-ONLY posture (2026-08-13), applied HERE and not in evaluate():
    # "no lanes" has two causes and only one of them is benign.
    if not ok and not lanes and not problems:
        from check_asan_cadence import manual_only_posture  # one shared rule
        accepted, why = manual_only_posture()
        if accepted:
            ok = True
            msgs = [
                "PASS (manual-only posture): %s." % why,
                "No timer can queue two lanes onto the shared %r runner, so the "
                "overlap this check bounds is UNREACHABLE rather than small."
                % SHARED_RUNNER_LABEL,
                "NOTE: that removes a TIMING hazard, not a CAPACITY one. A human "
                "who dispatches two lanes at once still serialises them on a "
                "single-capacity runner.",
            ]
        else:
            msgs.append("manual-only posture NOT accepted: %s" % why)

    print("[queue-overlap] workflows: %s" % ", ".join(str(p) for p in paths))
    for m in msgs:
        print("[queue-overlap] " + m)
    return 0 if ok else 1


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
