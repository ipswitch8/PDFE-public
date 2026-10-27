# pdfe-windows runner deployment package

Everything needed to turn a clean Windows x64 machine into a runner that can
serve `runs-on: [self-hosted, Windows, X64, pdfe-windows]`.

## Why this exists

The `windows-validation` workflow's UI E2E sweep needs roughly **19 hours**
against a **720-minute** job cap: 122 UI test files at a measured 9.23 min/file
(609 minutes completed 66 of 126 files on run 31359621883). Fitting that budget
means running the sweep across **more than one machine**, because it cannot be
parallelised on one: a stray or concurrent `pdfe.exe` holds a **machine-global
single-instance mutex**, so two UI shards on the same host wedge each other.

More machines is therefore infrastructure work, and this package is that work
made repeatable.

## Contents

| File | Purpose |
|---|---|
| `Bootstrap-PdfeRunner.ps1` | **The one file you copy to a new machine.** Fetches the rest from GitHub at a pinned commit. |
| `runner-requirements.json` | **Single source of truth** for what a runner needs. Both scripts read it, so a dependency cannot be installed-but-unverified or verified-but-uninstalled. |
| `Install-PdfeRunner.ps1` | Installs what is missing. Reports what it *tried* to do. |
| `Test-PdfeRunner.ps1` | Independently verifies readiness. **This is the authority**, not the installer. |
| `Test-InstallerUrls.ps1` | Downloads every pinned installer and checks it *is* the packaging family its silent args assume. Run after bumping any version. |
| `Set-R2Config.ps1` | Validates R2 credentials with a real PUT/GET/DELETE against the bucket, then sets the four `PDFE_R2_*` variables at machine scope. Sets **nothing** unless validation passes, so a failed run leaves the machine unchanged rather than half-configured. Knows that read-only is *correct* on a consume machine. |
| `smoke_uia.py` | Proves pywinauto can actually see and drive a real window — the failure the dependency check cannot catch. |
| *(watchdog)* | **Not in this package.** Use the existing `tools/ops/runner_watchdog.ps1` — see below. |

## Getting the package onto a new machine

**The repository is PRIVATE.** That rules out the usual one-liner: an
unauthenticated `iwr .../raw.githubusercontent.com/... | iex` returns 404, and
piping a 404 body into the shell fails confusingly rather than obviously.

Instead, copy **one** file to the new machine -- `Bootstrap-PdfeRunner.ps1` --
and run it. It authenticates (a logged-in `gh`, else `$env:GITHUB_TOKEN`),
fetches the rest, prints a SHA-256 per file, and writes a
`deployed-manifest.json` recording exactly what landed. It refuses to proceed on
a partial fetch, and stops with instructions rather than silently attempting an
unauthenticated download.

```powershell
gh auth login                                 # or set $env:GITHUB_TOKEN
.\Bootstrap-PdfeRunner.ps1                    # -> C:\pdfe-runner-deploy
gh repo clone ipswitch8/PDFE C:\src\PDFE      # for the pinned pip requirements
```

If you have no `gh` and no token, the token route is the quicker one — it needs
nothing installed. If you do want `gh`, **do not reach for winget** (see below);
download the MSI directly, then `gh auth login`. In a Claude Code session you can
run that interactive login yourself by prefixing it with `!`.

## There is no winget on these machines

This is worth stating up front because it is the first thing a Windows install
script normally reaches for, and here it silently cannot work.

The runner is **Windows Server 2019**, which has no Microsoft Store. `winget`
ships as the App Installer *Store* package, so it is absent — and it cannot be
added: on the live host the `Appx` module does not even load (`Operation is not
supported on this platform`). winget was never a fallback away from working on
this class of machine; it was unreachable by construction.

Every installable requirement in `runner-requirements.json` is therefore
`kind: "download"` — a pinned URL plus **explicit** `silent_args`. One code path,
which runs identically here and on a Windows 11 box that does have winget. A
winget-first-with-fallback design would have meant the fallback — the only path
that ever executes on the actual target — was the least exercised one.

Silent arguments are *data* rather than inferred from the file extension,
because the inference this replaced was wrong: it assumed NSIS `/S` for every
`.exe`, but Git for Windows is **Inno Setup**, which ignores `/S` and would sit
at an interactive prompt forever on a headless deploy. The installer also now
accepts exit codes **3010/1641** ("installed, reboot required"), which it
previously treated as failure — that alone would have failed a correct Visual
Studio install.

One consequence worth knowing: pinned URLs go stale. Git, Python, gh,
OpenCppCoverage and WinAppDriver are pinned to specific versions and will need
bumping eventually; they fail *visibly* on a 404 rather than installing
something unexpected. The Visual Studio bootstrapper (`aka.ms/vs/17/release`) is
the one deliberate exception — Microsoft publishes no stable per-version URL.

### `Test-InstallerUrls.ps1`, and the bug it found on its first run

