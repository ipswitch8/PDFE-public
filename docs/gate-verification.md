# Gate Verification — Hardened, Artifact-Based (Remediation Phase 1)

**Status:** in force for PDFE as of the harness-audit remediation.
**Closes Register:** C1 (gates verify prose, not results), H6 (test-runner gate
has no GoogleTest branch / trusts exit codes), M-e (no standing verification
control).
**Source of record:** `docs/audit/README.md`, `docs/audit/remediation-plan.md`.

---

## 1. Why this exists

The adversarial audit proved that "green" carried no guarantee. The legacy
gate hook (`~/.claude/hooks/phase-gate.sh`) decided a gate PASS by grepping the
gate agent's **chat prose**:

1. a `VERDICT: PASS` line, else
2. a bold `**PASS**` line, else
3. a **word-count fallback**: any stray `PASS` word with no `FAIL` word → green.

No test was re-run; no artifact was checked. A subagent (or a summary of one)
merely *saying* "pass" passed the gate. The forgery was even allow-listed:
`.claude/settings.local.json` permitted `jq '.current_gate_results[...] = true'`
+ `mv` one-liners that flipped a gate result straight from Bash.

## 2. The rule now: PASS requires a machine-checked artifact

The gate hook **never reads `last_assistant_message` to decide a verdict.**
A gate PASS requires an on-disk artifact.

> **Corrected 2026-08-03 (meta-audit follow-up F19, gaps A5-49 / A5-50).**
> This section used to say *"The repo-local hook `.claude/hooks/phase-gate.sh`"*.
> **There is no repo-local hook.** `.claude/hooks/` does not exist in this tree,
> `.gitignore:159` ignores `.claude/`, and `git ls-files .claude` returns
> exactly one path — `.claude/settings.json` — which itself records that the
> scripts *"were centralized to the global hooks dir and no longer exist under
> the project"*. The hook that actually runs is the **user-global**
> `~/.claude/hooks/phase-gate.sh`.
>
> Two consequences worth stating plainly rather than leaving implied:
>
> 1. **The control is not reproducible from a clone.** A fresh checkout gets
>    `settings.json` and no hooks at all. Every gate guarantee on this page
>    depends on a per-developer file outside the repository. That is a real
>    residual, not a documentation nit.
> 2. **The artifact path below is also the old layout.** Pipelines are now
>    namespaced per session, so the live path is
>    `.claude/pipelines/<pipeline-id>/gate-artifacts/<phase-id>/…`. The
>    singular `.claude/gate-artifacts/…` form is retained below only because
>    older evidence trails were written there.
>
> Related F17 guards already assert the *harness* side of this
> (`GateEvidenceHarness.ResolvesTheHookFromEitherHookDirectory` and
> `…KnownResidualMatchesWhichHookActuallyExists` in
> `tests/test_doc_drift_guards.cpp`). F19 adds
> `F19DocClaims.GateVerificationDocDoesNotClaimARepoLocalHook`, which fails if
> this page re-asserts a repo-local hook while none exists.

Current layout:

```
.claude/pipelines/<pipeline-id>/gate-artifacts/<phase-id>/<agent>.json
.claude/pipelines/<pipeline-id>/gate-artifacts/<phase-id>/gtest-results.xml
```

Historical layout (still found in committed evidence trails):

```
.claude/gate-artifacts/<phase-id>/<agent>.json      # {"verdict":"PASS"|"FAIL", ...}
.claude/gate-artifacts/<phase-id>/gtest-results.xml # required for evidence gates
```

**The two gate helper scripts live in `~/.claude/hooks/`, NOT in
`tools/gate/`.** `tools/gate/` contains only Python checkers. The correct
invocations are `~/.claude/hooks/emit-gate-verdict.sh` and
`~/.claude/hooks/check-hooks-alive.sh`. Several documents in this repo — and,
at the time of writing, `CLAUDE.md:83` and `CLAUDE.md:92` — give the
`tools/gate/` path, which does not resolve. See the F19 dispositions file for
the outstanding correction to `CLAUDE.md`.

Two gate classes:

| Class | Gates | What "PASS" requires |
|---|---|---|
| **Evidence** | `test-runner`, `perf-benchmarks` | artifact `verdict==PASS` **AND** the hook's own parse of `gtest-results.xml`: `failures=0`, `errors=0`, `tests>0`, ≥1 `testcase status="run"`, and (if the artifact names a `target`) that test present + run. The process **exit code is ignored** (GAP-A15). |
| **Judgment** | `karen`, `validator`, `security-audit` | artifact `verdict==PASS` **AND**, if the phase also has an evidence gate, that phase's `gtest-results.xml` is already green. A reviewer cannot sign off a phase whose tests are red. |

