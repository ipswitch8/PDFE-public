# Phase 7 — Sanitizer-instrumented test runs

Builds the PDFE GoogleTest suite with **clang-cl + AddressSanitizer
+ UndefinedBehaviorSanitizer** and runs all 2068 tests under that
instrumentation. Catches use-after-free, out-of-bounds reads/writes,
double-frees, leaks (LSan), and undefined behavior that the regular
optimizer can hide in Release/Debug builds.

## OPT-IN posture

The sanitizer build uses a **separate binary directory**
(`build/vs2022-asan`) so the regular VS2022 build under
`build/vs2022` is undisturbed. No CTest entries are registered for
the sanitizer run -- it's a time-bounded campaign invoked manually
or by phase 9's nightly job.

## All sanitizers + fuzzing: deferred to Linux

**Phases 4 (libfuzzer harnesses) and 7 (ASan/UBSan ctest run) are
deferred to a Linux build setup.** The Windows clang-cl + Ninja +
sanitizers toolchain has multiple known-fragile failure modes
that would each need individual workarounds; in practice, the
LLVM-on-Linux toolchain has no equivalent issues, and a Linux
build of PDFE is also a prerequisite for MSan.

### Specific Windows clang-cl issues we hit

A real attempt to wire phase 4 + 7 in one elevated session ran
into these in sequence (each fixed before exposing the next):

1. **clang-cl not on PATH.** VS Installer's "C++ Clang tools for
   Windows" workload installs clang-cl but does not modify PATH.
   *Fixed by referencing the absolute path in the preset:
   `C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/x64/bin/clang-cl.exe`.*
2. **`/fsanitize=undefined` not recognized.** clang-cl accepts
   `/fsanitize=address` (Windows-style slash) but not
   `/fsanitize=undefined`; needs `-fsanitize=undefined`
   (Unix-style dash). *Fixed by mixed-form flags.*
3. **AddressSanitizer + `/MDd` Debug runtime incompatibility.**
   `clang-cl: error: invalid argument '-MDd' not allowed with
   '-fsanitize=address'`. *Fixed by setting
   `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL`.*
4. **rc.exe / lld-link manifest gymnastics fail.** CMake's
   `-E vs_link_exe --rc=rc --mt=llvm-mt.exe --manifests` wrapper
   tries to compile a manifest.rc that doesn't exist, with rc.exe
   reporting "no such file or directory". This persists even with
   `vcvars64.bat` loaded into the calling session. **Not fixed.**

The common pattern: each clang-cl + Windows-SDK + sanitizer
combination has an environmental gotcha that's well-documented
on Linux but treated as edge-cases by MS-on-Windows tooling.

### What deferring to Linux would unlock

Adding a Linux build of PDFE enables ALL of the following at
once, with the same clang/clang++ toolchain that LLVM ships
upstream:

- **ASan** — heap buffer overflow / use-after-free / leaks
- **UBSan** — typed undefined behavior (signed overflow, bad
  shifts, misaligned load, etc.)
- **MSan** — uninitialized memory reads (Linux-only on Clang;
  not even attempted on Windows)
- **TSan** — data race detection (Linux/macOS only on Clang)
- **libfuzzer harnesses** — phase 4's six fuzz binaries, with
  `-fsanitize=fuzzer,address,undefined` working out of the box

Prerequisites for the Linux build:

1. A Linux build of PDFE (CMake config, Qt6 Linux, MuPDF Linux,
   QPDF Linux, tesseract Linux, harfbuzz Linux, the rest of
   vcpkg's transitive set under `x64-linux` triplet).
2. A `linux-debug-asan` / `linux-debug-msan` preset with the
   appropriate `-fsanitize=...` flags.
3. The dispatchers (`run_sanitizers.ps1`, `run_fuzz.ps1`) need
   Linux equivalents (bash) or a `linux-` mode added.

When a Linux build target is added, the new presets should be
wired into the same per-phase dispatcher pattern as the existing
ones. The `tests/security/sanitizers/baseline.json` schema
already supports per-finding hashes, so the same baseline file
can hold ASan, UBSan, AND MSan suppressions distinguished by
`kind`.

## Toolchain prerequisites

| Tool | Required for | Where to get it |
|---|---|---|
| `clang-cl` | ASan + UBSan instrumentation | Visual Studio Installer -> "C++ Clang tools for Windows" workload (path: `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-cl.exe`) |
| `clang_rt.asan*-x86_64.dll` | ASan runtime | bundled with clang-cl |
| Ninja | Generator (asan preset uses Ninja Multi-Config) | Visual Studio includes one; or `choco install ninja` |
| ctest | Test runner | bundled with CMake |

The asan CMake preset `windows-vs2022-asan` configures with
`CMAKE_C_COMPILER=clang-cl`, `CMAKE_CXX_COMPILER=clang-cl`, and
`/fsanitize=address /fsanitize=undefined /Zi /Od` flags. The
matching build preset is `debug-vs2022-asan` and the test preset
is `test-vs2022-asan`.

## Files in this directory

| File | Purpose |
|---|---|
| `lsan.supp`        | LeakSanitizer top-frame substring suppressions (Qt statics, MuPDF font cache, OpenSSL init) |
| `ubsan.supp`       | UBSan rule:source suppressions (Qt moc, MuPDF setjmp/longjmp) |
| `baseline.json`    | Hashes of accepted sanitizer findings |
| `triage.md`        | Per-finding disposition (fix-now / accept) |
| `README.md`        | This file |

## Running

```powershell
# Configure + build + test, full pipeline
pwsh scripts/security/run_sanitizers.ps1

# Just configure (sanity-check the asan preset wiring)
pwsh scripts/security/run_sanitizers.ps1 -ConfigureOnly

# Skip the build (assume the asan tree is already built)
pwsh scripts/security/run_sanitizers.ps1 -NoBuild

# Update baseline after triaging in triage.md
pwsh scripts/security/run_sanitizers.ps1 -UpdateBaseline
```

Output:

| Path | Contents |
|---|---|
| `tests/security/reports/sanitizers/run_<stamp>/ctest.log`     | Full ctest output including sanitizer reports |
| `tests/security/reports/sanitizers/run_<stamp>/findings.json` | Normalized findings (kind, test_name, frames[5], hash) |
| `tests/security/reports/sanitizers/run_<stamp>/summary.md`    | Counts by kind + non-suppressed verdict |
| `tests/security/reports/sanitizers/summary.md`                | Latest run mirrored to canonical path |

## Sanitizer environment variables

The dispatcher sets these before invoking ctest (paths shown
relative to repo root for readability; actual values are absolute,
resolved from `$RepoRoot/tests/security/sanitizers/`):

```
ASAN_OPTIONS=halt_on_error=0:abort_on_error=0:print_stacktrace=1:exitcode=42:detect_stack_use_after_return=1
UBSAN_OPTIONS=halt_on_error=0:print_stacktrace=1:print_summary=1:suppressions=<abs>/ubsan.supp
LSAN_OPTIONS=suppressions=<abs>/lsan.supp:print_suppressions=1
```

`halt_on_error=0` lets a single ctest run discover all findings
rather than aborting on the first. `exitcode=42` is the distinct
exit signal that lets the parent dispatcher detect "tests passed
but ASan flagged something."

## Severity gate

The dispatcher exits non-zero if **any non-suppressed finding**
appears (one whose hash is not in `baseline.json.suppressed_hashes`
AND whose stack frames don't match any substring rule in
`lsan.supp` / `ubsan.supp`). To accept a new finding:

1. Add a `leak:<frame>` rule to `lsan.supp` (or
   `<ub-rule>:<source>` to `ubsan.supp`) with a rationale comment.
2. Document the disposition in `triage.md`.
3. Run `run_sanitizers.ps1 -UpdateBaseline`.

## Why no live baseline is committed

Same Option A pattern as phases 3-6: the live ASan/UBSan run
requires:
1. clang-cl + the matching ASan runtime DLLs (Visual Studio LLVM
   workload).
2. A clean rebuild of the entire project under the asan preset
   (~hours of build time on first run; vcpkg dependencies have
   to be re-resolved per triplet).
3. Running all 2068 tests under instrumentation (each test runs
   ~2-3x slower under ASan).

Producing the canonical baseline.json is a hands-on step:

1. Install Visual Studio "C++ Clang tools for Windows" workload.
2. `pwsh scripts/security/run_sanitizers.ps1 -ConfigureOnly` to
   verify the preset configures.
3. `pwsh scripts/security/run_sanitizers.ps1` for the full run.
4. Triage every finding in triage.md (fix-now / accept).
5. For each `fix-now`, edit `src/`, add a regression test, re-run.
6. `pwsh scripts/security/run_sanitizers.ps1 -UpdateBaseline` to
   record canonical baseline hashes.

Phase 9's regression check then compares all future runs to that
baseline.

## Performance impact (perf-benchmarks gate)

ASan + UBSan typically slow tests by ~2-3x and increase memory
usage by ~3-4x. The phase-7 perf-benchmarks gate runs the
existing perf benchmarks under sanitizers and records the
slowdown ratio. If a benchmark slows by more than ~5x or runs out
of memory, that benchmark gets a `# slow under asan` annotation
in `tests/security/sanitizers/triage.md` rather than failing the
gate.

## Limitations

- **Windows only.** This phase explicitly does not target Linux
  builds. MSan and TSan (which is also Linux/macOS only on
  clang-cl) are deferred. Future Linux work should add a parallel
  preset.
- **Some Qt internals are intentionally suppressed.** Qt's
  signal/slot machinery has a few `function-pointer-of-different-type`
  UB patterns that are guaranteed-correct by the moc invariant
  but flagged by UBSan. See `ubsan.supp`.
- **Build time is significant.** A from-scratch asan build can
  take 30-60 minutes on a typical dev machine; vcpkg's
  transitives don't reuse the regular VS2022 build's binaries.
