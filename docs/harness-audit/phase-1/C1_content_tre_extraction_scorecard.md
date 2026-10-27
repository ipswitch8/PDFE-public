# C1 — content-edit + tre + extraction: Adversarial Oracle Scorecard

**Cluster:** `C1_content_tre_extraction` · **Files:** 66 · **Tests:** 680
**Stance:** the harness is the defendant. Every test is presumed worthless until it proves it asserts a
user-visible truth AND would fail when that truth breaks. Classification is per-FILE (dominant oracle);
smell counts are the number of FILES exhibiting each smell (a file may exhibit several).

Seeds verified directly: `test_content_stream_adversarial_ambient.cpp` confirmed assertion-free by design
(all 9 tests funnel through `finishCell()`→`SUCCEED()`, lines 126-131 — a strong internal glyph-quad+pixel
oracle that is *recorded, never asserted*). `test_content_stream_ctm_reset.cpp`, `test_content_stream_neg_tz.cpp`,
and `test_content_stream_multi_injector.cpp` confirmed **genuinely strong** (hard glyph-quad + pixel-ink
asserts on mirror/size/visibility/on-page).

---

## By-oracle (per file)

| Oracle | Files | Meaning |
|---|---:|---|
| true | 19 | asserts the actual user-visible output (glyph-quad, pixel, exact bytes, exact geometry) |
| pseudo | 10 | asserts against an independent re-derivation (golden sidecar, reference impl, exact struct) |
| metamorphic | 4 | asserts a relation (edit→undo round-trip; hybrid==full-scan parity) |
| heuristic | 4 | asserts a plausible property/range |
| **proxy** | **25** | asserts something correlated-but-not-equal (extraction-present, no-throw, return code) |
| no-oracle-smoke | 4 | asserts nothing meaningful (SUCCEED / assertion-free) |

**29 of 66 files (44%) carry a proxy or no-oracle-smoke dominant oracle.** The concentration is in the
content-edit and TRE reconstruction subsystems — exactly the VISUAL features where extraction is blind.

## By-smell (files exhibiting)

| # | Smell | Files |
|---|---|---:|
| 1 | assertion-free | 11 |
| 2 | tautological | 8 |
| 3 | EXPECT_NO_THROW-only | 16 |
| 4 | over-tolerant-threshold | 14 |
| 5 | **extraction-only-for-visual** | **16** |
| 6 | widget-exists-only | 8 |
| 7 | reopens-without-error-only | 9 |

Smell #5 (the highest-yield class for this cluster) infects 16 files: a VISUAL feature — rendered glyphs,
mirror, invisible/white text, color, position, image transform — is validated only by stext/text-extraction,
which recovers the logical codepoints regardless of how (or whether) they render. This is the exact
blind-spot that shipped the mirror-on-`-100 Tz`, half-size-under-CTM, and color-loss defects.

---

## Ranked weak-oracle findings

### HIGH — visual/critical feature with an extraction-only, tautological, or assertion-free oracle

1. **`test_ocr_routing_fix.cpp:314`** — `ImageCommandsDirtyTest.*` (all 15: Insert/Remove/Resize/Rotate/Crop × exec/undo/redo; reps 314,367,417,470,533) — *proxy; #5,#7*.
   Image edits are visual; the only oracle is `createDisplayList() != nullptr`. A command that no-ops, inserts at the wrong rect/size, rotates the wrong way, crops the wrong region, or an undo that fails to revert — the page still builds a display list → all 15 stay green. No pixels, geometry, XObject presence, or dirty-state checked.

2. **`test_content_stream_editor.cpp:172`** — `ApplySimpleEdit` (+ `PreserveUnmodifiedText:185`) — *proxy; #5,#3,#7*.
   Replaces a char with 'X', asserts only `EXPECT_NO_THROW` + `blocksAfter.size()>0`. Wrong glyph, dropped edit, mirrored/white/invisible text, corrupted position, or clobbered NEIGHBOUR spans (the shipped ambient-leak bug class) all pass — the page "still has some text".

3. **`test_text_edit_width_scaling.cpp:164`** — `CharRatioLogic` (+ six `*NoException` at 48,86,129,272,304,341) — *no-oracle-smoke; #2,#3,#1*.
   Feature is glyph width scaling. `CharRatioLogic` recomputes `oldLen/newLen` and asserts it equals `oldLen/newLen` — calls zero product code. Inverted/dropped/clamped width math (half/double-width, overlapping, off-line text) passes. The six no-throw tests read back no Tz/matrix/width.

4. **`test_region_reconstructor.cpp:229`** — `ReconstructHighQualityRegion` (+ `:256`) — *proxy; #5,#4*.
   Writes NEW glyphs into the content stream. All meaningful asserts are inside `if(result.success)` and only check text-field non-empty + bbox w/h>0. Nothing renders/re-extracts the produced PDF; wrong/garbage/mirrored/invisible reconstructed text, or a `success=false` mutation, passes.

