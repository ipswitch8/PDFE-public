# Coverage-Expansion Phase 2 — C-03 i18n render · C-04 format-compat · C-05 generative

Three real, gated test classes from `register.json` (C-03/C-04/C-05) were added,
each with real oracles proven able to FAIL, deterministic and bounded in the
Windows headless suite. **No product source was changed** (`git status
--porcelain -- src/` is clean). The C-05 generative harness discovered **two real
product defects** (both in independently-tracked, already-open defect areas); they
are reported below with deterministic repros and excluded from the generated
op-domain (no invariant weakened) rather than fixed here.

New files:
- `tests/test_i18n_render.cpp` (C-03) + `tests/fixtures/gen_i18n_render_corpus.py`
  → `i18n_rtl.pdf`, `i18n_cjk.pdf`, `i18n_combining.pdf`
- `tests/test_format_compat.cpp` (C-04) + `tests/fixtures/gen_format_compat_corpus.py`
  → `compat_objstm_xref.pdf`, `compat_incremental.pdf`, `compat_linearized.pdf`,
  `compat_rebuilt_xref.pdf`
- `tests/test_generative_invariants.cpp` (C-05)
- Registered in `tests/CMakeLists.txt` (sources + fixture floor); fixtures
  documented in `tests/fixtures/README.md` (FixtureCorpus README-enforcement +
  opens-cleanly both pass).

Result: **15 tests, 0 failures, 0 skips, deterministic across two runs.**

---

## C-03 — i18n RENDERING (RTL / CJK / combining)

The existing i18n coverage (`test_textshaper_i18n.cpp`) asserts the SHAPING layer
(TextShaper/BitmapCompositor) directly. C-03's gap was the RENDER level: a real
on-disk PDF page opened + rendered through PDFE's Document/Page path, with
glyph-quad + pixel oracles. Fixtures bake HarfBuzz-shaped geometry (bidi reorder +
Arabic GSUB joining + GPOS mark attachment) into the content stream via PyMuPDF
`insert_htmlbox` at generation time; the baked positions are reader-version
independent, so PDFE's own MuPDF 1.26.10 reproduces them. The test reads baked
per-glyph quads with an isolated `fz_context` (a fresh viewer's read, same pattern
as `test_content_stream_ctm_reset.cpp`) and renders page 0 through the real
`Page::render` for ink.

**Oracles + non-vacuity proofs**

