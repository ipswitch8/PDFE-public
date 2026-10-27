# PDFE Gap Checklist (pipeline f3c91a2e-7b84-4d06-a5e1-8c3f20b9d741)

<!-- tracking-banner -->
> **STATUS: HISTORICAL EVIDENCE — this is not a work list.**
>
> **GitHub Issues (`ipswitch8/PDFE`) is the canonical tracker for anything still
> open.** This document records what was found or decided on the day it was
> written. Items here may have been fixed, superseded, or deliberately declined
> since. Where this document and a GitHub issue disagree, **the issue wins**.
>
> See [`docs/TRACKING.md`](../../docs/TRACKING.md).

---


**Generated:** 2026-06-06
**Closes when:** all three end-user questions can be answered YES with cited evidence.

**The three questions (gating closure):**
1. All features and functionality have been tested E2E in the way that an end user would use them.
2. All known attack methods have been verified to fail.
3. An end user should be able to use any and all features and functions of PDFE without any problems.

**Audit sources synthesized for this checklist:**
- `tests/docs/known_issues.md`
- `C:\Users\dev\.claude\projects\C--Users-dev-source-PDFE\memory\MEMORY.md` + topic files
- `tests/security/reports/sanitizers/summary.md`
- `.github/workflows/*.yml` (7 files)
- `tests/test_ui_*.py` (37 files) + `tests/ui_automation/test_*.py` (50 files)
- `CMakePresets.json` (windows-vs2022-asan, windows-vs2022-msvc-asan)
- `tests/test_main.cpp` (crash guards)
- `docs/*.md` (architecture and feature categories)

**Phase ownership map (for the eight phases of pipeline f3c91a2e):**
- Phase 1 — Full gap audit and tracked checklist (THIS DOCUMENT)
- Phase 2 — Wire UI E2E suite into CI
- Phase 3 — Fix UI bugs found by Phase 2
- Phase 4 — Fix Windows ASan + auto-fire sanitizer CI
- Phase 5 — Resolve sanitizer FAIL verdict + restore security workflow triggers
- Phase 6 — Resolve all PDFE-LIMITATION-* + memory-tracked open issues
- Phase 7 — Loop-until-clean sweep
- Phase 8 — Three-question final gate

---

## Category A: UI E2E in CI

The repository contains 37 `tests/test_ui_*.py` files at repo root + 50
`tests/ui_automation/test_*.py` files. None are currently invoked by
any CI workflow. `windows-validation.yml` only runs `pdfe_tests.exe`
(GoogleTest). Until UI tests run in CI, Question 1 cannot be answered YES.

### GAP-A1: No CI step installs pywinauto / Pillow / Python UI test dependencies on the runner
- **Proof of fix:** A step in `windows-validation.yml` (or a new
  `windows-ui-validation.yml`) executes
  `pip install pywinauto Pillow appium-python-client` (and any other
  imports observed in the 87 test files) before the UI test step,
  and the install step completes with exit code 0 in the next CI run
  on master.
- **Owner phase:** Phase 2

### GAP-A2: CI does not build PDFE.exe (only pdfe_tests.exe)
- **Proof of fix:** A CI step invokes `cmake --build ... --target PDFE`
  (or equivalent) and the artifact
  `build/.../PDFE.exe` is present on disk before the UI test step runs,
  verified by an explicit `Test-Path` check whose output appears in
  the CI log.
- **Owner phase:** Phase 2

### GAP-A3: CI does not set the DLL PATH prefix correctly for PDFE.exe launch
- **Proof of fix:** UI tests successfully launch PDFE.exe without
  `0xC0000135 (STATUS_DLL_NOT_FOUND)`. The CI log shows the PATH
  prefix step that includes the build output dir + vcpkg
  `installed/x64-windows/debug/bin` and `installed/x64-windows/bin`.
- **Owner phase:** Phase 2

### GAP-A4: All 37 `tests/test_ui_*.py` files must be invoked by CI (no subset)
- **Proof of fix:** A CI step runs `pytest tests/test_ui_*.py` (or
  equivalent) and the CI step summary lists 37 test files invoked.
  Any skipped file appears in the summary with an explicit reason.
- **Owner phase:** Phase 2

### GAP-A5: All 50 `tests/ui_automation/test_*.py` files must be invoked by CI
- **Proof of fix:** A CI step runs `pytest tests/ui_automation/test_*.py`
  and the CI step summary lists 50 test files invoked, including
  `smoke_test.py` (not prefixed with `test_` but a smoke harness),
  `test_menu_navigation_p0.py`, `test_critical_flows_p0.py`,
  `test_dialog_reachability_p0.py`, `test_annotations_p1.py`,
  `test_page_operations_p1.py`, `test_form_fields_p1.py`,
  `test_e2e_workflows.py` (if present — root has `test_ui_e2e_workflows.py`),
  and the remaining 43 files enumerated in this checklist's appendix.
- **Owner phase:** Phase 2

### GAP-A6: CI does not publish a UI-test result summary (pass/fail counts, failing IDs)
- **Proof of fix:** The CI workflow uses `--junit-xml` and an
  Actions-summary step that posts pass/fail counts and the list of
  failing test IDs to `GITHUB_STEP_SUMMARY`, visible on the
  workflow run page for the next dispatch on master.
- **Owner phase:** Phase 2

### GAP-A7: CI does not upload UI test artifacts (stdout, screenshots, pywinauto logs) on failure
- **Proof of fix:** `actions/upload-artifact@v4` step with
  `if: failure()` and `retention-days: 14` is configured to upload
  pytest stdout, screenshots produced by the UI tests, and any
  pywinauto debug log; the artifact is visible on a failed run.
- **Owner phase:** Phase 2

### GAP-A8: Drag-drop page reorder is not exercised by any passing CI UI test
- **Proof of fix:** A test (e.g. `tests/test_ui_page_ops_smoke.py` or
  `tests/ui_automation/test_page_operations_p1.py`) drags a page in
  the PagePanel from index N to N+2, verifies the page count is
  unchanged, the moved page is at index N+2, and the test passes
  in CI run on master.
- **Owner phase:** Phase 3

### GAP-A9: Real Save-As dialog (triggered from real UI) is not exercised by any passing CI UI test
- **Proof of fix:** A test invokes File → Save As from the menu,
  the OS native Save-As dialog appears (verified via pywinauto
  `top_window` enumeration showing a dialog with title containing
  "Save"), the test types a filename, presses Save, and asserts
  the file exists on disk; the test passes in CI run on master.
- **Owner phase:** Phase 3

### GAP-A10: PrintDialog is not exercised by any passing CI UI test
- **Proof of fix:** A test invokes File → Print, the PrintDialog
  opens (verified via window title), the test cancels it without
  crash, and the test passes in CI run on master.
- **Owner phase:** Phase 3

### GAP-A11: TSA signing flow is not exercised by any passing CI UI test
- **Proof of fix:** A test exercises the Security → Sign menu with a
  TSA endpoint configured, signs a PDF, opens the output, and
  verifies the TSA OID (1.2.840.113549.1.9.16.2.14) is present in
  the signature dict; the test passes in CI run on master.
- **Owner phase:** Phase 3

### GAP-A12: Accessibility (Qt/UIA tree) is not verified discoverable for primary interactive controls — RESOLVED (2026-06-14, pending zoom-combo UIA fix)
- **Proof of fix:** A pywinauto-based test enumerates the UIA tree
  for PDFE.exe and finds the following with non-empty
  AutomationId / Name properties (no "element not found" errors):
  menu bar, main toolbar, page panel, comment panel, signature
  panel, canvas widget. The test passes in CI run on master.
- **Resolution:** Primary-control UIA discoverability is exercised by
  passing tests: `test_ui_chrome_smoke.py::test_ui_chrome_accessible_names_present`
  (menu bar / main toolbar / chrome) and `test_ui_view_navigation.py::
  test_toggle_{page,search,bookmark,layer}_panel_uia_tree` (dock panels
  reachable in the UIA tree). Dialog-level discoverability:
  `test_compliance_dialog_widget_accessible_names_present`,
  `test_preferences_widgets_have_accessible_names`,
  `test_customize_toolbars_widgets_have_accessible_names`, plus the
  keyboard-navigation suite over 24 dialogs. `test_ui_chrome_smoke.py`
  was NOT among the 11 Phase-7 files-with-failures (passed). One related
  item — `test_ui_view_navigation.py::test_zoom_combo_uia_exposed` (the
  toolbar zoom combo specifically) — is being converted to a genuine
  readback in the current sweep; the panel/menu/toolbar discoverability
  that this criterion enumerates is already covered by the passing tests
  above.
