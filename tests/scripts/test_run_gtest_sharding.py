# -*- coding: utf-8 -*-
"""Unit tests for the PURE sharding/merge logic in run_gtest.py.

These tests touch NO pdfe_tests process and NO real JUnit output — they feed
synthetic test-id lists and synthetic JUnit XML strings into the pure helpers
and assert the invariants the Windows harness depends on:

  * partition_tests   -> deterministic, disjoint, total, balanced,
  * parse_gtest_list  -> correct id extraction (incl. value-parameterized),
  * aggregate_shards  -> GAP-A15 totals + the crashed-shard -> RED invariant.

Run (safe — does not launch pdfe_tests):
    python -m pytest tests/scripts/test_run_gtest_sharding.py -q
"""
from __future__ import annotations

import os
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
REPO_ROOT = str(Path(__file__).resolve().parents[2])
from run_gtest import (  # noqa: E402
    aggregate_shards,
    parse_gtest_list,
    partition_tests,
    shard_filter,
    _failed_ids_from_root,
    _is_crashed,
)


# --------------------------------------------------------------------------- #
# partition_tests
# --------------------------------------------------------------------------- #


def _ids(n: int) -> list[str]:
    return [f"Suite{i // 10}.Test{i}" for i in range(n)]


def test_partition_is_disjoint_and_total():
    ids = _ids(100)
    buckets = partition_tests(ids, 7)
    flat = [t for b in buckets for t in b]
    assert sorted(flat) == sorted(ids), "every id must appear exactly once"
    assert len(flat) == len(set(flat)), "no id may appear in two shards"
    assert len(flat) == len(ids), "no id may be dropped or duplicated"


def test_partition_is_deterministic():
    ids = _ids(53)
    assert partition_tests(ids, 8) == partition_tests(ids, 8)


def test_partition_is_balanced():
    ids = _ids(100)
    sizes = [len(b) for b in partition_tests(ids, 7)]
    assert max(sizes) - min(sizes) <= 1, f"unbalanced shard sizes: {sizes}"


def test_partition_preserves_within_shard_order():
    ids = _ids(30)
    buckets = partition_tests(ids, 5)
    # Round-robin: shard 0 gets ids 0,5,10,... in that order.
    assert buckets[0] == [ids[i] for i in range(0, 30, 5)]


def test_partition_more_shards_than_tests_yields_empty_shards():
    ids = _ids(3)
    buckets = partition_tests(ids, 6)
    assert len(buckets) == 6
    assert sum(len(b) for b in buckets) == 3
    assert buckets[3] == [] and buckets[4] == [] and buckets[5] == []


def test_partition_single_shard_is_identity():
    ids = _ids(12)
    assert partition_tests(ids, 1) == [ids]


def test_partition_rejects_zero_shards():
    try:
        partition_tests(_ids(4), 0)
    except ValueError:
        return
    raise AssertionError("partition_tests must reject shards < 1")


def test_shard_filter_joins_with_colon():
    assert shard_filter(["A.b", "C.d"]) == "A.b:C.d"


# --------------------------------------------------------------------------- #
# parse_gtest_list
# --------------------------------------------------------------------------- #

_LIST_OUTPUT = """\
Running main() from gmock_main.cc
FooTest.
  Bar
  Baz
Params/QuuxTest.
  Case/0  # GetParam() = 8-byte object <00-00 ...>
  Case/1  # GetParam() = 8-byte object <01-00 ...>
"""


def test_parse_gtest_list_basic_and_parameterized():
    ids = parse_gtest_list(_LIST_OUTPUT)
    assert ids == [
        "FooTest.Bar",
        "FooTest.Baz",
        "Params/QuuxTest.Case/0",
        "Params/QuuxTest.Case/1",
    ], ids


def test_parse_gtest_list_ignores_blank_and_banner():
    ids = parse_gtest_list("\n\nSuite.\n  A\n\n")
    assert ids == ["Suite.A"]


