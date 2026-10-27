# -*- coding: utf-8 -*-
"""Tests for check_judgement_prose.py.

The checker exists because `claim-check` failed phase-5 of the repo-migration
pipeline twice over one record: once because a row's CLASSIFICATION contradicted
the diff, and once because a correctly-classified row's PROSE contradicted the
classification.

It failed a third time because the first version of the logic lived in a
throwaway job directory while being described as a standing control. So the
first test here asserts the checker is **tracked**, and the second asserts it is
**reachable from CI** — a checker that exists but runs nowhere is the same defect
in a different costume.
"""
import importlib.util
import io
import os
import pathlib
import subprocess
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
CHECKER = REPO_ROOT / "tools" / "gate" / "check_judgement_prose.py"


def _load():
    spec = importlib.util.spec_from_file_location("pdfe_judgement_prose",
                                                  str(CHECKER))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


mod = _load()
CHANGE = mod.CHANGE_TOKEN


class TheCheckerIsRealAndReachable(unittest.TestCase):
    """The lesson that cost phase-5 a third gate failure."""

    def test_the_checker_is_tracked(self):
        out = subprocess.run(
            ["git", "ls-files", "--error-unmatch",
             "tools/gate/check_judgement_prose.py"],
            cwd=str(REPO_ROOT), capture_output=True, text=True)
        self.assertEqual(
            out.returncode, 0,
            "check_judgement_prose.py is NOT tracked. An untracked control is "
            "not a control -- this is exactly what `claim-check` failed "
            "phase-5 for, and what it had already caught in phase-4 with an "
            "untracked rename preflight.")

    def test_this_test_file_is_tracked(self):
        out = subprocess.run(
            ["git", "ls-files", "--error-unmatch",
             "tools/gate/test_check_judgement_prose.py"],
            cwd=str(REPO_ROOT), capture_output=True, text=True)
        self.assertEqual(out.returncode, 0,
                         "the test must be tracked too, or nothing re-runs it")

    def test_the_checker_is_wired_into_ci(self):
        # Guards in this repository need wiring on a surface that actually
        # runs. A tracked-but-unreferenced checker passes every local sweep and
        # is never executed on push.
        wf = REPO_ROOT / ".github" / "workflows" / "fast-checks.yml"
        self.assertTrue(wf.exists(), "fast-checks.yml missing")
        with io.open(wf, encoding="utf-8") as fh:
            text = fh.read()
        self.assertIn(
            "tools/gate/check_judgement_prose.py", text,
            "the checker is not referenced by fast-checks.yml, so it would "
            "never run on push")
        self.assertIn(
            "tools/gate/test_check_judgement_prose.py", text,
            "this test file is not in the workflow's pytest batch, so the "
            "checker's own logic would go unexercised in CI")


class ContradictionDetection(unittest.TestCase):

    def test_a_change_row_describing_a_change_passes(self):
        self.assertEqual(
            mod.find_contradictions({"a.md": CHANGE},
                                    {"a.md": "Rewrote the broken pointer."}),
            [])

    def test_the_actual_phase5_defect_is_caught(self):
        # Verbatim shape of the sentence `claim-check` found.
        why = ("Two false claims rewritten; the version badge left alone "
               "because it reports 0.3.0 correctly off the live repository.")
        hits = mod.find_contradictions({"README.md": CHANGE},
                                       {"README.md": why})
        self.assertTrue(hits)
        self.assertEqual(hits[0][0], "README.md")

    def test_every_synonym_claimcheck_named_is_caught(self):
        # The first phrase list missed all five of these. Named explicitly so
        # a future narrowing of the patterns fails here rather than silently.
        for syn in ("remains as it was", "is unchanged", "kept as is",
                    "stays the same", "needs no modification"):
            with self.subTest(syn=syn):
                self.assertTrue(
                    mod.find_contradictions({"a.md": CHANGE},
                                            {"a.md": "The badge %s." % syn}),
                    "%r evades the check" % syn)

    def test_further_negations_are_caught(self):
        for syn in ("was not changed", "was not modified", "left untouched",
                    "requires no change", "left in place", "remains untouched",
                    "stayed the same", "is unmodified"):
            with self.subTest(syn=syn):
                self.assertTrue(
                    mod.find_contradictions({"a.md": CHANGE},
                                            {"a.md": "The badge %s." % syn}))

    def test_a_sentinel_marked_superseded_claim_is_excused(self):
        why = ('[superseded] an earlier version said the badge was "left '
               'alone because it reported 0.3.0" -- false on both counts.')
        self.assertEqual(
            mod.find_contradictions({"README.md": CHANGE}, {"README.md": why}),
            [])

    def test_prose_alone_does_not_excuse_without_the_sentinel(self):
        # The excuse is structural now. Convincing English is not enough.
        why = ("An earlier version of this paragraph said the badge was left "
               "alone because it reported 0.3.0. That was false: it renders "
               "red, and it was replaced with a static badge.")
        self.assertTrue(
            mod.find_contradictions({"README.md": CHANGE}, {"README.md": why}),
            "without the sentinel this must be flagged, however plausible the "
            "surrounding prose")


