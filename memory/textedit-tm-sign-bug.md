# Phase-2 root-cause analysis

## What the saved PDF actually contains

**File:** `C:\Users\dev\Desktop\Edited - The Lean Startup - Erick Ries.pdf`

**Diff against the original:** exactly **one** page differs — physical page 3
(0-based index 2 in the log, "page 2" in PDFE's UI which counts the title as
page 1). Every other page is byte-identical.

**Page-3 fonts after save:** `/F7` (the original `CharisSILRegular` Type0/Identity-H,
subset prefix `QBBAAA+`) AND a newly added `/Fsub1` (`Nirmala UI Semilight`,
also Type0/Identity-H, fully embedded — 1.5MB of TrueType, no `/CIDToGIDMap`).

**The inserted BT block in the saved content stream** (verbatim):

```
ET                ← end of preceding /F7 26 Tf BT
q                 ← push gs (sanitize-filter wrapper)
Q                 ← pop (immediate, no-op)
q                 ← push gs (wraps our insertion)
BT/Fsub1 12.48 Tf
4.3402779 0 0 4.3402779 41.666666 313.91667 Tm
<009400080051004800420003001500480044005200030054004d0057...>Tj
ET
q                 ← push (closing)
Q                 ← pop
Q                 ← pops to next-outer level (these 3 come from original stream)
Q
q                 ← next BT's wrapper
```

## The Tm sign inversion vs. surrounding blocks

Every BT block on the page (verified by parsing all 35 blocks of
`orig_page3_content.bin`) uses the **same** Tm pattern:

```
/F7 {26 or 34} Tf  1 0 0 -1 0 0 Tm  20 -{baseline} Td
```

So `Tm.d = -1` (negative) for every original block. With `Tfs=26` and
`CTM.d=-0.48`, effective `trm.d = -1 × 26 × -0.48 = +12.48` (POSITIVE) →
rendered right-side-up.

The inserted `/Fsub1` block has `Tm.d = +4.34` (POSITIVE). With `Tfs=12.48`
and the same `CTM.d=-0.48`, effective `trm.d = +4.34 × 12.48 × -0.48 =
-25.99` (NEGATIVE) → rendered upside-down.

**`Tm.d` sign on our inserted block has the opposite sign from every other
block on the page.** That is the visible upside-down bug.

## Why the math "looks right" but produces the wrong sign

`afterTextObjectWithSubstitutionCallback` recovers Tm via:

```cpp
fz_matrix ctm_inv      = fz_invert_matrix(inj.ctm);
fz_matrix tm_with_size = fz_concat(inj.trm, ctm_inv);
Tm.d = tm_with_size.d / fontSize;
```

The textFilter callback recorded `inj.trm = [26, 0, 0, -26, 20, 200]` and
`inj.ctm = [0.48, 0, 0, -0.48, 0, 350.68]`. Plugging in:

- `ctm_inv = [2.083, 0, 0, -2.083, 0, 730.58]`
- `tm_with_size = [54.17, 0, 0, +54.17, 41.67, 313.92]` ← Tm × Tfs comes out **+54.17** (positive d) because the two negatives in trm.d/ctm.d cancel
- `Tm = [4.34, 0, 0, +4.34, 41.67, 313.92]` after dividing by Tfs=12.48

This is what landed in the saved PDF (verified against the actual bytes). The
recovery is algebraically consistent with reproducing `inj.trm` IF the saved
file is rendered under the same CTM convention MuPDF used at the time of the
filter callback.

## Why the inj.trm value is impossible in the page's content stream

I parsed every BT in the original content stream (`orig_page3_content.bin`).
**Not one** of the 35 blocks on this page has a Tm that produces
`trm = [26, 0, 0, -26, …]` for any character.

