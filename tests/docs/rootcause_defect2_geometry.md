# Phase 3 — Root Cause: Defect 2 (overlay geometry mis-alignment) — pipeline b91f4e83

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
**Scope:** Analysis only — NO production code changes. Grounded in the app's own
`onPaintOverlay` qDebug log (`%LOCALAPPDATA%/PDFE/PDFE/pdfe.log`, the user's
2026-06-28 10:47 edit session of the target title) plus a source trace of the
bbox provenance, not visual inspection alone.

**Headline:** The overlay's white cover band and its redrawn glyphs are placed
from `span.boundingBox`, an **em-box-tall** rectangle (height == font size) in
**fitz page space**. The vertical offset originates in the **baseline formula of
the formatting-run branch** of `onPaintOverlay` (`TextEditTool.cpp:1311`), which
sets the baseline to `screenBbox.top() + maxAscent` where `maxAscent` is the
**substitute QFont's leading-inflated pixel ascent**. That ascent (40 px in the
log) **exceeds the full screen height of the em box** (38.1 px), so the overlay
text drops to/below the bottom of the white cover band and the original
serif-italic glyphs peek out along the top of the band. The fault is
**proportional (scale-type)**, driven by the substitute font's ascent-to-em
ratio versus the source font's, amplified by zoom — not a fixed-pixel constant.

---

## 1. Coordinate-space provenance of the span bbox (which space, what scale)

Traced end to end:

1. **Source (fitz/MuPDF):** `TextExtractor::extractFromPage`
   (`src/core/TextExtractor.cpp:47-119`) builds the structured text from
   `fz_new_stext_page_from_page(ctx, fzPage, nullptr)` — **no transform passed**,
   so the stext geometry is in **fitz device space at scale 1.0 = PDF points,
   Y-down** (fitz origin is top-left, Y increases downward — the opposite of PDF
   user space's bottom-left Y-up).
2. Each glyph's `ch->quad` is reduced to an axis-aligned `QRectF` by
   `quadToQRectF` (`:31-42`, min/max of the four quad corners). `textChar.quad`
   therefore is the glyph's layout box in fitz points.
3. **Span bbox:** `TextEditModel` ctor (`src/textedit/TextEditModel.cpp:39-52`)
   sets `span.boundingBox = union of the per-char ch.quad` over the span's run.
   So `span.boundingBox` is **fitz page space (points, Y-down)**.
4. **To screen:** `onPaintOverlay`'s `pdfToScreen` lambda
   (`TextEditTool.cpp:1214-1220`):
   `x = pageOrigin.x + pdfRect.left*zoom`, `y = pageOrigin.y + pdfRect.top*zoom`,
   `w = pdfRect.width*zoom`, `h = pdfRect.height*zoom`. The comment at `:1213`
   ("both Y-down, no flip needed") is correct: fitz space and the canvas screen
   space share Y-down, and the same `pageOrigin`/`zoom` the canvas tile uses, so
   **the horizontal/vertical mapping of the band itself is correct** — there is
   no missing CTM component, no Y-flip error, and no separate DPI assumption in
   this lambda (it inherits the canvas zoom directly).

**Conclusion on space:** the bbox→screen *rectangle* mapping is sound. The
defect is **not** in `pdfToScreen`. It is in (a) what `span.boundingBox`
vertically represents and (b) the **baseline** at which the overlay glyphs are
drawn inside that rectangle.

### What the bbox vertically represents (em box, not ink box)

Runtime log for the target span:

```
[TextEdit] onPaintOverlay: zoom= 1.15447 span.bbox= QRectF(145.344,276.931 303.302x33)
   span.fontSize= 33 span.fontName= "BAAAAA+PalatinoLTPro-BoldItalic"
[TextEdit] onPaintOverlay font metrics: pixelSize_set= 38 pointSize_resolved= -1
   fm.height= 51 fm.ascent= 40 fm.descent= 11 ...
```

`span.bbox` height = **33 == fontSize**. The font's own metrics (Phase 1: Ascent
726, Descent −274 per 1000 em → 0.726 + 0.274 = 1.000 em) confirm the MuPDF char
quad is the **em box** (one em tall, top at the ascender line, bottom at the
descender line), placed about the glyph baseline. So vertically the band is the
em box; horizontally it is the union of glyph quads (advance width). It is **not**
the rendered-ink box and includes no padding for anti-aliasing or italic
side-bearing overhang.

