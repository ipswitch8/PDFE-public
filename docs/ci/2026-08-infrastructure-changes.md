# CI and infrastructure changes — August 2026

A record of the build/CI work completed between 2026-08-19 and 2026-08-25, written
because none of it was reflected in the documentation and several of these changes
alter how a runner is provisioned. Product changes are not in scope here.

Every figure below is re-derivable from the tree or from a dated evidence file that
is already committed. Nothing here asserts a state that depends on a merged PR, on
deployed infrastructure, or on a registered CI runner.

---

## 1. The artifact quota was exhausted, and it was destroying the evidence

**964 MB live against a 500 MB free-tier limit → 53 MB.**

The consequence was not "storage is full". It was that the `report` job's upload
**failed on every run**, so the diagnostics of exactly the runs worth diagnosing
were the ones being lost.

**98% of the volume was crash minidumps**, 25–50 MB each, riding inside the 14-day
logs artifact. Two findings from fixing it are worth keeping:

- Dumps were never uploaded unconditionally. `writeMinidump` fires only from the
  SEH/terminate handlers, so the volume meant **crashes were happening on most
  runs** — the quota exhaustion was a symptom of a product problem, not a
  packaging one.
- Splitting the dumps into their own artifact **without** also excluding
  `ui-out/crashdumps/**` from the logs artifact stores them *twice*. The
  exclusion is what makes the split real.

Do **not** gate dump upload on a failure count: dumps exist to catch an
otherwise-invisible teardown access violation, which by definition can exit 0.

## 2. Crash dumps now go to private Cloudflare R2, never to a GitHub artifact

Keyed `pdfe-crashdumps/<commit>/run-<run id>/` — by commit **and** run, so two runs
of the same commit cannot silently overwrite each other's evidence.

The reason is containment, not cost. A minidump of this application can carry
document open-passwords, decrypted content from copy-restricted PDFs, OCSP/CRL
tokens and absolute host paths. Artifacts of a public repository are downloadable
by anyone, and this repository is intended to become public.

**If R2 is unconfigured, dumps are published NOWHERE** and the step says so loudly.
There is deliberately no artifact fallback — its ABSENCE is the security property,
so adding one "for convenience" would undo this entire change.

Note that `!ui-out/crashdumps/**` in the logs artifact **changed meaning without
changing text**: it used to prevent double storage (a cost concern) and is now the
only thing keeping dumps out of a published artifact (a containment control).

## 3. Build transport moved to R2 — `.github/actions/ensure-build-tree`

With runners on more than one machine, a downstream job can land on a host that
never built. The composite action at `.github/actions/ensure-build-tree/action.yml`
resolves a build tree in a fixed order:

    workspace → local per-SHA store → Cloudflare R2 → GitHub artifact → fail loudly

R2 is the only leg that serves BOTH local and remote runners at no egress cost
(10 GB free, egress free), which is why it beats a LAN share here.

Two behaviours that are deliberate and easy to "fix" wrongly:

- The R2 leg **fails closed on PARTIAL configuration** and falls through only when
  it is wholly unconfigured or on a genuine miss (`404|Not Found|NoSuchKey`).
  Treating a broken credential as a miss would silently downgrade to the artifact
  leg and hide the breakage.
- The workspace probe checks a `.pdfe-commit` stamp, not merely that files exist.
  Without it, any commit's binaries satisfy the probe.

Full detail: [`r2-build-store.md`](r2-build-store.md).

## 4. R2 credentials come from a git-ignored `.env`

`tools/runner-deployment/Set-R2Config.ps1` loads `AccountId` / `AccessKeyId` (and
the secret) from a `.env` beside it; explicit parameters override the file. The
tracked template is `tools/runner-deployment/.env.example`.

It **refuses a `.env` that git would track**, and validates the credentials with a
real PUT/GET/DELETE before setting anything — so a machine is never configured
with credentials that do not work.

- Machine-level environment only. Never a workflow `secrets:` block, never a
  committed file.
- **Consume-only machines MUST hold read-only, bucket-scoped tokens.** The
  `published-sha.txt` marker ships *inside* the writer-controlled zip, so it
  authenticates nothing against a hostile writer — it catches mis-keying and
  corruption only. Credential scoping is the actual control.

