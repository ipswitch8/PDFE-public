# C4 — Redaction / Image / Reflow / Print / Layers — Adversarial Oracle Scorecard

**Cluster:** `C4_redaction_image_visual` · **Files:** 44 · **Tests:** 604
**Stance:** adversarial — the harness is the defendant. Redaction carries a **confidentiality / data-leak** dimension: a weak redaction oracle is a security escape.

---

## 1. Oracle distribution (44 files)

| Oracle class | Files | Meaning |
|---|---:|---|
| **true** | 22 | Asserts the actual visible/confidential result (redacted pixels/bytes gone; transform pixels correct) |
| **pseudo** (golden) | 0 | — |
| **metamorphic** | 1 | Relation (DPI-detected ≈ DPI-declared) |
| **heuristic** | 4 | Plausible property (pixel-count > 0, threshold in range) |
| **proxy** | 16 | Correlated-not-equal (count delta, no-throw, reopens, CTM readback) |
| **no-oracle-smoke** | 1 | Asserts nothing meaningful |

Roughly **half the cluster (17/44) rests on proxy / heuristic / no-oracle oracles** — count deltas, reopen-succeeds, or matrix arithmetic — rather than rendered pixels or byte-absence.

## 2. Smell counts (files exhibiting)

| # | Smell | Files | Notes |
|---|---|---:|---|
| 1 | assertion-free | 0 | none |
| 2 | tautological | 8 | recomputes expected via the production formula / reads back the CTM it wrote / emits-then-spies the signal |
| 3 | EXPECT_NO_THROW-only (incl. `SUCCEED()`) | 11 | ctor tests, no-op paths, `SUCCEED();` tails |
| 4 | over-tolerant-threshold | 10 | SSIM/NCC/pixel bands wide enough to pass real regressions |
| 5 | extraction-only-for-visual | 14 | visual/print/layer/image result judged by count/metadata not pixels |
| 6 | widget-exists-only | 8 | `findChild` existence in panel tests |
| 7 | reopens-without-error-only | 13 | mutate → assert only that the file reopens |
| — | skips-hide-coverage | 7 | fixture/tessdata-gated `GTEST_SKIP` → whole file green when absent |

---

## 3. Redaction confidentiality — verdict

The cluster **does** cover the three confidentiality primitives — and covers them well where they run:

- **Flate-blindness — ADDRESSED.** `test_redaction_completeness` and `test_image_redaction_phase7` decompress the content stream (`decompressedPageContent`) and grep the DECODED operators, explicitly retiring the old raw-ASCII grep that was Flate-blind.
- **Image raster destruction — ADDRESSED.** `test_fidelity_r3_23_image_redaction` renders the reopened doc and asserts every interior pixel is wiped to colorspace-white **and none still shows the original raster** — proving samples are destroyed, not just the `/XObject` ref removed.
- **Boundary-straddle (intersection vs full-containment) — ADDRESSED.** `test_redaction_partial_overlap` asserts `checkTextRemoved==FALSE` for a glyph that intersects but is not contained (the b63ad8c fix), with a containment control.
- **Encrypted / annotation-preserving / OCR-layer — ADDRESSED** in `test_redaction_edge_cases` B4/B5/B6.

**The residual escape is not a missing oracle — it is conditionality:** the strongest byte/pixel-absence oracles are **skipped** unless the exact known-secret fixture `test_redaction.pdf` is present (and the roundtrip text-absence check is `#ifndef _WIN32`-skipped). Under fixture absence / on Linux, redaction assurance silently degrades to mark-count and no-throw proxies while the suite still reports green. Combined with the near-vacuous `test_redaction_verifier.cpp`, a mutation that neuters the leak detector can survive if the positive-detection tests are the ones that skip.

---

## 4. Ranked weak findings (highest first)

Confidentiality-relevant findings flagged **[CONF]**.

