# Post-b5e3a9f2 Followups

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


**Source pipeline:** b5e3a9f2 closeout (2026-05-22)
**Audit performed:** 2026-05-22 after all phase-8 gates passed
**Purpose:** Capture every unresolved item + test-matrix gap discovered during or exposed by pipeline b5e3a9f2, organized into actionable groups for the next pipeline.

This is the input to the next planning pass.

---

## Group A — Real product bugs (C++ failures + crashes)

These are genuine defects, not test-driver flakiness.

### A1. P-GAP-90 — UndoManager not registering CommentPanel delete
- **File:** `tests/test_annotations_phase4.cpp:467`
- **Test:** `Phase4AnnotationsTest.CommentPanelDeleteSignalTriggersDeleteCommand`
- **Symptom:** `EXPECT_TRUE(undo_->canUndo())` is `false` after CommentPanel emits `deleteAnnotation` and the lambda runs `undo_->execute(cmd)`.
- **Pre-existing since at least c94e2f17 closeout (8556c22).** Hidden by CRT-at-exit flake.
- **Fix path:** Step through CommentPanel::deleteAnnotation handler + UndoManager::execute() to find why canUndo() returns false after a successful command execution.
- **Effort:** S (small — one assertion failure, narrow scope)

### A2. P-GAP-94 — CommentPanel reply signal not emitting parent index
- **File:** `tests/test_thread_safety.cpp`
- **Test:** `CommentPanelThreadingTest.ReplySignalEmitsParentIndex`
- **Pre-existing since at least c94e2f17 closeout.**
- **Fix path:** Investigate signal-emission semantics for the reply workflow under concurrent dispatch.
- **Effort:** M (medium — thread-safety, may need synchronisation primitives)

### A3. P-GAP-91 — pdfe.exe crashes during form-fill + Save-As flow
- **File:** `tests/ui_automation/test_canvas_phase4.py`
- **Test:** `test_canvas_phase4[flow_flow_form_fill_save]`
- **Symptom:** Handler reports "pdfe crashed during form fill + Save As" — `_pdfe_alive()` returns false after Tab-fill into a form field + File→Save As menu chain.
- **Fix path:** Reproduce locally with `--gtest_brief=0` style trace; capture stderr/log to identify crash site (FormTool? Save dialog? FormManager teardown?).
- **Effort:** M (likely a null-pointer or use-after-free in the form-save path)

### A4. P-GAP-97 — Named "Nextpage" link doesn't advance page
- **Files:** `tests/ui_automation/test_follow_link_phase_post.py`, `tests/ui_automation/test_ui_follow_links.py`
- **Test:** `test_follow_named_nextpage_link_advances_one_page` (same name in both files; shared handler)
- **Symptom:** PDF named action `/Action /Named /N /Nextpage` doesn't trigger page navigation when followed via UIA click.
- **Fix path:** Audit named-action dispatch in MainWindow link handler. The PDF spec named actions to check: NextPage, PrevPage, FirstPage, LastPage. May also need a `testFollowNamedActionPipe` if real click dispatch is unreliable.
- **Effort:** S (likely a missing case in a switch/dispatch)

### A5. GAP-51 follow-up: missing path-traversal/embedded-null safety on save paths
- **Origin:** Bypassed in phase-4 by `testSaveAsPipe`.
- **Symptom:** Pipe takes any string and passes it to `Document::saveAs()` without sanitisation. Real Save-As dialog has OS-level filtering; the pipe doesn't.
- **Fix path:** Add path-sanitisation tests for both Document::saveAs() and the pipe handler. Reject paths with embedded nulls, parent-traversal, alternate streams, etc.
- **Effort:** S (additive tests; possibly small product-code guard)

---

## Group B — Synthetic-interaction gaps (a single common pattern)

Multiple xfailed tests share one root cause: pyautogui synthetic input doesn't reliably reach Qt6 widgets for certain interaction modes. The proven fix pattern is a Q_INVOKABLE pipe that synthesizes Qt events directly.

### B1. Universal `testDragPipe` for QListWidget InternalMove
- **Affects:** P-GAP-93 (drag_drop_reorder_short/far in page_operations_p1), P-GAP-95 (calc_order_dialog_drag_reorder), P10-INFRA-1 (historical).
- **Approach:** Add a `Q_INVOKABLE bool dragItemProgrammatic(int fromIndex, int toIndex)` on `QListWidget`-hosting panels — same pattern as `selectPagesProgrammatic`. Pipe handler reads "widgetObjectName,fromIdx,toIdx".
- **Effort:** M

