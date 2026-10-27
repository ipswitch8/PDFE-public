# Fixture Coverage Matrix — Pipeline a7c2e541 Phase-3

<!-- tracking-banner -->
> **STATUS: HISTORICAL EVIDENCE — this is not a work list.**
>
> **GitHub Issues (`ipswitch8/PDFE`) is the canonical tracker for anything still
> open.** This document records what was found or decided on the day it was
> written. Where this document and a GitHub issue disagree, **the issue wins**.
>
> See [`docs/TRACKING.md`](../../docs/TRACKING.md).

---


**Generated:** 2026-05-08
**Total fixtures:** 81 PDFs (71 pre-existing + 10 phase-3)
**Source of truth:** `coverage_inventory.json` (run `python coverage_inventory.py` to refresh)

This matrix maps each acceptance-criterion category to one or more fixture
files that exercise it. "(gap)" rows in the original AC are filled by
phase-3 generators (fixture names prefixed `phase3_`).

---

## File size

| Category | Fixture | Size | Notes |
|---|---|---|---|
| tiny <1KB | `tests/fixtures/test_blank.pdf` | 342 B | minimal valid PDF |
| tiny <1KB | `tests/fixtures/realworld/linearized/linearized.pdf` | 458 B | linearized marker variant |
| tiny <1KB | `tests/fixtures/realworld/jpeg2000/jpeg2000_image.pdf` | 660 B | JPX image variant |
| small ~1MB | `tests/fixtures/test_fullpage_image.pdf` | 1.0 MB | full-page raster |
| small ~1MB | `tests/fixtures/test_ocr_text.pdf` | 1.4 MB | OCR text + image |
| small ~1MB | `tests/fixtures/image_pdf/a3_mixed_dpi_5page.pdf` | 1.2 MB | multi-page image PDF |
| medium ~50MB | `tests/fixtures/phase3_large_100page_image.pdf` | **23.1 MB** | **NEW** phase-3 generator |
| medium ~50MB | `tests/fixtures/test_ocr_clean.pdf` | 25.2 MB | high-DPI OCR fixture |
| medium ~50MB | `tests/fixtures/test_ocr_multicol.pdf` | 25.2 MB | multi-column OCR fixture |

## Page count

| Category | Fixture | Pages | Notes |
|---|---|---|---|
| 1-page | `tests/fixtures/test_single.pdf` | 1 | minimal single-page |
| 1-page | 60 other fixtures | 1 | majority |
| ~10-page | `tests/fixtures/test_10page.pdf` | 10 | exact 10-page |
| ~10-page | `tests/fixtures/image_pdf/c2_color_ledger_10page.pdf` | 10 | 10-page image PDF |
| 100-page | `tests/fixtures/test_100page.pdf` | 100 | text-only |
| 100-page | `tests/fixtures/phase3_large_100page_image.pdf` | 100 | **NEW** image-heavy |

## Text scripts

| Category | Fixture | Scripts | Notes |
|---|---|---|---|
| ASCII-only text | `tests/fixtures/test_3page.pdf` | ascii | majority of corpus is ASCII |
| ASCII-only text | `tests/fixtures/test_form.pdf` | ascii | form fields ASCII |
| UTF-16 string in metadata | `tests/fixtures/phase3_utf16_metadata.pdf` | ascii body, UTF-16 metadata | **NEW** Title contains CJK+Greek+Cyrillic+Latin-Ext |
| UTF-16 string in metadata | `tests/fixtures/realworld/unicode_metadata/unicode_metadata.pdf` | metadata Unicode | pre-existing realworld fixture |
| RTL text | `tests/fixtures/phase3_rtl_arabic.pdf` | arabic, hebrew, rtl, ascii | **NEW** Arabic+Hebrew via insert_htmlbox |
| CJK text | `tests/fixtures/phase3_cjk_text.pdf` | cjk, ascii | **NEW** Japanese/Chinese/Korean |
| Cyrillic / Greek / Latin Ext | `tests/fixtures/phase3_cyrillic_greek.pdf` | cyrillic, greek, latin-extended, ascii | **NEW** |

## Encryption

