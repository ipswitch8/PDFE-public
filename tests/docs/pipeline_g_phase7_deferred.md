# Pipeline f7c2a4e8 phase-7 — deferred coverage items

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


**Status:** F1, F2 (1 of 3), F3 delivered in `tests/ui_automation/test_new_coverage_f7c2a4e8.py`. G items + F4 + remaining F2 deferred to future security-focused pipeline.

Each item below has a brief scope statement + fixture/infrastructure requirement + test scaffold direction so the next pipeline can pick them up directly.

## F2 (remaining 2 of 3)

- **F2b: select [0,2] + Extract Pages** — Pages → Extract Pages... opens a file-save dialog. Needs `testSaveAsPipe`-style dispatch for the Extract output path. Test scaffold:
  1. select_pipe.set_edit_text("0,2")
  2. click_menu_path("Pages -> Extract Pages...")
  3. Drive Save dialog (or use a future testExtractPipe)
  4. Assert extracted file has exactly 2 pages
- **F2c: select [0,2] + Rotate Clockwise** — Same handler. Test scaffold:
  1. select_pipe.set_edit_text("0,2")
  2. click_menu_path("Pages -> Rotate Clockwise")
  3. Read dirty marker / pageLabel; verify 2 of 3 page-rotations registered

## F4: broader Pages-menu workflows

The B-pipe work in phase-3 unlocked drag-drop and basic selection. Untested broader workflows:
- Pages → Move Up / Move Down / Move To Start (these were P-GAP-93)
- Pages → Insert From File (needs file-open dialog driving)
- Pages → Insert Blank (already covered by other tests)

Path: each gets a small test that uses testSelectPagesPipe to set up + menu dispatch to act + pageLabel oracle to verify.

## G1: UI-level redaction roundtrip

**Fixture needed:** A PDF with known text at known coords that can be redacted (`test_redaction_*.pdf` exists at `tests/fixtures/`).
**Infrastructure needed:** Headless byte-verifier from `tests/test_redaction_verifier.cpp` exposed via a Python helper.
**Test scaffold:**
1. Open redaction fixture
2. Activate RedactionTool via testToolActivatePipe
3. Use testAddRedactionMarkPipe (existing) to place a redaction rect
4. Click "Apply Redactions" menu
5. Save via testSaveAsPipe
6. Call a Python helper that wraps the C++ byte-verifier
7. Assert redacted bytes are absent from saved file

## G2: wrong-password retry/cancel

**Fixture needed:** An encrypted PDF where the test knows the correct password (e.g. `tests/fixtures/test_encrypted.pdf` with known password "test123").
**Test scaffold:**
1. Open encrypted PDF → password dialog appears
2. Branch A: Enter wrong password → assert error message → cancel → no document loaded
3. Branch B: Enter wrong → error → re-enter correct → assert document loads

This requires UIA-driving the password dialog including the error-toast/MessageBox handling.

## G3: JS form-action sandbox

**Fixture needed:** A form PDF with a hostile JS action (e.g. `/Action /JS app.launchURL('http://evil.com')`).
**Infrastructure needed:** A way to assert no external process spawned + no network request made.
**Test scaffold:**
1. Open hostile-JS PDF
2. Trigger the form action (Tab into field + Enter, or click button)
3. Assert no child process appeared in `tasklist`
4. Assert PDFE main window still alive
5. Assert no popup confirmation appeared (or a confirmation popup DID appear, depending on policy)

The MuJS layer needs to either reject or confirm these actions; verifying the policy is the test target.

## G4: encryption password-strength enforcement

**Fixture needed:** None (use Save with Encryption menu on existing doc).
**Test scaffold:**
1. Open any PDF
2. Click Security → Save with Encryption...
3. Enter a weak password (e.g. "abc")
4. Assert passwordStrengthBar/Label show a "weak" indicator
5. Assert the Save/OK button is DISABLED (or click it and assert the action is blocked)
6. Enter a strong password (e.g. "Str0ng!Passw0rd")
7. Assert strength shows "strong" + Save button enabled

This needs the strength-bar value read via UIA Value pattern.

## G5: visible/invisible signature workflows (P-GAP-78)

**Status:** 2 tests in test_security_compliance_p1.py xfailed.
**Fixture needed:** `tests/fixtures/test_signer.pfx` (exists, password may need verification).
**Per AC**, these can remain xfail with updated comments. The phase-7 acceptance bar for G5 is met by the existing xfail markers being current and accurately reasoned. No code change required.

Per-test xfail in conftest.py (already present from b5e3a9f2). KNOWN_WEAK_TESTS.txt P-GAP-78 is RESOLVED at the file-level (b5e3a9f2 phase-7) with 2 per-test residuals xfailed — those 2 residuals are P-GAP-78's "signing requires valid PFX + signature workflow setup" which remains accurate.

## Summary

| Item | Status | Path |
|---|---|---|
| F1 | DELIVERED | test_new_coverage_f7c2a4e8.py::test_f1_save_as_pipe_no_document_does_not_create_file |
| F2a (delete) | DELIVERED | test_new_coverage_f7c2a4e8.py::test_f2_select_pages_then_delete_via_menu |
| F2b (extract) | DEFERRED (honest skip — needs testExtractPipe or pymupdf-side verifier) | test_new_coverage_f7c2a4e8.py::test_f2b_select_pages_then_extract (skip-marked) |
| F2c (rotate) | DELIVERED | test_new_coverage_f7c2a4e8.py::test_f2c_select_pages_then_rotate_clockwise |
| F3 | DELIVERED | test_new_coverage_f7c2a4e8.py::test_f3_image_properties_panel_values_after_click |
| F4 | DEFERRED | needs per-menu-row test infrastructure |
| G1 | DEFERRED | needs C++ byte-verifier Python binding |
| G2 | DELIVERED (cancel branch) | test_new_coverage_f7c2a4e8.py::test_g2_wrong_password_then_cancel |
| G3 | DEFERRED | needs hostile-JS PDF fixture |
| G4 | DELIVERED (after amend 2: passes by selecting AES-128 first to enable userPasswordEdit) | test_new_coverage_f7c2a4e8.py::test_g4_password_strength_label_reflects_input |
| G5 | xfail (per AC) | conftest already has the 2 markers; AC allows xfail+comment |
