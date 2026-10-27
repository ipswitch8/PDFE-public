# PDFE UI Harness — Determinism Contract

This document specifies the exact environmental conditions required for the
pywinauto + PyAutoGUI UI tests to produce reproducible, non-flaky results.
Every bullet is a hard requirement unless marked "recommended".

---

## 1. DPI — 96 logical pixels per inch (100% scaling)

**Requirement:** The Windows display scaling for the monitor running the tests
must be set to **100%** (96 DPI).

**Enforcement in code:**
- `tests/ui_harness/core/launch.py` calls `SetProcessDpiAwareness(1)` on the
  Python test process before any screen coordinate work is performed.
- PDFE.exe is launched with `QT_SCALE_FACTOR=1`, `QT_AUTO_SCREEN_SCALE_FACTOR=0`,
  and `QT_ENABLE_HIGHDPI_SCALING=0` so Qt does not attempt its own scaling.
- These settings together ensure that logical pixels == physical pixels on a
  96-DPI display.

**If the display is at 125% or higher:** All PyAutoGUI coordinate-based tests
will fail because screen coordinates will be scaled by the DPI factor.  Set the
display to 100% and restart the test session.

---

## 2. Windows Scaling — System-wide 100%

**Requirement:** Go to Settings → Display → Scale and layout and set to 100%.
No per-application DPI override should be set for `pdfe.exe` in its executable
properties.

---

## 3. PDFE Window Size — 1600 × 1000 pixels

**Requirement:** The PDFE main window is programmatically moved and resized to
exactly 1600 × 1000 logical pixels at screen position (0, 0) by
`launch.py::_resize_window()`.

**Why:** PyAutoGUI canvas coordinates are computed from the live bounding rect
of the pdfCanvas widget.  If the window is a different size, the relative
offsets change and screenshot comparisons fail.

**Multi-monitor setups:** Ensure the primary monitor has at least 1600 × 1000
resolution.  The window is placed at (0, 0) on the primary monitor.

---

## 4. Zoom Level — Fit-Page or 100%

**Requirement:** After opening a PDF the test should pin the zoom to a known
level before taking baseline screenshots.  The recommended value is "Fit Page"
(invoked via `fitPageAction`) or an explicit 100% zoom.

**Enforcement:** The helpers in each subsystem stub are expected to call
`locate.by_auto_id(session.window, "fitPageAction")` and invoke it (or set the
zoom combo to "100%") before any screenshot is taken.

---

## 5. Theme — Light

**Requirement:** The OS must be in **Light mode** (Windows Settings → Personalisation
→ Colors → Choose your colour = Light).

**Why:** PDFE has a theme-aware icon set.  In dark mode the toolbar icons use
their dark-background variants.  Screenshot diff thresholds are calibrated for
light mode only.

---

## 6. Font Fallback

**Requirement:** The host machine must have either Arial or Segoe UI installed
(both are present by default on Windows 10/11 and Server 2019).  PDFE uses
Qt's default system font for toolbar labels; if neither is present, label
geometry differs and diff thresholds may trigger false failures.

---

## 7. Platform — Windows Server 2019 / Windows 10 / Windows 11

**Requirement:** The test harness has been validated on:
- Windows Server 2019 Standard (the primary CI target)
- Windows 10 22H2
- Windows 11 23H2

It has **not** been tested on Linux or macOS (PDFE is a Windows-only application).
The harness uses `pywinauto` (UIA backend) and `PyAutoGUI`, both of which are
Windows-specific for this use case.

---

## 8. Required Python Packages

```
pywinauto >= 0.6.8
pyautogui >= 0.9.54
Pillow >= 9.0
numpy >= 1.23
psutil >= 5.9
imagehash >= 4.3   # optional; used for pHash diff metric
pytest >= 7.0
```

Install with:

```bash
pip install pywinauto pyautogui Pillow numpy psutil imagehash pytest
```

---

## 9. CI Considerations

- Run CI on a **real desktop session** or a **Windows Server with RDP / Interactive
  Desktop enabled**.  Session-0 (non-interactive service context) does not have
  a display and PyAutoGUI will fail to create windows.
- Use `pdfe_tests.exe` for pure unit/integration tests in headless CI.
- Reserve the pywinauto / PyAutoGUI tests for a windowed CI node.

---

## Violation Consequences

| Violation | Likely symptom |
|---|---|
| Display scaling ≠ 100% | Wrong screen coords, all canvas tests fail |
| Window size differs | Screenshot diff failures on every test |
| Dark mode active | Icon colour diffs exceed max_delta threshold |
| Missing font | Label width changes, layout-dependent diff failures |
| Non-interactive session | `pyautogui.FailSafeException` or blank screenshots |
