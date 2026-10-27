#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Phase-3 sandboxed PDFE runner against the malicious-PDF corpus.

For every payload listed in the manifest:
  1. Verify Windows Firewall rule blocks all outbound traffic to
     non-loopback addresses for PDFE.exe (set up at start, torn
     down at exit).
  2. Reset the loopback listener event buffer.
  3. Launch PDFE.exe with the payload via pywinauto.
  4. Wait up to N seconds for any callback / dialog / crash.
  5. Close the document via File > Close (or kill if it's hung).
  6. Snapshot listener events + process state into a per-PDF
     observation record.
  7. Run the pure decide() logic to produce final_decision /
     final_decision_reason.

Output: tests/security/reports/corpus_run_<timestamp>.{json,md}
plus tests/security/reports/corpus_run_<timestamp>_loopback.log
(the canonical exfil evidence file).

The runner refuses to start without administrator elevation
(needed for the netsh firewall calls) and prints a clear bail-out
message instead of silently leaving the host unprotected while
opening malicious PDFs.

This module is also importable; the public functions are exposed
for testing without launching PDFE:

    build_observation(...) - assemble an observation dict from
                              listener events + process metrics
    classify(...)          - thin wrapper around decision.decide()
"""

from __future__ import annotations

import argparse
import ctypes
import datetime
import json
import os
import platform
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

# decision.py + listener.py live next to this file
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))
from decision import decide, summarize, VALID_EXPECTED  # noqa: E402
from listener import LoopbackListener                    # noqa: E402


# ============================================================
# Elevation / preflight
# ============================================================

def _is_windows() -> bool:
    return platform.system() == 'Windows'


def is_elevated() -> bool:
    """True if the current process is running with administrator rights."""
    if not _is_windows():
        # Non-Windows: phase 3 isn't supported; we still let the
        # offline test path import but the corpus-run entry point
        # bails.
        return False
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


def _firewall_rule_name() -> str:
    return 'PDFE-SecTest-BlockOutbound'


def _add_firewall_rule(pdfe_exe: Path) -> None:
    # Block all outbound EXCEPT to 127.0.0.1. A loopback-allow + block-
    # everything-else combo, scoped to the PDFE.exe binary so we don't
    # break the host's other traffic.
    #
    # netsh quirks discovered while wiring this up:
    #   1. remoteip rejects a mixed IPv4+IPv6 list like
    #      `127.0.0.1,::1` with "specified IP address or address
    #      keyword is not valid".
    #   2. Some netsh builds reject `remoteip=::1` outright (also
    #      with the same generic error), even when the rule is
    #      otherwise well-formed.
    # IPv6 loopback isn't worth fighting -- our listener.py only
    # binds 127.0.0.1, the corpus payloads phone home to
    # 127.0.0.1 (we configure --callback-host=127.0.0.1 in
    # gen_malicious_corpus.ps1), and the firewall log parser
    # filters `::1` out of the non_loopback_blocked list anyway.
    # If PDFE somehow tried ::1, it would be caught by the block
    # rule and silently dropped -- a minor coverage gap that's
    # documented in tests/security/runner/README.md Limitations.
    name_block = _firewall_rule_name() + '-Block'
    name_allow_v4 = _firewall_rule_name() + '-AllowLoopbackV4'
    # Idempotency: netsh allows duplicate rule names, so a previous
    # aborted run could leave residue. Delete any existing rules with
    # our names before re-adding -- this makes the runner safe to
    # invoke repeatedly without administrators having to clean up by
    # hand.
    for n in (name_allow_v4, name_block,
              _firewall_rule_name() + '-AllowLoopback',    # legacy
              _firewall_rule_name() + '-AllowLoopbackV6'):  # legacy
        subprocess.run([
            'netsh', 'advfirewall', 'firewall', 'delete', 'rule',
            f'name={n}',
        ], capture_output=True)
    # Allow IPv4 loopback first (rules evaluate in order; allow wins).
    subprocess.run([
        'netsh', 'advfirewall', 'firewall', 'add', 'rule',
        f'name={name_allow_v4}',
        'dir=out', 'action=allow',
        f'program={str(pdfe_exe)}',
        'remoteip=127.0.0.1',
    ], check=True, capture_output=True)
    subprocess.run([
        'netsh', 'advfirewall', 'firewall', 'add', 'rule',
        f'name={name_block}',
        'dir=out', 'action=block',
        f'program={str(pdfe_exe)}',
    ], check=True, capture_output=True)


def _remove_firewall_rule() -> None:
    for n in (_firewall_rule_name() + '-AllowLoopbackV4',
              _firewall_rule_name() + '-AllowLoopbackV6',  # legacy
              _firewall_rule_name() + '-AllowLoopback',    # legacy
              _firewall_rule_name() + '-Block'):
        subprocess.run([
            'netsh', 'advfirewall', 'firewall', 'delete', 'rule',
            f'name={n}',
        ], capture_output=True)


# ============================================================
# Windows Firewall log: capture non-loopback DROPs per payload
# ============================================================
#
# Karen-flagged HIGH: without reading pfirewall.log, the runner's
# firewall block rule catches non-loopback outbound but produces no
# evidence -- non_loopback_blocked is always [] and decide()'s
# 'exfiltration_attempt_nonloopback' branch is dead.
#
# Approach: enable Windows Firewall logging on dropped connections
# at the start of the run, snapshot the log file size before each
# payload launches, and after the payload completes parse the byte
# range we appended and filter for DROP entries to non-loopback
# destinations. The pfirewall.log line format is documented under
# the netsh advfirewall help -- the columns we use are:
#   date time action protocol src-ip dst-ip src-port dst-port ...
# Lines starting with '#' are headers; blank lines are separators.

def _firewall_log_path() -> Path:
    sysroot = os.environ.get('SystemRoot', r'C:\Windows')
    return Path(sysroot) / 'System32' / 'LogFiles' / 'Firewall' / 'pfirewall.log'


def _enable_firewall_logging() -> None:
    # Enable dropped-packet logging on currentprofile. Idempotent --
    # netsh tolerates re-enabling.
    subprocess.run([
        'netsh', 'advfirewall', 'set', 'currentprofile',
        'logging', 'droppedconnections', 'enable',
    ], capture_output=True)


def _disable_firewall_logging() -> None:
    subprocess.run([
        'netsh', 'advfirewall', 'set', 'currentprofile',
        'logging', 'droppedconnections', 'disable',
    ], capture_output=True)


def _firewall_log_position(log_path: Path | None = None) -> int:
    """Current size of pfirewall.log so callers can later read only
    the bytes appended since this snapshot. log_path arg exists for
    tests; production callers leave it None to use the system log."""
    p = log_path or _firewall_log_path()
    try:
        return p.stat().st_size if p.exists() else 0
    except Exception:
        return 0


_LOOPBACK_DSTS = ('127.0.0.1', '::1', '0.0.0.0', '::')


def _snapshot_pdfe_sockets(pid: int) -> set[tuple[str, int, str, int]]:
    """Snapshot the outbound sockets owned by `pid` via `netstat -ano`.

    Returns a set of (proto_lower, src_port, dst_ip, dst_port) tuples,
    one per active outbound binding. Listening sockets and sockets
    bound to a non-routable peer ("0.0.0.0:0", "*:*") are excluded.

    Used to attribute pfirewall.log DROP entries to PDFE.exe at the
    socket-tuple level. The pfirewall.log itself records no process
    info, so without this correlation the log captures every drop on
    the host during the test window — including ambient traffic from
    other processes / VMs / containers on the same subnet, which has
    historically inflated the non_loopback_blocked count by 25x.
    """
    if pid <= 0:
        return set()
    try:
        result = subprocess.run(
            ['netstat', '-ano', '-p', 'TCP'],
            capture_output=True, text=True, timeout=2.0)
        tcp_out = result.stdout
        result = subprocess.run(
            ['netstat', '-ano', '-p', 'UDP'],
            capture_output=True, text=True, timeout=2.0)
        udp_out = result.stdout
    except Exception:
        return set()

    out: set[tuple[str, int, str, int]] = set()

    # netstat -ano output:
    #   Proto  Local Address  Foreign Address  State  PID
    # UDP rows have no State column.
    def parse_endpoint(s: str) -> tuple[str, int]:
        # Handle IPv6 [::]:port and IPv4 a.b.c.d:port; UDP foreign
        # address is often "*:*".
        if s == '*:*':
            return ('*', 0)
        if s.startswith('['):
            # IPv6 — find closing bracket
            close = s.rfind(']')
            if close < 0 or close + 2 > len(s):
                return ('', 0)
            host = s[1:close]
            port_str = s[close + 2:]
        else:
            colon = s.rfind(':')
            if colon < 0:
                return ('', 0)
            host = s[:colon]
            port_str = s[colon + 1:]
        try:
            return (host, int(port_str))
        except ValueError:
            return (host, 0)

    pid_str = str(pid)
    for proto, text in (('tcp', tcp_out), ('udp', udp_out)):
        for raw in text.splitlines():
            line = raw.strip()
            if not line or not line.startswith(('TCP', 'UDP')):
                continue
            parts = line.split()
            if proto == 'tcp':
                # TCP <local> <remote> <state> <pid>
                if len(parts) < 5:
                    continue
                local, remote, _state, owner_pid = parts[1], parts[2], parts[3], parts[4]
            else:
                # UDP <local> <remote> <pid> (no state)
                if len(parts) < 4:
                    continue
                local, remote, owner_pid = parts[1], parts[2], parts[3]
            if owner_pid != pid_str:
                continue
            local_host, local_port = parse_endpoint(local)
            remote_host, remote_port = parse_endpoint(remote)
            if local_port <= 0:
                continue
            # Skip listening / unbound sockets (no real peer).
            if remote_host in ('', '*', '0.0.0.0', '::', '[::]'):
                continue
            out.add((proto, local_port, remote_host, remote_port))
    return out


def _attribute_drops_to_pdfe(
    drops: list[dict[str, Any]],
    pdfe_sockets: set[tuple[str, int, str, int]],
) -> list[dict[str, Any]]:
    """Filter pfirewall.log DROP entries to those PDFE.exe is
    plausibly responsible for.

    Match criterion: the drop's (proto, src_port, dst_ip, dst_port)
    tuple matches a socket PDFE held during the test window. A drop
    can also match a "fuzzy" key (proto, src_port, *, dst_port) for
    multicast destinations whose dst_ip we can't predict from the
    socket snapshot (LLMNR / mDNS broadcasts from a kernel resolver
    socket the user-mode netstat won't list).

    Returns drops in the same order as input, unchanged dicts.
    """
    if not pdfe_sockets:
        return []
    src_port_set = {(p, sp) for (p, sp, _di, _dp) in pdfe_sockets}
    full_set = pdfe_sockets
    out: list[dict[str, Any]] = []
    for d in drops:
        key = (d.get('protocol', ''), d.get('src_port', 0),
               d.get('dst_ip', ''), d.get('dst_port', 0))
        if key in full_set:
            out.append(d)
            continue
        # Fuzzy match: same proto+src_port, kernel-emitted multicast
        # the netstat snapshot may have missed.
        loose = (d.get('protocol', ''), d.get('src_port', 0))
        if loose in src_port_set:
            out.append(d)
    return out


def _parse_firewall_log_range(start_pos: int, end_pos: int,
                              log_path: Path | None = None
                              ) -> list[dict[str, Any]]:
    """Parse pfirewall.log entries between byte offsets [start_pos,
    end_pos). Returns DROP entries to non-loopback destinations.
    Tolerates rotation (file shrunk) and missing log file. log_path
    arg is for tests; production callers leave it None."""
    p = log_path or _firewall_log_path()
    if not p.exists():
        return []
    try:
        size = p.stat().st_size
    except Exception:
        return []
    # Rotation: if the file shrunk below start_pos, read from 0 up to
    # min(end_pos, size). This may double-count on a single rotation
    # but never silently drops drops we care about.
    if size < start_pos:
        start_pos = 0
    if end_pos > size:
        end_pos = size
    if end_pos <= start_pos:
        return []
    try:
        with p.open('rb') as f:
            f.seek(start_pos)
            data = f.read(end_pos - start_pos)
    except Exception:
        return []
    out: list[dict[str, Any]] = []
    # pfirewall.log is logged in the system codepage but ASCII for
    # the fields we read; latin-1 with replace is safest.
    for line in data.decode('latin-1', errors='replace').splitlines():
        if not line or line.startswith('#'):
            continue
        parts = line.split()
        if len(parts) < 8:
            continue
        try:
            date, t, action, protocol = parts[:4]
            src_ip, dst_ip, src_port, dst_port = parts[4:8]
        except ValueError:
            continue
        if action != 'DROP':
            continue
        if dst_ip in _LOOPBACK_DSTS:
            continue
        try:
            src_port_i = int(src_port) if src_port.isdigit() else 0
            dst_port_i = int(dst_port) if dst_port.isdigit() else 0
        except ValueError:
            src_port_i, dst_port_i = 0, 0
        out.append({
            'timestamp_iso': f'{date}T{t}',
            'action':        action,
            'protocol':      protocol.lower(),
            'src_ip':        src_ip,
            'dst_ip':        dst_ip,
            'src_port':      src_port_i,
            'dst_port':      dst_port_i,
        })
    return out


# ============================================================
# Observation building (testable without launching PDFE)
# ============================================================

def build_observation(
    relative_path: str,
    listener_events: list[dict[str, Any]],
    *,
    opened_ok: bool,
    crashed: bool,
    hang: bool,
    memory_blowup: bool,
    exit_code: int,
    stderr_tail: str,
    wer_dump_path_if_any: str | None,
    non_loopback_blocked: list[dict[str, Any]] | None = None,
    non_loopback_blocked_pdfe_attributed:
        list[dict[str, Any]] | None = None,
    launch_dialog_seen: bool = False,
    submit_dialog_seen: bool = False,
    js_alert_seen: bool = False,
) -> dict[str, Any]:
    """Assemble the per-PDF observation dict in the schema decide()
    expects. Public so tests can build synthetic observations
    without driving PDFE."""
    return {
        'relative_path':         relative_path,
        'opened_ok':             opened_ok,
        'crashed':               crashed,
        'hang':                  hang,
        'memory_blowup':         memory_blowup,
        'exit_code':             exit_code,
        'stderr_tail':           stderr_tail,
        'wer_dump_path_if_any':  wer_dump_path_if_any,
        'loopback_attempts':     list(listener_events),
        # Raw firewall log drops in the test window. Includes all
        # ambient subnet traffic the host emitted, not just PDFE's.
        'non_loopback_blocked':  list(non_loopback_blocked or []),
        # Subset of non_loopback_blocked attributed to PDFE.exe via
        # netstat socket-tuple matching during the test window. This
        # is the field decision logic should use; the raw field stays
        # for back-compat and forensics.
        'non_loopback_blocked_pdfe_attributed':
            list(non_loopback_blocked_pdfe_attributed or []),
        'launch_dialog_seen':    launch_dialog_seen,
        'submit_dialog_seen':    submit_dialog_seen,
        'js_alert_seen':         js_alert_seen,
    }


def classify(observation: dict[str, Any], expected: str
             ) -> dict[str, Any]:
    """Thin wrapper around decision.decide that augments the
    observation with the verdict fields. Public for testability."""
    final, reason = decide(observation, expected)
    out = dict(observation)
    out['expected_pdfe_behavior'] = expected
    out['final_decision'] = final
    out['final_decision_reason'] = reason
    return out


# ============================================================
# PDFE driving (live; gated on admin + AV-exclusion preflight)
# ============================================================

_KILL_GRACE_S = 3.0  # how long we wait after kill() before declaring hang


def _launch_and_observe(
    pdfe_exe: Path,
    payload: Path,
    listener: LoopbackListener,
    timeout_s: float,
    rss_ceiling_bytes: int,
    socket_capture: set[tuple[str, int, str, int]] | None = None,
) -> dict[str, Any]:
    """Launch PDFE with the payload, wait, snapshot, close. Returns
    a dict with the live-run observation fields. Imports pywinauto
    lazily so the offline test path can import this module without
    pywinauto installed.

    Hang semantics (karen-flagged MEDIUM, fixed): a "hang" means PDFE
    failed to terminate within _KILL_GRACE_S after we asked it to
    close. Reaching the wait-budget deadline is the *normal* path --
    PDFE doesn't auto-close after opening a PDF, so the runner is
    responsible for closing it. Only label hang=True when kill() did
    not actually terminate the process."""
    # Reset listener so events are scoped to this run.
    listener.reset()

    try:
        from pywinauto import Application  # type: ignore
    except ImportError:
        return {
            'opened_ok':           False,
            'crashed':             False,
            'hang':                False,
            'memory_blowup':       False,
            'exit_code':           -1,
            'stderr_tail':         'pywinauto not installed; runner cannot drive PDFE',
            'wer_dump_path_if_any': None,
        }

    try:
        import psutil  # type: ignore
    except ImportError:
        psutil = None  # type: ignore

    app = None
    proc_info = {
        'opened_ok':           False,
        'crashed':             False,
        'hang':                False,
        'memory_blowup':       False,
        'exit_code':           0,
        'stderr_tail':         '',
        'wer_dump_path_if_any': None,
    }
    start = time.monotonic()
    pid: int | None = None
    crashed_during_wait = False
    # Defense-in-depth: pywinauto.Application.start takes a single
    # command-line string and we double-quote each token; an embedded
    # `"` in pdfe_exe or payload would break out and inject extra
    # arguments. Reject any path containing a double-quote rather
    # than try to escape -- both fields come from local files we
    # control, so a quote here is anomalous.
    if '"' in str(pdfe_exe) or '"' in str(payload):
        return {
            'opened_ok':           False,
            'crashed':             False,
            'hang':                False,
            'memory_blowup':       False,
            'exit_code':           -1,
            'stderr_tail':         'path contains double-quote; refused to launch',
            'wer_dump_path_if_any': None,
        }
    try:
        # wait_for_idle=False is REQUIRED here: PDFE.exe Debug builds
        # have PE subsystem=CONSOLE (Qt's debug behavior to keep
        # stdout/stderr connected), and pywinauto's default
        # `WaitForInputIdle` call rejects CONSOLE-subsystem processes
        # with ERROR_NOT_GUI_PROCESS (1471) -- even though PDFE
        # actually creates GUI windows fine. We skip the WaitForInputIdle
        # and rely on the explicit `win.wait('visible', ...)` below
        # to confirm the GUI is up.
        app = Application(backend='uia').start(
            f'"{pdfe_exe}" "{payload}"',
            wait_for_idle=False)
        pid = getattr(app, 'process', None)
        # Wait for the main window or until timeout / crash.
        # opened_ok is True only when PDFE actually loaded the
        # document (title becomes "PDFE - [filename]"), NOT just
        # when its main chrome appeared (title "PDFE"). Without
        # this distinction, a magic-byte / UNC refusal where
        # Document::open throws but the main window stays visible
        # would falsely report opened_ok=True and trigger
        # `opened_when_should_refuse` for refuse-expected payloads.
        try:
            win = app.window(title_re='PDFE.*')
            win.wait('visible', timeout=min(15.0, timeout_s))
            # Poll briefly for the loaded-document title pattern.
            # Document::open + initial render typically completes in
            # well under 2s after window-visible.
            doc_loaded = False
            poll_deadline = time.monotonic() + 2.0
            while time.monotonic() < poll_deadline:
                try:
                    title = win.window_text()
                    if title and 'PDFE - [' in title:
                        doc_loaded = True
                        break
                except Exception:
                    pass
                time.sleep(0.1)
            proc_info['opened_ok'] = doc_loaded
        except Exception:
            proc_info['opened_ok'] = False
        # Idle for the rest of the budget so passive callbacks have
        # time to fire. Sample RSS while we wait.
        deadline = start + timeout_s
        while time.monotonic() < deadline:
            time.sleep(0.5)
            # Snapshot PDFE's owned outbound sockets each tick so the
            # parser can later attribute pfirewall.log DROP entries to
            # this process. The set is accumulated across ticks because
            # short-lived sockets (e.g., a single DNS query, an LLMNR
            # broadcast on one ephemeral port) might exist for less
            # than a single sample interval. Best-effort: if the
            # snapshot fails for any reason, we just don't attribute
            # those drops.
            if socket_capture is not None and pid:
                try:
                    socket_capture |= _snapshot_pdfe_sockets(pid)
                except Exception:
                    pass
            if psutil and pid:
                try:
                    p = psutil.Process(pid)
                    rss = p.memory_info().rss
                    if rss > rss_ceiling_bytes:
                        proc_info['memory_blowup'] = True
                        break
                except psutil.NoSuchProcess:
                    crashed_during_wait = True
                    break
                except Exception:
                    # Transient psutil error -- don't conflate with
                    # crash; just keep waiting.
                    pass
        # Post-loop: if the process is gone and we didn't see a
        # memory blowup, that's a crash.
        if not proc_info['memory_blowup'] and pid is not None:
            if psutil:
                try:
                    if not psutil.pid_exists(pid):
                        crashed_during_wait = True
                except Exception:
                    pass
        if crashed_during_wait:
            proc_info['crashed'] = True
    except Exception as e:
        proc_info['crashed'] = True
        proc_info['stderr_tail'] = repr(e)[-4096:]
    finally:
        # Ask the process to close, then verify it actually died
        # within _KILL_GRACE_S. Hang is defined as "kill failed to
        # terminate the process" -- not "process was still alive at
        # the wait-budget deadline" (every well-behaved PDFE run
        # would falsely register as a hang under that definition).
        if app is not None and pid is not None:
            try:
                app.kill()
            except Exception:
                pass
            kill_deadline = time.monotonic() + _KILL_GRACE_S
            still_alive = True
            while time.monotonic() < kill_deadline:
                if psutil:
                    try:
                        if not psutil.pid_exists(pid):
                            still_alive = False
                            break
                    except Exception:
                        # If psutil is broken, fall back to a single
                        # taskkill below.
                        break
                else:
                    # No psutil -- assume kill() worked. The
                    # belt-and-braces taskkill in
                    # _kill_orphan_pdfe runs after this returns.
                    still_alive = False
                    break
                time.sleep(0.1)
            if still_alive and not proc_info['crashed']:
                proc_info['hang'] = True

    return proc_info


def _kill_orphan_pdfe() -> None:
    """Belt-and-braces orphan kill in case pywinauto's app.kill()
    didn't catch a child window subprocess."""
    if not _is_windows():
        return
    subprocess.run(['taskkill', '/F', '/IM', 'PDFE.exe', '/T'],
                   capture_output=True)


# ============================================================
# Main entry
# ============================================================

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--pdfe-exe', default=r'C:\Users\dev\source\PDFE\build\vs2022\bin\Debug\PDFE.exe')
    ap.add_argument('--manifest', default=str(_HERE.parent / 'corpus' / 'manifest.json'))
    ap.add_argument('--corpus',   default=str(_HERE.parent / 'corpus' / 'malicious_pdf'))
    ap.add_argument('--report-dir', default=str(_HERE.parent / 'reports'))
    ap.add_argument('--timeout-s', type=float, default=30.0)
    ap.add_argument('--rss-ceiling-mb', type=int, default=2048)
    ap.add_argument('--callback-port', type=int, default=9999)
    ap.add_argument('--require-admin', action='store_true', default=True,
                    help='Refuse to run without admin (default: on)')
    ap.add_argument('--allow-non-admin-dry-run', action='store_true',
                    help='Skip firewall setup; for offline development only')
    args = ap.parse_args()

    if args.require_admin and not args.allow_non_admin_dry_run \
            and not is_elevated():
        sys.stderr.write(
            '[run_corpus] ABORT: not running as administrator.\n'
            '[run_corpus] Phase 3 requires elevation for netsh firewall '
            'rules that block non-loopback outbound traffic.\n'
            '[run_corpus] Re-launch from an elevated PowerShell, or pass '
            '--allow-non-admin-dry-run for an offline-only smoke test that '
            'skips the network sandbox (NOT for real corpus runs).\n')
        return 2

    pdfe_exe = Path(args.pdfe_exe)
    if not pdfe_exe.exists():
        sys.stderr.write(f'[run_corpus] PDFE.exe not found: {pdfe_exe}\n')
        return 3

    manifest_path = Path(args.manifest)
    corpus_dir = Path(args.corpus)
    if not manifest_path.exists():
        sys.stderr.write(f'[run_corpus] manifest not found: {manifest_path}\n')
        return 3
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    payloads = manifest['payloads']

    report_dir = Path(args.report_dir)
    report_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    report_json = report_dir / f'corpus_run_{stamp}.json'
    report_md   = report_dir / f'corpus_run_{stamp}.md'
    listener_log = report_dir / f'corpus_run_{stamp}_loopback.log'

    listener = LoopbackListener(callback_port=args.callback_port)
    rss_ceiling = args.rss_ceiling_mb * 1024 * 1024

    if not args.allow_non_admin_dry_run:
        _add_firewall_rule(pdfe_exe)
        _enable_firewall_logging()
    listener.start()

    records: list[dict[str, Any]] = []
    corpus_dir_resolved = corpus_dir.resolve()
    try:
        with listener_log.open('w', encoding='utf-8') as logf:
            logf.write(f'# corpus run {stamp}\n')
            for entry in payloads:
                rel = entry['relative_path']
                payload = corpus_dir / rel
                # Defense-in-depth (security-audit informational): the
                # relative_path field is generated by build_manifest.py
                # and is treated as trusted-in-repo content, but a
                # `..\..\foo` traversal in a hostile manifest.json
                # would escape corpus_dir without this guard. Reject
                # any payload that doesn't resolve under corpus_dir.
                try:
                    payload_resolved = payload.resolve()
                    payload_resolved.relative_to(corpus_dir_resolved)
                except ValueError:
                    records.append(classify(build_observation(
                        rel, [], opened_ok=False, crashed=False,
                        hang=False, memory_blowup=False, exit_code=-1,
                        stderr_tail='payload escapes corpus_dir; refused',
                        wer_dump_path_if_any=None,
                    ), entry['expected_pdfe_behavior']))
                    continue
                except OSError:
                    pass  # missing file -- handled by .exists() below
                if not payload.exists():
                    # AV may have eaten it; report as coverage gap.
                    records.append(classify(build_observation(
                        rel, [], opened_ok=False, crashed=False,
                        hang=False, memory_blowup=False, exit_code=-1,
                        stderr_tail='payload not on disk (AV deletion?)',
                        wer_dump_path_if_any=None,
                    ), entry['expected_pdfe_behavior']))
                    continue

                listener.reset()
                fw_pos_before = _firewall_log_position() \
                    if not args.allow_non_admin_dry_run else 0
                pdfe_socket_capture: set[tuple[str, int, str, int]] = set()
                proc_info = _launch_and_observe(
                    pdfe_exe, payload, listener,
                    args.timeout_s, rss_ceiling,
                    socket_capture=pdfe_socket_capture)
                events = listener.events()
                for e in events:
                    logf.write(json.dumps({'payload': rel, **e}) + '\n')
                fw_drops: list[dict[str, Any]] = []
                fw_drops_attr: list[dict[str, Any]] = []
                if not args.allow_non_admin_dry_run:
                    fw_pos_after = _firewall_log_position()
                    fw_drops = _parse_firewall_log_range(
                        fw_pos_before, fw_pos_after)
                    fw_drops_attr = _attribute_drops_to_pdfe(
                        fw_drops, pdfe_socket_capture)
                    for d in fw_drops:
                        logf.write(json.dumps(
                            {'payload': rel, 'firewall_drop': d}) + '\n')

                obs = build_observation(
                    rel, events,
                    non_loopback_blocked=fw_drops,
                    non_loopback_blocked_pdfe_attributed=fw_drops_attr,
                    **proc_info)
                rec = classify(obs, entry['expected_pdfe_behavior'])
                rec['upstream_payload_id'] = entry['upstream_payload_id']
                rec['category'] = entry['category']
                records.append(rec)
                _kill_orphan_pdfe()

    finally:
        listener.stop()
        if not args.allow_non_admin_dry_run:
            _remove_firewall_rule()
            _disable_firewall_logging()
        _kill_orphan_pdfe()

    summary = summarize(records)
    report_json.write_text(json.dumps({
        'stamp':       stamp,
        'pdfe_exe':    str(pdfe_exe),
        'manifest':    str(manifest_path),
        'summary':     summary,
        'records':     records,
    }, indent=2), encoding='utf-8')

    md_lines = [
        f'# Phase 3 corpus run {stamp}',
        '',
        f'- PDFE: `{pdfe_exe}`',
        f'- Manifest: `{manifest_path}`',
        f'- Loopback log: `{listener_log.name}`',
        '',
        f'**Verdict: {summary["overall_verdict"].upper()}**',
        '',
        f'- Total payloads: {summary["total"]}',
        f'- Passed: {summary["passes"]}',
        f'- Failed: {summary["fails"]}',
        '',
        '## Counts per category',
        '',
        '| Category | Pass | Fail | Total |',
        '|---|---:|---:|---:|',
    ]
    for cat in sorted(summary['totals_by_category']):
        p = summary['passes_by_category'].get(cat, 0)
        f = summary['fails_by_category'].get(cat, 0)
        t = summary['totals_by_category'].get(cat, 0)
        md_lines.append(f'| `{cat}` | {p} | {f} | {t} |')
    md_lines.append('')
    md_lines.append('## Failures by reason')
    md_lines.append('')
    for reason, count in sorted(summary['fails_by_reason'].items(),
                                key=lambda kv: -kv[1]):
        md_lines.append(f'- `{reason}` × {count}')
    md_lines.append('')
    md_lines.append('## Failed payload list')
    md_lines.append('')
    for p in summary['failed_paths']:
        md_lines.append(f'- {p}')
    report_md.write_text('\n'.join(md_lines), encoding='utf-8')

    print(f'[run_corpus] {summary["overall_verdict"]} '
          f'({summary["passes"]}/{summary["total"]} pass)')
    print(f'[run_corpus] {report_md}')
    return 0 if summary['overall_verdict'] == 'pass' else 1


if __name__ == '__main__':
    raise SystemExit(main())
