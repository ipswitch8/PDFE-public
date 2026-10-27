# PDFE Quality Gaps — Remediation Plan

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


**Source:** Audit on 2026-05-29 against three claims that proved false:
1. "All features tested E2E like an end user would use them"
2. "All known attack methods verified to fail"
3. "An end user can use any and all features without problems"

**Baseline:** 3829 C++ PASSED / 0 FAILED / 0 SKIPPED. ~38 Python UI test files.

**Organization:** Four pipelines by gap category and dependency order.
Estimated total effort: ~180-240 hours.

---

## Pipeline 1 — E2E User-Equivalent Coverage (~70 hr)

### 1A. Menu Navigation Flows (full traversal)

#### MENU-FULL-WALK [HIGH] — Every menu action exercised via UIA
Walk the entire menu tree (File, Edit, View, Pages, Document, Forms,
Tools, Security, Window, Help). For each action:
1. Click menu via UIA (not Q_INVOKABLE bypass)
2. Verify enabled/disabled state matches document context
3. If action opens dialog: dialog appears, has expected widgets, cancels cleanly
4. If action mutates state: verify dirty marker + undo availability
**Approach:** Generate test from MainWindow's menu list at startup; iterate.
**Effort:** L (12-16 hr)

#### KEYBOARD-NAV-COMPLETE [MEDIUM] — Tab order + mnemonics + shortcuts
Every dialog, every panel: Tab through all controls in expected order.
Verify Alt+mnemonic activates labeled control. Verify accelerator keys
(Ctrl+S, Ctrl+P, F5, etc.) work without focusing the menu.
**Approach:** pywinauto type_keys with deterministic focus verification.
**Effort:** M (6-8 hr)

### 1B. Multi-Document Workflows

#### MULTI-DOC-LIFECYCLE [HIGH] — Concurrent document state
- Open A, edit, open B in same window — verify A state preserved
- Open A, open B in new window — verify independent state
- Edit A, switch to B, edit B, save B, switch to A, save A — order independence
- Close A while B is in-progress save — no cross-document corruption
**Effort:** M (4-6 hr)

#### LONG-SESSION-STABILITY [MEDIUM] — Memory/leak under multi-hour load
Run a scripted session: open 50 PDFs sequentially, perform 10 ops on each,
measure RSS growth between iterations. Goal: bounded steady-state memory.
**Approach:** pywinauto with psutil snapshots; assert < 100MB growth over
100 doc-open/close cycles.
**Effort:** M (4 hr)

### 1C. Real Dialog Flows (not bypassed)

#### NATIVE-FILE-DIALOG [MEDIUM] — Open/Save dialogs via UIA
Replace testSaveAsPipe / testOpenPipe shortcuts with actual driving of
the Windows common file dialog: navigate folders, type filename, click
buttons. Pywinauto + Windows dialog automation.
**Effort:** M (6 hr)

#### PRINT-DIALOG-FULL [LOW] — Real printer dialog
Drive native print dialog: select printer, set paper size, click Print.
Validate output goes to "Microsoft Print to PDF" target file.
**Effort:** M (4 hr)

### 1D. Rendering Fidelity

#### RENDER-FIDELITY-DIFF [HIGH] — Pixel diff vs Acrobat reference
For a curated set of 20 reference PDFs (text, vector, image, mixed),
render each in PDFE and in Acrobat Reader (via headless invocation),
compare via SSIM + perceptual hash. Threshold: SSIM > 0.92.
**Effort:** L (10-12 hr — setup + corpus + harness)

#### MULTI-DPI-RENDERING [MEDIUM] — Scale factor correctness
Test render at 96, 120, 144, 192 DPI. Verify text doesn't blur,
images scale cleanly, dialog widgets remain readable.
**Approach:** Modify QApplication scale factor via env var; screenshot
comparison.
**Effort:** M (4 hr)

#### DARK-MODE-RENDER [LOW] — Theme switching
Toggle dark mode; verify dialogs, panels, canvas all repaint correctly.
**Effort:** S (2 hr)

### 1E. Accessibility With Real AT

#### SCREEN-READER-VERIFY [MEDIUM] — NVDA announcements
Drive PDFE with NVDA running; capture NVDA speech log; verify menu
items, dialog labels, and PDF content are announced correctly.
**Approach:** NVDA has a remote API + speech viewer; assert against
expected announcements.
**Effort:** L (8-10 hr — NVDA setup + speech capture harness)

