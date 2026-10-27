# Standing Controls (Phase-5 Capstone)

Durable, class-level defenses that replace one-off instance patches. Each control cites the specific
Register IDs it prevents recurrence of. **FIND-AND-DOCUMENT — proposals only.**

## SC-a — Adversarial-ambient fixture corpus × every injector, with pixel/quad oracles
Committed PDFs carrying HOSTILE ambient state (sheared/skewed CTM, rotated pages, non-zero-origin
MediaBox, leaked Tz/Tc/Tw/Ts/Tr/color) fed through every page-content injector (ContentStreamEditor,
AlignmentInjector, HeaderFooter, Watermark, TRE writeTextBlock, image insert/resize/rotate/crop,
RedactionEngine). Oracles are render-based: glyph-quad orientation/parallelogram, pixel-ink in the
intended upright ROI, colour-at-coordinate, and **unconditional** decompressed-byte-absence for
redaction. Extends `gen_adversarial_ambient_corpus.py` / `test_content_stream_adversarial_ambient.cpp`
(promote SUCCEED-only cells to ASSERTs; add the missing `shear` op).
**Closes:** R-02, R-03, R-04, R-05, R-06, R-07, R-33 · **Effort:** M–L

## SC-b — Oracle standard (visual feature ⇒ visual/quad oracle; ban weak-oracle smells)
Written, lint-enforceable standard: visual/positional features must assert a rendered glyph-quad/
pixel property (never extraction presence); ban lone `EXPECT_NO_THROW`, lone `SUCCEED()`/
assertion-free bodies, tautological predicates, and self-reimplementation; security verifiers require
**positive** detection (assert FALSE on a genuine leak/violation); regression tests must call the
production symbol under test.
**Closes:** R-03, R-14, R-16, R-18, R-19, R-20, R-21, R-22, R-12, R-13 · **Effort:** S–M

## SC-c — Mutation-kill CI gate with a REAL mutant set + enforced floor
Replace the green-by-construction 3-mutant `tools/gate/mutants.json` with the Phase-3 16-mutant set
(must include the 10 survivors M04/M05/M06/M07/M08/M09/M16/M11/M13/M15). Enforce a kill-rate floor;
a NEW survivor fails the build. Today the gate can only report 100% on 3 pre-fixed lines while the
real kill-rate is 37.5%.
**Closes:** R-24, R-02, R-05, R-08, R-10, R-11 · **Effort:** M

## SC-d — Coverage floor enforced (not best-effort)
Remove `continue-on-error:true`; treat tool absence as a hard error; enforce a per-subsystem coverage
floor that FAILS the build on regression.
**Closes:** R-32, R-27 · **Effort:** S–M

## SC-e — Mandatory escaped-defect → DCA → class-test loop
Promote `check_dca.py` / `check_dca_presence.py` from dispatch-only `governance.yml` to a required
per-commit merge gate: a bug-fix commit must add a DCA record + a class-level test or the build fails.
Ends the reactive posture (70/72 fixes escaped with no test).
**Closes:** R-26, R-31 · **Effort:** M

## SC-f — Live RTM enforcement (matrix "pass" = ran AND asserted)
A matrix cell is green only if its owning test RAN and executed ≥1 behavioral assertion in the SAME
CI run that produced the CSV. Forbid rebuilding from committed green; owner-only writes (kill the
last-write-wins clobber at `conftest.py:85/575`); dialog-reachability requires a real render.
**Closes:** R-28 · **Effort:** M

## SC-g — De-skip-wall the UI visual oracles + fix self-seeding goldens
Route menu/tool activation through `testToolActivatePipe` (invoke_action) with result-pipe readback
instead of UIA FindAll (defeats the gap_a14 hang that silently skips ~472 sites). Replace
regen-from-current goldens with committed **correctness** goldens from a trusted reference; add a
visual/icon oracle for toolbar buttons; add an ASan destruction-under-active-state matrix.
**Closes:** R-27, R-29, R-20, R-04, R-35 · **Effort:** L

## SC-h — Re-enable per-commit CI triggers
Restore push/PR auto-triggers for a fast lane (build + changed-subsystem core + mutation-kill on the
touched subsystem), full suite nightly; queue on the single runner rather than dropping. Keep the
JUnit sum-of-failures check as a required, non-removable step with a failing sentinel self-test.
**Closes:** R-25, R-30 · **Effort:** M

---

### Control → Register coverage map

| Control | Register IDs closed |
|---|---|
| SC-a | R-02, R-03, R-04, R-05, R-06, R-07, R-33, R-34 |
| SC-b | R-03, R-12, R-13, R-14, R-16, R-18, R-19, R-20, R-21, R-22 |
| SC-c | R-02, R-05, R-08, R-10, R-11, R-24 |
| SC-d | R-27, R-32 |
| SC-e | R-26, R-31 |
| SC-f | R-28 |
| SC-g | R-04, R-20, R-27, R-29, R-35 |
| SC-h | R-25, R-30, R-36, R-37 |

Every Register entry R-01…R-37 is addressed by at least one standing control and/or a remediation
phase (see `remediation-plan.json`). R-34 (orphan fixtures) is closed by SC-a (adversarial fixture
corpus wires orphan fixtures to consumer tests). R-01, R-09, R-15, R-17, R-23 are carried primarily
by the remediation phases (phase-1/3/4) rather than a standalone control. R-36/R-37 (macOS thin-subset,
flaky-quarantine ledger — added per the karen phase-5 gate) are closed by SC-h.
