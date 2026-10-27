# -*- coding: utf-8 -*-
"""Guard the migration scripts against the defect that phase-4 created.

WHAT WENT WRONG, MEASURED
-------------------------
Phase-4 renamed two repositories past each other. Every migration script that
had been correctly defaulting to `ipswitch8/PDFE` or `ipswitch8/PDFE-migration`
became wrong at that instant -- and silently, because GitHub redirects a
renamed slug: the request succeeds, data comes back, nothing fails.

The `claim-check` gate measured the consequence rather than arguing it: after
the rename, `ipswitch8/PDFE` and `ipswitch8/PDFE-migration` both resolve to id
1427819119. So a default-args `export_tracker.py` followed by
`verify_tracker.py` would have compared the migrated tracker **to itself** --
317 titles matching, 317 body hashes matching, sequence contiguous,
`RESULT: PASS` -- having verified nothing at all.

WHAT THESE TESTS ACTUALLY GUARD
-------------------------------
Not the defaults. A default is correct only until the next rename, and the next
rename will be exactly as quiet as this one. The guard is a three-link chain,
and these tests hold every link **offline**, with no network call:

  1. no migration script names a repository with a string literal -- they all
     go through `repo_ids.default_slug(role)`;
  2. the roles are pinned to distinct immutable ids, so two roles can never
     collapse onto one repository;
  3. `require_distinct()` refuses outright when two slugs resolve to the same
     id, whatever the defaults happen to say.

Link 3 is the load-bearing one and the only one that survives a future rename
with stale defaults, so it is tested against the real phase-4 scenario.

No test here calls GitHub. A network assertion would be slower, would be the
kind of check that reports `cancelled` on timeout and alerts nobody, and would
not add anything: the chain above makes a self-comparison unreachable by
construction rather than unobserved in one sample.
"""
import ast
import importlib.util
import os
import pathlib
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
MIGRATION_DIR = REPO_ROOT / "scripts" / "migration"


def _load_repo_ids():
    path = MIGRATION_DIR / "repo_ids.py"
    spec = importlib.util.spec_from_file_location("pdfe_repo_ids", str(path))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


repo_ids = _load_repo_ids()


def _exploding_resolver(*_a, **_k):
    raise AssertionError(
        "resolve_id was called -- these tests must not touch the network; "
        "inject a resolver instead")


