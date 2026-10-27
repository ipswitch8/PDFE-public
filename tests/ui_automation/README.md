# PDFE Native UI Automation

Test harness for the Qt desktop UI using native Windows automation tools.
**Not Selenium** — PDFE is a native C++/Qt app, not a web app.

## Tool stack

| Tool | Role |
|---|---|
| **pywinauto** (UIA backend) | Primary. All dialogs, menus, dock panels, toolbars, status bar. |
| **WinAppDriver** | Optional WebDriver-style harness for cross-language tests. |
| **FlaUI** | Fallback for custom Qt widgets pywinauto's UIA can't see cleanly. |
| **PyAutoGUI** | Canvas interior only — annotation handles, text-selection hit zones, in-canvas drag/drop. |

## Launching PDFE under pywinauto

```python
from pywinauto import Application

# Debug build
app = Application(backend="uia").start(
    r"C:\Users\dev\source\PDFE\build\vs2022\bin\Debug\pdfe.exe"
)
main = app.window(title_re="PDFE.*")
main.wait("visible", timeout=10)
```

Always close the app at teardown to avoid orphaned `pdfe.exe`:

```python
try:
    ...
finally:
    app.kill()
```

## Canonical `objectName` registry

The audit on 2026-04-12 brought all dialogs and panels to
automation-ready state. Pywinauto selects by `auto_id=<objectName>`.

### Main window
- `mainWindow` — the top-level `QMainWindow`
- `pdfCanvas` — the central rendering widget (QAccessible exposed via
  `PdfCanvasAccessible`, supports `Next Page` / `Previous Page` /
  `Zoom In` / `Zoom Out` invoke actions)
- Dock panels (all `QDockWidget`s named):
  `pagePanelDock`, `bookmarkPanelDock`, `commentPanelDock`,
  `searchPanelDock`, `signaturePanelDock`, `redactionPanelDock`,
  `propertiesPanelDock`, `defaultsLogPanelDock`,
  `formFieldPropertiesPanelDock`, `imagePropertiesPanelDock`,
  `annotationPropertiesDock`

### Menus & recent files
- `recentFilesMenu` — the File > Recent submenu
- `recentFile_0` .. `recentFile_N` — individual recent-file actions
  (added 2026-04-12; previously the dynamic actions had no IDs)
- `recentFilesNoneAction` — the disabled "(No Recent Files)" placeholder
- `recentFilesClearAction` — the "Clear Recent Files" action

### Dialogs
- `aboutDialog` — `appNameLabel`, `versionLabel`, `librariesLabel`,
  `licenseLabel`, `okButton`
- `passwordDialog` — `passwordPromptLabel`, `passwordEdit`, `buttonBox`,
  `okButton`
- `ComplianceDialog` — `LevelCombo`, `AnalyzeButton`, `ValidateButton`,
  `ConvertButton`, `IssueLabel`, `IssueTable`, `StatusLabel`
- `AtlasComparisonDialog` — `atlasOriginalPreview`, `atlasFontPreview`,
  `atlasFontCombo`, `atlasAtlasPreview`, `atlasFontCharsetScroll`,
  `atlasFontCharsetLabel`, `atlasUseFontButton`, `atlasFillMissingCheck`,
  `atlasAtlasCharsetScroll`, `atlasAtlasCharsetLabel`,
  `atlasUseScanButton`, `atlasCancelButton`
- `printPreviewDialog` — `previewToolBar`, `printPreviewWidget`,
  `zoomInAction`, `zoomOutAction`, `fitWidthAction`, `fitPageAction`
- `EncryptionDialog`, `MetadataDialog`, `PrintDialog`, `SignDialog` —
  all dialog controls already named (see source)

### Annotation properties (dock panel)
- `annotationPropertiesWidget` — container
- `annotTypeLabel`, `annotAuthorEdit`, `annotContentsLabel`,
  `annotContentsEdit`, `annotColorButton`, `annotOpacitySlider`,
  `annotOpacityLabel`
- FreeText subgroup: `annotFontGroup`, `annotFontCombo`,
  `annotFontSizeSpinner`, `annotTextColorButton`

## PyAutoGUI-only zones (no UIA subtree)

These regions inside the `pdfCanvas` rectangle require pixel/coordinate
automation. Obtain the canvas rect via pywinauto first, then do math:

```python
canvas = main.child_window(auto_id="pdfCanvas", control_type="Pane")
rect = canvas.rectangle()  # left, top, right, bottom in screen coords
```

- **Annotation selection handles** — 8 handles around the selected
  annotation bbox. Offsets come from `AnnotationManager::getAnnotationBounds`;
  convert page coords to canvas pixels via `PdfCanvas::pageToScreen`.
