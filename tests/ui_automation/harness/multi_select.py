# -*- coding: utf-8 -*-
"""Multi-select helper for PagePanel QListWidget items.

Uses keyboard-based range selection: click the first item to give
the list focus, then Shift+Down to extend selection downward.
This is more reliable than Ctrl+click through pywinauto on
QListWidget, which only selects the last clicked item.
"""
from __future__ import annotations

import time
from typing import List

from tests.ui_automation.harness.dialog_helpers import find_by_suffix


def select_page_items(fx, indices: List[int]) -> bool:
    """Select PagePanel ListItems at the given 0-based indices.

    Pipeline b5e3a9f2 phase-6 (P-GAP-73): prefers the
    ``testSelectPagesPipe`` pipe which calls
    ``PagePanel::selectPagesProgrammatic`` (Q_INVOKABLE) to set the
    selection model directly. This bypasses Qt6 ExtendedSelection's
    flaky Ctrl/Shift+click + keyboard-modifier flow via pywinauto.
    Falls back to the original click + Shift+Down approach if the
    pipe is not available (e.g. PDFE_TEST_PIPES unset). Non-
    contiguous selections (e.g. [0, 2]) only work via the pipe;
    the Shift+Down fallback handles only contiguous ranges.
    """
    pipe = find_by_suffix(fx.main_window, "testSelectPagesPipe")
    if pipe is not None:
        try:
            pipe.set_edit_text(",".join(str(i) for i in indices))
            time.sleep(0.3)
            return True
        except Exception:
            pass  # fall through to keyboard approach

    panel = find_by_suffix(fx.main_window, "pagePanelList")
    if panel is None:
        return False
    items = [
        d for d in panel.descendants()
        if d.element_info.control_type == "ListItem"
    ]
    if not items:
        return False

    first = min(indices)
    last = max(indices)
    if first < 0 or last >= len(items):
        return False

    try:
        items[first].click_input()
        time.sleep(0.2)
        for _ in range(last - first):
            panel.type_keys("+{DOWN}", pause=0.1)
            time.sleep(0.1)
    except Exception:
        return False

    time.sleep(0.2)
    return True
