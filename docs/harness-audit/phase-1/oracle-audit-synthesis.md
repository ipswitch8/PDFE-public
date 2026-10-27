# Phase 1 — Oracle Audit Synthesis

Adversarial classification of **every** C++ test file's oracle strength across 6 subsystem clusters (C1–C6). The harness is the defendant: a test counts only if it asserts a user-visible truth AND fails when that truth breaks. FIND-AND-DOCUMENT only — no product or test code was modified.

## Global totals — 402 files / 4365 tests

### Oracle strength (by file)

| Oracle category | Files | Tests |
|---|---:|---:|
| true | 175 | 1931 |
| pseudo | 18 | 164 |
| metamorphic | 35 | 315 |
| heuristic | 27 | 243 |
| proxy | 131 | 1569 |
| no-oracle-smoke | 16 | 143 |

**Weak-oracle (proxy + no-oracle-smoke): 147/402 files (36.6%), 1712/4365 tests (39.2%)** — i.e. roughly a third of the suite asserts something merely *correlated* with correctness, not correctness itself.

### Test-smell prevalence (files exhibiting)

| Smell (canonical 7) | Files |
|---|---:|
| EXPECT_NO_THROW-only | 81 |
| widget-exists-only | 77 |
| reopens-without-error-only | 74 |
| tautological | 73 |
| over-tolerant-threshold | 73 |
| extraction-only-for-visual | 71 |
| assertion-free | 42 |

### Novel smell class surfaced by this audit (not one of the original 7)

| Novel smell | Files |
|---|---:|
| skip-hidden-coverage | 10 |
| misnamed-test | 2 |
| source-grep-proxy | 2 |
| partial-contract-assert | 1 |
| no-removal-check | 1 |
| weak-positive-detection | 1 |

`skip-hidden-coverage` = an oracle that is real but GTEST_SKIP-gated on a fixture/platform, so it silently degrades to a weaker proxy (or zero coverage) and still reports green; `source-grep-proxy` = asserts by grepping source/decoded text rather than the rendered/saved artifact. These are the conditionality escapes.

*Note: 40 oracle-category tokens (heuristic, no-oracle-smoke, proxy, pseudo) had leaked into per-file `smells[]` lists in C3/C5; they are excluded from smell counts (they are oracle categories, not smells) and the cluster files were cleaned.*

*All raw smell tokens mapped to a canonical or named-novel smell — zero silently dropped.*

## Per-subsystem scorecard

| Subsystem | Files | Tests | true | pseudo | meta | heur | **proxy** | **smoke** | weak% (files) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fonts | 42 | 643 | 8 | 6 | 2 | 4 | 19 | 3 | 52% |
| textedit/content-edit | 39 | 324 | 10 | 5 | 3 | 2 | 16 | 3 | 49% |
| signatures | 25 | 239 | 4 | 0 | 4 | 1 | 13 | 3 | 64% |
| ui | 51 | 491 | 34 | 0 | 0 | 3 | 13 | 1 | 27% |
| image | 28 | 369 | 12 | 0 | 1 | 4 | 10 | 1 | 39% |
| annotations | 24 | 215 | 12 | 0 | 1 | 0 | 11 | 0 | 46% |
| save/document | 31 | 244 | 16 | 2 | 3 | 0 | 9 | 1 | 32% |
| core/infra | 22 | 195 | 11 | 0 | 1 | 2 | 8 | 0 | 36% |
| other/uncategorized | 49 | 466 | 30 | 0 | 6 | 6 | 5 | 2 | 14% |
| security/crypto | 13 | 203 | 5 | 0 | 0 | 2 | 6 | 0 | 46% |
| tre | 17 | 271 | 7 | 4 | 0 | 1 | 5 | 0 | 29% |
| extraction | 10 | 85 | 2 | 1 | 1 | 1 | 4 | 1 | 50% |
| redaction | 12 | 182 | 8 | 0 | 0 | 0 | 4 | 0 | 33% |
| forms | 17 | 199 | 9 | 0 | 5 | 0 | 3 | 0 | 18% |
| pages | 6 | 44 | 1 | 0 | 3 | 0 | 2 | 0 | 33% |
| bookmarks | 3 | 38 | 1 | 0 | 0 | 0 | 1 | 1 | 67% |
| print | 2 | 12 | 1 | 0 | 0 | 0 | 1 | 0 | 50% |
| layers | 1 | 21 | 0 | 0 | 0 | 0 | 1 | 0 | 100% |
| metadata | 6 | 50 | 1 | 0 | 4 | 1 | 0 | 0 | 0% |
| undo/commands | 3 | 54 | 2 | 0 | 1 | 0 | 0 | 0 | 0% |
| reflow | 1 | 20 | 1 | 0 | 0 | 0 | 0 | 0 | 0% |

