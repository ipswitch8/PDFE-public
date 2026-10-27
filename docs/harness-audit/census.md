# PDFE Test-Harness Census — Phase 0 (Adversarial Audit)

<!-- tracking-banner -->
> **STATUS: HISTORICAL EVIDENCE — this is not a work list.**
>
> **GitHub Issues (`ipswitch8/PDFE`) is the canonical tracker for anything still
> open.** This document records what was found or decided on the day it was
> written. Items here may have been fixed, superseded, or deliberately declined
> since. Where this document and a GitHub issue disagree, **the issue wins**.
>
> See [`docs/TRACKING.md`](../TRACKING.md).

---


**Generated:** 2026-07-27 from repo HEAD (`C:\Users\dev\source\PDFE`).
**Scope:** census only — no product/test code was changed; not committed.
**Method:** Grep/Glob + `grep`/`awk`/`python` over `tests/`, `.github/workflows/`,
`tools/gate/`; C++ counts cross-checked against the **built binary**
`build/vs2022/bin/Debug/pdfe_tests.exe` (built 2026-07-27). Every number below
states how it was derived so Phase 1–6 can trust or re-derive it.

Machine-readable mirror: `docs/harness-audit/census.json`.

---

## 1. Top-line numbers

| Metric | Value | Derivation |
|---|---|---|
| C++ GoogleTest files | **402** | `ls tests/test_*.cpp` |
| C++ tests (source macros) | **4,365** | `grep -E 'TEST(_[FP])?\('` → TEST 1363 + TEST_F 2992 + TEST_P 10 |
| C++ tests (binary-enumerated) | **4,625** | `pdfe_tests.exe --gtest_list_tests` leaf count |
| — source↔binary gap | **+260** | 10 `TEST_P` defs expand to 273 parametrized cases (273 lines contain `#`) |
| C++ assertions | **13,942** | `grep -E '(EXPECT|ASSERT)_[A-Z_]+\('` |
| C++ LOC (test) | **122,998** | `cat tests/test_*.cpp | wc -l` |
| Python UI test files | **129** | 43 `test_ui_*.py` + 86 `ui_automation/*.py` |
| Python UI test funcs | **730** | `grep 'def test_'` (450 + 280) |
| Python non-UI test files / funcs | **8 / 28** | `tests/test_*.py` minus `test_ui_*` |
| Qt Test standalone suites | **~0** | only `test_undo_manager.cpp` includes `<QtTest>`, compiled into the gtest binary |
| Fixtures (PDF, recursive) | **200** (86 top-level) | `find tests/fixtures -name '*.pdf'` |
| Fixture bytes | **183,799,897 (~175 MB)** | `find … -printf '%s' | awk sum` |
| CI workflows | **10** | `.github/workflows/*.yml` |
| Seed mutants | **3** | `tools/gate/mutants.json` |

**Source vs binary reconciliation:** binary (4,625) − source macros (4,365) =
260, fully explained by `TEST_P` instantiation expansion. **0 `DISABLED_`** tests
in either source or binary — the suite has no name-disabled tests; all runtime
exclusion is via `GTEST_SKIP()` guards (below).

**Assertion-count reconciliation (all within ~0.3% — not material).** Three
independent regex recounts of `EXPECT_*/ASSERT_*` over `tests/test_*.cpp` agree:
initial quick-grep **~13,975**, section-5 aggregate **13,942**, authoritative
per-file recount **13,937**. The spread (max 38 of 13,937 = **0.27%**) is pure
regex-method variance — whether the macro-tail char class is `[A-Z_]` vs
`[A-Za-z]` (the latter silently drops multi-underscore macros like
`ASSERT_NO_THROW` / `EXPECT_NO_FATAL_FAILURE`), line-based vs match-based
counting, and multi-line/commented-macro handling. **Authoritative ground truth
= the `per_file[]` sum = 13,937** (regex `\b(EXPECT|ASSERT)_[A-Z_]+\(`,
match-based, over all 402 files; see §8). The section-5 per-subsystem `assertions`
columns are left at their original 13,942-method values (Δ 5 = 0.036%) and are
not authoritative for exact totals — `per_file[]` is. Machine-readable:
`assertion_count_note` in `census.json`.

