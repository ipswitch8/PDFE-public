# PDFE Test-Coverage Expansion Register

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


The classes of testing that the harness-adversarial-audit (docs/harness-audit/) and its remediation (docs/harness-audit/remediation/) did NOT scope. The audit covered oracle strength / RTM coverage / input-domain / mutation / process. These are orthogonal test CLASSES, several present-but-thin/ungated, some absent. Ranked by (risk if untested) x (ROI verifiable in the Windows headless dev environment). 'verifiable_here' = can be built + proven now (headless gtest / Python); 'runner_only' = needs a specific CI runner (macOS/Linux/TSan/self-hosted-UI) and is WIRED + honestly disclosed, not faked.

**10 classes** — High 3, Med 7. Verifiable-here: yes=4, partial=4, no=2.

| ID | Class | Sev | Verifiable here | Risk if untested | Approach | Phase |
|---|---|---|---|---|---|---|
| C-01 | concurrency / thread-safety | High | partial (stress+invariants headless; TSan race detection is Clang/Linux CI) | A data race in tile rendering/caching corrupts a tile or UAFs a display list under real sc | Add a deterministic concurrency stress test: N worker threads hammering TileCache get/put/evict + prefetch can | phase-1 |
| C-02 | endurance / soak as a gated leak oracle | High | yes (RSS/handle delta headless) | A per-operation leak (memory or GDI/file handle) accumulates over a real editing session a | Add a headless endurance test: run a fixed long op sequence (open/edit/annotate/undo/redo/save/reopen x K) mea | phase-1 |
| C-03 | i18n rendering (RTL / CJK / combining) | High | yes (headless render + quad oracle) | Arabic/Hebrew renders unshaped or LTR, CJK renders tofu/mis-positioned, combining marks de | Generate deterministic RTL (Arabic/Hebrew), CJK, and combining/ZWJ fixtures; add glyph-quad/pixel render oracl | phase-2 |
| C-04 | format-compat / migration | Med | yes (fixtures + roundtrip) | A save-format change silently breaks opening documents produced by a prior release — a rea | Commit a small corpus of files representing prior on-disk shapes (older linearization, object-stream layouts,  | phase-2 |
| C-05 | property-based / generative | Med | yes (seeded generative gtest) | Edge cases the fixed corpus never represents (op-order interactions, unusual-but-valid str | Add a seeded generative harness: random valid edit-op sequences against a document asserting metamorphic invar | phase-2 |
| C-06 | accessibility / AT tree | Med | yes (QAccessible headless); real AT-driver = manual/runner | The app is unusable with a screen reader / fails PDF-UA-shaped a11y expectations; regressi | Strengthen the QAccessibleInterface tests to assert roles, parent/child hierarchy, and value/state for the can | phase-3 |
| C-07 | performance-regression as a gated floor | Med | yes (baseline + threshold; wall-clock noise handled with generous floors) | A hot-path regression (the XFA-save 317s class) ships unnoticed — measured but never asser | Add a perf-benchmark floor gate (like the mutation-kill floor): record baselines for key hot paths (open, save | phase-4 |
| C-08 | visual-regression suite (breadth) | Med | partial (headless render goldens; full live-UI = self-hosted runner) | A CSS/paint/icon/theme regression across the wider UI surface is invisible — only the one  | Expand the correctness-golden set: committed trusted-reference goldens for key toolbar/dialog/canvas render st | phase-3 |
| C-09 | fuzzing depth | Med | partial (harness oracle + seed extension; coverage-guided depth = Clang CI lane) | A malformed-input crash/UAF on a write surface not represented by the shallow corpus ships | Give the fuzz harnesses a real oracle (no-crash + bounded-time + output-invariant, not swallow-all); extend se | phase-4 |
| C-10 | cross-platform parity | Med | no (runner-only) — wire + honest disclosure | A platform-specific behavior divergence (path handling, font discovery, rendering) ships o | Define a core parity suite that MUST pass identically on all three platforms; wire it into linux-validation.ym | phase-5 |