A verdict artifact claiming `PASS` with a red or absent XML is forced to
`remediation`. Prose is used for exactly one thing: an optional `PHASE:
phase-N` tag that *routes* a result to the right phase — it can never *set* a
verdict.

### Authoritative override

The user-global legacy hook may still fire on the same `Stop`/`SubagentStop`
event (it is intentionally left untouched so other projects are unaffected).
The repo-local hook therefore **re-derives the current phase's gate results
from artifacts and overwrites state on every `Stop` event** (`rederive_phase`).
On end-of-turn `Stop`, artifact-truth is the final word regardless of hook
ordering: a prose-only "PASS" with no artifact is reset to `remediation`.

## 3. The test-runner gate convention

When the pipeline calls the `test-runner` gate for `<phase-id>`, the gate run
MUST, from the repo root:

```bash
mkdir -p .claude/gate-artifacts/<phase-id>
# Full suite. XML is the authority; the exit code is not trusted.
"C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" \
  --build --preset debug-vs2022 --target pdfe_tests
./build/vs2022/bin/Debug/pdfe_tests.exe \
  --gtest_output=xml:.claude/gate-artifacts/<phase-id>/gtest-results.xml

# Machine-check the XML (single source of truth, shared with CI):
python tools/gate/check_junit_xml.py \
  .claude/gate-artifacts/<phase-id>/gtest-results.xml
```

Then write the verdict artifact reflecting the checker's exit code:

```bash
# checker exit 0 -> PASS, else FAIL
echo '{"verdict":"PASS","agent":"test-runner","xml":"gtest-results.xml"}' \
  > .claude/gate-artifacts/<phase-id>/test-runner.json
```

Rules baked into this convention (were violated by the legacy gate):

- **Full suite, no regressions.** No "prefer a targeted subset" — the whole
  `pdfe_tests.exe` runs. A phase may additionally pass `--require-test
  <NewTest>` / `--min-tests <N>` to `check_junit_xml.py` to prove the phase's
  own new test is present and the suite didn't silently shrink.
- **XML, never the exit code.** `pdfe_tests.exe` can exit 0 with failures in
  the XML (GAP-A15). The checker and the hook both parse the XML.

Judgment gates (`karen`, `validator`, `security-audit`) write only their
`<agent>.json` verdict artifact; the hook cross-checks the phase XML for them.

## 4. CI parity

The dispatch-only CI workflows run `pdfe_tests.exe --gtest_output=xml:...` and
then call **`tools/gate/check_junit_xml.py`** — the same checker the gate
convention uses — as the pass/fail authority, so CI cannot be fooled by a
zero exit code either. CI also runs a mutation-kill-rate job and a coverage
step (see `.github/workflows/`).

## 5. `/compact` hook-delivery mitigation (chosen approach)

**Problem:** after a `/compact`, Claude Code stops delivering `SubagentStop`/
`Stop` hook events reliably for the rest of the session, so the first gate
invoked after a compaction may never have its result recorded (see project
memory `compact_breaks_hooks`).

**Mitigation (implemented, not just documented):** gate recording is moved
**off the fragile hook event** and onto a **file artifact the hook re-reads on
demand**. Because a gate's verdict lives in
`.claude/gate-artifacts/<phase>/<agent>.json`, it survives even if the
`SubagentStop` event that *would* have recorded it was dropped:

1. The repo-local hook re-derives the whole current phase from artifacts on
   **every** `Stop` event, and the `Stop` event is far more reliably delivered
   than `SubagentStop`. So a dropped `SubagentStop` is recovered on the next
   `Stop`.
2. **Operational rule:** after any `/compact` in a session that is mid-pipeline,
   **re-invoke the first gate of the current phase once** before trusting the
   gate state. The re-invocation is idempotent (it just re-reads the artifact),
   so it is safe to repeat. This is the belt-and-suspenders companion to (1).

This is the "move gate recording off the fragile hook" option from the
remediation plan, with the re-run rule kept as a cheap safety net.

## 6. Direct evidence

`docs/audit/phase1-evidence/verify_gate_hardening.sh` drives the real hook in
an isolated sandbox and asserts: fabricated prose does not flip a gate; a PASS
artifact with a red XML does not pass; a PASS artifact with a green XML does;
and a judgment gate cannot pass over red tests. Captured output is in
`docs/audit/phase1-evidence/README.md`. Run it any time with:

```bash
bash docs/audit/phase1-evidence/verify_gate_hardening.sh
```
