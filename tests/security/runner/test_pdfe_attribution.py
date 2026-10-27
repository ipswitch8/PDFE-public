#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Unit tests for the PDFE-attribution layer in run_corpus.

Background: pfirewall.log records every dropped packet on the host
with no process attribution. The corpus runner's netsh DROP rule
already scopes the BLOCK to PDFE.exe, but the *log* still captures
ambient subnet traffic (broadcasts from svchost, mDNS responder,
other VMs on the same /24, etc.). This produced a 25x inflation in
the 2026-04-26 corpus run's `non_loopback_blocked` total.

The fix: snapshot PDFE.exe's owned outbound sockets via `netstat -ano`
during the test window, then filter pfirewall.log DROP entries to
those PDFE actually held. These tests exercise that filter without
requiring PDFE / netstat / firewall / admin -- the input is a list of
synthetic drop dicts and a synthetic socket-tuple set.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_corpus import _attribute_drops_to_pdfe  # noqa: E402


def _drop(proto, src_port, dst_ip, dst_port):
    return {
        "timestamp_iso": "2026-04-26T12:00:00",
        "action": "DROP",
        "protocol": proto,
        "src_ip": "10.0.0.1",
        "dst_ip": dst_ip,
        "src_port": src_port,
        "dst_port": dst_port,
    }


def test_empty_socket_set_filters_everything():
    drops = [
        _drop("udp", 12345, "224.0.0.252", 5355),
        _drop("tcp", 23456, "192.168.0.50", 443),
    ]
    assert _attribute_drops_to_pdfe(drops, set()) == []


def test_exact_tuple_match():
    drops = [
        _drop("tcp", 23456, "192.168.0.50", 443),
        _drop("tcp", 99999, "192.168.0.50", 443),  # different src_port
    ]
    pdfe_sockets = {("tcp", 23456, "192.168.0.50", 443)}
    out = _attribute_drops_to_pdfe(drops, pdfe_sockets)
    assert len(out) == 1
    assert out[0]["src_port"] == 23456


def test_fuzzy_match_for_kernel_multicast():
    # LLMNR multicast: dst_ip = 224.0.0.252 wouldn't appear in
    # netstat output (kernel resolver socket), but if PDFE owns
    # the same src_port for some other dst the fuzzy match accepts
    # the multicast drop too.
    drops = [
        _drop("udp", 55003, "224.0.0.252", 5355),  # LLMNR v4
        _drop("udp", 55003, "ff02::1:3", 5355),    # LLMNR v6
        _drop("udp", 60000, "224.0.0.252", 5355),  # not PDFE's src_port
    ]
    pdfe_sockets = {("udp", 55003, "192.168.0.1", 53)}
    out = _attribute_drops_to_pdfe(drops, pdfe_sockets)
    # Only the two drops sharing src_port 55003 should match.
    assert len(out) == 2
    assert all(d["src_port"] == 55003 for d in out)


def test_protocol_distinguishes():
    drops = [
        _drop("tcp", 8080, "192.168.0.50", 443),
        _drop("udp", 8080, "192.168.0.50", 443),
    ]
    pdfe_sockets = {("tcp", 8080, "192.168.0.50", 443)}
    out = _attribute_drops_to_pdfe(drops, pdfe_sockets)
    assert len(out) == 1
    assert out[0]["protocol"] == "tcp"


def test_preserves_input_order():
    drops = [
        _drop("tcp", 1001, "1.1.1.1", 80),
        _drop("tcp", 1002, "2.2.2.2", 80),
        _drop("tcp", 1003, "3.3.3.3", 80),
    ]
    pdfe_sockets = {
        ("tcp", 1001, "1.1.1.1", 80),
        ("tcp", 1003, "3.3.3.3", 80),
    }
    out = _attribute_drops_to_pdfe(drops, pdfe_sockets)
    assert [d["src_port"] for d in out] == [1001, 1003]


def test_subnet_ambient_traffic_filtered_out():
    # The 2026-04-26 corpus storm pattern: 4 payloads each saw 400+
    # LLMNR drops with multi-IP sources (10.10.10.x, 192.168.0.x,
    # fe80::...). These could not have been PDFE-emitted because PDFE
    # only had a few sockets, on one src_port each. Confirm the
    # attribution filter strips the ambient noise while keeping any
    # genuine PDFE traffic.
    drops = []
    # 100 ambient drops from various src_ports we can't predict
    for i in range(100):
        drops.append(_drop("udp", 30000 + i, "224.0.0.252", 5355))
    # 1 genuine PDFE drop
    drops.append(_drop("tcp", 12345, "192.168.0.99", 80))
    pdfe_sockets = {("tcp", 12345, "192.168.0.99", 80)}
    out = _attribute_drops_to_pdfe(drops, pdfe_sockets)
    assert len(out) == 1
    assert out[0]["dst_ip"] == "192.168.0.99"
