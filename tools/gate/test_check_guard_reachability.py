# -*- coding: utf-8 -*-
"""Make `check_guard_reachability.py`'s own claims enforceable.

    python -m pytest tools/gate/test_check_guard_reachability.py -q

WHY THIS FILE EXISTS, IN THE GUARD'S OWN WORDS.

`check_guard_reachability.py` is the standing control for the half of test-gap
class C-BELOW where **the control itself is unreachable** (GitHub #255). Its
fourteen self-test cases are invoked only by `--self-test`, a manual CLI flag.
Until this file existed, nothing ran them: not `pytest tools/gate`, not CI. The
phase-1 `claim-check` gate found it, and the finding is worth stating plainly —
**the guard against unreachable guards was itself unreachable.** A regression in
its prefix-scoping refusal, its DCA-0052 import, its fail-open refusal or any of
its four mention-traps would have reddened nothing.

So this file is not decoration around a self-test. It is the mechanism that
makes the self-test a control rather than a diagnostic.

WHAT IT ASSERTS, BEYOND "THE SELF-TEST PASSES".

A self-test is written by whoever holds the blind spot, so a green run is weak
evidence about itself. Three of these tests therefore MUTATE the guard and
require the specifically-named case to go red. That distinction matters: a
mutation that reddens *some* case only proves the suite noticed; a mutation that
reddens *the case named after the broken function* proves the case measures what
its label claims. One of them is a regression test for a real defect — the
`comment-mention-not-reachable` case used to pass with `strip_shell_comments`
no-opped, because its fixture used a plain YAML scalar that PyYAML stripped
first. It was a vacuous oracle, and this is what stops it becoming one again.
"""
from __future__ import annotations

import importlib.util
import io
import os
import re
import sys
import tempfile

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(HERE))
GUARD = os.path.join(HERE, "check_guard_reachability.py")

#: Every case label the self-test is contracted to report. Listed here rather
#: than derived from the output, so DELETING a case fails this file instead of
#: silently shrinking the contract -- the failure mode a "count the cases"
#: assertion cannot see.
EXPECTED_CASES = (
    "non-check-prefixed-guard-in-population",
    "import-stem-reachability",
    "dispatch-only-wiring-not-reachable",
    "comment-mention-not-reachable",
    "docstring-mention-not-reachable",
    "always-false-if-not-reachable",
    "indirect-via-pytest-reachable",
    "indirect-via-reached-guard-reachable",
    "unparseable-workflow-is-an-error-not-a-pass",
    "ratchet-healed-entry-must-be-removed",
    "ratchet-deleted-entry-must-be-removed",
    "baseline-kind-outside-taxonomy-is-an-error",
    "admitted-gap-without-issue-is-an-error",
    "well-formed-baseline-still-passes",
)


#: Number words worth catching. Beyond "six" the phrasing stops being a tally
#: someone would write by hand about a set this size.
_NUMBER = r"(?:\d+|one|two|three|four|five|six)"

#: Prose that commits to HOW MANY there are.
#:
#: KNOWN LIMIT, stated because the alternative is a false claim of closure.
#: **This is a heuristic net over common phrasings, not a proof of absence.**
#: A passing test means no COMMON spelling of a tally was found; it does not
#: mean none is stated. `karen` demonstrated the gap twice: a fixed phrase list
#: fell to "There are 3 kinds of baseline entry", and this wider regex falls to
#: "The taxonomy comes in three flavors" — along with "a trio of kinds",
#: "cardinality of three", and any synonym outside the noun list.
#:
#: It is deliberately NOT widened further. Project memory is explicit that a
#: sweep keyed on PHRASING always comes back (`pipeline_f20_f25`), and each
#: widening is a bigger denylist wearing the costume of a structural rule. The
#: real mechanism in this file is the one above it — the kind NAMES compared
#: against `BASELINE_KINDS` in both directions, which is structural and cannot
#: be paraphrased around. This net is a cheap extra, and is worth exactly what
#: a net is worth.
#:
#: Also deliberately not matching a bare "both": ordinary English ("in both
#: directions") tripped it, and a rule that cries wolf trains the reader to
#: edit the rule instead of the drift.
_COUNT_PHRASE = re.compile(
    r"\b(?:only\s+%s\b|%s\s+(?:kinds?|things?|members?|entries|values?)\b|"
    r"all\s+%s\s+(?:kinds?|of\s+them)\b)" % (_NUMBER, _NUMBER, _NUMBER), re.I)


