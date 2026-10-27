# Metric-Compatible Font Substitution System Architecture

**Date:** 2026-02-05
**Status:** Proposed
**Author:** Backend Architect
**Complexity:** Complex (multi-component pipeline with caching, UI integration, and PDF structural manipulation)

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Component Architecture](#2-component-architecture)
3. [Embedded Font Analysis Pipeline](#3-embedded-font-analysis-pipeline)
4. [Substitute Font Selection Algorithm](#4-substitute-font-selection-algorithm)
5. [Glyph Scaling and Adjustment](#5-glyph-scaling-and-adjustment)
6. [Font Switching in PDF Content Streams](#6-font-switching-in-pdf-content-streams)
7. [Font Subset Extension](#7-font-subset-extension)
8. [User Interface for Font Choice](#8-user-interface-for-font-choice)
9. [Cache Strategy](#9-cache-strategy)
10. [Integration with Text Editing](#10-integration-with-text-editing)
11. [Edge Cases](#11-edge-cases)
12. [Data Flow: Keystroke Through Substitution](#12-data-flow-keystroke-through-substitution)
13. [C++ Header Signatures](#13-c-header-signatures)
14. [Implementation Roadmap](#14-implementation-roadmap)
15. [Architecture Decision Records](#15-architecture-decision-records)
16. [Sources](#16-sources)

---

## 1. Executive Summary

### Problem Statement

A PDF embeds "CorporateBrandFont-Bold" as a subset containing only 47 glyphs used in the original document. The user opens this PDF in PDFE and wants to add text requiring characters not in that 47-glyph subset. The full "CorporateBrandFont-Bold" is not installed on the user's system.

This is not a corner case. It is the normal state of most PDFs: nearly all professional PDF generators subset fonts by default to reduce file size. Any text editing of such a document immediately hits the missing-glyph boundary.

### Solution Architecture

The system operates as a five-stage pipeline:

1. **Full Font Search** -- Attempt to find the complete original font on the system
2. **Embedded Font Analysis** -- Extract precise visual metrics from the subset's glyph outlines
3. **Substitute Selection** -- Score and rank candidate fonts using measured metrics (not just PANOSE)
4. **Glyph Adjustment** -- Scale and transform substitute glyphs to harmonize with the original
5. **PDF Embedding** -- Embed the substitute as a secondary font with automatic font switching in the content stream

### Key Architectural Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Matching signal | Measured outline metrics over PANOSE metadata | PANOSE is often missing or wrong; measured metrics from actual glyphs are ground truth |
| Embedding strategy | Dual-font (original subset + substitute subset) | Avoids modifying the original font program, which would invalidate any digital signatures and risks corrupting glyph data |
| Substitution trigger | On text commit, not on keystroke | Per-keystroke font analysis would create unacceptable latency; during editing, a preview font is used |
| Cache scope | Per-font-program hash, persisted to disk | Font analysis is expensive (~50-200ms per font); caching across sessions is essential |

---

## 2. Component Architecture

### System Diagram

```
+====================================================================+
|                     Font Substitution Coordinator                   |
|  (Orchestrates the full pipeline, owns the decision workflow)       |
+====+=========+=========+=========+=========+=========+=============+
     |         |         |         |         |         |
     v         v         v         v         v         v
+--------+ +--------+ +--------+ +--------+ +------+ +---------+
| Full   | |Embedded| |Subst.  | |Glyph   | | PDF  | | Font    |
| Font   | |Font    | |Font    | |Scale   | | Font | | Analysis|
| Finder | |Analyzer| |Selector| |Adjuster| | Emb- | | Cache   |
|        | |        | |        | |        | | edder| |         |
+---+----+ +---+----+ +---+----+ +---+----+ +--+---+ +----+----+
    |          |          |          |          |          |
    v          v          v          v          v          v
+====================================================================+
|                     Font Data Access Layer                          |
|  FreeType 2 | HarfBuzz | Fontconfig/DirectWrite | QPDF            |
+====================================================================+
```

### Component Responsibilities

| Component | Responsibility | Key Dependencies |
|-----------|---------------|------------------|
| **FontSubstitutionCoordinator** | Orchestrates the full pipeline; makes the final decision about which strategy to use (extend subset vs. substitute) | All components below |
| **FullFontFinder** | Searches system and bundled fonts for the complete original font by PostScript name, family name, and embedded metadata | Fontconfig/DirectWrite, bundled font index |
| **EmbeddedFontAnalyzer** | Parses the embedded font program and extracts measured visual metrics from glyph outlines | FreeType 2 |
| **SubstituteFontSelector** | Scores candidate fonts against the measured metrics profile and returns a ranked list | FreeType 2, FontAnalysisCache, bundled font index |
| **GlyphScaleAdjuster** | Computes and applies affine transforms to substitute glyphs so they match the original font's visual metrics | FreeType 2, HarfBuzz |
| **PdfFontEmbedder** | Handles the PDF-level mechanics: extending a font subset, or embedding a new substitute font and rewriting content streams for font switching | QPDF, FreeType 2, HarfBuzz |
| **FontAnalysisCache** | Persists font analysis results (metric profiles, similarity scores) to disk to avoid recomputation | SQLite or flat file store |

### Dependency Graph

```
FontSubstitutionCoordinator
    |
    +---> FullFontFinder
    |         +---> PlatformFontDiscovery (Fontconfig / DirectWrite)
    |         +---> BundledFontIndex
    |
    +---> EmbeddedFontAnalyzer
    |         +---> FreeType 2 (glyph outline access)
    |         +---> FontAnalysisCache
    |
    +---> SubstituteFontSelector
    |         +---> EmbeddedFontAnalyzer (consumes metric profile)
    |         +---> PlatformFontDiscovery
    |         +---> BundledFontIndex
    |         +---> FontAnalysisCache
    |
    +---> GlyphScaleAdjuster
    |         +---> FreeType 2 (outline access + metrics)
    |         +---> HarfBuzz (shaping for kerning/positioning)
    |
    +---> PdfFontEmbedder
    |         +---> QPDF (PDF object manipulation)
    |         +---> HarfBuzz (subsetting via hb-subset)
    |         +---> FreeType 2 (glyph extraction)
    |
    +---> FontSubstitutionDialog (UI)
              +---> Qt 6 Widgets
              +---> SubstituteFontSelector (for candidate list)
              +---> EmbeddedFontAnalyzer (for metric display)
```

---

## 3. Embedded Font Analysis Pipeline

### 3.1 What to Measure

The analysis pipeline extracts two categories of metrics from the embedded font's available glyphs.

#### Category 1: Global Font Metrics (from font tables)

These are read directly from the font program's tables via FreeType:

| Metric | Source | FreeType API |
|--------|--------|-------------|
| Units per EM | `head` table | `face->units_per_EM` |
| Ascender (design) | `OS/2` table `sTypoAscender` | `TT_OS2->sTypoAscender` |
| Descender (design) | `OS/2` table `sTypoDescender` | `TT_OS2->sTypoDescender` |
| Line gap | `OS/2` table `sTypoLineGap` | `TT_OS2->sTypoLineGap` |
| xAvgCharWidth | `OS/2` table | `TT_OS2->xAvgCharWidth` |
| Weight class | `OS/2` table `usWeightClass` | `TT_OS2->usWeightClass` |
| Width class | `OS/2` table `usWidthClass` | `TT_OS2->usWidthClass` |
| PANOSE classification | `OS/2` table `panose` | `TT_OS2->panose[0..9]` |
| Cap height (if present) | `OS/2` v2+ `sCapHeight` | `TT_OS2->sCapHeight` |
| x-height (if present) | `OS/2` v2+ `sxHeight` | `TT_OS2->sxHeight` |
| Italic angle | `post` table | `TT_Postscript->italicAngle` |
| Serif style | PANOSE byte 1 | `TT_OS2->panose[1]` |

However, these table values are often incorrect, missing (especially in subset fonts where tables are stripped), or non-standardized. The following measured metrics are the real signal.

#### Category 2: Measured Metrics (from glyph outlines)

These are computed by analyzing the actual glyph outlines present in the subset. This is the core insight: with even a small number of glyphs, we can extract highly discriminating visual characteristics.

| Metric | How Computed | Minimum Glyphs Needed |
|--------|-------------|----------------------|
| **Measured x-height** | Bounding box top of lowercase glyphs (`x`, `o`, `z`, `n`, `e`) | Any 1 lowercase |
| **Measured cap height** | Bounding box top of uppercase glyphs (`H`, `I`, `E`, `T`) | Any 1 uppercase |
| **Measured ascender** | Bounding box top of `b`, `d`, `h`, `k`, `l` | Any 1 ascender glyph |
| **Measured descender** | Bounding box bottom of `p`, `q`, `g`, `y`, `j` | Any 1 descender glyph |
| **Stem weight (vertical)** | Width of the vertical stroke in `I`, `l`, `H`, or `T`, measured by analyzing x-coordinates of outline segments at the vertical midpoint | Any 1 of these glyphs |
| **Stem weight (horizontal)** | Height of the horizontal stroke in `H`, `E`, `T`, measured by analyzing y-coordinates | Any 1 of these glyphs |
| **Average character width** | Mean of advance widths across all available lowercase Latin glyphs | 5+ lowercase |
| **Width-to-height ratio** | `average_char_width / measured_cap_height` | 5+ glyphs |
| **Contrast ratio** | `vertical_stem_weight / horizontal_stem_weight` -- measures monolinear vs. high-contrast design | `H` or `E` |
| **Serif detection** | Analyze horizontal extent of terminal segments at baseline and cap line of `I`, `l`, `T` -- serifs extend horizontally beyond the stem | `I` or `l` |
| **Serif bracket depth** | If serifs present: vertical distance over which the serif transitions into the stem | `I` or `l` |
| **x-height to cap-height ratio** | `measured_x_height / measured_cap_height` -- strongly characterizes a typeface's visual proportion | 1 lowercase + 1 uppercase |
| **Roundness** | Ratio of curve segments to straight segments in `o`, `e`, `c` outlines -- distinguishes geometric from humanist designs | `o` or `e` |
| **Counter openness** | Width of the opening in `e`, `c`, `a` -- distinguishes open vs. closed counter designs | `e` or `c` |

### 3.2 Stem Weight Measurement Algorithm

Stem weight is the single most discriminating metric for visual matching. Here is the algorithm:

```
ALGORITHM: MeasureVerticalStemWeight(glyph_outline, units_per_em)

  1. Load the glyph outline decomposed into segments (FT_Outline_Decompose)

  2. Choose a measurement y-coordinate:
     y_measure = glyph_bbox.yMin + (glyph_bbox.yMax - glyph_bbox.yMin) * 0.5
     (midpoint of the glyph, avoids serifs at top/bottom)

  3. Cast a horizontal ray at y = y_measure

  4. Find all intersection points with the outline
     Sort intersections by x-coordinate

  5. Intersections come in pairs (entry/exit from filled regions)
     The stem width is the distance between the first entry-exit pair:
     stem_width = x_intersections[1] - x_intersections[0]

  6. If multiple stems are detected (e.g., in 'H'), take the narrowest
     as the primary stem weight

  7. Normalize: stem_weight_normalized = stem_width / units_per_em
     This gives a value independent of font size.
     Typical range: 0.05 (ultralight) to 0.20 (black/heavy)

  FALLBACK: If outline decomposition fails or produces no intersections
  (e.g., bitmap-only font), use TT_OS2->usWeightClass as a rough proxy.
```

### 3.3 Serif Detection Algorithm

```
ALGORITHM: DetectSerif(glyph_outline_of_I_or_l)

  1. Load the glyph outline

  2. Find the baseline y-coordinate (typically y = 0 in font units)

  3. Measure the horizontal extent of the outline at y = baseline + delta
     (where delta is a small value like 2% of units_per_EM, to be slightly
     above the baseline)
     -> baseline_width

  4. Measure the horizontal extent at y = x_height * 0.5
     (midpoint of the stem, well above any baseline serif)
     -> stem_width

  5. serif_ratio = baseline_width / stem_width

  6. Classification:
     serif_ratio < 1.1  ->  SANS_SERIF
     serif_ratio < 1.5  ->  TRANSITIONAL_SERIF (bracketed)
     serif_ratio < 2.0  ->  MODERN_SERIF (thin serifs, high contrast)
     serif_ratio >= 2.0 ->  SLAB_SERIF

  7. Repeat at cap line (top of glyph) for top serifs

  FALLBACK: If no suitable glyph is available, use PANOSE byte 1
  (serif style classification) from the OS/2 table.
```

### 3.4 The FontMetricProfile Structure

All measured metrics are collected into a single structure:

```cpp
struct FontMetricProfile {
    // --- Identity ---
    std::string postscript_name;
    std::string family_name;
    std::string style_name;
    uint64_t    font_program_hash;    // SHA-256 of the embedded font stream

    // --- Table Metrics (may be zero/absent) ---
    int32_t  units_per_em        = 0;
    int32_t  os2_weight_class    = 0;  // 100-900
    int32_t  os2_width_class     = 0;  // 1-9
    int32_t  os2_cap_height      = 0;  // from OS/2 v2+
    int32_t  os2_x_height        = 0;  // from OS/2 v2+
    int32_t  os2_avg_char_width  = 0;
    std::array<uint8_t, 10> panose = {};
    float    italic_angle        = 0.0f;

    // --- Measured Metrics (the real signal) ---
    float measured_x_height        = 0.0f;  // in font units / UPM
    float measured_cap_height      = 0.0f;
    float measured_ascender        = 0.0f;
    float measured_descender       = 0.0f;  // negative value
    float measured_avg_char_width  = 0.0f;
    float vertical_stem_weight     = 0.0f;  // normalized to UPM
    float horizontal_stem_weight   = 0.0f;
    float contrast_ratio           = 0.0f;  // vert/horiz stem
    float x_to_cap_ratio          = 0.0f;  // x-height / cap-height
    float width_to_height_ratio   = 0.0f;  // avg width / cap height

    // --- Structural Classification ---
    enum class SerifStyle : uint8_t {
        Unknown, SansSerif, TransitionalSerif, ModernSerif, SlabSerif
    };
    SerifStyle serif_style = SerifStyle::Unknown;
    float serif_bracket_depth = 0.0f;
    float roundness           = 0.0f;  // 0=geometric, 1=very round
    float counter_openness    = 0.0f;  // 0=closed, 1=very open

    // --- Coverage ---
    std::unordered_set<char32_t> available_codepoints;  // Unicode codepoints present
    uint32_t glyph_count = 0;

    // --- Quality flags ---
    bool has_reliable_table_metrics  = false;
    bool has_measured_metrics        = false;
    uint32_t measurement_glyph_count = 0;  // how many glyphs contributed to measurements
};
```

### 3.5 Metric Extraction Pipeline

```
Input: Embedded font stream bytes from PDF (via QPDF)
                    |
                    v
    +-------------------------------+
    | 1. Hash the font stream       |
    |    (SHA-256 for cache key)    |
    +---------------+---------------+
                    |
                    v
    +-------------------------------+
    | 2. Check FontAnalysisCache    |
    |    Hit? -> Return cached      |
    |    profile immediately        |
    +---------------+---------------+
                    |  (cache miss)
                    v
    +-------------------------------+
    | 3. Load font into FreeType    |
    |    FT_New_Memory_Face()       |
    +---------------+---------------+
                    |
                    v
    +-------------------------------+
    | 4. Extract table metrics      |
    |    FT_Get_Sfnt_Table(OS2)     |
    |    FT_Get_Sfnt_Table(POST)    |
    |    Read PANOSE, weight, etc.  |
    +---------------+---------------+
                    |
                    v
    +-------------------------------+
    | 5. Enumerate available glyphs |
    |    FT_Get_Char_Index() for    |
    |    all Unicode codepoints     |
    |    Build codepoint set        |
    +---------------+---------------+
                    |
                    v
    +-------------------------------+
    | 6. Measure glyph outlines     |
    |    For each diagnostic glyph  |
    |    (x, H, I, o, e, etc.)     |
    |    that exists in the subset: |
    |    - Load outline             |
    |    - Measure bounding box     |
    |    - Measure stem weights     |
    |    - Detect serifs            |
    |    - Compute roundness        |
    +---------------+---------------+
                    |
                    v
    +-------------------------------+
    | 7. Compute derived metrics    |
    |    - contrast ratio           |
    |    - x-to-cap ratio           |
    |    - width-to-height ratio    |
    |    - serif classification     |
    +---------------+---------------+
                    |
                    v
    +-------------------------------+
    | 8. Store in cache, return     |
    +-------------------------------+

Output: FontMetricProfile
```

---

## 4. Substitute Font Selection Algorithm

### 4.1 Scoring Function

The substitute font selector computes a **weighted distance** between the embedded font's `FontMetricProfile` and every candidate font's profile. Lower distance means better match.

The scoring function uses a tiered approach: some metrics are hard disqualifiers (serif mismatch), others are weighted continuous distances.

```
ALGORITHM: ComputeSubstitutionScore(source: FontMetricProfile,
                                     candidate: FontMetricProfile)
  -> float (0.0 = perfect match, higher = worse)

  // ---- Stage 1: Hard Disqualifiers ----

  IF source.serif_style != Unknown AND candidate.serif_style != Unknown
     AND source.serif_style != candidate.serif_style:
       // Sans-serif vs serif is never acceptable
       IF (source is SansSerif AND candidate is not SansSerif) OR
          (source is not SansSerif AND candidate is SansSerif):
            RETURN infinity  // Disqualified

  IF source.italic_angle == 0 AND abs(candidate.italic_angle) > 5:
       RETURN infinity  // Do not substitute italic for upright

  IF source.italic_angle != 0 AND abs(candidate.italic_angle) < 2:
       RETURN infinity  // Do not substitute upright for italic

  // ---- Stage 2: Weighted Metric Distance ----

  score = 0.0

  // Weight assignments -- these reflect visual impact priority
  //
  // Stem weight is the highest weight because it is the most immediately
  // visible difference between fonts. A thin font with a heavy substitute
  // is jarring. x-to-cap ratio is second because it controls the overall
  // "color" and proportion of the typeface.

  weights = {
      vertical_stem_weight:   25.0,
      x_to_cap_ratio:         20.0,
      measured_x_height:       15.0,
      measured_cap_height:     10.0,
      width_to_height_ratio:  10.0,
      contrast_ratio:           8.0,
      measured_avg_char_width:  5.0,
      roundness:                4.0,
      counter_openness:         3.0
  }

  FOR EACH (metric_name, weight) IN weights:
      source_val = source.get(metric_name)
      candidate_val = candidate.get(metric_name)

      IF source_val == 0 OR candidate_val == 0:
          CONTINUE  // Cannot compare if metric is unavailable

      // Normalized absolute difference
      diff = abs(source_val - candidate_val) / max(abs(source_val), 0.001)
      score += diff * weight

  // ---- Stage 3: Bonus/Penalty Adjustments ----

  // Serif sub-type bonus (e.g., both transitional serif)
  IF source.serif_style == candidate.serif_style AND
     source.serif_style != Unknown:
      score *= 0.9  // 10% bonus

  // PANOSE agreement bonus (if both have PANOSE data)
  IF source.panose != all_zeros AND candidate.panose != all_zeros:
      panose_distance = compute_panose_distance(source.panose, candidate.panose)
      IF panose_distance < 10:
          score *= 0.95  // 5% bonus for close PANOSE

  // Weight class proximity bonus (OS/2 usWeightClass)
  IF source.os2_weight_class > 0 AND candidate.os2_weight_class > 0:
      weight_diff = abs(source.os2_weight_class - candidate.os2_weight_class)
      IF weight_diff <= 50:
          score *= 0.95  // Very close weight class
      ELIF weight_diff >= 200:
          score *= 1.2   // 20% penalty for large weight difference

  // Unicode coverage bonus: prefer candidates that cover all needed glyphs
  needed_codepoints = (requested codepoints not in source.available_codepoints)
  covered = count of needed_codepoints present in candidate
  coverage_ratio = covered / len(needed_codepoints)
  IF coverage_ratio < 1.0:
      score *= (2.0 - coverage_ratio)  // Penalty: up to 2x for missing glyphs

  RETURN score
```

### 4.2 PANOSE Distance Function

When PANOSE data is available, it provides supplementary signal. The distance function uses per-digit weights based on the W3C PANOSE 2.0 specification:

```
PANOSE digit weights (for Latin Text family):
  [0] Family Kind:        0  (must match; different families are incomparable)
  [1] Serif Style:       10  (highest visual impact)
  [2] Weight:             8
  [3] Proportion:         7
  [4] Contrast:           6
  [5] Stroke Variation:   5
  [6] Arm Style:          4
  [7] Letterform:         4
  [8] Midline:            3
  [9] X-height:           6

FUNCTION compute_panose_distance(a[10], b[10]) -> float:
    IF a[0] != b[0]: RETURN 100.0  // Different family kinds

    total = 0.0
    FOR i IN 1..9:
        IF a[i] == 0 OR b[i] == 0:
            CONTINUE  // 0 = "Any" in PANOSE; skip
        diff = abs(a[i] - b[i])
        total += diff * panose_weights[i]
    RETURN total
```

### 4.3 Candidate Generation and Ranking Pipeline

```
Input: FontMetricProfile of embedded font,
       Set<char32_t> of needed codepoints

    +--------------------------------------+
    | 1. Generate Candidate Pool           |
    |    - All system fonts (via           |
    |      Fontconfig / DirectWrite)       |
    |    - All bundled fonts               |
    |    (Noto, Liberation, DejaVu)        |
    |    - Google Fonts PANOSE index       |
    |      (metadata only, not loaded)     |
    +------------------+-------------------+
                       |
                       v
    +--------------------------------------+
    | 2. Quick Filter (cheap checks)       |
    |    Eliminate candidates that:         |
    |    - Are the wrong serif class       |
    |      (if detectable from metadata)   |
    |    - Are italic when source is not   |
    |    - Have weight class > 200 apart   |
    |    - Do not cover any of the needed  |
    |      codepoints                      |
    |                                      |
    |    This reduces pool from ~500-2000  |
    |    fonts to ~20-100 candidates       |
    +------------------+-------------------+
                       |
                       v
    +--------------------------------------+
    | 3. Analyze Candidates                |
    |    For each surviving candidate:     |
    |    - Check FontAnalysisCache         |
    |    - If miss: load into FreeType,    |
    |      compute FontMetricProfile       |
    |    - Store in cache                  |
    |                                      |
    |    Parallelizable: use thread pool   |
    |    (4-8 threads)                     |
    +------------------+-------------------+
                       |
                       v
    +--------------------------------------+
    | 4. Score Candidates                  |
    |    ComputeSubstitutionScore() for    |
    |    each candidate vs. source         |
    +------------------+-------------------+
                       |
                       v
    +--------------------------------------+
    | 5. Sort by Score (ascending)         |
    |    Return top 10 candidates          |
    +--------------------------------------+

Output: Ranked list of (FontMetricProfile, score, font_file_path)
```

### 4.4 Known Substitution Table (Fast Path)

Before running the full analysis pipeline, check a hard-coded table of known metric-compatible pairs. This handles the most common cases in zero time:

| Embedded Font | Substitute | Notes |
|---------------|-----------|-------|
| Helvetica / Arial | Liberation Sans | Metric-compatible by design |
| Times / Times New Roman | Liberation Serif | Metric-compatible by design |
| Courier / Courier New | Liberation Mono | Metric-compatible by design |
| Helvetica Neue | Noto Sans | Close match (not metric-exact) |
| Calibri | Carlito (if available), else Noto Sans | Carlito is designed as Calibri substitute |
| Cambria | Caladea (if available), else Noto Serif | Caladea is designed as Cambria substitute |
| Myriad Pro | Noto Sans | Close match |
| Garamond | Noto Serif | Moderate match |

If the embedded font matches a known name in this table and the substitute is available, skip the analysis pipeline entirely and use the known substitute (still subject to user override via the dialog).

---

## 5. Glyph Scaling and Adjustment

### 5.1 The Problem

Even the best substitute font will have slightly different proportions. If we simply use the substitute's glyphs as-is, the new text will have noticeably different line height, character spacing, and visual weight compared to the original. The adjustment pipeline corrects for this.

### 5.2 Transformations Applied

For each substitute glyph, we compute an affine transformation matrix that maps the substitute's metrics to the source font's metrics. The transformations are applied in this order:

#### Transform 1: x-height Alignment (Vertical Scale)

The most critical adjustment. The substitute's x-height must match the source's.

```
scale_y = source.measured_x_height / substitute.measured_x_height

Applied only if both values are available and differ by more than 2%.
This scales the entire glyph vertically.
```

#### Transform 2: Width Adjustment (Horizontal Scale)

Match the average character width so that text runs have similar overall length.

```
scale_x = source.measured_avg_char_width / substitute.measured_avg_char_width

Applied only if both values are available and differ by more than 3%.
This prevents text from being significantly wider or narrower.
```

#### Transform 3: Baseline Offset (Vertical Translation)

After vertical scaling, the baseline position may shift. Correct for this:

```
// After scaling, where does the substitute's baseline land?
// The baseline is at y=0 in font coordinates, so scaling from the
// glyph's center would move it. We need to ensure baseline stays aligned.

baseline_offset_y = 0  (if scaling is applied from the baseline, no correction needed)

// However, if cap-height alignment is needed instead of x-height:
// This is a design choice. We align to x-height for body text
// and to cap-height for ALL-CAPS text.
```

#### Transform 4: Weight Compensation (Stroke Width Adjustment)

If the substitute's stem weight differs significantly from the source's, we apply a stroke width modification. This is the most complex transform and is optional (applied only when the difference exceeds a threshold).

```
weight_ratio = source.vertical_stem_weight / substitute.vertical_stem_weight

IF abs(1.0 - weight_ratio) > 0.15:  // More than 15% difference
    // Apply a FreeType auto-hinter style embolden/thin
    // FT_Outline_Embolden() or FT_Outline_EmboldenXY()
    // with a carefully computed strength value

    strength = (source.vertical_stem_weight - substitute.vertical_stem_weight)
               * units_per_em_substitute

    FT_Outline_EmboldenXY(outline, strength_x, strength_y)

    // WARNING: Emboldening distorts glyph shapes.
    // For differences > 30%, it is better to choose a different
    // weight variant of the substitute font family rather than
    // applying synthetic emboldening.
```

### 5.3 Combined Affine Transform

All transforms are composed into a single 2x3 affine matrix applied when the glyph is embedded into the PDF:

```
The PDF text matrix (Tm operator) provides the mechanism:

[scale_x   0       0  ]
[0         scale_y  0  ]
[offset_x  offset_y 1  ]

This is applied per-glyph or per-text-run in the PDF content stream
via the Tm (set text matrix) operator.

In practice, we embed the substitute font with a modified Widths array
and FontMatrix that incorporates the scaling, so no per-glyph Tm changes
are needed for width adjustment. The vertical scaling is handled by
adjusting the font size specified in the Tf operator.
```

### 5.4 Per-Glyph Width Override

Rather than scaling every glyph uniformly, we can set individual glyph widths in the substitute font's PDF descriptor to exactly match the source font's widths for characters that appear in both:

```
For character 'a':
    source_width = width_of_a_in_source_font  // from embedded font's Widths array
    sub_width    = width_of_a_in_substitute   // from substitute's metrics

    // The substitute font's Widths array in the PDF is set to:
    // (source_width / sub_width) * sub_width = source_width
    //
    // This means the PDF renderer uses the source's width for positioning
    // even though the glyph shape comes from the substitute.

    // For characters only in the substitute (the whole point),
    // use the substitute's native width adjusted by the global scale_x.
```

This approach means characters that exist in both fonts will have identical positioning, and characters unique to the substitute will have proportionally correct positioning.

### 5.5 Adjustment Quality Tiers

| Tier | Adjustments Applied | When Used |
|------|-------------------|-----------|
| **Tier 1: Metric-Exact** | No adjustment needed (e.g., Liberation Sans for Arial) | Known metric-compatible pairs |
| **Tier 2: Scaled** | x-height + width scaling via font size and Widths array | Score < 5.0 (good match) |
| **Tier 3: Transformed** | Scaling + weight compensation via emboldening | Score 5.0-15.0 (moderate match) |
| **Tier 4: Best-Effort** | All transforms applied; user warned of visual mismatch | Score > 15.0 (poor match) |

---

## 6. Font Switching in PDF Content Streams

### 6.1 The PDF Model for Fonts in Text

In PDF, text is rendered inside BT/ET (Begin Text / End Text) blocks. Within a block, the current font is set by the `Tf` operator, and text is rendered by `Tj`, `TJ`, `'`, or `"` operators.

Key constraint: **a single `Tj` or `TJ` operator uses exactly one font.** To switch fonts within what appears to be a continuous text run, you must:

1. End the current text string
2. Issue a new `Tf` operator to switch fonts
3. Continue with a new text string

### 6.2 Font Switching Strategy

When the user types text that requires characters from both the embedded font (for characters it has) and the substitute font (for characters it doesn't have), the system generates a content stream that alternates between fonts:

```
Example: User types "Hello World!" where the embedded font has all Latin uppercase
         and "o", "l", "d", "!" but NOT "e", "r" (hypothetical scenario).

PDF Content Stream:

BT
  /F1 12 Tf          % Select embedded font (CorporateBrandFont-Bold)
  100 700 Td          % Position
  (H) Tj              % 'H' from embedded font
  /F2 11.76 Tf        % Switch to substitute font (scaled: 12 * scale_y)
  (e) Tj              % 'e' from substitute
  /F1 12 Tf           % Switch back to embedded
  (llo Wo) Tj         % 'l','l','o',' ','W','o' all in embedded
  /F2 11.76 Tf        % Switch to substitute
  (r) Tj              % 'r' from substitute
  /F1 12 Tf           % Switch back to embedded
  (ld!) Tj            % 'l','d','!' all in embedded
ET
```

### 6.3 Optimizing Font Switches

Naively switching per-character would produce bloated content streams. The system batches consecutive characters that use the same font:

```
ALGORITHM: GenerateFontSwitchedContentStream(
    text: string,
    source_font: EmbeddedFont,
    substitute_font: SubstituteFont,
    position: (x, y),
    font_size: float,
    adjusted_font_size: float   // substitute's size after x-height scaling
)

  runs = []
  current_run = { font: null, chars: "" }

  FOR EACH char IN text:
      IF char is in source_font.available_codepoints:
          target_font = SOURCE
      ELSE IF char is in substitute_font.available_codepoints:
          target_font = SUBSTITUTE
      ELSE:
          target_font = FALLBACK  // .notdef or tofu -- should not happen
                                  // if substitute was chosen correctly

      IF target_font == current_run.font:
          current_run.chars += char
      ELSE:
          IF current_run.chars is not empty:
              runs.append(current_run)
          current_run = { font: target_font, chars: str(char) }

  IF current_run.chars is not empty:
      runs.append(current_run)

  // Generate content stream operators
  stream = "BT\n"
  stream += f"{position.x} {position.y} Td\n"

  FOR EACH run IN runs:
      IF run.font == SOURCE:
          stream += f"/F1 {font_size} Tf\n"
      ELSE:
          stream += f"/F2 {adjusted_font_size} Tf\n"

      // Encode the string with proper PDF string escaping
      stream += f"({escape_pdf_string(run.chars)}) Tj\n"

  stream += "ET\n"
  RETURN stream
```

### 6.4 Font Resource Registration

Both fonts must be registered in the page's Resources dictionary:

```
% Page Resources dictionary (via QPDF object manipulation)
<<
  /Font <<
    /F1 <reference to original embedded font object>
    /F2 <reference to substitute font object>
  >>
>>
```

The substitute font object is a new PDF font dictionary created by PdfFontEmbedder. It includes:
- The subset of the substitute font program (only needed glyphs)
- A CIDFont dictionary (for TrueType fonts) or Type1 dictionary
- A ToUnicode CMap for text extraction
- Width entries adjusted per Section 5.4
- Font descriptor with correct metrics

### 6.5 TJ Operator with Kerning Adjustments

When HarfBuzz shaping produces kerning adjustments, use the `TJ` array operator instead of `Tj`:

```
% TJ with kerning: "AV" where A-V has a -80 kern
/F1 12 Tf
[(A) -80 (V)] TJ

% Mixed fonts with kerning:
/F1 12 Tf
[(H) 0] TJ        % 'H' from embedded font, no kern
/F2 11.76 Tf
[(e) 0] TJ        % 'e' from substitute
/F1 12 Tf
[(llo) 0] TJ      % back to embedded
```

The kerning values in TJ arrays are in thousandths of a unit of text space. When switching fonts, kerning between the last glyph of one font and the first glyph of the next must be computed using the metrics of both fonts (typically the advance width from the outgoing font, plus manual adjustment based on the incoming glyph's left side bearing).

---

## 7. Font Subset Extension

### 7.1 When This Applies

If the FullFontFinder locates the complete original font on the system, we can extend the embedded subset rather than using a substitute font. This is the preferred path because it produces perfect visual consistency -- the new glyphs come from the same font as the originals.

### 7.2 The Extension Process

```
Input: - Embedded font subset (from PDF, via QPDF)
       - Complete original font file (from system)
       - Set<char32_t> of needed codepoints

    +----------------------------------------------+
    | 1. Verify Font Identity                      |
    |    Compare the outlines of glyphs present in |
    |    both the subset and the full font.         |
    |    At least 5 glyphs must match exactly.     |
    |    (Protects against name collisions --      |
    |    "Helvetica" from different foundries)     |
    +----------------------+-----------------------+
                           |
                           v
    +----------------------------------------------+
    | 2. Extract Needed Glyphs from Full Font      |
    |    Use HarfBuzz subsetting (hb_subset):      |
    |    - Input: full font + set of needed        |
    |      codepoints                              |
    |    - Output: minimal font program containing |
    |      only those glyphs (plus dependencies    |
    |      like composite glyph components)        |
    +----------------------+-----------------------+
                           |
                           v
    +----------------------------------------------+
    | 3. Merge Glyph Data                          |
    |    Build a new font program that contains:   |
    |    - All glyphs from the original subset     |
    |    - All new glyphs from the extracted set   |
    |                                              |
    |    For TrueType (glyf/loca tables):          |
    |    - Concatenate glyph data in glyf table    |
    |    - Rebuild loca table with new offsets      |
    |    - Update maxp (glyph count)               |
    |    - Extend cmap with new codepoint mappings |
    |    - Extend hmtx with new advance widths     |
    |    - Recalculate head checksum                |
    |                                              |
    |    For CFF (CFF table):                      |
    |    - Add new CharStrings to the CFF program  |
    |    - Extend the charset and encoding         |
    |    - Update Top DICT offsets                  |
    +----------------------+-----------------------+
                           |
                           v
    +----------------------------------------------+
    | 4. Update PDF Font Object                    |
    |    Via QPDF:                                 |
    |    - Replace the font stream with the        |
    |      merged font program                     |
    |    - Update /Widths array with new entries   |
    |    - Update /FirstChar and /LastChar         |
    |    - Update CIDFont's /W array (for CID)    |
    |    - Update /ToUnicode CMap                  |
    |    - Update /FontDescriptor lengths          |
    |    - Update /CIDSet if present (PDF/A)       |
    +----------------------------------------------+

Output: Modified PDF font object containing the extended subset
```

### 7.3 TrueType Table Merging Detail

The most complex part is merging TrueType tables. Here are the specific tables that must be updated:

| Table | Action |
|-------|--------|
| `glyf` | Append new glyph outlines. For composite glyphs, update component glyph indices. |
| `loca` | Rebuild entirely with offsets into the new `glyf` table. Format (short/long) preserved. |
| `hmtx` | Append horizontal metrics (advance width + left side bearing) for new glyphs. |
| `cmap` | Add new (codepoint -> glyph index) mappings. Preserve existing mappings. Platform/encoding subtables may need rewriting. |
| `maxp` | Update `numGlyphs`. |
| `head` | Recalculate `checkSumAdjustment`. Update `indexToLocFormat` if needed. |
| `name` | Optionally update the font name to indicate modification (e.g., append "+Extended"). Spec requires subset fonts use a tag prefix like `ABCDEF+FontName`. |
| `OS/2` | Update `usFirstCharIndex` and `usLastCharIndex` if the codepoint range expanded. |
| `post` | If format 2.0, add glyph name entries for new glyphs. |

### 7.4 CFF Table Merging Detail

For CFF (Compact Font Format) fonts:

| Element | Action |
|---------|--------|
| CharStrings INDEX | Append new charstring programs for new glyphs |
| Charset | Extend with SID (String ID) entries for new glyph names |
| Encoding | Extend with new code-to-glyph mappings (or use CIDFont approach with no encoding) |
| Top DICT | Update `charset` offset, `CharStrings` offset, sizes |
| String INDEX | Add new glyph name strings if needed |

### 7.5 Validation After Extension

After merging, validate the font program:

1. Load the merged font into FreeType and verify it parses without error
2. For each new glyph: load it, verify the outline is non-empty and has reasonable bounds
3. For each original glyph: load it, verify the outline matches the original (no corruption)
4. Verify the cmap returns correct glyph indices for all codepoints
5. Verify advance widths match expected values

### 7.6 Fallback: Side-by-Side Embedding

If font merging fails (e.g., due to font format complexity or validation errors), fall back to the dual-font approach described in Section 6. This is safe because it does not modify the original font program:

```
Strategy priority:
  1. Extend the embedded subset (best visual result)
  2. If extension fails: embed the full font as a second font, use font switching
  3. If full font not found: use substitute font with scaling (Section 5+6)
```

---

## 8. User Interface for Font Choice

### 8.1 Dialog Structure

The Font Substitution Dialog appears when the user:
- Begins editing text near an embedded font that is a subset
- Pastes text containing characters not in the embedded font's subset
- The system detects needed glyphs are missing during text commit

```
+================================================================+
|  Font Substitution Required                              [X]   |
+================================================================+
|                                                                |
|  The font "CorporateBrandFont-Bold" is embedded as a partial  |
|  subset (47 of ~220 glyphs). The characters you need are not  |
|  available in this subset.                                     |
|                                                                |
|  Missing characters: e, r, s, t (4 characters)                |
|                                                                |
+----------------------------------------------------------------+
|  EMBEDDED FONT ANALYSIS                                        |
|  +----------------------------------------------------------+  |
|  |  CorporateBrandFont-Bold                                 |  |
|  |  Style: Bold | Serif: Sans-serif | Weight: 700           |  |
|  |  x-height: 0.523 | Cap-height: 0.714 | Stem: 0.142      |  |
|  |                                                           |  |
|  |  Available glyphs:                                        |  |
|  |  A B C D F G H I J K L M N O P Q U V W X Y Z            |  |
|  |  a b c d f g h i j k l m n o p q u v w x y z             |  |
|  |  0 1 2 3 4 5 6 7 8 9 . , ! ? space                       |  |
|  |                                                           |  |
|  |  Sample: "ABCDEFGHIJKLM abcdefghijklm 0123456789"        |  |
|  |  [rendered with embedded font]                            |  |
|  +----------------------------------------------------------+  |
|                                                                |
+----------------------------------------------------------------+
|  SUBSTITUTE FONT OPTIONS                          [Sort: Best] |
|                                                                |
|  (*) 1. Liberation Sans Bold           Score: 2.3  [BEST]     |
|  |     +------------------------------------------------------+|
|  |     | Sample: "ABCDerst abcderst 0123456789"               ||
|  |     | [rendered with substitute, scaled to match]           ||
|  |     +------------------------------------------------------+|
|  |     | Comparison: "Hello World" rendered with:              ||
|  |     | Embedded:   [H][e][l][l][o][ ][W][o][r][l][d]        ||
|  |     | Substitute: [H][e][l][l][o][ ][W][o][r][l][d]        ||
|  |     | (characters from substitute shown with subtle bg)     ||
|  |     +------------------------------------------------------+|
|  |                                                             |
|  ( ) 2. Noto Sans Bold                  Score: 4.1             |
|  |     Sample: "ABCDerst abcderst 0123456789"                  |
|  |                                                             |
|  ( ) 3. DejaVu Sans Bold                Score: 6.7             |
|  |     Sample: "ABCDerst abcderst 0123456789"                  |
|  |                                                             |
|  [ ] Show all candidates (47 fonts)                            |
|                                                                |
+----------------------------------------------------------------+
|  [  ] Flag edited text as "substitute font used"               |
|                                                                |
|  [ Load Font File... ]    [ Cancel ]    [ Apply Substitute ]   |
+================================================================+
```

### 8.2 Visual Comparison Rendering

The comparison panel renders the same text string twice:

1. **Using the embedded font** for characters it has, and leaving gaps (or showing `.notdef` boxes) for missing characters
2. **Using the substitute font** for all characters, with the scale/transform applied

The rendering is done via FreeType directly into a `QImage`, not through the PDF renderer. This provides instant preview without modifying the PDF.

For the "mixed" comparison view, characters sourced from the substitute are rendered with a subtle background tint (e.g., pale yellow) so the user can visually assess the blending quality.

### 8.3 Dialog Interaction Flow

```
User action: Type character not in embedded font subset
    |
    v
System: Check if a substitute has already been chosen for this font
    |
    +-- YES: Use the previously chosen substitute silently
    |
    +-- NO: Show notification bar at top of editor:
            "Some characters require a substitute font. [Choose Font] [Use Default]"
            |
            +-- [Use Default]: Use top-ranked substitute automatically
            |
            +-- [Choose Font]: Open Font Substitution Dialog
                    |
                    +-- User selects substitute -> stored in document metadata
                    |
                    +-- User loads font file -> analyze, use if compatible
                    |
                    +-- User cancels -> characters shown as .notdef in preview,
                                        actual editing continues with visual indicator
```

### 8.4 "Load Font File" Flow

When the user has the original font file (e.g., received from the document creator):

1. User clicks "Load Font File..." and selects a .ttf/.otf/.woff file
2. System loads the font into FreeType
3. System verifies it matches the embedded font (compares glyph outlines for shared characters)
4. If match confirmed: extend the embedded subset (Section 7)
5. If match fails: inform user, offer to use it as a substitute instead

### 8.5 Selenium-Friendly Widget Structure

For testability, all dialog elements use explicit `objectName` properties:

```cpp
// Widget naming for Selenium/QTest automation
dialog->setObjectName("fontSubstitutionDialog");
embeddedFontInfo->setObjectName("embeddedFontInfoPanel");
substituteCandidateList->setObjectName("substituteCandidateList");
sortComboBox->setObjectName("substituteSortCombo");
applyButton->setObjectName("applySubstituteButton");
cancelButton->setObjectName("cancelSubstituteButton");
loadFontButton->setObjectName("loadFontFileButton");
flagSubstituteCheckbox->setObjectName("flagSubstituteCheckbox");
comparisonPanel->setObjectName("fontComparisonPanel");
showAllCandidatesCheckbox->setObjectName("showAllCandidatesCheckbox");
```

---

## 9. Cache Strategy

### 9.1 What is Cached

| Cache Entry | Key | Value | Typical Size |
|-------------|-----|-------|-------------|
| Embedded font analysis | SHA-256 of font stream bytes | `FontMetricProfile` | ~500 bytes |
| System/bundled font analysis | File path + modification timestamp | `FontMetricProfile` | ~500 bytes |
| Similarity score | (source_hash, candidate_hash) pair | `float score` | 12 bytes |
| Substitution decision | (source_hash, needed_codepoints_hash) | `(substitute_path, transform_params)` | ~200 bytes |

### 9.2 Cache Architecture

```
+----------------------------------------------------------+
|                  FontAnalysisCache                        |
+----------------------------------------------------------+
|  In-Memory Layer (LRU, 256 entries)                      |
|  - Most recently used FontMetricProfiles                 |
|  - All similarity scores for current document's fonts    |
|  - Hit rate target: >90% during active editing           |
+----------------------------------------------------------+
|  Persistent Layer (SQLite database)                      |
|  - All computed FontMetricProfiles                        |
|  - Location: user data directory                          |
|    Windows: %APPDATA%/PDFE/font_cache.db                 |
|    macOS:   ~/Library/Application Support/PDFE/           |
|    Linux:   ~/.local/share/PDFE/                         |
|  - Survives application restarts                         |
|  - Invalidated when font file changes (mtime check)     |
+----------------------------------------------------------+
```

### 9.3 Cache Schema (SQLite)

```sql
CREATE TABLE font_profiles (
    id              INTEGER PRIMARY KEY,
    cache_key       TEXT UNIQUE NOT NULL,     -- SHA-256 hash or filepath+mtime
    postscript_name TEXT,
    family_name     TEXT,
    style_name      TEXT,
    profile_json    TEXT NOT NULL,            -- Serialized FontMetricProfile
    created_at      INTEGER NOT NULL,         -- Unix timestamp
    last_accessed   INTEGER NOT NULL
);

CREATE INDEX idx_font_profiles_key ON font_profiles(cache_key);
CREATE INDEX idx_font_profiles_name ON font_profiles(postscript_name);
CREATE INDEX idx_font_profiles_family ON font_profiles(family_name);

CREATE TABLE similarity_scores (
    source_key      TEXT NOT NULL,
    candidate_key   TEXT NOT NULL,
    score           REAL NOT NULL,
    computed_at     INTEGER NOT NULL,
    PRIMARY KEY (source_key, candidate_key)
);

CREATE TABLE substitution_decisions (
    document_id         TEXT NOT NULL,        -- Hash of the PDF file
    source_font_key     TEXT NOT NULL,
    substitute_path     TEXT NOT NULL,
    transform_params    TEXT NOT NULL,        -- JSON: {scale_x, scale_y, ...}
    user_chosen         INTEGER NOT NULL,     -- 1 if user explicitly chose, 0 if auto
    created_at          INTEGER NOT NULL,
    PRIMARY KEY (document_id, source_font_key)
);
```

### 9.4 Cache Lifecycle

```
Application startup:
    1. Open/create font_cache.db
    2. Scan system fonts for new/changed files (background thread, ~1-3 seconds)
    3. Invalidate entries where font file mtime has changed
    4. Do NOT pre-analyze all system fonts (lazy analysis only)

Document open:
    1. For each embedded font in the PDF:
       - Compute font stream hash
       - Check cache for FontMetricProfile
       - If miss: analyze synchronously (needed for rendering)
    2. Pre-compute top 5 substitutes for each embedded subset font
       (background thread, results cached for when user needs them)

During editing:
    1. Cache hits for most operations (profiles already loaded)
    2. New candidates analyzed on-demand if user opens font dialog

Application shutdown:
    1. Flush in-memory cache to SQLite
    2. Prune entries older than 90 days and never accessed more than once

Cache size estimate:
    - 500 system fonts * 500 bytes = 250 KB of profiles
    - 500 * 500 / 2 similarity scores * 12 bytes = ~1.5 MB (if all computed)
    - In practice: <500 KB total, negligible
```

---

## 10. Integration with Text Editing

### 10.1 Editing Lifecycle

The font substitution system integrates with the text editor at three distinct phases:

```
Phase 1: EDITING (Live Typing)
    - User types into a text editing widget (QTextEdit overlay)
    - The editor uses a PREVIEW font for rendering in the overlay
    - The preview font is the best available substitute, loaded into Qt's
      font system, scaled to approximate the embedded font's size
    - No PDF modification happens during typing
    - Missing glyphs are visually indicated with a subtle underline or
      background tint in the editing widget
    - Performance target: zero perceptible latency on keystroke

Phase 2: COMMIT (User confirms edit)
    - User clicks outside the text area, presses Escape, or presses Enter
    - The system performs the full substitution pipeline:
      1. Determine which characters need the embedded font vs substitute
      2. Generate the font-switched content stream (Section 6)
      3. Embed the substitute font subset into the PDF (Section 7)
      4. Write the content stream into the PDF page
    - This is wrapped in an undo-able Command object
    - Performance target: <500ms for typical text edits

Phase 3: RENDER (Display Result)
    - After commit, MuPDF re-renders the affected page tile(s)
    - The tile cache is invalidated for the affected region
    - The user sees the final result rendered by MuPDF using the
      actual embedded fonts in the PDF
    - Any discrepancy between the preview and the final render
      indicates a bug in the scaling/adjustment pipeline
```

### 10.2 Real-Time Glyph Availability Check

During typing, the system must determine instantly whether each character is available in the embedded font:

```cpp
// This is called on every keystroke during text editing
// Must complete in <1ms

class FontSubstitutionCoordinator {
public:
    // Pre-computed during document open
    struct EditingContext {
        std::unordered_set<char32_t> embedded_codepoints;  // O(1) lookup
        QFont preview_font;                                 // Qt font for the overlay
        float preview_scale_factor;                         // Scale to match embedded
        bool substitute_available;                          // false = .notdef
        std::string substitute_font_path;
    };

    // Called per-keystroke
    GlyphSource classifyCharacter(char32_t codepoint,
                                  const EditingContext& ctx) const {
        if (ctx.embedded_codepoints.contains(codepoint)) {
            return GlyphSource::EmbeddedFont;
        }
        if (ctx.substitute_available) {
            return GlyphSource::SubstituteFont;
        }
        return GlyphSource::Unavailable;
    }
};
```

### 10.3 Preview Font Configuration

The text editing overlay (QTextEdit or similar) needs a Qt font that approximates the embedded font:

```cpp
// Configuring the preview font for the editing overlay
QFont configurePreviewFont(const FontMetricProfile& embedded_profile,
                           const SubstituteMatch& best_match) {
    QFont preview_font;

    // Load the substitute font into Qt's font database
    int font_id = QFontDatabase::addApplicationFont(
        QString::fromStdString(best_match.font_file_path));

    QStringList families = QFontDatabase::applicationFontFamilies(font_id);
    if (!families.isEmpty()) {
        preview_font.setFamily(families.first());
    }

    // Set the point size adjusted for x-height matching
    float base_size = embedded_profile.requested_point_size;
    float scale = embedded_profile.measured_x_height /
                  best_match.profile.measured_x_height;
    preview_font.setPointSizeF(base_size * scale);

    // Match weight
    if (embedded_profile.os2_weight_class > 0) {
        preview_font.setWeight(
            mapOs2WeightToQtWeight(embedded_profile.os2_weight_class));
    }

    // Match italic
    if (std::abs(embedded_profile.italic_angle) > 1.0f) {
        preview_font.setItalic(true);
    }

    return preview_font;
}
```

### 10.4 Undo/Redo Integration

Font substitution is part of the text editing Command:

```cpp
class TextEditCommand : public UndoCommand {
public:
    void execute() override {
        // 1. Insert/modify text in the logical text model
        // 2. Run font substitution pipeline
        // 3. Generate content stream with font switching
        // 4. Embed substitute font if not already embedded
        // 5. Write to PDF via QPDF
        // 6. Invalidate tile cache
    }

    void undo() override {
        // 1. Restore original content stream
        // 2. Remove substitute font if it was added by this command
        //    (reference counting: only remove if no other text uses it)
        // 3. Restore original page resources
        // 4. Invalidate tile cache
    }

private:
    // Stored for undo:
    std::string original_content_stream;
    QPDFObjectHandle original_resources;
    bool added_substitute_font;
    std::string substitute_font_name;  // e.g., "/F2"
};
```

### 10.5 Sequence Diagram: Keystroke to Rendered Glyph

```
User        TextEditor     FontSubCoord    EmbFontAnalyzer    SubSelector   PDFEmbedder    MuPDF
  |              |              |                |                |              |            |
  |--keypress--->|              |                |                |              |            |
  |              |--classify--->|                |                |              |            |
  |              |  char(c)     |                |                |              |            |
  |              |<--Source:Sub--|                |                |              |            |
  |              |              |                |                |              |            |
  |              |--render in-->|                |                |              |            |
  |              |  overlay     |                |                |              |            |
  |              |  (preview    |                |                |              |            |
  |              |   font)      |                |                |              |            |
  |<--see char---|              |                |                |              |            |
  |              |              |                |                |              |            |
  |  ... more keystrokes ...   |                |                |              |            |
  |              |              |                |                |              |            |
  |--commit----->|              |                |                |              |            |
  |  (click out) |--getText()--->              |                |              |            |
  |              |              |--analyze------>|                |              |            |
  |              |              |  embedded font |                |              |            |
  |              |              |<--profile------|                |              |            |
  |              |              |                |                |              |            |
  |              |              |--select sub----|--------------->|              |            |
  |              |              |  (if needed)   |                |              |            |
  |              |              |<--best match---|----------------|              |            |
  |              |              |                |                |              |            |
  |              |              |--embed & write-|----------------|------------->|            |
  |              |              |  font-switched |                |              |            |
  |              |              |  content stream|                |              |            |
  |              |              |<--done---------|----------------|--------------|            |
  |              |              |                |                |              |            |
  |              |              |--invalidate----|----------------|--------------|--render--->|
  |              |              |  tile cache    |                |              |            |
  |              |              |                |                |              |<--bitmap---|
  |<--see final--|<------------|----------------|----------------|--------------|            |
  |   render     |              |                |                |              |            |
```

---

## 11. Edge Cases

### 11.1 Fonts with No Usable Metrics

**Scenario**: The embedded font has a stripped or corrupt OS/2 table, and contains only glyphs like `0`, `1`, `2` (no letters for x-height or cap-height measurement).

**Handling**:

```
1. Attempt to read table metrics (OS/2, hhea, head)
   - Even subset fonts usually retain the OS/2 table

2. If table metrics are available but glyph outlines are not useful:
   - Use os2_weight_class and os2_width_class for coarse matching
   - Use PANOSE if present
   - Set has_measured_metrics = false in the profile
   - Increase the weight of table-based metrics in the scoring function

3. If no table metrics AND no useful glyphs:
   - Analyze whatever glyphs ARE present:
     - Digit glyphs (0-9) still give us advance widths and cap-height-like
       bounding boxes
     - Punctuation gives us some baseline reference
   - Fall back to PANOSE-only matching if available
   - Fall back to font name heuristic matching:
     Parse the PostScript name for keywords: "Bold", "Light", "Serif",
     "Sans", "Mono", "Condensed", etc.
   - Present user with top candidates and prominently warn about
     low confidence in the match

4. Absolute last resort:
   - Use Noto Sans (Regular) as the universal fallback
   - Warn user: "No metric information available for matching.
     Using default substitute font."
```

### 11.2 Custom Encodings (Non-Unicode CMaps)

**Scenario**: The embedded font uses a custom encoding where character codes do not map to Unicode. Common in older PDFs generated by TeX, old Adobe tools, or specialized systems.

**Handling**:

```
1. Check the font's /Encoding entry in the PDF dictionary:
   - /WinAnsiEncoding, /MacRomanEncoding: standard, mappable to Unicode
   - /Differences array: custom glyph name mappings
   - Symbolic font (flagged in font descriptor): entirely custom encoding

2. For /Differences arrays:
   - Parse the differences to get glyph names for each character code
   - Map glyph names to Unicode using the Adobe Glyph List (AGL):
     "Agrave" -> U+00C0, "fi" -> U+FB01, etc.
   - For non-standard glyph names (e.g., "a134", "glyph42"):
     Use the /ToUnicode CMap if present

3. For symbolic fonts (e.g., ZapfDingbats, Symbol, custom symbol fonts):
   - Use /ToUnicode CMap for mapping
   - If no /ToUnicode: attempt glyph name matching via AGL
   - If neither works: the font is opaque; cannot determine which
     Unicode characters are present
   - Treat ALL characters typed by the user as "missing" from the
     embedded font, and use the substitute for everything
   - Warn user: "This font uses a custom encoding. All new text will
     use the substitute font."

4. For CIDFont with Identity-H/Identity-V CMap:
   - Character codes are raw GIDs (glyph indices)
   - MUST have a /ToUnicode CMap to determine Unicode coverage
   - If /ToUnicode is present: parse it to build the coverage set
   - If /ToUnicode is absent: attempt to use the font program's
     internal cmap table (if it's a TrueType-based CIDFont)
   - If neither works: same as symbolic font handling above
```

### 11.3 Type 3 (Bitmap/User-Defined) Fonts

**Scenario**: The embedded font is a Type 3 font, where each glyph is defined as a PDF content stream (arbitrary drawing commands, not standard outlines).

**Handling**:

```
1. Type 3 fonts have NO TrueType/CFF outlines.
   FreeType cannot parse them. The glyph "outlines" are PDF drawing commands.

2. Metric extraction is still possible:
   - /Widths array gives advance widths per character code
   - /FontBBox gives overall bounding box
   - /FontMatrix gives the transform to text space
   - Individual glyph bounding boxes from /d1 operator in each
     glyph's content stream

3. Visual analysis is severely limited:
   - No outline-based stem weight measurement
   - No serif detection from outlines
   - Can only measure widths and bounding boxes

4. Strategy:
   - Extract width-based metrics (average width, width-to-height ratio)
   - Use font name for heuristic matching
   - Present user with the top candidates and warn:
     "This is a Type 3 font with custom glyph drawings.
     Substitute matching is based on character widths only.
     Visual consistency cannot be guaranteed."
   - Recommend: user provides their own font file if possible

5. Font switching still works normally:
   - Type 3 font remains /F1 for characters it has
   - Substitute (normal TrueType/CFF) becomes /F2
   - The Tf operator works the same way regardless of font type
```

### 11.4 CIDFonts with Identity CMaps

**Scenario**: A CIDFont (typically for CJK text) uses Identity-H or Identity-V as its CMap, meaning character codes are glyph indices directly.

**Handling**:

```
1. Determining Unicode coverage:
   - REQUIRE /ToUnicode CMap (most PDF generators include it)
   - Parse the ToUnicode CMap to build:
     cid_to_unicode: map<uint16_t, char32_t>
   - Invert to get: unicode_to_cid: map<char32_t, uint16_t>
   - The available_codepoints set is the values of cid_to_unicode

2. If /ToUnicode is missing:
   - Check if the CIDFont has a /CIDToGIDMap:
     If /Identity: CID = GID
     If stream: parse the mapping
   - Access the font program's internal cmap table via FreeType
   - Build Unicode coverage from the internal cmap
   - If this also fails: report the font as having unknown coverage

3. Glyph analysis:
   - CIDFonts are typically TrueType (CIDFontType2) or CFF (CIDFontType0)
   - Both are fully analyzable by FreeType for outline metrics
   - The stem weight, x-height, etc. measurements work normally

4. Substitution for CJK:
   - CJK substitution is handled differently:
     Noto Sans CJK / Noto Serif CJK are the primary substitutes
   - CJK fonts have relatively uniform metrics across foundries
     (weight and proportion are the main variables)
   - If the user needs CJK glyphs and the CJK font pack is not installed,
     prompt for download

5. Content stream for CIDFonts uses different text operators:
   - Character codes are multi-byte (typically 2 bytes)
   - The TJ/Tj operators work the same way, but string bytes are
     interpreted differently based on the CMap
   - When generating font-switched content streams, ensure the
     substitute CIDFont uses a compatible CMap
```

### 11.5 Fonts Using Glyph Names Instead of Unicode Mappings

**Scenario**: The font program has no cmap table (or a useless one), but glyphs are identified by name in the PDF's /Encoding dictionary.

**Handling**:

```
1. Build the name-to-Unicode mapping:
   - Load the Adobe Glyph List (AGL) -- a static table of ~4,600 entries
   - For each character code in the PDF font's encoding:
     a. Get the glyph name from /Encoding or /Differences
     b. Look up the glyph name in AGL -> Unicode codepoint
     c. If not in AGL: try the "uniXXXX" naming convention
        (e.g., "uni0041" -> U+0041 = 'A')
     d. If neither works: character is unmappable

2. For coverage determination:
   - The available_codepoints set is built from the AGL mapping
   - Characters that cannot be mapped are treated as unavailable
     for the purpose of substitution (they exist in the font
     but we cannot determine what they represent)

3. For text rendering:
   - When the user types a Unicode character, find the corresponding
     glyph name via reverse AGL lookup
   - Check if that glyph name exists in the embedded font's encoding
   - If yes: character available in embedded font
   - If no: needs substitute

4. Special case: .notdef glyph
   - If the embedded font uses .notdef as a fallback, do not count
     .notdef as "covering" any character
```

### 11.6 Encrypted/Protected Fonts

**Scenario**: The font's embedding permissions (fsType bits in the OS/2 table) restrict modification or subsetting.

**Handling**:

```
1. Check the OS/2 table's fsType field:
   Bit 0: Reserved
   Bit 1: Restricted License (no embedding allowed)
   Bit 2: Preview & Print (embedding OK, but not editable)
   Bit 3: Editable (embedding OK, document can be edited)
   Bit 8: No Subsetting
   Bit 9: Bitmap Embedding Only

2. For fonts with restrictive fsType:
   - DO NOT extend the embedded subset (Section 7)
   - The font is already embedded in the PDF (the PDF creator
     handled licensing), but we should not modify it further
   - Use the dual-font substitution approach instead
   - The substitute font must have permissive fsType

3. Log a warning if fsType is restrictive:
   "Font 'X' has embedding restrictions (fsType=0x0002).
   The embedded subset will not be modified. Using substitute font."

4. Note: PDF viewers are not technically required to enforce fsType,
   and many do not. However, respecting it is good practice and
   avoids potential legal issues. The architecture supports both
   approaches via a configuration flag.
```

### 11.7 Multi-Byte Encoding Edge Cases

```
Scenario: The font uses a CMap that maps multi-byte sequences to glyphs,
and the same byte can be the start of a multi-byte sequence or a
single-byte character depending on context.

Handling:
- Parse the CMap completely to build a byte-sequence-to-CID mapping
- When splitting text for font switching, ensure split points are
  at character boundaries, not in the middle of a multi-byte sequence
- The CMap parser must handle all CMap operators:
  begincodespacerange, beginbfchar, beginbfrange, begincidchar, begincidrange
- Use QPDF's CMap parsing if available, or implement per PDF spec
```

---

## 12. Data Flow: Keystroke Through Substitution

This section provides the complete end-to-end data flow for the scenario: the user presses a key that produces a character not in the embedded font's subset.

```
=== KEYSTROKE EVENT ===

[1] Qt Event Loop delivers QKeyEvent to TextEditOverlay widget
    |
    v
[2] TextEditOverlay::keyPressEvent(QKeyEvent* e)
    Extracts: char32_t codepoint = e->text().toUcs4()[0]
    |
    v
[3] FontSubstitutionCoordinator::classifyCharacter(codepoint, editing_context)
    Performs: O(1) lookup in editing_context.embedded_codepoints set
    Returns: GlyphSource::SubstituteFont
    Duration: <0.001ms
    |
    v
[4] TextEditOverlay inserts character into its QTextDocument
    The QTextDocument uses the preview_font (substitute, pre-scaled)
    The character renders immediately in the overlay widget
    If GlyphSource was SubstituteFont, the character gets a subtle
    yellow background highlight via QTextCharFormat
    Duration: <1ms (Qt text layout engine)
    |
    v
[5] User sees the character immediately. Continues typing.

=== COMMIT EVENT ===

[6] User clicks outside the text area (or presses Escape/Enter)
    TextEditOverlay emits commitText(QString text, QPointF position,
                                      float font_size,
                                      QPDFObjectHandle source_font)
    |
    v
[7] TextEditCommand is created (for undo/redo)
    TextEditCommand::execute() begins:
    |
    v
[8] FontSubstitutionCoordinator::resolveSubstitution(
        source_font,           // The embedded font's PDF object
        text,                  // The full text string
        font_size,             // Point size
        needed_codepoints      // Characters not in embedded subset
    )
    |
    +--[8a] Check: is substitution already resolved for this font?
    |        (Previous user choice stored in substitution_decisions cache)
    |        If YES: skip to step [9] with cached substitute
    |        If NO: continue
    |
    +--[8b] FullFontFinder::findFullFont(source_font metadata)
    |        Search by PostScript name, family name, embedded metadata
    |        Duration: ~10-50ms (system font enumeration, cached after first)
    |        If FOUND: proceed to font subset extension (Section 7)
    |                  Return SubstitutionResult::ExtendSubset
    |        If NOT FOUND: continue
    |
    +--[8c] EmbeddedFontAnalyzer::analyze(source_font stream bytes)
    |        Compute FontMetricProfile
    |        Duration: <1ms (cache hit) or ~50-200ms (cache miss, first time)
    |
    +--[8d] SubstituteFontSelector::selectBest(
    |            source_profile, needed_codepoints)
    |        Score all candidates, return ranked list
    |        Duration: <10ms (all profiles cached) or ~2-5s (first time, cold)
    |        IMPORTANT: First-time analysis is done in background thread
    |        during document open, so by commit time it should be cached
    |
    +--[8e] If best candidate score > THRESHOLD (e.g., 20.0):
    |        -> Show FontSubstitutionDialog to user
    |        -> User selects substitute
    |        Else: use top-ranked candidate automatically
    |
    v
[9] GlyphScaleAdjuster::computeTransform(
        source_profile, substitute_profile)
    Computes: scale_x, scale_y, weight_adjustment
    Duration: <0.1ms (pure arithmetic)
    |
    v
[10] PdfFontEmbedder::embedSubstituteFont(
         qpdf_document,
         substitute_font_file,
         needed_codepoints,
         transform_params,
         page_object)
     |
     +--[10a] Subset the substitute font (hb-subset)
     |         Keep only needed glyphs + dependencies
     |         Duration: ~20-100ms depending on font size
     |
     +--[10b] Create PDF font objects:
     |         - Font dictionary (/Type /Font)
     |         - CIDFont dictionary (for TrueType)
     |         - Font descriptor
     |         - Font stream (the subset program)
     |         - ToUnicode CMap
     |         - Widths array (adjusted per Section 5.4)
     |         Duration: ~5-20ms
     |
     +--[10c] Register font in page Resources:
              /F2 -> substitute font object
              Duration: <1ms (QPDF object manipulation)
     |
     v
[11] PdfFontEmbedder::generateContentStream(
         text, source_font, substitute_font,
         source_codepoints, position, font_size,
         adjusted_font_size, kerning_data)
     Generates: BT/ET block with Tf switches (Section 6)
     Duration: <1ms
     |
     v
[12] Write content stream to PDF page via QPDF
     Either append to existing content stream or create new one
     Duration: ~5-10ms
     |
     v
[13] Invalidate tile cache for affected page region
     |
     v
[14] MuPDF re-renders the affected tile(s)
     Reads the modified PDF bytes from QPDF
     Duration: ~20-50ms per tile
     |
     v
[15] Updated tile displayed on screen
     User sees the final, correctly rendered result
     Total commit duration: ~100-500ms (mostly dominated by
     font subsetting and MuPDF re-render)
```

---

## 13. C++ Header Signatures

### 13.1 FontMetricProfile (Data Structure)

```cpp
// File: src/fonts/font_metric_profile.h
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_set>

namespace pdfe::fonts {

enum class SerifStyle : uint8_t {
    Unknown = 0,
    SansSerif,
    TransitionalSerif,
    ModernSerif,
    SlabSerif
};

struct FontMetricProfile {
    // Identity
    std::string postscript_name;
    std::string family_name;
    std::string style_name;
    uint64_t    font_program_hash = 0;

    // Table metrics (from OS/2, hhea, head, post)
    int32_t  units_per_em        = 0;
    int32_t  os2_weight_class    = 0;
    int32_t  os2_width_class     = 0;
    int32_t  os2_cap_height      = 0;
    int32_t  os2_x_height        = 0;
    int32_t  os2_avg_char_width  = 0;
    std::array<uint8_t, 10> panose = {};
    float    italic_angle        = 0.0f;

    // Measured metrics (normalized to units_per_em)
    float measured_x_height       = 0.0f;
    float measured_cap_height     = 0.0f;
    float measured_ascender       = 0.0f;
    float measured_descender      = 0.0f;
    float measured_avg_char_width = 0.0f;
    float vertical_stem_weight    = 0.0f;
    float horizontal_stem_weight  = 0.0f;
    float contrast_ratio          = 0.0f;
    float x_to_cap_ratio          = 0.0f;
    float width_to_height_ratio   = 0.0f;

    // Structural classification
    SerifStyle serif_style        = SerifStyle::Unknown;
    float serif_bracket_depth     = 0.0f;
    float roundness               = 0.0f;
    float counter_openness        = 0.0f;

    // Coverage
    std::unordered_set<char32_t> available_codepoints;
    uint32_t glyph_count          = 0;

    // Quality flags
    bool has_reliable_table_metrics = false;
    bool has_measured_metrics       = false;
    uint32_t measurement_glyph_count = 0;

    // Serialization
    [[nodiscard]] std::string toJson() const;
    static FontMetricProfile fromJson(std::string_view json);
};

}  // namespace pdfe::fonts
```

### 13.2 EmbeddedFontAnalyzer

```cpp
// File: src/fonts/embedded_font_analyzer.h
#pragma once

#include "font_metric_profile.h"
#include <cstdint>
#include <memory>
#include <span>
#include <expected>
#include <string>

// Forward declarations
struct FT_LibraryRec_;
struct FT_FaceRec_;

namespace pdfe::fonts {

class FontAnalysisCache;

enum class AnalysisError {
    InvalidFontData,
    FreeTypeInitFailed,
    NoGlyphsAvailable,
    UnsupportedFontFormat,
    InternalError
};

class EmbeddedFontAnalyzer {
public:
    explicit EmbeddedFontAnalyzer(FT_LibraryRec_* ft_library,
                                  std::shared_ptr<FontAnalysisCache> cache);
    ~EmbeddedFontAnalyzer();

    // Non-copyable, movable
    EmbeddedFontAnalyzer(const EmbeddedFontAnalyzer&) = delete;
    EmbeddedFontAnalyzer& operator=(const EmbeddedFontAnalyzer&) = delete;
    EmbeddedFontAnalyzer(EmbeddedFontAnalyzer&&) noexcept;
    EmbeddedFontAnalyzer& operator=(EmbeddedFontAnalyzer&&) noexcept;

    /// Analyze an embedded font from raw bytes (e.g., extracted from PDF stream).
    /// Returns a FontMetricProfile with all extractable metrics.
    /// Uses cache if available (keyed by SHA-256 of font_data).
    [[nodiscard]] std::expected<FontMetricProfile, AnalysisError>
    analyze(std::span<const uint8_t> font_data) const;

    /// Analyze a font loaded from a file path on disk.
    /// Cache key includes file path and modification time.
    [[nodiscard]] std::expected<FontMetricProfile, AnalysisError>
    analyzeFile(const std::string& font_path) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    // Internal measurement methods
    [[nodiscard]] float measureXHeight(FT_FaceRec_* face) const;
    [[nodiscard]] float measureCapHeight(FT_FaceRec_* face) const;
    [[nodiscard]] float measureVerticalStemWeight(FT_FaceRec_* face) const;
    [[nodiscard]] float measureHorizontalStemWeight(FT_FaceRec_* face) const;
    [[nodiscard]] SerifStyle detectSerifStyle(FT_FaceRec_* face) const;
    [[nodiscard]] float measureRoundness(FT_FaceRec_* face) const;
    [[nodiscard]] float measureCounterOpenness(FT_FaceRec_* face) const;
    [[nodiscard]] std::unordered_set<char32_t>
        enumerateCodepoints(FT_FaceRec_* face) const;
};

}  // namespace pdfe::fonts
```

### 13.3 SubstituteFontSelector

```cpp
// File: src/fonts/substitute_font_selector.h
#pragma once

#include "font_metric_profile.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace pdfe::fonts {

class EmbeddedFontAnalyzer;
class FontAnalysisCache;
class PlatformFontDiscovery;

struct SubstituteCandidate {
    FontMetricProfile profile;
    std::string       font_file_path;
    float             score;         // lower is better, 0.0 = perfect
    bool              covers_all;    // true if all needed codepoints available
    bool              is_bundled;    // true if from PDFE's bundled fonts
    bool              is_known_pair; // true if from the known-substitution table
};

struct SelectionOptions {
    uint32_t max_candidates     = 10;     // Max results to return
    bool     include_downloaded = true;    // Include Google Fonts suggestions
    bool     prefer_bundled     = false;   // Boost bundled fonts in ranking
    float    minimum_score      = 50.0f;   // Reject candidates worse than this
};

class SubstituteFontSelector {
public:
    SubstituteFontSelector(
        std::shared_ptr<EmbeddedFontAnalyzer> analyzer,
        std::shared_ptr<FontAnalysisCache> cache,
        std::shared_ptr<PlatformFontDiscovery> font_discovery);
    ~SubstituteFontSelector();

    /// Find the best substitute fonts for a given embedded font profile.
    /// @param source  The analyzed profile of the embedded (subset) font.
    /// @param needed  Unicode codepoints that need to be available.
    /// @param options Configuration for the selection process.
    /// @return Ranked list of candidates (best first).
    [[nodiscard]] std::vector<SubstituteCandidate> selectCandidates(
        const FontMetricProfile& source,
        std::span<const char32_t> needed_codepoints,
        const SelectionOptions& options = {}) const;

    /// Compute the similarity score between two font profiles.
    /// Public for testing and for the UI to show scores.
    [[nodiscard]] static float computeScore(
        const FontMetricProfile& source,
        const FontMetricProfile& candidate);

    /// Check the known-substitution table for a quick match.
    /// Returns empty string if no known substitute.
    [[nodiscard]] std::string findKnownSubstitute(
        const std::string& postscript_name,
        const std::string& family_name) const;

    /// Asynchronous candidate pre-computation.
    /// Call during document open to pre-warm the cache.
    /// The callback is invoked on the main thread when complete.
    void precomputeCandidatesAsync(
        const FontMetricProfile& source,
        std::span<const char32_t> needed_codepoints,
        std::function<void(std::vector<SubstituteCandidate>)> callback);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace pdfe::fonts
```

### 13.4 GlyphScaleAdjuster

```cpp
// File: src/fonts/glyph_scale_adjuster.h
#pragma once

#include "font_metric_profile.h"
#include <cstdint>

namespace pdfe::fonts {

struct GlyphTransform {
    float scale_x           = 1.0f;   // Horizontal scale factor
    float scale_y           = 1.0f;   // Vertical scale factor (x-height alignment)
    float offset_x          = 0.0f;   // Horizontal translation (font units)
    float offset_y          = 0.0f;   // Vertical translation (baseline adjustment)
    float weight_adjustment = 0.0f;   // FT_Outline_EmboldenXY strength
    float adjusted_font_size = 0.0f;  // Font size to use in Tf operator

    enum class QualityTier : uint8_t {
        MetricExact   = 1,  // Known metric-compatible pair, no adjustment
        Scaled        = 2,  // x-height + width scaling only
        Transformed   = 3,  // Scaling + weight compensation
        BestEffort    = 4   // All transforms; visual mismatch expected
    };
    QualityTier quality_tier = QualityTier::BestEffort;
};

class GlyphScaleAdjuster {
public:
    /// Compute the affine transform needed to make the substitute font's
    /// glyphs visually match the source font's metrics.
    /// @param source      Profile of the embedded (original) font
    /// @param substitute  Profile of the chosen substitute font
    /// @param base_font_size  The point size at which text is rendered
    /// @return Transform parameters for PDF embedding
    [[nodiscard]] static GlyphTransform computeTransform(
        const FontMetricProfile& source,
        const FontMetricProfile& substitute,
        float base_font_size);

    /// Compute per-glyph width overrides for the substitute font's
    /// Widths array in the PDF font dictionary.
    /// For characters that exist in both fonts, the substitute's width
    /// is overridden to match the source's width.
    /// @param source       Profile of the embedded font
    /// @param substitute   Profile of the substitute font
    /// @param transform    Transform computed by computeTransform()
    /// @param codepoints   The codepoints to compute widths for
    /// @return Map of codepoint -> width (in PDF glyph space units, 1/1000 of text space unit)
    [[nodiscard]] static std::unordered_map<char32_t, int32_t>
    computeWidthOverrides(
        const FontMetricProfile& source,
        const FontMetricProfile& substitute,
        const GlyphTransform& transform,
        std::span<const char32_t> codepoints);

private:
    static constexpr float SCALE_THRESHOLD      = 0.02f;  // 2% difference triggers scaling
    static constexpr float WIDTH_THRESHOLD       = 0.03f;  // 3% difference triggers width adj
    static constexpr float WEIGHT_THRESHOLD      = 0.15f;  // 15% difference triggers emboldening
    static constexpr float POOR_MATCH_THRESHOLD = 15.0f;  // Score above this = best-effort tier
};

}  // namespace pdfe::fonts
```

### 13.5 PdfFontEmbedder

```cpp
// File: src/fonts/pdf_font_embedder.h
#pragma once

#include "font_metric_profile.h"
#include "glyph_scale_adjuster.h"
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declarations (QPDF types)
class QPDF;
class QPDFObjectHandle;

namespace pdfe::fonts {

enum class EmbedError {
    FontSubsetFailed,
    InvalidFontData,
    QpdfWriteError,
    ContentStreamGenerationFailed,
    FontMergeFailed,
    ValidationFailed
};

struct EmbedResult {
    std::string font_resource_name;    // e.g., "/F2" -- the name in the Resources dict
    QPDFObjectHandle font_object;      // The font dictionary object in the PDF
    bool subset_extended;              // true if we extended the original subset
};

/// Describes a run of text using a specific font
struct FontRun {
    enum class Source { Embedded, Substitute };
    Source source;
    std::u32string text;         // Unicode text for this run
    float font_size;             // Point size (may differ for substitute)
};

class PdfFontEmbedder {
public:
    explicit PdfFontEmbedder(QPDF& qpdf);
    ~PdfFontEmbedder();

    /// Attempt to extend an embedded font's subset with new glyphs from
    /// the complete font file found on the system.
    /// @param embedded_font  The existing font dictionary in the PDF
    /// @param full_font_path Path to the complete font file on disk
    /// @param new_codepoints Codepoints to add to the subset
    /// @return The updated font object, or error
    [[nodiscard]] std::expected<EmbedResult, EmbedError> extendSubset(
        QPDFObjectHandle embedded_font,
        const std::string& full_font_path,
        std::span<const char32_t> new_codepoints);

    /// Embed a substitute font as a new font object in the PDF.
    /// Creates a subset containing only the needed glyphs.
    /// @param substitute_font_path  Path to the substitute font file
    /// @param needed_codepoints     Codepoints to include in the subset
    /// @param transform             Scaling/adjustment parameters
    /// @param page                  The page to register the font on
    /// @return The new font object and its resource name
    [[nodiscard]] std::expected<EmbedResult, EmbedError> embedSubstitute(
        const std::string& substitute_font_path,
        std::span<const char32_t> needed_codepoints,
        const GlyphTransform& transform,
        QPDFObjectHandle page);

    /// Generate a content stream with automatic font switching for mixed
    /// embedded/substitute text.
    /// @param text               Full Unicode text string
    /// @param embedded_font_name Resource name of embedded font (e.g., "/F1")
    /// @param substitute_font_name Resource name of substitute (e.g., "/F2")
    /// @param embedded_codepoints Characters available in embedded font
    /// @param base_font_size     Point size for embedded font
    /// @param transform          Transform params (includes adjusted_font_size)
    /// @param position           Text position (x, y) in PDF user space
    /// @return PDF content stream bytes (BT ... ET block)
    [[nodiscard]] std::expected<std::string, EmbedError>
    generateFontSwitchedContentStream(
        std::u32string_view text,
        const std::string& embedded_font_name,
        const std::string& substitute_font_name,
        const std::unordered_set<char32_t>& embedded_codepoints,
        float base_font_size,
        const GlyphTransform& transform,
        std::pair<float, float> position);

    /// Split text into runs by font source.
    /// Public for testing.
    [[nodiscard]] static std::vector<FontRun> splitIntoFontRuns(
        std::u32string_view text,
        const std::unordered_set<char32_t>& embedded_codepoints,
        float base_font_size,
        float adjusted_font_size);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    // TrueType subset merging
    [[nodiscard]] std::expected<std::vector<uint8_t>, EmbedError>
    mergeTrueTypeSubsets(
        std::span<const uint8_t> original_subset,
        std::span<const uint8_t> new_glyphs_subset);

    // CFF subset merging
    [[nodiscard]] std::expected<std::vector<uint8_t>, EmbedError>
    mergeCffSubsets(
        std::span<const uint8_t> original_subset,
        std::span<const uint8_t> new_glyphs_subset);

    // Generate ToUnicode CMap for a set of codepoints
    [[nodiscard]] std::string generateToUnicodeCMap(
        const std::unordered_map<uint16_t, char32_t>& gid_to_unicode);
};

}  // namespace pdfe::fonts
```

### 13.6 FontSubstitutionCoordinator (Orchestrator)

```cpp
// File: src/fonts/font_substitution_coordinator.h
#pragma once

#include "font_metric_profile.h"
#include "glyph_scale_adjuster.h"
#include "substitute_font_selector.h"
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>

// Forward declarations
class QPDF;
class QPDFObjectHandle;

namespace pdfe::fonts {

class EmbeddedFontAnalyzer;
class FontAnalysisCache;
class FullFontFinder;
class PdfFontEmbedder;
class SubstituteFontSelector;

enum class SubstitutionStrategy {
    SubsetExtended,      // Full font found, subset was extended
    SubstituteEmbedded,  // Substitute font chosen and embedded
    UserProvidedFont,    // User loaded their own font file
    NoSubstitution       // All characters available, no action needed
};

struct SubstitutionResult {
    SubstitutionStrategy strategy;
    std::string content_stream;              // The BT/ET block to write
    std::string substitute_font_name;        // Resource name (e.g., "/F2")
    GlyphTransform transform;
    FontMetricProfile substitute_profile;
    float score;                             // Match quality score
    bool user_warned;                        // True if match quality was poor
};

/// Context for real-time character classification during editing.
/// Created once when the user starts editing a text block, reused for
/// every keystroke.
struct EditingContext {
    std::unordered_set<char32_t> embedded_codepoints;
    FontMetricProfile embedded_profile;
    std::string embedded_font_resource_name;
    float font_size;

    // Preview font (for the editing overlay)
    std::string preview_font_path;
    float preview_scale_factor;
    bool substitute_available;

    // Pre-computed best substitute (may be null if analysis pending)
    std::optional<SubstituteCandidate> best_substitute;
};

enum class GlyphSource {
    EmbeddedFont,
    SubstituteFont,
    Unavailable
};

class FontSubstitutionCoordinator {
public:
    FontSubstitutionCoordinator(
        QPDF& qpdf,
        std::shared_ptr<EmbeddedFontAnalyzer> analyzer,
        std::shared_ptr<SubstituteFontSelector> selector,
        std::shared_ptr<FullFontFinder> font_finder,
        std::shared_ptr<PdfFontEmbedder> embedder,
        std::shared_ptr<FontAnalysisCache> cache);
    ~FontSubstitutionCoordinator();

    /// Prepare an editing context for a specific embedded font.
    /// Called when the user starts editing a text block.
    /// This triggers background analysis if not cached.
    [[nodiscard]] EditingContext prepareEditingContext(
        QPDFObjectHandle font_object,
        float font_size);

    /// Classify a single character during editing.
    /// Must complete in <0.1ms for responsive typing.
    [[nodiscard]] GlyphSource classifyCharacter(
        char32_t codepoint,
        const EditingContext& ctx) const;

    /// Resolve and apply font substitution when text is committed.
    /// This runs the full pipeline: find full font -> analyze -> select ->
    /// scale -> embed -> generate content stream.
    /// @param ctx     The editing context (from prepareEditingContext)
    /// @param text    The committed text (Unicode)
    /// @param page    The PDF page to write to
    /// @param position Text position in PDF user space
    /// @return The substitution result, including the content stream to write
    [[nodiscard]] std::expected<SubstitutionResult, std::string>
    resolveSubstitution(
        const EditingContext& ctx,
        std::u32string_view text,
        QPDFObjectHandle page,
        std::pair<float, float> position);

    /// Request the user to choose a substitute font.
    /// This is called when automatic selection produces a poor match
    /// (score above threshold) or when the user explicitly requests it.
    /// @param callback  Called with the user's choice (on the main thread)
    using UserChoiceCallback = std::function<void(
        std::optional<SubstituteCandidate>,  // User's choice (nullopt = cancel)
        bool flag_as_substitute              // User wants substitute text flagged
    )>;
    void requestUserChoice(
        const EditingContext& ctx,
        const std::vector<SubstituteCandidate>& candidates,
        UserChoiceCallback callback);

    /// Pre-analyze all embedded fonts in the document.
    /// Call this during document open (background thread).
    /// Populates the cache so that editing is responsive.
    void preanalyzeDocumentFonts(
        std::function<void(int progress_percent)> progress_callback = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace pdfe::fonts
```

### 13.7 FontAnalysisCache

```cpp
// File: src/fonts/font_analysis_cache.h
#pragma once

#include "font_metric_profile.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace pdfe::fonts {

class FontAnalysisCache {
public:
    /// @param db_path Path to the SQLite cache database file.
    ///                Created if it does not exist.
    explicit FontAnalysisCache(const std::string& db_path);
    ~FontAnalysisCache();

    // Thread-safe: all methods are internally synchronized

    /// Look up a font profile by its cache key (hash or path+mtime).
    [[nodiscard]] std::optional<FontMetricProfile>
    getProfile(const std::string& cache_key) const;

    /// Store a font profile.
    void putProfile(const std::string& cache_key,
                    const FontMetricProfile& profile);

    /// Look up a precomputed similarity score.
    [[nodiscard]] std::optional<float>
    getScore(const std::string& source_key,
             const std::string& candidate_key) const;

    /// Store a similarity score.
    void putScore(const std::string& source_key,
                  const std::string& candidate_key,
                  float score);

    /// Look up a previous substitution decision for a document+font pair.
    struct SubstitutionDecision {
        std::string substitute_path;
        std::string transform_params_json;
        bool user_chosen;
    };
    [[nodiscard]] std::optional<SubstitutionDecision>
    getDecision(const std::string& document_id,
                const std::string& source_font_key) const;

    /// Store a substitution decision.
    void putDecision(const std::string& document_id,
                     const std::string& source_font_key,
                     const SubstitutionDecision& decision);

    /// Remove entries older than max_age_days that have been accessed
    /// fewer than min_access_count times.
    void prune(int max_age_days = 90, int min_access_count = 2);

    /// Return cache statistics for diagnostics.
    struct Stats {
        uint64_t profile_count;
        uint64_t score_count;
        uint64_t decision_count;
        uint64_t total_size_bytes;
        uint64_t hit_count;
        uint64_t miss_count;
    };
    [[nodiscard]] Stats getStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    mutable std::mutex m_mutex;
};

}  // namespace pdfe::fonts
```

### 13.8 FullFontFinder

```cpp
// File: src/fonts/full_font_finder.h
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pdfe::fonts {

struct FullFontMatch {
    std::string file_path;
    std::string postscript_name;
    std::string family_name;
    std::string style_name;
    float       confidence;       // 0.0-1.0, how confident we are this is the right font
    bool        outline_verified; // true if glyph outlines were compared and matched
};

class FullFontFinder {
public:
    FullFontFinder();
    ~FullFontFinder();

    /// Search for the complete original font on the system.
    /// Searches in priority order:
    ///   1. PostScript name match (highest confidence)
    ///   2. Family name + style match
    ///   3. Family name match (any style)
    ///
    /// @param postscript_name  The font's PostScript name (from PDF /BaseFont)
    /// @param family_name      The font's family name (from PDF /FontFamily or parsed)
    /// @param style_name       The style (Bold, Italic, etc.)
    /// @param embedded_data    Raw bytes of the embedded font program, for
    ///                         outline verification against candidates
    /// @return Best match, or nullopt if no acceptable match found
    [[nodiscard]] std::optional<FullFontMatch> findFullFont(
        const std::string& postscript_name,
        const std::string& family_name,
        const std::string& style_name,
        std::span<const uint8_t> embedded_data = {}) const;

    /// Verify that a candidate font file matches the embedded font
    /// by comparing glyph outlines for shared characters.
    /// @return true if at least 5 glyphs match within tolerance
    [[nodiscard]] bool verifyOutlineMatch(
        std::span<const uint8_t> embedded_data,
        const std::string& candidate_path,
        int min_matching_glyphs = 5,
        float tolerance = 0.01f) const;

    /// Register an additional search directory (e.g., user-provided font folder)
    void addSearchDirectory(const std::string& path);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace pdfe::fonts
```

### 13.9 PlatformFontDiscovery (Abstract Interface)

```cpp
// File: src/fonts/platform_font_discovery.h
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pdfe::fonts {

struct SystemFontInfo {
    std::string file_path;
    std::string postscript_name;
    std::string family_name;
    std::string style_name;
    int32_t     weight_class;    // 100-900, 0 if unknown
    bool        is_italic;
    uint64_t    file_mtime;      // Modification time for cache invalidation
};

/// Abstract interface for platform-specific font enumeration.
/// Implementations:
///   - FontconfigDiscovery  (Linux, macOS)
///   - DirectWriteDiscovery (Windows)
class PlatformFontDiscovery {
public:
    virtual ~PlatformFontDiscovery() = default;

    /// Enumerate all system-installed fonts.
    /// Cached internally after first call.
    [[nodiscard]] virtual std::vector<SystemFontInfo>
    enumerateSystemFonts() const = 0;

    /// Find fonts matching a PostScript name (exact match).
    [[nodiscard]] virtual std::vector<SystemFontInfo>
    findByPostScriptName(const std::string& name) const = 0;

    /// Find fonts matching a family name and optional style.
    [[nodiscard]] virtual std::vector<SystemFontInfo>
    findByFamilyName(const std::string& family,
                     const std::string& style = "") const = 0;

    /// Create the platform-appropriate implementation.
    [[nodiscard]] static std::unique_ptr<PlatformFontDiscovery> create();
};

}  // namespace pdfe::fonts
```

---

## 14. Implementation Roadmap

### Phase 1: Foundation (Weeks 1-2)

| Task | Component | Priority | Effort |
|------|-----------|----------|--------|
| Implement `FontMetricProfile` struct + JSON serialization | Data | P0 | 2 days |
| Implement `FontAnalysisCache` (SQLite layer) | Cache | P0 | 3 days |
| Implement `PlatformFontDiscovery` (Windows DirectWrite) | Discovery | P0 | 3 days |
| Implement `PlatformFontDiscovery` (Linux Fontconfig) | Discovery | P1 | 2 days |
| Implement `BundledFontIndex` (scan Noto/Liberation/DejaVu) | Discovery | P0 | 1 day |

### Phase 2: Analysis Pipeline (Weeks 3-4)

| Task | Component | Priority | Effort |
|------|-----------|----------|--------|
| Implement table metric extraction (OS/2, hhea, post) | Analyzer | P0 | 2 days |
| Implement glyph outline measurement (x-height, cap-height) | Analyzer | P0 | 3 days |
| Implement stem weight measurement (ray-casting algorithm) | Analyzer | P0 | 3 days |
| Implement serif detection algorithm | Analyzer | P1 | 2 days |
| Implement roundness + counter openness measurement | Analyzer | P2 | 2 days |
| Integration test: analyze 20 diverse fonts, verify metrics | Testing | P0 | 2 days |

### Phase 3: Substitute Selection (Weeks 5-6)

| Task | Component | Priority | Effort |
|------|-----------|----------|--------|
| Implement scoring function (weighted metric distance) | Selector | P0 | 3 days |
| Implement PANOSE distance function | Selector | P1 | 1 day |
| Implement known-substitution table (fast path) | Selector | P0 | 1 day |
| Implement candidate pool generation + quick filter | Selector | P0 | 2 days |
| Implement parallel candidate analysis (thread pool) | Selector | P1 | 2 days |
| Validation: compare top 5 results against manual expert selection for 10 test fonts | Testing | P0 | 3 days |

### Phase 4: Glyph Adjustment + PDF Embedding (Weeks 7-9)

| Task | Component | Priority | Effort |
|------|-----------|----------|--------|
| Implement `GlyphScaleAdjuster` (all transform computations) | Adjuster | P0 | 3 days |
| Implement substitute font subsetting (hb-subset integration) | Embedder | P0 | 3 days |
| Implement PDF font dictionary creation (CIDFont Type2 + descriptors) | Embedder | P0 | 4 days |
| Implement ToUnicode CMap generation | Embedder | P0 | 2 days |
| Implement font-switched content stream generation | Embedder | P0 | 3 days |
| Implement font resource registration in page Resources | Embedder | P0 | 1 day |
| Integration test: embed substitute in PDF, verify rendering in MuPDF | Testing | P0 | 3 days |

### Phase 5: Font Subset Extension (Weeks 10-11)

| Task | Component | Priority | Effort |
|------|-----------|----------|--------|
| Implement `FullFontFinder` (system search + outline verification) | Finder | P0 | 3 days |
| Implement TrueType table merging (glyf, loca, hmtx, cmap, maxp) | Embedder | P0 | 5 days |
| Implement CFF merging (CharStrings, charset, encoding) | Embedder | P1 | 4 days |
| Implement post-merge validation (FreeType load + glyph comparison) | Embedder | P0 | 2 days |
| Integration test: extend subset, verify original + new glyphs render | Testing | P0 | 2 days |

### Phase 6: UI + Editor Integration (Weeks 12-14)

| Task | Component | Priority | Effort |
|------|-----------|----------|--------|
| Implement Font Substitution Dialog (Qt Widgets) | UI | P0 | 4 days |
| Implement visual comparison panel (FreeType -> QImage rendering) | UI | P0 | 3 days |
| Implement "Load Font File" flow | UI | P1 | 2 days |
| Implement editing overlay integration (preview font, char classification) | Integration | P0 | 3 days |
| Implement commit-time substitution pipeline | Integration | P0 | 3 days |
| Implement undo/redo integration | Integration | P0 | 2 days |
| Implement `preanalyzeDocumentFonts` background processing | Integration | P1 | 2 days |

### Phase 7: Edge Cases + Hardening (Weeks 15-16)

| Task | Component | Priority | Effort |
|------|-----------|----------|--------|
| Handle Type 3 fonts (width-only matching) | Edge case | P1 | 2 days |
| Handle custom encodings + AGL mapping | Edge case | P1 | 3 days |
| Handle CIDFonts with Identity CMaps + missing ToUnicode | Edge case | P1 | 2 days |
| Handle fonts with fsType restrictions | Edge case | P2 | 1 day |
| Handle multi-byte encoding split points | Edge case | P1 | 2 days |
| Fuzz testing: random font streams, corrupt tables | Testing | P0 | 3 days |
| Performance profiling: ensure commit < 500ms for typical edits | Testing | P0 | 2 days |

**Total estimated effort: 16 weeks (1 developer)**

---

## 15. Architecture Decision Records

### ADR-FSub-001: Measured Metrics Over PANOSE for Font Matching

**Status:** Proposed

**Context:** The font matching pipeline needs a reliable signal for visual similarity. PANOSE classification (10 bytes in the OS/2 table) provides a standardized classification, but it has known weaknesses: many fonts have incorrect or missing PANOSE data, the classification is coarse (16 values per digit), and it was designed for the Latin alphabet only.

**Decision:** Use directly measured glyph outline metrics (stem weight, x-height, cap-height, serif detection, contrast ratio) as the primary matching signal, with PANOSE as supplementary signal when available.

**Consequences:**
- (+) More accurate matching, especially for fonts with missing/wrong PANOSE data
- (+) Works for any font that has at least a few Latin glyphs
- (-) More expensive to compute (~50-200ms per font vs. ~0.01ms for PANOSE comparison)
- (-) Requires ray-casting outline analysis code (complex to implement and test)
- Mitigation: Aggressive caching makes the cost one-time per font per system

### ADR-FSub-002: Dual-Font Embedding Over Glyph Merging

**Status:** Proposed

**Context:** When using a substitute font for missing glyphs, there are two approaches: (A) merge substitute glyphs into the original font program, creating a single modified font, or (B) embed the substitute as a separate font and use font-switching operators in the content stream.

**Decision:** Use approach (B) -- dual-font embedding with font switching.

**Consequences:**
- (+) Does not modify the original embedded font, preserving any digital signatures on the document
- (+) Simpler implementation: no need to merge font programs (which is error-prone for CFF especially)
- (+) Clean separation: original glyphs are untouched, substitute is clearly separate
- (+) Easier undo: removing the substitute font is a clean operation
- (-) Slightly larger PDF: two font programs instead of one (mitigated by subsetting the substitute)
- (-) More complex content streams: font-switching operators add verbosity
- (-) Subtle rendering differences at font-switch boundaries (kerning gaps)
- Note: Approach (A) is used ONLY when the full original font is found on the system (Section 7), because in that case we are adding genuine original glyphs, not substitutes

### ADR-FSub-003: Lazy Substitution (Commit-Time, Not Keystroke-Time)

**Status:** Proposed

**Context:** The system must decide when to run the font substitution pipeline: on every keystroke (immediate), or when the user commits the edit (deferred).

**Decision:** Run the full substitution pipeline at commit time. During editing, use a pre-selected preview font rendered by Qt's text engine in an overlay widget.

**Consequences:**
- (+) Zero-latency typing experience (Qt renders preview font instantly)
- (+) Font subsetting and PDF embedding only happens once per edit, not per character
- (+) Allows batching of all needed codepoints for optimal subsetting
- (-) The preview rendering may not exactly match the final PDF rendering
- (-) User sees a slight visual "jump" at commit time if the preview font diverges from the final result
- Mitigation: The preview font is the same font file as the substitute, loaded into Qt with matching scale factor. Divergence is minimal.

---

## 16. Sources

### FreeType API
- [FreeType Sizing and Scaling Reference](http://freetype.org/freetype2/docs/reference/ft2-sizing_and_scaling.html)
- [FreeType TrueType Tables Reference (TT_OS2)](http://freetype.org/freetype2/docs/reference/ft2-truetype_tables.html)
- [FreeType Tutorial Part II (Glyph Metrics)](https://freetype.org/freetype2/docs/tutorial/step2.html)
- [FreeType Glyph Conventions](https://freetype.org/freetype2/docs/glyphs/glyphs-1.html)

### PDF Content Streams and Font Handling
- [PDF Graphic Operators Cheat Sheet (PDF Association)](https://pdfa.org/wp-content/uploads/2023/08/PDF-Operators-CheatSheet.pdf)
- [Text in PDF: Fonts and Spacing (Jay Berkenbilt / QPDF)](https://medium.com/@jberkenbilt/text-in-pdf-fonts-and-spacing-eae6fd8d2b40)
- [PDF Text Operators (Syncfusion)](https://www.syncfusion.com/succinctly-free-ebooks/pdf/text-operators)

### Font Subsetting
- [Re-Subsetting Embedded Font Subsets (PDF Association)](https://pdfa.org/re-subsetting-embedded-font-subsets/)
- [Font Subsetting: How It Works and When to Use (PDF Tools)](https://blog.pdf-tools.com/2015/05/font-subsetting-how-it-works-and-where.html)
- [Fonts in PDF Files (Prepressure)](https://www.prepressure.com/pdf/basics/fonts)
- [fontTools Subset Documentation](https://fonttools.readthedocs.io/en/stable/subset/)

### PANOSE Classification
- [PANOSE 2.0 White Paper (W3C)](https://www.w3.org/Fonts/Panose/pan2.html)
- [PANOSE Metrics Guide (Monotype)](https://monotype.github.io/panose/pan1.htm)
- [Panosifier GitHub Repository](https://github.com/source-foundry/panosifier)

### OS/2 Table
- [OS/2 Table Documentation (fontTools)](https://fonttools.readthedocs.io/en/latest/ttLib/tables/O_S_2f_2.html)
- [OS/2 Table Reference (Apple TrueType Manual)](https://developer.apple.com/fonts/TrueType-Reference-Manual/RM06/Chap6OS2.html)

### CIDFont and CMap Handling
- [CIDFont Glyph Selection Issue (PDF Association GitHub)](https://github.com/pdf-association/pdf-issues/issues/476)
- [Adobe Font Embedding Guidelines](https://adobe-type-tools.github.io/font-tech-notes/pdfs/AcrobatDC_FontPolicies.pdf)
