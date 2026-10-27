#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Multi-protocol loopback listener for the phase-3 sandboxed runner.

Bound exclusively to 127.0.0.1 (and ::1). Catches any connection
attempt PDFE makes during a malicious-PDF run -- TCP, UDP, HTTP,
and DNS over TCP/UDP. ICMP echo is not listened for in user-mode
Python (raw socket needs admin and is platform-specific); the
phase-3 firewall rule blocks it instead and the fact that it was
attempted shows up as a Defender / firewall log entry that the
caller can correlate.

Each connection observed is appended to an in-memory event list as
a dict with: protocol, peer_port (the *connecting* port from PDFE,
helps disambiguate concurrent payloads), timestamp_iso,
payload_preview (first 200 bytes hex-escaped, or None for
no-data observations like a TCP SYN that closes before sending).

The listener is callable as a context manager:

    with LoopbackListener(callback_port=9999) as L:
        run_pdfe_against(payload)
    events = L.events()

It auto-binds free side ports (the well-known callback_port for the
upstream malicious-pdf default + a few decoys) so payloads with
hardcoded ports get caught.
"""

from __future__ import annotations

import datetime
import socket
import socketserver
import threading
import time
from typing import Any


def _now_iso() -> str:
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='microseconds')


def _hex_preview(data: bytes, max_bytes: int = 200) -> str:
    if not data:
        return ''
    head = data[:max_bytes]
    return head.hex()


class LoopbackListener:
    """Bind a TCP, UDP, and HTTP listener on 127.0.0.1.

    The HTTP server is just a TCP server that records the request
    line + headers + body and replies 204 (no content). Any DNS
    queries that PDFE happens to send via UDP land in the UDP
    handler and are recorded as `protocol="udp_dns"` if they parse
    as a DNS query header, else `udp`.
    """

    def __init__(
        self,
        callback_port: int = 9999,
        extra_tcp_ports: tuple[int, ...] = (80, 443, 8080, 8443),
        udp_ports: tuple[int, ...] = (53, 9999),
    ) -> None:
        self._tcp_ports = (callback_port,) + extra_tcp_ports
        self._udp_ports = udp_ports
        self._events: list[dict[str, Any]] = []
        self._events_lock = threading.Lock()
        self._servers: list[Any] = []
        self._threads: list[threading.Thread] = []
        self._started = False

    # ---------------- handlers ----------------------------------

    def _record(self, **kw: Any) -> None:
        kw['timestamp_iso'] = _now_iso()
        with self._events_lock:
            self._events.append(kw)

    def _make_tcp_handler(self, port: int):
        outer = self
        class _H(socketserver.BaseRequestHandler):
            def handle(self) -> None:
                peer = self.client_address
                try:
                    self.request.settimeout(0.5)
                    data = b''
                    try:
                        while True:
                            chunk = self.request.recv(4096)
                            if not chunk:
                                break
                            data += chunk
                            if len(data) > 4096:
                                break
                    except (socket.timeout, OSError):
                        pass
                    is_http = (data[:4] in (b'GET ', b'POST', b'HEAD',
                                             b'PUT ', b'DELE', b'OPTI')
                               or data.startswith(b'PROPFIND'))
                    proto = 'http' if is_http else 'tcp'
                    outer._record(
                        protocol      = proto,
                        port          = port,
                        peer_port     = peer[1],
                        payload_preview = _hex_preview(data),
                    )
                    if is_http:
                        self.request.sendall(
                            b'HTTP/1.1 204 No Content\r\nServer: pdfe-loopback-listener\r\n\r\n')
                except Exception:
                    # Don't propagate from the handler -- a misbehaving
                    # client mustn't kill the server thread.
                    pass
        return _H

    def _make_udp_handler(self, port: int):
        outer = self
        class _H(socketserver.BaseRequestHandler):
            def handle(self) -> None:
                data, _sock = self.request
                peer = self.client_address
                # Heuristic: if the upper bits of byte 2 look like a DNS
                # response/query flag and the question count looks sane
                # we tag as DNS; otherwise plain udp.
                proto = 'udp'
                if (len(data) >= 12 and data[4:6] != b'\x00\x00'):
                    proto = 'udp_dns'
                outer._record(
                    protocol      = proto,
                    port          = port,
                    peer_port     = peer[1],
                    payload_preview = _hex_preview(data),
                )
        return _H

    # ---------------- lifecycle ---------------------------------

    def start(self) -> None:
        if self._started:
            return
        for port in self._tcp_ports:
            try:
                server = socketserver.ThreadingTCPServer(
                    ('127.0.0.1', port), self._make_tcp_handler(port))
            except OSError:
                # Port may be in use; record but don't fail-open.
                self._record(
                    protocol = 'bind_failed',
                    port = port,
                    peer_port = 0,
                    payload_preview = None,
                )
                continue
            server.daemon_threads = True
            t = threading.Thread(target=server.serve_forever, daemon=True)
            t.start()
            self._servers.append(server)
            self._threads.append(t)
        for port in self._udp_ports:
            try:
                server = socketserver.ThreadingUDPServer(
                    ('127.0.0.1', port), self._make_udp_handler(port))
            except OSError:
                self._record(
                    protocol = 'bind_failed',
                    port = port,
                    peer_port = 0,
                    payload_preview = None,
                )
                continue
            t = threading.Thread(target=server.serve_forever, daemon=True)
            t.start()
            self._servers.append(server)
            self._threads.append(t)
        self._started = True

    def stop(self) -> None:
        for s in self._servers:
            try:
                s.shutdown()
                s.server_close()
            except Exception:
                pass
        self._servers = []
        self._threads = []
        self._started = False

    # ---------------- access ------------------------------------

    def events(self) -> list[dict[str, Any]]:
        with self._events_lock:
            return list(self._events)

    def callback_attempt_count(self) -> int:
        """Connections actually attempted by something else (excludes
        bind_failed bookkeeping events)."""
        return sum(1 for e in self.events() if e['protocol'] != 'bind_failed')

    def reset(self) -> None:
        """Clear event buffer between PDFs without restarting the
        servers."""
        with self._events_lock:
            self._events = []

    # ---------------- context manager ---------------------------

    def __enter__(self) -> 'LoopbackListener':
        self.start()
        return self

    def __exit__(self, *exc: Any) -> None:
        self.stop()


def main() -> int:
    """CLI entry: start the listener, print events as they arrive,
    Ctrl-C to stop. Useful for manual smoke-testing during phase-3
    development."""
    import argparse, json, signal, sys
    ap = argparse.ArgumentParser()
    ap.add_argument('--callback-port', type=int, default=9999)
    ap.add_argument('--idle-seconds', type=float, default=30.0,
                    help='exit after this many seconds with no events')
    args = ap.parse_args()

    listener = LoopbackListener(callback_port=args.callback_port)
    listener.start()
    print(json.dumps({
        'started': True,
        'tcp_ports': listener._tcp_ports,
        'udp_ports': listener._udp_ports,
    }))
    sys.stdout.flush()

    stop = {'flag': False}
    def _sig(*_):
        stop['flag'] = True
    signal.signal(signal.SIGINT, _sig)
    last = time.monotonic()
    last_count = 0
    try:
        while not stop['flag']:
            time.sleep(0.5)
            cur = listener.callback_attempt_count()
            if cur > last_count:
                # Print only the new events.
                for e in listener.events()[last_count:]:
                    print(json.dumps(e))
                    sys.stdout.flush()
                last_count = cur
                last = time.monotonic()
            elif time.monotonic() - last > args.idle_seconds:
                break
    finally:
        listener.stop()
    print(json.dumps({'stopped': True,
                      'total_events': listener.callback_attempt_count()}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
