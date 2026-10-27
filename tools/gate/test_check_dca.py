#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""DCA-check enforcement tests (pipeline e5b9c231 Phase 5 — criterion 4).

Proves check_dca.py actually enforces the escaped-defect -> DCA -> class-test
contract: a conformant record passes, and each way of cheating the contract
(missing section, placeholder left in, a test path that does not exist, an
empty causal field) is caught.

Run:
    python -m pytest tools/gate/test_check_dca.py -q
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_dca  # noqa: E402


CONFORMANT = """# DCA 9999 — sample

- **Date:** 2026-07-21
- **Escaped to:** build
- **Severity:** High
- **Reproducing test:** `{repro}::Suite.Instance`
- **Class-level test:** `{klass}::Suite.Class`
- **Harness property that missed it:** oracles asserted the edited element only.

## Escaped defect
Something rendered wrong.

## Reproducing test (instance)
The instance test reproduces it.

## Causal analysis
The oracle was blind to the class.

## Class-level test
A parametrised corpus catches the family.

## Prevention / standing control
A standing corpus keeps it dead.
"""


def _make_real_tests(tmp_path: Path):
    """Two real files the DCA can legitimately reference.

    Phase 41: these now contain the GTest macros the citations name. Before
    phase 41 they held only ``// real`` and the checker still passed them --
    that WAS the blind spot (it verified the file, never the ``Suite.Case``).
    """
    repro = tmp_path / "test_repro.cpp"
    klass = tmp_path / "test_class.cpp"
    repro.write_text("TEST(Suite, Instance) {}\n", encoding="utf-8")
    klass.write_text("TEST(Suite, Class) {}\n", encoding="utf-8")
    return repro, klass


def _dca_dir(tmp_path: Path) -> Path:
    d = tmp_path / "docs" / "audit" / "dca"
    d.mkdir(parents=True)
    return d


def test_conformant_record_passes(tmp_path):
    repro, klass = _make_real_tests(tmp_path)
    d = _dca_dir(tmp_path)
    (d / "0001-ok.md").write_text(
        CONFORMANT.format(repro=repro.relative_to(tmp_path).as_posix(),
                          klass=klass.relative_to(tmp_path).as_posix()),
        encoding="utf-8")
    ok, msgs = check_dca.scan(d, tmp_path)
    assert ok, "\n".join(msgs)


def test_nonexistent_test_path_fails(tmp_path):
    """A DCA cannot claim a class-level test it never wrote."""
    repro, _ = _make_real_tests(tmp_path)
    d = _dca_dir(tmp_path)
    (d / "0001-bad.md").write_text(
        CONFORMANT.format(repro=repro.relative_to(tmp_path).as_posix(),
                          klass="tests/does_not_exist.cpp"),
        encoding="utf-8")
    ok, msgs = check_dca.scan(d, tmp_path)
    assert not ok
    assert any("does not exist" in m for m in msgs)


def test_missing_section_fails(tmp_path):
    repro, klass = _make_real_tests(tmp_path)
    d = _dca_dir(tmp_path)
    body = CONFORMANT.format(repro=repro.relative_to(tmp_path).as_posix(),
                             klass=klass.relative_to(tmp_path).as_posix())
    body = body.replace("## Class-level test\n", "## (removed)\n")
    (d / "0001-nosec.md").write_text(body, encoding="utf-8")
    ok, msgs = check_dca.scan(d, tmp_path)
    assert not ok
    assert any("Class-level test" in m for m in msgs)


def test_placeholder_left_in_fails(tmp_path):
    repro, klass = _make_real_tests(tmp_path)
    d = _dca_dir(tmp_path)
    body = CONFORMANT.format(repro=repro.relative_to(tmp_path).as_posix(),
                             klass=klass.relative_to(tmp_path).as_posix())
    body += "\n\nStill <fill this in> later.\n"
    (d / "0001-ph.md").write_text(body, encoding="utf-8")
    ok, msgs = check_dca.scan(d, tmp_path)
    assert not ok
    assert any("placeholder" in m for m in msgs)


