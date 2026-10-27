# Phase 3 — Root Cause: Defect 3 (new-char wrong family) — pipeline b91f4e83

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
**Scope:** Analysis only — NO production code changes. Grounded in a full source
trace of `AutoMissingGlyphResolver::resolve` and `FontFamilyResolver::resolve`,
plus the app's own log line confirming the resolver fired for the new char.

**Headline:** Typing a character absent from the embedded subset (e.g. `z` into
"A Tale of Two Collards") routes through `AutoMissingGlyphResolver` →
**system-font fallback** → `FontFamilyResolver::resolve("PalatinoLTPro",
bold=true, italic=true)`. That resolver has **no name mapping** from the PDF
family name "PalatinoLTPro" to the installed serif "Palatino Linotype" and **no
serif default**, so it returns empty; `AutoMissingGlyphResolver` then falls back
to a **hard-coded `resolve("Arial", …)`** (`AutoMissingGlyphResolver.cpp:422`).
Arial is **sans** — hence the wrong family — but the bold/italic flags are
correctly carried through to Arial, so the new glyph is **sans, italic, bold**,
exactly matching the user's observation that *the slant is correct and only the
family is wrong*. This is a **distinct mechanism from the overlay** (defect 1),
which uses a different resolver (`mapPdfFontToQtFamily`) that *does* map
palatino→serif — so the Phase-1 "shared family root cause 1a==3" hypothesis is
**refuted** (defect 1a does not exist; the genuine sans fault is defect 3 alone).

---

## 1. The path a new character takes

`MainWindow` calls `AutoMissingGlyphResolver::resolve(context, page, document,
sourceFontName="BAAAAA+PalatinoLTPro-BoldItalic", sizePt, text="A Tale of Two
Collardz")`. Confirmed firing in the log
(`tests/fixtures/evidence/defect1_overlay_font_selected.txt`):

```
[TextEdit] applyText: AutoMissingGlyphResolver produced 1 runs for
   unencodable codepoints in text= "A Tale of Two Collardz"
```

Trace (`src/tre/AutoMissingGlyphResolver.cpp`):

1. **Extract embedded subset bytes** (`:287`, `extractEmbeddedFontBytes`
   `:90-174`): loads the FontFile2 for `BAAAAA+PalatinoLTPro-BoldItalic`.
2. **Find unencodable codepoints** (`:305`, `findUnencodable` `:201-216`): the
   original title has no `z`, so the subset cmap has no glyph for `z` →
   `FT_Get_Char_Index == 0` → `z` is collected as missing.
3. **Partition by reason** (`:325-333`): `z` has `gid == 0` → it is **absent**
   (`absentCps`), *not* stripped. (The `glyphOutlineIsEmpty` branch — the
   pipeline-e7d3c2a1-P7 empty-outline check — handles `gid != 0` glyphs whose
   outline was removed; `z` never reaches it. See §4.)
4. **Recipe check** (`:340-346`, `GlyphSynthesizer::findRecipe`): the synthesis
   recipe table is for composed Latin accented letters (base + diacritic). Plain
   `z` has **no recipe** → `anyRecipe == false`.
5. **EnrichedAtlas / Type-3 path BYPASSED** (`:369-372`): because `anyRecipe`
   is false, the atlas is never built and every absent codepoint is pushed to
   `sysCps`. So `z` goes to the **system-font fallback**, not Type-3 synthesis
   (see §4).
6. **System-font resolution** (`:416-447`) — the decision point for the family:

```cpp
// AutoMissingGlyphResolver.cpp:417-422
static pdfe::fonts::FontFamilyResolver resolver;
ParsedFontStyle style = parseFontStyle(sourceFontName);
std::string sysPath = resolver.resolve(style.family, style.bold, style.italic);
if (sysPath.empty())
    sysPath = resolver.resolve("Arial", style.bold, style.italic);   // <-- sans fallback
```

7. **Run emission** (`:450`, `buildRunsFromMissingMap`): the resolved
   `sysPath` becomes `FormattingRun.fontFilePath` for the run covering `z` — the
   single run the log reports.

---

## 2. The exact decision point that selects a sans family

