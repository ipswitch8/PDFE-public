# Phase 4 — Test-Gap Analysis: why every suite missed all three defects — pipeline b91f4e83

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


**Date:** 2026-06-28
**Scope:** Analysis only — NO source/test changes. Explains why the existing
render-verify suite, FontMatcher gtests, per-line font-size tests,
AutoMissingGlyphResolver tests, the overlay-font-mapping tests, and
`test_vector_heading_commit.cpp` all failed to catch defects 1b, 2, and 3.
Grounded in a line-level read of each test file (assertions cited by line).

**Defect numbering recap (from Phases 1–3):**
- **1a (overlay wrong family)** — *disproven*: the overlay family resolves to
  serif "Palatino Linotype" correctly. There is no 1a fault to catch — but note
  (§2) no test would have caught it if there were one, and the overlay's family
  correctness is itself unverified.
- **1b (overlay dropped italic + bold)** — real: `buildOverlayFont`
  (`TextEditTool.cpp:529-530`) never calls `setItalic`/`setBold`; `TextSpan`
  carries no style field.
- **2 (overlay geometry offset)** — real: formatting-run branch baseline
  `screenBbox.top() + maxAscent` (`TextEditTool.cpp:1311`) overshoots the em-box.
- **3 (new-char wrong family, italic correct)** — real:
  `FontFamilyResolver::resolve("PalatinoLTPro", …)` returns empty (no Palatino
  alias, no serif default) → hard-coded Arial sans (`AutoMissingGlyphResolver.cpp:422`).

---

## 1. The single structural reason, stated up front

Two independent gaps combine so that **no test could see any of these defects**:

1. **No oracle asserts font IDENTITY (family serif-vs-sans, or slant direction)
   from rendered pixels — anywhere.** The render-verify layer asserts ink
   presence/absence, SSIM, blank-detection, golden-SSIM, and font-*size* ratios.
   The C++ layer asserts metric flags (`isItalic`, `isSerif`), resolved family
   *name strings*, and resolved file *paths* — never that the rendered glyphs are
   serif, nor that they slant. Family and slant are exactly the attributes the
   defects corrupt.
2. **No fixture is an embedded, subsetted, ITALIC SERIF font.** The closest
   fixture (`invoice_vector_heading.pdf`) is `HDJKHA+Arial,Bold` — **sans, bold,
   non-italic**. A sans non-italic source cannot exhibit serif→sans (it is
   already sans) or italic-drop (it has no italic to drop), so even the
   ink-ratio test that *does* exercise the commit path is blind to defects 1b/3
   by construction of its input.

Everything below is a consequence of these two gaps.

---

## 2. Per-file analysis (what it asserts / does not / which defects)

Legend for "could catch": ✔ could have caught, ✘ could not, — not applicable.

### 2.1 `tests/ui_automation/test_render_text_edit.py` (pytest, ~6 cases)
- **Asserts:** ink-preservation ratio in the heading band `ink_after >=
  0.6*ink_before` (`:79`, undo `:105-107`, redo `:127-129`); tool routing
  `active == "treToolAction"` (`:144`, `:221-223`); canvas changed via
  SSIM<0.95/pixel-diff (`:165`, `:184`); form-field undo image match (`:201`).
- **Does NOT assert:** rendered serif-vs-sans; rendered slant; overlay-over-
  underlay alignment; new-char family/slant. Crops to a pre-defined bbox and
  measures ink in isolation.
- **Could catch:** 1b ✘ (ink-ratio is style-agnostic), 2 ✘ (measures a static
  region, never overlay-vs-underlay offset), 3 ✘ (fixture heading is Arial sans
  → substitute Arial sans is *correct* there; and only ink is checked).

### 2.2 `tests/ui_automation/harness/render_oracle.py` (oracle library)
- **Provides:** `capture_canvas` (`:250`); `canvas_changed` SSIM<0.95 OR
  pixel-diff≥1e-4 (`:120-133`); `images_differ` ≥2% pixel diff (`:296-313`);
  `region_is_blank` ≥98% background (`:316-340`); `matches_golden` SSIM≥0.85
  (`:359-378`); `save_golden` (`:343`); `RenderOracle` wrapper (`:388`).
