# -*- coding: utf-8 -*-
"""Pins for migrate_tracker.py's two load-bearing, previously-untested guards.

WHY THIS FILE EXISTS. The p101 claim-check gate flagged it twice: the two
mechanisms below are the ones that decide whether a migration preserves issue
numbering, and neither had a test.

  * The REFUSE-on-commits guard is the exact mechanism that would have
    prevented the burn of the first route-B fresh instance (Dependabot read a
    pushed dependabot.yml and consumed PR numbers 1-5 within 40 minutes of the
    mirror push; numbers are never reused). The first execution ordered the
    phases around the tool's own guard; the guard was right.
  * The got!=n CRITICAL ASSERTION is the only thing standing between a silent
    one-item numbering drift and ~444 files of falsified `GitHub #nn`
    citations.

Everything here is offline: gh_json is monkeypatched; the workdir is a
tmp_path; no network, no real repository.
"""
from __future__ import annotations

import importlib.util
import io
import json
import os
import sys

import pytest

_MIG = os.path.join(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__)))), "scripts", "migration")


def _load(tmp_workdir):
    """Load a fresh module instance with TMP/EXPORT/LABELS re-rooted."""
    if _MIG not in sys.path:
        sys.path.insert(0, _MIG)   # for `import repo_ids` inside the module
    spec = importlib.util.spec_from_file_location(
        "mig_under_test", os.path.join(_MIG, "migrate_tracker.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    mod.TMP = str(tmp_workdir)
    mod.EXPORT = os.path.join(str(tmp_workdir), "tracker-export.json")
    mod.LABELS = os.path.join(str(tmp_workdir), "labels-export.json")
    mod.DEFAULT_DELAY = 0.0   # no sleeping in tests
    return mod


def _write_export(mod, items):
    nums = [i["number"] for i in items]
    doc = {
        "repo": "o/old", "items": items,
        "contiguous": nums == list(range(1, len(nums) + 1)),
        "number_min": min(nums), "number_max": max(nums),
    }
    io.open(mod.EXPORT, "w", encoding="utf-8").write(json.dumps(doc))
    io.open(mod.LABELS, "w", encoding="utf-8").write(json.dumps([]))


def _item(n, kind="issue", state="open"):
    import hashlib
    body = "b%d" % n
    return {"number": n, "kind": kind, "state": state, "title": "t%d" % n,
            "body": body, "labels": [],
            "body_sha256": hashlib.sha256(body.encode("utf-8")).hexdigest()}


def _run(mod, argv):
    saved = sys.argv
    sys.argv = ["migrate_tracker.py"] + argv
    try:
        return mod.main()
    finally:
        sys.argv = saved


class TestRefuseOnCommits:
    """The guard that the burn proved load-bearing."""

    def test_a_target_with_commits_is_refused_before_anything_is_written(
            self, tmp_path):
        mod = _load(tmp_path)
        _write_export(mod, [_item(1)])
        calls = []

        def fake_gh(args, payload=None, allow_fail=False):
            calls.append((tuple(args), payload))
            if "commits" in args[0]:
                return [{"sha": "deadbeef"}]          # the target has history
            raise AssertionError("reached the API past the refusal: %r"
                                 % (args,))

        mod.gh_json = fake_gh
        with pytest.raises(SystemExit) as exc:
            _run(mod, ["--repo", "o/new"])
        msg = str(exc.value)
        assert "REFUSING TO START" in msg
        assert "Dependabot" in msg, \
            "the refusal must say WHY, or the next operator reorders around it"
        # Nothing but the probe ran: no POST ever happened.
        assert all(p is None for _a, p in calls)

    def test_an_empty_target_is_allowed_through_the_probe(self, tmp_path):
        mod = _load(tmp_path)
        _write_export(mod, [_item(1)])

        def fake_gh(args, payload=None, allow_fail=False):
            if "commits" in args[0]:
                return []                               # empty: no commits
            if args[0].startswith("repos/") and "issues?" in args[0]:
                return []
            if payload is not None and args[-1].endswith("/issues"):
                return {"number": 1}
            return {}

        mod.gh_json = fake_gh
        assert _run(mod, ["--repo", "o/new"]) == 0

    def test_a_non_contiguous_export_is_refused(self, tmp_path):
        mod = _load(tmp_path)
        _write_export(mod, [_item(1), _item(3)])        # gap at 2
        mod.gh_json = lambda *a, **k: []
        with pytest.raises(SystemExit) as exc:
            _run(mod, ["--repo", "o/new"])
        assert "not contiguous" in str(exc.value)


class TestCriticalNumberingAssertion:
    """got != n aborts the run instead of silently drifting every later
    citation."""

    def test_a_number_drift_aborts_immediately(self, tmp_path):
        mod = _load(tmp_path)
        _write_export(mod, [_item(1), _item(2)])
        posted = []

        def fake_gh(args, payload=None, allow_fail=False):
            if "commits" in args[0] or "issues?" in args[0]:
                return []
            if payload is not None and args[-1].endswith("/issues"):
                posted.append(payload)
                return {"number": 7}                    # WRONG number
            return {}

        mod.gh_json = fake_gh
        with pytest.raises(SystemExit):
            _run(mod, ["--repo", "o/new"])
        assert len(posted) == 1, \
            "the abort must happen at the FIRST drift, before item 2 is created"

    def test_correct_numbers_complete_and_checkpoint(self, tmp_path):
        mod = _load(tmp_path)
        _write_export(mod, [_item(1), _item(2, state="closed")])
        counter = {"n": 0}

        def fake_gh(args, payload=None, allow_fail=False):
            if "commits" in args[0] or "issues?" in args[0]:
                return []
            if payload is not None and args[-1].endswith("/issues"):
                counter["n"] += 1
                return {"number": counter["n"]}
            return {}

        mod.gh_json = fake_gh
        assert _run(mod, ["--repo", "o/new"]) == 0
        ck = json.load(io.open(os.path.join(
            str(tmp_path), "migrate-checkpoint-o_new.json"), encoding="utf-8"))
        assert ck["created"] == [1, 2]

    def test_a_pr_becomes_a_closed_placeholder_not_a_copy(self, tmp_path):
        mod = _load(tmp_path)
        _write_export(mod, [_item(1, kind="pr", state="open")])
        bodies, patched = [], []

        def fake_gh(args, payload=None, allow_fail=False):
            if "commits" in args[0] or "issues?" in args[0]:
                return []
            if payload is not None and args[-1].endswith("/issues"):
                bodies.append(payload["body"])
                return {"number": 1}
            if payload is not None and "/issues/" in args[-1]:
                patched.append((args[-1], payload))
                return {}
            return {}

        mod.gh_json = fake_gh
        assert _run(mod, ["--repo", "o/new"]) == 0
        assert "Placeholder" in bodies[0]
        assert "b1" not in bodies[0], "the PR body must not be copied"
        assert any(p.get("state") == "closed" for _u, p in patched), \
            "a PR placeholder must end closed"
