# -*- coding: utf-8 -*-
"""The secrets baseline's arithmetic must close.

WHY THIS EXISTS.

`tests/security/secrets/baseline.json` groups the full-history scan's findings
into classes, because 209 findings across a handful of mechanical classes would
otherwise produce 209 near-identical rationales -- and a baseline nobody can
read is a baseline nobody reviews.

Grouping has a failure mode that enumeration does not: **the classes can stop
adding up**, and nothing notices. The first version of that file asserted three
classes summing to **127 of 129** gitleaks findings and **78 of 80** trufflehog
findings. Four findings were in no class at all, and one class misattributed
its own hits -- it named seven PEM fixtures as the source of gitleaks hits that
had actually come from a generator script and from a checker's own test module.

Nothing caught it. The stamp was non-null, the prose said "all findings fall
into the three classes below", the phase's acceptance criterion ("findings are
triaged and recorded") read as satisfied, and the phase called itself done. It
took a gate reconstructing per-file counts from the raw reports to find it.

So the arithmetic is now a test. It cannot detect a WRONG rationale -- only a
human reading can -- but it can detect the specific lie that "every finding is
accounted for" when the sums say otherwise.

Run:  python -m pytest -q tools/gate/test_secrets_baseline_arithmetic.py
"""
from __future__ import annotations

import io
import json
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BASELINE = REPO / "tests" / "security" / "secrets" / "baseline.json"


def load():
    with io.open(BASELINE, encoding="utf-8") as fh:
        return json.load(fh)


class BaselineIsStamped(unittest.TestCase):
    def test_stamp_is_not_null(self):
        """A null stamp means the scan never ran. It stood null for a whole
        pipeline under an Option-A deferral, so this is worth asserting."""
        d = load()
        self.assertIsNotNone(
            d.get("stamp"),
            "baseline stamp is null: the full-history scan has not been "
            "recorded, so nothing here describes a real run")

    def test_the_deferral_flag_is_cleared(self):
        d = load()
        self.assertFalse(
            d.get("deferred_per_pipeline_option_a", False),
            "the Option-A deferral flag is still set while a stamp exists -- "
            "one of the two is wrong")


class ArithmeticCloses(unittest.TestCase):
    """The reason this file exists."""

    def _sums(self):
        d = load()
        stamp = d["stamp"]
        acc = d["accepted_findings"]
        return (
            sum(int(c.get("gitleaks_count", 0)) for c in acc),
            int(stamp["gitleaks"]["findings"]),
            sum(int(c.get("trufflehog_count", 0)) for c in acc),
            int(stamp["trufflehog"]["findings"]),
        )

    def test_gitleaks_class_counts_sum_to_the_stamped_total(self):
        gl_sum, gl_total, _, _ = self._sums()
        self.assertEqual(
            gl_sum, gl_total,
            "accepted_findings gitleaks_count sums to %d but the stamp "
            "records %d findings. %d finding(s) are in NO class, so 'every "
            "finding is accounted for' is false. This is the exact defect "
            "this test was written for." % (gl_sum, gl_total,
                                            abs(gl_total - gl_sum)))

    def test_trufflehog_class_counts_sum_to_the_stamped_total(self):
        _, _, th_sum, th_total = self._sums()
        self.assertEqual(
            th_sum, th_total,
            "accepted_findings trufflehog_count sums to %d but the stamp "
            "records %d findings. %d finding(s) are in NO class."
            % (th_sum, th_total, abs(th_total - th_sum)))

    def test_by_rule_and_by_detector_also_sum_to_their_totals(self):
        """The stamp's own breakdowns must agree with its own totals, or the
        stamp is internally inconsistent before any class is considered."""
        d = load()
        st = d["stamp"]
        self.assertEqual(sum(st["gitleaks"]["by_rule"].values()),
                         st["gitleaks"]["findings"],
                         "stamp.gitleaks.by_rule does not sum to its findings")
        self.assertEqual(sum(st["trufflehog"]["by_detector"].values()),
                         st["trufflehog"]["findings"],
                         "stamp.trufflehog.by_detector does not sum to its "
                         "findings")


class EveryClassIsUsable(unittest.TestCase):
    def test_each_class_has_a_rationale_and_at_least_one_finding(self):
        """A class with no findings is dead weight that makes the sums look
        fuller than they are; a class with no rationale is an unexplained
        dismissal, which AC3 forbids."""
        for c in load()["accepted_findings"]:
            name = c.get("class", "<unnamed>")
            with self.subTest(cls=name):
                self.assertTrue(c.get("rationale", "").strip(),
                                "class %r has no rationale" % name)
                self.assertGreater(
                    int(c.get("gitleaks_count", 0))
                    + int(c.get("trufflehog_count", 0)), 0,
                    "class %r accounts for zero findings" % name)
                self.assertTrue(c.get("paths"),
                                "class %r names no path" % name)

    def test_dispositions_are_from_the_documented_set(self):
        allowed = {"false-positive", "true-positive-rotated",
                   "true-positive-history-rewritten"}
        for c in load()["accepted_findings"]:
            with self.subTest(cls=c.get("class")):
                self.assertIn(c.get("disposition"), allowed,
                              "disposition must be one of triage.md's three")

    def test_a_true_positive_would_require_more_than_a_rationale(self):
        """Not currently exercised by the data -- there are no true positives
        -- but asserted so that adding one cannot quietly skip the rotation
        record triage.md requires."""
        for c in load()["accepted_findings"]:
            if c.get("disposition", "").startswith("true-positive"):
                with self.subTest(cls=c.get("class")):
                    self.assertTrue(
                        c.get("rotation_date"),
                        "a true positive needs a rotation_date; see triage.md")


if __name__ == "__main__":
    unittest.main(verbosity=2)
