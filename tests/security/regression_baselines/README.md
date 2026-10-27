# PDFE security regression baselines

Single directory holding the canonical baseline snapshot from
phases 2-8. `scripts/security/check_regressions.ps1` reads files
from here and compares them to fresh outputs under
`tests/security/reports/<phase>/`.

## File map

| Baseline file | Source phase | Source-of-truth file |
|---|---|---|
| `corpus_run_baseline.json`  | Phase 3 (sandboxed runner) | A clean corpus run from `tests/security/runner/run_corpus.py` |
| `fuzz_baseline.json`        | Phase 4 (fuzz harnesses)   | `tests/security/reports/fuzz/baseline.json` |
| `sast_baseline.json`        | Phase 5 (SAST sweep)       | `tests/security/sast/baseline.json` |
| `deps_baseline.json`        | Phase 6 (dependency CVEs)  | `tests/security/deps/baseline.json` |
| `sanitizers_baseline.json`  | Phase 7 (ASan/UBSan)       | `tests/security/sanitizers/baseline.json` |
| `secrets_baseline.json`     | Phase 8 (secrets scan)     | `tests/security/secrets/baseline.json` |

## Updating a baseline

A baseline update is a security-relevant change. The procedure:

1. Run the per-phase dispatcher with `-UpdateBaseline` (or copy the
   per-phase baseline by hand, for `corpus_run_baseline.json` which
   is produced from a live runner output).
2. Copy the per-phase baseline into this directory, overwriting the
   matching file.
3. Document the change in `tests/security/CHANGELOG.md` with a
   one-line entry per modified baseline.
4. Commit with a `security-baseline:` prefix in the commit message
   so the security-audit gate hook can flag it during review.

## Why a separate directory

Each phase's per-phase baseline (e.g. `tests/security/sast/baseline.json`)
is the working copy used by the per-phase dispatcher to compare
against during local triage. This `regression_baselines/` directory
is the **canonical snapshot** that the durable phase-9 regression
check reads from. Keeping them separate means a developer can
experiment with `run_sast.ps1 -UpdateBaseline` against the per-
phase baseline locally without the regression check picking up
the in-progress edit.

## Status (as of phase-9 commit)

All entries are placeholders pending live runs (Option A). The
regression check correctly reports `skipped` for phases whose
baseline is unpopulated, rather than fabricating a pass.
