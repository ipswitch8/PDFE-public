# -*- coding: utf-8 -*-
"""Reach toolbar controls that Qt has moved into the overflow ("chevron") menu.

GitHub #284 cluster B. Seven UI-matrix cases fail with
``formToolActionButton not reachable``. Measured during p96 phase-7 with
``docs/audit/p96-phase7/probe_toolbar_overflow_reachability.py``:

    toolbar extension buttons found: 1
      QApplication.mainWindow.annotationToolBar.qt_toolbar_ext_button

    zoomCombo             ABSENT   (not in the UIA tree either way)
    actionRotateView180   ABSENT   (not in the UIA tree either way)
    formToolActionButton  OVERFLOW (only after expanding the chevron)
    rotatePage180Action   ABSENT   (not in the UIA tree either way)
    deletePageAction      ABSENT   (not in the UIA tree either way)

So the overflow attribution is correct for ``formToolActionButton`` and WRONG
for the others, which are absent from the UIA tree with or without expansion
and are a separate defect. This helper addresses only the measured case; it
deliberately does not pretend to fix the rest, and
:func:`find_including_overflow` reports which situation it hit so a caller
cannot confuse them.

WHY A SEPARATE HELPER RATHER THAN PATCHING ``find_by_suffix``
=============================================================
``find_by_suffix`` is called from dozens of sites for dialogs, panels and
menus. Teaching it to click a chevron on every miss would make an ordinary
"not found" open a popup as a side effect, changing the UI state underneath
whatever the caller does next. Expanding a toolbar is a deliberate act and
should read as one at the call site.
"""

from __future__ import annotations

from typing import Callable, Optional, Tuple

#: Qt's internal object name for a toolbar's overflow button. Not ours to
#: rename, and stable across Qt 5/6 -- it is set in qtoolbarlayout.cpp.
EXT_BUTTON_SUFFIX = "qt_toolbar_ext_button"

#: How long to keep looking after expanding the chevron before concluding the
#: control really is absent. Generous on purpose: the cost of being too patient
#: is seconds on a path only taken when a control was NOT found directly, while
#: the cost of being too hasty is a false "not reachable" -- which is the exact
#: failure this helper exists to prevent, and which it caused for
#: `linkToolActionButton` until this poll replaced an immediate re-search.
OVERFLOW_SETTLE_TIMEOUT_S = 6.0
OVERFLOW_POLL_INTERVAL_S = 0.25


def toolbar_extension_buttons(root) -> list:
    """Every toolbar overflow button currently in ``root``'s UIA subtree.

    A toolbar only creates one when it actually has hidden actions, so an
    empty list means nothing has overflowed -- which is a meaningful answer,
    not a failure.
    """
    found = []
    try:
        descendants = root.descendants()
    except Exception:  # noqa: BLE001
        return found
    for d in descendants:
        try:
            aid = d.element_info.automation_id or ""
        except Exception:  # noqa: BLE001
            continue
        if aid.endswith(EXT_BUTTON_SUFFIX):
            found.append(d)
    return found


def expand_toolbar_overflow(root) -> int:
    """Click every overflow button found. Returns how many were clicked.

    ``click_input`` rather than ``invoke()``: the extension button is exposed
    as a CheckBox by Qt's UIA bridge (measured), and its toggle pattern does
    not reliably open the popup, whereas a real click does.
    """
    # ACTIVATE THE WINDOW FIRST. `click_input` synthesises a real mouse click at
    # screen coordinates, and Windows delivers that to whatever is foreground --
    # so against a background window the press can be swallowed activating it,
    # or the popup can open and close again immediately.
    #
    # HONESTY NOTE: this was added while chasing GitHub #253's
    # `linkToolActionButton` false negative and it did NOT fix it -- the actual
    # cause was a caller sizing the window WIDER THAN THE SCREEN, which put the
    # chevron (the rightmost item on the toolbar) past the right edge, where a
    # screen-coordinate click lands nowhere. See
    # `tests/test_ui_issue253_wired_components.py`, which now clamps to
    # `GetSystemMetrics(0)`.
    #
    # That this function returns 1 in that situation -- counting a click that
    # could not land -- is GitHub #310, which proposes keying the return value
    # on an observable the expansion PRODUCES instead of on the absence of an
    # exception. Until that lands, callers must not read a nonzero return as
    # proof the popup opened.
    #
    # It is kept because it is correct on its own terms rather than because it
    # fixed anything: clicking a background window is genuinely unreliable. The
    # note is here so nobody later reads this as the cure for that failure and
    # mis-diagnoses the next one.
    # Enumerate BEFORE focusing, and focus only if there is actually something
    # to click. Raising a window is a side effect, and this helper is called
    # from tests that may have a dialog in front; doing it unconditionally would
    # mean every miss on a window with no overflow at all still reordered the
    # z-order under the caller.
    buttons = toolbar_extension_buttons(root)
    if not buttons:
        return 0
    try:
        root.set_focus()
    except Exception:  # noqa: BLE001
        pass  # Not fatal: a click on an already-foreground window still works.

    clicked = 0
    for btn in buttons:
        try:
            btn.click_input()
            clicked += 1
        except Exception:  # noqa: BLE001
            continue
    return clicked


def find_including_overflow(
    root,
    object_name: str,
    finder: Optional[Callable] = None,
) -> Tuple[object, str]:
    """Find ``object_name``, expanding toolbar overflow if the first pass misses.

    Returns ``(widget_or_None, how)`` where ``how`` is one of:

      ``"direct"``    found without touching the toolbar;
      ``"overflow"``  found only after expanding -- GitHub #284 cluster B;
      ``"absent"``    not found either way, and NOT an overflow problem.

    The tri-state matters. Collapsing "absent" into "not found" is what let
    #284 describe seven failures as overflow when only one control actually
    was: a caller that cannot tell the two apart will keep reaching for the
    chevron to fix a control that was never in the tree.
    """
    if finder is None:
        from tests.ui_automation.harness.dialog_helpers import find_by_suffix
        finder = find_by_suffix

    hit = finder(root, object_name)
    if hit is not None:
        return hit, "direct"

    if expand_toolbar_overflow(root) == 0:
        # Nothing had overflowed, so expansion cannot be the explanation.
        return None, "absent"

    # SETTLE, THEN POLL. The first version searched immediately after the click.
    # Qt creates the popup and its buttons in RESPONSE to that click, so an
    # instant re-search can look before they exist and report "absent" for a
    # control that is in the popup.
    #
    # MEASURED (GitHub #253, `build/gtest_p97p7/probe_i253_overflow_popup.py`):
    # once expansion really happens, six ids appear that were not there before
    # -- circleToolActionButton, lineToolActionButton, linkToolActionButton,
    # stampToolActionButton, fileAttachmentToolActionButton and
    # annotColorPickerButton. In that probe a raw scan read ABSENT at t+0.0s and
    # found the control from t+0.5s on, so the race is real and observed.
    #
    # Scope, stated plainly: this poll did NOT fix #253's false negative (an
    # off-screen chevron did -- see `expand_toolbar_overflow`). `find_by_suffix`
    # actually resolved the control at t+0.0s in that same probe, so the race
    # window is narrow. The poll is cheap insurance on a path only reached after
    # a direct lookup already missed, and it costs nothing when the first
    # attempt succeeds.
    import time

    deadline = time.monotonic() + OVERFLOW_SETTLE_TIMEOUT_S
    while True:
        hit = finder(root, object_name)
        if hit is not None:
            return hit, "overflow"
        if time.monotonic() >= deadline:
            return None, "absent"
        time.sleep(OVERFLOW_POLL_INTERVAL_S)
