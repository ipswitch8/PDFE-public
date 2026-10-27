#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""GitHub #271 — the tool-activation acknowledgement seam.

#271 asks for a way to know a tool is ARMED, because
``MainWindow::activateToolByObjectName`` defers the trigger with
``QTimer::singleShot(0, ...)`` — deliberately, so that an action opening a modal
``QDialog::exec()`` cannot block the pipe handler and time out the UIA RPC. The
consequence is that a successful pipe write means **queued**, not **armed**, and
every caller that proceeded on it was racing the trigger.

#294 measured what the race costs. The 1000-annotation stress loop re-armed a
single-shot tool by clicking its toolbar button at a fixed rectangle; the
discriminator in ``docs/audit/p95-i294/`` showed that click re-arming
``[1, 1, 1]`` where the pipe re-armed ``[1, 2, 3]`` — it was not re-arming at
all. Every subsequent canvas click went to ``SelectTool`` and created nothing,
and the loop still reported 1000 because its counter was an unconditional
``created += 1``.

Two layers of tests here, for two different reasons:

* The ``AckParsing`` tests exercise ``activate_tool_and_wait``'s decision logic
  against a fake session. They need no desktop and no build, and they cover the
  cases a live run cannot reliably produce on demand — above all the STALE
  acknowledgement, where the pipe still holds the previous iteration's value.
  Accepting that is a false completion of exactly the class that hid #294, so it
  is asserted directly rather than hoped for.
* ``test_live_rearm_acknowledges_each_arming`` proves the seam against the real
  application, which is the only thing that can show the C++ side actually
  publishes.
