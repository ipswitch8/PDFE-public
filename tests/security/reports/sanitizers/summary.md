# Sanitizer run summary (most recent CI run)

**Verdict:** PASS
**Run dir:** `run_20260611T203905Z_msvc_ci/`
**Trigger:** push to master — auto-fire `windows-sanitizer.yml`
**Workflow run:** https://github.com/ipswitch8/PDFE/actions/runs/27367556714
**Commit:** edde1d9855da329ed508a65bb3506a4bf7e514b4
**Date:** 2026-06-11

> NOTE: The CI workflow writes a timestamped `run_<stamp>_msvc_ci/`
> directory on the runner and uploads it as an artifact, but does NOT
> git-commit it back (no auto-push from CI). This top-level summary is
> updated by hand from the authoritative CI artifact after each gate
> review. The 394-test figure below is from the CI run, not the
> earlier 247-test local pre-verification.

## Results (CI run 27367556714)

| Metric | Value |
|---|---:|
| Tests run | 394 |
| Tests passed | 394 |
| Tests failed | 0 |
| Test suites | 41 |
| gtest exit code | 0 |
| ASan findings (==N==ERROR: lines) | **0** |
| Non-suppressed findings | 0 |
| Wall time | 26.3 sec |
| Preset | windows-vs2022-msvc-asan (MSVC /fsanitize=address) |
| Runner | self-hosted pdfe-windows |

## Filter (security-focused subset under ASan)

```
MaliciousPdfTest.*:SecurityAudit.*:Encryption*.*:Redaction*.*:Phase8SecurityTest.*:Phase7RedactionTest.*:E2ERedaction.*:PathSecurity.*:JavaScriptEngineTest.*:JsSandboxSecurityTest.*:SignatureSecurityTest.*:Pkcs7FuzzSmokeTest.*:FontFuzzSmokeTest.*:Pkcs7BridgeTest.*:Pkcs7RoundTripTest.*:CertEnforcementTest.*:CertificationSignatureTest.*:InvisibleSignatureTest.*:SignatureVerifyResultTest.*:SignatureManagerTest.*:PermissionFlagsTest.*
```

The filter was expanded during pipeline f3c91a2e Phase 4 after the
security-audit gate flagged that the original 9-suite filter (298
tests) missed the signature-tamper and font/PKCS7 parser attack
surfaces — exactly where heap overflows are historically found in
PDF software. The expanded 21-pattern filter runs 394 tests across
41 suites, covering: malformed-PDF parsing, signature tamper /
byte-range, PKCS#7 + font parser fuzz harnesses, certification
signatures, JS sandbox isolation, and permission-bit enforcement.

## ASAN_OPTIONS

```
halt_on_error=0:print_stacktrace=1:detect_leaks=0:abort_on_error=0:log_path=asan_ci_finding
```

- `halt_on_error=0` / `abort_on_error=0` — let every finding print
- `detect_leaks=0` — Qt/MuPDF leak noise is tracked separately
- `print_stacktrace=1` — make any finding actionable
- `log_path=asan_ci_finding` — COLON-FREE relative base (a Windows
  drive-letter colon in this value collides with the `:` option
  separator and aborts ASan; see resolution history). ASan writes
  `asan_ci_finding.<pid>` to the process CWD; the CI step scans both
  stdout.log AND these files so a finding cannot be missed if the
  PowerShell `*>&1` stream merge drops the stderr line.
- `exitcode` deliberately NOT set: a real ASan finding still fails
  the run.

## Resolution history

- **CI-LIMITATION-CLANG-CL-ASAN-RUNTIME-WIN** (0xC0000135 loader
  failure): RESOLVED via Option B — use the MSVC `/fsanitize=address`
  preset (`windows-vs2022-msvc-asan`) instead of the clang-cl preset
  whose ASan runtime DLL failed to load on this host. See
  `tests/docs/known_issues.md`.
- **ASAN_OPTIONS colon collision**: a `log_path=C:\...` value broke
  ASAN_OPTIONS parsing (drive-letter `:` vs option separator `:`),
  aborting ASan at startup with "expected '=' in ASAN_OPTIONS" → 0
  tests, exit 1 (run 27335387496 FAIL). Caught by the sanitizer gate
  itself and fixed with a colon-free relative log_path (run
  27367556714 PASS). An explicit "in ASAN_OPTIONS" stdout check now
  fails the job loudly on any future malformed-options regression.

## Auto-fire

`.github/workflows/windows-sanitizer.yml` runs on
`schedule: '0 18 * * 0,2,4'` (Sun/Tue/Thu 18:00 UTC, max 3-day gap)
plus `workflow_dispatch`, on the self-hosted pdfe-windows runner. The
job fails red on any ASan `==N==ERROR:` line OR non-zero gtest exit OR
an `ASAN_OPTIONS` parse error.

> **F32 (2026-08-07).** This section said "runs on every push to
> master" — true when the run below was recorded, false since the
> `push` trigger was removed 2026-07-20 (cadence then tightened from
> weekly to Sun/Tue/Thu by F30). It was corrected by hand rather than
> left to regenerate: the workflow rewrites this file *on the runner*
> and uploads it as an artifact, but does **not** git-commit it back
> (see the NOTE at the top), so the committed copy only ever changes
> when a human updates it. "Self-correcting on the next real run" —
> as the deferred-backlog entry assumed — does not hold for the
> in-tree copy. The **Trigger:** line in the run header above is
> untouched: that run genuinely was push-triggered on 2026-06-11.

## What this subset does NOT cover (Phase 5/7 backlog)

- Full ~3900-test gtest suite — runs un-instrumented in
  `windows-validation.yml`.
- UBSan, libFuzzer — clang-cl-only on Windows. libFuzzer campaigns
  run separately via `windows-fuzz.yml` (Phase 5).
- Linux ASan — tracked under `CI-LIMITATION-PDFE-IS-WINDOWS-ONLY`.

## Predecessor CI runs

- `run_20260611T083423Z_msvc_ci/` (sha 477a3e3) — PASS, 298 tests /
  29 suites, 0 findings. First auto-fire run; original 9-suite filter.
- This run (sha edde1d9) supersedes it with the expanded 21-pattern
  filter (394 tests / 41 suites).