def test_cxx_generics_in_code_spans_are_not_placeholders(tmp_path):
    """`std::vector<char>` is C++, not an unfilled TEMPLATE.md placeholder.

    Two real records (0032, 0033) were rejected for `<size_t>`, `<char>` and
    `<int>` appearing inside backticks, which reddened `fast-checks` on correct
    content. A guard that fires on correct input trains people to ignore it and
    makes the one genuine failure indistinguishable from the noise.
    """
    repro, klass = _make_real_tests(tmp_path)
    d = _dca_dir(tmp_path)
    body = CONFORMANT.format(repro=repro.relative_to(tmp_path).as_posix(),
                             klass=klass.relative_to(tmp_path).as_posix())
    body += (
        "\n\nThe buffer is a `std::vector<char>` and the count is a `<size_t>`.\n"
        "\n```cpp\nstd::map<int, std::string> m;\n```\n"
    )
    (d / "0002-generics.md").write_text(body, encoding="utf-8")
    ok, msgs = check_dca.scan(d, tmp_path)
    assert ok, msgs


def test_placeholder_in_prose_still_fails_alongside_code_spans(tmp_path):
    """The narrowing must not become a bypass.

    Stripping code spans is precise, not permissive: an unfilled placeholder
    sits in PROSE, because that is what copying TEMPLATE.md leaves behind. A
    record containing both must still fail.
    """
    repro, klass = _make_real_tests(tmp_path)
    d = _dca_dir(tmp_path)
    body = CONFORMANT.format(repro=repro.relative_to(tmp_path).as_posix(),
                             klass=klass.relative_to(tmp_path).as_posix())
    body += "\n\nA `std::vector<char>` here, and <fill this in> in prose.\n"
    (d / "0003-mixed.md").write_text(body, encoding="utf-8")
    ok, msgs = check_dca.scan(d, tmp_path)
    assert not ok
    assert any("fill this in" in m for m in msgs)


def test_empty_harness_property_fails(tmp_path):
    repro, klass = _make_real_tests(tmp_path)
    d = _dca_dir(tmp_path)
    body = CONFORMANT.format(repro=repro.relative_to(tmp_path).as_posix(),
                             klass=klass.relative_to(tmp_path).as_posix())
    body = body.replace(
        "- **Harness property that missed it:** oracles asserted the edited element only.",
        "- **Harness property that missed it:** ")
    (d / "0001-empty.md").write_text(body, encoding="utf-8")
    ok, msgs = check_dca.scan(d, tmp_path)
    assert not ok
    assert any("Harness property" in m for m in msgs)


def test_template_and_readme_are_excluded(tmp_path):
    """TEMPLATE.md/README.md may hold placeholders and must not fail the scan."""
    d = _dca_dir(tmp_path)
    (d / "TEMPLATE.md").write_text("# T\n- **Date:** <YYYY>\n<fill me>\nTODO\n",
                                   encoding="utf-8")
    (d / "README.md").write_text("# readme <n/a>\n", encoding="utf-8")
    ok, _ = check_dca.scan(d, tmp_path)
    assert ok


def test_empty_dir_is_ok(tmp_path):
    d = _dca_dir(tmp_path)
    ok, _ = check_dca.scan(d, tmp_path)
    assert ok


# ---------------------------------------------------------------------------
# Phase 41 -- `Suite.Case` RESOLUTION (the R-26 blind spot).
#
# Before phase 41 check_dca.py resolved a citation by checking only that the
# FILE existed, so `tests/test_foo.cpp::Suite.TotallyMadeUpCase` passed a CI
# step named "...names real tests (R-26)". These tests exercise BOTH branches:
# a citation that resolves, and one that does not.
# ---------------------------------------------------------------------------

REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURE_DIR = REPO_ROOT / "tests" / "fixtures" / "dca_checker"
FIXTURE_CPP = FIXTURE_DIR / "dca_checker_sample.cpp"

