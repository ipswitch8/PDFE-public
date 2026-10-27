# Matrix triage — Phase 2 (pipeline c82a1f37)

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


**Date:** 2026-07-03  
**Scope:** per-row disposition of all 225 `blocked` rows remaining after P1 (green-wins aggregation recovered 110 act_ rows). Grounds every cat-4 'feature-absent' claim against `src/ui/*` and routes every row. No row is greened in this phase (no live run); greening is deferred to the owning re-measure phase.

## Headline: the 'feature-absent' premise is largely disproven

The investigation doc (d884c69) estimated ~77 rows were features genuinely absent from the build and would mostly become `wont-fix`. Direct grounding against source shows the opposite: the 'not implemented / no menu wiring / feature not built' notes are **stale** — the features exist with stable objectNames. Nearly every blocked row also carries the tag `[reconciled 9a4e2f71-P6: ... measured non-green on runner run 28465445439/28554887271 — demoted to blocked]` — the SAME corrupt aggregate whose act_ clobber P1 fixed — and many carry passing assertions in their own notes. Honest terminal `wont-fix` is therefore small (5), not >=30; the rest are recoverable (precondition / drag / re-measure).

### AC reconciliation

P2 AC bullet 3 required `wont-fix` to increase by >=30. That target was premised on the disproven estimate. Honest grounding yields **5** genuine wont-fix; inflating to 30 would fabricate wont-fix (the inverse of faking green) and is refused per the standing rule *wont-fix ONLY where a feature genuinely cannot be tested*. Re-scoped per user direction 2026-07-03.

### Disposition summary

| Disposition | Count | Meaning |
|---|---|---|
| WONT-FIX | 5 | genuinely un-testable at the UI/matrix layer (product/env/Qt-ownership); evidence-backed |
| MAPPING->P3 | 1 | feature exists under a different objectName; re-point the probe, then re-measure |
| P3-PRECONDITION | 48 | feature exists; probe needs a precondition established (image/atlas/form-tool/textedit) — P3 |
| P4-DRAG | 23 | needs a real canvas click-drag rect primitive (P-GAP-75) — P4 |
| P5-ENV | 3 | env-flaky on Server-2019 RDP; covered by C++/WAD; formalize in P5 |
| P5-FIXTURE | 1 | needs a new fixture (creatable); route to P5 |
| REMEASURE | 144 | feature exists / gtest passes / passing-assertion note; demoted by the blanket reconcile — a real run recovers to green |
| **total** | **225** | |

## WONT-FIX (5)

| test_id | subsystem | element | current | disposition justification |
|---|---|---|---|---|
| `canvas_sticky_note_drag_popup` | Canvas | canvas_sticky_note_drag_popup | wont-fix | wont-fix (product): StickyNotePopup steals focus from the drag gesture; requires a product focus-override change to be automatable. Not a test-layer defect. |
| `dlg_bookmarkPanel_bookmarkContextMenu` | Dialogs | bookmarkPanel.bookmarkContextMenu | wont-fix | wont-fix (Qt-internal): QMenu::exec popup is not in the Qt6 UIA tree at rest (WIDGET_BLOCKLIST); its action children are covered via the testContextActionsQueryPipe seam + C++ BookmarkContextMenuTest (11 gtests). |
| `dlg_commentPanel_commentContextMenu` | Dialogs | commentPanel.commentContextMenu | wont-fix | wont-fix (Qt-internal): stack-local QMenu::exec popup not exposed in the Qt6 UIA tree at rest (mirrors bookmarkContextMenu); action children commentReplyAction/commentDeleteAction reachable via testContextActionsQueryPipe (CommentPanel.cpp:186). |
| `dlg_printDialog_rangeButtonGroup` | Dialogs | printDialog.rangeButtonGroup | wont-fix | wont-fix (Qt-internal): QButtonGroup is not a QWidget and carries no settable objectName/AutomationId; the individual range radios ARE tested. Qt owns the group container. |
| `sec_sign_chain_validation` | Security | sec_sign_chain_validation | wont-fix | wont-fix (env/product): full chain validation needs a CA-signed cert (test_signer.pfx is self-signed) + OS trust-store import on the runner; not reproducible at the UI-test layer. Self-signed verify path is covered by sec_verify_signed. |

## MAPPING->P3 (1)

| test_id | subsystem | element | current | disposition justification |
|---|---|---|---|---|
| `menu_rotate_view` | View | View > RotateView | blocked | MAPPING: real objectName is 'actionRotateViewCW' in rotateMenu (MainWindow.cpp:3226,3755), NOT 'rotateViewAction' -> re-point + re-measure (not greened here). |

## P3-PRECONDITION (48)