| # | File:line | Test | Oracle | Severity | Missed user-visible / confidentiality failure |
|---|---|---|---|---|---|
| 1 | `test_redaction_completeness.cpp:67` **[CONF]** | RedactedTextAbsentFromBytes | proxy | **High** | The only decompressed-stream byte-absence assertion is `GTEST_SKIP`ped when SetUp falls back to `test_single.pdf`. A box-only "redaction" leaving the secret's Tj operators intact ships green; redacted SSN survives in the saved stream. |
| 2 | `test_image_redaction_phase7.cpp:428` **[CONF]** | AppliedRedactionRemovesSecretTextBytes | true→skipped | **High** | Strongest text-redaction truth oracle skips unless fixture==`test_redaction.pdf`; otherwise redaction P0 collapses to mark-count/index proxies and a real leak escapes. |
| 3 | `test_redaction_verifier.cpp:114` **[CONF]** | VerifyResult_HasCorrectFields (+ ConstructDestroy) | proxy | **High** | The security verifier's own unit file is near-vacuous: `EXPECT_TRUE(x‖!x)`, `EXPECT_GE(unsigned,0)`, `SUCCEED()`. No test here asserts `checkTextRemoved==FALSE` for real leftover text; a "never detect a leak" mutation passes the whole file (caught only in edge_cases:646). |
| 4 | `test_exif_stripper.cpp:148` **[CONF]** | StripImage/StripPageImages (PDF path) | proxy | **High** | PDF-path EXIF strips assert only return bool/count and never reopen the saved PDF to inspect image bytes. A no-op strip passes; GPS/camera metadata privacy on the shipped file is unverified. |
| 5 | `test_image_content_editor.cpp:63` | Resize/Rotate/CropImage_Success | proxy | **High** | Only `EXPECT_TRUE(result)` + unchanged count. A resize/rotate that no-ops or writes a wrong CTM (mirror / half-size-under-CTM bug class) is indistinguishable from correct. |
| 6 | `test_image_flow_oracle.cpp:71` | Insert/Rotate/Crop-VerifyViaDisplayList | proxy | **High** | Advertised "rendering oracle" only asserts the display list is non-null — never rasterizes. Wrong position / wrong rotation / invisible image passes; rotate reduces to `CTM delta>0.01`. |
| 7 | `test_image_replacer.cpp:171` | ReplaceBasicRegion / ApproachC | proxy | **High** | Text-into-image replacement validated only by echoed struct fields. Un-erased original scan text or blank/mispositioned/mirrored new glyphs (recurring defects) pass. |
| 8 | `test_image_text_edit_display.cpp:202` | whole file (41 skips) | proxy | **High** | Without tessdata/scanned fixtures ~30-41/42 tests skip → file green with zero image-text-edit coverage. Surviving oracle `EXPECT_NE(xobjBefore,xobjAfter)` proves only *a* change. |
| 9 | `test_image_text_edit_bugs.cpp:286` | RectUnion / PixelWidthExpanded | no-oracle-smoke | **High** | Fix re-implemented inside the test body then asserted against a copy of itself; production `ImageReplacer` never invoked. Proves only `std::min/max`/`ceil`. |
| 10 | `test_redaction_completeness.cpp:96` **[CONF]** | RedactionUndoDoesNotRestoreAfterSave | proxy | **Medium** | Misnamed: never invokes undo, asserts only `pageCount>=1`. If undo-after-save resurrected the redacted secret (a tracked UAF/rebind bug class), the test stays green. |
| 11 | `test_redaction_manager.cpp:234` **[CONF]** | ApplyPageRedactions | proxy | **Medium** | Apply asserted by NO_THROW + marks-cleared-to-0 only. A redaction that clears marks but removes no content passes; content-blind in isolation (roundtrip/edge_cases carry the real oracle). |
| 12 | `test_layers.cpp:96` | SetVisibilityChangesState | proxy | **Medium** | Toggling a layer invisible that never hides its rendered content passes — only the in-memory flag is round-tripped; page never rendered. Hidden-content-still-visible = info-disclosure for layered PDFs. |
| 13 | `test_printing.cpp:84` | PrintToFile / PrintAllPages | proxy | **Medium** | Blank / wrong-page / garbage print passes: `EXPECT_TRUE(result)` + `fileSize>0`. Content-blind; mitigated only by `test_fidelity_r3_21`. |
| 14 | `test_image_manager.cpp:271` | InsertImage/RemoveImage_Success | proxy | **Medium** | Count-delta only, no save/reopen/pixels; invisible or off-page insert passes. 27 skip guards → green when fixtures absent. |
| 15 | `test_image_to_pdf_wizard.cpp:77` | Single/MultiImageProducesPdf | true | **Medium** | Correctly-sized BLANK pages pass — geometry checked, image never rendered/verified visible. |
| 16 | `test_icc_profile_manager.cpp:101` | AddSrgbOutputIntent_VerifyStream | proxy | **Medium** | "VerifyStream" checks only `hasOutputIntent()==true`; corrupt/empty/wrong-N/non-sRGB profile passes — invalid PDF/A OutputIntent uncaught. |
| 17 | `test_c2_image_cache.cpp:227` | SpecimenB eviction tests | heuristic | **Medium** | Real byte-budget eviction depends on a hardcoded private Desktop PDF and skips on CI — the point of C2 never runs. |
| 18 | `test_image_text_edit_display.cpp:1797` | UsesTr3_OnImageDominatedPage | proxy | **Medium** | `imagesAreDifferent(...,0.05)` tolerates 5% pixel change; thin faint visible glyphs on a busy scan stay under it — a Tr-mode regression making OCR text visible passes. |
| 19 | `test_bitmap_compositor.cpp:81` | RenderSimpleText | heuristic | **Medium** | Asserts only >0 non-transparent pixels; wrong/mirrored/garbled glyphs pass. |
| 20 | `test_image_similarity.cpp:65` | OtsuThresholdBimodal | heuristic | **Low** | Accepts any threshold in [20,230] for modes 20/230 — nearly the whole range. |
| 21 | `test_image_tool_wiring.cpp:92` | RepaintRequestedSignal | no-oracle-smoke | **Low** | Emits the signal then asserts receipt — tests Qt moc, not product. |
| 22 | `test_source_image_dpi.cpp:105` | whole fixture | metamorphic | **Low** | All 4 DPI tests skip when generated fixtures absent → green on CI without gen_fixtures.py. |
| 23 | `test_image_properties_panel.cpp:145` | SetImageInfo_UpdatesLabels | true | **Low** | `dpiText.contains("72")` weak substring — '720'/'1972' satisfies it (pixel-dim label was hardened with a regex; DPI left weak). |