# GitHub #203: the committed Prevention-section fixture. One document, scanned
# as committed (must pass) and with one token mutated (must fail) -- a guard
# that cannot redden is decoration.
PREVENTION_FIXTURE = (REPO_ROOT / "tools" / "gate" / "fixtures" /
                      "dca_prevention_record.md")


def _cite(repro_case: str, klass_case: str) -> str:
    rel = FIXTURE_CPP.relative_to(REPO_ROOT).as_posix()
    return CONFORMANT.replace("{repro}::Suite.Instance", f"{rel}::{repro_case}") \
                     .replace("{klass}::Suite.Class", f"{rel}::{klass_case}")


def _scan_one(tmp_path: Path, body: str):
    d = _dca_dir(tmp_path)
    (d / "0900-case.md").write_text(body, encoding="utf-8")
    return check_dca.scan(d, REPO_ROOT)


# --- the committed discriminating fixture pair -----------------------------

def test_committed_fixture_pair_discriminates():
    """The PASSING half passes and the FAILING half fails, on the same repo.

    This is the end-to-end criterion-2 demonstration: two records identical
    but for one case name, scanned exactly as CI scans docs/audit/dca.
    """
    ok_pass, msgs_pass = check_dca.scan(FIXTURE_DIR / "resolving", REPO_ROOT)
    assert ok_pass, "\n".join(msgs_pass)

    ok_fail, msgs_fail = check_dca.scan(FIXTURE_DIR / "nonresolving", REPO_ROOT)
    assert not ok_fail
    joined = "\n".join(msgs_fail)
    # The message must name the record, the field and the unresolved case.
    assert "0901-nonresolving-citation.md" in joined
    assert "Class-level test" in joined
    assert "DcaCheckerSampleFixture.TotallyMadeUpCase" in joined
    # ...and that must be the ONLY complaint, so the pair is discriminating.
    assert len([m for m in msgs_fail if m.startswith("    - ")]) == 1, joined


# --- resolution: positive branches -----------------------------------------

def test_plain_test_macro_resolves(tmp_path):
    ok, msgs = _scan_one(tmp_path, _cite("DcaCheckerSample.PlainCase",
                                         "DcaCheckerSampleFixture.FixtureCase"))
    assert ok, "\n".join(msgs)


def test_multiline_macro_call_resolves(tmp_path):
    """`TEST_F(\\n Suite,\\n Case)` is legal C++ -- a line-based check misses it."""
    ok, msgs = _scan_one(tmp_path, _cite("DcaCheckerSample.PlainCase",
                                         "DcaCheckerSampleFixture.MultiLineCase"))
    assert ok, "\n".join(msgs)


def test_test_p_resolves(tmp_path):
    """A parameterised case is cited by its SOURCE-level `Suite.Case`."""
    ok, msgs = _scan_one(tmp_path, _cite("DcaCheckerSampleParam.ParameterisedCase",
                                         "DcaCheckerSampleFixture.FixtureCase"))
    assert ok, "\n".join(msgs)


def test_test_p_runtime_instantiated_name_resolves(tmp_path):
    """`Prefix/Suite.Case/0` -- the name gtest prints -- normalises to the source form."""
    ok, msgs = _scan_one(
        tmp_path,
        _cite("SamplePrefix/DcaCheckerSampleParam.ParameterisedCase/0",
              "DcaCheckerSampleFixture.FixtureCase"))
    assert ok, "\n".join(msgs)


# --- resolution: negative branches -----------------------------------------

def test_nonexistent_case_in_existing_file_fails(tmp_path):
    """The blind spot itself: real file, invented case."""
    ok, msgs = _scan_one(tmp_path, _cite("DcaCheckerSample.PlainCase",
                                         "DcaCheckerSample.TotallyMadeUpCase"))
    assert not ok
    joined = "\n".join(msgs)
    assert "Class-level test" in joined
    assert "DcaCheckerSample.TotallyMadeUpCase" in joined


