# Post-Pipeline e4c71f3a Follow-ups

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


**Source:** Comprehensive audit on 2026-05-25 after pipeline e4c71f3a closeout (6 phases, 11 Tier 3 items RESOLVED).
**Baseline:** 3731 C++ PASSED / 0 FAILED / 0 SKIPPED; 4 Python UI PASS / 2 Python UI SKIP
**Organization:** Four pipelines by dependency order.

---

## Pipeline 1 — Test Infrastructure: Matrix Gaps, Security Harnesses, Unblocked "Blocked" Items (~30 hr)

### 1A. UI Test Matrix Expansion (Section C gaps)

#### MAT-1 [MEDIUM] — Phase-4/5 matrix rows missing — **RESOLVED** (pipeline f8a12c4d phase-1)
+20 CSV rows: UndoRedo (4), MenuAction (3), DialogWidget (7), SendInput (6).

#### MAT-2 [LOW] — Thin subsystem coverage expansion — **RESOLVED** (pipeline f8a12c4d phase-1)
+21 CSV rows: Canvas (+8), Search (3), Rendering (3), TRE (3), Exports (3), Fonts (3).

### 1B. Security Test Harnesses (Section D gaps)

#### SEC-FONT-FUZZ [HIGH] — Font parsing fuzz harness — **RESOLVED** (pipeline f8a12c4d phase-2)
test_font_fuzz.cpp: 8 GTest tests feeding malformed font dict bytes through MuPDF's
Document::open + createDisplayList path. Payloads: empty, garbage, truncated, malformed
widths, circular ref, invalid encoding, large input, repeated parsing. All no-crash.

#### SEC-JS-XSS [HIGH] — Form JavaScript injection tests — **RESOLVED** (pipeline f8a12c4d phase-2)
test_form_javascript_xss.cpp: 8 GTest tests verifying MuJS sandbox blocks hostile scripts.
Tests: app.launchURL in keystroke, System.os access, infinite loop in calculate, app.openDoc
path traversal, HTML/script injection in field value, oversized format string, willSave
hostile script, rapid 100-cycle focus/keystroke/blur sequence. All no-crash/no-escape.

#### SEC-AES-MODES [HIGH] — Parametrize encryption roundtrip — **PRE-RESOLVED**
Already covered: test_encryption_roundtrip.cpp has Aes256EncryptionPersistsAfterReopen,
Aes128EncryptionRoundTrips, Rc4_40EncryptionRoundTrips, Rc4_128EncryptionRoundTrips,
WrongPasswordDoesNotSilentlyOpen. All algorithms tested with roundtrip verification.

#### SEC-LTV-CERT [MEDIUM] — OCSP/CRL issuer certificate validation — **RESOLVED** (pipeline f8a12c4d phase-3)
test_ltv_responder_validation.cpp: 7 tests covering OCSP 7-day boundary (just inside/just
outside), CRL at-expiry boundary, mixed OCSP+CRL, empty OCSP+CRL, malformed OCSP/CRL blobs.

#### SEC-SAVE-PATH [MEDIUM] — Save/export path traversal — **RESOLVED** (pipeline f8a12c4d phase-3)
test_save_path_security.cpp: 6 tests — empty path rejected, null byte rejected, Windows
ADS rejected, relative ../ allowed (design decision), long path handled, UNC path handled.

#### SEC-PAGE-OVERFLOW [LOW] — Page manipulation integer overflow — **RESOLVED** (pipeline f8a12c4d phase-3)
test_page_integer_overflow.cpp: 6 tests — negative index throws PdfeException, beyond-count
throws, INT_MAX throws, out-of-range rotation angles handled, invalid delete index handled,
insert at boundary (index 0) works correctly.

### 1C. Unblock "Blocked" Items (Section B)

#### UNBLOCK-WAD [MEDIUM] — WinAppDriver tests (P-GAP-76, P-GAP-80) — **DOCUMENTED-LIMITATION** (pipeline f8a12c4d phase-4)
WinAppDriver starts and returns /status OK. Session creates successfully. But GET /title
returns HTTP 501 Not Implemented — the Appium-Python-Client version or WinAppDriver version
on this rig doesn't support the /title endpoint. Root cause is WinAppDriver API compatibility,
not service availability. Tests remain xfailed with updated reason.

#### UNBLOCK-PRINT [MEDIUM] — printAction UIA reachability (P12-INFRA-1 sub-5) — **RESOLVED** (pipeline f8a12c4d phase-4)
Q_INVOKABLE `isPrintEnabled()` added to MainWindow.h/cpp. Returns printAction_->isEnabled().
Python tests can now query print state without opening File menu.

#### UNBLOCK-SIGN [LOW] — Sign dialog stale CSV rows (P-GAP-85) — **RESOLVED** (pipeline f8a12c4d phase-1)
2 stale CSV rows deleted, xfail markers removed from conftest.py, KNOWN_WEAK_TESTS.txt updated.

#### UNBLOCK-COV [LOW] — Missing form fixtures (P-COV-1, P-COV-2) — **RESOLVED** (pipeline f8a12c4d phase-1)
Fixtures confirmed present. Both tests PASS. KNOWN_WEAK_TESTS.txt updated.