| test_id | subsystem | element | current | disposition justification |
|---|---|---|---|---|
| `annot_textmarkup_set_color` | Annotations | annot_TextMarkup | blocked | needs Properties-Panel stacked page for the just-created markup annotation; route P3. |
| `dlg_atlasComparisonDialog_atlasAtlasCharsetLabel` | Dialogs | atlasComparisonDialog.atlasAtlasCharsetLabel | blocked | needs the TRE atlas chain (scanned fixture + OCR span-click + Compare Atlas Modes); route P3. |
| `dlg_atlasComparisonDialog_atlasAtlasCharsetScroll` | Dialogs | atlasComparisonDialog.atlasAtlasCharsetScroll | blocked | needs the TRE atlas chain (scanned fixture + OCR span-click + Compare Atlas Modes); route P3. |
| `dlg_atlasComparisonDialog_atlasAtlasPreview` | Dialogs | atlasComparisonDialog.atlasAtlasPreview | blocked | needs the TRE atlas chain (scanned fixture + OCR span-click + Compare Atlas Modes); route P3. |
| `dlg_atlasComparisonDialog_atlasCancelButton` | Dialogs | atlasComparisonDialog.atlasCancelButton | blocked | needs the TRE atlas chain (scanned fixture + OCR span-click + Compare Atlas Modes); route P3. |
| `dlg_atlasComparisonDialog_atlasFillMissingCheck` | Dialogs | atlasComparisonDialog.atlasFillMissingCheck | blocked | needs the TRE atlas chain (scanned fixture + OCR span-click + Compare Atlas Modes); route P3. |
| `dlg_atlasComparisonDialog_atlasFontCharsetLabel` | Dialogs | atlasComparisonDialog.atlasFontCharsetLabel | blocked | needs the TRE atlas chain (scanned fixture + OCR span-click + Compare Atlas Modes); route P3. |
| `dlg_atlasComparisonDialog_atlasFontCharsetScroll` | Dialogs | atlasComparisonDialog.atlasFontCharsetScroll | blocked | needs the TRE atlas chain (scanned fixture + OCR span-click + Compare Atlas Modes); route P3. |
| `dlg_atlasComparisonDialog_atlasFontCombo` | Dialogs | atlasComparisonDialog.atlasFontCombo | blocked | needs the TRE atlas chain (scanned fixture + OCR span-click + Compare Atlas Modes); route P3. |
| `dlg_atlasComparisonDialog_atlasFontPreview` | Dialogs | atlasComparisonDialog.atlasFontPreview | blocked | needs the TRE atlas chain (scanned fixture + OCR span-click + Compare Atlas Modes); route P3. |
| `dlg_atlasComparisonDialog_atlasOriginalPreview` | Dialogs | atlasComparisonDialog.atlasOriginalPreview | blocked | needs the TRE atlas chain (scanned fixture + OCR span-click + Compare Atlas Modes); route P3. |
| `dlg_atlasComparisonDialog_atlasUseFontButton` | Dialogs | atlasComparisonDialog.atlasUseFontButton | blocked | needs the TRE atlas chain (scanned fixture + OCR span-click + Compare Atlas Modes); route P3. |
| `dlg_atlasComparisonDialog_atlasUseScanButton` | Dialogs | atlasComparisonDialog.atlasUseScanButton | blocked | needs the TRE atlas chain (scanned fixture + OCR span-click + Compare Atlas Modes); route P3. |
| `dlg_formFieldPropertiesPanel_formFieldCheckbox` | Dialogs | formFieldPropertiesPanel.formFieldCheckbox | blocked | needs FormTool active + Tab to a field before the panel populates; route P3. |
| `dlg_formFieldPropertiesPanel_formFieldCombo` | Dialogs | formFieldPropertiesPanel.formFieldCombo | blocked | needs FormTool active + Tab to a field before the panel populates; route P3. |
| `dlg_formFieldPropertiesPanel_formFieldEditorStack` | Dialogs | formFieldPropertiesPanel.formFieldEditorStack | blocked | needs FormTool active + Tab to a field before the panel populates; route P3. |
| `dlg_formFieldPropertiesPanel_formFieldFlagsLabel` | Dialogs | formFieldPropertiesPanel.formFieldFlagsLabel | blocked | needs FormTool active + Tab to a field before the panel populates; route P3. |
| `dlg_formFieldPropertiesPanel_formFieldList` | Dialogs | formFieldPropertiesPanel.formFieldList | blocked | needs FormTool active + Tab to a field before the panel populates; route P3. |
| `dlg_formFieldPropertiesPanel_formFieldMultilineEdit` | Dialogs | formFieldPropertiesPanel.formFieldMultilineEdit | blocked | needs FormTool active + Tab to a field before the panel populates; route P3. |
| `dlg_formFieldPropertiesPanel_formFieldNameLabel` | Dialogs | formFieldPropertiesPanel.formFieldNameLabel | blocked | needs FormTool active + Tab to a field before the panel populates; route P3. |
| `dlg_formFieldPropertiesPanel_formFieldNoEditor` | Dialogs | formFieldPropertiesPanel.formFieldNoEditor | blocked | needs FormTool active + Tab to a field before the panel populates; route P3. |
| `dlg_formFieldPropertiesPanel_formFieldTextEdit` | Dialogs | formFieldPropertiesPanel.formFieldTextEdit | blocked | needs FormTool active + Tab to a field before the panel populates; route P3. |
| `dlg_formFieldPropertiesPanel_formFieldTypeLabel` | Dialogs | formFieldPropertiesPanel.formFieldTypeLabel | blocked | needs FormTool active + Tab to a field before the panel populates; route P3. |
| `dlg_imagePropertiesPanel_imageColorSpaceLabel` | Dialogs | imagePropertiesPanel.imageColorSpaceLabel | blocked | needs an image selected on canvas before the panel populates; route P3. |
| `dlg_imagePropertiesPanel_imageCompressionLabel` | Dialogs | imagePropertiesPanel.imageCompressionLabel | blocked | needs an image selected on canvas before the panel populates; route P3. |
| `dlg_imagePropertiesPanel_imageCropButton` | Dialogs | imagePropertiesPanel.imageCropButton | blocked | needs an image selected on canvas before the panel populates; route P3. |
| `dlg_imagePropertiesPanel_imageDisplayDimensionsLabel` | Dialogs | imagePropertiesPanel.imageDisplayDimensionsLabel | blocked | needs an image selected on canvas before the panel populates; route P3. |
| `dlg_imagePropertiesPanel_imageDpiLabel` | Dialogs | imagePropertiesPanel.imageDpiLabel | blocked | needs an image selected on canvas before the panel populates; route P3. |
| `dlg_imagePropertiesPanel_imageExtractButton` | Dialogs | imagePropertiesPanel.imageExtractButton | blocked | needs an image selected on canvas before the panel populates; route P3. |
| `dlg_imagePropertiesPanel_imageHeightSpin` | Dialogs | imagePropertiesPanel.imageHeightSpin | blocked | needs an image selected on canvas before the panel populates; route P3. |
| `dlg_imagePropertiesPanel_imageInfoGroup` | Dialogs | imagePropertiesPanel.imageInfoGroup | blocked | needs an image selected on canvas before the panel populates; route P3. |
| `dlg_imagePropertiesPanel_imagePixelDimensionsLabel` | Dialogs | imagePropertiesPanel.imagePixelDimensionsLabel | blocked | needs an image selected on canvas before the panel populates; route P3. |
| `dlg_imagePropertiesPanel_imageRemoveButton` | Dialogs | imagePropertiesPanel.imageRemoveButton | blocked | needs an image selected on canvas before the panel populates; route P3. |
| `dlg_imagePropertiesPanel_imageRotationCombo` | Dialogs | imagePropertiesPanel.imageRotationCombo | blocked | needs an image selected on canvas before the panel populates; route P3. |
| `dlg_imagePropertiesPanel_imageTransformGroup` | Dialogs | imagePropertiesPanel.imageTransformGroup | blocked | needs an image selected on canvas before the panel populates; route P3. |
| `dlg_imagePropertiesPanel_imageWidthSpin` | Dialogs | imagePropertiesPanel.imageWidthSpin | blocked | needs an image selected on canvas before the panel populates; route P3. |
| `dlg_imagePropertiesPanel_imageXObjectNameLabel` | Dialogs | imagePropertiesPanel.imageXObjectNameLabel | blocked | needs an image selected on canvas before the panel populates; route P3. |
| `calc_order_dialog_drag_reorder` | Forms | calc_order_dialog_drag_reorder | blocked | calculationOrderAction menu-wired (MainWindow.cpp:3552); reorder path exists -> feature exists; needs tool/dialog precondition, route P3. |
| `calc_order_dialog_open` | Forms | calc_order_dialog_open | blocked | calculationOrderAction in formsMenu (MainWindow.cpp:2814,3552); enabled at 4904 -> feature exists; needs tool/dialog precondition, route P3. |
| `atlas_mode_combo_set_persist` | Subsystem | atlasComparisonDialog.atlasModeCombo.setPersist | blocked | needs the TRE atlas chain (scanned fixture + OCR span-click + Compare Atlas Modes); route P3. |
| `atlas_mode_visual_diff` | Subsystem | atlasComparisonDialog.modeChange.visualDiff | blocked | needs the TRE atlas chain (scanned fixture + OCR span-click + Compare Atlas Modes); route P3. |
| `font_match_panose_top_candidate` | Subsystem | fontMatcher.panoseTopCandidate | blocked | needs the TRE atlas chain (scanned fixture + OCR span-click + Compare Atlas Modes); route P3. |
| `glyph_synth_coverage` | Subsystem | glyphSynthesizer.synthesizedGlyphRenders | blocked | needs the TRE atlas chain (scanned fixture + OCR span-click + Compare Atlas Modes); route P3. |
| `scan_style_toggle_persist` | Subsystem | atlasComparisonDialog.scanStyleTransfer.persist | blocked | needs the TRE atlas chain (scanned fixture + OCR span-click + Compare Atlas Modes); route P3. |
| `txt_align_center` | TextEdit | txt_align_center | blocked | alignCenterButton exists (TextEditToolBar.cpp:205-246) -> feature exists; needs tool/dialog precondition, route P3. |
| `txt_align_justify` | TextEdit | txt_align_justify | blocked | alignJustifyButton exists (TextEditToolBar.cpp:221-252) -> feature exists; needs tool/dialog precondition, route P3. |
| `txt_align_left` | TextEdit | txt_align_left | blocked | alignLeftButton exists with objectName + handler (TextEditToolBar.cpp:196-243) -> feature exists; needs tool/dialog precondition, route P3. |
| `txt_align_right` | TextEdit | txt_align_right | blocked | alignRightButton exists (TextEditToolBar.cpp:213-249) -> feature exists; needs tool/dialog precondition, route P3. |

