# GitHub #460 — Font settings are ignored when editing a form text field

Filing status: filed as GitHub #460.

Pipeline: form-textfield. Phase 1 recorded the red probes; the model layer landed in phase 4 and the UI layer in phase 5. All 7 probes for this issue are green.

## Summary

Changing the font, size or colour while a form text field is selected or being
edited has no effect on the field.

## Cause

- Nothing writes a widget's `/DA` (default appearance) after the field is
  created. `FormFieldData` carries no font information, and the form field
  properties panel has no font controls.
- The text-edit toolbar stays visible when switching from the Text Edit tool
  to the Form tool. Its signals are still connected to the destroyed text-edit
  tool, so its changes go nowhere.
- Undo and redo rebuild a field with a hard-coded `/DA`, so any font would be
  lost anyway.

## Expected

- a text field's font (a standard PDF font), size and colour can be set from
  the properties panel, through an undoable command;
- the field renders with them, and they survive undo, redo, delete+undo and
  save;
- a change made while typing keeps the typed text;
- the disconnected toolbar is not shown under the form tool.

## Contract the probes pin for the fix

- `FormManager::setFieldDefaultAppearance(const Page*, int widgetIndex,
  const std::string& font, float size, const QColor& color)` (a `double` size
  also satisfies the probe). It does not exist yet; the probes reach it through
  a compile-time detection idiom, so the build stays green and they fail at run
  time with "setFieldDefaultAppearance not implemented (GitHub #460)".
- `FormFieldPropertiesPanel` font controls with objectNames
  `formFieldFontCombo`, `formFieldFontSizeSpin` and `formFieldFontColorButton`,
  each with a non-empty accessibleName and shown for a Text field.

## Probes (tests/test_form_textfield_state.cpp)

- `FormTextfieldFontState.SetDefaultAppearanceReadsBackTiRo18Red`
- `FormTextfieldFontState.RenderedInkHeightScalesWithSizeAndIsRed`
- `FormTextfieldFontState.DefaultAppearanceSurvivesUndoAndRedoOfCreation`
- `FormTextfieldFontState.DefaultAppearanceSurvivesDeleteAndUndo`
- `FormTextfieldFontState.MidEditFontChangeKeepsTheBufferAndCommitUsesNewDa`
- `FormTextfieldPanelState.PropertiesPanelExposesFontControls`
- `FormTextfieldToolbarState.TextEditToolbarIsHiddenUnderTheFormTool`

## Resolution (model layer)

Fixed in the model layer in pipeline form-textfield, phase 4 (branch
`fix/form-textfield-visibility`). The properties-panel font controls and the
toolbar under the Form tool are the UI layer, phase 5, in the next section.
At the end of phase 4 the two UI probes (`FormTextfieldPanelState.*`,
`FormTextfieldToolbarState.*`) were still red; phase 5 turned them green.

Every statement below names the test that pins it. Statements without a test
are marked as such.

### Setting the font

`FormManager::setFieldDefaultAppearance(page, widgetIndex, font, size, color)`
returns `bool`. It writes the widget's `/DA` through MuPDF's
`pdf_set_annot_default_appearance`, then regenerates the appearance with
`pdf_update_annot`.

- **What it writes.** The string is `/<font> <size> Tf <r> <g> <b> rg`. For
  example, Courier 9 in blue is `/Cour 9 Tf 0 0 1 rg`.
  - The colour is always written as an RGB triple. HSV and CMYK colours are
    converted.
  - `getFormField` reads the new font, size and colour back.
  - Pinned by `FormTextfieldFontState.SetDefaultAppearanceReadsBackTiRo18Red`,
    `FormFieldDefaultAppearanceModel.SetterWritesTheDaStringAndReadsItBack`
    and `OtherColourSpecsAreConvertedToRgb`.
  - The rendered value follows the size and colour: the ink height doubles
    from 10 to 20 pt, and the ink is red. Pinned by
    `FormTextfieldFontState.RenderedInkHeightScalesWithSizeAndIsRed`.
