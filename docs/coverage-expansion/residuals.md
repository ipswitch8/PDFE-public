# Test-Coverage Expansion — Residual Register (honest open items)

<!-- tracking-banner -->
> **STATUS: HISTORICAL EVIDENCE — this is not a work list.**
>
> **GitHub Issues (`ipswitch8/PDFE`) is the canonical tracker for anything still
> open.** This document records what an audit found on the day it ran and what
> was decided about it. Rows here may have been fixed, superseded, or
> deliberately declined since. Where a row and a GitHub issue disagree, **the
> issue wins**; where a row describes something still broken and no issue
> exists, file one.
>
> See [`docs/TRACKING.md`](../TRACKING.md) for why, and for the parts of this repo that are
> operationally live rather than historical.

---


This is the deliberate anti-completeness-claim ledger. The coverage-expansion
pipeline (C-01…C-10) added real, gated tests — but it did NOT close everything,
and this file says so explicitly, mirroring the harness-audit's own discipline:
no "done" that hides open items. Three residual kinds: **OPEN PRODUCT DEFECTS**
(the important ones), **RUNNER-ONLY** verification (wired, validated by a specific
CI runner, not from the Windows dev box), and **DEFERRED long-tail**.

---

## 1. OPEN PRODUCT DEFECTS (must be fixed in product source; not test gaps)

Both found by the C-05 generative harness, committed as executable
`DISABLED_` regression locks in `tests/test_generative_invariants.cpp`. They do
NOT run in the default suite (so it stays green) and are NOT in the parity gate's
pass count, so they cannot be mistaken for "closed."

| ID | Lock (run with `--gtest_also_run_disabled_tests`) | Symptom | Fix owner |
|---|---|---|---|
| **B1** | `GenerativeInvariants.DISABLED_B1_RotateThenEditTextUndoRestoresText` | Text lost on undo after `RotatePage(0,90)` then `EditText(page0,span2)` — digest 470→403 chars; undo is not a clean inverse under a changed `/Rotate`. | content-stream editor (undo inverse) |
| **B2** | `GenerativeInvariants.DISABLED_B2_PageStructUndoRedoSaveAsNoCrash` | `0xC0000005` access violation: retained deleted-page objects on the undo stack fault on teardown after `saveAs`+reopen. Same class as `pipeline_f4a82c1e` held-Page-across-save. | command Page-ownership refactor |

**Exit criterion for each:** when the product defect is fixed, the lock flips
GREEN; remove the `DISABLED_` prefix to make it a permanent regression. Until
then these are OPEN.

## 2. RUNNER-ONLY verification (wired + honestly disclosed, NOT faked)

| Class | What is runner-only | Where it runs |
|---|---|---|
| C-01 | ThreadSanitizer *race detection* (the in-suite test is stress+invariants only) | `hetzner-sanitizer.yml` / ASan lane (Clang/Linux) |
| C-02 | GDI-handle leg is Windows-specific; the long UI journey soak | self-hosted Windows runner (`test_ui_journey_soak.py`) |
| C-06 | real assistive-technology driver (NVDA/JAWS/VoiceOver) pass | manual / runner (QAccessible headless is covered) |
| C-08 | live-window UI golden states (native menus, GPU canvas) | self-hosted Windows runner |
| C-09 | coverage-guided libFuzzer saturation + corpus persistence | `fuzz.yml` / `windows-fuzz.yml` nightly (Clang) |
| **C-10** | **macOS (Apple-Silicon) + Linux (Hetzner) parity-suite GREEN** | `macos-validation.yml` (weekly) + `linux-validation.yml` (dispatch) — proven by CI, **not** from the Windows dev box; wiring guarantees the identical selection runs there |

C-10 note: locally (Windows) the parity suite is proven GREEN — 123 suites / 953
tests, `check_junit_xml.py … --min-tests 200` → `PASS`. The macOS and Linux legs
are wired to run the **byte-identical** filter (same `parity_filter.py` over the
same `parity_suite.txt`) but their green is only observable when those runners
execute. No macOS/Linux result is fabricated in this pipeline.

## 3. DEFERRED long-tail (acknowledged, not attempted this pipeline)

- **Parity breadth:** the core parity suite is the *behavioral* core (123 suites).
  It deliberately EXCLUDES UI/widget, pixel-fidelity, perf, live-network, and the
  PreflightOpen/XPlatform OS-branch suites (documented in `parity_suite.txt`).
  Extending parity to a curated *headless-render* subset (with per-platform golden
  tolerance) is deferred.
- **Perf baseline portability (C-07):** baselines are same-machine only; a
  cross-runner perf baseline strategy is deferred.
- **Fuzz corpus depth (C-09):** seed corpora were extended (20 adversarial seeds)
  but a large persisted coverage-guided corpus is a nightly-campaign concern, not
  closed here.
- **C-01 latent hazard:** one documented latent tile-lifetime hazard (phase-1
  report) — not a confirmed race, flagged for TSan follow-up.
- **B2 broader class:** the command Page-ownership refactor that fixes B2 is a
  multi-family product change tracked separately in `pipeline_f4a82c1e`; B2 here
  is a deterministic lock of one instance of it.

---

## One-line honesty statement

The pipeline added gated coverage for all 10 register classes; **2 product
defects (B1, B2) remain OPEN**, and the C-01/C-06/C-08/C-09 sanitizer/AT/live-UI/
coverage-guided remainders plus the **C-10 macOS+Linux green** are RUNNER-ONLY —
wired and disclosed, never faked or claimed complete.
