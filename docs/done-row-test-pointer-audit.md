# DONE-row test-pointer audit

**Stamp:** 2026-05-02T04:40:15Z
**Pipeline:** E phase-2 (f3c2a1d7-8e4b-4f91-b6c0-2d5e9a7f3b81-phase-2)
**Scope:** every `test_*.cpp` reference in `docs/acrobat-pro-parity-status.md`
**Source of truth:** `tests/fixtures/gtest_list_output.txt` (3506 lines, 397 suites, 3109 tests)

---

> ## ⚠ THIS VERDICT EXPIRED — re-audited 2026-08-03 (meta-audit follow-up F19, gap A5-17)
>
> The 2026-05-02 summary below is **accurate for the tree it was run against**
> and every one of the 69 pointers it checked still resolves today. It is
> nonetheless **no longer a valid statement about the parity document**,
> because that document has grown by 43 pointers since:
>
> | | 2026-05-02 audit | 2026-08-03 re-audit |
> |---|---:|---:|
> | unique `test_*` file pointers in `acrobat-pro-parity-status.md` | 69 | **112** (96 `.cpp` + 16 `.py`) |
> | pointers NOT resolving | 0 | **1** |
> | specific test-case pointers | 1 | **3** |
> | specific-case pointers NOT resolving | 0 | 0 |
>
> The one that did not resolve was `` `test_cert_chain_extractor.cpp` (4) ``
> at `acrobat-pro-parity-status.md` — a file that has never existed under that
> name; `CertChainExtractor` coverage lives in `tests/test_ltv_dss_writer.cpp`
> (see its header comment, line 16). **It has been corrected in the parity
> document in the same commit as this note.**
>
> **A point-in-time audit is not a control.** This document went three months
> presenting a verdict that had silently stopped being true, and nothing
> re-derived it. The replacement is a standing one:
> `F19DocClaims.EveryParityDocTestPointerResolves` in
> `tests/test_f19_doc_claim_guards.cpp` re-extracts every pointer from the
> parity document on every suite run and fails on the first that does not
> resolve. Treat the snapshot below as history, and the guard as the answer to
> "do the pointers resolve?".

## Summary (as of 2026-05-02 — superseded, see the box above)

- Unique `test_*.cpp` files referenced: **69**
- Files resolving to ≥1 active test in the binary: **69**
- Files NOT resolving: **0**
- Specific test-case references (e.g. `file.cpp::CaseName`): **1**
  - Resolved: **1**
  - Not resolved: **0**

**VERDICT (2026-05-02): all 69 file pointers and 1 specific-case pointers verified.**

No DONE rows in `docs/acrobat-pro-parity-status.md` carried a stale `test_*.cpp`
pointer *at that stamp*. One did by 2026-08-03; see the box above.

## Resolved file pointers (sample test name shown)

