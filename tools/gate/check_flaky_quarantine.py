#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_flaky_quarantine.py — reruns must not silently mask a flaky test.

Remediation Phase 5, target R-37. The gtest/UI runners re-run a failed test in
a fresh process and go green if it heals (a "transient flake"). That is a
legitimate mitigation, but with no ledger it silently masks a test that is
DEGRADING toward broken. This check closes the loop:

  * ``tools/gate/flaky_quarantine.json`` is the registry of tests allowed to
    heal on rerun (``known_flaky``) plus expected-to-fail tests (``xfail``);
  * ``run_gtest.py`` writes ``healed_flakes.json`` (the tests that FAILED then
    PASSED on a fresh rerun this run);
  * this check FAILS when a test healed on rerun but is NOT registered, and when
    a registered entry has EXPIRED (so a quarantine is revisited, never eternal).

The pure decision core is ``evaluate`` (no I/O) for direct unit testing.

Usage:
  python tools/gate/check_flaky_quarantine.py \
      --registry tools/gate/flaky_quarantine.json \
      --healed build/gtest_ci_out/healed_flakes.json \
      [--today 2026-07-28]
Exit 0 = compliant; 1 = an untracked/expired flake; 2 = usage/IO error.
"""
from __future__ import annotations

import argparse
import datetime as _dt
import json
import sys
from pathlib import Path
from typing import Dict, List, Tuple


def _registered_names(registry: Dict) -> Dict[str, Dict]:
    out: Dict[str, Dict] = {}
    for key in ("known_flaky", "xfail"):
        for entry in registry.get(key, []) or []:
            name = (entry.get("test") or "").strip()
            if name:
                out[name] = entry
    return out


def _expired(entry: Dict, today: _dt.date) -> bool:
    exp = (entry.get("expires") or "").strip()
    if not exp:
        return False  # no expiry == indefinite (xfail on a tracked open defect)
    try:
        return _dt.date.fromisoformat(exp) < today
    except ValueError:
        return False


def evaluate(registry: Dict, healed: List[str], today: _dt.date
             ) -> Tuple[bool, List[str]]:
    """Return (ok, messages). ok is False on an untracked or expired flake."""
    reg = _registered_names(registry)
    msgs: List[str] = []
    ok = True

    untracked = [t for t in healed if t not in reg]
    if untracked:
        ok = False
        msgs.append("FAIL: test(s) healed on rerun but are NOT in the flaky "
                    "quarantine registry (an untracked flake would be silently "
                    "masked — register it with a reason/owner/expiry or fix it):")
        msgs.extend("    - %s" % t for t in sorted(untracked))

    # An expired entry that was actually exercised (healed this run) must be
    # revisited; also flag any expired registry entry so the ledger stays fresh.
    expired = [name for name, e in reg.items() if _expired(e, today)]
    if expired:
        ok = False
        msgs.append("FAIL: quarantine entr(y/ies) past their 'expires' date "
                    "(revisit — a flake must not be quarantined forever):")
        msgs.extend("    - %s (expires %s)" % (n, reg[n].get("expires")) for n in sorted(expired))

    tracked = [t for t in healed if t in reg]
    if tracked:
        msgs.append("note: %d healed test(s) tracked in the registry (tolerated): %s"
                    % (len(tracked), ", ".join(sorted(tracked))))
    if ok and not healed:
        msgs.append("OK: no tests healed on rerun this run.")
    elif ok:
        msgs.append("OK: every healed test is registered and no entry is expired.")
    return ok, msgs


def _load_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--registry", type=Path,
                    default=Path("tools/gate/flaky_quarantine.json"))
    ap.add_argument("--healed", type=Path, default=None,
                    help="healed_flakes.json produced by run_gtest.py. If absent "
                         "on disk, treated as an empty heal list (registry expiry "
                         "is still checked).")
    ap.add_argument("--today", default=None,
                    help="ISO date override for expiry checks (default: today).")
    args = ap.parse_args(argv)

    try:
        registry = _load_json(args.registry)
    except (OSError, ValueError) as exc:
        print("[flaky-quarantine] registry error: %s" % exc, file=sys.stderr)
        return 2

    healed: List[str] = []
    if args.healed and args.healed.exists():
        try:
            data = _load_json(args.healed)
            healed = list(data.get("healed", data) if isinstance(data, dict) else data)
        except (OSError, ValueError) as exc:
            print("[flaky-quarantine] healed-list error: %s" % exc, file=sys.stderr)
            return 2

    today = (_dt.date.fromisoformat(args.today) if args.today
             else _dt.date.today())
    ok, msgs = evaluate(registry, healed, today)
    for m in msgs:
        print("[flaky-quarantine] " + m)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