def _states_a_count(text):
    """Phrases in `text` that assert the size of the taxonomy."""
    return sorted({m.group(0).strip() for m in _COUNT_PHRASE.finditer(text)})


def _fresh_module():
    """A pristine import, so a mutating test cannot leak into another."""
    spec = importlib.util.spec_from_file_location("_cgr_under_test", GUARD)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _run_self_test(mod):
    """Return (exit_code, stdout). The self-test prints; capture it."""
    buf = io.StringIO()
    saved = sys.stdout
    sys.stdout = buf
    try:
        rc = mod.self_test()
    finally:
        sys.stdout = saved
    return rc, buf.getvalue()


def _reddened(output):
    """Case labels reported FAIL, ignoring the summary line."""
    out = set()
    for line in output.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[1] == "FAIL" and not parts[0].startswith("["):
            out.add(parts[0])
    return out


# --------------------------------------------------------------------------
# the self-test is a control, and it runs here
# --------------------------------------------------------------------------

def test_self_test_passes():
    rc, out = _run_self_test(_fresh_module())
    assert rc == 0, "self-test must pass on an unmodified tree:\n%s" % out


def test_every_contracted_case_is_present_and_passing():
    _, out = _run_self_test(_fresh_module())
    missing = [c for c in EXPECTED_CASES if c not in out]
    assert not missing, (
        "the self-test no longer reports these cases, so the contract shrank "
        "without anything failing: %s" % missing)
    assert not _reddened(out), "cases reported FAIL: %s" % sorted(_reddened(out))


# --------------------------------------------------------------------------
# mutation: each named case must redden for ITS OWN reason
# --------------------------------------------------------------------------

@pytest.mark.parametrize("case,mutate", [
    # The regression test for the vacuous-oracle defect. Before the fixture was
    # changed to a block scalar, PyYAML stripped the `#` comment itself and this
    # mutant SURVIVED.
    ("comment-mention-not-reachable",
     lambda m: setattr(m, "strip_shell_comments", lambda text: text)),
    ("always-false-if-not-reachable",
     lambda m: setattr(m, "ALWAYS_FALSE", re.compile(r"^(?!)"))),
    ("baseline-kind-outside-taxonomy-is-an-error",
     lambda m: setattr(m, "BASELINE_KINDS",
                       frozenset(("one-shot-phase-verifier", "admitted-gap",
                                  "wontfix")))),
])
def test_disabling_a_rule_reddens_the_case_named_after_it(case, mutate):
    mod = _fresh_module()
    mutate(mod)
    rc, out = _run_self_test(mod)
    assert rc != 0, "the mutant survived the whole self-test:\n%s" % out
    assert case in _reddened(out), (
        "disabling this rule reddened %s but NOT %r, so that case passes for "
        "some reason other than the one its name claims -- which is the "
        "vacuous-oracle shape this file exists to prevent."
        % (sorted(_reddened(out)) or "nothing", case))


def test_a_docstring_mention_is_not_an_invocation():
    """The finer half of `strip_python`: prose out, call sites kept."""
    mod = _fresh_module()
    body = mod.strip_python(
        '"""See parity_filter.py for the other half."""\n'
        'import subprocess\n'
        'subprocess.run(["python", "tools/gate/scrub_artifacts.py"])\n')
    assert not mod.mentions(body, "parity_filter.py"), \
        "a guard named only in a docstring was counted as an invocation"
    assert mod.mentions(body, "scrub_artifacts.py"), \
        "a guard named in a real call was discarded along with the prose"


def test_an_import_stem_counts_as_an_invocation():
    """`import x` names no `.py`, and missing it over-reports orphans."""
    mod = _fresh_module()
    body = mod.strip_python("import fz_try_raii_scan as scan\n")
    assert mod.mentions(body, "fz_try_raii_scan.py")


def test_a_longer_name_does_not_vouch_for_a_shorter_one():
    """Stem matching is word-bounded, or one guard silently covers another."""
    mod = _fresh_module()
    assert not mod.mentions("check_page_pointer_rebind", "check_page.py")


# --------------------------------------------------------------------------
# the live tree
# --------------------------------------------------------------------------

