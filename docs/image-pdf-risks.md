# Image-based PDF Edit Pipeline — Known Risks

**Last updated:** 2026-04-12
**Source of findings:** Code review pass (2026-04-12) + verification pass against current code + dependency analysis.

This document tracks the six remaining risks identified in the PDFE
image-based PDF editing pipeline (TRE subsystem). Each entry has been
verified against the current code at the cited line numbers. Risks
misidentified by the original review (A1 overlay/save mismatch, A4
cursor alignment, B1 union bbox, B4 floor/ceil rounding, B5 stale Tr=3
— all of which were either misreads or intentional mitigations) are
explicitly excluded here.

---

## Dependency graph

```
              ┌────────────────────────────────┐
              │ A3 — Source image DPI tracking │  (enabling prerequisite)
              └───────────┬────────────────────┘
                          │
         ┌────────────────┼────────────────┬──────────────┐
         ▼                ▼                ▼              ▼
  ┌────────────┐   ┌────────────┐   ┌────────────┐  ┌────────────┐
  │    B3      │   │    B2      │   │     C1     │  │  C4 / C5   │
  │ halftone   │   │ measured   │   │  Lanczos   │  │ polish     │
  │ detection  │   │ width      │   │  upscale   │  │            │
  └────────────┘   └────────────┘   └────────────┘  └────────────┘

  ┌────────────┐   ┌────────────┐
  │    C2      │   │    A2      │  (both independent of A3)
  │ cache      │   │ script     │
  │ capacity   │   │ detection  │
  └────────────┘   └────────────┘
```

**A3 is a load-bearing prerequisite for B3, B2, and C1.** Every downstream
subsystem that reads pixels or produces bitmaps depends on knowing the
source image's actual DPI to tune kernel sizes, measure widths, and
choose upscale/downsample paths correctly. C2 and A2 are cleanly
independent and can ship in any order.

### Why ordering matters

- **B3** uses variance kernels and pattern-copy/inpainting. Both are
  scale-dependent. A halftone dot at 600 DPI is ~5px; at 150 DPI it's
  ~1px. A variance threshold tuned at one DPI silently misclassifies
  the others. Fixing B3 before A3 produces a detector that's correct
  by accident for one DPI fixture and wrong everywhere else.
- **B2** measures advance widths through `TextShaper`, but the font it
  measures is chosen by `FontMatcher`, whose size detection depends on
  DPI assumptions. The ratio is dimensionless, but the font is not.
- **C1** exists because rendering happens at the wrong DPI and the
  result gets upscaled. Fixing A3 lets us render at the right DPI in
  the first place, shrinking C1's scope to genuinely unavoidable
  scale mismatches.

## Risk index (revised ordering)

| # | ID | Risk | Severity | Subsystem | Depends on | Status |
|---|----|------|----------|-----------|------------|--------|
| 1 | A3 | Source image DPI not tracked | **HIGH** (as enabler) | `src/tre/BitmapCompositor.cpp` | — | Open |
| 2 | C2 | 3-page LRU cache defeats JPEG recompression fix | MEDIUM | `src/tre/ImageReplacer.cpp` | — | Open |
| 3 | B3 | Halftone / colored-background detection | HIGH | `src/tre/BgColorDetector.cpp` | A3 | Open |
| 4 | B2 | Char-count width expansion ignores kerning/ligatures | MEDIUM | `src/tre/ImageReplacer.cpp` | A3 | Open |
| 5 | A2 | Non-Latin scripts fall through baseline averaging | MEDIUM | `src/tre/FontMatcher.cpp` | — | Open |
| 6 | C1/C4/C5 | Image quality polish bundle | LOW-MED | `src/tre/{PdfContentWriter,GlyphAtlas,BitmapCompositor}.cpp` | A3 (C1) | Open |

A3 has been promoted to HIGH even though in isolation it's a MEDIUM
correctness issue, because leaving it open silently corrupts the
outcomes of three downstream fixes and their test suites.

