#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Self-tests for tools/gate/check_asan_cadence.py (meta-audit follow-up F30).

Run explicitly — pytest.ini sets ``testpaths = tests``, so a file under
``tools/gate/`` is collected by NOTHING unless it is named. It is named in the
"Gate self-tests" step of .github/workflows/fast-checks.yml.

    python -m pytest -q tools/gate/test_check_asan_cadence.py

The control set is built to answer one question: *what would a checker that
trivially passes any YAML look like, and do these tests distinguish this
checker from it?* Four independently-sufficient discriminators:

* **String-matching discriminator** — ``0 18 * * 0,1`` lists two weekdays and
  would satisfy any "more than one day-of-week" regex, but is really a 6-day
  window. It must FAIL. Symmetrically ``0 18 1,15 * *`` (17-day gap) must FAIL
  even though it lists two days.
* **Vacuous-pass discriminator** — a perfectly valid workflow with only
  ``workflow_dispatch`` must FAIL, so "the file parsed" is never a pass.
* **PR-filter discriminator** — a bare ``pull_request:`` must FAIL while a
  genuinely path-scoped one PASSES, and a path list that misses an area
  (render but not codec) must FAIL.
* **The ``on:`` -> True trap** — asserted directly, because a checker that
  looked up ``doc["on"]`` would find nothing in every real workflow and pass
  vacuously forever.
