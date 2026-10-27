# Sanitizer fixtures (SEC-21)

Both files are **verbatim output of a real instrumented run**, not hand-written
samples of what AddressSanitizer "would" print. That distinction is the point:
the defects they pin are about where ASan puts its report, and a sample written
from documentation would have reproduced the documentation's mistake rather
than the runtime's behaviour.

## Provenance

A deliberate heap-use-after-free was compiled with the same toolchain the CI
lane uses:

```
cl /nologo /EHsc /Zi /fsanitize=address uaf.cpp /Fe:uaf.exe
```

```cpp
int* p = new int[8];
p[0] = 1;
delete[] p;
volatile int v = p[1];      // the injected fault
```

and run under the exact options from `.github/workflows/windows-sanitizer.yml`:

```
ASAN_OPTIONS=halt_on_error=0:print_stacktrace=1:detect_leaks=0:abort_on_error=0:log_path=asan_ci_finding
```

| File | What it is |
|---|---|
| `asan_real_heap_use_after_free.report.txt` | The `asan_ci_finding.<pid>` file the runtime wrote. Contains the `==<pid>==ERROR: AddressSanitizer: heap-use-after-free` header and stack. |
| `asan_real_stdout_with_log_path.txt` | The process's merged stdout/stderr for the same run. It contains the program's own `SEC21: allocating` line and **no ASan finding line at all**. |

## Why the second file matters

The workflow described `log_path` as making ASan "ALSO" write findings to files,
and scanned stdout as a second, independent source. It is not a second source:
setting `log_path` moves the report off stderr instead of duplicating it. The
stdout capture of a genuine heap-use-after-free is empty of `==ERROR:` lines,
so the older of the two detectors had been dead since that hardening landed.

The run also showed the surviving detector was coupled to the process CWD —
`log_path` must be relative and colon-free (a drive-letter colon is an
`ASAN_OPTIONS` separator), so the runtime resolves it against the CWD. Scanning
only `$GITHUB_WORKSPACE` worked because the step's CWD happened to equal it.

Driven by `tests/test_sanitizer_gate_fault_injection.cpp` against
`tests/scripts/asan_gate_verdict.ps1`.

## Related, and deliberately absent

There is no leak fixture here. The same probe under `detect_leaks=1` returns

```
==17512==AddressSanitizer: detect_leaks is not supported on this platform.
```

and exits before running any user code — LeakSanitizer is not part of the MSVC
ASan runtime. Leak evidence for A1-95 / R2-05 / R2-06 comes from in-suite
live-allocation oracles instead: `tests/test_leak_soak_r205_r206.cpp`.
