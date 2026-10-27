# Phase 1 — Defect Characterization (pipeline b91f4e83)

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
**Scope:** Observation only — NO source changes. Reproduce and characterize three
visual defects in the in-place text-edit feature, with pixel + font-descriptor
evidence, before any root-cause/fix work (phases 2–7).

**Target document:**
`C:\Users\dev\Desktop\SpecimenA Green A Tale of Two Collards - Original Recipe - Tuvache Betton - Copy.pdf`
(18.8 MB, page-1 title line "A Tale of Two Collards").

---

## 1. Ground truth — the font actually in the PDF

Read directly from the file with PyMuPDF (`get_text("dict")` + `xref_object`).

### Title span
- Text span: `ATaleofTwoCollards`, size **33 pt**, color **black (0)**.
- PyMuPDF span flags = **22** = bold(16) + serif(4) + italic(2) → classified
  serif, italic, bold.
- Font used: **`BAAAAA+PalatinoLTPro-BoldItalic`** (the H1 "Ka'amala's Greens"
  above it uses the non-italic sibling `AAAAAA+PalatinoLTPro-Bold`).

### Embedding type
- `/Subtype /Type0`, `/Encoding /Identity-H`, DescendantFont
  `/Subtype /CIDFontType2` (TrueType CID), `/CIDToGIDMap /Identity`,
  `/FontFile2` present (xref 1460) → **embedded + SUBSET** (the `BAAAAA+`
  six-letter prefix is the PDF subset tag).

### `/FontDescriptor` (xref 1461) — the authoritative attributes
| Key | Value | Interpretation |
|---|---|---|
| `/Flags` | **68** | `0b1000100` = bit3 **Symbolic** (4) + bit7 **Italic** (64). **The Serif bit (value 2) is NOT set**, and the font is flagged **Symbolic** (normal for a subset CID font with a custom Identity encoding). |
| `/ItalicAngle` | **−10** | Definitively italic. |
| `/Ascent` `/Descent` `/CapHeight` | 726 / −274 / 681 | present and usable. |
| `/StemV` | 281 | heavy stem → bold. |
| `/FontBBox` | −170 −271 1073 948 | present. |

**Key consequence #1 (italic is recoverable):** italic is determinable two
independent ways — `/ItalicAngle = −10` and the `/Flags` Italic bit (value 64).
So **defect 1b (overlay loses italic) and any italic question are propagation
bugs, not missing-data problems** — the data is unambiguously present.

**Key consequence #2 (serif is NOT in the flags):** the `/Flags` Serif bit is
**absent** and the font is **Symbolic**. Any classifier that derived serif/sans
from `/FontDescriptor` flags would conclude "not serif" → sans. The font *name*
("Palatino") and the embedded glyph outlines are the only in-file serif signals.

---

## 2. Live visual evidence

Primary evidence is the user's own live capture while editing the title line:
`N:\Personal\Images\Screenshots\Screenshot From 2026-06-28 10-34-39.png`,
preserved as `tests/fixtures/evidence/defect_all_three_live.png` with per-defect
crops alongside it. All three defects are visible in this single frame; they
reproduce deterministically for this file (the font facts above are static, so
the selection/rendering decisions are deterministic per launch).

Reference (correct) rendering in the same frame: the H1 **"Ka'amala's Greens"**
(not being edited) renders in correct **bold serif** — proving the page render
path itself is fine; the defects are specific to the **edit overlay** and the
**new-char** paths.

