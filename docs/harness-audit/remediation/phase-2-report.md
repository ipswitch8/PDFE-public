# Harness-Audit Remediation — Phase 2 Report

**Scope:** Give the content-stream / TRE / alignment VISUAL oracles real
pixel/glyph-quad assertions so the phase-3 surviving mutants **M04, M05, M06,
M07 now DIE**; de-tautologize the TRE coord-mapper tests (R-14); extend the
adversarial-ambient corpus with the zero-coverage **shear/rotate** CTM class
(R-33). Register targets: **R-03** (extraction-only-for-visual — the dominant
disease), **R-04/05/06/07** (per-injector), **R-33** (hostile-input corpus),
**R-14** (coord-mapper tautology).

**Guiding rule honored:** oracles were only STRENGTHENED / tests + fixtures ADDED.
No test was weakened, simplified, deleted, or skipped. **No product source was
changed** — every mutant was an oracle hole, exactly as the phase-1 audit
predicted.

---

## 1. Mutant kills — SURVIVED → KILLED

Re-verified with `docs/harness-audit/phase-3/run_mutations.py --only
M04,M05,M06,M07` (apply → build → run filter → revert). Evidence:
`phase-2-mutation-reverify.json` + per-mutant JUnit XML in `reverify-runs-p2/`.