---

## Testing philosophy

> **Every fix must be validated against a range of fixtures that
> reflects real-world variation, not a single synthetic datapoint.**
> A single-fixture test is a coincidental pass: the detector or
> estimator gets tuned to that exact fixture and silently fails on
> anything else. The real world has 72 DPI web captures, 150 DPI
> phone photos, 200 DPI fax, 300 DPI office scans, 600 DPI archival
> scans, and 1200 DPI OCR-optimized masters. It has Latin, CJK,
> Arabic, Hebrew, Thai, Devanagari. It has clean white backgrounds,
> yellowed newsprint, blue ledger paper, halftone photos, shaded
> scans, and JPEG-blocked low-quality originals.

Every risk below has an **expanded fixture matrix** that spans the
dimensions relevant to that specific risk. Tests must iterate over
the matrix, not cherry-pick one fixture.

> ## ⚠ THE PRESCRIPTION ABOVE WAS NOT FOLLOWED — measured 2026-08-03 (F19, gap A5-27)
>
> The fixes this document prescribes did land. **Its anti-coincidental-pass
> discipline did not.** Fixtures live in `tests/fixtures/image_pdf/`.
>
> | matrix | prescribed | on disk | consumed by a test |
> |---|---:|---:|---:|
> | A3 | 7 | 7 | **7** |
> | C2 | 6 | 5 | **0** |
> | B3 | 9 | 9 | **5** |
> | B2 | 8 | 0 | 0 |
> | A2 | 12 | 0 | 0 |
> | C145 | 6 | 0 | 0 |
> | **total** | **48** | **21** | **12** |
>
> - **Only A3 iterates.** `tests/test_source_image_dpi.cpp` loops its fixture
>   table (`SinglePageMatrix`, `:84`; `RoundTripInvariantAllFixtures`, `:160`).
>   That is the prescribed shape.
> - **B3 cherry-picks.** `tests/test_b3_bg_classification.cpp` hand-writes one
>   `TEST` per fixture (`:80`, `:99`, `:116`, `:133`, `:154`) and **silently
>   omits 4 of the 9 matrix rows**. No loop, no `INSTANTIATE_TEST_SUITE_P`.
>   This is exactly the cherry-pick the paragraph above forbids.
> - **C2's 5 fixtures have zero consumers** — generated, never used. So do
>   `b3_blue_ledger_300dpi`, `b3_gradient_shaded_300dpi`,
>   `b3_highlighted_yellow_300dpi` and `b3_jpeg_blocky_200dpi`.
> - **B2, A2 and C145 (26 fixtures) were never generated at all.**
> - **Every consumer guards on `fixtureExists(...)` + `GTEST_SKIP()`**
>   (e.g. `test_b3_bg_classification.cpp:81-82`,
>   `test_source_image_dpi.cpp:106`), so a missing fixture is a **silent
>   green**, not a failure — which defeats the anti-coincidental-pass intent at
>   the mechanism level, independently of how many fixtures exist.
>
> Consequently the "Final regression sweep" bullet near the foot of this
> document — *"the per-risk fixture matrices listed above, each iterated across
> all rows"* — **cannot be executed as written** and is annotated there too.
> This is recorded as an **open** gap. F19 did not build the 26 missing
> fixtures or convert B3 to an iterating suite; it stopped this document from
> claiming a discipline the tree does not practise.

---

## A3 — Source image DPI not tracked

- **Severity:** HIGH (prerequisite for B3, B2, C1)
- **File:** `src/tre/BitmapCompositor.cpp:74-106`; callers in `src/tre/ImageReplacer.cpp:268`

### Problem
`BitmapCompositor::renderText()` drives `FT_Set_Char_Size` with whatever
DPI the caller supplies, and every current caller passes
`ocrPipeline_->config().ocrDpi` (typically 300). The pipeline never
inspects the underlying image XObject's actual pixel dimensions
relative to the page's MediaBox to determine native DPI. For 600 DPI
archival scans, text is rendered at half resolution and bilinear-
upscaled. For 72 DPI web captures, text is rendered at ~4× the target
resolution and then downsampled, producing aliasing.