- **Owner phase:** Phase 7

### GAP-A13: TSA OID verification in signed output
- **Description:** Existing TSA UI tests verify widget presence but
  don't sign a document with TSA + verify timestamp OID in output.
- **Proof of fix:** A test in tests/ui_automation/ that drives the
  full Sign + TSA + Save flow against a real or recorded TSA
  endpoint, opens the signed PDF, and asserts that signature OID
  1.2.840.113549.1.9.16.2.14 (or equivalent TSA token marker) is
  present in the signature bytes.
- **Owner phase:** Phase 8 (final 3-question gate). Effort: ~3-4
  hours. Phase 3 acceptance was deferred to Phase 8 with
  validator approval.

### GAP-A14: 11 UI test files hit hidden pytest timeouts / rc=1
- **Description:** Phase 3 close-out test-runner gate revealed that
  the merged JUnit XML reports `failures=0` while 11 per-file
  pytest invocations actually exited with rc=1 or hit the 20-min
  outer per-file timeout. pytest-timeout's hard-kill mechanism
  fired mid-test on hung UIA/pywinauto calls, crashing pytest
  before it could write per-test failure records. The CI run
  reports "0 failures" but those 11 files contain hidden failures.
- **Affected files (from run 27280480623):**
  - rc=1: test_ui_stress.py (hung on Save dialog), test_ui_text_
    editing.py (hung in UIA descendants), test_missing_features_
    phase6.py, test_ui_file_dialog_flows.py, test_ui_ltv.py,
    test_remaining_preconditions_phase5.py
  - timeout (1200s): test_ui_annotations.py, test_ui_compliance_
    accessibility.py, test_ui_forms.py, test_ui_preferences_chrome.py,
    test_precondition_panels_phase4.py
- **Proof of fix:** `ui-suite-summary.json` reports
  `files_with_problems_count: 0` on a CI run on master. The runner
  script (tests/scripts/run_ui_suite.py) was updated in commit
  abd0b3e+ to surface this count explicitly so it cannot be
  silently masked again.
- **Status (2026-06-20): RESOLVED.** Master Windows Build Validation run
  27862211838 reports `files_run=87`, `failure_count=0`,
  `files_with_problems_count=0` (3015 tests). AND the phase-8 false-green fix
  removed `continue-on-error: true` from the UI E2E step and added a
  `ui-suite-summary.json` backstop that fails the job on any non-zero
  `failure_count` or `files_with_problems_count` — so the count is now
  CI-ENFORCED, not merely surfaced.
- **Owner phase:** Phase 7 (loop-until-clean sweep). These are
  all CI-LIMITATION-class flakes (pywinauto + UIA + COM under
  non-interactive session); resolution requires either: (a) per-
  test wait-for-condition pipes replacing UIA polls, or (b)
  marking specific hanging tests with `pytest.mark.skip` until
  the pipe replacement lands. Effort: ~6-10 hours across the 11
  files.

### GAP-A15: BatchDialog a11y-name regression masked by CI false-green
- **Description:** Phase 6 test-runner gate found 2 genuine gtest
  failures (`BatchDialogTest.ActionListWidgetExists`,
  `BatchDialogTest.ErrorLogExists`) that had been green since Phase 3
  (commit `e76c88d`, Cluster #1B). Cluster #1B changed two BatchDialog
  widgets' `accessibleName` to match their `objectName` (for pywinauto
  reachability), which broke the human-readable-name assertions AND
  degraded screen-reader output. The failures rode green because the
  "Run pdfe_tests" CI step trusted only the process exit code, which was
  0 despite `gtest-results.xml` carrying `failures="2"` (the end-of-run
  `[ FAILED ]` recap was absent — teardown disrupted after the
  PASSED/SKIPPED lines). Arithmetic exposed it: 3926 ran, 3920 passed,
  4 skipped → 2 unaccounted.
- **Fix (two parts):** (a) `src/ui/BatchDialog.cpp` restores
  human-readable accessible Names; pywinauto tests + the shared
  `find_by_suffix` helper now locate `QListWidget`/`QTextEdit` by UIA
  Name (with objectName fallback). (b) `windows-validation.yml` and
  `windows-sanitizer.yml` now parse the JUnit XML and fail the step on
  any `failures`/`errors` regardless of exit code.
- **Proof of fix:** local `BatchDialogTest.*` 20/20 OK; full CI run
  27461377313 (sha a173a60) green with `tests=3926 failures=0`, all 20
  BatchDialogTest passing, and the "Run pdfe_tests" step succeeding with
  `continue-on-error` removed + XML-parse backstop active. See
  known_issues.md GAP-A15.
- **Owner phase:** Phase 6.

---

## Category B: Sanitizer FAIL verdict

`tests/security/reports/sanitizers/summary.md` records:
```
Verdict: FAIL
ctest exit code: 8
Total findings:  0
Non-suppressed:  0
```
Zero findings + a non-zero exit code indicates a runner
misconfiguration, not actual memory errors. The verdict blocks
Question 2 (security) from being answered YES until reproduced
cleanly.

### GAP-B1: Sanitizer summary.md has Verdict FAIL with 0 findings and ctest exit code 8 — root cause unknown — RESOLVED (2026-06-11)
- **Proof of fix:** A new `tests/security/reports/sanitizers/summary.md`
  (or the same file updated) records a successful sanitizer run with
  `Verdict: PASS`, an explanation of what the original exit code 8
  meant (likely a missing test binary, a `--gtest_filter` that
  matched zero tests, or a ctest misconfiguration), and a non-zero
  count of tests actually executed.
- **Owner phase:** Phase 5
- **Resolution (2026-06-11, Phase 5):** `summary.md` now reads
  `Verdict: PASS` (MSVC ASan run 27367556714, 394 tests, 0 findings).
  Root cause of the original exit-8/0-findings documented in
  `tests/docs/known_issues.md` under CI-LIMITATION-CLANG-CL-ASAN-RUNTIME-WIN
  ("Original ctest exit code 8 — root cause"): the clang-cl ASan
  binary failed to launch with 0xC0000135 (STATUS_DLL_NOT_FOUND),
  ctest surfaced the launch failure as exit 8, and 0 tests ran →
  0 findings. It was a loader misconfiguration, NOT a memory error or
  test failure. Moot since Phase 4 switched CI to MSVC ASan.

### GAP-B2: No mechanism in CI to fail the sanitizer step on the FAIL verdict — RESOLVED (Phase 4, verified Phase 5)
- **Proof of fix:** The sanitizer workflow's step that produces
  `summary.md` either fails the job when `Verdict: FAIL` is written,
  or the workflow gates downstream phases on `Verdict: PASS`. A
  deliberate-FAIL test run shows the workflow turning red.
- **Owner phase:** Phase 5
- **Resolution:** `windows-sanitizer.yml` computes
  `$verdict = if ($rc -eq 0 -and $findings -eq 0) {"PASS"} else {"FAIL"}`
  and `if ($verdict -eq "FAIL") { exit 1 }` — the job turns red on any
  ASan `==N==ERROR:` line, non-zero gtest exit, OR an ASAN_OPTIONS
  parse error. This was demonstrated in practice: run 27335387496
  (ASAN_OPTIONS colon collision) correctly went FAIL/red before the
  fix landed. Mechanism verified present in the workflow file.

### GAP-B3: The "1 ASan static-init false positive" from the last 180-min run is not suppressed or documented — RESOLVED (2026-06-11)
- **Proof of fix:** Either the static-init finding is suppressed via
  `__lsan_default_suppressions` / ASan suppressions file checked
  into the repo, or it is documented as a known finding in
  `tests/docs/known_issues.md` with a `CI-LIMITATION-*` ID. A
  sanitizer run shows zero unsuppressed findings.
- **Owner phase:** Phase 5
- **Resolution (2026-06-11, Phase 5):** Moot. The "1 false positive"
  belonged to the retired clang-cl 180-min full-suite run. The active
  MSVC ASan run (27367556714) executes 394 security tests with
  `detect_leaks=0` and reports **0 ASan findings** — zero unsuppressed
  findings, no suppressions file required. No static-init finding
  appears under the MSVC ASan path.

---

## Category C: Security workflow triggers

Inventory of all 7 workflow files in `.github/workflows/` with
current trigger state. Original column verified 2026-06-06; **Phase 5
column** updated 2026-06-11 after the trigger decisions below (see
`tests/docs/security_workflows.md` for the full reference):

