# Content-Stream State-Continuity — Requirements Traceability Matrix (RTM)

Pipeline: **content-stream-continuity** (Phases 1–4).
Generated at Phase 4 (matrix lock + synthesis). All cells GREEN.

## What this matrix traces

The bug class is an **inbound ambient-state leak**: a page whose existing
`/Contents` leaves a hostile graphics/text-state operator *live* at the top
level (no matching `Q`/reset) corrupts anything a page-content injector appends,
because the appended fragment inherits that ambient state. Text extraction is
**blind** to the failure (stext recovers the logical characters whether they
render half-size, mirrored, invisible, or white-on-white), so every cell is
verified with a **pixel + glyph-quad render oracle**, never text extraction.

Each requirement cell = *"injector X, run against a page with hostile ambient
operator Y, must produce a visible, upright, full-size, on-page, uncorrupted
result and must not corrupt a pre-existing neighbour."*

## Hostile ambient operators (columns)

| Key | Ambient operator left live on `/Contents` | Failure it probes |
|---|---|---|
| `neg_tz` | `-100 Tz` (negative horizontal scale) | mirrored glyphs |
| `tc` | large `Tc` (char spacing) | blown-out spacing |
| `ts` | non-zero `Ts` (text rise) | vertical displacement |
| `tr` | `3 Tr` (invisible render mode) | invisible text |
| `white` | white fill (`1 1 1 rg`) | white-on-white |
| `cm` | `0.5 0 0 0.5 0 0 cm` (N-up / imposed) | half-size / shifted |
| `dangling_q` | unbalanced `q` (stack left pushed) | leaked nested state |
| `cmyk` | CMYK fill left active | colour-space leak |
| `multi` | **two different injectors on the same page** | double-guard / non-idempotent bracket |

## Coverage matrix — injector × hostile op

Legend: **AA** = `AdversarialAmbient.<Injector>` probe cell (72-cell measurement
grid, `tests/test_content_stream_adversarial_ambient.cpp`; each gtest case
iterates all 8 ops, so the op is the in-test cell). **CR** =
`CtmReset.<...>` hard-assertion regression
(`tests/test_content_stream_ctm_reset.cpp`). **MI** =
`MultiInjector.AlignThenHeaderFooterGuardIsIdempotent`
(`tests/test_content_stream_multi_injector.cpp`). All statuses **GREEN**
(`confirmed_corrupt=0`).

| Injector | neg_tz | tc | ts | tr | white | cm | dangling_q | cmyk | multi-injector |
|---|---|---|---|---|---|---|---|---|---|
| **ContentStreamEditor** (inline edit) | AA.ContentStreamEditor | AA | AA | AA | AA | AA | AA | AA | shared guard¹ |
| **AlignmentInjector / reflow** | AA.AlignmentInjector | AA | AA | AA | AA | AA **+ CR.AlignmentInjectorFullSizeUnderAmbientCm** | AA | AA | **MI (step 1)** |
| **HeaderFooterEngine** | AA.HeaderFooterEngine | AA | AA | AA | AA | AA **+ CR.HeaderFooterEngineFullSizeUnderAmbientCm** | AA | AA | **MI (step 2)** |
| **BatchEngine watermark** (text) | AA.WatermarkText | AA | AA | AA | AA | AA | AA | AA | shared guard¹ |
| **ImageInsert** | AA.ImageInsert | AA | AA | AA | AA | AA | AA | AA | shared guard¹ |
| **ImageResize** | AA.ImageResize | AA | AA | AA | AA | AA | AA | AA | shared guard¹ |
| **ImageRotate** | AA.ImageRotate | AA | AA | AA | AA | AA | AA | AA | shared guard¹ |
| **ImageCrop** | AA.ImageCrop | AA | AA | AA | AA | AA | AA | AA | shared guard¹ |
| **TRE `writeTextBlock`** | AA.TreWriteTextBlock | AA | AA | AA | AA | AA **+ CR.TreWriteTextBlockFullSizeUnderAmbientCm** | AA | AA | shared guard¹ |

¹ *These injectors route their append through the **identical** shared helper
`core/PageContentGuard.h::appendContentIsolatingAmbientCtm()` that MI exercises
directly. The idempotency logic is a single code path — MI's mutation kill (see
COVERAGE.md) validates it for all of them by construction. Only the two chained
in MI (AlignmentInjector → HeaderFooterEngine) are pairwise-tested end-to-end;
see COVERAGE.md "Residual / known limitations" for the honest scope note.*