- **Fonts accepted.** Exactly the five base-14 keys `Helv`, `TiRo`, `Cour`,
  `Symb` and `ZaDb`. The match is exact and case-sensitive. Pinned by
  `EveryBase14KeyIsAcceptedAndReadsBack`.
- **Field types accepted.** Text, combo and list fields. Check boxes, radio
  buttons, push buttons and signatures are rejected. Pinned by
  `ComboAndListFieldsAcceptTheSetter` and
  `NonVariableTextWidgetsAndBadIndicesAreRejected`.
- **Sizes accepted.** 0 (auto) or 1 to 1000 pt, the same cap as the read
  sanitiser from phase 3. `-0` is written as `0`. Pinned by
  `BoundarySizesAreAccepted`.

### What is rejected

A rejected call returns `false`, and the whole serialised document stays
byte-identical. Each case is checked against a control showing that
serialising twice gives the same bytes.

| Rejected input | Pinned by |
|---|---|
| Any other font: empty, other case, `Helvetica`, `F1`, a name with a space, a leading `/`, `)`, an embedded NUL, or 10 000 characters | `InvalidFontIsRejectedAndTheDocumentIsUnchanged` |
| Sizes NaN, ±inf, negative, below 1 (including `denorm_min`), above 1000, 1e30 and `FLT_MAX` | `NonFiniteNegativeAndOversizedSizesAreRejected` |
| An invalid `QColor`; an extended-RGB colour outside [0, 1], infinite or NaN; any translucent colour (a `/DA` colour has no alpha) | `OutOfRangeOrTranslucentColoursAreRejected` |
| A field that is not text, combo or list; a widget index of -1, out of range, `INT_MIN` or `INT_MAX`; a null page | `NonVariableTextWidgetsAndBadIndicesAreRejected` |

`QColor::fromRgbF(NaN, 0, 0)` is a separate case. Qt stores it as an ordinary
RGB colour with integer components, so no NaN is left to detect.
`NanThroughNonExtendedFromRgbFNeverReachesTheDa` pins the guarantee that holds:
either the call is rejected with the document unchanged, or the `/DA` contains
no `nan` or `inf` token.

### Undo and redo: SetFieldAppearanceCommand

`SetFieldAppearanceCommand` lives in `FormCommands`.

- **Undo** puts back the widget's own `/DA` byte for byte, together with the
  `/DS` and `/RC` that MuPDF's setter deletes. If the widget had no own `/DA`
  (it inherited one), undo removes the own `/DA` again, so the inherited one
  takes effect. Pinned by:
  - `CommandUndoAndRedoRestoreTheExactDaStrings` (an irregularly spaced `/DA`
    plus a `/DS`);
  - `FormFieldDefaultAppearanceRemediation.CommandUndoRestoresDsAndRcByteExactlyAndRedoRemovesThem`.
    The widget carries `/DA`, `/DS` and `/RC`. Execute removes `/DS` and
    `/RC`, undo restores all three byte for byte, and redo removes `/DS` and
    `/RC` again;
  - `CommandUndoOfAnInheritedDaRemovesTheOwnDa` (inherited from the AcroForm);
  - `FormFieldDefaultAppearanceRemediation.CommandUndoOnAParentOnlyDaInheritsTheParentAgain`
    (inherited from a `/Parent` field).
- **Redo** puts back exactly what the execute wrote, and the result is
  byte-identical to the execute's. Pinned by
  `CommandUndoAndRedoRestoreTheExactDaStrings`.
- **The undo path skips the whitelist on purpose.**
  `FormManager::restoreFieldOwnDefaultAppearance`, which undo and redo use,
  does NOT apply the base-14, size or colour validation of
  `setFieldDefaultAppearance`. It writes the captured bytes as they are,
  because its job is to put back exactly what the widget held, including a
  `/DA` that names a `/DR` font or is malformed. Only bytes captured from the
  same widget reach it. Callers must not use it to apply new, unvalidated
  values.