| Mutant | Defect | Phase-3 | Phase-2 | Killed by (new oracle) |
|---|---|---|---|---|
| **M04** | `hScale 100 → -100` — substituted text renders MIRRORED | SURVIVED | **KILLED** (9 tests, 1 fail) | `ContentStreamSubstitutionTest.PerRunSubstitutionEditNotMirroredKeepsColor` (quad `ur.x > ul.x` + L→R run advance) |
| **M05** | `Center offset → 0` — centered text left-aligned | SURVIVED | **KILLED** (6 tests, 1 fail) | `AlignmentInjectorTest.CenteredLineIsActuallyCenteredNotLeftAligned` (metamorphic: Center first-glyph x ≫ Left) |
| **M06** | drop the fill-colour emit — edited text loses colour | SURVIVED | **KILLED** (9 tests, 2 fail) | both `ContentStreamSubstitutionTest.{PerRun…,Legacy…}` (rendered glyph argb ≈ source grey #dbd0d0, not dark ambient) |
| **M07** | `initialFontSize × 0.5` — edited text HALF-SIZE | SURVIVED | **KILLED** (14 tests, 1 fail) | `ContentStreamSubstitutionTest.LegacySubstitutionEditFullSizeKeepsColor` (edited glyph height / KEEP-ME height ∈ [0.80,1.25]) |

**Result: killed=4, survived=0, kill_rate=100.0%.** `git status --porcelain --
src/` is CLEAN after the run (no leftover mutant).

### Why these needed care (branch routing)

`afterTextObjectWithSubstitutionCallback` sets `hScale` and `initialFontSize`
defaults (M04/M07 anchors) BEFORE splitting into the per-run vs legacy branch:

- **Per-run** branch keeps the default `hScale` (so M04's `-100` mirrors) but
  OVERWRITES `initialFontSize` from the anchor segment (so M07 does NOT apply).
- **Legacy** branch keeps `initialFontSize == inj.fontSize` (M07 applies) but
  overwrites `hScale` from the switch (so M04 needs the default path).
- The fill-colour path (M06) is shared by both.

So the two new tests were built deliberately: a **per-run** self-substitution
edit (kills M04 + M06) and a **legacy** self-substitution edit (kills M07 + M06).
Both drive `applyEditsWithSubstitution` on the committed `neg_tz_grey_text.pdf`
fixture (grey #dbd0d0 span under ambient `-100 Tz` + a same-size `KEEP ME`
neighbour → a colour signal, a mirror signal, and a size reference in one page)
and read the SAVED glyph quads + argb back through a fresh fitz context.

---

## 2. Tests & fixtures added

### `tests/test_content_stream_substitution.cpp`  (R-03, R-04/07 — substitution)
- New file-scope readback (`substEditNegTz`) + colour helper (`firstInkColor`).
- `PerRunSubstitutionEditNotMirroredKeepsColor` — mirror + colour render oracle.
- `LegacySubstitutionEditFullSizeKeepsColor` — half-size + colour + upright oracle.
- The 7 pre-existing extraction-only tests are untouched (kept, not weakened).

### `tests/test_alignment_injector.cpp`  (R-05 — alignment geometry)
- New `firstGlyphUlx` stext readback.
- `CenteredLineIsActuallyCenteredNotLeftAligned` — metamorphic Left-vs-Center
  first-glyph-x oracle (Center must sit ≥30pt right of Left, and past ~110pt).

### `tests/test_content_stream_ctm_reset.cpp`  (R-33 — shear class)
- `LineHit` extended with `firstLlx`; `assertInjectedUprightUnsheared` adds a
  **parallelogram-aware** left-edge-slope oracle on top of the existing
  full-size / upright / visible / on-page contract. A pure shear is
  determinant-1 (height + width preserved), so the half-size oracle is blind to
  it — the tell is `ll.x − ul.x` ≈ ±0.5·height under a leaked `1 0 0.5 1` shear.
- Three shear regressions (Alignment / HeaderFooter / TRE) prove each guarded
  text injector lands upright and un-skewed under an ambient shear CTM. All pass:
  `PageContentGuard`'s `q … Q` restores the page-initial identity CTM, so ANY
  ambient transform (scale, shear, rotate) is neutralised.

### `tests/fixtures/gen_aa_corpus.py` + corpus  (R-33)
- Added `shear` (`1 0 0.5 1 0 0 cm`) and `rotate` (+15°) hostile ops → 4 new
  fixtures: `aa_shear.pdf`, `aa_img_shear.pdf`, `aa_rotate.pdf`,
  `aa_img_rotate.pdf`. The 16 pre-existing fixtures regenerate byte-identical.
- `test_content_stream_adversarial_ambient.cpp` `ops()` extended with
  `shear`/`rotate` so the discovery grid measures those columns (additive
  measurement; hard assertions live in the CtmReset suite).

### `tests/test_tre_coord_mapper.cpp`  (R-14 — de-tautologization)
Four "critical" tests re-derived the mapping with the test's OWN arithmetic and
never called the product. Rewritten to CALL the real symbols:
- `NoYFlip_TopStaysTop`, `NoYFlip_BottomStaysBottom` → `mapper_->pdfToPixel(...)`
- `PixelRectToPdf_SimpleRect` → `mapper_->pixelRectToPdf(...)`
- `PdfRectToPixel_SimpleRect` → `mapper_->pdfRectToPixel(...)`

Regression proof by reasoning: inverting the product Y-mapping now moves the
returned `px.y` / rect `y0,y1`, so each assertion fails — the tautology can no
longer pass a Y-inverted product.

---

## 3. Verification

1. Stale `pdfe_tests.exe` killed; clean **synchronous** build succeeded.
2. `run_mutations.py --only M04,M05,M06,M07` → **all 4 KILLED** (baseline for all
   3 distinct filters GREEN first). `phase-2-mutation-reverify.json` +
   `reverify-runs-p2/res_M0{4,5,6,7}*.xml` capture the evidence.
3. `git status --porcelain -- src/` → **CLEAN** (no leftover mutant). The audit's
   `docs/harness-audit/phase-3/mutation-results.json` + `runs/` were restored via
   `git checkout` so the original audit record is intact.
4. Touched suites unmutated → **GREEN**: `ContentStreamSubstitutionTest.*`,
   `AlignmentInjectorTest.*`, `PerRunEmissionTest.*`, `CoordMapper*`,
   `CtmReset.*`, `AdversarialAmbient.*` — **78 tests, 0 failures**. (The one
   `AdversarialAmbient` "CORRUPT" line, `ContentStreamEditor × rotate`, is
   SUCCEED-based MEASUREMENT data from the discovery probe, not a failing test;
   it flags an inline-edit-under-rotate cell for a future phase, and does not
   involve the three guarded injectors.)

## 4. Product source

**No product source changed.** All four survivors were confirmed oracle holes
(extraction-only visual oracles blind to mirror/colour/size, and a Center-offset
never geometry-checked). The fix was entirely in the tests + fixtures.

---

## 5. Gap-closure (validator round 2)

The validator gate found three corpus-completeness / fixture-registration gaps
against the acceptance criteria. All three are closed below. **STRENGTHEN/ADD
only — no test weakened, no product source changed** (`git status --porcelain --
src/` CLEAN).

### Gap 1 — ROTATE promoted from measurement to a HARD oracle

`rotate` (+15° ambient `cm`) previously existed only in the SUCCEED()-based
discovery probe. Added three HARD-assertion rotate tests in
`tests/test_content_stream_ctm_reset.cpp`, parallel to the shear ones, for the
three guarded text injectors:

- `CtmReset.AlignmentInjectorUprightUnrotatedUnderAmbientRotate`
- `CtmReset.HeaderFooterEngineUprightUnrotatedUnderAmbientRotate`
- `CtmReset.TreWriteTextBlockUprightUnrotatedUnderAmbientRotate`

New oracle `assertInjectedUprightUnrotated` + `topEdgeSlope(LineHit)`: a rotation
is determinant-1 (height+width preserved → the half-size oracle is blind) and,
unlike a pure shear, tilts BOTH edges — so the tell is the glyph **top-edge
slope** (Δy/Δx across ul→ur): ~0 for upright, ≈±tan15°≈0.27 if leaked. `LineHit`
/ `readLine` extended with `firstUly`/`firstUry`. The injected mark must be as
level as the safe `KEEPME` neighbour (`EXPECT_NEAR(injSlope, keepSlope, 0.10)` +
absolute backstop `|injSlope| < 0.12`). **Built-in non-tautology control:** the
fixture's own `EDITME` span is drawn UNDER the live rotate, so the oracle
`ASSERT`s it reads TILTED (`|slope| > 0.15`) — if the fixture/stext failed to
expose rotation the injected-mark check would be meaningless.

### Gap 2 — MEDIABOX-OFFSET ambient variant added

New `aa_mediabox.pdf` / `aa_img_mediabox.pdf` (generator R-34 path): a page with
a **non-zero MediaBox origin `[100 100 712 892]`** carrying the imposition / N-up
pattern (half-scale `0.5 0 0 0.5 0 0 cm` ambient left live). Base KEEPME/EDITME/Im0
content is offset by the origin so it lands on the visible box. New hard oracle
`CtmReset.HeaderFooterEngineLandsOnPageUnderMediaboxOrigin` +
`assertInjectedOnPageAtMediaboxOffset`: injects a left footer (placed at
`origin.x+margin`) and asserts it lands ON the offset page — bbox within the
page, visible ink, and the discriminating check `EXPECT_GT(inj.x0, 2)` (left edge
a small positive margin). If the injector ignored the MediaBox origin the footer
would sit at normalised x ≈ 36−100 = −64 (off the left) and off the bottom, both
failing. HeaderFooterEngine reads the raw MediaBox origin and offsets every
placement, so the test PASSES with the real product code.

### Gap 3 — fixtures registered in the fixture floor + README

- `tests/CMakeLists.txt` `_pdfe_required_fixtures`: added all six new corpus
  fixtures (`adversarial_ambient/aa_{shear,rotate,mediabox}.pdf` +
  `aa_img_{shear,rotate,mediabox}.pdf`), so a missing fixture hard-fails CMake
  configure instead of silently skipping the guard-neutralization proof.
- `tests/fixtures/README.md`: new "Adversarial-ambient CTM corpus" table listing
  every `aa_*` fixture with its adversarial trait + covering test.

### Non-tautology verification (mechanical, via get_texttrace on the saved output)

| Output | span | normalised firstCharX | reading |
|---|---|---|---|
| `hf_rotate` | KEEPME (safe q…Q) | 72.0 | upright, angle 0° |
| `hf_rotate` | **HFMARK (injected)** | 279.0 | **level, angle 0°** — guard neutralised the rotate |
| `hf_rotate` | EDITME (under live rotate) | **−111.6** | rotated about the origin off the left edge — the positive control |
| `hf_mediabox` | **MBMARK (injected footer)** | **36.0** | origin ADDED (100+36−100) → on-page at the margin |
| `hf_mediabox` | EDITME (ambient-corrupted) | −14.0 | half-scaled + shifted off — what a dropped origin looks like |

The guarded injections land level / on-page while the un-guarded siblings on the
SAME page are visibly transformed — so a leaked rotate (`|slope|` → 0.27) or a
dropped origin (`x0` → −64) would fail the new assertions. The oracles are not
tautologies.

### Verification summary

1. Corpus regenerated (`python tests/fixtures/gen_aa_corpus.py`); the 16
   pre-existing fixtures stay **byte-identical**, six new fixtures written.
2. Stale exe killed; synchronous Debug build reconfigured (new fixture-floor
   entries) and succeeded.
3. `CtmReset.*` (10) + `FixtureCorpus.ReadmeListsEveryPdfFixture` +
   `FixtureCorpus.EveryPdfFixtureOpensCleanly` → **GREEN, 0 failures** (JUnit
   XML parsed: tests=11 failures=0 errors=0 for the CtmReset+README filter; the
   3 new rotate tests + mediabox test all PASS with real product code).
4. Non-tautology proven mechanically (table above).
5. `git status --porcelain -- src/` → **CLEAN**.
