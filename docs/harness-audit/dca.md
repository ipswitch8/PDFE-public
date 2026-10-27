# Defect Causal Analysis — 8 Escaped-Defect Seeds (Phase-5 Capstone)

Each seed from the audit plan (§5) is mapped to ≥1 Register entry (hard acceptance criterion),
the phase that caught its class, and the standing control that prevents recurrence. **All 8 seeds mapped.**

| Seed | Name | Escape class | Register entries | Caught in | Standing control |
|---|---|---|---|---|---|
| 1 | State-continuity / mirror (−100 Tz, ambient CTM/color/half-size) | oracle-weakness (extraction-blind + benign fixtures) | R-03, R-04 | P1 named; P3 M02/M03 KILLED, M04/M06/M07 SURVIVED | SC-a + SC-b |
| 2 | Crash-on-close mid-edit (~MainWindow UAF) | coverage-gap (no destruction-under-active-state test) | R-35 | P1 + P2 — **PARTIAL** (only save-throw sub-case exists) | SC-g (ASan lifecycle matrix) |
| 3 | Small-text-edit no-op | oracle-weakness (structure/self-reimpl, not produced glyph) | R-04, R-14, R-03 | P1 | SC-b + SC-a |
| 4 | Wrong icons / form separators | oracle-weakness (widget-exists-only; no pixel oracle) | R-20, R-29, R-27 | P1 + P4 | SC-g |
| 5 | GAP-A15 exit-code masking | process (trusted exit code) | R-30 | P6/P4 — now DEFENDED | SC-h |
| 6 | Gate fabrication | process (prose/manual verdicts) | R-31, R-24 | P6/P4 — structurally fixed; residual judgment gates | SC-e + SC-c |
| 7 | UI-matrix over-claim | process (pass ≠ ran-AND-asserted) | R-28, R-24 | P5/P6 | SC-f |
| 8 | RedactionVerifier full-containment | oracle-weakness (skip-gated + tautological; M11 SURVIVED) | R-02, R-15 | P1 + P3 (M11) + P4 | SC-a + SC-b + SC-c |

## Notes on partial coverage

- **Seed 2** is only partially covered today: the nearest existing test
  (`test_save_throw_undo_safety.cpp:286`) is EXPECT_NO_THROW-only and fails only on a hard ASan
  crash. A full close-window-mid-active-edit matrix does not yet exist — it is the seed-2 acceptance
  criterion of remediation phase-6 (R-35 / SC-g).
- **Seeds 5, 6** describe escapes that Phase-4 confirms are now structurally defended (JUnit-parsed
  CI; artifact-only verdicts, no forgery allow-list). They remain in the register as durable
  requirements (R-30, R-31) so the defenses are never silently removed, and because the mutation
  gate that should back seed-6's governance is itself hollow (R-24).

Full per-seed root-cause + reasoning is in `dca.json`.
