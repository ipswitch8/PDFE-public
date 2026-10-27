# Coverage-Expansion Phase-3 Report — Accessibility (C-06) + Visual-Regression Breadth (C-08)

Pipeline: `coverage-expansion`, phase-3. Register: `docs/coverage-expansion/register.json` (C-06, C-08).

Stance: ADD real oracles, never weaken/skip. No product source changed (none required — no bug found).

---

## C-06 — Accessibility / AT tree

New file: `tests/test_accessibility_tree.cpp` (9 tests). The pre-existing a11y
tests assert names/existence only; these assert **role**, **hierarchy**, and
**value/state** — the properties a screen reader actually depends on — plus a
tagged-PDF reading-order oracle.

### Assertions added + non-vacuity proof

| Test | Asserts | Non-vacuity (what a wrong tree would fail) |
|---|---|---|
| `CanvasAndPageChildrenExposeCorrectRoles` | canvas `role()==Canvas`; each page child `role()==StaticText`; child role ≠ canvas role | A generic-pane canvas fails EXPECT #1; a constant-role stub fails the `StaticText` / `!=` checks |
| `CanvasChildParentRoundTrip` | `child(i)->parent()->object()==canvas`; `indexOfChild(child)==i` on both canvas and resolved parent; `indexOfChild(non-child)==-1` | A null parent, or `indexOfChild` returning the query index, breaks the identity + the −1 case |
| `PageChildStateAndCanvasLiveValue` | page child `state().readOnly && multiLine`; canvas Name==filename; Value tracks **live** page (`page 1 of 3` → `page 2 of 3`) and zoom | A hard-coded value string cannot follow `goToPage()`/`setZoom()` — `EXPECT_NE(v0,v1)` / `EXPECT_NE(v1,v2)` catch it |
| `NoDocumentCanvasIsNamedLeaf` | empty-state: role Canvas, 0 children, Name=="No document", Value contains "no document loaded" | Guards the empty branch (a leaked stale child count would fail `childCount()==0`) |
| `TaggedPdfExposesCorrectReadingOrder` | (tagged fixture) child N carries exactly its own heading (Alpha/Bravo/Charlie), no cross-page leak; whole-doc order Alpha<Bravo<Charlie | Reverse page order → Charlie precedes Alpha (fails `EXPECT_LT`); a child returning whole-doc text → leak `EXPECT_FALSE` fails |
| `IntraPageReadingOrderIsPreserved` | (3-page p3) within-page order `Item 1 < Item 2 < Item 3` in the child's text interface | A shuffled stext order fails the strict `EXPECT_LT` chain |
| `TreEditDialogControlsExposeCorrectRoles` | dialog root `Dialog`; `treTextEdit`→`EditableText`; approach radios→`RadioButton`; a `QLabel`→`StaticText`; OK/Cancel→`Button` | Four mutually-distinct roles; a constant-role stub cannot satisfy all simultaneously |
| `RoleDiscriminatesControlKinds` | `role()` returns different values for edit vs radio vs label | Explicit proof `role()` discriminates (not a constant) |
| `MainWindowToolButtonsExposeActionableRole` | every realised `QToolButton` → actionable role (`Button`/`ButtonMenu`/`CheckBox`); ≥1 plain `Button`; never `Pane`/`StaticText`/`NoRole` | An empty toolbar passes vacuously → guarded by `checked>0` and `plainButtons>0`; a passive-role regression fails the `EXPECT_NE`s |

### Reading-order oracle

The AT walks ordered page children and, within each, the page's extracted text
(MuPDF stext reading order). The oracle asserts **both** granularities against
the true document order using the tagged fixture
(`test_tagged_multipage.pdf` — ordered Alpha/Bravo/Charlie headings, real
`StructTreeRoot`) for cross-page order and `test_3page.pdf` page 3 (ordered
Item 1/2/3 list) for intra-page order.

Note on the tagged-PDF API: the product exposes reading order through the
`QAccessible` tree (ordered `PdfPageAccessible` children + per-page stext), which
is the surface a screen reader consumes. It does **not** surface PDF `StructElem`
roles through `QAccessible` (the `StructureTreeBuilder` is a writer, not an
AT reader), so the oracle asserts the reading order the AT actually presents,
not a struct-tree role list that no AT path reads.