"""
from __future__ import annotations

import datetime as _dt
import sys
from pathlib import Path

import pytest
import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_asan_cadence as mod  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
LIVE_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "windows-sanitizer.yml"
OLD_FIXTURE = REPO_ROOT / "tools" / "gate" / "fixtures" / "windows-sanitizer-weekly-old.yml"


def _on(yaml_text: str):
    return mod.extract_on_block(yaml.safe_load(yaml_text))


def _sched(cron: str) -> str:
    return "on:\n  schedule:\n    - cron: '%s'\n  workflow_dispatch:\n" % cron


# ---------------------------------------------------------------------------
# cron field parsing
# ---------------------------------------------------------------------------

def test_parse_field_forms():
    assert mod._parse_field("*", 0, 6, "dow") == set(range(7))
    assert mod._parse_field("0,2,4", 0, 6, "dow") == {0, 2, 4}
    assert mod._parse_field("1-3", 0, 6, "dow") == {1, 2, 3}
    assert mod._parse_field("*/2", 0, 6, "dow") == {0, 2, 4, 6}
    assert mod._parse_field("1-6/2", 0, 6, "dow") == {1, 3, 5}


@pytest.mark.parametrize("expr", ["", "x", "1-", "*/0", "*/x", "99", "0-99", "5-1"])
def test_parse_field_rejects_garbage(expr):
    """An uninterpretable field must raise, never be assumed benign."""
    with pytest.raises(mod.CronError):
        mod._parse_field(expr, 0, 6, "dow")


def test_cron_wrong_field_count_rejected():
    with pytest.raises(mod.CronError):
        mod.CronSpec("0 18 * *")


def test_dow_7_is_sunday():
    assert mod.CronSpec("0 18 * * 7").dows == {0}
    # 2026-01-04 is a Sunday.
    assert mod.CronSpec("0 18 * * 7").firings(_dt.date(2026, 1, 4), 1)


def test_dom_and_dow_both_restricted_is_OR():
    """Vixie/GitHub semantics: restricted DOM *or* DOW matches."""
    spec = mod.CronSpec("0 18 1 * 0")
    fires = {d.date() for d in spec.firings(_dt.date(2026, 1, 1), 31)}
    assert _dt.date(2026, 1, 1) in fires   # DOM match (a Thursday)
    assert _dt.date(2026, 1, 4) in fires   # DOW match (a Sunday)


# ---------------------------------------------------------------------------
# gap computation — the property a pattern-matcher cannot have
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("cron,expected_max,expected_min", [
    ("0 18 * * 0",     7.0, 7.0),   # the OLD weekly cadence
    ("0 18 * * 0,2,4", 3.0, 2.0),   # the NEW cadence chosen by F30
    ("0 18 * * 0,1",   6.0, 1.0),   # looks multi-day, is a 6-day window
    ("0 18 * * 0,3,6", 3.0, 1.0),   # equivalent bound, but hits Saturday
    ("0 18 */3 * *",   3.0, 1.0),   # day-of-month stepping: irregular, not unsafe
    ("0 18 1,15 * *", 17.0, 14.0),
])
def test_max_gap_days_measured(cron, expected_max, expected_min):
    mx, mn, count = mod.max_gap_days([cron])
    assert count > 2
    assert mx == pytest.approx(expected_max)
    assert mn == pytest.approx(expected_min)


def test_day_of_month_step_gaps_are_measured_not_assumed():
    """Recorded because the F30 brief asserted `0 18 */3 * *` could produce a
    FOUR-day gap at a month boundary. It cannot: the largest real gap is 3.00 d
    (the 28th of a 30-day month to the 1st of the next). The objection to it is
    irregularity (1 d..3 d) and the inability to dodge a chosen weekday — not a
    bound violation. This test exists so the correction is enforced, not just
    written down."""
    mx, mn, _ = mod.max_gap_days(["0 18 */3 * *"])
    assert mx == pytest.approx(3.0)
    assert mn == pytest.approx(1.0)
    assert mx < 4.0


def test_gaps_measured_across_the_union_of_multiple_crons():
    """Two individually-weekly crons that interleave are judged combined."""
    mx, _, _ = mod.max_gap_days(["0 18 * * 0", "0 18 * * 3"])
    assert mx == pytest.approx(4.0)
    mx3, _, _ = mod.max_gap_days(["0 18 * * 0", "0 18 * * 2", "0 18 * * 4"])
    assert mx3 == pytest.approx(3.0)


def test_single_firing_is_unbounded_not_zero_gap():
    mx, mn, count = mod.max_gap_days(["0 18 1 1 *"], _dt.date(2026, 1, 1), 30)
    assert count == 1 and mx is None and mn is None


# ---------------------------------------------------------------------------
# the `on:` -> boolean True trap
# ---------------------------------------------------------------------------

def test_unquoted_on_key_parses_as_boolean_true():
    """The trap itself, asserted so it cannot silently change under us."""
    doc = yaml.safe_load("on:\n  workflow_dispatch:\n")
    assert True in doc and "on" not in doc


def test_extract_on_block_handles_boolean_and_string_keys():
    assert mod.extract_on_block(yaml.safe_load("on:\n  schedule: []\n")) == {"schedule": []}
    assert mod.extract_on_block(yaml.safe_load("'on':\n  schedule: []\n")) == {"schedule": []}
    assert mod.extract_on_block(yaml.safe_load("name: x\n")) is None
    # Scalar / sequence trigger forms are legal YAML but carry no schedule.
    assert mod.extract_on_block(yaml.safe_load("on: push\n")) == {"push": None}
    assert mod.extract_on_block(yaml.safe_load("on: [push, pull_request]\n")) == {
        "push": None, "pull_request": None}


def test_missing_on_block_fails():
    ok, msgs = mod.evaluate(None)
    assert not ok
    assert any("no trigger" in m for m in msgs)


# ---------------------------------------------------------------------------
# evaluate() — POSITIVE cases
# ---------------------------------------------------------------------------

def test_positive_new_cadence_passes():
    ok, msgs = mod.evaluate(_on(_sched("0 18 * * 0,2,4")))
    assert ok
    assert any("max gap 3.00" in m for m in msgs)


def test_positive_exactly_at_bound_passes_and_one_day_over_fails():
    """Boundary discrimination: 3 d passes, 4 d fails, at the same bound."""
    assert mod.evaluate(_on(_sched("0 18 * * 0,2,4")))[0]        # max 3 d
    assert not mod.evaluate(_on(_sched("0 18 * * 0,3")))[0]      # max 4 d


def test_positive_path_filtered_pr_trigger_passes_without_any_cron():
    ok, msgs = mod.evaluate(_on(
        "on:\n"
        "  pull_request:\n"
        "    paths:\n"
        "      - 'src/image/**'\n"
        "      - 'src/rendering/**'\n"
    ))
    assert ok
    assert any("qualifying path-filtered PR trigger" in m for m in msgs)


def test_live_workflow_is_unbounded_and_says_so():
    """UPDATED 2026-08-13. The live workflow no longer carries a cadence — every
    scheduled trigger in this repository was removed that day. `evaluate()` must
    therefore report the window as NOT bounded, and must keep doing so: the
    manual-only posture is applied by main(), never by the decision core, so
    that the synthetic negative cases below cannot be disarmed by the
    repository's own state (they were, briefly, and these self-tests caught
    it)."""
    ok, msgs = mod.evaluate(mod.extract_on_block(mod.load_workflow(LIVE_WORKFLOW)))
    assert not ok, msgs
    assert any("no `schedule:` cron entries" in m for m in msgs)


# ---------------------------------------------------------------------------
# evaluate() — NEGATIVE cases (the discriminating controls)
# ---------------------------------------------------------------------------

def test_negative_old_weekly_fixture_fails():
    """The committed snapshot of the pre-F30 workflow. This is the control that
    proves the checker discriminates against the exact content it replaced."""
    ok, msgs = mod.evaluate(mod.extract_on_block(mod.load_workflow(OLD_FIXTURE)))
    assert not ok
    assert any("max gap 7.00" in m for m in msgs)


def test_negative_two_weekdays_that_look_like_a_cadence_fail():
    """`0 18 * * 0,1` satisfies any 'lists several days' regex and is a 6-day
    window. This is the single most important control: it separates a computed
    gap from a pattern match."""
    ok, msgs = mod.evaluate(_on(_sched("0 18 * * 0,1")))
    assert not ok
    assert any("max gap 6.00" in m for m in msgs)


def test_negative_day_of_month_pair_fails():
    ok, _ = mod.evaluate(_on(_sched("0 18 1,15 * *")))
    assert not ok


def test_negative_valid_yaml_with_only_dispatch_fails():
    """Parsing is not passing."""
    ok, msgs = mod.evaluate(_on("on:\n  workflow_dispatch:\n"))
    assert not ok
    assert any("no `schedule:` cron entries" in m for m in msgs)


def test_negative_empty_on_block_fails():
    ok, _ = mod.evaluate({})
    assert not ok


def test_negative_bare_pull_request_does_not_qualify():
    """An UNFILTERED PR trigger is the per-commit ASan cost deliberately removed
    2026-07-20. It must not be accepted as a substitute for cadence."""
    ok, msgs = mod.evaluate(_on("on:\n  pull_request:\n"))
    assert not ok
    assert any("carries no `paths` filter" in m for m in msgs)


def test_negative_pr_paths_missing_an_area_fails():
    """Render but no codec: partial coverage is not coverage."""
    ok, msgs = mod.evaluate(_on(
        "on:\n  pull_request:\n    paths:\n      - 'src/rendering/**'\n"))
    assert not ok
    assert any("does NOT cover src/image" in m for m in msgs)


def test_negative_irrelevant_pr_paths_fail():
    ok, _ = mod.evaluate(_on(
        "on:\n  pull_request:\n    paths:\n      - 'docs/**'\n      - '**.md'\n"))
    assert not ok


def test_negative_negated_globs_do_not_count_as_coverage():
    ok, _ = mod.evaluate(_on(
        "on:\n  pull_request:\n    paths:\n"
        "      - '!src/image/**'\n      - '!src/rendering/**'\n"))
    assert not ok


def test_negative_unparseable_cron_fails_closed():
    ok, msgs = mod.evaluate(_on(_sched("0 18 * * notaday")))
    assert not ok
    assert any("uninterpretable cron" in m for m in msgs)


# ---------------------------------------------------------------------------
# CLI / exit codes
# ---------------------------------------------------------------------------

def test_main_exit_codes(tmp_path):
    # 0 for the live workflow, but now via the manual-only posture rather than a
    # cadence, and only because BOTH of its conditions hold in this tree.
    assert mod.main(["--workflow", str(LIVE_WORKFLOW)]) == 0
    # The old weekly fixture ALSO exits 0 now, and that is not a weakened
    # control: with nothing scheduled anywhere, a 7-day cadence in a file is not
    # what bounds anything. Its discrimination is asserted where it still means
    # something — against evaluate(), the pure core, in
    # test_negative_old_weekly_fixture_fails.
    assert mod.main(["--workflow", str(OLD_FIXTURE)]) == 0
    missing = tmp_path / "nope.yml"
    assert mod.main(["--workflow", str(missing)]) == 2
    bad = tmp_path / "bad.yml"
    bad.write_text("on:\n  - [unclosed\n", encoding="utf-8")
    assert mod.main(["--workflow", str(bad)]) == 2


def test_bound_is_configurable_and_load_bearing():
    """The bound must really be used. Asserted against evaluate() rather than
    main() since 2026-08-13: with no cron in the tree, main() would return 0 at
    EVERY bound via the posture, which would make this control vacuous — the
    exact failure mode this file exists to prevent."""
    on = _on(_sched("0 18 * * 0,2,4"))            # 3-day cadence
    assert mod.evaluate(on, 3)[0]
    assert not mod.evaluate(on, 1)[0]


def test_old_fixture_is_a_faithful_snapshot_of_the_weekly_content():
    """Guards the fixture itself: if someone 'refreshes' it from the current
    workflow the negative control silently evaporates. Compared on parsed
    structure, not bytes, so core.autocrlf=true cannot make this pass in the
    session that wrote the file and fail after a checkout."""
    old = mod.extract_on_block(mod.load_workflow(OLD_FIXTURE))
    live = mod.extract_on_block(mod.load_workflow(LIVE_WORKFLOW))
    assert mod.schedule_crons(old) == ["0 18 * * 0"]
    # UPDATED 2026-08-13: the live workflow has NO cron at all now. The fixture
    # assertion above is the one that matters — it is what stops a "refresh"
    # from quietly turning the negative control into a copy of the live file.
    assert mod.schedule_crons(live) == []



# ---------------------------------------------------------------------------
# The MANUAL-ONLY posture (2026-08-13) — the branch that lets main() exit 0
# with an admittedly UNBOUNDED window. It is the newest way this checker could
# become decoration, so it is tested in both directions.
# ---------------------------------------------------------------------------

def test_posture_is_accepted_in_this_tree():
    accepted, why = mod.manual_only_posture()
    assert accepted, why
    assert "schedule:" in why or "manual-only" in why


def test_posture_is_refused_when_a_schedule_comes_back(monkeypatch):
    """Restore any cron and the posture stops being accepted — the checker goes
    back to demanding a real bound rather than a written-down risk."""
    monkeypatch.setattr(mod, "_workflows_with_schedule",
                        lambda: ["windows-sanitizer.yml"])
    accepted, why = mod.manual_only_posture()
    assert not accepted
    assert "still carry" in why


def test_posture_is_refused_when_the_document_is_missing(monkeypatch, tmp_path):
    """An UNDOCUMENTED manual posture is drift, not a decision, and must fail."""
    monkeypatch.setattr(mod, "_repo_root", lambda: tmp_path)
    accepted, why = mod.manual_only_posture()
    assert not accepted
    assert "does not exist" in why


def test_posture_is_refused_when_the_marker_is_gone(monkeypatch, tmp_path):
    """The document existing is not enough: it must still SAY it applies, so a
    later rewrite that quietly drops the posture cannot leave the gate green."""
    doc = tmp_path / mod.POSTURE_DOC
    doc.parent.mkdir(parents=True, exist_ok=True)
    doc.write_text("# a posture document that no longer states a posture" + chr(10),
                   encoding="utf-8")
    monkeypatch.setattr(mod, "_repo_root", lambda: tmp_path)
    monkeypatch.setattr(mod, "_workflows_with_schedule", lambda: [])
    accepted, why = mod.manual_only_posture()
    assert not accepted
    assert "marker" in why