## Findings by severity

- **Critical: 2**
- **High: 77**
- **Med: 75**
- **Low: 27**

Total ranked weak-oracle findings: **181**

## Top ranked weak-oracle findings (Critical + High)

Each names the specific test and the concrete user-visible failure it would miss.

**1. [Critical] `tests/test_cve_replay.cpp:62` — verifySafeOpen (all 21 CVE replays)** (C5_annot_sig_security, oracle=proxy, smells=EXPECT_NO_THROW-only,over-tolerant-threshold, conf=High)  
   Misses: A regression re-introducing any of 21 replayed CVEs -- connecting to the attacker SMB share (CVE-2018-4993 NTLM leak), executing embedded /AA JavaScript (CVE-2021-30560), running injected font PostScript (CVE-2024-4367) -- ships GREEN as long as Document::open returns within 5-10s and doesn't throw. The catch-all try/catch (57-59) even hides an outright throw. No test asserts the attack was neutralized.

**2. [Critical] `tests/test_malicious_pdf_advanced.cpp:651` — ProcessSpawnAttemptBlocked** (C5_annot_sig_security, oracle=proxy, smells=EXPECT_NO_THROW-only, conf=High)  
   Misses: If MuJS were ever wired to a launch/openDoc transport, app.launchURL('file:///C:/Windows/System32/calc.exe') would spawn a process and this test still passes -- the only oracle is that fireKeystrokeEvent doesn't throw and pageCount() survives. The name promises a block it never verifies.

**3. [High] `tests/test_ocr_routing_fix.cpp:314` — ImageCommandsDirtyTest.* (15 cases: Insert/Remove/Resize/Rotate/Crop x exec/undo/redo; reps 314,367,417,470,533)** (C1_content_tre_extraction, oracle=proxy, smells=extraction-only-for-visual,reopens-without-error-only, conf=confirmed)  
   Misses: Image insert/remove/resize/rotate/crop are all VISUAL, yet the only oracle is createDisplayList()!=nullptr. A command that no-ops, inserts at the wrong rect/size, rotates the wrong way, crops the wrong region, or an undo that fails to revert — the page still builds a display list, so all 15 stay green. No pixels, geometry, XObject presence, or dirty-state verified.

**4. [High] `tests/test_content_stream_editor.cpp:172` — ContentStreamEditorTest.ApplySimpleEdit (and PreserveUnmodifiedText:185)** (C1_content_tre_extraction, oracle=proxy, smells=extraction-only-for-visual,EXPECT_NO_THROW-only,reopens-without-error-only, conf=confirmed)  
   Misses: Replaces a span's first char with 'X', then asserts only EXPECT_NO_THROW + blocksAfter.size()>0. The editor writing the wrong glyph, dropping the edit, mirroring the text, emitting it white/invisible, corrupting position, or clobbering/recoloring NEIGHBOUR spans (the exact shipped ambient-state-leak bug class) — all pass, because the page 'still has some text'.