---

## 2. Test layers

### C++ GoogleTest — the primary suite (402 files, 4,365→4,625 tests, 13,942 assertions)
Assertion density ~3.19 EXPECT/ASSERT per test. This is the load-bearing layer.
`test_undo_manager.cpp` is the lone consumer of `Qt6::Test` utilities (QSignalSpy
etc.) — there is **no separate `QTEST_MAIN` Qt Test layer**; gap noted in
`tests/docs/qt_test_gap_audit.md`.

### Python UI (pywinauto / WinAppDriver / FlaUI / PyAutoGUI) — 129 files, 730 funcs
- `tests/test_ui_*.py`: 43 files, 450 `def test_`.
- `tests/ui_automation/*.py`: 86 files, 280 `def test_`.
- Durable (permanent) markers to scrutinize in Phase 5: **1** `@pytest.mark.skip`,
  **27** `@pytest.mark.skipif`, **24** `@pytest.mark.xfail`.
- **1,142** `pytest.skip()` call sites — overwhelmingly *runtime environment
  guards* (headless / no-desktop / UIA-wedge gap-A14 / fixture-absent), not
  permanent disables. Phase 5 must separate legitimate env-guards from
  "green because it never runs."

### Python robustness / non-UI — 8 files, 28 funcs
e.g. `test_pdf_open_robustness.py`, `test_ui_harness_smoke.py`.

### UI test matrix (CSV)
**973** CSVs under `tests/_runtime/` (`ui_test_matrix_*.csv` +
`p1_combined_aggregate.csv`) — these are **runtime-generated run artifacts, not
source-of-truth matrices**. The matrix *source* lives in
`tests/docs/generate_function_matrix.py`, `tests/scripts/check_matrix_health.py`,
`tests/scripts/sweep_runner.py`. Phase 5 reconciles matrix "pass" against
runnable/asserting reality (historical over-claim class).

### Gates & CI
- **Gate agents:** karen, validator, test-runner (+ security-audit,
  perf-benchmarks). Scripts: `~/.claude/hooks/emit-gate-verdict.sh`,
  `phase-gate.sh`.
- **`tools/gate/`:** `check_junit_xml.py` (GAP-A15-safe parser),
  `mutation_kill.py` + `mutants.json` (3 seed mutants), `check_dca.py`,
  `check_dca_presence.py`, plus their self-tests.
- **10 workflows:** windows/linux/macos-validation, governance, mutation-kill,
  windows-sanitizer, hetzner-sanitizer, windows-fuzz. `sanitizer.yml` and
  `fuzz.yml` (Linux) are **RETIRED / dispatch-only**.

---

## 3. Skip / disabled inventory (zero-protection risk surface)

| Kind | Count | Note |
|---|---|---|
| `DISABLED_` gtest | **0** | none in source or binary |
| `GTEST_SKIP()` sites | **1,230** in 247 files | mostly runtime fixture/env guards |
| `pytest.skip()` sites | **1,142** | mostly runtime env guards |
| `@pytest.mark.skipif` | **27** | durable — audit in P5 |
| `@pytest.mark.xfail` | **24** | durable — audit in P5 |
| `@pytest.mark.skip` | **1** | durable |
| `KNOWN_WEAK_TESTS.txt` entries | **52** non-comment | **but ~all RESOLVED** — see below |

**Top `GTEST_SKIP` concentration:** `test_pdf_canvas_q_invokable.cpp` (51),
`test_image_text_edit_display.cpp` (41), `test_page_commands.cpp` (27),
`test_image_manager.cpp` (27), `test_ocr_routing_fix.cpp` (24),
`test_pdf_content_writer.cpp` (23), `test_form_commands.cpp` (23).