Its test harness is **`tools/runner-deployment/Test-SetR2Config.py`** (14 checks
against a mocked `aws` CLI, every run under `-WhatIf`), which exists because the
read-only-vs-writable behaviour is a *branch*: correct on a consume machine, a
failure on a build machine. Get it backwards and operators are pushed toward
giving every machine write access — the exact outcome the read-only rule prevents.

## 5. New standing checkers

Added under `tools/gate/` in this window:

| File | What it refuses to let happen |
|---|---|
| `check_ensure_build_tree.py` | a consuming job that never restores, an opt-in or vacuous restore, or a cross-RUN download missing `run-id` (which makes the whole path INERT while every name still matches) |
| `check_crashdump_artifact.py` | any artifact carrying dumps; a silent unconfigured-R2; dumps not excluded from the logs artifact; an unannounced truncation; a pruned PDB |
| `check_runner_label_coverage.py` | a `runs-on` naming a label no registered runner carries — GitHub does not error on that, the job queues then reports `cancelled`, which alerts nobody |
| `check_build_reuse_guards.py` | a producer guard that skips when a build was RESTORED rather than built |
| `check_chained_jobs.py` | a follow-on job deleting a build tree its own machine produced |
| `check_runner_deploy_pin.py` | a bootstrap pin that has drifted from the tracked package |

Supporting: `tools/gate/test_build_reuse_roundtrip.py` (executes the publish and
restore `run:` blocks extracted from the workflow itself, against a synthetic tree,
so what is tested is the shipped script rather than a re-implementation), plus
`tools/runner-deployment/Test-InstallerDryRun.py` and `Test-SetR2Config.py`.

## 6. Completed, dated events

- **History scrub — 2026-08-22.** An operator username removed from 1,290 affected
  commits across three surfaces (blobs, commit messages, author/committer
  identity). Record: [`../audit/history-scrub-2026-08-22.md`](../audit/history-scrub-2026-08-22.md).
- **Repository recreated — 2026-08-22.** The only self-service way to drop
  `refs/pull/*`, which GitHub owns and refuses to delete, and which `git clone`
  does not fetch — so three verification passes were blind to them by
  construction. Record: [`repo-migration-2026-08-22.md`](repo-migration-2026-08-22.md).
- **Rename completed — 2026-08-25.** The original repository became `PDFE_old`;
  the recreated one became `PDFE`. The pointer sweep that followed is a separate
  change (commit `04e7368`) and is not part of the migration record above.

## 7. A tracked file was regenerating the scrubbed username on every suite run

Found and fixed 2026-08-25, while running the suite for unrelated reasons.

`tests/support/ConformanceChecker.cpp` writes a conformance record into
`docs/audit/meta-audit-2026-07/followup/phase-14/conformance-checker-record.json`,
which is **tracked**. It embedded absolute host paths, so running the tests took
tracked-file occurrences of the operator's account name from **0 to 9** — the
exact string the 2026-08-22 history rewrite existed to remove. HEAD was clean
only because nobody had committed after a run.

Fixed with a `portablePath()` reduction to `<repo>` / `<home>` / `<temp>`, applied
both at emission and when merging persisted sidecars, and guarded by
`UiSuitePathHygiene.TheTrackedConformanceRecordIsPortable` (mutation-checked).
Full account, including why three existing controls all missed it, in
[`../audit/audit-of-audits-2026-08.md`](../audit/audit-of-audits-2026-08.md) §10a.

**The general lesson:** a history rewrite cleans the past. It does nothing to the
*producer* that wrote the string, and a producer nobody re-audits will refill
what the rewrite emptied.

## What this window did NOT change

- **No runner is registered against the renamed repository.** Registration is
  repo-scoped, so every self-hosted job is currently unschedulable, and
  `check_runner_label_coverage.py` is red *correctly*. Re-registration needs an
  interactive desktop session and is an operator action.
- **CI remains manual-only** for scheduled work; `fast-checks.yml` keeps its
  `push`/`pull_request` triggers because those are event-driven, not timers. See
  [`../audit/ci-manual-only-posture.md`](../audit/ci-manual-only-posture.md).
