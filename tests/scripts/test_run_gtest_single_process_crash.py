# -*- coding: utf-8 -*-
"""GitHub #288 — the DEFAULT run mode must not report a crash as CLEAN.

WHY THIS FILE EXISTS, AND WHAT ALREADY COVERED THE OTHER PATHS
==============================================================
`run_gtest.py` has three crash defences, and until now all three lived on the
SHARDED path:

  1. a shard that crashes without a parseable XML synthesises `shard-crash`
     error records, so its tests can never be silently dropped;
  2. `_is_abnormal_exit(rc)` catches the shard that runs every test, writes a
     COMPLETE and GREEN report, and then dies in teardown -- GAP-A15's sibling,
     a NON-zero exit code hidden BY the XML rather than a zero exit code hiding
     failures IN it;
  3. never-run ids are kept out of `healed` and stay in `unresolved` (F70b).

`run_single_process()` had none of them. It discarded `_run()`'s return code and
decided everything from the XML -- and it is the DEFAULT, because `--shards`
defaults to 1. So on the path most runs take, both crash shapes printed
"CLEAN on first run" and returned 0.

That is #288's literal complaint, and it survived three rounds of "already
addressed" review -- mine, and a karen gate's -- because all three of us checked
the sharded path. The `validator` gate found it and FAILED the phase. These tests
exist so the next reader does not have to rediscover it.

WHAT EACH TEST PINS
===================
The two crash shapes are genuinely different and a single test cannot cover
both: one has no report to read, the other has a perfect one.

Run:  python -m pytest tests/scripts/test_run_gtest_single_process_crash.py -q
"""

from __future__ import annotations

import importlib.util
import io
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
RUN_GTEST = REPO_ROOT / "tests" / "scripts" / "run_gtest.py"

GREEN_XML = """<?xml version="1.0" encoding="UTF-8"?>
<testsuites tests="2" failures="0" disabled="0" errors="0" name="AllTests">
  <testsuite name="S" tests="2" failures="0" disabled="0" errors="0">
    <testcase name="A" status="run" classname="S"/>
    <testcase name="B" status="run" classname="S"/>
  </testsuite>
</testsuites>
"""

#: A Windows access-violation exit code -- the real one quoted in
#: `_is_abnormal_exit`'s docstring, not an invented sentinel.
ACCESS_VIOLATION = 3221225477


def _load():
    spec = importlib.util.spec_from_file_location("run_gtest_m", RUN_GTEST)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["run_gtest_m"] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture(scope="module")
def mod():
    return _load()


def _fake_run(mod, monkeypatch, rc, write_xml: bool):
    """Stand in for `_run`, controlling BOTH the exit code and the report.

    The two are independent in the defect -- that is the whole point -- so the
    double has to be able to produce a green report alongside a crash code.
    """
    def fake(exe, out_xml, log, gtest_filter, flagfile=None):
        if write_xml:
            io.open(out_xml, "w", encoding="utf-8").write(GREEN_XML)
        Path(log).parent.mkdir(parents=True, exist_ok=True)
        io.open(log, "w", encoding="utf-8").write("fake run\n")
        return rc
    monkeypatch.setattr(mod, "_run", fake)
    # Keep the corpus reporter quiet; it reads the XML and is not under test.
    monkeypatch.setattr(mod, "report_external_corpus_skips", lambda *a, **k: None)


def test_a_teardown_crash_with_a_green_report_is_RED(mod, tmp_path, monkeypatch):
    """Every test passed, the XML is complete and green, the process still died.

    This is the shape that is invisible to everything which reads assertions,
    because the crash arrives after the last one. Only the exit code carries it.
    """
    _fake_run(mod, monkeypatch, ACCESS_VIOLATION, write_xml=True)
    rc = mod.run_single_process("fake.exe", tmp_path, None, 0)
    assert rc != 0, (
        "a green report plus an access-violation exit code was reported as a "
        "clean run -- this is exactly GitHub #288 on the default path")