**5. [High] `tests/test_text_edit_width_scaling.cpp:164` — CharRatioLogic (+ six *NoException at 48,86,129,272,304,341)** (C1_content_tre_extraction, oracle=no-oracle-smoke, smells=tautological,EXPECT_NO_THROW-only,assertion-free, conf=confirmed)  
   Misses: Feature is glyph horizontal-width scaling. CharRatioLogic recomputes charRatio=oldLen/newLen and asserts it equals oldLen/newLen — calls zero product code. If EditTextCommand's width math were inverted/dropped/clamped (text at half/double width, overlapping or spilling off-line), every assertion here passes. The six *NoException tests read back no Tz/matrix/width/geometry.

**6. [High] `tests/test_region_reconstructor.cpp:229` — ReconstructHighQualityRegion (and ReconstructReturnsRegionData:256)** (C1_content_tre_extraction, oracle=proxy, smells=extraction-only-for-visual,over-tolerant-threshold, conf=confirmed)  
   Misses: Reconstruction writes NEW glyphs into the page content stream (visual). All meaningful assertions are inside if(result.success) and only check originalText/editedText non-empty + bbox w/h>0. Nothing renders/re-extracts the produced PDF. A reconstructor writing wrong/garbage/mirrored/invisible text, or a mutation flipping success to false, passes.

**7. [High] `tests/test_text_edit_integration.cpp:236` — MultiplePageEdits (and SaveAfterEdit:275)** (C1_content_tre_extraction, oracle=no-oracle-smoke, smells=assertion-free,reopens-without-error-only,EXPECT_NO_THROW-only, conf=confirmed)  
   Misses: MultiplePageEdits has ZERO assertions after the edits ('without crash' comment) — an edit that no-ops, lands wrong glyphs/position, or corrupts the other page passes. SaveAfterEdit explicitly declines content verification (line 247) and only checks save didn't throw + reopen yields non-empty blocks — a saved PDF dropping/garbling/mirroring/off-page-writing the edit passes.

**8. [High] `tests/test_alignment_pipeline.cpp:154` — AlignmentPipelineTest.FlushAppliesPendingBlocksToOutputPdf** (C1_content_tre_extraction, oracle=proxy, smells=extraction-only-for-visual,reopens-without-error-only, conf=confirmed)  
   Misses: The ONE end-to-end test of an inherently VISUAL feature (text alignment). The injector could write every block Left-aligned when Center/Right was requested, off-page, or invisible — extractPlainText().find(marker) still succeeds, so it passes. No geometry/x-offset/render oracle exists.

**9. [High] `tests/test_alignment_injector.cpp:82` — all 6 tests (82-190)** (C1_content_tre_extraction, oracle=proxy, smells=extraction-only-for-visual, conf=confirmed)  
   Misses: Left/Center/Right alignment is the feature, but only extractPlainText().find(...) is checked. Text drawn left when it should be centered, positioned off-page, white/invisible, or with a leaked CTM all extract identically and pass. MultipleBlocksOnDifferentPages proves presence, never position.

**10. [High] `tests/test_text_edit_save_roundtrip.cpp:242` — BoldToggleReachesContentStream (+ font family/size roundtrips 140,173,192)** (C1_content_tre_extraction, oracle=proxy, smells=extraction-only-for-visual,over-tolerant-threshold,reopens-without-error-only, conf=confirmed)  
   Misses: Bold oracle is EXPECT_TRUE(nameMentionsBold || nameDiffers) (266): any font-name change for an unrelated reason (subset prefix, substitution, resource rename) passes as 'bold survived' even if strokes aren't heavier. Font family/size verified only via stext metadata — a correct Tf reference with glyphs rendered invisible/mirrored/white/off-page reports the right font regardless.

**11. [High] `tests/test_search.cpp:46` — SearchTest.BasicSearch (and SearchResultHitStructure:75)** (C1_content_tre_extraction, oracle=heuristic, smells=over-tolerant-threshold,EXPECT_NO_THROW-only, conf=confirmed)  
   Misses: Search returning ZERO hits for text actually on the fixture is invisible: BasicSearch asserts only totalMatches()>=0 (a tautology — returns size_t) and the hit-structure/quad-geometry checks live inside for(hit:result.hits) loops that run zero times when empty. Broken indexing/highlight-quads stay green.