- **Does NOT provide:** any serif/sans classifier; any slant/italic-angle
  measure; any overlay-vs-underlay alignment compare. **Entirely
  style-agnostic** — this is the root harness gap for 1a/1b/3.
- **Could catch:** 1b ✘, 2 ✘, 3 ✘ — no primitive exists to express any of these.

### 2.3 `tests/ui_automation/harness/render_drive.py` (driver helpers)
- **Provides:** `capture` (`:190`); `crop_to_page_content` (`:195`);
  `crop_pdf_bbox` (`:280`); `assert_changed` (`:558-564`); `assert_not_blank`
  (`:570-572`).
- **Does NOT provide:** `capture_overlay_over_underlay` or any helper returning
  the overlay band AND the same pre-edit underlay band for comparison.
  `crop_pdf_bbox` crops a *static* PDF bbox, not a placement comparison.
- **Could catch:** 2 ✘ (no overlay-vs-underlay capture exists — the exact
  missing primitive); 1b/3 ✘.

### 2.4 `tests/test_font_matcher.cpp` (gtest, ~50 cases)
- **Asserts:** size formula `EXPECT_NEAR(fontSizePt,…)` (`:158`,`:455`);
  xHeightRatio (`:161`); weight enum (`:164`,`:456`); `isItalic` flag from
  baseline slope — true at slope 0.15 (`:578`), false at 0.01 (`:596`),
  `EXPECT_FALSE(isItalic)` (`:167`,`:457`); `isSerif` *default* false (`:170`,
  not a measurement); match returns non-empty path (`:328`,`:414`); serif
  fallback name contains "times"/"liberationserif" (`:418-430`).
- **Does NOT assert:** that a returned font *renders* serif; that italic
  *renders* slanted (only the flag); overlay geometry; new-char family/slant.
- **Could catch:** 1b ✘ (this is OCR/raster FontMatcher, a different path from
  the embedded-span overlay; asserts a flag, not overlay rendering), 2 —, 3 ✘
  (new-char path is `AutoMissingGlyphResolver`/`FontFamilyResolver`, not tested
  here).

### 2.5 `tests/test_font_matcher_per_run_script.cpp` (gtest, ~7 cases)
- **Asserts:** per-run script detection `script == Latin/Arabic` (`:88`,
  `:120-121`,`:168`); script-mismatch distance penalty 0.0 vs 8.0
  (`:140-144`,`:181`). Synthetic `u32string`, no PDF.
- **Does NOT assert:** family/slant rendering, geometry, new-char.
- **Could catch:** 1b ✘, 2 —, 3 ✘ (script matching is orthogonal to the
  serif→sans family-resolution fault).

### 2.6 `tests/test_font_matcher_per_line.cpp` (gtest, ~5 cases)
- **Asserts:** per-line size differs (`:97-101`); per-line refactor leaves
  `isItalic` flag correct — false for mixed sizes (`:162`), true at slope 0.12
  (`:171`).
- **Does NOT assert:** rendered slant (flag only); family; geometry; new-char.
- **Could catch:** 1b ✘ (flag, not overlay render), 2 —, 3 ✘.

### 2.7 `tests/test_per_line_font_size_wiring.cpp` (gtest, ~7 cases)
- **Asserts:** per-line size formula head 30 / body 11 (`:72-74`); clicked-index
  selection (`:95-98`); empty/zero-sentinel fallbacks (`:108`,`:127-128`). This
  is purely about *size* wiring into the overlay.
- **Does NOT assert:** family, slant, geometry alignment, new-char. It wires
  per-line *size* to the overlay but never measures overlay *position*.
- **Could catch:** 1b ✘, 2 ✘ (size, not baseline placement vs underlay), 3 ✘.