def test_a_crash_with_no_report_at_all_is_RED(mod, tmp_path, monkeypatch):
    """Crashed before writing any XML, so there is nothing to parse.

    Distinct from the above: here the danger is that an absent report reads as
    zero tests and therefore zero failures, i.e. CLEAN with tests=0.
    """
    _fake_run(mod, monkeypatch, ACCESS_VIOLATION, write_xml=False)
    rc = mod.run_single_process("fake.exe", tmp_path, None, 0)
    assert rc != 0, (
        "a crash that wrote no XML was reported as a clean run -- an empty "
        "report means NOTHING passed, not that nothing failed")


def test_a_normal_green_run_is_still_GREEN(mod, tmp_path, monkeypatch):
    """The specificity direction, and it is not ceremonial.

    Both assertions above expect RED, so on their own they establish only that
    the path is now SENSITIVE. If the fix were "return 1 whenever rc != 0" it
    would also redden an ordinary all-passing run, and nothing above would
    notice. gtest exits 0 here, so this must stay green.
    """
    _fake_run(mod, monkeypatch, 0, write_xml=True)
    rc = mod.run_single_process("fake.exe", tmp_path, None, 0)
    assert rc == 0, "a clean run with rc=0 must not be reported as a failure"


def test_an_ordinary_test_failure_exit_is_not_treated_as_a_crash(mod, tmp_path,
                                                                monkeypatch):
    """gtest exits 1 for ordinary failures, so 1 must NOT mean "crash".

    Without this, the fix would reclassify every normal red run as a process
    crash, which would be both wrong and loud enough to get the check removed.
    The XML here is green while rc=1, which is artificial, but it isolates the
    exit-code classification from the report contents -- which is the only way
    to test that 1 is handled differently from 3221225477.
    """
    _fake_run(mod, monkeypatch, 1, write_xml=True)
    rc = mod.run_single_process("fake.exe", tmp_path, None, 0)
    assert rc == 0, (
        "rc=1 is gtest's ordinary failure code, not a crash; with a green XML "
        "and no failed testcases this path should report clean rather than "
        "claiming the process died")


# ---------------------------------------------------------------------------
# The RERUN path -- GitHub #288's FIFTH site, and the worst of the five.
#
# `_rerun_failures()` is shared by BOTH run modes (run_single_process and
# run_sharded), and it captured `rc` only to print it on the still-failing
# branch. The HEALED decision -- `if rt["tests"] >= 1 and not rfailed` --
# never consulted it.
#
# So a rerun whose lone test passed and whose PROCESS then died in teardown
# was recorded as a transient flake. That is strictly worse than the four
# paths above: those LOSE a crash, this one LAUNDERS it, returning 0 while
# also asserting the test is fine. This repo has already paid for that shape
# once -- a `--shards 8` run healed two access violations and printed CLEAN.
#
# The function's docstring states the rule outright ("a CRASH CANNOT BE HEALED
# HERE, whatever the rerun does"), but enforced it only via `unhealable`, i.e.
# for tests the INITIAL run saw faulting. A crash arriving only during the
# rerun of an ordinary assertion failure was never in that set.
# ---------------------------------------------------------------------------

def _fake_rerun(mod, monkeypatch, rc, write_xml: bool = True):
    """As `_fake_run`, but every rerun reports its single test as PASSING.

    The rerun is what decides healing, so the double must produce the exact
    shape that fools the old code: a green report from a dead process.
    """
    def fake(exe, out_xml, log, gtest_filter, flagfile=None):
        if write_xml:
            io.open(out_xml, "w", encoding="utf-8").write(GREEN_XML)
        Path(log).parent.mkdir(parents=True, exist_ok=True)
        io.open(log, "w", encoding="utf-8").write("fake rerun\n")
        return rc
    monkeypatch.setattr(mod, "_run", fake)