def test_wrong_suite_for_real_case_fails(tmp_path):
    """`Suite.Case` is matched as a PAIR, not as two independent names."""
    ok, msgs = _scan_one(tmp_path, _cite("DcaCheckerSample.PlainCase",
                                         "DcaCheckerSample.MultiLineCase"))
    assert not ok
    assert any("DcaCheckerSample.MultiLineCase" in m for m in msgs)


def test_commented_out_macro_does_not_resolve(tmp_path):
    ok, msgs = _scan_one(tmp_path, _cite("DcaCheckerSample.PlainCase",
                                         "DcaCheckerSample.CommentedOutCase"))
    assert not ok
    assert any("CommentedOutCase" in m for m in msgs)


def test_block_commented_macro_does_not_resolve(tmp_path):
    ok, msgs = _scan_one(
        tmp_path, _cite("DcaCheckerSample.PlainCase",
                        "DcaCheckerSampleFixture.BlockCommentedCase"))
    assert not ok
    assert any("BlockCommentedCase" in m for m in msgs)


def test_string_literal_macro_does_not_resolve(tmp_path):
    ok, msgs = _scan_one(tmp_path, _cite("DcaCheckerSample.PlainCase",
                                         "DcaCheckerSample.StringLiteralCase"))
    assert not ok
    assert any("StringLiteralCase" in m for m in msgs)


def test_instantiate_macro_is_not_a_definition(tmp_path):
    """INSTANTIATE_TEST_SUITE_P names a SUITE; it must not resolve a case."""
    ok, msgs = _scan_one(tmp_path,
                         _cite("DcaCheckerSample.PlainCase",
                               "DcaCheckerSampleParam.SamplePrefix"))
    assert not ok
    assert any("SamplePrefix" in m for m in msgs)


# --- fail-closed on malformed / unresolvable citations ----------------------

def test_citation_without_suite_case_fails(tmp_path):
    """A bare file path is not a test citation -- fail closed, never pass."""
    rel = FIXTURE_CPP.relative_to(REPO_ROOT).as_posix()
    body = CONFORMANT.replace("{repro}::Suite.Instance",
                              "%s::DcaCheckerSample.PlainCase" % rel) \
                     .replace("{klass}::Suite.Class", rel)
    ok, msgs = _scan_one(tmp_path, body)
    assert not ok
    assert any("no `::Suite.Case`" in m for m in msgs)


def test_unparseable_case_fails(tmp_path):
    ok, msgs = _scan_one(tmp_path, _cite("DcaCheckerSample.PlainCase",
                                         "NotADottedName"))
    assert not ok
    assert any("not a parseable" in m for m in msgs)


def test_unsupported_file_type_fails(tmp_path):
    """A citation this checker cannot resolve must exit non-zero, not pass."""
    repro, _ = _make_real_tests(tmp_path)
    other = tmp_path / "notes.txt"
    other.write_text("TEST(Suite, Class) {}\n", encoding="utf-8")
    d = _dca_dir(tmp_path)
    body = CONFORMANT.format(repro=repro.relative_to(tmp_path).as_posix(),
                             klass=other.relative_to(tmp_path).as_posix())
    (d / "0900-txt.md").write_text(body, encoding="utf-8")
    ok, msgs = check_dca.scan(d, tmp_path)
    assert not ok
    assert any("is not one this checker can resolve" in m for m in msgs)