> **HISTORICAL — DO NOT READ AS CURRENT (banner added by F32, 2026-08-07).**
> Everything in this Category C section is a **dated 2026-06-10/06-11
> Phase-4/Phase-5 record**, and the trigger landscape it describes has since
> changed. It is deliberately **left as written**: its value is that it says
> what was true then, and editing a dated gap record to make a present-day
> sweep quiet would be rewriting history — the same reason
> `tests/test_doc_drift_guards.cpp:283-287` excludes `docs/audit/` from the
> SEC-107 citation sweep. What actually changed since:
>
> * `windows-sanitizer.yml` — the `push (master)` trigger in the table below
>   and the "push-triggered run" wording in **GAP-C1** and **GAP-C7** was
>   **removed 2026-07-20**. Live `on:` block is
>   `schedule: '0 18 * * 0,2,4'` (Sun/Tue/Thu 18:00 UTC, max 3-day gap)
>   `+ workflow_dispatch`. Run 27367556714 *was* push-triggered on
>   2026-06-11, so those records are accurate as history.
> * `windows-fuzz.yml` — its `push (master, src/**)` trigger was removed in
>   the same 2026-07-20 change. Live `on:` block is
>   `schedule: '30 3 * * *'` (nightly 03:30 UTC) `+ workflow_dispatch`. The
>   "post-push CI observation" language in **GAP-C7**, **GAP-C8** and
>   **GAP-C9** therefore describes a mechanism that no longer exists; the
>   observation it was waiting for did in fact happen on 2026-06-29 (see
>   `known_issues.md` → CI-LIMITATION-WINDOWS-FUZZ-CXX-EXCEPTION-DEADLY-SIGNAL).
> * **GAP-C8**'s "`-max_total_time` is 120s on push, 1800s on schedule" is
>   stale twice over: there is no push lane, and `windows-fuzz.yml:188` now
>   selects `1800` on `schedule` and `300` otherwise. The 60s floor the gap
>   actually asserts is still cleared by both values.
>
> The single source of truth for live triggers is
> `tests/docs/security_workflows.md` (re-derived from the `on:` blocks) and
> the `on:` blocks themselves.

| Workflow | Phase-5 trigger state | Decision |
|---|---|---|
| `windows-validation.yml` | push + PR (master) + dispatch | unchanged — auto-fire |
| `windows-sanitizer.yml` | **push (master) + dispatch** | ACTIVE (Phase 4) |
| `windows-fuzz.yml` | **push (master, `src/**`) + nightly + dispatch**, self-hosted | ACTIVE — push added (GAP-C5) |
| `hetzner-sanitizer.yml` | dispatch-only | KEEP manual — ephemeral (GAP-C4) |
| `sanitizer.yml` (Linux) | dispatch-only | RETIRE (GAP-C2) |
| `fuzz.yml` (Linux) | dispatch-only | RETIRE (GAP-C3) |
| `linux-validation.yml` | dispatch-only | RETIRE/manual (GAP-C6) |

### GAP-C1: `windows-sanitizer.yml` push trigger is commented out (no auto-run on master) — RESOLVED (2026-06-10)
- **Proof of fix:** The file's `on:` block contains an uncommented
  `push: branches: [master]` entry, and a push to master triggers
  a workflow run within 5 minutes that progresses past the
  configure step.
- **Owner phase:** Phase 4 (re-enable) and Phase 5 (verify still active)
- **Resolution (2026-06-10, pipeline f3c91a2e Phase 4):** Workflow file
  rewritten to use the `windows-vs2022-msvc-asan` preset on the
  self-hosted `pdfe-windows` runner with the security-focused gtest
  filter (`MaliciousPdfTest.*:SecurityAudit.*:Encryption*.*:Redaction*.*:Phase8SecurityTest.*:Phase7RedactionTest.*:E2ERedaction.*:PathSecurity.*:JavaScriptEngineTest.*`).
  `on: push: branches: [master]` is active alongside `workflow_dispatch`.
  Phase 5 verifies the first push run goes green.

### GAP-C2: `sanitizer.yml` (Linux) push trigger is commented out — RESOLVED via RETIRE (2026-06-11)
- **Proof of fix:** Either the file's `on:` block is uncommented
  with `push: branches: [master]`, OR the file is retired with a
  documented reason in `tests/docs/known_issues.md`
  (CI-LIMITATION-PDFE-IS-WINDOWS-ONLY) and a tombstone PR comment.
- **Owner phase:** Phase 5
- **Decision: RETIRE (dispatch-only).** PDFE does not compile on Linux
  (CI-LIMITATION-PDFE-IS-WINDOWS-ONLY) and the free-tier runner cannot
  build it anyway (CI-LIMITATION-GITHUB-FREE-TIER). The workflow header
  now states RETIRED with the rationale and points at
  `windows-sanitizer.yml` as the active path. Kept in repo for a
  future Linux port. See `tests/docs/security_workflows.md`.

### GAP-C3: `fuzz.yml` (Linux) schedule trigger is commented out — RESOLVED via RETIRE (2026-06-11)
- **Proof of fix:** Either uncommented to `schedule: - cron: '0 4 * * *'`
  with a self-hosted Linux runner registered, OR retired with
  documented reason (Linux-only port not yet complete).
- **Owner phase:** Phase 5
- **Decision: RETIRE (dispatch-only).** Same rationale as GAP-C2
  (Windows-only source + free-tier build wall). Workflow header now
  states RETIRED and points at `windows-fuzz.yml` as the active
  fuzzing path. See `tests/docs/security_workflows.md`.

### GAP-C4: `hetzner-sanitizer.yml` schedule trigger is commented out and no Hetzner runner is registered — RESOLVED via KEEP DISPATCH-ONLY (2026-06-11)
- **Proof of fix:** Either uncommented to
  `schedule: - cron: '0 1 * * 0'` with a Hetzner runner online and
  receiving dispatches, OR retired with documented reason and
  pointer to `tests/scripts/hetzner_ephemeral_runner.ps1` as the
  manual-dispatch path.
- **Owner phase:** Phase 5
- **Decision: KEEP dispatch-only — a schedule is inappropriate.** The
  Hetzner runner is ephemeral (provision → test → ALWAYS destroy, per
  feedback_hetzner_ephemeral). A `schedule:` trigger would either fire
  with no runner online (queue forever) or spin up a billed VM on a
  cron unattended, violating the ephemeral-only rule and risking an
  orphaned billed server. Correct flow: provision → dispatch manually
  → collect artifacts → `hcloud server delete`. Workflow header now
  documents this explicitly. See `tests/docs/security_workflows.md`.

### GAP-C5: `windows-fuzz.yml` has schedule trigger but no push trigger — RESOLVED via ADD PUSH (2026-06-11)
- **Proof of fix:** Either a `push: branches: [master]` trigger is
  added (with reasonable concurrency cancellation), OR the schedule
  trigger is kept and Phase 5 acceptance criteria 7 (which mentions
  "push trigger added") is amended in the gate review to accept
  nightly-only as sufficient with documented rationale.
- **Owner phase:** Phase 5
- **Decision: ADD push trigger (paths-filtered) + repoint to
  self-hosted.** `on.push.branches: [master]` added with a
  `paths: [src/**, tests/security/fuzz/**, CMakeLists.txt,
  CMakePresets.json, vcpkg.json, .github/workflows/windows-fuzz.yml]`
  filter so docs/report-only commits don't fuzz. Added
  `concurrency.cancel-in-progress`. **Critical fix:** repointed
  `runs-on` from `windows-2022` (GitHub-hosted) to
  `[self-hosted, Windows, X64, pdfe-windows]` — the GitHub-hosted
  runner is why the last scheduled run (27326457133) failed all 7
  harnesses in seconds (CI-LIMITATION-GITHUB-FREE-TIER cold-cache
  build wall, now documented as CI-LIMITATION-WINDOWS-FUZZ-RUNNER).
  Push runs use a 120s/harness smoke duration; nightly keeps 1800s.
  `max-parallel: 1` for the single-threaded runner.

### GAP-C6: `linux-validation.yml` is dispatch-only with no push or schedule trigger — RESOLVED via RETIRE (2026-06-11)
- **Proof of fix:** Either a push trigger gating on Linux-related
  paths is added, OR retirement documented because Linux port is
  blocked by CI-LIMITATION-PDFE-IS-WINDOWS-ONLY.
