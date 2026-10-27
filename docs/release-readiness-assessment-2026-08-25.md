# Release-readiness assessment — 2026-08-25

**Question:** what remains to be worked on before PDFE is released?

**Short answer:** v0.1.0 is already released and is correctly code-signed. Nothing
found here retroactively invalidates it. Two **operator actions** currently block
all CI, and one newly-found defect would have blocked making the repository
public had it not been fixed today.

Every blocker below cites a file, a line, or an exact command whose output is
recorded in this repository. Where a previously-recorded belief turned out to be
wrong, the correction is stated rather than quietly dropped — several of them
were steering decisions.

**Scope note.** This is an assessment, not a plan. Nothing here is asserted as
done that is not done, and no recommendation depends on an EXTERNAL blocker being
resolved first.

---

## 1. Corrections — beliefs that were load-bearing and were false

| Belief | Reality | Established by |
|---|---|---|
| "v0.1.0 is NOT code-signed" | **It is signed, and the signature verifies.** `Get-AuthenticodeSignature` on the published `PDFE-0.1.0-win64.exe`: `Status: Valid`, signer `CN=Itwerx Corp, O=Itwerx Corp, L=Seattle, S=Washington, C=US`, issuer `Microsoft ID Verified CS EOC CA 04`, Microsoft TSA countersignature present. | [`security/unsigned-binary-risk-acceptance.md`](security/unsigned-binary-risk-acceptance.md), which carries the machine-readable markers `SIGNING-STATUS: SIGNED-MANUAL-ONLY` and `SIGNING-STATUS-COMMIT: 92e1328`. **Cited in preference to my own download-and-verify**, which was real but ephemeral — the `karen` gate correctly noted that a citation nobody else can re-run is not evidence. (The file is still *named* `unsigned-...` and says so in its own header; the name is stale, the content is not.) |
| (a would-be alarm) "the signing certificate expired 2026-07-20" | **Expected, not a defect.** Azure Trusted Signing issues short-lived per-signing certificates; the RFC-3161 timestamp preserves validity past leaf expiry. Raising this as a blocker would have been a false alarm. | `NotAfter` vs the TSA countersignature on the same file |
| "suite baseline 5879 / 0F / 9 skipped at commit `576a636`" | Stale on two figures of three, **and the commit no longer exists** — the 2026-08-22 history rewrite re-hashed every commit, so every pre-scrub SHA in older notes is a dead reference. | [`audit/release-readiness/gtest-run-summary.md`](audit/release-readiness/gtest-run-summary.md) |
| `CLAUDE.md`: "Primary suite, 2068 tests" | Stale by roughly 3,800 tests. Corrected. | same |
| `README.md`: "4,000+ automated tests" | True but badly understated. Corrected. | same |
| audit-of-audits §10, "Ranked — what to do next" | **Eight of its twelve items had already shipped.** A ranked list that still reads as outstanding sends the next reader at work that is already done. | [`audit/audit-of-audits-2026-08.md`](audit/audit-of-audits-2026-08.md) §10a, per-row evidence |

---

## 2. Current verified state

**Suite:** 5,898 registered / 5,882 passed / 0 failed / 0 errors / 16 skipped,
CLEAN across all 16 shards with **no reruns needed**. Recorded at
[`audit/release-readiness/gtest-run-summary.md`](audit/release-readiness/gtest-run-summary.md);
run of 2026-08-25 against a binary rebuilt the same day (the staleness preflight
passed for both `pdfe_tests.exe` and `pdfe.exe`, which mattered — C++ test sources
had changed on 2026-08-22 while the previous binary dated from 2026-08-19).

The 16 skips are environment-gated (absent external corpus, OCR tessdata,
perf/sandbox/cache gates), itemised individually in that file. There are **zero**
`DISABLED_`-prefixed cases.

**Acrobat-parity matrix:** 750 green / 1 blocked / 33 wont-fix / 0 planned / 3 xfail / 0 skip
= 787 rows. Re-derived from `tests/ui_automation/matrix/ui_test_matrix.csv` and
matching [`acrobat-pro-parity-status.md`](acrobat-pro-parity-status.md) exactly —
that document is **current**, unlike the registers in §1.

---

## 3. EXTERNAL blockers — operator action; not fixable from the working tree

### 3.1 No self-hosted runner is registered — the binding constraint

- `gh api repos/ipswitch8/PDFE/actions/runners` → **`total_count: 0`**, captured
  in [`audit/runner-labels.json`](audit/runner-labels.json).
- Runner registration is **repo-scoped** and did not survive the rename, so every
  self-hosted job is unschedulable. An unschedulable job does not error: it
  queues and is then reported `cancelled`, a status indistinguishable from a
  deliberate cancellation and one that alerts nobody.
- `python tools/gate/check_runner_label_coverage.py` → **exit 1**, refusing to
  score an empty fleet. **That red is correct.** Its `--self-test` passes 10 checks.

