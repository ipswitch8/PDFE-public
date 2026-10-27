# Render-Verification Triage Results — Pipeline e7d3c2a1, Phase 4

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


**Generated:** 2026-06-22
**Binary under test:** `build/vs2022/bin/Release/pdfe.exe` (git `4b7db73`, Release)
**Runner:** `tests/scripts/run_render_suite.py` (per-file pytest-subprocess isolation)
**Raw artifacts:** `tests/_render_triage_phase4/` — `render-junit-merged.xml`,
`render-suite-summary.json`, `per-file/*.xml`, `per-file/*.log`
**Wall clock:** 948.1 s (~15.8 min), 9 test files

This is the honest empirical ground truth: every `test_render_*.py` test executed
against the real Release binary, classified PASS / XFAIL / XPASS / FAIL / ERROR.
Per project rule GAP-A15 the verdicts are parsed from the **JUnit XML + per-file
pytest logs**, never from a process exit code.

---

## 1. Summary

| Metric | Count |
|---|---|
| **Total tests** | **39** |
| PASS | 27 |
| XFAIL (expected failure held) | 6 |
| XPASS (expected-failure test passed this run) | 6 |
| **FAIL (unexpected)** | **0** |
| **ERROR (infra)** | **0** |
| Files with problems (timeout / non-zero rc) | 0 |

- **Zero unexpected FAILs and zero ERRORs.** Every test either passed, or carried
  an `xfail` marker that resolved to XFAIL or XPASS.
- The 9 files = the 8 mandated category suites + `test_render_oracle_proof.py`
  (the Phase 2 proof cases, which match the `test_render_*` glob).

### XFAIL vs XPASS interpretation (read before the matrix)

The 12 expected-failure outcomes split into two behaviourally distinct groups:

- **Deterministic defect proofs — held as XFAIL (4 distinct defects, 6 rows):**
  `#2` mixed-DPI smear (×2: images + oracle-proof), `#3` Arabic OCR garble,
  `#4` mixed-content tool-routing (×2: text-edit + oracle-proof), and Tier-C
  `gap #20` theme-toggle no-repaint. These fail **every** run because the
  underlying behaviour is simply not implemented yet (per-region DPI, per-region
  OCR language, per-region routing) or is a deterministic SSIM-1.0 no-op (theme).
  They are the load-bearing red proofs the Phase 5/6 fixes must turn green.

- **`#1`-class intermittent proofs — XPASSed this run (5 rows) + 1 scanned (1 row):**
  the five `#1` vector-text-edit-invisible / undo / redo tests and the scanned
  recomposite test are marked `xfail(strict=False)` because the defect reproduces
  **intermittently under UI automation** (PrintWindow capture vs. commit/undo
  re-render timing). This triage run happened to catch frames where the heading
  was visible, so they registered XPASS. `strict=False` means an XPASS is **not**
  a suite failure — it is the documented, expected oscillation. The **deterministic**
  oracle for `#1`/`#5` is the Phase 5 gtest (`test_text_edit_span_provenance.cpp`),
  not this UI layer. The `#1` defect itself is real and visually proven (see the
  Phase 1 gap report §5 and the Phase 3 commit notes); the non-determinism is in
  the *automation's ability to observe it*, not in whether the bug exists.

This XFAIL/XPASS split is exactly why Phase 4 exists: it makes the empirical,
run-to-run behaviour explicit instead of trusting a single green/red snapshot.

---

## 2. Per-test results matrix

`result` is this run's observed outcome; `expected` is the test's marker intent.
`screenshot_path` is `—` for all rows because there were no unexpected FAIL/ERROR
results to capture (see `tests/ui_automation/screenshots/triage/README.md`).

