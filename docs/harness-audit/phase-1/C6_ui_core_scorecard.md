# C6 (ui + core/infra + other/uncategorized) — Adversarial Oracle Scorecard

**Cluster:** C6_ui_core — 122 files, 1152 tests.
**Stance:** the harness is the defendant. Each test is presumed worthless until it proves it asserts a *user-visible* truth AND fails when that truth breaks.
**Method:** every file opened, TEST bodies read (6 parallel batches, weakest-density-first). Dominant oracle per file; smells tallied at `file:line`. Known seed `test_coord_mapper_rotation` independently re-verified.

## Oracle distribution

| Oracle | Files | Share |
|---|---|---|
| true | 75 | 61% |
| proxy | 26 | 21% |
| heuristic | 11 | 9% |
| metamorphic | 7 | 6% |
| no-oracle-smoke | 3 | 2% |
| pseudo (dominant) | 0 | 0% |
| **Total** | **122** | |

Headline: ~61% of files carry a genuine user-visible oracle. The weak third is concentrated in **UI widget-existence files** and **"ran-without-error" wrappers** around convert/save/render/JS paths where the *effect* is never read back.

## Smell tally (files exhibiting each smell)

| Smell | Files | Notable offenders |
|---|---|---|
| widget-exists-only | 22 | dialog_object_names_phase11, deferred_panels, ui_integration, phase_f5_menu_wiring, dialog_widget_audit_phase2, uncovered_dialogs, installer, accessible_names |
| reopens-without-error-only | 16 | structure_tree_builder, compliance_manager, toolbar_toggles, preflight_fix_all, e2e_integration, realworld_external_corpus |
| EXPECT_NO_THROW-only | 12 | javascript_engine, compat_advanced, cli, crash_handler, performance_stress, realworld_corpus, external_flaw_corpus |
| tautological | 11 | hidpi, tile_renderer, javascript_engine, tile_prefetcher, pdfa_roundtrip, e2e_integration, cli |
| assertion-free | 11 | main, mainwindow_size_sanity, pdf_content_writer, dialog_widget_audit_phase2, uncovered_dialogs, performance_stress, header_footer |
| over-tolerant-threshold | 10 | performance_stress, stress_investigation, coordinate_transform, render_fidelity, c5_lcd_subpixel, branding_resources |
| extraction-only-for-visual | 10 | external_flaw_corpus, tile_renderer, thumbnails, page, b3_bg_classification, chrome_phase9, coord_mapper |

**UI widget-exists-only concentration:** ~12 of the ~55 UI files are dominated by existence/no-throw proxies rather than behavior oracles (dialog_object_names_phase11 15/15, deferred_panels 4/4, dialog_widget_audit_phase2, ui_integration 22/22, phase_f5_menu_wiring 8/8, uncovered_dialogs ~10/18, accessible_names, startup_ui_changes[MainWindow half], main_window_q_invokable[MenuOnly subset], installer, chrome_phase9[signal-locks], panel_interaction_phase8[LayerPanel]).

## Note on the known seed (`test_coord_mapper_rotation.cpp`)

The project's flagship "tautological rotation test" charge does **not** hold for the pure-math tests. `DecomposeQuarterTurn`/`ResidualZeroPageNonZeroRun` etc. (lines 34-91) feed hardcoded input matrices and assert **hardcoded expected integers** (90, 180, 270) — legitimate value oracles a mutation would kill. Verified independently by two readers. The genuine weakness is the *integration* test `NonRotatedRunMatchesPageTransform` (line 116): it only compares `computeRunTransform(identity)` to `getTransform()` on the integer `.rotation`/`.scaleX` fields — a self-consistency proxy that survives if `computeRunTransform` is a no-op passthrough or shares a translation/skew bug. Recommend re-tagging the seed against this specific test.

## Ranked weak findings (top → down by severity/impact)

