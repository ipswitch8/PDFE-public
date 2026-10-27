# Next Pipeline Plan — Post a7f3d2c9 Follow-up

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


**Status**: STAGED — do NOT write to `.claude/pipeline.json` until a7f3d2c9 phase-4 commits and closes.

**Reason for staging**: Gate discipline requires a7f3d2c9 phase-4 to complete its commit cycle (including
the deferred-AC6 note) before a new pipeline.json overwrites the active one. The validate-pipeline Stop
hook auto-initializes pipeline state when it detects a new pipeline.json — no manual initialization needed.

**Trigger**: After the user runs `/g` to commit a7f3d2c9 phase-4, copy the JSON block below verbatim
to `.claude/pipeline.json`.

---

## Pipeline JSON (ready to copy)

```json
{
  "pipeline_id": "e5b2c947-3f8a-4d61-b091-7a4e0c6f3d25",
  "task": "Address 264 pre-existing test failures + 3 test-infra bugs surfaced by pipeline a7f3d2c9 phase-4 sweep (GAP-28 through GAP-46, documented in tests/docs/post_a7f3d2c9_followups.md). GAP-28 (window width 1158 < required 1400px) is the cascade root cause for ~150 failures and must be resolved first. After phase-1 re-sweep, real product bugs become distinguishable from cascade artifacts and are fixed in subsequent phases. GoogleTest baseline must stay >= 3583 PASSED / 0 FAILED / 2 SKIPPED throughout.",
  "generated_at": "2026-05-18T00:00:00Z",
  "total_phases": 6,
  "phases": [
    {
      "id": "e5b2c947-3f8a-4d61-b091-7a4e0c6f3d25-phase-1",
      "name": "GAP-28 + GAP-43: Window width root cause + pytest infra workaround",
      "description": "Diagnose and fix why the test harness window lands at 1158px instead of the required 1400px on this Windows Server 2019 rig (DPI scaling, SetWindowPos failure, or Qt geometry clamping), apply a permanent pytest.ini fix for the pytest 8.4.2 capture teardown bug (GAP-43), then re-run the full pytest sweep to separate the ~150 GAP-28 cascade failures from the real product bugs that remain.",
      "acceptance_criteria": [
        "Root cause of window width=1158px is identified and documented: one of (a) display DPI != 100% detected and compensated in the test harness, (b) SetWindowPos failure intercepted and retried, or (c) Qt setGeometry clamping worked around by targeting a larger initial geometry.",
        "tests/ui_harness_smoke.py (or the relevant fixture/conftest): window-resize logic is updated so that a post-resize width assertion >=1400px passes on this rig; if DPI != 100% is the root cause, the harness either corrects for it or marks width-dependent tests as skip (with explicit reason) rather than failing.",
        "pytest.ini addopts field is permanently updated to include `-p no:capture` so the pytest 8.4.2 + Windows capture teardown bug (GAP-43) is suppressed without requiring a manual CLI flag on every run.",
        "Full pytest sweep re-run (pytest tests/ with no extra manual flags) completes without the ValueError: I/O operation on closed file teardown errors and with total FAILED count <= 132 (at least 50% reduction from the pre-fix baseline of 264 FAILED).",
        "Post-re-sweep failure list is captured to tests/docs/post_phase1_sweep_results.md documenting exactly which failures persisted vs auto-resolved, so phases 4 and 5 have a verified scope.",
        "pdfe_tests.exe (GoogleTest suite) passes with count >= 3583 PASSED / 0 FAILED / 2 SKIPPED (no C++ changes expected in this phase)."
      ],
      "gate_agents": ["karen", "validator", "test-runner"]
    },
    {
      "id": "e5b2c947-3f8a-4d61-b091-7a4e0c6f3d25-phase-2",
      "name": "GAP-44 + GAP-45: Test harness live-results segregation",
      "description": "Move all live test-run outputs (ui_test_matrix.csv status fields and regenerated baseline PNGs) out of source-controlled paths into a gitignored tests/_runtime/ directory, ensuring the source-controlled files remain stable documentation while runtime artifacts do not pollute git status after each sweep.",
      "acceptance_criteria": [
        "tests/_runtime/ directory exists and is listed in .gitignore (or the project root .gitignore) so that no file under tests/_runtime/ is ever tracked by git.",
        "tests/ui_automation/matrix/ui_test_matrix.csv: the test harness no longer writes live pass/fail status back to this file during a run; it is source-controlled documentation only. The harness instead writes a timestamped copy of live results to tests/_runtime/ui_test_matrix_<timestamp>.csv.",
        "Baseline PNG write paths in the test harness: any test that previously wrote updated baselines to the source-controlled tests/fixtures/ path during a run is updated to write actuals to tests/_runtime/baselines_actual/<test_name>.png instead; the source-controlled baseline at tests/fixtures/ is read-only at assertion time.",
        "A full pytest sweep run produces 0 modified files in tests/ui_automation/matrix/ and 0 modified files in tests/fixtures/ as confirmed by git diff --name-only after the run.",
        "tests/docs/test_strategy.md: a short note is added explaining the tests/_runtime/ convention — what goes there, why it is gitignored, and how to interpret runtime artifacts vs source-controlled baselines.",
        "pdfe_tests.exe count remains >= 3583 PASSED / 0 FAILED / 2 SKIPPED (no C++ changes in this phase)."
      ],
      "gate_agents": ["karen", "validator", "test-runner"]
    },
    {
      "id": "e5b2c947-3f8a-4d61-b091-7a4e0c6f3d25-phase-3",
      "name": "GAP-29: Save/Export dialog UIA enumeration fix",
      "description": "Fix the DialogDriveError: Dialog children (no children) failures on Save/Export/Extract dialogs by adding explicit settle/poll waits and a Win32 EnumChildWindows fallback in tests/helpers/file_dialog.py, then re-verify the ~30 save-flow tests that failed under this symptom.",
      "acceptance_criteria": [
        "tests/helpers/file_dialog.py: before attempting UIA child enumeration on a dialog, the helper polls for children with a configurable timeout (default >= 3 seconds, 100ms intervals) rather than enumerating once immediately after the dialog appears.",
        "tests/helpers/file_dialog.py: if UIA enumeration returns 0 children after the polling timeout, the helper falls back to Win32 EnumChildWindows (via ctypes or pywin32) to locate the filename Edit control by class name or control ID; this fallback is documented with a comment explaining the Windows Server 2019 / Common Item Dialog UIA tree difference.",
        "The 3 previously-failing tests (test_save_as_overwrite_existing_file, test_export_plain_text_opens_save_dialog, test_extract_current_page_to_new_file) pass in an isolated pytest run targeting those 3 tests.",
        "tests/ui_automation/ save-flow tests that were failing under the no-children symptom: at least 20 of the ~30 previously-failing tests in this cluster now pass (the remainder, if any, are documented in tests/docs/post_phase1_sweep_results.md as separate root causes).",
        "No regression in previously-passing dialog tests: a focused run of all tests matching *dialog* or *save* passes or xfails with documented reasons.",
        "pdfe_tests.exe count remains >= 3583 PASSED / 0 FAILED / 2 SKIPPED."
      ],
      "gate_agents": ["karen", "validator", "test-runner"]
    },
    {
      "id": "e5b2c947-3f8a-4d61-b091-7a4e0c6f3d25-phase-4",
      "name": "Real product bugs: GAP-30 + GAP-31 + GAP-32 + scope from phase-1 re-sweep",
      "description": "Fix the real product-layer bugs that remain after cascade resolution: comment panel not refreshing after annotation creation (GAP-30), text editing canvas not updating after type/backspace if not resolved by the GAP-28 fix (GAP-31), and redaction apply-state inconsistencies (GAP-32); exact scope confirmed by the phase-1 re-sweep results in tests/docs/post_phase1_sweep_results.md.",
      "acceptance_criteria": [
        "GAP-30 (comment panel refresh): AnnotationManager emits the appropriate signal (or calls CommentPanel::refresh() equivalent) after each annotation creation and deletion; tests test_sticky_note_appears_in_comment_panel, test_text_markup_appears_in_comment_panel, and test_comment_panel_lists_all_annotations_in_doc all pass.",
        "GAP-31 (text edit canvas not updating): if not already resolved by the GAP-28 window-width fix, the root cause is identified; tests test_type_replacement_text_changes_canvas and test_delete_text_via_backspace pass (SSIM before/after differs by at least a threshold consistent with a character being added or removed).",
        "GAP-32 (redaction inconsistencies): test_apply_partial_page_leaves_unmarked_intact passes with SSIM > 0.85 on the unmarked region, test_unmarked_text_still_present_after_save_reopen confirms non-redacted text survives a save/reopen cycle, and test_apply_redactions_across_multiple_pages confirms page 2 is unmodified when only page 1 has redaction marks.",
        "GAP-33 (zoom controls): test_actual_size_returns_to_100pct, test_zoom_combo_uia_exposed, and test_previous_page_returns are triaged; cascade-only failures are confirmed fixed by GAP-28; any real bugs are fixed or given a new tracking GAP number.",
        "pdfe_tests.exe count remains >= 3583 PASSED / 0 FAILED / 2 SKIPPED (if C++ signal-wiring is needed for GAP-30, new tests are added and count increases accordingly).",
        "cmake --build --preset debug-vs2022 exits with 0 errors and 0 new warnings after any C++ changes."
      ],
      "gate_agents": ["karen", "validator", "test-runner"]
    },
    {
      "id": "e5b2c947-3f8a-4d61-b091-7a4e0c6f3d25-phase-5",
      "name": "GAP-34..GAP-42: ui_automation cluster triage and cleanup",
      "description": "After cascade failures are resolved by phases 1-4, triage the remaining tests/ui_automation/ failures (GAP-34 toolbar reachability, GAP-35 panel/dialog reachability, GAP-36 workflow tests, GAP-37 canvas coords, GAP-38 page operations, GAP-39 text formatting toolbar, GAP-40 missing menu items, GAP-41 security/signing, GAP-42 misc); fix real bugs and document any items requiring separate product work as new tracked items.",
      "acceptance_criteria": [
        "GAP-34 (toolbar button reachability): after GAP-28 window fix, stickyNoteToolActionButton, highlightToolActionButton, inkToolActionButton, and rectangleToolActionButton are all reachable via UIA; any that remain unreachable after the window fix have their root cause identified and fixed or tracked.",
        "GAP-35 (panel/dialog widget reachability): annotFontCombo, bookmarkPanel_*, pagePanel_*, and imagePropertiesPanel_* are all reachable via UIA; complianceDialog_progressBar reachability is investigated and either fixed via a C++ AutomationId property or documented as a new tracked item.",
        "GAP-40 (missing menu items): each of the 17 failures is triaged — Preferences, Replace, Tab Order, OCR, TRE, Accessibility, Compliance Conversion menus are either confirmed present in the UIA tree (test fixed) or confirmed genuinely absent (new product GAP created for tracking).",
        "GAP-41 (security/signing): test_sign_visible and test_sign_invisible failures are triaged; if the signing workflow is product-complete and the test is a harness issue the harness is fixed; if the product feature is incomplete it is documented with a new GAP number.",
        "Total pytest FAILED count after this phase is <= 20 (residual should be only items requiring dedicated product-phase work or environment-specific issues with documented workarounds).",
        "pdfe_tests.exe count remains >= 3583 PASSED / 0 FAILED / 2 SKIPPED."
      ],
      "gate_agents": ["karen", "validator", "test-runner"]
    },
    {
      "id": "e5b2c947-3f8a-4d61-b091-7a4e0c6f3d25-phase-6",
      "name": "Closeout: RESOLVED annotations + full 0-FAILED sweep + strategy update",
      "description": "Annotate all resolved GAP items in post_a7f3d2c9_followups.md with RESOLVED markers and commit references, run the final full pytest sweep targeting 0 FAILED, and update tests/docs/test_strategy.md with the pipeline header for this run.",
      "acceptance_criteria": [
        "tests/docs/post_a7f3d2c9_followups.md: every GAP item resolved in this pipeline (GAP-28 through GAP-45, plus any sub-items) has a RESOLVED annotation with the phase number and commit hash where it was fixed.",
        "Full pytest sweep (pytest tests/ with no extra manual flags, using the permanent pytest.ini fix from phase-1) completes with 0 FAILED (xfail/xpass acceptable for items in KNOWN_WEAK_TESTS.txt with documented entries).",
        "Any remaining FAILED items are documented in KNOWN_WEAK_TESTS.txt with a new tracking ID, root cause summary, and reproduction steps — not silently ignored.",
        "tests/docs/test_strategy.md: pipeline header updated to reference pipeline e5b2c947 with the sweep date, final PASSED/FAILED/SKIPPED counts, and a brief summary of what was fixed.",
        "pdfe_tests.exe final run: count >= 3583 PASSED / 0 FAILED / 2 SKIPPED; exact counts quoted verbatim in the commit message.",
        "0 orphan PDFE.exe processes confirmed via tasklist after the final sweep."
      ],
      "gate_agents": ["karen", "validator", "test-runner"]
    }
  ]
}
```

---

## Staging Notes

- **Pipeline ID**: `e5b2c947-3f8a-4d61-b091-7a4e0c6f3d25`
- **Total phases**: 6
- **Phase ordering rationale**:
  - Phase 1 MUST run first (GAP-28 cascade root cause — resolves ~150 failures automatically).
  - Phase 2 is independent of phase-1 findings (test harness file path changes, not failure-count dependent).
  - Phase 3 (dialog UIA fix) is independent but benefits from phase-1 having stable window dimensions.
  - Phase 4 scope is explicitly gated on phase-1 re-sweep findings (post_phase1_sweep_results.md).
  - Phase 5 is the residual cluster cleanup — depends on phases 1-4 having resolved cascades.
  - Phase 6 is always last (closeout sweep).
- **No security-audit gate**: None of these phases touch auth, secrets, config, or infra. All are test harness and product UI bugs.
- **GoogleTest floor**: 3583 throughout (a7f3d2c9 phase-4 closeout baseline).