- **A throw part-way through execute.** If `setFieldDefaultAppearance` throws
  part-way (MuPDF writes the `/DA` and deletes `/DS` and `/RC` before
  `pdf_update_annot` runs), `execute()` puts the snapshot back as far as it
  can and rethrows. The command is then not marked executed, so a half-applied
  change never sits behind an undo that does nothing. This is **untested**: no
  test can provoke a MuPDF throw at that point without a fault-injection seam.
- **A non-string `/DA`** is not valid PDF. It is captured as absent, so undo
  removes it. Pinned by `CommandUndoOfANonStringDaRemovesIt`.
- **A rejected request** changes nothing, and its undo and redo change
  nothing either. Pinned by `RejectedCommandIsANoOpIncludingUndoAndRedo`.
- **Merging.** The command never merges: `mergeId()` is 0 and `mergeWith()`
  returns false. Each change is one undo step. Pinned by
  `CommandsNeverMergeSoEachUndoRevertsOneChange`.
  - The reason: `UndoManager::execute` runs a candidate's `execute()` once to
    try a merge, and again if the merge is declined.
  - `execute()` is also idempotent and keeps its first snapshot. Pinned by
    `ExecuteIsIdempotentAndKeepsTheFirstSnapshot`.
  - A UI that emits a stream of values, such as a spin box, should issue the
    command when editing finishes (phase 5).

### Surviving save, delete+undo and undo/redo of create

- **Save.** The `/DA` survives save and reopen, and the reopened value is
  drawn red. Pinned by `DaSurvivesSaveAndReopen`.
- **Delete+undo and undo/redo of create.** `restoreFormField` no longer
  rebuilds the field with the hard-coded `/Helv 12 Tf 0 g`. `readWidgetData`
  now captures the `/DA` string in effect, byte for byte, using the same
  lookup MuPDF uses: the widget's own `/DA`, then the nearest `/Parent`'s,
  then the AcroForm's. The restore writes that string back as the widget's own
  `/DA` before the appearance is generated, along with a `/Q` of 1 or 2.
  - The border and background (`/MK /BC`, `/MK /BG`) and `/BS` captured in
    phase 2 are still re-applied, and the restored value is right-aligned.
    Pinned by `DeleteAndUndoKeepsDaMkAndQ`.
  - Pinned for undo/redo of create by `UndoRedoOfCreateKeepsDaAndQ`.
  - The phase-1 probes `FormTextfieldFontState.DefaultAppearanceSurvivesUndoAndRedoOfCreation`
    and `FormTextfieldFontState.DefaultAppearanceSurvivesDeleteAndUndo` are
    green.
  - A `/Q` of 0 is not written. Pinned by `QuaddingZeroIsNotWrittenOnRestore`.
  - Restore data assembled by hand, rather than read from a widget, still
    gets `/Helv 12 Tf 0 g`, and an out-of-range `/Q` is dropped. Pinned by
    `HandBuiltRestoreDataGetsTheDefaultDaAndASaneQ`.
- **An inherited `/DA`** becomes the restored widget's own `/DA`, which looks
  the same. Each step of the lookup is pinned:
  - the own `/DA` wins over the parent's: `CommandUndoOnAParentOnlyDaInheritsTheParentAgain`;
  - a `/DA` held only by the `/Parent` field is captured and restored, even
    though a different AcroForm `/DA` exists:
    `FormFieldDefaultAppearanceRemediation.ParentOnlyDaIsCapturedAndRestoredAsTheWidgetsOwn`;
  - the AcroForm `/DA` is used when neither the widget nor a parent has one:
    `InheritedDaIsRestoredAsTheWidgetsOwn`.
