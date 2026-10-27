# -*- coding: utf-8 -*-
"""GitHub #275 C6 — the drifted-body duplicate, and both ends of the ratchet.

WHAT C6 ASKS FOR, AND WHY IT IS NOT THE SAME AS C5
==================================================
C5 asks whether the two DETECTORS agree about what a definition is. This asks
what the GUARD does once it has found two of them whose bodies have **drifted** —
different content, both at or over ``MIN_BODY_LINES``.

Drift is the specific hazard #275 measured rather than hypothesised: a poll-race
fix went into one copy of ``DirectoryAttacker``, all 20 TOCTTOU cases passed, and
a stability run under load then failed **9 of 12** in the other copy. The copies
were by then 0.519 similar — which is exactly why this guard thresholds on SIZE
and not on similarity, since any usable similarity cut-off would have excluded
the case the issue is named for. These tests therefore use copies that have
genuinely diverged, and assert that they have.

WHY BOTH DIRECTIONS
===================
``check_duplicate_test_definitions.py`` already implements a two-way ratchet:

  * a duplicate that is not in the baseline is **UNADJUDICATED** -> fail;
  * a baseline entry that is no longer a duplicate is **STALE** -> fail, because
    "a list that only grows reports 'closed' about itself".

Neither direction was tested. ``self_test()`` covers the matcher and the size
threshold and stops before the baseline, so the entire adjudication half of the
guard was unexercised — including the STALE direction, which is the half that
makes the baseline a burn-down rather than an amnesty.

Run:  python -m pytest tools/gate/test_duplicate_definition_ratchet.py -q
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
import sys
from difflib import SequenceMatcher
from pathlib import Path

import pytest

#: parents[2]: this file is <root>/tools/gate/<name>.py. Asserted, because a
#: misresolved root here would silently point the guard at a tree that does not
#: exist and every case would "pass" for the wrong reason.
REPO_ROOT = Path(__file__).resolve().parents[2]
assert (REPO_ROOT / ".github" / "workflows").is_dir(), (
    f"repo root misresolved to {REPO_ROOT!r}")
DETECTOR = REPO_ROOT / "tools" / "gate" / "check_duplicate_test_definitions.py"


def _load():
    spec = importlib.util.spec_from_file_location("dup_defs_ratchet", DETECTOR)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["dup_defs_ratchet"] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture(scope="module")
def mod():
    return _load()


# --------------------------------------------------------------------------
# Two copies of one helper whose bodies have DRIFTED. Same name, same
# column-0 definition, both well over MIN_BODY_LINES, different content --
# the state #275 measured, not identical copy-paste.
# --------------------------------------------------------------------------

_COPY_A = (
    "struct PollingAttacker {\n"
    "    std::atomic<bool> stop{false};\n"
    + "    int slotA;\n" * 28 +
    "    void start() {\n"
    "        while (!stop) { spin(); }\n"
    "    }\n"
    "};\n"
)

#: The SAME helper after a fix landed in only this copy -- a rendezvous added,
#: different members. This is the drift.
_COPY_B = (
    "struct PollingAttacker {\n"
    "    std::atomic<bool> stop{false};\n"
    "    std::binary_semaphore ready{0};\n"
    + "    int slotB;\n" * 27 +
    "    void start() {\n"
    "        ready.acquire();\n"
    "        while (!stop) { spin(); }\n"
    "    }\n"
    "};\n"
)

HELPER = "PollingAttacker"


def _tree(tmp_path: Path, files: dict[str, str]) -> Path:
    tests = tmp_path / "tests"
    tests.mkdir(parents=True, exist_ok=True)
    for name, text in files.items():
        io.open(tests / name, "w", encoding="utf-8", newline="\n").write(text)
    return tests


def _baseline(tmp_path: Path, entries: dict) -> Path:
    p = tmp_path / "baseline.json"
    payload = {"_what": "synthetic baseline for the ratchet test"}
    payload.update(entries)
    io.open(p, "w", encoding="utf-8", newline="\n").write(
        json.dumps(payload, indent=2))
    return p


def _run(mod, tests_dir: Path, baseline: Path) -> tuple[int, str]:
    """Drive the guard's real main() and collect everything it printed.

    stdout/stderr are redirected explicitly rather than via pytest's `capsys`:
    this repo's pytest configuration does not load the capture manager, so the
    fixture raises. Capturing both streams matters -- the UNADJUDICATED lines go
    to stdout and the explanatory failure goes to stderr.
    """
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = mod.main(["--tests-dir", str(tests_dir),
                       "--baseline", str(baseline)])
    return rc, out.getvalue() + err.getvalue()


def test_the_two_copies_really_have_drifted():
    """Guard the fixture itself.

    If the two bodies were identical this file would be testing plain
    duplication, not drift, while its name claimed otherwise. Both must also
    clear MIN_BODY_LINES or the guard is entitled to ignore them and every
    assertion below would pass for the wrong reason.
    """
    mod = _load()
    assert _COPY_A != _COPY_B, "the two copies are identical -- no drift"
    ratio = SequenceMatcher(None, _COPY_A, _COPY_B).ratio()
    assert ratio < 0.95, f"copies are {ratio:.3f} similar -- not meaningful drift"
    for label, body in (("A", _COPY_A), ("B", _COPY_B)):
        n = body.count("\n")
        assert n >= mod.MIN_BODY_LINES, (
            f"copy {label} is {n} lines, under MIN_BODY_LINES="
            f"{mod.MIN_BODY_LINES}; the guard would rightly ignore it")


def test_a_drifted_duplicate_is_reported_as_unadjudicated(mod, tmp_path):
    """Direction 1: the finding appears, by name, and fails the guard."""
    tests = _tree(tmp_path, {"a.cpp": _COPY_A, "b.cpp": _COPY_B})
    base = _baseline(tmp_path, {})
    rc, out = _run(mod, tests, base)
    assert rc == 1, f"a drifted duplicate did not fail the guard:\n{out}"
    assert "UNADJUDICATED" in out, out
    assert HELPER in out, f"the finding does not name {HELPER}:\n{out}"


def test_adjudicating_it_with_a_reason_clears_the_finding(mod, tmp_path):
    """Direction 2a: it clears by being recorded, with a reason."""
    tests = _tree(tmp_path, {"a.cpp": _COPY_A, "b.cpp": _COPY_B})
    base = _baseline(tmp_path, {
        HELPER: {
            "reason": "Synthetic entry for the ratchet test; the two copies "
                      "have drifted and the duplication is recorded rather "
                      "than silently accepted.",
            "issue": 275,
        }
    })
    rc, out = _run(mod, tests, base)
    assert rc == 0, f"an adjudicated duplicate still failed:\n{out}"
    assert "UNADJUDICATED" not in out, out


def test_resolving_it_by_removing_a_copy_clears_the_finding(mod, tmp_path):
    """Direction 2b: it clears by being FIXED, not just recorded.

    This is the direction that matters for a burn-down: if only adjudication
    cleared findings, the baseline would be an amnesty.
    """
    tests = _tree(tmp_path, {"a.cpp": _COPY_A, "b.cpp": "struct Other {\n};\n"})
    base = _baseline(tmp_path, {})
    rc, out = _run(mod, tests, base)
    assert rc == 0, f"a single definition was reported as duplication:\n{out}"
    assert HELPER not in out.replace("scanned", ""), out


def test_an_entry_that_is_no_longer_a_duplicate_goes_STALE(mod, tmp_path):
    """The OTHER end of the ratchet, and the one that keeps it honest.

    Once the duplication is resolved, the baseline entry must be removed. A
    baseline that only ever grows reports "closed" about its own contents
    rather than about the property -- so leaving the entry behind is itself a
    failure.
    """
    tests = _tree(tmp_path, {"a.cpp": _COPY_A, "b.cpp": "struct Other {\n};\n"})
    base = _baseline(tmp_path, {
        HELPER: {"reason": "stale on purpose", "issue": 275}
    })
    rc, out = _run(mod, tests, base)
    assert rc == 1, (
        "a baseline entry for a helper that is no longer duplicated was "
        f"accepted, so the ratchet only tightens in one direction:\n{out}")
    assert "STALE" in out, out


def test_an_empty_tree_is_a_failure_not_a_clean_pass(mod, tmp_path):
    """Vacuity. A scan that enumerated nothing must not report OK.

    Without this the three clearing cases above are satisfiable by a guard
    that silently found no sources at all.
    """
    tests = _tree(tmp_path, {})
    base = _baseline(tmp_path, {})
    rc, out = _run(mod, tests, base)
    assert rc == 1, f"an empty tree reported success:\n{out}"
    assert "0 source files" in out or "vacuous" in out, out
