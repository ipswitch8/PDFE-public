#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Class-level test for DCA 0081 -- the caller SET a gated-by-caller directive
claims about, over every placement a caller can occupy.

THE CLASS, not the instance
---------------------------
The instance (GitHub #184, p87-open-issues phase-6) was that
``verify_directive()`` dropped the DEFINING FILE wholesale::

    external = {rel: v for rel, v in hits.items() if rel != site_rel}

so an ungated caller added next to the directive was invisible and the lint
reported PASS. The instance fix narrows the exemption to the symbol's
DEFINITION lines.

The class is broader and is what this file pins: **a `gated-by-caller`
directive makes a universally-quantified claim -- "every caller gates first" --
and a checker that enumerates callers can be blind on any AXIS of that
enumeration.** File placement is one axis; it is the one that was blind. The
others are enumerated here as a matrix so the next blind axis has to survive a
test rather than a reviewer's attention:

  * WHERE the caller lives      -- external file / the defining file itself
  * WHETHER it gates            -- real guard / guard in a different function /
                                   a comment that merely describes a guard
  * HOW MANY callers exist      -- one / several / none at all
  * WHAT the occurrence IS      -- a call / the definition / a second overload's
                                   definition

An instance test alone ("the defining file case now fails") would let the next
sibling escape the same way -- e.g. a caller in a header, a caller reached only
through an overload, or a second definition mistaken for a call. Each row below
is one of those siblings.

BOTH DIRECTIONS, EVERY ROW. A row that can only ever redden is as useless as
one that can only ever pass: counting the definition as an unguarded call would
make the lint permanently red at every directive site, which is the other way
this check stops being read (DCA 0047).
"""
from __future__ import annotations

import io
import os
import sys
import tempfile

import pytest

try:  # pragma: no cover
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:  # pragma: no cover
    pass

_HERE = os.path.dirname(os.path.abspath(__file__))          # <repo>/tools/gate
_REPO = os.path.dirname(os.path.dirname(_HERE))             # <repo>
_SCRIPTS = os.path.join(_REPO, "tests", "scripts")
if _SCRIPTS not in sys.path:
    sys.path.insert(0, _SCRIPTS)

from lint_unc_gate import verify_directive  # noqa: E402

SITE_REL = "src/core/Probe.cpp"

#: The directive site: a definition carrying the directive and the open.
DEFINING_FILE = """\
UnencryptedWrapperInfo probeThing(Ctx* ctx, const std::string& path) {
    // lint-unc-gate: gated-by-caller probeThing
    // Gated by MainWindow::openFile, which refuses UNC first.
    pdf_document* doc = pdf_open_document(ctx, path.c_str());
}
"""

#: A SECOND overload's definition. Not a call -- must never be reported.
SECOND_OVERLOAD = """\

UnencryptedWrapperInfo probeThing(Ctx* ctx, const std::filesystem::path& p) {
    return probeThing(ctx, p.string());
}
"""

GATING_CALLER = """\
void open(const std::string& p) {
    if (pdfe::core::path_security::is_unc_or_remote(p)) return;
    probeThing(ctx, p);
}
"""

UNGATED_CALLER = """\
void open2(const std::string& p) {
    probeThing(ctx, p);
}
"""

PROSE_ONLY_CALLER = """\
// is_unc_or_remote(p) is applied by our caller, honest
void open3(const std::string& p) {
    probeThing(ctx, p);
}
"""

GUARD_IN_ANOTHER_FUNCTION = """\
void somethingElse(const std::string& q) {
    if (path_security::is_unc_or_remote(q)) return;
}
void open4(const std::string& p) {
    probeThing(ctx, p);
}
"""

SAME_FILE_UNGATED = """\

void alsoHere(Ctx* ctx, const std::string& p) {
    probeThing(ctx, p);
}
"""

SAME_FILE_GATED = """\

void alsoHere(Ctx* ctx, const std::string& p) {
    if (path_security::is_unc_or_remote(p)) return;
    probeThing(ctx, p);
}
"""


def _tree(tmp_path, probe_src, callers):
    """Materialise a src/ tree and return its root."""
    srcdir = tmp_path / "src" / "core"
    srcdir.mkdir(parents=True, exist_ok=True)
    with io.open(str(srcdir / "Probe.cpp"), "w", encoding="utf-8",
                 newline="\n") as fh:
        fh.write(probe_src)
    for name, body in callers.items():
        with io.open(str(srcdir / name), "w", encoding="utf-8",
                     newline="\n") as fh:
            fh.write(body)
    return str(tmp_path)


# --------------------------------------------------------------------------
# Axis 1 -- WHERE the caller lives. This is the axis that was blind.
# --------------------------------------------------------------------------

@pytest.mark.parametrize(
    "probe_src, callers, expect_problem, why",
    [
        # (a) external, gating -> clean. The baseline: without this row the
        #     whole file could be satisfied by a check that always reddens.
        (DEFINING_FILE, {"Caller.cpp": GATING_CALLER}, False,
         "an external caller that gates satisfies the directive"),
        # (b) external, ungated -> reported. Pre-existing behaviour, pinned
        #     here so the narrowing cannot regress it.
        (DEFINING_FILE,
         {"Caller.cpp": GATING_CALLER, "Other.cpp": UNGATED_CALLER}, True,
         "an external ungated caller is reported"),
        # (c) SAME FILE, ungated -> reported. THE INSTANCE. Before the fix
        #     this returned [] and the lint exited 0.
        (DEFINING_FILE + SAME_FILE_UNGATED, {"Caller.cpp": GATING_CALLER}, True,
         "an ungated caller in the DEFINING FILE is reported"),
        # (d) SAME FILE, gating -> clean. The other direction of (c): the
        #     narrowing must not report a caller that does gate.
        (DEFINING_FILE + SAME_FILE_GATED, {"Caller.cpp": GATING_CALLER}, False,
         "a gating caller in the defining file is accepted"),
    ],
)
def test_caller_placement_axis(tmp_path, probe_src, callers, expect_problem, why):
    root = _tree(tmp_path, probe_src, callers)
    problems = verify_directive(root, "probeThing", SITE_REL)
    assert bool(problems) is expect_problem, f"{why}: got {problems}"


# --------------------------------------------------------------------------
# Axis 2 -- WHAT the occurrence is. A definition is not a call.
# --------------------------------------------------------------------------

def test_the_definition_is_not_counted_as_an_ungated_call(tmp_path):
    """The exemption must survive the narrowing.

    If a definition line were treated as a call, EVERY directive site would be
    permanently red -- a guard that can only redden is as dead as one that can
    only pass.
    """
    root = _tree(tmp_path, DEFINING_FILE, {"Caller.cpp": GATING_CALLER})
    assert verify_directive(root, "probeThing", SITE_REL) == []


def test_a_second_overloads_definition_is_not_counted_as_an_ungated_call(tmp_path):
    """Two overloads is the real shape: probeUnencryptedWrapper has exactly two.

    The second overload's body legitimately calls the first, and that call IS
    subject to the guard rule -- so the row below asserts the DEFINITION is
    exempt while its delegating call is not silently exempt with it.
    """
    root = _tree(tmp_path, DEFINING_FILE + SECOND_OVERLOAD,
                 {"Caller.cpp": GATING_CALLER})
    problems = verify_directive(root, "probeThing", SITE_REL)
    # The delegating call inside the second overload has no guard above it in
    # its own function, so it IS a finding -- and it must name the defining
    # file, not an external one.
    assert len(problems) == 1
    assert "itself calls" in problems[0]


# --------------------------------------------------------------------------
# Axis 3 -- WHETHER it gates. Prose is not a guard; neither is a guard that
#           protects a different function.
# --------------------------------------------------------------------------

@pytest.mark.parametrize(
    "caller_src, why",
    [
        (PROSE_ONLY_CALLER,
         "a comment describing a guard is not a guard"),
        (GUARD_IN_ANOTHER_FUNCTION,
         "a guard in a DIFFERENT function does not protect this call"),
    ],
)
def test_non_guards_are_not_credited(tmp_path, caller_src, why):
    root = _tree(tmp_path, DEFINING_FILE,
                 {"Caller.cpp": GATING_CALLER, "Nope.cpp": caller_src})
    assert verify_directive(root, "probeThing", SITE_REL), why


# --------------------------------------------------------------------------
# Axis 4 -- HOW MANY callers. Zero external callers means the directive is
#           stale, not satisfied.
# --------------------------------------------------------------------------

def test_no_external_caller_at_all_is_reported_as_stale(tmp_path):
    """A directive with no callers is a claim about an empty set.

    'Every caller gates' is vacuously true of zero callers, which is exactly
    the vacuous-oracle shape this repo keeps finding. It must fail instead: the
    symbol was renamed, or the site is unreachable, and either way the
    directive needs re-checking rather than trusting.
    """
    root = _tree(tmp_path, DEFINING_FILE, {})
    problems = verify_directive(root, "probeThing", SITE_REL)
    assert len(problems) == 1
    assert "no file under src/" in problems[0]


def test_a_same_file_caller_alone_does_not_satisfy_the_liveness_check(tmp_path):
    """The two rules are independent and must not cancel each other out.

    A caller inside the defining file is subject to the GUARD rule (axis 1c),
    but it is not an EXTERNAL caller, so it must not silence the staleness
    report. Conflating them would let a directive whose only remaining caller
    is internal read as a live, externally-gated claim.
    """
    root = _tree(tmp_path, DEFINING_FILE + SAME_FILE_GATED, {})
    problems = verify_directive(root, "probeThing", SITE_REL)
    assert len(problems) == 1
    assert "no file under src/" in problems[0]


# --------------------------------------------------------------------------
# The real tree. A synthetic matrix that passes while the shipped directive is
# broken would prove nothing about this repository.
# --------------------------------------------------------------------------

def test_the_shipped_directive_holds_on_the_real_tree():
    assert verify_directive(_REPO, "probeUnencryptedWrapper",
                            "src/core/UnencryptedWrapperProbe.cpp") == []


if __name__ == "__main__":  # pragma: no cover
    sys.exit(pytest.main([__file__, "-q"]))
