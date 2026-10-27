# Security Workflow Inventory & Trigger Policy

<!-- tracking-banner -->
> **STATUS: HISTORICAL EVIDENCE — this is not a work list.**
>
> **GitHub Issues (`ipswitch8/PDFE`) is the canonical tracker for anything still
> open.** This document records what was found or decided on the day it was
> written. Items here may have been fixed, superseded, or deliberately declined
> since. Where this document and a GitHub issue disagree, **the issue wins**.
>
> See [`docs/TRACKING.md`](../../docs/TRACKING.md).

---


**Last updated:** 2026-08-01 (meta-audit follow-up **F5**, gap `SEC-109`)
**Previously updated:** 2026-06-11 (pipeline f3c91a2e Phase 5)
**Purpose:** Single reference for which security CI workflows fire
automatically, which are manual-only, and which are retired — so the
Phase 8 Question-2 ("all known attack methods verified to fail")
evaluation has one place to look.

> **THIS TABLE WAS WRONG BETWEEN 2026-07-20 AND 2026-08-01** and that is
> the whole of gap `SEC-109`. The 2026-07-20 change removed the `push`
> triggers from `windows-sanitizer.yml` and `windows-fuzz.yml` to protect
> the single self-hosted runner; this document was not updated, so it went
> on describing two push-triggered auto-fire lanes that in fact fired only
> on human dispatch. The table below is re-derived from the `on:` blocks as
> of 2026-08-01. **The authoritative per-workflow decision record is
> [`docs/audit/meta-audit-2026-07/followup/phase-5-dispositions.md`](../../docs/audit/meta-audit-2026-07/followup/phase-5-dispositions.md)**,
> which covers all 11 workflows (not just the security ones) and states the
> rationale for every dispatch-only lane.

---

## Summary table — verified against the `on:` blocks, 2026-08-01

| Workflow | Platform | Runner | Trigger policy | Status |
|---|---|---|---|---|
| `windows-sanitizer.yml` | Windows | self-hosted `pdfe-windows` | **`schedule: '0 18 * * 0,2,4'` (Sun/Tue/Thu 18:00 UTC, max 3-day gap) + dispatch** — no `push` | **ACTIVE — auto-fire** |
| `windows-fuzz.yml` | Windows | self-hosted `pdfe-windows` | **`schedule: '30 3 * * *'` (nightly 03:30 UTC) + dispatch** — no `push` | **ACTIVE — auto-fire** |
| `windows-validation.yml` | Windows | self-hosted `pdfe-windows` | **`schedule: '20 4 * * *'` + dispatch** — also runs `ctest -L security` as of F5 | **ACTIVE — auto-fire** |
| `governance.yml` | Windows | self-hosted `pdfe-windows` | **`schedule: '0 2 * * *'` + dispatch** (F5 / `SEC-28`; moved from `'0 22 * * *'` by F38 to clear the sanitizer and mutation-kill tails. Still the runner-liveness **probe**, `SEC-31`, but **not** the alarm: F38 measured that an unserviced run is `cancelled`, not failed, so it is silent. **F44 (2026-08-08) moved the alarm off this lane and off this host** — see `ci-liveness.yml` below and backlog row **F38-b**.) | **ACTIVE — auto-fire** |
| `mutation-kill.yml` | Windows | self-hosted `pdfe-windows` | **`schedule: '0 20 * * 6'` (weekly, Sat 20:00 UTC) + dispatch** (F5 / `SEC-27`) | **ACTIVE — auto-fire** |
| `fast-checks.yml` | Linux | GitHub-hosted ubuntu | **`push` + `pull_request` (master) + dispatch** | **ACTIVE — auto-fire** |
| `ci-liveness.yml` | Linux | GitHub-hosted ubuntu | **`schedule: '0 9 * * *'` + dispatch** (F44 / backlog **F38-b**, **F46**). Asserts a *recent genuine success* of `governance.yml` via `tools/gate/check_governance_liveness.py`. **Deliberately NOT on the self-hosted host** — a probe hosted on the machine it monitors cannot report that machine being down, which is why the 2026-07-25..08-07 outage was invisible. `cancel-in-progress: false`, so the alarm cannot silence its own queued run. | **ACTIVE — auto-fire** (expected RED until the governance lane succeeds once; that is the alarm working) |
| `hetzner-sanitizer.yml` | Linux (ephemeral) | self-hosted Hetzner (on demand) | **dispatch-only** — ephemeral billed runner; a cron would queue forever or spin up a billed VM unattended | MANUAL (by design) |
| `linux-validation.yml` | Linux (ephemeral) | self-hosted Hetzner (on demand) | **dispatch-only** — same ephemeral-runner reason, plus the port is blocked by deferred `#42` | MANUAL (by design) |
| `sanitizer.yml` | Linux | GitHub-hosted ubuntu | dispatch-only | RETIRED — replaced by `windows-sanitizer.yml` |
| `fuzz.yml` | Linux | GitHub-hosted ubuntu | dispatch-only | RETIRED — replaced by `windows-fuzz.yml` |

