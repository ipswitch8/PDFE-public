# Pipeline 3e9b1d7c Follow-ups (post-closeout backlog)

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


**Source:** Audit findings from `/a` query on 2026-05-23 after pipeline 3e9b1d7c closeout.
**Categories:** NEW issues from this pipeline, UNRESOLVED old items, UI test matrix gaps, security test matrix gaps.

---

## Theme A — Test integrity / new-issue remediation (smallest, highest urgency)

**A1 [HIGH] — `test_encryption_dialog.cpp` PasswordMismatch test integrity**
After phase-6 E4 added 8-char password minimum, tests 5+6 (`PasswordMismatch_UserPassword` and `PasswordMismatch_OwnerPassword`) at lines 151-209 use 7-char passwords ("user123"/"owner123"). The OK button is now disabled for length reasons before mismatch is ever checked. Tests pass under `EXPECT_FALSE(okButton->isEnabled())` but no longer exercise their named scenarios.
**Fix:** bump to 10-char passwords with deliberate mismatch, AND add `EXPECT_TRUE(validationLabel->text().contains("do not match"))` assertion to lock the actual mismatch check.
**Effort:** XS (15 min).

**A2 [MEDIUM] — EncryptionDialog Unicode password length**
`EncryptionDialog::validateInput()` at `src/ui/EncryptionDialog.cpp:261-275` uses `QString::length()` which counts UTF-16 code units, not bytes. Multi-byte CJK/emoji passwords may pass the 8-char UI check but fail at the encryption backend.
**Fix:** evaluate `toUtf8().size()` against the backend's minimum, OR document the UI/backend semantic mismatch with a comment.
**Effort:** S (30 min).

**A3 [MEDIUM] — TabOrderDialog moveProgrammatic seam**
Phase-3 added `moveProgrammatic(int from, int to)` Q_INVOKABLE to `CalculationOrderDialog`. The identical list-reorder pattern in `src/ui/TabOrderDialog.cpp` has no test seam.
**Fix:** mirror the CalculationOrderDialog pattern — add `Q_INVOKABLE bool moveProgrammatic(int fromIndex, int toIndex)` to `TabOrderDialog.h/.cpp`, gated by `PDFE_TEST_PIPES` if appropriate.
**Effort:** S (1 hr).

**A4 [LOW] — PreferencesDialog tab pane objectNames**
Phase-2 renamed `tabWidget_` to `preferencesTabs` but the 5 tab pages (general/view/security/ocr/advanced) at `src/ui/PreferencesDialog.cpp:44,66,125,167,193` have no objectNames. Tab-by-index lookups remain brittle.
**Fix:** add `setObjectName("preferencesGeneralTab")` etc. on each `QWidget` returned by `createXxxTab()` helpers.
**Effort:** XS (15 min).

---

## Theme B — Security coverage gaps (Critical/High)

**B1 [CRITICAL] — RC4-40 + RC4-128 roundtrip tests**
`tests/test_encryption_roundtrip.cpp` only covers AES-128/256. `RC4_40` and `RC4_128` are defined in `EncryptionSettings.h` with no roundtrip coverage.
**Fix:** add `RC4RoundTripTest` fixture with 4 cases: RC4-40 set + reopen, RC4-128 set + reopen. Each verifies password works, wrong password fails, permissions survive.
**Effort:** S (2 hr).

**B2 [HIGH] — Signature tampering detection**
`tests/test_signature_roundtrip.cpp` has 8 tests but does NOT verify byte-mutation detection.
**Fix:** add `SignatureRoundTripTest::TamperedSignatureDetected` — sign PDF, mutate 1 byte in content stream, reopen, assert `signature.digestValid == false` and `digestStatus` reports tamper.
**Effort:** S (1.5 hr).

**B3 [HIGH] — JS sandbox extensions (E2 follow-up)**
Phase-6 E2 covered `app.launchURL`, `app.execMenuItem`, `app.mailMsg`. Other potentially-hostile APIs not tested: `app.openDoc("file://...")`, `eval()`, property-hijacking chains (`this.getField("X").value = ...`).
**Fix:** extend `hostile_js_pdf` fixture in `test_security_e1_e4.py` with new hostile JS variants + assert no filesystem access + no field corruption via property setter chains.
**Effort:** M (3 hr).

**B4 [HIGH] — Redaction on encrypted documents**
`tests/test_redaction_edge_cases.cpp` covers mark/apply but not encryption-paired flows.
**Fix:** add `RedactionOnEncryptedPdf` test — create encrypted PDF → mark redactions → apply → save → reopen with password → verify redacted bytes absent.
**Effort:** S (2 hr).

**B5 [HIGH] — Redaction with annotations**
No test verifies sticky-note/highlight annotations survive `applyRedactions`.
**Fix:** add `RedactionPreservesAnnotations` test in `test_redaction_edge_cases.cpp` — add sticky note → mark redaction (non-overlapping) → apply → verify annotation count unchanged.
**Effort:** S (1.5 hr).

