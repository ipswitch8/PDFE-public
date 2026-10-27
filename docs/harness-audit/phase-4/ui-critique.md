# Phase-4 — Native-UI / E2E Harness Critique

**Adversarial audit of PDFE's native-UI (pywinauto/WinAppDriver/PyAutoGUI) and
UI-subsystem GoogleTest layers.** FIND-AND-DOCUMENT only — no product or test
code changed. The harness is the defendant: a "green" UI test that never runs
(permaskip) or only checks widget-existence provides **zero** protection.

Scope: 43 `tests/test_ui_*.py` (~450 `def test_`) + the UI-subsystem C++
GoogleTest (31 files / 332 tests / 191 `GTEST_SKIP` sites per `census.json`).

---

## Workstream 1 — Skip-guard audit

### Counts (source-derived, HEAD)

| Metric | Count |
|---|---:|
| `pytest.skip()` body-skip call sites in `test_ui_*.py` | **742** |
| `@pytest.mark.skipif` markers in `test_ui_*.py` | 83 |
| `self.skipTest(...)` | 2 |
| `GTEST_SKIP()` sites in UI-subsystem C++ | 191 |
| **Total UI-scope skip sites** | **~1018** |
| *(census-wide context: 1142 pytest.skip across all .py; 1230 GTEST_SKIP across 247 C++ files)* | |

Most-skipped UI files: `test_ui_redaction.py` (107), `test_ui_compliance_accessibility.py`
(92), `test_ui_forms.py` (89), `test_ui_annotations.py` (89), `test_ui_stress.py` (70),
`test_ui_text_editing.py` (65), `test_ui_encryption_signatures.py` (47).

### Classification

`_is_interactive_session()` returns `bool(os.environ.get("SESSIONNAME"))`
(`test_ui_annotations.py:207`). On the self-hosted Server-2019 runner `SESSIONNAME`
is set, so the **module-level** `REQUIRES_LIVE_UI` skipif (binary + pywinauto +
pyautogui + interactive) generally *passes* and the modules collect and start. The
damage is done by the **in-body** skips: the moment a test reaches a real UIA
menu/tree lookup, dialog open, or pipe read, it hits the documented **gap_a14
UIA-menu-hang** on Server-2019 and bails to `pytest.skip(...)` — green, protecting
nothing.

| Class | Count | Basis |
|---|---:|---|
| **legitimate-env-guard** | **~270** | fixture-missing (125), module skipif (83: binary/pywinauto/pyautogui/interactive), optional-dep pikepdf/pymupdf/fitz/cryptography (~50), DPI-unsupported. Would run on a real desktop / when the dep is present. |
| **silent-zero-protection** | **~472** | `not reachable via UIA` (262) + dialog `could not be opened / did not appear / did not load` (133) + `test pipe not reachable/empty` (25) + `Could not send/select/click/set/change/read/capture` (53). All fire deterministically on the gap_a14 CI runner. |

### Worst offenders (silent-zero-protection, ranked by consequence)

1. **`test_ui_redaction.py`** — 109 skip sites, 45 `not reachable`. Gates the
   **irreversible P0 `applyRedactionsAction`** (content actually removed from the
   saved PDF). Highest-consequence surface, most-skipped file.
2. **`test_ui_encryption_signatures.py:1052`** — `EncryptionDialog could not be
   opened via menu.` Gates saveWithEncryption / signDocument / verifySignatures
   (P0 security). Menu-driven dialogs = gap_a14 skips.
3. **`test_ui_forms.py`** — 31 `not reachable` / 89 skips. Flatten (P0), reset,
   XFDF round-trip, radio-group exclusivity never assert in CI.
4. **`test_ui_compliance_accessibility.py:909`** — `ComplianceDialog could not be
   opened via menu.` 92 skips; PDF/A + PDF/UA analyze/validate/convert dark.
5. **`test_ui_annotations.py:1142`** — `stickyNoteToolAction not reachable via UIA.`
   67 `not reachable` sites; whole annotation-create + undo contract skips.
6. **`test_ui_page_operations.py:720`** — `rotatePage180Action not reachable via
   UIA tree.` Page rotate/delete (P0 destructive) gated on UIA action lookup.

### Two structural anti-patterns worth calling out