For example, block 6 (the "Eric Ries unravels…" line — the visible target
of the user's edit) has:

```
/F7 26 Tf  1 0 0 -1 0 0 Tm  20 -143 Td <0016> Tj …
```

A standard PDF-spec evaluation of that yields `trm = [12.48, 0, 0, +12.48,
9.6, 419.32]` (Tm.d × Tfs × CTM.d = -1 × 26 × -0.48 = +12.48, **positive**).
The log says `trm.d = -26` (negative) — incompatible.

In other words: **the `trm` that the textFilter callback recorded does not
correspond to any character in the page's PDF-spec content stream.**

## What MuPDF's text_filter actually passes

The discrepancy is the smoking gun. MuPDF's `pdf_filter_processor` passes
`trm` and `ctm` in the matrix convention used by its rendering pipeline,
which is **fitz space (Y-down, origin top-left)**, not PDF user-space (Y-up,
origin bottom-left). MuPDF composes an internal `[1, 0, 0, -1, 0, pageH]`
Y-flip onto the page-level CTM so that downstream renderers can blit
top-down. The trm/ctm pair the filter sees has that Y-flip baked in.

So:

- A normal upright body-text glyph in PDF user-space has `trm.d > 0`.
- The same glyph in MuPDF fitz-space has `trm.d < 0`.

The `inj.trm = [26, 0, 0, -26, 20, 200]` and `inj.ctm = [0.48, 0, 0, -0.48,
0, 350.68]` recorded by the filter are both in fitz-space, with a baked-in
Y-flip relative to the PDF user-space numbers we see in the saved content
stream (e.g. `1 0 0 -1 0 0 Tm`).

When `ContentStreamEditor` recovers `Tm = trm × ctm⁻¹ / Tfs` and emits the
result back into a PDF content stream, the receiving renderer (Adobe,
PyMuPDF, etc.) interprets the emitted Tm in **PDF user-space**, not in
fitz-space. The Y-flip that the filter implicitly canceled out (because
both trm and ctm carry it) re-asserts itself when only ONE of those
matrices is reapplied. Result: the rendered character flips upside down.

The same effect explains the size: the recovered `Tm.a / Tm.d = +4.34` is
roughly `1 / 0.48 × correct_scale` — the rewrite is using a Tm that
"un-cancels" the CTM in fitz-space terms, but in PDF user-space terms that
becomes a 2x over-scale.

## Why the original "Eric Ries unravels…" line was not removed

A separate but related bug: the saved file still contains all 65 glyphs of
the original block 6 ("Eric Ries unravels the mysteries of entrepreneurship
and reveals"). The filter's `text_filter` callback returned `1` (REMOVE)
for the matched character, but the original glyphs survived. Two
possibilities for phase-3 to investigate:

1. The `text_filter` callback is gated on a per-character bbox test that
   matched only ONE character (or a few) of the visible line — the rest
   were below the bbox-overlap threshold and stayed.
2. MuPDF's `text_filter` returning 1 may not propagate correctly when the
   `after_text_object_with_substitution` callback is also wired.

The CIDs of the original block 6 (`<0016> <0017> <0018> <000A> …`) are
distinct from the CIDs we wrote (`<0094> <0008> <0051> …`), so the two
overlap visually but live in independent Tj operators.

## Symptom mapping

- **A (oversized live overlay):** preliminarily, the live-overlay path in
  TextEditTool::buildOverlayFont uses `pointSize = baseSizePt × zoom × 0.75`
  with `baseSizePt = span.fontSize = 12.48` — gives `pointSize=26` at
  zoom=2.85 (logged). At standard 96-DPI this should match the surrounding
  body text visually, but on a high-DPI display or with Qt's per-monitor
  scaling, the 0.75 factor produces a 2x divergence. Same family of
  fitz-vs-PDF-space mismatch as B, just on the QFont side.

- **B (oversized + upside-down + garbled saved text):** root cause is the
  fitz-space-vs-PDF-user-space convention mismatch in
  `ContentStreamEditor::afterTextObjectWithSubstitutionCallback`. The
  emitted Tm is computed assuming MuPDF's `trm` is in PDF user-space, but
  MuPDF actually passes it in fitz-space. The "garbled" appearance is the
  combined effect of upside-down + 2x scale. Glyphs themselves are correct
  Latin letters from `/Fsub1`'s ToUnicode mapping (CID 0x0008 → 'E', etc.).

- **C (Ctrl+Z upside-down ghost):** same root cause as B. Undo replays the
  same `EditTextCommand` data with the same `inj.trm`/`inj.ctm`, producing
  the same wrong-sign Tm in the rewritten stream.

## Cascading vs independent

A and B and C are **not independent**. They share one root cause: code
across `TextEditTool`, `ContentStreamEditor`, and `TextEditCommands`
treats MuPDF's `trm`/`ctm` as if they were in PDF user-space when they
are actually in MuPDF fitz-space (Y-down). The math reproduces the same
matrix in MuPDF's frame but produces a Y-flipped, 2x-scaled matrix when
re-emitted into a PDF that downstream readers evaluate in PDF user-space.

## Phase-3 fix direction (proposal)

Two options, in order of preference:

1. **Convert trm/ctm to PDF user-space before recovery.** Apply
   `fz_concat(trm, fitz_y_flip)` where `fitz_y_flip = [1, 0, 0, -1, 0, pageH]`
   to both `inj.trm` and `inj.ctm` (or to the composite as-needed) before
   computing `tm_with_size`. Then the recovered Tm is in PDF user-space
   and emits correctly. This requires `pageHeight` to be reachable in
   `afterTextObjectWithSubstitutionCallback` — it already is, in
   `state->pageHeight`.

2. **Don't recover from trm at all.** Emit Tm in the canonical
   surrounding-block style: `[1, 0, 0, -1, e_user, f_user] Tm` with
   `e_user, f_user` derived directly from the user's clicked position
   (which is already in fitz-space, so `f_user = pageHeight - userY` is
   already the right number). Use `Tfs = original_pt_size_of_block` (read
   from the surrounding /F7 BT, not computed). Skip the matrix algebra
   altogether — match the document's existing convention.

Option 2 is the more robust fix because it removes a whole class of
fitz-vs-PDF-space confusion from the rewrite path. It also produces a
content stream that diffs cleanly against the original, which is friendlier
for downstream tools and incremental saves. Option 1 is the smaller patch
but keeps a thin layer of fitz↔PDF translation that has to stay correct on
every future change.

## Open phase-2 items

- Verify the bbox-matching algorithm in `textFilterCallback` to understand
  why only some characters of the user-clicked line are being matched
  (the original line is still in the saved file). This is the second bug
  that needs a separate fix in phase-3.
- Confirm option 2's claim that `Tfs = 26` (read from the surrounding /F7
  BT) is the right value — verify by computing what visible point size
  results when you emit `Tm=[1,0,0,-1,e,f] Tfs=26` under the page's CTM.