def test_population_is_not_scoped_by_the_check_prefix():
    """#187's defect: 4 of its 7 scripts were not named check_*."""
    mod = _fresh_module()
    population = set(mod.discover_guards(REPO_ROOT))
    assert population, "no guards discovered at all"
    non_prefixed = {p for p in population if not p.startswith("check_")}
    assert non_prefixed, (
        "every discovered guard starts with check_, so a prefix filter has "
        "crept back in and #187's four non-check_ scripts would escape")


def test_no_companion_or_fixture_is_counted_as_a_guard():
    mod = _fresh_module()
    population = set(mod.discover_guards(REPO_ROOT))
    assert not [p for p in population if p.startswith("test_")]


def test_the_trigger_rule_is_imported_rather_than_restated():
    """DCA 0052: a rule implemented twice, with one copy fixed, diverges."""
    mod = _fresh_module()
    assert mod._triggers_module().AUTO_TRIGGERS == (
        "push", "pull_request", "pull_request_target")
    source = io.open(GUARD, encoding="utf-8").read()
    assert "AUTO_TRIGGERS = (" not in source, (
        "check_guard_reachability.py now defines its own AUTO_TRIGGERS; it "
        "must import the one in check_workflow_triggers.py")


def test_the_live_baseline_satisfies_its_own_rules():
    """Every entry typed, reasoned, and — if an admitted gap — issue-bearing."""
    mod = _fresh_module()
    entries = mod.load_baseline(REPO_ROOT)
    assert entries, "the live baseline is empty"
    for name, ent in entries.items():
        assert ent["kind"] in mod.BASELINE_KINDS, name
        if ent["kind"] == "admitted-gap":
            assert str(ent.get("issue", "")).strip(), name


# --------------------------------------------------------------------------
# mutation on synthetic trees: the control must REDDEN, one shape at a time
#
# Every test below builds its own tree under tmp_path and never touches the
# live `.github/workflows` or `tools/gate`. That is what makes the proof
# runnable on a normal checkout: demonstrating that a guard-reachability
# control works must not require breaking real CI to find out.
#
# The self-test covers the same shapes from inside the module. These exist
# because a self-test is written by whoever holds the blind spot: here the
# tree is built by the test, the wiring is removed by the test, and the
# assertion is that the EXIT CODE flips. A control that cannot be reddened is
# decoration.
# --------------------------------------------------------------------------

_STUB = "print('x')\n"


def _wf(steps, auto=True):
    trigger = "  push:\n    branches: [master]\n" if auto else "  workflow_dispatch:\n"
    return ("name: w\non:\n%sjobs:\n  j:\n    runs-on: ubuntu-latest\n    steps:\n%s"
            % (trigger, steps))


def _build(mod, root, workflows, guards, companions=None, baseline=None):
    """A synthetic repo under `root`. Returns the module's check() result."""
    mod._tree(str(root), workflows, guards, companions, baseline)
    return mod.check(str(root))


def _wired_tree(mod, root, guard="parity_filter.py", **kw):
    """Baseline scenario: `guard` IS invoked, so check() passes."""
    return _build(mod, root, {"a.yml": _wf(
        mod._step("python tools/gate/check_a.py\n"
                  "python tools/gate/%s" % guard))},
        {"check_a.py": _STUB, guard: _STUB},
        baseline=mod._baseline(), **kw)


def test_mutation_a_wired_guard_passes_first():
    """The control case. Without it, every assertion below is satisfiable by
    a checker that simply rejects everything."""
    mod = _fresh_module()
    with tempfile.TemporaryDirectory() as d:
        rc, lines = _wired_tree(mod, d)
    assert rc == 0, "the unmutated tree must PASS:\n%s" % "\n".join(lines)


def test_mutation_unwiring_a_guard_flips_pass_to_fail():
    mod = _fresh_module()
    with tempfile.TemporaryDirectory() as d:
        rc, lines = _build(mod, d,
            {"a.yml": _wf(mod._step("python tools/gate/check_a.py"))},
            {"check_a.py": _STUB, "parity_filter.py": _STUB},
            baseline=mod._baseline())
    assert rc != 0 and "parity_filter.py" in "\n".join(lines)


def test_mutation_moving_the_invoker_to_dispatch_only_flips_pass_to_fail():
    """The exact #187 shape: the guard is still named, in a workflow nobody
    triggers. A control that only asks 'is it named anywhere?' passes this."""
    mod = _fresh_module()
    with tempfile.TemporaryDirectory() as d:
        rc, lines = _build(mod, d,
            {"a.yml": _wf(mod._step("python tools/gate/check_a.py")),
             "m.yml": _wf(mod._step("python tools/gate/parity_filter.py"), auto=False)},
            {"check_a.py": _STUB, "parity_filter.py": _STUB},
            baseline=mod._baseline())
    assert rc != 0 and "parity_filter.py" in "\n".join(lines)