def test_parse_gtest_list_accepts_plain_id_list():
    # A cached "one full id per line" file must also parse.
    ids = parse_gtest_list("Alpha.One\nBeta.Two\n")
    assert ids == ["Alpha.One", "Beta.Two"]


# --------------------------------------------------------------------------- #
# aggregate_shards  +  crashed-shard -> RED invariant
# --------------------------------------------------------------------------- #


def _passing_xml(cases: list[tuple[str, str]]) -> ET.Element:
    """Build a gtest-style root where each (suite, name) is a passing case."""
    root = ET.Element("testsuites")
    ts = ET.SubElement(root, "testsuite", {"name": "S"})
    for suite, name in cases:
        ET.SubElement(ts, "testcase", {"classname": suite, "name": name,
                                       "status": "run"})
    return root


def _mixed_xml() -> ET.Element:
    root = ET.Element("testsuites")
    ts = ET.SubElement(root, "testsuite", {"name": "M"})
    ok = ET.SubElement(ts, "testcase", {"classname": "M", "name": "ok",
                                        "status": "run"})
    _ = ok
    bad = ET.SubElement(ts, "testcase", {"classname": "M", "name": "bad",
                                         "status": "run"})
    ET.SubElement(bad, "failure", {"message": "boom"})
    sk = ET.SubElement(ts, "testcase", {"classname": "M", "name": "skip",
                                        "status": "run"})
    ET.SubElement(sk, "skipped", {"message": "skipped"})
    return root


def test_aggregate_totals_are_gap_a15_compliant():
    shard0 = (["A.x", "A.y"], _passing_xml([("A", "x"), ("A", "y")]))
    shard1 = (["M.ok", "M.bad", "M.skip"], _mixed_xml())
    root, totals, per_shard = aggregate_shards([shard0, shard1])
    passed = (totals["tests"] - totals["failures"]
              - totals["errors"] - totals["skipped"])
    assert totals["tests"] == 5
    assert totals["failures"] == 1
    assert totals["skipped"] == 1
    assert totals["errors"] == 0
    assert passed == 3
    # Root attributes must match rolled-up totals (single-source for GAP-A15).
    assert int(root.get("tests")) == totals["tests"]
    assert int(root.get("failures")) == totals["failures"]
    assert per_shard[0]["crashed"] is False


def test_crashed_shard_none_root_becomes_all_errors_red():
    """A shard with NO xml (None root) must surface EVERY assigned test as an
    error — never silently dropped, and the aggregate must be RED."""
    good = (["A.x"], _passing_xml([("A", "x")]))
    crashed = (["Q.a", "Q.b", "Q.c"], None)
    root, totals, per_shard = aggregate_shards([good, crashed])
    assert totals["tests"] == 4, "crashed shard's assigned tests must be counted"
    assert totals["errors"] == 3, "every crashed-shard test must be an error"
    assert per_shard[1]["crashed"] is True
    # RED: there are error testcases in the aggregate.
    failed = _failed_ids_from_root(root)
    assert set(failed) == {"Q.a", "Q.b", "Q.c"}


def test_crashed_shard_empty_root_also_red():
    """An XML that parsed but contains zero testcases counts as crashed."""
    empty = ET.Element("testsuites")  # parseable but no <testcase>
    assert _is_crashed(empty) is True
    root, totals, per_shard = aggregate_shards([(["Z.a", "Z.b"], empty)])
    assert totals["errors"] == 2
    assert per_shard[0]["crashed"] is True


def test_no_shard_is_vacuously_green_when_one_crashes():
    """The whole point: even if every OTHER shard is green, one crashed shard
    keeps the run RED."""
    s0 = (["A.a"], _passing_xml([("A", "a")]))
    s1 = (["B.b"], _passing_xml([("B", "b")]))
    s2 = (["C.c", "C.d"], None)  # crashed
    root, totals, _ = aggregate_shards([s0, s1, s2])
    assert totals["errors"] == 2
    assert _failed_ids_from_root(root), "run must NOT be green with a crashed shard"