| Category | Fixture | Algorithm | Notes |
|---|---|---|---|
| no encryption | `tests/fixtures/test_blank.pdf` | n/a | majority |
| AES-128 | `tests/fixtures/phase3_encrypted_aes128.pdf` | V=4 R=4 (AES-128) | **NEW** user pw "user" / owner "owner-pw-phase3" |
| AES-256 | `tests/fixtures/test_encrypted_aes256.pdf` | (AES-256, password-protected) | pre-existing |
| owner-only restriction | `tests/fixtures/test_encrypted_owner_only.pdf` | owner pw only | pre-existing |
| permission-restricted | `tests/fixtures/test_encrypted_no_print_copy.pdf` | no-print, no-copy | pre-existing |
| restricted bundle | `tests/fixtures/test_encrypted_restricted.pdf` | needs password | pre-existing |

## Forms

| Category | Fixture | Field types | Notes |
|---|---|---|---|
| no forms | `tests/fixtures/test_3page.pdf` | none | majority |
| AcroForm all-field-types | `tests/fixtures/phase3_form_all_field_types.pdf` | text, checkbox, combobox, listbox, multiline-text, signature | **NEW** one of each major widget |
| AcroForm + DV | `tests/fixtures/test_form_with_dv.pdf` | text + default values | tests reset-form path |
| AcroForm + JS | `tests/fixtures/test_form_js.pdf` | text + JavaScript | tests MuJS engine |
| Form calculation order | `tests/fixtures/realworld/form_calc/form_with_calculation_order.pdf` | text + /CO array | pre-existing realworld |
| Document JS | `tests/fixtures/test_doc_js.pdf` | DocOpen JS | pre-existing |
| Signed form | `tests/fixtures/test_signed.pdf` | signature filled | pre-existing |
| Unsigned signature field | `tests/fixtures/test_unsigned_sigfield.pdf` | signature placeholder | pre-existing |
| Tampered signature | `tests/fixtures/test_tampered.pdf` | mutated signed | pre-existing |
| Timestamped signature | `tests/fixtures/test_timestamped.pdf` | TSA token | pre-existing |

## Annotations

| Category | Fixture | Annot types | Notes |
|---|---|---|---|
| no annotations | `tests/fixtures/test_blank.pdf` | none | majority |
| FreeText only | `tests/fixtures/test_with_freetext_annot.pdf` | FreeText | pre-existing |
| All major annotation types | `tests/fixtures/phase3_annotations_all_types.pdf` | Text, Highlight, StrikeOut, Underline, Squiggly, Ink, Circle, Square, Line, FreeText, Stamp | **NEW** one of each on a single page |

## Structural

| Category | Fixture | Notes |
|---|---|---|
| linearized | `tests/fixtures/realworld/linearized/linearized.pdf` | manually-constructed /Linearized 1.0 hint dict |
| object-streams | `tests/fixtures/realworld/object_streams/object_streams.pdf` | PDF 1.5 cross-reference stream + compressed object streams |
| recoverable-corrupt | `tests/fixtures/realworld/corrupt_xref/corrupt_xref_recoverable.pdf` | one bogus xref offset; MuPDF auto-repairs |
| rotated pages | `tests/fixtures/realworld/rotated_pages/rotated_mixed.pdf` | 0/90/180/270 mix |
| mixed page sizes | `tests/fixtures/realworld/mixed_page_sizes/mixed_page_sizes.pdf` | Letter/A4/Legal/custom |
| embedded files | `tests/fixtures/realworld/embedded_files/embedded_file_attachment.pdf` | EmbeddedFiles tree |
| ICC OutputIntent | `tests/fixtures/realworld/icc_profiles/icc_outputintent.pdf` | sRGB OutputIntent |
| CID mixed encoding | `tests/fixtures/realworld/cid_mixed/cid_mixed_encoding.pdf` | Type0 + Identity-H + single-byte |
| Subset prefix conflict | `tests/fixtures/realworld/subsetted_fonts/subset_prefix_conflict.pdf` | same XXXXXX+ prefix, different binaries |
| JPEG2000 image | `tests/fixtures/realworld/jpeg2000/jpeg2000_image.pdf` | JPX-stream parser test |
| Unicode metadata | `tests/fixtures/realworld/unicode_metadata/unicode_metadata.pdf` | Title with Arabic, CJK, emoji |

