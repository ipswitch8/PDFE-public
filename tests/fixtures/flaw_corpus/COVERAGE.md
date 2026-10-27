# External flaw-corpus coverage matrix & verification record

<!-- tracking-banner -->
> **STATUS: LIVE OPERATIONAL STATE — keep maintaining this file.**
>
> This is **not** a defect tracker and is **not** superseded by GitHub Issues;
> it is state that code or CI reads. **GitHub Issues (`ipswitch8/PDFE`) remains
> the canonical tracker for anything still open** — a defect recorded only here
> is still an untracked defect, so file an issue and cross-reference it.
>
> See [`docs/TRACKING.md`](../../../docs/TRACKING.md).

---


Maps each section of `pdf-test-corpus-reference.md` to the PDFE tests that now
cover it, and records the combined-suite verification result. Updated as the
tiered corpus work lands.

## Current counts (machine-checked)

These are the CURRENT counts, derived from `tests/test_external_flaw_corpus.cpp`.
The dated "Verification record" sections further down are **history** — each
records what was true on its own date and is deliberately not rewritten. Read
this block, not those, for today's numbers.

<!-- CORPUS-COUNTS-BEGIN -->
| Metric | Value |
|---|---|
| `ExternalFlawCorpusTest` cases, total | 88 |
| Tier 1 | 8 |
| Tier 2 | 14 |
| Tier 3 | 26 |
| Tier 4 | 40 |
| `DISABLED_` cases | 0 |
<!-- CORPUS-COUNTS-END -->

Fixture FILES are a different quantity from test CASES (several Tier-4 cases
share one fixture). Both documents therefore also carry a fixture table, derived
from the per-tier tables in `README.md` and from the directory listing itself:

<!-- FIXTURE-COUNTS-BEGIN -->
| Metric | Value |
|---|---|
| Tiers 1-3 fixture files | 47 |
| Tier 4 fixture files | 24 |
| Supplementary fixture files | 5 |
| Conformance fixture files | 23 |
| Fixture files on disk, total | 99 |
<!-- FIXTURE-COUNTS-END -->

Enforced by `FlawCorpusCountDocs` in `tests/test_doc_drift_guards.cpp`, which
counts `TEST_F(ExternalFlawCorpusTest, ...)` per `TIER-MARKER` section in the
source and fails if this table (or the matching block in `README.md`) disagrees.
Before that check existed the doc said "47 / 47" and then "70 pass, 1 DISABLED"
while the source held 75 — meta-audit gap `PDF-106`.

## Coverage by reference section

