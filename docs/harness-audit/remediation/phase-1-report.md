# Harness Remediation — Phase 1 Report

**Scope:** Convert the confidentiality + save-routing WEAK oracles into REAL
behavioral oracles so the phase-3 surviving mutants **M08, M09, M11, M16** now
DIE, and de-conditionalize the redaction byte/extraction-absence oracles so they
run unconditionally.

**Register targets:** R-02 (redaction confidentiality), R-08/R-21/R-23 (save
routing), R-15 (redaction verifier).

**Ground rule honored:** oracles were STRENGTHENED and tests/fixtures ADDED. No
test was weakened, deleted, or skipped. No product source changed (the surviving
mutants were oracle holes, not product bugs — confirmed).

---

## 1. Fixtures added (deterministic, committed + generators)

| Fixture | Generator | Property it pins |
|---|---|---|
| `tests/fixtures/redaction_multi_revision.pdf` | `gen_redaction_multi_revision.py` | Chained `/Prev` incremental update; object 3 survives at **generation 1** → `checkSingleRevision` must be FALSE. |
| `tests/fixtures/dedup_duplicate_objects.pdf` | `gen_dedup_duplicate_objects.py` | 3 byte-identical, all-referenced font objects → gc=1 keeps all (xref 8), gc=3 dedups (xref 6). |
| `tests/fixtures/recovery_verbatim_streams.pdf` | `gen_recovery_verbatim_streams.py` | Uncompressed content stream (ASCII marker) + uncompressed raw-RGB image (0xAB sentinel run) → verbatim recovery copy keeps both. |

All three (plus the pre-existing `redaction_partial_overlap.pdf`) were added to
the **CMake fixture floor** (`tests/CMakeLists.txt`) so the strengthened oracles
can never silently skip on a missing fixture.

---

## 2. Tests added / strengthened

- **M11** — `tests/test_redaction_verifier.cpp`, suite `RedactionVerifierFailPathTest`:
  - `CheckSingleRevision_MultiRevisionPdf_ReturnsFalse` — genuine multi-revision
    fixture (gen-1 object) → asserts `checkSingleRevision == false`.
  - `CheckTextRemoved_ResidualOverlappingText_ReturnsFalse` — un-redacted
    "SECRET" text overlapping a region → asserts `checkTextRemoved == false`.
- **M09** — `tests/test_command_page_rebind.cpp`, suite `CommandPageRebindTest`:
  - `FullGcEscalationDedupsDuplicateObjects` — differential oracle: same source
    saved with `markNeedsFullGc()` (gc=3) has **strictly fewer** objects than
    without it (gc=1). `countPdfObjects()` helper via MuPDF `pdf_count_objects`.
- **M08 + M16** — `tests/test_autosave_recovery.cpp`, suite `AutosaveRecoveryTest`:
  - `RecoveryCopyPreservesVerbatimStreams` — `writeRecoveryCopy` output must
    still contain the ASCII content marker (M16: `do_compress` must stay 0) AND
    the raw-image 0xAB sentinel run (M08: `do_compress_images` must stay 0).

## 3. Redaction de-conditionalization (R-02/R-15)

- `tests/test_redaction_completeness.cpp` — SetUp no longer falls back to
  `test_single.pdf` + `GTEST_SKIP`; the mid-test `GTEST_SKIP` guarding the
  byte-absence oracle was removed. Added an **extraction-absence** oracle
  (reopen → `extractPlainText` → secret absent, control present).
- `tests/test_image_redaction_phase7.cpp` — `Phase7RedactionTest` SetUp no longer
  falls back to `test_3page.pdf`; the `AppliedRedactionRemovesSecretTextBytes`
  mid-test `GTEST_SKIP` was removed. Both now run unconditionally (fixture floor).

---

## 4. Before → after mutant status

Run: `python docs/harness-audit/phase-3/run_mutations.py --only M08,M09,M10,M11,M16`
→ baselines all GREEN, then **killed=5 survived=0 kill_rate=100.0%**
(`docs/harness-audit/phase-3/mutation-results.json`).

| Mutant | Before (phase-3) | After (this phase) | Killing test (fail count) |
|---|---|---|---|
| M08 (`do_compress_images=0`→`1`) | SURVIVED | **KILLED** (6 tests, 1 fail) | `AutosaveRecoveryTest.RecoveryCopyPreservesVerbatimStreams` |
| M09 (`needsFullGc_ ? 3`→`? 1`) | SURVIVED | **KILLED** (6 tests, 1 fail) | `CommandPageRebindTest.FullGcEscalationDedupsDuplicateObjects` |
| M11 (`maxGeneration == 0`→`true`) | SURVIVED | **KILLED** (17 tests, 1 fail) | `RedactionVerifierFailPathTest.CheckSingleRevision_MultiRevisionPdf_ReturnsFalse` |
| M16 (`do_compress=0`→`1`) | SURVIVED | **KILLED** (6 tests, 1 fail) | `AutosaveRecoveryTest.RecoveryCopyPreservesVerbatimStreams` |
| M10 (control) | KILLED | **KILLED** (2 tests, 2 fail) | `RedactionPartialOverlap.*` |

## 5. Touched-suite green confirmation

Unmutated run of `RedactionVerifierTest.* : RedactionVerifierFailPathTest.* :
RedactionPartialOverlap.* : RedactionCompletenessTest.* : AutosaveRecoveryTest.* :
CommandPageRebindTest.* : Phase7RedactionTest.*` → **42 passed / 0 failed**.

No product source modified; `git status` shows only `tests/`, `tests/fixtures/`,
and `tests/CMakeLists.txt` changes (Document.cpp changes seen during the
mutation run are transient and reverted by the harness).

---

## 6. Consolidated mutation re-verification artifact

`docs/harness-audit/remediation/phase-1-mutation-reverify.json` aggregates the
phase-1 re-run outcome for all five mutants (raw JUnit XML in `reverify-runs/`):
**M08 KILLED, M09 KILLED, M10 (control) KILLED, M11 KILLED, M16 KILLED — 5/5.**
The audit's original `docs/harness-audit/phase-3/mutation-results.json` is
deliberately left at its committed state (the historical 6-killed/10-survived
record) — the phase-1 re-run evidence lives here under `remediation/`, not by
overwriting the audit artifact.
