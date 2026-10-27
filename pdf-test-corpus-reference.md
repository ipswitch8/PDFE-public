# PDF Test Corpus Reference

Curated, deduplicated set of individual PDF test files for stress-testing a PDF
processing pipeline (parsing, rendering, text/font extraction, layout analysis).

## Purpose & scope (read this first)

This file was built by cloning the browsable, individually-documented GitHub
repositories referenced from the PDF Association's corpus index
(https://github.com/pdf-association/pdf-corpora) and inspecting their actual
file listings and READMEs, rather than trusting descriptions secondhand.
Each entry below is a direct `raw.githubusercontent.com` link to one real file.

**Deliberately excluded:**
- Bulk undocumented corpora with no per-file metadata (GovDocs1, CommonCrawl,
  CC-MAIN-2021-31-PDF-UNTRUNCATED) — these are hundreds of GB / millions of
  files with no indication of which files exercise which bug. Good for
  large-scale fuzzing/scale-testing, useless for a curated "known flaw" list.
- Anything flagged as containing genuinely malicious payloads (UNSAFE-DOCS,
  Qiqqa's "Evil Base", CIC-Evasive-PDFMal2022). None of those files are
  included here.
- Sources not reachable from this environment: `pdfium.googlesource.com` and
  `cgit.ghostscript.com` are not on an allowed egress list here, so the
  pdfium and MuPDF/Ghostscript regression suites are named as *categories*
  at the bottom for manual follow-up, but no individual files are hand-picked
  from them.
- Duplicate/near-duplicate coverage: where multiple suites had files testing
  the same underlying flaw (e.g. "font not embedded" appears in both the
  Cabinet of Horrors and pdf.js corpora), only the clearer/better-documented
  example was kept.

**Suggested usage for Claude Code:** each `##` section below is a flaw
category. Each list item is `` `local/suggested/path.pdf` — [direct link] —
description ``. When expanding a test suite, download files into matching
subdirectories and use the description as the test's docstring/assertion
intent.

---

## 1. Font issues (missing, not embedded, encoding edge cases)

- `fonts/not-embedded-single.pdf` — https://raw.githubusercontent.com/openpreserve/format-corpus/master/pdfCabinetOfHorrors/text_only_fontsNotEmbedded.pdf — Text-only PDF where the used font is not embedded at all; baseline "missing font" case.
- `fonts/embedded-full.pdf` — https://raw.githubusercontent.com/openpreserve/format-corpus/master/pdfCabinetOfHorrors/text_only_fontsEmbeddedAll.pdf — Same content, fully embedded font; use as the "control" pair against the above.
- `fonts/embedded-subset.pdf` — https://raw.githubusercontent.com/openpreserve/format-corpus/master/pdfCabinetOfHorrors/text_only_fontsEmbeddedSubset.pdf — Font embedded as a subset only (common cause of "missing glyph" bugs when text is later edited/merged).
- `fonts/arial-times-not-embedded.pdf` — https://raw.githubusercontent.com/openpreserve/format-corpus/master/pdfCabinetOfHorrors/test_fontArialNotEmbedded.pdf — Two standard fonts (Arial, Times New Roman) referenced but not embedded; tests font-substitution fallback logic.
- `fonts/calisto-not-embedded.pdf` — https://raw.githubusercontent.com/openpreserve/format-corpus/master/pdfCabinetOfHorrors/calistoMTNoFontsEmbedded.pdf — Non-standard font (Calisto MT) not embedded; substitution has no safe standard-font fallback.
- `fonts/pdfa1a-embedded.pdf` — https://raw.githubusercontent.com/openpreserve/format-corpus/master/pdfCabinetOfHorrors/text_only_pdfa1b.pdf — PDF/A-1a compliant file, embedded fonts; positive control for archival font-embedding checks.
- `fonts/mixed-fonts.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/mixedfonts.pdf — Multiple distinct font types combined in one document.
- `fonts/complex-ttf.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/complex_ttf_font.pdf — TrueType font with complex glyph tables/hinting.
- `fonts/simple-type3.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/simpletype3font.pdf — Type 3 (procedural/bitmap) font — a font class many renderers handle poorly.
- `fonts/cidfont-cmap-overflow.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/cidfont_cmap_overflow.pdf — CID font with a CMap that overflows expected ranges; tests bounds handling in CJK text extraction.
- `fonts/arial-unicode-cid-ab.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/arial_unicode_ab_cidfont.pdf — Composite (Type 0) CID font, Arabic-range mapping.
- `fonts/arial-unicode-cid-en.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/arial_unicode_en_cidfont.pdf — Composite CID font, Latin-range mapping (pair with the Arabic one above to test CJK/RTL vs Latin CID handling).
- `fonts/zero-descent.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/zero_descent.pdf — Font metrics with a zero descent value; tests text-baseline/line-height math against degenerate metrics.
- `fonts/font-ascent-descent.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/font_ascent_descent.pdf — Unusual ascent/descent metric combinations.
- `fonts/standard-fonts.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/standard_fonts.pdf — Exercises the 14 standard PDF fonts as a baseline/regression reference.

## 2. Structural corruption / broken indexing (xref, trailer, object streams)

- `corrupt/one-byte-missing.pdf` — https://raw.githubusercontent.com/openpreserve/format-corpus/master/pdfCabinetOfHorrors/corruptionOneByteMissing.pdf — Exactly one byte deleted from the comment line following the file header; minimal-diff corruption case, good for testing graceful-degradation vs. hard failure.
- `corrupt/xref-command-missing.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/xref_command_missing.pdf — The `xref` keyword itself is missing; forces a fallback full-file object scan/rebuild path.
- `corrupt/multiple-filters-length-zero.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/multiple-filters-length-zero.pdf — Stream declares a filter chain but zero-length data; tests filter-pipeline edge case.
- `corrupt/helloworld-bad.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/helloworld-bad.pdf — Minimal document deliberately damaged; small enough to diff byte-for-byte against a known-good version.
- `corrupt/scan-bad.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/scan-bad.pdf — Damaged scanned-document PDF.
- `corrupt/pdfjsbad1586.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/pdfjsbad1586.pdf — Regression case from a filed pdf.js parser bug; real-world corruption pattern rather than synthetic.
- `corrupt/bad-pagelabels.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/bad-PageLabels.pdf — Malformed `/PageLabels` structure; tests page-numbering/index resilience.
- `corrupt/boundingbox-invalid.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/boundingBox_invalid.pdf — Invalid bounding box values on content; tests geometry sanity-checking.
- `corrupt/header-version-mismatch.pdf` — https://raw.githubusercontent.com/openpreserve/format-corpus/master/pdfCabinetOfHorrors/pdf-17-header18.pdf — File is actually PDF 1.7 but the header string says `%PDF-1.8`; causes false negatives in identification/version-sniffing tools (documented upstream: https://github.com/digital-preservation/droid/issues/114).

## 3. Image / XObject problems (missing, mismatched, wrong dimensions)

- `images/hires-valid.pdf` — https://raw.githubusercontent.com/openpreserve/format-corpus/master/pdfCabinetOfHorrors/veraPDFHiRes.pdf — Intact, valid PDF/A-1b with a bitmap image; use as the positive control for the two variants below.
- `images/hires-wrong-height.pdf` — https://raw.githubusercontent.com/openpreserve/format-corpus/master/pdfCabinetOfHorrors/veraPDFHiResChangedHeight.pdf — Identical to the above but the `/Height` entry in the Image XObject dictionary is wrong — tests whether a renderer trusts declared metadata over actual stream dimensions.
- `images/hires-wrong-objectid.pdf` — https://raw.githubusercontent.com/openpreserve/format-corpus/master/pdfCabinetOfHorrors/veraPDFHiResWrongObjectID.pdf — Same base file, but the page content references a non-existent XObject ID — classic "missing/broken image reference" case.
- `images/jpeg2000-in-pdfa1b.pdf` — https://raw.githubusercontent.com/openpreserve/format-corpus/master/pdfCabinetOfHorrors/balloon_a1b_jp2k.pdf — Claims PDF/A-1b conformance but embeds a JPEG2000 image, which PDF/A-1 disallows (plus other spec violations); good combined-fault case.
- `images/jbig2-file-header.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/jbig2_file_header.pdf — JBIG2-encoded image (common in scanned documents); tests that codec.
- `images/jbig2-symbol-offset.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/jbig2_symbol_offset.pdf — JBIG2 with symbol-dictionary offset edge case; known source of decoder bugs.
- `images/jpx-bug.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/bug_jpx.pdf — JPEG2000 (JPX) decode regression case.
- `images/ccitt-endofblock-false.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/ccitt_EndOfBlock_false.pdf — CCITT Group 4 fax image with `EndOfBlock` false; edge case in that decoder.
- `images/rotated-bw-ratio.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/image-rotated-black-white-ratio.pdf — Rotated bitonal image; tests rotation + color-ratio handling together.
- `images/bitmap-symbol-negative-offset.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/bitmap-symbol-negative-sbdsoffset.pdf — Negative symbol offset in a bitmap font/image structure; boundary/negative-number handling.

## 4. Layout / real-world extraction nightmares (multi-column, scanned tables, dense forms)

> These are real documents (not synthetic test cases) contributed specifically
> because they break naive text/table extraction — multi-column reflow,
> image-only election tables, embedded write-in lists, etc. Good for
> integration-level "does extraction produce sane output" tests rather than
> unit-level parser tests.

- `layout/multi-column-sports-records.pdf` — https://raw.githubusercontent.com/dwillis/nicar25-pdfs/main/fall_2023.pdf — Electronic (non-scanned) PDF with dense multi-column sports record-book layout; text order and column-reflow is the challenge, not OCR.
- `layout/election-writein-scanned-132pg.pdf` — https://raw.githubusercontent.com/dwillis/nicar25-pdfs/main/D001799202.135.pdf — Large scanned image-based election precinct report with itemized write-ins; stresses OCR + page-count + table-per-precinct extraction.
- `layout/precinct-canvass-results.pdf` — https://raw.githubusercontent.com/dwillis/nicar25-pdfs/main/Washington_County_2024_General_Precinct_Canvass_Results.pdf — Dense tabular election canvass results, real-world government formatting quirks.
- `layout/precinct-results-oregon.pdf` — https://raw.githubusercontent.com/dwillis/nicar25-pdfs/main/2024%20Crook%20County%2C%20OR%20precinct-level%20results.PDF — Another independently-formatted precinct results table; useful as a second, differently-laid-out sample in the same domain (tests generalization, not overfitting to one table style).
- `layout/historical-census-table.pdf` — https://raw.githubusercontent.com/dwillis/nicar25-pdfs/main/BlackPop1930.pdf — 1930s-style historical statistical table, non-standard layout conventions.
- `layout/supplemental-statement-form.pdf` — https://raw.githubusercontent.com/dwillis/nicar25-pdfs/main/568-Supplemental-Statement-20250205-38.pdf — Structured government form with mixed field/table layout.

## 5. Encryption / access-control variants

- `encryption/open-password.pdf` — https://raw.githubusercontent.com/openpreserve/format-corpus/master/pdfCabinetOfHorrors/encryption_openpassword.pdf — Requires a password just to open the file.
- `encryption/no-copy.pdf` — https://raw.githubusercontent.com/openpreserve/format-corpus/master/pdfCabinetOfHorrors/encryption_nocopy.pdf — Openable but content-copy permission is denied.
- `encryption/no-printing.pdf` — https://raw.githubusercontent.com/openpreserve/format-corpus/master/pdfCabinetOfHorrors/encryption_noprinting.pdf — Printing permission denied.
- `encryption/no-text-access.pdf` — https://raw.githubusercontent.com/openpreserve/format-corpus/master/pdfCabinetOfHorrors/encryption_notextaccess.pdf — Screen-reader/accessibility text-extraction permission denied — important if your pipeline does text extraction, since this specifically tests whether you respect that permission flag.
- `encryption/standard40.pdf` — https://raw.githubusercontent.com/itext/itext7/develop/kernel/src/test/resources/com/itextpdf/kernel/crypto/PdfDecryptingTest/java_regular/withPassword/standard40.pdf — RC4 40-bit standard security handler (legacy/weak algorithm compatibility test).
- `encryption/standard128.pdf` — https://raw.githubusercontent.com/itext/itext7/develop/kernel/src/test/resources/com/itextpdf/kernel/crypto/PdfDecryptingTest/java_regular/withPassword/standard128.pdf — RC4 128-bit standard security handler.
- `encryption/aes128.pdf` — https://raw.githubusercontent.com/itext/itext7/develop/kernel/src/test/resources/com/itextpdf/kernel/crypto/PdfDecryptingTest/java_regular/withPassword/aes128.pdf — AES-128 encryption, password-based.
- `encryption/aes256.pdf` — https://raw.githubusercontent.com/itext/itext7/develop/kernel/src/test/resources/com/itextpdf/kernel/crypto/PdfDecryptingTest/java_regular/withPassword/aes256.pdf — AES-256 encryption, password-based.
- `encryption/aes256-pdf2.pdf` — https://raw.githubusercontent.com/itext/itext7/develop/kernel/src/test/resources/com/itextpdf/kernel/crypto/PdfDecryptingTest/java_regular/withPassword/aes256Pdf2.pdf — AES-256 under the newer PDF 2.0 encryption revision (R6) — distinct algorithm path from the R4/R5 aes256 case above.
- `encryption/aes256-certificate.pdf` — https://raw.githubusercontent.com/itext/itext7/develop/kernel/src/test/resources/com/itextpdf/kernel/crypto/PdfDecryptingTest/java_regular/withCertificate/aes256Rsa.pdf — Certificate-based (public-key) encryption rather than password-based — a structurally different access-control path worth testing separately.

## 6. Unusual dimensions & numeric edge cases

> This is the category the source corpora cover least by name — most public
> suites don't specifically target "huge/zero/negative page size." The BFO
> PDF/A suite is the one source that does, deliberately, as part of spec
> boundary-testing:

- `dimensions/mediabox-over-14400.pdf` — https://raw.githubusercontent.com/bfosupport/pdfa-testsuite/master/pdfa2-6-1-13-bfo-t03-fail.pdf — Media box dimensions exceed 14400 units (the PDF/A limit, and close to the PDF spec's own practical ceiling) — good "very large page" case.
- `dimensions/artbox-under-3.pdf` — https://raw.githubusercontent.com/bfosupport/pdfa-testsuite/master/pdfa2-6-1-13-bfo-t02-fail.pdf — Art box dimensions below 3 units — good "degenerate tiny page/region" case.
- `dimensions/real-over-ieee754-and-pdfa-limit.pdf` — https://raw.githubusercontent.com/bfosupport/pdfa-testsuite/master/pdfa2-6-1-13-bfo-t04-fail.pdf — A real number in a content stream exceeds both the IEEE754 float range and the PDF/A numeric limit — tests numeric overflow handling in coordinate math.
- `dimensions/real-under-pdfa-min-structure.pdf` — https://raw.githubusercontent.com/bfosupport/pdfa-testsuite/master/pdfa2-6-1-13-bfo-t05-fail.pdf — Real number below the PDF/A minimum, in document structure (not content stream) — a second, structurally distinct underflow case.
- `dimensions/real-under-pdfa-min-contentstream.pdf` — https://raw.githubusercontent.com/bfosupport/pdfa-testsuite/master/pdfa2-6-1-13-bfo-t09-fail.pdf — Same underflow class but located in a content stream operand instead — pairs with the file above to test both code paths.
- `dimensions/real-over-ieee754-under-pdfa-limit-pass.pdf` — https://raw.githubusercontent.com/bfosupport/pdfa-testsuite/master/pdfa2-6-1-13-bfo-t06-pass.pdf — Boundary-valid case: over the IEEE754 float limit but still under the PDF/A limit — should be *accepted*; use as a false-positive-rejection guard alongside the fail cases above.
- `dimensions/nested-q-over-28.pdf` — https://raw.githubusercontent.com/bfosupport/pdfa-testsuite/master/pdfa2-6-1-13-bfo-t01-fail.pdf — More than 28 nested graphics-state `q`/`Q` operators — stresses the graphics-state stack depth, a common cause of stack-overflow bugs in naive interpreters.

## 7. Vector graphics complexity (patterns, shading, transparency, clipping)

- `vector/tiling-pattern-large-steps.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/tiling-pattern-large-steps.pdf — Tiling pattern with large step intervals; tests pattern-tiling performance/correctness at scale.
- `vector/tiling-patterns-variations.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/tiling_patterns_variations.pdf — Multiple tiling pattern variations in one file.
- `vector/mesh-shading-empty.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/mesh_shading_empty.pdf — Mesh shading with empty/degenerate data — boundary case for gradient-mesh rendering.
- `vector/function-based-shading.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/function_based_shading.pdf — Shading defined via a PostScript-style function rather than a sampled table — different code path than mesh/axial shading.
- `vector/shading-extend.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/shading_extend.pdf — Shading with the `/Extend` flag exercising area outside the defined gradient bounds.
- `vector/knockout-blend-multiply.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/knockout_blend_multiply.pdf — Knockout group combined with multiply blend mode — tests correct transparency-group compositing order.
- `vector/blendmode.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/blendmode.pdf — General blend-mode coverage.
- `vector/transparency-group.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/transparency_group.pdf — Isolated/non-isolated transparency group handling.
- `vector/clippath.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/clippath.pdf — Clipping path application.
- `vector/text-clip-cff-cid.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/text_clip_cff_cid.pdf — Text used as a clipping path, combined with CFF/CID font rendering — a compound case (overlaps categories 1 and 7 deliberately).
- `vector/indexedcs-negative-and-high.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/IndexedCS_negative_and_high.pdf — Indexed color space with out-of-range (negative and too-high) index values — color-space bounds checking.

## 8. Overlapping / degenerate regions & annotation edge cases

- `overlap/type3-content-stream-cycle.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/ContentStreamCycleType3insideType3.pdf — Type 3 font glyph procedure that recursively references itself (a Type 3 font inside its own content stream) — genuine infinite-recursion / cycle-detection test, not just visual overlap.
- `overlap/annotation-empty-rect.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/annotation-line-without-appearance-empty-Rect.pdf — Line annotation with an empty `/Rect` and no appearance stream — degenerate/zero-area region.
- `overlap/annotation-highlight-no-appearance.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/annotation-highlight-without-appearance.pdf — Highlight annotation missing its appearance stream, forcing the renderer to synthesize one — a common source of mispositioned/overlapping highlight boxes.
- `overlap/rotated-freetexts.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/rotated_freetexts.pdf — Rotated free-text annotations, which commonly end up overlapping page content when rotation isn't applied consistently to the bounding box.
- `overlap/zerowidthline.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/zerowidthline.pdf — Zero-width line drawing — degenerate geometry, tests that a stroke of width 0 doesn't crash the rasterizer.
- `overlap/bomb-giant.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/bomb_giant.pdf — Deliberately constructed to expand into an extremely large render surface/content volume from a small file ("PDF bomb") — resource-exhaustion / decompression-bomb-style test. **Run this one in a resource-limited/sandboxed test harness** (memory/time limits), same caution as a zip bomb.

## 9. Embedded/attached content & scripting

- `embedded/file-attachment-namedict.pdf` — https://raw.githubusercontent.com/openpreserve/format-corpus/master/pdfCabinetOfHorrors/fileAttachment.pdf — Document-level file attachment via the `/EmbeddedFiles` name dictionary (attaches a legacy Quattro Pro spreadsheet).
- `embedded/file-attachment-annotation.pdf` — https://raw.githubusercontent.com/openpreserve/format-corpus/master/pdfCabinetOfHorrors/fileAttachment_fileAttachmentAnnotation.pdf — Page-level attachment via a File Attachment *annotation* instead — structurally different from the name-dictionary case above; both are worth testing since extraction code often handles only one.
- `embedded/javascript.pdf` — https://raw.githubusercontent.com/openpreserve/format-corpus/master/pdfCabinetOfHorrors/javascript.pdf — Contains embedded JavaScript (built with Didier Stevens' make-pdf tool) — tests that your pipeline safely ignores/sandboxes active content rather than executing it.
- `embedded/video-avi.pdf` — https://raw.githubusercontent.com/openpreserve/format-corpus/master/pdfCabinetOfHorrors/embedded_video_avi.pdf — Embedded AVI video stream.
- `embedded/digitally-signed-3d-portfolio.pdf` — https://raw.githubusercontent.com/openpreserve/format-corpus/master/pdfCabinetOfHorrors/digitally_signed_3D_Portfolio.pdf — PDF 1.7 Portfolio (multiple sheets) containing forms and 3D content, with one sheet digitally signed — good compound/worst-case document for a "does the whole pipeline survive this" smoke test.
- `embedded/encrypted-attachment.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/encrypted-attachment.pdf — File attachment that is itself encrypted, nested inside the parent PDF.

## 10. Forms (AcroForm / XFA)

- `forms/acroform-calc-order.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/acroform_calculation_order.pdf — AcroForm with field calculation-order dependencies.
- `forms/xfa-filled.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/xfa_filled_imm1344e.pdf — Filled XFA (XML Forms Architecture) form, a real US immigration form template — XFA is a legacy but still-encountered form technology with a completely different internal structure from AcroForm.
- `forms/xfa-issue14315.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/xfa_issue14315.pdf — Regression case from a filed XFA-handling bug.
- `forms/checkbox-bad-appearance.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/checkbox-bad-appearance.pdf — Checkbox field with a malformed appearance stream.
- `forms/resetform.pdf` — https://raw.githubusercontent.com/mozilla/pdf.js/master/test/pdfs/resetform.pdf — Form with a reset-form action; tests that static extraction doesn't misinterpret action-triggered state changes as content.

## 11. PDF/A & PDF/UA conformance edge cases (bulk source, not individually curated)

The veraPDF corpus (https://github.com/veraPDF/veraPDF-corpus, branch `staging`)
contains ~2,900 files organized by ISO spec clause and is the single best
source if you need systematic, spec-clause-level conformance test coverage
(PDF/A-1 through PDF/A-4, PDF/UA-1/2). It also **bundles the full Isartor
suite** (205 files, `Isartor test files/` subdirectory) so you don't need to
separately register for that download. Structure:

```
PDF_A-1a/, PDF_A-1b/, PDF_A-2a/, PDF_A-2b/, PDF_A-2u/, PDF_A-3b/,
PDF_A-4/, PDF_A-4e/, PDF_A-4f/, PDF_UA-1/, PDF_UA-2/,
Isartor test files/, TWG test files/, ISO 32000-1/, ISO 32000-2/
```
Each filename encodes the spec clause and pass/fail expectation, e.g.
`6-2-3-2-t01-fail-b.pdf`. Given the volume and fine-grained spec-clause
organization, pick individual files matching whatever clauses your validator
targets rather than a hand-picked subset here — a representative sample:

- `conformance/example` — https://raw.githubusercontent.com/veraPDF/veraPDF-corpus/staging/ISO%2032000-1/veraPDF%20test%20suite%206-2-3-2-t01-fail-b.pdf — Example ISO 32000-1 clause 6.2.3.2 failure case; use as a template for pulling further clause-specific files from the same repo.

## 12. Bulk/scale sources (not individually curated — use for volume/fuzz testing)

These are legitimate, non-malicious sources but too large or undocumented at
the file level to hand-pick from. Use them if you need raw volume (thousands
of real-world files) rather than known, labeled flaws:

- **GovDocs1** — https://digitalcorpora.org/corpora/files — ~231,000 real-world PDFs from .gov domains, packaged as 1000 ZIPs (~308GB total). Good for volume/scale and "PDFs as they exist in the wild," bad for targeted regression testing since nothing is labeled.
- **Mozilla pdf.js full test corpus** — https://github.com/mozilla/pdf.js/tree/master/test/pdfs — 967 files total; this reference hand-picked ~45 of the most clearly-labeled ones above, but the full directory has many more `issueNNNN.pdf` / `bugNNNNNN.pdf` regression files worth grepping through for niche cases.
- **Google pdfium regression suite** — https://pdfium.googlesource.com/pdfium_tests/ — Not reachable from this environment's network egress list; browse manually if needed.
- **Artifex MuPDF/Ghostscript test corpus** — https://cgit.ghostscript.com/cgi-bin/cgit.cgi/tests.git/ — Same limitation, not reachable here.

## 13. Explicitly excluded — malicious datasets (do not add to a general test suite without isolation)

DO NOT USE:
- CIC-Evasive-PDFMal2022
- UNSAFE-DOCS
- Qiqqa's "Evil Base"

---

## Notes on methodology / caveats for Claude Code

- All links were verified to resolve (HTTP 200) as of 2026-07-16 via
  `raw.githubusercontent.com`, by cloning each source repository and reading
  its actual file listing — not inferred from search-result snippets.
- Every listed file was selected because either (a) an upstream README/manual
  explicitly documents the specific flaw it contains (Cabinet of Horrors, BFO
  suite, dwillis/nicar25-pdfs), or (b) its filename in a well-known regression
  suite (pdf.js, iText) unambiguously names the feature/bug under test.
- Where two suites had visibly duplicate coverage (e.g. "font not embedded"),
  only one representative was kept to avoid bloating the suite with redundant
  cases; the alternate is mentioned in the item's description when relevant.
- This is a snapshot/starting set, not exhaustive — the pdf.js and veraPDF
  corpora alone contain ~3,800 more files not individually listed here.
