# Harness-Remediation PHASE-6 (final) — UI/E2E hardening

Register targets: **R-35** (destruction-under-active-state / crash-on-close),
**R-29** (self-seeding visual goldens), **R-20** (widget-exists-only oracles),
**R-27** (UI permaskip wall / gap_a14 UIA-hang).

Stance held throughout: STRENGTHEN/ADD oracles, never weaken/skip. No product
source changed (`git status --porcelain -- src/` is CLEAN). Every deliverable
below is verified in THIS environment (headless Debug gtest + Python), with the
runner-only remainder documented honestly rather than faked.

---

## R-35 — destruction-under-active-state / crash-on-close (HIGH, headless-verifiable)

**Old weakness.** The nearest oracle (`tests/test_save_throw_undo_safety.cpp`)
proves a *thrown save* rebinds the undo stack; nothing covered destroying the
window/document graph while an edit is ACTIVE. The known real instance is the
`~MainWindow` mid-edit teardown crash (commit 4fae336): `canvas_` is a child
QWidget destroyed in `~QObject` *after* `document_`/`muPdfContext_` are freed, so
`~TextEditTool → cancelEdit()` dereferenced a dangling document → 0xC0000005 /
heap-UAF. The fix is `canvas_->clearTextEditTool()` at the top of `~MainWindow`.

**New oracle** — `tests/test_destruction_under_active_state.cpp`, two layers:

- **Layer A (real `~MainWindow`).** Construct a real headless `MainWindow`, open
  a native-text fixture, activate the text-edit tool, drive it into an ACTIVE
  edit session (`isEditing()==true`), then destroy the window DIRECTLY (no
  `close()`/`closeEvent` — the programmatic-teardown path the fix note calls
  out) and assert clean teardown. Plus an active-annotation-tool teardown and a
  document-open-no-edit control.
- **Layer B (headless matrix, 9 families).** For Annotation, Redaction, Form,
  Signature and Image insert/remove/resize/rotate/crop: execute the family
  command so the edit is ACTIVE on a real `UndoManager` (+ live `commandPages` +
  managers), then destroy the Document and its owning Pages FIRST (the
  window-close order) while the undo stack/managers still reference them, and
  assert the subsequent manager/undo destruction is UAF-clean.

**Proof it catches the defect class.** Temporarily reverted the 4fae336 fix
(commented out `clearTextEditTool()` in `~MainWindow`), rebuilt, and ran
`MainWindowActiveTeardownTest.DestroyWhileTextEditSessionActiveIsClean`:
`[ FAILED ]` (the crash guard converts the teardown 0xC0000005 into a test
failure, exit 1). Restored the fix (`git diff src/` empty), rebuilt → GREEN.
So the oracle is RED on a reintroduced teardown-UAF and GREEN on the fixed tree.

**Result:** 12 tests GREEN (3 MainWindow + 9-family matrix). Runs under
`test_main.cpp` crash guards; under msvc-asan a dangling access aborts
regardless of any catch.

---

## R-29 — self-seeding visual goldens (HIGH, verifiable via Python)

**Old weakness.** `tests/ui_harness/core/golden.py` self-seeded: on first run
`compare_to_golden(auto_regen_if_missing=True)` promoted the current capture as
the golden and returned `is_same=True`; `regen_golden` silently created a
baseline from any image. So the FIRST (possibly mirrored/half-size/blanked)
render became the reference every later run passed against. The smoke test
(`test_ui_harness_smoke.py:182`) did the same "create golden on first run".

**Fix.**
- `compare_to_golden` NEVER seeds. Missing golden ⇒ `is_same=False`,
  `golden_missing=True` (new `ImageDiffResult` field) — a loud failure, not an
  auto-pass. The `auto_regen_if_missing` parameter is removed.
- `regen_golden` is now a maintenance-only tool: it RAISES unless
  `PDFE_ALLOW_GOLDEN_SEED=1` is set, and refuses to overwrite without
  `force=True`. It is never reachable from an assert path.
- Smoke test line 182: a missing committed golden is now an assertion FAILURE,
  not a self-seed.

**Committed correctness golden + generator.** `generate_reference_goldens.py`
produces a deterministic, spec-defined (no screenshot/RNG) trusted reference,
committed at `tests/fixtures/ui_screenshots/baseline/reference/save_icon.png`
(128×128, asymmetric red/blue halves + amber right-pointing arrow + upper-left
disk + baseline bar), chosen so MIRROR / HALF-SIZE / WRONG-ICON renders are all
detectably different.

**Proof the oracle discriminates.** `tests/test_golden_no_selfseed.py` (7 tests,
all GREEN):
- missing golden ⇒ FAIL + `golden_missing` + **no baseline file minted**;
- `regen_golden` without the opt-in RAISES;
- CORRECT render PASSES against the committed golden;
- MIRRORED, HALF-SIZE, WRONG-ICON renders are each REJECTED.

Registered in `tests/fixtures/README.md` and the CMake fixture floor. No
auto-seed path remains on any assert path.

*(Adjacent note: `tests/visual/test_visual_baselines.py` SKIPS on a missing
baseline rather than auto-seeding — a lesser variant that cannot mint a false
pass; the auto-seed-to-pass defect lived only in the `ui_harness` golden path,
now fixed.)*

