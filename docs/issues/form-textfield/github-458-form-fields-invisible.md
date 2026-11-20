# GitHub #458 — Form text fields are invisible after placement

Filing status: filed as GitHub #458.

Pipeline: form-textfield. Phase 1 recorded the red probes; the fix landed in phase 2. All probes for this issue are green.

## Summary

A text field placed with the form tool cannot be seen on the page once it is
deselected. It shows only while it is being edited, because the tool draws a
selection outline then. Form fields usually have an obvious outline and a
shaded background, so you can see where to fill them in.

## Cause

`FormManager::createFormField` writes `/FT /Tx` and a `/DA`, but no `/MK`
border or background colour (`/BC`, `/BG`) and no `/BS` border style. MuPDF
draws the border and background of a field appearance only from `/MK`, so an
empty field's appearance stream draws nothing. `restoreFormField`, which is
used by undo and redo, rebuilds through the same path.

## Expected

- a new field (text, combo box and list box at least) has a visible border and
  a light shaded background;
- the same holds after undo/redo and after saving and reopening.

Check box, radio and push button fields are not covered by the render probes:
MuPDF 1.26.10's `pdf_write_widget_appearance` throws for `/FT /Btn`, so their
appearance does not come from the `/MK`-driven text/choice writer this bug is
about.

## Probes (tests/test_form_textfield_state.cpp)

Render probes, parameterised over Text, ComboBox and ListBox (the suffix of each
name is the field type):

- `FormTextfield/FormTextfieldAppearanceState.FreshFieldShowsBorderRingAndShadedInterior/{Text,ComboBox,ListBox}`
- `FormTextfield/FormTextfieldAppearanceState.FieldStaysVisibleAfterUndoRedo/{Text,ComboBox,ListBox}`
- `FormTextfield/FormTextfieldAppearanceState.FieldStaysVisibleAfterSaveAndReopen/{Text,ComboBox,ListBox}`

Structural probe:

- `FormTextfield/FormTextfieldAppearanceStructure.CreatedFieldCarriesMkBcBgAndBs/{Text,ComboBox,ListBox}`

## Resolution (pipeline form-textfield, phase 2)

### Fix

`src/forms/FormManager.cpp`:

- `FormManager::defaultBorderAppearance(type)` (new, public static) returns the
  look of a field the app creates. The border is dark slate blue
  `[0.20 0.30 0.50]` (luminance about 74 of 255). The background is a very
  light blue `[0.88 0.92 1.00]` (luminance about 233). The border style is
  1pt solid (`/BS << /W 1 /S /S >>`).
- `createFormField` now goes through a private
  `createFormFieldWithAppearance`. That function calls
  `writeBorderAppearance`, which writes `/MK << /BC [...] /BG [...] >>` and
  `/BS`, **before** `pdf_update_annot`. So the first appearance stream
  MuPDF generates already has the border and the background.
- `readWidgetData` (used by `getFormField`, and so by `deleteFormField`) now
  captures the widget's `/MK /BC`, `/MK /BG` and `/BS` (`/W`, `/S`) into
  `FormFieldData::borderAppearance`. It also sets
  `borderAppearanceCaptured = true` (`src/forms/FormFieldData.h`).
- `restoreFormField` (used by undo/redo of create and of delete) re-applies the
  captured appearance, or `defaultBorderAppearance(type)` for hand-built data.
  So a field the app created comes back with the app look, and a field loaded
  from a file comes back with its **own** look, or with none if it had none.

Fields loaded from existing files are never restyled. `writeBorderAppearance`
is reached only from `createFormFieldWithAppearance`, which builds a new
widget. Opening a file, filling a field, or saving does not write `/MK` or
`/BS`.

### Decision: check box, radio button and push button

**Decision: they get the same `/MK` + `/BS` as text, combo and list fields.**

Phase 1 wrote that MuPDF 1.26.10's `pdf_write_widget_appearance` throws for
`/FT /Btn`. That is true of that function, but it is not the path a button
takes. `pdf_update_appearance` special-cases button widgets
(`source/pdf/pdf-appearance.c` around line 3777 in the bundled
1.26.10 tree). It sends them to `pdf_update_button_appearance` (line 3176),
whose writers all read `/MK` and `/BS`:

- `draw_check_button` (line 3143) fills `/MK /BG` and strokes a `/MK /BC`
  rectangle at the `/BS /W` width;
- `draw_radio_button` (line 3106) does the same with a circle;
- `draw_push_button` (line 3033) fills `/MK /BG`, strokes `/MK /BC`, and adds
  a bevel derived from `/BG`.

