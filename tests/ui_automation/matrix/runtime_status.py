# -*- coding: utf-8 -*-
"""Runtime matrix status semantics (pipeline e5b9c231 Phase 5 — H4 / criterion 1).

The committed matrix CSV (``ui_test_matrix.csv``) carries *closeout* statuses:
many rows are ``green`` from a prior sweep. The original
``conftest._write_matrix_updates`` produced the live per-run CSV by overlaying
this session's results onto the *full committed CSV*, which meant every row that
did NOT run this session INHERITED its committed status — a stale ``green``
included. That made the runtime CSV dishonest: a ``green`` row could mean
"asserted this run" OR "green months ago, not run today", with no way to tell
them apart. That is the exact H4 hole the audit named — "matrix pass is a
runtime CSV rebuilt from committed green".

This module redefines runtime *pass* as **ran-and-asserted-this-run**: any
committed row not recorded in this session's ``results`` is written with status
``no-run`` (NOT the inherited committed status), so in the runtime CSV
``green`` ⟺ a test recorded green *this session*. ``wont-fix`` rows are
permanent, documented non-results and are preserved verbatim.

The function is deliberately pure (list-of-dicts in, list-of-dicts out, no I/O,
no clock) so it is directly unit-testable — see ``test_runtime_pass_semantics``.
"""
from __future__ import annotations

from typing import Dict, List

#: Status written for a committed row that did NOT record a result this session.
#: This is the whole point: a non-run row must not masquerade as committed green.
NO_RUN = "no-run"

#: Statuses that are documented dispositions rather than per-run outcomes.
#: A row carrying one of these is not "no-run" just because it wasn't exercised
#: this session, so when nothing was observed it is preserved verbatim.
#:
#: GITHUB #171 -- "PRESERVED VERBATIM" USED TO MEAN "OBSERVATION DISCARDED".
#: The preserve branch ran BEFORE ``results`` was consulted, so a live GREEN on
#: a ``wont-fix`` row was thrown away: the committed status beat the observed
#: one. That is backwards. The point of running a test is to find out what is
#: true now; a ``wont-fix`` records what somebody believed when they marked it,
#: and a ``wont-fix`` reason EXPIRES when unrelated work fixes the blocker.
#: Because the write-back was built not to notice, an expired reason could
#: never self-correct -- which is how canvas_canvas_rubber_band_select spent
#: two months describing itself as blocked while the behaviour was recoverable.
#: Same shape as ``xfail(strict=False)``: the unexpected pass is swallowed, so
#: a fix can never announce itself.
PRESERVE_STATUSES = frozenset({"wont-fix"})

#: The live status that CONTRADICTS a preserved disposition and must therefore
#: survive write-back. Only a pass is treated this way, deliberately: a
#: ``red``/``blocked`` observation AGREES with the committed ``wont-fix``, so
#: preserving the row verbatim loses nothing and avoids inflating the run's
#: failure counts with rows nobody expects to pass.
CONTRADICTS_PRESERVED = "green"

#: Appended to the notes of a row whose preserved disposition was contradicted,
#: so the disagreement is visible in the artifact and not only in a counter.
PROMOTABLE_NOTE = (
    "GitHub #171: committed %s but recorded GREEN this run -- the stated "
    "reason has expired; re-justify the disposition or promote the row")

#: The only status that means "this row was asserted-pass this run".
PASS = "green"


def compute_runtime_rows(
    committed_rows: List[Dict[str, str]],
    results: Dict[str, Dict[str, str]],
) -> List[Dict[str, str]]:
    """Overlay this session's ``results`` onto ``committed_rows`` honestly.

    Parameters
    ----------
    committed_rows:
        Rows read from the source-controlled matrix CSV (each a dict keyed by
        the CSV header; must contain ``test_id`` and ``status``).
    results:
        ``{test_id: {"status": "green"|"red"|"blocked", "notes": str}}`` — only
        the rows that actually recorded an outcome this session.

    Returns
    -------
    A new list of row dicts (inputs are not mutated) where:

    * a row whose ``test_id`` is in ``results`` takes that result's status and
      has the result notes appended;
    * a ``PRESERVE_STATUSES`` row (e.g. ``wont-fix``) is passed through verbatim;
    * every other row — i.e. a committed row that did not run this session —
      is forced to :data:`NO_RUN` instead of inheriting its committed status.

    Post-condition: a row is ``green`` in the output **iff** it recorded
    ``green`` this session. Stale committed green never survives as green.
    """
    out: List[Dict[str, str]] = []
    for row in committed_rows:
        new = dict(row)
        tid = (row.get("test_id") or "").strip()
        committed_status = (row.get("status") or "").strip()

        res = results.get(tid)

        if committed_status in PRESERVE_STATUSES:
            # Preserve verbatim UNLESS this session actually observed the row
            # passing, which contradicts the disposition (GitHub #171).
            if res is None or res.get("status") != CONTRADICTS_PRESERVED:
                out.append(new)
                continue
            new["status"] = CONTRADICTS_PRESERVED
            note = PROMOTABLE_NOTE % committed_status
            for extra in (res.get("notes", ""), note):
                if extra and extra not in (new.get("notes") or ""):
                    existing = new.get("notes") or ""
                    new["notes"] = existing + ("; " if existing else "") + extra
            out.append(new)
            continue

        if res is not None:
            new["status"] = res["status"]
            notes = res.get("notes", "")
            if notes:
                existing = new.get("notes") or ""
                if notes not in existing:
                    sep = "; " if existing else ""
                    new["notes"] = existing + sep + notes
        else:
            # Did not run / did not assert this session: do NOT inherit the
            # committed status. Honesty over continuity.
            new["status"] = NO_RUN
        out.append(new)
    return out
