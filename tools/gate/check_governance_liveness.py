#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_governance_liveness.py — does the daily ``governance`` lane actually RUN?

Meta-audit follow-up **F44**, closing deferred-backlog rows **F38-b** (the
governance liveness alarm does not alarm) and the detection half of **F46**
(the self-hosted runner is manual and its watchdog is dead).

===========================================================================
THE DEFECT THIS REPLACES
===========================================================================
``.github/workflows/governance.yml`` was given a second job by F5 / SEC-31:
be the daily *runner liveness probe*. The stated mechanism was

    "if the runner is offline or saturated, this run never picks up and
     TIMES OUT RED, which is the alert"

and ``timeout-minutes`` was raised 20 -> 90 to serve it.

**That mechanism does not exist.** ``timeout-minutes`` governs EXECUTION; it
does not run while a job is queued. An unserviced scheduled run therefore sits
in the queue and is eventually ``cancelled`` — by the ~24-hour queue-
cancellation limit, or sooner by this lane's own ``cancel-in-progress: true``
when the next day's run supersedes it. ``cancelled`` is not a failure. It
notifies nobody. The alarm was silent by construction.

Measured, not assumed (``gh run list --workflow=governance.yml`` on
2026-08-08, the six runs GitHub still lists — the whole visible history):

    31237514233  2026-08-08T03:34:53Z -> 03:36:04Z   failure     (71 s)
    31138298205  2026-08-07T01:31:32Z -> 21:13:10Z   failure     (1181 min)
    31055369961  2026-08-05T23:09:26Z -> 23:09:27Z+1d cancelled  (1440 min)
    30959077894  2026-08-04T23:10:16Z -> 23:09:27Z+1d cancelled  (1439 min)
    30861403874  2026-08-03T23:11:31Z -> 23:10:18Z+1d cancelled  (1439 min)
    30771356343  2026-08-02T23:01:08Z -> 23:01:09Z+1d cancelled  (1440 min)

``gh run list --workflow=governance.yml --status=success`` returns **``[]``**.
Since F5 scheduled it, this job has never once succeeded and never once
alerted. Run ``30959077894`` carried ``timeout-minutes: 90`` and did not time
out after **1439 minutes** — a direct measurement that the clock does not run
while queued.

===========================================================================
WHAT THIS CHECK ASSERTS
===========================================================================
The only signal that cannot be faked by a queue is a **recent genuine
success**. So the classification is deliberately positive-evidence-only:

    OK     <=> the history contains at least one run of the target workflow
               whose conclusion is `success` and whose completion is no older
               than --max-success-age-hours.
    ALARM  <=> anything else.

Every other outcome — no runs at all, an unparseable-but-present history with
zero matches, runs that only ever queued-and-cancelled, a success that has
gone stale — is ALARM. There is no input for which "no data" reads as
healthy. That is the whole point: **no data is exactly what a dead CI
produces**, and this project's defect-#50 history makes fail-open the pattern
to hunt. An input that cannot be parsed at all is a hard ERROR (exit 2), never
OK either.

ALARM carries a machine-readable reason so the alert says which failure it is:

    no-runs           the history holds zero runs of the target workflow
    never-succeeded   runs exist; none has ever concluded `success`
    stale-success     the newest success is older than the window

and an orthogonal list of observed PATTERNS, which is where the F38-b
signature is named explicitly:

    queued-then-cancelled  a `cancelled` run whose created->updated span is
                           >= --queue-cancel-minutes. Governance's own
                           `timeout-minutes` is 90, so a cancelled run
                           spanning ~24 h cannot have been EXECUTING; the only
                           mechanism that produces it is the queue-
                           cancellation cliff. This is the exact shape of run
                           30959077894 and is the state F38-b describes.
    currently-queued       the newest run is still `queued` / `in_progress`.

===========================================================================
WHY 48 HOURS
===========================================================================
``--max-success-age-hours`` defaults to **48**, and the number is derived, not
picked:

* ``governance.yml`` fires on ``cron: '0 2 * * *'`` — nominally every 1440
  minutes.
* GitHub's scheduled dispatch is late under load; this repository has measured
  ~70 minutes of lateness (cron 22:00, runs created 23:01-23:11 on four
  consecutive nights — visible in the evidence table above). So the real
  inter-run gap can exceed 24 h without anything being wrong.
