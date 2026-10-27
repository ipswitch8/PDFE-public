# Sanitizer finding triage

This document records the disposition for every sanitizer finding
in `tests/security/sanitizers/baseline.json`. Each entry must be
one of:

| Disposition | Meaning |
|---|---|
| `fix-now` | Real bug. Fix in `src/` this phase, with at least one regression test (AC9). Hash drops out of findings on subsequent runs. |
| `accept`  | Known false positive (e.g., Qt static globals, MuPDF font cache, OpenSSL init). Suppress via `lsan.supp` / `ubsan.supp` substring rule and add the hash to baseline. |

## Process

1. Run `pwsh scripts/security/run_sanitizers.ps1` (no flags).
2. Inspect `tests/security/reports/sanitizers/run_<stamp>/findings.json`.
3. For every finding not currently in baseline, decide disposition.
4. For `fix-now`: edit `src/`, add a regression test under `tests/`,
   re-run the script. Finding should disappear.
5. For `accept`: add a `leak:<frame>` substring rule to `lsan.supp`
   (or `<ub-rule>:<source>` to `ubsan.supp`), and run
   `run_sanitizers.ps1 -UpdateBaseline` to record the hash.
6. Commit suppressions + baseline + triage entry together.

## Per-entry format

```
### <kind> in <test_name>

**Disposition:** fix-now | accept

**Hash:** <sha1 from findings.json>

**Top frames:** (5 deepest)

**Rationale:** (required for accept)

**Fix commit:** (required for fix-now -- short SHA + message)
```

## Triaged findings

_None yet -- baseline is empty pending first real ASan/UBSan run
under Option A. Once an authorized contributor builds the
windows-vs2022-asan preset and runs run_sanitizers.ps1, populate
this section per the format above._
