#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_f38c_risk_acceptance.py — the F38-c capacity decision must stay
attached to the numbers it was made about.

Meta-audit follow-up **F45**, deferred-backlog row **F38-c**
(*"the shared runner is OVER-subscribed at worst-case durations"*).

--------------------------------------------------------------------------
WHAT THIS EXISTS TO PREVENT
--------------------------------------------------------------------------
F38-c is closed to a **DECIDED** state, not an implemented-fix state: the
options are on record, the exposure is accepted with a rationale, and the
remedy (capacity) is the user's hardware decision. That shape — the
F40/PERF-01 precedent — has one specific failure mode:

    The decision is recorded against a subscription of N minutes a day.
    Somebody later adds a lane, lengthens a `timeout-minutes`, or tightens a
    cadence. The subscription moves. The recommendation document still sits
    there, still dated, still reading as a current decision — but it is now a
    decision about a configuration that no longer exists.

``check_runner_queue_overlap.py`` already stops the subscription rising past
its pin without a decision. It does **not** notice a subscription that changes
while staying under the pin, and it has nothing to say about the document.
This check closes that: the recommendation's recorded figures are compared
against the SAME live re-derivation the queue checker uses, so any change to
the lane set, the crons or the timeouts makes the document stale **and makes
the build red**, forcing a revisit rather than allowing a quiet rot.

--------------------------------------------------------------------------
GATES
--------------------------------------------------------------------------
R1  The recommendation document exists at the pinned path.
R2  It carries YAML frontmatter with every required key.
R3  ``date`` parses as a real ISO date (a decision with no date is not a
    record, and "recent" cannot be judged without one).
R4  ``backlog_row`` is ``F38-c`` AND the body prose actually cites ``F38-c`` —
    frontmatter alone can be correct while the document is about something
    else.
R5  ``subscription_pin_minutes`` equals the LIVE
    ``DEFAULT_MAX_DAILY_SUBSCRIPTION_MINUTES`` in
    ``check_runner_queue_overlap.py``. Moving the pin to make a red row look
    green is the specific cheat this phase was written not to commit; doing it
    without revisiting the document now fails here.
R6  ``subscription_minutes_at_writing`` equals the live re-derived daily
    worst-case subscription (tolerance 0.5 min for rounding).
R7  ``worst_day_minutes_at_writing`` equals the live re-derived worst SINGLE
    day. The average alone hides the Sun/Tue/Thu peak, which is the day the
    decision actually turns on.
R8  At least TWO options, with distinct ids, and every option's ``title``
    appears verbatim as a heading in the body. A recommendation that lists
    options only in metadata has not presented them.
R9  ``recommendation`` names one of those option ids.
R10 The ``deferred-backlog.md`` F38-c row cites this document's path and
    records the state as DECIDED — so the two cannot drift apart.

Anti-cheat properties, because a checker that cannot fail is decoration:

* **The comparison figures are RE-DERIVED, never read from prose.** They come
  from ``check_runner_queue_overlap.build_lanes`` over the live workflow files,
  the same code path the F38 gate uses. Editing a number in the document
  cannot make this pass; it can only make it fail.
* **The pure decision core is** ``evaluate`` **(no I/O)**, so the self-tests
  perturb each input in turn and prove the corresponding gate goes RED.
* **Absent data is never healthy.** A missing document, absent frontmatter, an
  unparseable date and an empty options list are all failures, not skips.

CRLF: ``core.autocrlf=true`` means every tracked text file is CRLF on this
working tree. All reads here are text-mode with explicit UTF-8, so newlines are
universal-translated to ``\\n`` before any matching — a guard that compared raw
bytes would pass in the session that wrote the file and fail after checkout.

Usage:
  python tools/gate/check_f38c_risk_acceptance.py [--repo-root .]