def test_mutation_a_comment_only_mention_does_not_rescue_it():
    mod = _fresh_module()
    with tempfile.TemporaryDirectory() as d:
        rc, lines = _build(mod, d,
            {"a.yml": _wf(mod._step(
                "python tools/gate/check_a.py  # see also parity_filter.py"))},
            {"check_a.py": _STUB, "parity_filter.py": _STUB},
            baseline=mod._baseline())
    assert rc != 0 and "parity_filter.py" in "\n".join(lines)


def test_mutation_a_docstring_only_mention_does_not_rescue_it():
    mod = _fresh_module()
    with tempfile.TemporaryDirectory() as d:
        rc, lines = _build(mod, d,
            {"a.yml": _wf(mod._step("python tools/gate/check_a.py"))},
            {"check_a.py": '"""Companion to parity_filter.py."""\n',
             "parity_filter.py": _STUB},
            baseline=mod._baseline())
    assert rc != 0 and "parity_filter.py" in "\n".join(lines)


def test_mutation_an_always_false_if_does_not_rescue_it():
    mod = _fresh_module()
    with tempfile.TemporaryDirectory() as d:
        rc, lines = _build(mod, d,
            {"a.yml": _wf(mod._step("python tools/gate/check_a.py")
                          + mod._step("python tools/gate/parity_filter.py",
                                      cond="${{ false }}"))},
            {"check_a.py": _STUB, "parity_filter.py": _STUB},
            baseline=mod._baseline())
    assert rc != 0 and "parity_filter.py" in "\n".join(lines)


def test_mutation_an_unparseable_workflow_is_an_error_not_a_silent_pass():
    """C-FAILOPEN: an unmeasurable condition must never read as green."""
    mod = _fresh_module()
    with tempfile.TemporaryDirectory() as d:
        rc, lines = _build(mod, d,
            {"a.yml": _wf(mod._step("python tools/gate/check_a.py")),
             "broken.yml": "on: [push]\njobs:\n  j:\n    steps:\n   - bad\n  x: ["},
            {"check_a.py": _STUB}, baseline=mod._baseline())
    blob = "\n".join(lines)
    assert rc != 0, "an unparseable workflow passed silently"
    assert "broken.yml" in blob, "the error does not name the file: %s" % blob


def test_mutation_a_non_check_prefixed_guard_is_caught_identically():
    """#187's four non-`check_*` scripts. A prefix-scoped control misses these.

    Both directions, because catching everything is as useless as catching
    nothing: unwired must FAIL, and wired must PASS.
    """
    mod = _fresh_module()
    with tempfile.TemporaryDirectory() as d:
        rc, lines = _build(mod, d,
            {"a.yml": _wf(mod._step("python tools/gate/check_a.py"))},
            {"check_a.py": _STUB, "scrub_artifacts.py": _STUB},
            baseline=mod._baseline())
    assert rc != 0 and "scrub_artifacts.py" in "\n".join(lines), \
        "a guard without the check_ prefix escaped the population"

    with tempfile.TemporaryDirectory() as d:
        rc, lines = _wired_tree(mod, d, guard="scrub_artifacts.py")
    assert rc == 0, "a wired non-check_ guard was wrongly flagged"


def test_mutation_a_guard_reached_only_by_its_own_companion_is_unreached():
    """The closure half of the C-GUARDSELF fix, with its own named test.

    Deliberately distinct from `test_a_guard_is_not_credited_to_its_own_...`
    above, which exercises `mentions()` — the left-anchor half. Removing the
    CLOSURE rule reddened only the live-tree sweep, so the rule was covered but
    not by anything named after it. A case that passes for a reason other than
    the one its name claims is the defect this pipeline keeps finding, so the
    two halves now have one test each.

    Here `parity_filter.py` is named by nothing except `test_parity_filter.py`,
    which the workflow does run. That is a testing relationship, not an
    invocation: nothing executes the guard, so it must read UNREACHED.
    """
    mod = _fresh_module()
    with tempfile.TemporaryDirectory() as d:
        rc, lines = _build(mod, d,
            {"a.yml": _wf(mod._step("pytest tools/gate/test_parity_filter.py"))},
            {"parity_filter.py": _STUB},
            {"test_parity_filter.py": "import parity_filter\n"},
            baseline=mod._baseline())
    assert rc != 0 and "parity_filter.py" in "\n".join(lines), (
        "a guard vouched for by its OWN companion test read as reached; the "
        "control's reference is not independent of what it audits")