**This one fact is why CI is red.** `fast-checks.yml` is GitHub-hosted and
push-triggered, so it does run — and since the last green run (2026-08-22T22:44:48Z)
there have been **12 consecutive non-success `Fast Checks` runs: 11 `failure` and
1 `cancelled`**. Captured in
[`audit/release-readiness/ci-and-host-evidence-2026-08-25.md`](audit/release-readiness/ci-and-host-evidence-2026-08-25.md);
re-derive with `gh run list --repo ipswitch8/PDFE --limit 30`.

The failing step is *"Build reuse cannot silently skip a control (phase-77)"*, and
the failing command inside it is `check_runner_label_coverage.py`. Registering the
runner turns that lane green as a side effect.

*(An earlier draft of this document said "all nine pushes". That was wrong — it
was eyeballed off a truncated listing rather than counted, and the `karen` gate
caught it. The count is now taken from the committed capture.)*

Re-registration requires an **interactive desktop session** (session 0 blinds
UIA). Sequence: [`ci/repo-migration-2026-08-22.md`](ci/repo-migration-2026-08-22.md).

### 3.2 Disk headroom — this host qualifies for neither runner role

- `tools/runner-deployment/runner-requirements.json` →
  `profiles.consume.min_free_gb` = **15**, `profiles.build.min_free_gb` = **120**.
- Free space on `C:` was measured **three times on 2026-08-25 and disagreed each
  time**: 14.8 GB, then 12.18 GB during a build + full-suite run, then 15.05 GB
  after cleanup. Captured in
  [`audit/release-readiness/ci-and-host-evidence-2026-08-25.md`](audit/release-readiness/ci-and-host-evidence-2026-08-25.md);
  re-derive with `Get-PSDrive C`.
- So the honest reading is **not** "it fails the consume floor" — it is that the
  host sits ON the 15 GB threshold and crosses it in the ordinary course of doing
  work. A runner provisioned here would pass its precondition check at
  registration and fail it mid-job.
- The **build** profile is unambiguous: ~105 GB short, on a 300 GB volume that is
  95% full. At least one machine must satisfy it or the `build` and `release` jobs
  queue forever.

---

## 4. INTERNAL — found and fixed during this assessment

### 4.1 The suite regenerated the scrubbed username into a tracked file

`tests/support/ConformanceChecker.cpp` wrote absolute host paths into the tracked
`docs/audit/meta-audit-2026-07/followup/phase-14/conformance-checker-record.json`.
Measured: running the suite took `git grep -l <account-name>` over tracked files from
**0 to 9** — the string a 1,290-commit history rewrite, three leak surfaces and a
repository recreation existed to remove, on a repo intended to become public.

Three existing controls each had a scope limit that let it pass: F48 covered the
**Python UI runner**; `tools/gate/scrub_artifacts.py` covers **CI-produced**
artifacts; `UiSuitePathHygiene.EveryTrackedRunArtifactIsPortable` reads as
class-wide but enumerates **`tests/_runtime` only**.

Fixed and guarded (commit `fdba4dc`), verified by a **full** suite run leaving the
count at zero. Full account: [`audit/audit-of-audits-2026-08.md`](audit/audit-of-audits-2026-08.md) §10a.

### 4.2 The full-sweep matrix-health gate had silently lost two protections

`ValidationLaneHygiene.MatrixHealthGateIsFullSweepStrict` was RED at HEAD.
Commit `77e07ad` (the UI-sharding split) rewrote the invocation from
`--floor-delta 0 --require-ran` to `--floor-delta 5` with no `--require-ran`,
handing back both A1-81/A1-83 holes: a five-row green drop became tolerated, and a
committed-green row that never ran counted as "not red". Repaired in `92bdbc7`.

---

## 5. INTERNAL — open

Classified so the release call is separable from the engineering backlog.

| Item | Evidence | Read |
|---|---|---|
| **Use-after-free, 2026-08-13** | [`audit/crash-evidence/2026-08-13-pdfe-uaf.md`](audit/crash-evidence/2026-08-13-pdfe-uaf.md) — two AVs, `rax=0xdddddddddddddddd` (MSVC freed-memory fill), uptimes 23 s / 12 s | **Highest open product risk.** Post-dates its supposed fix by 19 days and is **not symbolisable** — the PDB is gone. Reproducing it needs CI, which needs §3.1. |
| **Security-testing regime unwired** | `scripts/security/run_secrets.ps1` and `check_regressions.ps1` exist; `git grep 'scripts/security' -- .github/` finds only a comment saying that path is "never reached". No Dependabot config. | Real gap: SAST, secrets and dependency-CVE scanning run in **no** lane, dispatchable or otherwise. |
| **F75 — 87 `fz_try`/RAII hazards** | `tools/gate/fz_try_raii_baseline.json` | Open **by choice** and bounded — each leaks only on an error path, and CI fails on any hazard not in the baseline, so the population cannot grow. |
| **F71 — staging-file DACL window** | [`audit/meta-audit-2026-07/followup/deferred-backlog.md`](audit/meta-audit-2026-07/followup/deferred-backlog.md) F71 | Confidentiality, not integrity: a restrictive-ACL document has a readable copy under the directory's inherited ACL for the duration of a save. |
| **F70 — shard death contaminates the healed-flake ledger** | [`audit/meta-audit-2026-07/followup/deferred-backlog.md`](audit/meta-audit-2026-07/followup/deferred-backlog.md) F70 | Plus a new sub-case found today — see §7. |
| **F72 — 11 toolbar actions have no artwork** | [`audit/meta-audit-2026-07/followup/deferred-backlog.md`](audit/meta-audit-2026-07/followup/deferred-backlog.md) F72 | Cosmetic, but the oracle is blind to the class: it iterates only actions currently *on* a toolbar, which depends on persisted QSettings. |
| **Linearization coverage** | `tests/test_linearization.cpp` — **2 tests** | Highest-blast-radius transform, thinnest coverage. |
| **26 planned UI-matrix rows (22 at P1)** | `tests/ui_automation/matrix/ui_test_matrix.csv` | **Coverage gaps, not missing features** — the distinction matters for the release call. |
| **Coverage floor 25 points below actual** | `windows-validation.yml` `COVERAGE_FLOOR: "0.50"` vs 75.65% measured | A floor that loose would not catch a large regression. |

