# -*- coding: utf-8 -*-
"""Self-tests for tools/gate/check_magnitude_guard.py (PERF-02 / F37).

pytest.ini sets `testpaths = tests`, so this file runs NOWHERE unless it is
named explicitly. It is named in the "Gate self-tests" step of
.github/workflows/fast-checks.yml, following the pattern F35 used for
tools/gate/test_f35_tranche_controls.py. If you move or rename this file,
that workflow step must move with it or the gate becomes decoration.
"""

from __future__ import annotations

import io
import json
import os
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(HERE))
SCANNER = os.path.join(HERE, "check_magnitude_guard.py")
BASELINE = os.path.join(HERE, "magnitude_guard_baseline.json")
POSITIVE = os.path.join(HERE, "fixtures", "magnitude_guard_positive.cpp")
NEGATIVE = os.path.join(HERE, "fixtures", "magnitude_guard_negative.cpp")

sys.path.insert(0, HERE)
import check_magnitude_guard as G  # noqa: E402


def run(*args):
    """Run the scanner as a subprocess and return (exit_code, stdout+stderr).

    Deliberately a subprocess and not main(): the exit CODE is the contract the
    CI step depends on, and a `cmd | tail` style pipeline silently reports the
    wrong one. Nothing here pipes.
    """
    proc = subprocess.run(
        [sys.executable, SCANNER] + list(args),
        cwd=REPO_ROOT, capture_output=True, text=True, encoding="utf-8",
        errors="replace")
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def suspects(src):
    return [m for m in G.scan_text("x.cpp", src) if m.classification == "suspect"]


# ---------------------------------------------------------------------------
# Criterion 3 -- the discriminating control
# ---------------------------------------------------------------------------

def test_positive_control_is_flagged():
    code, out = run("--repo-root", ".", "--paths", POSITIVE)
    assert code == 1, out
    assert "VIOLATION" in out
    assert out.count("VIOLATION") == 2, out


def test_negative_control_passes():
    code, out = run("--repo-root", ".", "--paths", NEGATIVE)
    assert code == 0, out
    assert "VIOLATION" not in out
    assert "suspect        : 0" in out, out


def test_controls_actually_differ_in_shape_not_just_verdict():
    """Both controls contain shape matches; only the verdict differs.

    Guards against a scanner that passes the negative control by failing to
    parse it at all -- which would make the control prove nothing.
    """
    _, pos = run("--repo-root", ".", "--paths", POSITIVE)
    _, neg = run("--repo-root", ".", "--paths", NEGATIVE)
    assert "shape matches    : 2" in pos, pos
    assert "shape matches    : 3" in neg, neg


# ---------------------------------------------------------------------------
# The two guards F27 actually fixed (d85ec1d), verbatim in their pre-fix form
# ---------------------------------------------------------------------------

F27_R3_24 = """
TEST_F(FidelityR3_24_EditSaveScale, FullRewriteSaveScalesRoughlyLinearly) {
    double small = timeSave(50);
    double big = timeSave(500);
    if (small > 0.02) {
        EXPECT_LT(big / small, 15.0) << "save must scale roughly linearly";
    }
}
"""

F27_PERF_STRESS = """
TEST_F(PerformanceStressTest, ThousandPageOpenScalingLinear) {
    double sec100 = timeOpen(path100);
    double sec1000 = timeOpen(path1000);
    if (sec100 > 0.05) {  // Only assert if 100p took measurable time
        EXPECT_LT(sec1000 / sec100, 15.0) << "open must scale roughly linearly";
    }
}
"""

F27_PERF_STRESS_FIXED = """
TEST_F(PerformanceStressTest, ThousandPageOpenScalingLinear) {
    double sec100 = timeOpen(path100);
    double sec1000 = timeOpen(path1000);
    EXPECT_LT(sec1000 / sec100, 15.0) << "open must scale roughly linearly";
    EXPECT_LT(sec1000, 10.0);
}
"""


@pytest.mark.parametrize("src,cond", [
    (F27_R3_24, "small > 0.02"),
    (F27_PERF_STRESS, "sec100 > 0.05"),
])
def test_the_two_f27_instances_are_detected(src, cond):
    found = suspects(src)
    assert [m.condition for m in found] == [cond]


