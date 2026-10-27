#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Unit tests for the pfirewall.log parser in run_corpus.

The parser feeds the runner's `non_loopback_blocked` field, which the
decide() function uses to detect exfiltration attempts that the
firewall caught. Karen flagged the absence of this parser as HIGH:
without it, the field was always [] in live runs and the
'exfiltration_attempt_nonloopback' decision branch was dead.

These tests exercise the parser against synthetic log files written
to a temp dir -- no admin, no firewall, no PDFE."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_corpus import (  # noqa: E402
    _firewall_log_position,
    _parse_firewall_log_range,
)


# A minimal pfirewall.log v1.5 header + a few entries. Field layout:
# date time action protocol src-ip dst-ip src-port dst-port size
# tcpflags tcpsyn tcpack tcpwin icmptype icmpcode info path
_HEADER = (
    "#Version: 1.5\n"
    "#Software: Microsoft Windows Firewall\n"
    "#Time Format: Local\n"
    "#Fields: date time action protocol src-ip dst-ip src-port "
    "dst-port size tcpflags tcpsyn tcpack tcpwin icmptype icmpcode "
    "info path\n"
    "\n"
)


def _write_log(tmp_path: Path, body: str) -> Path:
    p = tmp_path / "pfirewall.log"
    p.write_text(_HEADER + body, encoding="latin-1")
    return p


def test_empty_log_returns_empty(tmp_path):
    p = tmp_path / "missing.log"
    assert _parse_firewall_log_range(0, 1000, log_path=p) == []


def test_header_only_returns_empty(tmp_path):
    p = _write_log(tmp_path, "")
    assert _parse_firewall_log_range(0, p.stat().st_size,
                                     log_path=p) == []


def test_drop_to_non_loopback_recorded(tmp_path):
    body = (
        "2026-04-22 12:34:56 DROP TCP 127.0.0.1 8.8.8.8 12345 443 "
        "60 - 0 0 0 - - - SEND\n"
    )
    p = _write_log(tmp_path, body)
    rows = _parse_firewall_log_range(0, p.stat().st_size, log_path=p)
    assert len(rows) == 1
    assert rows[0]['action'] == 'DROP'
    assert rows[0]['dst_ip'] == '8.8.8.8'
    assert rows[0]['dst_port'] == 443
    assert rows[0]['protocol'] == 'tcp'
    assert rows[0]['timestamp_iso'].startswith('2026-04-22T')


def test_drop_to_loopback_filtered_out(tmp_path):
    body = (
        "2026-04-22 12:34:56 DROP TCP 10.0.0.5 127.0.0.1 12345 9999 "
        "60 - 0 0 0 - - - SEND\n"
        "2026-04-22 12:34:57 DROP UDP fe80::1 ::1 60001 53 "
        "60 - 0 0 0 - - - SEND\n"
    )
    p = _write_log(tmp_path, body)
    rows = _parse_firewall_log_range(0, p.stat().st_size, log_path=p)
    assert rows == [], rows


def test_allow_entries_filtered_out(tmp_path):
    body = (
        "2026-04-22 12:34:56 ALLOW TCP 127.0.0.1 8.8.8.8 12345 443 "
        "60 - 0 0 0 - - - SEND\n"
        "2026-04-22 12:34:57 INFO-EVENTS-LOST 1234567 - - - - - - "
        "- - - - - - -\n"
    )
    p = _write_log(tmp_path, body)
    assert _parse_firewall_log_range(0, p.stat().st_size,
                                     log_path=p) == []


def test_only_drops_in_byte_range_returned(tmp_path):
    body_first = (
        "2026-04-22 12:34:56 DROP TCP 127.0.0.1 1.2.3.4 12345 80 "
        "60 - 0 0 0 - - - SEND\n"
    )
    body_second = (
        "2026-04-22 12:34:57 DROP TCP 127.0.0.1 5.6.7.8 12345 80 "
        "60 - 0 0 0 - - - SEND\n"
    )
    p = _write_log(tmp_path, body_first + body_second)
    full_size = p.stat().st_size
    # Range corresponding to only the first entry.
    first_end = len(_HEADER) + len(body_first)
    rows_first = _parse_firewall_log_range(len(_HEADER), first_end,
                                           log_path=p)
    assert len(rows_first) == 1
    assert rows_first[0]['dst_ip'] == '1.2.3.4'
    # Range corresponding to only the second.
    rows_second = _parse_firewall_log_range(first_end, full_size,
                                            log_path=p)
    assert len(rows_second) == 1
    assert rows_second[0]['dst_ip'] == '5.6.7.8'


def test_malformed_lines_tolerated(tmp_path):
    body = (
        "garbled line with too few fields\n"
        "2026-04-22 12:34:56 DROP TCP 127.0.0.1 9.9.9.9 1 80 "
        "60 - 0 0 0 - - - SEND\n"
        "another garbled\n"
    )
    p = _write_log(tmp_path, body)
    rows = _parse_firewall_log_range(0, p.stat().st_size, log_path=p)
    assert len(rows) == 1
    assert rows[0]['dst_ip'] == '9.9.9.9'


def test_rotation_handled_when_file_shrinks(tmp_path):
    # First write a long log, snapshot a position past where the
    # short version will end -- mimicking rotation.
    body = (
        "2026-04-22 12:34:56 DROP TCP 127.0.0.1 1.2.3.4 1 80 "
        "60 - 0 0 0 - - - SEND\n"
    )
    p = _write_log(tmp_path, body)
    big_pos = p.stat().st_size + 5000  # pretend we saw a much bigger
    # Now rewrite the log with just the header (rotation).
    p.write_text(_HEADER, encoding="latin-1")
    # _parse_firewall_log_range with a start_pos past the new EOF
    # must not crash and must return [].
    rows = _parse_firewall_log_range(big_pos, big_pos + 100,
                                     log_path=p)
    assert rows == []


def test_position_zero_for_missing_log(tmp_path):
    p = tmp_path / "ghost.log"
    assert _firewall_log_position(p) == 0


def test_position_matches_file_size(tmp_path):
    p = _write_log(tmp_path, "")
    assert _firewall_log_position(p) == p.stat().st_size


def test_blank_lines_skipped(tmp_path):
    body = (
        "\n\n"
        "2026-04-22 12:34:56 DROP TCP 127.0.0.1 9.9.9.9 1 80 "
        "60 - 0 0 0 - - - SEND\n"
        "\n"
    )
    p = _write_log(tmp_path, body)
    rows = _parse_firewall_log_range(0, p.stat().st_size, log_path=p)
    assert len(rows) == 1


def test_non_digit_ports_become_zero(tmp_path):
    body = (
        "2026-04-22 12:34:56 DROP ICMP 127.0.0.1 9.9.9.9 - - "
        "60 - 0 0 0 - - - SEND\n"
    )
    p = _write_log(tmp_path, body)
    rows = _parse_firewall_log_range(0, p.stat().st_size, log_path=p)
    assert len(rows) == 1
    assert rows[0]['src_port'] == 0
    assert rows[0]['dst_port'] == 0
    assert rows[0]['protocol'] == 'icmp'


if __name__ == '__main__':
    raise SystemExit(pytest.main([__file__, '-v']))