## P4-DRAG (23)

| test_id | subsystem | element | current | disposition justification |
|---|---|---|---|---|
| `annot_select_delete` | Annotations | annot_Select | blocked | needs canvas-click to select the annotation before configuring (SelectTool); route P4. |
| `annot_select_set_color` | Annotations | annot_Select | blocked | needs canvas-click to select the annotation before configuring (SelectTool); route P4. |
| `annot_select_set_contents` | Annotations | annot_Select | blocked | needs canvas-click to select the annotation before configuring (SelectTool); route P4. |
| `annot_select_set_opacity` | Annotations | annot_Select | blocked | needs canvas-click to select the annotation before configuring (SelectTool); route P4. |
| `canvas_annot_handle_drag_corner` | Canvas | canvas_annot_handle_drag_corner | blocked | canvas click-drag rect placement gap (P-GAP-75); needs the P4 CANVAS_DRAG seam. |
| `canvas_annot_handle_drag_edge` | Canvas | canvas_annot_handle_drag_edge | blocked | canvas click-drag rect placement gap (P-GAP-75); needs the P4 CANVAS_DRAG seam. |
| `canvas_annot_handle_drag_rotate` | Canvas | canvas_annot_handle_drag_rotate | blocked | canvas click-drag rect placement gap (P-GAP-75); needs the P4 CANVAS_DRAG seam. |
| `canvas_annot_move_drag` | Canvas | canvas_annot_move_drag | blocked | canvas click-drag rect placement gap (P-GAP-75); needs the P4 CANVAS_DRAG seam. |
| `canvas_canvas_click_away_deselect` | Canvas | canvas_canvas_click_away_deselect | blocked | canvas click-drag rect placement gap (P-GAP-75); needs the P4 CANVAS_DRAG seam. |
| `canvas_canvas_rubber_band_select` | Canvas | canvas_canvas_rubber_band_select | blocked | canvas click-drag rect placement gap (P-GAP-75); needs the P4 CANVAS_DRAG seam. |
| `canvas_canvas_scroll_wheel` | Canvas | canvas_canvas_scroll_wheel | blocked | canvas click-drag rect placement gap (P-GAP-75); needs the P4 CANVAS_DRAG seam. |
| `canvas_text_selection_drag` | Canvas | canvas_text_selection_drag | blocked | canvas click-drag rect placement gap (P-GAP-75); needs the P4 CANVAS_DRAG seam. |
| `canvas_text_selection_triple_click` | Canvas | canvas_text_selection_triple_click | blocked | canvas click-drag rect placement gap (P-GAP-75); needs the P4 CANVAS_DRAG seam. |
| `field_checkbox_create` | Forms | field_Checkbox | blocked | canvas click-drag rect placement gap (P-GAP-75); needs the P4 CANVAS_DRAG seam. |
| `field_combobox_create` | Forms | field_ComboBox | blocked | canvas click-drag rect placement gap (P-GAP-75); needs the P4 CANVAS_DRAG seam. |
| `field_listbox_create` | Forms | field_ListBox | blocked | canvas click-drag rect placement gap (P-GAP-75); needs the P4 CANVAS_DRAG seam. |
| `field_radiobutton_create` | Forms | field_RadioButton | blocked | canvas click-drag rect placement gap (P-GAP-75); needs the P4 CANVAS_DRAG seam. |
| `field_signature_create` | Forms | field_Signature | blocked | canvas click-drag rect placement gap (P-GAP-75); needs the P4 CANVAS_DRAG seam. |
| `field_text_create` | Forms | field_Text | blocked | canvas click-drag rect placement gap (P-GAP-75); needs the P4 CANVAS_DRAG seam. |
| `txt_tre_edit_arabic` | TextEdit | txt_tre_edit_arabic | blocked | TRE edit needs a canvas click on the OCR text layer of a scanned fixture; route P4/canvas. |
| `txt_tre_edit_cjk` | TextEdit | txt_tre_edit_cjk | blocked | TRE edit needs a canvas click on the OCR text layer of a scanned fixture; route P4/canvas. |
| `txt_tre_edit_regular_font` | TextEdit | txt_tre_edit_regular_font | blocked | TRE edit needs a canvas click on the OCR text layer of a scanned fixture; route P4/canvas. |
| `txt_tre_edit_size_change` | TextEdit | txt_tre_edit_size_change | blocked | TRE edit needs a canvas click on the OCR text layer of a scanned fixture; route P4/canvas. |