**12. [High] `tests/test_search_performance.cpp:56` — SearchPage_FindsText (+ CaseInsensitive:138, QuadsNotEmpty:104)** (C1_content_tre_extraction, oracle=no-oracle-smoke, smells=assertion-free,over-tolerant-threshold, conf=confirmed)  
   Misses: File named 'performance' asserts NO timing. Verified: totalMatches() returns int hits.size(), so EXPECT_GE(...,0) is always true. Only real assert is query non-empty. Search returning zero hits for present text, or case-insensitive returning different sets for 'test' vs 'TEST', passes. Quad checks are in a vacuous loop.

**13. [High] `tests/test_tre_coord_mapper.cpp:161` — NoYFlip_TopStaysTop (+ NoYFlip_BottomStaysBottom:176, PixelRectToPdf_SimpleRect:191, PdfRectToPixel_SimpleRect:210)** (C1_content_tre_extraction, oracle=tautological, smells=tautological, conf=confirmed)  
   Misses: These are the comment-labelled 'critical' tests for the Y-flip regression. Each computes the mapping with the test's OWN arithmetic (e.g. pixY=(fitzTopLeft.y-tf.translateY)/tf.scaleY) and asserts on that — never calls mapper_->pdfToPixel / pixelRectToPdf. If the PRODUCT mapper inverts Y (a scanned-text selection lands on the opposite end of the page), the test cannot fail.

**14. [High] `tests/test_tre_commands.cpp:257` — ReconstructCommand_ExecuteAndUndo (and ImageReplaceCommand_ExecuteAndUndo:327)** (C1_content_tre_extraction, oracle=proxy, smells=EXPECT_NO_THROW-only,extraction-only-for-visual, conf=confirmed)  
   Misses: Reconstructing text onto a scanned page is visual. execute() is wrapped in EXPECT_NO_THROW; the comment concedes it 'may succeed or fail gracefully'; the only post-state check is image-data SIZE equality after undo. A command that silently does nothing, writes the wrong text, positions it wrong, or an undo restoring a same-size-but-different image all pass.

**15. [High] `tests/test_content_stream_per_run_emission.cpp:229` — TwoSegmentsProduceMultipleTfOps (+ SingleSegment/EmptySegment/OutOfBounds 270,310,338)** (C1_content_tre_extraction, oracle=proxy, smells=extraction-only-for-visual,reopens-without-error-only, conf=confirmed)  
   Misses: Per-run font/size emission is visual; oracle is distinct extracted-font-size count (EXPECT_GE(fontSizes.size(),2)). Per-run text rendered with wrong glyphs, wrong X/Y, mirrored, white/invisible, overlapping, or off-page all still yield >=2 distinct extracted sizes and pass. Emitted operators never inspected (streams FlateDecode-compressed).

**16. [High] `tests/test_word_export.cpp:207` — BoldIsEmittedWhenFontNameImpliesBold** (C1_content_tre_extraction, oracle=no-oracle-smoke, smells=extraction-only-for-visual,reopens-without-error-only, conf=confirmed)  
   Misses: Test name claims bold coverage; the final assert is EXPECT_NE(bytes.find("<w:document"),npos) — a structural shell check unrelated to bold. Bold never emitted into the DOCX (heuristic-to-<w:b/> wiring broken) passes; comment admits 'cannot assert the specific markup'.

**17. [High] `tests/test_text_edit_undo_chain.cpp:292` — UndoPreservesFormattingRunsBaseline** (C1_content_tre_extraction, oracle=no-oracle-smoke, smells=assertion-free, conf=confirmed)  
   Misses: Body ends in SUCCEED() ('Soft-asserting'). Undo dropping formatting runs (a blank/de-formatted line) — precisely the UI-TE-UNDO-01 bug named in the test's own header — cannot fail this test.