### B2. `testCanvasDragPipe` for handle drags + rubber-band + text selection
- **Affects:** P-GAP-92 (canvas_phase3 handle_drag_corner/edge/rotate, text_selection_drag/triple_click, rubber_band_select), P-GAP-93 (page_drag_drop_*), some P-GAP-70 residuals in canvas_coords_p2.
- **Approach:** `PdfCanvas::dragByPdfCoord(pageIdx, fromX, fromY, toX, toY)` — synthesizes Qt mouse press / multiple moves / release events through `QApplication::sendEvent`. Mirror `clickByPdfCoord` shape.
- **Special case:** Triple-click needs its own pipe entry (e.g. `tripleClickByPdfCoord`).
- **Effort:** M (uses existing `clickByPdfCoord` plumbing as a template)

### B3. `testScrollPipe` for canvas scroll-wheel
- **Affects:** P-GAP-92 row `canvas_canvas_scroll_wheel`.
- **Approach:** Synthesize Qt `QWheelEvent` against `PdfCanvas` via a small pipe taking pixel-delta or page-delta integer.
- **Effort:** S

### B4. `testKeyboardModifierClickPipe` for Ctrl+/Shift+click gestures
- **Affects:** P-GAP-93 rows `page_multi_select_ctrl_click`, `page_multi_select_shift_click` (which specifically test the gesture).
- **Approach:** Either accept these are environment-bound and BLOCKED-EXTERNAL, OR add a low-level Win32 SendInput wrapper that holds modifier key state across the mouse_event sequence. The pipe approach defeats the test's purpose (testing the gesture), so this might be the only path.
- **Effort:** L (deep Win32 input-injection work)

---

## Group C — UIA exposure gaps (object naming / widget reachability)

### C1. P-GAP-87 — search panel replace widgets + LayerPanel
- **Affects:** 5 tests in `test_phase7_new_features.py`.
- **Tests:** test_search_panel_has_replace_field, test_search_panel_has_replace_buttons, test_panel_reachable, test_tree_widget_exists, test_action_buttons_exist.
- **Approach:** Audit SearchPanel + LayerPanel widget constructors; add `setObjectName()` calls similar to the phase-5 `imagePropertiesPanel` fix.
- **Effort:** S (per-widget; possibly the same as P-GAP-72 QTimer::singleShot pattern if QToolButton children are involved)

### C2. P-GAP-89 — Sign-dialog tsaTimestampCheck not exposed
- **Affects:** 1 test (sec_sign_timestamp_present).
- **Approach:** Add objectName/accessibleName on the TSA timestamp QCheckBox in SignDialog constructor.
- **Effort:** S

### C3. P-GAP-85 — Sign-dialog certificationCombo + invisibleCheck not exposed
- **Affects:** 2 tests.
- **Same pattern as C2** — Sign dialog widgets need objectName.
- **Effort:** S (bundle with C2)

---

## Group D — Test infrastructure gaps (stale matrix entries, fixture issues)

### D1. P-GAP-88 — Stale `File → New` matrix row
- **File:** `tests/ui_automation/matrix/ui_test_matrix.csv` row `menu_open_new_document`
- **Symptom:** "Menu item 'New' not found in any popup" — the menu was renamed or moved.
- **Fix path:** Inspect actual MainWindow menu structure; reconcile matrix.
- **Effort:** S

### D2. P-GAP-74 — `test_precondition_panels_phase4.py` chronic flakiness
- **Symptom:** 14 of 23 xpassed entries flipped between sweeps.
- **Approach:** Identify the timing-sensitive assertion in the atlas-dialog precondition cluster; add explicit waits or convert to pipe-based dispatch.
- **Effort:** M

### D3. P-GAP-96 — annot_ink_set_color + test_txt_tre_edit_regular_font (flaky)
- **Symptom:** Pass in isolation, fail when run after other tests.
- **Approach:** Investigate test-ordering pollution; add fixture-isolation hooks (kill orphans, sleep between tests).
- **Effort:** S

### D4. P-GAP-79 — Search/replace workflow harness gaps (2 tests)
- **Effort:** M

### D5. P-GAP-82 — Bookmark panel state-dependent test (1 test)
- **Effort:** S

### D6. P-GAP-84 — View rotation environment-dependent (1 test)
- **Effort:** S

---

## Group E — Stale xfail markers (xpassed cleanup)

### E1. After Groups B/C/D land, re-run a confirmation sweep on every P-GAP-92, P-GAP-93, P-GAP-96 entry. Each newly-PASSing test gets its marker removed.