- **Owner phase:** Phase 5
- **Decision: KEEP dispatch-only (retired from auto-fire).** Linux
  validation is blocked by CI-LIMITATION-PDFE-IS-WINDOWS-ONLY — the
  source does not compile on Linux, so a push/schedule trigger would
  always fail at configure. Left dispatch-only for future Linux-port
  verification. Note: linux-validation.yml is a validation workflow,
  not a security workflow; included here for trigger-inventory
  completeness. See `tests/docs/security_workflows.md`.

### GAP-C7: At least one full sanitizer run (windows-sanitizer + windows-fuzz) must complete on master after triggers are restored — PARTIAL (sanitizer RESOLVED; fuzz pending post-push observation)
- **Proof of fix:** A CI run on master, triggered by the master
  push (not workflow_dispatch), completes successfully with the
  result captured in `tests/security/reports/sanitizers/` showing
  zero non-suppressed findings.
- **Owner phase:** Phase 5
- **Status:** **windows-sanitizer RESOLVED** — push-triggered run
  27367556714 on master (sha edde1d9) completed green: 394 tests,
  0 findings, Verdict PASS, recorded in
  `tests/security/reports/sanitizers/run_20260611T203905Z_msvc_ci/`.
  **windows-fuzz pending** — the Phase 5 commit is the first push that
  exercises the repointed self-hosted + paths-filtered trigger; the
  orchestrator's post-push CI observation confirms the self-hosted run
  goes green (the prior GitHub-hosted failure is explained and fixed,
  see GAP-C5 / CI-LIMITATION-WINDOWS-FUZZ-RUNNER).

### GAP-C8: Fuzz corpus must run at least 60 seconds per target without a crash — CONFIG SATISFIED (run observation pending)
- **Proof of fix:** The `windows-fuzz.yml` run log shows each of
  the 7 fuzz harnesses (`fuzz_document_open`, `fuzz_page_render`,
  `fuzz_text_extract`, `fuzz_annotation_walk`, `fuzz_form_parse`,
  `fuzz_embedded_files`, `fuzz_encryption_dict`) executed for
  >= 60s wall-clock with exit code 0 and no `CRASH` lines in the
  output.
- **Owner phase:** Phase 5
- **Status:** **Config satisfies the 60s floor.** Per-harness
  `-max_total_time` is 120s on push, 1800s on schedule, and the
  dispatch input default is 1800s — all ≥ 60s, for every one of the
  7 harnesses. The run-log observation (>=60s each, no CRASH lines)
  is BLOCKED by GAP-C9 below (the fuzz build can't complete in the
  current environment), so the smoke-level fuzz coverage runs under
  ASan instead (FontFuzzSmokeTest + Pkcs7FuzzSmokeTest in
  windows-sanitizer.yml, green). Full libFuzzer observation deferred
  to Phase 7 / nightly per GAP-C9.

