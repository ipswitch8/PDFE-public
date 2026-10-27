# -*- coding: utf-8 -*-
"""Pins for check_scrub_regressions.py, created p101 phase-6.

WHY THIS FILE EXISTS. The claim-check gate found two load-bearing fixes with
no regression test; this pins the one in this module: the `--history` scan
loads the FULL needle inventory (`load_all_needles`), not the 8-needle scrub
set (`load_needles`). p101 phase-4's claim-check gate got a clean `--history`
result while the pre-rewrite lineage was still reachable through a safety
branch -- only possible because `--history` was host-blind. A silent revert to
`load_needles()` would reopen exactly that hole, and nothing caught it.

Two halves: the loaders genuinely differ (so the choice matters), and the
`--history` code path picks the full one (so a revert reddens).
"""
from __future__ import annotations

import ast
import importlib.util
import os

_HERE = os.path.dirname(os.path.abspath(__file__))


def _load():
    spec = importlib.util.spec_from_file_location(
        "csr_under_test", os.path.join(_HERE, "check_scrub_regressions.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


csr = _load()


def test_load_all_needles_is_a_strict_superset_of_load_needles():
    """The host needle is in the full inventory and NOT in the scrub set --
    which is the whole reason --history must use the former."""
    scrub, err1 = csr.load_needles()
    full, err2 = csr.load_all_needles()
    assert err1 is None and err2 is None
    scrub_hashes = {n["sha256"] for n in scrub}
    full_hashes = {n["sha256"] for n in full}
    assert scrub_hashes < full_hashes, \
        "load_all_needles must be a STRICT superset of load_needles"
    extra = full_hashes - scrub_hashes
    assert extra, "the host needle(s) must be the difference"
    # the extra needle(s) must carry the build-host label
    extra_labels = {n.get("label") for n in full if n["sha256"] in extra}
    assert any("host" in (lab or "") for lab in extra_labels), \
        "the full-inventory extra needle should be the build-host one"


def _history_branch_source():
    """The body of the `if args.history:` branch in main(), as source text."""
    src = open(os.path.join(_HERE, "check_scrub_regressions.py"),
               encoding="utf-8").read()
    tree = ast.parse(src)
    main = next(n for n in ast.walk(tree)
               if isinstance(n, ast.FunctionDef) and n.name == "main")
    for node in ast.walk(main):
        if isinstance(node, ast.If):
            test = node.test
            # match `if args.history:`
            if isinstance(test, ast.Attribute) and test.attr == "history":
                return ast.get_source_segment(src, node)
    raise AssertionError("could not locate the `if args.history:` branch")


def test_history_branch_loads_the_full_inventory_not_the_scrub_set():
    """A revert to `load_needles()` in the --history branch must redden."""
    branch = _history_branch_source()
    assert "load_all_needles(" in branch, \
        "the --history branch must call load_all_needles() -- host-blind " \
        "otherwise (p101 phase-4 proved it)"
    # It must NOT fall back to the 8-needle set in this branch.
    assert "load_needles(" not in branch.replace("load_all_needles(", ""), \
        "the --history branch must not use the 8-needle load_needles()"


def test_scan_text_finds_a_host_needle_only_with_the_full_inventory():
    """End-to-end: a string carrying only the host needle is caught by the
    full inventory and missed by the scrub set -- the exact blindness."""
    full, _ = csr.load_all_needles()
    scrub, _ = csr.load_needles()
    host = (os.environ.get("COMPUTERNAME") or "").strip()
    if len(host) < 4:
        import pytest
        pytest.skip("COMPUTERNAME absent; cannot build the host probe")
    probe = "prefix %s suffix" % host
    assert csr.scan_text(probe, full), "full inventory must catch the host"
    assert not csr.scan_text(probe, scrub), \
        "the scrub set must MISS the host -- that is the blindness --history had"