### Per-op oracle applied in each AA cell

For every text cell: glyph present in stext, `ur.x > ul.x` (upright / not
mirrored), ink present in the rendered ROI (`> 60` px, catches invisible +
white-on-white + off-page), `0.70 < sizeRatio < 1.40` vs the KEEPME neighbour,
and (Alignment/HeaderFooter/TRE) black fill. Image cells: distinctive-colour
bbox present + not halved. Neighbour `KEEPME` must stay upright, black, visible
in every cell.

## Out-of-class findings → tests → disposition

These four defects were surfaced by the same content-stream sweep but sit
*outside* the ambient-CTM class. Disposition distinguishes **fixed-here**
(this pipeline) from **already-fixed** (landed earlier in commit `b63ad8c`,
pipeline `e5b9c231` Phase 2; this pipeline confirms + cross-references the lock).

| # | Finding | Test(s) | Disposition |
|---|---|---|---|
| **#1** | `RedactionVerifier::checkTextRemoved()` used **full containment** → a glyph straddling the region boundary was not flagged (**SECURITY** false-negative). Fixed to **intersection**. | `RedactionPartialOverlap.PartiallyOverlappingLeftoverTextIsDetected` + `.FullyContainedTextStillDetected` (`tests/test_redaction_partial_overlap.cpp`) | already-fixed `b63ad8c` (verified) |
| **#2** | HeaderFooter/watermark hardcoded resource key `/F1`, colliding with a page body font already bound to `/F1`. Fixed to a fresh, non-colliding key. | `HFWatermarkAdversarial.HeaderUsesFreshFontKeyNotCollidingF1` (`tests/test_hf_watermark_adversarial.cpp`) | already-fixed `b63ad8c` (verified) |
| **#3** | TRE glyph fallback emitted **Unicode codepoints as hex glyph indices** in an Identity-H Type0 show string (`'A' U+0041 → <0041>`), selecting the wrong glyph. Fixed: both the normal and error-fallback paths route through the single `PdfContentWriter::encodeGlyphIndicesHex()` encoder. | `ContentStreamOutOfClass.TreGlyphFallbackEmitsGlyphIndicesNotCodepoints` + `.TreGlyphFallbackUnresolvableDegradesToNotdef` (`tests/test_content_stream_out_of_class.cpp`) | **fixed-here** (commit `1518b75`, Phase 3) |
| **#4** | Watermark + HeaderFooter placed content relative to `(0,0)`, mis-centering on a non-zero MediaBox origin. Fixed to offset by the MediaBox origin. | `HFWatermarkAdversarial.HeaderCenteredDespiteNonZeroMediaBoxOrigin` + `.WatermarkCenteredDespiteNonZeroMediaBoxOrigin` (`tests/test_hf_watermark_adversarial.cpp`) | already-fixed `b63ad8c` (verified) |

## Emitted-testcase cross-check

Verified against the live binary
(`pdfe_tests.exe --gtest_list_tests --gtest_filter='*AdversarialAmbient*:*CtmReset*:*OutOfClass*:*Redaction*Overlap*:*HFWatermark*:*MultiInjector*'`):

```
AdversarialAmbient.            ContentStreamEditor / AlignmentInjector /
                               HeaderFooterEngine / WatermarkText /
                               TreWriteTextBlock / ImageInsert / ImageResize /
                               ImageRotate / ImageCrop            (9)
CtmReset.                      AlignmentInjectorFullSizeUnderAmbientCm /
                               HeaderFooterEngineFullSizeUnderAmbientCm /
                               TreWriteTextBlockFullSizeUnderAmbientCm   (3)
ContentStreamOutOfClass.       TreGlyphFallbackEmitsGlyphIndicesNotCodepoints /
                               TreGlyphFallbackUnresolvableDegradesToNotdef  (2)
MultiInjector.                 AlignThenHeaderFooterGuardIsIdempotent    (1)
RedactionPartialOverlap.       PartiallyOverlappingLeftoverTextIsDetected /
                               FullyContainedTextStillDetected           (2, b63ad8c)
HFWatermarkAdversarial.        HeaderCenteredDespiteNonZeroMediaBoxOrigin /
                               HeaderUsesFreshFontKeyNotCollidingF1 /
                               WatermarkCenteredDespiteNonZeroMediaBoxOrigin (3, b63ad8c)
```

Phase-4 suite run: **24 tests, 0 failures, 0 errors** (parsed from `p4.xml`
`<testsuites tests="24" failures="0" errors="0" disabled="0">`).
