# Phase-5 Report — Make the HOLLOW STANDING CI GATES REAL

The phase-4 audit found the standing quality gates were decorative: the mutation
gate was green-by-construction (3 curated pre-fixed mutants), the coverage floor
was unenforced (`continue-on-error`), the DCA loop was dispatch-only + manual
base ref, GAP-A15 had no per-commit trigger, and *all* CI was schedule/dispatch
since 2026-07-20 so nothing blocked a bad commit at merge. This phase converts
each into a gate that **actually fails the build when its condition is violated**,
with a local proof (command + non-zero exit) for every one.

**No product source changed** — `git status --porcelain -- src/` is empty. No test
was weakened, deleted, or skipped. All work is under `.github/workflows/`,
`tools/gate/`, and `tests/scripts/run_gtest.py` (a harness runner, not a test).
All 34 `tools/gate/test_*.py` unit tests pass; 24 matrix/pass-semantics tests pass.

---

## R-24 — Mutation-kill gate (stub → real 16-mutant floor)

**Was decorative:** `tools/gate/mutants.json` held **3 curated mutants**, all
predicted-and-known KILLED, so the floor was green-by-construction — it could
never fail.

**Now enforces:** `tools/gate/mutants.json` carries the **real 16 mutants** from
`docs/harness-audit/phase-3/mutants.json`, converted into the gate's execution
schema (phase-3 `test_filter` → gate `killing_test`, `target: pdfe_tests` added).
Before remediation phases 1-4, **10 of these 16 SURVIVED** (false-green oracles);
after phases 1-4 the strengthened oracles KILL all 16 (evidence:
`phase-{1,2,3}-mutation-reverify.json`). The floor (`--floor 1.0`, 100% kill)
returns exit 1 on ANY surviving mutant; `mutation-kill.yml` turns that into a
failed job. The workflow runs the 16 build+test cycles.

**Local proof:**
- Real catalog intact — all 16 find-strings present exactly once:
  `python tools/gate/mutation_kill.py --manifest tools/gate/mutants.json --dry-run` → **exit 0**, "16 mutant(s), all find-strings present exactly once."
- Floor logic fails on a survivor (1 killed + 1 planted survivor):
  `test_floor_gate_fails_when_survivor_below_floor` → `main(--floor 1.0)` returns **1** (kill-rate 0.500 < 1.000, "SURVIVORS: m_survivor"); at `--floor 0.5` returns **0**.

Post-remediation the 16 kill: M13→`Phase5FormsTest.ResetFormRestoresCheckboxDefaultOnState`, M15→`GlyphScaleAdjusterTest.HorizontalScaleIsOriginalOverSubstituteNotInverted`, M04-M07 by the phase-2 glyph-quad/colour oracles, M08/M09/M11/M16 by the phase-1 save/redaction oracles, controls M01/M02/M03/M10/M12/M14 stay dead.

## R-32 — Coverage floor (unenforced → enforced, tool-absence hard error)

**Was decorative:** the OpenCppCoverage→Cobertura step in `windows-validation.yml`
was `continue-on-error: true`; the report was uploaded but never gated, and a
runner without OpenCppCoverage silently `return`ed. A coverage collapse shipped green.

**Now enforces:** new `tools/gate/check_coverage_floor.py` parses the Cobertura
line-rate and exits 1 below `--floor` (default 0.50, ratchet up after first
measured run), **exit 2 (hard error) on a missing/unparseable report**. The
workflow step drops `continue-on-error`, makes OpenCppCoverage absence a **hard
failure** (attempt choco install, else fail), and gates on the floor.

**Local proof:**
- Below floor: `check_coverage_floor.py cov_low.xml --floor 0.60` (line-rate 0.31) → **exit 1** ("FAIL line-rate=31.00% < floor=60.00%").
- Tool/report absent: `check_coverage_floor.py nope.xml` → **exit 2** ("HARD ERROR: coverage report not found").
- Passing: line-rate 0.71 → **exit 0**.

## R-26 / R-31 — DCA loop (decorative / honor-system → per-commit gate)

**Was decorative:** `check_dca.py` / `check_dca_presence.py` existed and worked,
but `governance.yml` was `workflow_dispatch`-only and the presence check needed a
**manually supplied** base ref (`dca_base`) — pure honor system; a fix commit with
no DCA could merge with nobody running it.

**Now enforces:** the new `fast-checks.yml` lane runs both on **every push/PR** with
an **automatic base ref** (`pull_request.base.sha` / push `event.before`, skipping
only a zero/absent base). A defect-fix range touching `src/` or a test oracle that
adds no DCA record fails the required gate.

**Local proof** (scratch git repo): a `fix: null deref regression` commit touching
`src/Foo.cpp` with no DCA record → `check_dca_presence.py --base <base> --head HEAD`
**exit 1** ("looks like a defect fix … but adds no DCA record"); after adding
`docs/audit/dca/0002-foo.md` → **exit 0**. Conformance: real repo dir → **exit 0**;
bad-record cases covered by `test_check_dca.py` (7 tests).

## R-28 — Live RTM / matrix integrity (rebuildable-from-green → guarded)

**Was the risk:** a UI-matrix "pass" could be rebuilt from committed green — a stale
committed-green row leaking into the runtime CSV, or a SKIP clobbering a real result.