## P5-ENV (3)

| test_id | subsystem | element | current | disposition justification |
|---|---|---|---|---|
| `sec_encrypt_aes_128_set` | Security | sec_encrypt_aes_128_set | blocked | EncryptionDialog open-via-keystroke is env-flaky on Server-2019 RDP (COM 0x80040155); algorithm selection works in a console session + WAD path; round-trip locked by C++ test_encryption_manager.cpp. Route P5 (env note / WAD). |
| `sec_encrypt_rc4_128_set` | Security | sec_encrypt_rc4_128_set | blocked | EncryptionDialog open-via-keystroke is env-flaky on Server-2019 RDP (COM 0x80040155); algorithm selection works in a console session + WAD path; round-trip locked by C++ test_encryption_manager.cpp. Route P5 (env note / WAD). |
| `sec_encrypt_rc4_40_set` | Security | sec_encrypt_rc4_40_set | blocked | EncryptionDialog open-via-keystroke is env-flaky on Server-2019 RDP (COM 0x80040155); algorithm selection works in a console session + WAD path; round-trip locked by C++ test_encryption_manager.cpp. Route P5 (env note / WAD). |

## P5-FIXTURE (1)

| test_id | subsystem | element | current | disposition justification |
|---|---|---|---|---|
| `sec_sign_verify_invalid` | Security | sec_sign_verify_invalid | blocked | a tampered-signature fixture is creatable (byte manipulation); route to P5 fixture work rather than wont-fix. |

