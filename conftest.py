# -*- coding: utf-8 -*-
"""Repo-root conftest: one job only — give every pytest session its own
basetemp, inside the repository (GitHub #257).

WHY THIS FILE EXISTS AT THE ROOT

`pytest.ini` used to pin every session to `--basetemp=tests/_tmp`. pytest
DELETES an explicitly-set basetemp at session start, so a second concurrent
session -- or the nested pytest that
`tools/gate/test_desktop_outage_hook_end_to_end.py` spawns -- removed a running
session's `tmp_path` out from under it. The symptom is fixtures vanishing
mid-run, which reads as flakiness rather than as what it is.

The first attempt at the fix put this hook in `tests/conftest.py`. That covers
`tests/` and nothing else, and the gate suites under `tools/gate/` then fell
back to pytest's default basetemp in `%TEMP%` -- OUTSIDE the repository. Ten
tests in `tools/gate/test_check_crl_chain_fixture.py` errored immediately,
because its `in_repo_copy` fixture asserts its scratch directory is inside the
repo:

    assert base.is_relative_to(_REPO), (
        "self-test scratch is outside the repo; the mutations below cannot run")

That assertion exists precisely for this, and its docstring says so: "Asserted
rather than assumed so a future basetemp move fails here loudly instead of
silently skipping every mutation below." It did its job on the first run. The
hook belongs at the root, where it covers every session.

TWO PROPERTIES, BOTH LOAD-BEARING

  * INSIDE the repo -- `check_crl_chain_fixture` resolves its corpus under
    REPO_ROOT, so an out-of-tree scratch silently skips every mutation.
  * UNDER `tests/_tmp/` -- the janitor sweeps that root, and several guards
    (`check_magnitude_guard`, `check_reachability_skip_shape`,
    `check_packaging_testing_flag`, `check_rtm_live_ui_citations`) exclude that
    prefix when scanning the tree. Moving it elsewhere would silently re-admit
    thousands of copied files into those scans.

An explicit `--basetemp` on the command line still wins; this only fills in the
default.
"""
from __future__ import annotations

import os
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parent
_TESTS_TMP = _REPO_ROOT / "tests" / "_tmp"


def _unique_basetemp() -> Path:
    """`tests/_tmp/s<pid>`, disambiguated if that pid has been reused.

    Windows recycles pids promptly, and a crashed session leaves its directory
    behind, so the bare pid is not by itself unique over time.
    """
    root = _TESTS_TMP / ("s%d" % os.getpid())
    candidate = root
    n = 0
    while candidate.exists():
        n += 1
        candidate = Path(str(root) + "-%d" % n)
    return candidate


def pytest_configure(config):
    if getattr(config.option, "basetemp", None):
        return                      # an explicit --basetemp wins
    candidate = _unique_basetemp()
    try:
        candidate.mkdir(parents=True, exist_ok=True)
    except OSError:
        return                      # fall back to pytest's own default
    config.option.basetemp = str(candidate)
