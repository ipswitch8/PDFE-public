# PDFE Test-Harness — Flaws & Gaps Register

<!-- tracking-banner -->
> **STATUS: HISTORICAL EVIDENCE — this is not a work list.**
>
> **GitHub Issues (`ipswitch8/PDFE`) is the canonical tracker for anything still
> open.** This document records what an audit found on the day it ran and what
> was decided about it. Rows here may have been fixed, superseded, or
> deliberately declined since. Where a row and a GitHub issue disagree, **the
> issue wins**; where a row describes something still broken and no issue
> exists, file one.
>
> See [`docs/TRACKING.md`](../TRACKING.md) for why, and for the parts of this repo that are
> operationally live rather than historical.

---


Consolidated, severity-ranked register of the adversarial harness audit (phases 0-4). FIND-AND-DOCUMENT: no product/test code was changed. Each entry clusters related prior-phase findings by class; remediation is the separate gated program in `remediation-plan.json`.

**37 entries** — Critical 2, High 25, Med 8, Low 2. By class: coverage-gap 3, input-domain-gap 2, oracle-weakness 23, process 9.

Ranked entries (Critical → Low):

| ID | Sev | Class | Phase | Location | Description | Failure missed | Remediation |
|---|---|---|---|---|---|---|---|
| R-01 | Critical | oracle-weakness | P1 | security/CVE-replay & malware sandbox: tests/test_cve_replay.cpp:62, tests/test_malicious_ | The malformed-PDF / CVE-neutralization suites assert only that Document::open (and fireKeystroke/render) returns within a timeout and does not throw, wrapped in a catch-all try/catch that even hides an outright throw. 21 | Attacker exfiltrates NTLM credentials / executes embedded JavaScript / spawns a process on the victim opening a crafted PDF, with a fully green security suite. | Adopt a positive-neutralization oracle per attack class: assert no outbound socket/UNC connection attempt (mock transport spy), no process spawn (deny-list hook |
| R-02 | Critical | oracle-weakness | P1/P3 | redaction confidentiality: tests/test_redaction_completeness.cpp:67, tests/test_image_reda | The only oracles that prove the secret is gone from the saved decompressed stream GTEST_SKIP under fixture fallback; RedactionVerifier's own unit file constrains it with tautologies (EXPECT_TRUE(success\|\|!success)); ch | Redacted SSN / classified text survives in the saved stream or in an incremental-update shadow revision and is trivially recoverable; a data-disclosure defect s | Make the decompressed-stream byte-absence + extraction-absence oracle UNCONDITIONAL (own the fixture; no skip-on-fallback); add a RedactionVerifierFailPath test |
| R-03 | High | oracle-weakness | P1/P3 | 71 files across content-edit/TRE/fonts/image/alignment (dominant smell 'extraction-only-fo | The single most prevalent disease: a visual feature (glyph/image/layout) is verified only by extractPlainText().find(marker) or a non-null display list. fz_stext extraction reports the same codepoints whether the run ren | Edited/reconstructed text renders mirrored, invisible, white, half-size, or recolored while the suite is green because 'the page still has text'. | Oracle standard (SC-b): any test of a visual feature must render + assert a glyph-quad / pixel-ink property (upright, on-page, non-white, correct size band), no |
| R-04 | High | oracle-weakness | P1/P3 | content-stream editors: src/textedit/ContentStreamEditor.cpp; tests/test_content_stream_ed | Inline text-edit correctness (ambient CTM/Tz/Tc/Tw/Ts/Tr/color continuity, glyph identity, position, size, neighbour-span preservation) is unverified. MultiplePageEdits has ZERO assertions; the adversarial-ambient corpus | A user's edit lands the wrong glyph, at half width, mirrored, recolored, or clobbers the adjacent span; save drops/garbles the edit; all green. | Adversarial-ambient fixture corpus x ContentStreamEditor with pixel/quad oracles (SC-a); promote the SUCCEED-only measurement cells in test_content_stream_adver |
| R-05 | High | oracle-weakness | P1/P3 | alignment injector: src/reflow/TextReflowEngine.cpp; tests/test_alignment_injector.cpp:82, | Left/Center/Right/Justify is an inherently positional feature but every test asserts only extractPlainText().find(marker); the glyph x-offset that IS the alignment is never checked. The strong CTM-reset suite uses Left o | Text drawn left/off-page/invisible when Center or Right was requested; user sees mis-aligned document, suite green. | Add a glyph x-offset/geometry oracle (assert the injected block's left edge matches the requested alignment within tolerance) under the SC-a corpus. |
| R-06 | High | oracle-weakness | P1 | TRE reconstruction: tests/test_region_reconstructor.cpp:229, test_tre_coord_mapper.cpp:161 | TRE writes NEW glyphs into the content stream (visual) but nothing renders/re-extracts the produced PDF; meaningful checks sit inside if(result.success) and only test non-empty text + bbox>0. The 'critical' Y-flip coord- | User edits scanned/image-only text and the reconstructed glyphs are wrong/mirrored/off-page; the Y-flip regression the tests are named to guard cannot fail them | Render + re-extract the produced PDF with a glyph-quad oracle; rewrite the coord-mapper tests to call mapper_->pdfToPixel/pixelRectToPdf and assert against fixe |
| R-07 | High | oracle-weakness | P1 | image edit: tests/test_image_content_editor.cpp:63, test_image_flow_oracle.cpp:71, test_oc | Image insert/remove/resize/rotate/crop are visual but the only oracle is a return bool / non-null createDisplayList() / unchanged count. A no-op, a wrong CTM producing a mirrored or HALF-SIZE image (the exact a03f6b6 hal | User resizes/rotates/crops an image and gets a mirrored, half-size, or wrong-region result; suite green. | Pixel-at-known-coordinate + bounding-box geometry oracles for every CTM image op under the SC-a corpus (extend the image-edit injectors to the adversarial-ambie |
| R-08 | High | oracle-weakness | P3 | save/rewrite-routing: src/core/Document.cpp; tests/test_command_page_rebind.cpp:133, test_ | Entire save/rewrite subsystem had a 0% mutation kill-rate. Saved files are validated only by reopen + pageCount() (reopens-without-error), with no size/gc/perf/object-count oracle. Recovery-copy perf property (do_compres | Save silently drops an annotation, retains duplicated/stale objects (bloat + potential data retention), or a 30s autosave tick regresses from ~440ms to seconds  | Add an object-count/dedup oracle (assert gc=3 removes duplicated objects), a written-file size/perf budget oracle on the recovery path, and a saved-artifact con |
| R-09 | High | oracle-weakness | P1/P2 | autosave/crash-recovery: tests/test_autosave_capture_audit.cpp:187 (all 15 edit families) | The 15-family autosave capture audit is SUCCEED()-only with ZERO EXPECT_/ASSERT_; finishRow() records capture_present=NO as passing DATA. The exact crash-recovery data-loss class this project keeps shipping produces a gr | A user's edits are lost on crash because autosave dropped a whole edit family; suite green. | Convert finishRow()'s captured data into EXPECT_TRUE(capture_present) per family; treat capture_present=NO as a test failure, not recorded data. |
| R-10 | High | oracle-weakness | P1/P3 | fonts: src/fonts/GlyphScaleAdjuster.cpp; tests/test_glyph_scale_adjuster.cpp:102, test_gly | Glyph rasterization/synthesis/scaling/ranking are visual but oracles check only presence/non-null/format/size>0 or a wide bound (0.5,2.0). Transform recipes (b->d HFlip, 9->6 rotate) never inspect pixels; the ranker's co | User editing scanned text gets a serif substitute for a sans source, tofu/.notdef glyphs, or squeezed/overlapping substituted text; all green. | Pin golden per-string width ratios (kill M15); NCC/pixel-compare synthesized glyphs to a reference; assert a real serif-vs-sans discrimination margin (SameFont  |
| R-11 | High | oracle-weakness | P1/P3 | forms: src/forms/FormManager.cpp; tests/test_form_commands.cpp:77, test_metadata_manager.c | Field value/reset correctness is checked only against the in-memory FormManager/dict, never save+reopen; checkbox/radio /DV-restore on Reset Form has no positive oracle (reset coverage is text-field-only). | User fills a form / resets it and the values are lost or wrong on the saved+reopened file; a defaulted-on checkbox reverts to unchecked; all green. | Save+reopen every form/metadata assertion; add a checkbox/radio ResetFormRestoresDefaultValues case asserting /DV state after reset. |
| R-12 | High | oracle-weakness | P1 | signatures trust: tests/test_signature_roundtrip.cpp:425, test_certification_signatures.cp | Certificate trust-chain is never positively validated: every fixture is self-signed and MuPDF has no system trust store, so certificateStatus is always non-Okay and the trust tests assert only 'non-Okay OR !certValid'. D | User trusts a signature backed by an attacker cert / a modified certified document / a forged revocation status; all green. | Introduce a real trust anchor + a truly-trusted chained cert fixture; assert a trusted cert reports Okay AND an untrusted one reports non-Okay (positive+negativ |
| R-13 | High | oracle-weakness | P1 | signature tamper/revocation: tests/test_signature_security_phase2.cpp:310, test_ocsp_crl_v | Negative signature tests are weak or broken: the tamper test has an operator-precedence bug ('&b == 0') that may write a no-op so it may assert !isFullyValid on an UNMODIFIED doc; OCSP staleness/CRL-expiry enforcement co | Tampered / stale-revocation / un-timestamped signatures present to the user as valid; all green. | Fix the tamper mutation to deterministically flip a byte and assert detection; make freshness enforcement observable independent of trust state; assert the RFC- |
| R-14 | High | oracle-weakness | P1 | self-reimplementation: tests/test_cid_font_encoding.cpp:360, test_tre_coord_mapper.cpp:161 | A distinct class where the test copies the production algorithm (CMap scan loop, coord mapping, pixel-rect union, charRatio) into its own body and asserts the copy against itself, never invoking product code. Labelled as | The exact fixed bug regresses in production and the 'regression test' that claims to guard it cannot fail; zero coverage of the fix. | Oracle standard bans self-reimplementation: a regression test must call the production symbol under test; add a lint that flags algorithm duplication in test bo |
| R-15 | High | coverage-gap | P1 | skip-hidden-coverage (10 files): tests/test_image_text_edit_display.cpp:202 (41 skip sites | Files where the only real oracle is GTEST_SKIP-gated on fixture/tessdata presence, so on a realistic CI runner ~30-41 of 42 tests skip and the file reports GREEN with zero verification and no unconditional smoke assertio | Whole subsystems are silently unverified on the CI runner because their sole oracle skipped; suite green. | Own the fixtures (commit or generate them) so the strong oracle runs unconditionally; add an unconditional smoke assertion; fail the build if a P0 confidentiali |
| R-16 | High | oracle-weakness | P1 | search: tests/test_search.cpp:46, test_search_performance.cpp:56 | totalMatches() returns size_t/int hits.size(), so EXPECT_GE(...,0) is a tautology; quad/hit-structure checks live in for(hit:result.hits) loops that run zero times when the result is empty. A file named 'performance' ass | Search silently finds nothing / highlights nowhere and the suite is green. | Assert a known query on a known fixture returns EXACT expected hit count + non-empty quads; add a real timing budget to the performance file. |
| R-18 | High | oracle-weakness | P1 | assertion-free / SUCCEED-only class (42 files): e.g. test_text_edit_undo_chain.cpp:292, te | Whole test bodies end in SUCCEED() ('soft-asserting') or contain zero EXPECT_/ASSERT_, so they can only fail on a hard crash. The named regression (e.g. UI-TE-UNDO-01 formatting-run loss) cannot fail its own test. | Any behavioral regression that does not crash ships green in 42 files. | Oracle standard bans lone SUCCEED()/assertion-free bodies; every test must execute >=1 behavioral EXPECT/ASSERT (lint-enforceable). |
| R-19 | High | oracle-weakness | P1 | EXPECT_NO_THROW-only class (81 files) + tautological class (73 files) | 81 files whose dominant oracle is that a call did not throw, and 73 with logically-always-true asserts (EXPECT_TRUE(x\|\|!x), EXPECT_GE(unsigned,0), substrings that appear in timestamps/UUIDs). Both can never observe a w | Wrong verdicts, corrupted state, malformed output that does not throw ship green. | Oracle standard: no-throw is admissible only as a secondary check; each such test needs a value/state oracle. Flag lone-EXPECT_NO_THROW and tautological predica |
| R-20 | High | oracle-weakness | P1/P4 | widget-exists-only class (77 files); UI e.g. test_annotation_font_controls.cpp:129, test_b | UI tests assert an objectName/widget exists (or a boolean return) and place real checks inside if(widget && widget->isVisible()) or if(created) guards that skip silently when the control regresses. No pixel/icon/model or | A control paints the wrong icon/color, a dialog opens garbled, a bookmark insert no-ops -- all green. | Move assertions out of existence-guards (a missing control is a failure); add a visual/icon oracle for toolbar buttons (SC-g); assert model state, not the boole |
| R-21 | High | oracle-weakness | P1 | reopens-without-error-only class (74 files): e.g. test_optimize.cpp:175, test_command_page | Persistence tests validate a saved file only by reopen + pageCount()>0, never inspecting whether the intended change is present/absent in the written bytes. Metamorphic round-trips omit the != check (textAfter never asse | Saved file silently drops/leaks/no-ops the user's change; reopen still shows N pages; green. | Every persistence test must assert the change is present (or the leak is absent) in the reopened content, and metamorphic pairs must assert the transformed stat |
| R-24 | High | process | P4 | CI mutation gate: tools/gate/mutants.json (3 mutants), .github/workflows/mutation-kill.yml | The standing CI mutation manifest has 3 curated already-fixed mutants, so mutation-kill.yml can only report 100% -- green-by-construction. Phase-3's real 16-mutant run found 10 survivors (37.5% kill-rate) including R-02  | Every real surviving-mutant defect class is invisible to the standing gate; a false-green oracle regression merges with a 100% mutation report. | Replace the manifest with the phase-3 16-mutant set (esp. M11/M04/M05/M08/M09/M15/M13) and enforce a kill-rate floor; a survivor fails the build (SC-c). |
| R-25 | High | process | P4 | CI trigger posture: .github/workflows/windows-validation.yml:23-34, mutation-kill.yml, gov | As of 2026-07-20 ALL PDFE CI is nightly/workflow_dispatch only -- push/PR auto-triggers were removed to protect the single self-hosted runner. Every quality gate (validation, mutation-kill, governance, fuzz) is decoupled | Any regression in any class merges cleanly and is not caught until a manual/nightly run. | Re-enable per-commit/PR triggers for at least a fast lane (build + core + mutation-kill on the changed subsystem), with the full suite nightly; queue rather tha |
| R-26 | High | process | P4 | escaped-defect/DCA loop: docs/audit/dca/ (only 0001-*.md), governance.yml:51-58, tools che | The harness has been reactive since inception: of 72 mined bug-fixes, 70 escaped to a human before any test existed (oracle 41 / coverage 15 / input-domain 8 / process 6). The escaped-defect->DCA->class-test loop is impl | Each new escaped defect is patched instance-by-instance; the class re-escapes because no class-level test or DCA is required. | Make the DCA loop a mandatory merge gate: any bug-fix commit must add a DCA record + a class-level test, enforced per-commit not dispatch-only (SC-e). |
| R-27 | High | coverage-gap | P4 | UI permaskip wall: 43 UI test files, tests/ui_automation/*, gap_a14 UIA hang | ~472 of ~1018 UI skip sites are silent-zero-protection (gap_a14 UIA-hang on Server-2019), gating an estimated ~300 of ~450 UI test functions from running ANY behavioral assertion in CI. The visual oracles that could catc | The entire end-to-end UI layer runs zero behavioral assertions in CI; real-window regressions are invisible. | De-skip the UI visual oracles by routing menu/tool activation through the testToolActivatePipe (invoke_action) rather than UIA FindAll, and read results back vi |
| R-28 | High | process | P4 | UI matrix over-claim: tests/ui_automation/conftest.py:85,575; tests/_runtime/ui_test_matri | UI-matrix 'pass' is a runtime-generated CSV whose rows start as status=planned then flip green/red at runtime; the committed green reflects a one-off developer-workstation interactive run, not a reproducible CI result. R | A green UI matrix is presented as coverage while the backing tests skip in CI; over-claim of tested surface. | Live RTM enforcement: a matrix cell is 'pass' only if the owning test RAN and ASSERTED in the CI run that produced the CSV; forbid rebuilding the matrix from co |
| R-29 | High | oracle-weakness | P4 | visual oracles: tests/visual/diff.py, tests/ui_harness/core/golden.py, test_ui_annotations | PrintWindow/SSIM/pixel_diff/golden mechanisms exist but goldens self-seed (regen_golden), thresholds are relaxed (SSIM>=0.85), and the dominant assertion is a CHANGE-detector (SSIM<0.95 OR pixel_diff>=0.0001 == 'changed' | The exact shipped mirror/half-size/wrong-orientation render class passes the UI visual layer because 'it changed' is treated as 'it's correct'. | Commit correctness goldens produced by a trusted reference (not regen-from-current); assert against a known-correct baseline, not merely 'differs from before';  |
| R-35 | High | coverage-gap | P1/P2 | lifecycle/teardown: tests/test_save_throw_undo_safety.cpp:286 (nearest existing); no windo | There is no test that destroys the application/document while an edit or tool is active. The nearest coverage (save-that-throws undo rebind) is EXPECT_NO_THROW-only and only a hard crash on ASan fails it; on Debug/Releas | The app crashes (UAF) when the user closes the window while a text/annotation edit is active; no test catches it outside an ASan hard-crash. | Add a destruction-under-active-state test matrix (close window / destroy document while each edit family + tool is active) run under ASan, asserting clean teard |
| R-17 | Med | oracle-weakness | P1 | OCR: tests/test_ocr_pipeline.cpp:190 | OCR's user-visible truth is the recognized text; the oracle checks hocr non-empty + blocks>0. Gibberish recognition passes because no known word from the scanned fixture is asserted present; a DPI regression (RenderPageA | User OCRs a scan and gets unusable text while the suite is green. | Assert a known word/string from the fixture appears in the hOCR; pin the render-DPI to a tolerance band. |
| R-22 | Med | oracle-weakness | P1 | compliance/metadata: tests/test_prohibited_feature_scanner.cpp:39, test_xmp_metadata_write | PDF/A and compliance scanners are tested only against clean fixtures asserting the scanner reports NOTHING; a scanner that always returns false/empty (never detects encryption/JS/external-refs) passes every test. XMP pdf | Non-compliant / mislabelled PDF/A documents pass validation and are delivered as compliant; green. | Add positive-detection fixtures (a doc WITH encryption/JS/external-ref) and assert the scanner flags each; assert pdfaid part/conformance at the correct XML nod |
| R-23 | Med | oracle-weakness | P1 | optimize leak: tests/test_optimize.cpp:175 (OptimizeDoesNotLeakDeletedFormFieldValues) | Optimize (garbage=4) can leave stale/deleted form-field values or removed content embedded; the test reopens and asserts only pageCount>0, so the exact data-leak the test is named for ships green. (Related to R-08 gc-dow | A user 'removes' content and optimizes, but the content survives in the file; green. | Assert the deleted value's bytes are ABSENT from the decompressed optimized stream (a real byte-absence oracle, as with redaction). |
| R-30 | Med | process | P4/P6 | GAP-A15 exit-code masking: pdfe_tests.exe exit code vs JUnit XML; windows-validation.yml J | pdfe_tests.exe can exit 0 while the JUnit XML records failures (tell-tale: tests != passed+skipped). Now DEFENDED in CI (XML sum-of-failures parsed, not exit code) and honored in phase-3 mutation runs; registered as the  | If the JUnit-parse step is ever removed, a failing suite reports success on exit code alone. | Keep the JUnit sum-of-failures check as a required, non-removable CI step; add a self-test that a deliberately-failing sentinel fails the build (SC-h). |
| R-31 | Med | process | P4/P6 | gate governance: ~/.claude/hooks/phase-gate.sh:218-241; tools/gate/emit-gate-verdict.sh | The gate-reliability fix is REAL (verdicts derive from artifacts + parsed JUnit XML, no forgery allow-list). Residual: karen/validator/security-audit emit PASS with no independent evidence of their own; the hook only re- | A judgment gate signs off a phase that passes tests but does not meet acceptance criteria. | Require judgment gates to cite machine-checkable evidence (specific acceptance-criterion artifacts) in their verdict payload; keep the red-XML veto as the hard  |
| R-32 | Med | process | P4 | coverage floor: windows-validation.yml:346-374 (OpenCppCoverage->Cobertura) | Coverage is best-effort: continue-on-error:true, silently skipped when the tool is absent, gated by NO floor. It produces an artifact but can never fail the build, so coverage regressions are invisible. | Coverage silently erodes; the artifact exists but enforces nothing. | Enforce a per-subsystem coverage floor that fails the build on regression; make the tool's absence a hard error, not a silent skip (SC-d). |
| R-33 | Med | input-domain-gap | P2 | hostile-input-matrix zero classes + adversarial fixture backlog | 4 of 38 hostile-input classes have ZERO fixture coverage: sheared/skewed CTM in content-stream (the direct geometric sibling of the -100 Tz mirror bug this audit exists for), 0-page document, 10k-page scale boundary, com | Slanted/parallelogram edits on sheared-CTM pages, crash on 0-page docs, O(n^2) blowup at 10k pages, delete-half-a-grapheme corruption -- no red test today. | Add the phase-2 fixture backlog (aa_shear.pdf, aa_img_shear.pdf, rotated-edit, mediabox-offset, grapheme_clusters, zero_page, 10000page) and feed each through t |
| R-36 | Med | process | P4 | CI: .github/workflows/macos-validation.yml:118-120 | The macOS validation lane runs only PreflightOpen.*/PreflightXPlatform.* (a handful of isolation-boundary tests), not the full suite, so one of three advertised platforms provides almost no regression coverage. | Any macOS-only defect in the ~4360 tests outside the preflight subset is invisible on the macOS lane. | Run the full (or a representative core) suite on the macOS lane, or drop the platform's advertised support claim to match reality. |
| R-34 | Low | input-domain-gap | P2 | fixture catalog: 200 fixtures, 32 orphan, 1 missing-referenced; flaw_corpus (47) all -> on | Adversarial breadth exists but adversarial DEPTH per product-path is thin: 32 orphan fixtures are unreferenced by any test, and the 47-fixture flaw corpus all feed a single open-only smoke test (an open-parse pass is not | Adversarial inputs prove open() survives but never prove save/redact/render survive; orphan fixtures test nothing. | Wire orphan fixtures to a consumer test on their intended product path; extend the flaw corpus beyond open() to save/redact/render depth passes. |
| R-37 | Low | process | P4 | CI: census.json:451; .github/workflows/windows-validation.yml:291-304 | Rerun-on-failure exists but known-flaky tests (PerfStressTest.TenThousandAnnotationsOnePage, MainWindowIntegrationTest.ZoomComboExists) are tracked only in inline comments — there is no machine-readable quarantine/xfail  | A slowly-worsening test (eventually a real regression) is masked by reruns with no quarantine ledger to surface the trend. | Add a machine-readable flaky-quarantine/xfail registry with an entry+owner+expiry per known-flaky test; fail CI when an unregistered test flakes. |

## Full detail per entry

### R-01 [Critical] — oracle-weakness (P1)
- **Location:** security/CVE-replay & malware sandbox: tests/test_cve_replay.cpp:62, tests/test_malicious_pdf_advanced.cpp:651, tests/test_malicious_pdf_security.cpp:211
- **Description:** The malformed-PDF / CVE-neutralization suites assert only that Document::open (and fireKeystroke/render) returns within a timeout and does not throw, wrapped in a catch-all try/catch that even hides an outright throw. 21 replayed CVEs + SSRF/JS/process-spawn negatives share one timing assertion; no oracle asserts the attack was actually neutralized.
- **Exemplar escape:** A regression re-enabling CVE-2018-4993 (NTLM leak to attacker SMB share), CVE-2021-30560 (/AA JavaScript exec), CVE-2024-4367 (font PostScript), or /GoToR UNC SSRF ships GREEN.
- **Failure missed:** Attacker exfiltrates NTLM credentials / executes embedded JavaScript / spawns a process on the victim opening a crafted PDF, with a fully green security suite.
- **Evidence:** phase-1 ranked_weak_findings[0-1] (2 of the 2 Criticals); test_cve_replay.cpp:62 (EXPECT_NO_THROW + 5-10s timing); test_malicious_pdf_security.cpp:211 (GoToRRemotePathRejected/DataUriNotExecuted no-throw-only)
- **Remediation:** Adopt a positive-neutralization oracle per attack class: assert no outbound socket/UNC connection attempt (mock transport spy), no process spawn (deny-list hook that FAILs if invoked), no JS side-effect (canary), and remove the catch-all try/catch so a throw is a failure not a pass.

### R-02 [Critical] — oracle-weakness (P1/P3)
- **Location:** redaction confidentiality: tests/test_redaction_completeness.cpp:67, tests/test_image_redaction_phase7.cpp:428, tests/test_redaction_verifier.cpp:114, src/redaction/RedactionVerifier.cpp
- **Description:** The only oracles that prove the secret is gone from the saved decompressed stream GTEST_SKIP under fixture fallback; RedactionVerifier's own unit file constrains it with tautologies (EXPECT_TRUE(success||!success)); checkTextRemoved is only ever asserted TRUE, never FALSE on a real leftover. Phase-3 mutant M11 (checkSingleRevision -> always return true) SURVIVED 15 tests.
- **Exemplar escape:** Seed 8: RedactionVerifier full-containment oracle too weak. A redaction that paints a box but leaves the /Tj operators (or prior-revision recoverable content) intact ships GREEN.
- **Failure missed:** Redacted SSN / classified text survives in the saved stream or in an incremental-update shadow revision and is trivially recoverable; a data-disclosure defect ships with a green suite.
- **Evidence:** phase-3 M11-redaction-singlerev-always-clean SURVIVED (15 run/0 fail); phase-1 test_redaction_verifier.cpp:114, test_redaction_completeness.cpp:67 GTEST_SKIP@67-69, test_image_redaction_phase7.cpp:428 SKIP@428-430; phase-2 top_uncovered_capabilities[0]
- **Remediation:** Make the decompressed-stream byte-absence + extraction-absence oracle UNCONDITIONAL (own the fixture; no skip-on-fallback); add a RedactionVerifierFailPath test that asserts checkTextRemoved/checkSingleRevision return FALSE on a genuinely leaky multi-revision file; seed M11 into the standing mutation gate.

### R-03 [High] — oracle-weakness (P1/P3)
- **Location:** 71 files across content-edit/TRE/fonts/image/alignment (dominant smell 'extraction-only-for-visual')
- **Description:** The single most prevalent disease: a visual feature (glyph/image/layout) is verified only by extractPlainText().find(marker) or a non-null display list. fz_stext extraction reports the same codepoints whether the run renders upright, mirrored, invisible, white-on-white, wrong-size, off-page, or with clobbered neighbour spans.
- **Exemplar escape:** The shipped -100 Tz mirror / color-loss / half-size ambient-state-leak bug class (seeds 1 & 3). Phase-3 mutants M04 (Tz mirror in substitution), M06 (drop fill color), M07 (halve font size) all SURVIVED extraction-only suites.
- **Failure missed:** Edited/reconstructed text renders mirrored, invisible, white, half-size, or recolored while the suite is green because 'the page still has text'.
- **Evidence:** phase-1 totals.by_smell_files.extraction-only-for-visual=71; phase-3 M04/M06/M07 SURVIVED (7/7/12 run, 0 fail); test_content_stream_editor.cpp:172, test_text_edit_pipeline.cpp:129
- **Remediation:** Oracle standard (SC-b): any test of a visual feature must render + assert a glyph-quad / pixel-ink property (upright, on-page, non-white, correct size band), not extraction presence. Ban 'extraction-only-for-visual' as a lint-detectable smell.

### R-04 [High] — oracle-weakness (P1/P3)
- **Location:** content-stream editors: src/textedit/ContentStreamEditor.cpp; tests/test_content_stream_editor.cpp:172, test_text_edit_integration.cpp:236, test_text_edit_width_scaling.cpp:164, test_text_edit_save_roundtrip.cpp:242
- **Description:** Inline text-edit correctness (ambient CTM/Tz/Tc/Tw/Ts/Tr/color continuity, glyph identity, position, size, neighbour-span preservation) is unverified. MultiplePageEdits has ZERO assertions; the adversarial-ambient corpus cells are SUCCEED()-only; width-scaling recomputes charRatio in the test body and asserts it against itself.
- **Exemplar escape:** Seeds 1 & 3: state-continuity/mirror (extraction-blind, benign fixtures) and small-text-edit no-op (oracle asserted structure not the produced glyph).
- **Failure missed:** A user's edit lands the wrong glyph, at half width, mirrored, recolored, or clobbers the adjacent span; save drops/garbles the edit; all green.
- **Evidence:** phase-1 test_content_stream_editor.cpp:172, test_text_edit_integration.cpp:236 (zero asserts), test_text_edit_width_scaling.cpp:164 (tautological CharRatioLogic); phase-3 M04/M06/M07; phase-2 top_uncovered_capabilities[3]
- **Remediation:** Adversarial-ambient fixture corpus x ContentStreamEditor with pixel/quad oracles (SC-a); promote the SUCCEED-only measurement cells in test_content_stream_adversarial_ambient.cpp to ASSERTing tests.

### R-05 [High] — oracle-weakness (P1/P3)
- **Location:** alignment injector: src/reflow/TextReflowEngine.cpp; tests/test_alignment_injector.cpp:82, test_alignment_pipeline.cpp:154
- **Description:** Left/Center/Right/Justify is an inherently positional feature but every test asserts only extractPlainText().find(marker); the glyph x-offset that IS the alignment is never checked. The strong CTM-reset suite uses Left only, so it does not cover the center/right cells.
- **Exemplar escape:** Phase-3 mutant M05 (Center offset -> 0.0) rendered center-requested blocks left-aligned and SURVIVED 5 tests. Justify has a substring bug making fully un-justified output pass.
- **Failure missed:** Text drawn left/off-page/invisible when Center or Right was requested; user sees mis-aligned document, suite green.
- **Evidence:** phase-3 M05-statecont-alignment-offset-noop SURVIVED (5 run/0 fail); phase-1 test_alignment_injector.cpp:82, test_alignment_pipeline.cpp:154; phase-2 top_uncovered_capabilities[4]
- **Remediation:** Add a glyph x-offset/geometry oracle (assert the injected block's left edge matches the requested alignment within tolerance) under the SC-a corpus.

### R-06 [High] — oracle-weakness (P1)
- **Location:** TRE reconstruction: tests/test_region_reconstructor.cpp:229, test_tre_coord_mapper.cpp:161, test_tre_commands.cpp:257, test_content_stream_per_run_emission.cpp:229
- **Description:** TRE writes NEW glyphs into the content stream (visual) but nothing renders/re-extracts the produced PDF; meaningful checks sit inside if(result.success) and only test non-empty text + bbox>0. The 'critical' Y-flip coord-mapper tests recompute the mapping with the test's own arithmetic and never call the product mapper.
- **Exemplar escape:** A reconstructor writing wrong/garbage/mirrored/invisible text, or a product Y-flip that lands a scanned-text selection on the opposite end of the page, passes.
- **Failure missed:** User edits scanned/image-only text and the reconstructed glyphs are wrong/mirrored/off-page; the Y-flip regression the tests are named to guard cannot fail them.
- **Evidence:** phase-1 test_tre_coord_mapper.cpp:161 (tautological), test_region_reconstructor.cpp:229, test_tre_commands.cpp:257; phase-2 top_uncovered_capabilities[5]
- **Remediation:** Render + re-extract the produced PDF with a glyph-quad oracle; rewrite the coord-mapper tests to call mapper_->pdfToPixel/pixelRectToPdf and assert against fixed reference points.

### R-07 [High] — oracle-weakness (P1)
- **Location:** image edit: tests/test_image_content_editor.cpp:63, test_image_flow_oracle.cpp:71, test_ocr_routing_fix.cpp:314, test_image_replacer.cpp:171
- **Description:** Image insert/remove/resize/rotate/crop are visual but the only oracle is a return bool / non-null createDisplayList() / unchanged count. A no-op, a wrong CTM producing a mirrored or HALF-SIZE image (the exact a03f6b6 half-size-under-CTM bug class), or wrong-region crop is indistinguishable from correct.
- **Exemplar escape:** The recurring half-size / mirrored image-under-imposed-CTM defect; ImageCommandsDirtyTest's 15 cases all green on displayList!=nullptr.
- **Failure missed:** User resizes/rotates/crops an image and gets a mirrored, half-size, or wrong-region result; suite green.
- **Evidence:** phase-1 test_image_content_editor.cpp:63, test_image_flow_oracle.cpp:71, test_ocr_routing_fix.cpp:314; phase-2 top_uncovered_capabilities[6]; only image_ctm_leak/mediabox_origin_crop pin geometry
- **Remediation:** Pixel-at-known-coordinate + bounding-box geometry oracles for every CTM image op under the SC-a corpus (extend the image-edit injectors to the adversarial-ambient fixtures).

### R-08 [High] — oracle-weakness (P3)
- **Location:** save/rewrite-routing: src/core/Document.cpp; tests/test_command_page_rebind.cpp:133, test_optimize.cpp:175, AutosaveRecovery suite
- **Description:** Entire save/rewrite subsystem had a 0% mutation kill-rate. Saved files are validated only by reopen + pageCount() (reopens-without-error), with no size/gc/perf/object-count oracle. Recovery-copy perf property (do_compress=0, do_compress_images=0) and gc-escalation are unpinned.
- **Exemplar escape:** Phase-3 M08 (force image recompress), M09 (downgrade escalated gc 3->1, file bloat / stale-object retention), M16 (force stream recompress) all SURVIVED.
- **Failure missed:** Save silently drops an annotation, retains duplicated/stale objects (bloat + potential data retention), or a 30s autosave tick regresses from ~440ms to seconds on image-heavy scans; all green.
- **Evidence:** phase-3 save/rewrite-routing kill_rate=0.0 (M08/M09/M16 SURVIVED); phase-1 test_command_page_rebind.cpp:133, test_optimize.cpp:175
- **Remediation:** Add an object-count/dedup oracle (assert gc=3 removes duplicated objects), a written-file size/perf budget oracle on the recovery path, and a saved-artifact content oracle that asserts the created annotation/edit is present in the written bytes.

### R-09 [High] — oracle-weakness (P1/P2)
- **Location:** autosave/crash-recovery: tests/test_autosave_capture_audit.cpp:187 (all 15 edit families)
- **Description:** The 15-family autosave capture audit is SUCCEED()-only with ZERO EXPECT_/ASSERT_; finishRow() records capture_present=NO as passing DATA. The exact crash-recovery data-loss class this project keeps shipping produces a green suite.
- **Exemplar escape:** Autosave silently fails to capture an edit family on crash and every one of the 15 tests stays green.
- **Failure missed:** A user's edits are lost on crash because autosave dropped a whole edit family; suite green.
- **Evidence:** phase-1 test_autosave_capture_audit.cpp:187 (assertion-free); phase-2 top_uncovered_capabilities[7]
- **Remediation:** Convert finishRow()'s captured data into EXPECT_TRUE(capture_present) per family; treat capture_present=NO as a test failure, not recorded data.

### R-10 [High] — oracle-weakness (P1/P3)
- **Location:** fonts: src/fonts/GlyphScaleAdjuster.cpp; tests/test_glyph_scale_adjuster.cpp:102, test_glyph_synthesizer.cpp:195, test_vector_font_atlas_builder.cpp:106, test_type3_font_builder.cpp:332, test_font_fuzz.cpp:112, test_visual_font_ranker.cpp:121, test_cnn_font_classifier.cpp:170
- **Description:** Glyph rasterization/synthesis/scaling/ranking are visual but oracles check only presence/non-null/format/size>0 or a wide bound (0.5,2.0). Transform recipes (b->d HFlip, 9->6 rotate) never inspect pixels; the ranker's correctness check is wrapped in if(arialFirst){} so it self-neuters on the failure case; font fuzz is assertion-free.
- **Exemplar escape:** Phase-3 M15 (invert horizontalScale = substituteWidth/originalWidth) SURVIVED: substituted text renders too narrow/wide within the wide band.
- **Failure missed:** User editing scanned text gets a serif substitute for a sans source, tofu/.notdef glyphs, or squeezed/overlapping substituted text; all green.
- **Evidence:** phase-3 M15-fonts-glyph-scale-invert SURVIVED (15 run/0 fail); phase-1 test_glyph_scale_adjuster.cpp:102, test_glyph_synthesizer.cpp:195, test_visual_font_ranker.cpp:121, test_font_fuzz.cpp:112
- **Remediation:** Pin golden per-string width ratios (kill M15); NCC/pixel-compare synthesized glyphs to a reference; assert a real serif-vs-sans discrimination margin (SameFont - DifferentFont > delta); give font fuzz a rendered-glyph oracle.

### R-11 [High] — oracle-weakness (P1/P3)
- **Location:** forms: src/forms/FormManager.cpp; tests/test_form_commands.cpp:77, test_metadata_manager.cpp:124
- **Description:** Field value/reset correctness is checked only against the in-memory FormManager/dict, never save+reopen; checkbox/radio /DV-restore on Reset Form has no positive oracle (reset coverage is text-field-only).
- **Exemplar escape:** Phase-3 M13 (button reset hard-coded to /Off instead of /DV) SURVIVED 26 tests; a setFieldValue that updates the widget cache but never writes /V (value lost on save) passes.
- **Failure missed:** User fills a form / resets it and the values are lost or wrong on the saved+reopened file; a defaulted-on checkbox reverts to unchecked; all green.
- **Evidence:** phase-3 M13-forms-button-reset-to-off SURVIVED (26 run/0 fail); phase-1 test_form_commands.cpp:77, test_metadata_manager.cpp:124 (WriteMetadata_Roundtrip in-memory only)
- **Remediation:** Save+reopen every form/metadata assertion; add a checkbox/radio ResetFormRestoresDefaultValues case asserting /DV state after reset.

### R-12 [High] — oracle-weakness (P1)
- **Location:** signatures trust: tests/test_signature_roundtrip.cpp:425, test_certification_signatures.cpp:175, test_ltv_validator.cpp:342
- **Description:** Certificate trust-chain is never positively validated: every fixture is self-signed and MuPDF has no system trust store, so certificateStatus is always non-Okay and the trust tests assert only 'non-Okay OR !certValid'. DocMDP enforcement is only ever tested for ALLOWED changes; the forged-GOOD-untrusted-responder OCSP negative is missing.
- **Exemplar escape:** A signature from an untrusted/attacker-controlled issuer is indistinguishable from a trusted one; a disallowed edit under P1 NoChanges is never flagged; an injected self-signed GOOD OCSP is reported as validly revocation-checked.
- **Failure missed:** User trusts a signature backed by an attacker cert / a modified certified document / a forged revocation status; all green.
- **Evidence:** phase-1 test_signature_roundtrip.cpp:425, test_certification_signatures.cpp:175, test_ltv_validator.cpp:342; phase-2 top_uncovered_capabilities[2]
- **Remediation:** Introduce a real trust anchor + a truly-trusted chained cert fixture; assert a trusted cert reports Okay AND an untrusted one reports non-Okay (positive+negative); add a DocMDP-violation-flagged negative and a forged-GOOD-OCSP rejection.

### R-13 [High] — oracle-weakness (P1)
- **Location:** signature tamper/revocation: tests/test_signature_security_phase2.cpp:310, test_ocsp_crl_validation.cpp:279, test_tsa_real_endpoint.cpp:146
- **Description:** Negative signature tests are weak or broken: the tamper test has an operator-precedence bug ('&b == 0') that may write a no-op so it may assert !isFullyValid on an UNMODIFIED doc; OCSP staleness/CRL-expiry enforcement could be removed and the FIX-OCSP regression tests still pass (all paths collapse to UntrustedChain); the TSA token OID is printed, never asserted.
- **Exemplar escape:** A genuinely-broken verifier returning isFullyValid=true fails the tamper test only by luck of the target byte; a stale/expired revocation response is accepted as fresh; TSA timestamping is non-functional yet presents as timestamped.
- **Failure missed:** Tampered / stale-revocation / un-timestamped signatures present to the user as valid; all green.
- **Evidence:** phase-1 test_signature_security_phase2.cpp:310 (precedence bug), test_ocsp_crl_validation.cpp:279 (tautological UntrustedChain), test_tsa_real_endpoint.cpp:146 (print-only)
- **Remediation:** Fix the tamper mutation to deterministically flip a byte and assert detection; make freshness enforcement observable independent of trust state; assert the RFC-3161 token OID is embedded.

### R-14 [High] — oracle-weakness (P1)
- **Location:** self-reimplementation: tests/test_cid_font_encoding.cpp:360, test_tre_coord_mapper.cpp:161, test_image_text_edit_bugs.cpp:286, test_text_edit_width_scaling.cpp:164
- **Description:** A distinct class where the test copies the production algorithm (CMap scan loop, coord mapping, pixel-rect union, charRatio) into its own body and asserts the copy against itself, never invoking product code. Labelled as regression tests for named QA findings they cannot actually lock.
- **Exemplar escape:** Seed 3 small-text-edit no-op (charRatio); if the real CMap scanner still used unsigned short (the original infinite-loop bug) both 'regression' tests stay green.
- **Failure missed:** The exact fixed bug regresses in production and the 'regression test' that claims to guard it cannot fail; zero coverage of the fix.
- **Evidence:** phase-1 test_cid_font_encoding.cpp:360, test_tre_coord_mapper.cpp:161, test_image_text_edit_bugs.cpp:286
- **Remediation:** Oracle standard bans self-reimplementation: a regression test must call the production symbol under test; add a lint that flags algorithm duplication in test bodies.

### R-15 [High] — coverage-gap (P1)
- **Location:** skip-hidden-coverage (10 files): tests/test_image_text_edit_display.cpp:202 (41 skip sites), test_redaction_completeness.cpp:67, test_image_redaction_phase7.cpp:428
- **Description:** Files where the only real oracle is GTEST_SKIP-gated on fixture/tessdata presence, so on a realistic CI runner ~30-41 of 42 tests skip and the file reports GREEN with zero verification and no unconditional smoke assertion. The novel 'conditional-skip-on-only-real-oracle' pattern.
- **Exemplar escape:** A totally broken ImageReplacer or a redaction leak ships undetected when the fixture/tessdata is absent (the CI reality, per gap_a14).
- **Failure missed:** Whole subsystems are silently unverified on the CI runner because their sole oracle skipped; suite green.
- **Evidence:** phase-1 by_novel_smell_files.skip-hidden-coverage=10; test_image_text_edit_display.cpp:202, test_redaction_completeness.cpp:67, test_image_redaction_phase7.cpp:428
- **Remediation:** Own the fixtures (commit or generate them) so the strong oracle runs unconditionally; add an unconditional smoke assertion; fail the build if a P0 confidentiality oracle skips.

### R-16 [High] — oracle-weakness (P1)
- **Location:** search: tests/test_search.cpp:46, test_search_performance.cpp:56
- **Description:** totalMatches() returns size_t/int hits.size(), so EXPECT_GE(...,0) is a tautology; quad/hit-structure checks live in for(hit:result.hits) loops that run zero times when the result is empty. A file named 'performance' asserts no timing.
- **Exemplar escape:** Broken indexing/highlight-quads returning ZERO hits for text actually on the fixture is invisible; case-insensitive returning different sets for 'test' vs 'TEST' passes.
- **Failure missed:** Search silently finds nothing / highlights nowhere and the suite is green.
- **Evidence:** phase-1 test_search.cpp:46, test_search_performance.cpp:56; phase-2 top_uncovered_capabilities (full-text search)
- **Remediation:** Assert a known query on a known fixture returns EXACT expected hit count + non-empty quads; add a real timing budget to the performance file.

### R-18 [High] — oracle-weakness (P1)
- **Location:** assertion-free / SUCCEED-only class (42 files): e.g. test_text_edit_undo_chain.cpp:292, test_autosave_capture_audit.cpp:187, test_font_fuzz.cpp:112
- **Description:** Whole test bodies end in SUCCEED() ('soft-asserting') or contain zero EXPECT_/ASSERT_, so they can only fail on a hard crash. The named regression (e.g. UI-TE-UNDO-01 formatting-run loss) cannot fail its own test.
- **Exemplar escape:** Undo dropping formatting runs, autosave dropping edits, a font-parser rendering blank glyphs -- all green on non-crash.
- **Failure missed:** Any behavioral regression that does not crash ships green in 42 files.
- **Evidence:** phase-1 totals.by_smell_files.assertion-free=42; test_text_edit_undo_chain.cpp:292 (SUCCEED)
- **Remediation:** Oracle standard bans lone SUCCEED()/assertion-free bodies; every test must execute >=1 behavioral EXPECT/ASSERT (lint-enforceable).

### R-19 [High] — oracle-weakness (P1)
- **Location:** EXPECT_NO_THROW-only class (81 files) + tautological class (73 files)
- **Description:** 81 files whose dominant oracle is that a call did not throw, and 73 with logically-always-true asserts (EXPECT_TRUE(x||!x), EXPECT_GE(unsigned,0), substrings that appear in timestamps/UUIDs). Both can never observe a wrong-but-non-throwing result.
- **Exemplar escape:** test_save_throw_undo_safety (undo corrupts state, both clean-return and throw pass); test_xmp_metadata_writer (malformed pdfaid passes on substring match); test_font_compliance_checker (tautology).
- **Failure missed:** Wrong verdicts, corrupted state, malformed output that does not throw ship green.
- **Evidence:** phase-1 totals.by_smell_files.EXPECT_NO_THROW-only=81, tautological=73; test_save_throw_undo_safety.cpp:286, test_xmp_metadata_writer.cpp:50
- **Remediation:** Oracle standard: no-throw is admissible only as a secondary check; each such test needs a value/state oracle. Flag lone-EXPECT_NO_THROW and tautological predicates in review.

### R-20 [High] — oracle-weakness (P1/P4)
- **Location:** widget-exists-only class (77 files); UI e.g. test_annotation_font_controls.cpp:129, test_bookmark_panel_context_menu.cpp:169
- **Description:** UI tests assert an objectName/widget exists (or a boolean return) and place real checks inside if(widget && widget->isVisible()) or if(created) guards that skip silently when the control regresses. No pixel/icon/model oracle.
- **Exemplar escape:** Seed 4: wrong icons / missing form separators. A toolbar button renders the WRONG icon or a FreeText font-size spinner is never created and the test still reports GREEN with zero assertions executed (annotation toolbar has NO icons per UI-AN-22, undetectable).
- **Failure missed:** A control paints the wrong icon/color, a dialog opens garbled, a bookmark insert no-ops -- all green.
- **Evidence:** phase-1 totals.by_smell_files.widget-exists-only=77; phase-4 ui.invisible_regression_classes (wrong icon/garbled dialog); test_annotation_font_controls.cpp:129
- **Remediation:** Move assertions out of existence-guards (a missing control is a failure); add a visual/icon oracle for toolbar buttons (SC-g); assert model state, not the boolean return.

### R-21 [High] — oracle-weakness (P1)
- **Location:** reopens-without-error-only class (74 files): e.g. test_optimize.cpp:175, test_command_page_rebind.cpp:133, test_replace_page.cpp:66
- **Description:** Persistence tests validate a saved file only by reopen + pageCount()>0, never inspecting whether the intended change is present/absent in the written bytes. Metamorphic round-trips omit the != check (textAfter never asserted != textBefore), so a silent no-op round-trips trivially.
- **Exemplar escape:** OptimizeDoesNotLeakDeletedFormFieldValues (leak ships), ReplacePageCommandUndoRestoresOriginal (page-replace no-op ships), OrdinarySaveDrainsAndRebinds (dropped annotation ships).
- **Failure missed:** Saved file silently drops/leaks/no-ops the user's change; reopen still shows N pages; green.
- **Evidence:** phase-1 totals.by_smell_files.reopens-without-error-only=74; test_optimize.cpp:175, test_replace_page.cpp:66
- **Remediation:** Every persistence test must assert the change is present (or the leak is absent) in the reopened content, and metamorphic pairs must assert the transformed state differs from the original.

### R-24 [High] — process (P4)
- **Location:** CI mutation gate: tools/gate/mutants.json (3 mutants), .github/workflows/mutation-kill.yml
- **Description:** The standing CI mutation manifest has 3 curated already-fixed mutants, so mutation-kill.yml can only report 100% -- green-by-construction. Phase-3's real 16-mutant run found 10 survivors (37.5% kill-rate) including R-02 redaction confidentiality and R-05/R-08/R-10/R-11 defects, NONE represented in the manifest.
- **Exemplar escape:** Seeds 6/7 (fabrication/over-claim class): the gate meant to be the standing defense against false-green oracles exercises no known false-green oracle.
- **Failure missed:** Every real surviving-mutant defect class is invisible to the standing gate; a false-green oracle regression merges with a 100% mutation report.
- **Evidence:** phase-4 process 'Mutation gate is green-by-construction'; tools/gate/mutants.json (3); phase-3 mutation-report (16 mutants/10 survivors)
- **Remediation:** Replace the manifest with the phase-3 16-mutant set (esp. M11/M04/M05/M08/M09/M15/M13) and enforce a kill-rate floor; a survivor fails the build (SC-c).

### R-25 [High] — process (P4)
- **Location:** CI trigger posture: .github/workflows/windows-validation.yml:23-34, mutation-kill.yml, governance.yml (all workflow_dispatch/nightly)
- **Description:** As of 2026-07-20 ALL PDFE CI is nightly/workflow_dispatch only -- push/PR auto-triggers were removed to protect the single self-hosted runner. Every quality gate (validation, mutation-kill, governance, fuzz) is decoupled from the commit that would introduce a regression; nothing fails a bad commit at PR/merge time.
- **Exemplar escape:** A regression surfaces at best the next nightly, at worst only when a human dispatches -- the whole 'gates are mandatory' discipline is bypassed structurally.
- **Failure missed:** Any regression in any class merges cleanly and is not caught until a manual/nightly run.
- **Evidence:** phase-4 process 'CI trigger posture (meta-finding)'; windows-validation.yml:23-34; census.json:452
- **Remediation:** Re-enable per-commit/PR triggers for at least a fast lane (build + core + mutation-kill on the changed subsystem), with the full suite nightly; queue rather than drop on the single runner (SC-h).

### R-26 [High] — process (P4)
- **Location:** escaped-defect/DCA loop: docs/audit/dca/ (only 0001-*.md), governance.yml:51-58, tools check_dca.py/check_dca_presence.py
- **Description:** The harness has been reactive since inception: of 72 mined bug-fixes, 70 escaped to a human before any test existed (oracle 41 / coverage 15 / input-domain 8 / process 6). The escaped-defect->DCA->class-test loop is implemented but runs only inside workflow_dispatch-only governance.yml and only when a base ref is manually supplied.
- **Exemplar escape:** A defect fix can (and by the 1-record history does) ship with no DCA and no class-level test -- exactly the reactive posture the loop was meant to end.
- **Failure missed:** Each new escaped defect is patched instance-by-instance; the class re-escapes because no class-level test or DCA is required.
- **Evidence:** phase-4 process 'Escaped-defect / DCA loop not enforced'; docs/audit/README.md:34-35,51; only docs/audit/dca/0001-*.md exists
- **Remediation:** Make the DCA loop a mandatory merge gate: any bug-fix commit must add a DCA record + a class-level test, enforced per-commit not dispatch-only (SC-e).

### R-27 [High] — coverage-gap (P4)
- **Location:** UI permaskip wall: 43 UI test files, tests/ui_automation/*, gap_a14 UIA hang
- **Description:** ~472 of ~1018 UI skip sites are silent-zero-protection (gap_a14 UIA-hang on Server-2019), gating an estimated ~300 of ~450 UI test functions from running ANY behavioral assertion in CI. The visual oracles that could catch the real bug class sit behind this wall.
- **Exemplar escape:** Seed 4 wrong-icons and the shipped mirror/half-size content-stream bug class: the UI pixel oracles that would catch them never execute in CI.
- **Failure missed:** The entire end-to-end UI layer runs zero behavioral assertions in CI; real-window regressions are invisible.
- **Evidence:** phase-4 ui.silent_zero_protection=472, effectively_never_run_in_ci=472; gap_a14 (route menus via testToolActivatePipe)
- **Remediation:** De-skip the UI visual oracles by routing menu/tool activation through the testToolActivatePipe (invoke_action) rather than UIA FindAll, and read results back via the result pipe (SC-g).

### R-28 [High] — process (P4)
- **Location:** UI matrix over-claim: tests/ui_automation/conftest.py:85,575; tests/_runtime/ui_test_matrix_*.csv (973 CSVs)
- **Description:** UI-matrix 'pass' is a runtime-generated CSV whose rows start as status=planned then flip green/red at runtime; the committed green reflects a one-off developer-workstation interactive run, not a reproducible CI result. Row color is a fragile last-write-wins value, so a non-owning suite's SKIP can clobber a proven GREEN. 236 dialog rows are green on objectName-present-in-.cpp alone; 131 act_* rows green on pipe-fired-no-effect.
- **Exemplar escape:** Seed 7: UI-matrix over-claim (matrix integrity). A 'green' matrix reflects source presence + a one-off local run, not that the feature ran AND asserted in CI.
- **Failure missed:** A green UI matrix is presented as coverage while the backing tests skip in CI; over-claim of tested surface.
- **Evidence:** phase-4 ui.matrix_pass_is (runtime CSV, last-write-wins conftest.py:85/575); 236 dialog + 131 act_* over-claimed rows
- **Remediation:** Live RTM enforcement: a matrix cell is 'pass' only if the owning test RAN and ASSERTED in the CI run that produced the CSV; forbid rebuilding the matrix from committed green; make SKIP clobbering impossible (owner-only writes) (SC-f).

### R-29 [High] — oracle-weakness (P4)
- **Location:** visual oracles: tests/visual/diff.py, tests/ui_harness/core/golden.py, test_ui_annotations.py, test_ui_page_operations.py
- **Description:** PrintWindow/SSIM/pixel_diff/golden mechanisms exist but goldens self-seed (regen_golden), thresholds are relaxed (SSIM>=0.85), and the dominant assertion is a CHANGE-detector (SSIM<0.95 OR pixel_diff>=0.0001 == 'changed' == pass) rather than a correctness oracle.
- **Exemplar escape:** Canvas renders MIRRORED / wrong-zoom / half-size / tofu glyphs / wrong icon -- the change-detector sees a diff and PASSES; a self-seeded golden makes a plausible-looking wrong render indistinguishable from correct.
- **Failure missed:** The exact shipped mirror/half-size/wrong-orientation render class passes the UI visual layer because 'it changed' is treated as 'it's correct'.
- **Evidence:** phase-4 ui.visual_mechanisms + invisible_regression_classes (mirror/zoom/tofu/wrong-icon); self-seeding goldens, relaxed thresholds
- **Remediation:** Commit correctness goldens produced by a trusted reference (not regen-from-current); assert against a known-correct baseline, not merely 'differs from before'; tighten thresholds for orientation/scale-sensitive views (SC-g).

### R-35 [High] — coverage-gap (P1/P2)
- **Location:** lifecycle/teardown: tests/test_save_throw_undo_safety.cpp:286 (nearest existing); no window-close-mid-active-edit test
- **Description:** There is no test that destroys the application/document while an edit or tool is active. The nearest coverage (save-that-throws undo rebind) is EXPECT_NO_THROW-only and only a hard crash on ASan fails it; on Debug/Release a logic-corrupting rebind passes. No test drives real window-close during an in-flight edit.
- **Exemplar escape:** Seed 2: crash-on-close mid-edit (no destruction-under-active-state test). The shipped ~MainWindow mid-edit teardown 0xC0000005 UAF class.
- **Failure missed:** The app crashes (UAF) when the user closes the window while a text/annotation edit is active; no test catches it outside an ASan hard-crash.
- **Evidence:** phase-1 test_save_throw_undo_safety.cpp:286 (only-ASan-crash-fails); seed 2; memory pipeline_a3f8e21d (mid-edit teardown UAF)
- **Remediation:** Add a destruction-under-active-state test matrix (close window / destroy document while each edit family + tool is active) run under ASan, asserting clean teardown; partial today -- only the save-throw sub-case exists.

### R-17 [Med] — oracle-weakness (P1)
- **Location:** OCR: tests/test_ocr_pipeline.cpp:190
- **Description:** OCR's user-visible truth is the recognized text; the oracle checks hocr non-empty + blocks>0. Gibberish recognition passes because no known word from the scanned fixture is asserted present; a DPI regression (RenderPageAt300Dpi accepts >100px) silently degrades OCR input.
- **Exemplar escape:** OCR mis-recognizes the page into gibberish and passes.
- **Failure missed:** User OCRs a scan and gets unusable text while the suite is green.
- **Evidence:** phase-1 test_ocr_pipeline.cpp:190; phase-2 top_uncovered_capabilities (OCR recognition quality)
- **Remediation:** Assert a known word/string from the fixture appears in the hOCR; pin the render-DPI to a tolerance band.

### R-22 [Med] — oracle-weakness (P1)
- **Location:** compliance/metadata: tests/test_prohibited_feature_scanner.cpp:39, test_xmp_metadata_writer.cpp:50, test_font_compliance_checker.cpp:96
- **Description:** PDF/A and compliance scanners are tested only against clean fixtures asserting the scanner reports NOTHING; a scanner that always returns false/empty (never detects encryption/JS/external-refs) passes every test. XMP pdfaid part/conformance verified by substring-anywhere.
- **Exemplar escape:** A broken ProhibitedFeatureScanner silently passes non-compliant documents; a malformed pdfaid (part=3) passes.
- **Failure missed:** Non-compliant / mislabelled PDF/A documents pass validation and are delivered as compliant; green.
- **Evidence:** phase-1 test_prohibited_feature_scanner.cpp:39, test_xmp_metadata_writer.cpp:50, test_font_compliance_checker.cpp:96
- **Remediation:** Add positive-detection fixtures (a doc WITH encryption/JS/external-ref) and assert the scanner flags each; assert pdfaid part/conformance at the correct XML node, not substring-anywhere.

### R-23 [Med] — oracle-weakness (P1)
- **Location:** optimize leak: tests/test_optimize.cpp:175 (OptimizeDoesNotLeakDeletedFormFieldValues)
- **Description:** Optimize (garbage=4) can leave stale/deleted form-field values or removed content embedded; the test reopens and asserts only pageCount>0, so the exact data-leak the test is named for ships green. (Related to R-08 gc-downgrade M09.)
- **Exemplar escape:** Deleted form-field values remain recoverable in the optimized output.
- **Failure missed:** A user 'removes' content and optimizes, but the content survives in the file; green.
- **Evidence:** phase-1 test_optimize.cpp:175; phase-2 top_uncovered_capabilities (Optimize deleted-content leak); phase-3 M09
- **Remediation:** Assert the deleted value's bytes are ABSENT from the decompressed optimized stream (a real byte-absence oracle, as with redaction).

### R-30 [Med] — process (P4/P6)
- **Location:** GAP-A15 exit-code masking: pdfe_tests.exe exit code vs JUnit XML; windows-validation.yml JUnit parse
- **Description:** pdfe_tests.exe can exit 0 while the JUnit XML records failures (tell-tale: tests != passed+skipped). Now DEFENDED in CI (XML sum-of-failures parsed, not exit code) and honored in phase-3 mutation runs; registered as the residual standing requirement so the defense is never removed.
- **Exemplar escape:** Seed 5: GAP-A15 exit-code masking (CI integrity) -- the original escape where a green exit code hid failing tests.
- **Failure missed:** If the JUnit-parse step is ever removed, a failing suite reports success on exit code alone.
- **Evidence:** phase-4 process gap_a15_junit_parsed_in_ci=true; phase-3 test_runner_evidence.junit_xml_parsed_all=true; gtest_exit_code_masking memory
- **Remediation:** Keep the JUnit sum-of-failures check as a required, non-removable CI step; add a self-test that a deliberately-failing sentinel fails the build (SC-h).

### R-31 [Med] — process (P4/P6)
- **Location:** gate governance: ~/.claude/hooks/phase-gate.sh:218-241; tools/gate/emit-gate-verdict.sh
- **Description:** The gate-reliability fix is REAL (verdicts derive from artifacts + parsed JUnit XML, no forgery allow-list). Residual: karen/validator/security-audit emit PASS with no independent evidence of their own; the hook only re-checks the test-runner XML, so a judgment gate can falsely bless a phase whose tests are green but whose acceptance criteria were not truly met.
- **Exemplar escape:** Seed 6: gate fabrication (governance) -- now structurally prevented for test-backed verdicts; the residual honor-system exposure is the judgment gates.
- **Failure missed:** A judgment gate signs off a phase that passes tests but does not meet acceptance criteria.
- **Evidence:** phase-4 process gate_verdict_source=artifact, forgery_allowlist_present=false, 'Judgment-gate verdict is honor-system (bounded)'
- **Remediation:** Require judgment gates to cite machine-checkable evidence (specific acceptance-criterion artifacts) in their verdict payload; keep the red-XML veto as the hard floor.

### R-32 [Med] — process (P4)
- **Location:** coverage floor: windows-validation.yml:346-374 (OpenCppCoverage->Cobertura)
- **Description:** Coverage is best-effort: continue-on-error:true, silently skipped when the tool is absent, gated by NO floor. It produces an artifact but can never fail the build, so coverage regressions are invisible.
- **Exemplar escape:** A new untested subsystem (or a deleted test) drops coverage with no CI signal.
- **Failure missed:** Coverage silently erodes; the artifact exists but enforces nothing.
- **Evidence:** phase-4 process coverage_floor_enforced=false; windows-validation.yml:346-374
- **Remediation:** Enforce a per-subsystem coverage floor that fails the build on regression; make the tool's absence a hard error, not a silent skip (SC-d).

### R-33 [Med] — input-domain-gap (P2)
- **Location:** hostile-input-matrix zero classes + adversarial fixture backlog
- **Description:** 4 of 38 hostile-input classes have ZERO fixture coverage: sheared/skewed CTM in content-stream (the direct geometric sibling of the -100 Tz mirror bug this audit exists for), 0-page document, 10k-page scale boundary, combining-marks/ZWJ/grapheme clusters. The adversarial-ambient corpus has half-SCALE cm but no shear.
- **Exemplar escape:** An injector that fails to reset a sheared ambient CTM emits slanted/parallelogram text/images -- untested; a legally-empty (0-page) doc crashes page(0)-assuming paths.
- **Failure missed:** Slanted/parallelogram edits on sheared-CTM pages, crash on 0-page docs, O(n^2) blowup at 10k pages, delete-half-a-grapheme corruption -- no red test today.
- **Evidence:** phase-2 zero_coverage_hostile_classes (4); consolidated_adversarial_fixture_backlog (shear/rotated/mediabox-offset/grapheme/0-page/10k)
- **Remediation:** Add the phase-2 fixture backlog (aa_shear.pdf, aa_img_shear.pdf, rotated-edit, mediabox-offset, grapheme_clusters, zero_page, 10000page) and feed each through the injectors with the SC-a pixel/quad oracles.

### R-36 [Med] — process (P4)
- **Location:** CI: .github/workflows/macos-validation.yml:118-120
- **Description:** The macOS validation lane runs only PreflightOpen.*/PreflightXPlatform.* (a handful of isolation-boundary tests), not the full suite, so one of three advertised platforms provides almost no regression coverage.
- **Exemplar escape:** A macOS-specific regression outside the preflight subset ships green because the macOS lane never exercises the rest of the suite.
- **Failure missed:** Any macOS-only defect in the ~4360 tests outside the preflight subset is invisible on the macOS lane.
- **Evidence:** phase-4 process-critique.json finding 'macOS validation is a thin subset' (macos-validation.yml:118-120)
- **Remediation:** Run the full (or a representative core) suite on the macOS lane, or drop the platform's advertised support claim to match reality.

### R-34 [Low] — input-domain-gap (P2)
- **Location:** fixture catalog: 200 fixtures, 32 orphan, 1 missing-referenced; flaw_corpus (47) all -> one open-only smoke test
- **Description:** Adversarial breadth exists but adversarial DEPTH per product-path is thin: 32 orphan fixtures are unreferenced by any test, and the 47-fixture flaw corpus all feed a single open-only smoke test (an open-parse pass is not a save/redact/render pass).
- **Exemplar escape:** A fixture crafted to stress the save/redact/render path is only ever opened, so the deeper path it was built for is never exercised.
- **Failure missed:** Adversarial inputs prove open() survives but never prove save/redact/render survive; orphan fixtures test nothing.
- **Evidence:** phase-2 fixture_totals.orphan=32, missing_referenced=1; cross_validation (flaw_corpus open-only)
- **Remediation:** Wire orphan fixtures to a consumer test on their intended product path; extend the flaw corpus beyond open() to save/redact/render depth passes.

### R-37 [Low] — process (P4)
- **Location:** CI: census.json:451; .github/workflows/windows-validation.yml:291-304
- **Description:** Rerun-on-failure exists but known-flaky tests (PerfStressTest.TenThousandAnnotationsOnePage, MainWindowIntegrationTest.ZoomComboExists) are tracked only in inline comments — there is no machine-readable quarantine/xfail registry, so a test degrading from reliable to flaky is not a first-class signal and reruns can mask a slowly-worsening test.
- **Exemplar escape:** A test that silently drifts from reliable to intermittently-failing is absorbed by rerun-on-failure and never flagged as degrading.
- **Failure missed:** A slowly-worsening test (eventually a real regression) is masked by reruns with no quarantine ledger to surface the trend.
- **Evidence:** phase-4 process-critique.json finding 'No formal flaky quarantine ledger' (census.json:451; windows-validation.yml:291-304)
- **Remediation:** Add a machine-readable flaky-quarantine/xfail registry with an entry+owner+expiry per known-flaky test; fail CI when an unregistered test flakes.
