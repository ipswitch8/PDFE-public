# Phase 2 — Canonical Sharded Full-Suite Test Harness (Windows)

Pipeline: `windows-fixes`, phase-2.
Status: **implementation complete; full-suite VALIDATION deferred to the
orchestrator** (see "Deferred validation" below).

## Problem

A single-process run of the whole `pdfe_tests.exe` GoogleTest suite (~4700
tests) **non-deterministically hard-crashes** at a random late
Qt/MainWindow-heavy test. Because the process dies before GoogleTest writes its
JUnit XML, the run produces **no XML at all** — so there is nothing to enumerate,
nothing to re-run, and no green/red signal. Each run crashes at a *different*
late test, which is the signature of accumulated process state rather than a
single deterministic bug.

## Root cause (established, not re-investigated here)

**Cumulative process state**, i.e. process-wide resource accumulation across
thousands of tests in one `pdfe_tests.exe` process:

- Qt global singletons / static state,
- `QApplication`-level caches,
- leaked OS handles,
- shared temp-file / font-cache-sqlite / clipboard state (this is also
  **defect #39** — two concurrent `pdfe_tests.exe` processes corrupt each other
  through exactly these shared external resources).

Eventually a late Qt-widget-heavy test is corrupted. The canonical documented
example is `MainWindowIntegrationTest.ZoomComboExists`: after thousands of prior
tests, a freshly constructed `MainWindow`'s `createToolBar()` occasionally fails
to populate the toolbar (the `zoomCombo` `QWidgetAction` is missing from
`mainToolBar->actions()`), so the assertion fails — even though `zoomCombo` is
present by construction in the running app.

### Evidence pointers

- `.github/workflows/windows-validation.yml`, the "Run pdfe_tests" step comment
  (~lines 288–309): documents that such MainWindow-integration tests become
  "PROBABILISTICALLY flaky under the cumulative state of the ~3900-test single
  process ... though the test passes reliably in a FRESH process and in
  isolation."
- `tests/scripts/run_gtest.py` header docstring (pre-existing): same
  cumulative-process-state description for `ZoomComboExists`.
- The B1 fix agent independently confirmed the full suite passes **4706/0** when
  run as **16 sequential shards** (fresh process per shard), and crashes
  non-deterministically when run as one process.

**It is PROVEN pre-existing and PROVEN to be cumulative-state** — not a defect
introduced by any windows-fixes change.

## Why sharding fixes it

A shard is a **fresh `pdfe_tests.exe` process running only a few hundred tests**.
The process-wide accumulation never reaches the corruption threshold, so the
late Qt-widget-heavy tests that fail under cumulative state pass cleanly. Each
of the previously-observed crash sites passes in a fresh, lightly-loaded process
(and in isolation). Running shards **sequentially, never concurrently**, also
avoids defect #39 (two same-machine `pdfe_tests.exe` processes corrupting each
other via shared temp/font-cache-sqlite/clipboard).

This is a **harness institutionalization**, not a product change: the crash is a
cumulative-state artifact of the giant single-process run, so the resolution
(per phase-2 acceptance criteria) is a canonical single-process-at-a-time
execution harness, not a code edit.

## Design

Implemented by **extending** `tests/scripts/run_gtest.py` (no parallel fork).
The pre-existing single-process + rerun behavior is preserved as the default
(`--shards 1`) for back-compat; `--shards N` (N>1) selects the sharded harness.

### 1. Enumerate once

`_list_tests()` obtains the full test-id list either from
`pdfe_tests.exe --gtest_list_tests` **at run time** (enumeration lists only; it
executes no test body and is near-instant) or, when `--test-list-file` is given,
by reading a cached listing directly (no process spawned). `parse_gtest_list()`
handles the gtest listing format — non-indented `Suite.` headers, indented test
names, value-parameterized `# GetParam() = ...` comments stripped — and also a
plain "one id per line" file.

### 2. Partition deterministically

`partition_tests(ids, N)` assigns test *i* to shard *i % N* (round-robin).
Guaranteed and unit-tested:

- **deterministic** — same input → same partition,
- **disjoint + total** — every id in exactly one shard, none dropped/duplicated,
- **balanced** — shard sizes differ by at most 1,
- order-preserving within a shard; empty shards kept when N > test count so
  shard indices stay stable.

Each shard becomes a `--gtest_filter` (`shard_filter()` joins ids with `:`; test
ids contain no `:`).

### 3. Run each shard sequentially, own XML

Each non-empty shard runs as a **separate, sequential** `pdfe_tests.exe` process
writing `gtest_shard_<i>.xml` (+ `gtest_shard_<i>.log`). **Never concurrent.**

### 4. Merge → GAP-A15-compliant aggregate

`aggregate_shards()` merges the per-shard roots into one `gtest_full.xml`
`<testsuites>` tree. Totals are counted from the actual aggregated testcases so
that `tests == passed + failed + skipped + errors` (GAP-A15: no silent
truncation).

#### Crashed-shard → RED invariant (the core safety property)

If a shard produced **no parseable, non-empty XML** (`_is_crashed()` — a `None`
root *or* a parseable root with zero `<testcase>` elements), then **every test
assigned to that shard is synthesized as an `<error>`** in the aggregate. A
crashed shard therefore:

- is **never silently dropped** — its assigned tests are all accounted for, and
- **turns the aggregate RED** — the run can never be vacuously green just
  because the other shards passed.

