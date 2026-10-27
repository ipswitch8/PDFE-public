#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Unit tests for the phase-3 decision logic.

These tests verify the pure decide() function with synthetic
observation dicts. They do not launch PDFE, do not need admin,
and do not need network. They are designed to run as part of the
phase-3 build + ctest pass.
"""

import sys
from pathlib import Path

import pytest

# Make decision.py importable when this file is run directly.
sys.path.insert(0, str(Path(__file__).resolve().parent))

from decision import decide, summarize, VALID_EXPECTED


def base_obs(**overrides):
    """Synthetic clean observation: no callbacks, no crash, etc."""
    obs = {
        'opened_ok':            True,
        'crashed':              False,
        'hang':                 False,
        'memory_blowup':        False,
        'exit_code':            0,
        'stderr_tail':          '',
        'wer_dump_path_if_any': None,
        'loopback_attempts':    [],
        'non_loopback_blocked': [],
        'launch_dialog_seen':   False,
        'submit_dialog_seen':   False,
        'js_alert_seen':        False,
    }
    obs.update(overrides)
    return obs


# ===================================================================
# PRIMARY METRIC: any loopback attempt = fail
# ===================================================================

def test_any_tcp_loopback_attempt_is_fail():
    obs = base_obs(loopback_attempts=[
        {'protocol': 'tcp', 'port': 9999, 'peer_port': 51234,
         'timestamp_iso': 't', 'payload_preview': ''}
    ])
    assert decide(obs, 'open_clean') == ('fail', 'exfiltration_attempt')


def test_any_udp_loopback_attempt_is_fail():
    obs = base_obs(loopback_attempts=[
        {'protocol': 'udp', 'port': 53, 'peer_port': 12345,
         'timestamp_iso': 't', 'payload_preview': ''}
    ])
    assert decide(obs, 'open_clean') == ('fail', 'exfiltration_attempt')


def test_http_loopback_attempt_is_fail_even_with_dialog():
    """Even if PDFE showed a Launch dialog, an actual callback that
    hit our listener is the failure -- the dialog was bypassed."""
    obs = base_obs(
        loopback_attempts=[{'protocol': 'http', 'port': 9999,
                             'peer_port': 51234, 'timestamp_iso': 't',
                             'payload_preview': ''}],
        launch_dialog_seen=True,
    )
    assert decide(obs, 'prompt_user') == ('fail', 'exfiltration_attempt')


def test_bind_failed_is_not_a_real_attempt():
    """'bind_failed' is bookkeeping, not a callback. It must NOT
    count toward exfiltration."""
    obs = base_obs(loopback_attempts=[
        {'protocol': 'bind_failed', 'port': 9999, 'peer_port': 0,
         'timestamp_iso': 't', 'payload_preview': None}
    ])
    assert decide(obs, 'open_clean') == ('pass', '')


def test_dns_loopback_is_fail():
    obs = base_obs(loopback_attempts=[
        {'protocol': 'udp_dns', 'port': 53, 'peer_port': 12345,
         'timestamp_iso': 't', 'payload_preview': ''}
    ])
    assert decide(obs, 'open_clean') == ('fail', 'exfiltration_attempt')


# ===================================================================
# Non-loopback blocked outbound
# ===================================================================

def test_non_loopback_blocked_is_fail():
    # Back-compat path: observation lacks the new attributed field
    # (older artifact, dry-run, or capture failed). The raw list is
    # still authoritative.
    obs = base_obs(non_loopback_blocked=[
        {'remote': '8.8.8.8:443', 'timestamp_iso': 't'}
    ])
    decision, reason = decide(obs, 'open_clean')
    assert decision == 'fail'
    assert reason == 'exfiltration_attempt_nonloopback'


def test_attributed_drops_drive_decision_when_present():
    # Both fields present: decision uses the attributed subset, NOT
    # the raw list. This is the new (post-2026-04-26) path.
    obs = base_obs(
        non_loopback_blocked=[{'remote': '224.0.0.252:5355'}] * 100,
        non_loopback_blocked_pdfe_attributed=[
            {'remote': '8.8.8.8:443'}
        ],
    )
    decision, reason = decide(obs, 'open_clean')
    assert decision == 'fail'
    assert reason == 'exfiltration_attempt_nonloopback'


def test_subnet_ambient_storm_no_longer_fails_when_attribution_clears():
    # The 2026-04-26 storm pattern: 1900+ raw NLB entries with zero
    # PDFE-attributed entries means the storm was subnet-ambient
    # noise, not PDFE exfiltration. With attribution data present,
    # decide() must not fail on the raw list alone.
    obs = base_obs(
        non_loopback_blocked=[
            {'remote': '224.0.0.252:5355'} for _ in range(1900)
        ],
        non_loopback_blocked_pdfe_attributed=[],
    )
    decision, _reason = decide(obs, 'open_clean')
    assert decision == 'pass'


# ===================================================================
# Crash / hang / memory
# ===================================================================

def test_crash_is_fail():
    assert decide(base_obs(crashed=True), 'open_clean') == ('fail', 'crash')


def test_hang_is_fail():
    assert decide(base_obs(hang=True), 'open_clean') == ('fail', 'hang')


def test_memory_blowup_is_fail():
    assert decide(base_obs(memory_blowup=True), 'open_clean') == ('fail', 'memory_blowup')


def test_callback_takes_precedence_over_crash():
    """If both occur, the exfil attempt is the dominant signal -- a
    crash after a callback still leaks data."""
    obs = base_obs(
        crashed=True,
        loopback_attempts=[{'protocol': 'tcp', 'port': 9999,
                             'peer_port': 51234, 'timestamp_iso': 't',
                             'payload_preview': ''}],
    )
    assert decide(obs, 'open_clean') == ('fail', 'exfiltration_attempt')


# ===================================================================
# expected = open_clean
# ===================================================================

def test_open_clean_with_clean_observation_passes():
    assert decide(base_obs(), 'open_clean') == ('pass', '')


def test_open_clean_failed_to_open_is_fail():
    """open_clean expectation but the file didn't open --
    parser regression."""
    assert decide(base_obs(opened_ok=False), 'open_clean') == ('fail', 'failed_to_open')


# ===================================================================
# expected = prompt_user (Launch / SubmitForm dialogs)
# ===================================================================

def test_prompt_user_with_dialog_seen_passes():
    obs = base_obs(launch_dialog_seen=True)
    assert decide(obs, 'prompt_user') == ('pass', '')


def test_prompt_user_with_no_dialog_and_no_callback_passes():
    """User never clicked the link; no dialog, no exfil. Pass."""
    assert decide(base_obs(), 'prompt_user') == ('pass', '')


def test_prompt_user_with_callback_is_fail():
    """Confirmation dialog was bypassed."""
    obs = base_obs(loopback_attempts=[
        {'protocol': 'tcp', 'port': 9999, 'peer_port': 51234,
         'timestamp_iso': 't', 'payload_preview': ''}
    ])
    assert decide(obs, 'prompt_user') == ('fail', 'exfiltration_attempt')


# ===================================================================
# expected = refuse
# ===================================================================

def test_refuse_with_opened_is_fail():
    """PDFE opened a file it should have refused (e.g. SVG polyglot)."""
    obs = base_obs(opened_ok=True)
    assert decide(obs, 'refuse') == ('fail', 'opened_when_should_refuse')


def test_refuse_with_not_opened_passes():
    obs = base_obs(opened_ok=False)
    assert decide(obs, 'refuse') == ('pass', '')


# ===================================================================
# expected = sandbox_block
# ===================================================================

def test_sandbox_block_with_no_callback_passes():
    """HostAllowlist worked: no callback observed."""
    assert decide(base_obs(), 'sandbox_block') == ('pass', '')


def test_sandbox_block_with_callback_is_fail():
    """HostAllowlist failed: callback escaped."""
    obs = base_obs(loopback_attempts=[
        {'protocol': 'http', 'port': 9999, 'peer_port': 51234,
         'timestamp_iso': 't', 'payload_preview': ''}
    ])
    assert decide(obs, 'sandbox_block') == ('fail', 'exfiltration_attempt')


# ===================================================================
# Invalid expected
# ===================================================================

def test_invalid_expected_is_fail():
    assert decide(base_obs(), 'invalid_value')[0] == 'fail'


def test_all_valid_expected_values_documented():
    assert set(VALID_EXPECTED) == {
        'open_clean', 'prompt_user', 'refuse', 'sandbox_block',
    }


# ===================================================================
# summarize()
# ===================================================================

def test_summarize_all_pass():
    records = [
        {'final_decision': 'pass', 'relative_path': 'js/test1.pdf'},
        {'final_decision': 'pass', 'relative_path': 'uri/test5.pdf'},
    ]
    s = summarize(records)
    assert s['total'] == 2
    assert s['passes'] == 2
    assert s['fails'] == 0
    assert s['overall_verdict'] == 'pass'
    assert s['failed_paths'] == []


def test_summarize_one_fail_groups_by_reason():
    records = [
        {'final_decision': 'pass', 'relative_path': 'js/test1.pdf'},
        {'final_decision': 'fail', 'relative_path': 'launch/test6.pdf',
         'final_decision_reason': 'crash'},
        {'final_decision': 'fail', 'relative_path': 'launch/test32.pdf',
         'final_decision_reason': 'crash'},
        {'final_decision': 'fail', 'relative_path': 'js/test10.pdf',
         'final_decision_reason': 'exfiltration_attempt'},
    ]
    s = summarize(records)
    assert s['total'] == 4
    assert s['passes'] == 1
    assert s['fails'] == 3
    assert s['fails_by_reason'] == {
        'crash': 2, 'exfiltration_attempt': 1,
    }
    assert s['overall_verdict'] == 'fail'
    assert sorted(s['failed_paths']) == [
        'js/test10.pdf', 'launch/test32.pdf', 'launch/test6.pdf',
    ]


def test_summarize_empty():
    s = summarize([])
    assert s['total'] == 0
    assert s['overall_verdict'] == 'pass'  # vacuously


def test_summarize_groups_by_category():
    """AC8: MD report needs counts per category (js / launch /
    submitform / etc.). summarize() must aggregate per-category
    pass/fail/total from each record's 'category' field."""
    records = [
        {'final_decision': 'pass', 'relative_path': 'js/a.pdf',
         'category': 'js'},
        {'final_decision': 'fail', 'relative_path': 'js/b.pdf',
         'category': 'js', 'final_decision_reason': 'crash'},
        {'final_decision': 'pass', 'relative_path': 'launch/c.pdf',
         'category': 'launch'},
        {'final_decision': 'pass', 'relative_path': 'launch/d.pdf',
         'category': 'launch'},
        {'final_decision': 'fail', 'relative_path': 'uri/e.pdf',
         'category': 'uri', 'final_decision_reason': 'exfiltration_attempt'},
    ]
    s = summarize(records)
    assert s['totals_by_category']  == {'js': 2, 'launch': 2, 'uri': 1}
    assert s['passes_by_category']  == {'js': 1, 'launch': 2}
    assert s['fails_by_category']   == {'js': 1, 'uri': 1}


def test_summarize_unknown_category_when_missing():
    records = [
        {'final_decision': 'pass', 'relative_path': 'mystery.pdf'},
        {'final_decision': 'fail', 'relative_path': 'mystery2.pdf',
         'final_decision_reason': 'crash'},
    ]
    s = summarize(records)
    assert s['totals_by_category'] == {'unknown': 2}
    assert s['passes_by_category'] == {'unknown': 1}
    assert s['fails_by_category']  == {'unknown': 1}


if __name__ == '__main__':
    raise SystemExit(pytest.main([__file__, '-v']))