### Verified behavior
`BitmapCompositor.cpp:74-106` — `FT_Set_Char_Size(face, 0, fontSizePt*64, dpi, dpi)` with caller-supplied DPI, no validation. `ImageReplacer.cpp:268` — `config().ocrDpi` passed unconditionally.

### Scope (revised 2026-04-12 after reading the actual pipeline)

**The initial "thread source DPI through BitmapCompositor" framing
was too narrow.** Reading `OcrPipeline::renderPage()` at
`src/tre/OcrPipeline.cpp:257-277` and its consumers shows that the
entire TRE subsystem operates in a single working pixel space
defined by `config_.ocrDpi`. The pipeline renders the page via
MuPDF at `ocrDpi`, which means MuPDF decodes the image XObject at
its native resolution and downsamples/upsamples to produce a QImage
at exactly `ocrDpi`. Every downstream subsystem — FontMatcher,
BgColorDetector, BitmapCompositor, HocrParser, ImageReplacer,
RegionReconstructor — consumes that QImage and its associated
coordinates in one consistent DPI pixel space.

`ocrDpi` is therefore not a rendering hint. It is the working
coordinate system of the entire TRE subsystem. Fixing A3 means
introducing a **per-page working DPI** that can differ from the
document-level default, and threading it through every call site
that currently reads `config_.ocrDpi`.

### Fix direction (revised)

1. Add `Page::sourceImageDpi()` that inspects the dominant image
   XObject's pixel dimensions vs the page's CropBox dimensions in
   points. Returns 0 if no dominant image exists.
2. Introduce a **per-page working DPI** computed as
   `clamp(sourceImageDpi, 150, min(600, config_.ocrDpi*2))` when a
   source image is present, otherwise falling back to
   `config_.ocrDpi`.
3. Decide on one of two propagation strategies (this is the design
   decision that needs to be made before coding):
   - **(a) Re-run OCR at the per-page working DPI.** Correct but
     slow — each page with non-default DPI pays the full OCR cost.
     The cache in `OcrPipeline::Impl::hocrCache_` would need to be
     keyed by `(pageIndex, workingDpi)` rather than just
     `pageIndex`.
   - **(b) Run OCR once at `config_.ocrDpi` and scale HOCR bboxes**
     by `workingDpi/ocrDpi` when a page is edited at a different
     working DPI. Cheaper but assumes OCR accuracy is invariant to
     DPI scaling — approximately true for Tesseract but degrades
     below 150 DPI.
4. Thread the resolved per-page DPI through:
   - `OcrPipeline::renderPage(pageIndex, workingDpi)` — new overload
   - `FontMatcher::matchRegion(..., workingDpi)`
   - `BgColorDetector::detect(...)` — takes the QImage directly, so
     this one is automatic once renderPage uses the right DPI
   - `BitmapCompositor::renderText(..., workingDpi)`
   - `ImageReplacer::replaceRegion()` — resolves workingDpi and
     passes it down
   - `RegionReconstructor` — same
5. Add a sanity check: if inferred DPI is outside [36, 2400], log a
   warning and fall back to `ocrDpi`.
6. Audit all ~120 call sites that currently read `config_.ocrDpi`
   to distinguish "needs workingDpi" from "actually wants default".

### Honest estimate

500-1500 lines changed across 6-8 files. Non-trivial because it is
a design change to the TRE subsystem's coordinate contract, not a
bug fix in one function. This is the reason A3 is promoted to HIGH
and flagged as an enabling prerequisite: attempting B3, B2, or C1
before landing the working-DPI plumbing produces fixes that are
coincidentally correct at `ocrDpi=300` and silently wrong
elsewhere.

