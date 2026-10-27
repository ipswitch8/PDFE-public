# Pipeline c94e2f17 phase-1: ui_automation baseline sweep

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


**Date:** 2026-05-20
**Wall clock:** 6048.7s
**Files:** 47

## Totals

| Outcome | Count |
|---|---|
| passed | 321 |
| failed | 99 |
| skipped | 128 |
| errors | 25 |
| xfailed | 0 |
| xpassed | 0 |
| crashed_files | 1 |
| wall_clock_s | 6048.7 |

## Per-file results (sorted by FAILED desc)

| File | P | F | S | E | dur | crashed |
|---|---|---|---|---|---|---|
| test_page_operations_p1.py | 9 | 23 | 9 | 0 | 261.4s | False |
| test_dialog_reachability_p0.py | 117 | 20 | 50 | 0 | 464.5s | False |
| test_precondition_panels_phase4.py | 23 | 17 | 0 | 0 | 446.0s | False |
| test_features_phase7.py | 8 | 8 | 0 | 0 | 217.6s | False |
| test_canvas_coords_p2.py | 6 | 6 | 3 | 0 | 100.1s | False |
| test_canvas_phase3.py | 1 | 6 | 0 | 0 | 64.0s | False |
| test_annotations_p1.py | 23 | 3 | 4 | 0 | 437.6s | False |
| test_ui_encryption_flows_wad.py | 0 | 3 | 0 | 0 | 94.9s | False |
| test_canvas_phase4.py | 19 | 2 | 2 | 0 | 342.6s | False |
| test_security_compliance_p1.py | 17 | 2 | 6 | 0 | 389.1s | False |
| test_ui_search_replace.py | 0 | 2 | 2 | 0 | 44.1s | False |
| test_winappdriver_smoke.py | 0 | 2 | 0 | 0 | 62.9s | False |
| test_canvas_click_tre.py | 1 | 1 | 1 | 0 | 56.1s | False |
| test_critical_flows_p0.py | 18 | 1 | 2 | 0 | 247.5s | False |
| test_ui_bookmark_panel.py | 0 | 1 | 0 | 0 | 2.6s | False |
| test_ui_calc_order.py | 1 | 1 | 0 | 0 | 27.6s | False |
| test_ui_view_rotate.py | 0 | 1 | 0 | 0 | 20.8s | False |
| test_broader_phase_post.py | 1 | 0 | 0 | 0 | 25.2s | False |
| test_canvas_click_subsystems.py | 0 | 0 | 0 | 0 | 46.2s | False |
| test_dialog_cache_unit.py | 7 | 0 | 0 | 0 | 3.7s | False |
| test_final_phase8.py | 0 | 0 | 0 | 0 | 118.5s | False |
| test_flaui_bridge_unit.py | 2 | 0 | 3 | 0 | 5.1s | False |
| test_follow_link_phase_post.py | 4 | 0 | 0 | 0 | 34.0s | False |
| test_forked_marker_unit.py | 1 | 0 | 0 | 0 | 8.1s | False |
| test_form_fields_p1.py | 10 | 0 | 22 | 0 | 372.4s | False |
| test_forms_palette_phase_post.py | 1 | 0 | 7 | 0 | 114.6s | False |
| test_menu_navigation_p0.py | 0 | 0 | 0 | 0 | 841.0s | True |
| test_misc_phase_post.py | 3 | 0 | 2 | 0 | 71.7s | False |
| test_missing_features_phase6.py | 0 | 0 | 0 | 0 | 123.0s | False |
| test_phase7_new_features.py | 0 | 0 | 0 | 25 | 2.7s | False |
| test_preflight_phase_post.py | 2 | 0 | 1 | 0 | 37.2s | False |
| test_remaining_preconditions_phase5.py | 0 | 0 | 0 | 0 | 120.1s | False |
| test_text_formatting_p2.py | 24 | 0 | 8 | 0 | 238.3s | False |
| test_ui_alt_text.py | 2 | 0 | 0 | 0 | 26.5s | False |
| test_ui_comment_panel.py | 0 | 0 | 1 | 0 | 14.3s | False |
| test_ui_encryption_flows.py | 1 | 0 | 0 | 0 | 71.7s | False |
| test_ui_file_dialog_flows.py | 0 | 0 | 0 | 0 | 85.0s | False |
| test_ui_file_dialog_flows_wad.py | 7 | 0 | 0 | 0 | 107.3s | False |
| test_ui_follow_links.py | 4 | 0 | 0 | 0 | 36.3s | False |
| test_ui_form_palette.py | 3 | 0 | 0 | 0 | 38.1s | False |
| test_ui_ltv.py | 0 | 0 | 0 | 0 | 46.1s | False |
| test_ui_ocr_all_pages.py | 0 | 0 | 2 | 0 | 33.3s | False |
| test_ui_preferences.py | 1 | 0 | 0 | 0 | 15.9s | False |
| test_ui_recent_files.py | 2 | 0 | 0 | 0 | 34.2s | False |
| test_ui_sign_dialog.py | 0 | 0 | 2 | 0 | 29.4s | False |
| test_ui_statusbar.py | 0 | 0 | 1 | 0 | 11.3s | False |
| test_ui_theme.py | 3 | 0 | 0 | 0 | 29.4s | False |

## Notes

- This is the post-pipeline-2-fixes, post-DIALOG_CACHE_RESTART_EVERY=20 baseline.
- All subsequent phase ACs in pipeline c94e2f17 reference these counts.
- 1 crashed_files entry = file that hit sweep_runner.py per-file 8-minute tree-kill threshold.