- **Failure masked as skip:** `pytest.skip('Initial document did not finish
  loading.')` (`test_ui_encryption_signatures.py:1179`), `Source did not load`,
  `did not load on reopen` — a genuine *load regression* is converted from FAIL to
  SKIP and ships green.
- **Green-by-design no-ops:** `test_ui_annotations_smoke.py:150`,
  `test_ui_forms_smoke.py:117`, `test_ui_page_ops_smoke.py:107` add an
  always-pass headless-fallback test that just prints "tests above are
  pytest.skip" so `pytest -k` is non-empty. It asserts nothing.

### How many UI tests effectively never run in CI

**~472 pytest.skip call sites** fire deterministically on the gap_a14 Server-2019
runner. Because interaction-heavy tests carry several skip sites each, these gate
an estimated **~300 of ~450 UI test functions** from executing *any* behavioral
assertion in CI. Only three UI test classes truly protect anything on that runner:
(1) launch + objectName-existence smoke, (2) `testToolActivatePipe`/`invoke_action`
pipe-seam-driven action tests, (3) headless C++ GoogleTest driving managers
directly. Every pixel/SSIM/golden assertion runs **only** on an interactive
developer desktop.

---

## Workstream 2 — UI-matrix over-claim reconciliation

**Is "pass" a runtime result or a static committed value?** It is a **runtime**
artifact that is nonetheless **structurally over-claimed**. The matrix is a
runtime-generated CSV (`census.json`: 973 `ui_test_matrix_*.csv` under
`tests/_runtime/`). Rows are emitted from source as `status=planned` (`act_*` from
`MainWindow.cpp`, dialog-widget rows from `audit_source.py`), then flipped
green/red at runtime by `conftest.record_matrix_result()`. The committed green
therefore reflects a **one-off developer-workstation interactive run**, not a
reproducible CI result — on Server-2019 the backing pywinauto tests skip
(gap_a14). Worse, row color is a **last-write-wins** value
(`tests/ui_automation/conftest.py:85`, sessionfinish `:575`), so a non-owning
suite's SKIP can clobber a proven GREEN.

**Rows checked: 762** (`ui_coverage_audit_phase4.md` §3). Over-claimed rows:

| Row / group | Claim | Reality | Evidence |
|---|---|---|---|
| Dialog widget reachability (**236 rows**) | green | **widget-exists-only** — green = objectName present in the `.cpp` (`audit_source.py` 53/53), *no launch, no render, no behavior*. A dialog that opens garbled is green. | `ui_coverage_audit_phase4.md:88`, §1:30 ("static, no launch") |
| QAction invocation `act_*` (**131 rows**) | green | structural — green = `invoke_action()` pipe returned; asserts the action *fired*, not the correct visible effect. | `ui_coverage_audit_phase4.md:43-45, 217-221` |
| ~111 `act_`/`flow_`/`field_` rows | blocked (understated) | **aggregation false-block**: green clobbered by a non-owning suite's skip via last-write-wins. Proves color depends on execution order, not a stable oracle. | `matrix-nongreen-investigation.md:22-36` |
| Annotation create + canvas handle-drag (~30) | covered elsewhere | UI gesture **un-driveable** by the synthetic-click harness (P-GAP-75); only proven headless in gtest. No runnable UI oracle. | `matrix-nongreen-investigation.md:38-46` |
| Text-edit pixel rows `UI-TE-UI-01` / `UI-TE-UI-03` | LIVE_UI_GUARDED | pixel assertion runs only on interactive desktop; "on headless hosts the skip-summary case passes but the pixel contract is NOT validated." Green-that-never-ran in CI. | `UI_MATRIX_GAPS.md:176, 178` |
| 77 `menu_*`/`cmp_*`/`sec_*` "feature absent" | skip/blocked (some friendly-path green) | friendly `menu_*` rows named no stable objectName, so a domain LOOKED covered while the CI invocation path had zero grounded assertion (96/131 QActions pre-phase-4). | `ui_coverage_audit_phase4.md:34-41`; `matrix-nongreen-investigation.md:56-64` |

The prior audit already reached this conclusion — the matrix is a runtime CSV
rebuilt from committed green, structurally over-claimed. Phase-4's `act_*`
grounding closed the *invocation* gap but not the *effect* gap: 236 dialog rows
and 131 action rows remain structural.

---

## Workstream 3 — UI oracle / visual-regression critique

**Does any rendered-output verification exist? YES.** Unlike a pure-structural
harness, PDFE's UI layer has a real pixel path:

- **Win32 `PrintWindow` (`PW_RENDERFULLCONTENT`)** canvas capture —
  `test_ui_file_operations.py:407`, `test_ui_annotations.py:995`,
  `test_ui_page_operations.py:399`.
- **SSIM** compare via `tests/visual/diff.py` — thresholds `SSIM>=0.95` (relaxed
  `0.85`) in file-ops / annotations / page-ops.
- **pixel_diff_fraction** change-detection (`SSIM<0.95 OR pixel_diff>=0.0001`) —
  `_assert_canvas_changed`.
- **Golden-image** capture + compare — `tests/ui_harness/core/screenshot.py`
  `capture_canvas` + `golden.py` `compare_to_golden`/`regen_golden`
  (`test_ui_harness_smoke.py`, the `*_smoke.py` files).
- **PyAutoGUI** screenshot artifact (diagnostic, not asserted) —
  `test_ui_specimen_a_e2e_walkthrough.py:163`.

**But the visual layer is shallow, and mostly dark in CI:**

1. **Change-detection ≠ correctness.** `_assert_canvas_changed` only proves
   *something* changed. A page rendered mirrored / wrong-zoom / wrong-rotation
   still trips the diff and passes — there is no golden of the *correct* result to
   compare against for most rows.
2. **Golden self-seeding.** First run with no committed golden calls
   `regen_golden(force=False)` then compares the just-captured image against
   itself → zero delta → always pass (`test_ui_harness_smoke.py:183-208`). On a
   fresh runner with no source-controlled golden, the "compare" protects nothing.
3. **Relaxed / opted-out thresholds.** Many file-ops rows are annotated
   `VISUAL: no` (native-shell dialogs excluded as OS-chrome; net-zero pixel by
   design), and the relaxed `0.85` SSIM tolerates large drift.
4. **Behind the skip wall.** Every visual assert sits behind `REQUIRES_LIVE_UI`
   + the gap_a14 UIA/dialog skips, so on Server-2019 it never runs.

### Regression classes INVISIBLE to the current UI layer

- Canvas renders the page **mirrored** or rotated the **wrong direction**
  (change-detector sees a diff → passes).
- Canvas renders at the **wrong zoom / tile scale** (half/double size → still a
  diff → passes).
- Glyphs render as **.notdef/tofu or blurry** while text still extracts correctly
  (extraction-blind — the phase-1 systemic disease).
- A toolbar button paints the **wrong icon / wrong color** (objectName-exists
  oracle is pixel-blind; the annotation toolbar has *no icons at all* per
  `UI-AN-22` and nothing catches it).
- A dialog **opens but its contents are garbled** (the 236 reachability rows
  assert only that the objectName exists in source).
- **White / invisible / half-size** edited text — the exact shipped
  mirror/half-size/color-loss content-stream bug class — no UI pixel oracle
  catches it.
- **Wrong-but-plausible** rendered content in a region (no committed golden to
  diff against, because the golden is self-seeded).

### Cross-reference — phase-1

Phase-1's oracle synthesis (`oracle-audit-synthesis.md`) found **widget-exists-only
in 77 files**, `EXPECT_NO_THROW-only` in 81, `extraction-only-for-visual` in 71;
weak-oracle (proxy + no-oracle-smoke) = **147/402 files (36.6%)**. Its
`systemic_pattern` note names extraction-only-for-visual as "the dominant
cross-cluster disease … structurally blind to mirror/invisible/white/wrong-glyph/
half-size/off-page." The UI layer's structural (objectName / signal-fired /
dialog-opened) oracles are the **same disease one level up**: a real window
renders, but nothing checks *what* it rendered.

---

## Bottom line

The UI/E2E layer looks broad on paper (762 matrix rows, 43 pytest files, a real
SSIM/golden path) but in the actual Server-2019 CI it is thin: **~472 skip sites
fire deterministically (gap_a14), gating ~300 of ~450 UI test functions from ever
asserting anything.** The green that remains is dominated by **widget-exists-only**
(236 dialog rows) and **invocation-fired** (131 action rows) structural oracles;
the genuine pixel verification exists but self-seeds its goldens, only
change-detects, and runs only on a developer desktop. The matrix's "green" is a
runtime CSV rebuilt from a one-off interactive run — reproducible protection in CI
is far smaller than the row count implies.
