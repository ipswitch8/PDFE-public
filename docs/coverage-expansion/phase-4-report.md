# Coverage-Expansion Phase-4 Report — Perf-Regression Floor (C-07) + Fuzzing Depth (C-09)

Pipeline: `coverage-expansion`, phase-4. Register: `docs/coverage-expansion/register.json` (C-07, C-09).

Stance: ADD real gates + real oracles, never weaken/skip. No product source changed (none required — no bug found).

---

## C-07 — Performance-regression floor gate

The perf tests (`test_performance_stress.cpp` / `test_search_performance.cpp` /
`test_xfa_save_benchmark.cpp`) MEASURE hot paths but assert no floor — a
regression (the XFA-save 317s class) is timed and printed, never gated. Phase-4
turns measured timings into a GATE, modelled on the Phase-5 mutation-kill floor.

### Deliverables

| File | Role |
|---|---|
| `tests/test_perf_benchmark.cpp` | New `PerfBenchmark` gtest — measures 4 KEY hot paths (document open, save-routing, search, tile/display-list render) as medians over 5–7 iters and EMITS them to a measured JSON (`PDFE_PERF_JSON`). Also asserts generous absolute sanity ceilings + functional correctness (open page-count, search runs, display list produced), so it is a real test, not just a writer. |
| `tools/gate/check_perf_floor.py` | The gate. Compares a measured JSON to committed baselines; EXITS 1 on any metric past `baseline × threshold`, 2 on a dropped metric / missing-or-bad input, 0 within. |
| `tools/gate/perf_baselines.json` | Committed SAME-MACHINE baselines (per-metric `baseline_ms`, default 3× threshold). |
| `tools/gate/test_check_perf_floor.py` | 7 unit tests proving the floor gates (regressed→1, within→0, per-metric-threshold override, dropped-metric→2, missing-files→2, empty-metrics→2, JSON report). |
| `.github/workflows/windows-validation.yml` | New "Performance-regression floor gate (C-07)" step: runs `PerfBenchmark.*`, then the gate against the baseline. The perf-benchmarks gate agent consumes this lane. |

### Proof (regressed → exit 1, within → exit 0)

- Unit tests: **7 passed** (`test_check_perf_floor.py`).
- Synthetic exit-code proof against the committed baseline:
  - within-threshold measurement (doc_open 6.0ms vs baseline 2.5ms → 1.2× of the 3× limit) → **exit 0**.
  - regressed measurement (doc_open 25.0ms → 5.0× baseline, past 3× limit) → **exit 1**, only the regressed metric flagged.
  - dropped metric / missing file / empty metrics → **exit 2** (hard error, never a silent pass).
- Against a **real** `PerfBenchmark` run on this box → **exit 0** (all metrics 0.2–0.5× of baseline).

### Machine-variance approach + limitation (documented honestly)

Absolute wall-clock is machine/thermal/Debug-vs-Release dependent, so the gate is
**RELATIVE to a same-machine baseline** with a **generous 3× threshold**. Observed
medians on this dev box were extremely stable across runs (doc_open ~1.1ms,
save ~3.4ms, search ~0.5ms, tile ~0.13ms; variance <1.15× run-to-run). Baselines
are set to ~2× the observed median for headroom, so ordinary noise never trips the
gate while a real large hot-path regression (10×–100×) does. **Limitation:** a
baseline captured on a faster box produces false regressions on a slower one — the
baseline MUST be regenerated on the machine that runs the gate (documented in the
baseline file comment + the CI step + the gate script header).

---

## C-09 — Fuzzing depth (real oracles)

### `test_font_fuzz.cpp`: 0 → real assertions (the audit-flagged swallow-all removed)

The original harness wrapped every parse in a bare `catch (...)` that swallowed
EVERYTHING and asserted **nothing** — a genuine crash/UAF/hang or an unexpected
throw would have PASSED. Rewritten with a real oracle:

- **(a) no crash / UAF / unexpected throw** — the bare `catch (...)` is REPLACED
  by the benign-throw triage classifier (`pdfe::fuzz::triage_or_rethrow` from
  `security/fuzz/fuzz_support.h`): an expected malformed-input refusal
  (`MuPdfException`/parse error) is swallowed, but `std::bad_alloc`, a simulated
  memory fault, or any other non-refuse exception is **re-thrown and fails the
  test**. A non-std C++ throw / hard SEH fault is left uncaught on purpose so it
  crashes and is recorded.
