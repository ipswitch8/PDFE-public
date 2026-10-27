# -*- coding: utf-8 -*-
"""GitHub #253 — the four components wired in p97 phase-9 are really reachable.

WHAT #253 FOUND
===============
Eighteen classes were compiled into PDFE, exercised by the test suite, and
unreachable from `main()` by any chain of code references. A class in that state
cannot be reached by any user action, and — this is the whole point of test-gap
class C-BELOW — **a correct, tested, unreachable component looks exactly like
one that works**.

`tools/gate/check_engine_wiring.py` is the standing control. It computes a
transitive closure from `src/main.cpp` over *code* references (comments and
string literals stripped), so it is the authority on reachability and it now
fails if any of these four stops being reached.

WHY THIS FILE EXISTS AS WELL
============================
That guard is a STATIC analysis. It proves a reference chain exists; it cannot
prove the user can actually get at the thing. A control can be constructed in
code the guard can see and still be absent from the UI, disabled forever, or
unnamed and therefore invisible to automation — and this project's own history
is full of exactly that gap (`0056`: every oracle called `followLinkAt`
directly while the click route was dead for every user).

So this file asserts the user-facing half: that each new affordance is present
in the live window and addressable by automation. Together with the guard, both
halves of "reachable" are covered.

The four, and what the decision was:

  * **LinkTool** — the eleventh of eleven annotation tools; the other ten were
    reachable. Wired as `linkToolAction` on the annotation toolbar.
  * **DeleteFormFieldCommand** — six `FormCommands` siblings exist, four were
    referenced, Delete was not. There was no delete-field UI at all. Wired as
    `formDeleteFieldAction` on the form toolbar.
  * **SetChoiceSelectionCommand** — not merely unreachable: choice edits went
    through `SetFieldValueCommand`, which takes a single value STRING, so a
    multi-select list box silently committed only its FIRST selection. Now
    routed through the command that models a `vector<int>`.
  * **ModifyRedactionCommand** — there was no modify-redaction operation, so
    there was nothing for it to undo. Wired behind
    `redactionPropertiesButton` on the redaction panel.

Run:  python -m pytest tests/test_ui_issue253_wired_components.py -q
"""
from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[1]
assert (REPO_ROOT / ".github" / "workflows").is_dir(), (
    f"repo root misresolved to {REPO_ROOT!r}")
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

#: objectName -> what it makes reachable. Each is set in the C++ so pywinauto
#: can resolve it by auto_id, per the project's automation-friendliness rule.
#: The ids below are what UIA actually exposes, which is NOT the QAction
#: objectName. Qt's UIA bridge exposes the QToolButton, and MainWindow's
#: deferred naming pass sets its objectName to `action->objectName() + "Button"`
#: -- which is why phase-7's #284 work searched for `formToolActionButton`.
#:
#: Looking up the bare action name finds nothing, and the first version of this
#: file did exactly that and reported all three as absent. What caught it was
#: including `inkToolAction` in the probe: a tool that has worked for the life of
#: the project also read as missing, so the LOOKUP was wrong rather than the
#: product. A control known to be good is worth carrying in a diagnostic for
#: precisely that reason.
WIRED_CONTROLS = {
    "linkToolActionButton": "LinkTool (the eleventh annotation tool)",
    "formDeleteFieldActionButton": "DeleteFormFieldCommand",
    "redactionPropertiesButton": "ModifyRedactionCommand",
}


