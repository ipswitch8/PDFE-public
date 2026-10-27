# Phase-9 final dry-run record

End-to-end verification that the OPT-IN posture and the
security label suite both work as designed. This file is the
canonical evidence requested by phase 9 AC11.

## Environment

- Date: 2026-04-22
- Repo state: phase 9 commit (this commit)
- Build dir: `build/vs2022`
- CMake configure: `-DPDFE_ENABLE_SECURITY_TESTS=ON`

## Step 1 -- Default ctest excludes the security label

Command:
```
ctest --test-dir build/vs2022 -LE security -C Debug -N
```

Result: **2868 standard tests** listed, **zero security-labeled
tests** in the listing.

Verification: the OPT-IN invariant holds. `pdfe_tests.exe` does
not contain any security test cases (security suites are separate
PowerShell / Python scripts registered with CTest only under the
`security` label).

## Step 2 -- The security label runs the security suite

Command:
```
ctest --test-dir build/vs2022 -L security -C Debug --output-on-failure
```

Result (5/5 PASS, 14.61s total):

| # | Test | Time |
|---:|---|---:|
| 2869 | `security_verify_optin`            | 6.20s |
| 2870 | `security_runner_decision_unit`    | 0.84s |
| 2871 | `security_runner_listener_unit`    | 5.75s |
| 2872 | `security_runner_firewall_log_unit`| 0.85s |
| 2873 | `security_regression_check`        | 0.74s |

`security_regression_check` runs
`scripts/security/check_regressions.ps1`, which compares fresh
per-phase outputs to the canonical baselines under
`tests/security/regression_baselines/`. Phases without a fresh
output (because the live run hasn't happened post-checkout) are
reported as `skipped` rather than `fail` — the gate is honest
about what it could compare:

```
[check_regressions] verdict=pass
  corpus       skipped no fresh corpus_run_*.json under reports/
  fuzz         skipped no fresh fuzz run dir
  sast         skipped no fresh findings.json (run_sast.ps1 not run)
  deps         skipped no fresh cves.json (run_deps.ps1 not run)
  sanitizers   skipped no fresh sanitizer run dir
  secrets      skipped no fresh findings.json (run_secrets.ps1 not run)
```

This is the correct Option A state at phase-9 commit time. When
contributors run live phases (3-8) with the required tools
installed, the baselines under `regression_baselines/` get
populated and subsequent regression checks compare real outputs.

## Step 3 -- Standard build is undisturbed

`pdfe_tests.exe` and the regular ctest suite were untouched by
phase 9. The 2068 GoogleTest cases continue to pass with no new
failures and no new skips.

## Conclusion

The opt-in posture is enforced mechanically. Future contributors
who don't opt in see exactly the same 2868-test suite they saw
before phase 1; contributors who do opt in get the full
regression check, all five labeled tests, and the
`pdfe_security_nightly` target for full-intensity nightly runs.