| test_id | category | defect_ref | expected | result | screenshot_path |
|---|---|---|---|---|---|
| test_render_annotations::test_annotation_undo_reverts_canvas | annotations | — | PASS | PASS | — |
| test_render_annotations::test_freetext_create_changes_canvas | annotations | — | PASS | PASS | — |
| test_render_annotations::test_highlight_markup_changes_canvas | annotations | — | PASS | PASS | — |
| test_render_annotations::test_ink_draw_changes_canvas | annotations | — | PASS | PASS | — |
| test_render_annotations::test_rectangle_shape_changes_canvas | annotations | — | PASS | PASS | — |
| test_render_annotations::test_sticky_note_create_changes_canvas | annotations | — | PASS | PASS | — |
| test_render_images::test_convert_to_grayscale_changes_canvas | images | — | PASS | PASS | — |
| test_render_images::test_mixed_dpi_region_not_smeared | images | #2 | XFAIL | **XFAIL** | — |
| test_render_ocr::test_arabic_block_ocr_contains_arabic_script | ocr | #3 | XFAIL | **XFAIL** | — |
| test_render_ocr::test_latin_block_ocr_recognizes_latin | ocr | — | PASS | PASS | — |
| test_render_oracle_proof::test_ctrl_z_reverts_canvas | oracle-proof | #1 | XFAIL | XPASS | — |
| test_render_oracle_proof::test_mixed_content_scanned_region_routable | oracle-proof | #4 | XFAIL | **XFAIL** | — |
| test_render_oracle_proof::test_mixed_dpi_region_not_smeared | oracle-proof | #2 | XFAIL | **XFAIL** | — |
| test_render_oracle_proof::test_oracle_self_check | oracle-proof | — | PASS | PASS | — |
| test_render_oracle_proof::test_vector_text_edit_visible | oracle-proof | #1 | XFAIL | XPASS | — |
| test_render_page_ops::test_delete_page_updates_count_and_canvas | page-ops | — | PASS | PASS | — |
| test_render_page_ops::test_insert_blank_page_updates_count_and_canvas | page-ops | — | PASS | PASS | — |
| test_render_page_ops::test_move_page_down_changes_first_page_canvas | page-ops | — | PASS | PASS | — |
| test_render_page_ops::test_rotate_page_180_changes_canvas | page-ops | — | PASS | PASS | — |
| test_render_page_ops::test_rotate_page_cw_changes_canvas | page-ops | — | PASS | PASS | — |
| test_render_redaction::test_redaction_mark_paints_on_canvas | redaction | — | PASS | PASS | — |
| test_render_redaction::test_redaction_mark_undo_reverts_canvas | redaction | — | PASS | PASS | — |
| test_render_signatures::test_signature_field_placement_changes_canvas | signatures | — | PASS | PASS | — |
| test_render_signatures::test_signature_field_undo_reverts_canvas | signatures | — | PASS | PASS | — |
| test_render_text_edit::test_form_field_edit_undo_reverts_canvas | text-edit | — | PASS | PASS | — |
| test_render_text_edit::test_form_field_text_edit_changes_canvas | text-edit | — | PASS | PASS | — |
| test_render_text_edit::test_mixed_content_scanned_block_routes_to_region_tool | text-edit | #4 | XFAIL | **XFAIL** | — |
| test_render_text_edit::test_scanned_page_routes_to_region_tool | text-edit | — | PASS | PASS | — |
| test_render_text_edit::test_scanned_page_text_edit_changes_canvas | text-edit | — | XFAIL | XPASS | — |
| test_render_text_edit::test_vector_text_edit_redo_stays_visible | text-edit | #1 | XFAIL | XPASS | — |
| test_render_text_edit::test_vector_text_edit_undo_reverts | text-edit | #1 | XFAIL | XPASS | — |
| test_render_text_edit::test_vector_text_edit_visible | text-edit | #1 | XFAIL | XPASS | — |
| test_render_view_nav::test_actual_size_changes_canvas | view/zoom/nav/rotate/theme | — | PASS | PASS | — |
| test_render_view_nav::test_fit_page_changes_canvas | view/zoom/nav/rotate/theme | — | PASS | PASS | — |
| test_render_view_nav::test_next_page_changes_canvas | view/zoom/nav/rotate/theme | — | PASS | PASS | — |
| test_render_view_nav::test_rotate_page_cw_changes_canvas | view/zoom/nav/rotate/theme | — | PASS | PASS | — |
| test_render_view_nav::test_theme_toggle_repaints_canvas_gap20 | view/zoom/nav/rotate/theme | gap #20 / GitHub #217 | XFAIL | **PASS** | xfail removed in pipeline p87 phase-3; renamed from `test_theme_toggle_does_not_repaint_canvas_gap20` and now a plain positive assertion — see `tests/KNOWN_WEAK_TESTS.txt` P-P86-XFAIL-4 for why the original "pixel-identical" evidence was a no-op toggle rather than a theme-independent canvas |
| test_render_view_nav::test_zoom_in_changes_canvas | view/zoom/nav/rotate/theme | — | PASS | PASS | — |
| test_render_view_nav::test_zoom_out_changes_canvas | view/zoom/nav/rotate/theme | — | PASS | PASS | — |

**Every test in the suite appears above — no test is silently omitted (39 rows = 39 collected).**

---

## 3. Systemic-defect proof cases (Phase 2 / Phase 3 requirement check)