#### WCAG-COMPLIANCE-AUDIT [MEDIUM] — Run axe-core equivalent
Apply WCAG 2.1 AA rules to every dialog and panel. Document violations.
**Effort:** M (6 hr)

---

## Pipeline 2 — Security Hardening (~70 hr)

### 2A. Continuous Fuzzing Infrastructure

#### ASAN-UBSAN-BUILD [HIGH] — Sanitizer-enabled CI build
Add a Linux CI job that builds PDFE with -fsanitize=address,undefined
on every PR. Run full test suite + fuzz corpus under sanitizers.
**Why Linux:** MSVC ASan has limited PDF/MuPDF coverage; clang on Linux
catches more.
**Effort:** L (8-10 hr — CMake preset + CI yaml + 1-day shakedown)

#### LIBFUZZER-CONTINUOUS [HIGH] — Multi-hour fuzz campaigns
Wire up existing fuzz harnesses (fuzz_document_open, fuzz_page_render,
fuzz_text_extract, fuzz_annotation_walk, fuzz_form_parse,
fuzz_embedded_files, fuzz_encryption_dict, test_pkcs7_fuzz,
test_font_fuzz) as libFuzzer targets. Run nightly for 4+ hours each.
Coverage report via libfuzzer-dataflow.
**Effort:** L (12-14 hr — harness conversion + CI integration + corpus)

#### COVERAGE-GUIDED-MERGE [MEDIUM] — Corpus minimization
After fuzz campaigns, merge new inputs into seed corpus, deduplicate
by coverage. Commit minimized corpus to repo.
**Effort:** S (3 hr — one-time tool wiring)

### 2B. CVE Replay Corpus

#### CVE-REPLAY-CORPUS [HIGH] — Reproduce known PDF CVEs
Build a corpus of public PoC files for relevant CVEs:
- CVE-2017-12944 (Type3 procedure)
- CVE-2018-4993 (silent NTLM credential leak via GoToR)
- CVE-2018-1000168 (PDF.js DoS)
- CVE-2020-15920 (Foxit JBIG2)
- CVE-2024-25088 (poppler infinite loop)
- ... etc. (target: 30-50 CVEs)
For each: verify PDFE handles input safely (no crash, no exfiltration).
**Effort:** L (16-20 hr — CVE research + PoC collection + per-CVE assertion)

### 2C. Side-Channel Testing

#### TIMING-SIDE-CHANNEL [LOW] — Password check timing
Verify wrong-password rejection takes constant time regardless of how
close the wrong password is to the correct one (no early-exit on first
byte mismatch).
**Effort:** S (3 hr)

#### ERROR-MESSAGE-DISCLOSURE [MEDIUM] — Exception messages safe
Audit all exception messages thrown by PDFE. Ensure none disclose
internal paths, memory addresses, full stack traces to users (only to
debug logs).
**Effort:** M (4 hr — grep all throws + manual review)

### 2D. Network Sandbox Verification

#### NETWORK-SNIFFER-INTEGRATION [MEDIUM] — Zero outbound during PDF ops
Run PDFE through a Windows packet capture (npcap/Wireshark CLI).
Open the 70-PDF malicious corpus + 50 normal PDFs. Assert zero
outbound HTTP/HTTPS/DNS traffic except to user-approved hosts.
**Effort:** M (6 hr — capture setup + per-test assertion)

### 2E. Persistence Damage

#### CONFIG-CORRUPTION-RESISTANCE [LOW] — Malicious doc → next launch clean
After processing each malicious PDF: kill PDFE, relaunch with fresh
profile. Verify config/cache/recent-files don't show damage from the
prior session.
**Effort:** S (3 hr)

### 2F. Concurrent/Race Testing

#### PARALLEL-RENDER-MALICIOUS [LOW] — Multi-doc concurrent malice
Open 5 different malicious PDFs simultaneously in 5 PDFE windows.
Verify no inter-process state leakage, no shared-memory corruption.
**Effort:** M (4 hr)

---

## Pipeline 3 — Real-World Compatibility (~60 hr)

### 3A. Reference Document Corpus

