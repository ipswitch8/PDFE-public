# External flaw-corpus fixtures

<!-- tracking-banner -->
> **STATUS: LIVE OPERATIONAL STATE — keep maintaining this file.**
>
> This is **not** a defect tracker and is **not** superseded by GitHub Issues;
> it is state that code, CI or a fixture generator reads. **GitHub Issues
> (`ipswitch8/PDFE`) remains the canonical tracker for anything still open** — a
> defect recorded only here is still an untracked defect.
>
> See [`docs/TRACKING.md`](../../../docs/TRACKING.md).

---


Real, upstream-documented PDF test files added to close coverage gaps identified
by comparing PDFE's existing corpus against `pdf-test-corpus-reference.md`
(a curated index of documented "known flaw" PDFs from public suites).

Each file below is downloaded verbatim from its upstream regression suite and
exercises a specific flaw class PDFE previously had **no fixture + no asserting
test** for. The asserting tests live in `tests/test_external_flaw_corpus.cpp`.

Every `.pdf` under this directory MUST appear in exactly one of the three tables
below (Tiers 1-3, Tier 4, Supplementary). `FlawCorpusFixtureDocs` in
`tests/test_doc_drift_guards.cpp` walks the directory and fails on any file that
is not listed, any row naming a file that is not there, and any figure in the
FIXTURE-COUNTS block that disagrees with either. An undocumented fixture is how
`encryption/identity_stmf.pdf` sat in this corpus unlisted from phase-3 until
F16 found it (meta-audit gap `PDF-107`).

<!-- FIXTURE-TABLE-BEGIN: tiers-1-3 -->

