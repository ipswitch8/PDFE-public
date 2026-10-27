# PDFE Test-Harness Adversarial Audit

<!-- tracking-banner -->
> **STATUS: HISTORICAL EVIDENCE — this is not a work list.**
>
> **GitHub Issues (`ipswitch8/PDFE`) is the canonical tracker for anything still
> open.** This document records what was found or decided on the day it was
> written. Items here may have been fixed, superseded, or deliberately declined
> since. Where this document and a GitHub issue disagree, **the issue wins**.
>
> See [`docs/TRACKING.md`](../TRACKING.md).

---


**Complete, 2026-07-28.** A whole-harness adversarial audit of the PDFE test suite,
executed as a 6-phase gated pipeline. Stance: the harness is the **defendant** — every
green test is presumed worthless until it proves it asserts a user-visible truth AND
fails when that truth breaks. Motivation: repeated end-user-obvious defects kept escaping
(state-continuity/mirror, half-size text, redaction confidentiality, crash-on-close),
and prior reviews were themselves happy-path — they fixed the one escaped instance and
never measured oracle strength or whether tests catch injected bugs.

**FIND-AND-DOCUMENT only.** No product or test code was changed by this audit. The
phase-3 mutations were applied then reverted; `git` confirms zero `src/` diffs across
every commit. Remediation is the separate gated program specified in
[`remediation-plan.json`](remediation-plan.json).

## Deliverables

| Phase | Question | Output |
|---|---|---|
| 0 Census | What exists? | [`census.{json,md}`](census.md) — 402 C++ files / 4365 tests / 13,937 asserts / 200 fixtures |
| 1 Oracle audit | Do tests assert the truth? | [`phase-1/oracle-audit-synthesis.md`](phase-1/oracle-audit-synthesis.md) — every file scored; **147/402 files (36.6%) ride proxy/no-oracle-smoke**; 181 ranked weak findings |
| 2 RTM + input-domain | What's covered, with what inputs? | [`phase-2/`](phase-2/) — 87 capabilities (**only 1 with a strong visual oracle**); 200 fixtures classified; 38 hostile-input classes (**4 zero-coverage** incl. sheared-CTM) |
| 3 Mutation testing | Does green actually catch bugs? | [`phase-3/mutation-report.md`](phase-3/mutation-report.md) — real 16-mutant harness; **6 killed / 10 survived (37.5%)**, save/rewrite **0%**; **16/16 matched the phase-1 prediction** |
| 4 UI + governance | Do the UI tests & the process protect anything? | [`phase-4/`](phase-4/) — **472 silent-zero-protection UI skips**; matrix over-claim; self-seeding visual goldens; standing quality gates hollow; CI dispatch-only |
| 5 Synthesis | So what, and what next? | [`flaws-register.md`](flaws-register.md) · [`dca.md`](dca.md) · [`standing-controls.md`](standing-controls.md) · [`remediation-plan.json`](remediation-plan.json) |

## The verdict in one line

The suite is large and mostly **runs** the product — but a third of it asserts something
merely *correlated* with correctness, and the weakness concentrates exactly on the
**visual** and **security-enforcement** features a user or attacker would notice. This was
not asserted — it was **empirically proven** in phase 3: every predicted-weak oracle let
its injected bug through (10/10), every predicted-strong oracle killed its control (6/6).

## Flaws & Gaps Register

37 severity-ranked entries (2 Critical, 25 High, 8 Med, 2 Low). Headliners:

- **R-01 (Critical)** — CVE/malware neutralization asserted only by no-throw around a
  catch-all; no oracle proves the attack was neutralized.
- **R-02 (Critical)** — redaction confidentiality leak is invisible: the verifier is
  tautological and skip-gated; phase-3 mutant **M11 forced it to always report "clean"
  and no test objected**.
- **R-03 (High)** — `extraction-only-for-visual` oracle class (71 files): the dominant
  disease; text-extraction round-trips are structurally blind to mirror/invisible/white/
  wrong-glyph/half-size — the exact class that shipped the mirror & half-size defects.
- **R-08 (High)** — save/rewrite routing at **0% mutation kill-rate**; reopen+pageCount is
  the only oracle, blind to routing/fidelity regressions.
- **R-24 (High)** — the standing CI mutation gate is **green-by-construction** (3 curated
  already-fixed mutants) while the real 16-mutant run found 10 survivors.

## Defect Causal Analysis

All 8 escaped-defect seeds (plan §5) trace to Register entries that now cover them (seed 2,
crash-on-close, is honestly marked **PARTIAL** — only the save-throw sub-case exists today).
See [`dca.md`](dca.md).

## Standing controls (not one-off patches)

8 controls in [`standing-controls.md`](standing-controls.md): adversarial-ambient fixture
corpus × every injector with pixel/quad oracles; an oracle standard (visual feature ⇒ visual
oracle; ban SUCCEED-after-evidence / lone-EXPECT_NO_THROW / extraction-only-for-visual); a
**real** mutation-kill-rate CI gate with an enforced floor; an enforced coverage floor; a
mandatory escaped-defect→DCA→class-test loop; live RTM enforcement (matrix pass = ran AND
asserted); de-skip-walled UI visual oracles with non-self-seeding goldens; and re-enabled
per-commit CI. Every Register entry is closed by a control and/or a remediation phase.

## Process note

The audit held itself to its own standard: **every phase gate (karen/validator, plus
test-runner on the evidence phases) caught a real defect in the audit's own deliverables**
— a ⅓ smell-undercount from exact-string matching (P1), a permission-gap over-claim and an
RTM total that didn't match its array (P2), and — most fittingly — a *scope-narrowing
behind a matching total* in the capstone Register itself (P5), where two real findings had
been dropped while the count still read "7 = 7." Each was fixed and re-gated before the
phase was allowed to advance.