Writing the above by hand would have been a guess, so it is checked. The script
downloads all six pinned installers and identifies each one's packaging family
from its bytes and version resource (`inno` / `nsis` / `msi` / `burn` /
`vsboot`), then asserts the recorded `silent_args` are the ones that family
actually obeys. An **unclassifiable** installer is a failure, not a pass — a
checker that shrugs at the case it cannot classify would report green on exactly
the situation it exists to catch.

Its first run failed, and on something that predates the winget change:

> **OpenCppCoverage is Inno Setup, not NSIS.** The requirements file asserted
> NSIS and passed `/S` — and so, more importantly, did the workflow's own
> installer step (`windows-validation.yml`, added under phase-24 criterion 3).
> Inno **ignores** unrecognised switches and falls back to its **interactive
> wizard**. Under `Start-Process -Wait` on a headless runner that does not fail
> — it hangs, until the 720-minute job cap, reported as `cancelled`, alerting
> nobody. A silent-install bug is invisible precisely where it costs most.

Both call sites are fixed, and the workflow's now has a 10-minute timeout so any
future hang there is bounded and visible rather than eating the job cap.

The checker is itself mutation-tested by `Test-InstallerUrls.Mutations.ps1`,
which works on a copy in `%TEMP%` and leaves the real tree untouched: control
green, and 3/3 mutations killed — NSIS args on an Inno installer, a URL
re-pointed at differently-packaged bytes, and a dead URL. M1 is the historical
defect above, so the checker is known to redden on the bug it was written for
rather than merely being present.

**It pins a commit, not a branch.** A branch moves; deploying two runners a week
apart from `master` can silently give them different tooling, and "works on
runner A, fails on runner B" is expensive to chase. `-Ref master` takes the tip
deliberately.

It downloads and then stops -- fetching and executing are separate decisions, so
you can read what arrived. `-Run` chains into the installer if you prefer.

## The short version

```powershell
# From an ELEVATED, INTERACTIVE PowerShell on the new machine:
.\Deploy-PdfeRunner.ps1 -RepoRoot C:\src\PDFE -RunnerDir C:\actions-runner
```

That is the whole deployment. It inspects the machine, installs what is
missing, authenticates, clones the checkout, verifies, proves the UIA stack
works, registers the runner, arranges autostart and starts it — prompting
only for the things a script cannot know: GitHub auth, the registration
token, optional R2 credentials, and whether to change how the machine
behaves at logon.

Run it with `-WhatIf` first to see every decision without changing anything;
that works unelevated. Re-running it is safe — every step detects work
already done — and it ends by listing whatever is still outstanding rather
than claiming success.

**It breaks a dependency cycle the manual ordering below does not mention.**
`python-ui-deps` installs from a requirements file inside the checkout, but
`gh` — how you clone a private repo — is itself something the installer
provides. So it installs in two passes, with authentication and the clone in
between. Done by hand, you discover that ordering by hitting it.

## Order of operations (what the wizard does, and the manual fallback)

```powershell
# 0. Confirm the pinned installers still resolve and still match their silent
#    args. Cheap, and it fails HERE rather than mid-install:
.\Test-InstallerUrls.ps1

# 1. From an ELEVATED PowerShell, in an INTERACTIVE desktop session
#    (not RDP-disconnected, not a service):
.\Install-PdfeRunner.ps1 -RepoRoot C:\src\PDFE -VcpkgRoot C:\vcpkg

# 2. Verify. The installer's own report is not evidence.
.\Test-PdfeRunner.ps1 -RepoRoot C:\src\PDFE -RunnerDir C:\actions-runner

# 3. Prove the UIA stack actually works (5 seconds, no build needed):
python smoke_uia.py

# 4 + 5. Register and start. Prompts for the token, tells you exactly where
#        to get one, checks everything checkable BEFORE asking for it, and
#        starts the runner under the watchdog rather than bare run.cmd:
.\Register-PdfeRunner.ps1 -RunnerDir C:\actions-runner -Start
```

Steps 4 and 5 used to be prose here: a URL to find, a five-label `config.cmd`
line to retype, and a warning that bare `run.cmd` is the wrong way to start
it. `Register-PdfeRunner.ps1` does all three, and its label set is checked
against this file by `tools/gate/check_label_drift.py` so the automated path
cannot drift from the documented one.

It still cannot invent a credential, so it asks for one — everything around
that is automated. Run it with `-WhatIf` first to see every decision without
registering anything.

Steps 2 and 3 are not optional formalities. Every failure mode this package
guards against is one that stays invisible until a job has already burned an
hour.

## The one decision you have to understand: service vs interactive

**Do not install the runner as a Windows service.** Two real constraints
collide here and you should know which way this package resolves them:

- A **service** survives reboots and restarts on failure — which fixes the
  known problem that `run.cmd` dies silently, leaving jobs to queue and then
  report `cancelled`, alerting nobody.
- A service also runs in **session 0, with no visible desktop**. Every
  UIA-driven UI test needs a real desktop. In session 0 they do not fail
  cleanly; they blind, hang, or self-skip, which looks like a product bug.