#: Requested window width. An ASPIRATION, not a guarantee -- and the name is
#: aspirational too, which is why this comment is explicit about it.
#:
#: The intent is a width where the annotation toolbar's 15 actions all fit, so
#: this test answers the narrow question "is the control on the toolbar at all?"
#: without entangling GitHub #284's separate question of whether overflow is
#: reachable. On a wide enough desktop that is what happens.
#:
#: IT DOES NOT HAPPEN ON THIS ONE, and the honest statement matters more than
#: the tidy one. This desktop is 1874px, the fixture clamps the request to
#: `GetSystemMetrics(0)` (it must -- see the fixture), and 15 TextBesideIcon
#: actions do not fit in 1874, so the chevron is still present and
#: `linkToolActionButton` is still reached via `overflow`. MEASURED by
#: `docs/audit/p97-phase9/probes/probe_i253_overflow_popup.py`: at that width
#: the toolbar shows 12 buttons plus a chevron, and expanding it reveals
#: circle, line, link, stamp, file-attachment and the colour picker.
#:
#: An earlier version of this comment claimed `linkToolActionButton` reads
#: `direct` at 1894px. The probe above contradicts that, so it is corrected
#: here rather than left to be trusted.
#:
#: Hence `find_including_overflow` below, and hence `overflow` counting as
#: reachable: on a narrow desktop it is the only honest answer, and a control in
#: the chevron genuinely is reachable by a user.
WIDE_ENOUGH_FOR_FULL_TOOLBAR = 1920


@pytest.fixture(scope="function")
def session(tmp_path_factory):
    """A PDFE session with ISOLATED settings, widened to avoid overflow.

    FUNCTION-SCOPED so each control gets an independent measurement. Two of the
    three cases mutate the window to reach their control -- one clicks the Form
    tool to reveal `formToolBar_`, another the Redaction tool to reveal its dock
    -- and expanding a toolbar chevron is irreversible as far as UIA is
    concerned. A case that has expanded one has spent that window and cannot
    then measure a second control independently. Three launches is the price of
    three independent answers.

    HONESTY NOTE ON WHY THIS CHANGED: the scope was switched from module to
    function while chasing a `linkToolActionButton` false failure, on the theory
    that the first case was contaminating the second. That theory was WRONG --
    switching the scope did not fix it, and the actual cause was this fixture
    sizing the window wider than the screen (see the clamp below). The scope is
    kept on its own merit, stated above, and not as the cure for that failure.
    Recorded so the next person debugging this file does not inherit a
    confident-sounding mechanism that was never true.

    Settings isolation is not optional. `MainWindow` persists geometry and
    dock/toolbar state on close and restores them on open, so without
    `PDFE_SETTINGS_PATH` this test would both inherit the developer's window
    size and WRITE ITS OWN 1920px geometry back into the real profile -- a test
    that silently reconfigures the machine it runs on. The same trap was found
    and fixed in the #284 overflow census; this reuses that fix.

    The saved TOOLBAR LAYOUT used to escape that isolation: `MainWindow` built
    `ToolbarLayoutManager` bare, so it read and wrote HKCU no matter what
    `PDFE_SETTINGS_PATH` said, and this test inherited the developer's 14-button
    layout instead of the shipped 17-button default. Both sites now pass
    `AppSettings::settingsFilePath()`, so the isolation is real in both
    directions and `onCustomizeToolbars` can no longer write to the real
    profile from a test run (GitHub #309, p97 phase-9).

    `_has_saved_toolbar_layout()` is therefore DIAGNOSTIC ONLY now -- it is
    reported in the failure message and never gates the verdict. If it reads
    True while a control is missing, that points at the seam having regressed
    rather than at the action never having been added.
    """
    if not os.environ.get("SESSIONNAME"):
        pytest.skip("no interactive desktop session")
    from tests.helpers.pdfe_session import PDFESession
    from tests.test_ui_file_operations import FIX_SINGLE

    ini = tmp_path_factory.mktemp("i253") / "settings.ini"
    prev = os.environ.get("PDFE_SETTINGS_PATH")
    os.environ["PDFE_SETTINGS_PATH"] = str(ini)
    try:
        with PDFESession(pdf_path=str(FIX_SINGLE)) as s:
            try:
                from tests.visual.test_toolbar_overflow_reachability import (
                    _set_window_size)
                hwnd = s.window.element_info.handle
                # CLAMP TO THE DESKTOP. A window wider than the screen pushes
                # the toolbar's overflow chevron -- the rightmost item on it --
                # past the right edge, and `click_input` synthesises a click at
                # SCREEN coordinates, so a chevron at x > screen width is clicked
                # nowhere. That reads back as "expanded, still absent", which is
                # indistinguishable from a genuinely missing control unless the
                # geometry is checked. This desktop is 1874 wide and the request
                # below was 1920.
                import ctypes as _ct
                screen_w = _ct.windll.user32.GetSystemMetrics(0)
                want_w = min(WIDE_ENOUGH_FOR_FULL_TOOLBAR, screen_w)
                got_w, _got_h = _set_window_size(hwnd, want_w, 1000)
                s._i253_screen_width = screen_w
                s._i253_requested_width = want_w
                # Reported, never assumed: SetWindowPos is a request, and a
                # desktop narrower than 1920 will clamp it. A clamped width is
                # why a control might still overflow, so it must be visible in
                # the failure output rather than inferred.
                s._i253_achieved_width = got_w
            except Exception as exc:  # noqa: BLE001
                s._i253_achieved_width = f"resize failed: {exc!r}"
            yield s
    finally:
        if prev is None:
            os.environ.pop("PDFE_SETTINGS_PATH", None)
        else:
            os.environ["PDFE_SETTINGS_PATH"] = prev