---

## 6. Not blockers — each has been mistaken for one

- **Code signing** — verified valid (§1). This was recorded as an open item and was not.
- **Manual-only CI** — a recorded owner decision with its cost documented, not an
  oversight: [`audit/ci-manual-only-posture.md`](audit/ci-manual-only-posture.md).
  `fast-checks.yml` keeps `push`/`pull_request` because those are event-driven,
  not timers. The consequence to be honest about is that **nothing auto-alerts**:
  `ci-liveness.yml` exists and is correctly hosted off the machine it watches,
  but it is `workflow_dispatch`-only, so it is a manual probe rather than an alarm.
- **macOS / Linux lanes red** — expected; those platforms are not built yet
  ([`../README.md`](../README.md) platform-support section).
- **`check_runner_label_coverage.py` red** — correct fail-closed behaviour; it
  self-clears the moment §3.1 is done.

---

## 7. A method finding, recorded because it nearly produced a false green

The first full-suite run in this work reported `CLEAN` after "healing" one failure
on a rerun. It was **not** a flake. The failing test reads a workflow file at
runtime; it failed at 01:49:21, and that workflow was edited at 01:52:17 to repair
the regression it had just caught. The end-of-run rerun passed against the
repaired file, and the runner recorded **a real regression as a transient flake**.

`tools/gate/check_flaky_quarantine.py` correctly FAILED on the resulting ledger,
because the test is not in `tools/gate/flaky_quarantine.json`. Both available
exits were bad — accept a false CLEAN, or register a healthy test as a known flake
and destroy the registry's discriminating value. The correct third option is the
one neither exit offers: discard the run and re-run against a frozen tree.

**Rule:** do not edit any tracked file while a suite run is in flight, even to fix
a defect that run just found. At least eight guard files read
`windows-validation.yml` at runtime, so a run straddling an edit has shards that
measured different trees and is evidence of nothing. This generalises the existing
"never edit source while the mutation harness runs" lesson to *every* suite run —
the mechanism is the file read, not the harness.

---

## 8. Go / no-go

**v0.1.0, as already released to the private repository — GO.** It shipped, it is
correctly signed, and nothing found here invalidates it.

**Making the repository PUBLIC — GO, but only because §4.1 was fixed today.**
Before that fix, the first person to run the suite and commit would have undone
the history rewrite. Nothing further is strictly required; §5's open items are
disclosed engineering debt, not disclosure hazards.

**The NEXT release — NO-GO until §3.1 is done.** Not because the code is bad, but
because **nothing can be verified**. With no self-hosted runner there is no full
sweep, no render verification, no sanitizer lane, no UI E2E, and no way to
reproduce the open use-after-free. Shipping on a green that no lane produced is
precisely what this project's audit programme exists to prevent.

### Blocking vs tracked-but-non-blocking

**Blocking the next release:** §3.1 runner registration; §3.2 disk headroom; and
one full green `windows-validation` sweep once those are done.

**Tracked, not blocking:** every item in §5. The UAF is the one to watch — it is
non-blocking only in the sense that it is not newly introduced and its severity
cannot be established without CI. If a sweep reproduces it, it becomes blocking.

### Recommended order

1. **Register the runner** (§3.1) — unblocks everything and greens `fast-checks`.
2. **Free disk, or designate another build-profile host** (§3.2).
3. **Run a full `windows-validation` sweep.** First real signal since the
   migration — treat the first result as information, not as a regression report.
4. **Investigate the UAF under ASan.** A minidump without its PDB is an address,
   not a diagnosis, and this project's own record says an all-third-party-frames
   stack means heap corruption: use ASan, not dumps.
5. **Wire the security regime** (SAST / secrets / dependency-CVE) to a
   dispatchable lane.
6. **Cover linearization**, then work down F75.

Steps 3–6 are ordinary engineering. Steps 1–2 are not, and nothing else can be
trusted until they are done.