def test_the_fixed_form_is_not_flagged():
    assert G.scan_text("x.cpp", F27_PERF_STRESS_FIXED) == []


def test_shape_not_phrasing():
    """The recorded criterion insists on shape because a phrasing grep would
    have missed both instances. Same shape, no shared wording at all."""
    src = """
    TEST(T, X) {
        if (nanosPerOp > 1500) {
            ASSERT_GE(throughput, kFloor);
        }
    }
    """
    assert len(suspects(src)) == 1
    for word in ("scal", "linear", "measurable", "time", "Only assert"):
        assert word not in src


# ---------------------------------------------------------------------------
# Stage 0 -- blanking. If this regresses the counts become fiction.
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("src", [
    '// if (elapsedMs > 100) { EXPECT_LT(a, b); }\nint x = 1;\n',
    '/* if (elapsedMs > 100) { EXPECT_LT(a, b); } */\nint x = 1;\n',
    'const char* s = "if (elapsedMs > 100) { EXPECT_LT(a, b); }";\n',
    "const char c = \'x\'; // if (t > 9) { EXPECT_EQ(a, b); }\n",
    'auto r = R"js(if (elapsedMs > 100) { EXPECT_LT(a, b); })js";\n',
])
def test_comments_and_literals_produce_no_matches(src):
    assert G.scan_text("x.cpp", src) == []


def test_blanking_preserves_line_numbers():
    src = "// pad\n" * 40 + "TEST(T,X){ if (ms > 100) { EXPECT_LT(a, b); } }\n"
    found = suspects(src)
    assert len(found) == 1
    assert found[0].line == 41


def test_shift_operators_are_not_comparisons():
    src = 'TEST(T,X){ if (flag) { EXPECT_EQ(a, b) << 1 << 2; } }\n'
    assert G.scan_text("x.cpp", src) == []


# ---------------------------------------------------------------------------
# Brace awareness
# ---------------------------------------------------------------------------

def test_unbraced_single_statement_is_captured():
    src = 'TEST(T,X){ if (ms > 100) EXPECT_LT(a, b); }\n'
    assert len(suspects(src)) == 1


def test_assertion_after_the_guarded_block_is_not_attributed_to_it():
    """The assertion is OUTSIDE the guard, so nothing is disabled and there is
    no match. A non-brace-aware scanner reports a false positive here."""
    src = """
    TEST(T, X) {
        if (ms > 100) { record(ms); }
        EXPECT_LT(a, b);
    }
    """
    assert G.scan_text("x.cpp", src) == []


def test_nested_braces_do_not_end_the_block_early():
    src = """
    TEST(T, X) {
        if (ms > 100) {
            for (int i = 0; i < 3; ++i) { tick(); }
            EXPECT_LT(a, b);
        }
    }
    """
    assert len(suspects(src)) == 1


# ---------------------------------------------------------------------------
# Stage 2 -- the availability/magnitude discrimination
# ---------------------------------------------------------------------------

def test_zero_threshold_is_benign():
    src = 'TEST(T,X){ if (baselineMB > 0) { EXPECT_LT(a, b); } }\n'
    m = G.scan_text("x.cpp", src)
    assert [x.classification for x in m] == ["benign"]


def test_both_paths_asserting_is_benign():
    src = ('TEST(T,X){ if (ratio >= 4.5) { EXPECT_TRUE(p); }'
           ' else { EXPECT_FALSE(p); } }\n')
    m = G.scan_text("x.cpp", src)
    assert [x.classification for x in m] == ["benign"]


def test_else_without_an_assertion_does_not_exempt():
    src = ('TEST(T,X){ if (ms > 100) { EXPECT_LT(a, b); }'
           ' else { logSkip(); } }\n')
    assert len(suspects(src)) == 1


# ---------------------------------------------------------------------------
# Baseline behaviour, including the core.autocrlf hazard
# ---------------------------------------------------------------------------

def test_baseline_keys_survive_crlf():
    """core.autocrlf=true rewrites tracked text on checkout. A baseline keyed
    on raw bytes passes in the session that wrote it and fails after any
    checkout -- the exact trap recorded in this project's memory."""
    lf = "TEST(T,X){\n  if (ms > 100) {\n    EXPECT_LT(a, b);\n  }\n}\n"
    crlf = lf.replace("\n", "\r\n")
    assert [m.key() for m in G.scan_text("x.cpp", lf)] == \
           [m.key() for m in G.scan_text("x.cpp", crlf)]


