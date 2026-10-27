# Phase 3 — Mutation Testing (Empirical Oracle Effectiveness)

Real C++ **apply → incremental-build → run → revert** mutation testing across the 5 targeted core subsystems. 16 mutants, each a single precise source edit tied to a specific phase-1 oracle finding. Baseline confirmed all 13 gtest filters **green** before any mutation; the working tree was verified **clean** (zero product-code changes remain) after the run. FIND-AND-DOCUMENT — no product/test code changed.

## Headline

- **6 killed / 10 survived** of 16 → **kill-rate 37.5%** (set deliberately weighted toward suspected holes: 6 controls on strong oracles, 10 payloads on phase-1-flagged weak oracles).
- **16/16 mutants matched their phase-1-derived prediction** — every predicted-strong oracle killed its control, every predicted-weak oracle let its payload through. The phase-1 oracle classification is now **empirically confirmed**, not asserted.

## Per-subsystem kill-rate

| Subsystem | Mutants | Killed | Survived | Kill-rate |
|---|---:|---:|---:|---:|
| content-editing/state-continuity | 7 | 3 | 4 | 42.9% |
| fonts | 2 | 1 | 1 | 50.0% |
| forms | 2 | 1 | 1 | 50.0% |
| redaction-confidentiality | 2 | 1 | 1 | 50.0% |
| save/rewrite-routing | 3 | 0 | 3 | 0.0% |

## Mandatory state-continuity fix cross-check

Explicitly broke the recent state-continuity FIX operators and re-ran the strong oracles:

| Mutant | Fix operator broken | Result | Killer filter | tests(fail) |
|---|---|:--:|---|---:|
| M01-statecont-ctm-guard-drop-Q | `    fz_buffer* QbufEnd = fz_new_buffer_f` | **KILLED** | CtmReset.* | 3(3) |
| M02-statecont-preamble-tz-mirror | `"%.6g %.6g %.6g rg\n-100 Tz\n0 Tc\n0 Tw\` | **KILLED** | CtmReset.* | 3(3) |
| M03-statecont-editor-tz-mirror | `        chain->op_Tz(ctx, chain, -100.0f` | **KILLED** | NegTzGreyTextEdit.* | 2(1) |

All three state-continuity fix-operator mutants were **KILLED** — the CTM-guard / `100 Tz` reset / `rg` color emit are genuinely protected by the glyph-quad + pixel oracles built in the content-stream continuity pipeline. The fix is not a paper fix.

## Surviving mutants — 10 concrete defects that ship green

Each is a real behavior change that NO test in its (green) filter objected to.

### 1. M04-statecont-subst-tz-mirror — `src/textedit/ContentStreamEditor.cpp`
- **Mutation:** `        float hScale = 100.0f;` → `        float hScale = -100.0f;`
- **Behavior no test caught:** Same mirror defect as M03 but in afterTextObjectWithSubstitutionCallback (font-substitution edits). The substitution suite's oracle is extraction-only (fz_stext codepoints + an 'Arial' substring + 'page still has text blocks'), and stext extraction reports the same characters whether the run is drawn upright or horizontally mirrored -> the -100 Tz mirror is invisible and the suite stays green. Confirmed `float hScale = 100.0f;` unique (grep count 1).
- **Phase-1 finding:** tests/test_content_stream_editor.cpp:172 and test_cid_font_encoding.cpp:128 (oracle-audit-synthesis line ~732/1301): 'extraction-only-for-visual' — mirrored/invisible/off-page renders read back identically.
- **Filter that should have caught it (stayed green):** `ContentStreamSubstitutionTest.*` — 7 tests, 0 failures.

### 2. M05-statecont-alignment-offset-noop — `src/reflow/TextReflowEngine.cpp`
- **Mutation:** `        case Alignment::Center:  return (blockWidth - lineWidth) / 2.0f;` → `        case Alignment::Center:  return 0.0f;`
- **Behavior no test caught:** lineXOffset supplies the Center/Right x-shift that IS the alignment feature. Forcing Center to 0 renders center-requested blocks left-aligned. Every AlignmentInjector test asserts only extractPlainText().find(marker) (text present), never a glyph x-offset/geometry -> a block drawn at the wrong x extracts identically -> SURVIVED. Note the strong CTM-reset suite uses Left alignment only, so it does not cover this cell. Anchor unique (grep count 1).
- **Phase-1 finding:** tests/test_alignment_injector.cpp:82 and test_alignment_pipeline.cpp:154 (oracle-audit-synthesis line ~806/792): 'Left/Center/Right ... only extractPlainText().find is checked ... position never verified.'
- **Filter that should have caught it (stayed green):** `AlignmentInjectorTest.*` — 5 tests, 0 failures.

### 3. M06-statecont-subst-drop-fill-color — `src/textedit/ContentStreamEditor.cpp`
- **Mutation:** `        // R3-06: reproduce the captured source colourspace operator (k/g/scn)
 ` → `        // R3-06: reproduce the captured source colourspace operator (k/g/scn)
 `
- **Behavior no test caught:** Short-circuiting the fill-color re-emit with `&& false` means a DeviceRGB edited span in the substitution path no longer re-emits its captured `rg`, so the replacement drops to whatever ambient fill is active (the exact color-loss bug class). The substitution suite has no per-glyph argb oracle (extraction-only), so the wrong/lost color is invisible -> SURVIVED. The two preceding comment lines are unique to afterTextObjectWithSubstitutionCallback (the sibling block at line ~1170 has different comment text), making the 3-line anchor unique even though the bare `if(...)` line is not.
- **Phase-1 finding:** oracle-audit-synthesis: test_content_stream_editor.cpp:172 'emitting it white/invisible ... recoloring' pass because the page still has some text; extraction is blind to color.
- **Filter that should have caught it (stayed green):** `ContentStreamSubstitutionTest.*` — 7 tests, 0 failures.

### 4. M07-statecont-subst-fontsize-halved — `src/textedit/ContentStreamEditor.cpp`
- **Mutation:** `        float initialFontSize = inj.fontSize;` → `        float initialFontSize = inj.fontSize * 0.5f;`
- **Behavior no test caught:** Halving the initial font size feeds the canonical Tm/Tfs computation, so substituted/per-run text renders at half the intended point size. The per-run-emission oracle counts the number of DISTINCT extracted font sizes (EXPECT_GE(sizes,2)); a uniform 0.5x scale preserves distinctness, and the substitution oracle checks presence only -> neither notices absolute size -> SURVIVED. Anchor unique (grep count 1).
- **Phase-1 finding:** tests/test_content_stream_per_run_emission.cpp:229 (oracle-audit-synthesis line ~889): 'wrong size ... still yield >=2 distinct extracted sizes and pass.'
- **Filter that should have caught it (stayed green):** `ContentStreamSubstitutionTest.*:PerRunEmissionTest.*` — 12 tests, 0 failures.

### 5. M08-save-recovery-force-image-recompress — `src/core/Document.cpp`
- **Mutation:** `        opts.do_compress_images = 0;` → `        opts.do_compress_images = 1;`
- **Behavior no test caught:** writeRecoveryCopy deliberately sets do_compress_images=0 so a 30s autosave tick is a fast serialize-and-copy (the ~440ms/23MB no-recompress perf property). Forcing =1 re-flates every image on every tick — a large latency regression on image-heavy scans — but produces a still-valid, still-recoverable file. AutosaveRecovery tests assert only that the recovered document reopens with the expected edits present; there is NO timing/size oracle -> the perf regression ships green -> SURVIVED. Confirmed `opts.do_compress_images = 0;` unique (grep count 1).
- **Phase-1 finding:** tests/test_autosave_capture_audit.cpp:187 (oracle-audit-synthesis line ~956) 'Zero EXPECT_/ASSERT_ ... assertion-free'; recovery perf property is unpinned by any test.
- **Filter that should have caught it (stayed green):** `AutosaveRecoveryTest.*` — 5 tests, 0 failures.

### 6. M09-save-fullgc-downgrade — `src/core/Document.cpp`
- **Mutation:** `    return pImpl->needsFullGc_ ? 3 : kDefaultFullRewriteGc;` → `    return pImpl->needsFullGc_ ? 1 : kDefaultFullRewriteGc;`
- **Behavior no test caught:** effectiveGarbageLevel escalates to do_garbage=3 (dedup) exactly when an operation that duplicates objects ran (font embed, flatten, encryption). Downgrading the escalated case to 1 skips the dedup/compact sweep, so full-rewrite saves keep duplicated/redundant objects (file bloat, potential stale-object retention). The save-path tests validate the written file only by reopen + pageCount() (reopens-without-error), which a gc=1 file satisfies identically -> SURVIVED. Anchor unique (grep count 1); callers are the saveAs/save full-rewrite paths.
- **Phase-1 finding:** tests/test_command_page_rebind.cpp:133 and test_optimize.cpp:175 (oracle-audit-synthesis line ~1077/969): saved file validated only by reopen+pageCount; leak/bloat invisible.
- **Filter that should have caught it (stayed green):** `CommandPageRebindTest.*:*OrdinarySaveDrains*` — 5 tests, 0 failures.

### 7. M11-redaction-singlerev-always-clean — `src/redaction/RedactionVerifier.cpp`
- **Mutation:** `        return maxGeneration == 0;` → `        return true;`
- **Behavior no test caught:** checkSingleRevision returns false when residual incremental-update generations (recoverable prior data) survive a redaction rewrite. Forcing `return true` makes the verifier always report a single clean revision even when the pre-redaction content is still appended/recoverable. The unit file constrains this with tautologies (EXPECT_TRUE(result.success||!result.success)); no test asserts checkSingleRevision returns FALSE on a genuinely multi-revision file -> the confidentiality regression is invisible -> SURVIVED. Anchor unique (grep count 1).
- **Phase-1 finding:** tests/test_redaction_verifier.cpp:114 (oracle-audit-synthesis line ~1344): 'A mutation making check... always-return-true ... passes this entire file.'
- **Filter that should have caught it (stayed green):** `RedactionVerifierTest.*:RedactionVerifierFailPathTest.*` — 15 tests, 0 failures.

### 8. M13-forms-button-reset-to-off — `src/forms/FormManager.cpp`
- **Mutation:** `                pdf_set_field_value(ctx, pdfDoc, fieldObj, dvName, 1);` → `                pdf_set_field_value(ctx, pdfDoc, fieldObj, "Off", 1);`
- **Behavior no test caught:** The Btn branch of resetTxWidgetFromObj restores a checkbox/radio to its /DV (e.g. /Yes) rather than /Off. Hard-coding "Off" reverts a defaulted-on control to unchecked on Reset Form. ResetFormRestoresDefaultValues asserts only the text field FirstName; no test asserts a checkbox/radio returns to its /DV state after reset -> the button-reset regression is unobserved -> SURVIVED. `fieldObj, dvName, 1);` is unique (grep count 1).
- **Phase-1 finding:** oracle-audit-synthesis forms cluster: reset coverage is text-field-only; button /DV-restore path has no positive oracle (widget-exists/reopens-only smells dominate forms).
- **Filter that should have caught it (stayed green):** `Phase5FormsTest.*:FormManagerTest.*` — 26 tests, 0 failures.

### 9. M15-fonts-glyph-scale-invert — `src/fonts/GlyphScaleAdjuster.cpp`
- **Mutation:** `    result.horizontalScale = originalWidth / substituteWidth;` → `    result.horizontalScale = substituteWidth / originalWidth;`
- **Behavior no test caught:** horizontalScale must be originalWidth/substituteWidth so a metric-incompatible substitute is squeezed/stretched to the original run's width. Inverting it applies the reciprocal (e.g. 0.85x where 1.18x was needed), so substituted text renders visibly too narrow/wide/overlapping. The GlyphScaleAdjuster suite only bounds the scale into a wide (0.5,2.0) range and pins no golden per-string width ratio; for near-metric-compatible pairs the inverted value stays inside the band -> SURVIVED. Anchor unique (grep count 1).
- **Phase-1 finding:** tests/test_glyph_scale_adjuster.cpp:102 (oracle-audit-synthesis line ~1288): 'Bound (0.5,2.0) accepts it. No golden per-string width ratio is pinned.'
- **Filter that should have caught it (stayed green):** `GlyphScaleAdjusterTest.*` — 15 tests, 0 failures.

### 10. M16-save-recovery-force-compress — `src/core/Document.cpp`
- **Mutation:** `        opts.do_compress = 0;` → `        opts.do_compress = 1;`
- **Behavior no test caught:** Companion to M08: writeRecoveryCopy sets do_compress=0 so existing content/object streams are copied verbatim (fast). Forcing =1 re-deflates streams on every autosave tick — a latency regression — but yields a valid recoverable file. AutosaveRecovery tests verify recovered-content correctness only, with no timing/size oracle, so the regression is invisible -> SURVIVED. Confirmed `opts.do_compress = 0;` unique (grep count 1).
- **Phase-1 finding:** tests/test_autosave_capture_audit.cpp:187 (assertion-free) and the AutosaveRecovery suite's content-only oracle: recovery perf/size is unpinned.
- **Filter that should have caught it (stayed green):** `AutosaveRecoveryTest.*` — 5 tests, 0 failures.

## test-runner evidence (GAP-A15)

- Baseline: 13 filters, all green. Mutant builds succeeded: 16/16. JUnit XML parsed for baseline + all mutant runs (exit code alone not trusted). Working tree clean after run (git status --porcelain src/ empty).