def test_all_green_shards_produce_no_failures():
    s0 = (["A.a"], _passing_xml([("A", "a")]))
    s1 = (["B.b", "B.c"], _passing_xml([("B", "b"), ("B", "c")]))
    root, totals, _ = aggregate_shards([s0, s1])
    assert totals["failures"] == 0 and totals["errors"] == 0
    assert _failed_ids_from_root(root) == []
    assert totals["tests"] == 3


def test_scratch_directories_are_not_treated_as_compilable_sources(tmp_path):
    r"""`tests/_tmp/` is throwaway, not an input to any binary.

    WHY THIS EXISTS. `tools/gate/test_rebuild_assert.py` writes trees containing
    files literally named `new.cpp` into pytest's tmp area, and this repo's
    pytest tmp area is `tests/_tmp/` — which sits under one of the directories
    `_newest_source` sweeps. So running the gate self-tests and then the suite
    made the staleness preflight refuse to run at all:

        [gtest] STALE test binary: ...\pdfe_tests.exe
        [gtest]   is 0.8h older than ...\tests\_tmp\...\new.cpp

    Nothing was stale. A gitignored scratch file was counted as a compilable
    input and the guard blocked a legitimate full-suite run — the "over-broad
    guard gets switched off" failure `_newest_source`'s own docstring warns
    about, arriving in a second form.
    """
    import os

    import run_gtest as rg

    root = tmp_path
    (root / "src").mkdir()
    real = root / "src" / "real.cpp"
    real.write_text("int main(){}\n", encoding="utf-8")

    scratch = root / "tests" / "_tmp" / "case0"
    scratch.mkdir(parents=True)
    (scratch / "new.cpp").write_text("int newer(){}\n", encoding="utf-8")

    # Make the real source unambiguously OLDER, so the scratch file wins on
    # mtime unless it is excluded.
    st = real.stat()
    os.utime(real, (st.st_atime, st.st_mtime - 10_000))

    _t, newest_p = rg._newest_source(root, dirs=("src", "tests"))

    assert newest_p is not None, "the sweep found nothing at all"
    assert newest_p.name == "real.cpp", (
        f"scratch file {newest_p} was treated as a source; the preflight would "
        f"refuse to run against a perfectly current binary")


def test_the_python_and_cpp_scratch_exclusions_do_not_drift():
    """The staleness rule has TWO implementations. They must agree.

    THIS IS THE CLASS-LEVEL TEST for DCA 0052. The instance was: pytest scratch
    under `tests/_tmp/` was counted as compilable source. The *class* is that
    this repository implements the staleness sweep twice —

        tests/scripts/run_gtest.py::_newest_source                (Python)
        tests/test_binary_staleness_guard.cpp::newestSourceIn     (C++)

    — and fixing one is not fixing the rule. That is precisely what happened:
    the Python side was fixed first, the full suite then failed with the C++
    mirror *throwing* on the same scratch tree, and only that run revealed it.

    An exclusion added to one and forgotten in the other reproduces the whole
    defect, so the two lists are compared directly rather than trusted to be
    maintained in step.
    """
    import re
    from pathlib import Path

    import run_gtest as rg

    cpp = (Path(__file__).resolve().parents[1] /
           "test_binary_staleness_guard.cpp").read_text(encoding="utf-8",
                                                        errors="replace")
    m = re.search(r"kScratch\[\]\s*=\s*\{([^}]*)\}", cpp)
    assert m, ("could not find kScratch[] in test_binary_staleness_guard.cpp — "
               "either it was renamed or the C++ exclusion was removed, and "
               "this comparison must not pass vacuously")

    cpp_set = set(re.findall(r'"([^"]+)"', m.group(1)))
    py_set = set(rg._SCRATCH_DIRS)

    assert cpp_set, "the C++ scratch list parsed as empty"
    assert py_set, "the Python scratch list is empty"
    assert cpp_set == py_set, (
        f"the two staleness sweeps disagree about what is scratch.\n"
        f"  Python only: {sorted(py_set - cpp_set)}\n"
        f"  C++ only   : {sorted(cpp_set - py_set)}\n"
        f"Fixing one implementation is not fixing the rule (DCA 0052).")