Because the UI suite is the *reason* for adding runners at all, this package
chooses **interactive session + autologon + the existing watchdog**.

**Use `tools/ops/runner_watchdog.ps1`. Do not write a new one.** This package
originally shipped its own `Watch-PdfeRunner.ps1`; it was deleted before commit
on discovering that follow-up F44/F46 had already solved this, and solved it
better. The existing watchdog emits a **heartbeat**, because the defect in its
predecessor was that silence was ambiguous between "healthy, nothing to report"
and "dead" — and that ambiguity cost roughly two weeks of CI (2026-07-25 to
2026-08-07) with no signal. `tools/gate/test_runner_watchdog_heartbeat.py` pins
that behaviour in fast-checks. A second watchdog would have been a second
spelling of the same rule, which is the hazard DCA 0036 describes.

`tools/ops/register_runner_service.ps1` is the companion autostart helper. Note
it is DRY-RUN by default and requires `-Apply`, deliberately: choosing an
autostart mechanism changes how the operator's machine behaves at logon.

**Autologon is NOT available on these machines**, by policy. That is a
constraint on the whole availability story, not a detail:

1. after a reboot the box sits at the sign-in screen with no desktop;
2. a human signs in;
3. the **logon task** starts the runner in that session automatically — its
   trigger is the interactive logon, so it needs no autologon;
4. if the task did not fire, double-click **Start PDFE Runner** on the
   Desktop (created by `Start-PdfeRunner.ps1 -CreateShortcut`, and by the
   wizard).

Until someone signs in there is no desktop, so the UI lanes have nothing to
drive. No on-host mechanism can change that — the off-host alarm
(`ci-liveness.yml`) is what covers a machine nobody has logged into.

### If your RDP session drops while the runner is up

The session survives a disconnect; its **input desktop does not**. Nothing
about that is visible to the runner — it stays online and takes the next job.

**Measured on this hardware, 2026-09-03/04** (`docs/audit/issue-132/`,
`docs/audit/issue-133/`), because this used to be reasoned rather than known:

| | at disconnect | at reconnect |
|---|---|---|
| `OpenInputDesktop` | fails ~1 s after the drop, with `ERROR_ACCESS_DENIED` — the desktop still exists, this session may no longer touch it | recovers **11 s AFTER** the session reports `Active` |
| `pyautogui.screenshot()` (GDI screen grab) | raises `OSError: screen grab failed` ~2.2 s after the drop | recovers **13.5 s AFTER** the reconnect event |
| Win32 `PrintWindow` (window renders itself) | **keeps working throughout** | n/a |
| `OpenDesktop("Default")` | **keeps working throughout** — the session is alive; only the *input* desktop went | n/a |

Two consequences worth internalising:

- **Never gate a resume or a retry on the reconnect event.** Both capabilities
  above recover *after* Windows reports the session `Active` — 11 s and 13.5 s
  respectively. The watchdog waits on the probe, not the event, which is why
  it does not restart into a session that still cannot be driven.
- **Not every test breaks.** The `test_ui_*` families capture via `PrintWindow`
  and run through a disconnect unaffected; runs overlapping a real outage
  passed with genuine change detection. What breaks is the screen-grab path
  (`tests/ui_harness/core/screenshot.py`) and anything needing input delivery.

Since 2026-09 the watchdog detects this and **pauses**:

- it probes `OpenInputDesktop` every cycle — the same call `smoke_uia.py`
  uses, deliberately, so two definitions of “is there a desktop” cannot drift
  apart;
- with no desktop it stops the listener, so the runner goes **Offline** and
  queued jobs **wait** instead of running blind. The work is deferred, not
  failed;
- it will **not** stop a listener while `Runner.Worker` exists, i.e. while a
  job is actually running. Aborting a live job is a certain loss; letting an
  already-started job finish desktop-less is a loss that has already
  happened. **But it journals the outage window** (machine-wide JSONL, default
  `%ProgramData%\PDFE\desktop-outages.jsonl`), and the pytest session
  reclassifies any failure overlapping that window from `red` to `blocked`
  with the reason attached — so a mid-run drop stops producing failures that
  read as product defects. The test still fails; it stops lying about why;
- it does not restart into a desktop-less session, and resumes automatically
  when you reconnect;
- the heartbeat reports `paused-no-desktop`, which is a **third state**, not a
  flavour of `down`. “Nobody is serving jobs because the desktop went away”
  and “the listener crashed” need opposite responses.

`-IgnoreDesktopState` restores the pre-2026-09 behaviour, for a runner that
serves only non-UI lanes where a missing desktop is harmless and pausing
would be pure lost capacity.

**What this does not do.** It cannot keep the desktop alive. If you need the
machine to keep working while you are disconnected, redirect the session to
the physical console before you go (`tscon <id> /dest:console`) — that leaves
an unlocked desktop on the console, which is the same security trade-off
autologon was rejected for, so it is your call and the watchdog will not make
it for you.