* One entirely missed day is survivable and can have a benign cause (the
  single self-hosted runner busy inside ``windows-validation``'s 720-minute
  ceiling). Two consecutive misses cannot.

48 h = one tolerated miss plus the measured jitter. It is a parameter, not a
constant: pass ``--max-success-age-hours`` to state a different tolerance
explicitly rather than editing a magic number.

===========================================================================
WHERE THIS MUST RUN — the load-bearing constraint
===========================================================================
**A probe hosted on the machine it monitors cannot report that machine being
down.** Running this check on ``pdfe-runner-01`` would inherit the exact failure
it exists to detect, which is how F38-b happened in the first place. It is
therefore wired into ``.github/workflows/ci-liveness.yml``, which runs on
GitHub-hosted ``ubuntu-latest`` — off-host, with no dependency on the
self-hosted runner — and, fixture-driven, into the ``fast-checks.yml``
"Gate self-tests" step (also ``ubuntu-latest``).

===========================================================================
FIXTURE MODE vs LIVE MODE
===========================================================================
``--runs FILE`` reads a committed JSON array in ``gh run list --json`` shape.
``--from-gh`` shells out to ``gh`` and reads the same shape from its stdout.
**Both funnel into the same** :func:`parse_runs` **and** :func:`classify`,
so the committed fixtures exercise the production path rather than a parallel
one. `gh` is not on PATH on this host; ``--gh-path`` defaults to
``C:\\Users\\dev\\bin\\gh`` when that exists, else ``gh``.

The self-tests are fixture-driven on purpose (criterion 1): they must be
runnable from the working tree with no network and no runner, and a test that
called ``gh`` would be testing GitHub's availability, not this logic.

CRLF NOTE: ``core.autocrlf=true``. Everything here reads JSON with universal
newlines and compares parsed values, never bytes.

Exit: 0 OK; 1 ALARM; 2 usage / IO / parse error.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence

REPO_ROOT = Path(__file__).resolve().parents[2]

DEFAULT_WORKFLOW_NAME = "Governance"
DEFAULT_WORKFLOW_FILE = "governance.yml"

# See "WHY 48 HOURS" above.
DEFAULT_MAX_SUCCESS_AGE_HOURS = 48.0

# See "queued-then-cancelled" above. governance.yml's timeout-minutes is 90, so
# 1200 minutes (20 h) is far outside any possible EXECUTION span and squarely
# inside the ~1440-minute queue-cancellation cliff.
DEFAULT_QUEUE_CANCEL_MINUTES = 1200.0

# Fields gh emits that this checker reads. A record missing any of them is a
# hard error rather than a silently-skipped row: silently skipping rows is how
# a checker ends up reporting OK over an input it did not understand.
REQUIRED_FIELDS = ("status", "conclusion", "createdAt", "updatedAt")

REASON_NO_RUNS = "no-runs"
REASON_NEVER_SUCCEEDED = "never-succeeded"
REASON_STALE_SUCCESS = "stale-success"
REASON_RECENT_SUCCESS = "recent-success"

PATTERN_QUEUED_THEN_CANCELLED = "queued-then-cancelled"
PATTERN_CURRENTLY_QUEUED = "currently-queued"

STATE_OK = "OK"
STATE_ALARM = "ALARM"


class LivenessError(Exception):
    """Input this checker cannot understand. Always exit 2, never a verdict."""


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

def _parse_ts(value: Any, where: str) -> dt.datetime:
    if not isinstance(value, str) or not value.strip():
        raise LivenessError("%s: timestamp is not a non-empty string: %r" % (where, value))
    text = value.strip()
    # gh emits RFC3339 with a trailing Z; fromisoformat wants +00:00 before 3.11.
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    try:
        parsed = dt.datetime.fromisoformat(text)
    except ValueError as exc:
        raise LivenessError("%s: unparseable timestamp %r (%s)" % (where, value, exc))
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=dt.timezone.utc)
    return parsed.astimezone(dt.timezone.utc)


