# CI posture: ONE runner is the normal case, not a degraded one

- **Decided:** 2026-08-26, by the repository owner, in these words: *"the other
  private runners will not always be accessible, their absence should not cause a
  failure or error, just result in all of the work being done on the runner that
  is already present on this system."*
- **Status:** in force. Machine-checked — see "How this is enforced" below.
- **Scope:** the self-hosted Windows fleet. GitHub-hosted lanes (`fast-checks`,
  the `ci-liveness` alarm) are unaffected; they never depended on our runners.

## PDFE-RUNNER-POSTURE: SOLO-CAPABLE

*(That marker line is load-bearing, in the same way
[`../audit/ci-manual-only-posture.md`](../audit/ci-manual-only-posture.md)'s is: it
gives a checker something to look for before accepting the absence of a second
machine as anything other than a fault.)*

---

## The rule

**Every self-hosted Windows job in the tree must be servable by the single machine
that is present.** Other runners are an optimisation — more capacity, longer
campaigns — never a prerequisite. When they are absent the work simply lands on
the one that is here.

That is only true if the machine that is present carries **every label the
workflows ask for**. It is easy to believe this is automatic and it is not:
GitHub routes purely by label matching, and a `runs-on` that no registered runner
satisfies does **not** error. The job queues, and is then reported `cancelled` — a
status indistinguishable from a deliberate cancellation, and one that alerts
nobody. **The symptom of getting this wrong is silence.**

## What went wrong, so the shape is recognisable

Until 2026-08-26 the deployment package registered machines **without**
`pdfe-builder`. Only two jobs in the entire tree carry that label —
`windows-validation.yml`'s `build` and `release` — so a machine provisioned by our
own package looked completely healthy and silently could not build. Nothing
alerted, because nothing failed.

Worse, the tree contradicted itself:
[`repo-migration-2026-08-22.md`](repo-migration-2026-08-22.md) had the correct
label list and explained why, while the package — the path a machine actually
follows — had the wrong one. **When two places must agree, the one a machine
follows is the one that matters.** Fixed in `04e7368` / `66d843b`.

## The default profile, and the one case for narrowing it

`tools/runner-deployment/runner-requirements.json` defines two profiles:

| Profile | Labels | `min_free_gb` | When |
|---|---|---|---|
| `build` (**`is_default: true`**) | `self-hosted, Windows, X64, pdfe-windows, pdfe-builder` | 120 | **Always, unless you can name another machine carrying `pdfe-builder`.** The only correct profile for a machine on its own. |
| `consume` (`is_default: false`) | `self-hosted, Windows, X64, pdfe-windows` | 15 | **Multi-machine fleets only.** Carries `requires_another_machine_with: pdfe-builder`. |

`consume`'s reasoning is sound and was deliberately preserved: never route a build
job to a machine that cannot build, because that does not fail fast — it fails
deep into a compile. **That reasoning inverts on a solo machine.** With nothing
else registered, withholding `pdfe-builder` does not protect anything; it just
makes `build` and `release` unsatisfiable. `.github/actions/ensure-build-tree`'s
own header says the same thing from the other direction: `release` runs
`cmake --build`, so it "must stay co-located with `build` via the `pdfe-builder`
label."

## Absence of other fleets is expected, and costs less than it looks

- **The ephemeral Hetzner box** (`[self-hosted, linux, hetzner, sanitizer]`) is
  created per run and always `hcloud server delete`d afterwards, so it is
  *correctly* absent from a resting inventory. Both label checkers exempt exactly
  that set, by an exact match — a typo'd near-miss is **not** exempt, because an
  exemption keyed on "looks roughly like the Hetzner one" would let a genuinely
  unsatisfiable `runs-on` through under cover of it.
- **It cannot compile PDFE, so its absence costs nothing that currently works.**
  This is worth stating precisely, because the obvious guess is wrong. Despite the
  filename, `hetzner-sanitizer.yml` is not a sanitizer companion to
  `windows-sanitizer.yml`: its job is `extended-fuzz`, it builds clang-15
  libFuzzer harnesses, and its own trigger comment says the 2h/harness campaign
  "complements the push-triggered `windows-fuzz.yml` short campaign". (Its header
  names `sanitizer.yml`, which is itself titled "Linux — **RETIRED**".)

  And PDFE **does not compile on Linux at all** —
  `CI-LIMITATION-PDFE-IS-WINDOWS-ONLY` in
  [`../../tests/docs/known_issues.md`](../../tests/docs/known_issues.md),
  discovered 2026-06-01 by an end-to-end test on that very Hetzner runner:
  `std::filesystem::path::value_type` is `wchar_t` only on Windows, and similar
  assumptions are scattered through core. `windows-fuzz.yml`'s header states it
  "replaces the Linux fuzz workflow which can't compile PDFE".

  So the Hetzner box's absence removes a lane that could not have run anyway. The
  capabilities live on the local runner: MSVC AddressSanitizer via
  `windows-sanitizer.yml`, and clang-cl libFuzzer campaigns via
  `windows-fuzz.yml`. What is genuinely lost is the *longer* campaign duration
  that a dedicated, disposable box allowed — and that was already lost when the
  Linux build stopped working, not by any decision recorded here.
