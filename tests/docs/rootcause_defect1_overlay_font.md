# Phase 2 — Root Cause: Defect 1 (overlay font) — pipeline b91f4e83

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
**Scope:** Analysis only — NO production code changes. Confirmed with runtime
diagnostic evidence (the app's own `qDebug` log), not visual inspection alone.

**Headline correction (evidence-driven):** Phase 1 characterized defect 1a as
"overlay renders in a SANS family." **Runtime evidence refutes that.** The
overlay family resolves to **"Palatino Linotype" (serif, correct)**. The
overlay's real faults are **dropped italic (1b)** and **dropped bold/weight**
(a previously-unlisted fault with the same root cause). The genuine sans /
wrong-family fault lives in the **new-char path (defect 3)**, not the overlay —
so the Phase-1 "shared family root cause between 1a and 3" hypothesis is
**refuted** for the overlay.

---

## 1. Runtime diagnostic evidence (metric-level, not visual)

The app already logs the overlay font decision (`TextEditTool.cpp:532`). The
user's live edit session of the target title (`pdfe.log`, 2026-06-28 10:47:29 —
the same session as the 10:34 screenshot) recorded:

```
[TextEdit] buildOverlayFont: family= "Palatino Linotype"
   span.fontName= "BAAAAA+PalatinoLTPro-BoldItalic" span.fontSize= 33
   fontSizeOverride= 0 spanHasRealFont= true baseSizePt= 33 zoom= 1.15447
   visibleExtentRatio= 1 ... pixelSize= 38 overlayOverride= ""
[TextEdit] onPaintOverlay font metrics: pixelSize_set= 38 pointSize_resolved= -1
   fm.height= 51 fm.ascent= 40 fm.descent= 11 ... overlayWordStyles= 0
[TextEdit] applyText: AutoMissingGlyphResolver produced 1 runs for
   unencodable codepoints in text= "A Tale of Two Collardz"
```

Saved verbatim at `tests/fixtures/evidence/defect1_overlay_font_selected.txt`.

**Actual vs expected (overlay):**

| Attribute | Source span (from /FontDescriptor + font tables) | Overlay actually used | Verdict |
|---|---|---|---|
| Family class | serif (Palatino; PANOSE bSerifStyle=4; name "Palatino") | **"Palatino Linotype" (serif)** | **MATCH — not a defect** |
| Slant | italic (ItalicAngle −10; macStyle italic bit; Flags bit7) | **upright** (`overlayWordStyles=0`, no `setItalic`) | **MISMATCH → defect 1b** |
| Weight | bold (StemV 281; macStyle bold bit; name "Bold") | **regular** (no `setBold`) | **MISMATCH → dropped-bold (new)** |
| Size | 33 pt | 33 pt → 38 px @ zoom 1.154 | MATCH |

The `family= "Palatino Linotype"` reading is decisive: there is **no sans
substitution** in the overlay path. Palatino Linotype regular-upright, viewed at
screen scale over the bold-italic original, reads as a plain/“wrong” face — which
is what the low-resolution Phase-1 screenshot crop was misread as "sans."

---

## 2. Defect 1a (claimed overlay family fault) — DISPROVEN

`TextEditTool::buildOverlayFont` (`src/textedit/TextEditTool.cpp:460-603`) for
this span:

1. `spanHasRealFont = hasSubsetPrefix("BAAAAA+...") && fontSize>4` → **true**
   (`:470`; `hasSubsetPrefix` `:103-110`). With a real embedded font, the
   `overlayOverride` (FontMatcher/OCR) branch at `:472` is **skipped** —
   confirmed by `overlayOverride= ""` and `spanHasRealFont= true` in the log.
2. Strip subset prefix → `"PalatinoLTPro-BoldItalic"` (`:477-481`).
3. `mapPdfFontToQtFamily` (`:483`, table+partial-match `:438-454`): normalized
   key `"palatinoltprobolditalic"` partial-matches table key `"palatino"` →
   **`"Palatino Linotype"`** (`:401`).
4. `QFontInfo` validation (`:555-582`): "Palatino Linotype" is installed
   (`pala.ttf`/`palab.ttf`/`palai.ttf`/`palabi.ttf` present), so
   `resolvedFamily` contains the mapped name → `looksWrong=false` → the
   serif/sans name fallback at `:584-601` is **not** taken.

**Conclusion:** the overlay family selection is correct and robust for this
font. There is no family-selection bug in the overlay path. **Defect 1a, as
written ("wrong sans family"), does not exist in the overlay.** (The fallback at
`:584-601` would itself pick `PDFE_FAMILY_SERIF` for "palatino" anyway, so even
the error path is serif-safe.)

---

## 3. Defect 1b (dropped italic) — CONFIRMED + root cause

- **Exact fault site:** `src/textedit/TextEditTool.cpp:529-530`
  ```cpp
  QFont font(fontFamily);
  font.setPixelSize(pixelSize);
  ```
  The overlay `QFont` is built with family + pixel size **only**. There is **no
  `setItalic()` / `setStyle()` (nor `setBold()`) anywhere in `buildOverlayFont`
  (`:460-603`).** The font returned is therefore regular-upright regardless of
  the source span's style.