# ---------------------------------------------------------------------------
# GitHub #245 -- a fresh executable and a stale object read the same.
#
# `report_stale_binaries` compares the EXE's mtime against its sources, and
# MSBuild relinks whenever anything in the link line changes. So a relink that
# carried a stale object dates the exe freshly, the preflight says OK, and the
# suite runs green about code that is not in the tree. Objects are the level
# where a correct rebuild and a stale-carrying relink actually differ.
# ---------------------------------------------------------------------------
def test_a_fresh_object_is_not_reported_stale():
    import run_gtest as rg
    assert rg.stale_object_pairs(
        [("x/Foo.obj", 200.0)], {"foo": ("src/Foo.cpp", 100.0)}) == []


def test_an_object_older_than_its_source_is_reported():
    import run_gtest as rg
    out = rg.stale_object_pairs(
        [("x/Foo.obj", 100.0)], {"foo": ("src/Foo.cpp", 200.0)})
    assert len(out) == 1
    obj, src, age = out[0]
    assert obj.endswith("Foo.obj") and src.endswith("Foo.cpp")
    assert age == 100.0


def test_an_object_with_no_matching_source_is_ignored():
    """Third-party and generated objects must not accuse anyone."""
    import run_gtest as rg
    assert rg.stale_object_pairs(
        [("x/Vendor.obj", 1.0)], {"foo": ("src/Foo.cpp", 200.0)}) == []


def test_matching_is_case_insensitive_like_the_filesystem():
    import run_gtest as rg
    out = rg.stale_object_pairs(
        [("x/FOO.obj", 100.0)], {"foo": ("src/Foo.cpp", 200.0)})
    assert len(out) == 1


def test_the_newest_duplicate_stem_wins(tmp_path, monkeypatch):
    """A stale object must not be excused by an older same-named source.

    Two files called `Widget.cpp` in different directories is ordinary; taking
    whichever the walk happened to reach first would let the older one declare
    the object fresh.
    """
    import os
    import run_gtest as rg
    # pytest's tmp_path lives under tests/_tmp (pytest.ini --basetemp), and
    # `_tmp` is in _SCRATCH_DIRS, so the sweep would skip this whole tree --
    # the very trap _newest_source's docstring records. Disable the exclusion
    # for this case rather than staging sources outside the tmp area.
    monkeypatch.setattr(rg, "_SCRATCH_DIRS", frozenset())
    a = tmp_path / "src" / "a"
    b = tmp_path / "src" / "b"
    a.mkdir(parents=True)
    b.mkdir(parents=True)
    old, new = a / "Widget.cpp", b / "Widget.cpp"
    old.write_text("// old\n", encoding="utf-8")
    new.write_text("// new\n", encoding="utf-8")
    os.utime(old, (1000, 1000))
    os.utime(new, (9000, 9000))

    index = rg._source_index(str(tmp_path), ("src",))
    assert index["widget"][1] == 9000, "the older duplicate won"

    # An object built at t=5000 is stale against the NEWER source.
    assert rg.stale_object_pairs([("o/Widget.obj", 5000.0)], index)


def test_cannot_measure_is_not_a_pass(tmp_path):
    """No objects found must not read as 'nothing is stale'."""
    import contextlib
    import io as _io
    import run_gtest as rg
    # capsys is unavailable: pytest.ini sets `-p no:capture`.
    exe = tmp_path / "bin" / "Debug" / "pdfe_tests.exe"
    exe.parent.mkdir(parents=True)
    exe.write_bytes(b"x")
    buf = _io.StringIO()
    with contextlib.redirect_stdout(buf):
        result = rg.report_stale_objects(str(exe))
    assert result is False
    assert "cannot establish freshness" in buf.getvalue(), buf.getvalue()