def test_the_engine_wiring_guard_agrees_these_are_reachable():
    """The static half, asserted here so a failure names #253 rather than
    appearing as an unrelated guard failure in CI.

    Headless: this runs the guard, which is pure static analysis over `src/`.
    """
    import subprocess

    r = subprocess.run(
        [sys.executable, str(REPO_ROOT / "tools" / "gate" / "check_engine_wiring.py")],
        capture_output=True, text=True, cwd=str(REPO_ROOT))
    out = r.stdout + r.stderr
    assert r.returncode == 0, (
        "check_engine_wiring.py reports an unreachable class that is not "
        "baselined. If one of #253's newly-wired four has regressed, this is "
        "where it shows up:\n" + out[-3000:])

    # Non-vacuity: a pass that examined nothing proves nothing.
    assert "unreachable class" in out, out[-2000:]

    # And the four must NOT be in the baseline any more — a baselined entry
    # would make the guard pass while the class is unreachable again.
    import json
    import io as _io
    baseline = json.load(_io.open(
        REPO_ROOT / "tools" / "gate" / "engine_wiring_baseline.json",
        encoding="utf-8"))
    for cls in ("LinkTool", "DeleteFormFieldCommand",
                "SetChoiceSelectionCommand", "ModifyRedactionCommand"):
        assert cls not in baseline, (
            f"{cls} is back in engine_wiring_baseline.json. It was wired in p97 "
            f"phase-9 and its entry removed deliberately; re-adding it makes "
            f"the guard green while the class is unreachable, which is the "
            f"false green #253 is about.")