Exit 0 = the decision record is current; 1 = stale or malformed; 2 = usage/IO.
"""
from __future__ import annotations

import argparse
import datetime as _dt
import re
import sys
from pathlib import Path
from typing import Any, Dict, List, Sequence, Tuple

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

try:
    import yaml
except ImportError:  # pragma: no cover - environment problem, not a policy result
    print("[f38c] PyYAML is required (python -m pip install pyyaml)",
          file=sys.stderr)
    raise

import check_runner_queue_overlap as _queue  # noqa: E402  (path set above)

DOC_RELPATH = ("docs/audit/meta-audit-2026-07/followup/phase-45/"
               "f38c-capacity-recommendation.md")
BACKLOG_RELPATH = "docs/audit/meta-audit-2026-07/followup/deferred-backlog.md"
BACKLOG_ROW = "F38-c"

REQUIRED_KEYS: Tuple[str, ...] = (
    "backlog_row",
    "phase",
    "date",
    "disposition",
    "subscription_minutes_at_writing",
    "worst_day_minutes_at_writing",
    "subscription_pin_minutes",
    "runner_capacity_minutes_per_day",
    "options",
    "recommendation",
)

# The row is closed to a DECIDED state, never an implemented-fix state. The
# literal is pinned so relabelling it is a deliberate edit, visible in a diff.
EXPECTED_DISPOSITION = "DECIDED-accepted-with-rationale-not-implemented"

TOLERANCE_MINUTES = 0.5

# Extended ISO calendar date only (see R3 below for why the basic form is
# rejected rather than accepted).
_ISO_DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")


# ---------------------------------------------------------------------------
# live re-derivation (the figures the document is checked against)
# ---------------------------------------------------------------------------

def live_figures(repo_root: Path) -> Tuple[float, float, int, List[str]]:
    """Re-derive ``(daily_subscription, worst_single_day, pin, problems)``.

    Uses ``check_runner_queue_overlap``'s own parser over the live workflow
    files, so this check and the F38 gate can never disagree about what the
    schedule says.
    """
    paths = [repo_root / p for p in _queue.DEFAULT_WORKFLOWS]
    lanes, problems = _queue.build_lanes(paths)
    if problems or not lanes:
        return 0.0, 0.0, _queue.DEFAULT_MAX_DAILY_SUBSCRIPTION_MINUTES, (
            problems or ["no lane on the shared runner could be derived"])

    start, span = _queue.DEFAULT_SPAN_START, _queue.DEFAULT_SPAN_DAYS
    subscription = _queue.daily_subscription_minutes(lanes, start, span)

    per_day: Dict[_dt.date, float] = {}
    for lane in lanes:
        for firing in lane.firings(start, span):
            per_day[firing.date()] = (per_day.get(firing.date(), 0.0)
                                      + lane.timeout_minutes)
    worst_day = max(per_day.values()) if per_day else 0.0
    return (subscription, worst_day,
            _queue.DEFAULT_MAX_DAILY_SUBSCRIPTION_MINUTES, [])


# ---------------------------------------------------------------------------
# frontmatter
# ---------------------------------------------------------------------------

def split_frontmatter(text: str) -> Tuple[Any, str]:
    """Return ``(frontmatter_or_None, body)``.

    ``text`` must already be newline-normalised (text-mode read). A document
    with no leading ``---`` fence yields ``(None, text)``, which R2 treats as a
    failure rather than an absence.
    """
    if not text.startswith("---\n"):
        return None, text
    end = text.find("\n---\n", 3)
    if end == -1:
        return None, text
    raw = text[4:end + 1]
    body = text[end + 5:]
    try:
        return yaml.safe_load(raw), body
    except yaml.YAMLError:
        return None, body


# ---------------------------------------------------------------------------
# decision core
# ---------------------------------------------------------------------------

def evaluate(doc_text: str,
             backlog_text: str,
             live_subscription: float,
             live_worst_day: float,
             live_pin: int,
             live_problems: Sequence[str] = (),
             doc_relpath: str = DOC_RELPATH) -> Tuple[bool, List[str]]:
    """Pure decision core. Return ``(ok, messages)``. No I/O."""
    msgs: List[str] = []
    if live_problems:
        # PURITY: this core does NOT consult the repository's posture. When it
        # did (2026-08-13, briefly), the synthetic "unparseable lane model fails
        # loudly" control started PASSING — the third time in one afternoon that
        # putting repo-global state into a pure decision core disarmed the very
        # self-tests that prove the checker discriminates. main() applies the
        # manual-only posture, against the live tree, where it belongs.
        return False, ["FAIL(R5-R7): the live lane model could not be derived, "
                       "so the document's figures cannot be checked at all: "
                       + "; ".join(live_problems)]

    if doc_text is None:
        return False, ["FAIL(R1): the F38-c recommendation document is missing "
                       "at %s. The backlog row points at a decision record "
                       "that does not exist." % doc_relpath]

    fm, body = split_frontmatter(doc_text)
    if not isinstance(fm, dict):
        return False, ["FAIL(R2): %s has no parseable YAML frontmatter. The "
                       "risk acceptance is supposed to be machine-checked; "
                       "prose alone cannot be." % doc_relpath]

    missing = [k for k in REQUIRED_KEYS if k not in fm]
    if missing:
        return False, ["FAIL(R2): frontmatter is missing required key(s): %s"
                       % ", ".join(sorted(missing))]

    ok = True

    # -- R3 date -----------------------------------------------------------
    raw_date = fm["date"]
    parsed_date = None
    if isinstance(raw_date, _dt.date):
        parsed_date = raw_date
    # Extended form only. Python 3.11+ `date.fromisoformat` also accepts the
    # BASIC form, so a bare integer `date: 20260808` — which YAML resolves to
    # int, not a date — would otherwise parse cleanly and read as a valid
    # record. Caught by this checker's own self-test.
    elif _ISO_DATE_RE.match(str(raw_date)):
        try:
            parsed_date = _dt.date.fromisoformat(str(raw_date))
        except (TypeError, ValueError):
            parsed_date = None
    if parsed_date is None:
        ok = False
        msgs.append("FAIL(R3): `date: %r` is not an ISO date. An undated "
                    "acceptance cannot be judged current or stale."
                    % (raw_date,))
    else:
        msgs.append("R3 date = %s" % parsed_date.isoformat())

    # -- R4 row identity ---------------------------------------------------
    if str(fm["backlog_row"]).strip() != BACKLOG_ROW:
        ok = False
        msgs.append("FAIL(R4): `backlog_row: %r` is not %r."
                    % (fm["backlog_row"], BACKLOG_ROW))
    if BACKLOG_ROW not in body:
        ok = False
        msgs.append("FAIL(R4): the document body never cites %r. Frontmatter "
                    "can be right while the prose is about something else."
                    % BACKLOG_ROW)

    # -- R5 pin ------------------------------------------------------------
    try:
        doc_pin = int(fm["subscription_pin_minutes"])
    except (TypeError, ValueError):
        doc_pin = None
    if doc_pin != live_pin:
        ok = False
        msgs.append("FAIL(R5): the document records a pin of %r but "
                    "check_runner_queue_overlap.py pins %d. Either the pin "
                    "moved without the decision being revisited, or the "
                    "document is wrong about the decision it records."
                    % (fm["subscription_pin_minutes"], live_pin))
    else:
        msgs.append("R5 pin = %d min (matches check_runner_queue_overlap.py)"
                    % live_pin)

    # -- R6/R7 figures -----------------------------------------------------
    for key, live, label in (
            ("subscription_minutes_at_writing", live_subscription,
             "daily worst-case subscription"),
            ("worst_day_minutes_at_writing", live_worst_day,
             "worst SINGLE day")):
        try:
            recorded = float(fm[key])
        except (TypeError, ValueError):
            ok = False
            msgs.append("FAIL(R6/R7): `%s: %r` is not a number."
                        % (key, fm[key]))
            continue
        if abs(recorded - live) > TOLERANCE_MINUTES:
            ok = False
            msgs.append("FAIL(R6/R7): the document records %s = %.1f min, but "
                        "the LIVE crons and timeouts give %.1f min. The "
                        "schedule changed and the decision record did not. "
                        "Re-read %s against the current configuration and "
                        "update it — do not just edit the number."
                        % (label, recorded, live, doc_relpath))
        else:
            msgs.append("R6/R7 %s = %.1f min (live %.1f)"
                        % (label, recorded, live))

    # -- R8 options --------------------------------------------------------
    options = fm["options"]
    option_ids: List[str] = []
    if not isinstance(options, list) or len(options) < 2:
        ok = False
        msgs.append("FAIL(R8): at least TWO concrete options must be on "
                    "record; found %r."
                    % (len(options) if isinstance(options, list) else options,))
    else:
        for i, opt in enumerate(options):
            if not isinstance(opt, dict) or "id" not in opt or "title" not in opt:
                ok = False
                msgs.append("FAIL(R8): option %d needs both `id` and `title` "
                            "(got %r)." % (i, opt))
                continue
            oid, title = str(opt["id"]), str(opt["title"])
            option_ids.append(oid)
            # The guarantee is a HEADING, not mere containment: an option
            # demoted into inline prose has not been *presented* as an option,
            # and plain `title in body` accepted exactly that (found by the
            # karen gate, which demoted a real `## Option C` and stayed green).
            # NB the frontmatter is NOT reachable here -- split_frontmatter()
            # strips it, so `body` is the prose only. An earlier draft of this
            # comment claimed otherwise; it was wrong.
            if not re.search(r"^[ \t]*#{1,6}[ \t]+.*" + re.escape(title),
                             body, re.MULTILINE):
                ok = False
                msgs.append("FAIL(R8): option %r's title is not a heading in "
                            "the body: %r. An option listed only in metadata, "
                            "or demoted into prose, has not been presented."
                            % (oid, title))
        if len(set(option_ids)) != len(option_ids):
            ok = False
            msgs.append("FAIL(R8): duplicate option ids: %r" % (option_ids,))
        if ok:
            msgs.append("R8 options = %s" % ", ".join(option_ids))

    # -- R9 recommendation -------------------------------------------------
    rec = str(fm["recommendation"]).strip()
    if option_ids and rec not in option_ids:
        ok = False
        msgs.append("FAIL(R9): `recommendation: %r` names no option on record "
                    "(%s)." % (rec, ", ".join(option_ids)))
    elif option_ids:
        msgs.append("R9 recommendation = %s" % rec)

    # -- R10 backlog coupling ---------------------------------------------
    if str(fm["disposition"]).strip() != EXPECTED_DISPOSITION:
        ok = False
        msgs.append("FAIL(R10): `disposition: %r` is not %r. This row is "
                    "closed to a DECIDED state; it must not be relabelled as "
                    "an implemented fix without an actual fix."
                    % (fm["disposition"], EXPECTED_DISPOSITION))
    if backlog_text is None:
        ok = False
        msgs.append("FAIL(R10): %s could not be read." % BACKLOG_RELPATH)
    else:
        row_start = backlog_text.find("**%s:" % BACKLOG_ROW)
        if row_start == -1:
            ok = False
            msgs.append("FAIL(R10): no %r row found in %s."
                        % (BACKLOG_ROW, BACKLOG_RELPATH))
        else:
            row_end = backlog_text.find("\n", row_start)
            row = backlog_text[row_start:row_end if row_end != -1 else None]
            if "phase-45/f38c-capacity-recommendation.md" not in row:
                ok = False
                msgs.append("FAIL(R10): the %s row does not cite the "
                            "recommendation document, so a reader of the "
                            "backlog cannot reach the decision."
                            % BACKLOG_ROW)
            if "DECIDED" not in row:
                ok = False
                msgs.append("FAIL(R10): the %s row does not state DECIDED. "
                            "The F40/PERF-01 precedent requires the state to "
                            "be explicit, so nobody reads options-on-record "
                            "as a shipped fix." % BACKLOG_ROW)
            if ok:
                msgs.append("R10 backlog row cites the document and states "
                            "DECIDED")

    if ok:
        msgs.append("OK: the %s decision record is current against the live "
                    "schedule (%.1f min/day subscription, %.0f min worst day, "
                    "pin %d)."
                    % (BACKLOG_ROW, live_subscription, live_worst_day, live_pin))
    return ok, msgs


# ---------------------------------------------------------------------------
# I/O shell
# ---------------------------------------------------------------------------

def _read(path: Path):
    """Text-mode UTF-8 read (universal newlines), or ``None`` if absent."""
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return fh.read()
    except OSError:
        return None


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo-root", default=".", type=Path)
    ap.add_argument("--doc", default=None, type=Path,
                    help="Override the recommendation document path.")
    args = ap.parse_args(argv)

    root = args.repo_root.resolve()
    doc_path = args.doc if args.doc else (root / DOC_RELPATH)
    backlog_path = root / BACKLOG_RELPATH

    subscription, worst_day, pin, problems = live_figures(root)
    ok, msgs = evaluate(_read(doc_path), _read(backlog_path),
                        subscription, worst_day, pin, problems,
                        doc_relpath=str(doc_path))

    # THE MANUAL-ONLY POSTURE (2026-08-13), applied here rather than in
    # evaluate(). This document accepts an OVER-SUBSCRIPTION computed from
    # scheduled lanes — 1598.6 min/day against a 1440 min/day runner. With no
    # cron anywhere the daily subscription is 0 and that condition cannot arise,
    # so checking its arithmetic against a model that no longer exists would
    # fail forever over a risk that is gone.
    #
    # What is NOT acceptable is a document whose headline figures still read as
    # current, so this requires the record to DECLARE itself superseded.
    if not ok and problems:
        from check_asan_cadence import manual_only_posture  # one shared rule
        accepted, why = manual_only_posture()
        doc_text = _read(doc_path)
        fm, _body = split_frontmatter(doc_text) if doc_text else (None, "")
        superseded = (fm or {}).get("superseded_by") if isinstance(fm, dict) else None
        if accepted and superseded:
            ok = True
            msgs = [
                "PASS (manual-only posture): %s." % why,
                "The over-subscription this record accepts was computed from "
                "scheduled lanes; with no cron anywhere the daily subscription "
                "is 0 and the condition cannot arise.",
                "The record declares itself superseded by %r, so its 1598.6 "
                "min/day figure cannot be read as current. Restore any cron and "
                "the model is checked again." % superseded,
            ]
        elif accepted and not superseded:
            msgs.append("FAIL: nothing is scheduled, so this record's figures "
                        "describe a state that no longer exists — and it "
                        "declares no `superseded_by` frontmatter key. A stale "
                        "risk acceptance that still reads as current is the "
                        "defect, not the absent risk.")
        else:
            msgs.append("manual-only posture NOT accepted: %s" % why)

    print("[f38c] document: %s" % doc_path)
    for m in msgs:
        print("[f38c] " + m)
    return 0 if ok else 1


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
