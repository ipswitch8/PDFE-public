# Text Reflow Engine — Architecture

**Status:** design → implementation (Phase 9 of Acrobat-parity pipeline)
**Scope:** paragraph-level alignment persistence + multi-line reflow when
text length changes within a formatting block.

## Why

The pipeline's Phase-1 "text alignment persistence" item was deferred
because there was no paragraph-level abstraction: `FormattingRun` and
`TextSpan` are line/run primitives, not block primitives. Alignment is
a block-level property (a whole paragraph is left-aligned; individual
runs within it are not aligned separately), so bolting alignment onto a
FormattingRun would both break its semantics and force every caller to
carry redundant alignment state.

This document describes the `FormattingBlock` abstraction introduced in
Phase 9 and the narrow reflow algorithm chosen for its "text length
changes → rewrap lines" responsibility.

## Scope

In:

- `FormattingBlock` struct aggregating one or more `TextSpan`s that share
  a paragraph-level alignment setting.
- Alignment values: `Left`, `Center`, `Right`, `Justify`.
- A greedy line-breaking algorithm (documented alternative to Knuth-Plass)
  that rewraps a block when the cumulative advance width of its text no
  longer fits the block bounds.
- Persistence: `ContentStreamEditor` emits per-line `Td`/`Tm` offsets
  that place each line at the correct x-origin for the requested
  alignment. The saved PDF therefore renders with the alignment the
  user selected and the reflowed line structure.
- A perf budget test asserting <100 ms for a 500-character block.

Out (explicit non-goals):

- Hyphenation, microtypography, kerning adjustments.
- Bidi re-ordering inside a block (TextSpan already carries writingMode;
  the reflow engine treats LTR/RTL as pre-existing input).
- Full Knuth-Plass minimization. The greedy algorithm is acceptable per
  the pipeline spec ("Knuth-Plass or documented simpler alternative").
  If measured quality falls short, Knuth-Plass can drop in without
  changing `FormattingBlock`'s shape.
- Headers/footers/hanging-indent body styles. Phase 4d already handles
  header/footer content streams separately.

## Data model

```cpp
enum class Alignment { Left, Center, Right, Justify };

struct FormattingBlock {
    // Which spans of a TextEditModel belong to this block. Spans are
    // referenced by index, not by value, to avoid forcing every existing
    // call-site to know about FormattingBlock.
    std::vector<int> spanIndices;

    Alignment alignment = Alignment::Left;

    // Bounds of the block in PDF coords. If empty, the bounds union
    // of the referenced spans is used.
    QRectF bounds;
};
```

`TextSpan` and `FormattingRun` are UNCHANGED in layout. This matters
because adding members to `TextSpan` would be an ABI break and has
caused recurring stale-.obj heap-corruption crashes historically
(see `memory/crt_modal_dialogs_hang_ci.md`).

## Reflow algorithm

Given a block's text, font metrics, and bounds:

1. Collapse whitespace runs into single spaces (matching Tesseract-
   style output; whitespace inside a run is preserved).
2. Split text into words (whitespace-delimited; no hyphenation).
3. Greedy fill: accumulate words into the current line as long as the
   measured advance width fits within the block's width. When the next
   word would overflow, commit the current line and start a new one.
4. The last line's pre-justification width is recorded so the alignment
   pass can place it without distortion.

Greedy is O(N) in words. For the acceptance-budget case (500 characters,
~80 words), this runs in microseconds; the <100 ms budget is dominated
by the HarfBuzz advance-width call per word.

## Alignment pass

After reflow, each line has a measured width `w_line` and the block has
a width `W`. The x-origin offset applied via `Td` / `Tm` is:

| Alignment | Origin offset         | Inter-word spacing |
|-----------|----------------------|---------------------|
| Left      | 0                    | measured (unchanged)|
| Center    | (W - w_line) / 2     | measured (unchanged)|
| Right     | (W - w_line)         | measured (unchanged)|
| Justify   | 0                    | (W - w_line) / (words-1) extra; last line left-aligned |

Justify tweaks per-space advance via the PDF `Tc` / `Tw` operators
rather than per-glyph positioning, which keeps the content stream
small and readable.

## Persistence format

Each reflowed line emits a single text-showing operator group:

```text
BT
  /F<n> <size> Tf
  <Tm-matrix> Tm           % applies rotation + the x offset
  (<line-text>) Tj
ET
```

For Left-alignment with no reflow, the existing per-span `Td` offset
path is preserved — no change to existing byte output on the default
alignment code path. This keeps the Phase-1 deferral regression-safe.

The `FormattingBlock.alignment` is echoed into a private-use marker
comment immediately preceding the `BT`:

```text
%PDFE-ALIGN: left|center|right|justify
```

On reopen, `ContentStreamEditor` reads that marker back and rebuilds
the `FormattingBlock` with the recorded alignment. This is a
PDFE-private extension, not a PDF-spec standard; other viewers render
the same bytes without knowing alignment, which is fine because the
alignment is already baked into the `Td`/`Tm` offsets — the marker is
only so a second pass through PDFE can edit the paragraph's alignment
in-place without inferring it from visual positioning.

## Tests (acceptance matrix)

- Alignment round-trip for all four modes: set → save → reload →
  assert `FormattingBlock.alignment` matches.
- Reflow on insert/delete: grow text to overflow the block; confirm
  line count grows; shrink text; confirm line count shrinks.
- Reflow on font-size change: double fontSize; confirm line count
  roughly halves (within ±1 for overflow/underflow).
- Block-boundary preservation on save: multiple blocks survive with
  their own alignments; no cross-block merging.
- Perf benchmark: 500-character block reflow completes in <100 ms on
  the reference machine; the test asserts the budget with generous
  slack (<200 ms for CI variance) and records the observed time to
  the perf-bench log.

## Why not Knuth-Plass right now

Knuth-Plass globally minimizes badness across the whole paragraph at
the cost of a quadratic-worst-case DP. For the 500-character budget we
have (~80 words), its advantage over greedy is cosmetic and
perceptible mainly in paragraphs wider than typical PDF columns. The
cost to add it later is low because `FormattingBlock` already models
paragraph boundaries; swapping the line-break algorithm is a drop-in
inside `TextReflowEngine::reflow`.

## Open questions (tracked for follow-up)

- Mixing alignments within a single on-screen paragraph (e.g., right-
  aligned number on a line of left-aligned text). Not supported by
  `FormattingBlock.alignment` which is paragraph-scoped. If a user case
  arises, it would be handled by splitting into multiple blocks.
- Table-cell alignment. Out of scope here; Excel export (Phase 7c)
  owns table-cell formatting independently.
- RTL: the alignment constants (`Left`, `Right`) are visual rather than
  logical. For RTL paragraphs, `Right` is the natural default. The
  engine honors the visual meaning; logical-alignment UI will be a
  separate future phase.
