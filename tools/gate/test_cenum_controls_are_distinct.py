# -*- coding: utf-8 -*-
"""A credited C-ENUM control must be its own population, not a re-run of another.

GitHub #254. `docs/audit/test-gap-classes.md` credits C-ENUM -- the corpus's
highest-weight test-gap class -- with a hand-written list of standing controls.
That list said five. One of them, `test_win87_src_is_clean.py`, invokes
`check_win87_publish_shapes.py` as a subprocess over the same `SHAPES` dict: a
second TRIGGER for a population already counted, not a second population. The
class's defences were overstated by 25%.

The overcount is the class's own failure mode applied to its own paperwork. A
hand-made list cannot tell a duplicate member from an independent one, so
"five controls" read as five populations for as long as nobody opened them.

Prose saying "there are four" would rot the same way the "five" did, so this is
a mechanism instead: it re-derives the credited list from the document and
fails if any credited control names another credited control. That is the
signature of a re-run, and it is checkable without a model of what each control
means.

WHAT THIS DOES NOT CLAIM. It does not prove the remaining controls are
independent in any deeper sense -- two could still enumerate overlapping shapes
without mentioning each other. It closes the one failure that actually happened
and is mechanically detectable. #254 records the honest position on the rest:
"every API that publishes a file" is not lexically enumerable, so a single
derived source is not available for the asking.
"""
from __future__ import annotations

import io
import os
import re

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))          # tools/gate
REPO = os.path.dirname(os.path.dirname(HERE))              # repo root
DOC = os.path.join(REPO, "docs", "audit", "test-gap-classes.md")

#: The heading that opens the C-ENUM section, and the bold run that opens its
#: standing-controls paragraph. Both are matched loosely enough to survive
#: ordinary editing and strictly enough to fail rather than silently match
#: nothing -- see test_the_credited_list_is_actually_found.
_SECTION_RE = re.compile(r"^##\s*\d+\.\s*C-ENUM\b", re.M)
_CONTROLS_RE = re.compile(r"\*\*Standing controls[^*]*\*\*(.+?)(?:\n\n|\Z)", re.S)
_SCRIPT_RE = re.compile(r"`([A-Za-z0-9_]+\.py)`")


def _section_text() -> str:
    text = io.open(DOC, encoding="utf-8").read()
    m = _SECTION_RE.search(text)
    assert m, (
        "the C-ENUM section heading was not found in %s. This test derives the "
        "credited control list from that section; if the heading moved, it "
        "would otherwise check an empty list and pass vacuously." % DOC)
    rest = text[m.end():]
    nxt = re.search(r"^##\s", rest, re.M)
    return rest[: nxt.start()] if nxt else rest


def credited_controls() -> list[str]:
    body = _section_text()
    m = _CONTROLS_RE.search(body)
    assert m, (
        "no '**Standing controls...**' paragraph found in the C-ENUM section. "
        "Without it this test has no population and would pass having checked "
        "nothing.")
    return sorted(set(_SCRIPT_RE.findall(m.group(1))))


def _locate(name: str) -> str | None:
    for sub in ("tools/gate", "tests/scripts", "tests"):
        p = os.path.join(REPO, sub, name)
        if os.path.exists(p):
            return p
    return None


def test_the_credited_list_is_actually_found():
    """The population must be non-empty, or every assertion below is vacuous."""
    controls = credited_controls()
    assert len(controls) >= 2, (
        "found %d credited C-ENUM control(s): %s. A class credited with fewer "
        "than two controls cannot have a duplicate, so this file would be "
        "checking nothing -- which is far more likely to mean the document "
        "shape changed than that the controls were deleted."
        % (len(controls), controls))


def test_every_credited_control_exists_on_disk():
    missing = [c for c in credited_controls() if _locate(c) is None]
    assert not missing, (
        "%d credited C-ENUM control(s) do not exist in the tree: %s. A class "
        "credited with a control that is not there is defended by a name."
        % (len(missing), missing))


def test_no_credited_control_is_a_rerun_of_another():
    """The GitHub #254 defect, stated as a mechanism.

    A control that names another credited control is invoking it -- by
    subprocess or import -- and is therefore a second trigger for a population
    already counted, not a population of its own.
    """
    controls = credited_controls()
    offenders = []
    for name in controls:
        path = _locate(name)
        if path is None:
            continue  # covered by the test above
        body = io.open(path, encoding="utf-8", errors="replace").read()
        for other in controls:
            if other == name:
                continue
            # The bare filename is enough: a Python file mentioning another
            # script by name is either running it or documenting that it runs
            # it, and both mean it is not an independent population.
            if other in body:
                offenders.append((name, other))
    assert not offenders, (
        "%d credited C-ENUM control(s) name another credited control, so they "
        "are re-runs of a population already counted rather than populations "
        "of their own: %s.\n"
        "This is GitHub #254: the class's defences were once credited as five "
        "when they were four, because a hand-made list cannot tell a duplicate "
        "member from an independent one -- which is C-ENUM applied to C-ENUM's "
        "own paperwork. Either drop the duplicate from the credited list in "
        "docs/audit/test-gap-classes.md, or give it a population of its own."
        % (len(offenders), offenders))