### 2.8 `tests/test_vector_heading_commit.cpp` (gtest, 2 cases) — the closest test
- **Asserts:** heading uses subset font `hasSubsetPrefix` (`:170`); ink before
  >0.02 (`:187`); **`inkAfter >= 0.6*inkBefore` after commit** (`:218`, the D8-1
  regression lock); resolver returns non-empty runs for Q/u/t (`:254`).
- **Does NOT assert:** that the substituted glyphs are **serif** (only that they
  are not blank `.notdef`); that they preserve **italic**; overlay geometry;
  family congruence with the source.
- **Fixture:** `invoice_vector_heading.pdf` = `HDJKHA+Arial,Bold` (**sans, bold,
  non-italic, subset**).
- **Could catch:** **1b ✘ — source is non-italic, so there is no italic to
  drop.** **3 ✘ — source is Arial *sans*, so resolving a new glyph to Arial sans
  is the CORRECT outcome here; the serif→sans bug only appears when the source is
  serif and the name fails to resolve. The fixture masks the defect.** **2 ✘ —
  ink ratio is position-insensitive.** This test exercises the exact commit/
  resolver code path but its sans-non-italic fixture + ink-only oracle make it
  blind to all three. *This is the headline finding: the one test on the right
  code path has the wrong fixture and the wrong oracle.*

### 2.9 `tests/test_auto_missing_glyph_resolver.cpp` (gtest, ~12 cases)
- **Asserts:** null inputs → empty (`:49`); ≥1 run for unencodable char
  (`:232`); run carries Type3 XOR system font (`:263-266`); run size matches
  source (`:281`); adjacent same-dest runs collapse (`:297-301`); **AC1: every
  returned run's font actually encodes its codepoints `FT_Get_Char_Index != 0`**
  (`:375`); resolve <500ms (`:417`).
- **Fixture:** synthesized symbol font (Webdings/Wingdings) embedded into
  `test_single.pdf` in SetUp.
- **Does NOT assert:** that the resolved fallback **family matches the source's
  serif/sans class**; that it preserves the source **slant**. AC1 proves
  *encodability* (no `.notdef`), not *identity*.
- **Could catch:** 3 ✘ — a Webdings symbol source has no serif/sans/italic
  identity to match, so the test deliberately can't (and doesn't) assert family/
  slant congruence; the very property defect 3 violates is untested. 1b —, 2 —.

### 2.10 `tests/test_overlay_font_mapping.cpp` (gtest, ~25 cases)
- **Asserts:** `mapPdfFontToQtFamily` resolves Helvetica→Arial (`:138`),
  subset-prefixed Helvetica→Arial (`:148`), Times-Roman→Times New Roman (`:160`)
  via `QFontInfo` family **name**; FontMatcher returns non-empty family (`:317`);
  sans fallback "Arial" (`:454`), serif fallback "Times New Roman" (`:478`);
  size-override behaviour (`:573-578`); visible-extent default 0.85 (`:663`).
- **Does NOT assert:** the overlay QFont's **italic** or **bold** flags (the very
  attributes 1b drops — `buildOverlayFont` is invoked here but the tests only
  read `QFontInfo::family()`, never `italic()`/`weight()`); rendered pixels;
  geometry; new-char.
- **Could catch:** **1b ✘ — this is the most damning miss: the test constructs
  the overlay QFont via `buildOverlayFont` and inspects it, but only checks the
  family name, never `QFont::italic()`/`bold()`. A single
  `EXPECT_TRUE(font.italic())` on an italic-source span here would have caught
  1b at the unit level.** 1a would also be uncaught for the same reason (family
  *name* is checked, but for Palatino there is no test case at all). 2 —, 3 ✘.

### 2.11 `tests/test_font_family_resolver.cpp` (gtest, 6 cases)
- **Asserts:** Arial / Arial-Bold / Arial-Italic / Arial-BoldItalic resolve to
  non-empty paths (`:23`,`:28`,`:33`,`:38`); cache hit equality (`:50`);
  a non-existent family name ("NonExistentFont12345") resolves to **empty**
  (`ResolveNonExistent_Empty`, ~`:41-43`); different styles don't crash.