#### REAL-WORLD-CORPUS [HIGH] — 50 diverse production PDFs
Assemble a corpus from:
- IRS forms (W-2, 1040, W-9 — interactive AcroForm + JS)
- Government PDFs (USCIS, DMV — often quirky XFA fallback)
- Legal briefs (Bluebook citations, footnote-heavy)
- Scientific papers (LaTeX-generated, equations, vector figures)
- Corporate reports (multi-section, TOC, bookmarks, embedded fonts)
- Scanned documents (OCR'd 100+ pages)
- Acrobat-saved documents (real reader output)
- Word-exported (Office quirks)
- InDesign-exported (typography heavy)
- Mobile-captured (rotated, low DPI)

For each: open → render → save → diff round-tripped vs original at
pixel level + extracted text level.
**Effort:** L (16-20 hr — corpus acquisition + automation)

### 3B. Form Filling Interop

#### REAL-FORM-FILL [HIGH] — IRS/Gov forms end-to-end
Pick 10 real interactive forms. For each: open in PDFE, fill all fields
(tab navigation), save filled, reopen in Acrobat, verify all fields
preserved and JavaScript calculations match.
**Effort:** L (10-12 hr)

#### XFA-FALLBACK [MEDIUM] — XFA PDFs render usable view
Some gov PDFs are XFA-only with no static fallback. Verify PDFE either
(a) renders the static fallback if present, or (b) shows a clear error
message ("XFA not supported, view in Acrobat").
**Effort:** M (4 hr)

### 3C. Signature Interop

#### ADOBE-TRUST-STORE [HIGH] — Verify Acrobat-signed PDFs
Acrobat ships its own trust store (Adobe AATL). Test signed PDFs from
Acrobat: PDFE should validate the chain against Windows or AATL.
**Effort:** M (6 hr — fixture + trust store integration test)

#### WINDOWS-CERT-STORE [MEDIUM] — Sign using Windows-installed cert
User installs a code-signing cert in Windows. PDFE should detect it
and offer it as a signing option (currently only PFX file path).
**Effort:** L (10 hr — product change + tests)

#### TIMESTAMP-AUTHORITY [MEDIUM] — Real TSA endpoint
Sign with a real public TSA (e.g., DigiCert, GlobalSign). Verify
timestamp embedded correctly and validates round-trip.
**Effort:** S (3 hr — fixture + assertion)

### 3D. Print Output Verification

#### PRINT-TO-PDF-DIFF [MEDIUM] — Output matches render
Print test corpus to "Microsoft Print to PDF". Compare output PDF
visually against PDFE's render. Threshold: SSIM > 0.95.
**Effort:** M (6 hr)

### 3E. Localization

#### LOCALE-RENDERING [MEDIUM] — Non-English locales
Run PDFE under LANG=de_DE.UTF-8, ja_JP.UTF-8, ar_SA.UTF-8 (RTL),
zh_CN.UTF-8 (CJK). Verify:
- Menus translate (if i18n strings exist)
- PDF content with non-ASCII text renders correctly
- RTL languages flow right-to-left
- CJK fonts substitute correctly
**Effort:** L (8-10 hr)

#### UNICODE-PATH-FILENAMES [MEDIUM] — Non-ASCII paths
Open/save PDFs at paths like `C:\users\田中\文档\test.pdf` and
`/home/user/日本語.pdf`. Verify file operations work, dialog displays
unicode correctly.
**Effort:** S (3 hr)

---

## Pipeline 4 — Performance & Stress (~40 hr)

### 4A. Realistic Performance

#### LARGE-DOC-RESPONSIVENESS [HIGH] — 500-page corporate report
Open a 500-page PDF, scroll to page 250, zoom in/out 5x, search for a
string, navigate via bookmarks. Each operation should complete in < 2
seconds.
**Effort:** M (6 hr — corpus + harness)

#### SCANNED-1000PAGE-OCR [MEDIUM] — Bulk OCR throughput
Run TRE OCR on a 1000-page scanned archive. Measure throughput
(pages/min) + memory ceiling. Goal: complete without OOM on 8GB RAM.
**Effort:** M (6 hr)

#### COMPLEX-TYPOGRAPHY [MEDIUM] — Math-heavy PDFs
LaTeX-generated PDFs with embedded fonts, equation arrays, and
complex layouts. Verify rendering correctness pixel-diff vs reference.
**Effort:** M (4 hr)

### 4B. Memory Profiling

#### LEAK-DETECTION-LONG-SESSION [HIGH] — Multi-hour leak surveillance
Run PDFE for 4+ hours doing scripted operations (open, edit, save,
close, repeat). Use Application Verifier + UMDH to capture memory
snapshots every 30 min. Assert RSS growth < 50 MB over the session.
**Effort:** M (6 hr — tooling + scripted session)

#### HANDLE-LEAK-AUDIT [MEDIUM] — GDI/USER handle leaks
Same long session; track handle count via process info. Assert handle
count returns to baseline after each close cycle.
**Effort:** S (3 hr)

### 4C. Stress / DoS Resilience

#### ZIP-BOMB-RESILIENCE [MEDIUM] — Decompression bomb time/memory
Build PDFs with extreme compression ratios (1KB compresses to 1GB+).
Assert PDFE rejects or bounds decompression at < 200 MB / < 30 sec.
**Effort:** M (4 hr)

#### LARGE-ANNOTATION-COUNT [LOW] — 10K annotations on one page
Open a PDF with 10,000 annotations. Verify panel scrolls, selection
works, save completes in bounded time.
**Effort:** S (3 hr)

#### DEEPLY-NESTED-FORMS [LOW] — 100-level form field hierarchy
Form with 100 nested groups. Verify rendering, navigation, save.
**Effort:** S (2 hr)

### 4D. Multi-Monitor / High-DPI

#### MULTI-MONITOR-DRAG [MEDIUM] — Window drag between monitors
Drag PDFE window from a 96-DPI monitor to a 192-DPI monitor.
Verify scaling adjusts correctly, no widget overflow, dialogs follow
parent window.
**Effort:** M (4 hr — needs multi-monitor rig or VM with virtual displays)

---

## Cross-Cutting: Tooling & Process

### TOOL-1 [MEDIUM] — Sanitizer build preset
CMake preset `sanitizer-vs2022-clang` (or `sanitizer-linux-clang`)
with ASan + UBSan + leak detection. Wire to CI matrix.

### TOOL-2 [MEDIUM] — Coverage measurement
gcov/llvm-cov integration. Track coverage % per module. Goal:
- Core: > 85%
- UI: > 70%
- Security: > 90% (already close)

### TOOL-3 [LOW] — Test categorization tags
GTest labels: `smoke`, `slow`, `flaky`, `security`, `e2e`. Allow
selective runs (e.g., CI runs smoke + security; nightly runs all).

### PROCESS-1 [HIGH] — Manual QA checklist before release
Living document of E2E flows that require human verification:
- Print to a real printer
- Sign with a real eToken
- Render against an external monitor at 4K
- Use with screen reader for 15 min
- Try keyboard-only navigation through every dialog

### PROCESS-2 [MEDIUM] — Security review cadence
Quarterly: threat model review, dependency CVE scan (vcpkg audit),
external penetration test once/year.

---

## Estimated Totals

| Pipeline | Focus | Items | Effort |
|---|---|---|---|
| **1** | E2E user-equivalent | 11 | ~70 hr |
| **2** | Security hardening | 11 | ~70 hr |
| **3** | Real-world compatibility | 11 | ~60 hr |
| **4** | Performance & stress | 11 | ~40 hr |
| **Tooling** | Cross-cutting infra | 5 | ~20 hr |
| **Total** | | **49 items** | **~260 hr** |

## Priority Recommendation

If forced to pick a subset for the next 80 hours of work, do these first:
1. **REAL-WORLD-CORPUS** (16 hr) — biggest fidelity gap, foundation for
   other pipelines
2. **ASAN-UBSAN-BUILD + LIBFUZZER-CONTINUOUS** (22 hr) — true security
   posture, not just regression prevention
3. **CVE-REPLAY-CORPUS** (16 hr) — proves attack methods actually fail
4. **MENU-FULL-WALK** (14 hr) — closes biggest E2E coverage gap
5. **LEAK-DETECTION-LONG-SESSION** (6 hr) — catches production-critical
   stability issues
6. **PROCESS-1 manual QA checklist** (4 hr) — captures what automation
   can't, documents reality

Total: 78 hr. These six items would move the three claims from "false"
to "defensible for most product surface."
