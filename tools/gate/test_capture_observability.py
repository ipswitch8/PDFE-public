#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Tests for the capture-observability diagnosis.

WHY THIS EXISTS
---------------
A "did it change?" oracle reads

    changed = (ssim < threshold) or (pixel_fraction >= min_fraction)

and fails with "X did not visibly change the canvas" -- a statement about the
PRODUCT. That inference is only valid if the captures observed anything. When
they did not, before and after are the same bytes, SSIM is exactly 1.0, and the
oracle blames the product for a harness problem.

Not hypothetical, and not new. tests/docs/known_issues.md records it under
CI-LIMITATION-WINDOWS-VALIDATION-UI-E2E-FLAKES (2026-06-30):

    test_font_size_change_via_toolbar_spinner   runner: FAIL (SSIM=1.0)
                                                local:  PASS
    test_canvas_pyautogui_visual_regression     runner: FAIL (SSIM=0.81)
                                                local:  PASS   [desktop-grab]

`capture_region` is a `pyautogui.screenshot()` -- a GDI grab -- so it needs an
attached display. The SendInput canvas tests never capture, which is why they
pass straight through an RDP disconnect while the visual families do not.
Sampling only that family is how this got called "narrow" once already.

WHAT THIS DOES NOT DO
---------------------
It does not turn any failure green. A run that could not observe the product has
demonstrated nothing; a green there would be far worse than the false red it
replaces. It changes what the failure SAYS.
"""
from __future__ import annotations

import os
import sys

import pytest

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if REPO not in sys.path:
    sys.path.insert(0, REPO)

PIL = pytest.importorskip("PIL", reason="Pillow needed for image comparison")
# tests.visual.diff imports numpy. Guarding only Pillow was enough for as long
# as neither was installed on the fast-checks lane -- the Pillow skip fired
# first and the numpy import was never reached. When pikepdf was added for the
# GitHub #210 fixture guards it brought Pillow with it, the skip stopped
# firing, and this module went straight to a COLLECTION ERROR on numpy, which
# fails the whole pytest run rather than skipping one file.
#
# A skip guard has to name every dependency the module actually has, not just
# the one that happened to be missing when it was written.
pytest.importorskip("numpy", reason="numpy needed by tests.visual.diff")
from PIL import Image  # noqa: E402

from tests.visual.diff import (  # noqa: E402
    captures_are_indistinguishable,
    explain_unchanged,
    ssim,
)


def _img(color, size=(64, 64)):
    return Image.new("RGB", size, color)


# ---------------------------------------------------------------------------
# The discriminator
# ---------------------------------------------------------------------------

def test_identical_captures_are_indistinguishable():
    a = _img((0, 0, 0))
    assert captures_are_indistinguishable(a, a.copy()) is True


def test_identical_non_black_captures_too():
    """A frozen last frame is not necessarily black. Whatever the content, two
    identical captures carry no information about a change."""
    a = _img((37, 90, 210))
    assert captures_are_indistinguishable(a, a.copy()) is True


def test_a_single_differing_pixel_is_a_real_observation():
    """THE IMPORTANT NEGATIVE. If this returned True, the check would become a
    blanket excuse for every below-threshold failure -- it would swallow real
    product defects, which is the opposite of the intent."""
    a = _img((0, 0, 0))
    b = a.copy()
    b.putpixel((1, 1), (255, 0, 0))
    assert captures_are_indistinguishable(a, b) is False


def test_a_small_but_real_difference_is_not_swallowed():
    a = _img((10, 10, 10))
    b = _img((12, 12, 12))
    assert captures_are_indistinguishable(a, b) is False
    # Sanity: this pair is exactly the shape a change-oracle would REJECT as
    # too small, and it must still be reported as a real observation.
    assert ssim(a, b) > 0.5


# ---------------------------------------------------------------------------
# The diagnosis text
# ---------------------------------------------------------------------------

def test_identical_captures_are_explained_as_a_capture_problem():
    a = _img((0, 0, 0))
    msg = explain_unchanged(a, a.copy())
    assert "IDENTICAL" in msg
    assert "not evidence the product failed" in msg
    assert "known_issues.md" in msg, "the diagnosis must point at the recorded precedent"


def test_a_real_small_difference_is_explained_as_a_real_result():
    a = _img((10, 10, 10))
    b = _img((12, 12, 12))
    msg = explain_unchanged(a, b)
    assert "DIFFER" in msg
    assert "real result" in msg
    assert "IDENTICAL" not in msg


# ---------------------------------------------------------------------------
# Integration with the real oracles -- no UI session needed
# ---------------------------------------------------------------------------

ORACLE_MODULES = [
    "tests.test_ui_annotations",
    "tests.test_ui_redaction",
    "tests.test_ui_page_operations",
]


@pytest.mark.parametrize("modname", ORACLE_MODULES)
def test_the_real_oracle_reports_observability_on_identical_captures(modname):
    """Call the SHIPPING oracle, not a copy of its logic.

    Testing the helper alone would pass while the five call sites were never
    wired up -- the same parts-verified/integration-assumed gap that let an
    inert pytest hook sit behind 15 green unit tests earlier in this work.
    """
    mod = pytest.importorskip(modname)
    if not hasattr(mod, "_assert_canvas_changed"):
        pytest.skip("%s has no _assert_canvas_changed" % modname)
    a = _img((0, 0, 0))
    # pytest.fail raises Failed, which derives from BaseException, NOT
    # Exception -- so `pytest.raises(Exception)` does not catch it and the
    # test errors out while the code under test is working perfectly.
    with pytest.raises(pytest.fail.Exception) as exc:
        mod._assert_canvas_changed(a, a.copy(), "probe-op")
    text = str(exc.value)
    assert "OBSERVABILITY" in text, text[:800]
    assert "IDENTICAL" in text, text[:800]


@pytest.mark.parametrize("modname", ORACLE_MODULES)
def test_the_real_oracle_still_fails_it_is_not_excused(modname):
    """The failure must remain a failure. Nothing here makes a test pass."""
    mod = pytest.importorskip(modname)
    if not hasattr(mod, "_assert_canvas_changed"):
        pytest.skip("%s has no _assert_canvas_changed" % modname)
    a = _img((0, 0, 0))
    with pytest.raises(pytest.fail.Exception):
        mod._assert_canvas_changed(a, a.copy(), "probe-op")


@pytest.mark.parametrize("modname", ORACLE_MODULES)
def test_a_genuinely_changed_canvas_still_passes(modname):
    """The false-positive direction: a real change must not be reported as an
    observability problem, or every passing visual test starts failing."""
    mod = pytest.importorskip(modname)
    if not hasattr(mod, "_assert_canvas_changed"):
        pytest.skip("%s has no _assert_canvas_changed" % modname)
    before = _img((0, 0, 0))
    after = _img((255, 255, 255))
    mod._assert_canvas_changed(before, after, "probe-op")   # must not raise


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-q"]))


# ---------------------------------------------------------------------------
# The screen-grab failure path  (MEASURED on a real disconnect, 2026-09-04)
# ---------------------------------------------------------------------------
#   01:45:52.0  DISCONNECTED (TerminalServices event 24)
#   01:45:54.2  first `OSError: screen grab failed`   (+2.2 s)
#   01:47:19.0  RECONNECTED (event 25)
#   01:47:32.5  last OSError, grabs work again        (+13.5 s AFTER reconnect)
#
# 50 consecutive failing samples, 98.2 s. NOT black, NOT a frozen frame -- both
# were plausible and both would have been worse, because they hand an oracle a
# plausible-looking image to reason about.
#
# The 13.5 s trailing lag mirrors OpenInputDesktop's 11 s in #132: two unrelated
# capabilities, same shape, both recovering well after the session reports
# Active. Never gate a retry on the reconnect event.
#
# Meanwhile Win32 PrintWindow kept working: two runs of
# test_font_size_change_via_toolbar_spinner OVERLAPPING the outage passed with
# real change detection -- iteration 4 spent 48 of its 50 s inside the failure
# window, iteration 5 52 of its 60. NEITHER ran entirely inside it; an earlier
# version of this comment said they did, which the event-log timings disprove.
# The overlap is decisive, the original wording was not accurate.
#
# That split -- PrintWindow immune, pyautogui not -- is why the SendInput and
# _capture_canvas families sail through a disconnect while the screen-grab ones
# cannot. Evidence: docs/audit/issue-133/capture-during-disconnect-2026-09-04.log

def test_screen_grab_failure_explains_itself():
    """A bare 'screen grab failed' sends someone hunting a code defect.

    The diagnosis must name the measured cause and point at the alternative that
    does work, or this error costs an investigation every time it appears.
    """
    # This test does not USE pyautogui -- it replaces shot.pyautogui with a
    # stub -- but importing the harness module pulls pyautogui in, and
    # pyautogui needs a display. That makes it a genuine environment gate on a
    # headless lane, expressed as an import probe so the WIN-45 reachability
    # checker can see it is one. The other 15 tests in this file are pure and
    # run everywhere; only these two need the harness.
    shot = pytest.importorskip(
        "tests.ui_harness.core.screenshot",
        reason="pyautogui (needs a display) is required to import the capture harness",
    )

    class _Boom:
        @staticmethod
        def screenshot(region=None):
            raise OSError("screen grab failed")

    real, shot.pyautogui = shot.pyautogui, _Boom()
    try:
        with pytest.raises(OSError) as exc:
            shot.capture_region(0, 0, 10, 10, "probe", "probe")
    finally:
        shot.pyautogui = real

    text = str(exc.value)
    assert "screen grab failed" in text, "the original error must survive"
    assert "CAPTURE DIAGNOSIS" in text
    assert "RDP disconnect" in text, "the measured cause must be named"
    assert "PrintWindow" in text, "the working alternative must be named"


def test_a_working_screen_grab_is_not_disturbed():
    """The false-positive direction: the diagnosis must only apply on OSError."""
    # This test does not USE pyautogui -- it replaces shot.pyautogui with a
    # stub -- but importing the harness module pulls pyautogui in, and
    # pyautogui needs a display. That makes it a genuine environment gate on a
    # headless lane, expressed as an import probe so the WIN-45 reachability
    # checker can see it is one. The other 15 tests in this file are pure and
    # run everywhere; only these two need the harness.
    shot = pytest.importorskip(
        "tests.ui_harness.core.screenshot",
        reason="pyautogui (needs a display) is required to import the capture harness",
    )

    class _Fine:
        @staticmethod
        def screenshot(region=None):
            return Image.new("RGB", (10, 10), (1, 2, 3))

    real, shot.pyautogui = shot.pyautogui, _Fine()
    try:
        path = shot.capture_region(0, 0, 10, 10, "probe", "probe")
    finally:
        shot.pyautogui = real
    assert os.path.exists(path)