class Run:
    """One run record, normalised. Immutable in practice."""

    __slots__ = ("run_id", "workflow", "status", "conclusion", "created", "updated", "event")

    def __init__(self, run_id, workflow, status, conclusion, created, updated, event):
        self.run_id = run_id
        self.workflow = workflow
        self.status = status
        self.conclusion = conclusion
        self.created = created
        self.updated = updated
        self.event = event

    @property
    def span_minutes(self) -> float:
        return (self.updated - self.created).total_seconds() / 60.0

    @property
    def succeeded(self) -> bool:
        return self.status == "completed" and self.conclusion == "success"

    @property
    def pending(self) -> bool:
        return self.status in ("queued", "waiting", "in_progress", "pending", "requested")

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return "Run(%s, %s, %s/%s, %.0f min)" % (
            self.run_id, self.workflow, self.status, self.conclusion, self.span_minutes)


def parse_runs(text: str, source: str = "<input>") -> List[Run]:
    """Parse ``gh run list --json ...`` output into :class:`Run` objects.

    Raises :class:`LivenessError` on anything it does not fully understand.
    An EMPTY list parses fine and is a legitimate (ALARM-producing) input —
    "the workflow has no runs" is data, not a parse failure.
    """
    try:
        doc = json.loads(text)
    except ValueError as exc:
        raise LivenessError("%s: not valid JSON: %s" % (source, exc))
    if not isinstance(doc, list):
        raise LivenessError(
            "%s: expected a JSON ARRAY of run records (gh run list --json ...), got %s"
            % (source, type(doc).__name__))

    runs: List[Run] = []
    for idx, raw in enumerate(doc):
        where = "%s[%d]" % (source, idx)
        if not isinstance(raw, dict):
            raise LivenessError("%s: run record is not an object: %r" % (where, raw))
        missing = [f for f in REQUIRED_FIELDS if f not in raw]
        if missing:
            raise LivenessError(
                "%s: run record is missing required field(s) %s. Re-run gh with "
                "--json databaseId,workflowName,status,conclusion,createdAt,updatedAt"
                % (where, ", ".join(missing)))
        workflow = raw.get("workflowName") or raw.get("name")
        if not isinstance(workflow, str) or not workflow.strip():
            raise LivenessError(
                "%s: run record has neither a usable 'workflowName' nor 'name'" % where)
        status = raw.get("status")
        if not isinstance(status, str) or not status.strip():
            raise LivenessError("%s: 'status' is not a non-empty string: %r" % (where, status))
        conclusion = raw.get("conclusion")
        # gh emits "" or null for a run that has not concluded. Both are fine.
        if conclusion is None:
            conclusion = ""
        if not isinstance(conclusion, str):
            raise LivenessError("%s: 'conclusion' is not a string: %r" % (where, conclusion))
        runs.append(Run(
            run_id=raw.get("databaseId"),
            workflow=workflow.strip(),
            status=status.strip(),
            conclusion=conclusion.strip(),
            created=_parse_ts(raw.get("createdAt"), where + ".createdAt"),
            updated=_parse_ts(raw.get("updatedAt"), where + ".updatedAt"),
            event=raw.get("event") or "",
        ))
    return runs


def read_runs_file(path: Path) -> List[Run]:
    try:
        with open(path, "r", encoding="utf-8", newline=None) as fh:
            text = fh.read()
    except OSError as exc:
        raise LivenessError("cannot read %s: %s" % (path, exc))
    return parse_runs(text, source=str(path))


# ---------------------------------------------------------------------------
# Classification — the single production path, shared by fixture and live mode
# ---------------------------------------------------------------------------

class Verdict:
    __slots__ = ("state", "reason", "patterns", "detail", "matched", "newest_success", "newest")

    def __init__(self, state, reason, patterns, detail, matched, newest_success, newest):
        self.state = state
        self.reason = reason
        self.patterns = patterns
        self.detail = detail
        self.matched = matched
        self.newest_success = newest_success
        self.newest = newest

    @property
    def alarming(self) -> bool:
        return self.state == STATE_ALARM

    def to_dict(self) -> Dict[str, Any]:
        return {
            "state": self.state,
            "reason": self.reason,
            "patterns": list(self.patterns),
            "detail": self.detail,
            "runs_matched": len(self.matched),
            "newest_run_id": self.newest.run_id if self.newest else None,
            "newest_success_id": self.newest_success.run_id if self.newest_success else None,
            "newest_success_at": (self.newest_success.updated.isoformat()
                                  if self.newest_success else None),
        }


