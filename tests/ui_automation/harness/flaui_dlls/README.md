# FlaUI DLL staging directory

Pipeline L phase-2 — `tests/ui_automation/harness/flaui_bridge.py` searches
this directory for `FlaUI.Core.dll`, `FlaUI.UIA3.dll`, `Interop.UIAutomationClient.dll`,
and `System.Drawing.Common.dll`.

The DLLs are NuGet-distributed (~31 MB) and are NOT committed to git.

## Bootstrap

```powershell
pwsh tests/ui_automation/harness/flaui_dlls/install_flaui.ps1
```

That script downloads `nuget.exe` (if absent), installs FlaUI.Core +
FlaUI.UIA3 + System.Drawing.Common via NuGet, and stages the
.NET 8 versions of the DLLs to this directory's top level.

## Verify

```powershell
python -c "from tests.ui_automation.harness import flaui_bridge; print(flaui_bridge.is_available())"
```

Expected output: `True`. If `False`, call `flaui_bridge.load_error()`
to see why.

## Notes

- pythonnet 3.x defaults to .NET Framework on Windows; FlaUI 5 requires
  .NET 8. The bridge calls `pythonnet.load("coreclr")` before importing
  `clr` to switch to the .NET runtime.
- An alternate location can be supplied via `PDFE_FLAUI_DLL_DIR`.
