# annotations screenshots — Phase 4

Canvas screenshots for the annotations subsystem cycle.

## Phase 4 status

The annotation P0 contracts (UI-AN-01, 03, 06, 10, 12, 13, 14, 18, 33)
are locked by C++ regression tests in `tests/test_annotations_phase4.cpp`.
All passed against current code, so no production fix was needed in
this cycle.

UI-AN-02 (StickyNote popup-on-click pixel assertion) and UI-AN-22
(annotation toolbar icons) are smoke-covered by
`tests/test_ui_annotations_smoke.py`. The full pixel-level visual
fidelity work — including the `setIcon` fix that gives the toolbar
proper icons — is consolidated to Phase 9 (visual-fidelity / panels)
and Phase 10 (final integration baseline) per the audit row mapping.

## Files captured

- `toolbar_baseline.png` — pre-fix snapshot of the annotation toolbar
  (text-only labels). Phase 9 will diff against this after `setIcon`
  is wired.
- `README.md` — this file.

Phase D1 will populate per-test PNG files matching the
`tests/test_ui_annotations_*.py` test names.