| test_*.cpp | sample test in binary | TEST macros in file |
|---|---|---:|
| `test_alignment_e2e.cpp` | `AlignmentE2ETest.SaveWithoutQueueIsNoOp` | 5 |
| `test_alignment_injector.cpp` | `AlignmentInjectorTest.ApplyInjectsBlockAndFileRemainsParseable` | 5 |
| `test_alignment_pipeline.cpp` | `AlignmentPipelineTest.EmptyQueueReportsZeroPendingPages` | 9 |
| `test_alignment_writer.cpp` | `AlignmentMarker.EmitsExpectedWord` | 17 |
| `test_alt_text.cpp` | `AltTextTest.SetAndGetAltText` | 6 |
| `test_alt_text_dialog.cpp` | `AltTextDialogTest.ExposesNamedChildrenForAutomation` | 5 |
| `test_annotation_manager.cpp` | `AnnotationManagerTest.CreateTextAnnotation` | 14 |
| `test_annotation_tools.cpp` | `AnnotationToolsTest.StickyNoteCreation` | 8 |
| `test_attachment_panel.cpp` | `AttachmentPanelTest.PanelExposesNamedChildrenForAutomation` | 6 |
| `test_attachments.cpp` | `AttachmentSanitize.PlainBasenamePassesThrough` | 14 |
| `test_auto_tagger.cpp` | `AutoTaggerTest.ConstructDestroy` | 10 |
| `test_batch.cpp` | `BatchEngineTest.ConstructDestroy` | 33 |
| `test_bates_tokens.cpp` | `BatesToken.BasicDefaultWidth` | 10 |
| `test_block_params_builder.cpp` | `BlockParamsBuilderTest.BboxFieldsArePopulatedFromRect` | 10 |
| `test_bookmark_manager.cpp` | `BookmarkManagerTest.LoadProducesTree` | 14 |
| `test_calculation_order.cpp` | `CalculationOrderTest.ListFieldNamesExposesFormFields` | 10 |
| `test_comment_panel.cpp` | `CommentPanelTest.ConstructsSuccessfully` | 11 |
| `test_comment_panel_threading.cpp` | `CommentPanelThreadingTest.ReplyNestsUnderParentInTree` | 4 |
| `test_comment_threading.cpp` | `CommentThreadingTest.SetAndGetInReplyToOnSamePage` | 5 |
| `test_compliance_dialog.cpp` | `ComplianceDialogTest.ConstructDestroy` | 26 |
| `test_compliance_manager.cpp` | `ComplianceManagerTest.ConstructDestroy` | 8 |
| `test_diff.cpp` | `DiffEngineTest.ConstructDestroy` | 28 |
| `test_document_search_index.cpp` | `DocumentSearchIndexTest.LazyBuildOnFirstSearch` | 5 |
| `test_e2e_integration.cpp` | `E2EAlignment.FormattingBlockAlignmentSurvivesReflowCycle` | 5 |
| `test_excel_export.cpp` | `ExcelExportNumber.AcceptsIntegersAndDecimals` | 7 |
| `test_export_plain_text.cpp` | `ExportPlainTextTest.ActionExistsAndStartsDisabled` | 9 |
| `test_fixture_corpus.cpp` | `FixtureCorpus.PdfASidecarsExistForEveryConformanceLevel` | 7 |
| `test_follow_link.cpp` | `FollowLinkTest.GoToLinkNavigatesCanvasToTargetPage` | 6 |
| `test_form_creation.cpp` | `FormCreationTest.CreateTextField` | 10 |
| `test_form_submit.cpp` | `FormEncodePercent.AlphaNumPassThrough` | 19 |
| `test_form_submitter.cpp` | `FormSubmitterDecision.RejectsEmptyUrl` | 8 |
| `test_form_tool.cpp` | `FormToolTest.InitialState` | 11 |
| `test_forms_designer.cpp` | `FormsDesignerToolbarTest.AllSevenFieldTypeActionsPresent` | 6 |
| `test_header_footer.cpp` | `HeaderFooterTokens.ResolvesDate` | 18 |
| `test_image_exporter.cpp` | `ImageExporterPattern.PageTokenIsOneBasedAndZeroPadded` | 11 |
| `test_image_to_pdf_wizard.cpp` | `ImageToPdfWizardTest.SupportedExtensionsListedNonEmpty` | 7 |
| `test_incremental_save.cpp` | `IncrementalSaveTest.PreservesOriginalBytePrefix` | 5 |
| `test_invisible_and_certification_signatures.cpp` | `InvisibleSignatureTest.InvisibleFlagSignsSuccessfully` | 8 |
| `test_lanczos_resize.cpp` | `LanczosResizeKernel.ZeroReturnsOne` | 15 |
| `test_layers.cpp` | `LayerManagerTest.ConstructDestroy` | 21 |
| `test_link_actions.cpp` | `LinkActionTest.DefaultLinkHasNoAction` | 20 |
| `test_link_tool.cpp` | `LinkAnnotationTypeTest.LinkIsInEnum` | 5 |
| `test_mainwindow_size_sanity.cpp` | `MainWindowSizeSanityTest.SizeofIsNonZero` | 2 |
| `test_measure.cpp` | `MeasureCalcDistance.TwoHorizontalPoints` | 22 |
| `test_navigation_history.cpp` | `NavigationHistoryTest.NewHistoryIsEmpty` | 11 |
| `test_ocr_all_pages_command.cpp` | `OcrAllPagesCommand.ExecuteRunsOcrForEveryListedPage` | 7 |
| `test_ocr_language_preference.cpp` | `OcrLanguagePreferenceTest.DefaultLanguageIsEnglish` | 8 |
| `test_optimize.cpp` | `OptimizeEngineTest.DefaultOptionsSucceedOnSimpleFile` | 8 |
| `test_page_commands.cpp` | `PageCommandTest.RotatePageCommand_Execute` | 27 |
| `test_page_operations.cpp` | `PageOperationsTest.ExtractPages_SinglePage` | 10 |
| `test_pdfa_converter.cpp` | `PdfAConverterTest.ConstructDestroy` | 12 |
| `test_pdfua_converter.cpp` | `PdfUAConverterTest.ConstructDestroy` | 9 |
| `test_portfolio.cpp` | `PortfolioTest.PortfolioFixtureIsDetected` | 6 |
| `test_preflight_dialog.cpp` | `PreflightDialogTest.ExposesNamedControlsForAutomation` | 10 |
| `test_replace_page.cpp` | `ReplacePageTest.ReplacePagePreservesPageCount` | 4 |
| `test_scan_style_transfer.cpp` | `ScanStyleMeasurement.EdgeRoughness_CleanGlyph` | 24 |
| `test_stamp_and_watermark.cpp` | `StampToolTest.ToolTypeIsStamp` | 11 |
| `test_stamp_tokens.cpp` | `StampTokenResolution.DateTokenResolvesToIsoDate` | 12 |
| `test_structure_tree_builder.cpp` | `StructureTreeBuilderTest.ConstructDestroy` | 9 |
| `test_tantivy_search.cpp` | `TantivyIndexBasic.OpenCreatesIndexDirAndReportsZeroDocs` | 16 |
| `test_text_edit_clipboard.cpp` | `TextEditClipboardTest.CopyAfterSelectAll` | 5 |
| `test_text_reflow.cpp` | `Tokenize.SplitsOnWhitespaceAndCollapsesRuns` | 20 |
| `test_theme_and_field_count.cpp` | `ThemeManagerTest.ThemeToAndFromStringRoundTrip` | 10 |
| `test_tsa_and_taborder.cpp` | `TsaRequestTest.BuildTimeStampReqProducesValidDer` | 33 |
| `test_tsa_fixture.cpp` | `TsaFixtureTest.PdfParsesAndHasOnePage` | 6 |
| `test_view_rotation.cpp` | `ViewRotationTest.DefaultRotationIsZero` | 9 |
| `test_visual_font_ranker.cpp` | `VisualFontRankerTest.EmptyCandidatesReturnsEmpty` | 5 |
| `test_watermark_extended.cpp` | `WatermarkExtendedTest.LegacyApplyWatermarkAppliesToAllPages` | 8 |
| `test_word_export.cpp` | `WordExportHeuristics.BoldDetectedCaseInsensitive` | 12 |

## Resolved specific-case pointers

| Reference | Resolves to |
|---|---|
| `test_e2e_integration.cpp::CleanScanRecallAboveNinetyFivePercent` | `E2EOcr.CleanScanRecallAboveNinetyFivePercent` |

---

## Method

1. Dumped `build/vs2022/bin/Debug/pdfe_tests.exe --gtest_list_tests` to `tests/fixtures/gtest_list_output.txt` and parsed it into a `{Suite.Test}` set.
2. Extracted every `test_*.cpp` and `test_*.cpp::CaseName` reference from `docs/acrobat-pro-parity-status.md` via regex.
3. For each unique file: scanned the file for `TEST_F(Suite, Test)` and `TEST(Suite, Test)` macros and confirmed that ≥ 1 of them appears in the binary list.
4. For specific-case references: looked up the case name directly in the binary list.

A file pointer is considered RESOLVED if at least one of its declared TEST/TEST_F cases is present in the binary. A specific-case pointer is RESOLVED if and only if that exact case name appears.
