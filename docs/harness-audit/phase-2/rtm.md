# PDFE Harness Adversarial Audit — Phase-2 Requirements Traceability Matrix (RTM)

**Stance:** the harness is the defendant. This RTM maps every user-facing capability
(from `README.md` Features, `docs/pdfe-vs-acrobat-pro-comparison.md`, and the UI action
surface) to the tests that verify it, then runs a two-way gap analysis. FIND-AND-DOCUMENT
only — no product or test code was changed.

**Inputs:** `README.md`, `docs/pdfe-vs-acrobat-pro-comparison.md`,
`docs/harness-audit/census.json` (402 C++ files, 4,365 source tests, per-file assert
density), `docs/harness-audit/phase-1/oracle-audit-synthesis.json` (per-subsystem oracle
classes + ~55 ranked weak-oracle findings).

## Coverage-class rubric

| class | meaning |
|---|---|
| `none` | no test found |
| `happy-path-only` | only positive / valid-input tests |
| `has-negative-or-boundary` | at least one negative / malformed / boundary test file maps to the capability (**orthogonal to oracle strength**) |
| `strong-with-visual-oracle` | verified against a rendered bitmap or a reopened saved artifact's actual content — not extraction-present / no-throw |

`oracle_note` cites Phase-1 where the covering tests are weak-oracle (proxy / no-oracle-smoke
/ tautological / conditionally-skipped). **A row can be `has-negative-or-boundary` and still
have a weak oracle** — the two axes are independent.

## Headline numbers

- **87 capability rows.** Distribution: **strong-with-visual-oracle = 1**,
  **has-negative-or-boundary = 18**, **happy-path-only = 68**, **none = 0**.
  *(Corrected per validator phase-2 gate: totals recomputed from the actual array;
  distribution sums to 87.)*
- **none = 0 is the key finding inverted:** every shipped feature has a test *file*. The gap
  is oracle **strength**, not test **existence**. Phase-1 already found **36.6 % of files /
  39.2 % of tests are weak-oracle**; this RTM shows that weakness concentrates on **visual**
  and **security-enforcement** capabilities — precisely where a user notices a regression.
- Roadmap / out-of-scope items (public-key encryption, scanner input, Office/HTML import,
  Read-Out-Loud, spell-check, tag-tree/reading-order editor, Portfolio creation, Action-wizard
  macros) are **not** counted as gaps — they are unimplemented by design.

---

## RTM (one row per capability)

Legend for coverage: **S** strong-with-visual-oracle · **N** has-negative-or-boundary ·
**H** happy-path-only · **0** none. Impact: user-visible severity if the capability broke.