**B6 [HIGH] — OCR-text layer redaction**
Phase-6 E1 covered native text redaction. OCR'd text layers (ActualText / hidden Tj operators from Tesseract) not tested.
**Fix:** create OCR fixture with ActualText, add redaction roundtrip, verify byte-level absence.
**Effort:** M (2.5 hr).

**B7 [HIGH] — PDF/A malformed-claim detection**
A PDF can claim `/pdfaid:part=2` conformance via `/Catalog/Metadata` but contain non-compliant objects. `test_compliance_manager.cpp` doesn't test detection.
**Fix:** create malformed-claim fixture (PDF with `/pdfaid:part=2` but uses RGB without OutputIntent), assert `ComplianceManager::validate()` detects the violation.
**Effort:** M (3 hr).

**B8 [MEDIUM] — Encryption dict fuzzing**
`tests/security/fuzz/` has 6 harnesses, none target `/Encrypt` parsing.
**Fix:** add `fuzz_encryption_dict.cpp` LibFuzzer harness; seed with malformed `/Encrypt` objects (oversize key lengths, circular permission refs).
**Effort:** M (3 hr).

**B9 [MEDIUM] — Permission enforcement verification**
`RestrictedPermissionsSurviveSave` verifies bits round-trip but not enforcement (can copy-disabled PDF actually be copied?).
**Fix:** add integration test — encrypt with `copy=false`, attempt programmatic copy via PDFE API, assert failure.
**Effort:** M (3 hr).

**B10 [MEDIUM] — PFX/cert chain validation**
`test_signature_roundtrip.cpp` uses `test_signer.pfx` only. No expired cert / self-signed / intermediate CA cases.
**Fix:** generate expired PFX fixture, test `verifySignature` reports cert-expired status. Add self-signed + intermediate CA tests.
**Effort:** M (4 hr).

**B11 [MEDIUM] — Deeply-nested object DoS**
`fuzz_document_open.cpp` doesn't have recursive-reference seeds.
**Fix:** add corpus PDF with 1000-level `/Parent` chain to `fuzz_document_open` seeds; verify parser doesn't stack-overflow.
**Effort:** S (1.5 hr).

**B12 [LOW] — Cryptographic weak-password rejection documentation**
Phase-6 E4 validates UI button disable but not crypto-backend behavior with weak passwords.
**Fix:** add comment in `test_encryption_roundtrip.cpp` documenting MuPDF/QPDF behavior with < 8 char passwords (whether they reject or accept). Optionally add explicit test if reject.
**Effort:** XS (15 min).

---

## Theme C — UI test matrix gaps (P0/P1)

**C1 [P0] — 12 untracked dialogs/wizards**
The static `ui_test_matrix.csv` has zero rows for: BatchDialog, CombineFilesWizard, CompareDialog, ImageToPdfWizard, OptimizeDialog, PreflightDialog, CustomizeToolbarsDialog, TabOrderDialog, WatermarkDialog, MetadataDialog, PrintDialog, PrintPreviewDialog.
**Fix:** for each dialog, add at least 3 rows: `dialog_opens`, `dialog_widget_audit` (per-widget objectName checks), `dialog_closes_cleanly`. Use existing testToolActivatePipe pattern for opening.
**Effort:** L (8 hr — 12 dialogs × ~40min each).

**C2 [P0] — Image subsystem coverage**
Only smoke tests exist. ImagePropertiesPanel widget tests missing.
**Fix:** add panel widget audit + InsertImage flow + CropImage flow + RotateImage flow with PdfCanvas oracles.
**Effort:** M (3 hr).

**C3 [P1] — 4 panels missing interaction tests**
LayerPanel, DefaultsLogPanel, RedactionPanel — menu toggles tested but no add/delete/reorder flows.
**Fix:** for each panel, add 2 interaction tests (typical user op + edge case).
**Effort:** M (4 hr).

**C4 [P1] — Print dialog layout integration**
printDialog has 16 widget rows but no layout/button integration tests.
**Fix:** add Page Setup + Print Range + Color Mode integration tests using testToolActivatePipe.
**Effort:** M (3 hr).

---

## Theme D — Test infrastructure seams (unblocks deferred clusters)

**D1 [HIGH leverage] — testAtlasReadyPipe Q_INVOKABLE**
Unblocks P-GAP-74 (17 tests in `test_precondition_panels_phase4.py`).
**Design:** add a `Q_INVOKABLE bool isAtlasReady()` to AtlasComparisonDialog (or the relevant atlas-building widget). Tests poll this before driving downstream operations.
**Effort:** M (3 hr).

**D2 — testSearchReplacePipe Q_INVOKABLE**
Unblocks P-GAP-79 (test_ui_search_replace.py).
**Design:** add `Q_INVOKABLE int performReplace(QString find, QString replace, bool all)` to SearchReplaceDialog. Returns count of replacements.
**Effort:** M (3 hr).

**D3 — testCanvasRotationQueryPipe Q_INVOKABLE**
Unblocks P-GAP-84 (test_ui_view_rotate.py + test_view_rotate_cw_90).
**Design:** add `Q_INVOKABLE int viewRotation()` to PdfCanvas. Returns 0/90/180/270.
**Effort:** S (1 hr).

