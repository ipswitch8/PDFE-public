# Adversarial Audit of the PDFE Test Harness — Plan

**Date:** 2026-07-19
**Status:** PLAN (not yet executed)
**Author:** engineering, at owner's request after repeated escaped end-user-visible defects

---

## 1. Why this, and why past reviews failed

PDFE has a large test suite (~4,126 GoogleTests, Qt Test, pywinauto/WinAppDriver
UI automation, a "UI test matrix," python robustness scripts, gate agents, CI).
Yet serious, **immediately end-user-obvious** defects keep escaping — and each
one, in hindsight, sat in a blind spot the existing tests structurally could not
see. Recent examples:

- **State-continuity / "backwards text"** (2026-07): editing text on a `-100 Tz`
  page rendered it mirrored; color was lost. Six injectors shared the class
  (`docs/content-stream-state-continuity-audit.md`).
- **Crash-on-close, small-text-edit no-op, wrong icons/forms** (pipeline 3e91a7c4):
  six user-reported bugs a single manual walkthrough surfaced.
- **GAP-A15**: `pdfe_tests.exe` exits `0` while failing — CI trusted the exit code.
- **Gate fabrication** incidents (Phase-5; remediation 5+6): "green" that wasn't.
- Recurring **UI-matrix over-claim**: rows marked passing that weren't runnable.

**The pattern behind the pattern:** every prior review was itself happy-path. It
took the one escaped defect, added a test for *that instance*, and declared the
process improved — without ever asking the harness-level questions: *Do our
oracles actually assert the user-visible truth? Do our tests catch injected bugs?
What is the input domain and how little of it do we exercise? What does "done"
actually verify?* Incremental, anecdote-driven fixes cannot catch the *next*
class, because they never audit the harness as a whole. This plan does.

**Governing stance — adversarial, not confirmatory.** The harness is the
defendant, not the witness. The default assumption for every test is: *this test
is worthless until it proves otherwise* (it asserts something a real user would
notice, and it fails when that thing breaks). We actively try to demonstrate that
green means nothing — by mutating product code and watching tests stay green, by
feeding hostile inputs, by tracing requirements to their (missing) tests.

---

## 2. Methodology & frameworks (established, not invented here)

This audit is grounded in standard QA/QC test-assessment practice so the critique
is defensible and repeatable, not ad hoc:

- **The Test Oracle Problem** (Weyuker): the central lens. Most escaped defects
  here are *oracle* failures — the test ran the right code but asserted the wrong
  (or no) thing. We classify every test's oracle strength.
- **Oracle taxonomy**: true / pseudo (independent implementation) / metamorphic /
  heuristic / statistical / *no-oracle (smoke)*. We flag heavy reliance on weak
  oracles (text-extraction, exit code, "did not throw").
- **Mutation testing** (kill-rate as the empirical measure of test effectiveness).
  This is the objective proof a green suite is real. PDFE already has a
  `mutation-test-specialist` agent.
- **ISO/IEC/IEEE 29119** test-design techniques and **BS 7925-2** for coverage:
  equivalence partitioning, boundary-value analysis, decision tables, state
  transition — used to expose input-domain gaps.
- **Negative / adversarial / error-guessing / fault-injection** testing: the
  missing discipline behind the "benign fixtures only" gap.
- **Requirements Traceability Matrix (RTM)** + **specification coverage**: every
  user-facing capability → the test(s) that verify it; orphan tests and uncovered
  requirements both surfaced.
- **Test-smell catalog** (Meszaros *xUnit Test Patterns*; van Deursen et al.):
  assertion-free, tautological, mystery-guest, eager, conditional-logic-in-test,
  over-tolerant-threshold, permanently-skipped, flaky-quarantined.
- **Testing Pyramid/Trophy** distribution analysis: where coverage is thin
  (visual/E2E) vs redundant (unit).
- **Escaped-Defect / Defect Causal Analysis (DCA)** and **Defect Detection
  Percentage (DDP)**: root-cause each historical escape to the harness property
  that should have caught it — the seed for systemic fixes.
- **FMEA** applied to the *product* to derive what *should* be tested (failure
  modes → required oracles), independent of what *is* tested.
- **TMMi / TPI Next** process-maturity framing for the governance findings.

---

## 3. Phased plan

Each phase is adversarial (tries to prove the harness fails), produces a concrete
artifact, and feeds the **Flaws & Gaps Register** (§4). Phases 1–6 are largely
parallelizable across subsystems; Phase 7 synthesizes.

### Phase 0 — Census & instrumentation
**Goal:** an accurate map of the entire harness (you cannot audit what you cannot
enumerate).
**Do:**
- Inventory every test asset: GoogleTest files, Qt Test, `tests/*.py`,
  `tests/ui_automation/*`, the UI-test-matrix CSVs, fixtures (`tests/fixtures/`,
  `flaw_corpus/`), gate agents, CI workflows.
- Metrics per layer & per product subsystem: test count, assertion count,
  fixture count, LOC, last-run status, skip count.
- Wire up the measurement tooling the later phases need: coverage build
  (llvm-cov / OpenCppCoverage), a mutation-testing harness for C++, a JUnit-XML
  parser (GAP-A15-safe), a fixture classifier.
