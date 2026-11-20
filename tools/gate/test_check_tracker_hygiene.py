# -*- coding: utf-8 -*-
"""Pins for check_tracker_hygiene.py's decision logic.

Everything here is offline: the needle is synthetic (the matcher cannot tell a
real needle from a synthetic one, so a synthetic one proves firing just as
well), and the network layer is exercised only through its failure shapes.
The thing these tests must keep true is the guard's one promise: it NEVER
reports clean on a surface it did not actually read.
"""
from __future__ import annotations

import contextlib
import hashlib
import importlib.util
import io
import os
import sys

import pytest

_HERE = os.path.dirname(os.path.abspath(__file__))


def _load(name):
    spec = importlib.util.spec_from_file_location(
        name, os.path.join(_HERE, name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


th = _load("check_tracker_hygiene")
csr = _load("check_scrub_regressions")

SYN = {"label": "syn", "sha256": hashlib.sha256(b"zzsynth host").hexdigest(),
       "n": len("zzsynth host")}
NEEDLES = [SYN]


# --- scan_payload -----------------------------------------------------------

def test_a_planted_pair_needle_is_found_across_every_separator():
    for sep in ("-", "_", ".", " "):
        text = "prefix zzsynth%shost suffix" % sep
        assert th.scan_payload([("w", text)], NEEDLES, csr.scan_text) == \
            [("w", "syn")], "separator %r must normalise to the pair" % sep


def test_unrelated_text_and_none_bodies_do_not_hit():
    assert not th.scan_payload([("a", "nothing"), ("b", None), ("c", "")],
                               NEEDLES, csr.scan_text)


# --- controls ---------------------------------------------------------------

def test_controls_pass_only_when_sensitivity_fires_and_specificity_holds():
    sens, spec = th.controls_ok(csr.scan_text, NEEDLES, "zzsynth host")
    assert sens and spec


def test_an_empty_live_value_cannot_pass_sensitivity():
    # An empty COMPUTERNAME would make `scan_text("x  y")` trivially miss and
    # the old shape would have read that as "control passed vacuously".
    sens, _ = th.controls_ok(csr.scan_text, NEEDLES, "")
    assert not sens


# --- verdict: the never-vacuous promise --------------------------------------

def test_verdict_refuses_a_zero_item_population():
    v, _why = th.verdict(0, [])
    assert v == "refuse"


def test_verdict_refuses_a_window_where_nothing_was_readable():
    v, why = th.verdict(25, [], readable=0)
    assert v == "refuse"
    assert "unreadable" in why


def test_verdict_leak_beats_clean():
    assert th.verdict(10, [("x", "syn")])[0] == "leak"
    assert th.verdict(10, [])[0] == "clean"


# --- the network layer fails CLOSED ------------------------------------------

def test_scan_tracker_refuses_when_any_endpoint_is_unreadable(monkeypatch):
    """A 403 on comments must NOT scan issues+releases and report clean —
    partial coverage reported as full coverage is how #146/#277 stayed
    invisible."""
    calls = {}

    def fake_api(gh, path):
        calls[path] = True
        return None if "comments" in path else []

    monkeypatch.setattr(th, "api_json", fake_api)
    rc = th.scan_tracker(csr, NEEDLES, "gh", "o/r")
    assert rc == 2


def test_scan_tracker_refuses_an_empty_tracker(monkeypatch):
    # 0 issues + 0 comments + 0 releases on THIS repository is a read failure
    # wearing a success costume (the tracker has 324 items), not a clean pass.
    monkeypatch.setattr(th, "api_json", lambda gh, path: [])
    assert th.scan_tracker(csr, NEEDLES, "gh", "o/r") == 2


def test_scan_tracker_reports_a_leak(monkeypatch):
    def fake_api(gh, path):
        if "issues?" in path:
            return [{"number": 7, "title": "t", "body": "has zzsynth-host"}]
        return []

    monkeypatch.setattr(th, "api_json", fake_api)
    assert th.scan_tracker(csr, NEEDLES, "gh", "o/r") == 1


def test_scan_logs_refuses_when_the_run_list_is_unreadable(monkeypatch):
    monkeypatch.setattr(th, "api_json", lambda gh, path: None)
    assert th.scan_logs(csr, NEEDLES, "gh", "o/r", 25) == 2


# --- the live probe: Windows unchanged, Linux CI via PDFE_HYGIENE_PROBE (#407)

class _FakeGuard:
    """scan_text over the synthetic needle, recording every probe it sees."""
    def __init__(self):
        self.seen = []

    def scan_text(self, text, needles):
        self.seen.append(text)
        return csr.scan_text(text, needles)


def test_windows_keeps_the_computername_probe():
    probe, src = th.live_probe({"COMPUTERNAME": "ZZSYNTH-HOST",
                                th.PROBE_ENV: "zzsynth host"}, "u", "win32")
    assert src == "COMPUTERNAME"
    assert "ZZSYNTH-HOST" in probe


def test_windows_with_no_computername_still_fails_closed():
    # The env var must not rescue a Windows host: its behaviour is unchanged.
    assert th.live_probe({th.PROBE_ENV: "zzsynth host"}, "u", "win32") == \
        (None, "COMPUTERNAME absent/short")


def test_linux_uses_the_probe_env_var_when_set():
    probe, src = th.live_probe({th.PROBE_ENV: "zzsynth host"}, "runner",
                               "linux")
    assert src == th.PROBE_ENV
    assert csr.scan_text(probe, NEEDLES), "the supplied probe must hit"


@pytest.mark.parametrize("value", ["", "  ", "abc"])
def test_linux_a_set_but_short_probe_fails_closed(value):
    # Set-but-short must NOT fall through to the login name: a misconfigured
    # secret is reported, not papered over.
    probe, why = th.live_probe({th.PROBE_ENV: value}, "someone", "linux")
    assert probe is None and th.PROBE_ENV in why


def test_linux_without_the_env_var_falls_back_to_the_login_name():
    probe, src = th.live_probe({}, "someone", "linux")
    assert src == "login name" and "someone" in probe


def test_linux_with_no_env_var_and_no_login_fails_closed():
    assert th.live_probe({}, "", "linux")[0] is None


def _captured(fn, *args):
    """(return value, everything printed). capsys is unavailable under this
    repo's pytest configuration, so capture directly."""
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
        rv = fn(*args)
    return rv, buf.getvalue()


def test_run_controls_passes_with_a_hitting_probe():
    ok, out = _captured(th.run_controls, _FakeGuard(), NEEDLES,
                        {th.PROBE_ENV: "zzsynth host"}, "runner", "linux")
    assert ok is True and "sensitivity PASS" in out


def test_run_controls_fails_closed_when_the_probe_hits_nothing():
    # The CI container's login is `runner`, which is not a needle: without the
    # secret the control must FAIL, never pass vacuously.
    ok, out = _captured(th.run_controls, _FakeGuard(), NEEDLES, {}, "runner",
                        "linux")
    assert ok is False
    assert "sensitivity FAIL" in out and th.PROBE_ENV in out


def test_run_controls_fails_closed_when_the_probe_is_short():
    ok, out = _captured(th.run_controls, _FakeGuard(), NEEDLES,
                        {th.PROBE_ENV: "ab"}, "runner", "linux")
    assert ok is False and "cannot run" in out


def test_the_probe_value_is_never_printed():
    secret = "zzsynth host"
    g = _FakeGuard()
    _, out1 = _captured(th.run_controls, g, NEEDLES, {th.PROBE_ENV: secret},
                        "runner", "linux")
    _, out2 = _captured(th.run_controls, g, NEEDLES,
                        {"COMPUTERNAME": "ZZSYNTHPC"}, "u", "win32")
    for v in (secret, "ZZSYNTHPC"):
        assert v not in out1 and v not in out2
    assert any(secret in s for s in g.seen), "the probe was actually scanned"


def test_a_planted_needle_in_log_text_is_still_found():
    log = "2026-11-16T00:00:00Z ##[group]Run cd zzsynth-host/src\n"
    assert th.scan_payload([("run 1 job.txt", log)], NEEDLES,
                           csr.scan_text) == [("run 1 job.txt", "syn")]
