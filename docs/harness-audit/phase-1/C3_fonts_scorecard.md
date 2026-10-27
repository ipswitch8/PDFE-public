# C3 — Fonts Cluster: Adversarial Test-Oracle Scorecard

**Cluster:** C3_fonts — 42 files, 643 tests. Harness = the defendant; every test presumed worthless until it proves it asserts a user-visible truth AND fails when that truth breaks.

## Oracle distribution (dominant category per file)

| Oracle | Files | Share |
|---|---|---|
| **true** (asserts actual visible glyph/metric outcome) | 8 | 19% |
| **pseudo** (asserts vs golden/reference metrics) | 6 | 14% |
| **metamorphic** (relation: synth→bundle→decode==synth) | 2 | 5% |
| **heuristic** (plausible range) | 4 | 10% |
| **proxy** (correlated-not-equal: SOME font/label/no-throw) | 19 | 45% |
| **no-oracle-smoke** (asserts nothing meaningful) | 3 | 7% |

**Headline:** 52% of files (proxy + smoke) do NOT assert the user-visible truth their name promises. Only 10 of 42 files (true+metamorphic) actually pin a rendered/round-tripped outcome.

## Smell counts (files exhibiting each; a file may carry several)

| # | Smell | Files | Worst offenders |
|---|---|---|---|
| 1 | assertion-free | 5 | test_font_fuzz (whole file), compliance_checker:33/96/118, scale_adjuster:357, embedded_analyzer:99/146 |
| 2 | tautological (recomputes expected via code under test) | 10 | overlay_font_mapping:54-72, cid_encoding:360/393, matcher_per_line:97, serif_ascender:282, visual_ranker:121 |
| 3 | EXPECT_NO_THROW / "no crash" only | 7 | font_substitution_engine:168, cid_encoding:128, glyph_harvester:1499/1810, matcher:1129 |
| 4 | over-tolerant-threshold | 14 | scale_adjuster:102 (0.5–2.0), cnn_classifier:170 (<0.99), sub_selector:283 (<100), vec_atlas_builder:156 (±10/24px) |
| 5 | **extraction-only-for-visual** (name/metadata blind to drawn glyph) | 16 | type3_builder (all 17), vec_atlas_builder, vec_enriched, glyph_atlas, embedder, font_descriptor_style |
| 6 | widget-exists-only | 3 | atlas_comparison (ASSERT_NE nullptr on fresh new), annotation_font_controls:129/210, family_resolver |
| 7 | reopens-without-error-only | 4 | cid_encoding, font_script_integration, annotation_font_controls, auto_missing_glyph_resolver |

Smell #5 (extraction-only-for-visual) is the systemic disease of this cluster — 16 files assert a string/dict/entry-exists proxy for "was the correct glyph drawn." This is the exact class the audit predicted would be high-yield for fonts: a subset that drops a glyph → .notdef box shown, but a "font name present"/"atlas contains cp" oracle stays green.

## Ranked weak findings (file:line, concrete user-visible failure missed)

### HIGH severity

1. **test_font_fuzz.cpp:112** — `FontFuzzSmokeTest.*` (all 8). **assertion-free / no-oracle-smoke.** CONFIRMED: helper `fuzzFontParsing()` (75-102) opens+renders inside `try{}catch(...){}` with zero EXPECT_/ASSERT_. A malformed font that parses into a garbage/.notdef glyph program is undetected — only a segfault fails the test.

2. **test_cid_font_encoding.cpp:360** — `RangeOverflowProtection` / `MrangesSingleCodepointMapping` (393). **tautological.** Sold as regression locks for prior Karen QA findings, but the production CMap scanner is NEVER invoked — the test copies the loop into its own body and asserts on the copy. The real fix could be fully reverted with both tests green.

3. **test_type3_font_builder.cpp:332** — `CharProcContentStreamIsParseable` (+ all 17). **extraction-only-for-visual.** Asserts the Type3 dict shape and that the CharProc stream contains `d0/BI/ID/EI` tokens, but never decodes the inline image or renders the glyph. A CharProc drawing a blank/inverted/wrong bitmap, or `/Differences` mapping code 1 to the wrong name, passes all 17. Sibling bundler proves the NCC oracle is ~30 lines away.