**Important nuance for Phase 1/5:** the large skip counts are **not** 2,000+
permanently-dead tests. They are conditional `GTEST_SKIP()/pytest.skip()`
guards that fire only when a fixture or environment capability is absent. The
adversarial question is *how often that condition is true on CI* — a guard that
is always-true is a silent zero-protection test. That reconciliation is Phase 5
work (needs a real CI run log), not a static count.

**`KNOWN_WEAK_TESTS.txt` is a fixed-defect ledger, not a live weak list.** Of 52
non-comment entries, ~50 are `P-H-INFRA-*: RESOLVED …` post-mortems (125
"RESOLVED" occurrences); only ~2 entries remain open. Do not read "52" as "52
currently-weak tests."

---

## 4. Fixtures (input-domain seed for Phase 3)

- **200 PDFs, ~175 MB** total under `tests/fixtures/` (86 at top level).
- **`adversarial_ambient/` (17 PDFs)** — the newly-added hostile ambient-state
  corpus (`-100 Tz`, `Tr 3`, dangling `q`, non-RGB colorspace, MediaBox origin,
  rotation) seeded from the content-stream state-continuity audit. **This is the
  direct Phase-3 input-domain seed.**
- **`flaw_corpus/` (47 PDFs)** — tiers: corrupt, dimensions, encryption, fonts,
  forms, images, overlap, vector.
- **`realworld/` (29 PDFs)** — cid_mixed, corrupt_xref, embedded_files,
  form_calc, icc_profiles, jpeg2000, linearized, mixed_page_sizes,
  object_streams, rotated_pages, subsetted_fonts, unicode_metadata.
- `certs/` holds signing fixtures; `tessdata/` OCR data; `ui_screenshots/…` visual
  baselines.

---

## 5. Per-subsystem metrics (feeds Phase-1 oracle fan-out)

Filename-regex mapping over `tests/test_*.cpp`. `assert/test` counts only
`EXPECT_*/ASSERT_*`, so files using `SUCCEED()`/custom oracles **undercount** —
that undercount is itself a Phase-1 signal.

| Subsystem | Files | Tests | Assertions | Fixtures | Assert/Test |
|---|---:|---:|---:|---:|---:|
| other/uncategorized | 145 | 1451 | 4086 | 128 | 2.82 |
| textedit/content-edit | 41 | 518 | 1834 | 7 | 3.54 |
| fonts | 31 | 404 | 975 | 19 | 2.41 |
| ui | 31 | 332 | 1216 | 0 | 3.66 |
| tre | 16 | 246 | 759 | 7 | 3.09 |
| image | 21 | 244 | 824 | 5 | 3.38 |
| forms | 20 | 227 | 731 | 8 | 3.22 |
| redaction | 12 | 182 | 577 | 2 | 3.17 |
| signatures | 13 | 129 | 406 | 3 | 3.15 |
| save/document | 24 | 125 | 646 | 1 | 5.17 |
| annotations | 13 | 110 | 467 | 3 | 4.25 |
| security/crypto | 7 | 98 | 382 | 5 | 3.90 |
| core/infra | 8 | 88 | 306 | 1 | 3.48 |
| metadata | 5 | 43 | 144 | 2 | 3.35 |
| pages | 2 | 37 | 102 | 3 | 2.76 |
| undo/commands | 2 | 27 | 142 | 1 | 5.26 |
| extraction | 4 | 26 | 148 | 1 | 5.69 |
| bookmarks | 2 | 24 | 66 | 1 | 2.75 |
| reflow | 2 | 21 | 47 | 0 | 2.24 |
| layers | 1 | 21 | 45 | 0 | 2.14 |
| print | 2 | 12 | 39 | 3 | 3.25 |
| **TOTAL** | **402** | **4365** | **13942** | **200** | **3.19** |

