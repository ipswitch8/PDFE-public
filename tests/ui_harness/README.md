# PDFE UI Test Harness

Reusable pywinauto + PyAutoGUI test infrastructure for the PDFE native
Windows desktop PDF editor. Populated across pipeline phases 2-9 and
finalised in phase 10.

---

## Directory layout

```
tests/ui_harness/
├── __init__.py                   # Package root
├── README.md                     # This file
├── DETERMINISM.md                # Environmental requirements (DPI / scaling /
│                                 # window size / theme / font fallback)
│
├── core/                         # Shared low-level helpers (phase 2)
│   ├── __init__.py
│   ├── launch.py                 # Deterministic PDFE.exe launch + teardown
│   ├── locate.py                 # pywinauto UIA widget locator by objectName
│   │                             # / accessibleName + invoke / by_name helpers
│   ├── canvas.py                 # PyAutoGUI canvas pixel ops (drag, click,
│   │                             # colour pick, wait_stable)
│   ├── screenshot.py             # Screen capture →
│   │                             # tests/fixtures/ui_screenshots/<sub>/<t>.png
│   ├── diff.py                   # NCC + max-delta + pHash image diff
│   └── golden.py                 # Golden baseline regen + comparison
│
├── text_edit/    helpers.py      # phase-3 — text-edit subsystem helpers
├── annotations/  helpers.py      # phase-4 — annotation tools (sticky/free-text/
│                                 # ink/shape/text-markup) + comment-panel
├── forms/        helpers.py      # phase-5 — form fields, JS, XFDF
├── page_ops/     helpers.py      # phase-6 — rotate/insert/delete/extract/
│                                 # drag-reorder
├── image_edit/   helpers.py      # phase-7 — insert/replace/transform/EXIF
├── redaction/    helpers.py      # phase-7 — mark/apply/search-and-redact
├── signatures/   helpers.py      # phase-8 — sign/validate/certify
├── panels/       helpers.py      # phase-9 — search/bookmark/layer/attach/
│                                 # measure side panels
├── dialogs/      helpers.py      # phase-9 — compliance/encryption/about/
│                                 # preferences and 28 generic dialogs
└── menus/        helpers.py      # phase-9 — invoke_menu/toolbar/status/
                                  # tab-through/get_accessible_name
```

Every subsystem package exposes a stable, narrow API. Tests import
from `tests.ui_harness.<subsystem>.helpers` and never touch
`pywinauto` / `pyautogui` directly when a helper exists.

---

## Quick start

```python
from tests.ui_harness.core.launch import PDFESession
from tests.ui_harness.core.screenshot import capture_canvas
from tests.ui_harness.menus.helpers import invoke_menu, get_accessible_name
from tests.ui_harness.panels.helpers import open_panel, search_in_panel
from tests.ui_harness.dialogs.helpers import open_dialog, accept_dialog

with PDFESession(pdf_path="tests/fixtures/test_3page.pdf") as s:
    # Driving menus
    invoke_menu(s, "View->Bookmarks")

    # Driving panels
    open_panel(s, "search")
    search_in_panel(s, "Hello world")

    # Driving dialogs
    dlg = open_dialog(s, "metadata")
    # ...edit fields...
    accept_dialog(s, "metadata")

    # Capture a deterministic screenshot
    out = capture_canvas(s.app, subsystem="smoke", test="baseline")

    # Quick spot-check on accessible name presence
    name = get_accessible_name(s, "alignJustifyButton")
    assert name == "Justify"
```

---

## Determinism contract

See `DETERMINISM.md` for the full contract. Short version:

- Windows display at **100% scaling** (DPI 96).
- PDFE window is **1600 × 1000 px** (enforced by `core/launch.py`).
- OS theme: **Light mode**.
- Interactive desktop session (NOT service Session 0).
- Text-edit zoom: **fit-page** (or fixed 100%).
- Font fallback: process-side `setApplicationName` + `setOrganizationName`
  ensure stable QFontDatabase enumeration.
- All Python files: `# -*- coding: utf-8 -*-`, all file I/O
  `encoding="utf-8"`.

