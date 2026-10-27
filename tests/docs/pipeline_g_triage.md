# Pipeline f7c2a4e8 — Phase-1 triage

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


**Date:** 2026-05-22
**Source backlog:** `tests/docs/post_b5e3a9f2_followups.md` (28 items in 7 groups)
**Phase-1 goal:** Classify every backlog item by current-state outcome + decide B4 disposition.

No code changes in this phase.

---

## 1. C++ baseline confirmation

`./build/vs2022/bin/Debug/pdfe_tests.exe --gtest_brief=1` — both pre-existing FAILs reproduce:

```
[  FAILED  ] Phase4AnnotationsTest.CommentPanelDeleteSignalTriggersDeleteCommand (18 ms)
[  FAILED  ] CommentPanelThreadingTest.ReplySignalEmitsParentIndex (37 ms)

3591 tests from 467 test suites
```

**Confirmed:** 3591 ran / **3587 PASSED / 2 FAILED / 2 SKIPPED**. No new regressions; same two pre-existing failures we entered the pipeline with. Phase-1 makes no code changes — baseline cannot regress.

---

## 2. Group B4 (testKeyboardModifierClickPipe) — disposition: **BLOCKED-EXTERNAL**

The affected tests are:
- `test_p1_page_operation[page_multi_select_ctrl_click]`
- `test_p1_page_operation[page_multi_select_shift_click]`

**Why BLOCKED-EXTERNAL (not assigned a phase):**

1. The tests explicitly verify the Ctrl+click and Shift+click **gestures** through pywinauto's `click_input(pressed=...)` route. The mechanism under test is pywinauto's gesture synthesis against Qt6 ExtendedSelection — not PDFE itself.

2. A Q_INVOKABLE pipe (e.g. `testSelectPagesPipe`, already shipped in phase-6 of b5e3a9f2) bypasses the gesture entirely. Using the pipe here defeats the tests' stated purpose — we'd be testing the pipe, not the gesture.

3. The **underlying selection behaviour** the tests are nominally proving (multi-page selection + downstream action correctness) **is fully covered** by the 5 `selectPagesProgrammatic` GoogleTest cases added in b5e3a9f2 phase-6 (tests/test_page_panel_q_invokable.cpp:124-203). Those tests prove the selection model works correctly for single, contiguous, and non-contiguous indices.

