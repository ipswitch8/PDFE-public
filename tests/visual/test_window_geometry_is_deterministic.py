# -*- coding: utf-8 -*-
"""GitHub #284 — the harness window size must be a VALUE, not a lower bound.

WHAT WAS WRONG
==============
``tests/ui_harness/core/launch.py::_resize_window`` asks for
``WINDOW_WIDTH x WINDOW_HEIGHT`` (1600x1000) and then, if the achieved width is
under 1400, escalates through 1800 / 2000 / 2400 and stops at the first rung
that clears 1400. So it guarantees a **lower bound**, not a value. The achieved
width can be 1600, any rung, or whatever the DPI / virtual-screen clamp yields
— the function's own docstring records **1158 px** on this OS when the Python
process's DPI-awareness context differs from PDFE.exe's (GAP-28). The whole
block was wrapped in ``except Exception: pass``.

That matters because toolbars **overflow differently at different widths**, and
toolbar overflow drives both visual oracles and the UIA reachability of toolbar
actions. "At least 1400, probably" is not a basis for either.

It was NOT, however, the cause of the ten `phase5_*` oracle failures — those
were a capture-origin bug (GitHub #279), and the window here measures exactly
1600x1000, so the escalation ladder never fires. The two were conflated; this
file pins the half that remains real.

WHAT THESE TESTS PIN
====================
The function cannot *promise* an exact size — the OS has the last word. What it
can do is stop being silent, so a caller can assert on the real geometry rather
than assume one. So:

1. the outcome is RECORDED at all (it previously was not, and a failure was
   indistinguishable from success to every caller);
2. on this rig the achieved size is exactly the requested one;
3. it is the same across two independent launches — a lower bound that happens
   to land on 1600 once is not determinism;
4. ``PDFE_TEST_REQUIRE_EXACT_WINDOW=1`` turns a miss into a hard failure, for
   rigs and lanes that need the guarantee rather than the report.

Run:  python -m pytest tests/visual/test_window_geometry_is_deterministic.py -q
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
assert (REPO_ROOT / ".github" / "workflows").is_dir(), (
    f"repo root misresolved to {REPO_ROOT!r}")
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))


def _launch_and_report():
    """Launch one session and return a copy of the recorded resize outcome."""
    from tests.helpers.pdfe_session import PDFESession
    from tests.test_ui_file_operations import FIX_SINGLE
    from tests.ui_harness.core import launch as _launch

    with PDFESession(pdf_path=str(FIX_SINGLE)):
        return dict(_launch.LAST_RESIZE)


@pytest.fixture(scope="module")
def _requires_desktop():
    if not os.environ.get("SESSIONNAME"):
        pytest.skip("no interactive desktop session")


def test_the_resize_outcome_is_recorded(_requires_desktop):
    """The outcome must be observable. It previously was not.

    This is the load-bearing one: with no record, a window that came up at
    1158 px looked exactly like one that came up at 1600, and every oracle
    downstream silently measured a different layout.
    """
    report = _launch_and_report()
    assert report["requested"] is not None, (
        "_resize_window did not record a requested size, so LAST_RESIZE was "
        "never populated and callers still cannot tell what they got")
    assert report["error"] is None, (
        f"the resize reported an error: {report['error']!r}")
    assert report["achieved"] is not None, (
        "the resize recorded no achieved geometry")


def test_the_window_is_exactly_the_requested_size(_requires_desktop):
    """On this rig the escalation ladder must not be firing at all."""
    from tests.ui_harness.core.launch import WINDOW_WIDTH, WINDOW_HEIGHT

    report = _launch_and_report()
    assert report["achieved"] == (WINDOW_WIDTH, WINDOW_HEIGHT), (
        f"window came up {report['achieved']}, not "
        f"{(WINDOW_WIDTH, WINDOW_HEIGHT)}, after {report['escalations']} "
        f"escalation(s). Toolbar overflow depends on this, so pixel oracles "
        f"and UIA reachability of toolbar actions are both affected "
        f"(GitHub #284).")
    assert report["escalations"] == 0, (
        f"the escalation ladder fired {report['escalations']} time(s), so the "
        f"first SetWindowPos did not land -- the size is a lower bound here "
        f"even if it happens to be correct this run")


def test_two_launches_agree(_requires_desktop):
    """Determinism, not a single lucky landing.

    One launch at the right size is consistent with a width that varies run to
    run and happened to be right once -- which is exactly the failure mode
    #284 describes. Two independent launches must agree.
    """
    first = _launch_and_report()
    second = _launch_and_report()
    assert first["achieved"] == second["achieved"], (
        f"two launches produced different window geometry: "
        f"{first['achieved']} then {second['achieved']}. The harness window "
        f"size is not deterministic, so no pixel baseline taken in one "
        f"session is valid in another (GitHub #284).")


def test_the_strict_env_var_is_wired(_requires_desktop):
    """``PDFE_TEST_REQUIRE_EXACT_WINDOW=1`` must be able to fail the launch.

    Checked by reading the source rather than by forcing a bad resize: making
    the OS actually return the wrong size on demand is not something a test can
    do reliably, and a knob that silently does nothing is worse than no knob.
    Pinning both the read and the raise keeps the two in step.
    """
    src = (REPO_ROOT / "tests" / "ui_harness" / "core"
           / "launch.py").read_text(encoding="utf-8")
    assert src.count("PDFE_TEST_REQUIRE_EXACT_WINDOW") >= 2, (
        "the strict-mode variable is referenced fewer than twice, so one of "
        "the two paths (the size miss, or the exception) does not honour it")
    assert "raise RuntimeError(" in src, (
        "strict mode never raises, so setting it would change nothing")
