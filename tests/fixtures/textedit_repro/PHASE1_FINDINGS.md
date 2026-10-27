# Phase-1 Findings — Lean Startup page-2 text edit bugs

**Status:** PARTIAL. Save-path captured; live-overlay captured; **undo (symptom C) NOT
captured** in this run; **page-1 comparison NOT done**; **screenshots not collected**.

## Repro source

- PDF: `C:\Users\dev\Desktop\The Lean Startup - Erick Ries.pdf`, page 2
- Edit target span: line 9 of the page, original text starts with U+201C +
  `"Eric Ries unxravels the mysteries of entrepreneurship and reveals"`
- Build under test: `build/vs2022/bin/Debug/PDFE.exe` (matches Qt6Svgd dated 2026-04-27)

## Captured artifacts

- `page2_save_run_2026-04-27.log` — extracted lines 184586–184760 from
  `%LOCALAPPDATA%\PDFE\PDFE\pdfe.log`. Contains two save attempts:
  - 16:00:08 — first save, hits `afterTextObjectCallback` (no font substitution path).
  - 16:12:59 — second save, hits `afterTextObjectWithSubstitutionCallback` with
    full encode + writeTm + preTj instrumentation.

## Key log values (annotated)

### Live overlay (symptom A)

```
[TextEdit] buildOverlayFont: family="CharisSILRegular" span.fontName="CharisSILRegular"
  span.fontSize=12.48 fontSizeOverride=0 spanHasRealFont=false
  baseSizePt=12.48 zoom=2.84894 pointSize=26 overlayOverride=""
```

- `spanHasRealFont=false` because `hasSubsetPrefix("CharisSILRegular") = false` —
  there is **no `XXXXXX+` subset prefix**, so the code treats the font as if it
  is *not* a real embedded font, even though `CharisSILRegular` IS a real
  embedded font in the source PDF.
- `fontSizeOverride=0` — no `overlayFontSizePtOverride_` is wired in for this
  page, so `baseSizePt = span.fontSize = 12.48`.
- `pointSize = (int)(12.48 × 2.84894 × 0.75) = 26` — the live overlay is
  rendered at QFont pointSize 26.

### Save path (symptom B)

```
[TEXTEDIT_REPRO] textFilter match: trm=[26,0,0,-26,20,200]
  ctm=[0.48,0,0,-0.48,0,350.68] fontSize=12.48
  fontName="CharisSILRegular" newTextLen=66
[TEXTEDIT_REPRO] encode: wasMultiByte=true fontResKey="Fsub1"
  srcU32="[U+201c]Eric Ries unxravels..."
  encodedBytes(hex)=00 94 00 08 00 51 00 48 00 42 00 03 00 15 00 48 ...
[TEXTEDIT_REPRO] writeTm: fontSize=12.48
  tm_with_size=[54.1667,0,0,54.1667,41.6667,313.917]
  Tm_emitted=[4.34028,0,0,4.34028,41.6667,313.917]
[TEXTEDIT_REPRO] preTj: buf=0x1c9b095a660 len=132 hex=00 94 00 08 00 51 ...
```

- `trm.a = 26`, `ctm.a = 0.48` → original glyphs render at 26 device-points (after
  CTM compose). For the `bbox.height = 12.48` span, this means the original text
  was authored with `Tfs × Tm.a = 26/0.48 = 54.17` and the visible em-height on
  page is 26 device units, of which the cap-height crops to ~12.48 user-space
  points (the bbox.h).
- The rewrite computes `tm_with_size = trm × ctm⁻¹ = [54.17, 0, 0, 54.17, ...]`
  and divides by `inj.fontSize = 12.48`, producing `Tm_emitted.a = 4.34`. When
  reapplied with the surrounding `q ... 0.48 ... cm ... Q` scope, this yields
  `12.48 × 4.34 × 0.48 = 26` device units — same as the original. So the
  *math is consistent in absolute terms*. The on-page rendered glyph height of
  the rewritten text MUST come out to 26 device-points provided the q…Q scope
  is preserved by `pdf_filter_page_contents`.
