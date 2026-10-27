# Phase-4 — Process / Governance Critique

**Adversarial harness audit, process layer.** Stance: the harness's own
quality-control machinery is the defendant. FIND-AND-DOCUMENT only; no code
changed. Every claim below distinguishes *exists* from *enforced* — a workflow
or script that runs but never fails the build is decorative.

Sources read: the registered gate hooks (`~/.claude/hooks/phase-gate.sh`,
`pre-tool-use.sh`, `emit-gate-verdict.sh`), `tools/gate/*`, all ten
`.github/workflows/*.yml`, both `.claude/settings*.json`, the escaped-defect
history in `docs/audit/`, and the phase-3 mutation run.

---

## 1. Gate evidence-vs-attestation integrity — FIX IS REAL AND COMPLETE

**Verdict source: ARTIFACT, not prose.** The prior audit (Register C1) found the
legacy hook decided a gate PASS by grepping the agent's chat for
`VERDICT: PASS`, a bold `**PASS**`, or — worst — a word-count fallback where any
stray "PASS" with no "FAIL" flipped the gate green. That is gone.

The registered `~/.claude/hooks/phase-gate.sh` now:

- Derives every verdict **only** from an on-disk artifact
  `gate-artifacts/<phase>/<agent>.json` plus, for evidence gates
  (`test-runner`/`perf-benchmarks`), a JUnit XML the hook **parses itself**
  (`derive_verdict` / `validate_junit_xml`, lines 179-241): failures=0,
  errors=0, tests>0, ≥1 `status=run` case, and if the artifact names a target
  test, that test must be present and run. The XML is trusted, **not** the exit
  code (GAP-A15).
- On the end-of-turn Stop event, **authoritatively re-derives the entire current
  phase from artifacts and overwrites the ledger** (`rederive_phase`, lines
  442-443). So even if a legacy prose-based hook still fires on the same event,
  artifact-truth is the final word.
- Judgment gates (karen/validator/security-audit) have no XML of their own;
  their artifact verdict must be exactly `PASS`, **and** if the phase also has a
  `test-runner` gate that phase's XML must already be green — a judgment gate
  cannot sign off a phase whose tests are red (`phase_has_evidence_gate`, lines
  235-240).

**No forgery allow-list remains.** The prior audit found a forgery one-liner
allow-listed in `settings.local.json`. The current file's 195 allow entries are
all build/git/test/dumpbin commands — there is **no** emit-verdict, ledger-write,
or `echo VERDICT` entry. `pre-tool-use.sh` additionally hard-blocks any
Write/Edit/Bash/PowerShell that references the gate-ledger file or the
shell-unlock nonce (lines 48-215), so a script cannot forge state.

**Residual attestation paths (both low-risk):**

1. `phase-gate.sh` still greps the assistant message for a `**PHASE: phase-N**`
   tag (lines 384-389) — but only to **route** an artifact to the right phase;
   PASS/FAIL is never taken from prose. A mis-tag can misroute a verdict to a
   valid sibling phase, not manufacture a PASS.
2. Judgment-gate verdicts are honor-system: karen/validator emit their own PASS
   via `emit-gate-verdict.sh` with no independent evidence. The red-XML veto
   bounds the exposure to "green tests but acceptance criteria not truly met."

**Assessment: the reliability fix is real and, for the registered hook,
complete.** Verdict source = artifact; forgery allow-list = absent.

---

## 2. GAP-A15 (exit-code masking) — GENUINELY DEFENDED WHERE CI RUNS

`pdfe_tests.exe` can exit 0 with failures in the XML. All three OS lanes trust
the XML, not the exit code:

| Lane | XML parse | Full suite? | Evidence |
|---|---|---|---|
| Windows | `run_gtest.py` parses XML + `check_junit_xml.py` gate | yes | windows-validation.yml:305-344 |
| Linux | inline python: tests>0 ∧ failures=0 ∧ errors=0, explicit vacuous-run refusal | yes | linux-validation.yml:88-124 |
| macOS | `check_junit_xml.py` | **no — `PreflightOpen.*` subset only** | macos-validation.yml:118-130 |

The Windows job **removed** the `continue-on-error` flag that once let 2 real
`BatchDialogTest` failures ride green for 3 phases (comment at :249-261). The
single-source authority `tools/gate/check_junit_xml.py` is shared by CI and the
local `test-runner` gate, so both agree on what "passed" means.

On the "tell-tale (tests ≠ passed+skipped)": it is not computed literally, but
the wired check is the **stronger** direct form — sum of `<testsuite>`
failures/errors must be 0, tests>0, ≥1 case ran, plus an optional `--min-tests`
suite-shrinkage guard — which subsumes it.

**Two operational caveats (not logic bugs):** (a) Windows/Linux full validation
runs **nightly + workflow_dispatch only**; per-commit push/PR triggers were
removed 2026-07-20, so a masked failure surfaces the next night, not at merge.
(b) macOS covers only the preflight subset.

---

## 3. Mutation & coverage floors

**Mutation gate: REAL engine, green-by-construction manifest, on-demand only.**

- `tools/gate/mutation_kill.py` is a genuine apply → incremental-build → run
  killing-test → **always-revert** harness with real SURVIVOR detection
  (lines 99-127).
- `tools/gate/mutants.json` carries **3** seed mutants
  (`M2-coordmapper-origin-sign`, `M-a-coordmapper-rot90-axis`,
  `M4-exif-strip-noop`) — **all three are already-fixed lines curated to be
  killed**, so `mutation-kill.yml` can only ever report 100%.