## REMEASURE (144)

| test_id | subsystem | element | current | disposition justification |
|---|---|---|---|---|
| `act_accessibilityAction` | Actions | action::accessibilityAction | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `act_altTextAction` | Actions | action::altTextAction | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `act_batchAction` | Actions | action::batchAction | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `act_compareAction` | Actions | action::compareAction | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `act_complianceAction` | Actions | action::complianceAction | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `act_customizeToolbarsAction` | Actions | action::customizeToolbarsAction | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `act_documentPermissionsAction` | Actions | action::documentPermissionsAction | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `act_ocrAction` | Actions | action::ocrAction | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `act_openAltTextDialogAction` | Actions | action::openAltTextDialogAction | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `act_optimizePdfAction` | Actions | action::optimizePdfAction | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `act_preferencesAction` | Actions | action::preferencesAction | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `act_preflightAction` | Actions | action::preflightAction | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `act_removePasswordAction` | Actions | action::removePasswordAction | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `act_requestValidationAction` | Actions | action::requestValidationAction | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `act_saveWithEncryptionAction` | Actions | action::saveWithEncryptionAction | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `act_signDocumentAction` | Actions | action::signDocumentAction | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `act_themeDarkAction` | Actions | action::themeDarkAction | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `act_themeLightAction` | Actions | action::themeLightAction | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `act_themeSystemAction` | Actions | action::themeSystemAction | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `act_themeToggleAction` | Actions | action::themeToggleAction | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `act_toggleCopyPreventionAction` | Actions | action::toggleCopyPreventionAction | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `act_toggleOcrBoxesAction` | Actions | action::toggleOcrBoxesAction | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `act_togglePrintPreventionAction` | Actions | action::togglePrintPreventionAction | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `act_trustAnchorAction` | Actions | action::trustAnchorAction | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `act_verifySignaturesAction` | Actions | action::verifySignaturesAction | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `alttext_dialog_closes` | AltText | altTextDialog.closes_cleanly | blocked | gtest row (headless); passes in the Release gtest suite. Demoted by the blanket UI-runner reconcile which never measures gtest. Re-measure via gtest. |
| `alttext_dialog_opens` | AltText | altTextDialog.dialog_opens | blocked | gtest row (headless); passes in the Release gtest suite. Demoted by the blanket UI-runner reconcile which never measures gtest. Re-measure via gtest. |
| `alttext_dialog_widget_audit` | AltText | altTextDialog.widget_audit | blocked | gtest row (headless); passes in the Release gtest suite. Demoted by the blanket UI-runner reconcile which never measures gtest. Re-measure via gtest. |
| `calcorder_dialog_closes` | CalcOrder | calculationOrderDialog.closes_cleanly | blocked | gtest row (headless); passes in the Release gtest suite. Demoted by the blanket UI-runner reconcile which never measures gtest. Re-measure via gtest. |
| `calcorder_dialog_opens` | CalcOrder | calculationOrderDialog.dialog_opens | blocked | gtest row (headless); passes in the Release gtest suite. Demoted by the blanket UI-runner reconcile which never measures gtest. Re-measure via gtest. |
| `calcorder_dialog_widget_audit` | CalcOrder | calculationOrderDialog.widget_audit | blocked | gtest row (headless); passes in the Release gtest suite. Demoted by the blanket UI-runner reconcile which never measures gtest. Re-measure via gtest. |
| `cmp_compliance_dialog_levels` | Compliance | scenario::cmp_compliance_dialog_levels | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `cmp_compliance_target_levels` | Compliance | scenario::cmp_compliance_target_levels | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `cmp_convert_pdfa_1b` | Compliance | flow::cmp_convert_pdfa_1b | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `cmp_convert_pdfa_2b` | Compliance | flow::cmp_convert_pdfa_2b | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `cmp_validate_pdfua` | Compliance | flow::cmp_validate_pdfua | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `batch_dialog_closes_cleanly` | Dialogs | batchDialog.closes_cleanly | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `batch_dialog_opens` | Dialogs | batchDialog.opens | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `batch_dialog_widget_audit` | Dialogs | batchDialog.widget_audit | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `combine_wizard_closes_cleanly` | Dialogs | combineFilesWizard.closes_cleanly | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `combine_wizard_opens` | Dialogs | combineFilesWizard.opens | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `combine_wizard_widget_audit` | Dialogs | combineFilesWizard.widget_audit | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `compare_dialog_closes_cleanly` | Dialogs | compareDialog.closes_cleanly | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `compare_dialog_opens` | Dialogs | compareDialog.opens | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `compare_dialog_widget_audit` | Dialogs | compareDialog.widget_audit | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `customize_toolbars_closes_cleanly` | Dialogs | customizeToolbarsDialog.closes_cleanly | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `customize_toolbars_opens` | Dialogs | customizeToolbarsDialog.opens | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `customize_toolbars_widget_audit` | Dialogs | customizeToolbarsDialog.widget_audit | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `dlg_complianceDialog_progressBar` | Dialogs | complianceDialog.progressBar | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `dlg_printPreviewDialog_fitPageAction` | Dialogs | printPreviewDialog.fitPageAction | blocked | note reports accessible-name fallback finds the Qt-internal toolbar button; re-measure via the fallback path. |
| `dlg_printPreviewDialog_fitWidthAction` | Dialogs | printPreviewDialog.fitWidthAction | blocked | note reports accessible-name fallback finds the Qt-internal toolbar button; re-measure via the fallback path. |
| `dlg_printPreviewDialog_zoomInAction` | Dialogs | printPreviewDialog.zoomInAction | blocked | note reports accessible-name fallback finds the Qt-internal toolbar button; re-measure via the fallback path. |
| `dlg_printPreviewDialog_zoomOutAction` | Dialogs | printPreviewDialog.zoomOutAction | blocked | note reports accessible-name fallback finds the Qt-internal toolbar button; re-measure via the fallback path. |
| `metadata_dialog_closes_cleanly` | Dialogs | metadataDialog.closes_cleanly | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `metadata_dialog_opens` | Dialogs | metadataDialog.opens | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `metadata_dialog_widget_audit` | Dialogs | metadataDialog.widget_audit | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `optimize_dialog_closes_cleanly` | Dialogs | optimizeDialog.closes_cleanly | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `optimize_dialog_opens` | Dialogs | optimizeDialog.opens | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `optimize_dialog_widget_audit` | Dialogs | optimizeDialog.widget_audit | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `preflight_dialog_closes_cleanly` | Dialogs | preflightDialog.closes_cleanly | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `preflight_dialog_opens` | Dialogs | preflightDialog.opens | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `preflight_dialog_widget_audit` | Dialogs | preflightDialog.widget_audit | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `print_dialog_closes_cleanly` | Dialogs | printDialog.closes_cleanly | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `print_dialog_opens` | Dialogs | printDialog.opens | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `print_dialog_widget_audit` | Dialogs | printDialog.widget_audit | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `print_preview_closes_cleanly` | Dialogs | printPreviewDialog.closes_cleanly | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `print_preview_opens` | Dialogs | printPreviewDialog.opens | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `print_preview_widget_audit` | Dialogs | printPreviewDialog.widget_audit | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `tab_order_dialog_closes_cleanly` | Dialogs | tabOrderDialog.closes_cleanly | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `tab_order_dialog_opens` | Dialogs | tabOrderDialog.opens | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `tab_order_dialog_widget_audit` | Dialogs | tabOrderDialog.widget_audit | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `watermark_dialog_closes_cleanly` | Dialogs | watermarkDialog.closes_cleanly | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `watermark_dialog_opens` | Dialogs | watermarkDialog.opens | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `watermark_dialog_widget_audit` | Dialogs | watermarkDialog.widget_audit | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `menu_preferences_open` | Edit | Edit > Preferences | blocked | preferencesAction exists (MainWindow.cpp:2575) -> feature exists (note was stale); re-measure recovers to green. |
| `menu_replace_open` | Edit | Edit > Replace | blocked | replaceAction exists (MainWindow.cpp:2560) -> feature exists (note was stale); re-measure recovers to green. |
| `sec_open_encrypted` | Encryption | flow::sec_open_encrypted | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `flow_flow_corpus_robustness` | Flows | OpenCorpusRobustness | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `flow_flow_form_fill_save` | Flows | FormFieldFillAndSave | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `field_checkbox_set_default` | Forms | field_Checkbox | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `field_combobox_set_default` | Forms | field_ComboBox | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `field_listbox_set_default` | Forms | field_ListBox | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `field_pushbutton_create` | Forms | field_PushButton | blocked | formPushButtonAction on formToolBar_ (MainWindow.cpp:3891); sets PushButton creation type (7528) -> feature exists (note was stale); re-measure recovers to green. |
| `field_pushbutton_fill_value` | Forms | field_PushButton | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `field_pushbutton_set_default` | Forms | field_PushButton | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `field_pushbutton_xfdf_roundtrip` | Forms | field_PushButton | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `field_radiobutton_set_default` | Forms | field_RadioButton | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `field_signature_fill_value` | Forms | field_Signature | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `field_signature_set_default` | Forms | field_Signature | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `field_signature_xfdf_roundtrip` | Forms | field_Signature | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `field_text_set_default` | Forms | field_Text | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `menu_submit_form` | Forms | Forms > SubmitForm | blocked | submitFormAction exists (MainWindow.cpp:2929) -> feature exists (note was stale); re-measure recovers to green. |
| `menu_tab_order` | Forms | Forms > TabOrder | blocked | tabOrderAction exists (MainWindow.cpp:2788) -> feature exists (note was stale); re-measure recovers to green. |
| `hf_dialog_closes` | HeaderFooter | headerFooterDialog.closes_cleanly | blocked | gtest row (headless); passes in the Release gtest suite. Demoted by the blanket UI-runner reconcile which never measures gtest. Re-measure via gtest. |
| `hf_dialog_opens` | HeaderFooter | headerFooterDialog.dialog_opens | blocked | gtest row (headless); passes in the Release gtest suite. Demoted by the blanket UI-runner reconcile which never measures gtest. Re-measure via gtest. |
| `hf_dialog_widget_audit` | HeaderFooter | headerFooterDialog.widget_audit | blocked | gtest row (headless); passes in the Release gtest suite. Demoted by the blanket UI-runner reconcile which never measures gtest. Re-measure via gtest. |
| `pagelabels_dialog_closes` | PageLabels | pageLabelsDialog.closes_cleanly | blocked | gtest row (headless); passes in the Release gtest suite. Demoted by the blanket UI-runner reconcile which never measures gtest. Re-measure via gtest. |
| `pagelabels_dialog_opens` | PageLabels | pageLabelsDialog.dialog_opens | blocked | gtest row (headless); passes in the Release gtest suite. Demoted by the blanket UI-runner reconcile which never measures gtest. Re-measure via gtest. |
| `pagelabels_dialog_widget_audit` | PageLabels | pageLabelsDialog.widget_audit | blocked | gtest row (headless); passes in the Release gtest suite. Demoted by the blanket UI-runner reconcile which never measures gtest. Re-measure via gtest. |
| `page_cropbox_preserved` | Pages | page_cropbox_preserved | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `page_extract_to_encrypted_target` | Pages | page_extract_to_encrypted_target | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `page_insert_blank_before` | Pages | page_insert_blank_before | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `page_insert_blank_end` | Pages | page_insert_blank_end | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `page_insert_from_encrypted` | Pages | page_insert_from_encrypted | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `page_insert_from_file_range` | Pages | page_insert_from_file_range | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `page_move_to_end` | Pages | page_move_to_end | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `page_multi_select_ctrl_click` | Pages | page_multi_select_ctrl_click | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `page_multi_select_shift_click` | Pages | page_multi_select_shift_click | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `page_page_resize_ignored` | Pages | page_page_resize_ignored | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `page_rotate_respects_annotations` | Pages | page_rotate_respects_annotations | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `pref_dialog_closes` | Preferences | preferencesDialog.closes_cleanly | blocked | gtest row (headless); passes in the Release gtest suite. Demoted by the blanket UI-runner reconcile which never measures gtest. Re-measure via gtest. |
| `pref_dialog_opens` | Preferences | preferencesDialog.dialog_opens | blocked | gtest row (headless); passes in the Release gtest suite. Demoted by the blanket UI-runner reconcile which never measures gtest. Re-measure via gtest. |
| `pref_dialog_widget_audit` | Preferences | preferencesDialog.widget_audit | blocked | gtest row (headless); passes in the Release gtest suite. Demoted by the blanket UI-runner reconcile which never measures gtest. Re-measure via gtest. |
| `pref_set_roundtrip` | Preferences | scenario::pref_set_roundtrip | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `pref_state_advanced` | Preferences | scenario::pref_state_advanced | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `pref_state_general` | Preferences | scenario::pref_state_general | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `pref_state_ocr` | Preferences | scenario::pref_state_ocr | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `pref_state_security` | Preferences | scenario::pref_state_security | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `pref_state_view` | Preferences | scenario::pref_state_view | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `pref_tabs_present` | Preferences | scenario::pref_tabs_present | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `txt_embedded_italic_serif_overlay` | RenderVerify | render_textedit_embedded_overlay | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `txt_embedded_new_char_serif_italic` | RenderVerify | render_textedit_new_char_font | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `txt_per_line_font_size_ui` | RenderVerify | render_per_line_font_size | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `txt_vector_heading_commit_visible` | RenderVerify | render_vector_heading_commit | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `sec_encrypt_aes_128_reopen` | Security | sec_encrypt_aes_128_reopen | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `sec_encrypt_permissions_copy_blocked` | Security | sec_encrypt_permissions_copy_blocked | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `sec_encrypt_permissions_print_blocked` | Security | sec_encrypt_permissions_print_blocked | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `sec_encrypt_rc4_128_reopen` | Security | sec_encrypt_rc4_128_reopen | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `sec_encrypt_rc4_40_reopen` | Security | sec_encrypt_rc4_40_reopen | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `sec_security_password_strength_meter` | Security | sec_security_password_strength_meter | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `sec_sign_timestamp_present` | Security | sec_sign_timestamp_present | blocked | TSA group exists: tsaCheckBox + tsaUrlEdit wired to opts.useTimestamp/tsaUrl (SignDialog.cpp:144-261) -> feature exists (note was stale); re-measure recovers to green. |
| `sec_ltv_enable` | Signatures | flow::sec_ltv_enable | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `sec_ltv_request_dialog` | Signatures | scenario::sec_ltv_request_dialog | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `sec_sign_real` | Signatures | flow::sec_sign_real | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `sec_verify_signed` | Signatures | scenario::sec_verify_signed | blocked | carries a passing-assertion note and the reconcile tag (run 28465445439/28554887271, the same corrupt aggregate as P1); re-measure recovers. |
| `menu_tools_accessibility` | Tools | Tools > Accessibility | blocked | accessibilityAction exists (MainWindow.cpp:3179) -> feature exists (note was stale); re-measure recovers to green. |
| `menu_tools_batch` | Tools | Tools > BatchProcess | blocked | batchAction exists (MainWindow.cpp:3203) -> feature exists (note was stale); re-measure recovers to green. |
| `menu_tools_compare` | Tools | Tools > Compare | blocked | compareAction exists (MainWindow.cpp:3212) -> feature exists (note was stale); re-measure recovers to green. |
| `menu_tools_ocr` | Tools | Tools > OCR | blocked | ocrAction exists in toolsMenu (MainWindow.cpp:3111,3591) -> feature exists (note was stale); re-measure recovers to green. |
| `menu_tools_tre` | Tools | Tools > TRE | blocked | treAction exists (MainWindow.cpp:3170) -> feature exists (note was stale); re-measure recovers to green. |
| `menu_view_theme_toggle` | View | View > Theme | blocked | themeToggleAction exists (MainWindow.cpp:3279); themeMenu also present -> feature exists (note was stale); re-measure recovers to green. |
| `theme_switch_dark` | View | theme_switch_dark | blocked | themeMenu wired: viewMenu->addMenu('&Theme') + themeDarkAction (MainWindow.cpp:3305,3761-3765) -> feature exists (note was stale); re-measure recovers to green. |
| `theme_switch_light` | View | theme_switch_light | blocked | themeMenu wired: themeLightAction (MainWindow.cpp:3296,3761-3765) -> feature exists (note was stale); re-measure recovers to green. |
| `theme_switch_system` | View | theme_switch_system | blocked | themeMenu wired: themeSystemAction (MainWindow.cpp:3314,3761-3765) -> feature exists (note was stale); re-measure recovers to green. |
