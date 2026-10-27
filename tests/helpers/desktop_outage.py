# -*- coding: utf-8 -*-
"""Read the watchdog's desktop-outage journal and answer: was the desktop gone
while this test ran?

WHY THIS EXISTS
---------------
`runner_watchdog.ps1` stops the runner when the interactive desktop goes away,
so queued jobs WAIT rather than run blind. That protects jobs that have not
started. It does nothing for a job already running when an RDP session drops,
and such a job can fail in a way indistinguishable from a product defect --
which sends someone looking in the product for a bug that is not there.

HOW OFTEN THAT HAPPENS, MEASURED RATHER THAN ASSUMED (#133)
------------------------------------------------------------
Less often than the first version of this docstring claimed. It said "every UIA
lookup misses". That is wrong: across a real 40-second RDP disconnect, all seven
live tests in tests/test_ui_canvas_sendinput.py PASSED. A disconnected session
keeps its desktop objects; what fails is `OpenInputDesktop`, which asks who is
receiving INPUT. Evidence: docs/audit/issue-133/.

This mechanism is therefore INSURANCE, not a routine correction. It should stay
silent almost always, and a run where it starts firing often is itself a signal
worth reading. Something that excuses a red had better be quiet by default.

Measured on a real disconnect (GitHub #132,
`docs/audit/issue-132/desktop-probe-2026-09-03.log`): `OpenInputDesktop` starts
failing ~1 s after the session drops, and -- the part that matters here --
does NOT recover until ELEVEN SECONDS after the session reports Active again.
A test running across either edge sees a desktop that is not there.

WHAT THIS DOES, AND WHAT IT DELIBERATELY DOES NOT
-------------------------------------------------
It does NOT make the test pass. With no desktop the test genuinely could not be
driven, and a pass would be a lie in the opposite direction. It reclassifies the
outcome from `red` to `blocked` -- this repository's existing vocabulary for
"the product could not be driven" (WIN-45) -- and attaches the outage window as
the reason.

The journal is written by a different process, possibly in a different session,
so everything here is defensive: a missing, empty, truncated or half-written
journal must degrade to "no outage known" and never to an exception. A
bookkeeping helper that can fail a test run is worse than no bookkeeping.
"""
from __future__ import annotations

import datetime as _dt
import io
import json
import os
from typing import List, Optional, Tuple

#: Journal path. Must agree with Get-OutageLogPath in runner_watchdog.ps1.
#: The env var is the seam the tests use; without it this resolves to the same
#: machine-wide location the watchdog writes to.
ENV_VAR = "PDFE_DESKTOP_OUTAGE_LOG"


def journal_path() -> str:
    override = os.environ.get(ENV_VAR)
    if override:
        return override
    base = os.environ.get("ProgramData") or os.environ.get("TEMP") or "."
    return os.path.join(base, "PDFE", "desktop-outages.jsonl")


def _parse_stamp(text: str) -> Optional[_dt.datetime]:
    """Parse PowerShell's round-trip ("o") format into an aware datetime."""
    if not text:
        return None
    try:
        # PowerShell emits 7 fractional digits; datetime accepts at most 6.
        if "." in text:
            head, tail = text.split(".", 1)
            digits = ""
            rest = ""
            for i, ch in enumerate(tail):
                if ch.isdigit():
                    digits += ch
                else:
                    rest = tail[i:]
                    break
            text = head + "." + digits[:6] + rest
        stamp = _dt.datetime.fromisoformat(text.replace("Z", "+00:00"))
    except (ValueError, TypeError):
        return None
    if stamp.tzinfo is None:
        stamp = stamp.astimezone()
    return stamp


def read_outages(path: Optional[str] = None) -> List[Tuple[_dt.datetime, Optional[_dt.datetime]]]:
    """Every outage window in the journal, as (start, end).

    ``end`` is None for an outage that has not closed -- which is the NORMAL
    state during an outage, not a corrupt record, and must be treated as
    "ongoing" rather than skipped. Reading it as skippable would make the
    mechanism blind during exactly the window it exists for.
    """
    path = path or journal_path()
    try:
        with io.open(path, encoding="utf-8-sig", errors="replace") as fh:
            lines = fh.readlines()
    except (OSError, IOError):
        return []

    windows: List[Tuple[_dt.datetime, Optional[_dt.datetime]]] = []
    open_start: Optional[_dt.datetime] = None
    for line in lines:
        line = line.strip()
        if not line:
            continue
        try:
            rec = json.loads(line)
        except ValueError:
            # A half-written final line is expected: the writer appends while
            # this reads. Skip it; do not abandon the records before it.
            continue
        stamp = _parse_stamp(rec.get("at", ""))
        if stamp is None:
            continue
        event = rec.get("event")
        if event == "outage-start":
            if open_start is not None:
                # Two starts with no end between them: the watchdog restarted
                # mid-outage. Keep the EARLIER start -- the desktop was already
                # gone, and widening the window can only make the mechanism more
                # conservative, which is the right direction to err.
                continue
            open_start = stamp
        elif event == "outage-end" and open_start is not None:
            windows.append((open_start, stamp))
            open_start = None
    if open_start is not None:
        windows.append((open_start, None))
    return windows


def outage_overlapping(start: _dt.datetime, end: _dt.datetime,
                       path: Optional[str] = None,
                       grace_seconds: float = 12.0
                       ) -> Optional[Tuple[_dt.datetime, Optional[_dt.datetime]]]:
    """The first outage window overlapping [start, end], or None.

    ``grace_seconds`` extends each window's END, and defaults to 12 s because of
    a measured property rather than a guess: on reconnect the session reports
    Active a full ELEVEN seconds before `OpenInputDesktop` succeeds again
    (#132). A test that starts in that gap sees no desktop while the journal has
    already closed the outage. Twelve is eleven plus one sample.

    The start edge needs no grace: the journal opens the window at the first
    failing sample, which is already ~1 s AHEAD of anything a test could notice.
    """
    if start.tzinfo is None:
        start = start.astimezone()
    if end.tzinfo is None:
        end = end.astimezone()
    grace = _dt.timedelta(seconds=grace_seconds)
    for win_start, win_end in read_outages(path):
        effective_end = (win_end + grace) if win_end is not None else None
        if effective_end is not None and effective_end < start:
            continue
        if win_start > end:
            continue
        return (win_start, win_end)
    return None


def describe(window: Tuple[_dt.datetime, Optional[_dt.datetime]]) -> str:
    win_start, win_end = window
    if win_end is None:
        return ("no interactive desktop from %s (outage still open at read time)"
                % win_start.strftime("%H:%M:%S"))
    seconds = (win_end - win_start).total_seconds()
    return ("no interactive desktop %s-%s (%.0fs)"
            % (win_start.strftime("%H:%M:%S"), win_end.strftime("%H:%M:%S"), seconds))
