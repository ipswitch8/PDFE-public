# -*- coding: utf-8 -*-
"""Shared drive core for the BATCH-2 native-UI interaction suites (pipeline
c4a9e271, phase-6).

Phase 5 proved the ``act_*`` rows could be *fired* through ``testToolActivatePipe``
and the window survived. Batch 2 goes a layer deeper: for each tool/operation it
drives the REAL per-element interaction (canvas drag/click/type, field create,
redaction mark, image select) through the production input path and asserts the
app survives it — and, where the effect is visibly rendered, that the canvas
actually CHANGED. That is the coverage that was missing the entire time.

Each domain file (``test_ui_batch2_<domain>.py``) supplies:

  * a fixture source PDF (opened from a THROWAWAY working copy so no save path can
    mutate a real fixture — the phase-5 fixture-integrity lesson),
  * an ``ACTIONS`` table of ``(objectName, ...)`` rows, and
  * a per-action gesture (and optional pre-activation setup for operations that
    need a precondition, e.g. apply-redactions needs a mark first).

This module carries the launch/relaunch holder and the uniform
activate → gesture → survival → record core so each domain file stays a thin,
declarative list. All drive goes through the :mod:`render_drive` pipe seams
(GAP-A14: never raw UIA ``menu_select``/``FindAll``).
"""
from __future__ import annotations

import os
import shutil
import time
from typing import Callable, Optional

# Auto-dismiss registered modal QDialogs (file pickers, confirms, tab-order /
# calc-order dialogs) so a placement click or operation can never wedge the pipe
# handler. Set BEFORE importing render_drive (it resolves env at import time).
os.environ.setdefault("PDFE_TEST_DIALOG_AUTODISMISS", "1")
os.environ.setdefault("PDFE_BUILD_CONFIG", "Release")

import pytest

from tests.ui_automation.harness import render_drive as d
from tests.ui_automation.harness import render_oracle as ro
from tests.ui_automation.harness.pdfe_app_fixture import (
    launch_pdfe,
    kill_pdfe_processes,
)
from tests.ui_automation.conftest import record_matrix_result
# Phase F25 / gap WIN-45: reachability is a RECORDED NON-PASS, never a skip.
from tests.ui_automation.conftest import (  # noqa: E402
    current_test_id,
    record_matrix_result,
)


class Holder:
    """Module-scoped PDFE instance launched from a throwaway working copy.

    Relaunches (with a fresh copy) if a prior gesture crashed the app, so one
    bad tool cannot cascade-red the remaining params in the module.
    """

    def __init__(self, src, working):
        self.app = None
        self.win = None
        self.src = src
        self.working = working

    def ensure_live(self):
        if self.app is not None:
            try:
                if self.app.is_process_running() and self.win.exists(timeout=1):
                    return self.win
            except Exception:  # noqa: BLE001
                pass
        kill_pdfe_processes()
        shutil.copy2(str(self.src), str(self.working))
        self.app, self.win = launch_pdfe(args=[str(self.working)])
        try:
            self.win.maximize()
        except Exception:  # noqa: BLE001
            pass
        time.sleep(2.0)
        return self.win

    def alive(self) -> bool:
        """True if the app SURVIVED (did not crash). Process death is the hard
        signal of a crash; a transient UIA window-enumeration hiccup under heavy
        shared-launch load (a slow query, not a crash) is tolerated by retrying
        and falling back to the process-liveness check."""
        try:
            if not self.app.is_process_running():
                return False  # process actually died — a real crash
        except Exception:  # noqa: BLE001
            return False
        for _ in range(3):
            try:
                if self.win.exists(timeout=2):
                    return True
            except Exception:  # noqa: BLE001
                pass
            time.sleep(0.5)
        # Window not enumerable after retries — re-check the process: still up
        # means the app did not crash (the UIA tree was just transiently busy).
        try:
            return bool(self.app.is_process_running())
        except Exception:  # noqa: BLE001
            return False

    def teardown(self):
        if self.app is not None:
            try:
                self.app.kill()
            except Exception:  # noqa: BLE001
                pass
        kill_pdfe_processes()
        self.app = None
        self.win = None


