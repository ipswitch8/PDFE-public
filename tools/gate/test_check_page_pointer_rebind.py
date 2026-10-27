#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Tests for check_page_pointer_rebind.py, the C-ENUM standing control.

The guard's `--self-test` covers six synthetic shapes. These cover what it
cannot reach from inside itself: the ratchet against the REAL baseline, the
refusal to report a clean result from a measurement that did not happen, and
the one property that makes this guard a C-ENUM remedy rather than another
instance of the class -- that its population is DERIVED from the tree rather
than typed into a list.
"""
from __future__ import annotations

import ast
import importlib.util
import io
import json
import os
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
GUARD = os.path.join(HERE, "check_page_pointer_rebind.py")
BASELINE = os.path.join(HERE, "page_pointer_rebind_baseline.json")


def _load():
    spec = importlib.util.spec_from_file_location("cppr", GUARD)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _run(args):
    return subprocess.run([sys.executable, GUARD] + args, cwd=REPO,
                          capture_output=True, text=True)


def _baseline():
    data = json.load(io.open(BASELINE, encoding="utf-8"))
    return {k: v for k, v in data.items() if not k.startswith("_")}


def test_self_test_passes():
    r = _run(["--self-test"])
    assert r.returncode == 0, r.stdout + r.stderr
    assert "self-test OK" in r.stdout


def test_tree_is_clean_against_the_baseline():
    r = _run(["--repo-root", "."])
    assert r.returncode == 0, (
        "the page-pointer-rebind guard is red against the current tree:\n"
        + r.stdout + r.stderr)


def test_every_baseline_entry_states_a_kind_and_a_reason():
    """'It is not a Command' is a reason; 'it seems fine' is not."""
    entries = _baseline()
    assert entries, "baseline has no entries -- this test would prove nothing"
    allowed = {"different-mechanism", "not-a-command", "lexical-false-positive"}
    for name, e in entries.items():
        assert e.get("kind") in allowed, (
            "%s has kind %r, not one of %s" % (name, e.get("kind"), allowed))
        assert len(e.get("reason", "")) > 60, (
            "%s's reason is too short to be one: %r" % (name, e.get("reason")))


def test_the_population_is_derived_from_the_tree_not_typed_into_the_guard():
    """The whole point of this control, pinned.

    C-ENUM is "completeness asserted from a hand-made enumeration". If this
    guard's own subject list were typed, it would be another instance of the
    class it was written to close. So: the set of classes it examines must come
    from walking the tree, and must be substantially larger than the baseline.
    """
    mod = _load()
    missing = mod.find_missing(REPO)
    assert missing is not None, "could not measure"

    # Re-derive the full population the guard considers, not just the failures.
    strip = mod._strip_fn()
    files = subprocess.run(["git", "ls-files", "src"], cwd=REPO,
                           capture_output=True, text=True).stdout.split()
    headers = [p for p in files if p.endswith((".h", ".hpp"))]
    cachers = set()
    for p in headers:
        raw = io.open(os.path.join(REPO, p), encoding="utf-8",
                      errors="replace").read()
        try:
            t = strip(raw)
        except ValueError:
            continue
        for m in mod.CLASS_START.finditer(t):
            if mod.PAGE_MEMBER.search(mod._class_body(t, m.start())):
                cachers.add(m.group(1))

    assert len(cachers) >= 15, (
        "only %d classes caching a Page* were found; the population looks "
        "truncated, and a guard measuring a truncated population is the "
        "C-ENUM defect itself" % len(cachers))
    assert len(cachers) > len(_baseline()) * 3, (
        "the derived population (%d) is barely larger than the baseline (%d) "
        "-- that would mean the guard is effectively checking its own list"
        % (len(cachers), len(_baseline())))

    # And the guard must have no hard-coded roster. Checked against EVERY class
    # in the derived population, not a sample: a spot check of four names would
    # pass a guard that hard-coded the other seventeen, which is precisely the
    # C-ENUM shape this control exists to close. (The phase-4 validator gate
    # caught the earlier version asserting only four.)
    #
    # CODE only, not prose. The guard's docstring legitimately names
    # AnnotationTool while explaining why check_tool_cached_page.py's
    # population cannot see this defect. A class named in a comment is not a
    # roster entry -- the same distinction check_engine_wiring.py makes about
    # call sites, and getting it wrong here would make this test un-satisfiable
    # by any guard that documents itself.
    tree = ast.parse(io.open(GUARD, encoding="utf-8").read())
    docstrings = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef,
                             ast.AsyncFunctionDef)):
            d = ast.get_docstring(node, clean=False)
            if d:
                docstrings.add(d)
    code_strings, identifiers = [], set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            if node.value not in docstrings:
                code_strings.append(node.value)
        elif isinstance(node, ast.Name):
            identifiers.add(node.id)
        elif isinstance(node, ast.Attribute):
            identifiers.add(node.attr)
    blob = "\n".join(code_strings)
    named = sorted(n for n in cachers
                   if n in blob or n in identifiers)
    assert not named, (
        "the guard's CODE names %d class(es) from its own derived population "
        "(%s). The population must come from walking the tree; special-casing "
        "a name would turn this control into another instance of the class it "
        "closes." % (len(named), ", ".join(named)))


def test_an_empty_override_is_not_accepted_as_compliance():
    """Regression for the defect this guard's own mutation run exposed.

    Command::rebindPage's default body is empty. An override that is ALSO
    empty reproduces the defect exactly while satisfying a presence check --
    the vacuous-oracle shape. Pinned here as well as in the self-test because
    it is the single easiest way for this guard to rot into decoration.
    """
    mod = _load()
    strip = mod._strip_fn()
    corpus = {
        "src/a/Cmd.h": "class Cmd : public Command {\n"
                       "  void rebindPage(Page* p) override;\n"
                       "  pdfe::core::Page* page_ = nullptr;\n};",
        "src/a/Cmd.cpp": "void Cmd::rebindPage(Page* p) { }",
    }
    got = mod.scan({p: strip(t) for p, t in corpus.items()})
    assert "Cmd" in got, (
        "an override with an empty body was accepted as compliance")


def test_cannot_measure_is_an_error_not_a_pass():
    """C-FAILOPEN: pointed at a tree with no Page*-caching class, the guard
    must refuse rather than report a clean sweep."""
    mod = _load()
    assert mod.find_missing(os.path.join(REPO, "tools", "gate")) is None, (
        "the guard returned a result for a tree it could not enumerate; "
        "'no findings' and 'no measurement' must not be the same answer")
