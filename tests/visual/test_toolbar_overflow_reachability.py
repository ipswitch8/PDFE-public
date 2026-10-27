# -*- coding: utf-8 -*-
"""GitHub #284 cluster B — overflowed toolbar controls must be reachable, and
the four controls that were never overflowed must not be absorbed into that
story.

WHAT WAS MEASURED (p96 phase-7)
===============================
Seven UI-matrix cases fail with ``formToolActionButton not reachable``. The
issue attributes all seven, plus several other controls, to toolbar overflow.
Probing the live UIA tree showed that attribution is right for exactly one of
them:

    toolbar extension buttons found: 1
      QApplication.mainWindow.annotationToolBar.qt_toolbar_ext_button

    zoomCombo             ABSENT   (not in the UIA tree either way)
    actionRotateView180   ABSENT   (not in the UIA tree either way)
    formToolActionButton  OVERFLOW (only after expanding the chevron)
    rotatePage180Action   ABSENT   (not in the UIA tree either way)
    deletePageAction      ABSENT   (not in the UIA tree either way)

So ``formToolActionButton`` is genuinely overflowed and
``tests/helpers/toolbar_overflow.py`` makes it reachable. The other four are
**not** an overflow problem and expanding the chevron does nothing for them —
they are a separate defect, left open, and deliberately pinned here so that a
later "fix" which merely expands a toolbar cannot be mistaken for addressing
them.

WHY THIS FILE WAS RESTRUCTURED (p97 phase-7)
============================================
The previous version asserted the 1 in one test and the 4 in a parametrized
one, and the parametrized test **skipped** whenever a control resolved::

    hit, how = find_including_overflow(main_window, name)
    if hit is not None:
        pytest.skip(f"{name} now resolves ({how}); ...")

That makes the one error mode this is supposed to catch invisible. If a change
caused all five to classify as ``overflow`` — precisely the "all 5 are
overflow" misreading #284 made — the first test would pass and the other four
would *skip*. Five green-or-skipped results, nothing red, and the conflation
restored. A skip on the failure direction is not caution; it is the hiding
place.

It also carried an **order dependency**. Expanding a Qt toolbar's chevron
creates the popup's child widgets and they persist in the UIA tree for the rest
of the session — closing the popup does not remove them and Escape does not
either. So whichever test ran first destroyed the before-state for every test
after it, and the second test's reading of ``formToolActionButton`` depended on
collection order rather than on the product.

Both are fixed the same way: the before/after contrast is measured **exactly
once**, for all five controls, in a module-scoped fixture, and every test is a
pure assertion over that recorded census. The state that is destroyed by
observing it is observed once, and no test can reach the chevron itself.

THEN THE SKIP CAME BACK, IN THE PRECONDITION
============================================
The restructure above still measured at whatever width the harness produced.
Run on this rig, that is exactly 1600x1000 — and at 1600 **nothing overflows**::

    geometry={'achieved': (1600, 1000), 'exact': True}  ext_buttons=0
    how={'formToolActionButton': 'direct', ...}

so the two assertions that matter hit their "no cluster-B condition on this rig"
guard and skipped. Result: *5 passed, 2 skipped*, verifying none of the
criterion. Removing a `pytest.skip` from an assertion achieves nothing if the
condition it guarded is simply absent — the skip had moved from the assertion
into the precondition, which is the same hiding place one level up.

The reading is not a contradiction of p96's. Both are true, taken at different
widths, which is #284's entire thesis arriving from the other direction.
Measured with `docs/audit/p97-phase7/probe_overflow_onset_width.py`::

    1600 1500 1400 1300  ->  ext_buttons = 0
    1200 1100 1000 900 800 700  ->  ext_buttons = 1

**Overflow onset is 1200px, and the harness guarantees only >= 1400px.** Every
width from 1400 up satisfies the harness equally and they do not classify
alike, so p96's overflow sighting was a run where the DPI/virtual-screen clamp
produced something at or below 1200 — consistent with the 1158px the issue
documents.

So the fixture now **creates** the condition instead of waiting for it: it
ladders the width down, finds where overflow starts, and classifies there. The
onset width is re-derived every run rather than pinned, because it is a property
of the toolbar's contents and the DPI — and a hardcoded narrow width would be
exactly the guessed geometry #284 is about. Counting extension buttons has no
side effects, so the ladder is repeatable; only the single expansion at the end
is irreversible.

Result: **7 passed, 0 skipped**, with
`formToolActionButton` -> ``overflow`` and the other four -> ``absent``.

Run:  python -m pytest tests/visual/test_toolbar_overflow_reachability.py -q
"""

