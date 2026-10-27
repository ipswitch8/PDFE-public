# -*- coding: utf-8 -*-
"""Tests for check_record_vs_diff.py — the judgement record's primary control.

Three of these tests exist because of how this checker came to be tracked. It
lived in a Claude job's temp directory while three documents and another
checker's docstring called it "the primary control". The `claim-check` gate
failed phase-5 on its fifth pass for that, having searched the tracked tree and
found zero hits.

It was the third occurrence of the same shape in one pipeline (phase-4's rename
preflight, `check_judgement_prose.py`'s first version, then this), and the third
was committed *inside the claim used to justify the second one's known
weakness*. So the tracked-and-wired properties are asserted, not assumed.
"""
import importlib.util
import io
import pathlib
import subprocess
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
CHECKER = REPO_ROOT / "tools" / "gate" / "check_record_vs_diff.py"


def _load():
    spec = importlib.util.spec_from_file_location("pdfe_record_vs_diff",
                                                  str(CHECKER))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


mod = _load()


class TheCheckerIsRealAndReachable(unittest.TestCase):

    def _tracked(self, rel):
        out = subprocess.run(["git", "ls-files", "--error-unmatch", rel],
                             cwd=str(REPO_ROOT), capture_output=True, text=True)
        return out.returncode == 0

    def test_the_checker_is_tracked(self):
        self.assertTrue(
            self._tracked("tools/gate/check_record_vs_diff.py"),
            "NOT tracked. This checker spent most of phase-5 in a job temp "
            "directory while being described as the primary control, and "
            "`claim-check` failed the phase for it.")

    def test_this_test_file_is_tracked(self):
        self.assertTrue(self._tracked("tools/gate/test_check_record_vs_diff.py"))

    def test_both_are_wired_into_ci(self):
        wf = REPO_ROOT / ".github" / "workflows" / "fast-checks.yml"
        with io.open(wf, encoding="utf-8") as fh:
            text = fh.read()
        self.assertIn("tools/gate/check_record_vs_diff.py", text,
                      "not referenced by fast-checks.yml, so it never runs "
                      "on push -- tracked but unreachable is the same defect")
        self.assertIn("tools/gate/test_check_record_vs_diff.py", text,
                      "this test file is not in the workflow's pytest batch")


class Comparison(unittest.TestCase):

    def test_identical_sets_agree(self):
        self.assertEqual(mod.compare({"a", "b"}, {"a", "b"}, exclude=""),
                         ([], []))

    def test_a_diffed_file_with_no_row_is_reported(self):
        self.assertEqual(mod.compare({"a"}, {"a", "b"}, exclude=""),
                         ([], ["b"]))

    def test_a_claimed_row_with_no_diff_entry_is_reported(self):
        self.assertEqual(mod.compare({"a", "b"}, {"a"}, exclude=""),
                         (["b"], []))

    def test_both_directions_at_once(self):
        claimed, unjudged = mod.compare({"a", "x"}, {"a", "y"}, exclude="")
        self.assertEqual((claimed, unjudged), (["x"], ["y"]))

    def test_the_exclusion_applies_to_both_sides(self):
        own = "docs/ci/repo-migration-2026-10/phase-5.md"
        self.assertEqual(mod.compare({own}, set()), ([], []))
        self.assertEqual(mod.compare(set(), {own}), ([], []))


class TheFalseDisagreeRegression(unittest.TestCase):
    """The scratch version reported DISAGREE on two identical sets."""

    def test_indentation_does_not_fake_a_disagreement(self):
        # It indented paths for display inside the file it then diffed.
        # Formatting belongs in the output, never in the data compared.
        self.assertEqual(
            mod.compare(mod.normalise(["  a", "\tb"]),
                        mod.normalise(["a", "b"]), exclude=""),
            ([], []),
            "indented input must not read as a different set -- this exact "
            "mistake produced a false DISAGREE and sent a subagent chasing it")

    def test_quotes_and_separators_normalise(self):
        self.assertEqual(
            mod.compare(mod.normalise(['"x\\y"']), mod.normalise(["x/y"]),
                        exclude=""),
            ([], []))

    def test_blank_entries_are_dropped(self):
        self.assertEqual(mod.normalise(["", "   ", "a"]), {"a"})