**18. [High] `tests/test_text_edit_pipeline.cpp:129` — FontNameMapping / EditedTextAppears / MultipleSpansEdited (129,159,203)** (C1_content_tre_extraction, oracle=proxy, smells=extraction-only-for-visual, conf=confirmed)  
   Misses: Edited text rendered mirrored, invisible, white-on-white, wrong size, wrong color, or off-page — extraction-present (find(...)!=npos) sees the codepoints regardless of how/whether they render. The visual correctness of the edit is unverified.

**19. [High] `tests/test_ocr_pipeline.cpp:190` — OcrPipelineTest.OcrOnScannedPdf** (C1_content_tre_extraction, oracle=proxy, smells=extraction-only-for-visual, conf=likely)  
   Misses: OCR's user-visible truth is the RECOGNIZED TEXT. hocr non-empty + blocks>0 passes even when OCR mis-recognizes the page into gibberish; no assertion checks a KNOWN word from the scanned fixture appears in the hOCR.

**20. [High] `tests/test_autosave_capture_audit.cpp:187` — AutosaveCaptureAudit.* (all 15 families)** (C2_save_forms_persistence, oracle=no-oracle-smoke, smells=assertion-free, conf=high)  
   Misses: Autosave silently fails to capture ANY edit family on crash (the exact data-loss class this project keeps shipping) and every one of the 15 tests stays green: finishRow() records capture_present=NO as passing DATA and the only statement is SUCCEED(). Zero EXPECT_/ASSERT_ in the whole file. A crash-recovery regression that drops all edits produces a green suite.

**21. [High] `tests/test_optimize.cpp:175` — OptimizeDoesNotLeakDeletedFormFieldValues** (C2_save_forms_persistence, oracle=proxy, smells=reopens-without-error-only, conf=high)  
   Misses: Optimize (garbage=4) leaves stale/deleted form-field values or removed content embedded in the output; the test reopens and asserts only pageCount>0 (L194), so the exact data-leak the test is NAMED for ships green.

**22. [High] `tests/test_save_throw_undo_safety.cpp:286` — ThrownSaveStillRebindsUndoStack (9 families)** (C2_save_forms_persistence, oracle=proxy, smells=EXPECT_NO_THROW-only, conf=high)  
   Misses: After a save that throws (file locked), undo() can undo the WRONG op / corrupt the page / leave inconsistent edit state and the test stays green — both a clean return and any thrown exception satisfy the oracle. Only a hard crash on an ASan build fails it; on Debug/Release a logic-corrupting rebind passes.

**23. [High] `tests/test_compare_documents_view.cpp:81` — HighlightCountMatchesDiffEngineOutput** (C2_save_forms_persistence, oracle=pseudo, smells=tautological,over-tolerant-threshold, conf=high)  
   Misses: docA and docB are both test_3page.pdf (SetUp L54-57) so changedPages/expectedRegions are 0; the entire change-highlighting path is asserted only against 0==0. A dialog that computes change counts completely wrong for real differing docs passes.

**24. [High] `tests/test_xmp_metadata_writer.cpp:50` — BuildXmpPacket_PdfA1b** (C2_save_forms_persistence, oracle=heuristic, smells=over-tolerant-threshold,tautological, conf=high)  
   Misses: Writer emits a malformed pdfaid part/conformance (part=3, wrong letter). Asserting only that substrings "1" and "B" appear ANYWHERE in the packet (timestamps/UUIDs contain both) cannot detect wrong/missing PDF/A identification. Same pattern L66/68,82/84,98/100,114/116,150/152.