from __future__ import annotations

import ctypes
import os
import sys
import time
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
assert (REPO_ROOT / ".github" / "workflows").is_dir(), (
    f"repo root misresolved to {REPO_ROOT!r}")
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.helpers.toolbar_overflow import (  # noqa: E402
    expand_toolbar_overflow,
    find_including_overflow,
    toolbar_extension_buttons,
)

#: The one control measured as genuinely overflowed.
MEASURED_OVERFLOW = "formToolActionButton"

#: Controls measured as absent from the UIA tree with AND without expansion.
#: Not an overflow problem; see the module docstring.
MEASURED_ABSENT = ("zoomCombo", "actionRotateView180", "rotatePage180Action",
                   "deletePageAction")

#: All five controls GitHub #284 attributes to overflow. The census covers the
#: whole set in one pass, because "1 of 5, not all 5" is a statement about the
#: set and cannot be checked one control at a time.
ALL_FIVE = (MEASURED_OVERFLOW,) + MEASURED_ABSENT

#: Widths to try, widest first, looking for where overflow STARTS.
#:
#: The census must be taken at a width where the toolbar actually overflows, and
#: the test is responsible for CREATING that condition rather than waiting for
#: it. Measured on this rig 2026-10-15 with
#: `docs/audit/p97-phase7/probe_overflow_onset_width.py`:
#:
#:     1600 1500 1400 1300 -> ext_buttons = 0
#:     1200 1100 1000  900  800  700 -> ext_buttons = 1
#:
#: so onset is 1200px. That number is NOT hardcoded as the test width -- it is
#: re-derived every run, because it is a property of the toolbar's contents and
#: the DPI, both of which change. Hardcoding a narrow width would be another
#: guess, and guessed geometry is what #284 is about.
WIDTH_LADDER = (1600, 1500, 1400, 1300, 1200, 1100, 1000, 900, 800, 700)
LADDER_HEIGHT = 1000

#: The lower bound `tests/ui_harness/core/launch.py::_resize_window` guarantees.
#: Overflow onset (1200) sits BELOW it, which is precisely why a lower-bound
#: guarantee cannot pin overflow: every width from 1400 up is equally
#: "acceptable" to the harness and they do not classify alike.
HARNESS_LOWER_BOUND = 1400

_SWP_FRAMECHANGED_NOACTIVATE = 0x20 | 0x10


def _set_window_size(hwnd: int, w: int, h: int = LADDER_HEIGHT):
    """Set the window size and return what was ACHIEVED, via GetWindowRect.

    GetWindowRect rather than pywinauto's rectangle: the UIA backend can report
    scaled coordinates on a non-100%-DPI desktop, and a screenshot measurement
    has been observed disagreeing with the UIA canvas rect by ~145px because
    the two live in different DPI spaces. The achieved value is returned, never
    assumed -- SetWindowPos is a request.
    """
    from ctypes import wintypes

    user32 = ctypes.windll.user32
    user32.SetWindowPos(hwnd, 0, 0, 0, w, h, _SWP_FRAMECHANGED_NOACTIVATE)
    time.sleep(0.6)  # repaint + Qt toolbar layout recalculation
    rect = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    return rect.right - rect.left, rect.bottom - rect.top