The **ACTIVE Windows-native** workflows are the canonical security gate for
PDFE. Everything Linux is retired or manual because the project does not
compile on Linux, and that port is blocked by deferred tracker `#42` — which
is also why gap `SEC-17` (no MSan anywhere) and the **Linux half** of gap
`SEC-19` are recorded **BLOCKED-BY-#42**, not closed.

**Note on the auto-fire cadence.** None of the Windows lanes is
push-triggered, and that is deliberate, not an oversight: the project has a
**single-capacity** self-hosted runner shared by validation, fuzz, sanitizer,
governance and mutation. Per-commit triggers were removed on 2026-07-20 for
that reason and have not been restored. Automatic coverage is provided by
staggered schedules instead — see the slot arithmetic in each workflow header.

---

## Active (auto-fire)

### `windows-sanitizer.yml` — MSVC AddressSanitizer
- **Runner:** self-hosted `pdfe-windows` (warm vcpkg cache, VS2022
  ASan runtime on host).
- **Trigger:** `schedule: '0 18 * * 0,2,4'` (Sun/Tue/Thu 18:00 UTC — gaps of
  2, 2 and 3 days, so a max 3-day detection window) + `workflow_dispatch`.
  **There is no `push` trigger** — it was removed
  2026-07-20 so a ~6 h ASan build could not be started by every commit on
  the single self-hosted runner, and follow-up F4 replaced it with a
  weekly schedule rather than restoring per-commit cost.
  *(Corrected 2026-08-01, F5 / `SEC-109`: this line used to claim `push`.)*
  *(Tightened 2026-08-06, F30: weekly `'0 18 * * 0'` → `'0 18 * * 0,2,4'`.
  The F4 `security-audit` gate flagged that weekly leaves a 7-day window on
  an ASan-only surface — #47 and #48 were both ASan-only finds. The hour and
  minute are unchanged, so F4's runner non-collision argument is preserved;
  only the day-of-week set widened. Enforced by
  `tools/gate/check_asan_cadence.py`.)*
- **What it does:** builds `pdfe_tests` with the
  `windows-vs2022-msvc-asan` preset (`/fsanitize=address`) and runs a
  21-pattern security-focused gtest filter (394 tests / 41 suites):
  malformed-PDF parsing, encryption/permission bypass, redaction
  completeness, embedded-JS sandbox, signature tamper / byte-range,
  PKCS#7 + font parser fuzz harnesses.
- **Fails red on:** any ASan `==N==ERROR:` line, non-zero gtest exit,
  or an `ASAN_OPTIONS` parse error.
- **Last green:** run 27367556714, sha edde1d9, 2026-06-11 —
  Verdict PASS, 0 findings. Report:
  `tests/security/reports/sanitizers/run_20260611T203905Z_msvc_ci/`.
- **Why MSVC, not clang-cl:** the clang-cl ASan runtime DLL fails to
  load with 0xC0000135 on this host
  (CI-LIMITATION-CLANG-CL-ASAN-RUNTIME-WIN). MSVC's
  `asan_dynamic-x86_64.dll` ships with VS2022 and loads cleanly.

