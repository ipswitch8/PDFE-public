# C2 — Save / Forms / Persistence — Adversarial Oracle Scorecard

Cluster **C2** = save/document + forms + undo/commands + metadata + bookmarks + pages.
66 files, 636 tests. Harness is the defendant: every test presumed worthless until it proves it
asserts a user-visible truth AND fails when that truth breaks.

## Oracle distribution (dominant category per file)

| Oracle | Files | What it means here |
|---|---:|---|
| **true** | 30 | Exact saved bytes/objects, exact field value in reopened doc, exact undo-restored state |
| **metamorphic** | 16 | Round-trip: save→reopen→field==set value; do→undo→state==original |
| **proxy** | 15 | Correlated-not-equal: reopens/no-throw/size>0/pageCount only — **silent-data-loss blind** |
| **pseudo** | 2 | Vs independent golden (QPDF isLinearized, DiffEngine ref) |
| **heuristic** | 1 | Plausible property/range (XMP substring presence) |
| **no-oracle-smoke** | 2 | Runs, asserts nothing meaningful |

The cluster is healthier than a happy-path suite would be — **46 of 66 files (70%) are true or
metamorphic**, and the persistence-critical layers (`test_metadata_roundtrip`, `test_form_roundtrip`,
`test_xfdf_encode_decode`, `test_save_recompression`, `test_page_edit_destination_integrity`,
`test_autosave_recovery`, `test_autosave_matrix`, `test_autosave_sidecar_integrity`) do real
save→reopen→exact-value readback. The weakness is concentrated in the 15 proxy files and a long tail
of individual tests that stop at "reopens OK".

## Smell counts (files exhibiting each smell)

| # | Smell | Files |
|---|---|---:|
| 7 | **reopens-without-error-only** (highest-yield) | 19 |
| 3 | EXPECT_NO_THROW-only | 17 |
| 2 | tautological | 10 |
| 6 | widget-exists-only | 8 |
| 1 | assertion-free | 6 |
| 4 | over-tolerant-threshold | 5 |
| — | no-oracle-smoke (as smell) | 3 |
| 5 | extraction-only-for-visual | 2 |

`reopens-without-error-only` is the dominant smell — exactly the proxy pattern that misses the
autosave/save-routing silent-data-loss defects this project keeps shipping.

## Ranked weak findings (worst first)

Each: file:line, test, oracle, smell(s), and the concrete user-visible failure that stays green.

1. **test_autosave_capture_audit.cpp:187** — `AutosaveCaptureAudit.*` (all 15 families) — *no-oracle-smoke / assertion-free* — **HIGH**.
   Autosave silently fails to capture ANY edit family on crash and all 15 tests stay green: `finishRow()` records `capture_present=NO` as passing DATA; the only statement is `SUCCEED()`. Zero `EXPECT_/ASSERT_` in the entire file. A crash-recovery regression that drops every edit yields a green suite. (By-design "measurement instrument" — but adversarially it is a 15-test hole; the real assertions live only in `test_autosave_capture_fixes`/`_matrix`/`_recovery`.)

2. **test_optimize.cpp:175** — `OptimizeDoesNotLeakDeletedFormFieldValues` — *proxy / reopens-without-error-only* — **HIGH**.
   Optimize (garbage=4) leaves stale/deleted form-field values embedded in the output; the test asserts only `EXPECT_GT(pageCount(),0)`. The exact data-leak the test is named for ships green.

3. **test_save_throw_undo_safety.cpp:286** — `ThrownSaveStillRebindsUndoStack` (9 families) — *proxy / EXPECT_NO_THROW-only* — **HIGH**.
   After a save that throws (file locked), `undo()` can undo the WRONG op / corrupt the page / leave inconsistent edit state and stay green — both a clean return and any thrown exception satisfy `undoReturnedCleanly`. Only a hard crash on an ASan build fails it.