- **A `/DA` naming a non-base-14 font resource**, such as `/F1 9 Tf 0 0 1 rg`
  with `/F1` in the AcroForm `/DR`, is kept byte for byte, and the restored
  value is drawn in its blue. Pinned by
  `ExoticDrFontDaIsKeptThroughDeleteAndUndo`.
  - MuPDF's parsed font name for such a `/DA` is `Helv`.
  - A widget-level `/DR` is **not** carried through delete+undo, because the
    restore builds a new widget dictionary. The AcroForm `/DR` is unaffected,
    so the name still resolves. That test pins both facts.
- **A hostile `/DA` string** (garbage operators, `nan`/`inf`, an embedded NUL)
  is carried byte for byte through delete+undo and through the command's undo
  without throwing. Pinned by
  `HostileDaStringIsCarriedByteForByteWithoutThrowing`.
- **An overlong `/DA`.** A `/DA` over 4096 bytes is captured as its parsed
  values, which are all that MuPDF's 100-byte `/DA` parser reads anyway.
  Without this cap, a megabyte `/DA` would be copied on every live-edit
  repaint. Pinned by `OverlongDaIsCapturedAsItsParsedValues`.
  - **The re-print is never truncated.** The font is forced to a base-14 key.
    The buffer is 512 bytes, because MuPDF's `%g` never uses exponent form: a
    denormal colour component prints as "." followed by 44 zeros and its
    digits, so the worst case is about 250 bytes. Output that would fill the
    buffer is not captured. Pinned by
    `FormFieldDefaultAppearanceRemediation.OverCapDaWithTheLongestNumbersIsReprintedWhole`:
    ZaDb, size 1000 and four `1e-45` components re-print whole, at over 128
    bytes (the first buffer size). Also pinned by
    `FormFieldDefaultAppearanceRemediation.OverCapDaWithALongFontNameIsReprintedWhole`:
    a 300- or 40-character unknown font name re-prints as `Helv`, untruncated.

### Changing the font while typing

`FormTool` does not cache the `/DA`: `onPaintOverlay` reads the field afresh
on every paint. So a `/DA` change made mid-edit, by the setter or by the
command, shows in the next live paint without ending the edit or touching the
typed text:

- the ink doubles in height from 12 to 24 pt and turns red;
- the value is not written until commit;
- commit writes the value under the new `/DA`;
- undoing the command mid-edit repaints with the old `/DA` and keeps the
  buffer.

Pinned by `FormFieldDefaultAppearanceEditPaint.SetterMidEditIsPaintedLiveAndKeepsTheBuffer`,
`FormFieldDefaultAppearanceEditPaint.CommandAndItsUndoMidEditRepaintLive` and
the probe `FormTextfieldFontState.MidEditFontChangeKeepsTheBufferAndCommitUsesNewDa`.

The MainWindow-level hazard was not addressed in phase 4: a page-content
refresh calls `FormTool::onAnnotationsChanged`, which ends the edit. Phase 5
handles it; see "Changing the font from the panel while typing" below.

### Limits (untested unless named)

- **Widget indices.** Like every form command, `SetFieldAppearanceCommand`
  identifies its field by widget index. Deleting or restoring other fields
  between the change and its undo can point the index at a different field.
  This is a shared, pre-existing limitation and is untested.
- **Field flags.** `restoreFormField` still does not restore `/Ff` (multiline,
  password, comb). This is pre-existing and out of scope for GitHub #460. It was
  observed by reading the code and is untested.
- **Button `/DA`.** Only text, combo and list widgets have their `/DA`
  captured and restored. A push button's caption `/DA` is not. Untested.
- **`/Parent` cycles.** A `/Parent` cycle during the `/DA` lookup makes
  MuPDF's `pdf_dict_get_inheritable` throw. The read catches that and
  captures nothing. Found by review, untested.

## Resolution (UI layer)

Fixed in the UI layer in pipeline form-textfield, phase 5 (same branch).
All 7 probes above are green, including the two UI probes
`FormTextfieldPanelState.PropertiesPanelExposesFontControls` and
`FormTextfieldToolbarState.TextEditToolbarIsHiddenUnderTheFormTool`. The new
tests are in `tests/test_form_textfield_ui.cpp`: the suites
`FormFieldFontPanel`, `FormFieldFontMainWindow` and `FormFieldFontToolbar`.