def test_baseline_keys_survive_line_number_drift():
    lf = "TEST(T,X){\n  if (ms > 100) {\n    EXPECT_LT(a, b);\n  }\n}\n"
    shifted = "// unrelated edit above\n" * 12 + lf
    a = G.scan_text("x.cpp", lf)[0]
    b = G.scan_text("x.cpp", shifted)[0]
    assert a.key() == b.key()
    assert a.line != b.line


def test_repeated_identical_conditions_get_distinct_keys():
    src = ("TEST(T,A){ if (ms > 100) { EXPECT_LT(a, b); } }\n"
           "TEST(T,B){ if (ms > 100) { EXPECT_LT(a, b); } }\n")
    keys = [m.key() for m in G.scan_text("x.cpp", src)]
    assert len(keys) == 2 and len(set(keys)) == 2


def test_stale_baseline_entry_fails(tmp_path):
    bad = tmp_path / "baseline.json"
    bad.write_text(json.dumps({"entries": [
        {"file": "tests/does_not_exist.cpp", "condition": "gone > 1",
         "ordinal": 0, "classification": "suspect-accepted", "reason": "x"}]}),
        encoding="utf-8")
    code, out = run("--repo-root", ".", "--paths", NEGATIVE,
                    "--baseline", str(bad))
    assert code == 1, out
    assert "STALE-BASELINE" in out


def test_stale_baseline_can_be_waived_explicitly(tmp_path):
    bad = tmp_path / "baseline.json"
    bad.write_text(json.dumps({"entries": [
        {"file": "tests/does_not_exist.cpp", "condition": "gone > 1",
         "ordinal": 0, "classification": "suspect-accepted", "reason": "x"}]}),
        encoding="utf-8")
    code, _ = run("--repo-root", ".", "--paths", NEGATIVE,
                  "--baseline", str(bad), "--allow-stale-baseline")
    assert code == 0


def test_malformed_baseline_exits_two(tmp_path):
    bad = tmp_path / "baseline.json"
    bad.write_text("{ not json", encoding="utf-8")
    code, _ = run("--repo-root", ".", "--paths", NEGATIVE,
                  "--baseline", str(bad))
    assert code == 2


# ---------------------------------------------------------------------------
# The live tree -- and the contamination trap
# ---------------------------------------------------------------------------

def test_live_tree_passes_against_the_committed_baseline():
    """Criterion 5: the exact command fast-checks.yml runs, exit 0."""
    code, out = run("--repo-root", ".", "--paths", "tests",
                    "--baseline", "tools/gate/magnitude_guard_baseline.json")
    assert code == 0, out


def test_default_scan_root_does_not_reach_the_planted_fixtures():
    """F36's implementer swept one of its own new files into a baseline via a
    loose glob. The planted controls contain deliberate magnitude guards; if
    the default root reached tools/, the live count would be contaminated and
    the gate would be self-satisfying."""
    code, out = run("--repo-root", ".", "--list")
    assert code == 0
    data = json.loads(out)
    offenders = [e["file"] for e in data["entries"]
                 if not e["file"].startswith("tests/")]
    assert offenders == [], offenders
    assert data["shape_matches"] > 0


def test_baseline_does_not_readmit_the_two_f27_fixed_guards():
    with io.open(BASELINE, encoding="utf-8") as fh:
        data = json.load(fh)
    conditions = {e["condition"] for e in data["entries"]}
    assert "small > 0.02" not in conditions
    assert "sec100 > 0.05" not in conditions
    assert len(data["_f27_fixed"]) == 2


def test_every_baseline_entry_carries_a_written_reason():
    with io.open(BASELINE, encoding="utf-8") as fh:
        data = json.load(fh)
    assert data["entries"]
    for e in data["entries"]:
        assert e.get("reason", "").strip(), e
        assert len(e["reason"]) > 40, e
        assert e["classification"] in ("benign", "suspect-accepted"), e


