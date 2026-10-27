#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Control experiment for phase-3 firewall-log noise.

Runs PDFE against a KNOWN-CLEAN fixture PDF (tests/fixtures/test_blank.pdf)
under the same firewall sandbox as the malicious-PDF runner uses.
If any non_loopback_blocked entries are recorded for a clean PDF,
the firewall log is being polluted by ambient Windows traffic that
gets attributed to PDFE.exe -- not by anything PDFE actually does.

Usage (from elevated PowerShell):
    python tests/security/runner/test_av_baseline.py

This does NOT use pytest -- it requires admin and is not part of
the unit-test suite. It's a one-shot control script.
"""

import sys
import time
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))
import run_corpus  # noqa: E402

REPO = _HERE.parent.parent.parent
PDFE = REPO / 'build' / 'vs2022' / 'bin' / 'Debug' / 'PDFE.exe'
CLEAN_PDF = REPO / 'tests' / 'fixtures' / 'test_blank.pdf'

ROUNDS = 5
WAIT_SEC = 30.0


def main() -> int:
    if not run_corpus.is_elevated():
        print('[ctrl] not elevated; netsh and pfirewall.log require admin')
        return 2
    if not PDFE.exists():
        print(f'[ctrl] {PDFE} not found')
        return 3
    if not CLEAN_PDF.exists():
        print(f'[ctrl] {CLEAN_PDF} not found')
        return 3

    run_corpus._add_firewall_rule(PDFE)
    run_corpus._enable_firewall_logging()
    listener = run_corpus.LoopbackListener(callback_port=9999)
    listener.start()

    rss_ceiling = 2 * 1024 * 1024 * 1024
    findings_per_round = []

    try:
        for i in range(ROUNDS):
            print(f'[ctrl] round {i+1}/{ROUNDS}: opening {CLEAN_PDF.name} for {WAIT_SEC}s...')
            listener.reset()
            pos_before = run_corpus._firewall_log_position()
            proc_info = run_corpus._launch_and_observe(
                PDFE, CLEAN_PDF, listener, WAIT_SEC, rss_ceiling)
            time.sleep(2)  # let firewall log flush
            pos_after = run_corpus._firewall_log_position()
            drops = run_corpus._parse_firewall_log_range(pos_before, pos_after)
            findings_per_round.append(drops)
            print(f'  opened_ok={proc_info["opened_ok"]} '
                  f'crashed={proc_info["crashed"]} '
                  f'hang={proc_info["hang"]} '
                  f'non_loopback_blocked={len(drops)} entries')
            for d in drops[:5]:
                print(f'    {d["protocol"]:>5} '
                      f'-> {d["dst_ip"]}:{d["dst_port"]}')
            run_corpus._kill_orphan_pdfe()
    finally:
        listener.stop()
        run_corpus._remove_firewall_rule()
        run_corpus._disable_firewall_logging()
        run_corpus._kill_orphan_pdfe()

    total = sum(len(d) for d in findings_per_round)
    print()
    print(f'[ctrl] CONTROL RESULT: {total} firewall drops across '
          f'{ROUNDS} rounds opening a CLEAN PDF.')
    if total == 0:
        print('[ctrl] => Clean PDF produces no firewall drops. Any drops')
        print('[ctrl]    seen with malicious PDFs ARE caused by the PDF.')
    elif total > 0:
        print('[ctrl] => Clean PDF DOES trigger drops. The firewall log is')
        print('[ctrl]    polluted by ambient Windows traffic attributed to')
        print('[ctrl]    PDFE.exe. Variable findings in malicious-PDF runs')
        print('[ctrl]    are likely the same noise; only CONSISTENT findings')
        print('[ctrl]    across multiple runs are real PDFE-driven signals.')
    return 0 if total == 0 else 1


if __name__ == '__main__':
    raise SystemExit(main())