Related gap: **WIN-74** (CONFIRMED, High) — no test drives a session
transition.

> **Correction, 2026-09-04.** This paragraph used to argue that WIN-74's three
> RDP product workarounds made the transition "a known live defect source
> rather than a hypothetical". They are real, but they concern quirks of an
> **active** RDP session, not a disconnect, so citing them borrowed their
> CONFIRMED status for a claim they do not support. The table above is the
> evidence now; WIN-74 never was. The same overstatement was corrected in
> `tools/ops/runner_watchdog.ps1`, and this copy was missed at the time.

**Do not lock the session** while the runner is up — a locked workstation
hides the desktop from UIA, so the UI lanes fail while the runner reports
online, which reads as a product bug rather than a host one. `smoke_uia.py`
probes for exactly that (`OpenInputDesktop`).

## The runner needs a neutral TEMP, or artifacts carry the account name

**Create `<runner-root>\.env` before first start:**

```
TMP=C:\runner-temp
TEMP=C:\runner-temp
```

…and create that directory. The runner reads `.env` at **startup only**, so an
existing runner must be restarted for it to apply.

This is not hygiene theatre. Moving the runner workspace off the user profile
(2026-10-02) stopped *build output* embedding the account name, but TEMP still
resolved to `C:\Users\<account>\AppData\Local\Temp\2`. Anything that creates a
working directory there and then names it in its own output — pytest prints test
paths relative to its rootdir, so a file under TEMP comes out as
`..\..\Users\<account>\AppData\...` — writes the account name into a committed
artifact.

Measured 2026-10-05: `UiSuitePathHygiene.ARealRunnerInvocationEmitsNothingHost
Rooted` failed in CI for exactly this, on the first run that ever reached
`core-tests`. The text scrubber in `tests/scripts/run_ui_suite.py` was fixed in
the same change (its path rules anchored on drive letters and never saw a
climbing relative path), but redaction is the second line of defence: a path
shape can be missed again, whereas a neutral TEMP removes the category.

Verify after restarting the runner:

```powershell
# inside a job, or in a shell with the runner's environment
$env:TEMP   # must NOT be under C:\Users
python tools/gate/f48_producer_probe.py   # must exit 0
```

## Registering the runner

`Install-PdfeRunner.ps1` deliberately does not do this: it needs a short-lived
registration token that must be fetched by a human, and scripting that would
mean handling credentials this package has no business holding.

1. <https://github.com/ipswitch8/PDFE/settings/actions/runners/new>
2. Download the runner into e.g. `C:\actions-runner`
3. Configure with the labels the workflows expect:

<!-- PDFE-DEFAULT-LABELS -- the canonical default registration command for this
     package. tools/gate/check_label_drift.py reads the label set from the NEXT
     `--labels` after this marker, and fails if this file carries more than one
     marker. Other `--labels` strings below (the Nth-runner walkthrough, any
     narrower consume-role example) are deliberately UNMARKED and ignored. -->

```powershell
.\config.cmd --url https://github.com/ipswitch8/PDFE `
             --token <TOKEN> `
             --labels self-hosted,Windows,X64,pdfe-windows,pdfe-builder `
             --unattended
