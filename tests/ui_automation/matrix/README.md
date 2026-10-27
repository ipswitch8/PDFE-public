# PDFE UI Test Matrix

The master CSV spreadsheet mapping every planned end-to-end test case
for the PDFE Qt desktop app to a test ID, priority, and owning phase
of the regression pipeline in `.claude/pipeline.json`.

## Files

- **`ui_test_matrix.csv`** — the matrix itself. 762 rows covering all
  subsystems (as of pipeline c4a9e271 phase-4; grows as sweeps add
  passthrough rows). NOT committed directly from this file; regenerate
  from the generator for reproducibility.
- **`generate_matrix.py`** — deterministic Python script that
  produces the CSV. Run any time to refresh. No timestamps, no
  randomness — same input always yields the same bytes.
- **`verify_matrix.py`** — acceptance-criteria checker. Run in CI to
  fail the build if the matrix drifts out of spec.

## Regenerating

```powershell
python tests/ui_automation/matrix/generate_matrix.py
python tests/ui_automation/matrix/verify_matrix.py
```

## Columns

| column | meaning |
|---|---|
| `subsystem` | Top-level grouping (`File`, `Edit`, `Pages`, `Forms`, `Security`, `Image`, `Tools`, `View`, `Help`, `Dialogs`, `Annotations`, `TextEdit`, `Canvas`, `Flows`, `Actions`, `RenderVerify`, plus sweep groupings) |
| `category` | `Menu` / `Dialog` / `Flow` / `Regression` |
| `element` | Specific item — menu path, dialog widget, annotation tool, etc. |
| `test_id` | Unique ID, referenced from individual test implementations |
| `preconditions` | What state the app needs before the test can run |
| `action` | What the test does |
| `expected` | What the test asserts |
| `tool` | `pywinauto` / `FlaUI` / `PyAutoGUI` / `mixed` |
| `priority` | `P0` (every release) / `P1` (weekly) / `P2` (nightly) |
| `status` | `planned` / `green` / `red` / `blocked` |
| `owner_phase` | `phase-2` through `phase-10` per the execution plan |
| `fixture` | Required PDF or setup file |
| `notes` | Known-tricky bits, e.g. clipboard race helpers required |

## Relationship to the execution pipeline

Each phase in `.claude/pipeline.json` owns a slice of the matrix:

- **phase-2** (P0 menu navigation, 65 rows) — Click every leaf menu
  item, assert it opens its dialog or performs its action without
  crashing.
- **phase-3** (P0 dialog reachability, 168 rows) — For every dialog,
  verify each documented widget is reachable via UIA suffix match.
- **phase-4** (P0 critical flows, 20 rows) — Open/Save/Encrypted/
  Signed/Form/Annotation/Undo/Redo end-to-end flows.
- **phase-5** (P1 annotations, 30 rows) — Six annotation tools × five
  sub-tests (create, color, opacity, contents, delete).
- **phase-6** (P1 form fields, 32 rows) — Eight field types × four
  sub-tests (create, fill, default, XFDF round-trip).
- **phase-7** (P1 page operations, 40 rows) — rotate, delete, insert,
  move, extract, drag-drop, multi-select, undo/redo variations.
- **phase-8** (P1 security and compliance, 31 rows) — encryption
  round-trip for four variants, signature sign+verify,
  PDF/A (1b/2b/2u/2a/3b) and PDF/UA-1 validate/convert.
- **phase-9** (P2 text formatting, 32 rows) — font family × size ×
  bold × italic × underline × strikethrough × color × alignment plus
  four image-based (TRE) tests.
- **phase-10** (P2 canvas coordinates, 15 rows) — PyAutoGUI-driven
  tests for PdfCanvas interior: annotation handles, text selection
  drag, pan, zoom, rubber-band.

## QAction-grounded coverage layer (`Actions` subsystem)

Pipeline c4a9e271 phase-4 added an objectName-grounded layer: the generator
scrapes every `setObjectName("…Action")` from `src/ui/MainWindow.cpp`
(`scrape_qactions()`) and emits one `act_<objectName>` row per QAction. The
older `menu_*` rows are *friendly-path* ("File > Open") and do not name the
stable objectName the GAP-A14 `testToolActivatePipe` seam actually drives on
CI. The `act_*` rows close that gap and self-sync with source, so a newly
added action can never land without a matrix row. See
`tests/docs/ui_coverage_audit_phase4.md` for the full domain breakdown and
the phase-5/6/7 allocation plan.

## Keeping the matrix in sync with test reality

The matrix is the source of truth. When a test case is implemented,
its row's `status` flips from `planned` to `green` or `red` in the
same commit that adds the test. Phase-11 of the pipeline (`CI
Integration and Matrix Sync`) runs a karen-gated check that every
P0 row has a non-`planned` status after a suite run, to catch
silently-skipped tests.

## Qt UIA gotchas the matrix assumes

Every test that interacts with Qt widgets must handle the following
— these are in every test's notes column where relevant:

1. **Hierarchical auto_ids.** `setObjectName("X")` becomes
   `QApplication.mainWindow.X`. Match by suffix.
2. **QAction auto_ids are shared.** All menu items have the same
   `auto_id`. Navigate by `title` or `menu_select("Top -> Leaf")`.
3. **`wait_for_idle=False`** required for every `Application.start()`
   call against a Qt app.
4. **RDP clipboard race** — use the retry helpers landed in
   `src/textedit/TextEditTool.cpp` (commit `8371c81`).
5. **Dock widgets hidden by default** — show via View menu before
   querying their child widgets.
6. **Modal dialogs steal focus** — tests should be run in a focused
   session or in isolation.
7. **PdfCanvas custom-painted** — interior interactions use
   PyAutoGUI coordinates, not UIA.

## Regenerate-on-change contract

The generator script is designed so you can *add* rows without
renumbering existing test IDs. Test IDs are deterministic based on
the input lists (menu names, annotation tools, etc.), so inserting
a new row only changes that one row's surroundings in the CSV. The
verifier asserts uniqueness and structural correctness so a bad
edit to the generator fails fast.

Never edit `ui_test_matrix.csv` by hand. Always regenerate.