@pytest.fixture(scope="module")
def session(tmp_path_factory):
    """A PDFE session whose PERSISTED SETTINGS ARE ISOLATED.

    This matters more than it looks. `MainWindow` saves both its geometry and
    its dock/toolbar layout on close and restores them on open
    (`src/ui/MainWindow.cpp:3909-3910` save, `:3571-3573` restore). The census
    below ladders the window down to 700px, which forces the toolbars to
    overflow and rearrange, and it clicks a toolbar's overflow chevron. Both of
    those end up inside `saveState()`, so **the next launch of PDFE inherits
    them** -- in this session and in any later one on this machine.

    Measured consequence of not isolating: `tests/visual/` passed module by
    module, then as a whole suite
    `test_visual_baselines.py::test_baseline_document_open_page1` failed at
    SSIM=0.6768 / pixel_fraction=0.1802, with
    `test_canvas_pyautogui_regression` red as well. Restoring the window
    *width* on teardown was NOT enough and the failure reproduced byte-identical
    -- because the poisoned state was the toolbar layout, not the size.

    A cross-test state side effect presents exactly as a rendering regression,
    which is the subject of GitHub #279. `PDFE_SETTINGS_PATH` points AppSettings
    at an explicit INI (`src/app/AppSettings.cpp:14,20-24`), so pointing it at a
    throwaway file means nothing this module does can escape it. It also makes
    the census itself more deterministic, since it no longer starts from
    whatever layout the previous run happened to leave behind.
    """
    if not os.environ.get("SESSIONNAME"):
        pytest.skip("no interactive desktop session")
    from tests.helpers.pdfe_session import PDFESession
    from tests.test_ui_file_operations import FIX_SINGLE

    settings_ini = tmp_path_factory.mktemp("toolbar_overflow_settings") / "pdfe.ini"
    prev_settings = os.environ.get("PDFE_SETTINGS_PATH")
    prev_layout = os.environ.get("PDFE_TEST_DETERMINISTIC_LAYOUT")
    os.environ["PDFE_SETTINGS_PATH"] = str(settings_ini)
    os.environ["PDFE_TEST_DETERMINISTIC_LAYOUT"] = "1"
    try:
        with PDFESession(pdf_path=str(FIX_SINGLE)) as s:
            yield s
    finally:
        # Put the environment back exactly as it was, including the
        # "was not set" case -- leaving PDFE_SETTINGS_PATH pointing at a
        # deleted temp file would silently isolate every later test too.
        for key, prev in (("PDFE_SETTINGS_PATH", prev_settings),
                          ("PDFE_TEST_DETERMINISTIC_LAYOUT", prev_layout)):
            if prev is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = prev


@pytest.fixture(scope="module")
def main_window(session):
    return session.window