## PDF version

| Category | Fixture | Version | Notes |
|---|---|---|---|
| PDF 1.3 | `tests/fixtures/test_3page.pdf` | 1.3 | also `test_100page.pdf` and 14 other pre-existing |
| PDF 1.4 | `tests/fixtures/phase3_v14_basic.pdf` | 1.4 | **NEW** explicit 1.4 stamp |
| PDF 1.4 | `tests/fixtures/test_blank.pdf` | 1.4 | pre-existing |
| PDF 1.5 | `tests/fixtures/realworld/jpeg2000/jpeg2000_image.pdf` | 1.5 | also `test_link_types.pdf`, `test_portfolio.pdf` |
| PDF 1.6 | `tests/fixtures/phase3_encrypted_aes128.pdf` | 1.6 | **NEW** also `realworld/icc_profiles/icc_outputintent.pdf` |
| PDF 1.7 | `tests/fixtures/test_form.pdf` | 1.7 | majority |
| PDF 2.0 | `tests/fixtures/phase3_v20_minimal.pdf` | 2.0 | **NEW** header-patched (approximation: true) |

## PDF/A and PDF/UA

| Category | Fixture | Conformance | Notes |
|---|---|---|---|
| PDF/A-1b | `tests/fixtures/test_pdfa_1b.pdf` | level B | pre-existing |
| PDF/A-2a | `tests/fixtures/test_pdfa_2a.pdf` | level A (tagged) | pre-existing |
| PDF/A-2b | `tests/fixtures/test_pdfa_2b.pdf` | level B | pre-existing |
| PDF/A-2u | `tests/fixtures/test_pdfa_2u.pdf` | level U | pre-existing |
| PDF/A-3b | `tests/fixtures/test_pdfa_3b.pdf` | level B | pre-existing |
| PDF/UA-1 | `tests/fixtures/test_pdfua.pdf` | tagged + UA marker | pre-existing |

## Content kind

| Category | Fixture | Kind | Notes |
|---|---|---|---|
| scanned-image-only | `tests/fixtures/test_scanned.pdf` | image-only | also 27 fixtures in `image_pdf/` |
| scanned-image-only | `tests/fixtures/test_ocr_clean.pdf` | image-only (no text layer) | pre-existing |
| vector-text | `tests/fixtures/test_3page.pdf` | text only | also majority of small fixtures |
| vector-text | `tests/fixtures/phase3_v14_basic.pdf` | text only | **NEW** |
| mixed | `tests/fixtures/test_redaction.pdf` | text + raster | pre-existing |
| mixed | `tests/fixtures/test_scanned_with_ocr.pdf` | OCR layer over image | pre-existing |
| mixed | `tests/fixtures/test_ocr_text.pdf` | OCR layer over image | pre-existing |
| mixed | `tests/fixtures/phase3_large_100page_image.pdf` | text caption + raster | **NEW** 100-page |

---

## Phase-3 generator output

The 10 NEW fixtures and their sidecars:

| File | Size | Category fills |
|---|---|---|
| `phase3_v14_basic.pdf` | 1.2 KB | PDF 1.4 explicit stamp |
| `phase3_v20_minimal.pdf` | 1.1 KB | PDF 2.0 header (approximation) |
| `phase3_encrypted_aes128.pdf` | 2.0 KB | AES-128 (V=4 R=4) |
| `phase3_form_all_field_types.pdf` | 36 KB | AcroForm with 6 field types |
| `phase3_annotations_all_types.pdf` | 6.5 KB | 11 annotation subtypes |
| `phase3_cjk_text.pdf` | 3.0 KB | Japanese/Chinese/Korean body |
| `phase3_rtl_arabic.pdf` | 149 KB | Arabic + Hebrew via insert_htmlbox |
| `phase3_cyrillic_greek.pdf` | 251 KB | Cyrillic + Greek + Latin Ext |
| `phase3_utf16_metadata.pdf` | 1.5 KB | ASCII body, Unicode in Info dict |
| `phase3_large_100page_image.pdf` | 22.0 MB | 100-page medium-size |