- **Does NOT assert:** that a resolved path is serif; that italic paths differ
  from upright; **that an unresolvable *serif* name (e.g. "PalatinoLTPro") yields
  a serif fallback rather than an empty string that the caller defaults to sans.**
  The one unresolved-name case ("NonExistentFont12345") only proves `resolve`
  *returns empty* — it does NOT exercise the downstream behaviour in
  `AutoMissingGlyphResolver` (`:421-422`) where an empty return triggers the
  hard-coded `resolve("Arial", …)` sans default. So the defect-3 chain (serif
  source name unresolved → empty → Arial sans) is untested end-to-end: no test
  feeds a serif family name, and no test asserts the eventual fallback is serif.
- **Could catch:** **3 ✘ — directly tests the faulting function but only with
  "Arial"; never an unresolvable serif name, so the empty-return → sans-default
  behaviour is untested.** 1b —, 2 —.

---

## 3. Cross-cutting gap statements (acceptance criteria 3–7)

### 3.1 Font FAMILY identity is asserted NOWHERE in the render-verify layer (AC3)
No Python oracle classifies serif vs sans from pixels (`render_oracle.py`
offers only ink/SSIM/blank/golden — §2.2). C++ tests assert family *name
strings* (`test_overlay_font_mapping.cpp`) or file *paths*
(`test_font_family_resolver.cpp`), never that the rendered glyph is serif. So a
correct-name-but-wrong-render, or the actual defect 3 (wrong name resolved to
sans), is invisible to every render test.

### 3.2 Font SLANT preservation is asserted NOWHERE for the overlay path (AC4)
Italic exists only as a *flag* in OCR-side FontMatcher tests
(`isItalic`/`baselineSlope`, §2.4/2.6) — never as a rendered slant, and never on
the embedded-span overlay path. `test_overlay_font_mapping.cpp` builds the
overlay QFont but checks only `family()`, not `italic()` (§2.10). Therefore
**defect 1b (overlay drops italic) could not have been caught by any existing
test** — the closest one is a single missing `EXPECT_TRUE(font.italic())`.

### 3.3 No embedded ITALIC-serif-subset fixture exists (AC5)
`tests/fixtures/` (and README) contain **no embedded+subsetted italic serif**
PDF. The only subset heading fixture is `invoice_vector_heading.pdf` =
`HDJKHA+Arial,Bold` — **sans, bold, non-italic**. All serif references in the
corpus are *system* fonts (Liberation Serif / Times New Roman / Tinos), not
embedded subsets. Because the heading-commit fixture is sans-non-italic:
- it cannot exhibit serif→sans (already sans), so defect 3 is masked;
- it cannot exhibit italic-drop (no italic), so defect 1b is masked.
This is why the one test on the correct code path (§2.8) is structurally blind.

### 3.4 Overlay geometry alignment gap (AC6)
No test captures the overlay positioned OVER the underlying text and measures
the offset. `render_drive.py` has no `capture_overlay_over_underlay`; geometry-
adjacent tests measure a region in isolation (`crop_pdf_bbox`) or wire *size*
(`test_per_line_font_size_wiring.cpp`) without measuring *placement*. So defect 2
(baseline overshoot leaving the underlay peeking) has no oracle.

### 3.5 New-char family+slant gap, attributes tested independently (AC7)
No test injects a character absent from an embedded font and asserts the
rendered result matches the surrounding **family (serif)** WHILE separately
asserting the source **slant (italic) is preserved**.
`test_vector_heading_commit.cpp` checks only ink (not .notdef);
`test_auto_missing_glyph_resolver.cpp` checks only encodability (AC1), on a
symbol font with no serif/italic identity. The two attributes must be asserted
*independently* so a future fix that corrects family but drops italic (or
vice-versa) is caught — neither is asserted today.

---

