# Manual Verification & Testing Guide

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


**Scope:** every feature shipped in PDFE that still needs at least one
human-driven check before it can be called "verified end-to-end."

The C++ GoogleTest suite covers data-layer correctness. This document is the
bridge between that and shipping confidence — it lists the gaps the unit tests
don't cover and the exact steps to close them.

Organized by what needs to be verified, roughly in priority order.

> **Re-verified end to end on 2026-08-03 (meta-audit follow-up F19, gaps
> A5-32, A5-35, A5-36, A5-38, A5-39, A5-42, A5-43, A5-45, A5-46, A5-47).**
> This document had not been touched since 2026-04-23 and had drifted badly.
> Corrections made in that pass, each verified against the tree:
>
> | claim | was | is |
> |---|---|---|
> | suite size (this paragraph) | "2861/2861 green at last run" | removed — a run record does not belong in a checklist, and this one was ~2400 cases stale |
> | §0 matrix precondition | "Should print `450` rows" | **787** |
> | §2 heading | "Pending matrix rows (12 left)" | **zero rows have status `pending`; all 12 named rows are `green`** |
> | §3 export-plain-text | "3 cases" | 9 |
> | §3 create-from-images | "7 cases" | 13 |
> | §3 forms palette | "6 cases" | 12 |
> | §4/§6 OCR | "the one disabled unit test `DISABLED_OcrRecall`" | **no `DISABLED_` test exists anywhere in the suite**; the test is live as `E2EOcr.CleanScanRecallAboveNinetyFivePercent` |
> | §4 OCR variants | skewed + multicolumn "remain manual" | **both are automated**, in `test_fidelity_r3_08_ocr_wer.cpp` and `test_layout_extraction_corpus.cpp` |
> | §2 fixture | `tests\fixtures\test_attachments.pdf` | **that file does not exist** — see the note in §2 |
>
> **Every checklist item now carries an automation disposition** — see
> [§8 Automation disposition](#8-automation-disposition-f19) at the end. An
> item is either automated (with the test named, and that test verified to
> exist and be registered) or recorded as manual **with the written reason it
> cannot be automated**. `F19DocClaims.ManualVerificationDocMatchesTheTree`
> re-derives the matrix row count and the OCR test's live name and fails if
> this document drifts from them again.

---

## 0. Prerequisites

### Environment

- **Build:** `build\vs2022\bin\Debug\PDFE.exe` (or the Release equivalent)
  must exist. Run `cmake --build build\vs2022 --config Debug --target PDFE`
  if the binary is missing.
- **Python 3.10+** with these packages:
  ```
  pip install pywinauto pyautogui cryptography asn1crypto pyhanko
  ```
- **A clean `%APPDATA%` area** — several tests read/write QSettings
  under `PDFE-Test` and `PDFE`. If you've been running unit tests,
  purge `%APPDATA%\PDFE-Test` before interactive runs to avoid stale
  preferences leaking in.
- **A 1080p display or larger** — several dialogs assume enough
  vertical space for their full form layout. Pywinauto scripts use
  the window's current geometry for hit-testing.

### Fixtures

All required fixtures are under `tests\fixtures\`. The CMake configure
step auto-regenerates the TSA / portfolio / link-types fixtures when
they're missing (e.g., after a `git clean`) via `execute_process()` +
the Python generator scripts. For fixtures the CMake rule doesn't
handle, run the generator manually:

```
cd tests\fixtures
python generate_ca_signed_pdf.py
python generate_ocr_variants.py
python generate_pdfa_fixtures.py
```

Auto-regenerated on configure (no manual step needed unless the
Python deps aren't installed):

- `test_timestamped.pdf` + `test_tsa_token.der` + `test_tsa_cert.pem`
- `test_portfolio.pdf`
- `test_link_types.pdf`

If CMake warns that Python3 isn't found or regeneration failed, install
the Python deps listed above — the affected tests will `GTEST_SKIP`
with clear messages naming the generator to run.

Confirm with:

```
cd tests\ui_automation
python -c "import csv; print(sum(1 for _ in csv.DictReader(open('matrix/ui_test_matrix.csv', encoding='utf-8'))))"
```

Should print **787** rows.

*(F19: this said `450`. The matrix has grown to 787 data rows — 750 green /
33 wont-fix / 0 planned / 3 xfail / 0 skip / **1 blocked**. A precondition
check that fails on a correct tree teaches people to ignore preconditions.)*

---

## 1. Pywinauto scripts that have never run against a live PDFE.exe

These scripts are written and their matrix rows are marked `green`,
but they skip in this environment because `PDFE.exe` isn't in the
expected path. Each needs to be executed once interactively to
confirm the underlying UIA tree matches the script's assumptions.

### Run order

From `tests\ui_automation\`:

```
pytest -v test_preflight_phase_post.py
pytest -v test_forms_palette_phase_post.py
pytest -v test_follow_link_phase_post.py
pytest -v test_misc_phase_post.py
pytest -v test_broader_phase_post.py
```

### Per-test manual expectations

| Test | Expected outcome | Red flag to watch for |
|---|---|---|
| `test_preflight_dialog_opens` | Preflight dialog appears, has 6 profiles | Dialog's QComboBox items empty |
| `test_preflight_profile_combo_present` | All 6 profile strings present | Missing "PDF/UA-1" |
| `test_preflight_run_button_exists` | Run button is enabled on dialog open | Stays disabled |
| `test_form_palette_action_checks_exclusively` (×7) | Each palette action is reachable + enabled | `formRadioButtonAction` / `formPushButtonAction` / `formSignatureAction` unreachable ⇒ wiring regression |
| `test_tab_order_dialog_drag_reorder` | Dialog opens, fieldList is reachable | Skipped "not reachable" ⇒ menu path differs from expected |
| `test_follow_goto_link_navigates_to_target_page` | `pageInfoLabel` shows page 3 after click | Still shows page 1 ⇒ link hit-test or dispatch broken |
| `test_follow_named_nextpage_link_advances_one_page` | Page advances from 1 to 2 | No change ⇒ Named-action slot not wired |
| `test_follow_uri_link_dispatches_without_crashing` | Window stays responsive | Hang / crash ⇒ `QDesktopServices::openUrl` exception not swallowed |
| `test_follow_launch_link_prompts_confirmation` | A dialog with "Launch" in its title appears | No dialog ⇒ security prompt missing (critical — would mean auto-execute) |
| `test_open_portfolio_pdf` | Window title contains "portfolio" | Title shows the wrapper page name only |
| `test_sign_dialog_exposes_invisible_checkbox` | `invisibleCheck` visible, toggle-able | Checkbox missing ⇒ `SignDialog` regression |
| `test_sign_dialog_exposes_certification_combo` | Combo has exactly 4 items | Item count ≠ 4 ⇒ new cert level added without matching enum |
| `test_preflight_run_pdfa1b` | After Run, issue tree exists | Never populates ⇒ `ComplianceManager::validatePdfA` threw silently |
| `test_preflight_export_report_button_enables_after_run` | Export button is disabled before Run, enabled after | Never enables ⇒ `ComplianceResult::issues` empty (test PDF should have issues) |
| `test_calc_order_dialog_open` | Dialog opens with `calculationOrderFieldList` visible | Empty list on `test_form.pdf` ⇒ `listFieldNames` not finding /Fields |
| `test_calc_order_dialog_move_down_reorders` | Move-down button is reachable | Skipped ⇒ list has <2 named fields in fixture |
| `test_theme_switch` (×3 parametrized) | Each menu path succeeds | Menu path not reachable ⇒ theme submenu not where expected |
| `test_preferences_ocr_language_change` | `ocrLanguageCombo` reachable, can select "Français (fra)" | Combo missing ⇒ `PreferencesDialog` regression |
| `test_alt_text_dialog_open_via_menu` | Dialog opens or skips gracefully | Crash ⇒ regression in `AltTextDialog` construction |
| `test_view_rotate_cw_90` | Menu dispatch succeeds, window stays responsive | Canvas fails to repaint ⇒ rotation pipeline issue |

### What to do if a test fails

1. **Re-run with `-v -s`** to see stdout/stderr interleaved with pytest logs.
2. **Check the menu path.** Many tests try 2-3 path variants and skip
   on none-matching. If the menu layout changed, the script needs a
   new path added to the `for path in (...)` tuple.
3. **Use `pywinauto.print_control_identifiers()`** in an interactive
   REPL on the live window to dump the UIA tree. Confirm the
   `auto_id` the test uses actually exists.
4. **If a test hangs for >30s**, kill PDFE.exe manually (Task
   Manager). A hang usually means a modal dialog appeared that
   the test wasn't expecting — the pywinauto window handle became
   unresponsive.

---

## 2. Interactive-session spot-checks (formerly "pending matrix rows")

> **Corrected 2026-08-03 (F19).** This section was headed *"Pending matrix
> rows (12 left)"* and said these rows *"are status `pending`"*. **No row in
> `ui_test_matrix.csv` has status `pending` — the value is not in use at all**
> (the live set is `green`, `wont-fix`, `planned`, `xfail`, `skip`). All 12
> rows named below are **`green`**. The workflow in §7 that talks about
> *"promoting `status` from `pending` to `green`"* is likewise describing a
> transition that no longer exists.
>
> The steps below are still worth running as **manual spot-checks** — several
> of these rows are green under automation that does not inspect the resulting
> file — so the section is retained and re-framed rather than deleted. Per-row
> automation status is in [§8](#8-automation-disposition-f19).
>
> **Broken fixture reference:** two rows below tell you to open
> `tests\fixtures\test_attachments.pdf`. **That file does not exist**
> (`ls tests/fixtures/ | grep -i attach` → nothing, and §0 does not list it as
> auto-generated). Use an embedded-files fixture from
> `tests/fixtures/realworld/embedded_files/` or
> `tests/fixtures/flaw_corpus/embedded/` instead, or generate one.

These rows in `tests\ui_automation\matrix\ui_test_matrix.csv` require
multi-step file-dialog walks, filesystem inspection of output PDFs, or
per-annotation right-click priming — all of which are painful to automate and
were originally intended to be done once by hand.

### Row-by-row steps

#### `recent_files_populates_on_open`

1. `rm %APPDATA%\PDFE\PDFE.ini` to start with an empty recent-files list.
2. Launch PDFE.
3. File → Open → pick `tests\fixtures\test_3page.pdf`.
4. Close the document (File → Close).
5. File → Open Recent → should list `test_3page.pdf` at the top.

#### `recent_files_clear`

1. Follow the previous test so at least one entry is present.
2. File → Open Recent → Clear.
3. File → Open Recent → menu is empty (only the "Clear" entry remains).

#### `alt_text_dialog_save`

1. Open `tests\fixtures\test_single.pdf`.
2. Activate any annotation tool (e.g., Sticky Note).
3. Place a note on the canvas.
4. Right-click the note → Alt Text... (or Tools → Alt Text... with
   the note selected).
5. Type "sample alt text" in the alt-text field, click Save.
6. Close and reopen the file.
7. Right-click the note → Alt Text... → field should show "sample alt text".

#### `attachment_panel_list`

1. Open `tests\fixtures\test_attachments.pdf`.
2. View → Attachments Panel (or the corresponding toggle).
3. Panel should list every `/EmbeddedFiles` entry — name, size,
   mime-type, created/modified timestamps.

#### `attachment_panel_add`

1. Open any PDF.
2. Open the Attachments Panel.
3. Click Add → pick any file (e.g., a `.txt`).
4. New row appears. Save the PDF. Reopen. Row still present.

#### `attachment_panel_extract`

1. Open `tests\fixtures\test_attachments.pdf`.
2. Select any row.
3. Click Extract → pick an output directory.
4. Verify the extracted file appears in that directory with bytes
   matching the original (use `certutil -hashfile <path> SHA256` on
   both the extracted file and the original to compare).

#### `compliance_dialog_pdfa_convert`

1. Open `tests\fixtures\test_3page.pdf`.
2. Tools → Compliance... (or equivalent menu path).
3. Pick "Convert to PDF/A-2b", click Convert, choose an output path.
4. Open the resulting PDF with MuPDF's `mutool info` CLI or with a
   PDF/A validator (e.g., veraPDF):
   ```
   mutool info out.pdf | grep -i pdfa
   ```
   Should report PDF/A-2b conformance.

#### `compliance_dialog_pdfua_convert`

1. Open a tagged PDF.
2. Tools → Compliance... → Convert to PDF/UA → save.
3. Open the output. Metadata panel should show `/UA1` conformance.
4. Or: `mutool info` and look for `/MarkInfo` + `/StructTreeRoot`.

#### `view_rotate_180`

1. Open `test_3page.pdf`.
2. View → Rotate → 180° (or Ctrl+Shift+R if bound).
3. Page renders upside-down.
4. View → Reset Rotation → returns to upright.

#### `header_footer_dialog_apply`

1. Open `test_3page.pdf`.
2. Tools → Add Header & Footer...
3. Type "Confidential" in a header slot, "{page}/{pages}" in a footer slot.
4. Click Apply.
5. Every rendered page should show the header and footer. Save + reopen
   to confirm persistence.

#### `header_footer_dialog_bates`

1. Same as above, but enter `{bates:ACME:1000:6}` in a slot.
2. Click Apply.
3. Page 1 should show "ACME001000", page 2 "ACME001001", page 3 "ACME001002".

#### `calc_order_dialog_apply_persists`

1. Open a form PDF with calc-dependent fields.
2. Tools → Forms → Calculation Order.
3. Check two fields, drag to a specific order, click Apply.
4. Close, reopen. Reopen the Calc Order dialog. The previously-
   checked fields should still be at the top in the chosen order.
5. Verify on disk: run `mutool show <file> trailer/Root/AcroForm/CO`
   — the array should list the field objects in the chosen order.

---

## 3. Features with code + unit tests but no interactive check

These have C++ test coverage for the data layer but no human has
exercised the full "user clicks → real file written / rendered"
path. Each needs one manual session.

### Preflight report export

**Covered by:** `test_preflight_dialog.cpp` (10 cases — synthetic
ComplianceResult).
**Gap:** real file save path.
**Steps:**

1. Open `test_3page.pdf` (not PDF/A — validator will find issues).
2. Tools → Preflight → pick PDF/A-2b → Run.
3. Export Report → choose a path.
4. Open the written `.txt` file: should have a header line, counts,
   and one line per issue with `[ERROR]` / `[WARNING]` prefix and
   ISO rule + page number + description.

### Navigation history across a real document

**Covered by:** `test_navigation_history.cpp` (11 unit cases).
**Gap:** MainWindow wiring — does `onPageChanged` actually trigger
recording, does `View → Previous View` actually navigate?
**Steps:**

1. Open `test_3page.pdf`.
2. Jump around: page 3 → page 1 → page 2 → page 3.
3. View menu: Previous View / Next View should be enabled.
4. View → Previous View: canvas returns to page 2.
5. Alt+Left: canvas returns to page 1.
6. Alt+Right: canvas returns to page 2.
7. Navigate to page 3 now: forward history is truncated. Next View
   becomes disabled.
8. File → Close → reopen same file: nav history is empty.

### Export Plain Text to a real file

**Covered by:** `test_export_plain_text.cpp` (**9** cases; this said 3 — corrected F19). Action state is covered; **no test reads back the exported file's bytes**, so the BOM and form-feed expectations below are genuinely unasserted.
**Gap:** actual file contents.
**Steps:**

1. Open `test_3page.pdf`.
2. File → Export Plain Text → save as `out.txt`.
3. Open `out.txt` in Notepad. Should see the BOM rendered as nothing
   (Notepad auto-detects UTF-8) and text from all 3 pages separated
   by visible `¤` or `␌` characters (some editors show form-feed
   literally; vscode shows a small square).
4. Line-ending-sensitive: open in Notepad++ with "Show all characters"
   — verify `FF` byte between page 1's text and page 2's text.

### Create PDF from Images — interactive flow

**Covered by:** `test_image_to_pdf_wizard.cpp` (**13** cases; this said 7 —
corrected F19), all via `writePdf()` headless, including corrupt / oversized /
zero-image / extreme-aspect boundaries and the `kMaxPageUnits = 14400.0` clamp.
**Gap:** the `runInteractive` flow with two QFileDialogs.
**Steps:**

1. Place 3 image files of different sizes in a folder (e.g., a
   small PNG, medium JPEG, large TIFF).
2. File → Create PDF from Images...
3. Multi-select all 3 files in the picker.
4. Pick an output path (e.g., `test.pdf`).
5. After wizard completes, PDFE should open the new PDF automatically
   (via `openFile` hook).
6. Each page should be sized to its source image's pixel dimensions
   at 72 DPI (the 300×200 image → 300pt × 200pt page).

### Follow-link viewer dispatch

**Covered by:** `test_follow_link.cpp` (6 cases — calls `followLinkAt`
directly, bypassing canvas click).
**Gap:** the `canvasClicked` signal → `followLinkAt` slot path.
**Steps:**

1. Open `tests\fixtures\test_link_types.pdf`.
2. **Do not activate any tool.** The canvas needs to be in its
   default "pan" state.
3. Click on the URI link rectangle (top-left, around y=692 in PDF
   coords which is near the top of the page). Your default browser
   should open `https://example.test/follow-link-uri`.
4. Click on the GoTo link (right of URI). Canvas should jump to page 3.
5. Click on the Named NextPage link (below URI). Canvas should advance
   one page.
6. Click on the Launch link (bottom-right). A QMessageBox should
   appear warning about Launch risk — click Cancel. **No file should
   be opened.**

### Forms palette click → FormTool::setCreationFieldType

**Covered by:** `test_forms_designer.cpp` (**12** cases; this said 6 — corrected F19 — verifies action
presence + QActionGroup exclusivity).
**Gap:** confirming that clicking Radio/PushButton/Signature actually
causes the FormTool to enter that specific creation mode.
**Steps:**

1. Open `test_form.pdf`.
2. Tools → Forms → Create Field.
3. Click the Radio Button action in the form toolbar.
4. Drag-draw a rectangle on the canvas.
5. The new field should appear as a radio button (circular widget),
   not a text field or checkbox.
6. Repeat for Push Button (should be a clickable button style) and
   Signature (should be a signature field outline).

### SignDialog invisible + certification controls

**Covered by:** `test_sign_dialog.cpp` + `test_invisible_and_certification_signatures.cpp` (via direct `SignatureManager::signField`).
**Gap:** end-to-end dialog-driven sign with the new options.
**Steps:**

1. Open `tests\fixtures\test_unsigned_sigfield.pdf`.
2. Select the signature field → right-click → Sign.
3. In SignDialog:
   - PFX: `tests\fixtures\test_signer.pfx`
   - Password: `test123`
   - Check "Invisible signature"
   - Certification: pick "Certify — no changes allowed"
4. Click Sign.
5. Output PDF on disk:
   - `mutool show <file> trailer/Root/Perms/DocMDP` → should return a
     dict with `/Type /Sig`.
   - `mutool show <file> trailer/Root/AcroForm/Fields/0/V/Reference/0/TransformParams/P` → should be `1`.
   - `mutool show <file> trailer/Root/AcroForm/Fields/0/Rect` → should
     have zero area (all four coords equal, or width/height=0).

---

## 4. Visual-only features (require human eyeballs)

Automation can't assert "this looks right" — these need a sanity
pass by hand.

### Theme switching

1. View → Theme → Dark: all panels (page panel, properties, search,
   bookmark, comment) visibly re-colored to a dark palette. Text is
   readable. No white flashes on dialog open.
2. View → Theme → Light: reverts.
3. View → Theme → System: matches the OS current theme.
4. Close PDFE. Reopen. Theme persists across sessions.

### Atlas mode comparison (image-dominated pages)

1. Open a scanned PDF fixture (e.g., `test_ocr_skewed.pdf` or any
   scan you have).
2. Activate Text Edit Tool.
3. Click into a text span.
4. A prompt appears offering Scan Glyphs vs FreeType Styled mode
   for the first click.
5. Pick "Compare..." — a side-by-side dialog should show both
   renderings so you can pick which is more faithful to the scan.
6. Verify that Scan Glyphs preserves the distinctive font shape
   better than FreeType Styled on cursive / handwritten samples.

### Print preview

1. File → Print Preview.
2. Preview window shows pages at the chosen paper size.
3. Zoom controls work.
4. Print to PDF (Windows' built-in virtual printer) → output matches
   what's on screen.

### OCR accuracy on real scans

1. Open each of:
   - `test_ocr_clean.pdf` — should recognize cleanly
   - `test_ocr_skewed.pdf` — rotation tolerance
   - `test_ocr_multicol.pdf` — column detection
2. Tools → OCR Current Page (or OCR All Pages).
3. Select text after OCR completes. Selected text should match the
   rendered image's text.
4. **Expected recall threshold:** ≥95% for clean, ≥80% for skewed.

> **Corrected 2026-08-03 (F19, gap A5-45).** All three variants named above
> are **already automated** — this section had them down as visual-only:
>
> | fixture | automating test | file |
> |---|---|---|
> | `test_ocr_clean.pdf` | `E2EOcr.CleanScanRecallAboveNinetyFivePercent` (asserts `EXPECT_GE(recall, 0.95f)` against the authored `.expected.txt`) | `tests/test_e2e_integration.cpp:260` |
> | `test_ocr_skewed.pdf` | the rotated/noisy WER case | `tests/test_fidelity_r3_08_ocr_wer.cpp:316` |
> | `test_ocr_multicol.pdf` | column-detection / layout extraction | `tests/test_layout_extraction_corpus.cpp:162-163,256` |
>
> All three fixtures and their `.expected.txt` sidecars exist and are guarded
> by `FixtureCorpus.OcrVariantsEachHaveExpectedTextSidecar`
> (`tests/test_fixture_corpus.cpp:122`). The reference to a
> `DISABLED_OcrRecall` case is stale — **the suite contains zero
> `DISABLED_`-prefixed tests**.
>
> **What remains genuinely manual here** is only step 3 as a *human*
> judgement — that the selected text visually corresponds to the ink on the
> page — which the recall metrics already quantify. Treat this section as an
> optional sanity pass, not as coverage that is missing.

### Font rendering on scanned pages

1. Edit text on an image-dominated page.
2. Typed characters should visually blend with surrounding scanned
   text. If atlas mode is Scan Glyphs, the glyph shapes should match
   — same x-height, stroke weight, slant.
3. If the result looks jarring (wrong font family / weight), try
   switching atlas modes.

---

## 5. Features needing external infrastructure

These need something the test environment can't provide.

### Real RFC-3161 TSA round-trip

**Covered by:** cached-bytes fixture (`test_tsa_fixture.cpp`, 6 cases).
**Gap:** actual network TSA query.
**Steps (optional, only if testing real TSA interop):**

1. Open a PDF with a signable field.
2. SignDialog → fill PFX + password.
3. Check "Include TSA Timestamp" → URL: `http://timestamp.digicert.com`
   (or any public free TSA).
4. Sign.
5. Open the signed PDF. `mutool show <file> trailer/Root/AcroForm/Fields/0/V/TS` should be present.
6. Expect: this will fail on restricted networks. Document whatever
   TSA you use so the next person can reproduce.

### CA-chain trust validation

**Covered by:** `generate_ca_signed_pdf.py` fixture generation; C++
verification tests with self-signed CA.
**Gap:** verification against the OS trust store.
**Steps:**

1. Sign a PDF with a certificate whose issuer chains to a Windows
   trusted root (e.g., a DigiCert code-signing cert if you have one,
   or generate a test CA and add it to `certmgr.msc` → Trusted Root
   Certification Authorities).
2. Open the signed PDF.
3. Signature panel should show "Valid — chains to trusted root"
   rather than "Self-signed — not trusted."

---

## 6. ~~The one disabled unit test~~ — RESOLVED, section obsolete

> **Corrected 2026-08-03 (F19, gap A5-45).** There is no disabled unit test.
> **The suite contains zero `DISABLED_`-prefixed tests**
> (`pdfe_tests.exe --gtest_list_tests | grep DISABLED_` → no macro-position
> hits; `grep -ohE 'TEST(_F|_P)?\([^)]*DISABLED_' tests/*.cpp | wc -l` → 0).
>
> The test this section was written about is **live** as
> `E2EOcr.CleanScanRecallAboveNinetyFivePercent`
> (`tests/test_e2e_integration.cpp:260`). It still degrades gracefully when
> Tesseract's `eng.traineddata` is unavailable — but by `GTEST_SKIP` at run
> time, not by being disabled at compile time, so it runs wherever tessdata is
> present. The instruction to "rename `DISABLED_OcrRecall` to `OcrRecall`" has
> had no referent for some time and would fail.
>
> Retained as a stub rather than deleted so the section number stays stable
> for anyone following an older copy of this checklist.

To run it: `pdfe_tests.exe --gtest_filter="E2EOcr.*"`. Expected: PASS
(recall ≥95%) when `tests\fixtures\tessdata\eng.traineddata` is present,
`SKIPPED` otherwise.

---

## 7. How to log manual verification

For each item you verify:

1. Add a row to `tests\ui_automation\matrix\ui_test_matrix.csv` if
   the item corresponds to a matrix test, promoting `status` to `green`.
   *(F19: this used to say "promoting `status` from `pending` to `green`".
   The `pending` value is not used by the matrix — the live set is `green`,
   `wont-fix`, `planned`, `xfail`, `skip`. A row awaiting work is `planned`.)*
2. Append a dated note to the row's `notes` field:
   `"Verified manually 2026-MM-DD on Win11 <your initials>"`.
3. If a test fails, update the CSV with `status=red` and a one-line
   description of what broke.
4. Write up anything non-obvious in a new `docs\manual-test-log-YYYY-MM-DD.md`
   so the next pass has a baseline.

### When a manual test reveals a real bug

1. File the issue. Include the PDF (if fixture-worthy), the exact
   steps, and the observed vs expected behavior.
2. Add a red status note to the matrix CSV.
3. If the bug is in shipped feature code, add a failing C++ test
   that reproduces it at the smallest possible layer before fixing.

---

## Summary

| Category | Items needing manual verification | Time estimate |
|---|---|---|
| Phase-post pywinauto scripts | 5 Python files, ~30 test cases | 1-2 hours |
| Pending matrix rows | 12 interactive flows | 1-2 hours |
| Code + unit-test coverage, no interactive check | 8 features | 1 hour |
| Visual-only features | 4 categories | 1 hour |
| External-infrastructure (optional) | 2 | variable |
| Enable disabled unit test | 1 | 15 minutes |

Total one-time verification pass: **~4-6 hours**. After the initial
sweep, future regressions should surface through C++ unit tests first
and only need manual verification for visual / interactive flows.

---

## 8. Automation disposition (F19)

*Meta-audit follow-up phase-19 acceptance criterion 5: **every** manual-only
checklist item in this document is either **AUTOMATED** — naming the automating
test, which was verified to exist and to be registered in the explicit list in
`tests/CMakeLists.txt` — or **re-recorded as MANUAL with the written reason it
cannot be automated**. Compiled 2026-08-03 against the working tree.*

**The reason must be a property of the check, not of our appetite for the
work.** "Nobody has written it yet" is not a reason; it makes the item
**AUTOMATABLE — NOT YET DONE**, which is recorded as an open gap below rather
than filed under "manual". Eight items fall in that class and are named.

**Item numbering** follows this document top to bottom: §0 = 1–7, §1 = 8–29,
§2 = 30–41, §3 = 42–48, §4 = 49–53, §5 = 54–55, §6 = 56. **56 checklist items
in total.**

### §0 Prerequisites (1–7)

| # | Item | Disposition |
|---|---|---|
| 1 | `PDFE.exe` exists | **N/A — build precondition**, not a behavioural check. |
| 2 | Python deps installed | **N/A — environment precondition.** |
| 3 | Purge `%APPDATA%\PDFE-Test` | **N/A — test hygiene step.** |
| 4 | 1080p+ display | **MANUAL — irreducible.** A headless process has no display geometry to assert; the constraint is on the human's monitor. |
| 5 | Fixture generators run | **AUTOMATED** — `FixtureCorpus.*`, `tests/test_fixture_corpus.cpp` (registered): fixtures exist, open cleanly, carry sidecars, and are listed in `tests/fixtures/README.md`. |
| 6 | CMake auto-regeneration | **AUTOMATED** — the `_pdfe_required_fixtures` configure-time floor in `tests/CMakeLists.txt` fails configure on absence, plus `FixtureSkipIntegrity.*` in `tests/test_fixture_corpus.cpp`. |
| 7 | Matrix row count = 787 | **AUTOMATED (new in F19)** — `F19DocClaims.ManualVerificationMatrixRowCountMatchesTheCsv`, `tests/test_f19_doc_claim_guards.cpp`. This is the item that had been wrong (450) for three months; it is the clearest case in the document of a check that should never have been manual. |

### §1 Pywinauto scripts (8–29)

**All 21 per-test expectations: AUTOMATED, but as live-UI Python, not headless
GoogleTest.** All five scripts exist under `tests/ui_automation/`. A machine
executes them and they assert; they are not runnable in the headless gate,
which is why they live in this document.

**Reason the residue cannot move to GoogleTest:** each expectation is stated
over the **UIA tree of a running `PDFE.exe`** — dialog reachability, combo item
counts, `auto_id` presence, menu-path traversal. GoogleTest here constructs
widgets in-process and never publishes a UIA tree, so the property under test —
"an external accessibility client can find and drive this control" — is not
expressible in it. That is a real difference, not a convenience: these are
exactly the regressions in-process widget tests miss.

**Environment limitation, recorded not hidden:** on the Server-2019 CI box the
UIA menu walk hangs (gap A14); menus are routed through `testToolActivatePipe`.
Items 8–28 therefore need an **interactive desktop session**, not merely a runner.

| # | Item | Disposition |
|---|---|---|
| 8 | Run the 5 scripts in order | **AUTOMATED (live-UI).** All 5 files present. |
| 9–28 | The 20 per-test expectations | **AUTOMATED (live-UI)** — each is a named test function inside those scripts. |
| 29 | Failure-triage procedure | **MANUAL — irreducible.** A debugging procedure for a human, not an assertion. |

### §2 Interactive-session spot-checks (30–41)

All 12 matrix rows are **`green`** today. This table answers whether that green
rests on an oracle that inspects the result.

| # | Item | Disposition |
|---|---|---|
| 30 | `recent_files_populates_on_open` | **AUTOMATED** — `tests/test_recent_files.cpp`, `tests/test_app_settings.cpp` (registered); live-UI leg `tests/ui_automation/test_ui_recent_files.py`. |
| 31 | `recent_files_clear` | **AUTOMATED** — same files. |
| 32 | `alt_text_dialog_save` | **AUTOMATED** — `AltTextDialogTest.SaveWritesBackToAnnotation`, `tests/test_alt_text_dialog.cpp` (registered). |
| 33 | `attachment_panel_list` | **AUTOMATED** — `tests/test_attachment_panel.cpp`, `tests/test_attachment_panel_mainwindow_integration.cpp` (registered). |
| 34 | `attachment_panel_add` | **AUTOMATABLE — NOT YET DONE.** The seam exists (`triggerAddForTesting()`, `src/ui/MainWindow.cpp:2202-2203`); nothing drives add → save → reopen → assert the `/EmbeddedFiles` name tree still carries the row. Open gap. |
| 35 | `attachment_panel_extract` | **AUTOMATABLE — NOT YET DONE.** `triggerExtractForTesting()` exists (`src/ui/MainWindow.cpp:2204-2205`); the doc's own `certutil` hash step is a straight `EXPECT_EQ` on extracted bytes. Open gap. |
| 36 | `compliance_dialog_pdfa_convert` | **AUTOMATED (partially)** — `tests/test_compliance_manager.cpp`, `tests/test_fidelity_r3_19_pdfa_independent.cpp` (registered). The external `veraPDF` cross-check stays **MANUAL — irreducible**: it depends on a third-party validator not vendored here. |
| 37 | `compliance_dialog_pdfua_convert` | **AUTOMATED (weakly)** — `tests/test_pdfua_converter.cpp` (registered). **Caveat:** no tagged-PDF fixture exists and `src/compliance/PdfUAConverter.cpp:113` marks the conversion path out of scope, so this green is thinner than the row suggests (gap A5-79). |
| 38 | `view_rotate_180` + Reset Rotation | **AUTOMATABLE — NOT YET DONE.** No test drives the View▸Rotate actions and asserts `PdfCanvas` rotation state; `tests/test_coord_mapper_rotation.cpp` covers the maths, not the action. Open gap. |
| 39 | `header_footer_dialog_apply` | **AUTOMATED** — `tests/test_header_footer.cpp`, `tests/test_header_footer_state_leak.cpp` (registered). |
| 40 | `header_footer_dialog_bates` | **AUTOMATED** — `tests/test_bates_tokens.cpp` (registered) covers token expansion. **Sub-leg not done:** the apply → save → extract-text round-trip asserting `ACME001000/1/2` across successive pages. |
| 41 | `calc_order_dialog_apply_persists` | **AUTOMATED** — `tests/test_calculation_order.cpp` (11 cases), `tests/test_fidelity_r3_09_form_calc.cpp` (registered). The `mutool show .../AcroForm/CO` step is the same assertion out of process. |

### §3 Code + unit tests, no interactive check (42–48)

| # | Item | Disposition |
|---|---|---|
| 42 | Preflight report export to a real file | **AUTOMATABLE — NOT YET DONE (gap A5-35, confirmed open).** `tests/test_preflight_dialog.cpp` asserts only that Export is disabled before a run and enabled after. The writer at `src/ui/PreflightDialog.cpp:302-320` emits a header, `Profile:`, `Errors:`, `Warnings:` and `[ERROR] `/`[WARNING] ` lines with rule + `(page N)`; **no test writes or parses that file**, and `reportExportRequested` is spied nowhere. Needs a path-injection seam past `QFileDialog` — the `savePath.isEmpty()` early return at `:300` is the natural place. |
| 43 | Navigation history across a real document | **AUTOMATABLE — NOT YET DONE (gap A5-36, confirmed open).** `tests/test_navigation_history.cpp` has 11 tests, all against the pure `NavigationHistory` class; **no `MainWindow` is instantiated**. The wiring is real and locatable — `connect(canvas_, &PdfCanvas::pageChanged, …)` at `src/ui/MainWindow.cpp:2309`, `recordVisit` at `:6789`, `onPreviousView`/`onNextView` at `:5711-5730`, clear-on-open at `:7071-7074` — and `grep -rn "previousViewAction\|navHistory\|onPreviousView" tests/ --include=*.cpp` returns **zero hits**. |
| 44 | Export Plain Text — file contents / form-feed bytes | **AUTOMATABLE — NOT YET DONE.** 9 action-state cases exist; none reads the output bytes. The assertion is concrete: UTF-8 BOM prefix and exactly two `0x0C` bytes between three page texts. |
| 45 | Create PDF from Images — `runInteractive` | **AUTOMATABLE — NOT YET DONE (gap A5-38, confirmed open).** `writePdf()` is well covered (13 cases). `ImageToPdfWizard::runInteractive` (`src/ui/ImageToPdfWizard.cpp:92`) opens with `QFileDialog::getOpenFileNames` (`:94`), then `getSaveFileName` (`:98`), and auto-appends `.pdf` (`:101-103`); there is no injectable seam for either dialog. The pattern to copy already exists here — `setLaunchHandlerForTesting` in the `/Launch` path. |
| 46 | Follow-link viewer dispatch | **AUTOMATABLE — NOT YET DONE (gap A5-39, confirmed open, with a corrected line reference).** Both halves are tested *separately*: `followLinkAt` directly, by 6 tests in `tests/test_follow_link.cpp` plus `test_launch_link_follow.cpp`, `test_action_egress_inertness.cpp` and `test_external_actions_gate.cpp`; and the signal itself, by `QSignalSpy` on `PdfCanvas::canvasClicked` in `tests/test_pdf_canvas_q_invokable.cpp:281-300`. **The `connect()` that joins them is unexercised.** It is at `src/ui/MainWindow.cpp:2312-2315` — **not** at `:2216-2218` as the gap register recorded; that range is the attachment-panel test pipe. |
| 47 | Forms palette click → `setCreationFieldType` | **AUTOMATED** — `tests/test_forms_designer.cpp` (12 cases, registered), including the `FormsDesignerNegativeTest.*` boundary cases added in F17. |
| 48 | SignDialog invisible + certification | **AUTOMATED** — `tests/test_sign_dialog.cpp`, `tests/test_certification_signatures.cpp`, `tests/test_invisible_and_certification_signatures.cpp`, `tests/test_docmdp_enforcement.cpp` (all registered). |

### §4 Visual-only (49–53)

| # | Item | Disposition |
|---|---|---|
| 49 | Theme switching | **AUTOMATED for the mechanics** — `tests/test_theme_and_field_count.cpp` (10 cases, registered): string round-trip with a bad-value fallback, dark `QPalette::Window == QColor(53,53,53)` vs light, `QSignalSpy` on the change signal, `QSettings("PDFE","PDFE")/Preferences/theme` persistence, and `QApplication::palette()` actually changing. **MANUAL — irreducible residue:** "no white flashes on dialog open" (a transient during widget realisation, with no post-hoc observable), "text is readable" (a contrast judgement), "System matches the OS current theme" (depends on live OS state). |
| 50 | Atlas-mode comparison | **AUTOMATED for the engine** — `tests/test_atlas_comparison.cpp` (29+ cases, registered): mode defaults, both dialog buttons, the compare signal, null/empty inputs, enriched-atlas metrics. **MANUAL — irreducible residue:** "Scan Glyphs preserves the distinctive font shape better than FreeType Styled on cursive samples" is a *comparative perceptual* judgement between two renderings that are both correct outputs; there is no ground truth to compare against, so no oracle can be authored. |
| 51 | Print preview + Print-to-PDF | **AUTOMATED** — `tests/test_print_preview_render.cpp` (F17: drives `PrintPreviewDialog::onPaintRequested` headlessly through a PDF-format `QPrinter` and compares page-for-page against the print path and the authored fixture geometry — it found the auto-rotate clipping defect), plus `tests/test_fidelity_r3_21_print_vs_screen.cpp`. **MANUAL — irreducible residue:** printing to a *physical* printer. |
| 52 | OCR accuracy on real scans | **AUTOMATED — all three variants** (see the correction box in §4): clean `tests/test_e2e_integration.cpp:260`, skewed `tests/test_fidelity_r3_08_ocr_wer.cpp:316`, multicolumn `tests/test_layout_extraction_corpus.cpp:162-163`. This item was **wrongly** carried as visual-only. |
| 53 | Font rendering on scanned pages | **MANUAL — irreducible.** "Typed characters should visually blend with the surrounding scanned text" is a perceptual similarity judgement against an *arbitrary* scanned source; there is no authored ground truth for what the blend should be. The quantitative components (x-height, stroke weight, slant) *are* asserted — `tests/test_glyph_scale_adjuster.cpp`, the enriched-atlas metrics in `tests/test_atlas_comparison.cpp` — but they do not add up to "blends". |

### §5 External infrastructure (54–55)

| # | Item | Disposition |
|---|---|---|
| 54 | Real RFC-3161 TSA network round-trip | **MANUAL — irreducible.** `tests/test_tsa_fixture.cpp` (6 cases, registered) runs against **cached token bytes** and opens no socket. Automating the real round-trip would make the suite depend on a third-party service's availability and on outbound network from the build host — and this repo's security posture asserts the opposite: `NetworkEgressGuard` exists to prove tests make **zero** external connections. The offline substitute is the correct choice; the network leg stays manual by design. |
| 55 | CA-chain validation against the OS trust store | **MANUAL — irreducible.** `tests/test_ltv_validator.cpp` (5 cases, registered) supplies trust exclusively through PDFE's own `TrustAnchorManager`, and says so at `:359-365`: the test signer is self-signed, so *"without TrustAnchorManager the verdict would be UntrustedChain"*. At `:8` it deliberately accepts either verdict where the system store could influence the outcome. Asserting on the Windows store would require **installing a root certificate into the machine's trust store** — an administrative mutation of the host that a test suite must not perform. |

### §6 (56)

| # | Item | Disposition |
|---|---|---|
| 56 | Enable the disabled unit test | **OBSOLETE** — the test is live and the suite has no `DISABLED_` tests. No action. |

### Summary of dispositions — recounted from the tables above

| disposition | items | count |
|---|---|---:|
| N/A (build / environment / hygiene precondition) | 1, 2, 3 | 3 |
| AUTOMATED — headless GoogleTest | 5, 6, 7, 30, 31, 32, 33, 36, 37, 39, 40, 41, 47, 48, 49, 50, 51, 52 | 18 |
| AUTOMATED — live-UI Python (reason recorded why it cannot be headless) | 8–28 | 21 |
| MANUAL — irreducible, with written reason | 4, 29, 53, 54, 55 | 5 |
| **AUTOMATABLE — NOT YET DONE (open gaps)** | **34, 35, 38, 42, 43, 44, 45, 46** | **8** |
| OBSOLETE | 56 | 1 |
| **total checklist items** | | **56** |

3 + 18 + 21 + 5 + 8 + 1 = **56**.

Note that three of the AUTOMATED rows (49, 50, 51) additionally carry a
**manual irreducible residue** described in place, and one (40) carries an
un-built sub-leg. Those are counted under AUTOMATED because the item's main
assertion is automated; the residues are stated so they are not lost.

**F19 did not close the eight open gaps.** For each it established that the
concern *is* automatable, named the seam, and stated the assertion that would
close it. They are recorded here as open rather than left implicit in a
"manual" bucket — "manual" was doing the work of hiding them.