`parseFontStyle("BAAAAA+PalatinoLTPro-BoldItalic")`
(`AutoMissingGlyphResolver.cpp:53-82`):
- `stripSubsetPrefix` → `"PalatinoLTPro-BoldItalic"`.
- `lower` contains "bold" and "italic" → `bold = true`, `italic = true`
  (**style correctly detected** — this is why the new glyph is correctly italic).
- family = substring before the `-` that introduces a style word →
  **`"PalatinoLTPro"`**.

`FontFamilyResolver::resolve("PalatinoLTPro", true, true)`
(`src/fonts/FontFamilyResolver.cpp:13-52`):
- **candidates** = `["PalatinoLTPro"]` + `metricCompatibleSubstitutes("PalatinoLTPro")`.
  The substitutes table (`:62-84`) has entries for Arial/Times New Roman/Courier
  New/Calibri/Cambria/Liberation*/etc. — **no "Palatino", "PalatinoLTPro", or
  "Palatino Linotype" key** → substitutes list is **empty**.
- candidates = `["PalatinoLTPro"]` only.
- `systemFonts_.findByFamilyStyle("PalatinoLTPro", Bold, Italic)`: the installed
  font family is **"Palatino Linotype"** (`pala*.ttf`), *not* "PalatinoLTPro" —
  the family-name strings do not match → **no result**.
- `filePath` stays empty, is negative-cached, and **`resolve` returns ""**.

Back in `AutoMissingGlyphResolver` (`:421-422`): `sysPath.empty()` is true, so the
**hard-coded `resolver.resolve("Arial", true, true)`** runs. "Arial" *is* in the
substitutes table and Arial Bold Italic *is* installed on Windows → `sysPath =
arialbi.ttf`. **Arial is sans-serif.** The run for `z` is therefore Arial Bold
Italic → **sans family, correct slant, correct weight**.

**Two compounding faults at this decision point:**
- **(F1) No family-name normalization.** `FontFamilyResolver` is fed the raw PDF
  family token "PalatinoLTPro" and has no alias mapping to the installed
  "Palatino Linotype". (Contrast the overlay path's `mapPdfFontToQtFamily`,
  `TextEditTool.cpp:401`, which *does* map `"palatino"` → `"Palatino Linotype"`
  via partial substring match — see §3.)
- **(F2) Sans default fallback.** When the requested family can't be resolved,
  the fallback is a **hard-coded sans ("Arial")** regardless of whether the
  source font is serif. There is no serif-vs-sans branch on the source font's
  classification at this site.

Either fault alone produces the wrong family; both are present.

---

## 3. Is this in scoring/ranking, the metric distance function, or classification surfacing?

**None of those.** Defect 3 is **not** in `SubstituteFontSelector` scoring, **not**
in `FontMetricProfile` distance, and **not** in how `EmbeddedFontAnalyzer`'s
serif classification is surfaced. The new-char path does **not** consult any of
them — it goes straight through `FontFamilyResolver`, which does a **literal
family-name + style lookup** against `SystemFontDiscovery` with a small static
substitute table, and falls back to a hard-coded "Arial" string. The
serif/sans classification of the embedded font is **never read** on this path,
so the resolver has no way to prefer a serif fallback. The fault is a
**name-resolution + fallback-default** fault, located entirely in
`FontFamilyResolver::resolve` (`FontFamilyResolver.cpp:13-52`) and the
`AutoMissingGlyphResolver` Arial fallback (`AutoMissingGlyphResolver.cpp:422`).

**Relationship to defect 1 (overlay):** distinct mechanism, distinct code.
- Defect 3 (new char): `AutoMissingGlyphResolver` → `FontFamilyResolver` (no
  palatino mapping, sans default) → **Arial sans**.
- Defect 1 overlay family: `buildOverlayFont` → `mapPdfFontToQtFamily`
  (`"palatino"` → "Palatino Linotype") → **serif, correct** (Phase 2 proved the
  overlay family is correct; its real faults are dropped italic/bold).

The Phase-1 hypothesis that 1a and 3 share one upstream serif-classification
root cause is therefore **refuted**: 1a does not exist, and 3's mechanism is a
resolver that never classifies serif at all. The italic *drop* (1b) is also
unrelated — this path **preserves** italic (it is the overlay that drops it).

---

## 4. Type-3 synthesis vs system fallback; and the empty-outline (P7) check