5. **`test_text_edit_integration.cpp:236`** — `MultiplePageEdits` (+ `SaveAfterEdit:275`) — *no-oracle-smoke; #1,#7,#3*.
   `MultiplePageEdits` has ZERO assertions. `SaveAfterEdit` explicitly declines content verification (line 247) — only no-throw + reopen non-empty. A save that drops/garbles/mirrors/off-pages the edit passes.

6. **`test_alignment_pipeline.cpp:154`** — `FlushAppliesPendingBlocksToOutputPdf` — *proxy; #5,#7*.
   The ONE end-to-end test of a VISUAL feature (alignment). Blocks written Left when Center/Right requested, off-page, or invisible — `extractPlainText().find(marker)` still passes. No geometry/x-offset oracle.

7. **`test_alignment_injector.cpp:82`** — all 6 tests — *proxy; #5*.
   Left/Center/Right is the feature; only `extractPlainText().find(...)` is checked. Wrong alignment, off-page, white ink, leaked CTM all extract identically.

8. **`test_text_edit_save_roundtrip.cpp:242`** — `BoldToggleReachesContentStream` (+ font roundtrips 140,173,192) — *proxy; #5,#4,#7*.
   Bold oracle is `nameMentionsBold || nameDiffers` — ANY font-name change (subset prefix, substitution, rename) passes as "bold survived". Font family/size verified only via stext metadata; a correct Tf ref with glyphs rendered invisible/mirrored/white/off-page reports the right font regardless.

9. **`test_search.cpp:46`** — `BasicSearch` (+ `SearchResultHitStructure:75`) — *heuristic; #4,#3*.
   `totalMatches()>=0` is a tautology (returns `size_t`); quad-geometry checks are inside `for(hit:result.hits)` loops that run zero times when empty. Search returning zero hits for present text, and broken highlight-quads, stay green.

10. **`test_search_performance.cpp:56`** — `SearchPage_FindsText` (+ `CaseInsensitive:138`, `QuadsNotEmpty:104`) — *no-oracle-smoke; #1,#4*.
    File named "performance" asserts NO timing. `totalMatches()` returns `int hits.size()` so `EXPECT_GE(...,0)` is always true. Zero-hit search, or case-insensitive returning different sets, passes.

11. **`test_tre_coord_mapper.cpp:161`** — `NoYFlip_TopStaysTop` (+ `:176`, `PixelRectToPdf_SimpleRect:191`, `PdfRectToPixel_SimpleRect:210`) — *tautological; #2*.
    The comment-labelled "critical" Y-flip-fix tests compute the mapping with the test's OWN arithmetic and assert on that — they never call the product mapper. If the mapper inverts Y (scanned-text selection lands on the opposite page end), they cannot fail.

12. **`test_tre_commands.cpp:257`** — `ReconstructCommand_ExecuteAndUndo` (+ `ImageReplaceCommand_ExecuteAndUndo:327`) — *proxy; #3,#5*.
    `execute()` in `EXPECT_NO_THROW`; comment concedes "may succeed or fail gracefully"; only post-check is image-data SIZE after undo. Silent no-op, wrong text, wrong position, or a same-size-but-different restored image all pass.

13. **`test_content_stream_per_run_emission.cpp:229`** — `TwoSegmentsProduceMultipleTfOps` (+ 270,310,338) — *proxy; #5,#7*.
    Per-run font emission verified by distinct extracted-font-size count. Wrong glyphs/position, mirror, white, overlap, off-page all still yield ≥2 sizes. Streams are FlateDecode so operators are never inspected.

14. **`test_word_export.cpp:207`** — `BoldIsEmittedWhenFontNameImpliesBold` — *no-oracle-smoke; #5,#7*.
    Name claims bold coverage; the assert is `find("<w:document")!=npos` — unrelated to bold. Bold never emitted passes; comment admits "cannot assert the specific markup".

15. **`test_text_edit_undo_chain.cpp:292`** — `UndoPreservesFormattingRunsBaseline` — *no-oracle-smoke; #1*.
    Ends in `SUCCEED()`. The exact UI-TE-UNDO-01 formatting-drop bug named in its own header cannot fail it.

16. **`test_text_edit_pipeline.cpp:129`** — `FontNameMapping / EditedTextAppears / MultipleSpansEdited` — *proxy; #5*.
    Edited text mirrored, invisible, white-on-white, wrong size/color, or off-page — extraction sees the codepoints regardless.

17. **`test_ocr_pipeline.cpp:190`** — `OcrOnScannedPdf` — *proxy; #5* (confidence: likely).
    OCR's truth is the recognized text. `hocr` non-empty + `blocks>0` passes even on gibberish; no known word from the fixture is asserted.

### MED — feature partly covered, but a meaningful failure slips through