@pytest.fixture(scope="module")
def census(main_window, session):
    """THE one-and-only ordered reachability measurement for all five controls.

    Returns a dict with:

      ``harness_geometry`` what the harness achieved at launch, for the record;
      ``sweep``            [(requested, achieved_w, ext_buttons)] per ladder rung;
      ``onset``            the width where overflow STARTS, and its ext count;
      ``ext_buttons``      overflow buttons at the census width, BEFORE expanding;
      ``before``           {name: found?} from a plain lookup, all five, taken
                           before any chevron is touched;
      ``how``              {name: 'direct'|'overflow'|'absent'} after the
                           overflow-aware lookup.

    TWO THINGS ARE LOAD-BEARING HERE.

    **The width must be created, not awaited.** The first version of this
    fixture measured at whatever the harness produced. On this rig that is
    exactly 1600x1000, where NOTHING overflows -- so the two assertions that
    matter skipped, and the file reported "5 passed, 2 skipped" while verifying
    none of its criterion. That is the same shape as the `pytest.skip` this
    file was rewritten to remove; it had simply moved from the assertion into
    the precondition. The fixture now ladders the width DOWN until overflow
    appears and classifies there, so the condition cannot be absent.

    The onset width is re-derived every run rather than hardcoded. Measured
    1200px here, but it is a property of the toolbar's contents and the DPI,
    and a pinned narrow width would be exactly the guessed geometry #284 is
    about.

    **Every plain lookup happens before any expansion.** Expanding one
    toolbar's chevron permanently populates the UIA tree -- closing the popup
    does not undo it and neither does Escape -- so a later plain lookup would
    succeed for reasons unrelated to the control's own reachability. Counting
    extension buttons does NOT expand anything, which is why laddering is
    repeatable while the expansion below happens exactly once.
    """
    from tests.ui_automation.harness.dialog_helpers import find_by_suffix
    from tests.ui_harness.core.launch import LAST_RESIZE

    harness_geometry = {
        "requested": LAST_RESIZE.get("requested"),
        "achieved": LAST_RESIZE.get("achieved"),
        "exact": bool(LAST_RESIZE.get("exact")),
        "error": LAST_RESIZE.get("error"),
    }

    hwnd = session.hwnd

    # RESTORE THE WINDOW SIZE AFTERWARDS. This is not tidiness.
    #
    # `MainWindow` persists its geometry on close and restores it on open
    # (`src/ui/MainWindow.cpp:3909` saves, `:3567` restores), so a window this
    # fixture leaves narrow is inherited by whatever launches PDFE next. The
    # ladder below drives the width down to 700px and then settles at the
    # overflow onset, neither of which is the harness's 1600x1000.
    #
    # Measured consequence of not doing this: `tests/visual/` passed module by
    # module and then, run as a whole suite,
    # `test_visual_baselines.py::test_baseline_document_open_page1` failed at
    # SSIM=0.6768 / pixel_fraction=0.1802 and
    # `test_canvas_pyautogui_regression` failed too -- because the page was
    # being rendered into a canvas of a different width. A cross-test geometry
    # side effect reads exactly like a rendering regression, which is the whole
    # subject of GitHub #279.
    _restore_to = harness_geometry.get("achieved")

    # Find where overflow starts. Counting is side-effect free, so this sweep
    # leaves the UIA tree exactly as it found it.
    sweep = []
    onset = None
    for w in WIDTH_LADDER:
        aw, _ah = _set_window_size(hwnd, w)
        n = len(toolbar_extension_buttons(main_window))
        sweep.append((w, aw, n))
        if n > 0 and onset is None:
            onset = {"requested": w, "achieved": aw, "ext_buttons": n}

    # Classify at the onset width (restore it -- the sweep ran past it).
    if onset is not None:
        onset["achieved"] = _set_window_size(hwnd, onset["requested"])[0]

    ext_before = len(toolbar_extension_buttons(main_window))

    # PASS 1 -- plain lookups for all five, nothing expanded yet.
    before = {name: find_by_suffix(main_window, name) is not None
              for name in ALL_FIVE}

    # PASS 2 -- expand once, then re-resolve all five. find_including_overflow
    # expands on a miss itself; calling it after an explicit expansion keeps the
    # 'how' values comparable across the five rather than depending on which
    # control happened to trigger the expansion first.
    expanded = expand_toolbar_overflow(main_window)
    how = {}
    for name in ALL_FIVE:
        _hit, kind = find_including_overflow(main_window, name)
        # A control found only because WE expanded the chevron above is an
        # overflow case, not a direct one -- find_including_overflow cannot
        # know that, because by the time it looks the popup is already open.
        if kind == "direct" and not before[name]:
            kind = "overflow"
        how[name] = kind

    result = {"harness_geometry": harness_geometry, "sweep": sweep,
              "onset": onset, "ext_buttons": ext_before,
              "expanded": expanded, "before": before, "how": how}
    print("[#284 census] harness=%r" % (harness_geometry,), flush=True)
    print("[#284 census] width sweep (requested, achieved, ext_buttons): %r"
          % (sweep,), flush=True)
    print("[#284 census] onset=%r ext_buttons=%d expanded=%d"
          % (onset, ext_before, expanded), flush=True)
    print("[#284 census] how=%r" % (how,), flush=True)

    yield result

    # Teardown: put the window back to the size the harness established, so the
    # persisted geometry the next launch inherits is the harness's and not this
    # fixture's last ladder rung. In a `yield` fixture this runs even when a
    # test above it failed, which is the case that matters -- a failing run
    # that also poisons the next module is two defects reported as one.
    if _restore_to:
        try:
            got = _set_window_size(hwnd, int(_restore_to[0]),
                                   int(_restore_to[1]))
            print("[#284 census] restored window to %r (requested %r)"
                  % (got, tuple(_restore_to)), flush=True)
        except Exception as exc:  # noqa: BLE001
            # Never mask a test result with a teardown error, but never hide
            # the failure either: the next module's captures depend on it.
            print("[#284 census] WARNING: could not restore window geometry "
                  "to %r: %r -- subsequent visual captures in this session may "
                  "be taken at the wrong width (GitHub #279)"
                  % (tuple(_restore_to), exc), flush=True)
    else:
        print("[#284 census] WARNING: the harness never recorded an achieved "
              "geometry, so there is nothing to restore the window to; "
              "subsequent visual captures may inherit this fixture's width.",
              flush=True)


