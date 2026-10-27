# Phase 4 — MuPDF integration fuzzing

> **Status (2026-04-25):** Phase 4 live runs are **deferred to a
> Linux build setup.** Windows clang-cl + Ninja + libfuzzer is a
> fragile combination -- see
> `tests/security/sanitizers/README.md` § "All sanitizers +
> fuzzing: deferred to Linux" for the specific failure chain we
> hit when attempting to enable phase 4 + 7 on Windows. The
> harness sources, dictionary, seeds, dispatcher, and baseline
> placeholder remain ready for a Linux build to consume.



Six libFuzzer / AFL++ harnesses that fuzz the **PDFE-side wrappers
around MuPDF**, not MuPDF itself. The point is to catch integration
defects in PDFE's `MuPdfContext`, `Document`, `Page`, `AnnotationManager`,
`FormManager`, and `AttachmentManager` glue — the places where PDFE
chose how to interpret malformed PDF input. Upstream MuPDF has its
own fuzzing program; ours covers what PDFE adds.

## OPT-IN posture

The fuzz binaries are **not** built by default. Both flags must be on:

```
cmake -B build/vs2022 -S . `
  -DPDFE_ENABLE_SECURITY_TESTS=ON `
  -DPDFE_ENABLE_FUZZING=ON `
  -DCMAKE_CXX_COMPILER=clang-cl `
  -DCMAKE_C_COMPILER=clang-cl
```

Setting `PDFE_ENABLE_FUZZING=ON` automatically forces
`PDFE_ENABLE_SECURITY_TESTS=ON` (see top-level `CMakeLists.txt`).

## Toolchain prerequisites