### One expectation corrected (not a product bug)

First run flagged a checkable `QToolButton` reporting role 44 = `CheckBox`. That
is **correct Qt/AT behavior** — a checkable toolbar toggle is announced as a
checkbox (checked/unchecked), not a push button. The test expectation was
widened to the actionable set `{Button, ButtonMenu, CheckBox}` (still rejecting
passive roles and requiring ≥1 plain `Button`). No product change.

---

## C-08 — Visual-regression breadth

Expanded the committed correctness-golden set from **1** (`save_icon`) to **4**,
all produced by the deterministic trusted generator
`tests/fixtures/ui_screenshots/generate_reference_goldens.py` (a fixed drawing
spec — no screenshot, no RNG), so any machine regenerates byte-identical output.

New committed goldens (subsystem `reference`):

| Golden | Render state | Asymmetry anchors |
|---|---|---|
| `open_icon.png` | second toolbar icon (folder + up-arrow) | tab upper-left, arrow points up |
| `page_region.png` | synthetic rendered-PDF-page region | header block, ragged-right text lines, left red margin marker, bottom-right page-number box |
| `dialog_control.png` | dialog control paint | checkbox upper-left, primary button lower-right with green focus ring + off-centre label |

New test: `tests/test_golden_reference_breadth.py` — parametrized over every
golden × every defect class, going through `compare_to_golden` against the
committed baseline (no self-seed path). 29 pytest assertions green
(`test_golden_reference_breadth.py` + existing `test_golden_no_selfseed.py`).

### Per-golden defect-rejection proof (empirically verified, strict thresholds NCC≥0.99 / max_delta≤10 / pHash≤5)

| Golden | correct | mirror | half-size | wrong-color |
|---|---|---|---|---|
| save_icon | PASS | REJECT (NCC 0.009) | REJECT (Δ160) | REJECT (NCC −0.11) |
| open_icon | PASS | REJECT (NCC 0.78) | REJECT (NCC 0.97) | REJECT (NCC 0.04) |
| page_region | PASS | REJECT (NCC 0.55) | REJECT (NCC 0.96) | REJECT (NCC 0.94/Δ158) |
| dialog_control | PASS | REJECT (NCC 0.02) | REJECT (NCC 0.92) | REJECT (NCC 0.72/Δ171) |

Every golden PASSES its correct render and REJECTS all three defect classes —
the property a self-seeded baseline can never have.

### Headless vs runner-only

All four goldens are deterministic synthetic renders → fully headless-reproducible
and committed. FULL live-application window states (real MainWindow chrome,
native menus, GPU-composited canvas) are **runner-only**: they depend on the
self-hosted UI runner's exact display and are intentionally NOT committed here —
faking them would reintroduce the self-seeding anti-pattern. They remain covered
by the live-capture tests under `tests/ui_harness/` and are documented as
runner-only in the new test's module docstring.

---

## Verification

- Build: `taskkill` + synchronous `cmake --build build/vs2022 --config Debug --target pdfe_tests` → **succeeded** (only the pre-existing benign LNK4098 MSVCRT warning).
- gtest: `A11yTreeTest.*` + `A11yDialogRolesTest.*` = 9/9 GREEN. Full accessibility filter (new + existing) = **44 tests, 0 failures, 0 errors** (JUnit XML parsed). Deterministic across runs.
- pytest: golden breadth + no-selfseed = **29 passed**.
- `git status --porcelain -- src/`: **CLEAN** — no product source changed.
- Registration: `test_accessibility_tree.cpp` added to `tests/CMakeLists.txt`; new goldens are PNGs under `tests/fixtures/ui_screenshots/baseline/reference/` (the `FixtureCorpus.ReadmeListsEveryPdfFixture` test enforces README only for top-level `*.pdf`; no new `*.pdf` added, so no README change required). `save_icon.png` regenerated byte-identical (spec unchanged).

## Bug status

No real accessibility or render bug found. The single first-run red was a
too-narrow test expectation (checkable tool button = `CheckBox`), corrected in
the test with the correct expected roles; the product behavior is correct.
