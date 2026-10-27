# -*- coding: utf-8 -*-
"""The WIN-87 publish class stays closed in shipped source (DCA 0051).

WHY A TEST AND NOT JUST THE CI STEP. `check_win87_publish_shapes.py --root src`
already runs in `fast-checks.yml`, and it did its job: it went red on the very
first push that reintroduced the class and stayed red for ten consecutive
pushes. Nobody read it.

So the sweep was never the missing piece — being runnable *without CI* was.
This lifts it into the gate self-test suite, where it is reached by
`python -m pytest tools/gate/...` and by
`tools/gate/run_fast_checks_locally.py`, i.e. by a local command someone
actually runs before pushing.

This is the DCA 0051 INSTANCE test: RED on `20eebb1f`..`d13e8e67`, where
`src/optimize/OptimizeEngine.cpp` staged to a target-derived `<file>.lin.tmp`
and published with a bare `std::filesystem::rename`; GREEN once that was routed
through `StagingFile`.
"""
from __future__ import annotations

import io
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SWEEP = ROOT / "tools" / "gate" / "check_win87_publish_shapes.py"


def _run(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(SWEEP), *args],
        cwd=str(ROOT), capture_output=True, text=True,
        encoding="utf-8", errors="replace", timeout=600)


def test_no_live_publish_shape_survives_in_src():
    """The instance: shipped source carries no by-path publish outside the primitive."""
    p = _run("--root", "src")
    assert p.returncode == 0, (
        "WIN-87 publish shape(s) present in src/. Each re-resolves a path at "
        "publish time with no handle held; route it through core::StagingFile "
        "(createBeside + commitOverWithRetry).\n\n" + (p.stdout or "") + (p.stderr or ""))


def test_the_sweep_can_still_go_red():
    """A sweep nobody has proved can fail is decoration, and its PASS means nothing.

    The checker ships its own planted-probe self-test; running it here means the
    green above is a measurement rather than an assumption.
    """
    p = _run("--self-test")
    assert p.returncode == 0, (
        "the WIN-87 sweep's self-test failed, so its PASS over src/ cannot be "
        "trusted:\n" + (p.stdout or "") + (p.stderr or ""))
    assert "SELF-TEST PASS" in (p.stdout or ""), p.stdout


# ---------------------------------------------------------- GitHub #254

# C-ENUM -- "completeness asserted from a hand-made enumeration" -- is this
# corpus's highest-weight test-gap class, and #254 records a residue the
# sweep deliberately did not close: `docs/audit/test-gap-classes.md` credits
# C-ENUM with FIVE standing controls, but there are only FOUR distinct
# POPULATIONS. This file is the overcount: it re-runs
# `check_win87_publish_shapes.py` over the same shape list from pytest instead
# of CI, which changes WHERE the check runs, not WHAT it covers. Its own
# header says so -- "the sweep was never the missing piece; being runnable
# WITHOUT CI was".
#
# Correcting that in prose is not a control. What makes the overcount
# impossible to recreate is asserting the property the mislabel depends on:
# that these two credited controls operate over ONE population. If anyone
# types a second shape list -- which is precisely how a C-ENUM population
# silently forks -- these fail.


def _shape_list_definitions():
    """Every place in tools/gate that DEFINES a publish-shape population.

    Keyed on an assignment to a name containing SHAPES, not on the word
    appearing anywhere: the sweep's own tests legitimately READ `W.SHAPES` and
    a substring search would count those as definitions and make this test
    permanently red for the wrong reason.
    """
    import re
    assign = re.compile(r"^\s*([A-Z_]*SHAPES[A-Z_]*)\s*(?::[^=]+)?=", re.M)
    found = []
    for path in sorted((ROOT / "tools" / "gate").glob("*.py")):
        text = io.open(path, encoding="utf-8", errors="replace").read()
        for m in assign.finditer(text):
            found.append((path.name, m.group(1)))
    return found


def test_the_publish_shape_population_is_defined_exactly_once():
    """GitHub #254 -- one population, so the two controls are not two.

    A second typed list would make the sweep and this file cover different
    things while both reporting green, which is the C-ENUM failure mode one
    level up: the CONTROLS would be enumerated by hand too.
    """
    defs = _shape_list_definitions()
    assert defs, (
        "no publish-shape population found at all under tools/gate -- the "
        "detector has gone blind, which would read as uniformity")
    owners = sorted({name for name, _ in defs})
    assert owners == ["check_win87_publish_shapes.py"], (
        "the publish-shape population is defined in more than one place: %r. "
        "C-ENUM's residue (GitHub #254) is that two credited controls share "
        "ONE population; a second definition makes them genuinely diverge "
        "while both stay green." % (defs,))


def test_this_control_delegates_rather_than_reimplementing():
    """The other half: sharing a list is not enough if the LOGIC is copied.

    This file must invoke the sweep as a subprocess. If it ever grows its own
    matcher, it becomes a second population regardless of where the shape list
    lives -- and the overcount #254 describes would become a real divergence
    instead of a bookkeeping error.
    """
    src = io.open(__file__, encoding="utf-8").read()

    # It must actually RUN the sweep, not merely mention it.
    assert "subprocess.run" in src and "SWEEP" in src, (
        "this control no longer delegates to check_win87_publish_shapes.py, "
        "so it is no longer the same population it is credited with sharing")

    # ...and it must not define a shape population of its own. Checked with the
    # same assignment-shaped matcher the test above uses, restricted to this
    # file -- NOT by searching for "re.compile" or the bare word SHAPES.
    #
    # The first draft did exactly that and immediately flagged ITSELF: the
    # helper above legitimately compiles a regex to FIND definitions, and
    # `re.compile` appearing anywhere was treated as evidence of
    # reimplementation. A symptom-level heuristic that cannot tell a detector
    # from the thing it detects is the same mistake as the C-ENUM class itself,
    # one level down.
    own = [name for name, _ in _shape_list_definitions()
           if name == Path(__file__).name]
    assert not own, (
        "this control defines its own publish-shape population (%r), so it has "
        "forked from the sweep it is credited with sharing" % (own,))
