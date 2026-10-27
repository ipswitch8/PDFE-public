# Pipeline b5e3a9f2 — Phase-1 triage

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


**Date:** 2026-05-21
**Source sweep:** `tests/_runtime/sweeps/phase8_verify_20260521T142523/_final.json`
**Final totals (post-c94e2f17):** 235P / 0F / 124S / 25E / 80XF / 109XP / 1 crashed / 6188.6s

This document is the gating output of pipeline b5e3a9f2 phase-1. No
code changes; pure investigation.

---

## 1. The 25 ERROR items — single root cause

All 25 ERROR entries fall in **one file**: `tests/ui_automation/test_phase7_new_features.py`.

**Collection error (verbatim, all 25 cases):**

```
AttributeError: 'PdfeAppFixture' object has no attribute 'launch'
```

**Root cause:** test_phase7_new_features.py calls a `.launch()` method
on `PdfeAppFixture` that was renamed or removed in a prior refactor.
The current API on `tests/ui_automation/harness/pdfe_app_fixture.py`
uses `PdfeAppFixture.start()` (or similar) — confirm exact name in
phase-2.

**Proposed fix (phase-2):**
- Inspect `tests/ui_automation/harness/pdfe_app_fixture.py` for the
  current launch method name.
- Either: (a) rename all 25 call sites in test_phase7_new_features.py
  to use the correct method, or (b) add a backward-compatible
  `launch` alias to `PdfeAppFixture` if multiple test files depend
  on this name.

**Scope:** One file, one method-name correction. Estimated 10-20 minutes
including verification run.

---

## 2. The 1 CRASHED file — `test_menu_navigation_p0.py`

**Identity:** `tests/ui_automation/test_menu_navigation_p0.py`
**Wall-clock before tree-kill:** 841s (= 480s initial + 360s retry pass; both wedged at the 8-min sweep_runner per-file budget).
**Parametrizations collected:** 113 (one per P0 menu CSV row).
**Per-row launch cost:** ~9s (fresh PDFE.exe each row to detect crash).
**Predicted total runtime needed:** 113 × 9s ≈ 1017s, well above 480s budget.

**Root cause:** The test is parametrized over **all P0 Menu rows in the
CSV matrix** (~113 rows). Each parametrization **deliberately launches a
fresh `pdfe.exe`** in order to catch crash-on-menu-click — this is the
test's contract, not a defect. The arithmetic simply exceeds the
sweep_runner per-file budget.

**Fix options (phase-2 chooses one):**

| Option | Pros | Cons |
|---|---|---|
| **(a) Raise per-file timeout for this file** in sweep_runner.py via per-file override map | Preserves test semantics (fresh PDFE per row → real crash detection). Minimal risk. | Sweep wall-clock grows by ~10 min. |
| (b) Per-module session fixture sharing one PDFE | Faster | Defeats the test's stated purpose (crash detection per menu item). Recommend AGAINST. |
| (c) Split file into smaller buckets | Each bucket fits 480s budget | Loses parametrize ID coherence, complicates record_matrix_result CSV updates. |

**Recommendation: option (a)** — add a per-file timeout override map in
sweep_runner.py:
```python
PER_FILE_TIMEOUT_OVERRIDES = {
    "test_menu_navigation_p0.py": 1500,  # 25 min — 113 rows × ~9s + headroom
}
```

**Scope:** ~5 lines in sweep_runner.py + 1 doc note. Estimated 5 minutes
including verification.

---

## 3. xpassed confirmation run

A scoped sweep was launched as part of phase-1 over the 10 files
containing the 109 xpassed test IDs:

| File | xpassed in phase-8 sweep |
|---|---:|
| test_annotations_p1.py | 24 |
| test_canvas_click_tre.py | 1 |
| test_canvas_coords_p2.py | 6 |
| test_canvas_phase3.py | 1 |
| test_canvas_phase4.py | 19 |
| test_features_phase7.py | 8 |
| test_page_operations_p1.py | 9 |
| test_precondition_panels_phase4.py | 23 |
| test_security_compliance_p1.py | 17 |
| test_ui_calc_order.py | 1 |
| **Total** | **109** |

**Confirmation sweep:** `tests/_runtime/sweeps/phase1_xpass_confirm_20260521T235539/`
Totals: 0P / 0F / 25S / 0E / 84XF / **93XP** / 0 crashed / 2394.5s

**Decision rule applied in phase-7:**
- A test ID xpassed in BOTH sweeps → **safe to un-mark** (93 tests).
- A test ID xpassed in phase-8 but flipped to XFAIL in phase-1 → **flaky, keep marker** (16 tests).

**Per-file outcome:**

| File | xp8 | stable | flipped | missing |
|---|---:|---:|---:|---:|
| test_annotations_p1.py | 24 | 22 | 2 | 0 |
| test_canvas_click_tre.py | 1 | 1 | 0 | 0 |
| test_canvas_coords_p2.py | 6 | 6 | 0 | 0 |
| test_canvas_phase3.py | 1 | 1 | 0 | 0 |
| test_canvas_phase4.py | 19 | 19 | 0 | 0 |
| test_features_phase7.py | 8 | 8 | 0 | 0 |
| test_page_operations_p1.py | 9 | 9 | 0 | 0 |
| test_precondition_panels_phase4.py | 23 | 9 | **14** | 0 |
| test_security_compliance_p1.py | 17 | 17 | 0 | 0 |
| test_ui_calc_order.py | 1 | 1 | 0 | 0 |
| **TOTAL** | **109** | **93** | **16** | **0** |