"""

from __future__ import annotations

import os
import sys
import time
from pathlib import Path
from typing import List, Optional

import pytest

REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.helpers import qinvoke  # noqa: E402
from tests.helpers.canvas_geometry import point_on_page  # noqa: E402
from tests.helpers.click_safety import safe_set_cursor_pos  # noqa: E402
from tests.helpers.keys import press_to_own_foreground  # noqa: E402


# --------------------------------------------------------------------------
# Layer 1: the decision logic, against a fake session.
# --------------------------------------------------------------------------

class _FakeSession:
    """Serves a scripted sequence of pipe values.

    ``acks`` is consumed one entry per read; the last entry repeats once
    exhausted, so a test can model "the pipe settles on this value".
    """

    def __init__(self, acks: List[Optional[str]], write_ok: bool = True):
        self._acks = list(acks)
        self.write_ok = write_ok
        self.writes: List[str] = []
        self.reads = 0

    def next_ack(self) -> Optional[str]:
        self.reads += 1
        if not self._acks:
            return None
        if len(self._acks) == 1:
            return self._acks[0]
        return self._acks.pop(0)


@pytest.fixture
def fake(monkeypatch):
    """Patch the two seams activate_tool_and_wait depends on."""
    holder = {}

    def _install(session):
        holder["s"] = session
        monkeypatch.setattr(qinvoke, "read_tool_activate_ack",
                            lambda s: s.next_ack())
        # `settle` is accepted because activate_tool_and_wait passes settle=0
        # -- the acknowledgement replaces the blind pause. A stub with the
        # wrong signature would turn that into a TypeError here rather than
        # testing anything, so it mirrors the real one.
        monkeypatch.setattr(
            qinvoke, "activate_tool_by_name",
            lambda s, name, settle=0.25: (s.writes.append(name),
                                          s.write_ok)[1])
        return session

    return _install


class TestAckParsing:
    def test_a_fresh_armed_ack_succeeds(self, fake):
        s = fake(_FakeSession([None, "1:stickyNoteToolAction:armed"]))
        assert qinvoke.activate_tool_and_wait(s, "stickyNoteToolAction") is True
        assert s.writes == ["stickyNoteToolAction"]

    def test_a_stale_ack_is_not_accepted(self, fake):
        """The load-bearing case. The pipe holds the PREVIOUS arming.

        Sequence number 7 is already there when the call starts and never
        changes. The value names the right action and says ``armed``, so every
        content check passes — and accepting it would report success for an
        arming that never happened. Only the sequence number distinguishes
        them, which is why the seam publishes one.
        """
        s = fake(_FakeSession(["7:stickyNoteToolAction:armed"]))
        t0 = time.time()
        got = qinvoke.activate_tool_and_wait(s, "stickyNoteToolAction",
                                             timeout=0.2)
        assert got is False, (
            "a never-changing acknowledgement was accepted as a new arming; "
            "that is the false completion GitHub #294 was made of")
        assert time.time() - t0 >= 0.2, "it must have waited out the timeout"

    def test_a_changed_sequence_is_accepted_even_when_it_goes_backwards(
            self, fake):
        """CHANGED, not greater.

        PDFE restarts at zero, and a stress run that relaunches mid-test would
        see the counter fall. A ``>`` comparison would then hang forever on a
        perfectly valid acknowledgement, so the check is inequality.
        """
        s = fake(_FakeSession(["9:freeTextToolAction:armed",
                               "0:freeTextToolAction:armed"]))
        assert qinvoke.activate_tool_and_wait(s, "freeTextToolAction") is True

    def test_an_ack_for_a_different_action_fails(self, fake):
        s = fake(_FakeSession([None, "1:selectToolAction:armed"]))
        assert qinvoke.activate_tool_and_wait(
            s, "stickyNoteToolAction", timeout=0.2) is False

    def test_triggered_fails_when_armed_is_required(self, fake):
        """An action that ran but installed no tool is not an armed tool.

        ``MainWindow`` reports ``triggered`` when the QAction fired but
        ``canvas_->currentTool()`` is null — a menu command, or a tool action
        whose activation did not take. The stress loop needs the latter to be a
        failure, because the click that follows depends on the tool existing.
        """
        s = fake(_FakeSession([None, "1:stickyNoteToolAction:triggered"]))
        assert qinvoke.activate_tool_and_wait(
            s, "stickyNoteToolAction", timeout=0.2) is False

    def test_triggered_passes_when_armed_is_not_required(self, fake):
        s = fake(_FakeSession([None, "1:zoomInAction:triggered"]))
        assert qinvoke.activate_tool_and_wait(
            s, "zoomInAction", require_armed=False) is True

    def test_disabled_fails_even_when_armed_is_not_required(self, fake):
        """A refusal is not a trigger, and this is where that could slip.

        ``require_armed=False`` exists for actions that are not tools, where
        ``triggered`` is the right outcome. Without an explicit branch,
        ``disabled`` reaches the same code path and passes the only check left
        — so an action PDFE declined to run because it is disabled would be
        reported as having run. ``MainWindow`` publishes the refusal precisely
        so the caller can tell it from silence; collapsing it back into success
        would throw that away.
        """
        s = fake(_FakeSession([None, "1:zoomInAction:disabled"]))
        assert qinvoke.activate_tool_and_wait(
            s, "zoomInAction", require_armed=False, timeout=0.2) is False

    def test_disabled_is_reported_without_waiting_out_the_timeout(self, fake):
        """The point of publishing the refusal: a fast, specific answer.

        A disabled action used to produce no acknowledgement at all, so the
        caller paid the whole timeout and learned only that nothing arrived —
        indistinguishable from a missing pipe or a wedged application. At 1000
        iterations that is 3000 wasted seconds diagnosing nothing.

        Note what this does and does not cover. It asserts the contract of the
        C++ side — that the refusal IS published — modelled through the fake.
        It does NOT cover the Python ``state == "disabled"`` branch: with
        ``require_armed`` at its default, ``disabled`` already fails the
        ``!= "armed"`` check and returns just as fast. Mutating that branch
        away reddens the test above and not this one, which is the honest
        division of labour between them.
        """
        s = fake(_FakeSession([None, "1:stickyNoteToolAction:disabled"]))
        t0 = time.time()
        assert qinvoke.activate_tool_and_wait(
            s, "stickyNoteToolAction", timeout=5.0) is False
        assert time.time() - t0 < 1.0, (
            "a published refusal must return at once, not sit out the wait")

    def test_the_second_call_does_not_re_read_the_baseline(self, fake):
        """The 283.9 ms fix, asserted as a round-trip count.

        A UIA round trip costs more as the accessible tree grows: 44.6 ms on an
        empty document, 283.9 ms averaged over 1000 annotations. So the number
        of round trips per call is the thing to hold down, and the pre-read is
        removable — we published the previous acknowledgement ourselves, so we
        already know its sequence number.

        Counted rather than timed. A timing assertion here would be measuring
        the fake.
        """
        qinvoke.reset_ack_seq_cache()
        s = fake(_FakeSession([None, "1:t:armed", "2:t:armed"]))

        assert qinvoke.activate_tool_and_wait(s, "t") is True
        after_first = s.reads
        assert qinvoke.activate_tool_and_wait(s, "t") is True
        second_call_reads = s.reads - after_first

        assert second_call_reads == 1, (
            f"the second call made {second_call_reads} reads; with the baseline "
            f"cached it needs exactly one — the confirming read. More than that "
            f"means the pre-read came back, which is a third of the per-call "
            f"cost at scale.")

    def test_a_cold_cache_reads_rather_than_assuming_zero(self, fake):
        """Why the cold path must READ and not assume a fresh counter.

        The application may have been running before this process attached, so
        its counter is at some unknown value. Assuming zero would make the first
        call treat whatever is already in the pipe as new — accepting a stale
        acknowledgement on call one, which is the precise failure this function
        exists to prevent.
        """
        qinvoke.reset_ack_seq_cache()
        s = fake(_FakeSession(["7:t:armed"]))   # already at 7, never changes
        assert qinvoke.activate_tool_and_wait(s, "t", timeout=0.2) is False, (
            "a cold cache accepted the pre-existing acknowledgement, so it "
            "assumed the counter started at zero instead of reading it")

    def test_a_failed_write_fails_without_waiting(self, fake):
        s = fake(_FakeSession([None], write_ok=False))
        t0 = time.time()
        assert qinvoke.activate_tool_and_wait(
            s, "stickyNoteToolAction", timeout=5.0) is False
        assert time.time() - t0 < 1.0, (
            "a failed write must not sit out the acknowledgement timeout")

    def test_an_unreadable_pipe_times_out_rather_than_passing(self, fake):
        """An absent pipe must FAIL, never wave the caller through.

        If PDFE was built without the test pipes the widget does not exist and
        every read returns None. CLAUDE.md's rule for the canvas-geometry seam
        applies identically here: if the seam does not answer, fail — never
        fall back to a guess.
        """
        s = fake(_FakeSession([None]))
        assert qinvoke.activate_tool_and_wait(
            s, "stickyNoteToolAction", timeout=0.2) is False

    def test_a_malformed_ack_is_ignored_not_parsed_optimistically(self, fake):
        """Two fields, or five, is not an acknowledgement."""
        s = fake(_FakeSession([None, "1:stickyNoteToolAction",
                               "2:stickyNoteToolAction:armed"]))
        assert qinvoke.activate_tool_and_wait(s, "stickyNoteToolAction") is True


class _FakeWidget:
    def __init__(self, values):
        self._values = list(values)

    def get_value(self):
        if not self._values:
            return ""
        if len(self._values) == 1:
            return self._values[0]
        return self._values.pop(0)


@pytest.fixture
def fake_undo(monkeypatch):
    """Patch the two module-level seams `undo_repeat` uses."""
    state = {"written": [], "write_ok": True}

    def _install(result_values, write_ok=True):
        state["write_ok"] = write_ok
        wid = _FakeWidget(result_values)
        monkeypatch.setattr(qinvoke, "_find_pipe",
                            lambda s, auto_id, **kw: wid)
        monkeypatch.setattr(
            qinvoke, "_write_to_pipe",
            lambda s, auto_id, value, **kw: (state["written"].append(value),
                                             state["write_ok"])[1])
        return state

    return _install


class TestUndoRepeat:
    """`undo_repeat` must report operations PERFORMED, never requested.

    The loop it replaces fired 1010 Ctrl+Z hotkeys, performed zero undos, and
    reported success — it counted keystrokes sent. So the one thing this helper
    must never do is echo its own argument back.
    """

    def test_it_returns_the_performed_count_not_the_requested_one(
            self, fake_undo):
        st = fake_undo(["", "1:1000:exhausted"])
        assert qinvoke.undo_repeat(object(), 1010) == 1000
        assert st["written"] == ["1010"]

    def test_saturating_at_the_stack_depth_is_not_a_failure(self, fake_undo):
        """Asking for more than the stack holds is the intended usage.

        The caller wants headroom: request N+10 against an N-deep stack so the
        reply proves the stack was EXHAUSTED rather than merely poked N times.
        A helper that treated performed < requested as an error would make that
        impossible.
        """
        fake_undo(["", "1:3:exhausted"])
        assert qinvoke.undo_repeat(object(), 8) == 3

    def test_zero_performed_is_reported_as_zero_not_as_failure(
            self, fake_undo):
        """0 and None mean different things and must not be collapsed.

        0 is a fact about the document: there was nothing to undo. None means
        the harness could not find out. Collapsing them would let a broken
        pipe read as an empty undo stack — which is precisely the confusion
        that made the 1010-hotkey loop look like it worked.
        """
        fake_undo(["", "1:0:exhausted"])
        assert qinvoke.undo_repeat(object(), 5) == 0

    def test_redo_is_tagged_in_the_spec(self, fake_undo):
        st = fake_undo(["", "1:7:exhausted"])
        assert qinvoke.undo_repeat(object(), 7, redo=True) == 7
        assert st["written"] == ["redo:7"], (
            "a redo request must be distinguishable on the wire, or it would "
            "silently undo instead")

    def test_a_bad_spec_reply_is_None_not_a_negative_count(self, fake_undo):
        """The application answers -1 for a spec it could not parse."""
        fake_undo(["", "1:-1:bad-spec"])
        assert qinvoke.undo_repeat(object(), 5) is None

    def test_a_failed_write_returns_None(self, fake_undo):
        fake_undo([""], write_ok=False)
        assert qinvoke.undo_repeat(object(), 5) is None

    def test_set_undo_depth_returns_the_depth_the_app_confirms(
            self, fake_undo):
        """Not the depth requested — the depth the application reports setting.

        The stress test compares the two and fails if they differ, because with
        the 500 default its post-undo oracle is arithmetically unreachable:
        1000 creations can only ever undo down to 500 annotations. A helper that
        echoed the request would make that check vacuous.
        """
        st = fake_undo(["", "1:1100:depth-set"])
        assert qinvoke.set_undo_depth(object(), 1100) == 1100
        assert st["written"] == ["depth:1100"]

    def test_a_rejected_depth_is_None(self, fake_undo):
        """The application answers -1 for a depth it will not accept."""
        fake_undo(["", "1:-1:bad-depth"])
        assert qinvoke.set_undo_depth(object(), 0) is None

    def test_a_missing_undo_manager_is_None_not_a_silent_success(
            self, fake_undo):
        fake_undo(["", "1:-1:no-undo-manager"])
        assert qinvoke.set_undo_depth(object(), 1100) is None

    def test_an_identical_payload_with_a_new_sequence_IS_a_fresh_reply(
            self, fake_undo):
        """THE bug this suite got wrong the first time.

        This test used to assert the opposite — that an unchanged reply should
        time out — and in doing so it certified the defect as correct.

        Two consecutive batches legitimately produce the SAME payload: undoing
        1000 operations replies ``1000:exhausted`` and redoing 1000 replies
        ``1000:exhausted``. Byte-identical. A reader comparing payloads cannot
        see the second reply arrive at all.

        Measured 2026-10-09: that is exactly what happened in the stress test.
        The redo HAD executed — the window title went from ``[doc.pdf]`` to
        ``[doc.pdf]*`` within a second and no modal ever appeared — but the test
        reported "the redo-repeat seam gave no reply", and two full runs were
        spent hunting a crash and a hang in the application before the defect
        turned up in the reader.

        So the sequence number is what makes a reply fresh, and an identical
        payload under a new sequence number must be accepted.
        """
        fake_undo(["1:1000:exhausted", "2:1000:exhausted"])
        assert qinvoke.undo_repeat(object(), 1010) == 1000, (
            "an identical payload under a NEW sequence number was rejected; "
            "that is the redo-reply bug, and undo->redo of the same count is "
            "the common case, not an edge case")

    def test_a_reply_whose_sequence_has_not_moved_is_still_stale(
            self, fake_undo):
        """The other half: same sequence number means it is the old reply.

        Kept alongside the test above because the two together pin the rule to
        the SEQUENCE and not to the payload. Dropping either one leaves a
        reader that passes for the wrong reason.
        """
        fake_undo(["7:1000:exhausted"])    # already there, never changes
        t0 = time.time()
        assert qinvoke.undo_repeat(object(), 5, timeout=0.3) is None
        assert time.time() - t0 >= 0.3


# --------------------------------------------------------------------------
# Layer 2: against the real application.
# --------------------------------------------------------------------------

def test_live_rearm_acknowledges_each_arming(tmp_path):
    """Re-arm a single-shot tool three times THROUGH THE DEFERRED PATH.

    Rewritten after the ``claim-check`` gate failed phase-10 on this test (F3).
    The original asserted only that three armings produced three distinct
    sequence numbers, and claimed that reproduced #294's ``[1, 1, 1]``
    signature. It did not, for two independent reasons:

    1. **The oracle was unreachable.** ``MainWindow`` does
       ``++testToolActivateSeq_`` on *every* publish path, unconditionally, so
       three publishes always yield three distinct numbers. ``[1, 1, 1]``
       cannot occur in the current product whether the seam works or not. An
       assertion that cannot fail is decoration.
    2. **It took the wrong code path.** With nothing between the armings the
       tool stays armed, so iterations 2 and 3 hit
       ``activateToolByObjectName``'s ``isCheckable() && isChecked()`` early
       return. Only iteration 1 reached the deferred lambda -- the path this
       seam exists for.

    So this now CLICKS between armings. Each click creates a note and makes
    ``MainWindow`` switch back to ``SelectTool``, which forces the next re-arm
    through the deferred trigger.

    And it asserts something **the queueing site could not satisfy**: that
    ``activeToolDiag()`` reports ``stickyNoteToolAction`` once the
    acknowledgement arrives. A ``QActionGroup`` checks an action when it is
    TRIGGERED, so an acknowledgement published before the deferred trigger ran
    would arrive while the previously checked action was still checked. That is
    the discriminator ``armed`` alone cannot give: ``armed`` only means
    ``currentTool() != nullptr``, and ``SelectTool`` is itself a tool object.

    Three iterations rather than a thousand: the claim is that each arming is
    individually observable through the real path, as visible at three as at a
    thousand. The N=1000 cost lives in ``test_ui_stress.py``.
    """
    pytest.importorskip("pywinauto")
    from tests.helpers.pdfe_session import PDFESession  # noqa: PLC0415

    fixture = REPO_ROOT / "tests" / "fixtures" / "test_3page.pdf"
    if not fixture.exists():
        pytest.fail(
            f"fixture {fixture} is missing; this test needs a real document to "
            "open and must not be skipped into a false green")

    src = tmp_path / "i271_rearm.pdf"
    src.write_bytes(fixture.read_bytes())

    seqs: List[str] = []
    with PDFESession(pdf_path=str(src)) as s:
        assert s.wait_for_window_title(src.name, timeout=20), (
            "PDFE did not open the document")
        time.sleep(1.0)

        if qinvoke.read_tool_activate_ack(s) is None:
            # Distinguish "the seam is absent" from "the seam says no". A build
            # without PDFE_TESTING has no pipe at all, and that is a build
            # problem, not a product failure -- but it is still a failure here,
            # because a run that cannot observe arming proves nothing.
            pytest.fail(
                "testToolActivateResultPipe is not present in this build. It "
                "is gated on testPipesEnabled(); a Release build without "
                "PDFE_TESTING has no pipes, and this test cannot run against "
                "one. Build the Debug configuration (or see GitHub #182 on "
                "PDFE_TESTING never being OFF).")

        if qinvoke.active_tool(s) is None:
            pytest.fail(
                "testActiveToolQueryPipe/testActiveToolResultPipe are not "
                "readable in this build. Without them this test cannot tell a "
                "queueing-site acknowledgement from a post-trigger one, which "
                "is the only thing it meaningfully asserts.")

        import ctypes  # noqa: PLC0415
        user32 = ctypes.WinDLL("user32", use_last_error=True)

        for i in range(3):
            armed = qinvoke.activate_tool_and_wait(
                s, "stickyNoteToolAction", timeout=5.0)
            ack = qinvoke.read_tool_activate_ack(s)
            assert armed, (
                f"arming {i} was not acknowledged within 5 s; last pipe value "
                f"{ack!r}")
            assert ack is not None
            seqs.append(ack.split(":", 1)[0])

            # THE discriminating assertion. The acknowledgement has arrived, so
            # the deferred trigger must already have run -- and a QActionGroup
            # checks an action only on trigger.
            tool = qinvoke.active_tool(s)
            assert tool == "stickyNoteToolAction", (
                f"arming {i}: the acknowledgement said armed, but the "
                f"application reports the active tool as {tool!r}. That is an "
                f"acknowledgement published BEFORE the deferred "
                f"action->trigger() ran -- the false 'queued means armed' "
                f"signal this seam exists to eliminate.")

            # Click on the page: creates a note and makes MainWindow switch
            # back to SelectTool, which forces the NEXT re-arm through the
            # deferred path instead of the already-checked early return.
            # CLAUDE.md: never guess canvas coordinates.
            pt = point_on_page(s, page_index=0, fx=0.25 + 0.2 * i, fy=0.3)
            assert pt is not None, (
                f"arming {i}: point_on_page did not answer. Failing rather "
                f"than guessing -- a click in the grey margin would leave the "
                f"tool armed, the next iteration would take the early return, "
                f"and this test would silently stop testing the deferral.")
            safe_set_cursor_pos(*pt)
            user32.mouse_event(0x0002, 0, 0, 0, 0)
            time.sleep(0.005)
            user32.mouse_event(0x0004, 0, 0, 0, 0)
            time.sleep(0.35)
            try:
                press_to_own_foreground(s.pid, "escape", pause=0.05)
            except Exception:  # noqa: BLE001
                pass

            # Prove the switch-away happened, or the next iteration is not
            # testing what this test claims to test.
            after = qinvoke.active_tool(s)
            assert after != "stickyNoteToolAction", (
                f"after creation {i} the sticky-note action is STILL checked "
                f"({after!r}). The tool is documented single-shot -- MainWindow "
                f"switches back to SelectTool so the Properties Panel populates "
                f"on the annotation just made. If that stopped happening, every "
                f"later re-arm takes the early return and this test no longer "
                f"exercises the deferred path.")

    # Sanity check, NOT a reproduction of #294. The sequence counter increments
    # unconditionally on every publish, so distinct values here only confirm
    # three separate publishes occurred. They could not have detected the
    # [1, 1, 1] the toolbar-click route produced, because that route did not
    # publish at all. The real coverage is the two assertions in the loop.
    assert len(set(seqs)) == 3, (
        f"three arming requests produced sequence numbers {seqs}; three "
        f"separate publishes should yield three distinct values.")