def test_python_citation_resolves_and_fails_correctly(tmp_path):
    """A pytest nodeid resolves against a real `def`, and fails when invented."""
    repro, _ = _make_real_tests(tmp_path)
    py = tmp_path / "test_ui_thing.py"
    py.write_text("def test_real_case():\n    pass\n", encoding="utf-8")
    d = _dca_dir(tmp_path)

    good = CONFORMANT.format(repro=repro.relative_to(tmp_path).as_posix(),
                             klass=py.relative_to(tmp_path).as_posix())
    good = good.replace("`%s::Suite.Class`" % py.relative_to(tmp_path).as_posix(),
                        "`%s::test_real_case`" % py.relative_to(tmp_path).as_posix())
    (d / "0900-py.md").write_text(good, encoding="utf-8")
    ok, msgs = check_dca.scan(d, tmp_path)
    assert ok, "\n".join(msgs)

    bad = good.replace("::test_real_case", "::test_invented_case")
    (d / "0900-py.md").write_text(bad, encoding="utf-8")
    ok, msgs = check_dca.scan(d, tmp_path)
    assert not ok
    assert any("test_invented_case" in m for m in msgs)


def test_unreadable_file_fails_closed(tmp_path, monkeypatch):
    """An IO error during resolution must FAIL, not be swallowed."""
    def boom(_path):
        raise OSError("simulated read failure")
    monkeypatch.setattr(check_dca, "_read_source", boom)
    ok, msgs = _scan_one(tmp_path, _cite("DcaCheckerSample.PlainCase",
                                         "DcaCheckerSampleFixture.FixtureCase"))
    assert not ok
    assert any("could not be read" in m for m in msgs)


# --- regression corpus: the real, already-passing records ------------------

def test_live_records_0001_0003_still_resolve():
    """Records 0001-0003 pass today; the new resolution must not break them.

    They are the real-world regression corpus for this change: if one of them
    stopped resolving, that would be a finding about the record, not a licence
    to weaken the checker.
    """
    live = REPO_ROOT / "docs" / "audit" / "dca"
    for name in sorted(p.name for p in live.glob("000[1-3]-*.md")):
        problems = check_dca.check_record(live / name, REPO_ROOT)
        assert not problems, "%s: %s" % (name, problems)


def test_parser_sees_every_real_macro_form_in_the_fixture():
    """Direct assertion on the parser's view of the committed fixture source."""
    cases = check_dca.parse_gtest_cases(FIXTURE_CPP)
    assert cases == {
        "DcaCheckerSample.PlainCase",
        "DcaCheckerSampleFixture.FixtureCase",
        "DcaCheckerSampleFixture.MultiLineCase",
        "DcaCheckerSampleParam.ParameterisedCase",
    }, sorted(cases)


def test_ambiguous_duplicate_case_definition_fails_closed(tmp_path):
    """Two macros producing the same Suite.Case must be REJECTED, not resolved.

    `resolve_case`'s `n > 1` branch is the least reachable of its fail-closed
    paths -- no live record hits it, so without this test a mutation that
    turned `elif n > 1:` into a silent pass would survive. That matters more
    than the rarity suggests: an ambiguous citation is precisely the case
    where a reader cannot tell WHICH test the record means, so resolving it
    would let the record claim coverage it has not pinned down.

    Added after the phase-41 `karen` gate flagged the branch as untested.
    """
    src = tmp_path / "test_dup.cpp"
    src.write_text(
        "TEST(DupSuite, DupCase) {}\n"
        "TEST_F(DupSuite, DupCase) {}\n",
        encoding="utf-8",
    )
    problems = check_dca.resolve_case(
        "Class-level test", "test_dup.cpp::DupSuite.DupCase", tmp_path)
    assert len(problems) == 1, problems
    assert "ambiguous" in problems[0], problems[0]
    assert "2 macro definitions" in problems[0], problems[0]


def _prevention_fixture_in(tmp_path: Path, body: str | None = None):
    """Stage the committed Prevention fixture as a lone record and scan it.

    Scanned with the REAL repo root, exactly as CI scans ``docs/audit/dca`` --
    so the citations resolve (or fail to) against the real tree, not a mock.
    """
    d = tmp_path / "docs" / "audit" / "dca"
    d.mkdir(parents=True, exist_ok=True)  # callable twice in one test
    text = body if body is not None else PREVENTION_FIXTURE.read_text(
        encoding="utf-8")
    # newline="" so a CRLF body is written VERBATIM -- Python's text mode would
    # otherwise translate "\r\n" to "\r\r\n" and the CRLF test would be
    # exercising a corruption rather than a checkout.
    with open(d / "9903-prevention.md", "w", encoding="utf-8",
              newline="") as fh:
        fh.write(text)
    return check_dca.scan(d, REPO_ROOT)


