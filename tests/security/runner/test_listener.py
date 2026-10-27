#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Unit tests for the loopback listener.

Listener is bound to 127.0.0.1 only; tests connect from the same
loopback. No network egress, no admin needed, no PDFE needed.
"""

import socket
import sys
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from listener import LoopbackListener


def _free_port():
    """Pick a free TCP port on loopback so tests don't collide with
    a long-running upstream listener if one was left around."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]
    finally:
        s.close()


def test_listener_starts_and_stops_clean():
    L = LoopbackListener(callback_port=_free_port(),
                          extra_tcp_ports=(),
                          udp_ports=())
    L.start()
    assert L.callback_attempt_count() == 0
    L.stop()


def test_tcp_connection_recorded():
    port = _free_port()
    L = LoopbackListener(callback_port=port,
                          extra_tcp_ports=(),
                          udp_ports=())
    with L:
        # Wait briefly for the server thread to be listening.
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            try:
                s = socket.socket()
                s.settimeout(0.5)
                s.connect(('127.0.0.1', port))
                s.sendall(b'ping')
                s.close()
                break
            except OSError:
                time.sleep(0.05)
        # Allow the handler thread to record.
        time.sleep(0.3)
        events = L.events()
    assert any(e['protocol'] in ('tcp', 'http') for e in events), events
    e = events[0]
    assert e['port'] == port
    assert 'timestamp_iso' in e


def test_http_request_classified_as_http():
    port = _free_port()
    L = LoopbackListener(callback_port=port,
                          extra_tcp_ports=(),
                          udp_ports=())
    with L:
        time.sleep(0.2)
        s = socket.socket()
        s.settimeout(1.0)
        s.connect(('127.0.0.1', port))
        s.sendall(b'GET / HTTP/1.1\r\nHost: pdfe-test\r\n\r\n')
        try:
            s.recv(4096)
        except OSError:
            pass
        s.close()
        time.sleep(0.3)
        events = L.events()
    http_events = [e for e in events if e['protocol'] == 'http']
    assert http_events, events
    assert http_events[0]['payload_preview'].startswith('474554')  # 'GET' in hex


def test_udp_datagram_recorded():
    port = _free_port()
    L = LoopbackListener(callback_port=_free_port(),
                          extra_tcp_ports=(),
                          udp_ports=(port,))
    with L:
        time.sleep(0.2)
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.sendto(b'\x12\x34' + b'\x00' * 14, ('127.0.0.1', port))
        s.close()
        time.sleep(0.3)
        events = L.events()
    udp_events = [e for e in events if e['protocol'].startswith('udp')]
    assert udp_events, events


def test_callback_attempt_count_excludes_bind_failures():
    # Force a bind failure by holding the port from outside.
    port = _free_port()
    blocker = socket.socket()
    blocker.bind(('127.0.0.1', port))
    blocker.listen(1)
    try:
        L = LoopbackListener(callback_port=port,
                              extra_tcp_ports=(),
                              udp_ports=())
        L.start()
        # bind_failed event should be present but not count as
        # a "callback attempt".
        events = L.events()
        bind_fails = [e for e in events if e['protocol'] == 'bind_failed']
        assert bind_fails, events
        assert L.callback_attempt_count() == 0
        L.stop()
    finally:
        blocker.close()


def test_reset_clears_events_without_restarting_listener():
    port = _free_port()
    L = LoopbackListener(callback_port=port,
                          extra_tcp_ports=(),
                          udp_ports=())
    with L:
        time.sleep(0.2)
        s = socket.socket()
        s.connect(('127.0.0.1', port))
        s.close()
        time.sleep(0.3)
        assert L.callback_attempt_count() >= 1
        L.reset()
        assert L.callback_attempt_count() == 0
        # Server still running -- another connection should record.
        s2 = socket.socket()
        s2.connect(('127.0.0.1', port))
        s2.close()
        time.sleep(0.3)
        assert L.callback_attempt_count() >= 1


def test_listener_only_binds_loopback():
    """Verify the listener refuses external connections by virtue
    of binding to 127.0.0.1 only. We can't easily test "external"
    here without another network interface, but we can verify the
    server's address family + bound IP."""
    port = _free_port()
    L = LoopbackListener(callback_port=port,
                          extra_tcp_ports=(),
                          udp_ports=())
    L.start()
    try:
        # Inspect the underlying server socket.
        for s in L._servers:
            host, _ = s.server_address[:2]
            assert host == '127.0.0.1', f'listener bound to {host}, not loopback'
    finally:
        L.stop()


if __name__ == '__main__':
    raise SystemExit(pytest.main([__file__, '-v']))