def test_mutation_another_guards_test_still_counts_as_wiring():
    """The converse, so the rule above is narrow rather than blanket.

    `test_other.py` is not `parity_filter.py`'s companion, so its reference is
    a genuine second party and must still count — this is how the real
    `select_f35_tranche.py` is reached.
    """
    mod = _fresh_module()
    with tempfile.TemporaryDirectory() as d:
        rc, lines = _build(mod, d,
            {"a.yml": _wf(mod._step("pytest tools/gate/test_other.py"))},
            {"parity_filter.py": _STUB},
            {"test_other.py": "import parity_filter\n"},
            baseline=mod._baseline())
    assert rc == 0, (
        "a guard invoked by ANOTHER guard's test was wrongly flagged; the "
        "own-companion rule is too broad:\n%s" % "\n".join(lines))


def test_mutation_a_bare_import_stem_counts_as_wiring():
    """`import scrub_artifacts` names no `.py`. Missing it over-reports."""
    mod = _fresh_module()
    with tempfile.TemporaryDirectory() as d:
        rc, lines = _build(mod, d,
            {"a.yml": _wf(mod._step("pytest tools/gate/test_other.py"))},
            {"scrub_artifacts.py": _STUB},
            {"test_other.py": "import scrub_artifacts as s\n"},
            baseline=mod._baseline())
    assert rc == 0, (
        "a guard reached only through a bare import stem was reported "
        "unreachable:\n%s" % "\n".join(lines))


# --------------------------------------------------------------------------
# ratchet integrity: the baseline must not become an amnesty
# --------------------------------------------------------------------------

def test_ratchet_a_blank_reason_fails():
    mod = _fresh_module()
    with tempfile.TemporaryDirectory() as d:
        rc, lines = _build(mod, d,
            {"a.yml": _wf(mod._step("python tools/gate/check_a.py"))},
            {"check_a.py": _STUB, "parity_filter.py": _STUB},
            baseline=mod._baseline(parity_filter__py={
                "kind": "one-shot-phase-verifier", "reason": "   "}))
    assert rc != 0 and "reason" in "\n".join(lines)


def test_ratchet_removing_an_entry_while_the_script_stays_unreachable_fails():
    mod = _fresh_module()
    with tempfile.TemporaryDirectory() as d:
        rc, lines = _build(mod, d,
            {"a.yml": _wf(mod._step("python tools/gate/check_a.py"))},
            {"check_a.py": _STUB, "parity_filter.py": _STUB},
            baseline=mod._baseline())          # entry deliberately absent
    assert rc != 0 and "parity_filter.py" in "\n".join(lines)


def test_ratchet_a_baselined_guard_that_became_reachable_must_be_removed():
    """A list that only grows reports 'closed' about itself, not the tree."""
    mod = _fresh_module()
    with tempfile.TemporaryDirectory() as d:
        rc, lines = _build(mod, d,
            {"a.yml": _wf(mod._step("python tools/gate/check_a.py\n"
                                    "python tools/gate/parity_filter.py"))},
            {"check_a.py": _STUB, "parity_filter.py": _STUB},
            baseline=mod._baseline(parity_filter__py=None))
    blob = "\n".join(lines)
    assert rc != 0 and "parity_filter.py" in blob and "REACHED" in blob


def test_ratchet_a_baselined_guard_that_no_longer_exists_must_be_removed():
    mod = _fresh_module()
    with tempfile.TemporaryDirectory() as d:
        rc, lines = _build(mod, d,
            {"a.yml": _wf(mod._step("python tools/gate/check_a.py"))},
            {"check_a.py": _STUB},
            baseline=mod._baseline(deleted_guard__py=None))
    assert rc != 0 and "deleted_guard.py" in "\n".join(lines)