def test_prevention_fixture_with_resolving_citations_passes(tmp_path):
    """Direction 1: the honest fixture, whose Prevention citations all resolve.

    This is the half that stops the guard being a noise generator. It also pins
    the NON-citations: the same section deliberately contains `Document.cpp`,
    `MANIFEST.sha256`, `Qt::CaseInsensitive`, `std::vector` and the
    metasyntactic `Suite.Case`, none of which may be resolved as a test name.
    """
    ok, msgs = _prevention_fixture_in(tmp_path)
    assert ok, "\n".join(msgs)


def test_prevention_citing_nonexistent_suite_case_fails(tmp_path):
    """Direction 2a: a Prevention bullet naming a `Suite.Case` that is not in the tree."""
    body = PREVENTION_FIXTURE.read_text(encoding="utf-8").replace(
        "DcaCheckerSampleFixture.MultiLineCase",
        "DcaCheckerSampleFixture.TotallyMadeUpCase")
    ok, msgs = _prevention_fixture_in(tmp_path, body)
    assert not ok
    joined = "\n".join(msgs)
    assert "Prevention / standing control" in joined
    assert "DcaCheckerSampleFixture.TotallyMadeUpCase" in joined
    # ...and it must be the ONLY complaint, so the mutation is discriminating.
    assert len([m for m in msgs if m.startswith("    - ")]) == 1, joined


def test_prevention_citing_nonexistent_json_seed_id_fails(tmp_path):
    """Direction 2b: a `tools/gate/*.json` seed id that is not in the file.

    This is DCA 0001's phantom `AdversarialAmbient` seed, mechanically.
    """
    body = PREVENTION_FIXTURE.read_text(encoding="utf-8").replace(
        "`M01-statecont-ctm-guard-drop-Q` registered in",
        "`M99-seed-that-was-never-added` registered in")
    ok, msgs = _prevention_fixture_in(tmp_path, body)
    assert not ok
    joined = "\n".join(msgs)
    assert "M99-seed-that-was-never-added" in joined
    assert "tools/gate/mutants.json" in joined
    assert len([m for m in msgs if m.startswith("    - ")]) == 1, joined


def test_prevention_path_qualified_citation_is_resolved_against_that_file(tmp_path):
    """The `path::Suite.Case` half of the Prevention rule reddens too.

    Without this, the two mutations above would leave the path-qualified branch
    unpinned -- and a citation that names its own file is the form a reader is
    most likely to trust.
    """
    body = PREVENTION_FIXTURE.read_text(encoding="utf-8").replace(
        "::DcaCheckerSampleParam.ParameterisedCase",
        "::DcaCheckerSampleParam.NotAParameterisedCase")
    ok, msgs = _prevention_fixture_in(tmp_path, body)
    assert not ok
    assert any("DcaCheckerSampleParam.NotAParameterisedCase" in m for m in msgs)


def test_prevention_dated_historical_addendum_is_exempt(tmp_path):
    """A dated `**Correction, YYYY-MM-DD ...**` block quotes the past verbatim.

    DCA 0014 names four CRL tests by the spelling they had before a rename.
    Resolving those would redden an honest record for saying true things about
    history. The fixture carries such a block naming
    `OcspValidityTest.FreshCrlAccepted`, which no longer exists.
    """
    text = PREVENTION_FIXTURE.read_text(encoding="utf-8")
    assert "OcspValidityTest.FreshCrlAccepted" in text
    assert "OcspValidityTest.FreshCrlAccepted" not in \
        check_dca.tests_case_index(REPO_ROOT)
    ok, msgs = _prevention_fixture_in(tmp_path)
    assert ok, "\n".join(msgs)