def test_baseline_covers_every_live_shape_match():
    """Not just the suspects: the baseline is the durable 'recorded as checked'
    artifact the PERF-02 row promised and a prose paragraph could not be."""
    code, out = run("--repo-root", ".", "--list")
    assert code == 0
    live = {"%s::%s::%d" % (e["file"], e["condition"], e["ordinal"])
            for e in json.loads(out)["entries"]}
    with io.open(BASELINE, encoding="utf-8") as fh:
        base = {"%s::%s::%d" % (e["file"], e["condition"], e["ordinal"])
                for e in json.load(fh)["entries"]}
    assert live == base


def test_reading_variants_are_reported_and_informational():
    """The F27 prose recorded a count without recording the reading that
    produced it. The scanner reports the narrower readings so the next reader
    can see the spread instead of re-deriving it -- and they never affect
    pass/fail."""
    code, out = run("--repo-root", ".", "--list")
    rv = json.loads(out)["reading_variants"]
    assert rv["as_written"] >= rv["relational_only"] >= rv["relational_and_simple"]
    assert rv["as_written"] >= rv["simple_condition_only"] >= rv["relational_and_simple"]


# A conditional-magnitude shape the scanner really matches. Shared by the
# pruning test and its control so the two cannot drift apart -- an earlier
# draft planted `ASSERT_GT(n, 0)`, which the scanner does not match at all,
# making the pruning test pass for the wrong reason. The control below is what
# caught that.
PROBE_SOURCE = "TEST(Probe, X) {\n  if (n > 0) {\n    EXPECT_EQ(a, b);\n  }\n}\n"


def test_pytest_basetemp_is_pruned_from_the_scan(tmp_path):
    """A standing control must not be a function of scratch state.

    the repo-root ``conftest.py`` roots each session's basetemp at
    ``tests/_tmp/s<pid>`` (GitHub #257; ``pytest.ini`` set a single shared
    ``tests/_tmp`` until then), and several self-tests in
    this directory snapshot the real tree into it. Before F46, ``iter_sources``
    pruned only ``__pycache__``, so those COPIES were scanned as if they were
    sources: the live entry set grew phantom
    ``tests/_tmp/<testname>/snap/tests/test_*.cpp`` rows and
    ``test_baseline_covers_every_live_shape_match`` reddened -- but only if
    pytest had happened to run recently. The guard's verdict depended on
    whether someone had run the suite, which is the opposite of a control.
    """
    scratch = os.path.join(REPO_ROOT, "tests", "_tmp",
                           "magnitude_prune_probe", "snap", "tests")
    os.makedirs(scratch, exist_ok=True)
    planted = os.path.join(scratch, "test_planted_magnitude_probe.cpp")
    try:
        with io.open(planted, "w", encoding="utf-8") as fh:
            # A shape the scanner definitely matches (asserted by the control
            # test below), so a failure here means the pruning broke -- not
            # that the probe was too subtle to be seen.
            fh.write(PROBE_SOURCE)
        code, out = run("--repo-root", ".", "--list")
        assert code == 0
        files = {e["file"] for e in json.loads(out)["entries"]}
        assert not any("_tmp" in f for f in files), \
            "pytest's --basetemp leaked into the scan: %s" % (
                sorted(f for f in files if "_tmp" in f),)
    finally:
        if os.path.exists(planted):
            os.remove(planted)


def test_the_prune_probe_would_catch_a_regression():
    """Control for the test above: prove the planted shape IS matchable.

    Without this, a scanner that silently stopped matching anything would make
    the pruning test pass vacuously.
    """
    entries = G.scan_text("tests/test_probe_control.cpp", PROBE_SOURCE)
    assert entries, ("the planted shape must be matchable, else the pruning "
                     "test above passes vacuously")


# ---------------------------------------------------------------------------
# GitHub #274 -- GTEST_SKIP is a disabled check too
#
# The scanner keyed on conditional ASSERTIONS, so a threshold gating a
# GTEST_SKIP was structurally invisible. Same defect, different door, and under
# SKIP=FAIL the worse of the two: it announces "not run" and is then counted as
# a non-failure by the gtest summary, the merged XML and the skip baseline.
# ---------------------------------------------------------------------------

SKIP_MAGNITUDE = """
TEST(F, MagnitudeGatedSkip) {
    const std::string user = currentUserName();
    if (user.size() < 3) {
        GTEST_SKIP() << "the build account name is unavailable or too short";
    }
    EXPECT_NE(scrub(user).find(user), std::string::npos);
}
"""