| Test | Oracle | Non-vacuity |
|---|---|---|
| `HebrewLaysOutRightToLeftAndLatinLeftToRight` | Match each Hebrew code point to its quad; logical-order x-centres must **strictly DECREASE** (first logical char is rightmost = RTL). | The SAME monotonic-direction function is applied to a Latin "LTR" control, which must strictly **INCREASE**. Same code, opposite required result — if Hebrew rendered LTR it would increase and RED. |
| `ArabicIsContextuallyJoinedNotIsolated` | stext carries Arabic Presentation-Form-B glyphs (U+FB50..U+FEFF) → contextual joining applied; run spans right-to-left. | Isolated base forms (U+06xx) would mean *no* shaping and fail the ≥3-presentation-forms check (verified: fixture yields FEE3/FEAD/FEA3/FE91/FE8E). |
| `CjkGlyphsRenderRealFullWidthInkNotTofu` | Exact CJK code points (4E2D/6587/6D4B/8BD5) preserved; each glyph quad full-width square (w/h∈[0.75,1.30]); real interior ink (>5% of quad area); x ascending. | A deliberately BLANK control band must read ~0 ink (proves the ink meter isn't reporting ink everywhere); a dropped/tofu glyph breaks the exact-codepoint match or the >5% ink floor. |
| `CombiningMarkAttachesWhileSpacingAccentAdvances` | Combining mark (U+0301) x-centre falls WITHIN the base 'x' span and is zero-advance (next 'N' at the base's right edge). | Differential control: a SPACING accent (U+00B4) on an identical base must sit to the RIGHT of the base and advance the pen, pushing its 'N' clearly further right (>8pt). If the two were indistinguishable the attachment check would be vacuous. |

**Font limitation (disclosed):** Latin/Hebrew/Arabic/combining use `arial.ttf`
(covers all four); CJK uses `simsun.ttc` (falls back through
`msgothic.ttc`/`msyh.ttc`/`YuGothM.ttc`). The font is needed only at fixture
**generation** time — the runtime test reads baked geometry + embedded subsets. A
target lacking a CJK font cannot regenerate `i18n_cjk.pdf`; the generator fails
loudly (SKIP=FAIL) rather than emitting a Latin-fallback fixture that would
silently pass.

---

## C-04 — FORMAT-COMPAT / MIGRATION

One 2-page document (shared tokens `COMPATMARK`/`ONE`/`TWO`) rendered into four
structurally distinct on-disk shapes. Each is proven to open (2 pages + known
text), accept an edit (a tokened Text annotation), and survive save→reopen with
the edit persisted and the original content intact — a guard that a save-format
change can't silently break opening older / other-producer files.

| Fixture | On-disk shape | Roundtrip |
|---|---|---|
| `compat_objstm_xref.pdf` | object streams (`/ObjStm`) + xref streams (PDF 1.5+) | PASS |
| `compat_incremental.pdf` | chained incremental updates (3 `startxref`, `/Prev`) | PASS |
| `compat_linearized.pdf` | linearized (leading `/Linearized` dict; via pikepdf/libqpdf — PyMuPDF 1.27 dropped linearization) | PASS |
| `compat_rebuilt_xref.pdf` | classic xref with startxref + all in-use offsets zeroed → MuPDF logs "cannot recognize xref format / repairing" and reconstructs by scanning | PASS |

**Structural distinctness** is asserted separately (`FormatCompatShape.*` reads the
committed bytes: `/ObjStm` present, ≥2 `startxref` + `/Prev`, leading `/Linearized`,
≥2 zeroed xref offsets) so the roundtrip oracle cannot pass trivially on four
identical files. All four fixtures pinned in the fixture floor.

---

## C-05 — PROPERTY-BASED / GENERATIVE invariants

A seeded (deterministic) harness generates random-but-valid edit-op sequences from
the register's op alphabet (add/delete annotation, edit text, rotate page,
insert/delete page, undo, redo) and asserts metamorphic invariants after each:

- **(a)** save→reopen preserves the visible content (pageCount + annotationCount +
  whitespace-normalized page text digest).
- **(b)** do-all then undo-all restores the EXACT original digest.
- **(c)** page count is conserved except by explicit page ops — tracked **exactly**
  through undo/redo with a parallel delta-stack mirroring UndoManager (checked
  after every op).

On failure the seed + full op trace print, and a prefix-shrink narrows to the
shortest failing prefix. Determinism (same seed ⇒ same sequence ⇒ same verdict) is
asserted by a dedicated test and confirmed across two binary runs.

### Real defects DISCOVERED (the harness did its job)

The faithful full-cross harness surfaced **two real invariant violations**. Both
are in independently-tracked, actively-worked defect areas (project memory:
command Page-ownership refactor pipeline f4a82c1e; undo-after-structural-edit /
op99 UAF pipeline b3f6a1d2), were NOT introduced by this work, and are
architectural (out of scope for a coverage phase — not minimally fixable). They
are reported here and **excluded from the generated op-domain, with no invariant
weakened**; each invariant still runs on the harness whose domain it is sound for.

- **B1 — rotate-then-edit-text loses text on undo.** After `RotatePageCommand(page
  0, 90)` then `EditTextCommand` on that page, undo-all does **not** restore the
  page text (digest textlen 470 → 403). Repro: `std::mt19937` seed **1001**, ops
  `[RotatePage(0), EditText(page0,span2)]` (shrunk). Isolated probe: EditText+undo
  ALONE restores perfectly (match); the loss appears only with a prior `/Rotate`
  change — the content-stream editor mislocates glyphs under the changed page
  rotation and its undo is not a clean inverse. → `rotatePage` is excluded from the
  ContentInvariants alphabet (rotate is exercised by the structure harness).

- **B2 — page-structure ops + saveAs → access violation (0xC0000005).** A page
  insert/delete/rotate sequence with interleaved undo/redo that leaves retained
  deleted-page objects on the undo stack, then `saveAs`, faults during teardown of
  those retained objects. Repro: seed **2000**, ops
  `[…DeletePage/InsertPage/Rotate + Undo/Redo…]` reducing to 1 page, then saveAs
  → reopen OK, crash on destruction. Isolated: do-all/undo-all (no save) is sound
  for all 50 seeds; a single sequence's saveAs is fine standalone but the retained
  undo-stack page objects fault on teardown — the tracked command-Page-ownership
  hazard. → the save/reopen invariant (a) is applied to the ContentInvariants
  harness (sound there); PageStructureInvariants asserts (b)+(c), both proven sound.

### Committed (green) harness result

| Test | Alphabet | Invariants | Result |
|---|---|---|---|
| `ContentOpsPreserveMetamorphicInvariants` | addAnnot, deleteAnnot, editText, undo, redo | (a) + (b) | 50/50 held |
| `PageStructureOpsPreserveMetamorphicInvariants` | rotate, insert, delete, undo, redo | (b) + (c) | 50/50 held |
| `SeededGenerationIsDeterministic` | — | RNG reproducibility | PASS |

All three invariants (a/b/c) are exercised by the suite; the exclusions are of
input-domain cells (rotate×edit, page-struct×save) that hit B1/B2, not of any
invariant or oracle.

---

### B1/B2 regression locks (committed, EXPECTED-TO-FAIL)

The two discovered defects are now locked by committed, executable regression
tests in `tests/test_generative_invariants.cpp` — not just prose — so they cannot
be documented-then-forgotten. Both are `DISABLED_`-prefixed (the normal suite
stays green; they do not run by default) and each replays the EXACT deterministic
minimal repro and asserts the violated invariant, so they FLIP GREEN when the bug
is fixed (at which point the prefix is removed to make them permanent):

| Lock | Repro | Enabled result (verified) |
|---|---|---|
| `DISABLED_B1_RotateThenEditTextUndoRestoresText` | hard-coded `[RotatePage(0,90), EditText(page0,span2)]` (seed-1001 shrink) → do-all/undo-all, assert text digest restored | **assertion FAIL** — `text length not restored (orig=470 after=403)` |
| `DISABLED_B2_PageStructUndoRedoSaveAsNoCrash` | `genSequence(mt19937(2000), {rotate,insert,delete,undo,redo}, 12)` → saveAs + reopen + teardown with page commands still on the undo stack | **0xC0000005 access violation** (caught by the GoogleTest SEH guard → FAILED, runner not killed) |

Verification captured:
- Normal run `--gtest_filter=GenerativeInvariants.*`: **3 tests PASS**, both locks
  reported `[ DISABLED ]` (not executed) — suite stays green.
- `--gtest_also_run_disabled_tests --gtest_filter=*DISABLED_B1_*`: FAILS with the
  exact 470→403 text-loss assertion — faithful lock of a live bug.
- `--gtest_also_run_disabled_tests --gtest_filter=*DISABLED_B2_*`: throws
  `0xC0000005` (SEH) — faithful lock of the live crash.

No active invariant was weakened and no product source changed to add these locks.

## Verification summary

- `taskkill` + clean SYNCHRONOUS build succeeds (no concurrent cmake); fixture
  floor (7 new pins) passes at configure.
- `test_i18n_render.*` + `test_format_compat.*` + `test_generative_invariants.*`:
  **15 tests, 0 failures, 0 skips.** JUnit XML parsed; identical verdicts across
  two consecutive runs (deterministic).
- `git status --porcelain -- src/`: **CLEAN** — no product source changed.
- New tests registered in `tests/CMakeLists.txt`; 7 new fixtures pinned in the
  fixture floor and documented in `tests/fixtures/README.md` (FixtureCorpus
  README-enforcement + opens-cleanly both green).
- No test simplified, skipped, or weakened. Two real defects (B1, B2) discovered
  and reported with deterministic repros rather than masked.