As in the model-layer section, each statement names the test that pins it,
or says that it is untested.

### The Font group in Form Properties

`FormFieldPropertiesPanel` has a **Font** group with three controls:

| Control | objectName | accessibleName |
|---|---|---|
| Family combo | `formFieldFontCombo` | Form Field Font |
| Size spin box | `formFieldFontSizeSpin` | Form Field Font Size |
| Colour button | `formFieldFontColorButton` | Form Field Font Color |

The group itself is `formFieldFontGroup`, "Form Field Font Settings".

- **Names.** Each control has its objectName and accessibleName. Each of the
  four accessible names occurs exactly once among the main window's widgets,
  and each objectName once. Pinned by
  `FormFieldFontPanel.NewWidgetsHaveObjectAndAccessibleNames` and
  `FormFieldFontMainWindow.FontControlAccessibleNamesAreUniqueInTheWindow`.
  The second test checks only these four names, not the whole window.
- **Shown for text, combo and list fields only.** The group is hidden for
  check boxes, radio buttons, push buttons, signatures and an empty panel.
  Pinned by `FormFieldFontPanel.GroupIsShownForTextComboAndListOnly`,
  `FormFieldFontPanel.ButtonFieldsHideTheFontGroupAndClickingColourDoesNothing`
  and, through a real click on the fixture's check box,
  `FormFieldFontMainWindow.ButtonFieldHidesTheFontGroup`.
- **Populated from the field's `/DA`.** Family, size and colour are read
  from the `FormFieldData` the panel is given (gray, RGB and CMYK colours;
  size 0 shows as "Auto"; a fractional size shows rounded). Pinned by
  `FormFieldFontPanel.PopulatesFamilySizeAndColourFromTheFieldsDa`, and from
  the fixture's own `/Helv 10 Tf 0 0 0 rg` by
  `FormFieldFontMainWindow.SelectingAFieldPopulatesTheFontGroupFromItsDa`.
- **Families.** Helvetica, Times and Courier (`Helv`, `TiRo`, `Cour`).
  **Decision:** Symbol and ZapfDingbats are not offered for selection,
  because text typed in either comes out as Greek letters or dingbats. They
  are added to the list only while the selected field already uses one, so
  the combo shows the field's real font. Pinned by
  `FormFieldFontPanel.FamilyListIsBase14AndOffersSymbolFontsOnlyWhenInUse`.
- **Sizes.** The spin box runs from 0 ("Auto") to 1000 pt: exactly the sizes
  the setter accepts, so it cannot produce one the setter rejects. Pinned by
  `FormFieldFontPanel.SizeSpinIsBoundedToTheSettersRange`. **Decision:** it
  is an integer spin box. A fractional size from a file is shown rounded but
  not rewritten unless the user commits a different size, because the other
  two controls take the size from the document (see "One command per change"
  below).
- **Colour.** The button opens `QColorDialog` without an alpha channel.
  Cancel, a translucent colour and the colour already shown change nothing.
  Pinned by `FormFieldFontPanel.EachControlEmitsExactlyOncePerCommittedChange`
  through the picker seam `setColorPickerForTesting`. The real dialog is
  **untested**: it is modal and cannot be answered headlessly.

### One command per change

The panel emits `fieldFontChangeRequested` once per committed change. It
emits on a family selection, on a size commit (`editingFinished`: Enter or
leaving the box, not each arrow step) and on a chosen colour. It never emits
while it is being populated or for the value already shown. Pinned by
`FormFieldFontPanel.EachControlEmitsExactlyOncePerCommittedChange`: eight
spin steps followed by one Enter give one request, and a second Enter gives
none.

`MainWindow` turns each request into one `SetFieldAppearanceCommand` on the
undo stack, then calls `refreshPage`. Each control adds exactly one undo
step, and undo reverts it. Pinned by
`FormFieldFontMainWindow.EachControlIssuesOneCommandAndUndoRevertsIt`, which
also checks that redo puts the colour back.

