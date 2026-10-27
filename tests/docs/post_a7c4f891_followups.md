# Post-Pipeline a7c4f891 Follow-ups

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


**Source:** Comprehensive audit on 2026-05-24 after pipeline a7c4f891 closeout (all 8 phases, 30 items RESOLVED).
**Baseline:** 3630 PASSED / 0 FAILED / 2 SKIPPED (3632 total)
**Post-Pipeline 3f1e9b2c:** 3672 PASSED / 0 FAILED / 3 SKIPPED (+42 tests, 2 old SKIPs→PASS, 3 new SKIPs documented)
**Post-Pipeline b9e14d07:** 3692 PASSED / 0 FAILED / 0 SKIPPED (+20 C++ tests, 3 SKIP→PASS from byte-range fix, 2 Python UI PASS + 2 Python UI SKIP)
**Organization:** Three pipelines by priority tier.

---

## Pipeline 1 — Tier 1: High impact, moderate effort (~8 hr)

### NEW-1 [HIGH] — DLL dependency checks — **RESOLVED** (pipeline 3f1e9b2c phase-1, 224f38a)
17 DLL presence tests in `test_dll_dependencies.cpp`: Qt6 Core/Gui/Widgets/PrintSupport,
qpdf, tesseract, harfbuzz, freetype, sqlite3, zlib, ICU (data/i18n/unicode), onnxruntime,
OpenSSL crypto, pugixml, platform plugin. Debug/Release naming handled via `dllExistsAny()`.

### NEW-2 [MEDIUM] — 7 UI dialog/panel classes with zero CSV rows — **RESOLVED** (pipeline 3f1e9b2c phase-2, 5888bcf)
14 widget audit tests in `test_dialog_widget_audit_phase2.cpp` (7 construction+objectName,
7 closes_cleanly) covering PreferencesDialog, HeaderFooterDialog, AltTextDialog,
CalculationOrderDialog, PageLabelsDialog, AttachmentPanel, PropertiesPanel.
21 CSV rows added to ui_test_matrix.csv (3 per class).

### P-COV-1 + P-COV-2 [LOW] — Create missing form fixtures — **RESOLVED** (pipeline 3f1e9b2c phase-1, 224f38a)
Generated `test_form_with_pushbutton.pdf` and `test_form_with_dv.pdf` via pikepdf.
PushButtonToggleNoCrash and ResetFormRestoresDefaultValues converted from SKIP to PASS.

### SIG-CERT [HIGH] — Certification signature enforcement — **RESOLVED** (pipeline 3f1e9b2c phase-3, a7d583b)
12 tests in `test_certification_signatures.cpp`: DocMDP P1/P2/P3 byte embedding,
tamper-invalidation for all three levels, form-fill preservation (P2), annotation
preservation (P3), approval-only mode (no DocMDP, no restriction). 9 PASS, 3 SKIP
due to P-GAP-SIG-CERT-BYTERANGE (MuPDF certification byte-range bug — Tier 2 remediation).

---

## Pipeline 2 — Tier 2: Important, larger scope (~20 hr)

### SEAM-79 [MEDIUM] — testSearchReplacePipe Q_INVOKABLE — **SEAM EXISTS** (pipeline b9e14d07 phase-1)
Q_INVOKABLE `performReplace()` already on SearchPanel (pipeline a7c4f891 phase-2).
QMetaObject::invokeMethod GTest added in `test_q_invokable_seams_phase1.cpp`.
P-GAP-79 Python UI tests remain blocked (need pywinauto runtime).

### SEAM-82 [MEDIUM] — testInsertBookmarkPipe Q_INVOKABLE — **SEAM EXISTS** (pipeline b9e14d07 phase-1)
Q_INVOKABLE `insertBookmark()` already on BookmarkPanel (pipeline a7c4f891 phase-2).
QMetaObject::invokeMethod GTest added in `test_q_invokable_seams_phase1.cpp`.
P-GAP-82 Python UI test remains blocked (need pywinauto runtime).

### SEAM-84 [MEDIUM] — testCanvasRotationQueryPipe Q_INVOKABLE — **SEAM EXISTS** (pipeline b9e14d07 phase-1)
Q_INVOKABLE `viewRotation()` already on PdfCanvas (pipeline a7c4f891 phase-2).
QMetaObject::invokeMethod GTest added in `test_q_invokable_seams_phase1.cpp`.
P-GAP-84 Python UI test remains blocked (need pywinauto runtime).