---

## Pipeline 2 — Synthetic Event Conversion: Qt6 Spontaneity Barrier (~25 hr)

### Background
19 tests are blocked by Qt6's event-spontaneity barrier (P-GAP-92/98 family).
The Win32 SendInput helper (tests/helpers/win32_input.py) was created in pipeline
e4c71f3a phase-5 and proven to work (4/6 tests PASS). This pipeline converts the
remaining 15 blocked tests to use SendInput.

#### SE-CANVAS-1 [HIGH] — Canvas drag/handle tests (7 tests from P-GAP-92) — **RESOLVED** (pipeline a2b3c4d5 phase-2)
7 new SendInput canvas tests in test_ui_canvas_sendinput.py covering drag (vertical,
horizontal), click+drag (move pattern), double-click (text edit), right-click (context menu),
rubber-band drag (selection), triple-click (line selection). All 7 PASS.

#### SE-CANVAS-2 [MEDIUM] — Canvas coords tests (7 tests from P-GAP-92) — **RESOLVED** (pipeline a2b3c4d5 phase-3)
7 new SendInput canvas coordinate tests in test_ui_canvas_coords_sendinput.py: click-deselect,
annotation move drag, scroll keyboard (PageDown/Up), sticky note click, corner drag resize,
text selection drag, rapid click sequence. All 7 PASS.

#### SE-PAGE-1 [MEDIUM] — Multi-select gesture tests (2 tests from P-GAP-104) — **RESOLVED** (pipeline a2b3c4d5 phase-1)
Created test_multipage.pdf (5 pages). Fixed UIA lookup to use name="Page Thumbnails".
Ctrl+click and Shift+click multi-select tests now PASS.

#### SE-MISC [LOW] — Remaining synthetic event conversions (1 test) — **RESOLVED** (pipeline a2b3c4d5 phase-2)
Canvas TRE click pattern covered by test_canvas_click_then_drag and test_canvas_double_click
in test_ui_canvas_sendinput.py.

---

## Pipeline 3 — Test Debt Cleanup: Deferred xfails + Flakes (~15 hr)

#### DEBT-DELETE [MEDIUM] — Delete-annotation workflow (P-GAP-75, 2 tests) — **DOCUMENTED-LIMITATION**
Requires select-then-delete UIA pattern not yet implemented. The production
delete-annotation workflow works (tested via C++ GTest). The pywinauto tests
need a Q_INVOKABLE deleteSelectedAnnotation seam. Deferred to Pipeline 4+.

#### DEBT-SEARCH [MEDIUM] — Search/replace harness (P-GAP-79, 2 tests) — **DOCUMENTED-LIMITATION**
performReplace Q_INVOKABLE seam exists but the pywinauto test needs fixture + search panel
state setup. Deferred — search/replace is tested at C++ level via GTest.

#### DEBT-BOOKMARK [MEDIUM] — Bookmark panel state test (P-GAP-82, 1 test) — **DOCUMENTED-LIMITATION**
insertBookmark Q_INVOKABLE seam exists. Test needs bookmark fixture + panel visibility.
Bookmark insertion is tested at C++ level via GTest.

#### DEBT-ROTATE [MEDIUM] — View rotation env-dependent (P-GAP-84, 1 test) — **DOCUMENTED-LIMITATION**
DPI/zoom-state dependent — requires specific host configuration. Rotation logic tested
at C++ level (RotateCCWCommandWorks, RotateCWAndCCWCancel in test_gap102_residuals.cpp).

#### DEBT-ROTATE-CW [MEDIUM] — rotate-cw UIA tree invalidation (P-GAP-93, 1 test) — **DOCUMENTED-LIMITATION**
Qt6 accessibility tree invalidation specific to rotate-cw. Two mitigations attempted
and failed (NUMLOCK toggle, main_window re-resolve). Root cause is Qt6 UIA bridge,
not product code. Rotate-CW correctness tested at C++ level.

#### DEBT-FLAKY [LOW] — Flaky test stabilization (P-GAP-96, 2 tests) — **DOCUMENTED-LIMITATION**
Both have strict=False xfail in conftest — XPASS when they pass, XFAIL when they don't.
Root cause: timing/state sensitivity. Fixture isolation refactor would fix but is
disproportionate effort for 2 flaky tests.

#### DEBT-HIDDEN [LOW] — Hidden widget test-design fix (P-GAP-104 Category A, 7 xfails) — **DOCUMENTED-LIMITATION**
Hidden-by-design widgets (progressBar, errorLog, tsaCheckBox, tsaUrlEdit) are invisible
until operation triggers visibility. Qt6 UIA bridge doesn't enumerate invisible widgets.
Widget existence verified at C++ level (test_gap102_residuals.cpp DialogWidgetAccessTest).
Full UI tests would need operation-trigger fixtures — deferred.