| Capability | Source | Cov | Impact | Verifying tests (representative) | Oracle note (Phase-1) |
|---|---|:--:|:--:|---|---|
| Open / view / close | R/C/UI | N | High | test_document, test_external_flaw_corpus(47), test_realworld_corpus, test_malicious_pdf_advanced | corpus present; malicious oracles no-throw only |
| Save As (full rewrite) | R/C/UI | N | High | test_save, test_save_operations, test_document_save_atomic, test_save_error_paths | SaveClearsDirtyPages reopens-without-error only — silent content-drop passes |
| Incremental save | R/C | H | High | test_incremental_save, test_incremental_save_routing | — |
| Linearization | R/C | H | Med | test_linearization | 2 tests; "linearized" not proven |
| Merge / Combine Files | R/C/UI | H | Med | test_combine_files_wizard | — |
| Extract pages to new PDF | R/C/UI | H | Med | test_page_operations | ExtractPages_ReverseOrder checks only pageCount — wrong pages ship green |
| Create PDF from images | R/C/UI | H | Med | test_image_to_pdf_wizard | — |
| Print + preview | R/C/UI | H | Med | test_printing, test_fidelity_r3_21_print_vs_screen | 10 SKIP sites on printer-less runner |
| Zoom / pan / fit | R/C/UI | H | Med | test_hidpi, test_hidpi_render, test_pdf_canvas_q_invokable | **HiDPI/TileCoordinate TAUTOLOGICAL** — renderer math never called |
| View rotation | R/C/UI | H | Low | test_view_rotation | — |
| Page rotation (persisted) | R/C/UI | H | Med | test_page_commands, test_page_ops_phase6 | — |
| Thumbnails | R/C/UI | H | Low | test_thumbnails, test_thumbnail_cache_cancel | — |
| Bookmarks CRUD | R/C/UI | H | Med | test_bookmark_manager, test_bookmark_panel_context_menu | InsertBookmark no-op returns true → passes |
| Layers (OCG) | R/C/UI | H | Med | test_layers | whole subsystem PROXY (0 true oracles) |
| Full-text search | R/C/UI | H | **High** | test_search, test_tantivy_search, test_search_performance | **totalMatches()>=0 tautology** — zero-hits invisible |
| Find + Replace | R/C/UI | H | Med | test_find_replace | 2 tests for a mutating feature |
| Links (URI/GoTo/Named/Launch) | R/C/UI | N | Med | test_launch_link_follow, test_link_actions, test_link_tool | Launch has real negatives; test_link_tool density 0.8 |
| Page labels | R/C/UI | H | Low | test_page_labels_editor | — |
| Navigation history | R/C/UI | H | Low | test_navigation_history | — |
| Sticky notes | R/C/UI | H | Med | test_sticky_note_popup, test_annotation_commands | — |
| Highlight/underline/strike | R/C/UI | H | Med | test_annotation_tools, test_annotation_roundtrip | HighlightFromSelection assertion-free — broken tool passes |
| Ink / freehand | R/C/UI | H | Low | test_annotation_tools | — |
| Shapes | R/C/UI | H | Low | test_annotation_tools, test_annotations_phase4 | — |
| Free text / text box | R/C/UI | H | Med | test_annotations_phase4, test_annotation_font_controls | DA color never checked; font-size spinner test self-skips |
| Stamps (static/dynamic/image) | R/C/UI | H | Med | test_stamp_and_watermark, test_stamp_tokens, test_image_stamp | — |
| Comment panel + threading | R/C/UI | H | Med | test_comment_panel, test_comment_panel_threading | — |
| Measurement tools | R/C/UI | H | Med | test_measure | density 2.0; numeric correctness light |
| File-attachment annotations | R/C/UI | H | Med | test_file_attachment_annotation, test_attachments | — |
| Alt text / ActualText (annot) | R/C/UI | H | Med | test_alt_text, test_alt_text_dialog | — |
| Form field types (7) | R/C/UI | N | Med | test_form_manager, test_form_creation, test_forms_phase5 | RadioGroupExclusivity never re-reads sibling; SignatureWidgetType tautology+SUCCEED |
| Form designer (drag/props) | R/C/UI | H | Med | test_form_tool, test_form_field_properties_panel | — |
| Tab-order editor | R/C/UI | H | Low | test_tsa_and_taborder | — |
| Calc-order (/CO) editor | R/C/UI | H | Low | test_calculation_order | — |
| Per-field JavaScript | R/C/UI | N | Med | test_javascript_engine, test_form_javascript_xss | CalculateAll no-throw; FireValidate/Format tautology |
| Document-level JS events | R/C | H | Med | test_document_js_events | WillSave only no-throw; can't observe script ran |
| FDF / XFDF import/export | R/C/UI | H | Med | test_form_data_io, test_xfdf_encode_decode | — |
| Form submit (URL / POST) | R/C | N | Med | test_form_submit, test_form_submitter | host-allowlist rejection = negative |
| Flatten form | R/C/UI | H | Med | test_form_commands, test_form_roundtrip | — |
| Reset form (/DV) | R/C/UI | H | Low | test_form_manager, test_form_commands | SetFieldValue in-memory only, never saved+reopened |
| Password encryption (RC4/AES) | R/C/UI | N | High | test_encryption_manager, test_encryption_roundtrip, test_encryption_settings | roundtrip decrypts artifact; wrong-pw negatives |
| Permission flags | R/C/UI | N | Med | test_permission_enforcement, test_encryption_roundtrip | **CORRECTED (karen gate): copy/print restriction IS enforced by real behavioral tests (engine + UI refusal, mutation-killable); residual gap is narrow — encryption_roundtrip flag-read-only + saveEncrypted re-encryption path** |
| **Redaction (true removal)** | R/C/UI | **S** | **High** | test_redaction_edge_cases(77), test_redaction_completeness, test_image_redaction_phase7, test_redaction_verifier | **strongest byte-absence oracles GTEST_SKIP under fixture fallback; verifier never asserted FALSE** |
| Digital signatures (PKCS#7) | R/C/UI | N | **High** | test_signature_manager, test_signature_roundtrip, test_signature_security_phase2, test_pkcs7_fuzz | **trust chain never positively validated; tamper test may not tamper** |
| CA-chain signing | R/C | H | Med | test_signature_roundtrip, test_certification_signatures | self-signed satisfies the predicate |
| Certification sigs (/DocMDP) | R/C/UI | H | **High** | test_certification_signatures, test_docmdp_byterange | **enforcement never tested negatively — disallowed edit not flagged** |
| Invisible signatures | R/C/UI | H | Low | test_invisible_and_certification_signatures | density 1.75 |
| RFC 3161 timestamps | R/C/UI | H | Med | test_tsa_real_endpoint, test_tsa_fixture | token OID printed not asserted; TsaGroup widget-only |
| LTV / PAdES-B-LT (DSS/VRI) | R/C/UI | N | **High** | test_ltv_validator, test_ocsp_crl_validation, test_ocsp_crl_parser | **staleness never deciding; forged-GOOD OCSP not rejected** |
| Malformed-PDF sandbox | R/C | N | **High** | test_cve_replay(21), test_malicious_pdf_advanced, test_malicious_pdf_security, test_preflight_open | **CRITICAL: 21 CVEs / 1 timing assert; "Blocked/Rejected" never verified** |
| Edit existing text | R/C/UI | N | **High** | test_content_stream_editor, test_text_edit_integration, test_text_color_and_replace, test_content_stream_adversarial_ambient | **extraction-only; MultiplePageEdits 0 asserts; ambient corpus SUCCEED-only** (one red-pixel oracle exists) |
| Font substitution (scoring) | R/C | H | Med | test_font_matcher, test_glyph_scale_adjuster, test_pdf_font_embedder | findMatch style ignored; scale bound 0.5–2.0; embedder never rasterizes |
| Rich-text formatting | R/C/UI | H | Med | test_formatting_run, test_text_edit_toolbar, test_text_edit_save_roundtrip | Bold oracle = nameMentionsBold\|\|nameDiffers |
| **Text alignment (L/C/R/Justify)** | R/C/UI | H | **High** | test_alignment_writer, test_alignment_injector, test_alignment_pipeline, test_alignment_e2e | **visual; extraction-only, position never asserted; Justify substring bug** |
| **TRE scanned-text edit** | R/C | H | **High** | test_region_reconstructor, test_tre_commands, test_tre_coord_mapper, test_scan_style_transfer | **no render/re-extract oracle; Y-flip "critical" tests tautological** |
| OCR (Tesseract) | R/C/UI | H | **High** | test_ocr_pipeline, test_hocr_parser, test_ocr_all_pages_command | gibberish passes (no known-word check); DPI >100 over-tolerant |
| Multi-language OCR | R/C/UI | H | Low | test_ocr_language_preference, test_ocr_language_routing | — |
| Visual font identification | R/C | H | Med | test_visual_font_ranker, test_cnn_font_classifier | ranking check wrapped in `if(arialFirst)` — neutered on failure |
| Glyph atlas synthesis | R/C | H | Med | test_glyph_synthesizer, test_glyph_atlas_compositor, test_vector_font_atlas_builder | HFlip never pixel-checked; blank canvas passes; identical A/B/C fixtures |
| PDF/A convert + validate | R/C/UI | H | Med | test_pdfa_converter, test_pdfa_validator, test_xmp_metadata_writer | pdfaid part/conformance not pinned (substring '1'&'B') |
| PDF/UA + auto-tagging | R/C/UI | H | Med | test_structure_tree_builder, test_auto_tagger, test_content_stream_tagger | drops Figure/altText passes; PreservesRendering = pageCount>0 |
| Preflight + Fix All | R/C/UI | H | Med | test_preflight_fix_all, test_compliance_dialog, test_prohibited_feature_scanner | broken scanner (detects nothing) passes |
| ICC color profiles | R/C | H | Low | test_icc_profile_manager | density 2.0 |
| Font-embedding compliance | R/C | H | Med | test_font_compliance_checker | `allCompliant\|\|!allCompliant` tautology |
| Theme (light/dark/high-contrast) | R/C/UI | H | Low | test_high_contrast, test_theme_and_field_count | — |
| Insert / delete / replace image | R/C/UI | H | Med | test_image_manager, test_image_content_editor, test_image_replacer | no-op indistinguishable; ImageCommandsDirty uses displayList!=null |
| Resize / rotate / crop image | R/C/UI | N | **High** | test_image_content_editor, test_image_flow_oracle, test_image_ctm_leak, test_mediabox_origin_crop | **mirror/half-size-under-CTM passes**; ctm_leak/mediabox pin geometry |
| EXIF stripping | R/C/UI | H | Med | test_exif_stripper | PDF-path never reopens image bytes — no-op strip passes |
| Grayscale conversion | R/C/UI | H | Low | test_grayscale_converter | — |
| Batch export PNG/JPG | R/C/UI | H | Low | test_image_exporter | — |
| Optimize / compress | R/C/UI | H | Med | test_optimize, test_save_recompression, test_conditional_gc | named deleted-value leak ships green (pageCount>0 only) |
| Page insert/delete/reorder/replace | R/C/UI | N | Med | test_page_commands, test_replace_page, test_page_integer_overflow | Replace no-op passes; DeleteAtInvalidIndex assertion-free |
| Crop pages (/CropBox) | R/C/UI | N | Med | test_crop_tool, test_mediabox_origin_crop | non-zero-origin boundary pinned |
| Headers/footers + Bates | R/C/UI | N | Med | test_header_footer, test_bates_tokens, test_hf_watermark_adversarial | adversarial-ambient negatives; bates density 1.4 |
| Watermarks (text+image, range) | R/C/UI | N | Med | test_watermark_extended, test_watermark_state_leak, test_hf_watermark_adversarial | page-range contract never asserted (all/none passes) |
| Metadata (Info) + XMP | R/C/UI | H | Med | test_metadata_manager, test_metadata_roundtrip, test_xmp_metadata_writer | WriteMetadata in-memory dict, not serialized (only Title verified) |
| Attachments panel + Portfolio detect | R/C/UI | H | Low | test_attachment_panel, test_attachments, test_portfolio | portfolio density 1.67 |
| Plain-text export | R/C/UI | H | Med | test_export_plain_text | — |
| Word (DOCX/RTF) export (engine) | R/C | H | Med | test_word_export | BoldIsEmitted final assert is `<w:document` shell |
| Excel (XLSX) export (engine) | R/C | H | Med | test_excel_export | valid-zip only; empty/garbled sheets pass |
| Batch processing | R/C/UI | H | Med | test_batch | 33 tests, density 2.42 |
| Document diff / compare | R/C/UI | H | Med | test_diff, test_compare_documents_view | docA==docB → change path asserted only 0==0 |
| Command-line interface | R/C/UI | H | Med | test_cli | — |
| Undo / redo (all ops) | R/C/UI | N | **High** | test_undo_manager, test_command_page_rebind, test_save_throw_undo_safety | subsystem strong; but thrown-save can undo WRONG op green |
| Customizable toolbars | C/UI | H | Low | test_toolbar_customization | — |
| Recent files | C/UI | H | Low | test_recent_files | — |
| Preferences dialog | C/UI | H | Low | test_preferences_dialog, test_app_settings | — |
| Menu / action wiring | UI | H | Med | test_ui_integration, test_phase_f5_menu_wiring, test_dialog_object_names_phase11 | actions verified to EXIST only — wired-to-nothing passes |

*(R = README, C = comparison chart, UI = action surface.)*

---

## Ranked "uncovered & user-visible" shortlist (by impact)

Ordered worst-first. Every item ships a **green** test suite while the user-visible (or
security) guarantee can be silently broken.

1. **Redaction — true content removal (confidentiality).** The only oracles that prove the
   secret is gone from the saved decompressed stream (`test_redaction_completeness:67`,
   `test_image_redaction_phase7:428`) `GTEST_SKIP` under fixture fallback; `redaction_verifier`
   never asserts `checkTextRemoved==false`. A box-paint-only redaction can ship green.
2. **Malformed-PDF sandbox / CVE neutralization.** `test_cve_replay`: 21 CVE replays, **1
   timing assertion** around a catch-all. `ProcessSpawnAttemptBlocked` /
   `GoToRRemotePathRejected` / `DataUriNotExecuted` assert only no-throw. Re-enabled NTLM leak /
   `/AA` JS exec / font PostScript / SSRF all pass.
3. **Permission enforcement (copy / print) — DOWNGRADED High→Med (karen gate correction).**
   The main copy/print restriction surface IS enforced by real behavioral tests
   (`test_permission_enforcement.cpp`: `PlainTextExporterRefusesCopyDenied`,
   `PrintEngineRefusesPrintDeniedWithValidPrinter`, and the `OnPrint/OnCopy/TextEditCopyCut`
   UI-slot refusals — all invoke product code and assert refusal, mutation-killable). The
   residual gap is narrow: `test_encryption_roundtrip.cpp::CopyFalsePermissionEnforcedOnReopen`
   is flag-read-only, and `saveEncrypted`'s re-encryption path / future downstream consumers
   ignoring `PermissionFlags` have no enforcement test.
4. **Signature trust-chain + tamper + LTV forged-GOOD.** Cert trust never positively
   validated (self-signed satisfies every predicate); tamper test has an operator-precedence
   bug that may write a no-op; no test rejects a GOOD OCSP from an untrusted responder.
5. **Edit existing text — visual correctness.** Extraction-present oracles; mirror / invisible
   / wrong-glyph / neighbour-clobber all pass. `MultiplePageEdits` has **zero** assertions;
   the adversarial-ambient corpus test is `SUCCEED()`-only.
6. **Text alignment (L/C/R/Justify).** Purely visual; every test uses
   `extractPlainText().find()`. Position never asserted; Justify has a `' Tw'` substring bug.
7. **TRE scanned-text reconstruction.** New glyphs written to the stream but nothing
   renders/re-extracts; the "critical" Y-flip coord-mapper tests are tautological.
8. **Image resize / rotate / crop (CTM).** Success asserted via return bool / non-null display
   list; a mirrored or **half-size** image (a repeat regression) is indistinguishable from
   correct.
9. **Autosave / crash-recovery capture.** `test_autosave_capture_audit` (15 edit families) is
   `SUCCEED()`-only, zero EXPECT/ASSERT — a recovery regression that drops all edits is green.
10. **Full-text search — non-empty correctness.** `totalMatches()>=0` tautology; zero-hits for
    present text is invisible.
11. **OCR recognition quality.** hocr-non-empty passes gibberish; DPI `>100` accepts a 72-DPI
    render that degrades recognition.
12. **Optimize — deleted-content leak.** The named leak (stale form-field values after
    garbage=4) ships green under `pageCount>0`.
13. **Menu / action wiring.** Actions verified to exist by objectName; wired-to-nothing passes.
14. **EXIF stripping (privacy).** No-op strip that leaves GPS/camera metadata in the saved
    file passes (bytes never reopened).

---

## Orphan tests (no 1:1 user-facing capability)

Flagged under both interpretations: **internal-only** (legitimately not a user feature, tends
to back a capability) vs **outside-feature-list** (tests something not in the README feature
set — build/CI/meta).

- **Harness bootstrap:** `test_main.cpp` (0 tests, crash-guard install).
- **Coordinate/CTM math (internal):** `test_coordinate_transform`, `test_coord_mapper_rotation`,
  `test_coordinate_diagnostic`, `test_tre_coord_mapper` — back selection & render.
- **MuPDF wrapper (internal):** `test_mupdf_context`, `test_mupdf_context_clone_lifecycle`.
- **Tile render/cache (internal, backs view/zoom):** `test_tile_cache`, `test_tile_prefetcher`,
  `test_tile_renderer`, `test_tile_renderer_leak`, `test_cache_stats`, `test_c2_image_cache`.
- **Infra / reliability (internal):** `test_log_handler`, `test_crash_handler`,
  `test_single_instance_guard`, `test_startup_timer`, `test_probe_extent`,
  `test_process_mitigations`.
- **Build / packaging / CI / branding (outside-feature-list):** `test_dll_dependencies`,
  `test_installer`, `test_ci_fuzz_workflows`, `test_ci_supplychain`, `test_branding_resources`,
  `test_app_resources`, `test_pdfe_icons`, `test_main_window_icon`, `test_version_update`.
- **Path/UNC hardening + compliance internals (internal, backs sandbox/compliance):**
  `test_path_security`, `test_save_path_security`, `test_engine_unc_gate`,
  `test_prohibited_feature_scanner`.
- **Leak / stress / soak (internal reliability):** `test_open_close_leak_cycle`,
  `test_image_undo_after_save`, `test_journey_op99_crash`, `test_stress_investigation`,
  `test_performance_stress` (its `ConcurrentMaliciousPdfRendering` is also a Phase-1 High
  assertion-free finding).
- **Render / TRE sub-algorithms (internal, feed TRE & render quality):**
  `test_block_params_builder`, `test_bg_color_detector`, `test_b3_bg_classification`,
  `test_b3_pattern_copy`, `test_b2_measured_width`, `test_c4_perceptual_alpha`,
  `test_c5_lcd_subpixel`, `test_bitmap_compositor`, `test_lanczos_resize`,
  `test_image_similarity`.
- **Font/OCR analysis primitives (internal):** `test_a2_script_detection`,
  `test_font_metric_profile`, `test_font_analysis_cache`, `test_overlay_font_mapping`,
  `test_hocr_parser`, `test_vector_subset_detector`, `test_embedded_font_analyzer(_multiscript)`,
  `test_substitution_info_model`, `test_atlas_comparison`.
- **Content-stream injector primitives (internal, back text-edit/alignment/watermark):**
  `test_writing_mode_autoplumb`, `test_path_roundtrips`, `test_pdf_content_writer`,
  `test_content_stream_ctm_reset`, `test_content_stream_state_leak`,
  `test_content_stream_multi_injector`, `test_content_stream_out_of_class`,
  `test_content_stream_neg_tz`, `test_content_stream_per_run_emission`.
- **Qt test-seam / panel scaffolding (internal):** `test_*_q_invokable*`,
  `test_q_invokable_seams_phase1`, `test_deferred_panels`, `test_panel_interaction_phase8`,
  `test_select_tool_enhanced`, `test_canvas_stale_layout`.
- **Cross-cutting fidelity probes (span many capabilities):** `test_render_fidelity`,
  `test_fidelity_r3_06..24_*`.
- **Fuzz / CVE / audit harnesses (internal security-harness):** `test_gap102_residuals`,
  `test_fuzz_harness_smoke`, `test_font_fuzz` (0 asserts), `test_pkcs7_fuzz` (0.25),
  `test_cve_replay` (0.05), `test_security_audit` — behavioral oracle is essentially non-crash.

**Interpretation note.** Most "orphans" are legitimately internal seams beneath a real
capability — their existence is fine. The audit concern is that several of them are *also*
the weak-oracle no-crash harnesses (`font_fuzz`, `pkcs7_fuzz`, `cve_replay`) that give the
appearance of security/robustness coverage without a behavioral oracle.