- **Only the changed property comes from the panel.** `MainWindow` takes the
  other two from the document at the moment it issues the command. So a
  panel that shows a stale value cannot write it back. For example, after an
  undo the panel still shows the undone family, and a size change keeps the
  document's family. This is pinned in the same test.
- **Validation before the undo stack.** The font, size and colour are
  checked with the setter's own rules (`isBase14DefaultAppearanceFont`,
  `isAcceptedDefaultAppearanceSize`, valid and opaque). A request that fails
  issues no command, so the undo stack never gets a step that does nothing.
  The panel cannot produce such a request (see the family, size and colour
  tests above), so this path is a guard and **untested** on its own.
- **A stale widget index is refused.** The panel's widget index was taken
  when the field was selected. Before issuing the command, `MainWindow`
  re-reads that index and refuses, clearing the panel, unless the field there
  still has the same type, name and rectangle. An index or page past the end
  is refused the same way, without throwing. Pinned by
  `FormFieldFontMainWindow.StaleWidgetIndexIsRefused`.

### Changing the font from the panel while typing

The handler calls `refreshPage` only. It does **not** call `markPageDirty`
or `onPageContentChanged`, because those run `FormTool::onAnnotationsChanged`,
which ends the edit and clears the typed text. The field's `/DA` changes
straight away, and the live-edit overlay re-reads it on every paint (phase 4),
so the typed text is redrawn in the new font. The value is still written only
on commit.

Pinned at `MainWindow` level by
`FormFieldFontMainWindow.MidEditChangeFromThePanelKeepsTheTypingAndCommitUsesTheNewDa`:

- it clicks the fixture's FirstName field and types `XYZ`;
- it changes the family, then the size, then the colour from the panel;
- after each change, the edit is still running and the buffer is `XYZ`;
- the document has the new `/DA` (Times, 18 pt, red) but not yet the value;
- three changes are three commands, and the FormTool is the same object;
- after a click outside the field, the value is `XYZ` and the `/DA` is still
  Times 18 red.

The repaint itself is pinned by the phase-4 pixel tests
(`FormFieldDefaultAppearanceEditPaint.*`). This test checks the document
and the edit state, not pixels.

### The text-edit toolbar under the Form tool

The Edit Text toolbar is now shown only while a text-edit tool exists.
`PdfCanvas::clearTextEditTool` emits `textEditToolReleased` when it destroys
one, and `MainWindow` hides the toolbar and clears its font info on that
signal. Every annotation-tool switch destroys the text-edit tool inside
`PdfCanvas::setCurrentTool`, so this one place covers the Form tool and every
other tool, including tools installed from menus. Choosing Edit Text again
shows the toolbar.

- Pinned by the probe
  `FormTextfieldToolbarState.TextEditToolbarIsHiddenUnderTheFormTool`.
- Also pinned by
  `FormFieldFontToolbar.HiddenAndClearedUnderTheFormToolAndRestoredUnderTextEdit`:
  the size reads 0 and the family is empty after the switch, and the toolbar
  is back under Edit Text.
- Also pinned by `FormFieldFontToolbar.HiddenOnEveryOtherToolSwitchPath`,
  which goes from Edit Text to Forms > Create Field, Ink, Highlight, Select
  and Image, and back to Edit Text after each.

**Decision:** the toolbar is hidden rather than routed to the selected
field. The Font group in Form Properties is the one place where a form
field's font is set, and its families are limited to base-14. The toolbar's
family combo lists every installed font, which a form field's `/DA` cannot
name without embedding.

### Security review (phase 5)

