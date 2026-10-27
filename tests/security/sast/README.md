# Phase 5 — SAST sweep (cppcheck + clang-tidy)

Static analysis across `src/` with two complementary tools:
`cppcheck` (whole-program flow + style + portability) and
`clang-tidy` (LLVM-quality compiler-front-end checks: bugprone,
cert, clang-analyzer, cppcoreguidelines, misc, modernize).

**Coverity is explicitly out of scope** -- no license available.
The dispatcher (`scripts/security/run_sast.ps1`) contains no
`cov-build` / `cov-analyze` / `cov-format-errors` invocations and
no `COVERITY_TOKEN` conditional.

## Goal

Not zero findings. **A known, justified baseline** so any *new*
high-severity finding in future work is visible to the next gate
review. Phase 9's regression check compares future SAST runs to
`tests/security/sast/baseline.json` -- a new high-severity hash
not in that file fails the regression check.

## OPT-IN posture

The SAST sweep is OPT-IN: a manual run via `run_sast.ps1`, or a
scheduled invocation in phase 9's nightly job. It is **not** part
of the regular `ctest` / `pdfe_tests.exe` flow -- no CTest entry
is registered for it.

## Files in this directory

| File | Purpose |
|---|---|
| `.clang-tidy`            | clang-tidy config (checks list + per-check options) |
| `cppcheck.suppress`      | cppcheck whole/file/line suppressions with rationale |
| `baseline.json`          | Set of accepted high-severity finding hashes |
| `triage.md`              | One disposition per high-severity finding (fix-now / accept / wontfix) |
| `README.md`              | This file |

## Tooling prerequisites

| Tool | Where to get it on Windows |
|---|---|
| `cppcheck` | https://cppcheck.sourceforge.io/ (or `choco install cppcheck`, `scoop install cppcheck`) |
| `clang-tidy` | Visual Studio 2022 Installer → "C++ Clang tools for Windows" -- bundled at `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-tidy.exe` |

clang-tidy ideally needs `compile_commands.json`, which the Visual
Studio generator does **not** emit by default. To enable full
clang-tidy coverage, configure with the Ninja generator:

```
cmake -B build/ninja -S . -G "Ninja Multi-Config" `
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

Then `pwsh scripts/security/run_sast.ps1 -BuildDir build/ninja`.

The dispatcher tolerates a missing `compile_commands.json` and
runs clang-tidy in best-effort mode -- the report flags this so
the partial coverage is visible rather than masquerading as a
clean run.

## Running

```powershell
# Both tools, default paths
pwsh scripts/security/run_sast.ps1

# Skip a tool that's not installed
pwsh scripts/security/run_sast.ps1 -SkipCppcheck
pwsh scripts/security/run_sast.ps1 -SkipClangTidy

# Override clang-tidy path (e.g., a different LLVM install)
pwsh scripts/security/run_sast.ps1 `
    -ClangTidyExe "C:\tools\llvm\bin\clang-tidy.exe"

# Update baseline after triaging findings in triage.md
pwsh scripts/security/run_sast.ps1 -UpdateBaseline
```

Output:

| File | Contents |
|---|---|
| `tests/security/reports/sast/findings.json` | Normalized findings (tool, rule_id, severity, file, line, message, hash) |
| `tests/security/reports/sast/summary.md`    | Counts per tool + severity, top-10 by rule_id |
| `tests/security/reports/sast/cppcheck.xml`  | Raw cppcheck XML output |
| `tests/security/reports/sast/clang-tidy.txt`| Raw clang-tidy text output |

## Severity normalization

| Source severity | Normalized |
|---|---|
| cppcheck `error` | `high` |
| cppcheck `warning` | `medium` |
| cppcheck `style` / `performance` / `portability` | `low` |
| cppcheck `information` | `info` |
| clang-tidy `error` | `high` |
| clang-tidy `warning` matching `cert-` / `bugprone-` / `clang-analyzer-` | `medium` |
| clang-tidy `warning` (any other check) | `low` |
| clang-tidy `note` | `info` |

## Why no live baseline is committed

Phase 5 acceptance criterion 4 says "tests/security/sast/baseline.json
captures the current set of finding hashes." That requires an actual
sweep with both tools installed and a `compile_commands.json` so
clang-tidy sees full include paths. Producing that baseline is a
hands-on step:

1. Install cppcheck (see prerequisites above).
2. Configure with the Ninja generator + `CMAKE_EXPORT_COMPILE_COMMANDS=ON`.
3. Run `pwsh scripts/security/run_sast.ps1`.
4. Open `tests/security/reports/sast/findings.json`. For each
   high-severity finding, add a triage entry in `triage.md` with
   one of `fix-now` / `accept` / `wontfix`.
5. For every `fix-now`: edit `src/` (no functional changes, just
   the SAST-prescribed fix), confirm by re-running the script and
   seeing the finding gone.
6. Run `pwsh scripts/security/run_sast.ps1 -UpdateBaseline` to
   write the canonical baseline.json. Commit it alongside the
   triage.md updates.

Phase 9's regression check then compares all future runs to that
baseline.

## Limitations

- **clang-tidy without compile_commands.json is partial.** Runs
  with the Visual Studio generator (the project's default) will
  miss include-path-dependent diagnostics. The report flags this
  but does not fail the run.
- **cppcheck is whole-source, not whole-program.** It does not
  consume compile_commands.json -- it scans `src/` files
  individually with explicit `-I src` and runs against C++20.
- **No Coverity, no PVS-Studio, no PolySpace.** Open-source
  toolchain only, by design (license posture).
- **Headers are scanned only via the `HeaderFilterRegex` clause in
  `.clang-tidy`** (`src/.*\.(h|hpp|inl)$`). Generated headers
  under `build/` are excluded.