### Fixture matrix
| Fixture | DPI | Page | Content | Purpose |
|---------|-----|------|---------|---------|
| `a3_72dpi_web_letter.pdf` | 72 | US Letter | Black text on white | Web-capture low-DPI baseline |
| `a3_150dpi_a4_photo.pdf` | 150 | A4 | Phone photo of document | Mid-low DPI, phone capture |
| `a3_200dpi_fax_legal.pdf` | 200 | US Legal | Noisy fax-grade text | Legacy fax DPI |
| `a3_300dpi_office_letter.pdf` | 300 | US Letter | Standard office scan | Current default assumption |
| `a3_600dpi_archival_a4.pdf` | 600 | A4 | High-quality archival scan | Archival DPI — primary regression target |
| `a3_1200dpi_ocr_letter.pdf` | 1200 | US Letter | OCR-optimized master | Extreme high DPI |
| `a3_mixed_dpi_5page.pdf` | 72/150/300/600/1200 | mixed | One page per DPI | Per-page DPI tracking |

### Test assertions
- For each fixture, edit a known word and save. Compare the saved
  page image at native DPI against a reference: rendered text must
  land within ±2 px of the original baseline *at that DPI*, and
  glyph edges must not show bilinear softening (measured as high-
  frequency content in a Laplacian of Gaussian response).
- Across the matrix, the `Page::sourceImageDpi()` return value must
  match the fixture's actual DPI to within 5% (rounding / CropBox
  integer quantization).
- The mixed-DPI fixture must report different DPIs per page.

---

## C2 — 3-page LRU cache defeats JPEG recompression fix

- **Severity:** MEDIUM (silent quality loss, hard to diagnose)
- **File:** `src/tre/ImageReplacer.cpp:37-80`
- **Independent of A3**

### Problem
Commit `b4a59bf` added an LRU cache of decoded original page images to
prevent repeated JPEG recompression on sequential edits. The cache is
hardcoded to 3 pages. On any workflow that is not strictly sequential
(edit page 1, jump to 50, jump to 100, return to 1), page 1's cached
original has been evicted, and the re-edit reads
already-recompressed PDF data.

### Verified behavior
```cpp
// ImageReplacer.cpp:37-80
static constexpr int kMaxCachedPages = 3;
std::unordered_map<int, QImage> originalImageCache_;
std::vector<int> cacheAccessOrder_;  // LRU
```
Eviction at lines 67-72 drops oldest. No memory awareness, no
persistence, no configurable knob.

### Fix direction
1. Make capacity configurable via `AppSettings::imageCacheMaxBytes`
   with a sensible default (256 MB).
2. Evict on memory budget, not page count. Compute per-entry cost as
   `width * height * 4` (QImage::Format_ARGB32).
3. Optional second tier: persist decoded originals to
   `QStandardPaths::CacheLocation` as PNG (lossless) keyed by
   `(docHash, pageIndex)`. Clean up on `Document::close()` or when a
   full save lands.
4. Add metrics: cache hit/miss counters, eviction reason, current
   size in bytes. Expose via `ImageReplacer::cacheStats()`.

### Fixture matrix
| Fixture | Pages | DPI | Page size | Purpose |
|---------|-------|-----|-----------|---------|
| `c2_small_6page_300dpi.pdf` | 6 | 300 | Letter | 6 × ~24 MB = 144 MB (exceeds 3-page limit, fits in 256 MB) |
| `c2_large_12page_600dpi.pdf` | 12 | 600 | Letter | 12 × ~95 MB ≈ 1.1 GB (exceeds 256 MB budget) |
| `c2_tiny_20page_72dpi.pdf` | 20 | 72 | Letter | 20 × ~1.4 MB = 28 MB (cache all 20, verify no false eviction) |
| `c2_mixed_8page.pdf` | 8 | 72-600 | mixed | Mixed sizes for non-uniform budget |
| `c2_huge_3page_1200dpi.pdf` | 3 | 1200 | Tabloid | 3 × ~1.3 GB (single page exceeds budget — must either fit one or persist) |
| `c2_color_ledger_10page.pdf` | 10 | 300 | Legal | Color content, tests QImage conversion path |

