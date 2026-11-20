# -*- coding: utf-8 -*-
"""Pins for tools/ops/delete_leaky_run_logs.py's safety claims.

The tool deletes Actions run logs, which cannot be undone, so its promises are
pinned rather than trusted:
  * without --apply it deletes NOTHING;
  * with --apply it deletes only the runs it just found a hit in, and verifies
    each one is gone;
  * it never prints the matched text, only run id / workflow / branch / label;
  * a failed sensitivity control stops it before any log is read.

GitHub, the log download and the needle scanner are stubbed: a synthetic
needle in run 7's log, a clean log in run 8.
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import os
import types
import zipfile

_HERE = os.path.dirname(os.path.abspath(__file__))
_TOOL = os.path.join(os.path.dirname(_HERE), "ops", "delete_leaky_run_logs.py")

SECRET = "zzsynth host"


def _load_tool():
    spec = importlib.util.spec_from_file_location("dlrl", _TOOL)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class _World:
    """Two runs: 7 leaks, 8 is clean. Records every DELETE issued."""

    def __init__(self, controls_ok=True):
        self.deleted = []
        self.downloads = []
        self.controls_ok = controls_ok
        self.runs = [
            {"id": 7, "name": "Fast Checks", "head_branch": "master",
             "created_at": "2026-11-16T02:00:00Z"},
            {"id": 8, "name": "Fast Checks", "head_branch": "master",
             "created_at": "2026-11-16T01:00:00Z"},
        ]

    # --- stand-ins for check_tracker_hygiene / the scrub guard -------------
    def th(self):
        world = self
        guard = types.SimpleNamespace(
            load_all_needles=lambda: ([{"label": "syn"}], None),
            scan_text=lambda text, needles: ([(1, "syn")] if SECRET in text else []))
        return types.SimpleNamespace(
            _gh=lambda: "gh",
            _load_guard=lambda: guard,
            run_controls=lambda m, needles: world.controls_ok,
            api_json=lambda gh, path: [{"workflow_runs": world.runs}])

    # --- stand-ins for the network ------------------------------------------
    def download(self, gh, repo, rid, dest):
        self.downloads.append(rid)
        if rid in self.deleted:
            return False                      # logs gone after a DELETE
        body = "Machine name: '%s'\n" % SECRET if rid == 7 else "all clean\n"
        with zipfile.ZipFile(dest, "w") as z:
            z.writestr("1_job.txt", body)
        return True

    def run(self, args, **_kw):
        if "DELETE" in args:
            rid = int(args[-1].split("/")[-2])
            self.deleted.append(rid)
        return types.SimpleNamespace(returncode=0, stdout="", stderr="")


def _run(argv, world):
    mod = _load_tool()
    mod._load = lambda name: world.th()
    mod._download = world.download
    mod.subprocess = types.SimpleNamespace(run=world.run, PIPE=None)
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
        rc = mod.main(argv)
    return rc, buf.getvalue()


def test_without_apply_nothing_is_deleted():
    w = _World()
    rc, out = _run(["--logs", "10"], w)
    assert rc == 0
    assert w.deleted == [], "a dry run issued a DELETE"
    assert "run 7" in out and "run 8" not in out
    assert "dry run: nothing deleted" in out


def test_apply_deletes_only_the_runs_with_a_hit_and_verifies_them():
    w = _World()
    rc, out = _run(["--logs", "10", "--apply"], w)
    assert rc == 0
    assert w.deleted == [7], "deleted %r; only run 7 had a hit" % w.deleted
    assert "run 7: logs deleted (verified gone)" in out
    assert w.downloads.count(7) == 2, "the deletion was not re-checked"


def test_the_matched_text_is_never_printed():
    for argv in (["--logs", "10"], ["--logs", "10", "--apply"]):
        _rc, out = _run(argv, _World())
        assert SECRET not in out
        assert "needle=syn" in out


def test_a_failed_sensitivity_control_reads_and_deletes_nothing():
    w = _World(controls_ok=False)
    rc, _out = _run(["--logs", "10", "--apply"], w)
    assert rc == 2
    assert w.downloads == [] and w.deleted == []