- **`test_smalltext_edit_render.cpp:96`** — despite the "render" filename, renders NO pixels; extraction-only. Misses mirror/wrong-color/wrong-position/wrong-glyph/off-page. *(prompt guess "strong" did NOT hold)*
- **`test_textedit_ux_phase2.cpp:138`** — `ApplyTextColorWithCaretRecolorsWholeSpan` inspects the internal `formattingRuns` struct, never a rendered argb; color can be recorded yet not render.
- **`test_excel_export.cpp:106`** — asserts only zip-magic + `<sheetData>` substring presence; empty/garbled cell values pass.
- **`test_text_extraction.cpp:110`** — `CharacterPositions` accepts `quad.width()>=0.0`; every-glyph-collapsed-to-a-point (broken selection/highlight) passes.
- **`test_content_stream_tagger.cpp:86`** — `PreservesRendering` asserts only `pageCount>0`; `MultipleRegions` only `pageHasMarkedContent==true` (wrong tag/dropped region/wrong MCID → wrong reading order for assistive tech, all pass).
- **`test_textedit_tm_regression.cpp:129`** — checks bbox y/height (catches Tm.d sign flip, Tfs scale) but never `ur.x>ul.x`; a pure horizontal mirror slips.
- **`test_alignment_writer.cpp:191`** — `JustifyEmitsTw` defeated by substring aliasing (`find(" Tw\n")` matches `"0 Tw\n"`); fully un-justified text passes.
- **`test_text_selector.cpp:95`** — real checks wrapped in `if(!isEmpty)` guards / empty loops; an all-empty selection regression passes.
- **`test_text_color_and_replace.cpp:150`** — `ApplyEditWith(Black)ColorDoesNotCrash` are no-throw-only (mitigated by the pixel-counting `ColorPersists...:243`).
- **`test_ocr_pipeline.cpp:131`** — `RenderPageAt300Dpi` asserts `>100` where the comment expects ~2550×3300; a 72-DPI regression passes.
- **`test_textshaper_i18n.cpp:146`** — Arabic joining verified by "glyph id ∉ nominal cmap" — proves substitution happened, not that it was correct (garbage gids pass).
- **`test_text_edit_selection_render.cpp:164`** — highlight predicate counts ANY non-white pixel; wrong-color/wrong-position highlight passes.
- **`test_text_edit_commands.cpp:224`** — `ExecuteCallsApplyEdits` is `EXPECT_NO_THROW`-only despite the name; a silent no-op passes.
- **`test_tre_commands.cpp:161`** — `SaveImageXObjectRef_ReturnsValue`/`SaveContentsRef_ReturnsValue` have zero EXPECT despite the name.

### LOW — assertion-free/tautological but low product risk, or well-mitigated by a sibling

- `test_region_reconstructor.cpp:183` / `:212` — `AnalyzeDetectsBackground` / `AnalyzeMatchesFont` are pure tautologies (`A||B||C||(!A&&!B&&!C)`; `empty()||!empty()`).
- `test_text_shaper.cpp:52` — `ShapeAsciiTextReturnsGlyphs` asserts count + advance>0, not glyph IDs or metrics.
- `test_text_edit_clipboard.cpp:373` — `PasteWithEmptyClipboard` is `SUCCEED()`; `CopyWithNoSelection:311` is no-throw.
- `test_content_stream_out_of_class.cpp:99` — `EXPECT_EQ(gid)` half may be circular (shares `fz_encode_character`); mitigated by the independent `EXPECT_NE(...,hex4(cp))` at 105.
- `test_tre_undo_after_save.cpp:327` — Oracle 2 asserts only `!restoredData.empty()`; mitigated by exact-dims Oracle 1 (191).
- `test_alignment_e2e.cpp:183` — `MainWindowOwnsAlignmentPipeline` is no-throw + `objectName()`.
- `test_actualtext_extraction_crash.cpp:57` — asserts `"ABC"` present, not that the override text is absent.
- `test_scan_style_transfer.cpp:386` / `:208` — cache/no-op tests compare only size/null-parity, never pixels.

---

## Strongest files (the model the weak ones should follow)

- **`test_content_stream_ctm_reset.cpp` / `_neg_tz.cpp` / `_multi_injector.cpp` / `_state_leak.cpp`, `test_tre_state_leak.cpp`, `test_alignment_state_leak.cpp`** — read the rendered result back via fitz stext (glyph argb + quad) and/or pixel-ink after save/reopen, and hard-assert mirror direction, glyph height ratio vs a KEEPME neighbour, visibility, on-page bounds, and neighbour integrity. These assert the leaked state's VISUAL effect, not an internal flag.
- **`test_fidelity_r3_08_ocr_wer.cpp`** — independent Levenshtein WER/CER vs a committed golden sidecar, with a self-check that proves it kills garbage.
- **`test_text_edit_model.cpp`, `test_text_edit_commands_per_run.cpp`, `test_hocr_parser.cpp`, `test_tre_types.cpp`** — exact struct/field/geometry equality against independent constants.
- **`test_textedit_vertical_caret.cpp`, `test_tre_region_tool.cpp`, `test_search_engine_tantivy_parity.cpp`, `test_tantivy_search.cpp`, `test_text_edit_toolbar.cpp`** — exact geometry / quad parity / literal displayed-value oracles.
- **`test_fidelity_r3_07_text_extraction.cpp`, `test_export_plain_text.cpp`** — extraction IS the user feature here, cross-checked against real geometry and exact bytes (legitimate, not the #5 smell).