### Test sequences
Each test must execute an **explicit access pattern** and verify that
pages edited earlier are still bit-identical to their originals when
re-edited:

1. **Non-sequential access:** edit page 0 (recording the original
   bytes), then edit pages 1, 3, 5, 2, 4, then re-edit page 0. Assert
   page 0's pre-edit original bytes match its first-edit reading to
   within zero JPEG round-trips.
2. **Memory budget saturation:** set cache budget to 50 MB, open
   `c2_large_12page_600dpi.pdf`, iterate through all 12 pages, verify
   eviction logs report memory-budget eviction not page-count
   eviction.
3. **Persistence survival:** if temp-disk persistence is implemented,
   set memory budget to 0 MB and verify all edits still avoid JPEG
   round-trip (via the disk cache).
4. **Cleanup:** close the document and assert temp files are gone.

---

## B3 — Halftone / colored-background detection

- **Severity:** HIGH (visible user-facing artifact)
- **File:** `src/tre/BgColorDetector.cpp:109-150`
- **Function:** `findDominantColor()`
- **Depends on:** A3 (for DPI-aware variance kernel sizing)

### Problem
The detector samples pixels in non-text gaps, builds a color histogram,
and returns the most frequent color as a single RGB triple. On
halftone-dithered archival scans and on colored paper (yellowing,
light-blue ledger paper, highlighted regions), the sampled "background"
is a mixture of many near-identical shades or a dot pattern. A solid
RGB fill produces a visible flat rectangle at edit boundaries.

### Verified behavior
`BgColorDetector.cpp:109-150` computes a histogram with no variance
check and no pattern detection. The `uniformity` metric at line 152
only measures how many samples fall within a color distance threshold
of the dominant color — it does not distinguish uniform backgrounds
from dithered ones.

### Fix direction
1. Classify the background as `Uniform`, `Gradient`, `Textured`, or
   `Halftone` using local variance (Laplacian or Sobel) measured at
   a kernel size scaled to **the source image's actual DPI** (hence
   the A3 dependency — a 300 DPI kernel of 3px = a 600 DPI kernel of
   6px, and using the wrong size misclassifies everything).
2. For textured / halftone backgrounds, copy the actual background
   pattern into the replacement region (translation sampling from a
   nearby clean area) rather than filling a solid color.
3. For severe cases, use Poisson blending or a small inpainting
   kernel whose radius is also DPI-scaled.
4. Return a classification enum so `ImageReplacer` can choose the
   right strategy per edit.

### Fixture matrix
| Fixture | DPI | Background | Purpose |
|---------|-----|------------|---------|
| `b3_uniform_white_300dpi.pdf` | 300 | Clean white | Baseline, must still classify as Uniform and use solid fill |
| `b3_yellowed_news_300dpi.pdf` | 300 | Uniform yellow/cream | Colored but uniform — solid fill of the detected color |
| `b3_halftone_news_200dpi.pdf` | 200 | 50% gray halftone | Classic newspaper halftone |
| `b3_halftone_news_600dpi.pdf` | 600 | 50% gray halftone | Same pattern at higher DPI — must scale kernel |
| `b3_blue_ledger_300dpi.pdf` | 300 | Blue ledger lines | Ruled paper with horizontal lines through text |
| `b3_textured_gray_300dpi.pdf` | 300 | Speckled / noise | Random-noise background (photographed paper) |
| `b3_gradient_shaded_300dpi.pdf` | 300 | Page shadow from photography | Smooth gradient; test Gradient classification |
| `b3_highlighted_yellow_300dpi.pdf` | 300 | Yellow highlighter rectangle | Piecewise — text partially over highlight |
| `b3_jpeg_blocky_200dpi.pdf` | 200 | JPEG blocking artifacts | Low-quality source, block-sized variance |