---

## Group F — UI test matrix coverage gaps

### F1. `testSaveAsPipe` no-document path untested
- The handler short-circuits with `if (!document_ || !document_->isPdf()) return;` — no test exercises that branch.
- **Effort:** S (add a Python test that writes to the pipe when no document is open and asserts file isn't created)

### F2. PagePanel selection → multi-page action sequences
- `selectPagesProgrammatic` has unit tests, but no end-to-end coverage for:
  - Select pages [0, 2], then `Pages → Delete` → confirm both deleted
  - Select pages [0, 2], then `Pages → Extract Pages...` → confirm extract dialog gets both indices
  - Select pages [0, 2], then `Pages → Rotate Clockwise` → confirm both rotated
- **Effort:** S (3 test rows)

### F3. `imagePropertiesPanel` field-value tests
- Phase-5 exposed the panel via objectName but no test reads the actual values (DPI label, dimensions, image-info text).
- **Effort:** S

### F4. Pages menu workflows beyond Rotate
- P-GAP-93 leaves `Pages → Move Up/Down/To Start`, `Pages → Insert From File...`, and undo/redo dependencies untested.
- **Effort:** Already partially addressed by Group B if `testDragPipe` lands; remaining gaps after B should be small.

---

## Group G — Security test matrix coverage gaps

### G1. UI-level redaction roundtrip
- C++ side covers redaction-and-save (`test_redaction_apply.cpp`, `test_redaction_verifier.cpp`). No UI test confirms the GUI flow produces the same byte-removal guarantee.
- **Test idea:** Open redaction-fixture PDF in PDFE, place redaction via test pipe, apply via menu, save, then headless-verify the bytes are gone.
- **Effort:** M

### G2. Wrong-password retry/cancel flow
- `passwordEdit` widget is reachable but no test covers entering a wrong password, getting an error, then either cancelling or retrying with the right password.
- **Effort:** S

### G3. JavaScript form-action sandbox tests
- MuJS is bundled for form `keystroke`/`validate`/`calculate`/`format`/`focus`/`blur` events. No UI test confirms a hostile JS action (`app.launchURL`, filesystem operations) is sandboxed.
- **Test idea:** A `test_form_js_sandbox.py` that opens a form PDF with hostile JS and asserts the action is blocked / sanitised.
- **Effort:** M

### G4. Encryption password-strength enforcement
- `passwordStrengthBar` + `passwordStrengthLabel` reachability is tested but no test confirms that a weak password actually blocks the Save action.
- **Effort:** S

### G5. P-GAP-78 residuals: visible/invisible signature workflows
- 2 xfailed tests; needs PFX fixture validation + signing workflow trace.
- **Effort:** M

### G6. `testSaveAsPipe` path-traversal guard (overlaps A5)
- Same item as A5; listed here because it's security-relevant.

---

## Summary

| Group | # items | Effort total |
|---|---:|:--|
| A. Product bugs | 5 | S, M, M, S, S |
| B. Synthetic-interaction pipes | 4 | M, M, S, L |
| C. UIA exposure | 3 | S × 3 |
| D. Test infra / fixtures | 6 | S, M, S, M, S, S |
| E. xfail marker cleanup | 1 | depends on B/C/D |
| F. UI matrix gaps | 4 | S, S, S, dependent |
| G. Security matrix gaps | 6 | M, S, M, S, M, (dup A5) |
| **Total** | **~28** | mixed |

**Estimated total work:** moderate. Several items are S (one-line fixes); the L item is B4 (Win32 modifier-click injection — may be BLOCKED-EXTERNAL after investigation).

**Recommended phasing for the next pipeline** (planner will refine):
1. **Phase-1 inventory + sub-cluster confirmation** (pure investigation; no code)
2. **Phase-2 Group A small product fixes** (A1, A4, A5)
3. **Phase-3 Group B universal drag/scroll/triple-click pipes** (B1, B2, B3)
4. **Phase-4 Group A medium product fixes** (A2 thread-safety, A3 form-save crash)
5. **Phase-5 Group C UIA exposure** (C1, C2, C3)
6. **Phase-6 Group D test infra** (D1, D2, D3, D5, D6)
7. **Phase-7 Group F + G coverage gaps** (new tests for untested paths)
8. **Phase-8 Group E un-marking sweep + closeout**

Group B4 (Win32 modifier-click) likely defers to a future pipeline or becomes BLOCKED-EXTERNAL after a research phase.