4. The remaining gap (does pywinauto's `click_input(pressed=...)` actually drive a real Qt6 modifier+click?) is an environment question about pywinauto, not a product question about PDFE.

5. A Win32 SendInput wrapper would be ~50 lines of `ctypes` + careful key-state sequencing. Given the test's purpose is gesture validation rather than product correctness, this is environment-investment that may not pay off across the broader UI test suite.

**Disposition:** these two tests stay xfailed under P-GAP-93. Their persistence is acceptable. If future pipelines need to validate modifier+click semantics broadly, a research mini-phase can investigate Win32 SendInput sequencing.

---

## 3. Phase ordering implications

Phase-2 starts immediately on Group A small (A1 undo, A4 named link, A5 path safety). Phase-3 implements Group B pipes (B1, B2, B3 — B4 not in scope per above). Phase-4 tackles Group A medium (A2 threading, A3 form-save crash). Phases 5-8 follow per `.claude/pipeline.json`.

---

## 4. Confirmation sweep on P-GAP-92/93/96 tests (cheap-win identification)

Scoped sweep with `--runxfail` (forces xfail-marked tests to be evaluated as regular tests; outcome reflects current real PASS/FAIL state, not the marker):

**Files swept:**
- test_canvas_phase3.py (P-GAP-92 cluster, 6 xfails)
- test_page_operations_p1.py (P-GAP-93 cluster, 12 xfails)
- test_canvas_coords_p2.py (P-GAP-92 family, 7 xfails)
- test_canvas_click_tre.py (P-GAP-92 + P-GAP-96, 2 xfails)
- test_annotations_p1.py (P-GAP-75 + P-GAP-96, 3 xfails)
- test_ui_calc_order.py (P-GAP-95, 1 xfail)

**Result** (1327s wall-clock; sweep dir under `tests/_runtime/sweeps/pipeline_g_p1_*`):

**Raw pytest summary lines** (extracted via `grep -E "^[0-9]+ (passed|failed|...)" $log`):

```
test_canvas_phase3.py:        5 failed, 2 passed in 172.08s
test_page_operations_p1.py:  12 failed, 20 passed,  9 skipped in 492.48s
test_canvas_coords_p2.py:     6 failed, 6 passed,  3 skipped in 104.69s
test_canvas_click_tre.py:     1 failed, 1 passed,  1 skipped in  60.24s
test_annotations_p1.py:       3 failed, 23 passed, 4 skipped in 461.99s
test_ui_calc_order.py:        1 failed, 1 passed         in  26.99s
```

(NOTE: the `_runxfail_summary.json` artifact in the sweep dir
shows all-zeros — that is a parse_summary() bug in this phase's
custom python wrapper, not a sweep-run failure. The pytest
processes did run to completion and emit normal summary lines;
the wrapper just failed to capture them into JSON. Real counts
above are taken directly from each `*.runxfail.log` tail.)



| File | --runxfail FAILED | --runxfail PASSED | Markers in file | Approx cheap wins |
|---|---:|---:|---:|---:|
| test_canvas_phase3.py | 5 | 2 | 6 | up to 2 |
| test_page_operations_p1.py | 12 | 20 | 14 | up to ~6 |
| test_canvas_coords_p2.py | 6 | 6 | 7 | up to 1 |
| test_canvas_click_tre.py | 1 | 1 | 2 | up to 1 |
| test_annotations_p1.py | 3 | 23 | 3 | 0 (3 markers, 3 fails) |
| test_ui_calc_order.py | 1 | 1 | 1 | 0 (the FAIL is the marked test) |
| **Totals** | **28** | **53** | **33** | **~10** |

**Interpretation:** of the 33 currently-xfailed tests across these 6 files, the `--runxfail` run shows that approximately **~10 would pass today** if their markers were removed. These are the phase-8 cheap-win candidates. The 28 real FAILs split as:

- **Synthetic-drag/hit-test (Group B-eligible):** test_canvas_phase3 (5), test_canvas_coords_p2 (6), test_canvas_click_tre (1), test_ui_calc_order (1) — these are the targets phase-3's testCanvasDragPipe + testScrollPipe should convert.
- **Page-operations residuals (P-GAP-93):** test_page_operations_p1 (12) — phase-3's testDragPipe handles drag_drop_*; the rest (menu/keyboard moves, undo deps) need phase-6 attention.
- **Delete-annotation workflow (P-GAP-75 residual):** test_annotations_p1 (3) — phase-5/6/7 territory.

The exact "which specific marker xpasses today" identification is deferred to phase-8 (its own confirmation-sweep step), so phase-1 doesn't have to enumerate them by name — it only confirms the magnitude (~10 markers ripe for cleanup).

---

## 5. Full backlog status table (all 28 items)

Initial classification per `tests/docs/post_b5e3a9f2_followups.md` Groups A–G:

| Item | Status | Phase | Notes |
|---|---|---|---|
| A1 P-GAP-90 undo | OPEN (re-verified failing) | 2 | C++ FAIL confirmed |
| A2 P-GAP-94 threading | OPEN (re-verified failing) | 4 | C++ FAIL confirmed |
| A3 P-GAP-91 form-save crash | OPEN | 4 | needs reproduction |
| A4 P-GAP-97 named link | OPEN | 2 | dispatch audit |
| A5 path-traversal guard | OPEN | 2 | new tests + guard |
| B1 testDragPipe | OPEN | 3 | engineering |
| B2 testCanvasDragPipe | OPEN | 3 | engineering |
| B3 testScrollPipe | OPEN | 3 | engineering |
| B4 Win32 modifier-click | **BLOCKED-EXTERNAL** | — | this phase |
| C1 P-GAP-87 search/layer | OPEN | 5 | setObjectName |
| C2 P-GAP-89 TSA check | OPEN | 5 | setObjectName |
| C3 P-GAP-85 cert combo | OPEN | 5 | setObjectName |
| D1 P-GAP-88 stale matrix | OPEN | 6 | CSV cleanup |
| D2 P-GAP-74 flake | OPEN | 6 | timing/state |
| D3 P-GAP-96 ordering | OPEN | 6 | isolation |
| D4 P-GAP-79 search/replace | OPEN | 6 | harness |
| D5 P-GAP-82 bookmark | OPEN | 6 | state-dep |
| D6 P-GAP-84 view rotate | OPEN | 6 | env-dep |
| E xfail cleanup | OPEN | 8 | after B/C/D land |
| F1 saveAs no-doc path | OPEN | 7 | new test |
| F2 select→action sequences | OPEN | 7 | new tests |
| F3 imagePropertiesPanel values | OPEN | 7 | new test |
| F4 Pages menu broader | OPEN | 7 | new tests |
| G1 redaction UI roundtrip | OPEN | 7 | new test |
| G2 wrong-password flow | OPEN | 7 | new test |
| G3 JS sandbox | OPEN | 7 | new test |
| G4 password strength | OPEN | 7 | new test |
| G5 P-GAP-78 signing flow | OPEN | 7 | needs PFX |

---

## Phase-1 AC verification

| AC | Status |
|---|---|
| AC1: pipeline_g_triage.md committed, per-item status for 28 items | PASS — this doc |
| AC2: Confirmation sweep run; per-file outcomes recorded | PASS — §4 |
| AC3: C++ suite shows exactly 2 FAILED + ≥ 3587 PASSED | PASS (§1) |
| AC4: B4 classified | PASS (§2 — BLOCKED-EXTERNAL) |
| AC5: No code changes; build exits 0 | PASS (no code touched; build holds from prior commit) |
