#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Unit tests for check_publication_block.py.

The guard this tests exists because the `claim-check` gate failed pub-readiness
phase-3 for having its highest-consequence claim — "the repository must stay
private until GitHub confirms collection" — enforced by prose alone. A guard
added in response to that finding has to be tested in BOTH directions, or it
is the same failure one level up: something that looks like enforcement and
cannot fail.

Run:  python tools/gate/test_check_publication_block.py
      python -m pytest tools/gate/test_check_publication_block.py
"""
from __future__ import annotations

import inspect
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_publication_block as g  # noqa: E402


class DecideBlocksPublication(unittest.TestCase):
    """The irreversible case: public while objects are still served."""

    def test_public_while_prepared_is_a_violation(self):
        code, msg = g.decide("PREPARED", "none", is_private=False)
        self.assertEqual(code, g.VIOLATION)
        self.assertIn("PUBLIC", msg)

    def test_public_while_submitted_is_still_a_violation(self):
        # Submitted is not collected. The objects are served until GitHub says
        # otherwise, so filing the request changes nothing about the risk.
        code, _ = g.decide("SUBMITTED", "1234567", is_private=False)
        self.assertEqual(code, g.VIOLATION)

    def test_public_after_collection_is_allowed(self):
        code, _ = g.decide("COLLECTED", "1234567", is_private=False)
        self.assertEqual(code, g.OK)


class DecideHandlesTheRouteBSupersession(unittest.TestCase):
    """SUPERSEDED (added p101 phase-6): the publication candidate is now a
    repository that never contained the orphans the GC request concerns, so
    demanding a COLLECTED ticket about a permanently-private archive gated the
    wrong thing. The supersession must still be RECORDED, or it is a bypass.
    """

    def test_public_with_recorded_supersession_is_allowed(self):
        code, msg = g.decide("SUPERSEDED", "route-b-p101", is_private=False)
        self.assertEqual(code, g.OK)
        self.assertIn("never", msg)

    def test_private_with_recorded_supersession_is_compliant(self):
        code, _ = g.decide("SUPERSEDED", "route-b-p101", is_private=True)
        self.assertEqual(code, g.OK)

    def test_reasonless_supersession_fails_in_both_visibility_states(self):
        # A supersession nobody wrote down must not function as a bypass --
        # the same standard SUBMITTED/COLLECTED are held to.
        for priv in (True, False):
            with self.subTest(is_private=priv):
                code, _ = g.decide("SUPERSEDED", "none", is_private=priv)
                self.assertEqual(code, g.VIOLATION)

    def test_supersession_does_not_soften_unknown_visibility(self):
        code, _ = g.decide("SUPERSEDED", "route-b-p101", is_private=None)
        self.assertEqual(code, g.UNKNOWN)


class DecideAllowsTheNormalState(unittest.TestCase):
    """The green direction. Without these the guard could be all-red and
    nobody would notice it had stopped discriminating."""

    def test_private_and_prepared_is_compliant(self):
        code, _ = g.decide("PREPARED", "none", is_private=True)
        self.assertEqual(code, g.OK)

    def test_private_and_submitted_with_ticket_is_compliant(self):
        code, _ = g.decide("SUBMITTED", "1234567", is_private=True)
        self.assertEqual(code, g.OK)


class DecideCatchesStaleStatus(unittest.TestCase):
    """The half of status drift a machine can actually detect."""

    def test_submitted_without_ticket_fails(self):
        code, msg = g.decide("SUBMITTED", "none", is_private=True)
        self.assertEqual(code, g.VIOLATION)
        self.assertIn("ticket", msg)

    def test_collected_without_ticket_fails(self):
        code, _ = g.decide("COLLECTED", "n/a", is_private=True)
        self.assertEqual(code, g.VIOLATION)

    def test_placeholder_ticket_values_do_not_count_as_evidence(self):
        for placeholder in ("none", "None", "N/A", "tbd", ""):
            with self.subTest(ticket=placeholder):
                code, _ = g.decide("SUBMITTED", placeholder, is_private=True)
                self.assertEqual(code, g.VIOLATION)


class DecideFailsClosed(unittest.TestCase):
    """A guard that cannot check must not report success."""

    def test_missing_marker_is_a_violation_not_a_pass(self):
        code, msg = g.decide(None, None, is_private=True)
        self.assertEqual(code, g.VIOLATION)
        self.assertIn("marker", msg)

    def test_unrecognised_status_is_a_violation(self):
        code, _ = g.decide("DONE", "1234567", is_private=True)
        self.assertEqual(code, g.VIOLATION)

    def test_unknown_visibility_is_distinct_from_violation(self):
        # Exit 2, not 0 and not 1: an offline run must read as "could not
        # check", never as "assume private" and never as a false alarm.
        code, _ = g.decide("PREPARED", "none", is_private=None)
        self.assertEqual(code, g.UNKNOWN)
        self.assertNotEqual(code, g.OK)


class MarkerParsing(unittest.TestCase):
    def _write(self, text):
        d = Path(tempfile.mkdtemp())
        p = d / "doc.md"
        io.open(p, "w", encoding="utf-8", newline="\n").write(text)
        return p

    def test_parses_the_real_marker_shape(self):
        p = self._write("# t\n\n<!-- publication-block: status=PREPARED ticket=none -->\n")
        self.assertEqual(g.read_marker(p), ("PREPARED", "none"))

    def test_parses_a_submitted_marker_with_a_ticket(self):
        p = self._write("<!-- publication-block: status=SUBMITTED ticket=2851234 -->")
        self.assertEqual(g.read_marker(p), ("SUBMITTED", "2851234"))

    def test_absent_marker_yields_none(self):
        p = self._write("# a document with no marker at all\n")
        self.assertEqual(g.read_marker(p), (None, None))

    def test_missing_file_yields_none_rather_than_raising(self):
        self.assertEqual(g.read_marker(Path("no-such-file-here.md")), (None, None))


class TheShippedDocumentIsMachineReadable(unittest.TestCase):
    """The guard is useless if the document it reads drifts out of shape.
    This asserts against the REAL file, not a fixture."""

    def test_gc_request_carries_a_valid_marker(self):
        status, ticket = g.read_marker(g.GC_REQUEST)
        self.assertIsNotNone(
            status,
            f"{g.GC_REQUEST} lost its publication-block marker; the guard "
            "silently stops guarding when that happens")
        self.assertIn(status, g.VALID_STATUSES)
        self.assertIsNotNone(ticket)


class GuardedByImmutableId(unittest.TestCase):
    """The third hole `claim-check` found: a claim crediting a mechanism that did not exist.

    A comment in the guard justified its name-based design by asserting that
    identity "is only ever asserted by numeric id elsewhere in the pipeline".
    The gate grepped for that and found nothing — an imagined safety net, in
    the file governing the one irreversible action here. A reader auditing
    name-squatting or cross-owner transfer would have acted on that sentence
    and found nothing behind it.

    These tests are the sentence made true. They are also what makes the ids
    load-bearing rather than decorative: without an assertion, the tuple is
    just three numbers nobody checks.
    """

    # Measured 2026-10-21 via `gh api repositories/{id}`; extended 2026-10-24.
    #
    # This pin CAUGHT the 2026-10-24 change, which is what it is for: adding
    # 1151200494 to the guard reddened it with "the pinned ids drifted from the
    # measured ones" rather than being absorbed silently. Updated deliberately,
    # with the reason recorded, rather than loosened to a length or a subset
    # check -- an exact-set assertion is the only version that would notice an
    # id being quietly dropped.
    #
    # Two of the four no longer resolve. They stay pinned because GitHub can
    # RESTORE a deleted repository under its original id, so "deleted" is not
    # terminal and the id entry is the only control that would still see it.
    # Extended 2026-10-25 (p101 phase-2): this pin CAUGHT the route-B change
    # too -- adding 1433482788 and re-annotating 1427819119 reddened it until
    # updated here, deliberately, with the reason recorded.
    EXPECTED = {
        1343217722: "the original",
        1427819119: "the 2026-10 replacement, now PDFE_old3 (host name in "
                     "reachable history; stays private)",
        1433482788: "the FIRST route-B instance, PR numbers burned by "
                     "Dependabot; renamed PDFE-burned-numbering",
        1433896232: "the route-B replacement (second instance), carrying the "
                     "sixth-rewrite history; the publication candidate",
        1427598740: "the burned first attempt (DELETED 2026-10-24)",
        1151200494: "the pre-first-scrub original, PDFE_old (DELETED 2026-10-24)",
    }

    def test_the_guard_addresses_repositories_by_id(self):
        self.assertTrue(hasattr(g, "GUARDED_REPO_IDS"),
                        "the id-addressed set is the authoritative one; "
                        "without it the guard is back to trusting names it is "
                        "itself about to change")
        ids = [i for i, _note in g.GUARDED_REPO_IDS]
        self.assertEqual(sorted(ids), sorted(self.EXPECTED),
                         "the pinned ids drifted from the measured ones")

    def test_every_guarded_id_has_a_reason_recorded(self):
        """A bare id tells a future reader nothing about why it is guarded."""
        for repo_id, note in g.GUARDED_REPO_IDS:
            self.assertTrue(note and note.strip(),
                            f"id {repo_id} is guarded with no stated reason")

    def test_the_id_lookup_helper_exists_and_is_name_independent(self):
        """`GET /repositories/{id}` is the whole point — no slug in the path."""
        self.assertTrue(hasattr(g, "query_repo_by_id"))
        src = inspect.getsource(g.query_repo_by_id)
        self.assertIn("repositories/", src)
        self.assertNotIn("repos/", src,
                         "a name-addressed endpoint here would defeat the "
                         "reason this function exists")

    def test_ids_are_distinct(self):
        ids = [i for i, _ in g.GUARDED_REPO_IDS]
        self.assertEqual(len(ids), len(set(ids)))

    def test_the_rename_script_that_relies_on_ids_is_tracked(self):
        """The deeper version of the same defect.

        Phase-4's preflight asserts identity by id — but it lived only in a
        scratch directory, so from the repository's point of view the
        id-discipline it enforces did not exist either. A safety mechanism that
        is not in the repo is not a safety mechanism.
        """
        script = g.REPO_ROOT / "scripts" / "migration" / "rename_repos.py"
        self.assertTrue(script.exists(),
                        "phase-4's rename preflight must be tracked, not held "
                        "in a scratch directory")
        text = script.read_text(encoding="utf-8", errors="replace")
        self.assertIn("id", text)
        # DECOUPLED from EXPECTED on 2026-10-24, and the reason matters.
        #
        # This loop used to iterate `self.EXPECTED` -- the set of repositories
        # the PUBLICATION GUARD watches. The two sets were identical when both
        # were written, so the coupling looked free. It is not: adding
        # 1151200494 to the guard's coverage reddened this test, which asserts
        # something about a DIFFERENT question -- which repositories the rename
        # script renamed.
        #
        # The guard's watch-set grows when a repository needs watching. The
        # script's act-set is fixed forever: it renamed exactly two
        # repositories, in one phase, and is now spent. `PDFE_old` was never
        # renamed by it -- it was the off-limits third party -- and it no longer
        # exists at all, so requiring its id here would be adding data to a dead
        # script to satisfy an unrelated assertion.
        #
        # Narrowed to the act-set, not loosened: this is still an exact
        # membership check, and it still fails if the script stops pinning
        # either id it renamed.
        RENAMED_IDS = {
            1343217722: "the original, renamed to PDFE_old2",
            1427819119: "the replacement, renamed to PDFE",
        }
        for repo_id in RENAMED_IDS:
            self.assertIn(str(repo_id), text,
                          f"the rename preflight does not pin id {repo_id}, so "
                          "it cannot prove which repository it renamed")


class DirtyTrackerTextBlocksPublication(unittest.TestCase):
    """The `claim-check` finding: clean today, guarded by nothing tomorrow.

    Issue bodies, comments and release notes live in GitHub's database, not in
    git, so every scrub guard was blind to them -- and the migrated tracker's
    cleanliness sat directly upstream of a publication decision with no
    mechanism keeping it true. `decide_text` is that mechanism.

    These live here rather than only in `--self-test` because fast-checks runs
    the guard plainly and never passes `--self-test`; logic covered only by a
    self-test is unguarded in CI.
    """

    CAND = "ipswitch8/PDFE"
    OTHER = "ipswitch8/PDFE-migration"

    def _all_private(self):
        return [(s, g.VIS_PRIVATE) for s in g.GUARDED_SLUGS]

    def test_candidate_public_and_dirty_blocks(self):
        """The irreversible case."""
        results = [(self.CAND, g.VIS_PUBLIC)]
        blocking, note = g.decide_text("COLLECTED", results,
                                       {self.CAND: False})
        self.assertTrue(blocking)
        self.assertIn("scrubbed identifiers", note)

    def test_collected_and_dirty_blocks_even_while_private(self):
        blocking, _ = g.decide_text("COLLECTED", self._all_private(),
                                    {self.CAND: False})
        self.assertTrue(blocking)

    def test_sanctioned_but_unscannable_blocks(self):
        """Unknown must not read as clean -- same rule as visibility."""
        blocking, note = g.decide_text("COLLECTED", self._all_private(),
                                       {self.CAND: None})
        self.assertTrue(blocking)
        self.assertIn("refusing to assume", note)

    def test_candidate_public_and_clean_does_not_block(self):
        blocking, _ = g.decide_text("COLLECTED", [(self.CAND, g.VIS_PUBLIC)],
                                    {self.CAND: True})
        self.assertFalse(blocking)

    def test_private_and_dirty_warns_without_blocking(self):
        """Early warning, not a gate: publication is not happening yet.

        Deliberate: the RETIRED repository is meant to stay private and dirty
        forever, so blocking on dirt while private would make the guard
        permanently red -- which is how a guard becomes noise.
        """
        blocking, note = g.decide_text("PREPARED", self._all_private(),
                                       {self.CAND: False})
        self.assertFalse(blocking)
        self.assertIn("WILL block publication", note)

    def test_private_and_unscannable_does_not_block(self):
        blocking, _ = g.decide_text("PREPARED", self._all_private(),
                                    {self.CAND: None})
        self.assertFalse(blocking)

    # --- the hole `claim-check` found by running the real functions --------

    def test_a_non_candidate_going_public_blocks_outright(self):
        """THE FINDING.

        The first version bound the text check to a fixed slug while visibility
        spanned every guarded slug, and nothing coupled them: with
        PDFE-migration public and the candidate private-and-clean, the guard
        exited 0 while the actually-public repository's text had never been
        scanned. Reachable by one operator sequencing mistake.

        Only the candidate may ever be public. Anything else is a violation
        regardless of what its text says.
        """
        results = [(self.CAND, g.VIS_PRIVATE), (self.OTHER, g.VIS_PUBLIC)]
        blocking, note = g.decide_text("COLLECTED", results,
                                       {self.CAND: True, self.OTHER: True})
        self.assertTrue(blocking, "a public non-candidate must block even when "
                                  "every scanned text is clean")
        self.assertIn(self.OTHER, note)

    def test_the_required_set_is_derived_from_visibility_not_a_fixed_name(self):
        """The coupling itself, asserted directly."""
        required, sanctioned = g.slugs_requiring_clean_text(
            "PREPARED", [(self.OTHER, g.VIS_PUBLIC)])
        self.assertTrue(sanctioned)
        self.assertIn(self.OTHER, required,
                      "the slug that is actually public must be required to be "
                      "clean, whatever PUBLICATION_CANDIDATE happens to name")

    def test_nothing_public_and_not_collected_requires_nothing(self):
        required, sanctioned = g.slugs_requiring_clean_text(
            "PREPARED", self._all_private())
        self.assertFalse(sanctioned)
        self.assertEqual(required, [])

    def test_collected_with_nothing_public_requires_the_candidate(self):
        required, sanctioned = g.slugs_requiring_clean_text(
            "COLLECTED", self._all_private())
        self.assertTrue(sanctioned)
        self.assertEqual(required, [g.PUBLICATION_CANDIDATE])

    def test_the_retired_name_is_guarded_before_it_exists(self):
        """Pre-registered so phase-4's rename opens no unguarded window.

        It 404s today, which the guard reports as `absent` and treats as
        benign; once the rename makes it resolve, it is already covered.
        """
        self.assertTrue(any("PDFE_old2" in s for s in g.GUARDED_SLUGS),
                        "the retired repository's future name must be guarded "
                        "before the rename, or the rename silently un-monitors "
                        "the repo carrying the most exposure")

    def test_the_candidate_is_one_repo_not_the_whole_guarded_set(self):
        """Demanding cleanliness of every guarded slug would never go green."""
        self.assertIsInstance(g.PUBLICATION_CANDIDATE, str)
        self.assertIn(g.PUBLICATION_CANDIDATE, g.GUARDED_SLUGS)
        self.assertGreater(len(g.GUARDED_SLUGS), 1)

    def test_the_scanner_it_depends_on_actually_exists(self):
        """A guard citing a script is only as good as the script being there."""
        script = g.REPO_ROOT.joinpath(*g.TRACKER_SCANNER)
        self.assertTrue(script.exists(),
                        f"{script} is missing; the tracker-text condition "
                        "would silently become unenforceable")


class ItGuardsEveryRepositoryNotJustTheFirst(unittest.TestCase):
    """The defect two gates caught: it reported on repos it never checked.

    `check_publication_block.py` hardcoded one slug and printed "repository is
    private" while querying only `ipswitch8/PDFE` -- so the 2026-10 migration's
    two new repositories were unguarded, including the abandoned first attempt
    that had received a full history push and therefore serves the same
    orphaned objects.

    These live HERE rather than only in `--self-test` because fast-checks runs
    the guard plainly (`python tools/gate/check_publication_block.py`) and does
    NOT pass `--self-test`. Logic covered only by the self-test is unguarded in
    CI -- which is the same shape of defect one level up.
    """

    def test_the_guarded_set_covers_more_than_the_original_repo(self):
        self.assertGreater(
            len(g.GUARDED_SLUGS), 1,
            "the guarded set collapsed back to a single repository; the "
            "migration's new repos would be unguarded again")
        self.assertIn("ipswitch8/PDFE", g.GUARDED_SLUGS)

    def test_the_abandoned_attempt_is_still_guarded_BY_ID_after_deletion(self):
        """Retargeted 2026-10-24, from the slug tuple to the id tuple.

        This assertion used to read `any("abandoned" in s for s in
        GUARDED_SLUGS)`, with the reason "the abandoned first attempt received a
        full history push and serves the same objects; dropping it from the set
        reopens the hole". The owner then DELETED that repository, and the slug
        was dropped from `GUARDED_SLUGS` because a freed name eventually points
        at a stranger's repository.

        Pinning the slug was the weaker choice all along: `GUARDED_SLUGS` is not
        read at runtime -- the guard's own comment records that an AST walk found
        exactly one reference to it, the assignment -- so this test described a
        tuple no guard consults. `main()` iterates `GUARDED_REPO_IDS`.

        The original intent is preserved and strengthened by moving it to the id,
        which survives rename AND deletion: GitHub can restore a deleted
        repository under its original id, so this is the only assertion that
        would still cover it if that happened.
        """
        ids = [i for i, _note in g.GUARDED_REPO_IDS]
        self.assertIn(
            1427598740, ids,
            "the abandoned first attempt received a full history push; it is "
            "deleted, but a restore reuses this id, so dropping the id entry "
            "reopens the hole the slug entry used to cover")

    def test_the_pre_first_scrub_original_is_guarded_by_id(self):
        """The gap this list had until 2026-10-24.

        `PDFE_old` (id 1151200494) is the pre-first-scrub original --
        `docs/legal/publication-route-evaluation-2026-10.md` says it "should be
        assumed to contain the identity data in full, not merely in orphans" --
        and it appeared in NEITHER guarded collection. The dirtiest repository
        of the set was the one nothing watched. It is now deleted and guarded by
        id, so a restore cannot quietly become public.
        """
        ids = [i for i, _note in g.GUARDED_REPO_IDS]
        self.assertIn(1151200494, ids,
                      "the pre-first-scrub original must stay guarded by id; a "
                      "deleted repository can be restored under the same id")

    def test_one_public_repo_is_a_violation_even_among_private_ones(self):
        self.assertIs(
            g.worst_visibility([("a", g.VIS_PRIVATE), ("b", g.VIS_PUBLIC)]),
            False)

    def test_public_outranks_unknown(self):
        """A known violation must not be softened into "cannot tell"."""
        self.assertIs(
            g.worst_visibility([("a", g.VIS_UNKNOWN), ("b", g.VIS_PUBLIC)]),
            False)

    def test_unknown_fails_closed_rather_than_passing(self):
        self.assertIsNone(
            g.worst_visibility([("a", g.VIS_PRIVATE), ("b", g.VIS_UNKNOWN)]))

    def test_absent_is_benign_but_does_not_excuse_unknown(self):
        """A repo that does not exist cannot be public.

        This is what lets entries survive the phase-4 renames instead of
        turning into false alarms -- but it must not launder an UNKNOWN.
        """
        self.assertIs(
            g.worst_visibility([("a", g.VIS_PRIVATE), ("b", g.VIS_ABSENT)]),
            True)
        self.assertIsNone(
            g.worst_visibility([("a", g.VIS_ABSENT), ("b", g.VIS_UNKNOWN)]))

    def test_all_private_satisfies_it(self):
        self.assertIs(
            g.worst_visibility([("a", g.VIS_PRIVATE), ("b", g.VIS_PRIVATE)]),
            True)

    def test_absent_and_unknown_are_distinct_values(self):
        """They were the same `None` before, which is wrong across a list.

        "No such repository" and "could not reach GitHub" must not share a
        verdict: the first cannot be public, the second is genuinely unknown.
        """
        self.assertNotEqual(g.VIS_ABSENT, g.VIS_UNKNOWN)

    def test_aggregate_feeds_decide_correctly(self):
        """End to end: a public repo in the set must block, while private passes."""
        public_mix = g.worst_visibility([("a", g.VIS_PRIVATE), ("b", g.VIS_PUBLIC)])
        code, _ = g.decide("PREPARED", "none", public_mix)
        self.assertEqual(code, g.VIOLATION)

        all_private = g.worst_visibility([("a", g.VIS_PRIVATE), ("b", g.VIS_ABSENT)])
        code, _ = g.decide("PREPARED", "none", all_private)
        self.assertEqual(code, g.OK)


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    unittest.main(verbosity=2)