- **Encode path:** `fontResKey="Fsub1"` indicates the substitution path swapped
  /CharisSILRegular for /Fsub1 (a CID font, multi-byte → all `00 NN`-style 2-byte
  codes). 'E' (U+0045) → CID 0x0008. 'r' (U+0072) → CID 0x0051. These are valid
  2-byte CIDs IF /Fsub1's CMap maps them correctly; if /Fsub1 is registered as a
  single-byte simple font in the resource dict, they would render as 12 nulls +
  garbage. **Cannot confirm from the log alone**; phase-2 must inspect the saved
  PDF's `/Fsub1` Subtype + CMap.

### Routing decision

```
[TextEdit] applyText: page=2 isImageDominated=false span.fontName="CharisSILRegular"
[TextEdit] textEdited: isImageDominated=false
```

- **`Page::isImageDominated()` returns FALSE for page 2.** It checks whether
  image blocks cover >50% of page area AND text is present
  (`src/core/Page.cpp:237`). Page 2 of Lean Startup evidently does NOT meet
  the >50% image-area threshold.
- **This contradicts the task description**, which assumes page 2 is
  "image-dominated/scanned page routed through TRE image-replacer." The page
  has a `cm` operator pushing CTM = [0.48, 0, 0, -0.48, 0, 350.68] (visible in
  textFilter trm/ctm), but that is a *standard scaled XObject layout* (e.g., a
  chapter-quote pull-quote at half scale), not an image-replacer wrapper.
- Save path is therefore `ContentStreamEditor` (not `ImageReplacer`).
- Phase-2 implication: the symptoms are NOT in image-replacer code paths.
  They are in the **plain ContentStreamEditor save path operating inside a
  scaled-CTM scope**.

## Symptom C (Ctrl+Z) — NOT captured

The user's run did not press Ctrl+Z after the saves. No `EditTextCommand::undo`
or related events appear in the log between 16:00:08 (first save) and 16:13:09
(last edit). Phase-1 acceptance therefore has a known gap: symptom C cannot be
classified in phase-2 without an additional repro run that triggers Ctrl+Z and
captures the resulting overlay/render state.

## Page-1 scope check — NOT captured

The user did not click into page 1 with the text-edit tool active in this
session. Whether page 1 of Lean Startup reproduces the same symptoms is
unknown. Phase-2 should keep this open as "scope unverified" and either
request a follow-up repro or examine the page 1 content stream offline to
determine whether it has the same scaled-CTM layout block.

## Screenshots — NOT captured

Logs only. The visual evidence the user reported (oversized overlay,
upside-down/garbled save, upside-down ghost on undo) is described in the task
prompt but not screenshotted in this fixture set.

## What phase-2 needs

To complete root-cause analysis, phase-2 should:
1. Inspect the actual saved PDF (the `Save As…` output from the second run at
   16:12:59) — needed to confirm whether `/Fsub1` is a CID font or a simple
   font in the rewritten resource dict, and to confirm whether the q…Q scope
   around the rewritten BT…ET is preserved.
2. Run an additional repro that exercises Ctrl+Z so symptom C has a log trail.
3. Spot-check page 1 with the text-edit tool to scope page-2-specific vs. all
   pages with scaled-CTM blocks.

## Notes for phase-3 instrumentation cleanup

The TEXTEDIT_REPRO `qDebug()` calls live in
`src/textedit/ContentStreamEditor.cpp` at lines 340–355 (textFilter), 454–470
(non-substitution writeTm), 554–574 (encode), 612–628 (substitution writeTm),
and 642–649 (preTj). All are tagged with `[TEXTEDIT_REPRO] Phase-1 diagnostic`
in their preceding comment. Phase-3 should either delete them entirely or
convert to a permanent guarded log category once the root cause is fixed.