- **(b) bounded time** — each input's parse must return under a 5000ms wall-clock
  cap (slow-but-terminating pathological input fails here; a true infinite hang is
  the libFuzzer `-timeout` job's responsibility in the coverage-guided CI lane).
- **(c) output invariant** — a parse that succeeds returns a structurally-valid
  Document (non-negative, self-consistent page count; page obtainable when
  pageCount>0) AND the same input parsed twice yields the same structural outcome
  (determinism — a half-constructed state would not). A **positive control**
  (`BenignFontDictOpensAndRenders`) MUST open with pageCount==1 AND render,
  proving the harness actually reaches the parse/render path (not vacuously
  passing because open always fails).

Now **14 test cases** (was 8, all assertion-free), every one driving the shared
`expectRobustParse` oracle (6 assertion sites) plus the positive control — from
**0 assertions to a real oracle executed on every input**.

### Seed extension (deterministic, adversarial)

- Inline adversarial cases added to `test_font_fuzz.cpp`: oversized numeric values
  (int-overflow bait), deep nesting (recursion bait), embedded terminator tokens,
  progressive-truncation sweep, deterministic LCG-seeded random blocks.
- `tests/security/fuzz/seeds/gen_adversarial_seeds.py` — deterministic generator
  writing 5 adversarial CLASSES (malformed header, truncation, oversized values,
  deep nesting, circular ref) into 4 harness seed dirs (20 committed `.pdf`
  seeds). These are picked up automatically by `test_fuzz_harness_smoke.cpp`
  `driveCore` → exercised with the strengthened no-crash/bounded-time/triage
  oracle (confirmed: `FuzzWriteSurfaces.*` green with the new seeds present).

### `test_pkcs7_fuzz.cpp` strengthened

Added bounded-time caps, an OUTPUT INVARIANT (a successful `d2i_PKCS7` carries a
non-null content-type OID; zero-byte / uniform-garbage inputs are clean refusals,
not parses), and a determinism check (same seed → same parse verdict). Wrapped the
smoke drives in `ASSERT_NO_THROW`.

### Coverage-guided nightly campaign (extended + honest disclosure)

`.github/workflows/windows-fuzz.yml`: nightly per-harness duration extended
**1800s → 3600s** so coverage-guided depth compounds further each night against
the persisted + `-merge=1`-minimised corpus (all pre-existing R2-13 markers —
`schedule`/`cron`/`workflow_dispatch`, `continue-on-error: true`, `actions/cache`
+ `restore-keys`, `-merge=1`, the 4 write-surface harness names — preserved;
`CiFuzzWorkflows.*` 6/6 green). Added an explicit **coverage-guided disclosure**:
true coverage-guided libFuzzer saturation runs ONLY in the clang-cl self-hosted
lane (the only place PDFE builds with `-fsanitize=fuzzer`); the deterministic
SEED-REPLAY oracle runs EVERYWHERE in the MSVC gtest suite but does NOT mutate or
pursue new coverage. **Neither is faked as the other** — no coverage-guided run
was fabricated locally.

---

## Verification

- Build: `taskkill` + synchronous `cmake --build build/vs2022 --config Debug --target pdfe_tests` → **succeeded** (only the pre-existing benign LNK4098 MSVCRT warning).
- Perf gate: `test_check_perf_floor.py` 7/7; synthetic regressed→exit1 / within→exit0 / hard-error→exit2 proven; real `PerfBenchmark` run → gate exit 0.
- gtest (JUnit XML parsed): `PerfBenchmark.*` = 5 tests 0 fail/0 err; `FontFuzzSmokeTest.* + Pkcs7FuzzSmokeTest.* + Fuzz*` = 30 tests 0 fail/0 err; `CiFuzzWorkflows.*` = 6/6. Deterministic across repeated runs.
- `git status --porcelain -- src/`: **CLEAN** — no product source changed.
- Registration: `test_perf_benchmark.cpp` added to `tests/CMakeLists.txt`; `tools/gate/*` force-added past the broad `tools/` gitignore (`git add -f`); `__pycache__` excluded; 20 adversarial seeds + generator staged.

## Bug status

**No real bug or crash discovered.** The strengthened font/pkcs7 fuzz oracles and
the 20 new adversarial seeds all pass — every malformed input is refused/repaired
cleanly (benign, swallowed by triage), none tripped a MustCrash rethrow, and the
positive control opens+renders. src/ is clean; no product change was needed.