### Test assertions
- For each fixture, edit a word and save. Compare the resulting page
  image against a reference via:
  - **SSIM** outside the edit region must be ≥ 0.99 (nothing else
    changed).
  - **Edge-continuity metric** across the edit boundary: the
    background pattern must visually continue. Measure via Laplacian
    response integrated along the bbox edge — it should be
    statistically indistinguishable from the response on the same
    edge before editing.
  - **Classification correctness:** each fixture asserts the
    expected classification (Uniform / Gradient / Textured /
    Halftone).
- Halftone fixtures at 200 and 600 DPI must **both** classify as
  Halftone — this directly tests that the DPI-aware kernel sizing
  works (and catches the A3-dependency bug).

---

## B2 — Char-count width expansion ignores kerning/ligatures

- **Severity:** MEDIUM
- **File:** `src/tre/ImageReplacer.cpp:129-148`
- **Function:** `computeExpandedWidth()`
- **Depends on:** A3 (for correct font-size selection during measurement)

### Problem
When the replacement text is longer than the original, the compositor
expands the target bitmap width by `editU32.size() / origU32.size() * 1.2f`.
This ignores kerning (`"AV"` vs `"A V"`), ligatures (`"fi"` as one glyph),
Arabic final-form ligatures, and CJK fixed-width conventions. The 20%
safety margin papers over Latin cases but fails on dense Arabic and
heavily kerned Latin.

### Verified behavior
```cpp
// ImageReplacer.cpp:143-145
ratio = static_cast<float>(editU32.size()) / static_cast<float>(origU32.size());
ratio *= 1.2f;  // 20% safety margin
```
No call to `TextShaper::advance()` or `BitmapCompositor` to measure
actual rendered width.

### Fix direction
1. Render the candidate text once through `TextShaper::shape()` using
   the same font that will be used in the final composite, sum the
   advance widths, and compare against the original's measured width.
2. Apply the 20% safety margin only to the measured width, not to the
   ratio (the margin covers metric-vs-bitmap rounding, not
   translation uncertainty).
3. Fall back to the char-count formula for the degenerate case where
   the shaper is unavailable.

### Fixture matrix
| Fixture | Script | DPI | Notes | Purpose |
|---------|--------|-----|-------|---------|
| `b2_ascii_plain_300dpi.pdf` | Latin | 300 | Monospaced Courier | Control case — char count ≈ advance |
| `b2_kerned_latin_300dpi.pdf` | Latin | 300 | "AV", "To", "WA", "Ya" pairs in Garamond | Kerning-sensitive |
| `b2_ligated_latin_300dpi.pdf` | Latin | 300 | "fi", "fl", "ffi" ligatures | Ligature collapse |
| `b2_arabic_ligatures_300dpi.pdf` | Arabic | 300 | Arabic with initial/medial/final ligatures | Dense ligature script |
| `b2_arabic_ligatures_600dpi.pdf` | Arabic | 600 | Same at high DPI | DPI-scale cross-check with A3 |
| `b2_cjk_fixed_300dpi.pdf` | Simplified Chinese | 300 | Han ideographs | Fixed-width script |
| `b2_thai_stacked_300dpi.pdf` | Thai | 300 | Thai with above/below marks | Combining marks don't add width |
| `b2_mixed_latin_arabic_300dpi.pdf` | bidi | 300 | Latin + Arabic on same line | Bidi text run boundaries |

### Test assertions
- For each fixture, for each of three edit length changes (-30%,
  same, +30%), compute the expanded width and compare to a reference
  computed by rendering both strings through the real FreeType +
  HarfBuzz stack. Tolerance: ±5% or ±3 px, whichever is larger.
- The Arabic fixture at 600 DPI plus the same at 300 DPI must
  produce the same ratio (ratios are DPI-invariant), but different
  absolute pixel widths proportional to DPI — this directly asserts
  that A3 is correctly plumbed through.

---

## A2 — Non-Latin scripts fall through baseline averaging

- **Severity:** MEDIUM
- **File:** `src/tre/FontMatcher.cpp:88-127,141-182`
- **Independent of A3**