# ---------------------------------------------------------------------------
# GitHub #247 -- --shards 8 died with a bare WinError 206.
#
# Each shard is launched with an explicit --gtest_filter naming every test it
# owns, and Windows caps a command line at 32,767 characters. Fewer shards means
# more names per shard, so below a certain count the launch fails before any
# test runs, reporting a filename-too-long error that names nothing useful.
# ---------------------------------------------------------------------------
def _long_ids(n: int, width: int = 60) -> list:
    """Test ids of realistic length (this tree averages ~45 characters)."""
    return ["SomeReasonablyLongSuiteName%04d.AndAnEquallyLongCaseName%04d"
            % (i // 40, i) for i in range(n)][:n] or []


def test_a_shard_count_that_fits_is_accepted():
    import run_gtest as rg
    ok, worst, _min_viable = rg.shard_filter_budget(_long_ids(6400), 16)
    assert ok, worst


def test_a_shard_count_that_overflows_the_command_line_is_refused():
    import run_gtest as rg
    ok, worst, min_viable = rg.shard_filter_budget(_long_ids(6400), 2)
    assert not ok, "a 2-shard split of 6,400 long ids cannot fit in 32,767 chars"
    assert worst > rg.WINDOWS_CMDLINE_LIMIT
    assert min_viable > 2


def test_the_refusal_names_a_shard_count_that_actually_works():
    """The remedy it prints must itself pass the budget.

    A guard that refuses and then suggests something equally impossible has
    moved the failure, not explained it.
    """
    import run_gtest as rg
    ids = _long_ids(6400)
    ok, _worst, min_viable = rg.shard_filter_budget(ids, 2)
    assert not ok
    ok2, worst2, _ = rg.shard_filter_budget(ids, min_viable)
    assert ok2, (min_viable, worst2)
    # ...and it must be the SMALLEST such count, or the advice is wasteful.
    if min_viable > 1:
        ok3, _w, _m = rg.shard_filter_budget(ids, min_viable - 1)
        assert not ok3, "a smaller shard count would also have fitted"


def test_the_budget_leaves_room_for_the_rest_of_the_argv():
    """A filter that exactly equals the limit still cannot launch: the exe
    path, --gtest_output and the rest of argv share that budget."""
    import run_gtest as rg
    assert rg._SHARD_ARGV_OVERHEAD > 0
    ids = _long_ids(400)
    ok, worst, _ = rg.shard_filter_budget(
        ids, 1, limit=len(rg.shard_filter(ids)) + 1)
    assert not ok, "a filter one char under the raw limit was accepted"


# ---------------------------------------------------------------------------
# GitHub #247, second pass -- the filter moved OFF the command line.
#
# Shards used to be launched with an explicit --gtest_filter naming every test,
# which Windows caps at 32,767 characters, so --shards 8 (43,232 characters) was
# refused up front. The filter now goes in a --gtest_flagfile, whose size is
# bounded by the filesystem rather than by CreateProcess.
#
# The dangerous failure is SILENT: a googletest that ignores --gtest_flagfile
# applies no filter, so every shard runs the WHOLE suite and the aggregate
# counts each test once per shard -- a slow, plausible, entirely wrong run.
# flagfile_is_honoured() measures support before anything launches.
# ---------------------------------------------------------------------------
def test_the_flagfile_contains_the_filter_and_nothing_else(tmp_path):
    import run_gtest as rg
    p = rg.write_flagfile(tmp_path / "f.txt", "A.b:C.d")
    assert p.read_text(encoding="utf-8") == "--gtest_filter=A.b:C.d\n"


def test_the_flagfile_is_written_with_lf_regardless_of_autocrlf(tmp_path):
    """core.autocrlf is true here; a CRLF in a flagfile is a different file."""
    import run_gtest as rg
    p = rg.write_flagfile(tmp_path / "f.txt", "A.b")
    assert b"\r\n" not in p.read_bytes()


def test_the_flagfile_holds_a_filter_too_long_for_the_command_line(tmp_path):
    """The whole point: a filter argv cannot carry."""
    import run_gtest as rg
    ids = ["SomeReasonablyLongSuiteName%04d.AndAnEquallyLongCaseName%04d"
           % (i // 40, i) for i in range(900)]
    flt = rg.shard_filter(ids)
    assert len(flt) > rg.WINDOWS_CMDLINE_LIMIT, (
        "fixture too small to prove anything; it must exceed %d chars"
        % rg.WINDOWS_CMDLINE_LIMIT)
    p = rg.write_flagfile(tmp_path / "big.txt", flt)
    assert p.read_text(encoding="utf-8").startswith("--gtest_filter=")
    assert len(p.read_text(encoding="utf-8")) > rg.WINDOWS_CMDLINE_LIMIT


def test_a_binary_that_ignores_the_flagfile_is_caught(tmp_path, monkeypatch):
    """The silent-failure guard. An ignored flagfile lists the whole suite."""
    import run_gtest as rg

    monkeypatch.setattr(rg, "_list_tests", lambda *a, **k: ["A.b", "C.d", "E.f"])

    class _Proc:
        # what a binary that IGNORES the flag prints: everything
        stdout = "A.\n  b\nC.\n  d\nE.\n  f\n"

    monkeypatch.setattr(rg.subprocess, "run", lambda *a, **k: _Proc())
    ok, detail = rg.flagfile_is_honoured("exe", tmp_path)
    assert not ok
    assert "does not apply --gtest_flagfile" in detail, detail


def test_a_binary_that_honours_the_flagfile_is_accepted(tmp_path, monkeypatch):
    """The control: the probe must not reject a working binary."""
    import run_gtest as rg

    monkeypatch.setattr(rg, "_list_tests", lambda *a, **k: ["A.b", "C.d"])

    class _Proc:
        stdout = "A.\n  b\n"          # exactly the one probed test

    monkeypatch.setattr(rg.subprocess, "run", lambda *a, **k: _Proc())
    ok, detail = rg.flagfile_is_honoured("exe", tmp_path)
    assert ok, detail


def test_the_probe_refuses_when_it_cannot_enumerate(tmp_path, monkeypatch):
    """'Could not measure' must not read as 'supported'."""
    import run_gtest as rg
    monkeypatch.setattr(rg, "_list_tests", lambda *a, **k: [])
    ok, detail = rg.flagfile_is_honoured("exe", tmp_path)
    assert not ok
    assert "could not enumerate" in detail


# ---------------------------------------------------------------------------
# GitHub #264 -- a crash must not be healable by a rerun.
#
# `_rerun_failures` re-runs a failed test ALONE and treats passing as proof of
# flakiness. Sound for order-independent flakiness; exactly inverted for an
# order-dependent crash, because isolation is what makes both disappear. A
# --shards 8 run healed two access violations this way and printed CLEAN.
# "Passes when run alone" is that defect's SIGNATURE, not its refutation.
# ---------------------------------------------------------------------------
_CRASH_XML = """<?xml version="1.0"?>
<testsuites tests="2" failures="2">
  <testsuite name="S">
    <testcase classname="S" name="crashed">
      <failure message="SEH exception with code 0xc0000005 thrown in the test body."/>
    </testcase>
    <testcase classname="S" name="asserted">
      <failure message="Expected equality of these values: 1 and 2"/>
    </testcase>
  </testsuite>
</testsuites>
"""


def _root(xml: str):
    import xml.etree.ElementTree as ET
    return ET.fromstring(xml)


def test_an_seh_failure_is_classified_as_a_crash():
    import run_gtest as rg
    assert rg.crashed_test_ids(_root(_CRASH_XML)) == {"S.crashed"}


def test_an_assertion_failure_is_not_classified_as_a_crash():
    """The discriminator. Without it the rule would make everything unhealable
    and the flake mitigation this harness needs would be gone."""
    import run_gtest as rg
    assert "S.asserted" not in rg.crashed_test_ids(_root(_CRASH_XML))


@pytest.mark.parametrize("msg", [
    "SEH exception with code 0xc0000005 thrown in the test body.",
    "SEH exception with code 0xC0000374 thrown in the test body.",
    "Access violation reading location 0x0",
    "Segmentation fault",
    "terminated by signal 11",
    "signal SIGSEGV",
])
def test_every_fault_wording_is_caught(msg):
    import run_gtest as rg
    xml = ('<testsuites><testsuite name="S"><testcase classname="S" name="t">'
           '<failure message="%s"/></testcase></testsuite></testsuites>' % msg)
    assert rg.crashed_test_ids(_root(xml)) == {"S.t"}


def test_the_crash_text_is_also_read_from_element_body():
    """gtest puts the message in an attribute; other writers use the body."""
    import run_gtest as rg
    xml = ('<testsuites><testsuite name="S"><testcase classname="S" name="t">'
           '<failure>SEH exception with code 0xc0000005 thrown in the test '
           'body.</failure></testcase></testsuite></testsuites>')
    assert rg.crashed_test_ids(_root(xml)) == {"S.t"}


def test_a_crash_that_passes_alone_is_not_healed(tmp_path, monkeypatch):
    """The regression, end to end through _rerun_failures."""
    import run_gtest as rg

    monkeypatch.setattr(rg, "_run", lambda *a, **k: 0)
    monkeypatch.setattr(rg, "_failed_testcases", lambda p: [])      # passes alone
    monkeypatch.setattr(rg, "_totals", lambda p: {"tests": 1, "failures": 0,
                                                  "errors": 0, "disabled": 0,
                                                  "skipped": 0})

    healed, unresolved = rg._rerun_failures(
        "exe", tmp_path, ["S.crashed"], reruns=2, unhealable={"S.crashed"})
    assert healed == [], "a process fault was healed by a rerun"
    assert unresolved == ["S.crashed"]


def test_a_genuine_flake_that_passes_alone_is_still_healed(tmp_path, monkeypatch):
    """The control: the new rule must not disable flake mitigation."""
    import run_gtest as rg

    monkeypatch.setattr(rg, "_run", lambda *a, **k: 0)
    monkeypatch.setattr(rg, "_failed_testcases", lambda p: [])
    monkeypatch.setattr(rg, "_totals", lambda p: {"tests": 1, "failures": 0,
                                                  "errors": 0, "disabled": 0,
                                                  "skipped": 0})

    healed, unresolved = rg._rerun_failures(
        "exe", tmp_path, ["S.flaky"], reruns=2, unhealable=set())
    assert healed == ["S.flaky"]
    assert unresolved == []


def test_the_live_shards8_artifact_would_now_be_caught():
    """Against the REAL XML that was healed and reported CLEAN.

    This is the artifact from the run that motivated #264, kept as the fixture
    so the rule is pinned to the thing it failed on rather than to a mock.
    """
    import os
    import run_gtest as rg
    import xml.etree.ElementTree as ET
    p = os.path.join(REPO_ROOT, "tests", "_runtime", "shard8",
                     "gtest_shard_5.xml")
    if not os.path.exists(p):
        pytest.skip("shard-5 artifact not present in this checkout")
    ids = rg.crashed_test_ids(ET.parse(p).getroot())
    assert "UiClipboardDragDropTest.ARealDropStillWorksAfterAVirtualFileDrop" in ids
    assert "MediaBoxOriginCrop.BlueSelectionCropsBlueNotRed" in ids