---

## 2. Where the offset originates — baseline formula in the formatting-run branch

Editing the title to "A Tale of Two Collard**z**" injects a `FormattingRun` for
the new `z` (see `rootcause_defect3_new_char_font.md` — the resolver emits one
run). With `span.formattingRuns` non-empty, `onPaintOverlay` takes the
**formatting-run branch** (`TextEditTool.cpp:1298-1346`), whose baseline is:

```cpp
// TextEditTool.cpp:1300-1311
float maxAscent = 0.0f;
{
    QFontMetricsF baseFm(baseFont);
    maxAscent = static_cast<float>(baseFm.ascent());          // pixel ascent
    for (const auto& fr : span.formattingRuns) {
        QFont rf = buildRunFont(fr, span, zoom, ...);
        QFontMetricsF rfm(rf);
        maxAscent = std::max(maxAscent, static_cast<float>(rfm.ascent()));
    }
}
float textY = static_cast<float>(screenBbox.top()) + maxAscent;   // <-- fault
```

- `maxAscent` is the substitute QFont's **pixel ascent**, which Qt inflates with
  internal leading. The log probe shows **`fm.ascent = 40` px** for the overlay
  font at `pixelSize = 38`.
- `screenBbox.height() = 33 * zoom = 33 * 1.15447 = 38.1 px`.
- Therefore `textY = top + 40`, i.e. the baseline is **≈1.9 px below the bottom
  edge** of the 38.1-px white cover band. The overlay glyphs are drawn sitting on
  a baseline at/under the band's bottom, so the overlay caps land far lower than
  the original caps, which sit near the band's **top** (cap-top ≈ 0.06 em below
  em-top). The original glyphs are only masked within the 38.1-px band, so their
  upper portion is exposed **above** the displaced overlay text → "underlying
  text peeking out from under the overlay."

### This is the exact overshoot the *other* branch was already fixed for

The sibling **no-formatting-run branch** (`:1353-1363`) was deliberately
rewritten to avoid this very problem:

```cpp
// TextEditTool.cpp:1359-1362  (no-run branch — already corrected)
painter.setFont(baseFont);
float topRatio = ascenderRatioForFont(span.fontName);            // measured ratio
float textY = static_cast<float>(screenBbox.top())
              + topRatio * static_cast<float>(screenBbox.height());
```

Its comment (`:1353-1358`) states the fix verbatim: *"Position the baseline at the
SOURCE font's measured ascender ratio × bbox.height (not the substitute's
leading-inflated QFont ascent) … see the screenshot regression where Times's
ascent overshot CharisSIL's actual cap-top."* `ascenderRatioForFont`
(`:855-884`) returns the source font's measured baseline-from-top ratio
(default 0.75). For this span that yields `textY = top + 0.75 * 38.1 = top + 28.6`
— **11.4 px higher** than the formatting-run branch's `top + 40`, and inside the
band.