class PinnedIdentity(unittest.TestCase):
    """The ids are the identity. Names are documentation.

    Two generations now (p101, route B): the ACTIVE pins describe the
    2026-10-25 migration; the 2026-10 migration's pins are FROZEN as
    ROLE_IDS_2026_10 because its recorded evidence was produced against them.
    """

    def test_the_active_generation_is_pinned(self):
        # Updated in the SAME COMMIT that filled the replacement pin -- this
        # assertion held `replacement: None` until the fresh repository
        # existed, which is the ratchet that forced the pin and its test to
        # move together. 1433896232 is a measurement
        # (`gh api repos/ipswitch8/PDFE --jq .id`), not a transcription; it is
        # the SECOND fresh instance -- the first (1433482788) had its PR
        # numbers 1-5 burned by Dependabot before the tracker could land, and
        # was renamed PDFE-burned-numbering (see repo_ids.py).
        self.assertEqual(repo_ids.ROLE_IDS, {
            "original": 1427819119,
            "replacement": 1433896232,
        }, "the pinned ids are measurements; changing one silently re-aims "
           "every check in this directory")

    def test_the_2026_10_generation_is_frozen_verbatim(self):
        self.assertEqual(repo_ids.ROLE_IDS_2026_10, {
            "original": 1343217722,
            "replacement": 1427819119,
            "abandoned": 1427598740,
        }, "the prior generation is the record the 2026-10 migration's "
           "evidence was produced against; it must never be edited")

    def test_the_generations_join_only_at_the_documented_promotion(self):
        # The 2026-10 'replacement' IS the route-B 'original' -- that single
        # shared id is the hand-off. Any OTHER overlap would mean a role
        # collapse, which makes a source/target comparison vacuous.
        active = {v for v in repo_ids.ROLE_IDS.values() if v is not None}
        frozen = set(repo_ids.ROLE_IDS_2026_10.values())
        self.assertEqual(active & frozen, {1427819119})

    def test_roles_map_to_distinct_repositories(self):
        ids = [v for v in repo_ids.ROLE_IDS.values() if v is not None]
        self.assertEqual(len(ids), len(set(ids)),
                         "two roles sharing an id would make a source/target "
                         "comparison vacuous no matter how careful the caller")

    def test_every_role_has_a_current_slug_entry(self):
        self.assertEqual(set(repo_ids.ROLE_IDS),
                         set(repo_ids.ROLE_SLUGS_CURRENT))

    def test_source_and_target_defaults_are_different_repositories(self):
        # The whole vacuity hazard in one assertion, offline. Restored after
        # phase-2 filled the replacement pin (it was unanswerable while the
        # replacement did not exist).
        self.assertNotEqual(repo_ids.default_slug("original"),
                            repo_ids.default_slug("replacement"))

    def test_an_uncreated_role_refuses_a_default_loudly(self):
        # The None-pin refusal must survive for the NEXT migration generation,
        # even though no active role is None today. Patch the module state to
        # the shape phase-1 had, and restore in finally.
        saved_ids = dict(repo_ids.ROLE_IDS)
        saved_slugs = dict(repo_ids.ROLE_SLUGS_CURRENT)
        try:
            repo_ids.ROLE_IDS["replacement"] = None
            repo_ids.ROLE_SLUGS_CURRENT["replacement"] = None
            with self.assertRaises(repo_ids.RepoIdentityError) as ctx:
                repo_ids.default_slug("replacement")
            self.assertIn("does not exist yet", str(ctx.exception))
        finally:
            repo_ids.ROLE_IDS.clear()
            repo_ids.ROLE_IDS.update(saved_ids)
            repo_ids.ROLE_SLUGS_CURRENT.clear()
            repo_ids.ROLE_SLUGS_CURRENT.update(saved_slugs)

    def test_unknown_role_is_refused_rather_than_defaulted(self):
        with self.assertRaises(repo_ids.RepoIdentityError):
            repo_ids.default_slug("new")


class RequireDistinctRefusesSelfComparison(unittest.TestCase):
    """Link 3: the check that survives stale defaults and future renames."""

    def test_the_actual_phase4_scenario_is_refused(self):
        # Exactly what the old defaults produced: two different names, one
        # repository, because the interim name redirects to the replacement.
        both = {"ipswitch8/PDFE": 1427819119,
                "ipswitch8/PDFE-migration": 1427819119}
        with self.assertRaises(repo_ids.RepoIdentityError) as ctx:
            repo_ids.require_distinct("ipswitch8/PDFE",
                                      "ipswitch8/PDFE-migration",
                                      resolver=both.__getitem__)
        msg = str(ctx.exception)
        self.assertIn("1427819119", msg)
        self.assertIn("verifies nothing", msg)
        self.assertIn("two NAMES for one repository", msg,
                      "the message must explain WHY two distinct-looking slugs "
                      "are one repository, or the operator re-runs it")

    def test_identical_slugs_are_also_refused(self):
        with self.assertRaises(repo_ids.RepoIdentityError):
            repo_ids.require_distinct("a/b", "a/b", resolver=lambda _s: 7)

    def test_two_genuine_repositories_are_allowed_through(self):
        ids = {"a/old": 1343217722, "a/new": 1427819119}
        self.assertEqual(
            repo_ids.require_distinct("a/old", "a/new",
                                      resolver=ids.__getitem__),
            (1343217722, 1427819119))

    def test_it_does_not_consult_the_pinned_ids(self):
        # Must keep working after the NEXT rename, when the pins may describe
        # different names. Two ids in no pinned role at all are fine.
        ids = {"x/one": 111, "x/two": 222}
        repo_ids.require_distinct("x/one", "x/two", resolver=ids.__getitem__)

    def test_a_default_resolver_is_never_reached_when_one_is_given(self):
        saved = repo_ids.resolve_id
        repo_ids.resolve_id = _exploding_resolver
        try:
            ids = {"a/old": 1, "a/new": 2}
            repo_ids.require_distinct("a/old", "a/new",
                                      resolver=ids.__getitem__)
        finally:
            repo_ids.resolve_id = saved


