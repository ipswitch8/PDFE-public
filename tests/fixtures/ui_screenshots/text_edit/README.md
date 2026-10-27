# text_edit screenshots — Phase 3

This directory holds canvas screenshots for the text-edit subsystem cycle
of the project-wide UI verification pipeline.

## Phase 3 status

Phase 3 of the pipeline focused on the C++ regression locks for the three
reported user-bugs (UI-TE-SEL-01 selection rendering, UI-TE-SAVE-01
overlay save round-trip, UI-TE-UNDO-01 undo state corruption) plus the
surrounding P0 text-edit rows. The pixel-level UI-screenshot rows
(UI-TE-UI-01..05) are deferred to Phase D1 (final integration), where
the visual-regression baseline is built end-to-end against a live
PDFE.exe instance under the harness's deterministic-launch contract
(see ../../../ui_harness/DETERMINISM.md).

## Why deferred

The harness can launch PDFE deterministically (1600x1000, 100% scaling,
DPI-aware) and capture canvas pixels via PyAutoGUI. What requires
Phase D1 is:

1. The golden-image regen workflow (tests/ui_harness/core/golden.py)
   needs an authoritative baseline run, and that baseline only makes
   sense once every subsystem has its own UI tests, so all baselines
   are captured in one consistent session (font fallback, theme,
   render seed all stable).
2. Visual-fidelity rows (alignJustify ↕ glyph and the broader
   `← ↔ → ↕` alignment family) have side effects on the toolbar
   layout, so screenshot baselines for text-edit overlays should
   be captured AFTER the visual-fidelity fix to avoid double-baselining.

## Manifest

Phase 3 contributed:
- `phase3_baseline.txt` — placeholder noting the deferred status
- `README.md` (this file)

Phase D1 will populate per-test PNG subdirectories matching the
`tests/test_ui_text_edit_*.py` file names.