@pytest.mark.parametrize("object_name", sorted(WIRED_CONTROLS))
def test_each_wired_control_is_present_and_addressable(session, object_name):
    """The user-facing half: can the control actually be reached?

    WHY THIS IS OVERFLOW-AWARE AND SAVED-LAYOUT-AWARE -- both were MEASURED
    while wiring these, and each would otherwise make this test lie.

    1. THE ANNOTATION TOOLBAR OVERFLOWS, AND ON THIS DESKTOP IT ALWAYS WILL.
       With the full action set it carries 15 wide (TextBesideIcon) actions, and
       Qt pushes whatever does not fit into the chevron, where UIA cannot see it
       until it is expanded. MEASURED at 1894px (this desktop's maximum, see the
       width constant above): 12 buttons visible plus one chevron, with circle,
       line, link, stamp, file-attachment and the colour picker revealed only
       after expanding. So `linkToolActionButton` is reached via `overflow`
       here, not `direct`.

       That is GitHub #284's territory, and `find_including_overflow` is the
       tool phase-7 built for exactly it -- so `overflow` counts as reachable,
       because a control in the chevron genuinely is reachable by a user.

    2. A SAVED TOOLBAR LAYOUT HIDES NEW ACTIONS ENTIRELY. `loadSavedToolbarLayout()`
       rebuilds each toolbar from the stored layout, so an action added to
       MainWindow after that layout was saved is dropped -- not overflowed,
       absent. Measured on this machine: 14 buttons with the saved layout
       present, 17 with it removed. That is GitHub #309, a pre-existing product
       defect this work uncovered.

       RESOLVED FOR THIS TEST BY CLOSING THE SEAM, not by tolerating it.
       `ToolbarLayoutManager` was constructed bare in `MainWindow`, so it read
       and wrote HKCU even when `PDFE_SETTINGS_PATH` had moved every other
       settings consumer onto a throwaway INI. It now honours that path on both
       the load and save sides, so this test sees a clean layout store and the
       shipped default toolbar, and `onCustomizeToolbars` can no longer write to
       the developer's real layout from a test run.

       An earlier version of this file instead DETECTED a saved layout and
       skipped. That was wrong twice over: it left the result dependent on
       un-isolated machine state, and the two skips it added were exactly the
       WIN-45 reachability-skip shape -- a run reporting not-a-failure because
       the product could not be driven. `check_reachability_skip_shape.py`
       flagged both as NEW findings against the 413-site baseline, which is the
       guard working as designed. Fixing the seam removed the need for them.

    3. THE WINDOW MUST NOT BE WIDER THAN THE SCREEN. The chevron is the
       rightmost item on the toolbar, and `click_input` synthesises a click at
       screen coordinates, so a window wider than the desktop puts the chevron
       where no click can reach it -- and `expand_toolbar_overflow` still
       returns 1, because the click raised nothing. The result is a confident
       "control not reachable" for a control sitting in the overflow popup.
       MEASURED: requesting 1920 on this 1874px desktop achieved 1894 and
       produced exactly that false failure; clamping to `GetSystemMetrics(0)`
       made the same assertion pass against the same binary. The helper's part
       of this is GitHub #310; the clamp below is this test's part.
    """
    from tests.helpers.toolbar_overflow import find_including_overflow

    # Activate the surface that HOSTS the control first, the way a user would.
    # Two of the three live on surfaces that are hidden at startup --
    # `formToolBar_` is created with setVisible(false) and `redactionPanelDock_`
    # is created hidden -- and UIA does not expose a hidden widget. Looking
    # without activating reports "absent" for a perfectly wired control, which
    # is a false failure, and the first version of this test produced exactly
    # that for `redactionPropertiesButton`.
    host_problem = _activate_host_surface(session, object_name)

    hit, how = find_including_overflow(session.window, object_name)
    if hit is not None:
        assert how in ("direct", "overflow"), how
        return

    # Absent -- and if the host surface never opened, say so, because that is a
    # different fault with a different fix. Still a FAILURE, not a skip: the
    # control could not be reached, whatever the reason, and dressing that up as
    # "nothing to report" is the WIN-45 false green this file is part of
    # burning down.
    if host_problem is not None:
        pytest.fail(
            f"{object_name} could not be evaluated: {host_problem}. The control "
            f"may well be wired -- the engine-wiring guard above is the "
            f"authority on that -- but this check could not confirm a user can "
            f"reach it, which is a failure of this check and not a pass.")

    # DIAGNOSTICS IN THE FAILURE, not in a throwaway probe script. Three
    # successive theories about an earlier failure here (stale binary, overflow
    # width, cross-test chevron contamination) were each plausible, each wrong,
    # and each cost a full launch cycle to disprove -- because the failure
    # message said only "not reachable" and the evidence had to be recreated
    # from outside. These three lines distinguish the stages that can fail:
    # nothing overflowed / it overflowed and expansion failed / it expanded and
    # the control still is not there.
    from tests.helpers.toolbar_overflow import toolbar_extension_buttons

    try:
        exts = toolbar_extension_buttons(session.window)
        ext_count = len(exts)
        # The chevron's SCREEN rect, against the screen bounds. An off-screen
        # chevron cannot be clicked, and that must be visible here rather than
        # inferred -- it is the difference between "not on the toolbar" and
        # "could not reach the control that reveals it".
        import ctypes as _ct
        sw = _ct.windll.user32.GetSystemMetrics(0)
        ext_rects = []
        for b in exts:
            try:
                r = b.rectangle()
                ext_rects.append(
                    f"L{r.left},R{r.right} (screen width {sw}; "
                    f"{'OFF-SCREEN' if r.right > sw or r.left < 0 else 'on screen'})")
            except Exception as exc:  # noqa: BLE001
                ext_rects.append(f"rect failed: {exc!r}")
    except Exception as exc:  # noqa: BLE001
        ext_count = f"query failed: {exc!r}"
        ext_rects = []
    raw_hits = []
    try:
        for d in session.window.descendants():
            try:
                aid = d.element_info.automation_id or ""
            except Exception:  # noqa: BLE001
                continue
            if "annotationToolBar." in aid:
                raw_hits.append(aid.split("annotationToolBar.")[-1])
    except Exception as exc:  # noqa: BLE001
        raw_hits = [f"scan failed: {exc!r}"]

    pytest.fail(
        f"{object_name} is not reachable, with no saved toolbar layout to "
        f"explain it, so {WIRED_CONTROLS[object_name]} cannot be reached by any "
        f"user action.\n"
        f"  overflow chevrons present: {ext_count} {ext_rects}\n"
        # Reported, not acted on. If this says True while the app under test is
        # supposed to be reading an isolated INI, the PDFE_SETTINGS_PATH seam in
        # ToolbarLayoutManager has regressed and GitHub #309's masking is back
        # -- which is a very different diagnosis from "the action was never
        # added". It must never gate the verdict, though: that was the skip this
        # file used to take, and it made the result depend on machine state.
        f"  a layout exists in the REAL HKCU store (informational; the app "
        f"under test should be isolated from it): "
        f"{_has_saved_toolbar_layout()}\n"
        f"  annotationToolBar children seen by a raw scan ({len(raw_hits)}): "
        f"{sorted(raw_hits)}\n"
        f"  target in that raw scan: {object_name in raw_hits}\n"
        f"Window width achieved: "
        f"{getattr(session, '_i253_achieved_width', 'unknown')} (requested "
        f"{WIDE_ENOUGH_FOR_FULL_TOOLBAR}); if that came back clamped well below "
        f"the request, the desktop is too narrow and the control may merely "
        f"have overflowed rather than being absent.")