| # | File:line | Test | Oracle | Smell | User-visible failure MISSED | Sev |
|---|---|---|---|---|---|---|
| 1 | test_compat_advanced.cpp:213 | HiddenTextInOcgLayerDetected | no-oracle-smoke | no-throw | Name claims OFF-layer OCG text is NOT exposed; body only asserts open+display-list don't throw. **Info-disclosure**: extraction leaking the hidden 'SECRET' string passes green. | high |
| 2 | test_hidpi.cpp:43 | ScaledTileRendering | tautological | tautological | Computes `zoom*dpr` and `TILE_SIZE*dpr` in the test and asserts ==2.0/==1024. **No renderer runs** — a renderer that forgets DPR (half-res/blurry HiDPI) stays green. | high |
| 3 | test_tile_renderer.cpp:306 | TileCoordinateCalculation | tautological | tautological | Recomputes `col*TILE_SIZE/zoom` itself, asserts ==768, **never calls TileRenderer**. Wrong tile→PDF math (shifted/blank pages) passes. | high |
| 4 | test_dialog_object_names_phase11.cpp:88 | SignDialog.TsaGroup | proxy | widget-exists-only | tsaGroup exists but checkbox/URL edit may be unwired → user enables RFC-3161 timestamping, TSA URL ignored, signature saved **without a trusted timestamp** (LTV silently absent). | high |
| 5 | test_javascript_engine.cpp:67 | CalculateAllFields | proxy | no-throw | `calculateAll()` could compute nothing/wrong; sole assertion is no-throw. **Form Total never recalculates** and ships green. | high |
| 6 | test_javascript_engine.cpp:198 | FireValidateEvent | proxy | tautological | `EXPECT_TRUE(result || !result)` passes for any bool. Field validation wrongly accepting/rejecting is invisible. (FireFormatEvent:257 identical.) | high |
| 7 | test_ui_integration.cpp:84 | AllActionsExist | proxy | widget-exists-only | 22 actions checked by objectName; **none triggered**. Save/Open/Find wired to no/wrong slot → do nothing when clicked, all pass. | high |
| 8 | test_phase_f5_menu_wiring.cpp:74 | FormsMenuHasCalculationOrderAction | proxy | widget-exists-only | File NAMED 'menu_wiring' yet only checks QAction exists/text/shortcut — **never triggers the wire**. Unwired menu item does nothing, passes. (altTextAction:120 same.) | high |
| 9 | test_toolbar_toggles.cpp:52 | AnnotationFilter_FilteredDisplayListCreated | proxy | reopens-without-error | Sets HideStickyNotes, asserts only display-list non-null. Filter failing to remove sticky notes → still non-null → **notes still render**, passes. | high |
| 10 | test_structure_tree_builder.cpp:99 | BuildMultiLevelTree | metamorphic | reopens-without-error | Builds H1+P+Figure(altText) but asserts only `hasStructureTree()==true`. Dropping the Figure/altText or writing all `/P` → **fails screen-reader use**, passes. | high |
| 11 | test_performance_stress.cpp:305 | ConcurrentMaliciousPdfRendering | no-oracle-smoke | assertion-free | `successCount+exceptionCount==5` always true if threads finish (successCount++ runs even on null open). **Cross-thread state bleed / corrupt tiles** all pass. | high |
| 12 | test_preflight_fix_all.cpp:193 | FixIssueAppliesXmpMetadataFix | proxy | reopens-without-error | Asserts only `r.ok`; comment waives verifying the XMP packet. Fixer returning ok but writing no `/Metadata` (doc still fails PDF/A) passes. (211/218 same.) | med |
| 13 | test_compliance_manager.cpp:54 | ConvertToPdfA_2b | proxy | reopens+tautological | File-exists + `targetLevel` echoes the input arg. **Non-conformant PDF/A-2b** (missing OutputIntent, unembedded fonts) passes — same file's validator could detect it. | med |
| 14 | test_pdf_content_writer.cpp:280 | GetImageDataFromScannedPageReturnsNonEmpty | no-oracle-smoke | assertion-free | Name says 'ReturnsNonEmpty', body is `SUCCEED()` — returned data never inspected. Empty/garbage image bytes pass. (GetContentStreamData:308 same.) | med |
| 15 | test_e2e_integration.cpp:155 | IncrementalSavePreservesSignedByteRange | proxy | reopens-without-error | Name promises byte-range preservation; asserts only pageCount survives. A save that **invalidates the digital signature** passes. | med |
| 16 | test_external_flaw_corpus.cpp:338 | Font_*_OpenRenderExtract (12) | proxy | extraction-only | Comment says 'extraction must return text' but `extractPlainText()` is `(void)`-discarded. **Empty/garbage text on non-embedded/CID fonts** passes; only pageCount+no-crash enforced. | med |
| 17 | test_pdf_canvas_q_invokable.cpp:388 | ScrollsToAnnotationRect | proxy | proxy | Asserts only `currentPage()==0` (scrollOffset_ private). Wrong/no scroll offset → **annotation off-screen**, passes; real check punted to Python diff. | med |
| 18 | test_uncovered_dialogs.cpp:202 | TrustAnchorDialogRefreshIsIdempotent | no-oracle-smoke | assertion-free | `refresh()` ×3 then `SUCCEED()`. Refresh that **duplicates trust-anchor rows** passes silently. Amid ~5 findChild!=null-only construct tests (71/140/168/191). | med |
| 19 | test_page_panel_q_invokable.cpp:102 | MovePageProgrammatic_ValidMove | proxy | proxy | Asserts `pageMoved(0,2)` signal args but never checks the list rows / page order changed. Signal emitted without performing the move passes. | med |
| 20 | test_stress_investigation.cpp:305 | SaveTimeAt500Fields | proxy | over-tolerant+extraction | Asserts only save <30s + file exists; **never reopens to confirm 500 fields persisted**. Dropping/corrupting fields but returning fast passes. | med |
| 21 | test_c5_lcd_subpixel.cpp:209 | LcdBlackText_SubpixelInAlphaChannel | heuristic | assertion-free+tautological | Computes gray vs LCD distinct-alpha counts to prove LCD carries more subpixel info but only asserts each >1, **never compares them**. LCD collapsing to grayscale passes. | med |
| 22 | test_crash_handler.cpp:62 | TestCallback_Set | proxy | no-throw | Installs a capturing callback, never triggers a crash, discards it; `callbackCalled` never asserted true. Broken crash→callback plumbing (**CI modal-wedge risk**) regresses silently. | med |
| 23 | test_log_handler.cpp:107 | FileLogging_Enable_Disable | proxy | reopens-without-error | Asserts `isFileLogging()==true` + path getter; never emits a message nor checks the file. Broken file writing with correct flag passes. | med |
| 24 | test_gap102_residuals.cpp:276 | IsPrintEnabledSeamWorks | proxy | proxy | Asserts only `invokeMethod` returns true (invokable); never that the returned bool matches real print-enabled state. **Print greyed with a doc / enabled with none** undetected. | med |
| 25 | test_coord_mapper_rotation.cpp:116 | NonRotatedRunMatchesPageTransform | proxy | extraction-only | (Seed) Compares two mapper code paths on integer rotation/scale only, never a mapped point/quad. Shared translation/skew error placing reconstructed text at the wrong pixel passes. | low |
| 26 | test_coordinate_transform.cpp:311 | AnnotationRendersAtCorrectPosition | true | over-tolerant | ±30px search window at zoom 1.0 for a 50×50 annotation. A **~30px visible misplacement** still finds red, passes. | low |
| 27 | test_render_fidelity.cpp:87 | RenderProducesNonBlankImage | heuristic | over-tolerant | Comment says '≥5% non-white'; code asserts `>0`. A page with **99.9% content dropped** (one stray dark pixel) passes. | low |
| 28 | test_single_instance_guard.cpp:149 | DefaultNameIsPerUser | proxy | proxy | Asserts prefix + length, not that the suffix is the OS username. A constant suffix → **two users collide on one single-instance domain**, stealing each other's file-open. | low |

## Recommended next actions (for the RTM / mutation phase)
- **Highest ROI mutation targets** (should survive mutation today → prove the gap): #1, #2, #3, #9, #10, #11, plus the JS-effect no-throws (#5/#6).
- **Systemic pattern 1 — "convert/fix/save returns ok, never re-validated":** compliance_manager, preflight_fix_all, pdfa_converter, stress_investigation, e2e_integration. Add a reopen-and-assert-conformance/persistence step.
- **Systemic pattern 2 — "widget exists ≠ widget wired":** the 22 widget-exists-only files. Convert the highest-risk (Sign/TSA, menu wiring, form actions) to trigger-and-assert-effect.
- **Systemic pattern 3 — "no-throw stands in for a security/correctness property":** compat_advanced OCG, external_flaw_corpus font extraction, crash_handler callback. Assert the actual extracted/rendered content, not just survival.
- **Silent CI coverage loss:** b3_bg_classification and others GTEST_SKIP when gitignored fixtures are absent — DPI-kernel and font regressions ship green on a clean checkout. Gate fixture generation or fail loudly.
