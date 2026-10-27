# -*- coding: utf-8 -*-
"""PDFE UI automation harness.

Shared infrastructure for the pywinauto/FlaUI regression suite:

- :class:`pdfe_app_fixture.PdfeAppFixture` — base class that launches
  ``pdfe.exe`` with ``wait_for_idle=False``, exposes ``self.app`` and
  ``self.main_window``, and on teardown both calls ``app.kill()`` and
  runs ``taskkill /F /IM pdfe.exe`` as a fallback to avoid orphaned
  processes that wedge subsequent tests.

- :mod:`menu_nav` — navigate Qt menus by title. Works around the
  shared-``auto_id`` problem (every ``QAction`` has auto_id
  ``QMenuBar.QAction``).

- :mod:`dialog_helpers` — dialog lookup and child-widget search by
  suffix match on Qt's hierarchical auto_ids.

- :mod:`canvas_coords` — PdfCanvas pixel ↔ PDF-point coordinate
  mapping for PyAutoGUI-driven tests.

- :mod:`clipboard` — RDP-safe clipboard set/get with retry loops,
  mirroring the production fix from commit 8371c81.

- :mod:`retry` — ``@retry`` decorator that re-runs a test function on
  known-flaky exceptions (``WaitForInputIdle``, element not available,
  clipboard empty).

See ``tests/ui_automation/matrix/ui_test_matrix.csv`` for the
complete test-case list these helpers serve.
"""
from __future__ import annotations

__all__ = [
    "pdfe_app_fixture",
    "menu_nav",
    "dialog_helpers",
    "canvas_coords",
    "clipboard",
    "retry",
]