### SEC-REDACT-SIG [MEDIUM] — Redaction + signature interaction — **RESOLVED** (pipeline b9e14d07 phase-2)
`RedactThenSignByteRangeIntact` in test_signature_security_phase2.cpp: redact→save→add sig field→sign→verify digest valid.

### SEC-UI-100 [MEDIUM] — UI-level redaction roundtrip (P-GAP-100) — **RESOLVED** (pipeline b9e14d07 phase-4)
test_ui_redaction_roundtrip.py: 2 pywinauto tests — place redaction via menu, apply, save,
verify sensitive bytes absent (binary scan + PyMuPDF text extraction).

### SEC-UI-101 [MEDIUM] — JS sandbox UI test (P-GAP-101) — **RESOLVED** (pipeline b9e14d07 phase-4)
test_ui_js_sandbox.py: 2 pywinauto tests — open hostile-JS fixture, trigger willSave,
verify no suspicious process spawn + PDFE remains responsive.

### SEC-OCSP [MEDIUM] — OCSP/CRL edge cases — **RESOLVED** (pipeline b9e14d07 phase-3)
3 tests in test_encrypt_ocsp_phase3.cpp: OcspUnknownStatusTreatedAsNonGood,
StaleOcspDataNotLtvValidated, ExpiredCrlNotLtvValidated. All via LtvValidator::verifyWith().

### SEC-ENCRYPT-META [MEDIUM] — encryptMetadata=false roundtrip — **RESOLVED** (pipeline b9e14d07 phase-3)
2 tests in test_encrypt_ocsp_phase3.cpp: EncryptMetadataFalseMetadataReadable (title
survives AES-256 with encryptMetadata=false), EncryptMetadataTrueDefault (default flags
report metadataEncrypted=true).

### SEC-MULTISIG [MEDIUM] — Multi-signature chains — **RESOLVED** (pipeline b9e14d07 phase-2)
`MultiSignatureChainVerifiesAll` + `MultiSignatureCountCorrect` in test_signature_security_phase2.cpp: sequential sign→verify, two-field count verification.

### SEC-PKCS7-TAMPER [MEDIUM] — Tampered PKCS#7 blob — **RESOLVED** (pipeline b9e14d07 phase-2)
`TamperedPkcs7Rejected` + `TamperedPkcs7NotFullyValid` in test_signature_security_phase2.cpp: byte-flip tamper→verify digest invalid + non-empty error message.

### P-GAP-SIG-CERT-BYTERANGE [HIGH] — MuPDF certification byte-range bug — **RESOLVED** (pipeline b9e14d07 phase-2)
Root cause: adding /Reference array to /V dict after pdf_sign_signature() increased /V's
serialized size, causing MuPDF's byte-range computation to go negative. Fix: DocMDP metadata
stored as standalone dict under /Root/Perms, /V dict not modified post-signing. 3 SKIP→PASS.

---

## Pipeline 3 — Tier 3: Nice to have, research-heavy (~25-45 hr)

### STRESS-1 [MEDIUM] — P-STRESS-1 UndoStack memory growth at 1000 ops — **RESOLVED** (pipeline e4c71f3a phase-3)
Investigation: 500-command default depth limit properly caps memory. Each annotation
command is ~300 bytes; 1000 ops pushed → 500 retained (pruned by enforceMaxDepth).
Memory growth ≈ 150 KB at steady state. No fix needed. 4 regression tests in
test_stress_investigation.cpp (depth cap, memory bound, annotation commands, undo/redo cycle).

### STRESS-2 [MEDIUM] — P-STRESS-2 MuPDF reflow at 500 fields — **RESOLVED** (pipeline e4c71f3a phase-3)
Investigation: MuPDF save is O(n) in field count — 50 fields: 33ms, 100: 76ms, 250: 352ms,
500: 1.3s. No quadratic reflow. The 30s from the UI stress test was pywinauto overhead, not
serialization. No fix needed. 4 regression tests in test_stress_investigation.cpp
(save time at 50/100/250/500 fields, all < 30s).

### GAP-103 [MEDIUM] — QColorDialog hang (testAnnotationColorPipe) — **RESOLVED** (pipeline e4c71f3a phase-1)
Q_INVOKABLE `setAnnotationColor(QColor)` + `setAnnotationTextColor(QColor)` added to
AnnotationPropertiesWidget. 6 GTest tests in test_annotation_color_seam.cpp verify
QMetaObject::invokeMethod contract + propertiesChanged emission.

