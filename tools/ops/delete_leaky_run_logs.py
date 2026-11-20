#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Find, and optionally delete, Actions run logs that carry a scrubbed identifier.

WHY. check_tracker_hygiene.py --logs N fails when a recent run's log matches
the needle inventory. Every job on the self-hosted Windows runner writes that
machine's name into its set-up log, and the name is a needle (GitHub #409).
Deleting those logs is the clean-up; renaming the runner's machine is the fix,
and it belongs to the Windows runner's owner.

WHAT IT DOES. It reads the same window of runs, with the same needle inventory
and the same sensitivity control, as check_tracker_hygiene.py --logs. It lists
every run whose log has a hit: run id, workflow, branch and needle LABEL only,
never the matched text. With --apply it deletes those runs' logs
(`DELETE /repos/{repo}/actions/runs/{id}/logs`) and then re-reads each one to
confirm it is gone. The run records stay; only the log text goes.

It never deletes a log it did not just find a hit in, and it deletes nothing
without --apply. Deleted logs cannot be recovered.

Usage:
  python3 tools/ops/delete_leaky_run_logs.py [--logs 100] [--repo owner/name]
  python3 tools/ops/delete_leaky_run_logs.py --logs 100 --apply
"""
from __future__ import annotations

import argparse
import importlib.util
import io
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

for _n in ("stdout", "stderr"):
    _s = getattr(sys, _n, None)
    if _s is not None and hasattr(_s, "reconfigure"):
        _s.reconfigure(encoding="utf-8", errors="replace")

_HERE = os.path.dirname(os.path.abspath(__file__))
_GATE = os.path.join(os.path.dirname(_HERE), "gate")


def _load(name):
    spec = importlib.util.spec_from_file_location(
        name, os.path.join(_GATE, name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _download(gh, repo, rid, dest):
    with io.open(dest, "wb") as fh:
        p = subprocess.run([gh, "api", "repos/%s/actions/runs/%d/logs" % (repo, rid)],
                           stdout=fh, stderr=subprocess.PIPE)
    return p.returncode == 0 and os.path.getsize(dest) > 0


def leaky_runs(th, m, needles, gh, repo, window):
    """[(run dict, sorted needle labels)] for runs in the window with a hit.
    Returns None if the run list could not be read."""
    runs = th.api_json(gh, "repos/%s/actions/runs?per_page=100" % repo)
    if runs is None:
        return None
    flat = []
    for r in runs:
        flat.extend(r.get("workflow_runs", []) if isinstance(r, dict)
                    and "workflow_runs" in r else [r])
    flat = [r for r in flat if isinstance(r, dict) and r.get("id")]
    flat.sort(key=lambda r: r.get("created_at") or "", reverse=True)
    out = []
    tmp = tempfile.mkdtemp(prefix="pdfe-leaky-")
    try:
        for r in flat[:window]:
            zp = os.path.join(tmp, "%d.zip" % r["id"])
            if not _download(gh, repo, r["id"], zp):
                continue
            labels = set()
            try:
                with zipfile.ZipFile(zp) as z:
                    for name in z.namelist():
                        if not name.endswith("/"):
                            text = z.read(name).decode("utf-8", "replace")
                            labels.update(lab for _ln, lab in m.scan_text(text, needles))
            except zipfile.BadZipFile:
                continue
            finally:
                os.remove(zp)
            if labels:
                out.append((r, sorted(labels)))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo", default="ipswitch8/PDFE")
    ap.add_argument("--logs", type=int, default=100, metavar="N",
                    help="scan the N most recent runs (default 100)")
    ap.add_argument("--apply", action="store_true",
                    help="delete the logs of every run found (irreversible)")
    args = ap.parse_args(argv)

    th = _load("check_tracker_hygiene")
    gh = th._gh()
    if not gh:
        print("FAIL: gh not found")
        return 2
    m = th._load_guard()
    needles, err = m.load_all_needles()
    if err:
        print("FAIL: could not load the needle inventory: %s" % err)
        return 2
    # The same control the gate uses: a scan that cannot see a real identifier
    # finding "nothing" is not evidence that nothing is there.
    if not th.run_controls(m, needles):
        return 2

    found = leaky_runs(th, m, needles, gh, args.repo, args.logs)
    if found is None:
        print("FAIL: could not list workflow runs")
        return 2
    print("runs with a scrubbed identifier in their log: %d (of the %d most recent)"
          % (len(found), args.logs))
    for r, labels in found:
        print("  run %d  %-28s %-34s needle=%s"
              % (r["id"], (r.get("name") or "")[:28],
                 (r.get("head_branch") or "")[:34], ",".join(labels)))
    if not args.apply:
        if found:
            print("dry run: nothing deleted. Re-run with --apply to delete these logs.")
        return 0

    bad = 0
    for r, _labels in found:
        rid = r["id"]
        d = subprocess.run([gh, "api", "-X", "DELETE",
                            "repos/%s/actions/runs/%d/logs" % (args.repo, rid)],
                           capture_output=True, text=True)
        tmp = tempfile.mkdtemp(prefix="pdfe-leaky-check-")
        try:
            still = _download(gh, args.repo, rid, os.path.join(tmp, "x.zip"))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
        ok = d.returncode == 0 and not still
        bad += 0 if ok else 1
        print("  run %d: %s" % (rid, "logs deleted (verified gone)" if ok
                                else "FAILED to delete (%s)" % (d.stderr or "").strip()[:120]))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
