# Repository migration — `PDFE` → `PDFE-scrubbed`, 2026-08-22

The history scrub removed an operator's username from all 1,290 affected
commits and was verified clean on `master` and both tags. It could **not**
remove it from `refs/pull/1|2|3/head`: GitHub owns those refs, refuses to delete
them (`deny updating a hidden ref`), and `git clone` does not fetch them — which
is why three separate verification passes missed it. Full account in
`docs/audit/history-scrub-2026-08-22.md`.

Recreating the repository is the only fully self-service way to drop those refs.
That is what this records.

## State

| | |
|---|---|
| New repository | `ipswitch8/PDFE-scrubbed` (private) |
| History | 1,307 commits, **zero** occurrences across all objects |
| Refs served | `master`, `assets-v1`, `v0.1.0` — **no `refs/pull/*`** |
| Releases | `assets-v1` and `v0.1.0`, all three assets re-uploaded |
| Old repository | `ipswitch8/PDFE` — still present, **you are deleting it** |

**Rename it to `PDFE` once the old one is gone.** GitHub frees the name on
deletion, and Settings → General → Repository name will take it. Every
operational reference in the tree points at `PDFE-scrubbed` today, so a rename
means one more sweep — or leave the name and skip that.

> ### ADDENDUM 2026-08-25 — done, and the route differed from the one above
>
> **This section's instruction is now satisfied. Everything above it is left
> exactly as written, because it is the record of what was true on 2026-08-22.**
>
> The old repository was **not deleted** — it was **renamed to `PDFE_old`**,
> which frees the `PDFE` name just as well and keeps the original recoverable.
> The recreated repository was then renamed `PDFE-scrubbed` → **`PDFE`**. So the
> State table's "Old repository … **you are deleting it**" describes an intent
> that was superseded, not what happened.
>
> The sweep this section anticipated was done in commit `04e7368`: 25
> occurrences across 11 files, plus the local git remote. It deliberately did
> NOT touch this file, on the rule that a dated record of a past state is not a
> pointer — which is why the body above still says `PDFE-scrubbed` throughout
> and should continue to.
>
> The pointer was verified to RESOLVE rather than merely to read correctly:
> `check_runner_label_coverage.py --refresh` queried
> `repos/ipswitch8/PDFE/actions/runners` successfully, and its `fetch_live()`
> raises `SystemExit` on a bad repo name instead of returning an empty document.
>
> **Still outstanding, and unchanged by the rename:** no self-hosted runner is
> registered against the new repository. Registration is repo-scoped, so the
> re-registration described below is still required, and until it happens every
> self-hosted job is unschedulable and `fast-checks` stays red on every push.

## What was repointed, and what deliberately was not

**Operational** references moved — the runner registration URL, the guard's
`REPO` constant, `Bootstrap-PdfeRunner.ps1`'s fetch target, clone instructions,
the security-advisory link, the release badge. Eleven files.

**Historical** references did not. Every citation under `docs/audit/`,
`tests/docs/three_question_gate.md` and the `tools/gate/fixtures/run-*.json`
captures records something that happened *at `ipswitch8/PDFE`*, and the run ids
they cite do not exist on the new repository. Rewriting them would falsify the
record to make a link resolve. A dead link that is honest beats a live link that
is wrong.

## THE RUNNER IS NOT REGISTERED YET — CI cannot run until it is

A runner registration is repo-scoped. `pdfe-runner-01` is still bound to the old
repository; the new one has **zero** runners, so every `self-hosted` job is
currently unschedulable.

`check_runner_label_coverage.py` **fails closed** on this and says so:

```
no runners in the inventory at all -- refusing to report coverage
against an empty fleet.
```

That red is **correct** and self-clears the moment the runner is registered and
the capture refreshed. It is not a defect to work around.

### Re-registering

Done by hand, deliberately. The runner **must** run in an interactive desktop
session — session 0 has no visible desktop and every UIA-driven UI test blinds
or hangs there. Starting it from an automation shell risks landing it in the
wrong session, which fails in a way that looks like a product bug.

