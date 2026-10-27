# -*- coding: utf-8 -*-
"""Pins for check_tracker_hygiene.py's decision logic.

Everything here is offline: the needle is synthetic (the matcher cannot tell a
real needle from a synthetic one, so a synthetic one proves firing just as
well), and the network layer is exercised only through its failure shapes.
The thing these tests must keep true is the guard's one promise: it NEVER
reports clean on a surface it did not actually read.
"""
from __future__ import annotations

import hashlib
import importlib.util
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