- The kill-floor **is** mechanically enforced: floor default 1.0, exit 1 on
  below-floor fails the job (mutation-kill.yml:61-75). But the workflow is
  **workflow_dispatch only** (:23-24), and `governance.yml` runs merely a
  `--dry-run` manifest validation (:60-62) — no build.

**Versus phase-3.** Phase-3 (`docs/harness-audit/phase-3/mutation-report.md`)
built a **real 16-mutant** harness across 5 subsystems and found **10 surviving
mutants — 37.5% kill-rate.** Those 10 are concrete behavior-changing defects
that ship green today, e.g.:

- redaction `checkSingleRevision` forced `return true` — a **confidentiality
  regression** (recoverable pre-redaction content reported clean);
- font-substitution `-100 Tz` mirror and dropped `rg` fill color — invisible to
  extraction-only oracles;
- halved font size, alignment-offset no-op, glyph-scale inversion, GC downgrade,
  two forced-recompress autosave perf regressions.

**The standing CI mutation gate represents NONE of these 10 survivors.** The
gate meant to be the standing defense against false-green oracles exercises only
3 lines chosen because they already pass. It would catch a *new* regression on
those 3 lines and nothing else.

**Coverage floor: UNENFORCED.** OpenCppCoverage → Cobertura is
`continue-on-error: true`, silently skipped when the tool is absent, and gated
by **no floor** (windows-validation.yml:346-374). It emits an artifact but can
never fail the build — decorative.

---

## 4. Flaky policy, Definition-of-Done, DCA loop

**Flaky policy: PRESENT (rerun-on-failure), no formal quarantine.**
`run_gtest.py --reruns 2` re-runs only failed tests in a *fresh* process; a
transient flake heals, a genuine failure fails every rerun
(windows-validation.yml:291-326). The UI suite mirrors this via
`run_ui_suite.py --reruns`. Known-flaky tests
(`PerfStressTest.TenThousandAnnotationsOnePage`,
`MainWindowIntegrationTest.ZoomComboExists`) are tracked only in **inline
comments** — there is no machine-readable quarantine/xfail registry, and
`KNOWN_WEAK_TESTS.txt` is a resolved-defect ledger, not a live quarantine.

**Definition-of-Done: ABSENT.** No first-class DoD artifact for a test/feature
exists. `check_junit_xml --min-tests` (suite-shrinkage guard) and the governance
standing controls approximate one, but there is no crisp per-feature "done"
contract.

**DCA loop: DECORATIVE (real machinery, opt-in trigger, unused).**
`check_dca.py` validates existing records (mandatory sections + named tests must
exist on disk) and `check_dca_presence.py` fails a defect-fix git range that
adds no DCA record (with a `[skip-dca]` escape hatch). Both are real and both
are wired into `governance.yml:51-58`. **But** `governance.yml` is
`workflow_dispatch` only, and `check_dca_presence` runs only when
`inputs.dca_base` is manually supplied (:55). So the escaped-defect → DCA →
class-test loop is **never enforced automatically on a commit or PR** — it fires
only when a human dispatches governance with an explicit base ref. Consistent
with that, only **one** DCA record exists
(`docs/audit/dca/0001-content-stream-state-leak.md`).

**Escaped-defect governance implication.** The harness has been **reactive since
inception**: of 72 bug-fixes mined from 1,015 commits, **70 escaped to a human**
and were tested only afterward (oracle 41 / coverage 15 / input-domain 8 /
process 6 — docs/audit/README.md:34-35). The DCA loop is the designed remedy,
and it is well-built — but because it is opt-in and human-triggered, it has not
changed the reactive posture. One record against seventy escapes.

---

## Ranked governance findings

| # | Sev | Finding |
|---|---|---|
| 1 | **High** | **CI is decoupled from the offending commit.** All PDFE CI is nightly/workflow_dispatch only (push/PR removed 2026-07-20). No gate fails a bad commit at PR/merge time; regressions surface next nightly at best, only on manual dispatch at worst. |
| 2 | **High** | **Mutation gate is green-by-construction.** 3 curated-to-be-killed mutants → always 100%. Phase-3's real run found 10 survivors (incl. a redaction confidentiality regression) that the standing gate does not test. |
| 3 | **High** | **Escaped-defect/DCA loop not enforced.** 70/72 fixes were reactive; the DCA machinery that would end that is opt-in, human-triggered, and has 1 record. |
| 4 | Med | **Coverage floor unenforced** — best-effort Cobertura, no floor, silently skippable. |
| 5 | Med | **macOS lane is a thin subset** — only `PreflightOpen.*`, not the full suite. |
| 6 | Low | **No formal flaky quarantine ledger** — known flakes live in comments; a degrading test isn't a first-class signal. |
| 7 | Low | **Judgment-gate verdict is honor-system** — bounded by the red-XML veto. |

**Bottom line.** The gate *integrity* work is genuine and complete: verdicts come
from artifacts, not prose, and the forgery allow-list is gone. The GAP-A15
XML-truth defense is real. The weakness is not fabricated evidence anymore — it
is **enforcement decoupling**: the strongest checks (full validation, real
mutation, DCA presence, coverage) are all mechanically sound but operationally
opt-in or green-by-construction, so nothing blocks a regression at the moment it
is introduced. The machinery is L3-grade; the *triggering* is not.