A registration token was minted and written to
`C:\pdfe-history-backup\runner-registration-token.txt` (**not committed**, and
it expires about an hour after minting — regenerate with the command below if
stale).

```powershell
# 0. Stop the current listener. It is `run.cmd` in a window, not a service.
#    Confirm it is not mid-job first: busy=false on the runners API.

# 1. Unregister from the OLD repo (skip if you have already deleted it —
#    deleting the repo unregisters the runner for you).
cd C:\Users\<account>\actions-runner
$rm = gh api -X POST repos/ipswitch8/PDFE/actions/runners/remove-token --jq .token
.\config.cmd remove --token $rm

# 2. Register against the NEW repo. Keep BOTH labels: pdfe-builder is what
#    routes the build and release jobs, and dropping pdfe-windows would make
#    every other job unschedulable.
$reg = gh api -X POST repos/ipswitch8/PDFE-scrubbed/actions/runners/registration-token --jq .token
.\config.cmd --url https://github.com/ipswitch8/PDFE-scrubbed `
             --token $reg `
             --labels self-hosted,Windows,X64,pdfe-windows,pdfe-builder `
             --unattended --replace

# 3. Start under the EXISTING watchdog, from an interactive desktop session:
<repo>\tools\ops\runner_watchdog.ps1

# 4. Refresh the capture so the guard goes green on real evidence:
python tools\gate\check_runner_label_coverage.py --refresh --live
```

Answer **no** to "run as service" if prompted — see the session-0 note above.

## The deploy pin went red twice, for two different reasons

Separated here because the phase-5 `validator` gate went looking for this and
found only half of it recorded — the docs covered the rewrite and were silent on
the migration.

**Across the history rewrite it did NOT go red.** `Bootstrap-PdfeRunner.ps1`'s
`$Ref` is itself a commit SHA, so the commit-map repair remapped it along with
every other citation and it stayed valid. Recorded in
`docs/audit/history-scrub-2026-08-22.md`.

**After the migration it DID.** Repointing the operational references changed
`Install-PdfeRunner.ps1` and the runner `README.md` — both files the bootstrap
fetches — leaving the pin naming a commit that predates them. A machine
bootstrapped from it would have received scripts still pointing at
`ipswitch8/PDFE`, a repository being deleted, and said nothing about it.

`check_runner_deploy_pin.py` caught it; fixed by the inherent follow-up commit
(`e229b1f`, pinning `5e096be`). A commit cannot contain its own SHA, so the
two-step is structural, and the guard being red in between is correct rather
than a nuisance — during that window the bootstrap really would have deployed
stale scripts.

## Other consequences

**The local checkout's `origin` already points at the new repo.** Any other
clone — including the runner's `_work` checkout — must be repointed or deleted;
its history no longer shares ancestry with the new remote.

**`C:\pdfe-build-store\` is orphaned.** Every directory is named for a
pre-rewrite commit and nothing will match again. Safe to delete; the next run
repopulates. If a pre-rewrite crash dump still matters, copy its store out
first — that store holds the only PDB that can symbolise it.

**The ONNX asset id changed** (`440223541` → `525529818`) and this no longer
matters. `windows-validation.yml` now resolves `font_embedder.onnx` **by name**
from the `assets-v1` release rather than by a hard-coded id, precisely because
an id does not survive a re-upload. Verified against the new repo: exactly one
match, and its SHA-256 equals the value the workflow asserts.

**The artifact storage quota is a property of the account, not the repo**, so
recreating the repository does not reset it. It matters much less now — build
transport moved to Cloudflare R2 (`docs/ci/r2-build-store.md`) and crash dumps
were split to a 3-day artifact, taking live usage from 964 MB to ~53 MB.

## Backups

Both pre-rewrite backups remain and are the only copies of the original history:

```
C:\pdfe-history-backup\pdfe-pre-scrub-9b2fcd8.bundle   (git bundle verify clean)
C:\pdfe-history-backup\pdfe-pre-scrub.git              (mirror, HEAD 9b2fcd8)
C:\pdfe-history-backup\release-assets\                 (3 assets, SHA-verified)
```

Keep them until the new repository has completed a full green CI run. Deleting
the old repository does not affect them.