```

> **`pdfe-builder` belongs in that list, and this is the default.** It used to be
> shown here as an optional extra, which was wrong: `windows-validation`'s `build`
> and `release` jobs are the only ones that ask for it, and GitHub does not error
> on a `runs-on` no runner satisfies — the job **queues, then reports
> `cancelled`**, a status that alerts nobody. A machine registered without it
> looks healthy and silently cannot build.
>
> Drop `pdfe-builder` **only** on a multi-machine fleet where you can name the
> other registered machine that carries it — see `profiles.consume` in
> `runner-requirements.json`. On a machine that is on its own, the narrower label
> set is never correct.
>
> **What this consolidates, and the risk that comes with it.** Adding
> `pdfe-builder` is what makes `build` and `release` actually land on this host —
> before it, they could not be scheduled here at all. So on a solo machine those
> two now run, **in serial**, on the same host and workspace root as `fuzz`,
> `windows-sanitizer`, `mutation-kill` and `governance`, which execute
> attacker-shaped PDF input under instrumentation. Release *signing* credentials
> are NOT exposed by this — `scripts/sign/Sign-Artifacts.ps1` is manual-only and
> is invoked by no workflow — and `build` checks out with the default
> `clean: true`, so the producing lane wipes gitignored leftovers each run. The
> residual is disk contention and non-repo-tree state (`%TEMP%`, the per-SHA
> store, orphaned processes). Full statement, including why the machine-scoped
> R2 credential is accepted: `_consolidation_risk` in `runner-requirements.json`.
>
> The tree used to contradict itself here:
> [`docs/ci/repo-migration-2026-08-22.md`](../../docs/ci/repo-migration-2026-08-22.md)
> had it right and explained why, while this package — the *automated* path —
> had it wrong. **That doc is the source of truth these files were brought into
> agreement with**, not the other way round. When two places must agree, the one
> a machine follows is the one that matters.

Answer **no** to "run as service".

If you prefer to run that interactively in this session, prefix it with `!` in
the Claude Code prompt so its output lands in the conversation.

## What the workflows actually need, and why

Full detail with per-item rationale is in `runner-requirements.json`. The
non-obvious ones:

- **OpenCppCoverage** — `windows-validation.yml` treats its absence as a **hard
  error** (R-32), not a skip. The workflow's own fallback tries Chocolatey,
  which is broken on the existing runner; this package installs from the GitHub
  release instead. That broken-choco path is precisely what phase-24's
  criterion 3 exists to fix.
- **WinAppDriver ≥ 1.2.1** — the 2016 v0 beta answers `/status` but cannot
  create sessions, so a too-old install is *worse* than none: it looks present.
- **Developer Mode** — WinAppDriver will not start sessions without it.
- **Long paths** — vcpkg buildtrees plus Qt's header depth exceed `MAX_PATH`.
  The symptom is a confusing mid-build file-not-found, not an obvious path error.
- **Visual Studio 2022 *Community*** — `CMakePresets.json` hard-codes the
  VS-bundled Ninja under the **Community** path. Professional or Enterprise
  will install fine and then fail to configure. If you use another edition, the
  preset needs updating; the verifier accepts any edition and this README is
  where that discrepancy is recorded rather than left to be discovered.

## After the runner is live

Sharding the UI sweep across N runners is a **separate change to
`windows-validation.yml`** and is not part of this package. The step already
accepts an `--only` filter (`inputs.ui_only`), which is the natural seam for a
matrix strategy. The arithmetic to size it:

```
122 files x 9.23 min/file = 1126 min
720 min cap - ~100 min of build/test steps = ~600 min usable per job
1126 / 600 = 1.88  ->  2 runners minimum, 3 for headroom
```

Two runners leaves almost no margin for a slow file or a rerun; three is the
number to aim for.

## First run against the existing host (2026-08-19)

`Test-PdfeRunner.ps1` was run against the current runner machine
(Windows Server 2019, runner at `C:\Users\dev\actions-runner`). Result:
**13 passed, 3 failed.** The three failures are real, and one of them explains a
known CI symptom:

| Item | Finding |
|---|---|
| **OpenCppCoverage** | **Not installed.** This is phase-24 criterion 3 confirmed on the real host - the coverage step treats absence as a hard error (R-32), so every `windows-validation` run fails there, and the workflow's choco fallback does not rescue it. |
| **VCPKG_ROOT** | Not set as a machine/user environment variable. vcpkg *is* present at `C:\Users\dev\source\vcpkg`; the workflow supplies its own via `lukka/run-vcpkg`, so this is a local-build inconvenience rather than a CI break - but the verifier reports it rather than assuming. |
| **Free disk** | **42.7 GB free, against the 120 GB this package asks for.** Worth acting on: vcpkg buildtrees plus two build configs will not comfortably fit, and disk exhaustion on this project has previously presented as an unrelated failure. |

### The bootstrap was tested end-to-end, and that found a corruption bug

`Bootstrap-PdfeRunner.ps1` was run against the private repo: all five files
fetched, byte-identical to the working tree, and the FETCHED copy of the
verifier then ran correctly.

That is the *second* attempt. The first used `gh api ... > $OutFile`, which
routes the response through PowerShell's **text** redirection operator. Under
Windows PowerShell 5.1 that writes **UTF-16LE**, and the files came back at
exactly twice their real size -- corrupted, not merely reformatted. It was easy
to miss because a naive check still said "content matches": .NET's
`ReadAllText` transparently honours the BOM, and `ConvertFrom-Json` would have
parsed it happily. `python smoke_uia.py` would not have. The bytes are now
downloaded verbatim via `Invoke-WebRequest -OutFile`, and it was the
byte-for-byte hash comparison -- not the content comparison -- that caught it.

Running the verifier also found two bugs **in the verifier itself**, both fixed:
a PowerShell format-string precedence error (`-f` binds tighter than `+`), and a
`Set-StrictMode` null-dereference that reported "check threw" instead of the
honest "VCPKG_ROOT is not set". It also failed a working machine over an
**invented** git floor of 2.40 - corrected to the 2.18 that `actions/checkout`
actually documents. An invented requirement is as bad as a missing one.

## Adding the Nth runner (N = 2, 3, 4 — the sequence is identical)

Nothing in this package, in any workflow, or in any guard hardcodes a runner
count. Shard counts are a property of the WORK (the UI sweep is 3 shards because
the suite is ~548 minutes), not of the machine count, so 2, 3 and 4 machines all
work with no workflow edit. What differs per machine is one thing: its ROLE.

### EXISTING runners went from "ready" to "not ready" — read this first

Adding the `aws-cli` requirement changed the verdict for machines that were
already registered and working. This is deliberate, but it is a *silent*
behaviour change unless it is written down, so:

**Any runner provisioned before this will now FAIL `Test-PdfeRunner.ps1`** on
`aws-cli` (error severity, both profiles) and report WARN or FAIL on
`r2-config`. Nothing about that machine got worse — the check got more honest.
Without the AWS CLI it cannot reach the R2 build store, which is now the primary
transport, so "ready" was the wrong answer before.

`r2-config` severity is per profile, and the asymmetry is the point:

| | `build` | `consume` |
|---|---|---|
| No R2 configured | **WARN** — still compiles, still publishes to its own local store, so it degrades to single-machine behaviour | **FAIL** — cannot obtain binaries at all |

To bring an existing runner up:

```powershell
.\Install-PdfeRunner.ps1 -RepoRoot <checkout> -RunnerProfile build   # installs aws-cli
copy .env.example .env    # fill in, then:
.\Set-R2Config.ps1 -RunnerProfile build
# restart the runner — it reads its environment at PROCESS START
.\Test-PdfeRunner.ps1 -RepoRoot <checkout> -RunnerProfile build
```

A machine you deliberately want to leave on the old single-machine arrangement
can skip the CLI (`-Skip aws-cli`) and stay on `build`, where the missing R2
config is only a warning. It will build and test locally and publish nothing
other machines can use.

### Choose the role first

| | `build` | `consume` |
|---|---|---|
| Compiles pdfe | yes | no |
| Requirements | 15 | 12 |
| Free disk | **≥ 120 GB** | **≥ 15 GB** |
| Extra label | `pdfe-builder` | — |
| Gets which jobs | `build`, `release`, and everything else | tests only |

The 120 GB is not a steady-state figure — it exists for the **cold vcpkg
buildtrees peak**, which spikes into the tens of GB transiently. A consume-only
machine needs none of that: measured on the live host, Visual Studio is 12 GB,
the vcpkg repo ~2.7 GB and its caches 2.4 GB, and a consume-only box needs
none of the three. What remains is the ~2.3 GB restored runtime closure plus
Python, WinAppDriver, the runner and `gh` — about 5 GB. 15 GB is that with
headroom for a second commit's store and log churn.

**At least one machine must carry `build`.** The `build` and `release` jobs
declare `runs-on: [self-hosted, Windows, X64, pdfe-windows, pdfe-builder]` —
the role label is APPENDED to the base four, never substituted for one. With no
such runner online those two jobs queue forever and report `cancelled`, which
alerts nobody.

That distinction is not pedantry. This line previously read
`[self-hosted, Windows, X64, pdfe-builder]`, matching a version of the workflow
that dropped `pdfe-windows` — which the phase-2 `karen` gate failed, because
removing a base label WIDENS eligibility: a machine carrying only
`[self-hosted, Windows, X64, pdfe-builder]` would have matched. The workflow was
fixed; this line was not, and the `validator` gate caught the doc lagging the
code. `tools/gate/check_ensure_build_tree.py` rule 0 now fails any job whose
`runs-on` drops a base label.

### The sequence

**This is identical whether N is 2, 3 or 4.** Nothing below is per-machine
except the role you pick at step 2 and the labels that follow from it.

```powershell
# 0. GET THE PACKAGE ONTO THE MACHINE. Bootstrap-PdfeRunner.ps1 is the ONE file
#    you copy by hand; it fetches the rest of tools/runner-deployment/ at a
#    pinned commit. Needs a GitHub token in $env:GITHUB_TOKEN / $env:GH_TOKEN,
#    or an authenticated `gh` — the repo is PRIVATE, so anonymous fetch 404s.
.\Bootstrap-PdfeRunner.ps1 -Destination C:\pdfe-runner-deploy
cd C:\pdfe-runner-deploy