# ---------------------------------------------------------------------------
# And the typed populations fail LOUDLY when they diverge from their anchor.
#
# #254's first section lists the shape-lists that remain hand-typed. Two of them
# are anchored to something they can be checked against, and those anchors are
# what this half tests -- end to end through each guard's main(), because the
# anchor checks live in main() and asserting on the helper alone would be the
# "verified at the wrong level" mistake: it proves the predicate computes, not
# that the guard acts on it.
#
# What is NOT claimed: this does not make the lists derived. #254 is explicit
# that "every API that publishes a file" is not lexically enumerable, and the
# residue is recorded in each guard's own header with the measurement behind it.
# ---------------------------------------------------------------------------

import subprocess                                             # noqa: E402
import sys                                                    # noqa: E402
import tempfile                                               # noqa: E402
import pathlib                                                # noqa: E402

_BASE_HEADER = "src/annotations/AnnotationTool.h"


def _run_cached_page_guard(repo_root: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, "-u",
         os.path.join(REPO, "tools", "gate", "check_tool_cached_page.py"),
         "--repo-root", repo_root],
        capture_output=True, text=True, encoding="utf-8", errors="replace")


def _base_header_text() -> str:
    return io.open(os.path.join(REPO, _BASE_HEADER),
                   encoding="utf-8", errors="replace").read()


@pytest.mark.parametrize("label,mutate,expect", [
    ("renamed",
     lambda s: s.replace("pdfe::core::Page* page_ = nullptr;",
                         "pdfe::core::Page* cachedPage_ = nullptr;"),
     "declares the cached page as `cachedPage_`"),
    ("removed",
     lambda s: s.replace("pdfe::core::Page* page_ = nullptr;",
                         "int unrelated_ = 0;"),
     "declares no raw pdfe::core::Page* member at all"),
    # This one is the genuinely NEW detection. The guard's original anchor was
    # `PAGE_MEMBER.search(base_text)`, which a second pointer leaves satisfied --
    # so a shadowable member the subclass scan never looks for could be added to
    # the base with no complaint.
    ("second pointer",
     lambda s: s.replace(
         "pdfe::core::Page* page_ = nullptr;",
         "pdfe::core::Page* page_ = nullptr;\n"
         "    pdfe::core::Page* hoverPage_ = nullptr;"),
     "declares more than one raw page pointer"),
])
def test_cached_page_guard_reddens_when_its_anchor_moves(label, mutate, expect):
    original = _base_header_text()
    mutated = mutate(original)
    assert mutated != original, (
        "the %r mutation did not change AnnotationTool.h, so this case proves "
        "nothing -- the anchor text it edits has moved. Fix the mutation, do "
        "not delete the case." % label)

    with tempfile.TemporaryDirectory() as td:
        root = pathlib.Path(td)
        (root / "src" / "annotations").mkdir(parents=True)
        io.open(root / _BASE_HEADER, "w", encoding="utf-8",
                newline="\n").write(mutated)
        r = _run_cached_page_guard(str(root))

    combined = r.stdout + r.stderr
    assert r.returncode == 1, (
        "check_tool_cached_page.py accepted a base header whose cached-page "
        "member had diverged (%s), exit=%d. Shadowing requires the SAME member "
        "name, so a guard scanning for the old name checks for something that "
        "cannot shadow anything and reports OK over an open hole.\n%s"
        % (label, r.returncode, combined))
    assert expect in combined, (
        "the guard failed, but not for the %r reason -- so this case is not "
        "testing what it says. Wanted %r in:\n%s" % (label, expect, combined))


def test_cached_page_guard_accepts_the_real_header():
    """The control. Without it, every case above is satisfied by a guard that
    rejects every input, which would prove nothing about divergence."""
    r = _run_cached_page_guard(REPO)
    assert r.returncode == 0, (
        "the committed tree does not satisfy check_tool_cached_page.py:\n%s"
        % (r.stdout + r.stderr))


def test_publish_shape_completeness_guard_is_wired_and_green():
    """#254's other typed population. Its own mutation coverage lives in
    tools/gate/test_publish_shape_completeness.py; this asserts only that the
    guard the issue asked for EXISTS and is reachable from here, so that
    deleting it fails a test filed against #254 rather than passing silently."""
    guard = os.path.join(REPO, "tools", "gate",
                         "check_publish_shape_completeness.py")
    assert os.path.isfile(guard), (
        "the completeness guard #254 asked for is gone. The issue names it as "
        "the one partial improvement available for the publish-shape list: "
        "'a check that the shape list is complete with respect to some external "
        "reference, so that adding a new publish call to the tree without "
        "adding its shape is detectable'.")
    r = subprocess.run([sys.executable, "-u", guard, "--root", "src"],
                       cwd=REPO, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    assert r.returncode == 0, (r.stdout + r.stderr)