---

## R-20 — widget-exists-only → real behavioral assertions (HIGH)

**Old weakness.** The highest-traffic UI oracles (`test_ui_integration.cpp`
`AllActionsExist`, `*HasStandardShortcut`) assert only `findChild<QAction*>(...)
!= nullptr`. A build where Save did nothing, Open loaded no model, or Find
opened no panel would still pass; a missing control was often a SKIP.

**New oracle** — `tests/test_ui_action_behavior.cpp` (3 tests, GREEN), asserting
each high-value action's EFFECT headlessly through the real QAction/slot path (a
missing control is `ASSERT_NE`, never a skip):

- **Open** — before `openFile` the model is null; after, `documentForTesting()`
  is non-null with the expected page count AND known page text (`PAGEBIG0`) —
  Open LOADS a model.
- **Save** — a Replace-All edit (`REPLNINE`→`DONE9SAVE`, count==3) saved via the
  real `saveAction->trigger()` is PERSISTED TO DISK: reopening the file in a
  fresh window shows all 3 replacements and none of the original.
- **Find** — `findAction->trigger()` SHOWS the search-panel dock (hidden→shown),
  the action's user-visible effect, not merely its existence.

Model-state effects are asserted at the gtest level (headless), per the target's
preference.

---

## R-27 — UI permaskip wall / gap_a14 UIA-hang (HIGH; largely runner-only)

**The problem.** On the Server-2019 runner PDFE's UIA provider intermittently
WEDGES; every `IUIAutomation::FindAll` blocks and pywinauto's timeout does not
bound the COM call, so UI tests that drove menus/tools via UIA FindAll hang and
were guarded to SKIP (gap-A14). Skip-call sites across the UI test files number
~1140 (upper bound; the gap-A14 wedge is the dominant trigger).

**Route (product side already wired).** PDFE exposes a UIA-FindAll-FREE seam
pair: `MainWindow::activateToolByObjectName()` (the `testToolActivatePipe`
dispatch target) to DRIVE a tool, and `MainWindow::activeToolDiag()` (a
result-pipe readback) to VERIFY it. `tests/helpers/qinvoke.py` already routes
tool activation and state readback through this pair instead of a menu FindAll.

**Locally-verifiable proof** — `tests/test_tool_activation_pipe_route.cpp` (2
tests, GREEN): on a headless `MainWindow`, `activateToolByObjectName()` followed
by `activeToolDiag()` drives→reads back a 5-tool matrix with ZERO UIA — each
activation switches the active tool and the readback reflects it AND changes
between tools (a real oracle, not a constant); an unknown action is a defined
negative (`false`). This LOCKS the product seam the pipe route depends on and
demonstrates the exact behavioral core the wedged UIA path could not reach.

**Honest conversion accounting.**
- **Converts to real assertions NOW (verifiable here):** the drive+verify
  contract for tool activation/active-tool state — any UI test asserting "tool X
  is active" can route through `activateToolByObjectName` + `activeToolDiag`
  instead of a UIA menu/toolbar FindAll. The same pattern already exists in
  qinvoke for toolbar visibility, font families, form-field values, comment
  inventory, text-edit selection diag, etc. (result-pipe readbacks).
- **Remains runner-only (NOT faked):** the Python side still LOCATES the pipe
  QLineEdit via a *bounded* UIA probe in `_find_pipe` (`_uia_responsive` +
  Strategy A.5 raw-COM children scan). A fully UIA-free transport (e.g. a
  file/socket command channel) would need product changes and live-runner
  validation; that is out of scope for a no-product-change phase and is left as
  a tracked TODO rather than a deleted skip. No skips were deleted; the
  conversion is via the existing pipe route.

---

## Verification summary

| Target | New test(s) | Count | Verified here |
|---|---|---|---|
| R-35 | `test_destruction_under_active_state.cpp` | 12 | Headless gtest GREEN; RED proven via reverted 4fae336 fix |
| R-29 | `test_golden_no_selfseed.py` + generator + committed golden | 7 | Python GREEN; mirror/half/wrong-icon rejected, correct passes, no auto-seed |
| R-20 | `test_ui_action_behavior.cpp` | 3 | Headless gtest GREEN (Open/Save-persist/Find effects) |
| R-27 | `test_tool_activation_pipe_route.cpp` | 2 | Headless gtest GREEN (UIA-free drive+readback) |

- Clean Debug build after each add (anti-collision taskkill; synchronous).
- New-suite batch (incl. adjacent `SaveThrowUndoSafetyTest`): **26 GREEN**.
- Adjacent suites unbroken: `MainWindowIntegrationTest` + `SmallTextEditRender`
  **25 GREEN**; smoke test collects cleanly.
- Fixtures registered: CMake fixture floor (`multipage_smalltext_edit.pdf`,
  `ui_screenshots/baseline/reference/save_icon.png`) + `tests/fixtures/README.md`
  (R-29 golden corpus). Golden image committed.
- **No product source changed** (`git status --porcelain -- src/` empty). No
  real crash was discovered to fix — the R-35 UAF class was already fixed
  (4fae336); this phase adds the regression lock that was missing. No test
  weakened, deleted, or skipped.