Without `/MK`, an Off check box or radio button and an empty push button draw
nothing at all, which is the same bug. Evidence: the
`AllPlaceableTypes/FormFieldVisibilityModel.*/{Checkbox,RadioButton,PushButton}`
tests in `tests/test_form_textfield_model.cpp` render each of them with a
visible border on every side (for radio, at the circle's four extreme points)
and a shaded interior with no ink. They do this after create, after
undo+redo, after delete+undo, and after a restore from hand-built data.

**Signature: no `/MK`.** An unsigned signature widget is drawn from MuPDF's
own display list (`pdf_signature_appearance_unsigned`, the `ft == Sig`
branch next to the button branch). That path ignores `/MK`, so writing it
would change nothing visible. `FormFieldSignatureModel.SignatureFieldGetsNoMkOrBs`
pins this.

### Tests

- Phase-1 probes (unchanged): all 12 Bug 1 probes in
  `tests/test_form_textfield_state.cpp` are green.
- New, in `tests/test_form_textfield_model.cpp`:
  - `AllPlaceableTypes/FormFieldVisibilityModel.*` covers Text, Checkbox,
    RadioButton, ComboBox, ListBox and PushButton. It checks the structure
    against the default, the render after create, after undo+redo of create,
    and after delete+undo, a restore from hand-built data, and the delete
    capture.
  - `FormFieldVisibilityDefaults.DefaultColoursAreInkBorderAndLightShade`
    checks that the default colours fall in the oracle's ink and shade
    luminance bands.
  - `FormFieldSignatureModel.SignatureFieldGetsNoMkOrBs`.
  - `TextAndChoice/FormFieldLoadedAppearanceModel.*` covers a field loaded
    from a file with its own `/MK` (red border, yellow background, 2pt
    dashed `/BS`). It keeps that look as loaded, through a value change and
    through delete+undo, and it still renders red. A loaded field without
    `/MK` is not given one by delete+undo.

### Existing tests that compare a new field's appearance

I grepped every test that creates a field (`createFormField`,
`CreateFormFieldCommand`, `restoreFormField`, `DeleteFormFieldCommand`) for an
appearance stream, hash, digest, pixel or golden comparison. No C++ test
hashes or compares a newly created field's appearance stream, so none needed
updating and no assertion was changed. One match was unrelated: SHA-256 TSA
digests in `test_tsa_and_taborder.cpp`. The Windows pywinauto test
`tests/ui_automation/test_ui_batch2_forms.py` names a golden directory,
`goldens/batch2_forms`, but that directory does not exist in the tree, so
there is no stored golden to go stale. If one is seeded later, it will show
the new outlined fields.

### Known limit

`restoreFormField` re-applies `/MK /BC`, `/MK /BG`, `/BS /W` and `/BS /S`. It
does not restore other `/MK` entries (`/CA`, `/R` and so on) or a `/BS /D`
dash array, and it did not restore them before this change either.

## Reconciliation with origin/master (pipeline form-reconcile)

origin/master fixed the same symptom a different way while this branch was
open: its `FormTool::onPaintOverlay` draws, for every unselected widget, a
1 px outline and a light tint on the overlay. The branch merged origin/master
(merge commit fc98acc9). Keeping both as they were would have drawn two
rectangles on the edge of every field the app creates: the slate-blue `/MK`
border in the page and master's grey stroke on top of it.

The merged tree keeps both mechanisms with one rule
(`overlayOutlineNeeded()` in `src/forms/FormTool.cpp`):

- every unselected widget still gets master's tint, a fill-me cue inside the
  field;
- the outline is stroked only when the widget draws no border of its own:
  - a **Signature** field, whose unsigned appearance comes from MuPDF's own
    display list and ignores `/MK` (so this branch never writes `/MK` for one);
  - a widget with **no `/MK /BC`**, typically one loaded from a file that has
    no border colour;
  - a widget whose border width is 0 (`/BS /W 0`);
- a widget with an `/MK` border (every field the app creates, other than a
  signature) gets the tint and no stroke.

The `/MK` and `/BS` writes described above are unchanged, so the field still
shows its border in other viewers and in print, where the overlay does not
exist.

Pinned by `FormFieldReconcileOverlay.OutlineIsNotDrawnOverAnMkBorder`,
`FormFieldReconcileOverlay.OutlineIsDrawnForAWidgetWithoutAnMkBorder`,
`FormFieldReconcileOverlay.OutlineIsDrawnWhenTheMkBorderHasZeroWidth` and
`FormFieldReconcileOverlay.SignatureIsOutlinedWithOrWithoutAnMkBorder` in
`tests/test_form_reconcile.cpp`. These check the edge band of the overlay,
which master's own `FormToolOverlayRenderTest.*` (unchanged and green) does
not look at.

Known limit: a widget with `/MK /BC`, no `/BS` and a `/Border` array of width
0 counts as bordered, because `/Border` is not captured. It still gets the
tint, so it stays visible.