**25. [High] `tests/test_bookmark_panel_context_menu.cpp:169` — InsertBookmarkAddsToManager** (C2_save_forms_persistence, oracle=proxy, smells=widget-exists-only,reopens-without-error-only, conf=high)  
   Misses: insertBookmark returns true but never inserts the node (or inserts wrong title/index). The test checks only the boolean return and explicitly gives up on the model oracle ('BookmarkPanel doesn't expose manager_', L187), so a no-op insert that returns true passes despite the test name.

**26. [High] `tests/test_replace_page.cpp:66` — ReplacePageCommandUndoRestoresOriginal** (C2_save_forms_persistence, oracle=metamorphic, smells=reopens-without-error-only, conf=high)  
   Misses: Captures textAfter (L68) but NEVER asserts textAfter != textBefore. If replacePage silently no-ops, textAfter==textBefore and the undo round-trip trivially passes — a page-replace that does nothing (user's replacement lost) ships green.

**27. [High] `tests/test_metadata_manager.cpp:124` — WriteMetadata_Roundtrip** (C2_save_forms_persistence, oracle=metamorphic, smells=tautological,reopens-without-error-only, conf=med)  
   Misses: writeMetadata may stage 8 fields in an in-memory dict never serialized on save; readMetadata reads the same in-memory dict so all 8 EXPECT_EQ pass, yet the saved file carries no title/author/dates. Only Title is ever save/reopen-verified (Test 14).

**28. [High] `tests/test_form_commands.cpp:77` — SetFieldValue_ExecuteAndUndo** (C2_save_forms_persistence, oracle=metamorphic, smells=reopens-without-error-only, conf=med)  
   Misses: Value equality is checked only against the in-memory FormManager; the field is never saved and reopened. A regression where setFieldValue updates the widget cache but fails to write /V (value lost on save) passes.

**29. [High] `tests/test_command_page_rebind.cpp:133` — OrdinarySaveDrainsAndRebindsAnnotationCommand** (C2_save_forms_persistence, oracle=true, smells=reopens-without-error-only, conf=med)  
   Misses: The saved file is validated only by reopen + pageCount()==3. A save that rebinds correctly but silently DROPS the created annotation from the written PDF passes (reopened doc still has 3 pages, in-memory undo still decrements). User loses their annotation on disk with a green test.

**30. [High] `tests/test_font_fuzz.cpp:112` — FontFuzzSmokeTest.* (all 8, via assertion-free helper fuzzFontParsing)** (C3_fonts, oracle=no-oracle-smoke, smells=assertion-free, conf=high)  
   Misses: A malformed font dict that MuPDF parses into a garbage glyph program (wrong widths, missing glyphs, silent .notdef substitution) is never detected — the harness only survives non-crash. A font-parser regression that renders 'Hello' as blank/.notdef passes green. Zero assertions = zero behavioral oracle.

**31. [High] `tests/test_cid_font_encoding.cpp:360` — RangeOverflowProtection / MrangesSingleCodepointMapping (393)** (C3_fonts, oracle=no-oracle-smoke, smells=tautological, conf=high)  
   Misses: Labelled 'regression test for Karen QA finding #2/#4' but the production CMap-scan code is NEVER called — the test copies the loop into its own body and asserts on that copy. If the real ContentStreamEditor CMap scanner still used unsigned short (the original infinite-loop bug) or mishandled mranges, both tests stay green. Zero coverage of the fix they claim to lock.

**32. [High] `tests/test_type3_font_builder.cpp:332` — CharProcContentStreamIsParseable (+ SingleGlyphRoundTrip, EncodingDifferences, all 17)** (C3_fonts, oracle=proxy, smells=extraction-only-for-visual, conf=high)  
   Misses: Type3FontBuilder writes a CharProc whose inline image is blank, inverted, or from the wrong glyph, OR /Differences maps code 1 to the wrong Unicode name. Every test passes because they only check /Subtype is Type3 and the stream contains 'd0','BI','ID','EI'. User sees garbled/blank/wrong reconstructed Type3 text. Sibling bundler proves the NCC oracle is ~30 lines away.

**33. [High] `tests/test_visual_font_ranker.cpp:121` — MultipleCandidatesSortedByCombinedScore** (C3_fonts, oracle=heuristic, smells=tautological,over-tolerant-threshold, conf=high)  
   Misses: The ONLY ranking-correctness check (Arial should win when the region was rendered in Arial) is wrapped in if(arialFirst){...}. When the ranker is wrong and Arial does NOT rank first, the EXPECT_GT is skipped and the test passes — the assertion is neutered exactly on the failure case. User editing scanned text gets Times substituted for Arial (serif glyphs).

**34. [High] `tests/test_pdf_font_embedder.cpp:52` — EmbedArialReturnsResourceName (all 6)** (C3_fonts, oracle=proxy, smells=extraction-only-for-visual,reopens-without-error-only, conf=high)  
   Misses: embedFont allocates 'Fsub0' but writes a malformed/empty FontFile2 stream or drops the glyph program; the PDF opens but embedded text renders as .notdef/tofu. Green because the test only inspects a returned string — it never reopens the doc nor rasterizes a glyph. Below even the reopens-without-error bar.

**35. [High] `tests/test_vector_font_atlas_builder.cpp:106` — AtlasContainsCommonAsciiCodepoints / RasterizedGlyphsHavePremultipliedAlpha** (C3_fonts, oracle=proxy, smells=extraction-only-for-visual, conf=high)  
   Misses: A subsetting/rasterization defect that maps 'A' to the .notdef box outline (or wrong glyph index) still yields a non-null ARGB32 bitmap of positive size that atlas.contains(U'A') reports true. User sees tofu; oracle asserts only presence+format+size>0, never that the rasterized outline is the letter A.

**36. [High] `tests/test_vector_enriched_atlas_builder.cpp:205` — EnrichedAtlasPreSynthesizesRequestedMissing / RecipeSynthesisProducesNonNullImage (184)** (C3_fonts, oracle=proxy, smells=extraction-only-for-visual, conf=high)  
   Misses: GlyphSynthesizer's b->d HFlip recipe emits a wrong/garbage/non-mirrored bitmap. Test asserts only ea.contains(U'd') + !d.isNull() + quality>=kMinQualityThreshold (self-computed by the synthesizer). User sees a malformed 'd' in edited scanned text; test stays green. No pixel comparison to a reference d or to HFlip(b).

**37. [High] `tests/test_glyph_atlas_compositor.cpp:77` — AllCharsInAtlas (+ 8 dimension-echo siblings)** (C3_fonts, oracle=no-oracle-smoke, smells=over-tolerant-threshold, conf=high)  
   Misses: compose() returns a correctly-sized but BLANK canvas (no glyphs blitted, wrong offsets, all-transparent). Asserts width==200 & height==50 which are the requested INPUTS echoed back. A compositor that draws nothing passes 9 of 12 tests. User sees empty overlay where composited glyphs should be.

**38. [High] `tests/test_glyph_atlas.cpp:11` — makeTestImage + BuildFromCharData / LookupExistingCharacter** (C3_fonts, oracle=proxy, smells=extraction-only-for-visual, conf=high)  
   Misses: Fixture paints three IDENTICAL 15x20 black rectangles for A,B,C. If GlyphAtlas maps codepoints to wrong crop regions (stores 'C' bitmap under key 'A'), every test passes because all three bitmaps are byte-identical. An atlas rendering .notdef or the wrong glyph is indistinguishable from a correct one.

**39. [High] `tests/test_glyph_synthesizer.cpp:195` — Synthesize_b_from_d_HFlip / _9_from_6_Rotate180 (223)** (C3_fonts, oracle=proxy, smells=extraction-only-for-visual, conf=high)  
   Misses: Assertions are success/!isNull/donorUsed==U'd'/quality>0. None inspect pixels. A synthesizer that copies 'd' verbatim WITHOUT the horizontal flip produces a visibly wrong 'b' on the page yet passes. The transform — the entire point of synthesis — is never validated.

**40. [High] `tests/test_annotation_font_controls.cpp:129` — FontSizeRange / FontSizeChangeEmitsSignal (210) / SetAnnotationUpdatesFontControls (151)** (C3_fonts, oracle=proxy, smells=widget-exists-only, conf=high)  
   Misses: if(spinner && spinner->isVisible()){ EXPECT_EQ(min,6); EXPECT_EQ(max,144); } — if the spinner is never created or hidden for FreeText (a real regression), the body is skipped and the test reports GREEN with zero assertions executed. The FreeText font-size control could be fully broken and the suite stays green.

**41. [High] `tests/test_cnn_font_classifier.cpp:170` — DifferentFontLowerSimilarity** (C3_fonts, oracle=heuristic, smells=over-tolerant-threshold, conf=medium)  
   Misses: The CNN must DISCRIMINATE Arial (sans) from Times (serif). EXPECT_LT(sim,0.99f) lets a broken/text-dominated embedding scoring ~0.98 for serif-vs-sans pass. A classifier that cannot tell serif from sans -> user gets a serif substitute for a sans source -> wrong glyphs, while the discrimination test stays green. Should assert a real margin (SameFont - DifferentFont > delta).

**42. [High] `tests/test_font_matching.cpp:159` — MatchWithWeightAndStyle / MatchSerifPreference (165) / Bold/ItalicInferred (171,177)** (C3_fonts, oracle=proxy, smells=no-oracle-smoke, conf=high)  
   Misses: findMatch(Arial,Bold) silently returns the Regular face; user's bold text renders non-bold. Each test states the behavior in a comment and asserts only familyName non-empty, so weight/style/serif regressions are invisible.

**43. [High] `tests/test_font_compliance_checker.cpp:96` — AllFontsCompliant_Basic / CheckFonts_EmbeddedStatus (118)** (C3_fonts, oracle=proxy, smells=tautological, conf=high)  
   Misses: EXPECT_TRUE(allCompliant || !allCompliant) and if(isEmbedded||!isEmbedded) are literal logical tautologies that can never fail. allFontsCompliant() could return the wrong verdict and the test passes. Zero coverage of the compliance decision the user relies on.

**44. [High] `tests/test_script_aware_font_selection.cpp:1276` — BitmapCompositorStyled.StyledRunsBoldUnderline_ProducesNonEmptyBitmap** (C3_fonts, oracle=no-oracle-smoke, smells=no-oracle-smoke,over-tolerant-threshold, conf=high)  
   Misses: renderStyledText ignores the bold flag and the underline (renders plain 'Hello World'); >50 ink pixels still present so it passes. That a bold run is heavier and 'World' is underlined is never verified. Same at 1338 (italic+strikethrough) and 1233 (asserts equal SIZE only).

**45. [High] `tests/test_glyph_scale_adjuster.cpp:102` — DifferentFontsScaleNonOne (+5 range-only siblings)** (C3_fonts, oracle=heuristic, smells=over-tolerant-threshold, conf=high)  
   Misses: Substitute-font horizontal scale computed as 1.6x when a true width-match needs 1.05x — substituted text renders ~50% too wide/overlapping. Bound (0.5,2.0) accepts it. No golden per-string width ratio is pinned.

## Systemic patterns

- **systemic_pattern**: extraction-only-for-visual is the dominant cross-cluster disease (content-edit/TRE/fonts/redaction/image): a visual feature verified only by text-extraction round-trip, structurally blind to mirror/invisible/white/wrong-glyph/half-size/off-page — the exact class that shipped the mirror/half-size/color-loss content-stream defects.

- **tautological_pattern**: Multiple comment-labelled 'critical'/'regression-lock' tests re-implement the production formula in-test and assert against the copy, never invoking the product code path (Y-flip coord mapper, width-scaling, CID CMap loop, tile/HiDPI math) -> a revert of the real fix cannot fail them.

- **security_theatre**: cve_replay + malicious-PDF hostile-JS tests share a no-throw / wall-clock-timing oracle around a catch-all; signature trust-chain / DocMDP / OCSP-CRL rejections are masked by permanently-untrusted or self-signed fixtures; a live operator-precedence bug (&b==0) can no-op a tamper test.

- **conditionality_escape**: The strongest redaction byte/pixel-absence oracles GTEST_SKIP under secret-fixture absence / on Windows and STILL report green -> silent degradation to mark-count proxies; box-only redaction leaving Tj operators in the saved stream would ship undetected.