| § | Reference category | Status | PDFE coverage |
|---|---|---|---|
| 1 | Font issues | **Covered** | Tier-1 `zero_descent`, `font_ascent_descent`; Tier-2 `complex_ttf`; Tier-3 12 fixtures (not-embedded/embedded-full/subset trio, Arial+Times & Calisto not-embedded, PDF/A-1b, mixed, Type3, CID CMap overflow, Arabic+Latin CID, standard-14) |
| 2 | Structural corruption / broken indexing | **Covered** | Tier-1 `bad_pagelabels`; Tier-2 `header_version_mismatch`; Tier-3 7 fixtures (one-byte-missing, xref-keyword-missing, zero-length filter chain, helloworld-bad, scan-bad, pdf.js #1586, invalid bbox). **Only 4 of the 7 force xref RECONSTRUCTION** — see the note below (GitHub #126) |
| 3 | Image / XObject problems | **Covered (1 defect found + FIXED)** | Tier-2 `rotated_bw_ratio`; **Tier-4** `jbig2-file-header`, `jbig2-symbol-offset`, `ccitt-endofblock-false`, `hires-valid` (positive control), `hires-wrong-height` (declared-metadata over-read guard), `hires-wrong-objectid` (dangling XObject ref) render gracefully. **`jpx-bug` (JPX) FOUND + FIXED a real security defect (#50):** malformed JPEG2000 heap-overflows OpenJPEG 2.5.4 (latest) → heap corruption / potential RCE, untrappable by `fz_try/fz_catch`. **Fixed** by a MuPDF decode gate (JPX OFF by default; opt-in preference + warn-once + placeholder overlay). **Load-bearing oracle: `JpxDecode_YieldsThePlaceholderNotARealDecode`** — it asserts the gate's POSITIVE claim, that the decoded pixmap is the 1×1 placeholder and not a decode at the XObject's declared dimensions. `JpxDecode_OpenRendersGracefully` (re-enabled, still passing) is a **secondary** no-crash check only: #50's crash was probabilistic (~53% of runs), so "it did not crash this run" cannot distinguish a held gate from a regressed one that happened to survive — see GitHub #65 / SEC-84. `docs/audit/dca/0011-jpx-openjpeg-heap-overflow.md` |
| 4 | Layout / real-world extraction | **Partially covered (1 defect found + FIXED)** | `tests/test_layout_extraction_corpus.cpp` (F16) is the harness the old deferral said was missing: **scanned/image-only path** — `test_ocr_multicol.pdf` OCR'd and asserted for column-major reading order (complete left/right partition over word boxes) and word accuracy (WER ≤ 0.15) against the AUTHORED text in `test_ocr_multicol.expected.txt`; **native-text path** — `test_PDFs/multicolumn.pdf` asserted as a complete line-level column partition, not the two word anchors `FidelityR3_07_TextExtraction` pins. **Found a real defect:** Tesseract merged both columns into single hOCR lines (every PSM), so scanned two-column text came back interleaved row-by-row; fixed by column re-derivation from word-box geometry in `HocrParser::parse` — see `docs/audit/meta-audit-2026-07/followup/phase-16-dispositions.md`. **Still open:** the EXTERNAL real-world files this section names (multi-column reflow sample, 132-page scanned election report) are not vendored — the coverage above is authored-ground-truth, not real-world scale |
| 5 | Encryption / access-control | **Covered** | Tier-1 `aes256_certificate` (graceful cert-encryption fail); Tier-2 `aes256_r5` (real R5 decrypt); Tier-3 `standard40`/`standard128`/`aes128` decrypt + `open_password` graceful-reject + permission trio (`no_copy`/`no_printing`/`no_text_access`). `aes256-pdf2` (R6) already covered by `test_encryption_manager` |
| 6 | Unusual dimensions & numeric edge cases | **Covered** | Tier-2 `mediabox_over_14400`, `artbox_under_3`, `real_underflow_structure`; **Tier-4** `nested-q-over-28` (gfx-state depth bound), `real-over-ieee754-and-pdfa-limit`, `real-under-pdfa-min-structure`, `real-under-pdfa-min-contentstream`. Deferred: pass-boundary micro-guard |
| 7 | Vector graphics complexity | **Covered** | Tier-1 `clippath`, `text_clip_cff_cid`, `indexedcs_negative_and_high`; Tier-2 `mesh_shading_empty`, `tiling_pattern_large_steps`, `shading_extend`; **Tier-4** `function-based-shading`, `knockout-blend-multiply`, `blendmode`, `transparency-group`. Deferred: further tiling variations |
| 8 | Overlapping / degenerate & annotation | **Covered** | Tier-1 `type3_content_stream_cycle` (recursion bound); Tier-2 `rotated_freetexts`, `zerowidthline`; **Tier-4** `annotation-empty-rect` (zero-area, no div-by-zero), `annotation-highlight-no-appearance` (renderer synthesizes AP). **Bomb coverage (F16):** the upstream `bomb-giant` download is still NOT vendored — its size and its upstream availability make it unreproducible from this tree — but the rationale that it awaited "a resource-limited sandbox harness" is retired, because that harness now exists: `tests/test_resource_bomb_sandbox.cpp` opens and renders a committed, deterministic 1029:1 bomb (`corrupt/decompression-bomb.pdf`, built by `tests/fixtures/gen_decompression_bomb.py`) in a CHILD PROCESS under a Windows Job Object memory cap + wall clock, and separately proves those caps kill a runaway child |
| 9 | Embedded / attached content & scripting | **Covered** | **Tier-4** `javascript` (SECURITY: `EmbeddedJavaScript_DoesNotAutoExecuteOnOpen` — no OpenAction/doc-JS execution on open, `isModified()==false`), `file-attachment-namedict` (/EmbeddedFiles name tree), `file-attachment-annotation` (page-level annot). Deferred: embedded video, signed 3D portfolio; `encrypted-attachment` unusable (unknown password) |
| 10 | Forms (AcroForm / XFA) | **Covered** | Tier-2 `checkbox_bad_appearance`; **Tier-4** `acroform-calc-order`, `xfa-filled` (3 MB real XFA), `xfa-issue14315`, `resetform` (reset-form action) — all open/render gracefully |
| 11 | PDF/A & PDF/UA conformance | **Covered locally (clause-sampled) — external corpora still NOT vendored, 2026-09-15, owner dev@itwerx.net** | meta-audit gap `PDF-108`, GitHub #8 / #14 / #7. **What landed:** a locally-authored, clause-sampled corpus at `tests/fixtures/flaw_corpus/conformance/`, generated deterministically by `tests/fixtures/gen_conformance_corpus.py`, exercised by `tests/test_pdfa_isartor_corpus.cpp` (10 PDF/A clause negatives + 3 controls), `tests/test_pdfua_clause_coverage.cpp` (4 PDF/UA clause negatives + 1 control) and `tests/test_conformance_corpus_sampling.cpp` (the corpus as an inventory). **The F16 objection is answered, not dropped.** F16 refused to fabricate clause fixtures because "a hand-made file asserting our own reading of a clause would be the same self-consistent oracle wearing a corpus's name" — correct about the naive version. Three things break the loop here: every negative is its CONTROL plus exactly ONE construct and the control must validate with ZERO errors (so a validator that rejected everything fails immediately); every negative must be rejected naming ITS clause while the control must NOT report that clause; and the clause list the fixtures must cover is parsed out of `src/compliance/PdfAValidator.cpp`, `ProhibitedFeatureScanner.cpp` and `PdfUAConverter.cpp` rather than typed into the test, so a clause added to the product with no fixture reddens the suite. **Still NOT claimed, and still open:** this is not an INDEPENDENT reading of ISO 19005/14289 — it pins PDFE's validator against files whose single differing construct is stated. Independent judgement comes only from veraPDF via `tests/support/ConformanceChecker.cpp` (`tests/test_conformance_independent.cpp`). The Isartor suite is not redistributable and the veraPDF corpus is a ~2,900-file network download; neither is vendored and neither should be |
| 12 | Bulk / scale sources | **Excluded by design** | GovDocs1, full pdf.js/pdfium/ghostscript corpora — unlabeled volume, for fuzz/scale not curated regression |
| 13 | Malicious datasets | **Excluded by design** | CIC-Evasive-PDFMal2022, UNSAFE-DOCS, Qiqqa "Evil Base" — never added to a general suite |

## Verification records (history — see "Current counts" above for today)

> The sections below are dated records of past runs. They are preserved
> verbatim as history. Where a figure in one of them was wrong **at the time it
> was written**, an indented correction note says so rather than editing the
> record.

### Verification record (2026-07-16)

- **`ExternalFlawCorpusTest`** (the combined corpus suite): **47 / 47 pass** —
  8 Tier-1 + 13 Tier-2 + 26 Tier-3.
- **Full `pdfe_tests` suite**: **4111 ran, 4105 passed, 0 failed, 6 skipped.**
  The 6 skips are the pre-existing XFA-slowness / external-real-world save
  gates (`IncrementalSaveRoutingTest.FormFillSaveIsFastOnXfa`,
  `ConditionalGcTest.SaveAsFastOnXfaWithoutStructuralOp`,
  `XfaSaveBenchmark.MeasureGcLevels`, three `ExternalRealWorldTest.SaveRoundTrip`
  cases) — unrelated to this work. `passed + skipped == ran`, so there is no
  gtest exit-code masking.
- **Product defects surfaced by the corpus: ZERO** (for Tiers 1-3).

### Verification record (2026-07-30) — Tier-4 expansion

- **`ExternalFlawCorpusTest`** (combined corpus suite): **70 pass, 1 DISABLED**
  (was 47) — added 24 Tier-4 cases across §3 image codecs, §6 numeric extremes,
  §7 vector/transparency, §8 degenerate annotations, §9 embedded/JS-non-exec,
  §10 forms. The 1 DISABLED is `DISABLED_JpxDecode_OpenRendersGracefully`
  (see below).
- **Full sharded suite** (`run_gtest.py --shards 16 --reruns 2`):
  **4749 enumerated, 4739 passed, 0 failed, 0 errors, 9 skipped; HARNESS_EXIT=0
  (CLEAN).** No crashed shard. The lone pre-rerun failure
  (`OcrLanguagePreferenceTest.DefaultLanguageIsEnglish`) healed on a fresh rerun
  (known transient). The 9 skips are the pre-existing XFA/real-world save gates.
- **Product defect surfaced by the corpus: ONE, real and security-relevant.**
  `jpx-bug.pdf` (malformed JPEG2000) heap-overflows **OpenJPEG 2.5.4 (latest)**
  during decode → STATUS_HEAP_CORRUPTION (0xC0000374) ~53% of runs; `fz_try/fz_catch`
  cannot trap it. Tracked as **#50**; root cause + evidence in
  `docs/audit/dca/0011-jpx-openjpeg-heap-overflow.md` and
  `docs/audit/jpx-openjpeg-heap-overflow-2026-07-30.txt`. The test's oracle is
  left intact and marked `DISABLED_` (the B1/B2 tracked-regression pattern — **not**
  a weakened assertion), re-enabled once #50 is fixed. All other 23 Tier-4 cases
  pass with **no product code changed to make them pass and no oracle weakened**.
- **`javascript.pdf`**: verified PDFE does **not** auto-execute an embedded
  OpenAction script on open (`isModified()` stays false through open + render +
  text-extract) — the §9 security-negative assertion is green.

### Verification record (2026-07-31) — #50 JPX defect FIXED (pipeline jpx-optin-49-50)

The #50 OpenJPEG heap-overflow is now **fixed** via a secure opt-in JPX feature
(commits `fc56e7d` / `f6ff1b1` / `2d51fca`):

- **MuPDF decode gate** (`vcpkg-overlay-ports/libmupdf/0001-pdfe-jpx-decode-gate.patch`):
  JPX decoding is DISABLED BY DEFAULT — `fz_load_jpx`/`fz_load_jpx_info` return a
  placeholder WITHOUT entering openjpeg while off, so a malformed JPX is never
  decoded. `FZ_ENABLE_JPX` stays on (runtime gate, not compile-out).
- **Opt-in preference** (`Security/jpxRenderingEnabled`, default off) wired to the
  gate at startup + on change, with a **warn-once** confirmation on enable.
- **Placeholder overlay**: the render layer draws "JPEG2000 image hidden for
  security — enable in Preferences" over each hidden JPX image.
- **Regression `JpxDecode_OpenRendersGracefully` is now ENABLED and passing** —
  it renders the former crasher `jpx-bug.pdf` with JPX off and asserts no crash
  (measured 15/15 no-crash, was 8/15 heap-corruption). `ExternalFlawCorpusTest`
  is now **74 pass, 0 DISABLED**.
  > **Correction (F9, 2026-08-02).** "74" was wrong when written. At that very
  > commit (`41a9c49`) `tests/test_external_flaw_corpus.cpp` already declared
  > **75** `TEST_F(ExternalFlawCorpusTest, ...)` cases — the figure was carried
  > forward by hand from the 2026-07-30 record (70) plus the four expected
  > additions, and was never derived from the source. The "0 DISABLED" half was
  > correct. This is precisely the hand-carried-number failure the machine
  > check at the top of this file now prevents.
  >
  > **Correction (GitHub #65/#66, 2026-09-12) — oracle demoted.** As of this
  > record, `JpxDecode_OpenRendersGracefully` was the ONLY behavioural oracle
  > for the gate, and it is a negative one: it would pass equally if MuPDF had
  > dropped the image, or if the gate had regressed and openjpeg really had
  > decoded the malformed stream without corrupting the heap on that run (the
  > crash was ~53% probabilistic — "15/15 no-crash" above is therefore weaker
  > evidence than it reads). The **load-bearing oracle is now
  > `JpxDecode_YieldsThePlaceholderNotARealDecode`**
  > (`tests/test_external_flaw_corpus.cpp`), added for SEC-84 / GitHub #65: it
  > asserts the decoded pixmap is the 1×1 placeholder `fz_load_jpx` returns
  > while JPX is off, contrasted against the dimensions the XObject dictionary
  > DECLARES, so an actual decode is detectable in a single non-crashing run.
  > `JpxDecode_OpenRendersGracefully` remains enabled and passing as a
  > secondary no-crash check; it is no longer the evidence the gate holds.
- **Full sharded suite: 4760 enumerated, 4752 passed, 0 failed, 0 errors, 8
  skipped; CLEAN.** (Skips dropped 9→8 — JpxDecode is no longer DISABLED.)
- No oracle weakened; the DCA (`docs/audit/dca/0011-jpx-openjpeg-heap-overflow.md`)
  records the resolution. **#50 CLOSED.**

### Verification record (2026-08-02) — counts reconciled + drift check added (F9)

- **`ExternalFlawCorpusTest`: 75 cases, 0 `DISABLED_`** — 8 Tier-1 + 13 Tier-2 +
  26 Tier-3 + 28 Tier-4, derived from the `TIER-MARKER` sections of
  `tests/test_external_flaw_corpus.cpp` rather than carried by hand. No test was
  added, removed, renamed, disabled or weakened by this phase; the source count
  was already 75 and only the documents were wrong.
- **Drift check added:** `FlawCorpusCountDocs` in
  `tests/test_doc_drift_guards.cpp` re-derives all six numbers on every suite
  run and fails if this file or `README.md` disagrees.
- **Product defects surfaced: none new.** (Tier-4's one real find, #50, was
  fixed on 2026-07-31 — see the record above.)

### Verification record (2026-08-02) — F16: fixture inventory, §4 harness, §8 bomb

- **Counts unchanged and machine-checked**: `ExternalFlawCorpusTest` 75 cases,
  0 `DISABLED_` (8 + 13 + 26 + 28). No corpus test was added, removed, renamed,
  disabled or weakened by F16.
- **Fixture inventory is now derived too.** `FlawCorpusFixtureDocs` walks
  `tests/fixtures/flaw_corpus/` and requires every `.pdf` to appear in exactly
  one README table. It immediately found one: `encryption/identity_stmf.pdf`
  (added by phase-3 for `PDF-25`) had been in the corpus, undocumented, in
  neither table — 72 files against 71 listed. Fixed by adding the Supplementary
  table. Fixture files: 47 (Tiers 1-3) + 24 (Tier 4) + 2 (Supplementary) = 73.
- **§4 is no longer Deferred.** `tests/test_layout_extraction_corpus.cpp` (3
  cases) asserts scanned-path column-major reading order and word accuracy
  against the authored `test_ocr_multicol.expected.txt`, plus a complete
  line-level column partition on the native-text path. **It found a real
  product defect on its first run** — Tesseract merges both columns into single
  hOCR lines, so scanned two-column text came back interleaved row-by-row (WER
  0.57). Fixed in `HocrParser::parse` by re-deriving columns from word-box
  geometry; WER after the fix is within the 0.15 bound. The external real-world
  files §4 also names remain un-vendored and are stated as such in the row above.
- **§8's bomb rationale was false and is retired.** The "resource-limited
  sandbox harness" it was waiting on did not exist; it does now
  (`tests/test_resource_bomb_sandbox.cpp`, 3 cases) and runs a committed 1029:1
  bomb out-of-process under a Job Object memory cap.
- **Product defects surfaced by this phase: ONE (the §4 reading-order defect),
  fixed here.** Plus one documentation defect (the undocumented fixture) and
  one false rationale (§8), both corrected and now guarded.

## Triage note

Because the corpus tiers assert behavior that was first characterized against
the same MuPDF 1.26 core PDFE ships (via PyMuPDF), the expected outcome is that
PDFE already handles them — the value is a durable regression guard against
future changes, not the discovery of current bugs. The one genuinely
unsupported path (certificate/public-key encryption) is asserted as a *graceful
failure*, not a pass. Any future red in this suite is therefore a real
regression signal.


### Recovery coverage is 4 of those 7 fixtures, not 7 (GitHub #126)

The seven Tier-3 `Corrupt_*` fixtures are described as the corrupt/recovery
class. **Three of them are malformed in ways MuPDF parses without rebuilding the
xref**, so they do not exercise the recovery path at all:

| fixture | forces xref reconstruction |
|---|---|
| `corrupt/one_byte_missing.pdf` | **yes** |
| `corrupt/xref_command_missing.pdf` | **yes** |
| `corrupt/helloworld_bad.pdf` | **yes** |
| `corrupt/scan_bad.pdf` | **yes** |
| `corrupt/multiple_filters_length_zero.pdf` | no — filter-chain edge case |
| `corrupt/pdfjsbad1586.pdf` | no — parser-bug regression |
| `corrupt/boundingbox_invalid.pdf` | no — invalid geometry |

That is fine, and all seven are worth having. What was not fine is that a reader
of this table would reasonably conclude all seven cover xref reconstruction — so
**a regression in the rebuild path would be caught by four tests, and nothing
said so.**

**This is now asserted rather than described.** Each of the seven tests calls
`repairPathTaken()` (which reads MuPDF's own `pdf_was_repaired()`) and asserts
the value above — `EXPECT_TRUE` for the four, `EXPECT_FALSE` for the three. The
table cannot silently go stale in either direction: a fixture that stops
repairing reddens, and so does one that starts.

Verified by mutation: with the probe stuck at `false` exactly the four
recovery tests fail; stuck at `true`, exactly the other three do.

**Naming caveat:** `Corrupt_PdfJsBad1586_RecoversGracefully` is named as though
it exercises recovery. It does not — the name predates this measurement. It was
left alone rather than renamed, because `FlawCorpusCountDocs` and the RTM key
off these test names; the assertion inside it now states the truth
unambiguously.

**Before adding more recovery tests**, note that only four fixtures currently
reach that path — adding more tests against the same four does not widen it.