class ExpectRoleCatchesARedirect(unittest.TestCase):
    """Link 2: a slug that redirects to the wrong repository fails loudly."""

    def test_wrong_repository_is_refused_and_named(self):
        # A slug for the route-B original that actually resolves to the
        # PERMANENTLY-PRIVATE archive (the 2026-10 original) must refuse,
        # naming both ids, so the operator sees which repository it hit.
        with self.assertRaises(repo_ids.RepoIdentityError) as ctx:
            repo_ids.expect_role("ipswitch8/PDFE", "original",
                                 resolver=lambda _s: 1343217722)
        msg = str(ctx.exception)
        self.assertIn("1343217722", msg)
        self.assertIn("1427819119", msg)

    def test_a_retired_name_says_so(self):
        with self.assertRaises(repo_ids.RepoIdentityError) as ctx:
            repo_ids.expect_role("ipswitch8/PDFE-migration", "original",
                                 resolver=lambda _s: 1343217722)
        self.assertIn("RETIRED", str(ctx.exception))

    def test_an_uncreated_role_is_refused_before_any_resolution(self):
        # `None != got` must not fall through to a nonsense "the role is id
        # None" mismatch; the refusal has to say the repository does not exist
        # -- and it must happen BEFORE any resolution (exploding resolver).
        # The active generation has no None pin any more, so patch one in and
        # restore in finally: the branch protects the NEXT migration.
        saved = dict(repo_ids.ROLE_IDS)
        try:
            repo_ids.ROLE_IDS["replacement"] = None
            with self.assertRaises(repo_ids.RepoIdentityError) as ctx:
                repo_ids.expect_role("a/whatever", "replacement",
                                     resolver=_exploding_resolver)
            self.assertIn("does not exist yet", str(ctx.exception))
        finally:
            repo_ids.ROLE_IDS.clear()
            repo_ids.ROLE_IDS.update(saved)

    def test_the_right_repository_passes(self):
        self.assertEqual(
            repo_ids.expect_role("ipswitch8/PDFE", "original",
                                 resolver=lambda _s: 1427819119),
            1427819119)

    def test_unknown_role_is_refused(self):
        with self.assertRaises(repo_ids.RepoIdentityError):
            repo_ids.expect_role("a/b", "nope", resolver=lambda _s: 1)


