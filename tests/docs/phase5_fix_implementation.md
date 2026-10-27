# Phase 5 — Fix all three text-edit defects (C++) — pipeline b91f4e83

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
**Scope:** Implementation + live verification. First code-changing phase.
Fixes the three defects root-caused in Phases 2–3 on the target PDF title
"A Tale of Two Collards" (`BAAAAA+PalatinoLTPro-BoldItalic`, embedded subset
CIDFontType2/Identity-H, size 33, Flags=68 Symbolic+Italic, ItalicAngle −10,
OS/2 PANOSE bSerifStyle=4 → serif). Builds on the analysis docs
`rootcause_defect1_overlay_font.md`, `rootcause_defect2_geometry.md`,
`rootcause_defect3_new_char_font.md`, `test_gap_analysis_phase4.md`.

---

## 1. Code changes (3 files)

### Defect 1b — overlay drops italic + bold
**File:** `src/textedit/TextEditTool.cpp`, `buildOverlayFont` (after the
`QFont font(fontFamily); font.setPixelSize(pixelSize);` at ~:529).

**Root cause (Phase 2):** `buildOverlayFont` built the overlay QFont with
family + pixelSize only and **never** called `setItalic`/`setBold`, so the
overlay rendered the italic-serif title upright/regular. `buildRunFont`
(the per-run builder) *did* set them (:642–645); the base overlay font did
not. `TextSpan` carries no style field, so the descriptor-detected style was
dropped before the overlay builder. (Defect 1a — wrong *family* — was
DISPROVEN in Phase 2: the overlay family is already serif "Palatino
Linotype", and `buildOverlayFont` already has a serif-aware misresolution
fallback at :584–601.)

**Fix:** derive bold/italic from the source font name suffix (after the
subset `+` prefix) and apply `setBold`/`setItalic` on the overlay font.

**Why name-parse, not the raw /FontDescriptor flag:** `core::TextChar`
(`src/core/TextChar.h`) carries no style flags; plumbing descriptor flags
through `TextExtractor` → `TextChar` → every `TextSpan` build site is a large
change with real regression risk against the 4000+ baseline. The codebase
already derives style from the font name in
`AutoMissingGlyphResolver::parseFontStyle` — the **new-character path uses it
and gets slant+weight right** — so name-parse keeps the overlay and the
inserted-glyph paths in agreement. For this font the name suffix
("-BoldItalic"), the descriptor Flags (Italic bit) and head.macStyle
(italic+bold) all concur, so the observable result is identical to a
descriptor-driven read. Applied before the override early-return and the
serif/sans fallback; `QFont::setFamily` preserves bold/italic so the
fallback keeps them. For OCR pages (glyphless placeholder font names) the
suffix is absent → no behavioural change.

### Defect 2 — overlay geometry / baseline overshoot
**File:** `src/textedit/TextEditTool.cpp`, `onPaintOverlay` formatting-run
branch (~:1300–1311).

**Root cause (Phase 3):** the run branch placed the baseline at
`screenBbox.top() + maxAscent`, where `maxAscent` is the substitute QFont's
**leading-inflated pixel ascent** (40 px in the log) — larger than the em-box
white cover band height (33 × 1.15447 = 38.1 px). So the overlay dropped
to/below the band bottom and the original serif-italic glyphs peeked out
along the top. The sibling no-run branch (:1359–1362) was already corrected
to use the source-measured `ascenderRatioForFont(span.fontName) *
screenBbox.height()`; the run branch was never updated. Any new/substituted
character creates a formatting run → flips the overlay onto the un-fixed
baseline.

**Fix:** replace the `maxAscent` computation + `top + maxAscent` with the
identical source-measured baseline the no-run branch uses
(`top + ascenderRatioForFont(span.fontName) * screenBbox.height()`), so both
branches share one baseline convention. No change to `pdfToScreen`
(verified correct in Phase 3) or the bbox provenance.

### Defect 3 — newly-typed char gets sans family
**Files:** `src/fonts/FontFamilyResolver.cpp` (substitutes table) +
`src/tre/AutoMissingGlyphResolver.cpp` (ultimate fallback).

