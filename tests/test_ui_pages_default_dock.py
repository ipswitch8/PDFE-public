# -*- coding: utf-8 -*-
"""End-to-end pywinauto test for bugfix-3e91a7c4 #3 — the Pages panel must be
the default/active left-dock tab on a fresh launch (the reported bug was that
the left dock came up showing only Layers).

This is the pywinauto realization oracle required by the Phase-4 acceptance
criteria (a UIA test asserting the Pages panel — not Layers — is the
default/active left-dock tab on fresh launch, populated with a thumbnail/entry
per page for a multi-page PDF). It complements the race-free C++ structural
test in ``tests/test_ui_structure_bugs.cpp`` by verifying the behaviour end to
end against the live built ``pdfe.exe``.

VISUAL: no -- asserted via the UIA tree (dock visibility + populated
pagePanelList), which is deterministic and session-independent; a screenshot
diff would only re-test the OS compositor.

Run with::

    python -m pytest tests/test_ui_pages_default_dock.py -v
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
REPO_ROOT = _HERE.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.helpers.pdfe_session import (  # noqa: E402
    PDFESession,
    PDFE_EXE,
    no_orphans,
)

FIXTURES = REPO_ROOT / "tests" / "fixtures"
FIX_3PAGE = FIXTURES / "test_3page.pdf"


def _has_pdfe_binary() -> bool:
    return Path(PDFE_EXE).exists()


def _is_interactive_session() -> bool:
    """UIA needs a real interactive Windows desktop session."""
    if os.environ.get("PDFE_UI_FORCE_HEADLESS") == "1":
        return False
    if sys.platform != "win32":
        return False
    try:
        import ctypes
        # A window station with a visible desktop is required for UIA.
        return bool(ctypes.windll.user32.GetProcessWindowStation())
    except Exception:  # noqa: BLE001
        return False


pytestmark = [
    pytest.mark.skipif(not _has_pdfe_binary(),
                       reason=f"pdfe.exe not built at {PDFE_EXE}"),
    pytest.mark.skipif(not _is_interactive_session(),
                       reason="UIA requires an interactive Windows desktop"),
]


def test_pages_is_default_left_dock_tab_on_launch():
    """On a fresh launch with a multi-page PDF, the left dock shows the Pages
    panel (populated, one entry per page) as the active tab — not Layers."""
    assert FIX_3PAGE.exists(), f"fixture missing: {FIX_3PAGE}"

    with PDFESession(pdf_path=str(FIX_3PAGE)) as s:
        s.wait_for_window_title("PDFE")

        # #3: the Pages dock must be present and visible as the active left tab.
        pages_dock = s.find(auto_id="pagePanelDock")
        assert pages_dock.is_visible(), \
            "Pages dock must be the visible/active left-dock tab on launch"

        # It must be populated with one thumbnail entry per page. The
        # pagePanelList items surface to UIA as ListItem elements (their text is
        # the page number/label); enumerate them under the dock.
        items = pages_dock.descendants(control_type="ListItem")
        assert len(items) >= 3, (
            f"Pages panel must show a thumbnail entry per page (>=3 for the "
            f"3-page fixture); found {len(items)} ListItems"
        )

        # Corroborate the document really is multi-page via the status bar,
        # which reliably exposes "Page 1 of N" as its accessible Name.
        page_label = s.find(auto_id="pageLabel")
        label_text = page_label.window_text() or ""
        assert "of 3" in label_text, \
            f"status bar should report a 3-page document; got {label_text!r}"

        # Layers must NOT be the active/visible left tab (it is hidden by
        # default and only shown on demand for documents that have layers).
        layers_dock = s.find_optional(auto_id="layerPanelDock", timeout=2.0)
        assert layers_dock is None or not layers_dock.is_visible(), \
            "Layers dock must not be the active tab by default (Pages is)"

    assert no_orphans(), "pdfe.exe orphan left running after the test"