def test_the_real_live_tree_sweep_runs_here_too():
    """Re-run the ACTUAL enforcement, not just the self-test.

    Phase-2's claim-check gate deleted the `Guard reachability` step from a
    synthetic copy of `fast-checks.yml` and found that nothing went red: the
    full 77-guard sweep was performed by that step and by nothing else, so
    removing it silently removed the enforcement this control exists to add.

    This test runs `check(REPO_ROOT)` — the same call the workflow step makes —
    so the enforcement survives the deletion of any single wiring site. If a
    guard becomes unreachable, this fails whether or not the step still exists.
    """
    mod = _fresh_module()
    rc, lines = mod.check(REPO_ROOT)
    assert rc == 0, (
        "the live tree has a guard that runs on neither push nor locally and "
        "is not baselined:\n%s" % "\n".join(lines))


def test_a_guard_is_not_credited_to_its_own_companion_test():
    """`test_foo.py` naming `foo.py` is a test relationship, not a call.

    Two distinct escapes, both closed, both found by the phase-2 claim-check
    gate. The plain substring test matched `check_guard_reachability.py`
    INSIDE `test_check_guard_reachability.py`, and the closure then credited a
    guard for being named by the very file that exists to test it. Between
    them those let a guard vouch for itself, and — because every guard here has
    a companion — for the whole population.
    """
    mod = _fresh_module()
    batch = "python -m pytest tools/gate/test_check_guard_reachability.py"
    assert not mod.mentions(batch, "check_guard_reachability.py"), (
        "naming a guard's COMPANION TEST credits the guard itself; the match "
        "is not anchored at the left")
    assert mod.mentions("python tools/gate/check_guard_reachability.py",
                        "check_guard_reachability.py"), \
        "anchoring broke the ordinary invocation match"


def test_the_workflow_comment_names_exactly_the_kinds_the_code_enforces():
    """Stop the step's own comment drifting away from the taxonomy again.

    The phase-3 claim-check gate found this comment asserting the baseline had
    "12 entries ... plus scan_binary_path_hygiene.py (#256)" when the live file
    had 16 and four admitted gaps across two issues. Nothing compared the prose
    to the thing it described, so a maintainer reading the enforcement step to
    judge how much was parked would have undercounted by three and never seen
    #258.

    The count is now deliberately absent from the comment — a growing ratchet
    makes any hardcoded number stale — but the KIND NAMES are a closed set, so
    they can be checked. Both directions: a kind added to the code and not
    documented fails here, and a kind described in the comment that the code
    does not enforce fails too.
    """
    mod = _fresh_module()
    wf = os.path.join(REPO_ROOT, ".github", "workflows", "fast-checks.yml")
    text = io.open(wf, encoding="utf-8").read()

    marker = "Guard reachability"
    start = text.find(marker)
    assert start != -1, "the guard-reachability step is gone from %s" % wf
    end = text.find("\n      - name:", start)
    step = text[start:end if end != -1 else len(text)]

    # Anchored on the comment's STRUCTURED list — a `#   <kind>  <description>`
    # line — not on loose prose. A free regex over the whole block also matched
    # "dispatch-only" and "test-gap" from ordinary sentences, which would have
    # made this test fail for a reason unrelated to the taxonomy: a false
    # alarm is as bad as a missed one, since it trains the reader to edit the
    # test rather than the drift.
    documented = set(re.findall(r"^\s*#\s{3}([a-z][a-z-]+)\s{2,}\S", step, re.M))
    assert documented == set(mod.BASELINE_KINDS), (
        "the step's comment and BASELINE_KINDS disagree.\n"
        "  documented: %s\n  enforced:   %s"
        % (sorted(documented), sorted(mod.BASELINE_KINDS)))


def test_the_taxonomy_is_not_summarised_anywhere_above_its_definition():
    """No second copy of the kind set, because the copy is what goes stale.

    Third instance of one shape in this pipeline. The header above
    `BASELINE_KINDS` once read "The only two things a baseline entry may BE"
    and listed both; `deliberately-local-only` was added and the sentence
    quietly became false. The workflow comment had the same problem with a
    count. In every case a SUMMARY of the set diverged from the set.

    So the rule is structural rather than editorial: the text preceding the
    definition may explain the policy, but must not restate the members or
    their number. If you want to know the kinds, read the tuple.
    """
    mod = _fresh_module()
    source = io.open(GUARD, encoding="utf-8").read()

    marker = "BASELINE_KINDS = frozenset(("
    assert marker in source, "BASELINE_KINDS is no longer defined as expected"
    header = source[source.rfind("\n\n", 0, source.index(marker)):
                    source.index(marker)]

    restated = sorted(k for k in mod.BASELINE_KINDS if k in header)
    assert not restated, (
        "the comment above BASELINE_KINDS names %s — that is a second copy of "
        "the set, and the copy is what goes stale. Describe each kind at its "
        "own line inside the tuple instead." % restated)

    counted = _states_a_count(header)
    assert not counted, (
        "the comment above BASELINE_KINDS states a count (%s); adding a kind "
        "will silently falsify it, which has already happened once" % counted)


