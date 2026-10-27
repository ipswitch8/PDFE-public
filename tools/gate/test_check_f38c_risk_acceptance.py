#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Self-tests for ``check_f38c_risk_acceptance`` (meta-audit follow-up F45).

Every gate is exercised in BOTH directions: a green baseline, and a mutation of
exactly one input that must turn it red. A check no mutation can redden is
decoration, so the discrimination controls at the bottom take the REAL
committed document, perturb one thing about the live world, and require the
real checker to fail.

``pytest.ini`` sets ``testpaths = tests``, so a self-test under ``tools/gate/``
runs NOWHERE unless it is named in ``fast-checks.yml``'s *"Gate self-tests +
matrix pass-semantics (R-24/R-26/R-28/R-32/R-37)"* step. One test below asserts
this file is named there, by step NAME rather than line number — line numbers
into that file rot every phase by construction.
"""
from __future__ import annotations

import copy
import sys
from pathlib import Path

import pytest
import yaml

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import check_f38c_risk_acceptance as C  # noqa: E402
import check_runner_queue_overlap as Q  # noqa: E402

REPO_ROOT = _HERE.parent.parent
# Frozen snapshots of the five shared-runner lanes as they stood while this
# repository still ran scheduled jobs. See
# docs/audit/ci-manual-only-posture.md.
FROZEN_LANES = _HERE / "fixtures" / "scheduled-lanes-pre-2026-08-13"

LIVE_SUB = 1598.5714285714287
LIVE_WORST = 1770.0
LIVE_PIN = 1600

GOOD_FM = {
    "backlog_row": "F38-c",
    "phase": 45,
    "date": "2026-08-08",
    "disposition": C.EXPECTED_DISPOSITION,
    "subscription_minutes_at_writing": 1598.6,
    "worst_day_minutes_at_writing": 1770,
    "subscription_pin_minutes": 1600,
    "runner_capacity_minutes_per_day": 1440,
    "options": [
        {"id": "A", "title": "Option A - a second self-hosted runner"},
        {"id": "B", "title": "Option B - move governance off the host"},
    ],
    "recommendation": "A",
}

GOOD_BODY = (
    "\n# F38-c capacity recommendation\n\n"
    "Row F38-c is decided, not fixed.\n\n"
    "## Option A - a second self-hosted runner\n\ntext\n\n"
    "## Option B - move governance off the host\n\ntext\n")

GOOD_BACKLOG = (
    "| **F38-c: the shared runner is OVER-subscribed** | body | "
    "**DECIDED 2026-08-08** see "
    "[`phase-45/f38c-capacity-recommendation.md`]"
    "(phase-45/f38c-capacity-recommendation.md). |\n")


def doc(fm=None, body=None) -> str:
    fm = GOOD_FM if fm is None else fm
    return ("---\n" + yaml.safe_dump(fm, sort_keys=False, allow_unicode=True)
            + "---\n" + (GOOD_BODY if body is None else body))


def run(doc_text=None, backlog=None, sub=LIVE_SUB, worst=LIVE_WORST,
        pin=LIVE_PIN, problems=()):
    return C.evaluate(doc() if doc_text is None else doc_text,
                      GOOD_BACKLOG if backlog is None else backlog,
                      sub, worst, pin, problems)


def mutated(**kw):
    fm = copy.deepcopy(GOOD_FM)
    fm.update(kw)
    return doc(fm)


# ---------------------------------------------------------------------------
# baseline
# ---------------------------------------------------------------------------

def test_baseline_is_green():
    ok, msgs = run()
    assert ok, "\n".join(msgs)


def test_baseline_reports_every_gate_it_checked():
    _, msgs = run()
    joined = "\n".join(msgs)
    for token in ("R3 date", "R5 pin", "R8 options", "R9 recommendation",
                  "R10 backlog row"):
        assert token in joined, joined


# ---------------------------------------------------------------------------
# R1 — the document must exist
# ---------------------------------------------------------------------------

def test_r1_missing_document_fails():
    ok, msgs = C.evaluate(None, GOOD_BACKLOG, LIVE_SUB, LIVE_WORST, LIVE_PIN)
    assert not ok
    assert any("FAIL(R1)" in m for m in msgs), msgs


# ---------------------------------------------------------------------------
# R2 — frontmatter
# ---------------------------------------------------------------------------

def test_r2_no_frontmatter_fails():
    ok, msgs = run(doc_text="# just prose about F38-c\n")
    assert not ok
    assert any("FAIL(R2)" in m for m in msgs), msgs


def test_r2_unterminated_frontmatter_fails():
    ok, msgs = run(doc_text="---\nbacklog_row: F38-c\n\n# body F38-c\n")
    assert not ok
    assert any("FAIL(R2)" in m for m in msgs), msgs


def test_r2_unparseable_yaml_fails():
    ok, msgs = run(doc_text="---\n: : :\n  - [\n---\n# body F38-c\n")
    assert not ok
    assert any("FAIL(R2)" in m for m in msgs), msgs


@pytest.mark.parametrize("key", C.REQUIRED_KEYS)
def test_r2_each_required_key_is_load_bearing(key):
    fm = copy.deepcopy(GOOD_FM)
    del fm[key]
    ok, msgs = run(doc_text=doc(fm))
    assert not ok, "removing %r left the check green" % key
    assert any("FAIL(R2)" in m and key in m for m in msgs), msgs


# ---------------------------------------------------------------------------
# R3 — date
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("bad", ["", "soon", "2026-13-45", "08/08/2026", 20260808])
def test_r3_non_iso_date_fails(bad):
    ok, msgs = run(doc_text=mutated(date=bad))
    assert not ok
    assert any("FAIL(R3)" in m for m in msgs), msgs


def test_r3_accepts_a_real_yaml_date_object():
    # PyYAML resolves an unquoted 2026-08-08 to datetime.date, not str.
    fm = copy.deepcopy(GOOD_FM)
    import datetime as dt
    fm["date"] = dt.date(2026, 8, 8)
    ok, msgs = run(doc_text=doc(fm))
    assert ok, "\n".join(msgs)


# ---------------------------------------------------------------------------
# R4 — row identity in BOTH frontmatter and prose
# ---------------------------------------------------------------------------

def test_r4_wrong_row_in_frontmatter_fails():
    ok, msgs = run(doc_text=mutated(backlog_row="F38-b"))
    assert not ok
    assert any("FAIL(R4)" in m for m in msgs), msgs


def test_r4_body_that_never_mentions_the_row_fails():
    body = GOOD_BODY.replace("F38-c", "F99-z")
    assert "F38-c" not in body
    ok, msgs = run(doc_text=doc(GOOD_FM, body))
    assert not ok
    assert any("FAIL(R4)" in m and "body" in m for m in msgs), msgs


# ---------------------------------------------------------------------------
# R5 — the pin
# ---------------------------------------------------------------------------

def test_r5_pin_moved_in_the_checker_without_revisiting_the_doc_fails():
    """The exact cheat this phase was written not to commit."""
    ok, msgs = run(pin=2000)
    assert not ok
    assert any("FAIL(R5)" in m for m in msgs), msgs


def test_r5_pin_edited_in_the_doc_alone_fails():
    ok, msgs = run(doc_text=mutated(subscription_pin_minutes=2000))
    assert not ok
    assert any("FAIL(R5)" in m for m in msgs), msgs


def test_r5_non_integer_pin_fails():
    ok, msgs = run(doc_text=mutated(subscription_pin_minutes="lots"))
    assert not ok
    assert any("FAIL(R5)" in m for m in msgs), msgs


# ---------------------------------------------------------------------------
# R6 / R7 — the figures track the LIVE schedule
# ---------------------------------------------------------------------------

def test_r6_subscription_drifts_when_a_lane_changes():
    """A schedule change under the pin must still redden: that is the whole
    anti-rot mechanism, and the F38 gate cannot see it."""
    ok, msgs = run(sub=1508.6)          # e.g. governance moved off the host
    assert not ok
    assert any("FAIL(R6/R7)" in m and "subscription" in m for m in msgs), msgs


def test_r7_worst_day_drift_is_caught_even_when_the_average_holds():
    ok, msgs = run(worst=1680.0)
    assert not ok
    assert any("FAIL(R6/R7)" in m and "SINGLE" in m for m in msgs), msgs


def test_r6_small_rounding_difference_is_tolerated():
    ok, msgs = run(sub=1598.9)
    assert ok, "\n".join(msgs)


def test_r6_just_outside_tolerance_fails():
    ok, msgs = run(sub=1598.6 + C.TOLERANCE_MINUTES + 0.2)
    assert not ok, "\n".join(msgs)


def test_r6_non_numeric_figure_fails():
    ok, msgs = run(doc_text=mutated(subscription_minutes_at_writing="about 1600"))
    assert not ok
    assert any("FAIL(R6/R7)" in m for m in msgs), msgs


# ---------------------------------------------------------------------------
# R8 / R9 — the options must actually be presented
# ---------------------------------------------------------------------------

def test_r8_single_option_fails():
    ok, msgs = run(doc_text=mutated(options=[GOOD_FM["options"][0]]))
    assert not ok
    assert any("FAIL(R8)" in m for m in msgs), msgs


def test_r8_empty_options_fails():
    ok, msgs = run(doc_text=mutated(options=[]))
    assert not ok
    assert any("FAIL(R8)" in m for m in msgs), msgs


def test_r8_option_listed_only_in_metadata_fails():
    opts = copy.deepcopy(GOOD_FM["options"])
    opts.append({"id": "C", "title": "Option C - a heading nobody wrote"})
    ok, msgs = run(doc_text=mutated(options=opts, recommendation="A"))
    assert not ok
    assert any("FAIL(R8)" in m and "'C'" in m for m in msgs), msgs


def test_r8_option_demoted_from_heading_to_prose_fails():
    """R8 says *heading*, so it must mean heading.

    The first version of this checker used plain substring containment while
    its docstring and failure message both promised a heading -- the stated
    and the exercised guarantee diverging a few lines apart, which is this
    programme's signature defect. Demoting a real heading into inline prose
    left it exit 0. It must not.
    """
    body = GOOD_BODY.replace(
        "## Option B - move governance off the host",
        "We also weighed Option B - move governance off the host, briefly.")
    ok, msgs = run(doc_text=doc(body=body))
    assert not ok, "a heading demoted into prose must redden R8"
    assert any("FAIL(R8)" in m and "'B'" in m for m in msgs), msgs


def test_r8_frontmatter_mention_alone_is_not_presentation():
    """Declaring an option in metadata is not presenting it.

    This case was already caught before R8 became heading-anchored --
    ``split_frontmatter`` strips the metadata, so the option's own entry was
    never reachable as a substring of ``body``. It is kept as a REGRESSION
    control on that stripping: were the frontmatter ever folded back into
    ``body``, containment would silently become unfalsifiable, and the
    heading anchor is what keeps this red either way.
    """
    ok, msgs = run(doc_text=doc(body="\n# F38-c capacity recommendation\n"))
    assert not ok
    assert any("FAIL(R8)" in m for m in msgs), msgs


def test_r8_accepts_any_heading_depth():
    for hashes in ("#", "###", "######"):
        body = GOOD_BODY.replace("## Option B", hashes + " Option B")
        ok, msgs = run(doc_text=doc(body=body))
        assert ok, "depth %r should still be a heading: %s" % (hashes, msgs)


def test_r8_duplicate_ids_fail():
    opts = [{"id": "A", "title": "Option A - a second self-hosted runner"},
            {"id": "A", "title": "Option B - move governance off the host"}]
    ok, msgs = run(doc_text=mutated(options=opts))
    assert not ok
    assert any("FAIL(R8)" in m and "duplicate" in m for m in msgs), msgs


def test_r8_malformed_option_entry_fails():
    ok, msgs = run(doc_text=mutated(options=["A", "B"]))
    assert not ok
    assert any("FAIL(R8)" in m for m in msgs), msgs


def test_r9_recommendation_naming_no_option_fails():
    ok, msgs = run(doc_text=mutated(recommendation="Z"))
    assert not ok
    assert any("FAIL(R9)" in m for m in msgs), msgs


# ---------------------------------------------------------------------------
# R10 — the backlog row and the document cannot drift apart
# ---------------------------------------------------------------------------

def test_r10_disposition_relabelled_as_implemented_fails():
    ok, msgs = run(doc_text=mutated(disposition="CLOSED-fix-implemented"))
    assert not ok
    assert any("FAIL(R10)" in m for m in msgs), msgs


def test_r10_backlog_row_absent_fails():
    ok, msgs = run(backlog="| **F99-z: unrelated** | x | y |\n")
    assert not ok
    assert any("FAIL(R10)" in m and "no 'F38-c' row" in m for m in msgs), msgs


def test_r10_backlog_row_without_the_citation_fails():
    ok, msgs = run(backlog=GOOD_BACKLOG.replace(
        "phase-45/f38c-capacity-recommendation.md", "somewhere-else.md"))
    assert not ok
    assert any("FAIL(R10)" in m and "does not cite" in m for m in msgs), msgs


def test_r10_backlog_row_without_decided_fails():
    ok, msgs = run(backlog=GOOD_BACKLOG.replace("**DECIDED 2026-08-08**",
                                                "**CLOSED 2026-08-08**"))
    assert not ok
    assert any("FAIL(R10)" in m and "DECIDED" in m for m in msgs), msgs


def test_r10_unreadable_backlog_fails():
    ok, msgs = C.evaluate(doc(), None, LIVE_SUB, LIVE_WORST, LIVE_PIN)
    assert not ok
    assert any("FAIL(R10)" in m for m in msgs), msgs


# ---------------------------------------------------------------------------
# a broken live model is a failure, never an assumed pass
# ---------------------------------------------------------------------------

def test_unparseable_lane_model_fails_loudly():
    ok, msgs = run(problems=["governance: uninterpretable cron"])
    assert not ok
    assert any("could not be derived" in m for m in msgs), msgs


# ---------------------------------------------------------------------------
# DISCRIMINATION CONTROLS against the real committed artifacts
# ---------------------------------------------------------------------------

def test_the_real_committed_record_passes_the_real_checker():
    """RE-POINTED 2026-08-13. `live_figures` now derives an EMPTY model, because
    every schedule was removed, so the record is checked against the FROZEN
    snapshots of the lanes it was written about — the same fixtures
    test_check_runner_queue_overlap.py uses. Checking a risk acceptance against
    a model that no longer exists would only ever prove that the risk is gone,
    never that the document is still right about it."""
    frozen = [FROZEN_LANES / n for n in ("governance.yml", "windows-sanitizer.yml",
                                         "mutation-kill.yml", "windows-fuzz.yml",
                                         "windows-validation.yml")]
    lanes, problems = Q.build_lanes(frozen)
    assert not problems, problems
    start, span = Q.DEFAULT_SPAN_START, Q.DEFAULT_SPAN_DAYS
    sub = Q.daily_subscription_minutes(lanes, start, span)
    # Same arithmetic as check_f38c_risk_acceptance.live_figures(), against the
    # frozen lanes: total timeout minutes landing on the single busiest day.
    per_day = {}
    for lane in lanes:
        for firing in lane.firings(start, span):
            per_day[firing.date()] = per_day.get(firing.date(), 0.0) + lane.timeout_minutes
    worst = max(per_day.values()) if per_day else 0.0
    pin = Q.DEFAULT_MAX_DAILY_SUBSCRIPTION_MINUTES
    ok, msgs = C.evaluate(C._read(REPO_ROOT / C.DOC_RELPATH),
                          C._read(REPO_ROOT / C.BACKLOG_RELPATH),
                          sub, worst, pin, problems)
    assert ok, "\n".join(msgs)


def test_the_real_record_goes_red_if_a_lane_timeout_changes():
    """Break the live world in a COPY and require the real document to fail.

    This is the anti-rot claim under test: change what the runner is offered,
    and the decision record about that load stops being current.
    """
    sub, worst, pin, _ = C.live_figures(REPO_ROOT)
    ok, msgs = C.evaluate(C._read(REPO_ROOT / C.DOC_RELPATH),
                          C._read(REPO_ROOT / C.BACKLOG_RELPATH),
                          sub + 60.0, worst + 60.0, pin)
    assert not ok, "adding an hour of daily load left the record green"
    assert any("FAIL(R6/R7)" in m for m in msgs), msgs


def test_the_two_checkers_still_share_one_parser():
    """This checker and check_runner_queue_overlap must never disagree about what
    a schedule says — they share the parser precisely so they cannot. Asserted
    against the FROZEN lanes since 2026-08-13; against the live tree both now
    derive an empty model, which they would agree about vacuously."""
    frozen = [FROZEN_LANES / n for n in ("governance.yml", "windows-sanitizer.yml",
                                         "mutation-kill.yml", "windows-fuzz.yml",
                                         "windows-validation.yml")]
    lanes, probs = Q.build_lanes(frozen)
    assert not probs, probs
    assert Q.daily_subscription_minutes(lanes) > 1440.0
    assert Q.DEFAULT_MAX_DAILY_SUBSCRIPTION_MINUTES == 1600


def test_live_figures_now_derive_an_empty_model():
    """And the live tree really is unscheduled — the frozen fixtures above are
    only honest while that holds."""
    _sub, _worst, _pin, problems = C.live_figures(REPO_ROOT)
    assert problems, "a lane was derived from a tree that has no schedules"


def test_the_f38_pin_is_unchanged_by_this_phase():
    """F45 is investigate-and-recommend only. Widening the bound to make the
    row look closed is the failure mode the phase exists to avoid."""
    assert Q.DEFAULT_MAX_DAILY_SUBSCRIPTION_MINUTES == 1600


# ---------------------------------------------------------------------------
# the self-test must actually run somewhere
# ---------------------------------------------------------------------------

def test_this_file_is_named_in_the_fast_checks_gate_self_tests_step():
    wf = REPO_ROOT / ".github" / "workflows" / "fast-checks.yml"
    text = wf.read_text(encoding="utf-8")
    marker = "- name: Gate self-tests"
    start = text.find(marker)
    assert start != -1, "the Gate self-tests step was renamed; re-anchor this test"
    nxt = text.find("\n      - name:", start + 1)
    step = text[start:nxt if nxt != -1 else None]
    assert "tools/gate/test_check_f38c_risk_acceptance.py" in step, (
        "pytest.ini has testpaths = tests, so this file runs nowhere unless it "
        "is named in the fast-checks 'Gate self-tests' step")


def test_the_checker_itself_is_wired_as_a_fast_checks_step():
    wf = REPO_ROOT / ".github" / "workflows" / "fast-checks.yml"
    text = wf.read_text(encoding="utf-8")
    assert "tools/gate/check_f38c_risk_acceptance.py" in text
    # F43: every independent check step must carry a non-cancelled guard, or
    # one red step silently disables the ones after it.
    idx = text.find("check_f38c_risk_acceptance.py")
    start = text.rfind("- name:", 0, idx)
    assert "!cancelled()" in text[start:idx], (
        "the F38-c step must carry `if: ${{ !cancelled() }}` (F43 ratchet)")