| Local path | Flaw class | Upstream source |
|---|---|---|
| `overlap/type3_content_stream_cycle.pdf` | Type 3 font glyph that recursively references itself (unbounded-recursion / DoS) | mozilla/pdf.js `test/pdfs/ContentStreamCycleType3insideType3.pdf` |
| `fonts/zero_descent.pdf` | Font with a zero descent metric (baseline/line-height math) | mozilla/pdf.js `test/pdfs/zero_descent.pdf` |
| `fonts/font_ascent_descent.pdf` | Unusual ascent/descent metric combination | mozilla/pdf.js `test/pdfs/font_ascent_descent.pdf` |
| `vector/clippath.pdf` | Clipping path (W/W\*) application | mozilla/pdf.js `test/pdfs/clippath.pdf` |
| `vector/text_clip_cff_cid.pdf` | Text used as a clipping path (Tr 7) + CFF/CID font | mozilla/pdf.js `test/pdfs/text_clip_cff_cid.pdf` |
| `vector/indexedcs_negative_and_high.pdf` | Indexed colour space with out-of-range (negative/too-high) indices | mozilla/pdf.js `test/pdfs/IndexedCS_negative_and_high.pdf` |
| `corrupt/bad_pagelabels.pdf` | Malformed `/PageLabels` structure | mozilla/pdf.js `test/pdfs/bad-PageLabels.pdf` |
| `encryption/aes256_certificate.pdf` | Certificate / public-key encryption (must fail gracefully — PDFE cannot decrypt) | itext/itext7 `.../crypto/PdfDecryptingTest/java_regular/withCertificate/aes256Rsa.pdf` |
| `vector/mesh_shading_empty.pdf` | Mesh shading with empty/degenerate data | mozilla/pdf.js `test/pdfs/mesh_shading_empty.pdf` |
| `vector/tiling_pattern_large_steps.pdf` | Tiling pattern, large XStep/YStep on a wide page | mozilla/pdf.js `test/pdfs/tiling-pattern-large-steps.pdf` |
| `vector/shading_extend.pdf` | Shading with `/Extend` beyond gradient bounds | mozilla/pdf.js `test/pdfs/shading_extend.pdf` |
| `images/rotated_bw_ratio.pdf` | Rotated bitonal (black/white) image | mozilla/pdf.js `test/pdfs/image-rotated-black-white-ratio.pdf` |
| `overlap/rotated_freetexts.pdf` | Rotated free-text annotations (bbox overlap) | mozilla/pdf.js `test/pdfs/rotated_freetexts.pdf` |
| `overlap/zerowidthline.pdf` | Zero-width line stroke (degenerate geometry) | mozilla/pdf.js `test/pdfs/zerowidthline.pdf` |
| `forms/checkbox_bad_appearance.pdf` | Checkbox field with a malformed appearance stream | mozilla/pdf.js `test/pdfs/checkbox-bad-appearance.pdf` |
| `fonts/complex_ttf.pdf` | TrueType font with complex glyph tables / hinting | mozilla/pdf.js `test/pdfs/complex_ttf_font.pdf` |
| `corrupt/header_version_mismatch.pdf` | Header says `%PDF-1.8` but file is really 1.7 | openpreserve/format-corpus `pdfCabinetOfHorrors/pdf-17-header18.pdf` |
| `dimensions/mediabox_over_14400.pdf` | Real MediaBox exceeding the 14400-unit limit (14401 wide) | bfosupport/pdfa-testsuite `pdfa2-6-1-13-bfo-t03-fail.pdf` |
| `dimensions/artbox_under_3.pdf` | ArtBox dimensions below 3 units (degenerate tiny region) | bfosupport/pdfa-testsuite `pdfa2-6-1-13-bfo-t02-fail.pdf` |
| `dimensions/real_underflow_structure.pdf` | Real number below the PDF/A minimum in document structure | bfosupport/pdfa-testsuite `pdfa2-6-1-13-bfo-t05-fail.pdf` |
| `encryption/aes256_r5.pdf` | AES-256 **Revision 5** (deprecated Adobe extension, /V 5 /R 5) — distinct decrypt path from R6; user password `user` | itext/itext7 `.../crypto/PdfDecryptingTest/java_regular/withPassword/aes256.pdf` |
| `fonts/not_embedded_single.pdf` | Text-only, font NOT embedded (baseline missing-font) | openpreserve/format-corpus `pdfCabinetOfHorrors/text_only_fontsNotEmbedded.pdf` |
| `fonts/embedded_full.pdf` | Same content, font fully embedded (control pair) | openpreserve/format-corpus `.../text_only_fontsEmbeddedAll.pdf` |
| `fonts/embedded_subset.pdf` | Font embedded as a subset only (missing-glyph source) | openpreserve/format-corpus `.../text_only_fontsEmbeddedSubset.pdf` |
| `fonts/arial_times_not_embedded.pdf` | Standard fonts (Arial/Times) referenced but not embedded — substitution fallback | openpreserve/format-corpus `.../test_fontArialNotEmbedded.pdf` |
| `fonts/calisto_not_embedded.pdf` | Non-standard font (Calisto MT) not embedded — no safe fallback | openpreserve/format-corpus `.../calistoMTNoFontsEmbedded.pdf` |
| `fonts/pdfa1a_embedded.pdf` | PDF/A-1b compliant, embedded fonts (positive control) | openpreserve/format-corpus `.../text_only_pdfa1b.pdf` |
| `fonts/mixed_fonts.pdf` | Multiple distinct font types in one document | mozilla/pdf.js `test/pdfs/mixedfonts.pdf` |
| `fonts/simple_type3.pdf` | Type 3 (procedural/bitmap) font | mozilla/pdf.js `test/pdfs/simpletype3font.pdf` |
| `fonts/cidfont_cmap_overflow.pdf` | CID font with a CMap that overflows expected ranges | mozilla/pdf.js `test/pdfs/cidfont_cmap_overflow.pdf` |
| `fonts/arial_unicode_cid_ab.pdf` | Composite (Type 0) CID font, Arabic-range mapping | mozilla/pdf.js `test/pdfs/arial_unicode_ab_cidfont.pdf` |
| `fonts/arial_unicode_cid_en.pdf` | Composite CID font, Latin-range mapping | mozilla/pdf.js `test/pdfs/arial_unicode_en_cidfont.pdf` |
| `fonts/standard_fonts.pdf` | Exercises the 14 standard PDF fonts (baseline) | mozilla/pdf.js `test/pdfs/standard_fonts.pdf` |
| `encryption/open_password.pdf` | Requires a password just to OPEN; upstream password not published, so the test asserts graceful rejection (no usable open without a password) | openpreserve/format-corpus `.../encryption_openpassword.pdf` |
| `encryption/no_copy.pdf` | Openable, but copy/extract permission denied — `getPermissions().copy == false` | openpreserve/format-corpus `.../encryption_nocopy.pdf` |
| `encryption/no_printing.pdf` | Openable, but printing permission denied — `getPermissions().print == false` | openpreserve/format-corpus `.../encryption_noprinting.pdf` |
| `encryption/no_text_access.pdf` | Openable, but accessibility text-extraction permission denied — `getPermissions().accessibility == false` | openpreserve/format-corpus `.../encryption_notextaccess.pdf` |
| `encryption/standard40.pdf` | RC4 40-bit standard security handler; user password `user` | itext/itext7 `.../withPassword/standard40.pdf` |
| `encryption/standard128.pdf` | RC4 128-bit standard security handler; user password `user` | itext/itext7 `.../withPassword/standard128.pdf` |
| `encryption/aes128.pdf` | AES-128 password-based; user password `user` | itext/itext7 `.../withPassword/aes128.pdf` |
| `corrupt/one_byte_missing.pdf` | One byte deleted after the header (minimal-diff corruption) | openpreserve/format-corpus `.../corruptionOneByteMissing.pdf` |
| `corrupt/xref_command_missing.pdf` | The `xref` keyword itself is missing — forces full-file rebuild | mozilla/pdf.js `test/pdfs/xref_command_missing.pdf` |
| `corrupt/multiple_filters_length_zero.pdf` | Filter chain declared but zero-length data | mozilla/pdf.js `test/pdfs/multiple-filters-length-zero.pdf` |
| `corrupt/helloworld_bad.pdf` | Minimal document deliberately damaged | mozilla/pdf.js `test/pdfs/helloworld-bad.pdf` |
| `corrupt/scan_bad.pdf` | Damaged scanned-document PDF | mozilla/pdf.js `test/pdfs/scan-bad.pdf` |
| `corrupt/pdfjsbad1586.pdf` | Real-world parser-bug regression (pdf.js #1586) | mozilla/pdf.js `test/pdfs/pdfjsbad1586.pdf` |
| `corrupt/boundingbox_invalid.pdf` | Invalid bounding-box values on content | mozilla/pdf.js `test/pdfs/boundingBox_invalid.pdf` |

<!-- FIXTURE-TABLE-END: tiers-1-3 -->

## Supplementary fixtures

Files that live in this corpus but arrived through targeted follow-up work
rather than a tier expansion. They are locally generated (not upstream
downloads), so they carry a generator script instead of a provenance URL.

<!-- FIXTURE-TABLE-BEGIN: supplementary -->

| Local path | Flaw class | Origin |
|---|---|---|
| `corrupt/decompression-bomb.pdf` | Decompression bomb: 509 KiB on disk, 512 MiB of inert PDF whitespace after `FlateDecode` (1029:1). Opened and rendered ONLY out-of-process under a Job-Object memory cap by `tests/test_resource_bomb_sandbox.cpp` (meta-audit gap `PDF-109`) | locally generated by `tests/fixtures/gen_decompression_bomb.py` (deterministic; `--check` verifies the committed bytes) |
| `encryption/identity_stmf.pdf` | `/Encrypt` present but `/StmF /Identity` — streams are effectively plaintext while the document reports itself encrypted (meta-audit gap `PDF-25`) | locally generated by `tests/fixtures/gen_identity_stmf.py`; asserted by `EncryptionHandlerSecurityTest.IdentityStreamFilterReportedAsUnprotected` |
| `encryption/v4r4_cfm_v2.pdf` | `/V 4 /R 4` whose crypt filter is `/CFM /V2` — RC4-128, not AES. Legal per ISO 32000-1 Table 25 and reported as `AES-128` before F24, which OVERSTATED the protection | locally generated by `tests/fixtures/gen_v4r4_cfm_corpus.py`; asserted by `V4R4CryptFilterMethodTest.CfmV2IsReportedAsRc4128NotAes128` |
| `encryption/v4r4_cfm_aesv2.pdf` | The CONTROL for the row above: same `/V 4 /R 4` shape with `/CFM /AESV2`, i.e. genuine AES-128, which must still be reported as AES-128 | locally generated by `tests/fixtures/gen_v4r4_cfm_corpus.py`; asserted by `V4R4CryptFilterMethodTest.ControlGenuineAesV2IsStillReportedAes128` |
| `vector/tiling_pattern_self_referential.pdf` | Self-referential tiling pattern: the pattern object's own `/Resources /Pattern /P1` is that same object, and its content stream fills with `/P1`. Unbounded recursion in the pattern PAINTER — every nesting level is a structurally valid tile, so nothing is syntactically malformed. Complements `vector/tiling_pattern_large_steps.pdf`, which stresses the tiler's arithmetic rather than its depth (GitHub `#53`) | locally generated by `tests/fixtures/gen_tiling_pattern_self_referential.py` (deterministic; `--check` verifies the committed bytes); asserted by `ExternalFlawCorpusTest.TilingPatternSelfReferential_RendersBounded` |

<!-- FIXTURE-TABLE-END: supplementary -->

<!-- FIXTURE-TABLE-BEGIN: conformance -->

### Conformance (PDF/A + PDF/UA clause sampling, and the structurally-valid security-corpus subset)

Added by pipeline `p85-remaining-open` phase 3 for GitHub #8, #14, #7 and #37.

**These are NOT upstream downloads**, and the distinction matters. The Isartor
suite is not redistributable and the veraPDF corpus is a ~2,900-file network
download; this project's fixtures must be committed and bounded. So the
clause fixtures are authored locally by a deterministic generator, in the same
one-file-per-violated-clause shape Isartor uses. Each clause negative is its
CONTROL plus exactly one construct, the control is required to validate with
zero errors, and the clause list the fixtures must cover is parsed out of
`src/compliance/PdfAValidator.cpp`, `ProhibitedFeatureScanner.cpp` and
`PdfUAConverter.cpp` rather than typed into the test. See
`tests/fixtures/gen_conformance_corpus.py` for why that construction answers
COVERAGE.md §11's "self-consistent oracle" objection rather than ignoring it.

The `security_subset/` files are copies, not new material: the
structurally-valid subset of `tests/security/corpus/`, chosen by a documented
six-rule selector and cross-checked against MuPDF's own `pdf_was_repaired` on
every suite run.

| Local path | Flaw class | Upstream source |
|---|---|---|
| `conformance/pdfa/pdfa_1b_control.pdf` | CONTROL -- the validator must ACCEPT this: control -- nothing wrong; every negative below is this file plus exactly one defect | locally authored by `tests/fixtures/gen_conformance_corpus.py` (deterministic; `--check` verifies the committed bytes) |
| `conformance/pdfa/pdfa_2u_control.pdf` | CONTROL -- the validator must ACCEPT this: control for the ToUnicode clause -- part 2 conformance U, every font carries /ToUnicode | locally authored by `tests/fixtures/gen_conformance_corpus.py` (deterministic; `--check` verifies the committed bytes) |
| `conformance/pdfa/pdfa_2a_control.pdf` | CONTROL -- the validator must ACCEPT this: control for the tagging clause -- part 2 conformance A, /MarkInfo /Marked true plus a /StructTreeRoot | locally authored by `tests/fixtures/gen_conformance_corpus.py` (deterministic; `--check` verifies the committed bytes) |
| `conformance/pdfa/pdfa_6_7_11_no_xmp_identification.pdf` | clause 6.7.11 negative: /Catalog /Metadata removed -- no pdfaid:part/conformance | locally authored by `tests/fixtures/gen_conformance_corpus.py` (deterministic; `--check` verifies the committed bytes) |
| `conformance/pdfa/pdfa_6_2_2_no_output_intent.pdf` | clause 6.2.2 negative: /Catalog /OutputIntents removed | locally authored by `tests/fixtures/gen_conformance_corpus.py` (deterministic; `--check` verifies the committed bytes) |
| `conformance/pdfa/pdfa_6_3_3_font_not_embedded.pdf` | clause 6.3.3 negative: /FontDescriptor /FontFile2 removed -- font referenced by name | locally authored by `tests/fixtures/gen_conformance_corpus.py` (deterministic; `--check` verifies the committed bytes) |
| `conformance/pdfa/pdfa_6_3_8_no_tounicode.pdf` | clause 6.3.8 negative: /Font /ToUnicode removed from the 2u control | locally authored by `tests/fixtures/gen_conformance_corpus.py` (deterministic; `--check` verifies the committed bytes) |
| `conformance/pdfa/pdfa_6_1_3_encrypted.pdf` | clause 6.1.3 negative: RC4-128 (/V 2 /R 3) standard security handler with an empty user password; /OutputIntents dropped because its literal strings would otherwise be decrypted into noise | locally authored by `tests/fixtures/gen_conformance_corpus.py` (deterministic; `--check` verifies the committed bytes) |
| `conformance/pdfa/pdfa_6_6_1_javascript.pdf` | clause 6.6.1 negative: /Catalog /Names /JavaScript name tree added | locally authored by `tests/fixtures/gen_conformance_corpus.py` (deterministic; `--check` verifies the committed bytes) |
| `conformance/pdfa/pdfa_6_6_3_launch_action.pdf` | clause 6.6.3 negative: a /Link annotation whose action is /S /Launch | locally authored by `tests/fixtures/gen_conformance_corpus.py` (deterministic; `--check` verifies the committed bytes) |
| `conformance/pdfa/pdfa_6_4_transparency_group.pdf` | clause 6.4 negative: page /Group <</S /Transparency>> added | locally authored by `tests/fixtures/gen_conformance_corpus.py` (deterministic; `--check` verifies the committed bytes) |
| `conformance/pdfa/pdfa_6_1_7_object_stream.pdf` | clause 6.1.7 negative: identical objects, re-indexed through an /ObjStm and a cross-reference STREAM instead of an xref table | locally authored by `tests/fixtures/gen_conformance_corpus.py` (deterministic; `--check` verifies the committed bytes) |
| `conformance/pdfa/pdfa_6_8_untagged.pdf` | clause 6.8 negative: /MarkInfo and /StructTreeRoot removed from the 2a control | locally authored by `tests/fixtures/gen_conformance_corpus.py` (deterministic; `--check` verifies the committed bytes) |
| `conformance/pdfua/pdfua_control.pdf` | CONTROL -- the validator must ACCEPT this: control -- structure tree, /Lang, and a /Figure carrying /Alt | locally authored by `tests/fixtures/gen_conformance_corpus.py` (deterministic; `--check` verifies the committed bytes) |
| `conformance/pdfua/pdfua_7_1_no_structure_tree.pdf` | clause 7.1 negative: /Catalog /StructTreeRoot removed | locally authored by `tests/fixtures/gen_conformance_corpus.py` (deterministic; `--check` verifies the committed bytes) |
| `conformance/pdfua/pdfua_7_2_no_document_language.pdf` | clause 7.2 negative: /Catalog /Lang removed | locally authored by `tests/fixtures/gen_conformance_corpus.py` (deterministic; `--check` verifies the committed bytes) |
| `conformance/pdfua/pdfua_7_3_figure_without_alt.pdf` | clause 7.3 negative: the /Figure structure element's /Alt removed (and no /ActualText substituted -- either would satisfy the clause) | locally authored by `tests/fixtures/gen_conformance_corpus.py` (deterministic; `--check` verifies the committed bytes) |
| `conformance/pdfua/pdfua_7_16_encrypted.pdf` | clause 7.16 negative: RC4-128 standard security handler over the control document | locally authored by `tests/fixtures/gen_conformance_corpus.py` (deterministic; `--check` verifies the committed bytes) |
| `conformance/security_subset/asciihexdecode.pdf` | Structurally-valid copy of `tests/security/corpus/pdfjs/asciihexdecode.pdf` -- opens with no cross-reference reconstruction | selected by `tests/scripts/select_structurally_valid_corpus.py` (rules S1-S6); asserted by `ConformanceCorpusSamplingTest` |
| `conformance/security_subset/checkbox_no_appearance.pdf` | Structurally-valid copy of `tests/security/corpus/pdfjs/checkbox_no_appearance.pdf` -- opens with no cross-reference reconstruction | selected by `tests/scripts/select_structurally_valid_corpus.py` (rules S1-S6); asserted by `ConformanceCorpusSamplingTest` |
| `conformance/security_subset/clippath.pdf` | Structurally-valid copy of `tests/security/corpus/pdfjs/clippath.pdf` -- opens with no cross-reference reconstruction | selected by `tests/scripts/select_structurally_valid_corpus.py` (rules S1-S6); asserted by `ConformanceCorpusSamplingTest` |
| `conformance/security_subset/extractPages_null_in_array.pdf` | Structurally-valid copy of `tests/security/corpus/pdfjs/extractPages_null_in_array.pdf` -- opens with no cross-reference reconstruction | selected by `tests/scripts/select_structurally_valid_corpus.py` (rules S1-S6); asserted by `ConformanceCorpusSamplingTest` |
| `conformance/security_subset/issue4461.pdf` | Structurally-valid copy of `tests/security/corpus/pdfjs/issue4461.pdf` -- opens with no cross-reference reconstruction | selected by `tests/scripts/select_structurally_valid_corpus.py` (rules S1-S6); asserted by `ConformanceCorpusSamplingTest` |

<!-- FIXTURE-TABLE-END: conformance -->

## Provenance / licensing

- mozilla/pdf.js test PDFs are distributed under the Apache License 2.0.
- itext/itext7 test resources are distributed under the AGPL v3 (PDFE is
  AGPL-3.0, so this is compatible).

These are small, non-malicious test files. Deliberately EXCLUDED from this set:
bulk/undocumented corpora (GovDocs1, CommonCrawl) and any dataset flagged as
containing genuinely malicious payloads (see `pdf-test-corpus-reference.md`).

## Status

Tiers 1, 2, 3 and 4 are complete. Current test counts, derived from
`tests/test_external_flaw_corpus.cpp` and machine-checked by
`FlawCorpusCountDocs` in `tests/test_doc_drift_guards.cpp`:

<!-- CORPUS-COUNTS-BEGIN -->
| Metric | Value |
|---|---|
| `ExternalFlawCorpusTest` cases, total | 88 |
| Tier 1 | 8 |
| Tier 2 | 14 |
| Tier 3 | 26 |
| Tier 4 | 40 |
| `DISABLED_` cases | 0 |
<!-- CORPUS-COUNTS-END -->

Fixture *files* are counted separately from test *cases* — several Tier-4 tests
share one fixture, so the two tables legitimately differ. These figures are
re-derived from the tables above and from the directory itself by
`FlawCorpusFixtureDocs`:

<!-- FIXTURE-COUNTS-BEGIN -->
| Metric | Value |
|---|---|
| Tiers 1-3 fixture files | 47 |
| Tier 4 fixture files | 24 |
| Supplementary fixture files | 5 |
| Conformance fixture files | 23 |
| Fixture files on disk, total | 99 |
<!-- FIXTURE-COUNTS-END -->

One real product defect has been surfaced by this corpus to date — **#50**, a
malformed-JPEG2000 heap overflow in OpenJPEG, found by Tier-4 and fixed on
2026-07-31 by the default-off JPX decode gate. Tiers 1-3 surfaced none.

> This section previously read "the combined `ExternalFlawCorpusTest` suite is
> 47/47 green" and dated itself 2026-07-16 — a figure 28 cases out of date by
> the time anyone read it (meta-audit gap `PDF-106`). The table above is
> re-derived from the source on every test run and cannot drift again.

See `COVERAGE.md` for the reference-section coverage matrix (covered / partial /
deferred / excluded-by-design) and the dated verification records.

## Adding more

`pdf-test-corpus-reference.md` (repo root) lists ~90 curated files by flaw
category with direct download URLs. To expand: download into the matching
`flaw_corpus/<category>/` subdir (NOT `external/` — that directory name is
swallowed by a broad `.gitignore` rule) and add an asserting case to
`test_external_flaw_corpus.cpp` using the reference entry's description as the
assertion intent. `COVERAGE.md` lists the categories still deferred.

### Tier-4 (image codecs, numeric edges, transparency, degenerate annotations, embedded/JS, forms)

<!-- FIXTURE-TABLE-BEGIN: tier-4 -->

| Fixture | Flaw class | Upstream source |
|---|---|---|
| `images/jbig2-file-header.pdf` | JBIG2-encoded image (common in scanned documents); tests that codec. | mozilla/pdf.js/…/test/pdfs/jbig2_file_header.pdf |
| `images/jbig2-symbol-offset.pdf` | JBIG2 with symbol-dictionary offset edge case; known source of decoder bugs. | mozilla/pdf.js/…/test/pdfs/jbig2_symbol_offset.pdf |
| `images/jpx-bug.pdf` | JPEG2000 (JPX) decode regression case. | mozilla/pdf.js/…/test/pdfs/bug_jpx.pdf |
| `images/ccitt-endofblock-false.pdf` | CCITT Group 4 fax image with `EndOfBlock` false; edge case in that decoder. | mozilla/pdf.js/…/test/pdfs/ccitt_EndOfBlock_false.pdf |
| `images/hires-valid.pdf` | Intact, valid PDF/A-1b with a bitmap image; use as the positive control for the two variants below. | openpreserve/format-corpus/…/pdfCabinetOfHorrors/veraPDFHiRes.pdf |
| `images/hires-wrong-height.pdf` | Identical to the above but the `/Height` entry in the Image XObject dictionary is wrong — tests whether a renderer trust | openpreserve/format-corpus/…/pdfCabinetOfHorrors/veraPDFHiResChangedHeight.pdf |
| `images/hires-wrong-objectid.pdf` | Same base file, but the page content references a non-existent XObject ID — classic "missing/broken image reference" cas | openpreserve/format-corpus/…/pdfCabinetOfHorrors/veraPDFHiResWrongObjectID.pdf |
| `dimensions/real-over-ieee754-and-pdfa-limit.pdf` | A real number in a content stream exceeds both the IEEE754 float range and the PDF/A numeric limit — tests numeric overf | bfosupport/pdfa-testsuite/…/pdfa2-6-1-13-bfo-t04-fail.pdf |
| `dimensions/real-under-pdfa-min-structure.pdf` | Real number below the PDF/A minimum, in document structure (not content stream) — a second, structurally distinct underf | bfosupport/pdfa-testsuite/…/pdfa2-6-1-13-bfo-t05-fail.pdf |
| `dimensions/real-under-pdfa-min-contentstream.pdf` | Same underflow class but located in a content stream operand instead — pairs with the file above to test both code paths | bfosupport/pdfa-testsuite/…/pdfa2-6-1-13-bfo-t09-fail.pdf |
| `dimensions/nested-q-over-28.pdf` | More than 28 nested graphics-state `q`/`Q` operators — stresses the graphics-state stack depth, a common cause of stack- | bfosupport/pdfa-testsuite/…/pdfa2-6-1-13-bfo-t01-fail.pdf |
| `vector/function-based-shading.pdf` | Shading defined via a PostScript-style function rather than a sampled table — different code path than mesh/axial shadin | mozilla/pdf.js/…/test/pdfs/function_based_shading.pdf |
| `vector/knockout-blend-multiply.pdf` | Knockout group combined with multiply blend mode — tests correct transparency-group compositing order. | mozilla/pdf.js/…/test/pdfs/knockout_blend_multiply.pdf |
| `vector/blendmode.pdf` | General blend-mode coverage. | mozilla/pdf.js/…/test/pdfs/blendmode.pdf |
| `vector/transparency-group.pdf` | Isolated/non-isolated transparency group handling. | mozilla/pdf.js/…/test/pdfs/transparency_group.pdf |
| `overlap/annotation-empty-rect.pdf` | Line annotation with an empty `/Rect` and no appearance stream — degenerate/zero-area region. | mozilla/pdf.js/…/test/pdfs/annotation-line-without-appearance-empty-Rect.pdf |
| `overlap/annotation-highlight-no-appearance.pdf` | Highlight annotation missing its appearance stream, forcing the renderer to synthesize one — a common source of misposit | mozilla/pdf.js/…/test/pdfs/annotation-highlight-without-appearance.pdf |
| `embedded/file-attachment-namedict.pdf` | Document-level file attachment via the `/EmbeddedFiles` name dictionary (attaches a legacy Quattro Pro spreadsheet). | openpreserve/format-corpus/…/pdfCabinetOfHorrors/fileAttachment.pdf |
| `embedded/file-attachment-annotation.pdf` | Page-level attachment via a File Attachment *annotation* instead — structurally different from the name-dictionary case  | openpreserve/format-corpus/…/pdfCabinetOfHorrors/fileAttachment_fileAttachmentAnnotation.pdf |
| `embedded/javascript.pdf` | Contains embedded JavaScript (built with Didier Stevens' make-pdf tool) — tests that your pipeline safely ignores/sandbo | openpreserve/format-corpus/…/pdfCabinetOfHorrors/javascript.pdf |
| `forms/acroform-calc-order.pdf` | AcroForm with field calculation-order dependencies. | mozilla/pdf.js/…/test/pdfs/acroform_calculation_order.pdf |
| `forms/xfa-filled.pdf` | Filled XFA (XML Forms Architecture) form, a real US immigration form template — XFA is a legacy but still-encountered fo | mozilla/pdf.js/…/test/pdfs/xfa_filled_imm1344e.pdf |
| `forms/xfa-issue14315.pdf` | Regression case from a filed XFA-handling bug. | mozilla/pdf.js/…/test/pdfs/xfa_issue14315.pdf |
| `forms/resetform.pdf` | Form with a reset-form action; tests that static extraction doesn't misinterpret action-triggered state changes as conte | mozilla/pdf.js/…/test/pdfs/resetform.pdf |

<!-- FIXTURE-TABLE-END: tier-4 -->