def test_the_workflow_step_avoids_common_taxonomy_count_phrasings():
    """The same rule, applied where a Python-only test structurally cannot see.

    The `Guard reachability` step's comment MAY list the kind names — that copy
    is checked, in both directions, by the test above it. It may NOT state how
    many there are, because nothing checks a count, which is precisely how the
    two earlier instances went stale.

    This exists because the shape recurred a third time in the commit meant to
    retire it: the step opened with a tally directly beneath a paragraph
    explaining why the count had just been removed from the other file.

    **The count half of this test is a heuristic, not a proof** — see the
    KNOWN LIMIT on `_COUNT_PHRASE`. It catches the phrasings people actually
    write; it does not establish that no tally is stated. The load-bearing
    guarantee for this comment is the NAMES check in the test above, which is
    structural. Do not read a green here as "no count exists".
    """
    wf = os.path.join(REPO_ROOT, ".github", "workflows", "fast-checks.yml")
    text = io.open(wf, encoding="utf-8").read()
    start = text.find("Guard reachability")
    assert start != -1, "the guard-reachability step is gone from %s" % wf
    end = text.find("\n      - name:", start)
    step = text[start:end if end != -1 else len(text)]

    counted = _states_a_count(step)
    assert not counted, (
        "the Guard reachability step states a taxonomy count (%s). The NAMES "
        "may live here because a test compares them to BASELINE_KINDS; a count "
        "may not, because nothing compares that." % counted)


def test_the_baseline_prose_does_not_restate_the_taxonomy():
    """The same no-second-copy rule, applied to the baseline's own `_rules`.

    Fifth instance of one shape in this pipeline, and the one that stayed
    hidden longest: `load_baseline` strips every `_`-prefixed key before
    validating, so `_rules` is read by NOTHING. It described two kinds and
    ended "anything else is an admitted-gap and needs an issue" — which the
    same file contradicts, since `run_fast_checks_locally.py` is a third kind
    and carries no issue.

    Prose that no code reads is the easiest place for a claim to rot, so the
    kinds are defined once in `BASELINE_KINDS` and named nowhere else here.
    """
    mod = _fresh_module()
    path = os.path.join(REPO_ROOT, "tools", "gate",
                        "guard_reachability_baseline.json")
    import json
    data = json.load(io.open(path, encoding="utf-8"))

    prose = "\n".join(
        v if isinstance(v, str) else "\n".join(v)
        for k, v in data.items() if k.startswith("_"))

    restated = sorted(k for k in mod.BASELINE_KINDS if k in prose)
    assert not restated, (
        "the baseline's own metadata names %s. Nothing reads these fields — "
        "`load_baseline` strips every `_`-prefixed key — so a claim here rots "
        "silently. Define the kinds once, in BASELINE_KINDS." % restated)

    counted = _states_a_count(prose)
    assert not counted, (
        "the baseline's metadata states a count (%s); it has already gone "
        "stale once" % counted)


def test_the_taxonomy_summary_row_agrees_with_its_own_detail_section():
    """A summary table is a second copy, so something has to check it.

    Sixth and seventh instances of this pipeline's recurring shape. C-BELOW's
    **detail section** (§3) was updated to say both halves are defended and
    membership is at least 14; the **summary-table row four lines above it** was
    not, and went on saying `Members 7`, `Standing control: None found`,
    `Sweep candidate: Yes — worst coverage`. A reader trusting the canonical
    summary was told the opposite of the truth, and nothing compared the two.

    Only C-BELOW is checked, deliberately. It is the row this pipeline changed
    and therefore the one whose drift this pipeline is responsible for; asserting
    over all fifteen classes would mean encoding fourteen more verdicts here,
    which is another second copy. When another class's controls change, extend
    this.
    """
    doc = os.path.join(REPO_ROOT, "docs", "audit", "test-gap-classes.md")
    text = io.open(doc, encoding="utf-8").read()

    row = [l for l in text.splitlines()
           if l.startswith("| 3 |") and "C-BELOW" in l]
    assert len(row) == 1, "expected exactly one C-BELOW summary row, got %d" % len(row)
    row = row[0]

    start = text.index("## 3. C-BELOW")
    end = text.index("\n## 4.", start)
    section = text[start:end]

    # The section is the source of truth; the row must not contradict it.
    section_defended = "BOTH HALVES NOW DEFENDED" in section
    row_defended = "None found" not in row

    assert section_defended == row_defended, (
        "the C-BELOW summary row and section 3 disagree about standing "
        "controls.\n  row     : %s\n  section says defended: %s"
        % (row.strip(), section_defended))

    if "at least 14" in section or "least 14" in section:
        assert "14" in row, (
            "section 3 says membership is at least 14; the summary row still "
            "shows a different figure:\n  %s" % row.strip())