When a determinism guard is missing, screenshots will diff under
`core.diff.assert_zero_delta` even on a clean re-run. Add the missing
guard to `core/launch.py` before treating the diff as a real
regression.

---

## Running suites

### Per-subsystem smoke

```bash
cd C:/Users/dev/source/PDFE
python -m pytest tests/test_ui_text_edit_smoke.py     -v
python -m pytest tests/test_ui_annotations_smoke.py   -v
python -m pytest tests/test_ui_forms_smoke.py         -v
python -m pytest tests/test_ui_page_ops_smoke.py      -v
python -m pytest tests/test_ui_image_redaction_smoke.py -v
python -m pytest tests/test_ui_security_smoke.py      -v
python -m pytest tests/test_ui_chrome_smoke.py        -v
python -m pytest tests/test_ui_harness_smoke.py       -v
python -m pytest tests/test_ui_titlebar_icon.py       -v
python -m pytest tests/test_ui_pdfe_smoke.py          -v
```

### Whole UI suite

```bash
cd C:/Users/dev/source/PDFE
python -m pytest tests/test_ui_*.py -v \
       --junitxml=tests/fixtures/ui_audit/final_ui_results.xml
```

`tasklist | findstr /I PDFE` should be empty after the run. The
`PDFESession` context manager closes the app on `__exit__`; orphaned
processes indicate a missing teardown call.

### Headless mode

Set `PDFE_UI_FORCE_HEADLESS=1` to skip every live-UI case (used by CI
and machines without an interactive desktop session). Smoke tests use
`@REQUIRES_LIVE_UI` skip guards that respect this env var.

---

## Helper matrix (per subsystem)

| Subsystem | Module | Top-level helpers |
|---|---|---|
| Core | `core.launch` | `PDFESession` (context manager) |
| Core | `core.locate` | `by_auto_id_or_name`, `by_name`, `invoke` |
| Core | `core.canvas` | `wait_stable`, drag / click / colour-pick |
| Core | `core.screenshot` | `capture_canvas`, `capture_window` |
| Core | `core.diff` | `assert_zero_delta`, `compare_images` |
| Core | `core.golden` | `regen_golden`, golden baseline ops |
| text_edit | `text_edit.helpers` | `open_in_text_edit_mode`, `click_into_span`, `type_keys`, `capture_overlay`, `save_and_reopen`, `select_word_at`, `toggle_bold`, `toggle_italic`, `set_font_size` |
| annotations | `annotations.helpers` | `activate_tool`, `place_sticky_note`, `draw_ink_stroke`, `draw_shape`, `highlight_range`, `set_color`, `open_comment_panel`, `open_annotation_properties` |
| forms | `forms.helpers` | `activate_form_tool`, `tab_to_field`, `fill_text_field`, `toggle_checkbox`, `select_radio`, `choose_combo`, `select_list_items`, `trigger_js_keystroke`, `export_xfdf`, `import_xfdf` |
| page_ops | `page_ops.helpers` | `rotate_page`, `delete_page`, `insert_blank`, `insert_from_file`, `extract_pages`, `drag_reorder`, `page_panel_context`, `select_pages` |
| image_edit | `image_edit.helpers` | `insert_image`, `replace_image`, `transform_image`, `crop_image`, `rotate_image`, `strip_exif` |
| redaction | `redaction.helpers` | `mark_redaction`, `apply_redactions`, `search_and_redact` |
| signatures | `signatures.helpers` | `sign_document`, `validate_signature`, `certify_document` |
| panels | `panels.helpers` | `open_panel`, `search_in_panel`, `expand_bookmark`, `toggle_layer`, `measure_distance`, `open_attachment` |
| dialogs | `dialogs.helpers` | `open_compliance_dialog`, `convert_to_pdfa`, `convert_to_pdfua`, `validate_pdfa`, `set_encryption`, `open_dialog`, `accept_dialog`, `cancel_dialog` |
| menus | `menus.helpers` | `invoke_menu`, `invoke_toolbar`, `check_status_bar`, `tab_through_window`, `get_accessible_name` |