- **Where the sans selection happens:** in the **system-font fallback**
  (`AutoMissingGlyphResolver.cpp:416-447`, specifically the Arial fallback at
  `:422`) — **not** in the EnrichedAtlas / Type-3 synthesis path.
- **Is Type-3 synthesis triggered or bypassed?** **Bypassed** for `z`:
  `GlyphSynthesizer::findRecipe('z')` returns null → `anyRecipe == false` →
  `VectorEnrichedAtlasBuilder::build` is never called (`:352`), the atlas is
  never built, and `z` is routed to `sysCps` (`:369-372`). The synthesis path
  only engages for absent codepoints with a composition recipe (accented Latin).
- **Is the pipeline-e7d3c2a1-P7 empty-outline check relevant?** **No — distinct
  code path.** That fix (`glyphOutlineIsEmpty`, `:189-193`, used at the
  partition step `:329`) handles a **stripped** glyph: `gid != 0` but the subset
  removed the outline (D8-1: an edited bold heading's `u` rendered blank). `z`
  here is **`gid == 0` (genuinely absent)** — it is classified via the
  `gid == 0` branch and never touches `glyphOutlineIsEmpty`. The empty-outline
  check correctly routes *stripped* glyphs to the same system fallback, so it
  would hit the *same* `FontFamilyResolver` sans-default bug downstream — but the
  trigger for *this* defect is the absent-codepoint branch, and the P7 check is
  not the cause.

---

## 5. Confirmation that italic IS correctly carried (locks correct behaviour)

The new glyph's slant is **correct** and must be preserved by any Phase-5 fix:
- `parseFontStyle` sets `italic = true` from the "Italic" token in the name
  (`:61-62`) and `bold = true` from "Bold" (`:60`).
- Both flags are passed into `FontFamilyResolver::resolve(family, bold, italic)`
  at **both** call sites (`:420` and the Arial fallback `:422`), which maps them
  to `FontStyle::Italic` / `FontWeight::Bold` and selects the italic-bold face
  file (`FontFamilyResolver.cpp:24-25,41`).
- Result: Arial **Bold Italic** (`arialbi.ttf`) — sans, but italic+bold. This is
  exactly the user's report (slant correct, family wrong) and is the ground truth
  the regression tests will lock: **family must change to serif; italic+bold must
  stay.**

---

## 6. Answers to Phase-3 acceptance questions (defect 3)

1. **Exact path + decision point causing sans selection:** §1 (path) + §2
   (decision point) — `AutoMissingGlyphResolver::resolve` system-font fallback,
   `FontFamilyResolver::resolve("PalatinoLTPro", …)` returns empty (no alias to
   "Palatino Linotype", no serif default), then the hard-coded
   `resolve("Arial", …)` at `AutoMissingGlyphResolver.cpp:422` picks **sans**.
   Italic IS carried through (§5).
2. **Scoring / metric-distance / classification-surfacing?** None — it is a
   name-resolution + sans-default fault in `FontFamilyResolver`; the embedded
   font's serif classification is never consulted on this path (§3).
3. **Type-3 synthesis triggered or bypassed; where sans selection occurs:**
   bypassed (no recipe for `z`); sans selection is in the **system-font
   fallback**, not synthesis (§4).
4. **Is the e7d3c2a1-P7 empty-outline check relevant?** No — distinct path; `z`
   is absent (`gid == 0`), not stripped (§4).

---

## 7. Impact on the plan

- Phase-5 defect-3 fix targets the family decision only:
  (a) normalize the PDF family name before resolving (map "PalatinoLTPro" /
  "Palatino*" → "Palatino Linotype", reusing the overlay's `mapPdfFontToQtFamily`
  logic or adding a Palatino row to the substitutes table), and/or
  (b) make the ultimate fallback serif-aware (choose a serif default when the
  source font is classified serif — e.g. via `EmbeddedFontAnalyzer`/the name —
  instead of unconditionally "Arial").
  **Leave `parseFontStyle`'s bold/italic handling untouched** — it is correct.
- Phase-6 test: `AutoMissingGlyphResolver` resolves a char absent from the
  italic-serif subset to a **serif** face that is **also italic** — the two
  attributes asserted independently (criteria 6c/6d), so a future regression that
  fixes family but drops italic (or vice-versa) is caught.