## 4. Prioritized new assertions/fixtures for Phase 6 (AC8)

Each maps to a defect sub-fault and the gap above. Ordered by
severity × cheapness.

1. **[fixture] `embedded_italic_serif_subset.pdf`** — an embedded, subsetted,
   italic serif span (ItalicAngle≠0 / Flags Italic bit; serif face; subset
   prefix), matching the PalatinoLTPro-BoldItalic profile. Unblocks 1b/3 testing.
   *(Gap §3.3; defects 1b, 3.)*
2. **[gtest] overlay QFont carries italic+bold** — in
   `test_textedit_font_identity.cpp`: build the overlay font for an italic-serif-
   subset span and `EXPECT_TRUE(font.italic())` + bold check. Catches 1b at unit
   level — the single assertion `test_overlay_font_mapping.cpp` omitted.
   *(Gap §3.2; defect 1b.)*
3. **[gtest] new-char resolves to SERIF** — `AutoMissingGlyphResolver` /
   `FontFamilyResolver` for a char absent from the italic-serif subset returns a
   **serif** face (not Arial sans). Catches defect 3's family fault directly.
   *(Gap §3.1/§3.5; defect 3.)*
4. **[gtest] new-char preserves ITALIC, asserted independently** — the same
   resolved fallback is italic (locks the correct behaviour separately from
   family, so a future regression in either is caught). *(Gap §3.5; defect 3.)*
5. **[gtest] `FontFamilyResolver` unknown-serif-name fallback** — resolving an
   unresolvable serif family name (e.g. "PalatinoLTPro") returns serif, not an
   empty string that defaults to sans. Locks the §2.11 untested branch.
   *(Gap §3.1; defect 3.)*
6. **[oracle] `assert_overlay_font_class(img, "serif"|"sans")`** — pixel
   serif-vs-sans classifier (stroke-modulation / ink-distribution heuristic) in
   `render_oracle.py`. The missing identity primitive. *(Gap §3.1; defects 1a/3.)*
7. **[oracle] `assert_slant_matches(img, expected_italic: bool)`** — directional
   slant measure (ink-centroid column regression) that fails on missing italic
   AND on unwanted italic. *(Gap §3.2; defect 1b.)*
8. **[oracle+driver] `assert_overlay_alignment` + `capture_overlay_over_underlay`**
   — capture the overlay band and the pre-edit underlay band and assert the
   bbox offset ≤ N px. The missing geometry primitive. *(Gap §3.4; defect 2.)*
9. **[render test] end-to-end on the new fixture** — open the italic-serif-subset
   fixture, edit the span, and assert overlay class=serif (6), slant=italic (7),
   alignment≤N px (8); then type an absent char and assert it is serif (3) AND
   italic (4). *(Gaps §3.1–3.5; defects 1b, 2, 3.)*
10. **[gtest] serif/weight congruence** — resolved fallback's serif class matches
    source, and weight within a defined distance of source. Hardens 3 against
    partial fixes. *(Gap §3.1; defect 3.)*

---

## 5. Answers to Phase-4 acceptance questions

1. **Every relevant file covered** — §2.1–2.11 (the 8 named files + the two
   directly-relevant extras `test_overlay_font_mapping.cpp` and
   `test_auto_missing_glyph_resolver.cpp`).
2. **Per file: asserts / does-not / which defect catchable** — §2, with the
   ✔/✘ "could catch" mapping for 1b/2/3 (1a N/A — disproven).
3. **Family identity not asserted in render-verify** — §3.1.
4. **Slant preservation not asserted for overlay (1b uncatchable)** — §3.2.
5. **No embedded italic-serif-subset fixture; heading-commit fixture is
   sans-non-italic → masks 1a/3** — §3.3 (+ §2.8).
6. **Overlay geometry alignment gap** — §3.4.
7. **New-char family+slant gap, asserted independently** — §3.5.
8. **Prioritized 6–10 new assertions/fixtures** — §4 (10 items, each mapped to a
   defect and gap).