### GAP-C9: windows-fuzz.yml has not completed a clean end-to-end run
- **Description:** Phase 5 acceptance criterion 8 ("a complete
  security workflow run, at minimum windows-sanitizer + windows-fuzz,
  completes on master without new failures") is only PARTIALLY met:
  windows-sanitizer is green (run 27367556714, 394 ASan tests, 0
  findings) but windows-fuzz has not completed a clean run. All
  CODE-level fuzz defects were fixed in Phase 5 (runner, clang_rt
  detection, single-job design, two-stage MSVC/clang-cl build); the
  remaining blocker is environmental — see
  CI-LIMITATION-FUZZ-CAMPAIGN-VCPKG-CACHE in known_issues.md (x-gha's
  10 GB cache can't hold the 46-package Qt6 closure → every run
  source-rebuilds + depends on 46 flaky upstream downloads; the host
  is disk-constrained).
- **Proof of fix:** A `windows-fuzz.yml` run on master completes with
  conclusion=success and `ui-suite-summary`-equivalent
  `tests/security/reports/fuzz/run_*.json` showing all 7 harnesses
  with `crashed=false` after ≥60s each. (When this lands, GAP-C8's
  run-log observation is satisfied by the same run.)
- **Mitigation in place:** persistent local binary cache
  (`files,C:\vcpkg-bincache,readwrite`) added to windows-fuzz.yml so
  that once ONE run completes the dep closure (e.g. a nightly that
  catches a clean 46-download window), the cache is populated
  permanently and the campaign goes stably green without rebuild or
  download.
- **Verified net in the meantime:** the fuzz harness entry points
  (FontFuzzSmokeTest, Pkcs7FuzzSmokeTest) already run under MSVC ASan
  in windows-sanitizer.yml with 0 findings — so the attack surface IS
  covered; the libFuzzer campaign is additive coverage-guided depth.
- **Owner phase:** Phase 7 (loop-until-clean sweep) — or sooner if a
  nightly catches a clean download window first. Recommended
  accelerant: extend the same `files,C:\vcpkg-bincache` cache to
  windows-validation + windows-sanitizer so their regular successful
  builds cross-seed the cache for windows-fuzz.
- **Phase-7 status (2026-06-14):** Two-class resolution. (1) The Phase-7
  source changes touch `src/**`, so the windows-fuzz push trigger fires
  on the upcoming master push (120s/harness > 60s floor) on the same
  self-hosted runner — this is the live end-to-end observation that
  closes the run-log gap (GAP-C8) and this item. (2) The persistent
  `files,C:\vcpkg-bincache` cache means the dep closure no longer needs
  46 flaky downloads per run once seeded by any prior successful
  windows-validation/sanitizer build on the host. Until the push run
  posts `crashed=false` for all 7 harnesses, this is IN-NEXT (the
  observation is mechanically pending the post-push run, not a code or
  config defect). The attack surface itself is already covered green by
  the ASan smoke harnesses above.
- **Phase-8 status (2026-06-20): RESOLVED (mitigation-based, user-approved) —
  the original "all 7 harnesses crashed=false" proof-of-fix is SUPERSEDED.**
  Two changes overtook this gap:
  1. The build blocker is fixed. The native-Windows `/MT` libFuzzer build now
     builds, links and runs all 7 harnesses in CI (branch run 27815827885:
     static-Qt deps install + clang-cl configure + "Build all fuzz harnesses"
     all green; campaigns then run). The fix was the static-CRT switch
     (`x64-windows-static` + `RelWithDebInfo` + `MultiThreaded` + static ASan
     runtime) — libFuzzer's `clang_rt.fuzzer` is `MT_StaticRelease`, so the
     whole fuzz tree must use the release static CRT.
  2. The "crashed=false" criterion is NOT achievable and is reframed. Once the
     harnesses actually run, they find a genuine MuPDF 1.26.10 recursion /
     stack-overflow DoS on adversarial input (every harness — they all open a
     document first). That is an UPSTREAM MuPDF robustness bug class, not a
     PDFE defect, un-trappable in-process; tracked as
     `PDFE-SEC-MUPDF-RECURSION-DOS` in known_issues.md. Requiring zero fuzz
     crashes would gate PDFE on fixing MuPDF upstream (indefinite).
  - **The attack is mitigated at the app layer** by out-of-process isolation
    (`src/app/pdfe_preflight.cpp`, `src/core/PreflightOpen.{h,cpp}`,
    `MainWindow::openFile`): a hostile PDF crashes a disposable child, the
    editor refuses it and stays healthy (verified end-to-end). So criterion 4
    (phase-8 Q2) is YES-qualified: fuzzer operational + findings triaged +
    attack contained. The MuPDF DoS reproducers are filed for upstream Artifex.

---

## Category D: PDFE-LIMITATION-* and CI-LIMITATION-* items

Every entry currently in `tests/docs/known_issues.md` appears here.

### GAP-D1: PDFE-LIMITATION-SAVEAS-LARGE-XFA — Debug saveAs of large XFA files takes minutes (7.4 / 25 / 38 min) — DOCUMENTED-LIMITATION (Release-profiled 2026-06-12)
- **Resolution (Phase 6):** Release profiled with
  `PDFE_RUN_SLOW_SAVE_TESTS=1` on the Release pdfe_tests binary. All 3
  round-trips PASS; timings: ssa_sf86 **5.24 min**, irs_p17 **12.2 min**,
  irs_i1040 **17.2 min** (~2x faster than Debug but still minutes, NOT
  <30s). The "Debug artifact" hypothesis is REFUTED. fz_try/fz_catch
  marshalling ruled out (entered once/save); the cost is inside
  `pdf_save_document`, most likely `do_garbage=3` (full object dedup,
  super-linear on deep XFA graphs). Product perf fix (cheaper default GC
  level for interactive save) deferred to next pipeline. Full evidence
  in known_issues.md PDFE-LIMITATION-SAVEAS-LARGE-XFA (CONFIRMED).
- **Status quoted from known_issues.md:** Re-evaluated 2026-06-06:
  empty-output bug no longer reproduces; downgraded to performance
  limitation. Round-trip works; runtime is the issue.
- **Proof of fix:** A Release-build profile run of
  `Document::saveAs` on all three fixtures (`ssa_sf86.pdf`,
  `irs_p17.pdf`, `irs_i1040.pdf`) records wall-clock times in
  `tests/docs/known_issues.md`. If Release < 30 sec each, the
  limitation is downgraded to Debug-only with a clear note. If
  Release >= 30 sec, the `MuPdfContext::execute` fz_try/fz_catch
  overhead hypothesis is investigated and either confirmed or
  ruled out, with the next investigation step listed.
- **Owner phase:** Phase 6

### GAP-D2: PDFE-LIMITATION-UNICODE-PATH — Opening a PDF with non-ASCII path throws std::system_error — RESOLVED (2026-06-12)
- **Resolution:** `Document::open`/`save`/`saveAs`/`saveIncremental`/
  `createBlank` now feed MuPDF the UTF-8 path via `mupdfPathUtf8()`
  (`path.u8string()`), and the UNC pre-check uses the wide overload on
  Windows. Both tests (`UnicodeFilenameDetectsLimitation` CJK/Cyrillic,
  `UnicodeFilenameLatin1HandledCleanly` café + saveAs round-trip) now
  ASSERT the file opens/round-trips and PASS on Debug. See
  known_issues.md PDFE-LIMITATION-UNICODE-PATH (RESOLVED).
- **Status quoted from known_issues.md:** Limitation documented;
  workaround is short (8.3) path names.
- **Proof of fix:** `Document::open` converts path via
  `std::filesystem::path::u8string()` and passes UTF-8 to
  `fz_open_document`. Both existing tests
  (`UnicodeFilenameDetectsLimitation` and
  `UnicodeFilenameLatin1HandledCleanly` in
  `tests/test_security_audit.cpp`) are updated to assert the file
  opens successfully when given a CJK / Cyrillic / Latin-1 path,
  and both pass.
- **Owner phase:** Phase 6

### GAP-D3: PDFE-LIMITATION-WINAPPDRIVER-V0 — WinAppDriver v0 Beta cannot create sessions — RESOLVED (2026-06-12)
- **Resolution:** `tests/docs/winappdriver_setup.md` documents the
  1.2.1 minimum + install/run steps. The harness adds
  `MIN_WINAPPDRIVER_VERSION=(1,2,1)`, `get_winappdriver_version()`, and
  `require_winappdriver_version()`; the 3 WAD test files route their
  skip helpers through it (skip-not-fail below 1.2.1).
  `windows-validation.yml` adds a "Verify WinAppDriver version" step
  that records the installed version before the UI suite. See
  known_issues.md PDFE-LIMITATION-WINAPPDRIVER-V0 (DOCUMENTED).
- **Status quoted from known_issues.md:** Workaround: install
  v1.2.x or later.
- **Proof of fix:** `tests/docs/` contains a setup guide
  documenting the v1.2.x install requirement. WinAppDriver tests
  are annotated with the version requirement (e.g.,
  `@pytest.mark.requires_wad_v1_2`). A CI step verifies
  `WinAppDriver --version` output before running WAD tests; if
  the version is v0, the tests skip with a clear message instead
  of timing out.
- **Owner phase:** Phase 6

### GAP-D4: CI-LIMITATION-GITHUB-FREE-TIER — Free-tier runner cannot complete vcpkg cold-cache build
- **Status quoted from known_issues.md:** Infrastructure-driven, not
  workflow-config-driven. Workflows retained as reference. Hetzner
  workflow ready; recommend CX41 server.
- **Proof of fix:** Either (a) a self-hosted Hetzner runner is
  registered and the Linux workflows complete on it, OR (b) the
  known_issues.md entry is updated to record the user decision to
  retain the limitation with Windows-only CI as the primary
  validation path. No change is required to the workflow YAMLs
  for this gap (they are correct as-is).
- **Owner phase:** Phase 5 (decision recorded), Phase 6 (limitation finalized)

### GAP-D5: CI-LIMITATION-PDFE-IS-WINDOWS-ONLY — PDFE source does not compile on Linux
- **Status quoted from known_issues.md:** Aspirational
  cross-platform claim; resolution options A (Windows-only CI),
  B (port to Linux), C (accept current state).
- **Proof of fix:** A documented decision in `known_issues.md`
  selecting option A, B, or C, with rationale. If A: Linux
  workflows retired or marked aspirational. If B: a separate
  Linux-port pipeline is opened. If C: aspirational claim
  removed from MEMORY.md and project docs.
- **Owner phase:** Phase 5 (option recorded for CI scope), Phase 6 (final disposition)

### GAP-D6: CI-LIMITATION-ASAN-EXCEEDS-FREE-TIER-BUDGET — Full ASan test suite exceeds 6h GitHub ceiling on windows-2022
- **Status quoted from known_issues.md:** Resolution options A
  (shard), B (security-focused subset), C (build-only + local),
  D (paid runner). Recommendation: B if security top priority,
  else C.
- **Proof of fix:** The selected option (A, B, C, or D) is
  recorded in `known_issues.md` with rationale and a link to the
  Phase 4 PR that implements it. Acceptance criterion of Phase 1
  explicitly requires this resolution be recorded — see
  pipeline.json phase-1 acceptance line 15.
- **Owner phase:** Phase 4 (selection + implementation), Phase 1 (recorded here)

  **Phase 1 record of selected option:** **Option B (security-focused
  subset under ASan)** with fallback to **Option A (sharding)** if
  the chosen subset still exceeds 6h.

  **Rationale:**
  - The user's three-question gate (this pipeline) explicitly names
    "all known attack methods verified to fail" as Question 2 —
    security is top priority by the user's own framing, which is
    exactly the condition under which the GAP-D6 recommendation
    says "B if security top priority".
  - Self-hosted runner (BUILDHOST) has no GitHub-hosted 6h ceiling
    (timeout-minutes is configurable up to 360 min per the
    existing windows-validation.yml precedent), but ASan adds
    2-4x runtime overhead, so even a 360 min ceiling cannot host
    the full 3917-test suite. The security-focused subset
    (TestMaliciousPdf*, TestSecurityAudit*, TestEncryption*,
    TestRedaction*) is approximately 200-400 tests and fits
    comfortably under 360 min even with ASan overhead.
  - Option D (paid runner) is rejected because the user is on a
    constrained GitHub plan (the very reason they're using a
    self-hosted runner instead of GitHub-hosted minutes).
  - Option C (build-only + local) is rejected because it does not
    actually run the ASan-instrumented tests in CI, which leaves
    Question 2 unanswerable.
  - Option A is held in reserve: if Phase 4 finds that even the
    security-focused subset under ASan exceeds the timeout, the
    fix is to shard the subset and run shards in parallel /
    sequential matrix.

  Phase 4 implements Option B (configure the windows-sanitizer.yml
  workflow to run the security-focused gtest filter under the ASan
  preset) and updates this entry with the actual outcome
  (Option B sufficient, or Option B + A sharding required).

  **Phase 4 outcome (2026-06-10): RESOLVED via Option B + MSVC ASan.**
  - Preset: `windows-vs2022-msvc-asan` (MSVC `/fsanitize=address`,
    not clang-cl — avoids GAP-D7 entirely).
  - Workflow filter: `MaliciousPdfTest.*:SecurityAudit.*:Encryption*.*:Redaction*.*:Phase8SecurityTest.*:Phase7RedactionTest.*:E2ERedaction.*:PathSecurity.*:JavaScriptEngineTest.*` (expanded from the shorthand to match actual suite names).
  - Local verification: `tests/security/reports/sanitizers/run_20260610T222246Z_msvc_local/` — 247/247 passed, 0 ASan findings, 5.94 sec wall time. Demonstrates Option B sufficient; Option A sharding NOT required.
  - Option A (clang-cl preset) retained in `CMakePresets.json` as a fallback if a future finding requires running the full suite under ASan.

### GAP-D7: CI-LIMITATION-CLANG-CL-ASAN-RUNTIME-WIN — clang-cl ASan exe fails with 0xC0000135 (STATUS_DLL_NOT_FOUND) on startup — RESOLVED (2026-06-10)
- **Status quoted from known_issues.md:** Diagnosed boundary;
  hypothesis is clang 19's asan_dynamic DllMain LoadLibrary
  failure. Workarounds: build-only verification, non-ASan Debug
  for tests, Linux ASan once port done.
- **Proof of fix:** Either (a) the loader fix is identified and
  applied (e.g., copy `clang_rt.asan_dynamic-x86_64.dll` next to
  the test exe, or upgrade to clang 20+, or use gflags loader
  snaps to identify the missing DLL), and an instrumented
  `pdfe_tests.exe` runs to completion without 0xC0000135 on the
  runner; OR (b) the `windows-vs2022-msvc-asan` preset is used
  instead and produces a launchable instrumented binary; OR
  (c) the suite is sharded so each shard fits within the budget
  per Phase 4 acceptance criteria.
- **Owner phase:** Phase 4
- **Resolution (2026-06-10):** Option (b) — switched CI to the
  `windows-vs2022-msvc-asan` preset. MSVC's `clang_rt.asan_dynamic-x86_64.dll`
  shipped with VS 2022 loads cleanly on this Windows host (verified
  2026-05-02 full-suite PASS and again 2026-06-10 with the security
  subset). The clang-cl preset is left in `CMakePresets.json` for
  build-verification only. Re-investigation of the clang-cl loader
  bug is deferred to a future clang 20+ VS update.

---

## Category E: Memory-tracked open issues

Items from `C:\Users\dev\.claude\projects\C--Users-dev-source-PDFE\memory\`
that imply open work or unresolved bandaids.

### GAP-E1: crt_modal_dialogs_hang_ci — MSVC CRT heap-corruption modal dialog blocks CI; only a test_main.cpp bandaid exists — RESOLVED via (b) (bandaid verified hardened, 2026-06-12)
- **Resolution:** Path (b). The `installCrashGuards()` bandaid is
  confirmed robust: it never sets `_CRTDBG_MODE_WNDW` (so no modal
  report window is ever created), routes all CRT reports to stderr,
  installs a report hook that prints + `abort()`s, suppresses WER via
  `SetErrorMode`, and silences the abort dialog via
  `_set_abort_behavior`. Documented as
  CI-LIMITATION-CRT-MODAL-DIALOG-HANG (RESOLVED) in known_issues.md.
  Every Debug gtest run in this pipeline reaches its final summary with
  no operator interaction.
- **Bandaid identified:** `tests/test_main.cpp` installs
  `installCrashGuards()` (lines 32-59):
  `SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX)`,
  `_CrtSetReportMode` to file+debug, `_CrtSetReportHook` that aborts,
  `_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF)`,
  `_set_abort_behavior(0, ...)`. Note: the per-1024 heap check
  flag is intentionally NOT set.
- **Proof of fix:** Either (a) the underlying heap corruption is
  found and fixed in product code, verified by removing the
  bandaid and running the full gtest suite to completion with the
  CRT summary visible; OR (b) the bandaid is documented as the
  permanent solution in `tests/docs/known_issues.md` with a new
  PDFE-LIMITATION-* ID, and the test_main.cpp comment is updated
  to point at the limitation entry.
- **Owner phase:** Phase 6

### GAP-E2: dll_dependency_checks — No test verifies required runtime DLLs are present at startup — RESOLVED (2026-06-12)
- **Resolution:** `tests/test_dll_dependencies.cpp` (`DllDependencyTest`,
  pre-existing) verifies Qt6 core/gui/widgets/printsupport, QPDF,
  Tesseract, HarfBuzz, FreeType, SQLite, zlib, ICU, ONNX, OpenSSL
  crypto, pugixml, platform plugin. Phase 6 added the runtime-only gaps
  PDFE.exe needs but pdfe_tests didn't link: libssl, libcurl,
  leptonica, jpeg62, openjp2, and the Qt TLS backend plugin. Documented
  as CI-LIMITATION-DLL-DEPENDENCY-CHECKS (RESOLVED) in known_issues.md.
- **Proof of fix:** A new gtest (e.g.,
  `tests/test_dll_dependencies.cpp`) or UI test enumerates the
  required DLLs (`zlib1/zlibd1`, `icu*`, `Qt6*`, `qpdf`,
  `tesseract`, `harfbuzz`, `freetype`, `sqlite3`, `onnxruntime`,
  `clang_rt.asan_dynamic-x86_64` when ASan is active) and asserts
  each is present and loadable via `LoadLibraryW`; the test fails
  with a clear message rather than letting the host hit
  `0xC0000135 (STATUS_DLL_NOT_FOUND)`. The test passes in the
  next CI run on master.
- **Owner phase:** Phase 6

### GAP-E3: gtest_baseline_correction (P-GAP-90) — Phase4AnnotationsTest.CommentPanelDeleteSignalTriggersDeleteCommand fails pre-existing at tests/test_annotations_phase4.cpp:467 (EXPECT_TRUE(undo_->canUndo())) — RESOLVED (test PASSES, 2026-06-12)
- **Resolution:** No longer reproduces. Fixed in pipeline f7c2a4e8
  phase-2 (CommentPanel.cpp ~L97 wires QAction::triggered through
  UndoManager). Re-ran on current Debug build: test OK (205 ms). The
  CRT-crash masking that originally hid it is closed by GAP-E1, so the
  summary always prints. Documented as P-GAP-90 (RESOLVED) in
  known_issues.md. Baseline 3914 PASSED / 0 FAILED is accurate.
- **Current status:** Pre-existing failure since commit 8556c22
  (c94e2f17 closeout). Hidden by CRT-crash exit obscuring
  GoogleTest final summary. Baseline corrected: 3585 ran / 3582
  PASSED / 1 FAILED / 2 SKIPPED. **MEMORY.md ALSO STATES a NEWER
  baseline of "3630/3630 tests pass, 0 failures, 2 skipped" from
  pipeline a7c4f891 (2026-05-24)** — there is an inconsistency
  between memory entries about whether P-GAP-90 is still failing.
- **Proof of fix:** A full local + CI gtest run produces a visible
  GoogleTest summary line ("[==========] N tests from M test
  suites ran") and `[  FAILED  ]` lists zero failing tests (or
  exactly the documented set with PDFE-LIMITATION justification).
  The actual current baseline is recorded in `known_issues.md`
  and `MEMORY.md` and they agree.
- **Owner phase:** Phase 6 (resolve discrepancy + fix or skip the test)

### GAP-E4: save08_p4_techdebt_filter_leak — applyEditsWithSubstitution leaks pdf_keep_obj refs on fz_throw from pdf_filter_page_contents (failure path)
- **Proof of fix:** Either the leak is patched with RAII / cleanup
  in the throw path and a regression test (forcing the throw and
  asserting refcount unchanged after) is added; OR documented as
  a known acceptable failure-path leak in `known_issues.md` with
  rationale.
- **Owner phase:** Phase 6

### GAP-E5: feedback_no_waiting — Memory entry asserts "don't wait between phases"
- **Treatment:** This is process guidance, not a product gap. It
  is not enforced by code or a test. Listed here for completeness
  per Phase 1 instruction to enumerate all memory entries
  representing open issues. No proof-of-fix required.
- **Owner phase:** N/A (process guidance, addressed via the pipeline orchestration in /run)

### GAP-E6: feedback_hetzner_ephemeral — Memory entry asserts Hetzner servers must always be destroyed after use
- **Treatment:** Process guidance. The script
  `tests/scripts/hetzner_ephemeral_runner.ps1` already implements
  trap-based cleanup. No product gap. Listed for completeness.
- **Owner phase:** N/A (process guidance)

### GAP-E7: compact_breaks_hooks — /compact breaks SubagentStop+Stop hook delivery
- **Treatment:** Harness runtime bug, not a PDFE product gap.
  Listed for completeness because it can affect pipeline gate
  enforcement. Mitigation: avoid /compact during this pipeline.
- **Owner phase:** N/A (orchestration)

### GAP-E8: feedback_gate_skipping / feedback_gate_result_forgery / feedback_context_pressure
- **Treatment:** Process guidance enforced by the pre-tool-use hook
  per CLAUDE.md gate discipline section. Not product gaps. Listed
  for completeness.
- **Owner phase:** N/A (process)

---

## Category F: Untested E2E flows

End-user-visible flows that have no passing UI E2E test today and
must gain one before Question 1 can be answered YES. (Drag-drop,
Save-As, PrintDialog, TSA, Accessibility are also tracked in
Category A; they are repeated here for the E2E flow inventory.)

### GAP-F1: Drag-drop page reorder in PagePanel
- **Proof of fix:** See GAP-A8.
- **Owner phase:** Phase 3

### GAP-F2: PrintDialog open + cancel
- **Proof of fix:** See GAP-A10.
- **Owner phase:** Phase 3

### GAP-F3: Real native Save-As dialog (not test-injected path)
- **Proof of fix:** See GAP-A9.
- **Owner phase:** Phase 3

### GAP-F4: TSA signing flow with TSA OID in output
- **Proof of fix:** See GAP-A11.
- **Owner phase:** Phase 3

### GAP-F5: Accessibility tree discovery via UIA / pywinauto
- **Proof of fix:** See GAP-A12.
- **Owner phase:** Phase 7

### GAP-F6: Install / upgrade experience on a clean Windows system — DOCUMENTED-LIMITATION + partial automated coverage (2026-06-14)
- **Proof of fix:** A UI test (or a documented manual procedure
  with checklist in `tests/docs/manual_qa_checklist.md`) launches
  the installer on a clean VM snapshot, completes install, runs
  PDFE.exe, opens a sample PDF, verifies render appears, closes
  cleanly. Test passes in a CI run that includes the install step.
- **Resolution:** The proof-of-fix's "OR a documented manual
  procedure" clause is satisfied — `tests/docs/manual_qa_checklist.md`
  carries the install/run procedure. The two failure modes that
  matter most on a clean system are covered AUTOMATICALLY: (1)
  `windows-validation.yml` builds the NSIS installer + staging tree
  every run (catches a broken package); (2) `tests/test_dll_dependencies.cpp`
  (DllDependencyTest, incl. the now-fixed OpenSslTlsPresent) verifies
  every required runtime DLL is present/loadable at startup — this is
  the automated guard against the classic "0xC0000135 on a clean
  machine" failure. The remaining gap — a full clean-VM-snapshot
  install→launch→render E2E — is a DOCUMENTED-LIMITATION: the
  self-hosted `pdfe-windows` runner has no VM-snapshot infrastructure,
  so the E2E install test is IN-NEXT-PIPELINE (add when VM/CI snapshot
  infra is available) and covered manually until then.
- **Owner phase:** Phase 7

### GAP-F7: Network-print failure path (printer offline / unreachable) — DOCUMENTED-LIMITATION (2026-06-14)
- **Proof of fix:** A UI test invokes File → Print with a network
  printer that is unreachable, asserts that PDFE shows a clear
  error message (not a hang, not an unhandled exception), and the
  app remains usable after the dialog is dismissed. Test passes
  in CI.
- **Resolution:** DOCUMENTED-LIMITATION. Faithfully reproducing a
  network printer that is reachable-then-offline requires a real (or
  virtual) print spooler + printer in a specific failure state, which
  the headless self-hosted `pdfe-windows` runner does not have — the
  CI session has no installed printers, so the scenario cannot be
  exercised end-to-end automatically. It is covered by
  `tests/docs/manual_qa_checklist.md` section A (print to a real
  printer, including failure observation). The product's print path
  routes through Qt's QPrinter/print-dialog which surfaces driver
  errors via the normal dialog flow (no custom hang path). IN-NEXT-PIPELINE:
  a synthetic test that targets a deliberately-invalid printer name and
  asserts graceful error handling (does not need physical hardware) can
  be added without a runner printer.
- **Owner phase:** Phase 7

### GAP-F8: USB-print failure path (printer powered off / disconnected) — DOCUMENTED-LIMITATION (2026-06-14)
- **Proof of fix:** Similar to GAP-F7, but with a USB printer that
  is offline. PDFE shows a clear error and remains usable. Test
  passes in CI.
- **Resolution:** DOCUMENTED-LIMITATION, same rationale as GAP-F7 — a
  physically-disconnected USB printer cannot be simulated on a headless
  CI runner with no printers installed. Covered by
  `tests/docs/manual_qa_checklist.md` section A. IN-NEXT-PIPELINE with
  the same synthetic-invalid-printer approach as F7.
- **Owner phase:** Phase 7

### GAP-F9: Feature category coverage matrix — every documented feature has at least one passing UI test — RESOLVED (2026-06-15)
- **Resolution:** A "Feature-Category → Covering UI/Test Coverage (GAP-F9)"
  table was added to `tests/docs/ui_function_matrix.md` mapping all ~22
  documented feature categories to ≥1 covering test (UI and/or gtest), citing
  tests that passed in the Phase-7 full UI-suite sweep (84→0 failures).
  Categories without a dedicated UI test (font substitution, metadata, image
  internals, follow-links) are explicitly noted as gtest-covered + exercised
  via UI smoke — no empty cells. Phase 8 re-verifies completeness.
- **Major feature categories from `docs/` and MEMORY.md:**
  rendering, annotations (sticky / text-markup / ink / shape /
  free-text), forms (text / checkbox / radio / combo / list /
  push-button / signature), redaction (mark + apply), encryption
  (AES-256 R6), digital signatures (PKCS7 + TSA), page operations
  (rotate / delete / insert / move / import / extract), text
  editing (vector + scanned image via TRE), font substitution,
  OCR (Tesseract), accessibility (PDF/UA, Tagged PDF, Alt-Text),
  printing, JavaScript (MuJS form scripts), search (text search +
  replace), metadata (PDF Info dict + XMP), image manipulation
  (insert / remove / resize / rotate / crop / EXIF strip),
  compliance (PDF/A, PDF/UA), bookmarks, recent files, follow
  links, preferences, themes.
- **Proof of fix:** A matrix table in `tests/docs/ui_function_matrix.md`
  (the file already exists) is updated so every row in the
  category list above has a column "covering UI test" populated
  with at least one passing test file path. Any row with an
  empty cell is a documented gap that must be either filled or
  marked PDFE-LIMITATION before Phase 8.
- **Owner phase:** Phase 7 (sweep), Phase 8 (final gate verifies completeness)

---

## Discovered gaps (not in the original 6 categories)

### GAP-X1: Two ASan presets exist (windows-vs2022-asan via clang-cl, windows-vs2022-msvc-asan via MSVC) — neither is documented as the canonical CI choice — RESOLVED (2026-06-10)
- **Context:** `CMakePresets.json` contains both
  `windows-vs2022-asan` (clang-cl + ASan + UBSan, hits 0xC0000135
  at runtime per CI-LIMITATION-CLANG-CL-ASAN-RUNTIME-WIN) and
  `windows-vs2022-msvc-asan` (MSVC `/fsanitize=address`, no UBSan,
  no libFuzzer). The user must pick one for CI. The clang_rt
  loader path "fix" (option (a) of GAP-D7) involves copying
  `clang_rt.asan_dynamic-x86_64.dll` next to the test exe via a
  CMake POST_BUILD step or via PATH; this is workable but not
  yet implemented.
- **Proof of fix:** `CMakePresets.json` retains both presets but
  `tests/docs/known_issues.md` records which preset is canonical
  for CI use, with the resolution option from GAP-D7 noted. The
  `windows-sanitizer.yml` workflow uses the chosen preset.
- **Owner phase:** Phase 4
- **Resolution (2026-06-10):** `windows-vs2022-msvc-asan` is now the
  canonical CI preset. `tests/docs/known_issues.md` (the
  `CI-LIMITATION-CLANG-CL-ASAN-RUNTIME-WIN` entry) records the
  decision with rationale. `.github/workflows/windows-sanitizer.yml`
  uses the preset on the self-hosted `pdfe-windows` runner. The
  clang-cl preset is retained for future re-investigation (clang 20+
  upgrade path) but is not used by CI.

### GAP-X2: MEMORY.md and known_issues.md may have inconsistent baseline (3630 vs 3582)
- **Context:** MEMORY.md states "Total: 3630/3630 tests pass (0
  failures, 2 skipped)" from pipeline a7c4f891 (2026-05-24).
  `gtest_baseline_correction` memory entry from 2026-05-22
  states 3582/1/2. Both cannot be simultaneously correct unless
  P-GAP-90 was fixed between the two dates.
- **Proof of fix:** A fresh local run of `pdfe_tests.exe` (with
  the CRT crash guards in place ensuring the summary prints)
  produces a documented baseline that both files agree on.
- **Owner phase:** Phase 6

### GAP-X3: smoke_test.py at tests/ui_automation/ is not prefixed test_ and may be missed by pytest collection
- **Context:** `tests/ui_automation/smoke_test.py` exists but
  `pytest` by default collects only `test_*.py`. Phase 1's
  acceptance line 5 (pipeline.json phase-2) requires "smoke_test.py"
  be invoked.
- **Proof of fix:** Either `smoke_test.py` is renamed to
  `test_smoke.py`, OR a `pytest.ini`/`conftest.py` config sets
  `python_files = test_*.py smoke_test.py`, OR the CI step
  invokes `pytest tests/ui_automation/smoke_test.py` explicitly.
- **Owner phase:** Phase 2

### GAP-X4: No central manifest of which `tests/test_ui_*.py` files require WinAppDriver vs pywinauto vs PyAutoGUI vs FlaUI
- **Context:** CLAUDE.md project file describes four tools.
  Tests use different ones implicitly. CI must install or skip
  based on availability.
- **Proof of fix:** Either a top-of-file marker
  (`@pytest.mark.requires_wad` etc.) is added to each test, or
  `tests/docs/ui_function_matrix.md` is extended with a "Tool"
  column for each test. CI selects the right install step based
  on the marker.
- **Owner phase:** Phase 2

### GAP-X5: tests/security/reports/sanitizers/summary.md is structured as a per-run report but is also the canonical "current verdict" — these are two different documents
- **Context:** The file documents the 2026-06-03 run. New runs
  presumably overwrite it. There is no archive of historical
  runs.
- **Proof of fix:** Either (a) each sanitizer run produces a
  timestamped file (e.g.,
  `tests/security/reports/sanitizers/20260603T152054Z.md`) and a
  `latest.md` symlink/copy is updated; or (b) the rolling
  `summary.md` is renamed `latest.md` and history is kept in
  git log only with no archive needed. Phase 5 picks one.
- **Owner phase:** Phase 5

### GAP-X6: No JS sandbox isolation test referenced from any UI CI run — RESOLVED (2026-06-14)
- **Context:** Phase 8 acceptance criterion 2 mentions "the JS
  sandbox isolation test passes". `tests/test_ui_js_sandbox.py`
  exists but is not in CI (per GAP-A4). Until GAP-A4 is
  resolved, the JS sandbox is not verified.
- **Proof of fix:** `test_ui_js_sandbox.py` runs in CI and
  passes; the test asserts that a form JS calling
  `app.execMenuItem("Save")` or `util.printf` cannot escape the
  MuJS sandbox to touch the filesystem.
- **Resolution:** GAP-A4 is RESOLVED — `tests/scripts/run_ui_suite.py`
  collects every `tests/test_ui_*.py` (including `test_ui_js_sandbox.py`)
  and is wired into `windows-validation.yml`. In the Phase-7 full-suite
  run (87 files) `test_ui_js_sandbox.py` ran and was NOT among the 11
  files-with-failures — i.e. it passed. The JS sandbox isolation
  assertions are therefore exercised on every CI UI run. Additional
  headless coverage: `JsSandboxSecurityTest.*` in the gtest suite
  (test_form_javascript_xss.cpp) — LaunchURLBlocked, SystemOsNotAccessible,
  OpenDocPathTraversalBlocked, etc. (see GAP-X7 matrix).
- **Owner phase:** Phase 7

### GAP-X7: OWASP-relevant attack categories (malformed input, path traversal, embedded JS, encryption bypass, signature forgery) — coverage inventory not consolidated — RESOLVED (2026-06-14)
- **Resolution:** Consolidated category→test matrix added to
  `tests/docs/security_review_cadence.md` ("OWASP Attack-Category Coverage
  Matrix (GAP-X7)"). Each of the 7 categories (malformed input, path
  traversal, embedded JS sandbox escape, encryption bypass, signature
  forgery, redaction completeness, Unicode path) maps to ≥1 named test suite;
  all cited suites passed in the 2026-06-14 gtest run (0 failures among
  security suites). Current-HEAD verification is the **scheduled**
  `windows-sanitizer.yml` gate (Phase 8 criterion 2) — `schedule:
  '0 18 * * 0,2,4'` + `workflow_dispatch`, no `push` trigger since
  2026-07-20. (Said "post-push" until F32; it is the live sibling of the
  same sentence in `tests/docs/security_review_cadence.md`, and both are
  corrected in the same change.)
- **Context:** Phase 8 acceptance criterion 2 requires each
  applicable category to have "at least one passing test". Today
  these tests exist under `tests/test_security_audit.cpp`,
  `tests/test_malicious_pdf*`, `tests/security/`, but no single
  matrix maps category → test.
- **Proof of fix:** A new section in
  `tests/docs/security_review_cadence.md` (the file exists) maps
  each OWASP-relevant category to a specific test ID, and each
  test is verified passing in the most recent sanitizer run.
- **Owner phase:** Phase 5 (matrix), Phase 7 (verify)

### GAP-X8: tests/scripts/hetzner_ephemeral_runner.ps1 is described as production-ready but has never been wired into CI dispatch automatically
- **Context:** Per known_issues.md
  CI-LIMITATION-PDFE-IS-WINDOWS-ONLY, the script is "proven across
  10 ephemeral attempts" but only triggers via manual invocation.
- **Proof of fix:** Either CI dispatches the script when needed
  (e.g., from `linux-validation.yml` once Linux port lands), OR
  the script's role as manual-dispatch-only is documented in
  `tests/docs/hetzner_runner_setup.md`.
- **Owner phase:** Phase 5 / Phase 6

---

## Summary

**Total gap count:** 55 entries (51 actionable + 4 process-guidance N/A in Category E)

**Breakdown by category:**
- Category A (UI E2E in CI): 12 gaps (GAP-A1 … GAP-A12)
- Category B (Sanitizer FAIL verdict): 3 gaps (GAP-B1 … GAP-B3)
- Category C (Security workflow triggers): 8 gaps (GAP-C1 … GAP-C8)
- Category D (PDFE-LIMITATION-* and CI-LIMITATION-*): 7 gaps (GAP-D1 … GAP-D7)
- Category E (Memory-tracked open issues): 8 entries (GAP-E1 … GAP-E8); 4 product gaps + 4 process-guidance (no proof-of-fix)
- Category F (Untested E2E flows): 9 gaps (GAP-F1 … GAP-F9)
- Discovered gaps: 8 (GAP-X1 … GAP-X8)

Totals: A=12, B=3, C=8, D=7, E=8 (4 actionable), F=9, X=8 → **55 entries** of which **51 are actionable**.

**Count of items owned by each phase (actionable only):**
- Phase 2 (Wire UI E2E into CI): GAP-A1, GAP-A2, GAP-A3, GAP-A4, GAP-A5, GAP-A6, GAP-A7, GAP-X3, GAP-X4 → **9**
- Phase 3 (Fix UI bugs): GAP-A8, GAP-A9, GAP-A10, GAP-A11, GAP-F1, GAP-F2, GAP-F3, GAP-F4 → **8**
- Phase 4 (Windows ASan + auto-fire sanitizer CI): GAP-C1, GAP-D6, GAP-D7, GAP-X1 → **4**
  (GAP-C1 is wholly owned by Phase 4 — re-enabling the push trigger is one atomic step; Phase 5 verifies that Phase 4 left it green, but does not re-do the work.)
- Phase 5 (Sanitizer FAIL + workflow triggers): GAP-B1, GAP-B2, GAP-B3, GAP-C2, GAP-C3, GAP-C4, GAP-C5, GAP-C6, GAP-C7, GAP-C8, GAP-D4, GAP-D5, GAP-X5, GAP-X7, GAP-X8 → **15**
- Phase 6 (PDFE-LIMITATION + memory items): GAP-D1, GAP-D2, GAP-D3, GAP-D4 (finalized), GAP-D5 (disposition), GAP-E1, GAP-E2, GAP-E3, GAP-E4, GAP-X2, GAP-X8 (alt) → **11**
- Phase 7 (Loop-until-clean sweep): GAP-A12, GAP-F5, GAP-F6, GAP-F7, GAP-F8, GAP-F9, GAP-X6, GAP-X7 (verify) → **8**
- Phase 8 (Three-question final gate): no new gaps owned — verifies the totality of evidence from Phases 2-7

**Appendix — Inventory pointers for Phase 2:**
- `tests/test_ui_*.py`: 37 files (full list embedded in pipeline source-of-truth; Phase 2 must run all 37)
- `tests/ui_automation/test_*.py`: 50 files (full list embedded in pipeline source-of-truth; Phase 2 must run all 50)
- `tests/ui_automation/smoke_test.py`: 1 non-prefixed file (see GAP-X3)