def classify(runs: Sequence[Run],
             workflow: str = DEFAULT_WORKFLOW_NAME,
             now: Optional[dt.datetime] = None,
             max_success_age_hours: float = DEFAULT_MAX_SUCCESS_AGE_HOURS,
             queue_cancel_minutes: float = DEFAULT_QUEUE_CANCEL_MINUTES) -> Verdict:
    """Classify a run history as OK or ALARM. Never returns OK without a
    recent, genuine ``success`` for ``workflow``."""
    if now is None:
        now = dt.datetime.now(dt.timezone.utc)
    if now.tzinfo is None:
        now = now.replace(tzinfo=dt.timezone.utc)
    if max_success_age_hours <= 0:
        raise LivenessError("--max-success-age-hours must be > 0, got %r" % max_success_age_hours)

    wanted = workflow.strip().casefold()
    matched = [r for r in runs if r.workflow.casefold() == wanted]
    matched.sort(key=lambda r: r.created, reverse=True)

    patterns: List[str] = []
    newest = matched[0] if matched else None
    if newest is not None and newest.pending:
        patterns.append(PATTERN_CURRENTLY_QUEUED)
    for r in matched:
        if r.conclusion == "cancelled" and r.span_minutes >= queue_cancel_minutes:
            patterns.append(PATTERN_QUEUED_THEN_CANCELLED)
            break

    if not matched:
        return Verdict(
            STATE_ALARM, REASON_NO_RUNS, patterns,
            "no runs of workflow %r in a history of %d run(s). An empty history is "
            "NOT healthy: it is what a workflow that never dispatches, or a lane "
            "whose runs have aged out unexecuted, looks like." % (workflow, len(runs)),
            matched, None, newest)

    successes = [r for r in matched if r.succeeded]
    if not successes:
        return Verdict(
            STATE_ALARM, REASON_NEVER_SUCCEEDED, patterns,
            "%d run(s) of %r in the history and NONE has ever concluded 'success' "
            "(conclusions seen: %s)."
            % (len(matched), workflow,
               ", ".join(sorted({(r.conclusion or r.status) for r in matched}))),
            matched, None, newest)

    newest_success = max(successes, key=lambda r: r.updated)
    age_hours = (now - newest_success.updated).total_seconds() / 3600.0
    if age_hours > max_success_age_hours:
        return Verdict(
            STATE_ALARM, REASON_STALE_SUCCESS, patterns,
            "newest successful run of %r (id %s) completed %s — %.1f h ago, beyond "
            "the %.1f h window." % (workflow, newest_success.run_id,
                                    newest_success.updated.isoformat(),
                                    age_hours, max_success_age_hours),
            matched, newest_success, newest)

    return Verdict(
        STATE_OK, REASON_RECENT_SUCCESS, patterns,
        "newest successful run of %r (id %s) completed %s -- %.1f h ago, within the "
        "%.1f h window." % (workflow, newest_success.run_id,
                            newest_success.updated.isoformat(),
                            age_hours, max_success_age_hours),
        matched, newest_success, newest)


# ---------------------------------------------------------------------------
# Live mode
# ---------------------------------------------------------------------------

def default_gh_path() -> str:
    """Locate ``gh``.

    On a GitHub-hosted ubuntu runner -- where this check is MEANT to run --
    ``gh`` is preinstalled on PATH and the bare name works. On the pdfe-runner-01
    host it is NOT on PATH. ``C:\\Users\\dev\\bin\\gh`` there is a *bash*
    shim (``exec .../gh.exe "$@"``), which ``subprocess`` cannot execute
    directly -- WinError 193. So the real executable is preferred, then the
    ``.cmd`` shim, then PATH. ``GH_PATH`` overrides everything.
    """
    override = os.environ.get("GH_PATH")
    if override:
        return override
    for candidate in (
        Path(r"C:\Users\dev\AppData\Local\Programs\gh\bin\gh.exe"),
        Path(r"C:\Users\dev\bin\gh.cmd"),
    ):
        if candidate.exists():
            return str(candidate)
    return "gh"