| Tool | Required for | Where to get it |
|---|---|---|
| **clang-cl** | libFuzzer backend (Windows) | Visual Studio Installer → "C++ Clang tools for Windows" |
| **afl-clang-fast++** | AFL++ backend (Linux CI) | apt install afl++ / [aflplus.plus](https://aflplus.plus) |
| `compiler-rt` runtime | ASan/UBSan symbols at link time | bundled with clang/clang-cl |

`-fsanitize=fuzzer,address,undefined` is **mandatory** for the
libFuzzer backend; the harness build will fail at link time without it.

If your default compiler is MSVC `cl.exe`, the CMake configure step
will emit a warning and skip the fuzz subdir — `cl.exe` does not
support `-fsanitize=fuzzer`.

## Backends

`PDFE_FUZZ_BACKEND` selects the entry-point macro the harnesses use:

| Value | Backend | Notes |
|---|---|---|
| `libfuzzer` (default) | clang-cl `LLVMFuzzerTestOneInput` | Windows-friendly |
| `aflpp` | AFL++ persistent-mode `__AFL_LOOP` | Linux CI |

```
cmake ... -DPDFE_FUZZ_BACKEND=aflpp
```

## Six harnesses

| Binary | Entry point fuzzed | Source |
|---|---|---|
| `fuzz_document_open` | `Document::open(ctx, path)` + dtor | `fuzz_document_open.cpp` |
| `fuzz_page_render` | `Document::open` -> `Page::render(QRectF, scale)` | `fuzz_page_render.cpp` |
| `fuzz_text_extract` | `Page::extractPlainText` (TextExtractor) | `fuzz_text_extract.cpp` |
| `fuzz_annotation_walk` | `AnnotationManager::getAnnotations(Page*)` | `fuzz_annotation_walk.cpp` |
| `fuzz_form_parse` | `FormManager::listFieldNames` + `getFormFields` | `fuzz_form_parse.cpp` |
| `fuzz_embedded_files` | `AttachmentManager::list` + `isPortfolio` + `portfolioView` | `fuzz_embedded_files.cpp` |

Each harness has the same shape: bound input size, write bytes to a
per-iteration temp file (PDFE's `Document::open` takes a path, not a
buffer), open + exercise + tear down, swallow C++ exceptions because
"refused to open malformed input" is the *expected* path — only
ASan/UBSan-detected memory faults and crashes count as findings.

## Seeds

`seeds/<harness>/` contains one or more representative valid PDFs.
At configure time CMake stages them into `build/<preset>/fuzz/seeds/<harness>/`
where libFuzzer / AFL++ pick them up via the `seed_corpus` argument.

Default seeds:

| Harness | Seed |
|---|---|
| fuzz_document_open | `tests/fixtures/test_blank.pdf` |
| fuzz_page_render | `tests/fixtures/test_blank.pdf` |
| fuzz_text_extract | `tests/fixtures/test_3page.pdf` |
| fuzz_annotation_walk | `tests/fixtures/test_with_freetext_annot.pdf` |
| fuzz_form_parse | `tests/fixtures/test_form.pdf` |
| fuzz_embedded_files | `tests/fixtures/test_portfolio.pdf` |

Adding malicious-PDF corpus entries (the structurally-valid ones)
to the seed pool is encouraged once a baseline run is clean.

## Dictionary

`pdf.dict` contains PDF token strings (object types, content-stream
operators, /JS /Launch /SubmitForm and other PDFE attack-surface
keywords). libFuzzer's `-dict=pdf.dict` and AFL++'s `-x pdf.dict`
both consume this format.

## Driving a run

Use `scripts/security/run_fuzz.ps1`:

```powershell
# 5-minute smoke run per harness (default; ~30 minutes total)
pwsh scripts/security/run_fuzz.ps1 -Smoke

# 60-minute nightly run per harness (~6 hours total)
pwsh scripts/security/run_fuzz.ps1 -Nightly

# Single harness, custom budget
pwsh scripts/security/run_fuzz.ps1 -PerHarnessSeconds 600 -Harness fuzz_form_parse
```

Output: `tests/security/reports/fuzz/run_<timestamp>/`
- `summary.json` — overall verdict + per-harness execs/sec, coverage,
  unique crashes, OOMs, timeouts
- `<harness>/fuzz.log` — libFuzzer's own stdout/stderr
- `<harness>/crashes/crash-*` — original crash inputs
- `<harness>/min_crash-*` — minimized crash inputs (libFuzzer
  `-minimize_crash=1` post-pass)

The wrapper exits 0 if every harness reports zero unique crashes,
1 otherwise.

## Why no live baseline is committed

Phase 4 acceptance criterion 8 requires a clean baseline (zero
crashes for a 5-minute-per-harness smoke). That baseline must be
produced from an actual fuzz campaign with the right toolchain
(clang-cl + ASan/UBSan), which is a hands-on step:

1. Configure with `-DPDFE_ENABLE_FUZZING=ON -DCMAKE_CXX_COMPILER=clang-cl`
2. Build the six harnesses
3. `pwsh scripts/security/run_fuzz.ps1 -Smoke`
4. If `summary.json` reports `overall_verdict=pass`, copy it to
   `tests/security/reports/fuzz/baseline.json`. Phase 9's
   regression check compares future runs to that baseline.

If the smoke run does discover a crash, AC9 says: file as a defect,
either fix in this phase or accept with a written rationale and a
regression test under `tests/security/fuzz/known_issues/`. **Do
not silence findings.**

## Stopping rules

A fuzz campaign is done when EITHER:
- The wall-clock budget runs out (libFuzzer's `-max_total_time`), OR
- A crash is discovered and minimized (early-exit is acceptable for
  smoke runs; nightly runs can use `-jobs=N` to keep going past
  individual crashes).

Coverage plateaus do not by themselves end a run; they signal a
seed-corpus expansion is needed (add a malicious-PDF corpus entry
to the seeds dir) rather than that the harness is "done."

## Known limitations

- **MSVC cl.exe is unsupported.** `cl.exe` lacks `-fsanitize=fuzzer`.
  The CMake configure step skips the fuzz subdir with a warning when
  the compiler ID isn't Clang.
- **Each iteration writes a temp file.** `Document::open` takes a
  filesystem path, not a buffer. The temp-file write/unlink is per-
  iteration overhead (~5-15% on smoke runs). Fixing this requires a
  PDFE source change to add an open-from-buffer entry point —
  deferred to a follow-up phase if perf becomes an issue.
- **The harnesses suppress C++ exceptions.** Throws from
  `MuPdfException` are *expected* refusals, not bugs. Memory
  corruption (caught by ASan/UBSan) and SIGSEGV remain failure
  signals.
- **Fuzz binaries are not registered as CTest entries.** Phase 4's
  outputs are time-bounded campaigns, not boolean tests. Phase 9
  wires nightly runs into a separate cron-style invocation.