4. **test_visual_font_ranker.cpp:121** — `MultipleCandidatesSortedByCombinedScore`. **tautological (neutered-on-failure).** The only ranking-correctness check (`EXPECT_GT(result[0].visualScore, result[1].visualScore)`) is wrapped in `if(arialFirst)`. When the ranker is wrong and Arial does NOT rank first, the assertion is skipped and the test passes. User gets Times substituted for Arial (serif glyphs on sans source).

5. **test_pdf_font_embedder.cpp:52** — `EmbedArialReturnsResourceName` (all 6). **extraction-only-for-visual / no reopen.** Asserts only the returned `resourceName` string. A malformed/empty FontFile2 stream (embedded text renders .notdef/tofu) passes — the test never reopens the doc nor rasterizes a glyph. Below even the reopens-without-error bar.

6. **test_vector_font_atlas_builder.cpp:106** — `AtlasContainsCommonAsciiCodepoints`. **extraction-only-for-visual.** A rasterization defect mapping 'A' → .notdef box (or wrong glyph index) still yields a non-null ARGB32 bitmap of positive size that `atlas.contains(U'A')` reports true. User sees tofu; oracle checks only presence+format+size>0.

7. **test_vector_enriched_atlas_builder.cpp:205** — `EnrichedAtlasPreSynthesizesRequestedMissing` / `RecipeSynthesisProducesNonNullImage` (184). **extraction-only-for-visual.** Asserts `contains('d')` + `!isNull()` + `quality>=kMinQualityThreshold` (self-computed by the synthesizer). A garbage/non-mirrored 'd' passes; no pixel comparison to a reference d or to HFlip(b).

8. **test_glyph_atlas_compositor.cpp:77** — `AllCharsInAtlas` (+8 siblings). **dimension-echo / no-oracle-smoke.** 9/12 tests assert only `width==200 & height==50` — the compose() INPUT args echoed back. A BLANK canvas of the right size passes; no glyph is verified as composited.

9. **test_glyph_atlas.cpp:11** — fixture `makeTestImage` + `BuildFromCharData`/`LookupExistingCharacter`. **extraction-only-for-visual.** The fixture paints IDENTICAL black rectangles for A/B/C, so `glyph('A')==glyph('B')` byte-for-byte; a codepoint→region mixup or .notdef render is provably undetectable.

10. **test_glyph_synthesizer.cpp:195** — `Synthesize_b_from_d_HFlip` / `_9_from_6_Rotate180` (223). **extraction-only-for-visual.** Asserts success/donorUsed/quality>0, never that pixels were flipped/rotated. A synth returning the donor UNCHANGED passes both. The transform (the entire point) is never validated.

11. **test_annotation_font_controls.cpp:129** — `FontSizeRange` / `FontSizeChangeEmitsSignal` (210). **widget-exists-only.** `if(spinner && spinner->isVisible()){ EXPECT_EQ(min,6); ... }` — if the spinner is never created or hidden for FreeText (a real regression), the body is skipped and the test is GREEN with zero assertions executed.

12. **test_cnn_font_classifier.cpp:170** — `DifferentFontLowerSimilarity`. **over-tolerant-threshold.** `EXPECT_LT(sim,0.99f)` lets a broken/text-dominated embedding scoring ~0.98 for serif-vs-sans pass. A CNN that cannot tell serif from sans → user gets a serif substitute for a sans source, test green. Needs a real margin (`SameFont − DifferentFont > delta`).

13. **test_font_matching.cpp:159** — `MatchWithWeightAndStyle` / `MatchSerifPreference` (165) / `Bold/ItalicInferred` (171,177). **no-oracle-smoke.** Each states the behavior in a comment and asserts only `familyName` non-empty. `findMatch(Arial,Bold)` silently returning Regular passes; user's bold renders non-bold.

14. **test_font_compliance_checker.cpp:96** — `AllFontsCompliant_Basic` / `CheckFonts_EmbeddedStatus` (118). **tautological.** `EXPECT_TRUE(x||!x)` and `if(isEmbedded||!isEmbedded)` are literal tautologies that can never fail. The compliance verdict the user relies on has zero coverage.

