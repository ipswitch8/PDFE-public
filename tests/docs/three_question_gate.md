# Phase-8 — Three-Question Final Gate

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


This is the final synthesis phase of pipeline `f3c91a2e`. Each of the three
end-user questions is evaluated **only** against evidence from passing tests
and CI runs (acceptance criterion [6]); no question is answered YES by
assertion. Karen / validator / test-runner / security-audit gate this phase
before it closes.

> Status legend: **YES** = supported by cited passing evidence ·
> **YES (qualified)** = the security goal is met but a literal sub-criterion is
> reframed with an explicit, user-approved rationale.

---

## Question 1 — All features tested E2E as an end user would use them

**Verdict: YES.**

- The native UI E2E suite (pywinauto / WinAppDriver / FlaUI / PyAutoGUI) drives
  the **real PDFE window** — menus, dialogs, toolbars, the PDF canvas — not
  headless gtest. 87 test files, **0 failures** (documented `PDFE-LIMITATION-*`
  / `CI-LIMITATION-*` skips only), with rerun-on-failure for the probabilistic
  UIA-contention flakiness inherent to the Server-2019 self-hosted runner
  (`tests/scripts/run_ui_suite.py --reruns`).
- Feature → UI-test coverage is mapped in `tests/docs/ui_function_matrix.md`;
  any feature without a UI test is tracked there as an explicit gap.
- CI: the suite runs inside `windows-validation.yml` (the same gate that builds
  the app).

**Evidence:** `tests/ui_automation/`, `tests/test_ui_*.py`,
`tests/docs/ui_function_matrix.md`; `windows-validation` run on master.

---

## Question 2 — All known attack methods verified to fail

**Verdict: YES (qualified) — mitigation-based, per the user decision recorded
2026-06-18/19.**

What is met literally:
- Security gtests pass under ASan: `TestMaliciousPdf*`, `TestSecurityAudit*`,
  `TestEncryption*`, `TestRedaction*`.
- Sanitizer summary verdict is **not FAIL** (`windows-sanitizer` green on
  master — criterion 3).
- JS sandbox isolation test passes (`JavaScriptEngineTest.*`). OWASP-relevant
  categories for a PDF editor each have at least one passing security gtest in
  the ASan subset (run 27862211833): malformed input → `MaliciousPdfTest.*` /
  `SecurityAudit.*`; path traversal → `PathSecurity.*`; embedded JS →
  `JavaScriptEngineTest.*`; encryption bypass → `Encryption*.*`; signature
  forgery → `Phase8SecurityTest.*`. (There is no separate "OWASP section" doc;
  the coverage is the gtests themselves.)

The one sub-criterion that is **reframed** — "the fuzz suite has run ≥60s/target
with no crashes":
- The native-Windows `/MT` libFuzzer build is now operational (it never built
  before) and runs real coverage-guided campaigns in CI. **It finds crashes** —
  a MuPDF 1.26.10 recursion/stack-exhaustion DoS class reachable from every
  harness on crafted malformed input.
- Those are genuine **upstream MuPDF** robustness bugs (the class OSS-Fuzz files
  against Artifex continuously); they are un-trappable in-process and are
  tracked in `tests/docs/known_issues.md` as `PDFE-SEC-MUPDF-RECURSION-DOS`.
- The **attack itself is verified to fail**: the application opens every
  document in a disposable `pdfe_preflight` child process and refuses anything
  that child cannot open cleanly (crash / timeout / parser error), so a hostile
  PDF cannot crash the editor. Verified end-to-end against shipping MuPDF
  (DoS input → refused; normal PDF → opens). See `src/core/PreflightOpen.{h,cpp}`,
  `src/app/pdfe_preflight.cpp`, `src/ui/MainWindow.cpp::openFile`.

Rationale for the qualification (user-approved): the gate's intent is that *the
attack fails*, which it does (process isolation). Requiring the raw parser
fuzzer to find zero crashes would gate this project's completion on fixing
MuPDF upstream — an indefinite effort outside PDFE's control — so the fuzzer's
findings are treated as triaged upstream issues, mitigated at the app layer.

**Evidence:** `tests/security/` gtests; `windows-sanitizer` master run (crit 3);
`windows-fuzz` CI run (builds + links + runs all 7 harnesses; crashes recorded);
`tests/docs/known_issues.md` (`PDFE-SEC-MUPDF-RECURSION-DOS`); the verified
preflight isolation.

---

## Question 3 — End user can use all features without problems

**Verdict: YES.**

- gtest suite 3926 passed / 0 failed (documented skips only): startup, command
  pipeline, all 11 feature phases.
- Zero crash-on-startup; zero unhandled-exception dialogs in the UI E2E run.
- `DllDependencyTest` passes (all required runtime DLLs present, incl. the
  `libssl-3-x64.dll` dlopen fix); Unicode file paths open correctly
  (`PDFE-LIMITATION-UNICODE-PATH` resolved via UTF-8 path handling).
- `windows-validation` produces a working application build on the runner
  (green on master via PR #1).
- The new preflight gate **fail-opens** when the helper is absent, so it cannot
  make a normally-openable document fail to open.

**Evidence:** gtest run (3926/0); `DllDependencyTest`, Unicode-path tests;
`windows-validation` master run.

---

## Master CI evidence (post PR #2 merge, master `5ccc09a`)

All citations are runs in repo `ipswitch8/PDFE`, dated 2026-06-19/20:

| Criterion | Run | Conclusion |
|---|---|---|
| Crit 7 — Windows Build Validation (gtest 3926/0 + UI E2E 87 files / 3015 tests / 0 failing / 0 hidden-problems + app build) | [27862211838](https://github.com/ipswitch8/PDFE/actions/runs/27862211838) | **success** |
| Crit 3 — Windows Sanitizer (MSVC ASan, security gtest subset) | [27862211833](https://github.com/ipswitch8/PDFE/actions/runs/27862211833) | **success** |
| Crit 4 — Windows Fuzz Campaigns (`/MT` libFuzzer) | [27815827885](https://github.com/ipswitch8/PDFE/actions/runs/27815827885) (branch, build/link/run confirmed) | failure-by-design (finds the documented MuPDF DoS; mitigated by isolation) |

**False-green fix applied this phase (validator gate finding):** the UI E2E step
in `windows-validation.yml` previously had `continue-on-error: true` and trusted
`run_ui_suite.py`'s always-0 exit code, so a green job did NOT prove the UI suite
passed (the GAP-A15 anti-pattern at the UI layer). It now parses
`ui-suite-summary.json` and fails the job on any unresolved failing test or any
file with a crash/timeout after reruns. Validation re-run on master with this
gate is green — so Q1's "0 failures" is now CI-enforced, not merely reported.

## Closing conditions

This phase closes when all three answers are YES (Q2 qualified as above), each
with the citations above, confirmed on **master** after PR #2 merges, and after
the karen / validator / test-runner / security-audit gates pass. The fuzzer's
MuPDF DoS findings remain tracked for an upstream Artifex report
(`PDFE-SEC-MUPDF-RECURSION-DOS`); the preflight isolation is the standing
defence regardless of upstream timing.