#: control id -> the tool whose activation reveals the surface hosting it.
#: Clicking the tool is the real user route: activating the Form tool shows
#: `formToolBar_`, and activating the Redaction tool shows the redaction dock.
_HOST_TOOL = {
    "formDeleteFieldActionButton": "formToolActionButton",
    "redactionPropertiesButton": "redactToolActionButton",
}


def _activate_host_surface(session, object_name: str):
    """Click the tool that reveals `object_name`'s host surface, if any.

    Returns None on success or when no activation is needed, else a string
    saying why activation could not happen. The caller must report that reason
    INSTEAD of "the control is missing": if the Redaction tool button is itself
    hidden, nothing has been learned about `redactionPropertiesButton`, and
    blaming the button would be a false failure pointing at the wrong control.
    """
    from tests.helpers.toolbar_overflow import find_including_overflow

    host = _HOST_TOOL.get(object_name)
    if not host:
        return None  # Lives on the annotation toolbar, visible from startup.
    try:
        hit, _how = find_including_overflow(session.window, host)
    except Exception as exc:  # noqa: BLE001
        return f"could not search for host tool {host}: {exc!r}"
    if hit is None:
        return (f"the host tool {host} is itself not reachable, so the surface "
                f"carrying {object_name} was never shown")
    try:
        hit.click_input()
    except Exception as exc:  # noqa: BLE001
        return f"host tool {host} found but not clickable: {exc!r}"
    import time
    time.sleep(1.5)  # let Qt show the toolbar / dock and build its children
    return None


def _has_saved_toolbar_layout() -> bool:
    """True when HKCU holds a saved toolbar layout.

    Read-only. Returns False off-Windows or if the key cannot be read -- an
    unreadable registry must not be reported as "a layout is hiding it", which
    would turn a real failure into a skip.
    """
    try:
        import winreg  # noqa: PLC0415 -- Windows-only, imported where used
    except ImportError:
        return False
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                            "Software" + chr(92) + "PDFE" + chr(92) + "PDFE"
                            + chr(92) + "pdfe" + chr(92) + "toolbarLayout") as k:
            winreg.QueryValueEx(k, "toolbars")
            return True
    except OSError:
        return False
