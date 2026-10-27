# Autosave / Crash-Recovery — Coverage Summary

Phase-6 synthesis narrative for the **autosave-coverage** pipeline (phases 1–6).
Companion to `RTM.md` (the per-family × per-dimension traceability matrix).

## 1. What autosave is, and how it works

PDFE autosaves a **crash-recovery snapshot** of the open document to a private
recovery directory (`%APPDATA%/PDFE/PDFE/recovery` on Windows) so that unsaved
work survives a crash or force-kill. It never touches the user's source file —
recovery is offered on the next launch via a modal recover/discard prompt.

**Engine.** `AutosaveManager` (`src/core/AutosaveManager.cpp`) is a headless
engine over a `Document` + optional `UndoManager`. On a periodic `QTimer` tick
(default 30 s) or an explicit `autosaveNow()`, it decides whether to write.

**Trigger — two independent dirty signals** (`autosaveNow()`):
1. the undo stack is not at its clean marker (every command-driven edit); OR
2. `Document::contentVersion()` moved since the last autosave — this catches
   **direct** mutations that push no undo command (redaction burn-in, page-label
   rewrite, direct layer ops). Signal (2) was added in this pipeline; without it
   those edits were silently lost on crash (Phase-1 gap-register).

**Debounce.** If neither the undo-stack size nor the content version changed
since the last successful write, the (potentially expensive) rewrite is skipped.

**Pre-snapshot flush.** Some edits live in an in-panel manager tree until
committed (notably the bookmark tree in `BookmarkManager`). `autosaveNow()`
invokes a registered `preSnapshotFlush_` callback *before* snapshotting so the
live document actually carries the deferred edit. A flush exception is swallowed
(a flush error must never abort crash-recovery).

**Capture.** `Document::writeRecoveryCopy()` (`src/core/Document.cpp`) writes a
**fast, no-recompress full snapshot**: `pdf_save_document` with GC and
image/font recompression **disabled** (`do_compress_images = 0`) — a
serialize-and-copy of already-encoded streams. The autosave timer can fire
repeatedly while the user keeps editing, so re-encoding every image on each
write would stall the app.

**Sidecar schema (v1).** Each snapshot writes a JSON sidecar
(`<uuid>.pdfe_recovery.json`) atomically via `QSaveFile` (temp + rename). Fields:
`originalPath`, `recoveryFilePath`, `timestamp` (ISO-8601 ms), `sessionId`,
`schema` (=1). The recovery **copy** is written *before* the sidecar so a sidecar
never points at a missing/partial copy.

**Restore.** At launch, the static `AutosaveManager::scanForRecoverable()`
enumerates the recovery dir, parses each sidecar, and returns only records that
are well-formed (valid JSON, non-empty `sessionId`, and an existing recovery
copy). A clean save calls `clearRecovery()`; the startup skip-path
(`PDFE_TEST_SKIP_UNSAVED_PROMPT`) calls `discardAllRecoverable()`.

## 2. Total NEW test count

Counted directly from the built suites (`--gtest_list_tests`, cross-checked
against a green run of `229` C++ instances):

| File | Suite(s) | Count |
|---|---|---|
| `tests/test_autosave_recovery.cpp` | `AutosaveRecoveryTest` | 5 |
| `tests/test_autosave_capture_audit.cpp` | `AutosaveCaptureAudit` | 15 |
| `tests/test_autosave_capture_fixes.cpp` | `AutosaveCaptureFixTest` (4 gap locks + 1 perf lock) | 5 |
| `tests/test_autosave_matrix.cpp` | `AutosaveMatrixTest` (186) + `Mixed` (6) + `SignatureException` (1) + `Tre` (1) | 194 |
| `tests/test_autosave_sidecar_integrity.cpp` | `AutosaveSidecarIntegrityTest` | 10 |
| **C++ subtotal** | | **229** |
| `tests/test_ui_autosave_recovery.py` | pywinauto `@pytest.mark.autosave` | 3 |
| **TOTAL NEW TESTS** | | **232** |

All 229 C++ instances pass (verified: `229 tests from 8 test suites … PASSED`).
The 3 pywinauto tests run on the Windows UI rig (skip on headless boxes).

## 3. The 5 gaps found in Phase-1 and their disposition

The Phase-1 discovery probe (`AutosaveCaptureAudit`, 15 families →
`gap-register.json`) empirically drove each family and asked whether the
recovery copy carried the edit after a crash. It found **5 families whose edits
were NOT captured**, of which **4 were fixed** and **1 is by-design**:

| # | Gap family | Root cause | Disposition |
|---|---|---|---|
| 1 | **RedactionApply** | burn-in is a direct full-rewrite, no undo command → undo-only trigger never fired | **Fixed** — added the `contentVersion()` trigger. Lock: `AutosaveCaptureFixTest.RedactionApplyIsCapturedByAutosave` |
| 2 | **Bookmarks** | edit lived in `BookmarkManager` tree, uncommitted to the document at snapshot time | **Fixed** — pre-snapshot flush commits the tree. Lock: `AutosaveCaptureFixTest.BookmarkEditIsCapturedByAutosave` |
| 3 | **PageLabels** | editor mutated labels directly with no undo command | **Fixed** — routed through `EditPageLabelsCommand` (`src/core/PageLabels.cpp`), which dirties the undo stack. Lock: `AutosaveCaptureFixTest.PageLabelEditIsCapturedByAutosave` |
| 4 | **Layers (direct)** | direct layer ops pushed no undo command | **Fixed** — layer mutations route through undoable commands (`SetLayerVisibilityCommand`). Lock: `AutosaveCaptureFixTest.LayerToggleIsCapturedByAutosave` |
| 5 | **SignatureSign** | signs via its own incremental `pdf_save_document`; no undo command, no content-version bump | **By-design** — the signature is persisted by the signing path itself, not the recovery snapshot. Locked explicitly: `AutosaveMatrixSignatureException.SignIsNotCapturedByAutosave` |

Final `gap-register.json`: `gap_count = 1` (SignatureSign, by-design).

## 4. Real product defects found + fixed across the pipeline

1. **Content-version trigger missing (silent data loss).** Direct, non-undo
   mutations — redaction burn-in, page-label rewrite — never triggered autosave,
   so a crash lost them silently. Fixed by adding the `Document::contentVersion()`
   dirty signal to `AutosaveManager::autosaveNow()` plus `markModified()` on the
   direct-mutation paths (`src/core/Document.cpp`, `AutosaveManager.cpp`). This is
   the fix behind gaps #1 and #3 above.
2. **Bookmark edits bypassed the snapshot (commit-bypass).** The bookmark tree
   was never committed into the live document before the snapshot, so recovery
   copies lacked new bookmarks. Fixed with the pre-snapshot flush hook.
3. **`scanForRecoverable` surfaced session-less (corrupt) sidecars.** A sidecar
   missing its `sessionId` was returned as long as its recovery copy existed —
   an unaddressable record that would show as a blank/garbage entry in the
   startup prompt. Fixed with an empty-`sessionId` exclusion in the scan
   (`AutosaveManager.cpp` line ~238). Found + locked by
   `AutosaveSidecarIntegrityTest.MalformedSidecarsAreExcludedFromScan`.

(Additionally, a stale-sidecar startup-wedge was hardened: the
`PDFE_TEST_SKIP_UNSAVED_PROMPT` path now actively `discardAllRecoverable()`s
rather than ignoring stale sidecars — locked by
`AutosaveRecoveryTest.DiscardAllRecoverableClearsStaleSidecars`.)

## 5. Performance result

`AutosaveCaptureFixTest.RecoverySnapshotIsFastOnLargeImagePdf` writes a recovery
snapshot of a 23 MB / 100-page image PDF and asserts it completes fast and
reopens as a valid PDF with the same page count. The no-recompress path
serialize-and-copies this document in **~440 ms** on the dev box (the test's
guard bound is a deliberately generous 8000 ms so it catches a gross recompression
regression on loaded CI without flaking). A recompress regression on this path
took many seconds.

## 6. Residual known gaps (with rationale)

These are honest, catalogued limitations — none is an unclosed capture gap:

- **(a) SignatureSign is not autosaved (by-design).** Signing persists through
  its own incremental save; the undo/content-version-driven autosave is bypassed.
  Explicitly locked (see §3 #5). Not a defect.
- **(b) UNC and `>MAX_PATH` source paths are not tested on this box.** The path
  suite (`AutosaveSidecarIntegrityTest.PathHandlingUnicodeLongSpaces`) covers
  Unicode (Greek + CJK + emoji), spaced, and 100-char names — all comfortably
  under Windows `MAX_PATH` (260). True `>MAX_PATH` and UNC (`\\server\share`)
  sources need opt-in long-path support / a share and are environment-gated; they
  are documented here rather than asserted. No known defect, but unverified on
  this rig.
- **(c) Layer runtime visibility bit is not serialized by any save.** Toggling a
  layer's on/off state via `SetLayerVisibilityCommand` dirties the undo stack and
  triggers autosave correctly, and the layer's *existence* survives recovery
  (`Cell/LayerVisibility_*`). But whether MuPDF's `pdf_enable_layer` `/D` usage
  state is written by `pdf_save_document` is a separate, orthogonal MuPDF/save
  limitation — it affects normal Save identically, not just recovery. The
  autosave-capture contract is "autosave captures whatever a normal save would",
  so this is out of scope for this pipeline and noted in the matrix (RTM footnote 4).

## 7. DCA — defect-catching assurance (mutation spot-check)

Three representative NEW production lines were mutated to confirm the suite
actually catches a regression at each (apply → rebuild serial → run the specific
test → confirm RED → revert → rebuild → confirm GREEN). All mutations were
reverted; the tree is back to its committed state.

| # | Mutation (production line) | Expected-to-red test | Result |
|---|---|---|---|
| (a) | `AutosaveManager::autosaveNow()` — break the content-version trigger: `if (!undoDirty && !contentChanged)` → `if (!undoDirty)` (ignore `contentChanged`) | `AutosaveCaptureFixTest.RedactionApplyIsCapturedByAutosave` | **RED** — see below |
| (b) | `AutosaveManager::scanForRecoverable()` — revert the empty-`sessionId` skip (remove the `if (rec.sessionId.empty()) continue;`) | `AutosaveSidecarIntegrityTest.MalformedSidecarsAreExcludedFromScan` | **RED** — see below |
| (c) | `AutosaveManager::autosaveNow()` — comment out the `preSnapshotFlush_()` call | `AutosaveCaptureFixTest.BookmarkEditIsCapturedByAutosave` | **RED** — see below |

### DCA results (verbatim RED evidence)

All three mutations produced a RED at the predicted test; all were reverted and
the source rebuilt to GREEN. Method for each: apply → serial rebuild → run the
one test → observe RED → revert → (final) serial rebuild → run all three → GREEN.

**(a) content-version trigger disabled** — `autosaveNow()` `!undoDirty && !contentChanged` → `!undoDirty`:

```
[  FAILED  ] AutosaveCaptureFixTest.RedactionApplyIsCapturedByAutosave (34 ms)
   > direct redaction apply must still trigger an autosave
[  PASSED  ] 0 tests.  [  FAILED  ] 1 test
```
The direct redaction burn-in (undo stack clean) no longer triggered autosave —
exactly the silent-data-loss defect this trigger fixed.

**(b) empty-`sessionId` scan skip reverted** — `scanForRecoverable()`:

```
[  FAILED  ] AutosaveSidecarIntegrityTest.MalformedSidecarsAreExcludedFromScan (23 ms)
[  PASSED  ] 0 tests.  [  FAILED  ] 1 test
```
Without the skip, the session-less-but-copy-present sidecar (case (c) in the
test) was surfaced, so the scan returned 2 records instead of the expected 1.

**(c) pre-snapshot flush call disabled** — `autosaveNow()`:

```
[  FAILED  ] AutosaveCaptureFixTest.BookmarkEditIsCapturedByAutosave (42 ms)
[  PASSED  ] 0 tests.  [  FAILED  ] 1 test
```
The bookmark tree was never committed into the live document before the
snapshot, so the recovery copy did not carry the new bookmark.

**Revert + GREEN confirmation** (source `git diff` clean, rebuilt):

```
[==========] 3 tests from 2 test suites ran. (100 ms total)
[  PASSED  ] 3 tests.
```

The suite catches a real regression at each of the three representative
production fix sites — the coverage is defect-catching, not tautological.

## 8. Pipeline status — COMPLETE

All 6 phases of the `autosave-coverage` pipeline are done, gated
(karen/validator/test-runner artifact-verified per phase; phase-6 = karen +
validator), committed, and pushed:

| Phase | Commit | What |
|---|---|---|
| 1 | `46a5a4c` | Adversarial capture audit — 15-family probe, 5 gaps found |
| 2 | `80a7c51` | Fixed 4 gaps (content-version trigger, pre-snapshot flush, undoable page-labels) |
| perf | `ec6e634` | Fast no-recompress recovery writes (~440 ms / 23 MB) |
| 3 | `9990bff` | 194-instance capture×restore×trigger matrix |
| 4 | `1afa868` | Sidecar/session/timing integrity + empty-sessionId scan fix |
| 5 | `3721fde` | pywinauto UI recover/discard/live-autosave E2E |
| 6 | `ae7b6a2` | This RTM + coverage synthesis + DCA |

**Net:** 232 new tests, 3 real product defects found + fixed, 1 performance
fix, 5 capture gaps closed (4 fixed + SignatureSign documented by-design).