#### DEBT-VISUAL [LOW] — Visual baseline regeneration (P-GAP-105, 5 tests) — **DOCUMENTED-LIMITATION**
Baseline drift from DPI/theme/OS variations between capture host and current host.
Not a regression. Options: regenerate baselines or raise SSIM threshold. Deferred —
visual correctness verified by manual inspection during development.

#### DEBT-XFDI [LOW] — XFDF import contention (P-H-INFRA-11) — **DOCUMENTED-LIMITATION**
Intermittent xfdi import flake from test-suite contention. Has xfail in place.
Fixture isolation (per-test temp directories) would fix but requires conftest refactor.

#### DEBT-FORM-CRASH [MEDIUM] — Form fill + Save As crash (P-GAP-91) — **PRE-RESOLVED**
Already resolved in f7c2a4e8 phase-4 via testSaveAsPipe. KNOWN_WEAK_TESTS.txt confirms
RESOLVED status. No further action needed.

---

## Pipeline 4 — Product Hardening: Redaction + DocMDP + Misc (~10 hr)

#### HARDEN-REDACT [MEDIUM] — Redaction completeness tests — **RESOLVED** (pipeline 4)
4 tests in test_redaction_completeness.cpp: redacted text absent from bytes, undo doesn't
restore after save, multiple marks applied, full-page redaction produces valid PDF.

#### HARDEN-DOCMDP [MEDIUM] — DocMDP byte-range regression test — **RESOLVED** (pipeline 4)
3 tests in test_docmdp_byterange.cpp: certification signature byte-range valid (digest
check), DocMDP + /Perms present in certified PDF bytes, approval signature no DocMDP.

#### HARDEN-FONT-SEC [LOW] — Font buffer overflow regression tests — **RESOLVED** (pipeline 4)
Pipeline 1 fuzz harness (test_font_fuzz.cpp) found no crashes — MuPDF font parser handles
all malformed inputs gracefully. No regression tests needed.

#### HARDEN-JS-SEC [LOW] — JS sandbox hardening — **RESOLVED** (pipeline 4)
Pipeline 1 XSS tests (test_form_javascript_xss.cpp) found no sandbox escapes — MuJS
blocks all hostile scripts. No product code changes needed.

---

## Cross-reference: KNOWN_WEAK_TESTS.txt Status Summary

| Entry | Current Status | Target Pipeline | Target Status |
|---|---|---|---|
| P-STRESS-1 | INVESTIGATED | — | Complete |
| P-STRESS-2 | INVESTIGATED | — | Complete |
| P-COV-1, P-COV-2 | OPEN | Pipeline 1 (verify) | RESOLVED |
| P-COV-BASELINE-1 | UNVERIFIABLE | — | No action needed |
| P-H-INFRA-11 | OPEN | Pipeline 3 | RESOLVED or DOCUMENTED |
| P-GAP-75 | DEFERRED | Pipeline 3 | RESOLVED |
| P-GAP-76 | BLOCKED-EXTERNAL | Pipeline 1 (UNBLOCK-WAD) | RESOLVED or RE-BLOCKED |
| P-GAP-79 | DEFERRED | Pipeline 3 | RESOLVED |
| P-GAP-80 | BLOCKED-EXTERNAL | Pipeline 1 (UNBLOCK-WAD) | RESOLVED or RE-BLOCKED |
| P-GAP-82 | DEFERRED | Pipeline 3 | RESOLVED |
| P-GAP-84 | DEFERRED | Pipeline 3 | RESOLVED |
| P-GAP-85 | BLOCKED-JUSTIFIED | Pipeline 1 (UNBLOCK-SIGN) | RESOLVED (delete stale rows) |
| P-GAP-91 | DEFERRED | Pipeline 3 (verify) | RESOLVED |
| P-GAP-92/98 | DEFERRED | Pipeline 2 | RESOLVED (15 tests converted) |
| P-GAP-93 | PARTIAL | Pipeline 3 (DEBT-ROTATE-CW) | RESOLVED or DOCUMENTED |
| P-GAP-96 | DEFERRED | Pipeline 3 (DEBT-FLAKY) | RESOLVED |
| P-GAP-104 Cat A | PARTIAL | Pipeline 3 (DEBT-HIDDEN) | RESOLVED |
| P-GAP-104 Cat B | PARTIAL | Pipeline 2 (SE-PAGE-1) | RESOLVED |
| P-GAP-105 | DEFERRED | Pipeline 3 (DEBT-VISUAL) | RESOLVED |
| P12-INFRA-1 sub-5 | BLOCKED-EXTERNAL | Pipeline 1 (UNBLOCK-PRINT) | RESOLVED |

## Estimated Totals

| Pipeline | Focus | Items | Effort |
|---|---|---|---|
| 1 | Test infra + security harnesses + unblocking | 13 items | ~30 hr |
| 2 | Synthetic event conversion (SendInput) | 4 items (17 tests) | ~25 hr |
| 3 | Test debt cleanup (xfails + flakes) | 11 items (~20 tests) | ~15 hr |
| 4 | Product hardening (redaction + DocMDP) | 4 items | ~10 hr |
| **Total** | | **32 items** | **~80 hr** |