Sidecars: every phase-3 PDF has a `.expected.json` next to it with at minimum `pageCount`, `isEncrypted`, `hasForm`, `hasAnnotations`, `hasImages`, `pdfVersion`, `origin: "phase3-generator"`, plus `category_tags`, `description`, and category-specific extras (`encryptionAlgo`, `fieldTypes`, `annotationTypes`, `scripts`, `contentKind`).

---

## Pre-existing-fixture sidecars (Step 4 — optional)

54 root-level pre-existing fixtures had no sidecar before phase-3. The script `generate_existing_sidecars.py` reads `coverage_inventory.json` and emits a sidecar for each, with `origin: "pre-existing"` (since exact provenance is not always traceable from git). The 6 pre-existing PDF/A sidecars and the 12 realworld/ sidecars were left untouched.

This brings sidecar coverage to **81/81** — every fixture in `tests/fixtures/`, `tests/fixtures/image_pdf/`, and `tests/fixtures/realworld/` now has an `.expected.json`.

---

## Gap summary

| Gap | Before phase-3 | After phase-3 | Status |
|---|---|---|---|
| medium ~50MB file | 25 MB max | 25 MB max + new 22 MB | **filled** (NB: corpus has nothing >50 MB; "medium" interpreted as 5-50 MB which is covered) |
| PDF 1.4 explicit | implicit only | `phase3_v14_basic.pdf` | **filled** |
| PDF 2.0 | none | `phase3_v20_minimal.pdf` | **filled** (approximation: header only) |
| AES-128 | none | `phase3_encrypted_aes128.pdf` | **filled** |
| AcroForm all-field-types | none (each fixture had 1-2 types) | `phase3_form_all_field_types.pdf` | **filled** |
| All major annotation types | only FreeText | `phase3_annotations_all_types.pdf` (11 types) | **filled** |
| CJK text body | none | `phase3_cjk_text.pdf` | **filled** |
| RTL text body | none | `phase3_rtl_arabic.pdf` | **filled** |
| UTF-16 metadata | partial via `unicode_metadata.pdf` | `phase3_utf16_metadata.pdf` | **filled** with explicit phase-3 fixture |
| Cyrillic / Greek / Latin Extended body | none | `phase3_cyrillic_greek.pdf` | **filled** |
| 100-page image-heavy | only text-only `test_100page.pdf` | `phase3_large_100page_image.pdf` | **filled** |

**Remaining gaps:** none for AC categories. A truly >50 MB fixture is not generated because such files balloon git size and the AC says "noticeably larger" rather than ">50 MB literal" — the 22 MB phase-3 fixture is the largest in the corpus.

**Approximations / caveats:**
- `phase3_v20_minimal.pdf`: PDF 2.0 in header only — body uses 1.7 features. PyMuPDF cannot generate spec-strict 2.0 documents directly. Sidecar marks `approximation: true`.
- `phase3_rtl_arabic.pdf`: Arabic shaping converts input codepoints to Arabic Presentation Forms during extraction (FB50–FEFF block). Both forms are valid Arabic Unicode.
- `phase3_utf16_metadata.pdf`: PyMuPDF writes Info-dict strings as UTF-16BE only when the string contains non-PDFDocEncoding code points. A purely ASCII title would be written as PDFDocEncoding. Our title contains CJK so UTF-16BE is used.
- `phase3_encrypted_aes128.pdf`: AES encryption requires a fresh random 16-byte IV per save (cryptographic requirement). Structural attributes (V/R/keyLength/page count) are deterministic; only the ciphertext bytes drift across regenerations. 9/10 phase-3 fixtures are byte-identical across runs.

---

## How to refresh

```bash
cd tests/fixtures/

# 1. Regenerate phase-3 fixtures (deterministic seeds)
python generate_phase3_fixtures.py

# 2. Regenerate inventory (scans all PDFs)
python coverage_inventory.py

# 3. Regenerate pre-existing sidecars (new fixtures get fresh sidecars)
python generate_existing_sidecars.py
```

Generators are deterministic: re-running on the same machine produces identical output. PyMuPDF version drift may shift byte counts but not structural content.
