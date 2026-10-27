# Text Formatting Feature Gaps

<!-- tracking-banner -->
> **STATUS: HISTORICAL EVIDENCE — this is not a work list.**
>
> **GitHub Issues (`ipswitch8/PDFE`) is the canonical tracker for anything still
> open.** This document records what an audit found on the day it ran and what
> was decided about it. Rows here may have been fixed, superseded, or
> deliberately declined since. Where a row and a GitHub issue disagree, **the
> issue wins**; where a row describes something still broken and no issue
> exists, file one.
>
> See [`docs/TRACKING.md`](../TRACKING.md) for why, and for the parts of this repo that are
> operationally live rather than historical.

---


**Discovered:** phase-9 (P2 Text Editing Rich Formatting)
**Status:** open backlog

The phase-0 UI test matrix enumerated text-formatting operations
that do not correspond to widgets in pdfe's `TextEditToolBar`.
Phase-9 catalogued the gap; this document tracks each missing
feature and its unblock path.

## Available Widgets (Source of Truth)

From `src/ui/TextEditToolBar.cpp`:

| Widget | objectName | Type | Notes |
|---|---|---|---|
| Font family combo | `fontComboBox` | QFontComboBox | System fonts |
| Font size spin | `fontSizeSpinBox` | QDoubleSpinBox | 0.0 – 999.0 pt |
| Bold toggle | `boldButton` | QToolButton, checkable | Ctrl+B |
| Italic toggle | `italicButton` | QToolButton, checkable | Ctrl+I |
| Underline toggle | `underlineButton` | QToolButton, checkable | Ctrl+U |
| Strike toggle | `strikethroughButton` | QToolButton, checkable | — |
| Atlas mode combo | `atlasModeCombo` | QComboBox | TRE atlas selection |
| Compare button | `compareAtlasButton` | QPushButton | TRE compare dialog |

## Missing from Phase-0 Matrix Expectations

### 1. Text color picker

Matrix rows `txt_color_red`, `txt_color_blue`, `txt_color_green`,
`txt_color_custom` assumed a color picker button on the format
toolbar. No such widget exists in `TextEditToolBar`. The existing
annotation color picker (`colorPickerButton_` in `MainWindow.cpp:980`)
is for annotation colors, not text edit formatting colors.

**Unblock:** add a `textColorButton_` to `TextEditToolBar` that
opens a `QColorDialog` on click and applies the selected color to
the active `FormattingRun`. Wire through to `TextEditTool::setTextColor`
or equivalent.

**Affected test rows:** 4 (txt_color_red/blue/green/custom)

### 2. Text alignment

Matrix rows `txt_align_left`, `txt_align_center`, `txt_align_right`,
`txt_align_justify` assumed alignment controls. TextEditToolBar has
none. PDF text runs don't inherently have "alignment" — alignment is
a block-level concept that requires paragraph-aware content-stream
manipulation, which pdfe's text edit layer does not currently model.

**Unblock:** this is a significantly larger feature than a color
button. It requires `FormattingRun` or an enclosing `FormattingBlock`
to carry an alignment attribute and for the TextEditTool to reflow
text within its bounding box based on that attribute. Unlikely to
ship in a near-term release.

**Affected test rows:** 4 (txt_align_left/center/right/justify)

### 3. Document-state verification oracle

Phase-9's green rows use toolbar widget state (`get_toggle_state()`,
`selected_text()`, spinner value) as the oracle. The stronger oracle
would be "text span in the document has the bold attribute set".
That requires saving the PDF and inspecting content-stream state,
or reading an in-memory `FormattingRun` via a programmatic hook the
test harness does not currently have.

**Unblock paths:**

1. **Add a test-only diagnostic API**: expose a
   `TextEditTool::pendingFormattingRuns()` accessor via a test-hook
   build flag, read it from the Python layer via a file-drop
   side channel (e.g. write JSON to a tempfile on each format
   change when `PDFE_TEST_HOOK_DIR` env var is set).

2. **Full save + reload oracle**: after applying formatting, trigger
   a text edit commit (blocked on canvas-click gap), save the PDF
   via File > Save As, reopen, and inspect the saved content
   stream for `Tf`/`Ts`/`Tm` operators with the expected arguments.
   Much stronger but requires the phase-5 canvas-click harness fix.

3. **Accept toolbar-state coverage for phase-9**: the current
   approach. The toolbar widgets genuinely change state on click,
   the shortcut keys work end-to-end (Ctrl+B toggles boldButton),
   and the production code paths that wire the buttons to the
   FormattingRun are covered by existing GoogleTest unit tests
   (`tests/test_text_edit_formatting.cpp` and similar). Phase-9
   thus verifies the UI-to-state-change layer; the state-change-to-
   document-persistence layer is already unit-tested.

Phase-9 currently pursues path 3, with path 2 available once the
canvas-click harness gap lands.

**Affected test rows:** 8 toggle + 4 font + 5 size rows use the
weaker toolbar-state oracle. The tests are honest but not
end-to-end.

### 4. TRE image-dominated text editing

4 matrix rows (`txt_tre_edit_regular_font`, `_arabic`, `_cjk`,
`_size_change`) require the full image-dominated edit flow:
1. Activate Text Edit tool on a scanned/OCR'd page
2. Click a text span to select it for editing
3. Type replacement text
4. Apply formatting
5. Commit the edit
6. Verify the content-stream update

Step 2 is blocked on the canvas-click harness gap. Without the
click, there is no span to format, and the format toolbar operations
are no-ops in a TRE context.

**Unblock:** resolve `docs/canvas-click-harness-gap.md` first.
The TRE rows will likely flip to green almost automatically once
canvas clicks commit annotations — the TextEditTool's span click
dispatches through the same `PdfCanvas::mousePressEvent` path.

**Affected test rows:** 4 TRE rows
