# Pipeline 3e9b1d7c — Phase-1 triage

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
**Source backlog:** `tests/docs/post_f7c2a4e8_followups.md` (23 items, Themes A-H)
**Phase-1 goal:** Inventory + XPASS confirmation sweep + G2 wrapper-bug fix + C++ baseline check + G1 timeout verify.

## 1. C++ baseline confirmation

```
[==========] 3595 tests from 467 test suites ran. (439457 ms total)
[  PASSED  ] 3593 tests.
[  SKIPPED ] 2 tests.
```

**Confirmed:** 3595 ran / **3593 PASSED / 0 FAILED / 2 SKIPPED.** Same as f7c2a4e8 closeout.

## 2. G2 wrapper-bug investigation

The sweep_runner.parse_summary regex requires `^=+` fence at start of summary line. Captured subprocess output from `pytest -q` mode produces the summary WITHOUT the fence (just `"1 failed, 1 passed in 57.36s"`).

**Fix path:** change `^=+\s+` to `^=*\s*` (zero-or-more, making fence optional). Inline parse function in phase-1 sweep wrapper applied the fix. Patch to be committed in this phase.

## 3. XPASS confirmation sweep — modified scope

**Critical finding:** `test_annotations_p1.py` triggers a chronic `QColorDialog`-modal-hang under `--runxfail` (the `annot_*_set_color` tests open a color picker that pywinauto cannot dismiss). The hang occurred on the first attempt at this sweep; pdfe.exe had to be manually killed. The file was excluded from the dual-run sweep that followed.

**Sweep result (4 files, 2 runs each, 1417s wall-clock):**

| File | Run 1 | Run 2 | Flipped |
|---|---|---|---|
| test_canvas_click_tre.py | 1F / 1P / 1S | 1F / 1P / 1S | 0 |
| test_canvas_coords_p2.py | 6F / 6P / 3S | 6F / 6P / 3S | 0 |
| test_canvas_phase3.py | 5F / 2P | 5F / 2P | 0 |
| test_page_operations_p1.py | 10F / 22P / 9S | 10F / 22P / 9S | 0 |
| **Totals (across runs)** | 22F / 31P / 13S | 22F / 31P / 13S | **0** |

**Interpretation:**

- **0 flipped** — the affected tests are perfectly stable across runs (run order, fresh PDFE per test, etc. don't change outcomes)
- **22 stably failing** — these are the real residual failures already documented under P-GAP-92/93/95/96
- **31 stably passing tests** — many of these are likely candidates for xfail-marker removal in phase-8 (cheap-wins)

**Cheap-win identification (deferred to phase-8):** `pytest -q` mode doesn't emit per-test outcome lines for PASSED tests (only FAILED short-summary entries). Identifying exact passed test IDs from these logs requires either a `-rA` flag in a re-sweep or cross-referencing collected test IDs with the FAILED set. Phase-8's un-marking work will re-run with `-v` to get the precise per-test outcomes before removing markers.

## 4. test_annotations_p1.py — QColorDialog hang

**Symptom:** First sweep attempt hung indefinitely. User reported "Select Annotation Color dialog has been showing for many minutes." pdfe.exe was manually killed; sweep timed out at 1800s.

**Cause:** `annot_*_set_color` tests (annot_stickynote_set_color, annot_textmarkup_set_color, annot_freetext_set_color, annot_ink_set_color) trigger `QColorDialog::exec()` which is system-modal. The test driver opens the color picker but doesn't provide UIA-driven dismissal logic. `pytest --timeout=60` cannot break out of a modal QDialog's event loop.

**Disposition:** add P-GAP-103 covering this hang. Could be xfail-marked at the file level for this specific sub-cluster, OR could be addressed by:
- Adding a `testAnnotationColorPipe` that sets the color programmatically (no dialog)
- Mocking QColorDialog::exec() to auto-accept in test builds

**Phase-8 closeout** should explicitly classify all 8 XPASS markers — including the chronic hang as BLOCKED until a non-modal color-set path exists.

## 5. G1: test_dialog_reachability_p0.py timeout override verify

**Deferred to phase-8** — running this file in isolation takes ~14 minutes. The 1500s override is in place (sweep_runner.py PER_FILE_TIMEOUT_OVERRIDES). Final phase-8 sweep will exercise it; if it completes without tree-kill, G1 is verified.

## 6. Full backlog status (23 items)

| # | Theme | Item | Status |
|---|---|---|---|
| 1 | A | A1 PreferencesDialog tab widget | OPEN, phase-2 |
| 2 | A | A2 ComplianceDialog progress bar | OPEN, phase-2 |
| 3 | A | A3 BatchDialog widgets (3) | OPEN, phase-2 |
| 4 | A | A4 CompareDialog | OPEN, phase-2 |
| 5 | B | B1 8 XPASS cleanup | INVESTIGATED, deferred to phase-8 |
| 6 | B | B2 P-GAP-96 flaky pair | OPEN, phase-8 |
| 7 | C | C1 broader Pages workflows | OPEN, phase-5 |
| 8 | C | C2 P-GAP-93 18-test cluster | OPEN, phase-7 |
| 9 | C | C3 calc-order drag-reorder | OPEN, phase-3 |
| 10 | D | D1 P-GAP-98 spontaneity research | OPEN, phase-4 |
| 11 | D | D2 P-GAP-92 followup | OPEN, phase-4 |
| 12 | E | E1 redaction roundtrip | OPEN, phase-6 |
| 13 | E | E2 JS sandbox | OPEN, phase-6 |
| 14 | E | E3 signature workflow | OPEN, phase-6 |
| 15 | E | E4 password enforcement | OPEN, phase-6 |
| 16 | F | F1 atlas timing | DEFERRED, future pipeline |
| 17 | F | F2 search/replace | OPEN, phase-8 closeout |
| 18 | F | F3 bookmark panel | OPEN, phase-8 closeout |
| 19 | F | F4 view rotate | OPEN, phase-8 closeout |
| 20 | G | G1 dialog timeout verify | DEFERRED to phase-8 final sweep |
| 21 | G | G2 wrapper parse bug | INVESTIGATED, fix to be committed in phase-1 |
| 22 | H | H1 STALE matrix audit | OPEN, phase-8 |
| 23 | H | H2 count-comparison note | DONE — already in test_strategy.md |

**New finding tracked:** P-GAP-103 (QColorDialog hang in `annot_*_set_color`).

## 7. Phase-1 AC verification

| AC | Status |
|---|---|
| G2 wrapper fix produces correct counts | PARTIAL — fix identified, patch needed in commit |
| Confirmation sweep counts recorded | PASS — §3 table |
| Each XPASS classified | PARTIAL — file-level stability confirmed; per-test classification deferred to phase-8 |
| G1 timeout verified | DEFERRED to phase-8 final sweep |
| C++ baseline ≥ 3593/0/2 | PASS — §1 |
| pipeline_h_triage.md committed | PASS — this doc |
| No C++ changes | PASS — phase-1 only touched test infra (pending wrapper fix commit) |