**D4 — testInsertBookmarkPipe Q_INVOKABLE**
Unblocks P-GAP-82 (test_ui_bookmark_panel.py).
**Design:** add `Q_INVOKABLE bool insertBookmark(int pageIndex, QString title)` to BookmarkPanel.
**Effort:** S (1.5 hr).

---

## Theme E — Old PARTIAL items

**E1 — P8-BUG-3 + P8-BUG-4 UIA flakes (2 tests each)**
Intermittent UIA wait timeouts. Possibly addressable with the same `_wait_title_is_dirty`-style polling pattern added in phase-7.
**Effort:** M (3 hr) — investigate + apply polling wrappers.

**E2 — P12-INFRA-1 printAction UIA reachability**
**Effort:** S (1.5 hr) — likely needs setObjectName + accessibleName audit on PrintDialog.

**E3 — P13-INFRA-4 AtlasComparisonDialog AC4/AC5**
Coupled to D1 (testAtlasReadyPipe).
**Effort:** rolled into D1.

**E4 — P-G-INFRA-7 form checkbox save/reopen**
**Effort:** S (2 hr) — test isolation issue likely.

**E5 — P-GAP-105 visual baseline regeneration**
5 visual baseline tests drift from stored references.
**Fix:** regenerate baselines via existing capture script, OR raise SSIM tolerance to ~0.7, OR add tests/conftest.py-level xfail dict for these test_ids.
**Effort:** S (1 hr).

---

## Recommended pipeline (phase outline)

The natural ordering is:

1. **Phase 1**: Theme A (test integrity + small fixes) — A1, A2, A3, A4 (~2 hr total)
2. **Phase 2**: Theme D infrastructure seams (D3 → unblocks F4; D4 → unblocks F3; D2 → unblocks F2) (~5 hr)
3. **Phase 3**: Theme E (P-GAP-79/82/84 conversions using new seams from phase 2, plus E5 baselines, E2 printAction) (~4 hr)
4. **Phase 4**: Theme B Critical/High security — B1 (RC4 roundtrip), B2 (tampering), B3 (JS extensions), B4-B6 (redaction triplet), B7 (PDF/A claim) (~14 hr)
5. **Phase 5**: D1 + E3 testAtlasReadyPipe — unblocks 17 P-GAP-74 tests (~4 hr)
6. **Phase 6**: Theme B Medium security — B8-B11 (fuzzing + permission + cert + DoS) (~12 hr)
7. **Phase 7**: Theme C P0 UI matrix gaps — C1 12 dialogs + C2 Image subsystem (~11 hr)
8. **Phase 8**: Theme C P1 + Theme E remaining + closeout — C3, C4, E1, E4, B12, full sweep, KWT update

Total estimated effort: ~55 hr (7 days at 8hr/day).

The highest-leverage phase is **Phase 5** (D1 unblocks 17 tests for a 3hr investment).
The highest-urgency phase is **Phase 4** (security gaps are externally-facing concerns).

---

## Final Disposition — Pipeline a7c4f891 (all 8 phases complete)

| Item | Status | Phase / Commit |
|------|--------|----------------|
| A1 | RESOLVED | phase-1 |
| A2 | RESOLVED | phase-1 |
| A3 | RESOLVED | phase-1 |
| A4 | RESOLVED | phase-1 |
| B1 | RESOLVED | phase-4 (3a34655) |
| B2 | RESOLVED | phase-4 (3a34655) |
| B3 | RESOLVED | phase-4 (3a34655) |
| B4 | RESOLVED | phase-4 (3a34655) |
| B5 | RESOLVED | phase-4 (3a34655) |
| B6 | RESOLVED | phase-4 (3a34655) |
| B7 | RESOLVED | phase-4 (3a34655) |
| B8 | RESOLVED | phase-6 (9c04993) |
| B9 | RESOLVED | phase-6 (9c04993) |
| B10 | RESOLVED | phase-6 (9c04993) |
| B11 | RESOLVED | phase-6 (9c04993) |
| B12 | RESOLVED | phase-6 (9c04993) |
| C1 | RESOLVED | phase-7 (29a1e4d) |
| C2 | RESOLVED | phase-7 (29a1e4d) |
| C3 | RESOLVED | phase-8 |
| C4 | RESOLVED | phase-8 |
| D1 | RESOLVED | phase-5 (a65cafb) |
| D2 | RESOLVED | phase-2 |
| D3 | RESOLVED | phase-2 |
| D4 | RESOLVED | phase-2 |
| E1 | RESOLVED | phase-3 |
| E2 | RESOLVED | phase-3 |
| E3 | RESOLVED | phase-5 (a65cafb) — coupled to D1 |
| E4 | RESOLVED | phase-3 |
| E5 | RESOLVED | phase-3 |

All 30 items from the post-3e9b1d7c audit have been addressed across
pipeline a7c4f891 phases 1-8. C++ suite final: 3630 PASSED / 0 FAILED / 2 SKIPPED.