---

## 5. Strong tests worth preserving (positive controls)

- `test_fidelity_r3_23_image_redaction.cpp` — image raster destruction, pixel-level.
- `test_redaction_partial_overlap.cpp` — intersection-semantics security regression.
- `test_redaction_edge_cases.cpp` B4/B5/B6 + `CheckTextRemoved_TextStillPresent@646` — encrypted redaction, annotation preservation, OCR-layer strip, and the verifier's only real positive-detection test.
- `test_redaction_roundtrip.cpp` — redacted pixels black + text-substring absence + untouched-neighbor.
- `test_mediabox_origin_crop.cpp`, `test_image_ctm_leak.cpp` — measure the transform bug in rendered geometry/colour, not recomputed matrices.
- `test_image_replacer_dpi.cpp`, `test_lanczos_resize.cpp`, `test_text_reflow.cpp` — exact-value / algorithmic oracles.

## 6. Recommended standing controls (for remediation phases)

1. **Kill the conditional-skip escape on confidentiality tests.** The known-secret fixture (`test_redaction.pdf`) must be a committed, non-optional prerequisite; a missing secret-fixture should FAIL the redaction suite, not skip it. Same for the `#ifndef _WIN32` text-absence gate.
2. **Add a positive leak-detection unit test in `test_redaction_verifier.cpp`** (`checkTextRemoved` must return FALSE for real leftover text on a real PDF) so the detector is constrained in its own file, and delete the tautological `VerifyResult_HasCorrectFields` asserts.
3. **Promote count/no-throw image oracles to pixel oracles** for the no-op-passes cluster (content_editor, replacer, flow_oracle, manager, image_commands) — render-and-compare, following the r3_23 / ctm_leak / mediabox pattern already in the tree.
4. **Ban in-test recomputation of production formulas** (image_text_edit_bugs, image_text_edit_display font-cap) — call the product function.