def test_a_rerun_that_passes_but_crashes_is_NOT_healed(mod, tmp_path,
                                                       monkeypatch):
    """The laundering case: green rerun report, dead rerun process.

    `S.A` was an ordinary assertion failure in the main run, so it is NOT in
    `unhealable` and the GitHub #264 carve-out above does not apply to it.
    """
    _fake_rerun(mod, monkeypatch, ACCESS_VIOLATION)
    healed, unresolved = mod._rerun_failures("fake.exe", tmp_path, ["S.A"], 1)
    assert healed == [], (
        "a rerun whose process died with an access violation was recorded as "
        "a healed transient flake -- that launders a crash into a success, "
        "which is GitHub #288 on the rerun path")
    assert unresolved == ["S.A"], "the crashing rerun must stay unresolved"


def test_a_genuine_flake_is_still_healed(mod, tmp_path, monkeypatch):
    """The specificity direction, and it is load-bearing.

    The assertion above expects NOT-healed, so alone it is satisfied by a fix
    that simply never heals anything -- which would disable the rerun
    machinery wholesale and turn every real flake into a red build. A rerun
    that passes with rc=0 is what healing is FOR, and must survive.
    """
    _fake_rerun(mod, monkeypatch, 0)
    healed, unresolved = mod._rerun_failures("fake.exe", tmp_path, ["S.A"], 1)
    assert healed == ["S.A"], (
        "a test that passes cleanly on a fresh rerun is the transient flake "
        "this function exists to detect; it must still heal")
    assert unresolved == []


def test_a_crash_detected_in_the_main_run_is_still_not_healed(mod, tmp_path,
                                                             monkeypatch):
    """Regression guard for the pre-existing GitHub #264 carve-out.

    This path already worked, and the new check sits in the same `if`. Pinning
    it here so a later edit cannot satisfy the new case by restructuring the
    branch in a way that drops the old one -- the two crash origins (seen in
    the main run vs. seen only in the rerun) are independent.
    """
    _fake_rerun(mod, monkeypatch, 0)
    healed, unresolved = mod._rerun_failures("fake.exe", tmp_path, ["S.A"], 1,
                                             unhealable={"S.A"})
    assert healed == [], (
        "a test the main run saw CRASH passes alone by definition -- that is "
        "the signature of an order-dependent crash, not its refutation")
    assert unresolved == ["S.A"]


# ---------------------------------------------------------------------------
# The MERGE path -- GitHub #288's SIXTH site, and the only one that is not a
# `_run()` caller at all.
#
# The five sites above all live in ONE process, where `rc` is in scope. This
# one is a PROCESS BOUNDARY. `run_one_shard` classifies its crash correctly and
# returns it as an exit code; the documented use pattern is a later, SEPARATE
# `--merge-only` invocation that sees only what is on disk. `_run()` persisted
# `rc` nowhere, so a shard that ran every test, wrote a complete green XML and
# then died in teardown left behind perfect-looking evidence and no trace of
# the crash -- and the merge believed the XML.
#
# A JUnit XML records assertions. It cannot record that the process which wrote
# it then died, because nothing was left to write that down. So the fix
# persists the classification beside the report, and the merge fails CLOSED
# when a present XML has no such record: a bare XML is not evidence of health.
#
# This site was missed by a property-based enumeration that was otherwise
# correct, because the property had been scoped to a single process. A `karen`
# gate found it.
# ---------------------------------------------------------------------------

def _merge_fixture(mod, monkeypatch, tmp_path, *, status: dict | None):
    """One non-empty shard with a GREEN XML, and whatever status we choose.

    `_buckets_for` and `_check_manifest` are stubbed because they enumerate
    tests out of a real executable and compare partition manifests -- neither
    is under test here, and both would otherwise require a process shim. What
    IS under test is the verdict merge_shards reaches from on-disk evidence.
    """
    monkeypatch.setattr(mod, "_buckets_for",
                        lambda *a, **k: [["S.A", "S.B"]])
    monkeypatch.setattr(mod, "_check_manifest", lambda *a, **k: [])
    monkeypatch.setattr(mod, "report_external_corpus_skips", lambda *a, **k: None)
    io.open(tmp_path / "gtest_shard_0.xml", "w",
            encoding="utf-8").write(GREEN_XML)
    if status is not None:
        import json
        io.open(tmp_path / "gtest_shard_0.status.json", "w",
                encoding="utf-8").write(json.dumps(status))