**Deliverable:** `harness-census.md` + machine-readable inventory (JSON).

### Phase 1 — Oracle audit (highest-yield)
**Goal:** find every test that asserts nothing, asserts a tautology, or asserts a
proxy blind to the user-visible truth. This is where the state-continuity class
died (text-extraction oracle blind to mirror/invisible/white-on-white).
**Adversarial method:**
- Classify each test's oracle by the taxonomy above; flag `no-oracle`, `heuristic`,
  and `proxy` oracles on *visual/behavioural* features.
- Grep + read for smells: `EXPECT_*` absent; `ASSERT_TRUE(true)`;
  `EXPECT_NO_THROW` as the *only* assertion on a feature; over-tolerant thresholds;
  oracles that read back only the edited element and never a neighbor / following
  content; exit-code trust.
- Specifically flag: visual features verified only by `extractPlainText`; UI
  features verified only by "widget exists"; save features verified only by
  "reopens without error."
**Deliverable:** oracle-strength scorecard per subsystem; ranked list of
weak-oracle tests with the user-visible failure each would miss.

### Phase 2 — Requirements traceability & specification coverage
**Goal:** find user-facing capability with **no** test or **happy-path-only** test,
and tests that trace to no real requirement.
**Adversarial method:**
- Build/repair an RTM from the feature set (the public README/comparison-chart
  capabilities, the UI function matrix, menu/toolbar actions) → tests.
- Two-way gap analysis: uncovered requirements (feature with no meaningful test)
  and orphan tests (test with no requirement / tests an artifact of the impl).
- For each covered requirement, mark happy-path-only vs has-negative/boundary.
**Deliverable:** RTM with coverage class per requirement; the "uncovered &
user-visible" shortlist ranked by user impact.

### Phase 3 — Input-domain & adversarial-fixture audit
**Goal:** measure how little of the hostile input space the fixtures represent —
the BVA/negative-testing gap behind the state-continuity class.
**Adversarial method:**
- Catalog every fixture; classify benign vs adversarial; tag which product path
  consumes it.
- Enumerate the *should-exist* hostile-input space via EP/BVA + FMEA:
  ambient graphics state (`-100 Tz`, `Tr 3`, top-level `cm`, non-RGB colorspace,
  dangling `q`, non-zero `Ts/Tc/Tw`), MediaBox origin ≠ 0, rotated pages,
  malformed/truncated PDFs, huge/degenerate geometry, encrypted/linearized/XFA,
  mixed vector+scan, exotic encodings/CID fonts, RTL/CJK, 0-page, 10k-page.
- Compute the coverage delta: hostile classes with **zero** fixtures.
**Deliverable:** input-domain coverage matrix; prioritized adversarial-fixture
backlog (this directly becomes the corpus recommended in the state-continuity
audit).

### Phase 4 — Mutation testing (empirical oracle effectiveness)
**Goal:** *prove* whether green means anything. A test that stays green when the
product is broken is worthless; mutation kill-rate measures exactly that.
**Adversarial method:**
- Run mutation testing on core subsystems (content editing, save/rewrite, forms,
  fonts, redaction, image, reflow). Mutants: flip conditionals, drop emitted
  operators (e.g. delete the `op_Tz(100)` we just added), perturb constants/signs,
  swap matrix terms.
- Kill-rate per subsystem; enumerate **surviving mutants** — each is a concrete,
  reproducible hole (a change to shipping behaviour no test objects to).
- Cross-check: do the state-continuity fixes survive mutation? (Delete the
  `q…Q`/`Tz`/`rg` and confirm a test dies — if not, the fix is untested.)
**Deliverable:** mutation report with kill-rates and a ranked surviving-mutant
list (the highest-confidence "these are real gaps" evidence in the whole audit).

### Phase 5 — UI / E2E harness critique
**Goal:** critique the native-UI layer (pywinauto/WinAppDriver/FlaUI/PyAutoGUI) and
the "UI test matrix" — historically the source of over-claim and silent skips.
**Adversarial method:**
- Skip-guard audit: which live-UI tests are *permanently* skipped on CI
  (headless/no-desktop/UIA-wedge, gap-A14) and thus provide zero protection?
  Distinguish legitimate environment guards from "green because it never ran."
- Oracle critique of the UI layer: structural oracles (widget-exists, state-pipe
  readback) replaced flaky pixel baselines — but do they now miss *visual*
  regressions entirely? Is there ANY rendered-output verification?
- Matrix integrity: reconcile matrix "pass" rows against actual runnable/asserting
  tests; find over-claim (row green, test skipped/absent/assertion-free).
- Flake & hygiene: orphaned `PDFE.exe`, PrintWindow render variance, CI
  cancellation habits.
**Deliverable:** UI-harness critique with the "green-but-never-runs" and
"runs-but-can't-see-the-defect" lists; matrix-reconciliation report.

### Phase 6 — Process & governance audit
**Goal:** critique the *process* that lets weak tests pass as done.
**Adversarial method:**
- Gate discipline: audit the karen/validator/test-runner gates and the
  fabrication history; are gates evidence-based or attestation-based? Can they be
  (were they) bypassed under context pressure?