- **Why the data never arrives:** `struct TextSpan`
  (`src/textedit/TextEditModel.h:78-109`) stores `fontName` and `fontSize` but
  has **no `isItalic` / `isBold` / weight field**. The italic/bold attributes are
  detected upstream by `EmbeddedFontAnalyzer` (FreeType `FT_STYLE_FLAG_ITALIC`
  from `head.macStyle`, and `post.italicAngle = −10` → `isItalic`; bold from
  weight/StemV) but are **dropped before the span reaches the overlay builder**.
  `buildOverlayFont` also never parses "Bold"/"Italic" out of the `fontName`
  string (it uses the name only for the family-table lookup).
- **Proof the path is capable of style:** the sibling `buildRunFont`
  (`:606-660`) applies `font.setBold(run.bold); font.setItalic(run.italic); …`
  (`:642-645`) from a `FormattingRun` (which HAS `bold`/`italic`,
  `TextEditModel.h:43-54`). The base overlay path is the only one missing it,
  and `overlayWordStyles= 0` in the log shows no formatting runs are active
  during plain overlay edit — so the style-less `buildOverlayFont` output is
  exactly what paints (`painter.setFont(baseFont)` at `:1296/:1377`).

**Classification:** this is an **overlay-wiring / data-model fault**, not a
classification or metric-matching fault. Two viable fixes (Phase 5): (a) add
`isItalic`/`isBold` (or a style enum) to `TextSpan`, populate it where spans are
built from the PDF, and apply it in `buildOverlayFont`; or (b) have
`buildOverlayFont` parse Bold/Italic from the (already-available) `fontName`
suffix the same way `AutoMissingGlyphResolver::parseFontStyle` does
(`src/tre/AutoMissingGlyphResolver.cpp:53-82`) and call `setItalic/setBold`.
Option (a) is more correct (descriptor-driven); (b) is a smaller change that
reuses an existing parser. Decision deferred to Phase 5.

---

## 4. Dropped bold/weight — newly identified, SAME root as 1b

The source font is **Bold** (CIDFont name "…-Bold…", `/StemV 281`, macStyle bold
bit). `buildOverlayFont` never calls `setBold()` (`:529-530`), and `TextSpan`
carries no weight, so the overlay is **regular weight**. This is the same
root cause as 1b (style dropped between descriptor and overlay) and is, visually,
the **dominant** difference (a bold→regular shift is more obvious than the −10°
slant). Any Phase-5 fix for 1b must restore weight at the same time; treating
italic in isolation would leave a visibly wrong (still un-bold) overlay.

---

## 5. /FontDescriptor data usage (family vs slant, separately)

- **Family:** the overlay path does **not** consult `/FontDescriptor` at all; it
  derives family from the font **name** via the mapping table. (This happens to
  be correct here.) The `/Flags` Serif bit being absent — see Phase 1 — is
  therefore irrelevant to the overlay family decision.
- **Slant/weight:** `/FontDescriptor` data (`ItalicAngle −10`, Flags Italic bit,
  Ascent/Descent, and the font's `head.macStyle`/`post` tables) **is read** by
  `EmbeddedFontAnalyzer` but is **ignored by the overlay path** because it is not
  propagated into `TextSpan`. So the data exists and is even computed; it simply
  never reaches `buildOverlayFont`.

---

## 6. Call site summary

- Overlay `QFont` constructed: `src/textedit/TextEditTool.cpp:529-530`.
  - Attributes SET: family (ctor, from name→table mapping), pixel size.
  - Attributes NOT set: **italic, bold, style, weight**.
- Applied to the canvas: `painter.setFont(baseFont)` in `onPaintOverlay`
  (`~:1296`, `~:1377`), where `baseFont = buildOverlayFont(span, …)`.

---

## 7. Answers to Phase-2 acceptance questions

1. **Family fault (1a) location:** none — runtime proves family resolves to
   serif "Palatino Linotype". The premise is disproven; documented in §2.
2. **Is 1a in classification / metric / wiring, and same root as defect 3?**
   Neither — there is no overlay family fault. **Not** the same as defect 3;
   defect 3's sans fault is in the new-char resolver path (Phase 3), a different
   code path. The Phase-1 shared-root hypothesis is refuted for the overlay.
3. **Slant fault (1b) location:** `TextEditTool.cpp:529-530` (no `setItalic`),
   root-enabled by `TextSpan` carrying no italic field (`TextEditModel.h:78-109`).
4. **Is descriptor data read/used or ignored — family vs slant separately:**
   family = descriptor not used (name-based, happens to be correct); slant/weight
   = descriptor read by `EmbeddedFontAnalyzer` but ignored by the overlay path
   (dropped at `TextSpan`). §5.
5. **Overlay font construction call site + attributes set/not set:** §6.
6. **Confirmed by diagnostic evidence vs visual only:** yes — the `qDebug`
   `buildOverlayFont`/`onPaintOverlay` log lines (§1), captured to
   `tests/fixtures/evidence/defect1_overlay_font_selected.txt`. No production
   code changed; the diagnostics were already present in the shipping code.

---

## 8. Impact on the plan

- Phase 5 fix for "defect 1" should be re-scoped from "fix overlay family" to
  **"restore overlay italic AND bold/weight"** (a single style-propagation fix).
  The family needs no change.
- Defect 3 (new-char sans family) remains the genuine family fault and is
  root-caused separately in Phase 3.
- Phase 6/7 oracles should assert overlay **slant** and **weight** match the
  source (not family alone), and the new-char **family** (serif) with preserved
  italic — consistent with the corrected picture.
