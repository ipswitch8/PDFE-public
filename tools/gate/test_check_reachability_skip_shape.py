#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Self-tests for tools/gate/check_reachability_skip_shape.py.

Run explicitly -- `pytest.ini` sets `testpaths = tests`, so a file under
`tools/gate/` is NOT auto-collected by a bare `pytest`. It runs because it is
named in the "Gate self-tests" step of `.github/workflows/fast-checks.yml`.

The claim under test
--------------------
`phase-25-dispositions.md` section 9.5:

    Three phases have now keyed a sweep on a phrase and been wrong. ... The
    durable form of this check keys on the **statement shape**: a guard whose
    failure branch exits without failing. That is an AST property, not a string
    property, and no phrase list can express it.

So the POSITIVE cases below deliberately include a fifth, previously-unseen
wording containing none of "reachable", "unreachable" or "UIA". If the checker
were phrase-keyed it would miss it, and `test_positive_fifth_unseen_wording_*`
would fail.

The NEGATIVE cases are the discriminating control against false positives. A
detector that flags every `pytest.skip` in the tree would pass every positive
case above and be worthless. The negatives -- a converted site that DOES fail,
an environment gate, a platform gate, a fixture-existence gate -- are what make
the positives mean something.
"""

from __future__ import annotations

import contextlib
import io
import json
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest

GATE_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(GATE_DIR))

from check_reachability_skip_shape import main, scan_source  # noqa: E402


def flags(code: str) -> int:
    return len(scan_source(textwrap.dedent(code), "sample.py"))


# ---------------------------------------------------------------------------
# POSITIVE -- the FOUR HISTORICAL reachability spellings, numbered 1-4 in the
# order `phase-25-dispositions.md` numbers them.
#
# The authority is the guard regex at phase-25-dispositions.md:381 --
#
#     not\\s+reachable | unreachable | (not in|absent from|missing from)\\s+(the\\s+)?UIA
#
# -- and the mutation table at phase-25-dispositions.md:439-445, which supplies
# the exact wording used in each test below. Every message here is copied from
# that table, with its source line cited in the test's docstring.
#
# These four tests were WRONG in the first F29 draft and the F29 validator gate
# caught it: spelling 1 (bare "not reachable") and spelling 4 ("absent from UIA
# tree") had no named test at all, while two novel phrases -- "UIA could not see
# the control" and "could not locate control" -- were labelled `spelling3` and
# `spelling4` as though they were historical. A test NAME asserting coverage the
# test does not provide is the same species of defect this whole programme keeps
# finding. The novel phrases were kept, honestly renamed, and moved below.
# ---------------------------------------------------------------------------


def test_positive_spelling1_bare_not_reachable():
    """Spelling 1 -- bare `not reachable`, "the spelling F11 missed".

    Wording from phase-25-dispositions.md:439 (mutation case 1).
    Regex alternation `not\\s+reachable`, phase-25-dispositions.md:381.
    """
    assert (
        flags(
            """
            import pytest
            def test_probe_widget(session):
                probeWidget = session.find("probeWidget")
                if probeWidget is None:
                    pytest.skip("probeWidget not reachable.")
                probeWidget.click()
            """
        )
        == 1
    )


def test_positive_spelling2_not_reachable_via_uia():
    """Spelling 2 -- `not reachable via UIA`, "the F11 spelling".

    Wording from phase-25-dispositions.md:440 (mutation case 2).
    """
    assert (
        flags(
            """
            import pytest
            def test_probe_widget_uia(session):
                probeWidget = session.find("probeWidget")
                if probeWidget is None:
                    pytest.skip("probeWidget not reachable via UIA.")
                probeWidget.click()
            """
        )
        == 1
    )


def test_positive_spelling3_unreachable():
    """Spelling 3 -- `unreachable`, "the third spelling".

    Wording from phase-25-dispositions.md:441 (mutation case 3).
    Regex alternation `unreachable`, phase-25-dispositions.md:381.
    """
    assert (
        flags(
            """
            import pytest
            def test_probe_widget_unreachable(session):
                if not _lookup(session, "probeWidget"):
                    pytest.skip("probeWidget unreachable")
            """
        )
        == 1
    )


def test_positive_spelling3_unreachable_prose_form():
    """Spelling 3 again, in prose form -- `was unreachable.`

    Wording from phase-25-dispositions.md:442 (mutation case 4), which the table
    labels "third spelling in prose form". Included because F25's own mutation
    set treated it as a distinct case worth reddening.
    """
    assert (
        flags(
            """
            import pytest
            def test_atlas_dialog(session):
                dlg = _open_atlas_dialog(session)
                if dlg is None:
                    pytest.skip("AtlasDialog was unreachable.")
            """
        )
        == 1
    )


@pytest.mark.parametrize(
    "message",
    [
        # The three alternations of the regex's third branch,
        # `(not in|absent from|missing from)\\s+(the\\s+)?UIA`
        # (phase-25-dispositions.md:381). The first is verbatim from
        # phase-25-dispositions.md:445 (mutation case 7, "the fourth spelling").
        "probeCanvas absent from UIA tree",
        "probeCanvas not in the UIA tree",
        "probeCanvas missing from the UIA tree",
    ],
)
def test_positive_spelling4_absent_from_uia_tree(message):
    """Spelling 4 -- `absent from` / `not in` / `missing from` [the] UIA [tree].

    This is the spelling F25's §9 recorded as the one that "turned out to exist"
    after the phase predicted a fourth phrasing would not be caught. It had NO
    named positive test in the first F29 draft.
    """
    assert (
        flags(
            """
            import pytest
            def test_probe_canvas(session):
                probeCanvas = session.find("probeCanvas")
                if probeCanvas is None:
                    pytest.skip(%r)
            """
            % message
        )
        == 1
    )


# ---------------------------------------------------------------------------
# POSITIVE -- the split record/skip shape (WIN-45's third recurrence)
# ---------------------------------------------------------------------------


def test_positive_split_record_then_innocuous_skip():
    """Reachability wording in the RECORD, innocuous wording in the SKIP.

    Wording from phase-25-dispositions.md:444 (mutation case 6) verbatim:
    `record_matrix_result(tid, "blocked", "probeDialog unreachable")` then
    `pytest.skip("probe chain prerequisite failed")`. That table calls case 6
    "the only mutation in this set that the pre-gate guard would have passed".

    Shape-wise it is just "bookkeeping call, then skip, no fail", so it needs no
    special rule here -- which is the whole argument for keying on shape.
    """
    assert (
        flags(
            """
            import pytest
            def test_split(session):
                probeDialog = find_control(session, "probeDialog")
                if probeDialog is None:
                    record_matrix_result(current_test_id(), "blocked", "probeDialog unreachable")
                    pytest.skip("probe chain prerequisite failed")
            """
        )
        == 1
    )


# ---------------------------------------------------------------------------
# POSITIVE -- CRITERION 3's "fifth, previously-unseen wording". No phrase list
# has it, and every message below is asserted to contain none of "reachable",
# "unreachable" or "UIA" -- so if this checker were phrase-keyed these rows
# would go red. This is the criterion-3 case; the two
# `test_positive_additional_unseen_wording_*` tests above are extra
# reinforcement of the same point, not substitutes for it.
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "message",
    [
        "widget not found",
        "dialog did not open",
        "Save dialog did not appear.",
        "annotColorButton click failed.",
        "",
    ],
)
def test_positive_fifth_unseen_wording_is_still_caught(message):
    for banned in ("reachable", "unreachable", "UIA"):
        assert banned.lower() not in message.lower(), "case must not leak a known phrase"
    assert (
        flags(
            """
            import pytest
            def test_unseen(session):
                dlg = open_dialog(session)
                if not dlg:
                    pytest.skip(%r)
            """
            % message
        )
        == 1
    )


def test_positive_additional_unseen_wording_uia_could_not_see():
    """NOT one of the four historical spellings -- an additional unseen wording.

    This case was mislabelled `spelling3` in the first F29 draft. It is kept
    because it is genuinely useful reinforcement, but it is NOT in
    phase-25-dispositions.md's regex or mutation table, and it DOES contain
    "UIA", so it is not the criterion-3 "fifth previously-unseen wording"
    either. Named for what it is.
    """
    assert (
        flags(
            """
            import pytest
            def test_probe(session):
                widget = _probe(session)
                if widget is None:
                    pytest.skip("UIA could not see the control")
            """
        )
        == 1
    )


def test_positive_additional_unseen_wording_could_not_locate():
    """NOT one of the four historical spellings -- an additional unseen wording.

    Mislabelled `spelling4` in the first F29 draft. It contains none of
    "reachable", "unreachable" or "UIA", so it reinforces the same point as the
    criterion-3 fifth-wording cases below; it is simply not a historical
    spelling and no longer claims to be.
    """
    assert (
        flags(
            """
            import pytest
            def test_locate(session):
                widget = find_control(session, "x")
                if widget is None:
                    pytest.skip("could not locate control")
            """
        )
        == 1
    )


def test_positive_skip_with_no_message_at_all_is_caught():
    """There is no message to key on. A phrase list has nothing to match."""
    assert (
        flags(
            """
            import pytest
            def test_nomsg(session):
                dlg = open_dialog(session)
                if dlg == 0:
                    pytest.skip()
            """
        )
        == 1
    )


def test_positive_raise_skiptest_exit_is_caught():
    assert (
        flags(
            """
            import unittest
            def test_raise(session):
                dlg = open_dialog(session)
                if dlg is None:
                    raise unittest.SkipTest("nope")
            """
        )
        == 1
    )


def test_positive_bare_return_in_a_test_is_caught():
    assert (
        flags(
            """
            def test_bare_return(session):
                dlg = open_dialog(session)
                if dlg is None:
                    return
                assert dlg.visible
            """
        )
        == 1
    )


def test_positive_else_branch_exit_is_caught():
    """The exiting arm may be the `else`, not the `if`."""
    assert (
        flags(
            """
            import pytest
            def test_else(session):
                if find_control(session, "x"):
                    pass
                else:
                    pytest.skip("nope")
            """
        )
        == 1
    )


# ---------------------------------------------------------------------------
# NEGATIVE -- the discriminating controls against false positives
# ---------------------------------------------------------------------------


def test_negative_converted_site_that_actually_fails_is_not_flagged():
    """THE control. This is the shape F25 converted every site TO.

    If this were flagged, the checker would demand undoing the fix it exists to
    protect, and every one of the positive cases above would be meaningless.
    """
    assert (
        flags(
            """
            import pytest
            def test_converted(session):
                widget = find_control(session, "x")
                if widget is None:
                    _reach = ("x not reachable.")
                    record_matrix_result(current_test_id(), "blocked", _reach)
                    pytest.fail(
                        f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
                widget.click()
            """
        )
        == 0
    )


def test_negative_branch_that_asserts_is_not_flagged():
    assert (
        flags(
            """
            def test_asserts(session):
                widget = find_control(session, "x")
                if widget is None:
                    assert False, "x not reachable"
            """
        )
        == 0
    )


def test_negative_branch_that_raises_is_not_flagged():
    assert (
        flags(
            """
            def test_raises(session):
                widget = find_control(session, "x")
                if widget is None:
                    raise RuntimeError("x not reachable")
            """
        )
        == 0
    )


def test_negative_environment_variable_gate_is_not_flagged():
    assert (
        flags(
            """
            import os
            import pytest
            def test_env():
                if os.environ.get("PDFE_LIVE_UI") != "1":
                    pytest.skip("live UI not enabled")
                if not os.getenv("PDFE_TEST_ANNOT_PIPES"):
                    pytest.skip("pipes off")
            """
        )
        == 0
    )


def test_negative_platform_gate_is_not_flagged():
    assert (
        flags(
            """
            import sys
            import pytest
            def test_platform():
                if sys.platform != "win32":
                    pytest.skip("windows only")
            """
        )
        == 0
    )


def test_negative_fixture_existence_gate_is_not_flagged():
    assert (
        flags(
            """
            import pytest
            from pathlib import Path
            def test_fixture():
                p = Path("tests/fixtures/a.pdf")
                if not p.exists():
                    pytest.skip(f"Fixture missing: {p}")
            """
        )
        == 0
    )


def test_negative_module_level_path_constant_chain_is_not_flagged():
    """`_HERE = Path(f).resolve().parent` / `ROOT = _HERE.parent` / `F = ROOT / "x"`.

    Three links. This exact chain is `tests/test_compat_workflows.py:20-24`, and
    before the provenance walker followed it the checker reported six false
    positives in that one file.
    """
    assert (
        flags(
            """
            import pytest
            from pathlib import Path
            _HERE = Path(__file__).resolve().parent
            REPO_ROOT = _HERE.parent
            FIXTURES = REPO_ROOT / "tests" / "fixtures"
            def test_chain():
                fixture = FIXTURES / "test_single.pdf"
                if not fixture.exists():
                    pytest.skip("fixture missing")
            """
        )
        == 0
    )


def test_negative_importorskip_is_not_flagged():
    assert (
        flags(
            """
            import importlib.util
            import pytest
            def test_optional_dep():
                if importlib.util.find_spec("pikepdf") is None:
                    pytest.skip("pikepdf not installed")
            """
        )
        == 0
    )


def test_negative_static_predicate_with_no_call_is_not_flagged():
    """(c): a guard on a plain value is not a runtime probe of the product."""
    assert (
        flags(
            """
            import pytest
            def test_static(param):
                if param == 3:
                    pytest.skip("case 3 is not applicable")
            """
        )
        == 0
    )


def test_negative_deferred_failure_accumulator_is_not_flagged():
    """`mismatches.append(...); continue` then `assert not mismatches` at the end."""
    assert (
        flags(
            """
            def test_accumulate(entries):
                mismatches = []
                for e in entries:
                    if not probe(e).is_ok():
                        mismatches.append(e)
                        continue
                    check(e)
                assert not mismatches
            """
        )
        == 0
    )


def test_negative_return_in_a_helper_is_not_flagged():
    """A `return` in a non-test helper is ordinary control flow, not an exit."""
    assert (
        flags(
            """
            def _open_dialog(session):
                hwnd = session.top_window()
                if not hwnd:
                    return 0
                return hwnd
            """
        )
        == 0
    )


def test_negative_nested_helper_return_does_not_taint_the_enclosing_branch():
    assert (
        flags(
            """
            import pytest
            def test_nested(session):
                if find_control(session, "x"):
                    def _inner():
                        return None
                    _inner()
                    pytest.fail("bad")
            """
        )
        == 0
    )


def test_negative_unittest_style_assertion_counts_as_failing():
    assert (
        flags(
            """
            class T:
                def test_x(self, session):
                    w = find_control(session, "x")
                    if w is None:
                        self.assertIsNotNone(w, "x not reachable")
            """
        )
        == 0
    )


# ---------------------------------------------------------------------------
# CRLF -- core.autocrlf=true
# ---------------------------------------------------------------------------


def test_crlf_source_is_scanned_identically_to_lf():
    src = (
        "import pytest\n"
        "def test_crlf(session):\n"
        "    w = find_control(session, 'x')\n"
        "    if w is None:\n"
        "        pytest.skip('nope')\n"
    )
    assert len(scan_source(src, "a.py")) == 1
    assert len(scan_source(src.replace("\n", "\r\n"), "a.py")) == 1


# ---------------------------------------------------------------------------
# CLI + ratchet behaviour
# ---------------------------------------------------------------------------


def _write(tmp_path: Path, name: str, body: str) -> Path:
    target = tmp_path / name
    target.parent.mkdir(parents=True, exist_ok=True)
    with open(target, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(textwrap.dedent(body))
    return target


OFFENDING = """
    import pytest
    def test_offender(session):
        w = find_control(session, "x")
        if w is None:
            pytest.skip("nope")
"""


def _run_main(argv):
    """Run main() and return (rc, stdout).

    `pytest.ini` sets `-p no:capture`, so the `capsys` fixture does not exist in
    this repo's pytest configuration. Capture explicitly instead of depending on
    a plugin that is switched off -- a self-test that only runs under a bare
    pytest is exactly the failure mode criterion 5 is about.
    """
    buffer = io.StringIO()
    with contextlib.redirect_stdout(buffer):
        rc = main(argv)
    return rc, buffer.getvalue()


def test_cli_exits_nonzero_on_a_new_finding(tmp_path):
    _write(tmp_path, "tests/test_bad.py", OFFENDING)
    rc, out = _run_main(["--root", str(tmp_path), "--paths", "tests"])
    assert rc == 1
    assert "test_bad.py:5" in out


def test_cli_exits_zero_when_clean(tmp_path):
    _write(
        tmp_path,
        "tests/test_good.py",
        """
        import pytest
        def test_good(session):
            w = find_control(session, "x")
            if w is None:
                pytest.fail("x not reachable")
        """,
    )
    assert main(["--root", str(tmp_path), "--paths", "tests"]) == 0


def test_baseline_ratchet_absorbs_known_and_still_catches_new(tmp_path):
    _write(tmp_path, "tests/test_bad.py", OFFENDING)
    baseline = tmp_path / "baseline.json"

    rc, _ = _run_main(
        [
            "--root",
            str(tmp_path),
            "--paths",
            "tests",
            "--baseline",
            str(baseline),
            "--update-baseline",
        ]
    )
    assert rc == 0
    payload = json.loads(baseline.read_text(encoding="utf-8"))
    assert payload["known_sites"] == ["tests/test_bad.py:5"]

    # Known site: absorbed.
    rc, _ = _run_main(
        ["--root", str(tmp_path), "--paths", "tests", "--baseline", str(baseline)]
    )
    assert rc == 0

    # A NEW site in a different file must still fail.
    _write(tmp_path, "tests/test_new.py", OFFENDING)
    rc, out = _run_main(
        ["--root", str(tmp_path), "--paths", "tests", "--baseline", str(baseline)]
    )
    assert rc == 1
    assert "test_new.py" in out


def test_repo_tree_is_clean_against_the_committed_baseline():
    """The committed ratchet must hold for the real tree.

    This is what makes the checker a gate rather than a report: if someone lands
    a new reachability skip, THIS test goes red in fast-checks.
    """
    repo_root = GATE_DIR.parent.parent
    result = subprocess.run(
        [
            sys.executable,
            str(GATE_DIR / "check_reachability_skip_shape.py"),
            "--root",
            str(repo_root),
            "--paths",
            "tests",
            "--baseline",
            str(GATE_DIR / "reachability_skip_baseline.json"),
        ],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert "0 NEW" in result.stdout
    # The other direction (GitHub #204): the committed file must not carry
    # entries that match nothing live either.
    assert "0 STALE baseline entry(ies)" in result.stdout
    assert "STALE-BASELINE" not in result.stdout


# ---------------------------------------------------------------------------
# GitHub #204 -- the baseline is BIDIRECTIONAL.
#
# Before #204 the ratchet asked "is this live finding in the baseline?" and
# never "is this baseline entry still live?", so an entry whose guard had been
# deleted or rewritten sat in the file forever. `check_magnitude_guard.py:483`
# has always asked both. These tests are the reason to believe the new half is
# real: a guard that cannot redden is decoration, so the FIRST thing pinned is
# that a planted stale entry makes the checker exit non-zero.
# ---------------------------------------------------------------------------


def _baseline_for(tmp_path: Path, extra_sites=(), extra_fingerprints=()) -> Path:
    """Generate a baseline for the tree at `tmp_path`, then plant entries in it.

    Generating rather than hand-writing matters: the planted entry then sits in
    a file that is otherwise exactly right, so a failure can only come from the
    plant.
    """
    baseline = tmp_path / "baseline.json"
    rc, _ = _run_main(
        [
            "--root",
            str(tmp_path),
            "--paths",
            "tests",
            "--baseline",
            str(baseline),
            "--update-baseline",
        ]
    )
    assert rc == 0
    payload = json.loads(baseline.read_text(encoding="utf-8"))
    payload["known_sites"] = list(payload["known_sites"]) + list(extra_sites)
    payload["known_fingerprints"] = list(payload["known_fingerprints"]) + list(
        extra_fingerprints
    )
    payload["count"] = len(payload["known_sites"])
    with open(baseline, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(payload, handle, indent=2, ensure_ascii=False)
        handle.write("\n")
    return baseline


STALE_FINGERPRINT = (
    "tests/test_vanished.py|test_gone|skip|w is None#0"  # names a file that does not exist
)


def test_planted_stale_baseline_entry_exits_nonzero(tmp_path):
    """THE reddening test for #204. Remove the stale check and this goes green."""
    _write(tmp_path, "tests/test_bad.py", OFFENDING)
    baseline = _baseline_for(tmp_path, extra_fingerprints=[STALE_FINGERPRINT])

    rc, out = _run_main(
        ["--root", str(tmp_path), "--paths", "tests", "--baseline", str(baseline)]
    )
    assert rc == 1, out
    assert "STALE-BASELINE %s" % STALE_FINGERPRINT in out
    assert "1 STALE baseline entry(ies)" in out
    # It must be reported as STALE, not misreported as a new finding.
    assert "0 NEW" in out


def test_clean_baseline_exits_zero(tmp_path):
    """The paired control: same tree, same code path, nothing planted."""
    _write(tmp_path, "tests/test_bad.py", OFFENDING)
    baseline = _baseline_for(tmp_path)

    rc, out = _run_main(
        ["--root", str(tmp_path), "--paths", "tests", "--baseline", str(baseline)]
    )
    assert rc == 0, out
    assert "0 STALE baseline entry(ies)" in out


def test_allow_stale_downgrades_stale_but_not_an_unreviewed_site(tmp_path):
    """`--allow-stale-baseline` is an escape for ONE half of the check only.

    An escape flag that also muted a genuine unreviewed finding would be a way
    to turn the whole gate off, and someone would eventually add it to the CI
    invocation to quiet a stale entry.
    """
    _write(tmp_path, "tests/test_bad.py", OFFENDING)
    baseline = _baseline_for(tmp_path, extra_fingerprints=[STALE_FINGERPRINT])

    # Half one: the stale entry alone is downgraded to a warning.
    rc, out = _run_main(
        [
            "--root",
            str(tmp_path),
            "--paths",
            "tests",
            "--baseline",
            str(baseline),
            "--allow-stale-baseline",
        ]
    )
    assert rc == 0, out
    assert "STALE-BASELINE %s" % STALE_FINGERPRINT in out
    assert "downgraded by --allow-stale-baseline" in out

    # Half two: with the SAME flag, a genuinely unreviewed site still fails.
    _write(tmp_path, "tests/test_new.py", OFFENDING)
    rc, out = _run_main(
        [
            "--root",
            str(tmp_path),
            "--paths",
            "tests",
            "--baseline",
            str(baseline),
            "--allow-stale-baseline",
        ]
    )
    assert rc == 1, out
    assert "test_new.py" in out
    assert "1 NEW" in out


def test_stale_detection_survives_the_legacy_line_keyed_schema(tmp_path):
    """A schema-1 baseline (no `known_fingerprints`) is still checked both ways.

    This is the shape `check_magnitude_guard.py` uses, and every baseline
    written before #204 is in it. If the fallback silently passed, an old file
    would keep the old one-directional behaviour forever.
    """
    _write(tmp_path, "tests/test_bad.py", OFFENDING)
    baseline = tmp_path / "legacy.json"
    with open(baseline, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(
            {
                "count": 2,
                "known_sites": ["tests/test_bad.py:5", "tests/test_vanished.py:9"],
            },
            handle,
            indent=2,
        )
        handle.write("\n")

    rc, out = _run_main(
        ["--root", str(tmp_path), "--paths", "tests", "--baseline", str(baseline)]
    )
    assert rc == 1, out
    assert "STALE-BASELINE tests/test_vanished.py:9" in out
    assert "0 NEW" in out


# ---------------------------------------------------------------------------
# #204, second half: why the entries are fingerprinted rather than line-keyed.
#
# A hard fail on stale `file:line` entries would redden on any edit ABOVE a
# guard -- which is most edits -- and the cheapest way out would be
# `--update-baseline`, silently re-baselining everything else that drifted at
# the same time. That is a guard that trains people to bypass it. These two
# tests pin the property that makes the hard fail liveable.
# ---------------------------------------------------------------------------


def test_line_drift_is_neither_new_nor_stale(tmp_path):
    """Insert lines above the guard: same guard, different line, still known."""
    _write(tmp_path, "tests/test_bad.py", OFFENDING)
    baseline = _baseline_for(tmp_path)

    _write(
        tmp_path,
        "tests/test_bad.py",
        """
        # a new comment
        # and another
        import pytest
        def test_offender(session):
            w = find_control(session, "x")
            if w is None:
                pytest.skip("nope")
        """,
    )
    rc, out = _run_main(
        ["--root", str(tmp_path), "--paths", "tests", "--baseline", str(baseline)]
    )
    assert rc == 0, out
    assert "0 NEW" in out
    assert "0 STALE baseline entry(ies)" in out
    # The drift IS reported -- quietly, and as drift.
    assert "DRIFT-SITE tests/test_bad.py:5" in out


def test_a_drifted_line_key_cannot_absorb_a_different_guard(tmp_path):
    """The hazard that made fingerprints authoritative rather than additive.

    If line keys still counted, a DIFFERENT, never-reviewed guard landing on a
    line a baseline entry used to occupy would be absorbed in silence. Here the
    original guard is deleted and an unrelated one is written at the same line.
    """
    _write(tmp_path, "tests/test_bad.py", OFFENDING)
    baseline = _baseline_for(tmp_path)
    assert "tests/test_bad.py:5" in json.loads(baseline.read_text(encoding="utf-8"))[
        "known_sites"
    ]

    _write(
        tmp_path,
        "tests/test_bad.py",
        """
        import pytest
        def test_offender(session):
            other = lookup_dialog(session, "save")
            if other is None:
                pytest.skip("different guard entirely")
        """,
    )
    rc, out = _run_main(
        ["--root", str(tmp_path), "--paths", "tests", "--baseline", str(baseline)]
    )
    assert rc == 1, out
    assert "1 NEW" in out  # the replacement is NOT absorbed
    assert "1 STALE baseline entry(ies)" in out  # and the original is reported gone


def test_identical_guards_in_one_file_get_distinct_entries(tmp_path):
    """Fingerprints must not collapse N identical guards onto one entry.

    If they did, deleting one of three identical guards would leave the
    baseline matching and the stale check blind.
    """
    _write(
        tmp_path,
        "tests/test_twins.py",
        """
        import pytest
        def test_a(session):
            w = find_control(session, "x")
            if w is None:
                pytest.skip("nope")
            w2 = find_control(session, "x")
            if w2 is None:
                pytest.skip("nope")
        """,
    )
    baseline = _baseline_for(tmp_path)
    payload = json.loads(baseline.read_text(encoding="utf-8"))
    assert payload["count"] == 2
    assert len(set(payload["known_fingerprints"])) == 2

    # Delete the second guard: exactly one entry must go stale.
    _write(
        tmp_path,
        "tests/test_twins.py",
        """
        import pytest
        def test_a(session):
            w = find_control(session, "x")
            if w is None:
                pytest.skip("nope")
        """,
    )
    rc, out = _run_main(
        ["--root", str(tmp_path), "--paths", "tests", "--baseline", str(baseline)]
    )
    assert rc == 1, out
    assert "1 STALE baseline entry(ies)" in out


def test_update_baseline_preserves_the_ownership_block(tmp_path):
    """The defect that erased the real `_ownership` block on 2026-09-18.

    `--update-baseline` rewrote the payload from scratch, so commit e6c36ae1 --
    a routine refresh after a test edit -- deleted the ownership record written
    on 2026-09-07 without anyone noticing. An operator-authored `_` key must
    survive a refresh.
    """
    _write(tmp_path, "tests/test_bad.py", OFFENDING)
    baseline = _baseline_for(tmp_path)

    payload = json.loads(baseline.read_text(encoding="utf-8"))
    payload["_ownership"] = {"owner": "harness", "issue": "https://example.invalid/1"}
    payload["_hand_written_note"] = "do not lose me"
    with open(baseline, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(payload, handle, indent=2, ensure_ascii=False)
        handle.write("\n")

    _write(tmp_path, "tests/test_second.py", OFFENDING)
    rc, out = _run_main(
        [
            "--root",
            str(tmp_path),
            "--paths",
            "tests",
            "--baseline",
            str(baseline),
            "--update-baseline",
        ]
    )
    assert rc == 0, out
    refreshed = json.loads(baseline.read_text(encoding="utf-8"))
    assert refreshed["_ownership"] == {
        "owner": "harness",
        "issue": "https://example.invalid/1",
    }
    assert refreshed["_hand_written_note"] == "do not lose me"
    # And the refresh must SAY what it just absorbed, so a silent re-baseline of
    # unreviewed sites is at least visible in the log.
    assert "ABSORBED-NEW tests/test_second.py" in out


def test_gtest_scratch_directories_are_not_scanned_but_real_tests_are(tmp_path):
    """`tests/_gtest_*/` is gitignored run scratch, and is full of COPIES.

    The pair matters: the exclusion is by prefix, so the control below proves it
    did not widen into "any directory starting with an underscore" or, worse,
    stop scanning the real tests. Measured cause: a concurrent run left
    `tests/_gtest_209proof/` -- copies of four `tests/ui_automation` files --
    and the checker reported four new findings that exist nowhere in the
    tracked tree.
    """
    _write(tmp_path, "tests/_gtest_scratch/test_copy.py", OFFENDING)
    rc, out = _run_main(["--root", str(tmp_path), "--paths", "tests"])
    assert rc == 0, out
    assert "0 structurally-matching guard(s)" in out

    _write(tmp_path, "tests/ui_automation/test_copy.py", OFFENDING)
    rc, out = _run_main(["--root", str(tmp_path), "--paths", "tests"])
    assert rc == 1, out
    assert "tests/ui_automation/test_copy.py:5" in out
    assert "_gtest_scratch" not in out


def test_committed_baseline_carries_a_usable_ownership_record():
    """The #204 ask: the block must not name the closed issue it used to.

    Issue state cannot be read offline, so what is pinned here is what is
    checkable from the tree: the block exists, it does not point at #143, it
    names an issue URL, and it says when and why it was repointed.
    """
    payload = json.loads(
        (GATE_DIR / "reachability_skip_baseline.json").read_text(encoding="utf-8")
    )
    ownership = payload.get("_ownership")
    assert ownership, "the ownership block was deleted again -- see #204"
    issue = ownership["issue"]
    assert issue.startswith("https://github.com/ipswitch8/PDFE/issues/")
    assert not issue.rstrip("/").endswith("/143"), (
        "#143 is CLOSED (2026-09-07); the burn-down owner must be an OPEN issue"
    )
    assert ownership["repointed"] == "2026-09-18"
    assert ownership["repoint_note"].strip()
    assert ownership["disposition"] == "burn-down"


def test_committed_baseline_is_schema_2_and_internally_consistent():
    """One fingerprint per site, all distinct, `count` agreeing with both."""
    payload = json.loads(
        (GATE_DIR / "reachability_skip_baseline.json").read_text(encoding="utf-8")
    )
    assert payload["schema"] == 2
    sites = payload["known_sites"]
    fingerprints = payload["known_fingerprints"]
    assert payload["count"] == len(sites) == len(fingerprints)
    assert len(set(fingerprints)) == len(fingerprints)
    assert len(set(sites)) == len(sites)
    # A fingerprint carries no line number: its first field is the file, and the
    # site key for the same index names that same file.
    for site, fingerprint in zip(sites, fingerprints):
        assert fingerprint.split("|", 1)[0] == site.rsplit(":", 1)[0]


# ---------------------------------------------------------------------------
# DCA 0075 -- the shape that escaped into tests/ui_automation/test_ui_canvas_mat_live.py
# at 66c2a05c: a helper takes the fixture as a PARAMETER and skips when it does
# not exist. Both fixtures it is ever called with are TRACKED, so the branch can
# only mean a broken checkout -- a skip there is a false green.
#
# The pair below pins the distinction the checker actually draws, which is NOT
# "fixture-existence gates are fine" (see
# test_negative_fixture_existence_gate_is_not_flagged) but "can the path be
# traced to a module-level constant?". A parameter cannot be, so the checker
# cannot prove the file is tracked and must flag it. Both directions are pinned:
# drop either and the checker could regress to all-flag or all-exempt and stay
# green.
# ---------------------------------------------------------------------------


def test_positive_skip_on_a_parameter_path_existence_check_is_caught():
    """The literal pre-fix shape of `_session()` in test_ui_canvas_mat_live.py."""
    assert (
        flags(
            """
            import pytest
            def _session(fixture, settle=1.5):
                if not fixture.exists():
                    pytest.skip(f"fixture missing: {fixture}")
                return 1
            """
        )
        == 1
    )


def test_negative_same_check_on_a_module_level_constant_is_not_flagged():
    """Same branch, same wording -- but the path is traceable to a constant.

    This is the control for the test above. It differs by exactly one thing:
    where the path comes from. If this one started flagging, the checker would
    be rejecting the legitimate environment-gate shape the project relies on.
    """
    assert (
        flags(
            """
            import pytest
            from pathlib import Path
            FIX = Path("tests/fixtures/a.pdf")
            def _session(settle=1.5):
                if not FIX.exists():
                    pytest.skip(f"fixture missing: {FIX}")
                return 1
            """
        )
        == 0
    )


# ---------------------------------------------------------------------------
# GitHub #305 -- a STRING predicate on a value in hand is not a product probe.
#
# #305 reported this checker red on `master` with four unbaselined sites, three
# genuine and one a false positive:
#
#     tests/visual/test_failure_artifacts_are_run_scoped.py
#         for line in src.splitlines():
#             s = line.strip()
#             if s.startswith("#"):
#                 continue
#
# That skips COMMENT LINES while parsing source text. Nothing is driven and
# nothing can be unreachable, but it matched the shape (a `continue` under a
# guard whose branch does not fail). The issue was explicit that the fix
# belonged in the CHECKER -- "the fourth needs the checker taught, not the
# baseline grown" -- because baselining a false positive grows a burn-down list
# with an entry that has nothing to burn down.
#
# Teaching it retired a SECOND site of the same shape that was already
# baselined (`name.startswith('Page ')` in test_ui_annotations.py), which the
# bidirectional ratchet then reported as STALE -- #204 working as designed.
#
# The second test below is the one that matters most: it pins the GREEN
# DIRECTION, so the exemption cannot have blinded the checker to the class it
# exists for. An exemption without that test is indistinguishable from a hole.
# ---------------------------------------------------------------------------


def test_negative_i305_a_comment_line_filter_is_not_a_reachability_probe():
    assert flags(
        """
        def test_every_definition_goes_through_the_helper():
            src = io.open("x.py").read()
            for line in src.splitlines():
                s = line.strip()
                if s.startswith("#"):
                    continue
                assert "FORBIDDEN" not in s
        """
    ) == 0, (
        "GitHub #305: skipping comment lines while parsing source text was "
        "reported as a WIN-45 reachability skip. A string predicate answers a "
        "question about a value already in hand; it cannot mean 'the product "
        "could not be driven'."
    )


def test_negative_i305_other_string_predicates_are_also_exempt():
    # The same property through the other spellings actually used in this tree.
    for pred in ("endswith('.py')", "isspace()", "isdigit()", "isidentifier()"):
        code = f"""
        def test_parse(session):
            for line in open("x").read().splitlines():
                if line.{pred}:
                    continue
                assert line
        """
        assert flags(code) == 0, f"line.{pred} was treated as a product probe"


def test_positive_i305_a_real_reachability_continue_is_STILL_flagged():
    # THE GREEN DIRECTION. If this ever returns 0 the #305 exemption has
    # widened into a hole and the checker has stopped doing its job.
    assert flags(
        """
        def test_each_control_is_reachable(session):
            for name in ("linkToolAction", "formToolAction"):
                if session.find_by_suffix(name) is None:
                    continue
                assert session.find_by_suffix(name).is_enabled()
        """
    ) == 1, (
        "a `continue` guarded by a live lookup returning None is the WIN-45 "
        "shape and must still be flagged -- the #305 exemption must cover "
        "string predicates ONLY"
    )


def test_positive_i305_a_string_predicate_does_not_launder_a_lookup_guard():
    # A lookup whose RESULT is then string-tested is still a lookup guard: the
    # outermost predicate is the comparison, not a string method on a value in
    # hand. This is the shape that would make the exemption dangerous if it
    # keyed on "a string method appears anywhere in the condition".
    assert flags(
        """
        def test_title(session):
            if session.top_window().window_text() == "":
                pytest.skip("no window")
            assert session.top_window()
        """
    ) == 1, (
        "a guard built from a live product lookup must stay flagged even when "
        "its result is compared as text"
    )


# GitHub #305, the HOLE the p98 phase-3 claim-check gate found by probing the
# live classifier rather than reading the tests.
#
# The first version of the TEXT_PREDICATES exemption keyed only on the
# outermost call being a string predicate. That exempted
#
#     if find_by_suffix(name).window_text().startswith("Page "):
#         continue
#
# which is a PRODUCT PROBE with a string predicate bolted on the end -- the
# same shape as the `isinstance`-wrapping-a-lookup blind spot already
# documented above, but reachable by an idiom somebody would plausibly write,
# since chaining off a lookup is natural.
#
# The exemption now applies only when nothing in the receiver chain is itself a
# non-environmental call. These pin both directions, because an exemption
# without a green-direction test is indistinguishable from a hole -- which is
# precisely what this one was.


def test_positive_i305_a_text_predicate_cannot_launder_a_lookup_CHAIN():
    assert flags(
        """
        def test_pages(session):
            for name in ("one", "two"):
                if session.find_by_suffix(name).window_text().startswith("Page "):
                    continue
                assert name
        """
    ) == 1, (
        "GitHub #305: a string predicate chained off a LIVE LOOKUP was exempted "
        "as a value-in-hand. The lookup is the probe; `.startswith` on its "
        "result does not make it one."
    )


def test_positive_i305_the_same_hole_through_a_bare_call():
    assert flags(
        """
        def test_title(session):
            if find_window_title().startswith("PDFE"):
                pytest.skip("already open")
            assert find_window_title()
        """
    ) == 1, (
        "a text predicate on the result of a bare (non-environmental) call is "
        "still a product probe"
    )


def test_negative_i305_an_environmental_receiver_stays_exempt():
    # os.getenv is in ENV_CALLS, so a string predicate on its result is an
    # environment gate and must NOT be flagged -- the exemption has to stay
    # useful, not just safe.
    assert flags(
        """
        def test_env(session):
            if os.getenv("PDFE_MODE", "").startswith("ci"):
                pytest.skip("not for CI mode")
            assert session
        """
    ) == 0, "a string predicate on an environment probe is an environment gate"


def test_negative_i305_a_plain_local_string_stays_exempt():
    # The regression this exemption exists for, re-asserted after narrowing:
    # narrowing must not have reintroduced the original false positive.
    assert flags(
        """
        def test_parse():
            for line in open("x").read().splitlines():
                s = line.strip()
                if s.startswith("#"):
                    continue
                assert s
        """
    ) == 0, "narrowing the exemption reintroduced the #305 false positive"


# GitHub #305, the OVER-CORRECTION — found by the p98 phase-3 claim-check gate
# probing the narrowed classifier with the single-expression form of the very
# idiom the exemption exists for.
#
# Narrowing the exemption to break on any call in the receiver chain meant
#
#     if line.strip().startswith("#"):
#         continue
#
# was FLAGGED, while the split form
#
#     s = line.strip()
#     if s.startswith("#"):
#         continue
#
# stayed exempt. Two spellings of the same thing, opposite verdicts, and the
# chained one is the more natural. Since this checker gates `fast-checks.yml`
# on push, a developer writing the natural form would have got a false WIN-45
# red whose only escapes were baselining a false positive — which #305
# explicitly calls the wrong fix — or rewriting correct code around a checker
# bug.
#
# `TEXT_TRANSFORMS` fixes it: a string transform in the chain is still a value
# in hand. The negative control below is the important half — a transform must
# not launder a real lookup.


def test_negative_i305_a_transform_chained_into_a_predicate_stays_exempt():
    assert flags(
        """
        def test_parse():
            for line in open("x").read().splitlines():
                if line.strip().startswith("#"):
                    continue
                assert line
        """
    ) == 0, (
        "GitHub #305: the single-expression form of the comment-line filter was "
        "flagged while the split form was exempt. A string transform in the "
        "chain is still a value in hand."
    )


def test_negative_i305_several_transforms_in_a_chain_stay_exempt():
    assert flags(
        """
        def test_parse():
            for raw in open("x").read().splitlines():
                if raw.strip().lower().removeprefix("x").startswith("#"):
                    continue
                assert raw
        """
    ) == 0, "a chain of pure string transforms is still a value in hand"


def test_positive_i305_a_transform_does_NOT_launder_a_lookup():
    # THE NEGATIVE CONTROL. If this returns 0, TEXT_TRANSFORMS has become a
    # bypass: anyone could hide a product probe behind `.strip()`.
    assert flags(
        """
        def test_title(session):
            for name in ("a", "b"):
                if session.find_by_suffix(name).text().strip().startswith("P"):
                    continue
                assert name
        """
    ) == 1, (
        "a string transform between a live lookup and a predicate must NOT "
        "restore the exemption -- the lookup is still the probe"
    )


def test_positive_i305_a_transform_on_a_bare_call_is_still_a_probe():
    assert flags(
        """
        def test_env(session):
            if read_window_title().strip().startswith("PDFE"):
                pytest.skip("already open")
            assert session
        """
    ) == 1, "an unrecognised call in the chain must break the exemption"


# GitHub #305, the THIRD correction — the unbound-method call form, found by
# the p98 phase-3 claim-check gate probing the live classifier.
#
# Python lets the same predicate be written two ways:
#
#     s.startswith("P")             # bound   -- receiver IS the value
#     str.startswith(s, "P")        # unbound -- receiver is the TYPE
#
# A receiver-only check trivially passes the unbound form, because
# `call.func.value` is the bare name `str` and contains no calls at all — so it
# never inspects the argument, where a live lookup can sit. Measured:
#
#     session.find_by_suffix("x").text().startswith("P")       -> flagged
#     str.startswith(session.find_by_suffix("x").text(), "P")  -> MISSED
#
# Same guard, two spellings, opposite verdicts — and the missed one is exactly
# the WIN-45 false green this checker exists to catch.
#
# The rule is now the whole expression: receiver AND positional AND keyword
# arguments must all be value-in-hand. Being stricter cannot create a false
# green; it can only decline to exempt, and the live-tree count is unchanged at
# 412, so it declines nothing actually written here.
#
# This exemption has been wrong three times — a laundering hole, then a false
# positive on correct code, then this. Each wrong version passed the tests I
# wrote alongside it. Every one was found by a gate probing the LIVE classifier
# with a shape my tests did not contain, which is the lesson worth keeping.


def test_positive_i305_the_unbound_form_cannot_launder_a_lookup():
    assert flags(
        """
        def test_unbound(session):
            if str.startswith(session.find_by_suffix("x").text(), "P"):
                pytest.skip("already open")
            assert session
        """
    ) == 1, (
        "GitHub #305: `str.startswith(<live lookup>, ...)` was exempted because "
        "the receiver is the bare type name and the argument was never "
        "inspected. The value can live in an argument."
    )


def test_negative_i305_the_unbound_form_on_a_local_stays_exempt():
    # The exemption must remain useful in the unbound spelling too, or the fix
    # has just moved the false positive somewhere less obvious.
    assert flags(
        """
        def test_unbound_ok():
            for line in open("x").read().splitlines():
                if str.startswith(line, "#"):
                    continue
                assert line
        """
    ) == 0, "the unbound form on a plain local is still a value in hand"


def test_positive_i305_a_lookup_as_the_predicate_ARGUMENT_is_a_probe():
    # Narrower than the unbound case and worth its own pin: the receiver is
    # innocent, but the predicate's truth depends on a live lookup passed in.
    assert flags(
        """
        def test_arg(session):
            s = "abc"
            if s.startswith(session.find_by_suffix("x").text()):
                pytest.skip("matched")
            assert s
        """
    ) == 1, (
        "a text predicate whose ARGUMENT is a live lookup still derives its "
        "truth from that lookup"
    )


def test_negative_i305_keyword_arguments_are_checked_too():
    # `expandtabs(tabsize=...)`-style keywords go through the same path; a
    # literal keyword must not break the exemption.
    assert flags(
        """
        def test_kw():
            for line in open("x").read().splitlines():
                if line.expandtabs(tabsize=4).startswith("#"):
                    continue
                assert line
        """
    ) == 0, "a literal keyword argument must not revoke the exemption"


# GitHub #305, the FOURTH correction — and the one that changed the mechanism
# rather than extending a list.
#
# The p98 phase-3 claim-check gate found this by driving the live classifier:
#
#     if windows[0].title().startswith("P"):
#         continue
#
# was exempted. The previous version walked the receiver with `ast.walk()` and
# inspected only `ast.Call` nodes, so the SUBSCRIPT `windows[0]` was invisible
# — and the one call present, `.title()`, collides by name with the string
# method `str.title()` while here being a UI accessor. Neither half fails
# alone; only the combination slipped.
#
# Patching `Subscript` would have been a fourth enumeration fix with `BoolOp`,
# `IfExp`, comprehensions and f-strings all still waiting. So
# `_chain_is_value_in_hand` was rewritten as a WHITELIST: an expression is
# value-in-hand only if every node in it is one of a few shapes that provably
# cannot look anything up. Anything unrecognised fails CLOSED into "flag it".
#
# These tests pin the inversion: the shapes that must stay exempt, and a
# representative set of the ones that must break the chain — including two the
# whitelist handles for free that no previous version did.


def test_positive_i305_a_subscript_in_the_chain_breaks_the_exemption():
    assert flags(
        """
        def test_subscript(session):
            windows = session.windows()
            for w in windows:
                if windows[0].title().startswith("P"):
                    continue
                assert w
        """
    ) == 1, (
        "GitHub #305: a subscripted receiver was invisible to the old "
        "Call-only walk, and `.title()` collided with the string transform of "
        "the same name. A subscript must break the chain."
    )


def test_positive_i305_a_conditional_expression_breaks_the_exemption():
    # Not a shape any previous version considered; the whitelist rejects it
    # because IfExp is simply not on the permitted list.
    assert flags(
        """
        def test_ifexp(session):
            s = "a"
            if (s if s else session.lookup()).startswith("P"):
                pytest.skip("x")
            assert s
        """
    ) == 1, "a conditional expression can hide a lookup in either branch"


def test_positive_i305_arithmetic_in_the_chain_breaks_the_exemption():
    assert flags(
        """
        def test_concat(session):
            if ("prefix" + session.title()).startswith("P"):
                pytest.skip("x")
            assert session
        """
    ) == 1, "a BinOp can carry a lookup on either side"


def test_negative_i305_the_permitted_shapes_all_stay_exempt():
    # The whitelist has to remain USEFUL, not merely safe. Each of these is a
    # value in hand reached by a permitted shape.
    cases = [
        'line.startswith("#")',
        'line.strip().startswith("#")',
        'line.strip().lower().removeprefix("x").startswith("#")',
        'str.startswith(line, "#")',
        'line.expandtabs(tabsize=4).startswith("#")',
        # An attribute chain is permitted only where its root provably is not a
        # product handle. `obj.name.startswith("#")` USED to be on this list and
        # is now a positive below -- see the fifth correction.
        'sys.platform.startswith("win")',
        'os.name.startswith("nt")',
    ]
    for cond in cases:
        code = f"""
        def test_parse(obj):
            for line in open("x").read().splitlines():
                if {cond}:
                    continue
                assert line
        """
        assert flags(code) == 0, f"{cond} should be exempt and was flagged"


# GitHub #305, the FIFTH correction -- and the first one this exemption caused
# itself rather than inherited.
#
# The whitelist rewrite (fourth correction) permitted `obj.attr` recursively,
# reasoning in a comment that a pure attribute chain off a live object was
# harmless because "with no call anywhere, the condition fails requirement (c)
# PROBES and is never a finding regardless".
#
# The p98 phase-3 claim-check gate instrumented `_condition_provenance` and
# showed that is FALSE in the only situation the helper is ever consulted
# about -- a chain wrapped in a text predicate, where the PREDICATE supplies
# `has_call=True` itself, so (c) is satisfied and permitting the receiver then
# sets `all_calls_environmental`:
#
#     session.current_window().window_text().startswith("Foo")  -> flagged
#     session.current_window.title.startswith("Foo")            -> MISSED
#     session.current_window.title.strip().startswith("Foo")    -> MISSED
#
# Same laundering as the three fixed `Call` holes, through a shape with no call
# syntax at all. Nor is attribute access inert here: a pywinauto wrapper
# resolves children and round-trips to UIA on attribute READ, so `.title` IS a
# product probe.
#
# The arm now permits a chain only where its ROOT is provably not a product
# handle -- a named `ENV_ATTRS` entry, a `SAFE_MODULE_ROOTS` module, a local the
# path typing proved is a filesystem path, or a literal. The pair of tests below
# is the point: the idiom that must survive (`sys.platform.startswith`) and the
# shape that must now break.
#
# Five wrong versions, every one found by a gate driving the LIVE classifier
# with a shape the tests written beside it did not contain. That is the finding
# worth keeping, more than any individual hole.


def test_positive_i305_a_bare_attribute_chain_off_a_live_object_is_a_probe():
    assert flags(
        """
        def test_attr(session):
            if session.current_window.title.startswith("Foo"):
                pytest.skip("already open")
            assert session
        """
    ) == 1, (
        "GitHub #305: the whitelist's Attribute arm permitted any `obj.attr` "
        "chain, so a live UIA property read laundered itself through a text "
        "predicate with no call in the receiver at all."
    )


def test_positive_i305_a_transform_does_not_rescue_a_bare_attribute_chain():
    assert flags(
        """
        def test_attr2(session):
            if session.current_window.title.strip().startswith("Foo"):
                pytest.skip("already open")
            assert session
        """
    ) == 1, "a string transform must not restore the exemption for an attribute chain"


def test_negative_i305_an_environment_attribute_predicate_stays_exempt():
    # THE CONTROL on the other side. `sys.platform.startswith("win")` is the
    # most common skip condition in this repo's Python tests; flagging it would
    # redden correct code in a push gate, which is what the second correction
    # had to undo. The root is a named ENV_ATTRS entry, so it stays exempt.
    assert flags(
        """
        def test_plat(session):
            if sys.platform.startswith("win"):
                pytest.skip("not on windows")
            assert session
        """
    ) == 0, "an environment-attribute predicate must stay exempt"


def test_positive_i305_self_attributes_are_not_assumed_inert():
    # `self.` is not a safe root either: in a harness class the attribute can be
    # the live session. Fails closed.
    assert flags(
        """
        def test_self(session):
            helper = session
            if helper.window.title.startswith("Foo"):
                pytest.skip("x")
            assert helper
        """
    ) == 1, "an opaque local's attribute chain cannot be told from a live handle"


# GitHub #305, the SIXTH correction -- the fifth correction undone by a line
# break.
#
# `_chain_is_value_in_hand` treated a bare `ast.Name` as value-in-hand
# UNCONDITIONALLY. The p98 phase-3 claim-check gate drove the real
# `scan_source` and measured:
#
#     if session.current_window.title.startswith("Foo"): skip   -> 1 finding
#     title = session.current_window.title
#     if title.startswith("Foo"): skip                          -> MISSED
#     title = session.current_window().window_text()
#     if title.startswith("Foo"): skip                          -> MISSED
#
# Same lookup, same predicate, same skip; the only difference is whether the
# lookup sits on the line above. Nothing about that rewrite is evasive -- it is
# what anyone would write for readability -- and `title = session.window.
# window_text()` is used dozens of times across the UI tests, one `.startswith`
# guard away from laundering a real reachability probe.
#
# The old comment claimed `s.startswith("#")` was settled "regardless of where
# `s` came from, because asking what a string BEGINS WITH cannot be a probe".
# The predicate was never the probe: the BINDING is, and the condition's truth
# depends on it. So the `Name` arm now chases the binding through the assignment
# index and applies the same whitelist to whatever it was bound from.
#
# One live site changed verdict -- `tests/test_ui_annotations.py:3606`, a benign
# page-header row filter inside an enumeration loop -- and its baseline entry,
# which this phase had removed as stale, is restored. The count is back to the
# 413 it was at HEAD, so this correction grows nothing: the real reachability
# bail in that test (`if target_row is None: pytest.skip(...)`) was already a
# separate known site.


def test_positive_i305_a_split_attribute_chain_is_still_a_probe():
    assert flags(
        """
        def test_split_attr(session):
            title = session.current_window.title
            if title.startswith("Foo"):
                pytest.skip("not reachable")
            assert session
        """
    ) == 1, (
        "GitHub #305: moving the lookup to the line above defeated the fifth "
        "correction, because a bare name was value-in-hand without regard to "
        "its binding."
    )


def test_positive_i305_a_split_call_chain_is_still_a_probe():
    assert flags(
        """
        def test_split_call(session):
            title = session.current_window().window_text()
            if title.startswith("Foo"):
                pytest.skip("not reachable")
            assert session
        """
    ) == 1, "the call-chain form must not be laundered by a line break either"


def test_positive_i305_a_walrus_binding_of_a_lookup_is_a_probe():
    # `:=` goes through the same index (`ast.NamedExpr`), so it must not be an
    # oblique route back to the same hole.
    assert flags(
        """
        def test_walrus(session):
            if (t := session.window.title).startswith("Foo"):
                pytest.skip("x")
            assert t
        """
    ) == 1, "a walrus binding is a binding"


def test_negative_i305_a_split_transform_still_stays_exempt():
    # THE CONTROL. This is the shape the SECOND correction had to rescue; if
    # chasing bindings reddens it, this correction has just re-created that
    # false positive on the natural spelling of a text filter.
    assert flags(
        """
        def test_split_transform():
            for line in open("x").read().splitlines():
                s = line.strip()
                if s.startswith("#"):
                    continue
                assert s
        """
    ) == 0, "a binding to a pure transform of a loop variable is a value in hand"


def test_negative_i305_a_self_referential_binding_terminates_and_stays_exempt():
    # `s = s.strip()` would recur forever through the index. The chase stops on
    # a repeated name and allows, which is sound: every step traversed to get
    # back here was itself permitted, and an impermissible one would already
    # have returned False. If this ever hangs or flags, the cycle guard broke.
    assert flags(
        """
        def test_self_binding():
            for line in open("x").read().splitlines():
                s = line
                s = s.strip().lower()
                if s.startswith("#"):
                    continue
                assert s
        """
    ) == 0, "a self-referential binding must terminate and stay exempt"


def test_negative_i305_an_environment_attribute_through_a_local_stays_exempt():
    assert flags(
        """
        def test_plat_local(session):
            plat = sys.platform
            if plat.startswith("win"):
                pytest.skip("x")
            assert session
        """
    ) == 0, "chasing the binding must also carry the environment exemption through"


# GitHub #305, the SEVENTH correction -- a hole inside the mechanism the sixth
# correction added, found by the p98 phase-3 claim-check gate asking the one
# question the fix had not been tested against: can a CONDITIONAL rebinding
# route around the binding chase?
#
#     if cond:
#         t = session.find_window().title()
#     else:
#         t = "default"
#     if t.startswith("P"):
#         continue                                  -> 0 findings
#
# `_AssignmentIndex.latest_before` returns the TEXTUALLY last binding at or
# before the guard line, which is not the one that ran. The literal in the
# `else` is written second, so the chase was handed the literal and never saw
# the lookup. Swap the two branch bodies and the same code is flagged: same
# semantics, opposite verdict, decided by which branch came second. And it is
# the ordinary way anyone writes "use the real value if special-cased, else a
# default" -- nothing evasive about it.
#
# The class docstring two lines above the bug claimed the index "errs toward
# reporting rather than hiding". For this accessor that was false; corrected
# there as well.
#
# Path-sensitivity is NOT needed to fix it, and guessing at it would be its own
# source of error. MULTIPLICITY is enough: `all_before` returns every candidate
# binding and the exemption requires ALL of them to be value-in-hand. One
# lookup among the candidates is a lookup, whichever branch wrote it. The
# direction is the same one every correction in this sequence has needed -- when
# the static answer is ambiguous, decline to exempt.


def test_positive_i305_a_conditional_rebinding_cannot_hide_the_lookup():
    assert flags(
        """
        def test_cond(session, cond):
            for w in session.titles():
                if cond:
                    t = session.find_window().title()
                else:
                    t = "default"
                if t.startswith("P"):
                    continue
                assert w
        """
    ) == 1, (
        "GitHub #305: the binding chase took the textually last assignment, so "
        "an innocuous `else` branch written second hid the lookup in the `if`."
    )


def test_positive_i305_the_same_shape_with_the_branches_swapped():
    # The pair is the point. Before the fix these two disagreed; a checker whose
    # verdict depends on which branch was written second is not deciding on the
    # code's meaning.
    assert flags(
        """
        def test_cond2(session, cond):
            for w in session.titles():
                if cond:
                    t = "default"
                else:
                    t = session.find_window().title()
                if t.startswith("P"):
                    continue
                assert w
        """
    ) == 1, "both orderings of the same conditional rebinding must agree"


def test_positive_i305_a_try_except_rebinding_is_also_multiple_bindings():
    assert flags(
        """
        def test_try(session):
            try:
                t = session.window.title
            except Exception:
                t = ""
            if t.startswith("P"):
                pytest.skip("x")
            assert session
        """
    ) == 1, "try/except is the other everyday two-binding shape"


def test_negative_i305_two_innocuous_bindings_of_one_name_stay_exempt():
    # THE CONTROL on the strictness. Requiring ALL bindings to be harmless must
    # not mean "more than one binding is suspicious" -- two transforms of a loop
    # variable are both values in hand and the site must stay exempt.
    assert flags(
        """
        def test_two(cond):
            for line in open("x").read().splitlines():
                if cond:
                    s = line.strip()
                else:
                    s = line.lower()
                if s.startswith("#"):
                    continue
                assert s
        """
    ) == 0, "multiplicity alone is not evidence of a lookup"


def test_negative_i305_mutually_referring_names_terminate():
    # `a -> b -> a` would recur forever without the `seen` guard, and the guard
    # now has to survive every candidate binding being chased rather than one.
    # If this hangs, the cycle guard broke under the multiplicity change.
    assert flags(
        """
        def test_mutual():
            for line in open("x").read().splitlines():
                a = line
                b = a.strip()
                a = b.lower()
                if a.startswith("#"):
                    continue
                assert a
        """
    ) == 0, "mutually referring names must terminate and stay exempt"


def test_positive_i305_a_loop_carried_binding_below_the_guard_is_a_probe():
    """GitHub #305, the loop-carried case -- found by SELF-probing, not by a gate.

    A guard inside a loop is reached again after a binding written BELOW it, so
    a cutoff at the guard's own line saw only the empty string and exempted the
    site. `all_before(name, None)` drops the cutoff; the live tree did not move
    (413/413), so the strictness costs nothing this repo writes.

    Recorded as self-found deliberately: after six gate refusals the useful
    question stopped being "what did the gate say" and became "what has nobody
    probed yet", which is what the probe harness is for.
    """
    assert flags(
        """
        def test_loop(session):
            t = ""
            for w in session.titles():
                if t.startswith("P"):
                    continue
                t = session.find_window().title()
                assert w
        """
    ) == 1, "a binding below the guard still reaches it on the next iteration"


# GitHub #305, the EIGHTH correction -- the one that replaced approximation with
# analysis.
#
# Two line-ordered answers were tried for "which binding is in effect at this
# guard", and the p98 phase-3 claim-check gate falsified BOTH, one in each
# direction:
#
#   * the textually last binding -- UNSOUND. A conditional rebinding whose
#     innocuous branch was written second hid the lookup in the other branch.
#   * every binding in the function -- SOUND but OVER-STRICT. A name reused for
#     two unrelated purposes in one long test function reddened a guard with no
#     probe anywhere in its reach. In a push gate that is not cosmetic: #305
#     says neither baselining a false positive nor rewriting correct code around
#     a checker bug is an acceptable answer.
#
# No cutoff can separate those two, because they differ in STRUCTURE and not in
# line order: the loop-carried binding is BELOW its guard and reaches it, while
# the reused name's earlier binding is ABOVE its guard and does not. So
# `_AssignmentIndex.reaching()` computes the reaching set -- backwards through
# preceding siblings, outwards through enclosing blocks, stopping at the first
# definite kill, taking the back edge of every enclosing loop, and picking up a
# binding made by the statement that OWNS a block (`with ... as t:`), which
# stepping straight out had missed.
#
# A binding whose value cannot be named -- a `for` target, an `except ... as` --
# is `None` in the result and PERMITTED. That is the disclosed residual, kept
# visible rather than papered over.


def test_negative_i305_a_name_reused_for_two_purposes_stays_exempt():
    assert flags(
        """
        def test_reused_name(session):
            title = session.find_window().title()
            assert title
            LINES = ("# a comment", "real line")
            for line in LINES:
                title = line.strip()
                if title.startswith("#"):
                    continue
                assert title
        """
    ) == 0, (
        "GitHub #305: requiring EVERY binding in the function reddened a guard "
        "whose own reach contains no probe -- the lookup bound to the same name "
        "earlier is killed before the guard on every path."
    )


def test_negative_i305_the_reused_name_case_in_the_other_order():
    # Order must not matter. Before the fix it did, in both directions at once:
    # one ordering was a false positive and another a false green.
    assert flags(
        """
        def test_reused_name2(session):
            LINES = ("# a comment", "real line")
            for line in LINES:
                title = line.strip()
                if title.startswith("#"):
                    continue
                assert title
            title = session.find_window().title()
            assert title
        """
    ) == 0, "a binding after the guard, outside any shared loop, does not reach it"


def test_positive_i305_a_with_binding_of_a_lookup_is_a_probe():
    # The guard has no preceding sibling, so stepping out of the `with` body
    # without reading the `with`'s own items missed this entirely (measured).
    assert flags(
        """
        def test_with(session):
            with session.open_window() as t:
                if t.startswith("P"):
                    pytest.skip("x")
                assert t
        """
    ) == 1, "`with <lookup> as t` binds t from a lookup"


def test_negative_i305_a_with_binding_of_file_text_stays_exempt():
    assert flags(
        """
        def test_with_ok():
            with open("x") as fh:
                for line in fh.read().splitlines():
                    s = line.strip()
                    if s.startswith("#"):
                        continue
                    assert s
        """
    ) == 0, "the `with` arm must not redden the ordinary file-parsing shape"


def test_negative_i305_str_of_a_value_in_hand_stays_exempt():
    # Pre-existing false positive, flagged at HEAD too: `str(x)` renders what it
    # was given and cannot look anything up.
    assert flags(
        """
        def test_str(session, exc):
            if str(exc).startswith("P"):
                pytest.skip("x")
            assert session
        """
    ) == 0, "a value-preserving conversion of something in hand is in hand"


def test_positive_i305_str_of_a_lookup_is_still_a_probe():
    # THE CONTROL on that addition. The Call arm requires every argument to be
    # value-in-hand first, so the conversion cannot become a laundry route.
    assert flags(
        """
        def test_str_lookup(session):
            if str(session.find_window().title()).startswith("P"):
                pytest.skip("x")
            assert session
        """
    ) == 1, "a conversion must not launder a lookup passed into it"


# GitHub #305, the NINTH correction -- the fix had been wired into ONE of the
# two places that needed it.
#
# Corrections 6-8 built a reaching-definitions binding chase, and all of it went
# into the value-in-hand classifier, reached only when the guard's outer
# predicate is a `TEXT_PREDICATES`/`VALUE_PREDICATES` call. The GENERAL path --
# `_condition_provenance.visit()`, which decides whether a condition probes at
# all -- still chased one binding with `latest_before`. So for the CANONICAL
# WIN-45 shape, which has no predicate anywhere in it, every false green those
# three corrections closed was still open:
#
#     if cond:
#         w = session.find_window()
#     else:
#         w = None
#     if w is None:
#         pytest.skip("not reachable")       -> 0 findings
#
# Swap the branches and it flags. Truthiness, `!=`, and the try/except spelling
# measured the same. Every positive test for corrections 6-8 used
# `.startswith(...)` as the outer predicate -- the one path that DID route
# through the new code -- so the regression reopened without moving a single
# test from green to red. Found by the p98 phase-3 claim-check gate asking where
# else the mechanism was needed, instead of probing more spellings of the shape
# it had already broken four times.
#
# `visit()` now chases EVERY reaching binding. Visiting more bindings can only
# ADD calls to the provenance, never remove one, so the direction is toward
# reporting. It made 16 pre-existing true positives visible -- the
# `record_matrix_result(...); if status == "blocked": pytest.skip(...)` idiom the
# module docstring names -- which are tracked in GitHub #314 and baselined by
# APPEND (413 -> 429), no entry re-keyed.


def test_positive_i305_a_conditional_rebinding_with_no_predicate_at_all():
    assert flags(
        """
        def test_cond_isnone(session, cond):
            if cond:
                w = session.find_window()
            else:
                w = None
            if w is None:
                pytest.skip("not reachable")
            assert w
        """
    ) == 1, (
        "GitHub #305: the binding chase was wired into the value-in-hand "
        "classifier only, so the canonical `if w is None: skip` shape kept the "
        "old single-binding chase and stayed a false green."
    )


def test_positive_i305_the_same_shape_with_the_branches_swapped():
    assert flags(
        """
        def test_cond_isnone2(session, cond):
            if cond:
                w = None
            else:
                w = session.find_window()
            if w is None:
                pytest.skip("not reachable")
            assert w
        """
    ) == 1, "both orderings must agree here too"


def test_positive_i305_truthiness_is_the_same_hole():
    assert flags(
        """
        def test_truthy(session, cond):
            if cond:
                w = session.find_window()
            else:
                w = None
            if not w:
                pytest.skip("not reachable")
            assert w
        """
    ) == 1, "a truthiness guard is the same shape without `is None`"


def test_positive_i305_try_except_rebinding_with_no_predicate():
    assert flags(
        """
        def test_try_isnone(session):
            try:
                w = session.find_window()
            except Exception:
                w = None
            if w is None:
                pytest.skip("not reachable")
            assert w
        """
    ) == 1, "the try/except spelling of the same rebinding"


def test_positive_i305_an_inequality_on_a_rebound_status():
    # The `status == "blocked"` family -- the idiom behind all 14 of the UI
    # matrix sites in GitHub #314, in miniature.
    assert flags(
        """
        def test_ne(session, cond):
            if cond:
                status = session.read_status()
            else:
                status = "ok"
            if status != "ok":
                pytest.skip("not reachable")
            assert status
        """
    ) == 1, "a comparison against a rebound probe result is still a probe"


def test_negative_i305_an_environment_gate_survives_the_rebinding_chase():
    # THE CONTROL. Chasing every binding adds calls to the provenance, so the
    # risk is reddening a legitimate environment gate written with a fallback.
    assert flags(
        """
        def test_env_rebind(session, cond):
            if cond:
                mode = os.getenv("MODE")
            else:
                mode = None
            if mode is None:
                pytest.skip("no mode")
            assert session
        """
    ) == 0, "every call feeding the condition is still environmental"


def test_negative_i305_the_fixture_path_gate_still_survives():
    # The single most common legitimate gate in this tree. If this reddens, the
    # checker is useless whatever else it catches.
    assert flags(
        """
        FIXTURES = Path(__file__).parent / "fixtures"
        def test_fixture():
            fixture = FIXTURES / "test_single.pdf"
            if not fixture.exists():
                pytest.skip("fixture missing")
            assert fixture
        """
    ) == 0, "a filesystem-existence gate on a proven path is an environment gate"