**The root cause is the inconsistency:** the no-run branch uses the
source-measured `topRatio * height`; the **formatting-run branch was never
updated** and still uses the raw, leading-inflated `maxAscent`. Any edit that
creates a formatting run (every new/substituted character — defect 3's path)
flips the overlay onto the un-fixed baseline formula and the geometry offset
appears.

---

## 3. Secondary contributor — tight em-box cover rect (no overhang padding)

`painter.fillRect(screenBbox, Qt::white)` (`:1294`) masks **only** the em box.
Two effects make the original visible even for a small baseline error:

- **Italic horizontal overhang:** the source face is italic (ItalicAngle −10);
  slanted glyph extremities (top-right, bottom-left) extend beyond the
  axis-aligned union-of-quads box, so their tips are never covered.
- **Anti-aliased edges:** the cover rect is pixel-tight to the em box; sub-pixel
  glyph fringes at the band edges survive.

This is a contributor (it makes any misplacement visible) but **not** the primary
cause; the dominant offset is the §2 baseline overshoot. A geometry fix must
address §2 (baseline) and may additionally pad the cover rect by a small margin.

---

## 4. Fixed-pixel vs proportional (cross-referenced to the evidence)

**Proportional / scale-type.** The offset is
`maxAscent − topRatio*height = QFontAscent(px) − sourceRatio*emHeight(px)`. Both
terms scale with `pixelSize`, which scales with `fontSize * zoom`. The error is
the *difference between two ratios* (substitute QFont ascent/em ≈ 40/38.1 ≈ 1.05
vs source measured ratio 0.75) times the em height in pixels — so it **grows with
font size and zoom**, and vanishes only if the substitute's leading-inflated
ascent ratio happened to equal the source ratio. It is **not** a constant pixel
offset. This is consistent with the Phase-1 screenshot
(`tests/fixtures/evidence/defect2_overlay_geometry_offset.png`): the 33-pt title
shows a large band offset; the offset would shrink proportionally on smaller
text. (A precise post-fix px delta will be measured live in Phase 5 against
`assert_overlay_alignment`, the Phase-7 oracle primitive.)

---

## 5. Call sites (summary)

| Concern | Location | Issue |
|---|---|---|
| bbox space | `TextExtractor.cpp:47-119`, `TextEditModel.cpp:39-52` | fitz page space (points, Y-down), em-box tall — correct, but tight |
| rect→screen | `TextEditTool.cpp:1214-1220` (`pdfToScreen`) | correct; no DPI/CTM/flip error |
| **baseline (run branch)** | **`TextEditTool.cpp:1311`** | **`top + maxAscent` (leading-inflated px ascent) overshoots band bottom — PRIMARY** |
| baseline (no-run branch) | `TextEditTool.cpp:1359-1362` | already correct (`top + topRatio*height`); the model the run branch should follow |
| cover rect | `TextEditTool.cpp:1294` | tight em box, no overhang/AA padding — secondary |

---

## 6. Answers to Phase-3 acceptance questions (defect 2)

1. **Functions computing overlay position/size; coordinate space; DPI/scale;
   origin of offset:** §1 + §2. Space = fitz page space (PDF points, Y-down),
   em-box height; scale = canvas `zoom` (no extra DPI factor in the mapping
   lambda). Offset originates **not** in the rect mapping but in the
   **formatting-run branch baseline** `screenBbox.top() + maxAscent`
   (`TextEditTool.cpp:1311`) using the substitute's leading-inflated pixel ascent
   instead of the source-measured `ascenderRatioForFont(...) * height` the no-run
   branch uses — a **baseline-vs-top + ascent-overshoot** confusion, not a missing
   CTM or HiDPI mismatch.
2. **Fixed-pixel or proportional:** **proportional (scale-type)** — §4 —
   cross-referenced to `defect2_overlay_geometry_offset.png`.

---

## 7. Impact on the plan

- Phase-5 defect-2 fix: make the **formatting-run branch** use the same
  source-measured baseline as the no-run branch (`screenBbox.top() +
  ascenderRatioForFont(span.fontName) * screenBbox.height()`), so a single
  baseline convention covers both branches; optionally pad the white cover rect
  for italic/AA overhang. No change to `pdfToScreen` or the bbox provenance.
- Phase-7 oracle: `assert_overlay_alignment(overlay_img, underlay_img,
  max_offset_px)` must capture overlay-over-underlay and measure the band offset
  — the gap that let this through (geometry tests measured the overlay in
  isolation) is covered in `test_gap_analysis_phase4.md`.