- **Colour dialog input.** Only a valid, opaque colour is used. It is
  converted to RGB, and only its components reach the document (through the
  setter's own colour validation). The button's swatch style sheet is built
  from `QColor::name()` (`#rrggbb`), never from document text. A `/DA`
  colour with an out-of-range or NaN component is clamped before display.
  Pinned by `FormFieldFontPanel.EachControlEmitsExactlyOncePerCommittedChange`
  (cancel, translucent) and
  `FormFieldFontPanel.PopulatesFamilySizeAndColourFromTheFieldsDa` (clamping).
- **Spin bounds.** 0 to 1000, the setter's range. A hostile size from the
  document (1e9, NaN) is shown inside the range. Pinned by
  `FormFieldFontPanel.SizeSpinIsBoundedToTheSettersRange`.
- **Null page or field.** The handler returns early with no document, form
  manager, canvas or undo manager, or for a page index out of range. A
  widget index that no longer exists is caught (`getFormField` throws) and
  refused. Pinned by `FormFieldFontMainWindow.StaleWidgetIndexIsRefused` for
  the index and page cases. The no-document case is found by review and
  untested.
- **Stale index between selection and command.** Refused, as above. The
  identity check compares type, name and rectangle. Two fields with the same
  name, type and rectangle on one page would pass it; that is untested.
- **The undo-time index** is the shared, pre-existing limit listed under the
  model layer: a later delete or restore of another field can still move an
  index between the command and its undo.

### Limits (phase 5)

- **The panel is not re-read after undo or redo.** After an undo, the Font
  group can still show the undone value until the field is selected again.
  It never writes that stale value back (pinned, see above).
- **A `/DA` naming a `/DR` font.** MuPDF parses such a font as `Helv`, so the
  combo shows Helvetica. Any font change from the panel rewrites the `/DA`
  with a base-14 font. Found by review, untested at UI level.
- **The real `QColorDialog`** is untested (modal). The tests use the picker
  seam.
- **Entering interaction mode.** When this section was written, the Form tool
  left field-creation mode by itself after a field was placed (GitHub #369).
  Since the merge of origin/master, placement is sticky: the tool stays in
  creation mode until Esc or a tool switch, and then clicks select fields. The
  `MainWindow` tests put the tool into interaction mode directly
  (`setCreationFieldType(Unknown)`), which is the state after Esc.

## Reconciliation with origin/master (pipeline form-reconcile)

origin/master fixed font settings for form fields in parallel with this
branch. It had its own /DA setter, its own undo command, a font row in Form
Properties with bold and italic check boxes, and its own `MainWindow` handler.
The branch merged origin/master (merge commit fc98acc9). Both setters had the
same name and arguments and differed only in return type, so keeping both was
a compile error, and keeping both UIs would have put two font rows in the
panel. The merged tree has exactly one of each piece. Everything described in
the two sections above is that one piece, with the additions listed here.

### One API, one command, one panel

- **API.** One `FormManager::setFieldDefaultAppearance(page, widgetIndex,
  font, size, color)`, returning `bool` and validating everything listed in
  "What is rejected". master's overload (which returned the previous `/DA` and
  wrote any font name and any size, NaN included), its raw string writer
  `setFieldDefaultAppearanceString` and `base14FontName` were removed.
  master's read-only accessor `getFieldDefaultAppearance` is kept.
  - **Added from master:** if MuPDF cannot generate the appearance after the
    `/DA` is written, the setter raises `/AcroForm /NeedAppearances true` so
    another viewer regenerates it. This happens for a widget whose `/RV` is
    nested deeper than MuPDF's XML parser accepts. Pinned by
    `FormFieldReconcileNeedAppearances.SetterRaisesNeedAppearancesWhenTheAppearanceCannotBeGenerated`,
    with the control
    `FormFieldReconcileNeedAppearances.SetterLeavesTheFlagAloneWhenTheAppearanceIsGenerated`.
- **Command.** One `SetFieldAppearanceCommand`. master's
  `SetFieldDefaultAppearanceCommand` was removed with no alias, and its test
  was ported to the kept class. The kept command now also reports `failed()`
  for a refused request, so `UndoManager` does not push it. Pinned by
  `FormFieldReconcileCommand.RefusedRequestReportsFailedAndIsNotAnUndoStep`.
- **Panel.** One Font group (the table above), one signal,
  `fieldFontChangeRequested`, and one slot,
  `MainWindow::onFormFieldFontChanged`, which carries the handler described
  above. master's `fieldFontChanged` signal, its font row (`QFontComboBox` of
  all system fonts, a fractional size box that emitted on every step, no
  colour) and its handler (which used the current page rather than the
  field's page and forced the colour to black) were removed.
  - **Added from master:** for a read-only field the family combo, size spin
    box and colour button are disabled, and the panel emits nothing on any
    path. Pinned by
    `FormFieldReconcilePanel.ReadOnlyFieldEmitsNothingOnAnyFontPath`, with the
    control `FormFieldReconcilePanel.EditableFieldEmitsOncePerPathUnderTheSameDrive`,
    and by master's ported
    `FormFieldPropertiesPanelTest.ReadOnlyTextFieldFontControlsDisabled`.

### Undo never merges

master's command merged every consecutive change to one widget into one undo
step: a family change and a colour change made minutes apart undid together.
It also restored only the `/DA`, so the `/DS` and `/RC` that MuPDF's setter
deletes were lost for good. The merged tree keeps this branch's rule: each
committed change is one undo step, and undo restores `/DA`, `/DS` and `/RC`
exactly.

- master needed merging because its panel emitted on every spin-box step. The
  kept panel emits once per committed change, so there is nothing to absorb.
- Merging is unsafe here: `UndoManager::execute` runs a declined merge
  candidate's `execute()` twice, and this command takes its undo snapshot in
  `execute()`. Pinned by
  `FormFieldReconcileCommand.UndoManagerExecutesADeclinedMergeCandidateTwice`.
- Pinned by `FormFieldReconcileCommand.ConsecutiveTweaksToOneFieldAreSeparateUndoSteps`
  and `FormFieldDefaultAppearanceModel.CommandsNeverMergeSoEachUndoRevertsOneChange`.

### Bold and italic are not offered

master offered bold and italic and wrote style-variant keys such as `/HeBo`
into the `/DA`. The merged tree offers neither, and the setter refuses those
keys with the document unchanged.

The reason is the bundled MuPDF 1.26.10. Its `/DA` parser maps every font
name other than `Helv`, `TiRo`, `Cour`, `Symb` and `ZaDb` to `Helv`, and the
appearance writer loads its font through the same kind of mapping. A field
whose `/DA` says `/HeBo` therefore renders, and reads back, as regular
Helvetica. Offering the check box would show "bold" in the panel and regular
text on the page, and a viewer that regenerates appearances might show bold,
so the field would look different in different viewers.

- Pinned by `FormFieldReconcileStyle.MuPdfRendersAStyleVariantDaAsTheRegularFace`:
  `/HeBo`, `/HeOb`, `/HeBO`, `/TiBo`, `/TiIt` and `/CoBo` each render
  pixel-identical to `/Helv`, while `/TiRo` differs. If a future MuPDF renders
  the variants, this test goes red, and bold and italic should be revisited.
- Pinned by `FormFieldReconcileStyle.StyleVariantKeysAreRejectedAndTheDocumentIsUnchanged`
  and master's ported `FormCreationTest.Base14FontNameMapping`.
- The planner's acceptance criterion for the reconciliation asked for bold and
  italic through the variant keys. This is a recorded deviation from it, for
  the reason above.

### FormFieldData

master added an uncapped `FormFieldData::defaultAppearance` copy of the `/DA`,
read for every widget on every overlay repaint. It duplicated this branch's
capped `defaultAppearanceString` and was removed. Pinned by
`FormFieldReconcileData.FormFieldDataHasNoUncappedRawDaCopy`, which makes
re-adding it a compile failure.

### The text-edit toolbar

master turned the form, main and annotation toolbars into wrapping toolbars.
That touched different objects from this branch's toolbar change, so both are
kept unchanged: the Edit Text toolbar is still hidden under the Form tool.