### `windows-fuzz.yml` — clang-cl libFuzzer
- **Runner:** self-hosted `pdfe-windows` (repointed from the
  GitHub-hosted `windows-2022` in Phase 5 — see below).
- **Trigger:** *(corrected 2026-08-01, F5 / `SEC-109` — the `push` and
  `0 4 * * *` claims below were both stale)*
  - nightly `schedule` (`30 3 * * *`, 03:30 UTC) — the full campaign.
  - `workflow_dispatch` (inputs: `duration_seconds`, `detect_leaks`).
  - **No `push` and no `pull_request`.** Deliberate: the single
    self-hosted runner must not be tied up on every commit.
- **Per-harness duration:** 1800 s on schedule, 300 s on a bare dispatch,
  or the dispatched `duration_seconds`. All ≥ the 60 s GAP-C8 floor.
  **12** harnesses: `fuzz_document_open`, `fuzz_page_render`,
  `fuzz_text_extract`, `fuzz_annotation_walk`, `fuzz_form_parse`,
  `fuzz_embedded_files`, `fuzz_encryption_dict`, plus the four R2-15/16
  write-surface harnesses (`fuzz_content_stream_edit`,
  `fuzz_save_roundtrip`, `fuzz_mujs_exec`, `fuzz_redaction_apply`) and
  the F4 image-codec harness (`fuzz_image_decode`).
- **Campaign vs timeout (F5 / `SEC-02`):** 12 × 1800 s = **360 min**
  campaign inside a **600 min** `timeout-minutes`. The scheduled duration
  had been raised to 3600 s, making the campaign 720 min — longer than the
  job's own timeout — so the tail of the array (all four write-surface
  harnesses plus the image-codec one) was cancelled unrun every night.
- **Real findings now fail the run (F5 / `SEC-01`):** the campaign job
  keeps `continue-on-error: true` so the documented benign
  `CI-LIMITATION-WINDOWS-FUZZ-CXX-EXCEPTION-DEADLY-SIGNAL` false positive
  cannot red the nightly and evict the corpus cache — but a **separate,
  blocking** `real-crash-gate` job reads the campaign's `any_real_crash`
  output and fails the workflow when it is true (or when the campaign
  published no verdict at all).
- **Concurrency:** `cancel-in-progress` on `github.ref` — a superseded
  run is cancelled.
- **Build design (Phase 5):** SINGLE job — configure once, build all 7
  harnesses, fuzz each sequentially — NOT a 7-way matrix (the matrix
  rebuilt the full dep closure per harness, ~16 h). Two-stage compile:
  stage 1 installs vcpkg deps with default MSVC; stage 2 builds
  PDFE + harnesses with clang-cl + libFuzzer reusing the MSVC deps
  (clang-cl is MSVC-ABI-compatible). This keeps vcpkg off the
  cache-invalidating clang-cl path.