class StoredIdentityIsAnIdNotAName(unittest.TestCase):
    """A stored NAME goes stale exactly like a stored default -- and did.

    `build/migration/data/tracker-export.json`, the real phase-1 capture of the
    ORIGINAL, records `"repo": "ipswitch8/PDFE"`. Phase-4 handed that name to
    the REPLACEMENT, so resolving it makes a genuinely-good export look like a
    self-comparison. It fails closed, which is the right direction and still
    the wrong reason: the export should have recorded the id.
    """

    def test_require_distinct_ids_refuses_equal_ids(self):
        with self.assertRaises(repo_ids.RepoIdentityError) as ctx:
            repo_ids.require_distinct_ids(1427819119, 1427819119)
        self.assertIn("verifies nothing", str(ctx.exception))

    def test_require_distinct_ids_allows_two_repositories(self):
        self.assertEqual(
            repo_ids.require_distinct_ids(1343217722, 1427819119),
            (1343217722, 1427819119))

    def test_require_distinct_ids_labels_both_sides(self):
        # The operator has to be able to tell WHICH side was wrong.
        with self.assertRaises(repo_ids.RepoIdentityError) as ctx:
            repo_ids.require_distinct_ids(7, 7, source_label="the export",
                                          target_label="the live target")
        msg = str(ctx.exception)
        self.assertIn("the export", msg)
        self.assertIn("the live target", msg)

    def test_require_distinct_ids_needs_no_network(self):
        saved = repo_ids.resolve_id
        repo_ids.resolve_id = _exploding_resolver
        try:
            repo_ids.require_distinct_ids(1, 2)
        finally:
            repo_ids.resolve_id = saved

    def test_the_exporter_records_the_resolved_id(self):
        src = (MIGRATION_DIR / "export_tracker.py").read_text(encoding="utf-8")
        tree = ast.parse(src)
        keys = set()
        for node in ast.walk(tree):
            if isinstance(node, ast.Dict):
                for k in node.keys:
                    if isinstance(k, ast.Constant) and isinstance(k.value, str):
                        keys.add(k.value)
        self.assertIn("repo_id", keys,
                      "the export must record its source by id, or every "
                      "later verification has to resolve a name that a rename "
                      "may already have moved")

    def test_the_verifier_hard_fails_on_an_export_with_no_id(self):
        # NOT a fallback to default_slug("original"). A fallback on the source
        # side is how a self-comparison in substance passes a distinctness
        # check in form: an export genuinely taken from the TARGET, carrying no
        # id, would fall back to a DIFFERENT slug, clear the check, and verify
        # nothing. Asserted on the AST so a reworded message cannot break it
        # and a deleted guard cannot survive it.
        tree = ast.parse((MIGRATION_DIR / "verify_tracker.py")
                         .read_text(encoding="utf-8"))
        raisers = [n for n in ast.walk(tree)
                   if isinstance(n, ast.Raise)
                   and isinstance(n.exc, ast.Call)
                   and isinstance(n.exc.func, ast.Name)
                   and n.exc.func.id == "SystemExit"]
        self.assertTrue(raisers, "the verifier must be able to refuse at all")
        joined = (MIGRATION_DIR / "verify_tracker.py").read_text(encoding="utf-8")
        self.assertIn("repo_id", joined,
                      "the verifier must read the recorded id")
        # The specific anti-pattern: no default_slug fallback on the SOURCE id.
        self.assertNotIn('old_id = src.get("repo_id") or', joined,
                         "a defaulted source id re-opens the vacuity hole")

    def test_a_bool_is_not_a_repository_id(self):
        # THE bug this predicate was extracted for. `bool` subclasses `int`, so
        # `isinstance(True, int)` is True and an export carrying
        # `"repo_id": true` passed the original inline check and became id 1.
        #
        # What happened next was not what I predicted: I assumed
        # `GET /repositories/1` would 404 and refuse. `claim-check` checked --
        # id 1 is a real repository, so the call SUCCEEDED and the run failed
        # later on the content comparison. The safety property held for a
        # different reason than the code claimed, which is why this is now an
        # explicit predicate instead of a comment asserting it cannot happen.
        self.assertFalse(repo_ids.valid_repo_id(True))
        self.assertFalse(repo_ids.valid_repo_id(False))

    def test_only_a_positive_integer_is_a_repository_id(self):
        for bad in (0, -1, -1343217722, "1343217722", 12.0, None, [], {},
                    "", "abc"):
            self.assertFalse(repo_ids.valid_repo_id(bad),
                             "%r must not be accepted as a repository id" % (bad,))
        for good in (1, 1343217722, 1427819119, 1427598740):
            self.assertTrue(repo_ids.valid_repo_id(good))

    def test_the_verifier_uses_the_shared_predicate(self):
        # Not an inline isinstance() -- an inline check cannot be unit-tested
        # without building an export file, and that is precisely why the bool
        # case went uncovered for as long as it did.
        tree = ast.parse((MIGRATION_DIR / "verify_tracker.py")
                         .read_text(encoding="utf-8"))
        calls = [n for n in ast.walk(tree)
                 if isinstance(n, ast.Call)
                 and isinstance(n.func, ast.Attribute)
                 and n.func.attr == "valid_repo_id"]
        self.assertTrue(calls,
                        "the verifier must validate the recorded id through "
                        "repo_ids.valid_repo_id, so the predicate is testable")

    def test_the_refusal_distinguishes_absent_from_invalid(self):
        # "records no repo_id" is a false statement about a file that records
        # `true` or `0`, and those are the cases the predicate was widened for.
        src = (MIGRATION_DIR / "verify_tracker.py").read_text(encoding="utf-8")
        self.assertIn('if "repo_id" not in src:', src,
                      "the refusal must tell an older export apart from a "
                      "hand-edited one -- they have different remedies")

    def test_the_verifier_resolves_the_name_forward_from_the_id(self):
        tree = ast.parse((MIGRATION_DIR / "verify_tracker.py")
                         .read_text(encoding="utf-8"))
        calls = [n for n in ast.walk(tree)
                 if isinstance(n, ast.Call)
                 and isinstance(n.func, ast.Attribute)
                 and n.func.attr == "slug_for_id"]
        self.assertTrue(calls,
                        "API paths need a name, and the name must come from "
                        "the id -- not from the name the export stored, which "
                        "may since have been handed to another repository")