# 1. Confirm the pinned installers still resolve. Cheap, and it fails HERE
#    rather than 40 minutes into an install.
.\Test-InstallerUrls.ps1

# 2. CHOOSE THE ROLE, then install. -RunnerProfile selects which requirements
#    apply; it defaults to `build`, so an existing invocation is unchanged.
#    From an ELEVATED PowerShell, in an INTERACTIVE desktop session.
.\Install-PdfeRunner.ps1 -RepoRoot C:\src\PDFE -VcpkgRoot C:\vcpkg           # build
.\Install-PdfeRunner.ps1 -RepoRoot C:\src\PDFE -RunnerProfile consume        # consume

# 3. Verify. The installer's own report is not evidence, and the free-space
#    floor follows the profile — a consume box is judged against 15 GB, not 120.
.\Test-PdfeRunner.ps1 -RepoRoot C:\src\PDFE -RunnerDir C:\actions-runner -RunnerProfile consume

# 4. Prove the UIA stack works (5 seconds, no build needed):
python smoke_uia.py

# 4b. SET THE R2 VARIABLES (machine scope). NOT installable by the script --
#     credentials are yours, and a script that minted or stored them would be
#     holding something it has no business holding. Consume-only machines get a
#     READ-ONLY token. See docs/ci/r2-build-store.md.
#     Test-PdfeRunner reports these as WARN on a build machine (it can still
#     compile and publish locally) and FAIL on a consume-only one (it cannot
#     obtain binaries at all).
#     Use the helper rather than setting them by hand -- it does a real
#     PUT/GET/DELETE against the bucket first and sets NOTHING if that fails,
#     so a typo cannot become a job failure an hour later:
copy .env.example .env      # then fill in account id / bucket / access key id
notepad .env
.\Set-R2Config.ps1 -RunnerProfile build     # or: consume
#     .env is git-ignored, and the script REFUSES to read one that is not --
#     so a credential cannot end up one `git add` away from a commit. Leave the
#     secret blank in .env to be prompted for it securely instead.
#     Parameters still work and OVERRIDE the file:
#       .\Set-R2Config.ps1 -Bucket other-bucket -RunnerProfile consume
#     Then RESTART the runner -- it reads its environment at PROCESS START, so
#     anything set after run.cmd launched is invisible to every job it runs.

