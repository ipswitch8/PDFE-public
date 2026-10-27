# Form Fixture Debt — `test_form_with_dv.pdf`

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
> See [`docs/TRACKING.md`](TRACKING.md) for why, and for the parts of this repo that are
> operationally live rather than historical.

---


**Status:** open
**Discovered:** phase-6 (P1 Form Field Coverage)
**Blocks:** 5 phase-6 rows (`field_*_set_default` for Text, Checkbox,
RadioButton, ComboBox, ListBox).

## Context

Phase-6's `set_default` tests exercise `Forms > Reset Form` and assert
that fields return to their default values. MuPDF's `pdf_reset_form`
only resets fields that have an explicit `/DV` (default value) entry
in their field dictionary — fields with only `/V` (current value) are
left unchanged. This is documented in `CLAUDE.md`:

> `pdf_reset_form` only resets fields with `/DV` (default value)
> entries; fields without DV are unchanged

`tests/fixtures/test_form.pdf` sets `/V` on several fields
(Country=USA, Interests=Programming, AgreeTerms=Off, Subscribe=Yes)
but no `/DV`. Exporting the form reflects the current `/V`, override
via XFDF import + Reset Form is a no-op, and the re-export still
shows the override value. That is correct MuPDF behavior, but it
makes the set_default test impossible to prove against this fixture.

## Required Fixture

`tests/fixtures/test_form_with_dv.pdf` with:

- **Text: FirstName** — `/V` empty, `/DV` = `"Default Name"`
- **Checkbox: AgreeTerms** — `/V` = `Off`, `/DV` = `Yes`
- **RadioButton: Gender** — `/V` empty, `/DV` = `Female`
- **ComboBox: Country** — `/V` = `Canada`, `/DV` = `USA`
- **ListBox: Interests** — `/V` = `Reading`, `/DV` = `Programming`

After Reset Form, each field's `/V` should be restored to its `/DV`.

## Generation Approach

Use `tests/fixtures/generate_test_form.py` (if it exists) or extend
it. PyPDF / pikepdf can set `/DV` directly on form field objects:

```python
import pikepdf
from pikepdf import Name
pdf = pikepdf.open("test_form.pdf")
acroform = pdf.Root.AcroForm
for field in acroform.Fields:
    name = str(field.T)
    if name == "FirstName":
        field[Name("/DV")] = "Default Name"
    # ... per-field DV assignments
pdf.save("test_form_with_dv.pdf")
```

## Follow-up Work When Fixture Exists

In `tests/ui_automation/test_form_fields_p1.py`:

1. Change `FIELD_MAP` entries to use `test_form_with_dv.pdf` for
   set_default ops (keep `test_form.pdf` for fill_value / xfdf_roundtrip
   which don't depend on `/DV`).
2. Remove the blanket blocked return in `_op_set_default` and restore
   the original baseline-export → override → Reset Form → export path.
3. Assert that post-Reset field value equals the known `/DV` from the
   fixture (not the `/V` baseline, which has been changed by the
   override step).
4. Re-run phase-6; set_default rows should flip from blocked to green.

## Tracking

This file is the single source of truth for the debt. Referenced from:

- `tests/ui_automation/test_form_fields_p1.py` `_op_set_default` docstring
- Phase-6 karen review findings (CONDITIONAL FAIL → PASS, Item 3)