- **Text-selection drag** — click-drag to select text on the rendered
  page. No UIA elements exist for character boxes.
- **In-canvas sticky note popup** — the popup itself has `objectName`
  `stickyNotePopup` and *is* reachable via pywinauto once it appears;
  only the click that opens it needs coordinate math.
- **QColorDialog swatch picking** — `QColorDialog` is a native Windows
  control and pywinauto handles it. Only the *result* color isn't
  exposed as a property of `annotColorButton`; parse the stylesheet
  or use `accessibleDescription`.

## Running the tests

Two scripts, two layers:

```powershell
cd C:\Users\dev\source\PDFE
python -m pip install pywinauto pyautogui

# Static source audit — no process launch, fast, safe
python tests\ui_automation\audit_source.py

# Live UIA smoke test — launches pdfe.exe, walks main window
python tests\ui_automation\smoke_test.py
```

### audit_source.py
Greps `src/ui/` for `setObjectName("...")` calls and confirms every
widget this test layer depends on is named. Does not launch PDFE.
Fast, CI-friendly, and the authoritative check for dialogs (which
are modal and steal focus, so they're not exercised live).

### smoke_test.py
Launches `pdfe.exe` and verifies main-window widgets are reachable
via pywinauto's UIA backend. Covers: `pdfCanvas`, `mainToolBar`,
`annotationToolBar`, and the dock widgets visible at startup.
Hidden dock widgets (bookmark, redaction, signatures, defaults log)
must be shown via the View menu before they appear in the UIA tree.

### Important: Qt UIA gotchas

- **Hierarchical auto_ids.** Qt's UIA bridge prepends the parent path,
  so `objectName("pdfCanvas")` becomes `QApplication.mainWindow.pdfCanvas`.
  The smoke test matches by suffix (`find_by_suffix`) — do the same
  in any new tests.
- **QAction auto_ids are shared.** All menu items share
  `QMenuBar.QAction`, so you cannot select menu items by `auto_id`.
  Navigate by `title` (`child_window(title="Help", control_type="MenuItem")`)
  or use `win.menu_select("Help -> About PDFE")`.
- **`wait_for_idle=False`.** Qt's startup doesn't set the idle flag
  in a way `WaitForInputIdle` likes. Always pass `wait_for_idle=False`
  to `Application.start()`.
- **Modal dialogs and `menu_select` can hang.** Verifying dialog
  controls live requires opening each dialog (which is modal and
  noisy). Prefer `audit_source.py` for dialog coverage.

Neither script asserts functional behaviour — that's the GoogleTest
suite. This layer only verifies automation-friendliness.

## Render-oracle display scaling / zoom robustness

The pixel oracles in `harness/render_oracle.py` (`assert_slant_matches`,
`assert_overlay_font_class`, `assert_overlay_alignment`) read the rendered
canvas, so they are sensitive to the rig's display + app zoom. The CI/dev rig
renders the canvas at a combined scale of roughly **1.62** (the "~162%" figure
quoted throughout the b91f4e83 calibration), recorded as
`render_oracle.RIG_DISPLAY_SCALE` / `render_drive.RIG_REFERENCE_ZOOM`.

Each oracle takes an optional `scale_factor` (default `1.0`), but only one of
the three actually uses it:

| Oracle | Scale behaviour |
|---|---|
| `assert_overlay_alignment` | **Scale-dependent.** Tolerance is in screen pixels; a fixed PDF-point registration error grows linearly with zoom, so `max_offset_px` is multiplied by `scale_factor`. |
| `assert_slant_matches` | **Scale-invariant.** Keys on a shear *angle* (degrees); `scale_factor` is an intentional no-op. |
| `assert_overlay_font_class` | **Scale-invariant.** Size-normalises live + reference shapes before correlating; `scale_factor` is an intentional no-op. |

Get the live `scale_factor` with `render_drive.get_app_zoom_factor(win)`, which
reads the zoom-combo pipe seam and normalises against the reference scale
(`1.0` at the reference zoom). The two scale-invariant oracles deliberately
ignore the value — faking a scale-dependent angle / normalised-correlation
threshold would make them *incorrect*, not more robust. Cross-zoom robustness is
proved end-to-end by `test_render_oracle_zoom.py`, which runs all three oracles
at two distinct `zoomInAction` levels (both above the ~50px-glyph
correlation-reliability floor — fit-zoom is deliberately not used for the
template-correlation oracle, where its margin collapses) and requires PASS at
both.

Live-UI pytest in this directory must be run via the **Bash** tool, not the
PowerShell tool (the PowerShell tool runs in a desktop-less RDP session where
UIA cannot see the main window).