### Problem
FontMatcher's fix for Tesseract `x_size` underestimating the em-square
uses baseline-to-ascender averaging, gated by `if (!isCJK)`. `isCJK` is
detected by `detectScript()`, which today only distinguishes CJK from
everything else. Arabic, Hebrew, Thai, Devanagari, Lao, Myanmar, and
Burmese all have meaningful baselines but different ascender/descender
ratios. They fall into the non-CJK branch and get Latin-optimized
averaging, systematically biasing the detected font size.

### Verified behavior
`FontMatcher.cpp:141`:
```cpp
if (!isCJK) {
    // baseline-to-ascender averaging (Latin-tuned)
    ...
}
```
No per-script calibration. No explicit branch for Arabic/Thai/etc.

### Fix direction
1. Widen `FontScript` enum to include `Arabic`, `Hebrew`, `Thai`,
   `Devanagari`, `Lao`, `Myanmar`.
2. In `detectScript()`, sniff Unicode block ranges of the OCR text
   (U+0600-06FF Arabic, U+0590-05FF Hebrew, U+0E00-0E7F Thai,
   U+0900-097F Devanagari, etc.).
3. Per-script calibration table: CJK uses x_size only; Latin ~0.78
   ascender ratio; Arabic ~0.72 (taller letterforms); Thai/Devanagari
   ~0.80; Hebrew ~0.75.
4. Verify with fixtures that font-size error stays under ±10% across
   all supported scripts at multiple DPIs.

### Fixture matrix
| Fixture | Script | DPI | Sample font | Purpose |
|---------|--------|-----|-------------|---------|
| `a2_latin_serif_300dpi.pdf` | Latin | 300 | Times | Control — must not regress |
| `a2_latin_sans_600dpi.pdf` | Latin | 600 | Arial | Control at high DPI |
| `a2_cjk_simplified_300dpi.pdf` | Chinese (Simpl) | 300 | Noto Sans CJK SC | CJK baseline |
| `a2_cjk_traditional_600dpi.pdf` | Chinese (Trad) | 600 | Noto Serif CJK TC | CJK at high DPI |
| `a2_japanese_mixed_300dpi.pdf` | Japanese | 300 | Noto Sans JP | Mixed kanji+hiragana+katakana |
| `a2_korean_300dpi.pdf` | Korean | 300 | Noto Sans KR | Hangul |
| `a2_arabic_naskh_300dpi.pdf` | Arabic | 300 | Amiri | Arabic naskh baseline |
| `a2_arabic_naskh_600dpi.pdf` | Arabic | 600 | Amiri | Arabic high DPI |
| `a2_hebrew_300dpi.pdf` | Hebrew | 300 | Frank Ruehl | Hebrew baseline |
| `a2_thai_300dpi.pdf` | Thai | 300 | Sarabun | Thai with above/below vowels |
| `a2_devanagari_300dpi.pdf` | Devanagari | 300 | Mangal | Shirorekha + combining marks |
| `a2_mixed_latin_cjk_300dpi.pdf` | mixed | 300 | Noto Sans | Latin + CJK on same line |

### Test assertions
- For each fixture, FontMatcher's detected `sizePt` must be within
  ±10% of the fixture's known true size (recorded at generation time).
- Script detection must correctly classify each fixture's primary
  script.
- Distance-to-candidate scoring must respect script mismatch penalty
  (a Latin font must not be the top match for an Arabic line even if
  metrics are similar).

---

## C1/C4/C5 — Image quality polish bundle

