# forms screenshots — Phase 5

Canvas screenshots for the forms subsystem cycle.

The Phase 5 P0 contracts are locked in `tests/test_forms_phase5.cpp`
(7/9 PASS, 2 SKIP with documented fixture-deficiency rationale). UI
integration scenarios — JS Validate event flow, signature widget
click → SignDialog opening, XFDF dialog UI — are covered by the
Python smoke at `tests/test_ui_forms_smoke.py` for activation,
deferred to Phase D1 for pixel-level UI assertions.

## Files

- `form_tool_baseline.png` — captured by `test_ui_forms_form_tool_baseline`
  when run live. Pre-fix snapshot for any future visual-regression diff.
- `README.md` — this file.
