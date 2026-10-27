# -*- coding: utf-8 -*-
"""FlaUI bridge — optional .NET UIAutomation wrapper for paths where
pywinauto's pywin32+comtypes stack is unreliable on this host.

Pipeline L phase-2 (Option 3): pywinauto's UIA backend goes through
pywin32 and comtypes, which accumulate COM proxy state across tests
in the same Python process and trigger HRESULT 0x80040155
(REGDB_E_CLASSNOTREG) after 2-3 cycles on the Server-2019 RDP host.
FlaUI is a .NET wrapper around the system UIAutomation API; it does
not share the same proxy table and is independently reliable for
the dialog-walk paths that pywinauto fails on.

Bridge architecture
===================

This module uses pythonnet (`pip install pythonnet`) to load the
FlaUI.Core .NET assembly into the Python process. FlaUI is NOT on
PyPI — its DLLs must be on the .NET assembly load path. The bridge
searches:

  1. Anywhere clr's default resolver finds it (GAC, current dir).
  2. The directory pointed to by env var ``PDFE_FLAUI_DLL_DIR``.
  3. ``tests/ui_automation/harness/flaui_dlls/`` (project-local).

If FlaUI.Core can't be loaded, ``is_available()`` returns False and
all other API functions raise RuntimeError or return None. Callers
MUST gate all bridge use with ``if flaui_bridge.is_available():``
or ``pytest.skip("FlaUI not available")``.

Public API
==========

``is_available() -> bool``
    Returns True if FlaUI.Core was loaded successfully.

``find_window_by_class(class_name) -> Element | None``
    Find a top-level window whose ClassName matches.

``find_descendant_by_automation_id(root, automation_id) -> Element | None``
    Walk the UIA tree under ``root`` and return the first descendant
    whose AutomationId matches.

``click(element) -> bool``
    Invoke (UIA Invoke pattern) or click on the element.

``get_value(element) -> str | None``
    Read the element's Value property (UIA ValuePattern), falling
    back to Name then nothing.

``set_value(element, value: str) -> bool``
    Set the element's Value property. Returns False on failure.

Setup
=====

To install FlaUI on this host so the bridge becomes available:

  1. Install nuget.exe: choco install nuget.commandline (or download
     from https://www.nuget.org/downloads).
  2. nuget install FlaUI.Core -OutputDirectory C:\\flaui-dlls
  3. nuget install FlaUI.UIA3 -OutputDirectory C:\\flaui-dlls
  4. set PDFE_FLAUI_DLL_DIR=C:\\flaui-dlls\\FlaUI.Core.<version>\\lib\\netstandard2.0
  5. Verify: python -c "from tests.ui_automation.harness.flaui_bridge import is_available; print(is_available())"

Or for a project-local install:

  mkdir tests/ui_automation/harness/flaui_dlls
  nuget install FlaUI.Core -OutputDirectory tests/ui_automation/harness/flaui_dlls
"""
from __future__ import annotations

import os
from pathlib import Path
from typing import Optional

_FLAUI_LOADED: Optional[bool] = None
_FLAUI_LOAD_ERROR: Optional[str] = None
_AUTOMATION = None  # FlaUI.UIA3.UIA3Automation singleton, when loaded


def _candidate_dll_dirs() -> list:
    """Return ordered list of directories to search for FlaUI DLLs."""
    dirs = []
    env_dir = os.environ.get("PDFE_FLAUI_DLL_DIR")
    if env_dir:
        dirs.append(Path(env_dir))
    # Project-local
    here = Path(__file__).resolve().parent
    dirs.append(here / "flaui_dlls")
    return [d for d in dirs if d.is_dir()]