4. **test_compare_documents_view.cpp:81** — `HighlightCountMatchesDiffEngineOutput` — *pseudo / tautological + over-tolerant* — **HIGH**.
   `docA` and `docB` are both `test_3page.pdf`, so `changedPages`/`expectedRegions` are 0; the whole change-highlighting path is asserted only against `0==0`. A dialog that computes change counts completely wrong for real differing docs passes.

5. **test_xmp_metadata_writer.cpp:50** — `BuildXmpPacket_PdfA1b` — *heuristic / over-tolerant + tautological* — **HIGH**.
   Asserts only that substrings `"1"` and `"B"` appear ANYWHERE in the packet (timestamps/UUIDs contain both) — cannot detect a wrong/missing PDF/A `pdfaid` part or conformance. Repeats at L66/68, 82/84, 98/100, 114/116, 150/152.

6. **test_bookmark_panel_context_menu.cpp:169** — `InsertBookmarkAddsToManager` — *proxy / widget-exists + reopens-only* — **HIGH**.
   `insertBookmark` returns true but may never insert the node (or insert wrong title/index). The test checks only the boolean return and explicitly gives up on the model oracle ("BookmarkPanel doesn't expose manager_"). A no-op insert that returns true passes despite the test name.

7. **test_replace_page.cpp:66** — `ReplacePageCommandUndoRestoresOriginal` — *metamorphic / reopens-only* — **HIGH**.
   Captures `textAfter` but NEVER asserts `textAfter != textBefore`. If `replacePage` silently no-ops, the undo round-trip trivially passes — a page-replace that does nothing (user's replacement lost) ships green.

8. **test_metadata_manager.cpp:124** — `WriteMetadata_Roundtrip` — *metamorphic / tautological + reopens-only* — **HIGH (conf: med)**.
   `writeMetadata` may stage 8 fields in an in-memory dict never serialized on save; `readMetadata` reads the same dict so all 8 `EXPECT_EQ` pass, yet the saved file carries no title/author/dates. Only Title is ever save/reopen-verified (Test 14).

9. **test_form_commands.cpp:77** — `SetFieldValue_ExecuteAndUndo` — *metamorphic / reopens-only* — **HIGH (conf: med)**.
   Value equality checked only against the in-memory `FormManager`; never saved+reopened. A regression that updates the widget cache but fails to write `/V` (value lost on save) passes.

10. **test_command_page_rebind.cpp:133** — `OrdinarySaveDrainsAndRebindsAnnotationCommand` — *true / reopens-only* — **HIGH (conf: med)**.
    Saved file validated only by reopen + `pageCount()==3`. A save that rebinds correctly but silently DROPS the created annotation from the written PDF passes; user loses the annotation on disk with a green test.

11. **test_save_operations.cpp:118** — `SaveClearsDirtyPages` — *proxy / reopens-only + NO_THROW-only* — **MED**.
    `save()` could silently drop/corrupt page content and stay green — only checks file exists, >0 bytes, reopens non-null. Nothing compares any page content or object before/after. (Sibling `MultipleMarkPageDirty` L166 identical.)

12. **test_metadata_commands.cpp:53** — `Execute_WritesNewMetadata` — *metamorphic / reopens-only* — **MED**.
    Metadata set in the in-memory model; `readMetadata` runs on the same live doc, never reopened from disk. A save that drops the metadata edit passes.

13. **test_forms_phase5.cpp:152** — `RadioGroupExclusivity` — *heuristic* — **MED**.
    Asserts only that toggled radio B != "Off"; never re-reads radio A to confirm it was DESELECTED. A radio group that fails to clear the sibling (the exact bug named) passes.

14. **test_forms_phase5.cpp:476** — `SignatureWidgetTypeIsDistinctFromOthers` — *no-oracle-smoke / widget-exists* — **MED**.
    Sole real assertion is `EXPECT_GE((int)FormFieldType::Signature,0)` — a compile-time tautology — then `SUCCEED()`; counts discarded via `(void)`. If `getFormFields` mis-tags signature widgets, the UI dispatches the wrong dialog and this never notices.

15. **test_page_ops_phase6.cpp:290** — `PagePanelPageMovedSignalContract` (and `MultiSelectDelete` L316) — *heuristic / widget-exists + tautological* — **MED**.
    The test emits `pageMoved(0,2)` itself then checks the spy caught (0,2). No drag-drop path is exercised, so a broken/never-fired reorder handler is undetected — it only proves Qt's signal plumbing works.

16. **test_page_integer_overflow.cpp:77** — `DeletePageAtInvalidIndex` — *proxy / assertion-free* — **MED**.
    Bare try/catch with ZERO assertions. `DeletePageCommand` at `INT_MAX`/-1 could delete the wrong page or no-op WITHOUT throwing and the test passes; neither a throw nor an unchanged page count is asserted.

17. **test_page_operations.cpp:178** — `ExtractPages_ReverseOrder` (and `_Multiple` L61) — *proxy / reopens-only* — **MED**.
    Pages requested reverse `{2,1,0}` but emitted forward; only `pageCount==3` checked, so page-order not being honored ships green. `_Multiple` can extract the wrong pages.

18. **test_document_js_events.cpp:151** — `WillSaveFiresOnFixtureWithDocLevelJs` — *proxy / NO_THROW-only* — **MED**.
    A willSave/AA script that silently fails to execute passes: only asserts `fireDocumentEvent` doesn't throw, and documents it cannot observe the script ran. A regression disabling doc-level JS stays green.

19. **test_qpdf_wrapper.cpp:81** — `SaveToFile` — *proxy / reopens-only* — **MED**.
    Verifies only reopen + `pageCount()==3`. A save that drops/mangles page content but preserves the count passes.

20. **test_diff.cpp:190** — `CompareIdenticalDocuments` — *proxy / tautological* — **MED (conf: med)**.
    Compares `test_3page.pdf` against itself, expecting similarity~1.0 / changedPages==0. A comparator hardwired to 1.0 (never detects any difference) passes.

Lower-severity tail (see JSON `ranked_weak_findings`): `test_document_save_atomic.cpp:161` (recovery save only pageCount>0), `test_form_data_io.cpp:344` (XfdfContainsOnlyNonEmptyValues → SUCCEED), `test_batch.cpp:117` (may-succeed-or-fail action), `test_page_commands.cpp:390` (move identity via shared dims), `test_fidelity_r3_24:195` (only endpoints of 500-page reversal checked), `test_save_path_security.cpp:71` (LongPath/UNC zero-assertion), `test_incremental_save.cpp:166` (mis-named throw test), `test_bookmarks.cpp:74` (unsigned `>=0`), `test_formatting_run.cpp:570` (ReversedRange zero-assertion), `test_metadata_dialog.cpp:63` (widget-exists, dialog never wired to a Document/save anywhere), `test_command_family_undo_after_save_probe.cpp:260` (OCR self-referential callbacks).

## Systemic patterns

- **In-memory round-trips masquerading as persistence.** `test_metadata_manager`, `test_metadata_commands`, `test_form_commands` verify values against the same in-memory manager/dict they just wrote — never save→reopen. A "stages in memory but never serializes on save" bug is invisible. The strong counterparts (`test_metadata_roundtrip`, `test_form_roundtrip`, `test_xfdf_encode_decode`) prove the correct pattern exists; the command-layer tests should adopt it.
- **"reopens OK / pageCount survives" as the persistence oracle** (19 files). Silent content/field/annotation drop on save is the exact defect class the project ships; the count-only oracle cannot see it. Highest-value fix: after each save, assert the specific edited object/field/annotation is present with the correct value in the reopened file (as `test_command_page_rebind` does for the in-memory undo count, but NOT for the on-disk copy).
- **Self-emitted-signal contract tests** (`test_page_ops_phase6`) and **file-vs-itself diff tests** (`test_diff`, `test_compare_documents_view`) assert against a trivially-true baseline and exercise none of the real logic.
- **`SUCCEED()`/`RecordProperty` measurement instruments** (`test_autosave_capture_audit`, `test_xfa_save_benchmark`) are honest by design but contribute ~16 assertion-free "tests" to the count; they should not be mistaken for correctness coverage.
