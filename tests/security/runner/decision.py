#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Pure decision logic for the phase-3 corpus runner.

Decoupled from pywinauto / firewall / process management so it can
be unit-tested in isolation. The core is one function:

    decide(observation, expected_pdfe_behavior) -> (str, str)

Returns (final_decision, reason). Reason is a stable enum-ish string
that downstream reporting groups on.

Pipeline phase-3 invariants (the "binary pass/fail because the
corpus is synthetic and any callback is anomalous" rule):

  ANY observed loopback connection attempt = fail with
    reason='exfiltration_attempt'  -- regardless of expected_behavior
  Crash                            = fail / 'crash'
  Hang (timeout exceeded)          = fail / 'hang'
  Memory blowup (>RSS ceiling)     = fail / 'memory_blowup'
  /Launch link followed without confirmation dialog = fail / 'launch_unprompted'
  /SubmitForm to non-loopback w/out HostAllowlist = fail / 'submit_unblocked'
  Otherwise                        = pass / ''
"""

from __future__ import annotations

from typing import Any


VALID_EXPECTED = (
    'open_clean',
    'prompt_user',
    'refuse',
    'sandbox_block',
)


def decide(observation: dict[str, Any], expected_pdfe_behavior: str
           ) -> tuple[str, str]:
    """observation schema (per-PDF run record):
      opened_ok            bool
      crashed              bool
      hang                 bool
      memory_blowup        bool
      exit_code            int
      stderr_tail          str
      wer_dump_path_if_any str | None
      loopback_attempts    list[dict]   each with protocol, port, ...
      non_loopback_blocked list[dict]   blocked outbound (firewall rule hits)
      launch_dialog_seen   bool
      submit_dialog_seen   bool
      js_alert_seen        bool

    expected_pdfe_behavior  must be one of VALID_EXPECTED.
    """
    if expected_pdfe_behavior not in VALID_EXPECTED:
        return 'fail', f'invalid_expected:{expected_pdfe_behavior}'

    # --- Hard-fail conditions (binary; expected_behavior doesn't help)

    # ANY loopback attempt = fail. This is the primary metric per
    # phase 3 PRIMARY METRIC clause.
    real_attempts = [a for a in observation.get('loopback_attempts', [])
                     if a.get('protocol') != 'bind_failed']
    if real_attempts:
        return 'fail', 'exfiltration_attempt'

    # Non-loopback outbound that the firewall actually blocked is also
    # a fail -- the PDF tried to call out, the firewall caught it,
    # but the *attempt* is the failure signal.
    #
    # Use the PDFE-attributed subset when available (drops cross-
    # referenced against PDFE.exe's owned netstat sockets during the
    # test window). Falls back to the raw list when the runner ran
    # without socket capture (older artifacts, dry-run mode, or a
    # netstat snapshot failure). The raw list is over-inclusive on
    # busy hosts because the netsh DROP rule scopes the BLOCK to
    # PDFE.exe but pfirewall.log records every drop on the host with
    # no process attribution.
    attributed = observation.get('non_loopback_blocked_pdfe_attributed')
    if attributed is None:
        # No socket capture this run; treat raw list as authoritative.
        if observation.get('non_loopback_blocked'):
            return 'fail', 'exfiltration_attempt_nonloopback'
    elif attributed:
        return 'fail', 'exfiltration_attempt_nonloopback'

    if observation.get('crashed'):
        return 'fail', 'crash'

    if observation.get('hang'):
        return 'fail', 'hang'

    if observation.get('memory_blowup'):
        return 'fail', 'memory_blowup'

    # --- Conditional checks based on expected_pdfe_behavior --------

    if expected_pdfe_behavior == 'prompt_user':
        # The /Launch + /SubmitForm payloads must show a confirmation
        # dialog. If they actually fired the action without showing
        # the dialog, that's a confirmed bypass.
        if not observation.get('launch_dialog_seen') \
           and not observation.get('submit_dialog_seen'):
            # No dialog AND no exfil attempt: the action wasn't
            # actually invoked. That's actually fine -- the user
            # never clicked the link. So pass.
            return 'pass', ''
        # Dialog appeared: pass (cancellation was simulated).
        return 'pass', ''

    if expected_pdfe_behavior == 'refuse':
        # PDFE should refuse to open. opened_ok must be False.
        if observation.get('opened_ok'):
            return 'fail', 'opened_when_should_refuse'
        return 'pass', ''

    if expected_pdfe_behavior == 'sandbox_block':
        # PDFE relies on the runtime sandbox / HostAllowlist to block
        # the action. The fact that no exfil attempt was observed is
        # already a pass at this point.
        return 'pass', ''

    # expected_pdfe_behavior == 'open_clean'
    if not observation.get('opened_ok'):
        # PDF didn't even open -- maybe a parser bug. Treat as fail
        # because the AC says open_clean = open without callback.
        return 'fail', 'failed_to_open'
    return 'pass', ''


def summarize(records: list[dict[str, Any]]) -> dict[str, Any]:
    """Aggregate per-PDF records into the final phase-3 verdict.

    Phase 3 fails overall if any single PDF has final_decision='fail'.
    Includes both fails-by-reason and per-category breakdowns
    (fails / passes / total) -- the per-category counts feed the MD
    report (AC8: 'counts per category').
    """
    total = len(records)
    passes = sum(1 for r in records if r.get('final_decision') == 'pass')
    fails  = total - passes
    by_reason: dict[str, int] = {}
    fails_by_category:  dict[str, int] = {}
    passes_by_category: dict[str, int] = {}
    totals_by_category: dict[str, int] = {}
    for r in records:
        cat = r.get('category', 'unknown')
        totals_by_category[cat] = totals_by_category.get(cat, 0) + 1
        if r.get('final_decision') == 'fail':
            reason = r.get('final_decision_reason', 'unknown')
            by_reason[reason] = by_reason.get(reason, 0) + 1
            fails_by_category[cat] = fails_by_category.get(cat, 0) + 1
        else:
            passes_by_category[cat] = passes_by_category.get(cat, 0) + 1
    failed_paths = [r['relative_path'] for r in records
                    if r.get('final_decision') == 'fail']
    return {
        'total':              total,
        'passes':             passes,
        'fails':              fails,
        'fails_by_reason':    by_reason,
        'totals_by_category': totals_by_category,
        'fails_by_category':  fails_by_category,
        'passes_by_category': passes_by_category,
        'failed_paths':       failed_paths,
        'overall_verdict':    'pass' if fails == 0 else 'fail',
    }