**Notable finding:** `test_precondition_panels_phase4.py` is the bulk of
the flake — 14 of its 23 phase-8 xpassed flipped to xfailed in phase-1.
The P-GAP-74 entry's "all 17 fail at 'atlas dialog: textEditToolActionButton
not reachable'" is timing-dependent and dice-rolling. **All 23 markers in
this file stay** — the 9 currently "stable" passes are not yet trustworthy
enough to remove markers from. Phase-7 must explicitly skip this file.

**Stable-IDs list:** `tests/_runtime/sweeps/phase1_stable_xpassed.txt` (93 IDs)
**Flaky-IDs list:** `tests/_runtime/sweeps/phase1_flaky_keep_marker.txt` (16 IDs)
These are the inputs to phase-7.

---

## 4. P-GAP fix-cluster partitioning (80 xfailed items)

The 80 xfailed test instances cluster as follows (from
`tests/KNOWN_WEAK_TESTS.txt` P-GAP-70..86):

| Cluster | Files | xfail count | Phase | Approach |
|---|---|---:|---|---|
| **(a) P-GAP-72 FormToolBar UIA** | test_features_phase7.py | 8 | **3** | Apply QTimer::singleShot deferred QToolButton naming (mirrors GAP-57 fix for annotationToolBar_). C++ change in src/ui/MainWindow.cpp. |
| **(b) P-GAP-71 canvas_phase3 launch** | test_canvas_phase3.py | 6 | **5** | Diff launch fixture vs test_canvas_phase4.py to find missing wait/maximize step. Pure test-infra. |
| **(c) P-GAP-73 page_operations multi-select** | test_page_operations_p1.py | 23 | **6** | Win32 keybd_event sequencing OR Q_INVOKABLE multi-select pipe. Largest cluster. |
| **(d) GAP-51 save-overwrite product bug (P-GAP-77 + P-GAP-86)** | test_canvas_phase4.py (2), test_critical_flows_p0.py (1) | 3 | **4** | Real product bug in Document::saveAs() — file not overwritten when target exists. C++ change. |
| **(e) Environment-bound (WAD, PFX)** | test_ui_encryption_flows_wad.py (3), test_winappdriver_smoke.py (2), test_security_compliance_p1.py (2 P-GAP-78), test_dialog_reachability_p0.py (2 P-GAP-85) | 9 | **8** | BLOCKED-EXTERNAL — no fix in this pipeline. |
| **(f) Atlas dialog precondition** | test_precondition_panels_phase4.py | 17 | **8 residual triage** | Same QTimer::singleShot pattern as P-GAP-72 may apply, but to a different toolbar. Evaluate in phase-8. |
| **(g) Delete-annotation workflow** | test_annotations_p1.py | 4 | **8 residual triage** | GAP-60b — select-then-delete harness pattern not implemented. May fix in phase-8 or DEFERRED. |
| **(h) Small isolated items** | canvas_coords_p2 (6), canvas_click_tre (1), search_replace (2), bookmark_panel (1), calc_order (1), view_rotate (1) | 12 | **8 residual triage** | Mix of distinct root causes. Phase-8 triage either fixes inline or marks DEFERRED with specific new GAP IDs. |

**Cluster sum:** 8 + 6 + 23 + 3 + 9 + 17 + 4 + 12 = 82 (slight over-count
vs. headline 80 due to per-test-id markers like P-GAP-85 counting tests
that aren't in the file-level xfail counts).

---

## 5. C++ baseline status

```
build/vs2022/bin/Debug/pdfe_tests.exe --gtest_list_tests | wc -l
3585
```

Binary unchanged from c94e2f17 closeout (May 20 build). Runtime
baseline locked at **3585 ran / 3583 PASSED / 0 FAILED / 2 SKIPPED**.

Phase-1 makes zero code changes — baseline cannot regress.

---

## 6. Phase-1 acceptance verdict

| AC | Status | Evidence |
|---|---|---|
| AC1: 25 error files identified + 1-line proposed fix | **PASS** | §1: all 25 errors in test_phase7_new_features.py, single AttributeError root cause |
| AC2: crashed file identified + timeout characterized + fix proposed | **PASS** | §2: test_menu_navigation_p0.py, 113 × 9s exceeds 480s, recommend per-file timeout override |
| AC3: scoped confirmation sweep of 109 xpassed run; per-ID PASS/XPASS recorded | **PASS** | §3: 93 stable / 16 flaky; precondition_panels_phase4 must be excluded from phase-7 un-marking |
| AC4: 80 P-GAP partitioned into fix-clusters | **PASS** | §4 |
| AC5: pdfe_tests.exe ≥ 3583P / 0F / 2S | **PASS** | §5 (no code changed) |

No code changes in this phase. Output is this triage document plus
the confirmation-sweep artifact under `tests/_runtime/sweeps/`.