**Weakest-oracle candidates by assertion density** (Phase-1 priority): layers
2.14, reflow 2.24, fonts 2.41, bookmarks 2.75, pages 2.76 — but density is only a
proxy; the real oracle-strength scoring is Phase 1.

### 5b. Per-subsystem SKIP breakdown

`GTEST_SKIP()` sites in each subsystem's `.cpp` files, and `pytest.skip()` /
`skipif` / `xfail` in its `.py` files. Aggregated over the **`per_file[]` broad
classifier** membership (§8) — this attributes more files than the narrower
section-5 regex, but the two **reconcile at the totals level**: Σ gtest-skip =
**1,230** (= §3), Σ pytest skip-sites = **1,142**, Σ skipif = **27**, Σ xfail =
**24**. (For a single fully self-consistent view where files/tests/assertions/
skips all use the same broad classifier, see `per_file_subsystem_rollup` in
`census.json`.) The bulk of pytest skips land in `ui` because nearly all Python
tests are UI-driver tests.

| Subsystem | GTEST_SKIP | pytest.skip | skipif | xfail |
|---|---:|---:|---:|---:|
| fonts | 207 | 3 | 0 | 0 |
| ui | 191 | 344 | 6 | 6 |
| image | 120 | 0 | 0 | 1 |
| other/uncategorized | 119 | 221 | 14 | 5 |
| forms | 77 | 114 | 2 | 2 |
| textedit/content-edit | 76 | 78 | 0 | 1 |
| save/document | 73 | 1 | 0 | 0 |
| tre | 69 | 11 | 0 | 0 |
| signatures | 62 | 57 | 0 | 2 |
| extraction | 43 | 4 | 0 | 0 |
| annotations | 42 | 102 | 1 | 1 |
| redaction | 32 | 120 | 0 | 5 |
| undo/commands | 28 | 0 | 0 | 0 |
| pages | 21 | 27 | 1 | 1 |
| core/infra | 19 | 17 | 0 | 0 |
| security/crypto | 19 | 31 | 3 | 0 |
| print | 13 | 6 | 0 | 0 |
| metadata | 10 | 0 | 0 | 0 |
| bookmarks | 8 | 6 | 0 | 0 |
| layers | 1 | 0 | 0 | 0 |
| reflow | 0 | 0 | 0 | 0 |
| **TOTAL** | **1,230** | **1,142** | **27** | **24** |

Each `census.json` subsystem entry now carries `gtest_skip_sites` + `pytest_skips`
fields (same numbers); basis documented in `subsystem_skip_basis_note`.

---

## 6. Instrumentation notes (what Phase 1–4 need wired)

- **Coverage — WIRED but NOT GATED.** `.github/workflows/windows-validation.yml:346`
  ("Code coverage (OpenCppCoverage → Cobertura) — best-effort", Register M-e
  standing control) already runs `OpenCppCoverage.exe` against
  `build/vs2022/bin/Debug/pdfe_tests.exe` on the self-hosted runner and uploads a
  Cobertura XML artifact — but it is `continue-on-error: true`, skips gracefully if
  the tool is absent, and enforces **no coverage floor**. So the gap is NOT
  "stand up coverage tooling" — it is "**add a coverage-floor / kill-rate gate on
  top of the existing best-effort step**" (a much smaller Phase-1/4 task). The
  step itself defers the gate decision to a later phase in its own comment.
  Cross-platform alt if ever needed: clang-cl + source-based llvm-cov (the
  `windows-fuzz` lane already uses clang-cl).
- **JUnit-XML / GAP-A15.** `tools/gate/check_junit_xml.py` already implements the
  safe parse: `pdfe_tests.exe` **exits 0 while the XML records failures**, so
  trust the XML. It asserts `sum(failures)==0` across `<testsuite>`, `tests>0`
  (empty run ≠ pass), and optional `--min-tests N` against silent suite
  shrinkage. Tell-tale of a masked failure: **`tests != passed + skipped`**. CI
  must run with `--gtest_output=xml` and pipe through this parser (not the exit
  code).