def test_a_merged_shard_that_crashed_in_teardown_is_RED(mod, tmp_path,
                                                        monkeypatch):
    """The laundering case, one process boundary over from the five above.

    Every assertion passed and the XML says so. The shard process still died,
    and the recorded exit code is the only thing that knows.
    """
    _merge_fixture(mod, monkeypatch, tmp_path,
                   status={"shard": 0, "rc": ACCESS_VIOLATION,
                           "crashed": False, "teardown_crash": True})
    rc = mod.merge_shards("fake.exe", tmp_path, 1, None, None)
    assert rc != 0, (
        "a shard whose process died after writing a green XML was merged as a "
        "pass -- this is GitHub #288 across the --shard/--merge-only process "
        "boundary, where the exit code does not survive in memory")


def test_a_merge_of_a_bare_XML_with_no_status_REFUSES(mod, tmp_path,
                                                      monkeypatch):
    """Fail closed: absent evidence is not evidence of absence of a crash.

    Without this, the fix above is trivially bypassed by merging an XML
    produced by any older runner, or by a shard that died before writing its
    status -- the crash shape most likely to matter.
    """
    _merge_fixture(mod, monkeypatch, tmp_path, status=None)
    rc = mod.merge_shards("fake.exe", tmp_path, 1, None, None)
    assert rc != 0, (
        "an XML with no accompanying exit-code record was merged as a pass; a "
        "bare XML cannot attest that the process which wrote it survived")


def test_a_merge_of_an_unreadable_status_REFUSES(mod, tmp_path, monkeypatch):
    """Corrupt is treated as absent, not as permissive.

    A status file that will not parse is the one case where a reader is most
    tempted to shrug and continue. Pinned so it cannot drift into a pass.
    """
    _merge_fixture(mod, monkeypatch, tmp_path, status=None)
    io.open(tmp_path / "gtest_shard_0.status.json", "w",
            encoding="utf-8").write("{not json at all")
    rc = mod.merge_shards("fake.exe", tmp_path, 1, None, None)
    assert rc != 0, "an unparseable status file must fail closed, not open"


def test_a_clean_merge_is_still_GREEN(mod, tmp_path, monkeypatch):
    """The specificity direction for all three assertions above.

    They all expect RED, so on their own they are satisfied by a merge that
    refuses everything -- which would make the whole --shard/--merge-only
    workflow unusable. A green XML with a clean recorded exit code is the
    normal case and must still merge to 0.
    """
    _merge_fixture(mod, monkeypatch, tmp_path,
                   status={"shard": 0, "rc": 0,
                           "crashed": False, "teardown_crash": False})
    rc = mod.merge_shards("fake.exe", tmp_path, 1, None, None)
    assert rc == 0, (
        "a shard with a green XML and a clean recorded exit code is an "
        "ordinary pass and must merge green")


def test_run_one_shard_writes_the_status_the_merge_requires(mod, tmp_path,
                                                            monkeypatch):
    """The two halves must actually meet.

    The producer and the consumer were added together, so nothing else would
    notice if they disagreed about the filename or the keys -- and the failure
    mode would be every merge refusing, which reads as a broken runner rather
    than as a missing write.
    """
    monkeypatch.setattr(mod, "_buckets_for", lambda *a, **k: [["S.A", "S.B"]])
    _fake_run(mod, monkeypatch, ACCESS_VIOLATION, write_xml=True)
    rc = mod.run_one_shard("fake.exe", tmp_path, 1, 0, None, None)
    assert rc != 0, "run_one_shard must still report its own crash"

    st = mod._read_shard_status(tmp_path, 0)
    assert st is not None, (
        "run_one_shard wrote no status file, so a later --merge-only would "
        "refuse to merge a shard that actually ran")
    assert st["teardown_crash"] is True, (
        "the status file must record the classification, not just the raw code")
    assert st["rc"] == ACCESS_VIOLATION