The four Phase 2 proof cases must appear as XFAIL — **not** ERROR, **not** silently
skipped. None errored and none were skipped. Their per-run resolution:

| Defect | Proof test(s) | This run | Notes |
|---|---|---|---|
| **#1** vector text blank | `oracle_proof::test_vector_text_edit_visible`, `text_edit::test_vector_text_edit_visible` | XPASS | `xfail(strict=False)` — intermittent under automation; deterministic oracle = Phase 5 gtest. Marker present, ran, not errored/skipped. |
| **#1** Ctrl-Z no-revert | `oracle_proof::test_ctrl_z_reverts_canvas`, `text_edit::test_vector_text_edit_undo_reverts` | XPASS | same `strict=False` non-determinism. |
| **#2** mixed-DPI smear | `oracle_proof::test_mixed_dpi_region_not_smeared`, `images::test_mixed_dpi_region_not_smeared` | **XFAIL** | deterministic — per-region DPI not implemented. |
| **#4** mixed-content routing | `oracle_proof::test_mixed_content_scanned_region_routable`, `text_edit::test_mixed_content_scanned_block_routes_to_region_tool` | **XFAIL** | deterministic — per-region routing not implemented. |

**#3 multilingual OCR** (`ocr::test_arabic_block_ocr_contains_arabic_script`):
status **XFAIL**, verified via the OCR text pipe (not the pixel oracle). Observed
OCR output for the Arabic block contained **no** Arabic-script codepoints
(U+0600–U+06FF) — the doc-wide `eng` language garbles the Arabic block, exactly
the `#3` defect. Fixes in Phase 6.

All proof cases are accounted for as XFAIL/XPASS (marker fired), none as ERROR or
silent skip. ✅

---

## 4. Newly Surfaced Bugs

**None.** Zero unexpected FAIL and zero ERROR results across all 39 tests. No new
render defect was surfaced beyond the already-tracked systemic defects #1–#4 and
gap #20, which were *intentionally* pinned as `xfail` in Phases 2–3. There is
therefore nothing to add to a reproduction-steps backlog from this triage.

---

## 5. Coverage delta — previously-EYEBALL_ONLY paths now under render verification

From the Phase 1 gap report (`tests/docs/render_oracle_gap_report.md` §4): of 479
visual-verify=YES rows, **~20 were EYEBALL_ONLY** (group G14: vector-edit on an
image-dominated page, mixed-DPI render, multilingual OCR render, mixed-content
routing, and print-to-physical-printer), with **no automated visual check of any
kind**.

Phase 3 placed the **top-priority EYEBALL_ONLY paths** under automated render
verification, exercised empirically by this triage:

| Previously EYEBALL_ONLY (G14) | Now |
|---|---|
| `#1`/`#5` vector edit + undo + redo on image-dominated page | render-asserted (`xfail` proofs, oscillating XFAIL/XPASS pending Phase 5) |
| `#2` mixed-DPI region replace | render-asserted (deterministic XFAIL proof) |
| `#3` multilingual (Latin+Arabic) OCR edit | OCR-text-pipe-asserted (deterministic XFAIL proof) |
| `#4` mixed-content tool routing | render-asserted (deterministic XFAIL proof) |
| Print-to-physical-printer appearance | still EYEBALL_ONLY — manual-QA by the matrix's own admission (GAP-F7/F8) |

**~18 of the ~20 G14 EYEBALL_ONLY rows (~90%) are now under automated render or
OCR-pipe verification.** The residual ~2 (search-result-highlight paint and
bold/italic on an image-dominated page were deferred in Phase 3; print-to-printer
is inherently manual) remain EYEBALL_ONLY and are explicitly **not** claimed as
covered — no silent truncation.

Across the *whole* matrix this lifts render-asserted coverage of the formerly
dark corner from 0% to ~90% of its top-priority rows; the broad PROXY_ASSERTED
majority (~75% of all YES rows) is unchanged — Phase 3's scope was the
EYEBALL_ONLY gap, not the proxy tier.

---

## 6. Conclusion

The render-verification layer runs end-to-end against the real Release binary and
produces a stable, honest matrix: **27 PASS / 6 XFAIL / 6 XPASS / 0 FAIL / 0 ERROR**.
The deterministic defect proofs (#2, #3, #4, gap #20) are red as designed and ready
to be flipped green by the Phase 5–7 fixes; the #1-class proofs are correctly pinned
`strict=False` with the Phase 5 gtest as their deterministic oracle. No unexpected
regressions surfaced.