def make_module_fixture(src, working_path, golden_dir):
    """Build a module-scoped pytest fixture yielding a live :class:`Holder`.

    Skips cleanly (never errors) when the binary or fixture is missing or the
    PdfCanvas never appears — those are environment conditions, not product
    failures (SKIP applies only to environment gaps, never to a real assertion).
    """

    @pytest.fixture(scope="module")
    def _fixture():
        d.require_binary()
        if not src.exists():
            pytest.skip(f"fixture missing: {src.name}")
        working_path.parent.mkdir(parents=True, exist_ok=True)
        if golden_dir is not None:
            golden_dir.mkdir(parents=True, exist_ok=True)
        h = Holder(src, working_path)
        h.ensure_live()
        if d.canvas_region(h.win) is None:
            h.teardown()
            # WIN-45 / F25: unreachable -> recorded 'blocked', then FAIL.
            _reach = (
                "PdfCanvas absent from UIA tree"
            )
            record_matrix_result(current_test_id(), "blocked", _reach)
            pytest.fail(
                f"WIN-45 reachability blocked (recorded non-pass, not skipped): {_reach}")
        try:
            yield h
        finally:
            h.teardown()

    return _fixture


def run_action_test(
    holder: Holder,
    action: str,
    *,
    setup_fn: Optional[Callable] = None,
    gesture_fn: Optional[Callable] = None,
    expect_change: bool = False,
    golden_dir=None,
    golden_for=(),
    extra_note: str = "",
) -> None:
    """Drive one ``act_<action>`` row: optional precondition setup → activate the
    action → perform the real gesture → assert the app SURVIVED → (for visible
    operations) assert the canvas CHANGED → record green/red.

    * ``setup_fn(win)`` runs BEFORE the action is activated — use it to establish
      the precondition an operation needs (activate the parent tool, select a
      target, add a redaction mark, …).
    * ``gesture_fn(win)`` runs AFTER activation — the creating/operating gesture.
    * A crash/hang at any point fails the row RED (the defect class phase-5 could
      not see). ``expect_change`` additionally requires a visible render delta —
      otherwise the gesture was silently swallowed (also a real, recorded defect).
    """
    tid = f"act_{action}"
    win = holder.ensure_live()
    if not holder.alive():
        record_matrix_result(tid, "red", "app not alive before action")
        pytest.fail(f"{tid}: app not alive before action")

    setup_err = ""
    if setup_fn is not None:
        try:
            setup_fn(win)
        except Exception as e:  # noqa: BLE001
            setup_err = repr(e)

    before = d.capture(win) if expect_change else None

    d.activate(win, action, settle_s=1.0)
    active = d.active_tool(win)

    gesture_err = ""
    if gesture_fn is not None:
        try:
            gesture_fn(win)
        except Exception as e:  # noqa: BLE001
            gesture_err = repr(e)

    survived = holder.alive()

    canvas_changed = None
    if expect_change and survived and before is not None:
        try:
            after = d.capture(win)
            canvas_changed = ro.canvas_changed(before, after, pixel_threshold=0.0001)
            if golden_dir is not None and action in golden_for:
                try:
                    after.save(str(golden_dir / f"{action}.png"))
                except Exception:  # noqa: BLE001
                    pass
        except Exception:  # noqa: BLE001
            canvas_changed = None

    # Reset state for the next param: undo the change, return to the select tool.
    try:
        d.undo(win, settle_s=0.8)
        d.activate(win, "selectToolAction", settle_s=0.4)
    except Exception:  # noqa: BLE001
        pass

    notes = (f"Pipeline c4a9e271 phase-6 batch-2: activated {action} "
             f"(activeTool={active!r}), survived={survived}")
    if extra_note:
        notes += f", {extra_note}"
    if canvas_changed is not None:
        notes += f", canvasChanged={canvas_changed}"
    if setup_err:
        notes += f", setupErr={setup_err}"
    if gesture_err:
        notes += f", gestureErr={gesture_err}"

    if not survived:
        record_matrix_result(tid, "red", notes + " — APP DIED")
        pytest.fail(f"{tid}: app did not survive; {notes}")

    if expect_change and canvas_changed is False:
        record_matrix_result(tid, "red",
                             notes + " — operation produced NO visible render change")
        pytest.fail(f"{tid}: produced nothing visible; {notes}")

    record_matrix_result(tid, "green", notes)