SKIP_AVAILABILITY_NO_LITERAL = """
TEST(F, ExistenceGatedSkip) {
    if (!std::filesystem::exists(fixture)) {
        GTEST_SKIP() << "fixture not available";
    }
    EXPECT_TRUE(open(fixture));
}
"""

SKIP_ZERO_THRESHOLD = """
TEST(F, ZeroThresholdSkip) {
    if (pageCount == 0) {
        GTEST_SKIP() << "no pages to measure";
    }
    EXPECT_GT(render(0), 0);
}
"""


def test_a_magnitude_threshold_gating_a_skip_is_suspect():
    """THE #274 CASE. Before the fix this returned [] -- no assertion in the
    guarded block, so the shape never matched and the sweep said nothing."""
    found = suspects(SKIP_MAGNITUDE)
    assert len(found) == 1, found
    assert "GTEST_SKIP" in found[0].assert_snippet, found[0].assert_snippet
    assert "GTEST_SKIP" in found[0].reason, found[0].reason


def test_an_existence_gated_skip_is_not_matched_at_all():
    """The 635 sites #274's census counted are `!exists(fixture)` guards. They
    carry no numeric literal, so stage 1 never reaches them -- which is why
    making the scanner skip-aware does not drown it."""
    assert G.scan_text("x.cpp", SKIP_AVAILABILITY_NO_LITERAL) == []


def test_a_zero_threshold_skip_stays_benign():
    """Availability, not magnitude: `== 0` cannot become permanently false as
    hardware gets faster. The existing exemption must still apply to skips."""
    assert suspects(SKIP_ZERO_THRESHOLD) == []
    matches = G.scan_text("x.cpp", SKIP_ZERO_THRESHOLD)
    assert len(matches) == 1 and matches[0].classification == "benign", matches


def test_both_paths_cannot_exempt_a_skip():
    """The BOTH-PATHS exemption means "no path is silent". A skip IS the silent
    path -- it abandons the whole test body, not just its branch -- so an else
    that asserts must NOT rescue it. Without this the fix would have a hole
    exactly where a careless author would put one."""
    src = """
TEST(F, SkipWithAssertingElse) {
    if (names.size() < 2) {
        GTEST_SKIP() << "need two";
    } else {
        EXPECT_EQ(names.size(), 2u);
    }
}
"""
    found = suspects(src)
    assert len(found) == 1, ("an else-branch assertion must not exempt a "
                             "skip-gated guard: %s" % (found,))


def test_the_skip_detection_is_not_vacuous():
    """Guard the guard: prove the three sources above differ in VERDICT, not
    merely in text. A scanner that flagged everything, or nothing, would make
    the three tests above pass or fail together and say nothing."""
    assert len(suspects(SKIP_MAGNITUDE)) == 1
    assert suspects(SKIP_ZERO_THRESHOLD) == []
    assert G.scan_text("x.cpp", SKIP_AVAILABILITY_NO_LITERAL) == []


def test_the_scanner_can_print_a_non_ascii_finding():
    """It could not, and died doing it. A suspect condition whose snippet
    contained U+2265 raised UnicodeEncodeError from cp1252 on the VIOLATION
    print, so the sweep crashed at the moment it had something to report and the
    non-zero exit came from the traceback rather than the finding. Found
    2026-10-07 while landing #274, on a real test
    (`GTEST_SKIP() << "need >=2 fields to reorder"` with the sign as U+2265)."""
    src = (
        "TEST(F, NonAsciiMessage) {\n"
        "    if (names.size() < 2) {\n"
        "        GTEST_SKIP() << \"need \u2265 2 fields\";\n"
        "    }\n"
        "    EXPECT_TRUE(ok());\n"
        "}\n"
    ).encode("utf-8").decode("unicode_escape")
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        planted = os.path.join(td, "test_non_ascii_probe.cpp")
        io.open(planted, "w", encoding="utf-8").write(src)
        code, out = run("--repo-root", ".", "--paths", planted)
    assert "UnicodeEncodeError" not in out, out
    assert "Traceback" not in out, out
    assert code == 1, out
    assert "VIOLATION" in out, out