**Now enforced (pre-existing guard, now CI-wired + documented):** the runtime
transform (`conftest._write_matrix_updates`) records a committed-green row that did
**not** run this session as `no-run`, never inherited `green`; a higher-authority
`green` cannot be clobbered by a later `skip`/`blocked`. `check_matrix_health.py
--require-ran` fails the build when a committed-green row degrades to
red/skip/blocked/planned or (full-sweep) was not exercised. These are unit-tested
by `test_runtime_pass_semantics.py` (incl. `test_committed_green_not_run_this_session_becomes_no_run` — the exact "rebuilt from committed green" defect) and `test_aggregation_precedence.py`. Both run in `windows-validation.yml`'s matrix-health step AND the new `fast-checks.yml` lane. **Local proof:** the 24 matrix/pass-semantics tests pass; the pass-semantics suite asserts stale green → `no-run`.

## R-25 / R-30 — CI triggers restored + GAP-A15 wired

**Was decorative:** every workflow was schedule/dispatch-only since 2026-07-20 —
nothing blocked a bad commit at merge. GAP-A15 (`check_junit_xml.py`) had no
per-commit trigger.

**Now enforces:** new `fast-checks.yml` runs on **push + pull_request to master** on
a GitHub-hosted `ubuntu-latest` runner (pure-Python, no C++ toolchain, **no
self-hosted contention** — which is exactly why per-commit was removed). It fails
merge on any standing-gate violation. **GAP-A15 (R-30):** a committed **red** JUnit
sentinel (`tools/gate/fixtures/sentinel_red.xml`) MUST be rejected by
`check_junit_xml.py`; if it ever passes, the lane fails.

**Local proof:**
- `check_junit_xml.py fixtures/sentinel_red.xml` → **exit 1** ("FAIL … failures=1"); the fast lane inverts this so a passing red sentinel fails the build.
- `check_junit_xml.py fixtures/sentinel_green.xml` → **exit 0** (checker is not always-red). Covered by `test_check_junit_xml.py` (4 tests).

## R-36 — macOS thin subset (dispatch-only claim → recurring real subset)

**Was decorative:** PDFE claims macOS support but `macos-validation.yml` was
dispatch-only, so the claim went unverified for weeks.

**Now honest:** added a **weekly schedule** (`cron: 0 6 * * 1`) so the **real** core
subset already present — `--gtest_filter='PreflightOpen.*:PreflightXPlatform.*'`
(the macOS-only `_NSGetExecutablePath`/`fork`/`waitpid` fail-closed paths) — runs
on a GitHub-hosted arm64 runner (separate minute pool, no self-hosted contention).
If the macOS port breaks it surfaces within a week. This runs a real subset rather
than dropping the claim.

## R-37 — Flaky-quarantine ledger (none → tracked, reruns can't mask)

**Was decorative:** `run_gtest.py`/`run_ui_suite.py` re-run a failed test in a fresh
process and go green if it heals — with **no ledger**, silently masking a degrading
test forever.

**Now enforces:** `run_gtest.py` now writes `healed_flakes.json` (tests that failed
then passed on rerun). New `tools/gate/flaky_quarantine.json` is the machine-readable
registry (known-flaky + xfail, each with reason/owner/expiry). New
`tools/gate/check_flaky_quarantine.py` **fails** when a healed test is **not
registered**, or when a registry entry has **expired** (a quarantine can't live
forever). Wired into `windows-validation.yml` (after the gtest run) and `fast-checks.yml`
(registry validity).

**Local proof:**
- Untracked healed flake: `check_flaky_quarantine.py --healed healed_bad.json` (healed=`SomeSuite.UntrackedFlake`) → **exit 1** ("NOT in the flaky quarantine registry").
- Tracked (`MainWindowIntegrationTest.ZoomComboExists`) → **exit 0**. Expired entry → **exit 1** (covered by `test_check_flaky_quarantine.py`, 5 tests).

---

## Verification summary

| Gate | Violating input | Exit | Passing input | Exit |
|---|---|---|---|---|
| Mutation floor (R-24) | 1 planted survivor, floor 1.0 | **1** | rate ≥ floor | 0 |
| Mutation catalog (R-24) | — | — | 16 find-strings present | 0 (dry-run) |
| Coverage floor (R-32) | line-rate 0.31 < 0.60 | **1** | 0.71 ≥ 0.60 | 0 |
| Coverage report absent (R-32) | missing/corrupt XML | **2** | present | 0 |
| DCA presence (R-26/R-31) | fix touches src, no DCA | **1** | DCA added | 0 |
| GAP-A15 (R-30) | red JUnit sentinel | **1** | green sentinel | 0 |
| Flaky-quarantine (R-37) | untracked healed flake | **1** | registered | 0 |
| Matrix semantics (R-28) | committed-green not-run | `no-run` (not green) | ran+asserted | green |

**Files:** new `tools/gate/{check_coverage_floor,check_flaky_quarantine}.py`,
`flaky_quarantine.json`, `fixtures/sentinel_{red,green}.xml`, three new
`test_check_*.py`; rewritten `tools/gate/mutants.json` (16 real mutants); new
`.github/workflows/fast-checks.yml`; edited `windows-validation.yml` (coverage
floor + flaky gate), `macos-validation.yml` (weekly schedule), `tests/scripts/run_gtest.py` (healed ledger). `tools/gate/` files are force-added past the repo's broad `tools/` gitignore, matching their tracked siblings.

**Follow-up (documented, not a gap):** `COVERAGE_FLOOR` starts at 0.50 — ratchet up
after the first measured nightly. The full 16-mutant build run (`mutation-kill.yml`)
and full macOS gtest remain schedule/dispatch (self-hosted-runner budget), by design.
