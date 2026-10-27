# Phase-3 Harness Remediation Report

**Scope:** Strengthen the FONTS + FORMS + AUTOSAVE oracles so the phase-3
surviving mutants **M13** and **M15** die; convert the assertion-free autosave
capture-audit into a real oracle (R-09); make form/metadata round-trips assert
via save+reopen (R-10/R-11); give the OCR pipeline a content oracle (R-18).

**Rule discipline:** oracles were STRENGTHENED and tests ADDED only. No test was
weakened, deleted, or skipped. **No product source was changed** — all mutants
survived because oracles were weak, and the fixes are stronger tests.
`git status --porcelain -- src/` is CLEAN.

---

## 1. Mutants SURVIVED → KILLED

Re-verified with `python docs/harness-audit/phase-3/run_mutations.py --only
M13,M15`. Baseline (unmutated) filters green; both mutations now fail a targeted
test. Kill rate **100% (2/2)**.

| Mutant | File | Prior | Now | Killing test |
|---|---|---|---|---|
| **M13** button-reset-to-off | `src/forms/FormManager.cpp` | SURVIVED | **KILLED** (27 tests, 1 fail) | `Phase5FormsTest.ResetFormRestoresCheckboxDefaultOnState` |
| **M15** glyph-scale-invert | `src/fonts/GlyphScaleAdjuster.cpp` | SURVIVED | **KILLED** (16 tests, 1 fail) | `GlyphScaleAdjusterTest.HorizontalScaleIsOriginalOverSubstituteNotInverted` |

### M13 — checkbox /DV reset
The Btn branch of `detail::resetTxWidgetFromObj` restores a checkbox/radio to
its `/DV` (e.g. `/Yes`); the mutation hard-codes `"Off"`, reverting a
defaulted-ON control to unchecked. The pre-existing `ResetFormRestoresDefaultValues`
asserts only the `/Tx` FirstName field, so the button path had **no positive
oracle**. New test opens `test_form_with_dv.pdf` (AgreeTerms checkbox: `/DV=/Yes`,
AP states `/Off`,`/Yes`), dirties it to `/Off`, calls `resetForm`, and asserts
`/V == "Yes"` **both in memory and across save+reopen**. Under the mutation the
widget resets to `/Off` → both assertions fail.

### M15 — horizontal glyph scale inversion
`horizontalScale` must equal `originalWidth / substituteWidth` (squeeze/stretch a
metric-incompatible substitute to the ORIGINAL run width); the mutation applies
the reciprocal. The existing cases only bounded the scale into the wide
`(0.5, 2.0)` band or checked same-font symmetry (scale ≈ 1.0) — both invariant
under inversion. New test measures the same widths the production code measures
(Arial vs Times over a long asymmetric string), asserts the pair is genuinely
asymmetric (relDiff > 2%), then pins `horizontalScale ≈ originalWidth/substituteWidth`,
asserts it is NOT the inverse, and cross-checks direction (wider→stretch>1,
narrower→squeeze<1). Under the inversion the exact-ratio assertion fails.

**Evidence:** `docs/harness-audit/remediation/phase-3-mutation-reverify.json`
aggregates the verdicts; per-mutant JUnit XML + logs copied to
`docs/harness-audit/remediation/reverify-runs-p3/`. The audit record
(`phase-3/mutation-results.json`, `phase-3/runs/`) was restored via
`git checkout` so it reflects the original 16-mutant catalog run.

---

## 2. Autosave capture-audit → real oracle (R-09)

`tests/test_autosave_capture_audit.cpp` was 15 `SUCCEED()`-only families
(assertion-free — it recorded `capture_present` per family but never asserted).
`finishRow` now:

- **Asserts `EXPECT_TRUE(capture_present)`** for every probed, capturable family
  (**14 families** now assert capture == YES: TextEdit, Annotations,
  RedactionMark, RedactionApply, Image, PageOps, FormFill, Metadata, Bookmarks,
  PageLabels, LayersUndoable, LayersDirect, SignatureFieldCreate, TreScannedText).
- Uses an explicit **documented allowlist** for `SignatureSign` — it signs via
  its own incremental save (not undoable by design), so autosave legitimately
  never fires. That by-design absence is **LOCKED** with `EXPECT_FALSE(capture_present)`
  so it cannot silently flip.
- Turns environmental blockers (missing fixture/tessdata/pfx) into honest
  `GTEST_SKIP`s, never silent passes.

No `SUCCEED()`-only bodies remain. Full-suite run: all 15 families probed,
capture register matches expectations (only SignatureSign = NO, allowlisted).

## 3. Form / metadata save→reopen round-trips (R-10/R-11)

- **R-10** `tests/test_form_commands.cpp :: SetFieldValue_ExecuteAndUndo` — added
  a save→reopen block: after the in-memory value check, the document is saved,
  reopened in a fresh `Document`/`FormManager`, and the field value asserted on
  disk. Catches a "stages in the manager, never serializes" bug the in-memory
  check alone misses.
- **R-11** `tests/test_metadata_manager.cpp :: WriteMetadata_Roundtrip` — the
  misnamed in-memory-only "Roundtrip" now actually round-trips through disk:
  after the in-memory read-back it saves, reopens, and asserts title/author/
  subject/keywords/creator persisted.

## 4. OCR content oracle (R-18)

`tests/test_ocr_pipeline.cpp :: OcrOnScannedPdf` asserted only non-empty +
structurally-valid HOCR. Added `OcrRecognizesKnownWordsOnCleanScan`: runs OCR on
`test_ocr_clean.pdf` (known pangram ground truth), flattens the parsed HOCR to
text, and asserts ≥8/10 distinctive known words appear (plus the distinctive
"sphinx" specifically). Garbage/wrong recognition that is still non-empty now
fails. (Complements the existing WER/CER oracle in
`test_fidelity_r3_08_ocr_wer.cpp`.)

## 5. Fixtures

**No new fixtures introduced** — all reused existing committed, README-listed
fixtures. Two now load-bearing fixtures were pinned into the
`tests/CMakeLists.txt` fixture floor so the kill tests always run instead of
silently skipping: `test_form_with_dv.pdf` (M13) and `test_ocr_clean.pdf`
(R-18). `.gitignore` extended to keep `reverify-runs-p3/*.xml` (mirrors the p2
precedent) as durable audit evidence.

---

## Verification summary

- Clean build succeeded (`taskkill` first; synchronous).
- Touched suites unmutated — **96 tests, 0 failures, 0 skipped** (GlyphScaleAdjusterTest,
  Phase5FormsTest, FormManagerTest, FormCommandsTest, MetadataManagerTest,
  AutosaveCaptureAudit, OcrPipelineTest). The 3 new oracle tests all ran and passed.
- Mutation reverify: **M13 KILLED, M15 KILLED** (were SURVIVED), kill_rate 100%.
- `git status --porcelain -- src/` **CLEAN** — no product source changed.
- No test weakened, deleted, or skipped.