# 5. GET A REGISTRATION TOKEN. It is short-lived (~1 hour) and must be fetched
#    by a human — nothing in this package will do it for you, deliberately:
#      https://github.com/ipswitch8/PDFE/settings/actions/runners/new
#    Download the runner into e.g. C:\actions-runner, then from THERE:

# 6. Register, with the labels for the role. NOTE: -RunnerProfile is NOT
#    -Profile, because $Profile is a PowerShell automatic variable.
.\config.cmd --url https://github.com/ipswitch8/PDFE `
             --token <TOKEN> `
             --labels self-hosted,Windows,X64,pdfe-windows,pdfe-builder `
             --unattended
#    pdfe-builder is in the DEFAULT set deliberately. Remove it only on a
#    multi-machine fleet where another registered machine carries it; on a solo
#    machine, omitting it makes build+release queue then report `cancelled`.

# 7. Start under the EXISTING watchdog, not bare run.cmd:
<repo>\tools\ops\runner_watchdog.ps1

# 8. CONFIRM THE FLEET CAN ACTUALLY SCHEDULE EVERY JOB. Run this from a PDFE
#    checkout after the new machine is registered. A mistyped label does not
#    error — the job queues and is later reported `cancelled`, alerting nobody —
#    so this is the step that turns silence into a message.
python tools\gate\check_runner_label_coverage.py --refresh --live
```

Answer **no** to "run as service" — see the session-0 note above.

A label can also be ADDED to an already-registered runner with no
re-registration, which is how the live `pdfe-runner-01` gained `pdfe-builder`:

```powershell
gh api --method POST repos/ipswitch8/PDFE/actions/runners/<id>/labels -f "labels[]=pdfe-builder"
```

### How GitHub routes work — and the failure to expect

Purely by LABEL MATCHING. Nothing is "set up" in GitHub when a job is queued: a
job goes to any idle runner whose label set is a SUPERSET of its `runs-on` list.
So a machine registered with the four base labels is immediately eligible for
every test job, with no workflow change — and a typo in one label silently makes
the machine invisible while the job queues forever and then reports `cancelled`.
**Queued-but-never-picked-up is the failure mode to expect**, and it alerts
nobody (see `selfhosted_runner_ops`).

Consume-only machines deliberately LACK `pdfe-builder`, so a compile can never
land where it cannot run. That matters because such a job would not fail fast —
it would fail deep into a build, after burning an hour.

### How a runner that never built gets the binaries

Every test job now begins with `./.github/actions/ensure-build-tree`, which:

1. **probes the workspace** — the same-machine fast path `clean: false` provides;
2. **probes the local per-SHA store** — `C:\pdfe-build-store\<SHA>` (and
   `<SHA>-debug`, published by the `build` job for `core-tests`, which runs
   concurrently with `release` and so cannot wait for the full closure);
3. **downloads the published artifact** for this commit, constrained to an
   unexpired artifact from a non-fork run of THIS workflow file at THIS
   `head_sha`;
4. **fails loudly** naming which of five refusals applied — no marker, wrong
   commit, incomplete restore, nothing published, or nothing anywhere.

It is automatic, not opt-in: an opt-in restore is a restore that is off on the
run where it was needed. `tools/gate/check_ensure_build_tree.py` fails if a new
consuming job is added without it, if it becomes opt-in, if `required-files`
empties out (which would make the completeness check vacuous), or if the
download loses `run-id`/`github-token` — without which
`actions/download-artifact@v4` searches the CURRENT run only and the whole path
is silently INERT.

### THE REMAINING PRECONDITION — read before registering a consume-only machine

The artifact leg only functions when the producing run was dispatched with
**`publish_build_artifact`** set, and that input is OFF by default because this
account's **artifact storage quota is exhausted** (the reporting job's upload
fails every run with "Artifact storage quota has been hit"). Turning it on today
would redden every run.

So the honest state is: a cold machine now fails **loudly and explicably**
instead of failing obscurely on a missing exe, and the mechanism that will serve
it is built and guarded — but it is **not a working cross-machine lane until the
quota is freed**. Free the quota, dispatch with `publish_build_artifact`, and
confirm one green run before adding a consume-only machine. A second BUILD
machine has no such precondition.

One thing `ensure-build-tree` deliberately does not cover: `release` runs
`cmake --build`, which needs a CONFIGURED tree rather than a runtime closure. It
stays co-located with `build` via the `pdfe-builder` label. With more than one
build-capable machine that pairing would need its own treatment — so for now,
**one builder**.

## Honest limits of this package

- The **unelevated branches of `Install-PdfeRunner.ps1` are NOT exercised.** The
  one run that reached them raised a **UAC prompt on the operator's desktop**,
  which was accepted, so the installer took the ELEVATED path and reported `ok`
  truthfully — the portable fallback never executed. On a machine where nobody
  can accept that prompt (headless, or an account with no elevation route) the
  fallback is what runs, and it has only been verified *manually*: the payload
  was extracted with `innoextract`, placed under `%LOCALAPPDATA%\Programs`, and
  produced a real Cobertura report (`lines-valid=53399`). Treat the wiring
  through the installer as unproven until a genuinely unelevated deploy runs it.
- The **installer's data** is now verified — every pinned URL downloads and its
  silent args match its actual packaging family (`Test-InstallerUrls.ps1`, 6/6,
  with 3/3 mutants killed). That probe now RETRIES: it once reported vs2022 as
  a dead URL on a single TCP connect timeout to the `aka.ms` redirector, while
  the link was perfectly good. A one-shot network probe reporting a hard FAIL is
  a flaky gate, and a flaky gate gets ignored.
  The installer's **execution** still has not been run end-to-end on a clean
  machine; this session could not, being unelevated. So "the right bytes with
  the right switches" is evidence; "installs cleanly on a bare box" is not yet.
- **`Install-PdfeRunner.ps1` is substantially UNTESTED, and this is the biggest
  gap in the package.** Precisely: of its **eleven** installable requirements,
  exactly **one** (`opencppcoverage`) has ever been executed by the script — and
  that run took the *elevated* path, because the installer raised a UAC prompt
  on the operator's desktop which was accepted. So:
  - `git`, `vs2022`, `python`, `python-ui-deps`, `vcpkg`, `winappdriver`,
    `developer-mode`, `long-paths`, `actions-runner` — the dispatch for these
    has **never run**. Their URLs and silent args are verified
    (`Test-InstallerUrls.ps1`), but the code that acts on them is not.
  - `Install-Portable` has never executed *through the script*. The mechanism
    was verified by hand (innoextract → `%LOCALAPPDATA%\Programs` → a real
    Cobertura report), but its wiring is unproven.
  - The unelevated branches (`NEEDS ELEVATION — not attempted`, and
    "go straight to portable") have never been reached.

  **UPDATED — the dry-run seam now exists, and it closes part of this.**
  `-WhatIf` used to check `ShouldProcess` *before* the dispatch `switch`, so it
  skipped every branch and exercised nothing; that is fixed. The branches now
  run and resolve their arguments, with only the effectful calls gated, so a dry
  run **reaches all 11 requirements** and fails on a malformed one.
  `Test-InstallerDryRun.py` asserts that (10 checks) and requires six mutations
  of `runner-requirements.json` to redden it — each for the *specific* reason it
  should, because "exited non-zero" is satisfied just as well by an unrelated
  crash. It runs on every push, on a `windows-latest` job.

  **What that does and does not buy.** It proves every requirement's spec is
  well-formed and reachable by the dispatch: no missing url, no unknown kind, no
  malformed `portable_fallback`, no absent pip requirements file. It does **not**
  prove any of them installs. The count that matters is unchanged: **1 of 11 has
  ever actually been installed by this script.**
- The **verifier** has been run against a real machine; the **installer** has
  NOT been run end-to-end on a clean one. Every path, URL and registry key was
  derived from the workflow and from this repo's documented runner behaviour,
  but a first real deployment will find things - that is what
  `Test-PdfeRunner.ps1` and `smoke_uia.py` are for, and why they fail loudly
  rather than skipping.
- `smoke_uia.py` has not been run here either: it opens a Notepad window on the
  operator's desktop, which is the operator's call to make.
- `Bootstrap-PdfeRunner.ps1`'s **no-authentication** path is reasoned but
  untested: exercising it would mean logging `gh` out on this machine. The
  authenticated path is tested end-to-end.
- The OpenCppCoverage and WinAppDriver download URLs pin specific releases. If
  those assets move, the installer fails visibly rather than installing
  something unexpected.
- It does not provision the machine itself (OS install, domain join, disk).
- It does not configure the ONNX model fetch or vcpkg binary caching; the
  workflow handles both per-run.