def test_the_guard_does_not_exempt_itself():
    """Self-exemption would be test-gap class C-GUARDSELF.

    Two distinct escapes, both closed. It must not baseline itself, and it must
    be inside its own population — a guard that quietly dropped itself from
    `discover_guards` could never report itself unreachable.
    """
    mod = _fresh_module()
    assert "check_guard_reachability.py" not in mod.load_baseline(REPO_ROOT), (
        "the guard has been added to its own baseline, which is the one entry "
        "that can never be legitimate: it would be exempting itself from the "
        "rule it enforces on every other guard")
    assert "check_guard_reachability.py" in mod.discover_guards(REPO_ROOT), (
        "the guard has dropped itself from its own population, so it could "
        "never report itself unreachable")


# ---------------------------------------------------------------------------
# GitHub #251 -- guard DELETION was only half-covered.
#
# Scoping, checked rather than asserted: the existing `gone` check
# (set(baseline) - set(population)) and its self-test case
# `ratchet-deleted-entry-must-be-removed` already fail when a BASELINED guard
# disappears. They say nothing about the reached majority, which are not in the
# baseline at all: delete one together with its workflow step and the
# population simply shrinks, because a set derived from the filesystem cannot
# miss what is no longer on it. That is C-GUARDSELF, and the census is the
# independent expected-set that closes it.
# ---------------------------------------------------------------------------
def _census_tree(mod, root, census):
    """A wired, passing tree whose baseline carries `_known_guards`."""
    return _build(
        mod, root,
        {"a.yml": _wf(mod._step("python tools/gate/check_a.py\n"
                                "python tools/gate/check_b.py"))},
        {"check_a.py": _STUB, "check_b.py": _STUB},
        baseline=mod._baseline(**{"_known_guards": census}))


_FIXTURE_GUARDS = ["check_a.py", "check_b.py", "check_workflow_triggers.py"]


def test_the_live_census_lists_every_guard_that_exists():
    mod = _fresh_module()
    census = mod._census(REPO_ROOT)
    assert census is not None, "the baseline records no `_known_guards` census"
    assert sorted(mod.discover_guards(REPO_ROOT)) == census


def test_a_missing_census_is_not_an_empty_one():
    """None and [] must differ, or deleting the key disables the check while
    leaving it looking present."""
    mod = _fresh_module()
    assert mod._census(os.path.join(REPO_ROOT, "does", "not", "exist")) is None


def test_a_census_that_matches_passes():
    """The control. Without it every assertion below is satisfiable by a
    checker that rejects everything."""
    mod = _fresh_module()
    with tempfile.TemporaryDirectory() as d:
        rc, lines = _census_tree(mod, d, _FIXTURE_GUARDS)
    assert rc == 0, chr(10).join(lines)


def test_a_guard_that_vanished_is_caught():
    mod = _fresh_module()
    with tempfile.TemporaryDirectory() as d:
        rc, lines = _census_tree(mod, d, _FIXTURE_GUARDS + ["check_vanished.py"])
    assert rc != 0
    assert any("check_vanished.py" in ln for ln in lines), chr(10).join(lines)


def test_a_guard_absent_from_the_census_is_caught():
    """Both directions, or the census rots into a list of things that used to
    exist and can no longer notice a loss."""
    mod = _fresh_module()
    with tempfile.TemporaryDirectory() as d:
        rc, lines = _census_tree(mod, d, ["check_a.py", "check_workflow_triggers.py"])
    assert rc != 0
    assert any("check_b.py" in ln for ln in lines), chr(10).join(lines)