class NoScriptNamesARepositoryDirectly(unittest.TestCase):
    """Link 1: the recurrence guard.

    This is the test that would have failed the moment phase-4's rename landed,
    had it existed -- and it is the one that will fail at the NEXT rename
    instead of redirecting quietly. Comments are not in the AST, so the
    explanatory comments added alongside the fixes do not trip it; docstrings
    are excluded deliberately, because prose describing the history must stay
    free to name the old repositories.
    """

    EXEMPT = {
        # The one place the names are allowed to live: it is what every other
        # script asks, and it pins each name to an id so the name is checkable.
        "repo_ids.py": "owns the name <-> id mapping",
    }

    def _string_constants_outside_docstrings(self, path):
        tree = ast.parse(path.read_text(encoding="utf-8"))
        docstrings = set()
        for node in ast.walk(tree):
            if isinstance(node, (ast.Module, ast.FunctionDef,
                                 ast.AsyncFunctionDef, ast.ClassDef)):
                doc = ast.get_docstring(node, clean=False)
                if doc:
                    docstrings.add(doc)
        return [n.value for n in ast.walk(tree)
                if isinstance(n, ast.Constant)
                and isinstance(n.value, str)
                and n.value not in docstrings]

    def test_migration_scripts_exist_to_be_checked(self):
        # An empty glob would make every assertion below vacuously true. This
        # project has shipped a scanner that scanned 245 of 3,490 files and
        # reported OK; the fix is to assert the population, not to trust it.
        scripts = sorted(p.name for p in MIGRATION_DIR.glob("*.py"))
        self.assertGreaterEqual(len(scripts), 8,
                                "found only %r -- the scan population is "
                                "wrong, so its result means nothing" % scripts)
        for required in ("export_tracker.py", "verify_tracker.py",
                         "probe_orphans.py", "scan_tracker_text.py",
                         "redact_tracker_text.py", "check_update_feed.py",
                         "rename_repos.py", "repo_ids.py"):
            self.assertIn(required, scripts)

    def test_no_script_hardcodes_a_repository_slug(self):
        offenders = {}
        for path in sorted(MIGRATION_DIR.glob("*.py")):
            if path.name in self.EXEMPT:
                continue
            hits = sorted({s for s in self._string_constants_outside_docstrings(path)
                           if "ipswitch8/" in s})
            if hits:
                offenders[path.name] = hits
        self.assertEqual(offenders, {},
                         "these scripts name a repository by string literal. A "
                         "literal is correct only until the next rename, and a "
                         "rename does not fail -- GitHub redirects. Use "
                         "repo_ids.default_slug(role) instead.")

    def test_the_exemption_is_justified_and_minimal(self):
        for name in self.EXEMPT:
            self.assertTrue((MIGRATION_DIR / name).exists(),
                            "%s is exempted but does not exist -- a stale "
                            "exemption silently stops guarding a real file"
                            % name)
            self.assertTrue(self.EXEMPT[name].strip())
        self.assertEqual(len(self.EXEMPT), 1)

    def test_every_comparing_script_imports_the_identity_module(self):
        # A script that compares two repositories, or acts on one by name, must
        # route through repo_ids -- otherwise link 1 holds and links 2 and 3 are
        # simply never called, which is the "mechanism credited but not wired"
        # shape this gate has already caught twice in this pipeline.
        for name in ("export_tracker.py", "verify_tracker.py",
                     "probe_orphans.py", "scan_tracker_text.py",
                     "redact_tracker_text.py", "check_update_feed.py"):
            src = (MIGRATION_DIR / name).read_text(encoding="utf-8")
            self.assertIn("import repo_ids", src,
                          "%s does not import the identity module" % name)

    def test_the_two_repository_scripts_call_require_distinct(self):
        # Checked through the AST, as a CALL, not as a substring of the source.
        # The first version of this test searched the text for
        # "require_distinct" and stayed green when the call was deleted --
        # because the word survived in the comment explaining the call. A grep
        # hit is not a mechanism; this project has the scar and a memory note
        # to match. Mutation-tested: deleting the call turns this RED.
        # `verify_tracker.py` uses the *_ids variant because its source
        # identity comes from the export file rather than a live name lookup;
        # `probe_orphans.py` resolves two slugs and uses the name variant.
        # Either satisfies the requirement -- what must not happen is neither.
        for name in ("verify_tracker.py", "probe_orphans.py"):
            tree = ast.parse((MIGRATION_DIR / name).read_text(encoding="utf-8"))
            calls = [n for n in ast.walk(tree)
                     if isinstance(n, ast.Call)
                     and isinstance(n.func, ast.Attribute)
                     and n.func.attr in ("require_distinct",
                                         "require_distinct_ids")
                     and isinstance(n.func.value, ast.Name)
                     and n.func.value.id == "repo_ids"]
            self.assertTrue(
                calls,
                "%s compares two repositories, so it must CALL "
                "repo_ids.require_distinct and refuse to compare one with "
                "itself. No such call node is present." % name)
            # Two positional arguments, or it is not comparing two things.
            for call in calls:
                self.assertGreaterEqual(len(call.args), 2,
                                        "%s: require_distinct needs both "
                                        "repositories" % name)

    def test_the_writer_has_no_default_target(self):
        # redact_tracker_text.py PATCHes issue bodies. Phase-4 moved the
        # repository its default named; a convenient default on a writer whose
        # target changed underneath it is how an archive gets written to.
        src = (MIGRATION_DIR / "redact_tracker_text.py").read_text(encoding="utf-8")
        tree = ast.parse(src)
        found = False
        for node in ast.walk(tree):
            if not isinstance(node, ast.Call):
                continue
            if not (isinstance(node.func, ast.Attribute)
                    and node.func.attr == "add_argument"):
                continue
            if not (node.args and isinstance(node.args[0], ast.Constant)
                    and node.args[0].value == "--repo"):
                continue
            found = True
            kw = {k.arg: k.value for k in node.keywords}
            self.assertNotIn("default", kw,
                             "--repo must not have a default on a script that "
                             "writes to the repository it is given")
            self.assertIn("required", kw)
            self.assertIs(kw["required"].value, True)
        self.assertTrue(found, "did not find the --repo argument to check")


if __name__ == "__main__":
    unittest.main(verbosity=2)