- **A second Windows machine** being absent costs throughput only. Jobs serialise
  on a single-capacity runner rather than failing —
  `tools/gate/check_runner_queue_overlap.py` models exactly this and reports it as
  serialisation, not as an error.
- **Linux and macOS validation** genuinely cannot fall back to a Windows runner.
  They are separately red because those platforms **are not built yet** — see the
  platform-support section of the top-level README. That is a different fact from
  runner absence and should not be conflated with it.

## The build transport already degrades correctly — no change was needed

`.github/actions/ensure-build-tree` resolves a build tree in this order:

    workspace  ->  local per-SHA store  ->  Cloudflare R2  ->  GitHub artifact  ->  fail loudly

On a single machine the first or second leg serves, because the machine that
consumes is the machine that built. The R2 leg **falls through** when wholly
unconfigured, and fails closed only on *partial* configuration — a broken
credential is not treated as a miss, because that would silently downgrade to a
weaker leg and hide the breakage. Verified as part of this work; the action itself
required **no modification** for solo operation.

## How this is enforced

Two checkers, deliberately asking different questions. Neither replaces the other:

| Checker | Question | Needs a runner inventory? |
|---|---|---|
| `tools/gate/check_runner_label_coverage.py` | *Can the fleet that **exists right now** serve this job?* | **Yes** — fails closed without one |
| `tools/gate/check_label_drift.py` | *Would a machine **our own package provisions** be able to serve this job?* | **No** — meaningful on an empty fleet |

The second exists because the first could not have caught the defect above: there
was no fleet to check against. It compares every self-hosted `runs-on` in
`.github/workflows/` against the labels the deployment path registers, and is
wired into `fast-checks.yml`.

**Scope limit of the drift guard, stated rather than implied.** It reads the label
set from the ONE command marked `PDFE-DEFAULT-LABELS` in each declaration file and
**ignores every unmarked `--labels` occurrence**. That is deliberate: taking the
widest string instead let a narrowed canonical command hide behind a wider sibling
(the `security-audit` gate failed the guard's first design for exactly that), and
requiring all occurrences to agree would false-positive on a legitimate narrower
`consume`-role example. The consequence is that an **unmarked** occurrence can
drift wrong without the guard noticing. It is not live today — the package
README's two occurrences agree — but if you add a `--labels` example, either mark
nothing new (leaving the canonical one authoritative) or check it by hand.

## What is NOT true yet

**No runner is registered against this repository at all** — `gh api
repos/ipswitch8/PDFE/actions/runners` returns `total_count: 0`. Registration is
repo-scoped and did not survive the 2026-08-22 rename.

`check_runner_label_coverage.py` is therefore RED, and that is **correct**: it
refuses to score coverage against an empty fleet rather than passing vacuously.
It self-clears the moment a runner is registered. Re-registration needs an
interactive desktop session (session 0 blinds UIA) and is an **operator action** —
nothing in this document, and nothing in the pipeline that produced it, registers
a runner or tests against a live one. Sequence:
[`repo-migration-2026-08-22.md`](repo-migration-2026-08-22.md).

Register with the default label set:

    --labels self-hosted,Windows,X64,pdfe-windows,pdfe-builder

## The trade this posture accepts

Consolidation puts `build` and `release` — which compile and produce published
artifacts — on the same host and workspace root as `fuzz`, `windows-sanitizer`,
`mutation-kill` and `governance`, which execute attacker-shaped PDF input under
instrumentation. In serial, never concurrently: every workflow is
`workflow_dispatch`-only and the runner is single-capacity.

Release **signing** credentials are not exposed by this — `Sign-Artifacts.ps1` is
invoked by no workflow and is a manual, `az login`-first tool. The machine-scoped
`PDFE_R2_*` credentials are a build-cache credential, on a trust boundary that was
already the machine. `build` checks out with the default `clean: true`, so the
producing lane wipes gitignored leftovers each run.

The residual is disk contention (an availability risk — and the reason the `build`
profile asks 120 GB) and non-repo-tree state that `git clean -ffdx` cannot reach.
Full statement: `_consolidation_risk` in
[`../../tools/runner-deployment/runner-requirements.json`](../../tools/runner-deployment/runner-requirements.json).