**Root cause (Phase 3):** for a char absent from the embedded subset (e.g.
'z'), `AutoMissingGlyphResolver` calls
`FontFamilyResolver::resolve("PalatinoLTPro", bold, italic)`, which returned
EMPTY (the substitutes table had no Palatino entry; the installed family is
"Palatino Linotype" ≠ "PalatinoLTPro"; no serif default). The code then fell
back to a hard-coded `resolve("Arial", …)` → a **sans** face. (Italic+bold
were already correctly preserved by `parseFontStyle` — left untouched per the
user's correction.)

**Fix (two layers):**
1. **`FontFamilyResolver` substitutes table:** add Palatino aliases
   (`PalatinoLTPro`, `Palatino LT Pro`, `Palatino Linotype`, `Palatino`,
   `Book Antiqua`) → the installed serif "Palatino Linotype" + free
   metric-compatible serif substitutes (Book Antiqua, URW Palladio L, P052)
   for Linux/CI. This resolves the *exact* correct installed face.
2. **Serif-aware ultimate fallback** in `AutoMissingGlyphResolver` (new
   `looksSerifFontName` helper, mirroring the serif-fragment list in
   `buildOverlayFont`): when the requested family is still unresolved, fall
   back to "Times New Roman" (serif) for a serif source instead of
   hard-coded sans Arial; Arial retained as the last resort. General safety
   net for other unmapped serif fonts. `parseFontStyle` (bold/italic)
   unchanged.

### Diagnostic (free runtime evidence)
- `buildOverlayFont` debug log extended with `bold=`/`italic=`.
- `AutoMissingGlyphResolver` logs the serif-aware fallback decision when the
  empty-fallback branch is reached.

---

## 2. Verification

### Build
Release, clean (`cmake --build --preset release-vs2022`). All three changed
TUs compiled; `pdfe.exe` + `pdfe_tests.exe` relinked, 0 errors.

### gtest (acceptance criterion 5)
Full Release suite: **4014 ran, 4011 passed, 0 failed, 3 skipped** (the 3
pre-existing XFA skips — skip count did NOT increase). Run twice; the only
blip was `MainWindowIntegrationTest.ZoomComboExists`, a **documented
non-deterministic flake** (toolbar QWidgetAction realization race under
full-suite load — `tests/docs/known_issues.md`, and the test's own comment
at `test_ui_integration.cpp:259`): it FAILED in run 1, PASSED in run 2 and
PASSED in isolation + with its whole suite (22/22). Unrelated to the font
code (no toolbar/zoom changes). The directly-affected suites
(`*Font*:*Overlay*:*TextEdit*:*Glyph*:*Resolver*:*VectorHeading*`) = 788/788.

### Live verification (acceptance criterion 6)
Drove the Release `pdfe.exe` on the target PDF via the deterministic
`PDFE_TEST_PIPES` seams (coordinate double-click + type — UIA-free).
Screenshots in `tests/fixtures/evidence/`:

| File | Shows |
|---|---|
| `postfix_defect1.png` | title overlay in **italic serif** (1b + 1a) |
| `postfix_defect2.png` | overlay band aligned between "Greens"/"Original Story", **no peeking** (2) |
| `postfix_defect3.png` | newly-typed **'z' in italic serif** (3) |
| `postfix_defect1_full.png`, `postfix_defect3_full.png` | full-canvas context |

**Runtime log proof** (`%LOCALAPPDATA%/PDFE/PDFE/pdfe.log`):
- `buildOverlayFont: family= "Palatino Linotype" … bold= true italic= true`
  (was: bold/italic never set) — defects 1a (serif) + 1b (italic+bold).
- `applyText: AutoMissingGlyphResolver produced 1 runs … text= "z"` — the
  new-char path ran; the serif-aware-fallback log did **not** fire because
  the new substitutes-table entry resolved `PalatinoLTPro` directly to the
  installed **Palatino Linotype** serif face (best outcome — exact face, no
  generic substitute needed). Visually confirmed serif+italic in
  `postfix_defect3.png`.

All six acceptance criteria met.