def fetch_runs_via_gh(workflow_file: str, limit: int, gh_path: str,
                      repo: Optional[str] = None) -> List[Run]:
    argv = [gh_path, "run", "list", "--workflow", workflow_file,
            "--limit", str(limit), "--json",
            "databaseId,name,workflowName,status,conclusion,createdAt,startedAt,updatedAt,event"]
    if repo:
        argv += ["--repo", repo]
    try:
        proc = subprocess.run(argv, capture_output=True, text=True, encoding="utf-8")
    except OSError as exc:
        raise LivenessError("cannot execute %r: %s" % (gh_path, exc))
    if proc.returncode != 0:
        raise LivenessError("gh exited %d: %s" % (proc.returncode, (proc.stderr or "").strip()))
    return parse_runs(proc.stdout, source="gh run list --workflow %s" % workflow_file)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="check_governance_liveness.py",
        description="Classify a GitHub Actions run history as OK (a recent genuine "
                    "success exists) or ALARM (it does not). Fail-closed: no data is "
                    "ALARM, never OK.")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--runs", metavar="FILE",
                     help="JSON array in `gh run list --json ...` shape (fixture mode).")
    src.add_argument("--from-gh", action="store_true",
                     help="Fetch the history live via `gh run list`. Same parse/classify "
                          "path as --runs. Must NOT be hosted on the runner it probes.")
    ap.add_argument("--workflow", default=DEFAULT_WORKFLOW_NAME,
                    help="Workflow display name to match (default: %(default)s).")
    ap.add_argument("--workflow-file", default=DEFAULT_WORKFLOW_FILE,
                    help="Workflow file for --from-gh (default: %(default)s).")
    ap.add_argument("--max-success-age-hours", type=float,
                    default=DEFAULT_MAX_SUCCESS_AGE_HOURS,
                    help="Freshness window for the newest success (default: %(default)s; "
                         "see WHY 48 HOURS in this file's docstring).")
    ap.add_argument("--queue-cancel-minutes", type=float,
                    default=DEFAULT_QUEUE_CANCEL_MINUTES,
                    help="A `cancelled` run spanning at least this long was queued, not "
                         "executing (default: %(default)s).")
    ap.add_argument("--limit", type=int, default=40, help="--from-gh run count.")
    ap.add_argument("--gh-path", default=None, help="Path to the gh executable.")
    ap.add_argument("--repo", default=None, help="OWNER/REPO for --from-gh.")
    ap.add_argument("--now", default=None,
                    help="Override 'now' (RFC3339) so a fixture verdict is deterministic.")
    ap.add_argument("--json", action="store_true", dest="as_json",
                    help="Emit the verdict as JSON on stdout.")
    return ap


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        now = _parse_ts(args.now, "--now") if args.now else dt.datetime.now(dt.timezone.utc)
        if args.from_gh:
            runs = fetch_runs_via_gh(args.workflow_file, args.limit,
                                     args.gh_path or default_gh_path(), args.repo)
            source = "gh run list --workflow %s" % args.workflow_file
        else:
            runs = read_runs_file(Path(args.runs))
            source = args.runs
        verdict = classify(runs, workflow=args.workflow, now=now,
                           max_success_age_hours=args.max_success_age_hours,
                           queue_cancel_minutes=args.queue_cancel_minutes)
    except LivenessError as exc:
        sys.stderr.write("[liveness] ERROR: %s\n" % exc)
        return 2

    if args.as_json:
        payload = verdict.to_dict()
        payload["source"] = source
        payload["now"] = now.isoformat()
        print(json.dumps(payload, indent=2, sort_keys=True))
    else:
        print("[liveness] source: %s" % source)
        print("[liveness] %s (%s) -- %s" % (verdict.state, verdict.reason, verdict.detail))
        if verdict.patterns:
            print("[liveness] patterns: %s" % ", ".join(verdict.patterns))
        if verdict.alarming:
            print("[liveness] The daily governance lane is NOT demonstrably running.")
            print("[liveness] See deferred-backlog rows F38-b / F46 and "
                  "docs/audit/meta-audit-2026-07/followup/phase-44-dispositions.md.")
    return 1 if verdict.alarming else 0


if __name__ == "__main__":
    sys.exit(main())
