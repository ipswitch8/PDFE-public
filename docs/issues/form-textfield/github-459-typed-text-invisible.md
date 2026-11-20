# GitHub #459 — Typing into a form text field shows nothing until you click out of it

Filing status: filed as GitHub #459.

Pipeline: form-textfield. Phase 1 recorded the red probes; the fix landed in phase 3. All probes for this issue are green.

## Summary

While you type into a form text field, the typed text is not drawn. It
appears only after the field loses focus and the value is committed.

## Cause

`FormTool` keeps the typed text in its edit buffer and writes the field value
only on commit. Its overlay paints the caret but never the buffer or the IME
preedit text. This was recorded as the known deferral
IME-FORM-PREEDIT-PAINT-01.

## Expected

While editing, the field shows the text typed so far, including any IME
preedit text, in the field's own font, size and colour, with the caret after
the last character. The document is still written only on commit.

## Probes (tests/test_form_textfield_state.cpp)

Red on the unfixed code:

- `FormTextfieldEditPaintState.TypedTextIsPaintedInsideTheFieldBeyondTheCaret`
- `FormTextfieldEditPaintState.PaintedInkGrowsWithMoreCharacters`
- `FormTextfieldEditPaintState.PreeditTextIsPainted`
- `FormTextfieldEditPaintState.CaretXMatchesTheTextAdvance`

Controls (green on the unfixed code by design; they stop the fix from writing
the field on every keystroke):

- `FormTextfieldEditPaintState.DocumentValueIsUnchangedWhileEditing`
- `FormTextfieldEditPaintState.CommitStillWritesTheValue`

## Resolution

Fixed in pipeline form-textfield, phase 3 (branch
`fix/form-textfield-visibility`).

**Approach.** While a text field is being edited, `FormTool::onPaintOverlay`
paints the field itself. The page raster under the overlay comes from the
widget's appearance stream, which stays stale until commit, so the overlay
draws the following on top of it:

- an opaque fill in the widget background (`/MK /BG`, or white when there is
  none);
- `editBuffer_`, plus any IME preedit, drawn at the cursor and underlined;
- the text in a `QFont` mapped from the `/DA` base-14 font: `Helv` to
  Helvetica, `TiRo` to Times, `Cour` to Courier, and anything else to a
  sans-serif face. The pixel size is the `/DA` size times the zoom, and the
  colour is the `/DA` colour;
- a layout that follows MuPDF's `write_variable_text`:
  - padding of twice the border width (2pt for a 1pt border);
  - a size-0 `/DA` fits the field height and shrinks to fit the width, or is
    12pt when multiline;
  - single-line text is vertically centred and scrolls to keep the caret
    visible;
  - `/Q` quadding (left, centred, right) places the text as the committed
    appearance does. Single-line text is shifted by `(w - tw) / 2` or
    `w - tw` inside the padded box. Multiline text is aligned per line, by the
    line's width without the space a soft wrap broke at. The preedit and the
    caret move with the aligned text, and comb fields ignore `/Q`. `/Q` is
    read with MuPDF's own `pdf_annot_quadding`, the accessor its appearance
    writer uses. In 1.26.10 that accessor reads the widget's `/Q` and not an
    inherited one, so the live paint follows it and the text does not jump on
    commit. `FormFieldEditPaintParentQuadding` pins this with a `/Q` set only
    on the parent field: both the live paint and the committed appearance are
    left-aligned;
  - multiline text word-wraps at a 1.116 em line height;
  - password fields show one bullet per character;
  - comb fields show one character centred in each of `/MaxLen` cells;
- the caret, placed at the `QFontMetricsF::horizontalAdvance` of the text
  before it. `caretScreenRect()` reports this same rectangle to the IME.