- **Mutation testing — home-grown, 3 seed mutants only.**
  `tools/gate/mutation_kill.py` + `mutants.json`. Each mutant is a single
  exact-unique `find`/`replace` source edit, validated by `--dry-run`, then
  apply → rebuild (`debug-vs2022`) → run the named `killing_test` → expect it to
  fail (killed). Phase 4 must grow the manifest per subsystem (flip conditionals,
  drop emitted operators e.g. `op_Tz(100)`, perturb signs/matrix terms) and wire
  `mutation-kill.yml` as a kill-rate floor gate. There is no off-the-shelf C++
  mutation harness here.
- **Known flaky crash.** Full-suite intermittent crash at
  `PerfStressTest.TenThousandAnnotationsOnePage`
  (`tests/test_performance_stress.cpp`) — non-deterministic full-run instability;
  quarantine-aware runs needed so it neither masks nor is masked by real failures.
- **Self-hosted CI reality.** Single Windows Server-2019 runner; never cancel an
  in-progress self-hosted job (wedges the runner). UIA menu automation hangs
  (gap-A14) → menus routed via `testToolActivatePipe`; a large share of live-UI
  skips are this env guard, not genuine coverage.

---

## 7. Surprises / flags for downstream phases

1. **Assertion-free / near-zero-oracle C++ files** (only `SUCCEED()` or ~1
   assertion despite many tests). NOTE — two categories here, do NOT conflate:
   (a) **intentional measurement-probes** (0 hard asserts BY DESIGN — `SUCCEED()`
   + `RecordProperty()` writing a machine-checked gap-register; each file's header
   self-documents "MEASUREMENT INSTRUMENT, not a correctness suite"): these are
   NOT weak-oracle defects — they are discovery instruments from prior completed
   pipelines, and the hard correctness assertions live in their companion regression
   files. (b) **genuine no-oracle smells** to score in Phase-1:
   - `test_content_stream_adversarial_ambient.cpp` — 9 tests, 0 EXPECT/ASSERT —
     **category (a), intentional probe** (companion hard-assert file:
     `test_content_stream_ctm_reset.cpp`). Not a defect.
   - `test_autosave_capture_audit.cpp` — 15 tests, 0 EXPECT/ASSERT —
     **category (a), intentional probe** (companion: `test_autosave_capture_fixes.cpp`).
     Not a defect.
   - `test_cve_replay.cpp` — **21 tests, 1 assertion — category (b), GENUINE weak
     oracle**: shared `verifySafeOpen()` = one `EXPECT_LT` timing bound around a
     `try{}catch(...){}` that swallows all exceptions → proves "didn't hang," NOT
     "handled safely." Prime Phase-1 target.
   - `test_font_fuzz.cpp` — 8 tests, 0 macro assertions (fuzz harness) — verify in
     Phase-1 whether it asserts via a helper or is a genuine smoke smell.
2. **Skip counts look alarming but mostly aren't permanent** — 1,230 gtest +
   1,142 pytest skip *sites* are largely runtime guards, not dead tests
   (0 `DISABLED_`). The real risk is *always-true* guards; needs a CI run log to
   quantify (Phase 5).
3. **`KNOWN_WEAK_TESTS.txt` is a resolved-defect ledger**, not a live weak list
   (~50/52 entries RESOLVED). Don't mistake it for current debt.
4. **Mutation coverage is a stub** — only 3 seed mutants exist; the empirical
   "green means something" proof (Phase 4) is essentially unbuilt.
5. **Qt Test layer is effectively absent** (1 file uses QtTest utilities).
6. **`other/uncategorized` = 145 files / 1,451 tests (33% of the suite)** — the
   regex map leaves a third of the suite unclassified; Phase 1 needs a hand pass
   to attribute these to subsystems before oracle scoring.
