# -*- coding: utf-8 -*-
"""IUIAutomation::GetFocusedElement wrapper for tab-order tests.

Replaces the descendants()-based focus walk in _record_focused_aid which
unreliably reports has_keyboard_focus() == True on Qt 6.10 child widgets
after Tab.  Direct UIA API query is authoritative.

Preferred usage::

    from tests.helpers.uia_focus import get_focused_element

    element = get_focused_element()
    if element:
        print(element.best_id)  # automation_id or name
"""
from __future__ import annotations

from typing import Optional, NamedTuple


class FocusedElement(NamedTuple):
    automation_id: str  # Qt objectName exposed as UIA AutomationId
    name: str           # visible text / accessibleName
    class_name: str     # Qt widget class name

    @property
    def best_id(self) -> str:
        """Preferred stable identifier: automation_id if set, else name."""
        return self.automation_id or self.name


def get_focused_element() -> Optional[FocusedElement]:
    """Return what UIA reports as currently focused, or None if unavailable.

    Uses pywinauto's already-initialised IUIAutomation singleton (which is
    guaranteed to be available on Windows since pywinauto depends on
    comtypes).  Falls back to a direct comtypes typelib path when
    pywinauto is absent.  Returns None if anything fails — callers should
    treat that as "could not determine focus".

    The returned AutomationId is the full UIA string (which Qt may prefix
    with "QApplication.<parent>." depending on the widget hierarchy).
    Callers that need the bare objectName should call
    ``_strip_qt_aid_prefix(element.automation_id)`` or use
    ``element.automation_id.rsplit(".", 1)[-1]``.
    """
    # --- preferred path: reuse pywinauto's already-warm IUIA singleton -------
    try:
        from pywinauto.uia_defines import IUIA  # type: ignore[import]
        iuia = IUIA()
        elem = iuia.get_focused_element()
        if elem is None:
            return None
        return FocusedElement(
            automation_id=str(elem.CurrentAutomationId or ""),
            name=str(elem.CurrentName or ""),
            class_name=str(elem.CurrentClassName or ""),
        )
    except Exception:  # noqa: BLE001
        pass

    # --- fallback path: direct comtypes typelib binding ----------------------
    try:
        import comtypes.client  # type: ignore[import]
        comtypes.client.GetModule(("{944de083-8fb8-45cf-bcb7-c477acb2f897}", 1, 0))
        from comtypes.gen.UIAutomationClient import (  # type: ignore[import]
            CUIAutomation,
            IUIAutomation,
        )
        ua = comtypes.client.CreateObject(CUIAutomation, interface=IUIAutomation)
        elem = ua.GetFocusedElement()
        if elem is None:
            return None
        return FocusedElement(
            automation_id=str(elem.CurrentAutomationId or ""),
            name=str(elem.CurrentName or ""),
            class_name=str(elem.CurrentClassName or ""),
        )
    except Exception:  # noqa: BLE001
        return None