This is directly unit-tested (`test_crashed_shard_none_root_becomes_all_errors_red`,
`test_no_shard_is_vacuously_green_when_one_crashes`,
`test_crashed_shard_empty_root_also_red`).

### 5. Rerun genuine failures (flake healing preserved)

After the sharded run, every failed/errored test id is re-run individually in a
**fresh process** (`_rerun_failures()`, up to `--reruns`, default 2). A transient
flake heals on the fresh rerun; a genuine failure fails every rerun and stays
red (exit non-zero). Healed vs unresolved tests are written to
`healed_flakes.json` for the R-37 flaky-quarantine gate. A shard that spuriously
crashed can heal here (its tests re-run individually); a real failure inside a
crashed shard stays red.

### Summary output

Per run: enumerated count, per-shard `test count / rc / ok|CRASHED`, aggregate
`tests/passed/failures/errors/skipped`, the list of crashed shard indices (if
any), and the genuine-failure list.

## How to invoke

```sh
# Canonical Windows harness (16 sequential fresh-process shards):
python tests/scripts/run_gtest.py \
    --exe build/vs2022/bin/Debug/pdfe_tests.exe \
    --output-dir build/gtest_out \
    --shards 16 --reruns 2

# Partition from a cached listing instead of enumerating at run time:
python tests/scripts/run_gtest.py --exe .../pdfe_tests.exe \
    --output-dir build/gtest_out --shards 16 \
    --test-list-file build/gtest_out/gtest_list.txt

# Back-compat single-process mode (default):
python tests/scripts/run_gtest.py --exe .../pdfe_tests.exe \
    --output-dir build/gtest_out            # == --shards 1
```

Exit code 0 iff, after reruns, there are zero unresolved failures **and** no
shard crashed unrecovered. `gtest_full.xml` is the aggregate JUnit for artifact
upload / the GAP-A15 single-source checker.

> **Consumer contract (GAP-A15).** The **aggregate `gtest_full.xml` is the
> authority — never the harness process exit code.** The aggregate is written
> once, from the raw per-shard XMLs, *before* the rerun-heal pass; a crashed
> shard is recorded there as `<error>` cases and stays RED even if a later rerun
> "heals" the exit code to 0 (the heal is tracked separately in
> `healed_flakes.json`). CI and every downstream gate must parse the aggregate
> XML and assert `failures==0 && errors==0 && tests==passed+failed+skipped+errors`
> — exactly the existing red-JUnit-sentinel / single-source-checker discipline —
> so a healed-but-crashed shard can never present as silently green.

## Pure-logic unit tests

`tests/scripts/test_run_gtest_sharding.py` covers the process-free logic with
synthetic id lists and synthetic JUnit XML — it **never launches pdfe_tests** and
runs under plain `python -m pytest`:

- partition: disjoint, total, deterministic, balanced, order-preserving,
  more-shards-than-tests, single-shard identity, rejects 0 shards;
- `parse_gtest_list`: basic + value-parameterized + banner/blank rejection +
  plain-id list;
- `aggregate_shards`: GAP-A15 totals, and the crashed-shard→RED invariant in
  three forms (None root, empty root, one-crash-among-greens), plus the
  all-green case.

Result at implementation time: **16 passed** (`python -m pytest
tests/scripts/test_run_gtest_sharding.py -q`).

## Crash sites the harness must show passing (once validated)

Per the phase-2 acceptance criteria, the following previously-observed crash
sites must all pass cleanly under the sharded harness:

- `CompareDocumentsView.NextChangeNavigation`
- `ValidationRequestTest` (suite)
- `MainWindowIntegrationTest.ZoomInActionHasStandardShortcut`
- `TreCommandsTest.ReconstructCommand`
- `Phase9ChromeTest` (suite)
- `PdfCanvasKeyboardScroll`
- `DocumentSaveAtomicTest`

(Also the canonical `MainWindowIntegrationTest.ZoomComboExists`.)

## Deferred validation

**Full-suite VALIDATION is deferred to the orchestrator.** Running
`pdfe_tests.exe` under this harness (the phase-2 criteria: a complete, GAP-A15
JUnit with 0 unexplained failures across **≥2 consecutive runs**, with the crash
sites above green) requires building and executing `pdfe_tests.exe`. At
implementation time a self-hosted **mutation-kill CI run was in progress on the
same Windows machine**, rebuilding and running `pdfe_tests.exe` ~16 times. Two
concurrent `pdfe_tests.exe` processes corrupt each other via shared external
resources (defect #39), so this phase was **code-writing + docs only** — no
build, no `pdfe_tests` run, no `cmake`/MSBuild, no CI dispatch. The orchestrator
runs the sharded harness for ≥2 runs once the runner is free and gates on the
result.

### Items to confirm during validation

- **Shard count**: 16 shards matched the B1 agent's 4706/0 result; confirm this
  count stays comfortably below the corruption threshold (a shard should be a
  few hundred tests, not thousands).
- **id ↔ XML matching**: `parse_gtest_list` emits full ids exactly as
  `--gtest_list_tests` prints them (including value-parameterized `Prefix/Suite.
  Test/0`). The crashed-shard synthesis derives `classname`/`name` by splitting
  on the first `.`; confirm the aggregate testcase identities match what a real
  shard XML emits for parameterized suites (the merge counts real XML cases
  verbatim, so this only affects the *crashed* path's synthesized names).
- **Enumeration parity**: confirm `--gtest_list_tests` (optionally filtered) at
  run time yields the same population the shards then run, so no test is
  enumerated-but-unassigned or assigned-but-missing.