def _try_load_flaui() -> bool:
    """Attempt to load FlaUI.Core + FlaUI.UIA3. Idempotent.

    pythonnet defaults to .NET Framework on Windows, which can't
    load FlaUI's net8.0-windows7.0 DLLs. We explicitly switch to
    CoreCLR before importing clr — this MUST happen before any
    other module imports clr, so the bridge does it as the first
    step.
    """
    global _FLAUI_LOADED, _FLAUI_LOAD_ERROR, _AUTOMATION
    if _FLAUI_LOADED is not None:
        return _FLAUI_LOADED

    # Step 1: switch pythonnet to CoreCLR. Failure to import
    # pythonnet means it's not installed (graceful degrade path).
    try:
        from pythonnet import load as _pythonnet_load  # type: ignore
    except ImportError as e:
        _FLAUI_LOAD_ERROR = f"pythonnet not installed: {e}"
        _FLAUI_LOADED = False
        return False
    try:
        _pythonnet_load("coreclr")
    except Exception:  # noqa: BLE001
        # Already loaded — pythonnet raises if you switch runtimes
        # after clr is in use. We don't fatal here; the AddReference
        # below will surface a clearer error if there's an actual
        # incompatibility.
        pass

    try:
        import clr  # type: ignore  # noqa: F401
    except ImportError as e:
        _FLAUI_LOAD_ERROR = f"clr (pythonnet) import failed: {e}"
        _FLAUI_LOADED = False
        return False

    import sys
    # Add candidate dirs to sys.path so clr's loader can find them.
    for d in _candidate_dll_dirs():
        if str(d) not in sys.path:
            sys.path.insert(0, str(d))

    try:
        import clr as _clr  # noqa: F811
        _clr.AddReference("FlaUI.Core")
        _clr.AddReference("FlaUI.UIA3")
        from FlaUI.UIA3 import UIA3Automation  # type: ignore
        _AUTOMATION = UIA3Automation()
        _FLAUI_LOADED = True
        return True
    except Exception as e:  # noqa: BLE001
        # The FileNotFoundException from the CLR shows up here as a
        # generic Exception. Capture its repr for diagnostics.
        _FLAUI_LOAD_ERROR = (
            f"FlaUI.Core / FlaUI.UIA3 not found on .NET load path: "
            f"{type(e).__name__}: {e}. Install via nuget and set "
            f"PDFE_FLAUI_DLL_DIR or place DLLs in flaui_dlls/. "
            f"Note: pythonnet 3.x requires CoreCLR mode for .NET 8 "
            f"assemblies; the bridge calls pythonnet.load('coreclr') "
            f"before importing clr."
        )
        _FLAUI_LOADED = False
        return False


def is_available() -> bool:
    """Return True if FlaUI.Core was loaded; False otherwise.

    On first call, attempts to load the assembly. Subsequent calls
    return the cached result. Never raises.
    """
    return _try_load_flaui()


def load_error() -> Optional[str]:
    """Return the last load error message (None if FlaUI loaded OK
    or hasn't been tried yet)."""
    return _FLAUI_LOAD_ERROR


def find_window_by_class(class_name: str):
    """Find the first top-level window whose ClassName matches.

    Returns the FlaUI Element wrapper or None.
    """
    if not is_available():
        return None
    try:
        # FlaUI 5 exposes ConditionFactory as a property on the
        # Automation singleton; constructing via the type's
        # ctor + PropertyLibrary works in older FlaUI versions but
        # not in 5.x — use the property accessor.
        cf = _AUTOMATION.ConditionFactory
        desktop = _AUTOMATION.GetDesktop()
        condition = cf.ByClassName(class_name)
        return desktop.FindFirstChild(condition)
    except Exception:  # noqa: BLE001
        return None


def find_descendant_by_automation_id(root, automation_id: str):
    """Walk the UIA tree under root and return the first descendant
    whose AutomationId matches.
    """
    if root is None or not is_available():
        return None
    try:
        # FlaUI 5 exposes ConditionFactory as a property on the
        # Automation singleton; constructing via the type's
        # ctor + PropertyLibrary works in older FlaUI versions but
        # not in 5.x — use the property accessor.
        cf = _AUTOMATION.ConditionFactory
        condition = cf.ByAutomationId(automation_id)
        return root.FindFirstDescendant(condition)
    except Exception:  # noqa: BLE001
        return None


def click(element) -> bool:
    """Invoke or click an element. Returns True on success."""
    if element is None or not is_available():
        return False
    try:
        # Prefer Invoke pattern (button/menu); fall back to click.
        try:
            element.Patterns.Invoke.Pattern.Invoke()
            return True
        except Exception:  # noqa: BLE001
            element.Click()
            return True
    except Exception:  # noqa: BLE001
        return False


def get_value(element) -> Optional[str]:
    """Return Element.Value (UIA ValuePattern), falling back to Name."""
    if element is None or not is_available():
        return None
    try:
        try:
            return str(element.Patterns.Value.Pattern.Value.Value)
        except Exception:  # noqa: BLE001
            return str(element.Name) if element.Name else None
    except Exception:  # noqa: BLE001
        return None


def set_value(element, value: str) -> bool:
    """Set Element.Value via UIA ValuePattern. Returns True on success."""
    if element is None or not is_available():
        return False
    try:
        element.Patterns.Value.Pattern.SetValue(value)
        return True
    except Exception:  # noqa: BLE001
        return False