7. **UI subsystem shows 0 mapped fixtures** — UI tests drive the live app and
   generate/consume fixtures at runtime (screenshots, temp PDFs), so static
   fixture attribution misses them.

---

## 8. Per-file enumeration (Phase-1 oracle-audit input)

`census.json` now carries a **`per_file[]` array — one entry per C++ gtest file
(402)**: `{file, subsystem, tests, assertions, assert_density, gtest_skip_sites}`.
This is the granular data Phase-1's oracle audit needs to rank low-oracle files;
`subsystem` is the documented broad classifier (`per_file_classifier` in the
JSON). Authoritative sums: **402 files, 4,365 tests, 13,937 assertions, 1,230
GTEST_SKIP sites.**

### 8a. Lowest assert-density files — the Phase-1 no-oracle-smoke shortlist

The 18 lowest `assert_density` files with `tests>0`. **The two intentional
measurement-probes are EXCLUDED** from this smell ranking and listed separately
below (0 asserts *by design* — `SUCCEED()` + `RecordProperty()`; hard
correctness assertions live in companion regression files). Two entries here are
fuzz/robustness harnesses whose oracle may sit in a helper (Phase-1 confirms
genuine-smoke vs helper-asserted); `test_cve_replay` is the already-confirmed
genuine weak oracle.

| File | Tests | Asserts | Density | Note |
|---|---:|---:|---:|---|
| `tests/test_font_fuzz.cpp` | 8 | 0 | 0.00 | fuzz harness — helper-asserted? |
| `tests/test_cve_replay.cpp` | 21 | 1 | 0.05 | CONFIRMED genuine weak oracle |
| `tests/test_pkcs7_fuzz.cpp` | 4 | 1 | 0.25 | fuzz harness — helper-asserted? |
| `tests/test_save_path_security.cpp` | 6 | 4 | 0.67 | |
| `tests/test_link_tool.cpp` | 5 | 4 | 0.80 | |
| `tests/test_dll_dependencies.cpp` | 23 | 23 | 1.00 | |
| `tests/test_dialog_object_names_phase11.cpp` | 16 | 16 | 1.00 | |
| `tests/test_cnn_font_ranker.cpp` | 5 | 5 | 1.00 | |
| `tests/test_app_resources.cpp` | 3 | 3 | 1.00 | |
| `tests/test_xfa_save_benchmark.cpp` | 1 | 1 | 1.00 | |
| `tests/test_malicious_pdf_advanced.cpp` | 30 | 34 | 1.13 | |
| `tests/test_a2_script_detection.cpp` | 15 | 17 | 1.13 | |
| `tests/test_font_family_resolver.cpp` | 7 | 8 | 1.14 | |
| `tests/test_b2_measured_width.cpp` | 14 | 17 | 1.21 | |
| `tests/test_javascript_engine.cpp` | 23 | 28 | 1.22 | |
| `tests/test_pdfua_converter.cpp` | 9 | 11 | 1.22 | |
| `tests/test_scan_style_transfer.cpp` | 24 | 30 | 1.25 | |
| `tests/test_pdfa_validator.cpp` | 12 | 15 | 1.25 | |

**Excluded intentional measurement-probes (0 asserts BY DESIGN — NOT smells):**

| File | Tests | Asserts | Companion hard-assert file |
|---|---:|---:|---|
| `tests/test_content_stream_adversarial_ambient.cpp` | 9 | 0 | `tests/test_content_stream_ctm_reset.cpp` |
| `tests/test_autosave_capture_audit.cpp` | 15 | 0 | `tests/test_autosave_capture_fixes.cpp` |

Full ranking and both lists are machine-readable under
`per_file_low_density_shortlist` in `census.json`.

---

## Files written
- `C:\Users\dev\source\PDFE\docs\harness-audit\census.json`
- `C:\Users\dev\source\PDFE\docs\harness-audit\census.md`