- CI integrity: exit-code masking (GAP-A15), JUnit parsing, flaky-test policy
  (quarantine vs delete vs ignore), required-vs-advisory checks.
- "Definition of Done": what does merging actually verify? Is test-first real or
  retrofitted? Is there an escaped-defect log + mandatory DCA?
- Why incremental reviews keep failing: document the anecdote-driven loop and
  specify the class-based, empirically-gated replacement.
**Deliverable:** process-maturity findings (TMMi-framed) + governance change list.

### Phase 7 — Synthesis, DCA, and standing remediation program
**Goal:** turn findings into a durable capability, not another one-off patch.
**Do:**
- Consolidate the Register (§4); severity-rank by (user-visible impact × exposure
  × likelihood).
- **Defect Causal Analysis** over the escaped-defect seed catalog (§5): map each
  historical escape to the harness property that should have caught it, and to the
  audit finding that now covers it — proving the audit would have caught the last
  N escapes.
- Propose the **standing controls** so the *next* class is caught, e.g.:
  - an **adversarial-ambient/hostile-input fixture corpus** run through every
    injector, with pixel/glyph-quad oracles asserting element *and* neighbor;
  - an **oracle standard** (visual features require a visual/quad oracle; save
    features require a render or byte oracle, never extraction-only);
  - a **mutation-kill-rate gate** on core subsystems in CI;
  - a mandatory **escaped-defect → DCA → class-test** loop;
  - RTM kept live; UI-matrix "pass" defined as "ran and asserted," enforced.
**Deliverable:** the ranked Flaws & Gaps Register + a phased remediation pipeline
(hand off to the `planner` for gated execution).

---

## 4. The Flaws & Gaps Register (documentation schema)

Every finding, from every phase, recorded uniformly so the output is a single
actionable register, not scattered notes:

| Field | Meaning |
|---|---|
| ID | stable slug |
| Phase | 0–7 |
| Class | oracle-weakness / coverage-gap / input-domain-gap / surviving-mutant / ui-skip / process |
| Location | file:line or subsystem / gate / CI step |
| Description | the flaw, concretely |
| Exemplar escape | which real bug this blind spot did (or could) let through |
| Failure the test would miss | the user-visible symptom the current test cannot see |
| Severity | impact × exposure × likelihood |
| Evidence | mutation survivor / grep / RTM row / run log |
| Remediation | the specific fix + the standing control it belongs to |

Register lives at `docs/test-harness-flaws-register.md`; machine-readable mirror
in JSON for tracking.

---

## 5. Escaped-defect seed catalog (DCA input)

Ground the audit in reality — each of these must map to a Phase-1..6 finding, and
the audit is only credible if it would have caught them:

1. State-continuity / mirror (oracle blind: extraction; fixtures benign) → P1, P3, P4.
2. Crash-on-close mid-edit (no destruction-under-active-state test) → P2, P4.
3. Small-text-edit no-op (oracle asserted structure, not the produced glyph) → P1, P4.
4. Wrong icons / form separators (no visual oracle) → P1, P5.
5. GAP-A15 exit-code masking (CI integrity) → P6.
6. Gate fabrication (governance) → P6.
7. UI-matrix over-claim (matrix integrity) → P5, P6.
8. RedactionVerifier full-containment (security oracle too weak) → P1, P4.

---

## 6. Exit criteria

The audit is complete when:
- every test layer and product subsystem has an oracle-strength score (P1) and RTM
  coverage class (P2);
- the hostile-input coverage matrix is quantified with a prioritized fixture
  backlog (P3);
- mutation kill-rates exist for all core subsystems with an enumerated
  surviving-mutant list (P4);
- the UI-matrix "pass" claims are reconciled against runnable/asserting reality
  (P5);
- every seed-catalog escape (§5) traces to at least one finding that now covers it
  (P7 DCA);
- the Register is ranked and a gated remediation pipeline is written.

**Non-goal:** fixing the flaws. This audit *finds and documents*; remediation is a
separate, gated program driven off the Register.

---

## 7. Execution model

- **Parallelizable:** Phases 1, 2, 3, 5 fan out cleanly by subsystem (one agent per
  subsystem cluster, as the state-continuity sweep did with 7 agents). Phase 4
  (mutation) is compute-heavy and runs per-subsystem in the background. Phase 6 is
  a focused single-stream audit. Phase 0 precedes all; Phase 7 follows all.
- **Agents:** `qa-expert` (orchestration), `mutation-test-specialist` (P4),
  `code-quality-pragmatist` / `test-runner` (P1 evidence), plus general-purpose
  fan-out per subsystem. Adversarial verification (independent second agent
  re-checks a sample of each phase's findings) to avoid the audit itself becoming
  happy-path.
- **Rough effort:** P0 ~½ day; P1/P2/P3/P5 ~1–2 days each (parallel);
  P4 compute-bound (background, 1–3 days); P6 ~1 day; P7 ~1 day. Wall-clock ~1
  week with fan-out.
- **Cadence:** each phase's Register entries land as it completes; no big-bang.