15. **test_script_aware_font_selection.cpp:1276** — `BitmapCompositorStyled.StyledRunsBoldUnderline`. **no-oracle-smoke.** `renderStyledText` ignoring the bold flag and underline (renders plain text) still leaves >50 ink pixels → passes. Same at 1338 (italic+strikethrough), 1233 (size-only).

16. **test_glyph_scale_adjuster.cpp:102** — `DifferentFontsScaleNonOne` (+5). **over-tolerant-threshold.** A 1.6x scale where a true width-match needs 1.05x (text ~50% too wide/overlapping) passes the (0.5,2.0) band. No golden per-string width ratio pinned.

17. **test_cid_font_encoding.cpp:128** — `CidFontPath_ViaEmbeddedFont` / `EmbeddedCidFont_HasMultiByteEncoding` (317). **EXPECT_NO_THROW-only / extraction-only.** A broken Identity-H/CIDToGIDMap indexing wrong glyphs passes: asserts only no-throw + "page still has blocks" + an "Arial" substring. Extraction round-trip rides ToUnicode (orthogonal to the drawing CMap).

### MEDIUM severity

18. **test_overlay_font_mapping.cpp:415** (+544,569,589,635) — `OverrideSkippedForRealEmbeddedFont`. **tautological.** `expectedProbePixelSize()` (54-72) is a verbatim clone of buildOverlayFont's probe formula; a wrong production formula is recomputed identically and asserted equal.

19. **test_cnn_font_ranker.cpp:47** — all 5. **proxy.** Every test asserts `ranked.empty()` on a degenerate input; none loads a working model to check ranking is correct. Mis-ordered candidates on the live path never fail a test.

20. **test_font_script_integration.cpp:112** — `OcrRegion_SelectsCoveringFont_NoPenalty` (+160). **reopens-without-error / vacuous.** The coversScript()/no-penalty checks nest inside `if(!fontFilePath.empty()) { if(prof.has_value()) {...} }`; an empty file path or failed analysis silently skips them. The Arabic-run-matched-to-Latin bug this file targets slips through.

21. **test_font_family_resolver.cpp:23** — `ResolveArial_NonEmpty` (+Bold/Italic 28,33,38). **extraction-only.** `resolve('Arial')` returning comic.ttf passes; Bold/Italic overloads never verify the face is actually bold/italic.

22. **test_auto_missing_glyph_resolver.cpp:344** — `AllReturnedRunsEncodeAssignedCodepoints` (empty-runs branch). **reopens-without-error.** The strong `FT_Get_Char_Index(font,cp)!=0` invariant only runs when runs are non-empty; a total-drop regression takes the `runs.empty()` branch and calls `SUCCEED()`.

23. **test_embedded_font_analyzer.cpp:99** — `StemWeightMeasured` / `XHeightCapHeightPopulated` (146). **assertion-free path.** If the analyzer measures nothing (total regression), all EXPECT_* are skipped with a cout note → green.

24. **test_font_matcher.cpp:882** — `FontMatcherVisualRanking.WithCompositorAlwaysUsesVisualRanking` (+861,907). **no-oracle-smoke.** Visual re-ranking could be dead code or re-rank to a worse font; passes as long as confidence>0. Explicitly declines to distinguish the visual path from metric-only.

25. **test_tessdata_font_detection.cpp:353** — `RealWorldPdf_*`. **host-locked / over-tolerant.** Serif-selection/font-size-cap/ghost-text bugs only exercise on one dev's machine (hard-coded `C:/Users/dev/...SpecimenC.pdf`); GTEST_SKIP everywhere else.

26. **test_content_stream_substitution.cpp:152** — `AccentedSubstitutionTextIsPresent`. **over-tolerant.** Edits to "Café résumé" but asserts only the ASCII skeletons 'Caf'/'sum' survive round-trip; a dropped accent/tofu (the whole point of substitution) is not caught. (Otherwise this is one of the strongest files — a real apply→save→reopen→extract metamorphic loop.)

