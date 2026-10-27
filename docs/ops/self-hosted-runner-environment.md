# Self-hosted runner environment — account-name containment

**Operational reference, actively maintained.** This is not a historical audit
record: it describes the runner's current configuration and the reasoning
behind it. If the runner is rebuilt or re-registered, this is what to reapply.

Companion to [`docs/legal/github-gc-request-2026-10.md`](../legal/github-gc-request-2026-10.md)
and GitHub #200.

---

## The problem

Actions run logs are an artifact of the repository. When it goes public they
become readable, and they had been recording the build machine's account name.

Measured 2026-10-18 across **all 290** workflow runs in the repository:

| | count |
|---|---|
| runs whose log carried the account name | **12** |
| runs whose log carried any `C:\Users\…` path | **18** |
| runs mentioning profile environment variables | 4 |
| runs whose log carried the internal email domain | **0** |

### Not 283

The pipeline's phase description says "the 283 identified Actions run logs".
That number is **not reproducible** against the current API and is not what was
acted on. The figure acted on is the measured one — 20 runs targeted, being the
union of the account-name set and the `C:\Users\` set. A remembered count is
not evidence; the scan is, and its raw output is under `build/p98/phase4/`.

## Four producers, only two of them still live

The phase description attributes the leak to the runner "inheriting the
interactive user's PATH". That is one producer of four, and it accounted for a
minority of the affected runs.

| # | producer | status |
|---|---|---|
| 1 | `actions/checkout` logging `Copying 'C:\Users\<ACCOUNT>\.gitconfig' to …`, resolved from `HOME`/`USERPROFILE` | **was live** — fixed here |
| 2 | the MSVC dev-shell step printing `PATH` and `__VSCMD_PREINIT_PATH`, which contained profile directories | **was live** — fixed here |
| 3 | `TEMP` resolving under the profile | already fixed 2026-10-05 (same `.env`) |
| 4 | **our own hygiene guard printing the account name it had just found** | already fixed 2026-09-08, commit `107ad236` |

Producer 4 is worth keeping in mind: `check_tracked_path_hygiene.py` existed to
stop the account name reaching tracked files, and was printing it into a CI log
that is itself a published artifact. A guard can be the leak.

## The fix

`C:\actions-runner\.env`, which the runner reads **at startup only** — a
restart is required for any change to apply:

```
HOME=C:\runner-profile
USERPROFILE=C:\runner-profile
PATH=<the machine+user PATH, with every profile-rooted entry rerouted>
TMP=C:\runner-temp          # from 2026-10-05
TEMP=C:\runner-temp         # from 2026-10-05
```

`C:\runner-profile` is a **directory junction** over the profile root:

```
mklink /J C:\runner-profile "C:\Users\<ACCOUNT>"
```

### Why a junction over the whole profile, rather than editing PATH entries

The first attempt rewrote the two obviously-offending `PATH` entries and
dropped the rest. Its dry run showed that would have **silently removed 13
entries**, including OpenCppCoverage (coverage jobs), the `gh` CLI,
`.cargo\bin` and `.dotnet\tools`. Dropping tools to hide a string is the wrong
trade.

Routing every profile-rooted entry through one junction changes **no
behaviour** — same files, same resolution order, 17 entries rerouted and none
dropped — while every path string printed into a log is neutral.

### Rollback

Each change backs up the previous file as `.env.bak-<timestamp>` beside it.
Restore it and restart the runner. The junction is additive and harmless to
leave in place.

## Proof it works, and why it is not vacuous

A fresh dispatch of `dependency-audit.yml` (self-hosted) after the restart —
run `39202095541`, **conclusion success**, so the environment change did not
break the job:

| | |
|---|---|
| account-name occurrences in the log | **0** |
| internal-domain occurrences | **0** |
| `C:\Users\…` paths | **0** |

The non-vacuity evidence is that the **same line still appears**, carrying the
same information, with only the identity gone:

```
Checkout   Copying 'C:\runner-profile\.gitconfig' to 'C:\actions-runner\_work\_temp\…'
```

A log that merely stopped mentioning paths would prove nothing.

> `ci-liveness.yml` is listed in the phase criteria as an acceptable workflow
> for this proof, but it runs on `ubuntu-latest` and therefore cannot
> demonstrate anything about this runner. `dependency-audit.yml` was used
> instead.

## Deleting the historical logs

Logs for the 20 targeted runs were deleted with
`DELETE /repos/ipswitch8/PDFE/actions/runs/{id}/logs` — **logs only, not the
runs**. The run records, their conclusions and their `head_sha` values survive,
which matters because phase-3's orphan evidence is built from those SHAs.

### A verification that was wrong the first time

The first check used `gh run view --log` and reported all 20 logs still present
and 12 still leaking. **`gh` caches downloaded run logs on disk**, so that
check was reading back the very bytes the pre-deletion scan had downloaded.
Verifying a deletion with the tool that cached the data proves nothing.

Re-verified against the REST endpoints directly:

| | |
|---|---|
| `GET /actions/runs/{id}/logs` → 404 | **20 / 20** |
| job logs still carrying the account name | **0** |
| control: an untargeted run's archive | **200** |

The control is the part that makes the 404s mean something — without it, "all
404" is equally consistent with the endpoint being broken.

## What this does NOT do

Deleting run logs narrows one discovery path for the orphaned pre-rewrite
commit SHAs, because `head_sha` values can be read from the Actions API. It
does **not**:

* collect the orphaned objects — 251 of them remain served by SHA, 28 still
  exposing the removed identity. Only GitHub can collect them, which is what
  the Support request asks for; and
* undo discovery that has already happened.

**Phase-4 does not close the publication exposure.** Only GitHub confirming
collection does. See the GC request and `tools/gate/check_publication_block.py`.