def test_the_historical_exemption_requires_a_date(tmp_path):
    """The exemption must not be claimable by writing 'Correction' in front.

    Strip the ISO date out of the addendum's bold lead and the same block is
    scanned again -- otherwise the carve-out would be a bypass.
    """
    body = PREVENTION_FIXTURE.read_text(encoding="utf-8").replace(
        "**Correction, 2026-08-09 (fixture).**", "**Correction (fixture).**")
    ok, msgs = _prevention_fixture_in(tmp_path, body)
    assert not ok
    assert any("OcspValidityTest.FreshCrlAccepted" in m for m in msgs)


def test_prevention_resolution_survives_crlf(tmp_path):
    """`core.autocrlf=true`: a CI checkout hands these records CRLF.

    The block split is on a blank line, and `\\r\\n\\r\\n` does not match
    `\\n[ \\t]*\\n`. Unfixed, the whole section collapses into one block and the
    historical-addendum exemption stops applying.

    ASSERTED ON THE DIRECT API, deliberately. `check_record` reads with
    universal newlines, so `scan()` can never see a `\\r`, and a scan-level
    assertion here would pass with or WITHOUT the normalisation -- a vacuous
    test of the kind this repo keeps finding. Deleting
    `text.replace("\\r\\n", "\\n")` from `prevention_section` makes the FIRST
    assertion below fail (verified by doing exactly that), which is the only
    reason this test is worth having.
    """
    crlf = PREVENTION_FIXTURE.read_text(encoding="utf-8").replace(
        "\n", "\r\n")
    assert check_dca.check_prevention(crlf, REPO_ROOT) == []

    # ...and it still DISCRIMINATES on CRLF input, so the pass above is not
    # "resolved nothing".
    bad = crlf.replace("DcaCheckerSampleFixture.MultiLineCase",
                       "DcaCheckerSampleFixture.TotallyMadeUpCase")
    problems = check_dca.check_prevention(bad, REPO_ROOT)
    assert any("TotallyMadeUpCase" in p for p in problems), problems

    # The scan path is covered as well (it gets there via universal newlines).
    ok, msgs = _prevention_fixture_in(tmp_path, crlf)
    assert ok, "\n".join(msgs)


def test_prevention_index_is_built_from_real_gtest_macros():
    """The corpus the bare-name rule resolves against is the real suite.

    A guard whose index is empty passes everything. Pin that it is populated
    and that it contains a name only a real macro parse could produce.
    """
    index = check_dca.tests_case_index(REPO_ROOT)
    assert len(index) > 4000, len(index)
    assert "DcaCheckerSample.PlainCase" in index
    # Blanked-out definitions must NOT be in it (the fixture has both forms).
    assert "DcaCheckerSample.CommentedOutCase" not in index
    assert "DcaCheckerSample.StringLiteralCase" not in index


def test_live_records_have_resolving_prevention_sections():
    """Every committed record passes the new rule -- no false positives.

    81 records were green before this rule existed; a rule that reddens an
    honest one is worse than the hole it closes.
    """
    live = REPO_ROOT / "docs" / "audit" / "dca"
    bad = {}
    for rec in sorted(live.glob("*.md")):
        if rec.name in check_dca.EXCLUDED:
            continue
        problems = check_dca.check_prevention(
            rec.read_text(encoding="utf-8", errors="replace"), REPO_ROOT)
        if problems:
            bad[rec.name] = problems
    assert not bad, bad


def test_prevention_rule_actually_fires_on_the_live_corpus():
    """...and it is not vacuous: real records DO carry resolved citations.

    A rule that resolves nothing would also produce zero failures. Count the
    citations the live corpus actually submits to it.
    """
    live = REPO_ROOT / "docs" / "audit" / "dca"
    index = check_dca.tests_case_index(REPO_ROOT)
    resolved = 0
    for rec in sorted(live.glob("*.md")):
        if rec.name in check_dca.EXCLUDED:
            continue
        section = check_dca.prevention_section(
            rec.read_text(encoding="utf-8", errors="replace"))
        for block in check_dca._blocks(section):
            if check_dca._is_historical_block(block):
                continue
            for tok in check_dca._BACKTICKED.findall(block):
                tok = tok.strip()
                if check_dca._looks_like_case(tok):
                    assert tok in index, tok
                    resolved += 1
    assert resolved >= 10, resolved


