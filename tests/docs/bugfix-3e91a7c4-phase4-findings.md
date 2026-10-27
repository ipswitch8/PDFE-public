# bugfix-3e91a7c4 Phase 4 — UI cluster #1/#2/#3 (findings + RED→GREEN evidence)

<!-- tracking-banner -->
> **STATUS: HISTORICAL EVIDENCE — this is not a work list.**
>
> **GitHub Issues (`ipswitch8/PDFE`) is the canonical tracker for anything still
> open.** This document records what was found or decided on the day it was
> written. Items here may have been fixed, superseded, or deliberately declined
> since. Where this document and a GitHub issue disagree, **the issue wins**.
>
> See [`docs/TRACKING.md`](../../docs/TRACKING.md).

---


Phase 4 fixes the three UI defects from the six-bug report and, per the pipeline
mandate, does so **test-first with the RED observed in commit history**.

## The three defects and their fixes

| # | Defect | Root cause | Fix |
|---|--------|-----------|-----|
| 1 | Toolbars look like "a series of words" | Buttons text-only (no icons) + the Form-fields toolbar had no separators grouping its 7 field types | `ToolButtonTextBesideIcon` on the main/annotation/form toolbars + 2 separators grouping the form fields (text / choice / button+signature) |
| 2 | Missing icons | 0 of 124 `QAction`s had `setIcon`; **no icon assets existed**; and even once added, the `.qrc` sits in the `pdfe_ui` STATIC lib so its RCC init symbol was dropped by the linker → every `QIcon(":/icons/actions/*.svg")` resolved **null** | Created a 25-glyph monochrome SVG set (`src/ui/icons/action_*.svg` via `make_action_icons.py`) embedded through `action_icons.qrc`; `setIcon()` on every main/annotation toolbar action; pinned the resource with a global-scope `::qInitResources_action_icons()` call in `createToolBar()` (same pattern `AboutDialog.cpp` uses for `branding.qrc`) |
| 3 | Left dock shows only Layers | `restoreState()` in the ctor reapplies a stale saved windowState (Layers raised / Pages hidden); a persisted `Panels/pagePanel=false` compounds it | `MainWindow::ensurePagesDockDefault()` — force Pages visible + raised, hide Layers/Bookmarks (re-shown on demand by `openFile`) — called **after** `restoreState()` so the restore cannot override it |

## Why the prior suite missed all three — the meta-cause (feeds the P5 plan)

These are **realization** defects, and the suite only had **reachability**
oracles: it asserted that controls *exist* in the UIA tree, never that a toolbar
action carries a non-null icon, that toolbars have separators, or *which* dock
tab is the default. The three new tests are realization oracles:

- `MainAndAnnotationToolbarActionsHaveIcons` — non-null `QIcon` per action.
- `FormToolbarGroupsFieldsWithSeparators` — the form toolbar groups with `>=1` separator.
- `PagesForcedAsDefaultLeftDockTabAfterStaleRestore` — Pages forced back as the
  default left tab even after a simulated stale restore.

Plus the pywinauto E2E `test_ui_pages_default_dock.py`, which asserts against the
live `pdfe.exe` that Pages (not Layers) is the active left tab, populated one
`ListItem` per page, corroborated by the status bar "of N".

## Test-first RED → GREEN (observed in commit history)

- `2ba5e0a` — tests + minimal scaffolding (accessors + an **empty**
  `ensurePagesDockDefault()` stub) committed against otherwise-unmodified
  product code. Built and run: **all three FAIL for the correct reason**:
  - `MainAndAnnotationToolbarActionsHaveIcons` — `a->icon().isNull()` is `true`
    for every action (icons never set / resource unregistered).
  - `FormToolbarGroupsFieldsWithSeparators` — `separators` is `0`, expected `>=1`.
  - `PagesForcedAsDefaultLeftDockTabAfterStaleRestore` — `pages->isHidden()` is
    `true` (stays hidden) and `layers->isHidden()` is `false` (Layers stays up).
- `c465c85` — the fix. Rebuilt and run: **all three PASS**.

(Raw captured runs: `p4_RED.txt` / `p4_GREEN.txt` from the phase-4 session.)

## Live validation

Launched the built `pdfe.exe` on the real 51-page *SpecimenA Green* document: the
toolbar shows icons beside labels with grouped buttons, and the left dock shows
the **Pages** panel (page thumbnails) as the active tab — not Layers. The
pywinauto test passes against the same build.