---

## Adding tests for a new subsystem or scenario

1. Find the audit row in
   `tests/fixtures/ui_audit/UI_MATRIX_GAPS.md`. Every test must
   correspond to at least one row id.
2. If the row exposes a behaviour the existing helpers don't cover,
   add the helper to the appropriate `tests/ui_harness/<subsystem>/
   helpers.py`. Keep the API narrow and deterministic — every helper
   takes a `PDFESession` and returns either nothing or a small typed
   value.
3. Write `tests/test_ui_<subsystem>_<scenario>.py`:
   - `# -*- coding: utf-8 -*-` shebang.
   - Skip guard via `@REQUIRES_LIVE_UI` when the case requires a live
     PDFE.exe + interactive Windows session.
   - Always include a `*_skip_summary` test that prints whether the
     live-UI cases ran (helps audit machines that can never run them).
4. Capture a baseline screenshot via `core.screenshot.capture_canvas`
   on first run and compare against
   `tests/fixtures/ui_screenshots/baseline/<subsystem>/<test>.png` on
   subsequent runs.
5. Update the per-phase closeout in `UI_MATRIX_GAPS.md` so the
   `covered_by` column points at the new test.

---

## Regenerating golden baselines

The first time a baseline is captured, copy it from
`tests/fixtures/ui_screenshots/<sub>/<t>.png` to
`tests/fixtures/ui_screenshots/baseline/<sub>/<t>.png`. After a
deliberate UI change, re-run the test, eyeball the diff, then update
the baseline:

```python
from tests.ui_harness.core.golden import regen_golden
regen_golden("text_edit", "bold_on", "/abs/path/to/new.png", force=True)
```

`force=True` overwrites without prompting; reserve it for known-good
baselines.

---

## Screenshot artifacts

```
tests/fixtures/ui_screenshots/
├── baseline/                     # Canonical golden baselines
│   ├── text_edit/
│   ├── annotations/
│   ├── forms/
│   ├── page_ops/
│   ├── image_edit/
│   ├── redaction/
│   ├── signatures/
│   ├── compliance/
│   ├── encryption/
│   ├── panels/
│   ├── dialogs/
│   ├── menus/
│   ├── chrome/
│   ├── visual_fidelity/
│   └── smoke/
│
├── manifest.json                 # Index of every captured screenshot
└── <subsystem>/                  # Per-subsystem run output
    └── <test>.png
```

`manifest.json` is updated on every `capture_canvas` call. Re-runs
overwrite the `<subsystem>/<test>.png` and append to the manifest's
history list — diff comparisons read from `baseline/`, never from
the per-run output.

---

## Debugging failures

| Symptom | Likely cause | Where to look |
|---|---|---|
| `RuntimeError: Could not locate ...` | objectName drifted in src/ui/ | `tests/fixtures/ui_audit/OBJECTNAME_AUDIT.md` |
| Screenshot diff != 0 on re-run | Determinism guard missing | `tests/ui_harness/DETERMINISM.md` + `core/launch.py` |
| `pywinauto.timings.TimeoutError` waiting for dialog | Menu accelerator changed | `src/ui/MainWindow.cpp` `createMenus()` |
| Orphan PDFE.exe after run | Test bypassed `PDFESession` context manager | `tests/ui_harness/core/launch.py` |
| Live test runs in CI but skips on local | `PDFE_UI_FORCE_HEADLESS=1` set in env | `unset` it |
| `ValueError: I/O on closed file` from pytest | pytest tried capturing stdout for skipped tests | run with `-s` to disable capture |

---

## Cross-reference

- Audit: `tests/fixtures/ui_audit/UI_MATRIX_GAPS.md`
- Per-phase baselines: `tests/fixtures/ui_audit/phase_*_baseline_failures.txt`
- Final report: `tests/fixtures/ui_audit/FINAL_REPORT.md`
- Object-name audit: `tests/fixtures/ui_audit/OBJECTNAME_AUDIT.md`
- Determinism: `tests/ui_harness/DETERMINISM.md`