27. **test_font_descriptor_style.cpp:133** — `BuildOverlayFont_DescriptorItalicSpan_IsItalic`. **extraction-only.** Asserts the QFont::italic() REQUEST flag, not that Qt resolves an italic face or draws slanted glyphs; "overlay dropped italic" ships green.

28. **test_glyph_harvester.cpp:1494** — `TemplateSelection_RejectsLowNCC` / `TemplatePadding_*`. **no-crash-only / conditional.** Substantive checks wrapped in `if(atlas.contains(X))`; comment at 1499 concedes "the key invariant is no crash." (Bulk of this 65-test file is genuinely strong pixel-geometry oracles; liability is concentrated in the template section ~1467-1815.)

### LOW severity

29. **test_serif_ascender_descriptor.cpp:282** — `AscenderRatio_ProfileDerived`. **tautological.** Recomputes `expected` via the same clamp+fallback the production path uses; partly saved by `EXPECT_NE(actual,0.75)`.

30. **test_font_matcher_per_line.cpp:97** — `DifferentXSizePerLineGivesDifferentSizes`. **tautological.** Expected per-line sizes restate the code's own `x_size*72/dpi` formula; a shared systematic estimator error passes.

31. **test_system_font_discovery_panose.cpp:147** — `EnumerationCompletesWithoutCrashOnFontWithoutPanose`. **no-oracle-smoke.** Name claims fallback-classification validation; asserts only `fonts.size()>0`. (Rest of file is strong fixed-golden serif/sans oracles.)

## Model files to emulate (strong, discriminating oracles)

- **test_synthesized_type3_font_bundler.cpp** — decodes the bundled CharProc inline image back to a QImage and NCC-compares to the synthesized glyph (≥0.85). The visual round-trip the Type3/CID/atlas builders all lack.
- **test_font_metric_profile.cpp** — `distanceTo()` vs spec-derived exact constants (EXPECT_NEAR 1e-5), plus an old-code regression guard (L481).
- **test_content_stream_substitution.cpp** — apply→saveAs→reopen-in-fresh-context→extract-text, asserting edited present AND original gone; written explicitly to kill a prior EXPECT_NO_THROW failure mode.
- **test_textedit_font_identity.cpp** — FreeType-classifies the RESOLVED .ttf (PANOSE serif class, OS/2 italic/weight) rather than trusting the request.
- **test_auto_missing_glyph_resolver.cpp:319** `AllReturnedRunsEncodeAssignedCodepoints` — `FT_Get_Char_Index(routedFont,cp)!=0` proves the saved PDF won't render .notdef.

## Top-5 weakest oracles (one-line each)

1. **test_font_fuzz.cpp** — 8 tests, genuinely 0 assertions; only catches a crash.
2. **test_cid_font_encoding.cpp** — regression "locks" re-implement the production loop in-test; the real CMap scanner is never called (tautological), plus no-throw-only CID embed tests.
3. **test_type3_font_builder.cpp** — 17 tests verify dict shape + operator tokens, never decode/render the glyph; blank/inverted/wrong CharProc passes.
4. **test_glyph_atlas_compositor.cpp** — 9/12 tests echo the compose() input dimensions; a blank canvas passes.
5. **test_visual_font_ranker.cpp** — the sole ranking-correctness assertion is skipped exactly when the ranker is wrong (`if(arialFirst)`).

**CNN/ranker verdict:** The CNN/ranker/PANOSE tests are NOT uniformly tautological, but the two that must prove discrimination are defective — the CNN classifier's serif-vs-sans test is **over-tolerant** (`<0.99`, effectively non-discriminating), and the CNN ranker (`test_cnn_font_ranker.cpp`) never exercises a loaded model at all (5/5 tests are degenerate empty-return checks). The visual ranker's one real check is neutered by an `if(arialFirst)` guard. By contrast the PANOSE byte→serif/sans tests (`test_system_font_discovery_panose.cpp`) and the per-run script penalties (`test_font_matcher_per_run_script.cpp`, `..._multiscript.cpp`) are genuine fixed-golden oracles.