- **Severity:** LOW-MEDIUM
- **Depends on:** A3 (for C1 — A3's fix eliminates most upscaling)

### C1: Bilinear upscaling instead of Lanczos
- **Files:** `src/tre/PdfContentWriter.cpp:160-161`,
  `src/tre/ImageReplacer.cpp:556-557,710-711`
- **Problem:** All three sites scale with `Qt::SmoothTransformation`
  (bilinear). For 2× upscales of text bitmaps, Lanczos preserves edge
  sharpness noticeably better.
- **Scope note:** Once A3 lands, most 2× upscales go away because we
  render directly at source DPI. C1 only matters for residual
  downsampling and unavoidable rounding cases.
- **Fix:** Lanczos-3 filter for ≥ 1.5× ratios; fall back to
  SmoothTransformation otherwise.

### C4: Euclidean RGB distance in alpha isolation
- **File:** `src/tre/GlyphAtlas.cpp:1034-1082`
- **Function:** `isolateAlpha()`
- **Problem:** Computes alpha as the ratio of Euclidean distance to
  textColor vs bgColor. Antialiased edges and shaded scans produce
  ragged alpha.
- **Fix:** Perceptual distance (Lab ΔE76 or weighted YCbCr);
  morphological close on the alpha channel post-isolation.

### C5: Grayscale FT_LOAD_RENDER, no LCD subpixel
- **File:** `src/tre/BitmapCompositor.cpp:132`
- **Problem:** `FT_Load_Glyph(face, id, FT_LOAD_RENDER)` produces
  grayscale coverage. Modern LCD subpixel rendering triples
  horizontal resolution for text on RGB-striped displays.
- **Fix:** Switch to `FT_LOAD_TARGET_LCD` and composite as RGB;
  fall back to grayscale when the output target doesn't support
  subpixel (print, archival save).

### Fixture matrix
| Fixture | DPI | Content | Purpose |
|---------|-----|---------|---------|
| `c145_antialiased_white_300dpi.pdf` | 300 | AA text on white | Baseline edge sharpness |
| `c145_antialiased_texture_300dpi.pdf` | 300 | AA text on noise | C4 alpha isolation stress |
| `c145_small_text_150dpi.pdf` | 150 | 6pt text | Low-DPI, subpixel AA most visible |
| `c145_large_text_1200dpi.pdf` | 1200 | 48pt text | High-DPI, upscale quality |
| `c145_colored_text_300dpi.pdf` | 300 | Dark-blue text on cream | Perceptual distance over Euclidean |
| `c145_shaded_scan_300dpi.pdf` | 300 | Gradient-shaded page | Alpha isolation with varying bg |

### Test assertions
- **C1:** After an edit+save cycle, the saved image's Laplacian
  response in text regions must be within 10% of the reference
  rendered with Lanczos, not bilinear.
- **C4:** The alpha channel of isolated glyphs must have edge
  smoothness (second derivative of alpha along glyph contour) below
  a threshold set by rendering the same glyph via FreeType directly.
- **C5:** Horizontal edge frequency content of LCD-rendered text
  must exceed grayscale-rendered text by a factor of 1.5 (proxy for
  "subpixel resolution is actually being used").

---

## Revised fix ordering

The pipeline plan tackles these in dependency order:

1. **A3** (phase 3 in the execution plan) — enables everything
   downstream; must land first or all subsequent fixes are
   coincidentally tuned against one DPI.
2. **C2** (phase 4) — ImageReplacer cache, cleanly isolated.
3. **B3** (phase 5) — halftone detection with DPI-aware kernels
   (uses A3's plumbing).
4. **B2** (phase 6) — measured advance widths using the now-correct
   font sizes from the A3-aware FontMatcher path.
5. **A2** (phase 7) — script detection widening, independent of the
   above.
6. **C1/C4/C5** (phase 8) — polish bundle; C1's scope is reduced by
   A3 having landed earlier.

Final regression sweep runs:
- Full GoogleTest suite (~2068 tests).
- The native UI smoke test in `tests/ui_automation/`.
- ~~The per-risk fixture matrices listed above, each iterated across
  all rows, asserting no coincidental passes.~~ **NOT EXECUTABLE as written
  (F19, gap A5-27):** 21 of the 48 prescribed fixtures exist and 12 are
  consumed; only the A3 matrix iterates. See the correction box in the
  "expanded fixture matrix" section above.