| Evidence file | Shows |
|---|---|
| `defect1_overlay_font_mismatch.png` | The active-edit blue box around "A Tale of Two Collards": overlay glyphs are **sans-serif** and **upright** — i.e. defect **1a** (family serif→sans) AND defect **1b** (slant italic→upright) simultaneously. |
| `defect2_overlay_geometry_offset.png` | The original serif-italic glyphs **peeking out from under** the mis-placed overlay box (overlay band offset vs the underlying span). |
| `defect3_new_char_wrong_family.png` | Right edge: the newly-typed `z` (the "…dz" tail beyond the overlay) renders **clearly slanted/italic** — slant is **correct** — with the family being the disputed fault (serif→sans per the user's live high-DPI view; the raster screenshot cannot resolve serif-vs-sans at this glyph size, which is itself an observability point — see §5). |

---

## 3. Code path each defect is believed to trace through

(From a read-only source survey. Locations are starting points for the
phase-2/3 root-cause work, not yet confirmed causes.)

### Defect 1a — overlay wrong FAMILY (serif→sans)
- Overlay font is built in `src/textedit/TextEditTool.cpp` **`buildOverlayFont()`
  (~line 460)**, family chosen via `mapPdfFontToQtFamily(strippedName)`
  (~476–489), with a name-substring serif/sans fallback at ~584–601 (matches
  "palatino").
- **DISCREPANCY TO RESOLVE IN PHASE 2:** statically, that path looks like it
  *should* land on a serif family ("Palatino Linotype" / serif fallback), yet
  the live render is sans. Competing hypotheses: (i) "Palatino Linotype" is not
  installed → Qt silently substitutes a sans default and the `looksWrong`
  guard fails to catch it; (ii) the serif/sans classifier (§2-classification)
  misfires on the subset; (iii) an `overlayOverride` / visual-ranker path is
  taken. Phase 2 must confirm with a metric/QFont readback.

### Defect 1b — overlay dropped ITALIC (italic→upright)
- `buildOverlayFont()` sets only `setPixelSize()` and family; it has **no
  `setItalic()` / `setStyle()` call**, whereas the sibling `buildRunFont()`
  (~642) does `font.setItalic(run.italic)`.
- `TextSpan` (`src/textedit/TextEditModel.h`) appears to carry **no `isItalic`
  field**, so the italic attribute detected by `EmbeddedFontAnalyzer`
  (`isItalic` / `italicAngle`, ~454–467) never reaches the overlay builder.
- This is the cleanest, most likely-confirmed defect: data present, path drops it.

### Defect 2 — overlay GEOMETRY offset
- Overlay is **painted directly** (no child widget): `pdfToScreen` lambda
  `src/textedit/TextEditTool.cpp` ~1214–1220 maps `pageOrigin + pdfRect*zoom`;
  baseline placed via `ascenderRatioForFont()` ~1362
  (`textY = top + topRatio*height`). Bbox source `span.boundingBox` (~1260),
  with an HOCR/atlas-adjusted branch ~1266–1279.
- Phase 3 must determine whether the offset is a **fixed-pixel** (constant) or
  **proportional/scale** error (cross-reference the evidence crop), and whether
  it stems from a baseline-vs-top assumption, the ascender-ratio, or a
  zoom/DPR mismatch.

### Defect 3 — new-char wrong FAMILY (italic correct)
- Missing-glyph path: `src/tre/AutoMissingGlyphResolver.cpp` —
  `parseFontStyle()` (~53–82) extracts bold/italic from the font name;
  `buildRunsFromMissingMap()` (~222–269) builds the `FormattingRun`; system
  fallback chosen via `FontFamilyResolver::resolve(family,bold,italic)`
  (~418–422). Serif-ness flows from the same `EmbeddedFontAnalyzer`
  classification as defect 1a.
- **OBSERVED-VS-STATIC DISCREPANCY (Phase 3 to resolve):** the read-only survey
  flagged that `run.italic` may not be set in `buildRunsFromMissingMap`, which
  would predict an *upright* new char — but the **live render is italic
  (correct)**, per the user and the screenshot. So either italic is applied on
  a later path the survey missed, or the survey misread the run construction.
  **Per the user's ground truth, the new char's italic is CORRECT** and must be
  preserved by any fix; the ONLY new-char fault is family (serif→sans). Phase 3
  must identify the family-selection decision point and confirm the italic path
  is genuinely correct (likely the subset font lacks PANOSE → serif classifier
  falls back to glyph-extent measurement and misclassifies the few subset glyphs
  as sans — the leading shared-root hypothesis below).

---

## 4. Do 1a and 3 share a root cause?

**Leading hypothesis: a shared UPSTREAM cause with distinct DOWNSTREAM paths.**
Both the overlay family choice (1a) and the new-char family choice (3) ultimately
depend on the serif/sans classification produced by
`EmbeddedFontAnalyzer` (PANOSE byte-1 primary, glyph-extent `serifExtent`
fallback at threshold 0.3). **Subset CID fonts commonly strip the OS/2 PANOSE
table**, which would force both consumers onto the glyph-extent fallback; on a
small subset that measurement can misclassify a serif as sans. That single
upstream misclassification would explain a serif→sans fault appearing in BOTH
the overlay and the new-char render.

However, the two consumers diverge downstream — overlay uses
`mapPdfFontToQtFamily` + Qt family substitution + a name-substring guard, while
new-char uses `FontFamilyResolver` — so it is also possible the faults have
*different* proximate causes that merely look identical on screen. Phases 2 and 3
must verify the classification output for this exact subset and decide
shared-vs-distinct with metric evidence.

Defect 1b (dropped italic) is **separate** and overlay-path-specific — the
new-char path preserves italic correctly, so 1b cannot share 3's mechanism.

---

## 5. Observability gap (contributing harness factor)

The only text-edit diagnostic seam is **font SIZE**:
`MainWindow::textEditSelectionDiag()` appends `;ovsz=<pt>`
(`TextEditTool::overlayFontSizePtOverride()`). There is **no readback of the
overlay's resolved font FAMILY or ITALIC state** over the testToolActivatePipe.
Consequently no automated test could observe the family/slant the app actually
chose — defects 1a/1b/3 were invisible to the harness at the data level, and the
raster render oracles never asserted serif-vs-sans or slant either (full
test-gap analysis is Phase 4). This absence satisfies the Phase-1
"document the absence of font-identity observability" criterion in lieu of a
live family readback (none exists to capture). A family+italic diagnostic seam
is a candidate addition in phase 6/7.

---

## 6. Summary

| # | Defect | Confirmed faults | Believed path | Determinism |
|---|---|---|---|---|
| 1a | Overlay wrong family | sans, should be serif | `buildOverlayFont` family/classification | deterministic |
| 1b | Overlay dropped italic | upright, should be italic (data present: ItalicAngle −10) | `buildOverlayFont` has no `setItalic`; `TextSpan` carries no italic | deterministic |
| 2 | Overlay geometry offset | underlay peeks out | `pdfToScreen` + `ascenderRatioForFont` baseline | deterministic |
| 3 | New char wrong family | sans, should be serif; **italic is CORRECT** | `AutoMissingGlyphResolver` family via `FontFamilyResolver`; shared serif classifier with 1a | deterministic |

All three reproduce deterministically for this file. Italic is recoverable from
the descriptor (defect 1b is a propagation bug, not missing data). The serif bit
is absent from `/Flags` (font is Symbolic), making the serif/sans classifier the
prime suspect shared between 1a and 3. No source changed in this phase.