- **STATUS — infrastructure complete, full campaign environment-
  constrained.** Across pipeline f3c91a2e Phase 5 every CODE-level
  defect was fixed: GitHub-hosted → self-hosted runner; the
  `clang-cl -dumpversion` null-Trim crash in clang_rt detection →
  glob resolution; the 7-way matrix rebuild → single job; the
  clang-cl-forces-vcpkg-rebuild blowout → two-stage MSVC/clang-cl
  split. The remaining blocker is **not PDFE code** — see
  `CI-LIMITATION-FUZZ-CAMPAIGN-VCPKG-CACHE` in known_issues.md: the
  x-gha binary cache (GitHub's 10 GB per-repo limit) cannot hold
  PDFE's 46-package Qt6 closure, so every run source-rebuilds all 46
  deps (~90 min) and depends on 46 flaky upstream source downloads
  (gumbo failed in run 27430972625; qtbase in 27397287924). A
  **persistent local binary cache** (`files,C:\vcpkg-bincache`) was
  added so that once ONE run completes the closure (e.g. a nightly
  catching a clean download window), every later run restores
  instantly and the campaign goes stably green.
- **Verified net in the meantime:** the fuzz HARNESS entry points are
  already exercised under MSVC ASan via `FontFuzzSmokeTest` +
  `Pkcs7FuzzSmokeTest` in `windows-sanitizer.yml` (0 findings). The
  libFuzzer campaign adds coverage-guided depth on top of that.

---

## Manual-only (by design)

### `hetzner-sanitizer.yml` — extended Linux libFuzzer (ephemeral)
- **Trigger:** `workflow_dispatch` only. **A schedule is deliberately
  NOT used.**
- **Why dispatch-only:** the Hetzner runner is *ephemeral* — provisioned
  on demand (`tests/scripts/hetzner_ephemeral_runner.ps1`), used for
  one run, and **always destroyed** afterward
  (`hcloud server delete`), per the `feedback_hetzner_ephemeral`
  project rule. A `schedule:` trigger would either fire with no runner
  online (queue forever) or, if paired with auto-provisioning, spin up
  a billed VM on a cron unattended — violating the ephemeral-only rule
  and risking an orphaned billed server. Correct flow: provision →
  dispatch manually → collect artifacts → destroy.
- **Role:** longer (2 h/harness default) campaign complementing the
  nightly-scheduled `windows-fuzz.yml` *(corrected 2026-08-01, F5: that
  lane is schedule-triggered, not push-triggered)*. Note: PDFE does not
  compile on Linux today (CI-LIMITATION-PDFE-IS-WINDOWS-ONLY), so this
  path is reserved for when a Linux port exists — which is blocked by
  deferred tracker `#42`.

### `linux-validation.yml` — Linux build/test validation
- **Trigger:** `workflow_dispatch` only. **A schedule is deliberately
  NOT used** — see the in-file comment on its `on:` block.
- **Why dispatch-only:** same ephemeral-Hetzner-runner reason as
  `hetzner-sanitizer.yml` (no runner exists on a timer), *plus* the port
  itself is deferred by tracker `#42` under explicit user direction, so a
  timer trigger would manufacture a standing red.
- **Consequence, stated in writing:** gap `SEC-17` (no MSan) and the
  **Linux half** of gap `SEC-19` are **BLOCKED-BY-#42**. MSan does not
  exist on Windows/MSVC, so no Windows lane can substitute.

---

## Retired (kept for reference, do not auto-fire)

### `sanitizer.yml` — Linux ASan/UBSan
- **Status:** RETIRED, `workflow_dispatch` only.
- **Why:** two compounding blockers —
  (1) CI-LIMITATION-PDFE-IS-WINDOWS-ONLY (source doesn't compile on
  Linux), and (2) CI-LIMITATION-GITHUB-FREE-TIER (free-tier runner
  can't complete the vcpkg cold-cache build).
- **Replaced by:** `windows-sanitizer.yml`.

### `fuzz.yml` — Linux libFuzzer
- **Status:** RETIRED, `workflow_dispatch` only.
- **Why:** same two blockers as `sanitizer.yml`.
- **Replaced by:** `windows-fuzz.yml`.

### `linux-validation.yml` — Linux build/test validation
- **Status:** dispatch-only (retired from auto-fire). Not a security
  workflow; listed for trigger-inventory completeness.
- **Why:** blocked by CI-LIMITATION-PDFE-IS-WINDOWS-ONLY — would always
  fail at configure on Linux. Left dispatch-only for future
  Linux-port verification.

---

## When to revisit

Re-enable the Linux workflows (sanitizer.yml, fuzz.yml,
hetzner schedule, linux-validation push) **only when both** of these
become true:

1. PDFE compiles and passes tests on Linux
   (CI-LIMITATION-PDFE-IS-WINDOWS-ONLY resolved), AND
2. A Linux runner with a warm vcpkg cache exists (a registered
   self-hosted Linux host, or a paid larger GitHub runner —
   CI-LIMITATION-GITHUB-FREE-TIER).

Until then, the Windows-native pair (`windows-sanitizer.yml` +
`windows-fuzz.yml`) is the complete, auto-firing security gate.