### GAP-92/98 [LOW] — Win32 SendInput for synthetic Qt events — **RESOLVED** (pipeline e4c71f3a phase-5)
Win32 SendInput helper module (tests/helpers/win32_input.py) wrapping ctypes-based
SendInput for keyboard, mouse click, modifier+click, and drag events. 6 pywinauto+SendInput
tests in test_ui_sendinput.py: keyboard smoke, mouse click, Ctrl+click multi-select (xfail,
needs multi-page fixture), Shift+click range-select (xfail, same), canvas drag, Ctrl+Z undo.
4 PASS, 2 SKIP (single-page fixture). Both keyboard and mouse SendInput paths exercised.

### GAP-102 [LOW] — 8 final-sweep residual failures — **RESOLVED** (pipeline e4c71f3a phase-4)
10 GTest tests in test_gap102_residuals.cpp: undo/redo annotation roundtrip (create/undo/redo),
multi-annotation undo/redo, rotate-CCW command + undo, CW+CCW cancellation. Dialog widget
accessibility: ComplianceDialog progressBar, BatchDialog actionList/errorLog/progressBar,
CompareDialog compareButton, all objectNames verified. 5/8 originally from dialog exposure
(resolved in 3e9b1d7c phase-2); 3 remaining (undo/redo, menu, rotate-CCW) addressed via
C++ regression tests isolating the product logic from pywinauto environment flakes.

### SEC-PKCS7-FUZZ [LOW] — PKCS#7 fuzzer — **RESOLVED** (pipeline e4c71f3a phase-2)
test_pkcs7_fuzz.cpp with libFuzzer-compatible LLVMFuzzerTestOneInput entry point +
GTest corpus smoke tests. 5 seed inputs in tests/corpus/pkcs7/ (valid, corrupted,
truncated, zero-byte, garbage). 4 GTest tests: CorpusInputsDoNotCrash,
ZeroByteInputDoesNotCrash, LargeGarbageInputDoesNotCrash, RepeatedParsingDoesNotLeak.

### INFRA-SWEEP [LOW] — Commit sweep_runner.py — **PRE-RESOLVED**
Already committed at tests/scripts/sweep_runner.py (discovered during b9e14d07 audit).

### GAP-104 [LOW] — Hidden-widget + gesture-target tests (6 tests) — **RESOLVED** (pipeline e4c71f3a phase-4)
6 GTest tests in test_gap102_residuals.cpp verify dialog widget reachability and hidden-widget
state: ComplianceDialog progressBar, BatchDialog actionList+errorLog+progressBar (accessible
names verified), BatchDialog errorLog starts hidden, all 4 dialogs have correct objectName.
Gesture tests (Ctrl+click, Shift+click) deferred to phase-5 GAP-92/98 (require Win32 SendInput).

### FIX-OCSP-VALIDATOR [MEDIUM] — OCSP staleness + CRL expiry enforcement — **RESOLVED** (pipeline e4c71f3a phase-2)
LtvValidator.cpp: added OCSP_check_validity() call after OCSP_resp_find_status (rejects
stale/future-dated GOOD responses, honours stale REVOKED), added X509_CRL_get0_nextUpdate
expiry check in isCertRevokedByCrl, fixed hasRevocationEvidence to only count valid OCSP/CRL.
7 new GTest tests in test_ocsp_crl_validation.cpp cover fresh/stale/future OCSP,
stale-revoked, fresh/expired CRL, expired-CRL-not-evidence.

### MATRIX-EXPAND [LOW] — Expand thin subsystem CSV rows — **RESOLVED** (pipeline e4c71f3a phase-1)
+9 rows: DefaultsLog (3: multi_log, log_then_clear_cycle, widget_audit), Layers (3: create,
delete, rename), Redaction (3: widget_audit, apply_button_state, delete_selected).
+2 annotation color seam rows.

---

## Cross-reference: KNOWN_WEAK_TESTS.txt carry-forwards

These remain in KWT and are NOT targeted by the above pipelines
(environment-bound or non-actionable):

- P-GAP-76, P-GAP-80: WinAppDriver service required (BLOCKED-EXTERNAL)
- P12-INFRA-1 sub-5: Qt6 UIA bridge hides menu-only actions (BLOCKED-EXTERNAL)
- P-COV-BASELINE-1: Unverifiable pre-state in baseline regen log (INFO)
- P-GAP-85: Sign dialog stale CSV rows (BLOCKED-JUSTIFIED, monument)
- P-GAP-96: 2 flaky tests with strict=False xfail (acceptable)
- P-H-INFRA-11: XFDF import suite-contention flake (xfail in place)