`FormFieldData` now carries the `/DA` font name, size and colour (`daFontName`,
`daFontSize`, `daColorComponents`, `daColor`). `FormManager::readWidgetData`
reads them through `pdf_annot_default_appearance`. The background colour is
the `/MK /BG` that `borderAppearance` already captures (GitHub #458).

**Not changed.** The document is not written on each keystroke. The value
still reaches the PDF only in `commitEdit()`, through `SetFieldValueCommand`,
so undo and the JS blur and calculate sequence behave as before.

**Not covered.** Rich text (`/RV`) is not reproduced.

**Hostile input (phase-3 security-audit follow-ups).** Every number the live
paint takes from the PDF is clamped twice: where `FormManager` reads it, and
again where `FormTool` uses it, because a `FormFieldData` can also be built by
hand. The limits are as follows.

- **`/DA` size.** MuPDF's `/DA` parser runs `fz_strtof`, so `1e30`, `1e50`
  (inf), `inf`, `nan` and negative sizes all reach the reader. A non-finite or
  negative size becomes 0, which means auto-size. A positive size is capped at
  1000 pt. The same rule applies to the `/DA` colour: a non-finite component
  reads as 0, and components are clamped to [0, 1].
- **Font pixel size.** Size times zoom is clamped to [1, 10000] px before it
  is converted to `int`. Within that range the result is the same
  `max(1, qRound(size x zoom))` as before.
- **Zoom and field box.** The paint draws nothing, and reports no caret,
  when:
  - the zoom is not finite, is not positive, is below 1e-3 or is above 1000
    (the canvas stays within 0.1 to 10). The lower bound keeps a denormal
    zoom from making the line height infinite and the culling range NaN;
  - the field box is not finite, or extends beyond 1e7 px.
- **Layout.**
  - **Wrapping.** Each line costs time proportional to its length. Summed
    per-character advances estimate the line break. The original exact
    substring measure then confirms it, usually in two measurements, so the
    breaks are the same as before.
  - **Exact-confirmation budget.** Exact confirmation stops after 65536
    UTF-16 units have been shaped in one layout, and it never applies to a
    line longer than 4096 units. Past either limit, the scale-corrected
    estimate decides the break.
  - **Length caps.** No line is longer than 65536 units, and no single draw
    call gets more than 16384 units.
  - **Caching.** The layout is cached across repaints, keyed on the composed
    text (preedit included), the font, the content width and the mode. Any
    change rebuilds it, and it is freed when the edit ends.
  - **Culling.** Only lines that can put ink inside the field are drawn. As
    a backstop, a multiline paint never draws more than
    `ceil((box height + 2 x glyph reach) / line height) + 2` lines, even if
    the culling range were wrong.
  - **Long single lines.** A single line of more than 4096 units is measured
    with cached per-codepoint advance sums. Only the stretch that can reach
    the field is drawn.
- **Comb fields.** A codepoint past `/MaxLen` is not drawn. When cells are
  narrower than a pixel (for example, `/MaxLen` 1e9), only the first cell in
  each pixel column is drawn.
- **`/MK` and `/BS` (GitHub #458, carried over from the phase-2 audit).**
  These rules apply on read, on write (`restoreFormField`) and in the paint:
  - a colour array must have 1, 3 or 4 entries;
  - components are clamped to [0, 1], and a non-finite or non-numeric
    component becomes 0;
  - `/BS /W` is clamped to [0, 100] pt, and a non-finite width becomes 1.

**Pixel identity (a one-off check, not a standing guarantee).** When the
security-audit remediation was made, a temporary scratch harness compared its
output for normal input with the phase-3 original, once, on the development
host: 1099 scenarios, image and caret rectangle, byte-for-byte, with no
difference. That harness was then removed, and no golden images were kept: the
glyphs come from whatever faces the host's font configuration resolves for
Helvetica, Times and Courier, so reference images would hold only on the host
that made them. Nothing re-runs the comparison. What guards the output now is
the named model tests below, which check behaviour (alignment, wrapping, caret
position, fonts, bullets, comb cells, preedit, bounds) rather than exact
pixels. The one-off comparison covered:

- single-line and multiline layouts;
- `/Q` 0, 1 and 2;
- five `/DA` fonts and sizes, including auto-size;
- kerning pairs, CJK text, emoji, hard newlines and leading or trailing
  spaces;
- zoom 1, 1.5, 2 and 3;
- a mid-text preedit;
- comb and password fields.

**Timing.** These are single-paint times on the development host:

| Input | Phase-3 original | Now |
|---|---|---|
| 1 MB multiline value, 0.5 pt `/DA` | ~232 s | ~0.2 s |
| 1 MB single-line value | 1.6 s | 0.03 s |
| 1 MB value in a 4-cell comb | 12 s | 1 ms |

**Closes** the deferral IME-FORM-PREEDIT-PAINT-01 (`docs/audit/rtm-coverage.md`,
the phase-76 dispositions, `phase-76/manual-qa-ime.md`, `FormTool.h`).

**Tests.**

- The four probes above are now green, and both controls stay green.
- `tests/test_form_textfield_model.cpp` gains two suites:
  `FormFieldEditPaintModel`, with 20 tests, and `FormFieldEditPaintQuadding`,
  with 8 tests. The quadding tests check centred, right-aligned and multiline
  alignment, the caret and preedit under `/Q`, and that comb fields ignore
  `/Q`. They also check that, for `/Q` 0, 1 and 2, the live ink spans the same
  horizontal extent (within 4px) as the committed appearance that MuPDF
  renders. The model tests cover:
  - the background fill;
  - the `/DA` colour and size, including auto-size;
  - multiline wrap, password bullets and comb cells;
  - the caret after cursor moves;
  - the preedit underline and its position;
  - zoom;
  - that the document is not written during the edit;
  - the `FormFieldData` `/DA` read-back.
- A third suite, `FormFieldEditPaintHostile` (18 tests), was appended for the
  security-audit follow-ups. It covers:
  - hostile `/DA` sizes (1e30, 1e50, inf, -inf, nan, negative and 0), both
    read from the file and passed in a hand-built `FormFieldData`;
  - the pixel-size clamp;
  - a non-finite or absurd zoom or box;
  - `/MaxLen` 1e9 combs and values longer than `/MaxLen`;
  - invalid UTF-8 and lone surrogates;
  - 1 MB multiline, single-line and zero-width values, each painted in
    under 1 s;
  - invalidation of the layout cache;
  - hostile `/MK` arrays (length 2, 5 or 6, non-numeric, NaN or out of
    range) and hostile `/BS /W` values, on read and on restore.
- A mutation check records each mutation turning at least one probe red:
  skipping the buffer draw, skipping the preedit draw, ignoring the `/DA` size,
  and mis-positioning the caret. Ignoring `/Q` turns the quadding tests red.
  The record is
  `.claude/pipelines/form-textfield/evidence/phase-3/mutation-check.txt`.
- The security-audit remediation adds 14 mutations, recorded in the same
  file. Each was killed by at least one hostile test:
  - removing the `/DA` size clamp, separately on read and at the paint site;
  - removing the pixel-size clamp;
  - restoring the original quadratic `wrapLines`;
  - removing the comb cap;
  - removing the `/MK` colour clamp, on read and on write;
  - removing the `/BS /W` clamp, on read, on write and at the paint site;
  - removing visible-line culling;
  - keying the layout cache without the text;
  - removing long single-line windowing;
  - removing the zoom guard.
- Remediation round 2 (claim-check gate) appends five suites (14 tests),
  each pinning a claim that had rested on prose alone:
  - `FormFieldEditPaintCanvasIme` (3 tests) builds a real `PdfCanvas` with an
    editing `FormTool`, paints it and asks it for `Qt::ImCursorRectangle`.
    The answer must equal the caret `FormTool` recorded and lie within 1 px
    of the caret line actually drawn, after typing, after a cursor move and
    with a preedit. A fixed 1x1 answer, which survived the gate's
    spot-check, is now red.
  - `FormFieldEditPaintDrawnCaret` (4 tests) checks the drawn caret column
    against the recorded rect, within 1 px, for single-line, `/Q` 1,
    multiline and comb layouts. A caret drawn 2 px off either way is red.
  - `FormFieldEditPaintParentQuadding` (1 test) sets `/Q` only on the parent
    field. The live paint and the committed appearance are both left-aligned
    and span the same extent. Making the live paint inherit `/Q` is red.
  - `FormFieldEditPaintFonts` (4 tests) covers `TiRo` (Times advance),
    `Cour` (equal advance for "iiii" and "WWWW"), the sans-serif fallback
    (`Symb`, `ZaDb` and a hand-built unknown name), and 12 pt auto-size for
    multiline. MuPDF maps an unknown `/DA` font name in a file to `Helv`
    before the paint sees it.
  - `FormFieldEditPaintHostileBounds` (2 tests) covers the two LOW audit
    items: a zoom below 1e-3, including a denormal one, paints nothing, and
    a 1 MB multiline paint never draws more lines than the box can show.
  - Twelve more mutations are recorded in `mutation-check.txt`, and 11 were
    killed. The one survivor is expected: removing the line cap alone changes
    nothing while culling is correct, because the cap is a backstop. With
    culling removed, the cap alone still holds the line bound, and removing
    both turns the bound test red.

## Reconciliation with origin/master (pipeline form-reconcile)

origin/master also painted the edit buffer while this branch was open. It
filled the field white and drew the buffer with `drawText` in the painter's
default font, at 0.6 times the field height, in black, with a caret placed by
a uniform-advance estimate. The branch merged origin/master (merge commit
fc98acc9), and the two paints conflicted in `FormTool::onPaintOverlay`.

The merged tree has ONE live paint path, the one described above
(`paintFieldEdit()`). master's white fill and `drawText` block were removed.
master's text was in the wrong font, size and colour and moved on commit, and
it ignored `/Q`, password masking (it showed a password in clear text while
typing), comb fields, multiline wrapping and the IME preedit.

What master added around it is kept:

- the at-rest overlay tint and, for a widget with no border of its own, the
  outline (see GitHub #458);
- master's paste into a field, which fills the same buffer through
  `handleTextInput`, so pasted text is painted live as well.

master's own oracle, `FormToolOverlayRenderTest.TypedTextRendersLiveBeforeCommit`,
asks only that dark ink appears after typing and before commit. It is
unchanged and green under this paint.