class TheTwoHistoricalBypassesStayClosed(unittest.TestCase):
    """Regression cases from `claim-check`'s third and fourth FAILs.

    Each defeated a proximity-based excuse. They are kept verbatim so a future
    reversion to window or sentence matching fails here rather than quietly
    re-admitting the defect.
    """

    def test_pass3_character_window_bypass(self):
        # Defeated a 260-before/160-after character window: "corrected" was
        # about the heading, and the window could not tell.
        why = ("We corrected the heading above. The badge was left alone "
               "because it's fine.")
        self.assertTrue(
            mod.find_contradictions({"README.md": CHANGE}, {"README.md": why}))

    def test_pass4_same_sentence_bypass(self):
        # Defeated sentence scoping: the marker shares the sentence and refers
        # to a DIFFERENT file. Deciding otherwise is reference resolution, not
        # pattern matching -- which is why the excuse is now a sentinel.
        why = ("An earlier version of the OTHER file was corrected; this file "
               "was left alone.")
        self.assertTrue(
            mod.find_contradictions({"README.md": CHANGE}, {"README.md": why}))

    def test_further_unrelated_correction_constructions(self):
        for why in (
                "A typo nearby was corrected; this file was left alone.",
                "The heading was fixed elsewhere, but this file is unchanged.",
                "An earlier draft of the sibling doc was wrong; this one "
                "remains as it was.",
        ):
            with self.subTest(why=why):
                self.assertTrue(
                    mod.find_contradictions({"a.md": CHANGE}, {"a.md": why}),
                    "an unrelated correction must not excuse a live "
                    "contradiction")

    def test_the_sentinel_must_share_the_negations_line(self):
        why = "[superseded] some other claim.\nThe badge was left alone."
        self.assertTrue(
            mod.find_contradictions({"a.md": CHANGE}, {"a.md": why}),
            "a sentinel on another line must not reach across to excuse")

    def test_no_marker_vocabulary_remains_in_the_excuse_path(self):
        # The excuse must not consult wording at all. If CORRECTION_MARKERS
        # comes back, this fails -- the arms race is not to be restarted.
        self.assertFalse(
            hasattr(mod, "CORRECTION_MARKERS"),
            "the excuse path must be structural; a marker vocabulary is what "
            "was defeated twice")
        self.assertTrue(hasattr(mod, "SUPERSEDED_SENTINEL"))

    def test_a_no_change_row_is_not_policed_for_this(self):
        self.assertEqual(
            mod.find_contradictions({"b.md": "no change"},
                                    {"b.md": "Left alone entirely."}),
            [])

    def test_missing_rationale_does_not_crash(self):
        self.assertEqual(mod.find_contradictions({"a.md": CHANGE}, {}), [])


class RecordParsing(unittest.TestCase):

    RECORD = """
# title

## Files changed

### `README.md`

The badge was left alone.

### `other.md`

Rewrote it.

## Every candidate, with its judgement

| file | reference target | n | judgement |
|---|---|---|---|
| `README.md` | release asset | 3 | **CHANGE** |
| `other.md` | prose reference | 1 | no change |
"""

    def test_classifications_and_rationales_are_paired(self):
        cls, why = mod.parse_record(self.RECORD)
        self.assertEqual(cls["README.md"], "**CHANGE**")
        self.assertEqual(cls["other.md"], "no change")
        self.assertIn("left alone", why["README.md"])

    def test_the_pairing_finds_the_contradiction(self):
        cls, why = mod.parse_record(self.RECORD)
        hits = mod.find_contradictions(cls, why)
        self.assertEqual([h[0] for h in hits], ["README.md"])

    def test_an_unparsable_record_is_reported_not_passed(self):
        # An empty population is structurally unable to fail. This repository
        # has shipped a scanner that read 245 of 3,490 files and said OK.
        self.assertTrue(mod.check_file(os.devnull))

    def test_the_real_record_passes(self):
        rec = (REPO_ROOT / "docs" / "ci" / "repo-migration-2026-10"
               / "phase-5-file-judgements.md")
        if not rec.exists():
            self.skipTest("record not present in this tree")
        self.assertEqual(mod.check_file(str(rec)), [])

    def test_the_real_record_is_actually_parsed(self):
        # Guards against the above passing because nothing was read.
        rec = (REPO_ROOT / "docs" / "ci" / "repo-migration-2026-10"
               / "phase-5-file-judgements.md")
        if not rec.exists():
            self.skipTest("record not present in this tree")
        with io.open(rec, encoding="utf-8") as fh:
            cls, why = mod.parse_record(fh.read())
        self.assertGreaterEqual(len(cls), 70,
                                "parsed only %d rows -- the population is "
                                "wrong, so a pass means nothing" % len(cls))
        self.assertTrue(any(CHANGE in v for v in cls.values()),
                        "no CHANGE row parsed, so the check had nothing to "
                        "police and would pass vacuously")


class SelfTestIsHonest(unittest.TestCase):

    def test_self_test_passes(self):
        out = subprocess.run(
            ["python", str(CHECKER), "--self-test"],
            cwd=str(REPO_ROOT), capture_output=True, text=True,
            encoding="utf-8", errors="replace")
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertIn("self-test: PASS", out.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