def test_the_census_is_attributable_to_a_known_window_geometry(census):
    """GitHub #284's determinism half, asserted rather than assumed.

    ``_resize_window`` guarantees a LOWER BOUND, not a value: the achieved
    width may be the requested 1600, any rung of its escalation ladder, or
    whatever the DPI/virtual-screen clamp yields — 1158px has been observed.
    This asserts the two facts that make that a defect rather than untidiness:

    * every width in the sweep was actually ACHIEVED, so the classification
      below is attributable to a real geometry rather than to a request; and
    * overflow onset sits BELOW the harness's 1400px lower bound, which is the
      whole problem. Every width from 1400 up satisfies the harness equally,
      and they do not classify alike — so "at least 1400, probably" cannot
      pin a pixel oracle or a UIA reachability assertion.
    """
    sweep = census["sweep"]
    assert sweep, "the width sweep recorded nothing"

    misses = [(req, ach) for req, ach, _n in sweep if abs(req - ach) > 2]
    assert not misses, (
        f"the window did not reach these requested widths: {misses}. The "
        f"census is then a measurement at an unknown geometry, which is the "
        f"#284 defect rather than a test problem — but it must not be "
        f"reported as a classification. Full sweep: {sweep}")

    onset = census["onset"]
    assert onset is not None, (
        f"no width between {WIDTH_LADDER[0]} and {WIDTH_LADDER[-1]}px produced "
        f"a toolbar overflow button, so the cluster-B condition could not be "
        f"created and nothing below is verifying anything. Either the toolbars "
        f"fit at {WIDTH_LADDER[-1]}px (implausible) or the resize is not "
        f"reaching the window. Sweep: {sweep}")

    assert onset["achieved"] < HARNESS_LOWER_BOUND, (
        f"overflow onset is {onset['achieved']}px, at or above the harness's "
        f"{HARNESS_LOWER_BOUND}px lower bound. That would mean the harness's "
        f"guaranteed-minimum window ALWAYS overflows, which is a different "
        f"(and worse) situation than #284 describes and changes what the "
        f"pixel-oracle cluster A fix has to do. Sweep: {sweep}")

    print(f"[#284] overflow onset {onset['achieved']}px; harness guarantees "
          f"only >= {HARNESS_LOWER_BOUND}px, so widths that satisfy the "
          f"harness span both sides of the overflow boundary.", flush=True)