def test_single_definition_of_that_same_case_still_resolves(tmp_path):
    """Control for the test above: one definition of the identical name passes.

    Without this, `test_ambiguous_duplicate_case_definition_fails_closed`
    would also pass if `resolve_case` rejected the name for some unrelated
    reason -- the assertion would be true for the wrong cause.
    """
    src = tmp_path / "test_dup.cpp"
    src.write_text("TEST(DupSuite, DupCase) {}\n", encoding="utf-8")
    assert check_dca.resolve_case(
        "Class-level test", "test_dup.cpp::DupSuite.DupCase", tmp_path) == []


# ---------------------------------------------------------------------------
# GitHub #252. TEMPLATE.md has carried **Severity** and **Escaped to** since the
# first record and this checker validated NEITHER, so a record could omit both
# and still report OK. Three did (0083, 0084, 0085) and went unnoticed for five
# days. "Escaped to" is the load-bearing one: it is the difference between a
# defect a test caught and one that reached a user, which is the reason these
# records exist at all.
# ---------------------------------------------------------------------------
def _record_without(tmp_path: Path, line_startswith: str) -> tuple:
    repro, klass = _make_real_tests(tmp_path)
    body = CONFORMANT.format(repro=repro.relative_to(tmp_path).as_posix(),
                             klass=klass.relative_to(tmp_path).as_posix())
    kept = [ln for ln in body.split("\n")
            if not ln.startswith(line_startswith)]
    assert len(kept) < len(body.split("\n")), (
        "the fixture never contained %r, so this test would be vacuous"
        % line_startswith)
    d = _dca_dir(tmp_path)
    (d / "9999-sample.md").write_text("\n".join(kept), encoding="utf-8")
    return check_dca.scan(d, tmp_path)


def test_a_record_without_severity_fails(tmp_path):
    ok, msgs = _record_without(tmp_path, "- **Severity:**")
    assert not ok
    assert any("Severity" in m for m in msgs), msgs


def test_a_record_without_escaped_to_fails(tmp_path):
    ok, msgs = _record_without(tmp_path, "- **Escaped to:**")
    assert not ok
    assert any("Escaped to" in m for m in msgs), msgs


def test_an_unknown_severity_is_rejected(tmp_path):
    repro, klass = _make_real_tests(tmp_path)
    body = CONFORMANT.format(repro=repro.relative_to(tmp_path).as_posix(),
                             klass=klass.relative_to(tmp_path).as_posix())
    body = body.replace("- **Severity:** High", "- **Severity:** Cosmetic")
    d = _dca_dir(tmp_path)
    (d / "9999-sample.md").write_text(body, encoding="utf-8")
    ok, msgs = check_dca.scan(d, tmp_path)
    assert not ok
    assert any("Cosmetic" in m for m in msgs), msgs


def test_low_is_accepted_because_ten_committed_records_use_it(tmp_path):
    """The template listed Critical/High/Medium; ten records say Low.

    The checker was widened to match the practice rather than ten historical
    severities being rewritten to match a list nothing had ever enforced.
    """
    assert "Low" in check_dca.SEVERITIES
    repro, klass = _make_real_tests(tmp_path)
    body = CONFORMANT.format(repro=repro.relative_to(tmp_path).as_posix(),
                             klass=klass.relative_to(tmp_path).as_posix())
    body = body.replace("- **Severity:** High", "- **Severity:** Low")
    d = _dca_dir(tmp_path)
    (d / "9999-sample.md").write_text(body, encoding="utf-8")
    ok, msgs = check_dca.scan(d, tmp_path)
    assert ok, msgs