class RowParsing(unittest.TestCase):

    def test_a_change_row_is_recognised(self):
        self.assertEqual(
            mod.change_rows("| `a/b.md` | prose | 1 | **CHANGE** |"),
            {"a/b.md"})

    def test_a_no_change_row_is_not(self):
        self.assertEqual(
            mod.change_rows("| `a/b.md` | prose | 1 | no change |"), set())

    def test_all_rows_counts_both_kinds(self):
        text = ("| `a.md` | prose | 1 | **CHANGE** |\n"
                "| `b.md` | prose | 1 | no change |")
        self.assertEqual(mod.all_rows(text), {"a.md", "b.md"})

    def test_an_empty_record_is_reported_not_passed(self):
        import os
        self.assertTrue(mod.check(record=os.devnull))

    def test_the_real_record_parses_a_real_population(self):
        rec = (REPO_ROOT / "docs" / "ci" / "repo-migration-2026-10"
               / "phase-5-file-judgements.md")
        if not rec.exists():
            self.skipTest("record not present")
        with io.open(rec, encoding="utf-8") as fh:
            text = fh.read()
        self.assertGreaterEqual(
            len(mod.all_rows(text)), 70,
            "too few rows parsed -- an under-read population passes vacuously")
        self.assertTrue(mod.change_rows(text),
                        "no CHANGE rows parsed, so the comparison would be "
                        "one-sided and could not fail in that direction")


class ItMustNotBeStructurallyUnableToPass(unittest.TestCase):
    """The defect found one minute after this checker was first committed.

    The first version compared only against `git status --porcelain` — the
    WORKING diff. Correct while the work is uncommitted; wrong the instant it
    is committed, because the tree is then clean and every `**CHANGE**` row
    reads as a claim about work that never happened. It reported exactly that,
    on all seven rows, immediately after phase-5 was committed.

    Since the checker is wired into `fast-checks.yml`, which runs on push
    against a clean checkout, that version was **structurally unable to pass
    in CI** — the mirror of this repository's recorded "structurally unable to
    fail" failure mode, and worse in one respect: it would have reddened CI for
    a reason unrelated to the tree being wrong.
    """

    def test_a_committed_record_compares_against_its_commit(self):
        diffed, source = mod.record_commit_diff(mod.DEFAULT_RECORD,
                                                cwd=str(REPO_ROOT))
        if diffed is None:
            self.skipTest("record neither committed nor modified here")
        self.assertTrue(
            source.startswith("commit ") or source == "working tree",
            "the comparison source must be named, not implied: %r" % source)
        self.assertTrue(
            diffed,
            "a committed record must compare against a NON-EMPTY set of files "
            "-- an empty set makes every CHANGE row look like a false claim, "
            "which is how this checker failed on its own first commit")

    def test_the_real_record_passes_in_its_current_state(self):
        # Whether committed or dirty, the live record must pass. This is the
        # end-to-end assertion that the two-source logic actually works.
        self.assertEqual(
            mod.check(record=mod.DEFAULT_RECORD, cwd=str(REPO_ROOT)), [],
            "the live record must agree with whichever source applies")

    def test_an_uncommitted_record_uses_the_working_tree(self):
        # Sanity-check the branch selection reports the right source name
        # rather than silently picking one.
        _, source = mod.record_commit_diff(mod.DEFAULT_RECORD,
                                           cwd=str(REPO_ROOT))
        dirty = subprocess.run(
            ["git", "status", "--porcelain", "--", mod.DEFAULT_RECORD],
            cwd=str(REPO_ROOT), capture_output=True, text=True).stdout.strip()
        if dirty:
            self.assertEqual(source, "working tree")
        else:
            self.assertTrue(source.startswith("commit "))

    def test_a_record_neither_committed_nor_modified_is_reported(self):
        # Fails closed: no comparison available is reported, never passed.
        diffed, source = mod.record_commit_diff("no/such/record.md",
                                                cwd=str(REPO_ROOT))
        self.assertIsNone(diffed)
        self.assertIn("no commit", source)


class SelfTestIsHonest(unittest.TestCase):

    def test_self_test_passes(self):
        out = subprocess.run(["python", str(CHECKER), "--self-test"],
                             cwd=str(REPO_ROOT), capture_output=True,
                             text=True, encoding="utf-8", errors="replace")
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertIn("self-test: PASS", out.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