def test_exactly_one_of_the_five_is_an_overflow_case_not_all_five(census):
    """THE criterion: 1 of 5, not all 5.

    This is the assertion the previous structure could not make. It is a
    statement about the whole set, so it is checked over the whole set in one
    place, and it fails in the direction that matters -- a change which made
    every control resolve through the chevron would turn this red instead of
    producing four skips.
    """
    how = census["how"]
    overflow = sorted(n for n, k in how.items() if k == "overflow")

    # NO SKIP HERE, deliberately. The previous version skipped when nothing was
    # overflowing -- and on this rig nothing was, because the harness window is
    # exactly 1600x1000 and onset is 1200. So the criterion's central assertion
    # never ran and the file reported "5 passed, 2 skipped". The census fixture
    # now CREATES the condition, so its absence is a failure of the test's own
    # precondition and is reported as one by
    # test_the_census_is_attributable_to_a_known_window_geometry.
    assert census["ext_buttons"] > 0, (
        f"the census was taken at a width with no overflow button, so "
        f"'1 of 5' cannot be measured. onset={census['onset']!r} "
        f"sweep={census['sweep']!r}")

    assert overflow == [MEASURED_OVERFLOW], (
        f"GitHub #284 attributes all five of {list(ALL_FIVE)} to toolbar "
        f"overflow; exactly one of them actually is. Controls classified as "
        f"overflow here: {overflow}. Full census: {how}. If this lists all "
        f"five, the conflation the tri-state helper exists to prevent has "
        f"come back; if it lists none, the overflow-aware lookup has stopped "
        f"working and cluster B is no longer fixed.")


def test_the_overflowed_control_goes_from_unreachable_to_reachable(census):
    """Cluster B as a BEFORE/AFTER transition: unreachable, then reachable.

    That transition is what makes the helper a fix rather than a no-op. If the
    control were reachable all along, the helper would be untested by this file
    no matter how green it looked.
    """
    assert census["how"][MEASURED_OVERFLOW] != "absent", (
        f"{MEASURED_OVERFLOW} is not reachable even after expanding the "
        f"toolbar overflow ({census['ext_buttons']} extension button(s) "
        f"present, {census['expanded']} expanded), so GitHub #284 cluster B "
        f"is NOT fixed")

    if not census["before"][MEASURED_OVERFLOW]:
        assert census["how"][MEASURED_OVERFLOW] == "overflow", (
            f"the plain lookup missed {MEASURED_OVERFLOW} but the "
            f"overflow-aware lookup reported "
            f"{census['how'][MEASURED_OVERFLOW]!r} rather than 'overflow'")
    else:
        assert census["how"][MEASURED_OVERFLOW] == "direct"


@pytest.mark.parametrize("name", MEASURED_ABSENT)
def test_the_absent_controls_are_not_reclassified_as_overflow(census, name):
    """Pin the distinction the issue conflated, in both failing directions.

    These four are not in the UIA tree at all — expanding a chevron does
    nothing for them. Two things must not happen silently:

    * being reported as ``overflow``, which folds them back into cluster B and
      lets a fix for one control appear to address five;
    * quietly resolving, which is GOOD NEWS (they would finally be exposed to
      UIA) but changes what GitHub #284 says and must therefore be noticed.

    The previous version skipped on the second case and, because a resolving
    control takes that branch first, on the first case too. Both are failures
    here: a skip is how this stopped being able to fail. A ratchet that
    reddens on an improvement costs one roster update; a skip that hides a
    regression costs the defect coming back unobserved.
    """
    how = census["how"][name]
    assert how != "overflow", (
        f"{name} was classified as 'overflow'. It was measured ABSENT from the "
        f"UIA tree with and without expansion during p96 phase-7, and it is a "
        f"separate defect from cluster B. Classifying it as overflow is "
        f"exactly the conflation GitHub #284 made: a chevron-expanding fix "
        f"then appears to address five controls while fixing one. Census: "
        f"{census['how']}")
    assert how == "absent", (
        f"GOOD NEWS, NOT A PRODUCT REGRESSION: {name} now resolves as {how!r}, "
        f"having been measured ABSENT during p96 phase-7. That means it is "
        f"finally exposed to UIA. Update MEASURED_ABSENT in this file and "
        f"GitHub #284 to match. This is deliberately a failure rather than a "
        f"skip -- the skip it replaces also swallowed the 'reclassified as "
        f"overflow' regression above, which is the one case this file exists "
        f"to catch.")
