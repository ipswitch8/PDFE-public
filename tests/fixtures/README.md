# PDFE Test Fixtures

<!-- tracking-banner -->
> **STATUS: LIVE OPERATIONAL STATE — keep maintaining this file.**
>
> This is **not** a defect tracker and is **not** superseded by GitHub Issues;
> it is state that code, CI or a fixture generator reads. **GitHub Issues
> (`ipswitch8/PDFE`) remains the canonical tracker for anything still open** — a
> defect recorded only here is still an untracked defect.
>
> See [`docs/TRACKING.md`](../../docs/TRACKING.md).

---


Every fixture in this directory is **test-synthetic**. None contains
proprietary content, PII, or third-party copyrighted material. Fixtures
are regenerated via the `generate_*.py` scripts below so CI or a clean
checkout can rebuild them deterministically.

## Regenerating everything

```bash
cd tests/fixtures
python generate_pdfa_fixtures.py     # PDF/A-1b, 2b, 2u, 2a, 3b
python generate_signed_pdf.py        # test_signed.pdf + test_signer.pfx
python generate_100page.py           # test_100page.pdf
python generate_encrypted_pdfs.py    # test_encrypted_*.pdf
python generate_form_with_dv.py      # test_form_with_dv.pdf
python generate_test_blank_pdf.py    # test_blank.pdf
python generate_test_images.py       # test_image_rgb.png etc
python generate_test_redaction_pdf.py
python generate_test_scanned_pdf.py
python create_ocr_fixture.py         # test_ocr_text.pdf
python generate_tampered_pdf.py      # test_tampered.pdf (Phase 10)
python generate_ocr_variants.py      # test_ocr_{clean,skewed,multicol}.pdf
python generate_taggable_rich_pdf.py # test_taggable_rich.pdf (Phase 16, AutoTagger fixture)
python generate_tagged_multipage_pdf.py # test_tagged_multipage.pdf (R3-03, tagged 3-page struct-tree fixture)
python generate_tagged_rolemap_pdf.py # test_tagged_rolemap.pdf (GitHub #9/#10/#11/#12, /RoleMap + /Artifact + distinctive /Alt + 3-MCID reading order)
python gen_tagged_authored_struct.py # test_tagged_authored_struct.pdf (GitHub #15, authored /Alt + /ID + /Part>/Sect>/L>/LI>/LBody nesting; PDF/A-2a struct-tree PRESERVATION oracle)
python gen_pdf20_associated_files.py # pdf20_associated_files.pdf (GitHub #2, PDF 2.0 associated files with four distinct /AFRelationship values)
python gen_neg_tz_grey_text.py       # neg_tz_grey_text.pdf (-100 Tz + grey-fill edit repro)
python gen_spot_overprint_fixture.py # adv_spot_overprint.pdf (R3-06 CMYK/Separation-spot/overprint colour)
python gen_cmyk_identity.py          # cmyk_identity.pdf (R3-06 edited-span colour-operator identity)
python gen_mediabox_offset.py        # mediabox_offset.pdf (R3-12 non-zero-origin MediaBox selection crop)
python gen_table_extract.py          # table_extract.pdf (R3-07 table row/column extraction)
python gen_ligature_fixture.py       # text_ligature.pdf (R3-07 ligature/hyphenation extraction)
python gen_form_calc_sum.py          # form_calc_sum.pdf (R3-09 form-JS /C calculate: field3=field1+field2)
python gen_cmyk_image_xobject.py     # cmyk_image_xobject.pdf (A1-22 CMYK/Separation raster image XObject)
python gen_jbig2_redaction.py        # jbig2_redaction.pdf (A1-88 JBIG2 decode + redaction confidentiality)
python gen_cropbox_offset.py         # cropbox_offset.pdf (A1-25 hostile class 9: CropBox != MediaBox, large offset)
python gen_sig_shadow_corpus.py      # F21 adversarial signature corpus (shadow attacks, wrapping, FieldMDP)
python gen_hybrid_xref.py            # hybrid_xref.pdf (A1-87 hybrid-reference /XRefStm on the incremental-save path)
python gen_font_descriptor_matrix.py # font_descriptor_matrix.pdf (A1-37 /FontDescriptor style + /BaseFont untruncation)
python gen_image_wizard_boundary.py  # image_wizard_{corrupt,oversized}.png (A1-31 create-PDF-from-images boundaries)
python gen_form_radio_group.py       # form_radio_group.pdf (A1-61 two-kid radio group, authored exclusivity)
python gen_form_validate_js.py       # form_validate_js.pdf (A1-60 authored /AA /V Validate verdicts)
python gen_form_importdata_action.py # form_importdata_action.pdf + .fdf (PDF-49 /ImportData inertness)
python gen_form_comb_maxlen.py       # form_comb_maxlen.pdf (PDF-44 / #16 comb bit 25 + /MaxLen cell layout)
python gen_form_aa_events.py         # form_aa_events.pdf (PDF-46 / #17 authored /AA /F Format + /AA /K Keystroke)
python gen_annot_measure.py          # annot_measure.pdf (PDF-57 / #21 page /VP + /Measure, three viewports)
python gen_vertical_cjk.py           # i18n_vertical_cjk.pdf (PDF-71 / #22 /Identity-V tategaki + /Identity-H control)
python gen_soft_hyphen.py            # text_soft_hyphen.pdf (PDF-73 / #24 authored U+00AD discretionary hyphen)
python gen_encoding_edge_cases.py    # encoding_edge_cases.pdf (PDF-75 / #25 /Differences + WinAnsi 0x80-0x9F, read side)
python gen_word_boundary_corpus.py   # text_word_boundary_tj.pdf (PDF-76 / #26 TJ numeric-offset word gaps + tight-kerning control)
```

### Meta-audit WP4 fixture-absence fixtures

| File | Purpose |
|---|---|
| `cmyk_image_xobject.pdf` | A1-22 hostile-class 5 (CMYK/Separation raster IMAGE): a 24x4 8-bit `/DeviceCMYK` image XObject with six authored patches (pure C/M/Y, `(26,102,179,64)`, the same CMY with K dropped, pure K) and a 12x4 `[/Separation /SpotRed /DeviceCMYK]` image with tints 0/128/255 over a type-2 transform `C0=[0 0 0 0] C1=[0 1 1 0]`. Both unfiltered, so the sample bytes are greppable. Pins image-path plane order, K-plate survival and tint-transform evaluation. Generated by `gen_cmyk_image_xobject.py`. |
| `jbig2_redaction.pdf` | A1-88 JBIG2 decode + redaction path: a 64x64 1-bit `/Filter /JBIG2Decode` image (MMR generic region) drawn over user-space x 50..250, y 50..250 of a 300x300 page, authored as four quadrants (top-left BLACK, top-right WHITE, bottom-left deterministic speckle for coded-byte entropy, bottom-right BLACK), plus a `KEEPVISIBLE` control line outside the redaction. Backs the decode oracle and the redaction-confidentiality byte scan. Generated by `gen_jbig2_redaction.py`. |
| `cropbox_offset.pdf` | A1-25 hostile class 9 — `/CropBox [400 500 1000 1100]` inside `/MediaBox [0 0 1200 1600]` (a 400/500 offset). Authored at known absolute user-space coordinates: red block `[450 1030 560 1070]`, `REDACTME` at (450,1000), `KEEPME` at (450,700), `OUTSIDEBOX` at (100,200) — outside the CropBox, inside the MediaBox. Pins that redaction rects, extracted text boxes, annotation `/Rect` and render output are all CropBox-relative. Generated by `gen_cropbox_offset.py`. |
| `hybrid_xref.pdf` | A1-87 genuine hybrid-reference file: a classic `xref` table written as subsections `0 5` and `6 2` (object 5 omitted) plus a trailer `/XRefStm` pointing at a `/Type /XRef` stream (`/W [1 4 2]`, `/Index [5 1]`, FlateDecode) that alone describes object 5 — the page's second content stream, which draws `HYBRIDONLY`. `TABLEBASE` (object 4) is reachable from the classic table. Exercises `Document::saveIncremental()` on hybrid input. Generated by `gen_hybrid_xref.py`. |
| `font_descriptor_matrix.pdf` | A1-37 `/FontDescriptor` style + `/BaseFont` untruncation matrix: eleven fonts on one page covering the `/Flags` 64 italic bit, a non-zero `/ItalicAngle` with that bit CLEAR, `/Flags` 0x40000 ForceBold, `/StemV` 140/139 straddling the bold threshold, a font with NO descriptor, and a Type0 composite whose descriptor lives on the descendant CIDFont — plus two distinct 35/36-char `/BaseFont` names sharing an identical 31-char prefix, a subset-prefixed short name, and a 38-char name reachable only through a Form XObject's own `/Resources`. Every `/BaseFont` is named after the DESCRIPTOR FACT, so a name-keyword implementation cannot pass. Generated by `gen_font_descriptor_matrix.py`. |
| `image_wizard_corrupt.png` | A1-31 create-PDF-from-images corrupt-input boundary: valid PNG signature + CRC-correct 64x64 IHDR + an IDAT truncated mid-deflate, no IEND. Sniffs as a PNG, cannot be decoded. Generated by `gen_image_wizard_boundary.py`. |
| `image_wizard_oversized.png` | A1-31 create-PDF-from-images oversized-input boundary: a 69-byte file whose IHDR declares 65535x65535 RGBA (~17 GB) with a real 1x1 IDAT/IEND body, so Qt's `QImageIOHandler::allocateImage` must refuse it at `QImageReader::allocationLimit()` before any giant allocation. Generated by `gen_image_wizard_boundary.py`. |

### R3 fidelity-oracle fixtures (Phase 5)

| File | Purpose |
|---|---|
| `adv_spot_overprint.pdf` | R3-06 colour preservation: `EDITME` in DeviceCMYK (`k`) under an overprint ExtGState + `SPOTME` in a `/Separation` spot colourspace (`scn`). Generated by `gen_spot_overprint_fixture.py`. |
| `cmyk_identity.pdf` | R3-06 edited-span colour-operator identity: five spans — `PUREK` (`0 0 0 1 k`), `RICHBK` (`.2 .2 .2 1 k`), `SPOTA`/`SPOTB` (`/CS1 cs 1 scn`), `RGBRED` (`1 0 0 rg`) — asserting an edit preserves the original `k`/`scn`/`rg` operator instead of flattening to RGB. Generated by `gen_cmyk_identity.py`. |
| `mediabox_offset.pdf` | R3-12 non-zero-origin MediaBox selection crop: MediaBox `[100 100 700 900]` with a red block (x[140..280] y[640..760]) and a blue block (x[420..560] y[190..310]) in PDF user space; selecting one and copying as image must crop the correct region once the bounds origin is subtracted. Generated by `gen_mediabox_offset.py`. |
| `table_extract.pdf` | R3-07 table row/column reconstruction: deterministic 3×3 grid of `R<r>C<c>` cells at known positions (row tops ≈79/129/179, column lefts ≈100/280/460) for row-major reading-order + geometry oracles. Generated by `gen_table_extract.py`. |
| `text_ligature.pdf` | R3-07 text-extraction fidelity: `office`/`flourish` (ligature normalization) + `cooper-`/`ation` (hyphenation). Generated by `gen_ligature_fixture.py`. |
| `form_calc_sum.pdf` | R3-09 form-JS calculation correctness: `field1`,`field2`,`field3` with a `/AA/C` calculate action `field3=field1+field2`, `/CO=[field3]`. Generated by `gen_form_calc_sum.py`. |

### Scan-classification fixtures (field report 2026-08-28)

`Page::isImageDominated()` gates the entire OCR/TRE path. These two pin the
ways it refused a real 16-page scanned manual on 15 of its 16 pages. See
`docs/audit/field-report-2026-08-28-scanned-manual.md` (defect 1).

| File | Purpose |
|---|---|
| `scan_offpage_image.pdf` | No text; one on-page image plus one placed wholly beyond the right page edge. MuPDF clips such a block's far edge to the page boundary and leaves the near edge outside, producing an INVERTED bbox (x0 > x1) whose signed area is NEGATIVE — it used to SUBTRACT coverage, and three pages of the reported manual summed negative. Not a mirrored CTM: MuPDF normalises those, and a synthetic mirror reproduced nothing. Generated by `make_scan_classification_fixtures.py`. |
| `scan_small_page.pdf` | A 180x240pt scan (no text layer, one image with legible raster text) used by `tests/test_ui_tre_region_ocr.py` at 25% zoom. Exists because **PDFE fits the page to the viewport** — measured, a 180pt and a 612pt page both cover ~58-60% of the canvas — so page SIZE cannot make a page that a canvas-fraction guess would miss; only zoom can. Generated by `make_small_page_scan.py`. |
| `scan_two_blocks.pdf` | Image-only 612x792pt scan with TWO widely separated raster text blocks — `ALPHA BRAVO` at 15-30% of page height, `CHARLIE DELTA` at 70-85%, with an empty 40% gutter between them. Exists because the region-OCR test's old oracle ("a dialog opened with text in it") is insensitive to WHERE the region was, so it could never separate a correct region from a displaced or over-large one — both recognise *something*. Selecting only the upper block and asserting the recognised text contains ALPHA/BRAVO and NOT CHARLIE/DELTA is sensitive to region POSITION, which is the property `page_rect_on_canvas` is responsible for. The gutter is the tolerance: the drag must be displaced by more than a third of the page height before it reads the wrong block. Words are chosen to be mutually non-confusable under a single-character OCR slip. Generated by `make_two_block_scan.py`. |
| `scan_signed_area_straddle.pdf` | Text-bearing page + 8 off-page image placements, built for an isolating test of the area-SIGN half of `isImageDominated()`. **It does not yet work and is kept as evidence:** PyMuPDF reports signed 0.444 / clamped 0.995 (a clean straddle of the 0.5 gate), but the INSTRUMENTED C++ on the same file reports `ratio=3.6297` with the clamp reverted — the synthetic `cm` placements do not produce inverted bboxes in the C++ stext path at all. See the gap note in `tests/test_ocr_routing_fix.cpp`. Generated by `make_scan_classification_fixtures.py`. |
| `scan_low_coverage.pdf` | No text at all, one upright image covering ~21% of the page. A page with nothing on it but pixels is a scan whatever fraction they cover; the old `imageRatio > 0.5` alone refused it and OCR never ran. Generated by `make_scan_classification_fixtures.py`. |

## File inventory

### Core document fixtures

| File | Purpose |
|---|---|
| `test_single.pdf` | Minimal one-page document |
| `test_3page.pdf` | Three pages with distinct text |
| `test_multipage.pdf` | Five pages for multi-select gesture tests |
| `test_100page.pdf` | Large doc for scroll/page-panel stress |
| `test_blank.pdf` | Empty pages |
| `test_landscape.pdf` | Landscape-oriented pages |
| `test_multisize.pdf` | Pages with varying dimensions |
| `test_multispan.pdf` | Text with multiple font/size runs |
| `test_portfolio.pdf` | Portfolio (PDF package): /Collection /View /D + two embedded files (a.txt, b.txt). Generated by `generate_portfolio_pdf.py`. |
| `test_timestamped.pdf` | PDF with a /DocTimeStamp signature carrying a cached RFC-3161 TimeStampToken. Accompanied by `test_tsa_token.der` (the cached token bytes), `test_tsa_cert.pem` / `test_tsa_key.pem` (self-signed TSA identity). Generated offline by `generate_timestamped_pdf.py`. |
| `test_link_types.pdf` | 3-page PDF with four link annotations on page 0 (one per action type: URI / GoTo / Named NextPage / Launch). Used by the follow-link pywinauto scripts. Generated by `generate_link_types_pdf.py`. |
| `test_dest_integrity.pdf` | 5-page PDF carrying KNOWN destinations for the R3-01 destination-integrity oracle: each page draws a unique marker `PDFEPAGEMARK0..4`; three bookmarks (`/Outlines`) target pages 1/2/3; two `/GoTo` link annotations on page 0 target pages 2/3. Used by `test_page_edit_destination_integrity.cpp` to verify page delete/reorder remaps bookmark & link dests by content. Generated by `generate_dest_integrity_pdf.py`. |
| `neg_tz_grey_text.pdf` | Two spans (grey `#dbd0d0` + dark) drawn under an ambient `-100 Tz` (negative horizontal text scaling) and negative `Tf` — the GDI-style triple-flip that reproduces the text-edit **color-loss** and **mirroring** defects. Generated by `gen_neg_tz_grey_text.py`. |

### Encrypted variants

| File | Encryption / permission |
|---|---|
| `test_encrypted_aes256.pdf` | AES-256 |
| `test_encrypted_restricted.pdf` | Basic permissions restricted |
| `test_encrypted_owner_only.pdf` | Owner password, no user password |
| `test_encrypted_no_print_copy.pdf` | Print + copy denied |

### Forms

| File | Purpose |
|---|---|
| `test_form.pdf` | AcroForm with text/check/radio/button |
| `test_form_js.pdf` | Form with JavaScript field actions |
| `test_form_with_dv.pdf` | Fields carry /DV (default value) |
| `test_form_with_pushbutton.pdf` | AcroForm with a push button field |

### PDF/A corpus (Phase 10)

| File | Version | Conformance | Color space | Font embedding |
|---|---|---|---|---|
| `test_pdfa_1b.pdf` | PDF/A-1 | b | DeviceRGB | fully embedded |
| `test_pdfa_2b.pdf` | PDF/A-2 | b | DeviceRGB | fully embedded |
| `test_pdfa_2u.pdf` | PDF/A-2 | u | DeviceRGB | fully embedded + Unicode map |
| `test_pdfa_2a.pdf` | PDF/A-2 | a | DeviceRGB | fully embedded + tagged |
| `test_pdfa_3b.pdf` | PDF/A-3 | b | DeviceRGB | fully embedded + attachments allowed |
| `test_pdfua.pdf` | PDF/UA-1 | — | DeviceRGB | tagged + StructTreeRoot + DisplayDocTitle |
| `test_taggable_rich.pdf` | (untagged, AutoTagger input) | — | DeviceRGB | Helvetica + Helv-Bold; 2 pages with 24pt H1 + 18pt H2 + 11pt body + bullet/numbered lists for AutoTagger heuristics (Phase-16, P13-INFRA-5 closure) |
| `test_tagged_multipage.pdf` | tagged (PDF/UA-shaped) | — | DeviceRGB | 3 pages, each with a real `/P <</MCID 0>> BDC…EMC` marked-content sequence + `/StructParents`; full `/StructTreeRoot`→`/Document`→per-page `/P` `/StructElem` (`/Pg`+`/K`) + real `/ParentTree` `/Nums`. R3-03 struct-tree-preservation fixture (text-edit / page-delete / redaction). |
| `test_tagged_authored_struct.pdf` | tagged, AUTHORED | — | DeviceRGB | 3 pages, 14 structure elements nested `/Document`→`/Part`→two `/Sect`→(`/H1`, `/H2`, `/P`, `/L`→`/LI`→`/LBody`). Every leaf carries an authored `/Alt` and `/ID`, and the grouping elements an authored `/T`, none of which appear anywhere in the page content. GitHub #15: the identity an auto-tagger CANNOT re-derive, so `PdfA2aConversionPreservesExistingStructTree` can tell a preserved tree from a rebuilt one. Generator: `gen_tagged_authored_struct.py`. |
| `pdf20_associated_files.pdf` | PDF 2.0 (ISO 32000-2 §14.13) | — | DeviceRGB | Three catalog-level associated files (`/Source`, `/Data`, `/Supplement`) plus one page-level one (`/Alternative`), all four also reachable through `/Names /EmbeddedFiles`. Four DISTINCT `/AFRelationship` values, so GitHub #2's `AssociatedFilesSurviveRoundtrip` fails an implementation that keeps the attachments and drops (or flattens) their relationships. Generator: `gen_pdf20_associated_files.py`. |

Each `test_pdfa_*.pdf` has a matching `*.expected.json` sidecar
containing the metadata block tests assert against:

```json
{
  "version": "PDF/A-1",
  "conformance": "b",
  "colorSpace": "DeviceRGB",
  "fontsFullyEmbedded": true,
  "hasMarkedContent": false
}
```

### Signature corpus (Phase 10 / Phase 12)

| File | Description |
|---|---|
| `test_unsigned_sigfield.pdf` | Has signature field, not yet signed |
| `test_signed.pdf` | **Real PKCS#7-signed PDF** (phase-12 remediation): generated by `generate_signed_pdf.py` using `certs/phase12_test_signer.pfx` (password `testpw123`) via pyhanko. Carries a valid `/ByteRange` + PKCS#7 `/Contents`. Cert is self-signed (no trust anchor) so chain validation fails but the signature is structurally intact (`intact=True`). |
| `test_tampered.pdf` | `test_signed.pdf` with a byte flipped **inside the first `/ByteRange` segment** so the PKCS#7 digest no longer matches the document bytes. `generate_tampered_pdf.py` regenerates this from the live `test_signed.pdf`. |
| `test_signer.pfx` | Legacy PKCS#12 private key + cert (CN "PDFE Test Signer", password `test123`) kept for backwards compatibility. **Test-only, no trust relationship with any real CA.** **Unsuitable for anything that reasons about CA authority** — it is a *self-signed end-entity* certificate (`CA:FALSE`; `keyUsage` critical `digitalSignature, contentCommitment`; subject == issuer) that acts as its own CA. Any rule keyed on a `keyUsage` bit only a CA carries, or on a path longer than one link, is structurally unreachable through this fixture; use `crl_chain/` below instead. (Meta-audit F47/F1, phase-48 — two live defects hid behind exactly that blindness; see `docs/audit/dca/0014-crl-issuer-keyusage-and-intermediate-ocsp.md`.) |
| `certs/phase12_test_signer.pfx` | Self-signed test PFX (CN contains literal `TEST`, password `testpw123`) used for phase-12 signing. See `certs/README.md`. |

CA-chain material for the signature E2E tests is produced by
`generate_ca_signed_pdf.py` (needs Python `cryptography`):

| File | Purpose |
|---|---|
| `test_ca_cert.pem` / `test_ca_key.pem` | Self-signed test root CA (10-year validity) |
| `test_signer_ca.pem` / `test_signer_ca_key.pem` | Signer cert issued by the test CA |
| `test_signer_ca.pfx` | PKCS#12 bundle (signer key + cert + CA chain), empty password |

`test_signed_ca.pdf` — the PDF signed with the CA-chain bundle — is a
derived artifact rather than a committed fixture; it is produced on
demand by the C++ `SignatureManager` tests that use
`test_signer_ca.pfx` as the signing identity.

`test_timestamped.pdf` (RFC 3161 TSA token) remains deferred until a
mock TSA server is available.

#### `crl_chain/` — two-level CA corpus for CRL/OCSP issuer authority (F47/F1, phase-48)

Regenerate or verify with `tests/fixtures/gen_two_level_ca_signer.py`
(`--check` verifies the committed bytes against a fresh generation). Read that
script's docstring before changing anything here — every serial, the validity
window and the signature algorithm are load-bearing for reproducibility.

**The byte pin is gated.** `tools/gate/check_crl_chain_fixture.py` verifies
`crl_chain/MANIFEST.sha256` against the committed bytes, verifies that nothing
committed into the directory is unpinned, and runs `--check`. If you regenerate
this corpus you must regenerate the manifest in the **same** change, or that
gate reds. Its self-tests are `tools/gate/test_check_crl_chain_fixture.py`.

The shared C++ helpers for using this corpus (load, mint a CRL with a chosen
validity window and revoked serials, sign a PDF, install a scoped trust anchor)
live in `tests/support/CrlChainFixture.h`. Use them rather than copying — three
suites already share that one definition, and the copies are how a
self-signed-end-entity CRL gets reintroduced.

Two chains, produced by the **same** generator code path and differing in
exactly one boolean: whether the intermediate's `keyUsage` asserts `cRLSign`.
That is what makes the negative case a *control* rather than a
differently-shaped test.

| File | Purpose |
|---|---|
| `root.der` | Self-signed root CA (`keyCertSign + cRLSign`), serial `0x2001`. The trust anchor. |
| `inter_crlsign.der` / `inter_crlsign_key.pem` | Intermediate CA **with** `cRLSign`, serial `0x2002`. The key is committed so tests can mint CRLs with wall-clock-relative validity — a *committed* CRL would age out and redden the suite on a calendar boundary. |
| `leaf_crlsign.der` / `leaf_crlsign.pfx` | End-entity signer issued by that intermediate, serial `0x2003`. PKCS#12 password `test123`, bundling intermediate + root. |
| `inter_nocrlsign.der` / `inter_nocrlsign_key.pem` | Intermediate CA **without** `cRLSign`, serial `0x2004` — identical in every other respect. The negative control. |
| `leaf_nocrlsign.der` / `leaf_nocrlsign.pfx` | End-entity signer issued by *that* intermediate, serial `0x2005`. |
| `MANIFEST.sha256` | SHA-256 of the byte-deterministic members. The two `.pfx` files are deliberately absent from it — see below. |

**Validity is pinned to absolute instants**, 2020-01-01 → **2199-01-01**, never
`datetime.now()`. The tests verify at wall-clock time, so the corpus must be
valid *now* on any machine for the life of the repository.

**Determinism, stated exactly.** The five `.der` certificates and both `.pem`
keys are **byte-reproducible** (embedded keys, fixed serials, fixed window,
deterministic RSA PKCS#1 v1.5 signatures). The two `.pfx` files are **not**, and
cannot be: RFC 7292 mandates a fresh random salt and IV per serialisation.
`--check` compares those by unwrapped *content* instead, so provenance is still
establishable.

**Test-only key material with no relationship to any real CA.** The private keys
are embedded verbatim in the generator on purpose — that is what makes the
certificate bytes reproducible. Do not reuse them for anything.

Consumed by `tests/test_crl_issuer_keyusage.cpp` and
`tests/test_ltv_responder_validation.cpp`.

### OCR corpus (Phase 10)

| File | Scenario | Expected text sidecar |
|---|---|---|
| `test_ocr_text.pdf` | Clean synthetic "text-as-image" page | `test_ocr_text.expected.txt` |
| `test_ocr_clean.pdf` | 300-DPI synthetic clean scan | `test_ocr_clean.expected.txt` |
| `test_ocr_skewed.pdf` | 200-DPI synthetic scan with rotation + noise | `test_ocr_skewed.expected.txt` |
| `test_ocr_multicol.pdf` | Newspaper-style two-column layout | `test_ocr_multicol.expected.txt` |

Each expected-text file contains the source text verbatim; tests
compare OCR output against it with a recall threshold (default
≥95% for clean, ≥80% for skewed/multi-column).

> **Synthetic scans, not photographed scans.** The acceptance
> criterion's "real-world" language permits synthetic fixtures so
> long as the degradation characteristics are realistic. Noise and
> skew are applied deterministically via Pillow so the OCR recall
> numbers stay stable across runs.

### Pipeline a7c2e541 phase-3 corpus

Generated deterministically by `generate_phase3_fixtures.py`. Coverage
matrix in `coverage_matrix.md` maps each AC category to its covering
fixture; `validate_sidecars.py` cross-checks each `.expected.json`
against the actual PDF via PyMuPDF.

| File | Scenario | Sidecar tags |
|---|---|---|
| `phase3_v14_basic.pdf` | PDF 1.4 explicit version stamp; tiny <1KB | `pdf-1.4`, `tiny`, `vector-text` |
| `phase3_v20_minimal.pdf` | PDF 2.0 header (body 1.7); approximation flagged | `pdf-2.0`, `approximation` |
| `phase3_encrypted_aes128.pdf` | AES-128 V=4 R=4, user pw `user`; non-deterministic ciphertext IV | `aes-128`, `encrypted` |
| `phase3_form_all_field_types.pdf` | AcroForm: text, checkbox, combobox, listbox, multiline, signature | `acroform`, `all-field-types` |
| `phase3_annotations_all_types.pdf` | 11 annotation subtypes on one page | `all-annotation-types` |
| `phase3_cjk_text.pdf` | Japanese + Chinese + Korean body text | `cjk` |
| `phase3_rtl_arabic.pdf` | Arabic + Hebrew via `insert_htmlbox` | `rtl` |
| `phase3_cyrillic_greek.pdf` | Cyrillic + Greek + Latin Extended | `cyrillic`, `greek` |
| `phase3_utf16_metadata.pdf` | UTF-16 metadata, ASCII body | `utf-16-metadata` |
| `phase3_large_100page_image.pdf` | 100 pages, deterministic 320x240 noise PNG per page (~22MB) | `medium`, `100-page`, `image-heavy` |

### PDF version corpus (meta-audit follow-up F14)

Generated deterministically by `gen_pdf_version_corpus.py`. These drive
`tests/test_pdf_version_handling.cpp`, which pins the ISO 32000-1 §7.5.5 rule
that a catalog `/Version` overrides the `%PDF-x.y` header **only when it is
later**. MuPDF applies the override in *both* directions, so without these a
1.7 document carrying `/Version /1.4` is silently re-emitted as `%PDF-1.4` —
the save-downgrade defect F14 found and fixed.

Note the header value is deliberately read from the file's own first bytes:
`pdf_document::version` is clobbered with the catalog value at open time, so
it cannot be used to observe the header independently.

| File | Header | Catalog `/Version` | Purpose |
|---|---|---|---|
| `version_header_1_4.pdf` | 1.4 | *(absent)* | Header-only baseline, lowest version |
| `version_header_1_7.pdf` | 1.7 | *(absent)* | Header-only baseline, common version |
| `version_header_2_0.pdf` | 2.0 | *(absent)* | Header-only baseline, PDF 2.0 |
| `version_catalog_older_1_4.pdf` | 1.7 | 1.4 | Override is EARLIER — must be ignored (the downgrade case) |
| `version_catalog_raises_2_0.pdf` | 1.7 | 2.0 | Override is LATER — must win |

### Support data

| File | Purpose |
|---|---|
| `test_image_rgb.png` | Reference image for watermark / image-watermark tests |
| `test_image_with_exif.jpg` | EXIF-stripping test input |
| `test_pdf_with_images.pdf` | Image-manipulation tests |
| `test_fullpage_image.pdf` | Full-page image for dominated-page path |
| `test_scanned.pdf` | OCR routing test — image-only page |
| `test_scanned_with_ocr.pdf` | OCR routing test — same image as `test_scanned.pdf` plus an invisible (Tr=3) OCR text layer; required for the Group D atlas tests in `tests/ui_automation/test_canvas_click_subsystems.py` because TextEditTool needs clickable spans to drive the atlas-build pipeline. Regenerated by `make_test_scanned_with_ocr.py`. |
| `test_redaction.pdf` | Redaction workflow |
| `test_with_freetext_annot.pdf` | Document with a pre-existing FreeText annotation for editing-path tests |
| `test_with_bookmarks.pdf` | Document with pre-existing bookmarks (Outlines) for bookmark-panel + bookmark-edit tests. Added in pipeline 7d4b2f91 phase-C. |
| `test_doc_js.pdf` | Document with /Names /JavaScript willSave handler — Pipeline E phase-9 document-level JS event tests (regenerated by `make_doc_js_fixture.py`) |
| `test_10page.pdf` | 10 blank pages — Pipeline E phase-11 combine-files perf measurement (regenerated by `make_10page_fixture.py`) |
| `sample_hocr.xml` | Reference Tesseract HOCR output |
| `tessdata/` | Tesseract language models (eng.traineddata) |

### Render-oracle corpus (pipeline e7d3c2a1, UI render-verification layer)

Generated deterministically by `generate_render_oracle_fixtures.py` (except
the invoice, which is a verbatim copy of a real user document committed
directly). These drive the pixel-level render proofs in
`tests/ui_automation/test_render_oracle_proof.py`, each exercising one
systemic "coarse decision applied per-element" defect.

| File | Scenario | Defect exercised |
|---|---|---|
| `invoice_vector_heading.pdf` | Real invoice; big bold "Invoice" Arial-Bold 30pt heading | #1 — editing the vector heading blanks it (Tr=3 forced via page-wide ImageDominated misclassification); Ctrl-Z no-revert |
| `mixed_dpi_page.pdf` | Image-dominated page: a 300-DPI rasterised-text block (~62% of page) + a 150-DPI region | #2 — ImageReplacer re-renders the 150-DPI region at the page-max 300 DPI (upsample/smear) |
| `multilingual_latin_arabic.pdf` | One page with a **scanned** (rasterised) Latin text block and an Arabic-codepoint block — image content so OCR actually engages (a pure vector text layer does not reach PDFE's OcrPipeline render path; Tesseract saw nothing on the old vector version) | #3 — doc-wide OCR language baked at open instead of per-region |
| `mixed_content_page.pdf` | Live vector text block + a rasterised "scanned" image block on one page | #4 — page-wide `hasTextContent()` routes the whole page to the span tool, so the scanned block can't be region-edited |
| `nav_selfcheck.pdf` | Two visually very different pages (blue "PAGE ONE" / red "PAGE TWO", ~78% pixel delta) | render-oracle self-check control (page navigation must change the canvas) |
| `coord_rotation_page.pdf` | A `/Rotate 90` page carrying a text run whose own Tm matrix also encodes a 90-degree rotation (the double-rotation trap) | #6 — `CoordMapper` applied the page `/Rotate` as a blanket correction to every element, so a self-rotated run was rotated twice and pushed off-screen; `computeRunTransform` decomposes the run's own rotation and applies the page rotate only as the residual |
| `occluded_write_target.pdf` | Three images on one page, arranged so the recomposite's write-back target is **deterministically the wrong one**: a 1200×1200 base at `Rect(51,51,561,651)` (largest by pixel area), a 300×300 decoy at `Rect(400,100,472,172)` that exists only to share dimensions with the third, and a 300×300 top image at `Rect(80,470,224,614)` drawn **last** over the base, carrying the edited OCR span | GitHub #262 — region-aware selection bails out on the dimension tie, the fallback picks the base, and the top image paints over the result. The only arrangement in which the pre-write occlusion check can fire, so without it that control would be untestable. See `docs/audit/dca/0105-a-success-reported-without-checking-the-thing-it-claims.md` |

### Per-line font-size corpus (pipeline c4a9e271, UI render-verification layer)

Generated deterministically by `make_mixed_size_scan.py`. Drives the native-UI
visual proof in `tests/ui_automation/test_per_line_font_size_ui.py`.

| File | Scenario | Defect exercised |
|---|---|---|
| `mixed_size_scan.pdf` | One **scanned** (image-dominated) page with a 96px-bold heading line + four ~30px body lines (~3.2x glyph ratio), plus an invisible (Tr=3) OCR text layer so `hasTextContent()` routes it to the span-click flow (where the per-line fix lives) and `extractText()` yields clickable spans | per-line font size — clicking the heading vs a body line must give the overlay each line's OWN measured size (`clickedLineSizePt` → `setOverlayFontSize`), not the single region average; proven on two axes (`;ovsz=` readback + rendered glyph-ink height) |

### Embedded italic-serif subset corpus (pipeline b91f4e83, text-edit font identity)

Generated deterministically by `make_embedded_italic_serif_subset.py`. Drives the
gtest regression locks in `tests/test_textedit_font_identity.cpp` and the
render-verify proof in `tests/ui_automation/test_render_textedit_embedded_font.py`.

| File | Scenario | Defect exercised |
|---|---|---|
| `embedded_italic_serif_subset.pdf` | Two text lines (33pt heading + 13pt body) in ONE embedded Type0/Identity-H **subset** of a serif **bold italic** face (PANOSE serif, macStyle bold+italic, weight 700 — the `PalatinoLTPro-BoldItalic` profile). The unused letter `z` is unencodable in the embedded program, so it is the canonical "new char absent from the embedded subset" probe. | text-edit font identity — overlay must render italic+bold serif (defect 1b/1a); a newly-typed `z` must resolve to a **serif italic** fallback, not Arial sans (defect 3); two-line per-line font sizes track line heights (AC g). The only embedded italic-serif-subset fixture in the corpus (phase-4 gap §3.3). |
| `embedded_longname_italic_serif.pdf` | One 33pt heading in an embedded Type0/Identity-H **subset** of a serif bold-italic face renamed to a deliberately **>31-char** `/BaseFont` (`XXXXXX+Palatino Linotype Bold Italic`, 36 chars) whose 31-char truncation (`XXXXXX+Palatino Linotype Bold I`) **hides the trailing `Italic`**. Generated by `make_embedded_longname_italic_serif.py`. | untruncated `/BaseFont` recovery — MuPDF's `fz_font.name` is a `char[32]` buffer, so `fz_font_name()` clips long names and the overlay's name-parse loses italic. Proves `TextExtractor` now recovers the full `/BaseFont` from the PDF font dict so the overlay renders italic (gtest `TextEditUntruncatedBaseFont`). |
| `embedded_descriptor_style.pdf` | One 33pt heading in an embedded subset whose `/BaseFont` is style-**NEUTRAL** (`XXXXXX+Sutpenia Regular` — no bold/italic/oblique keyword) but whose `/FontDescriptor` carries `/Flags` Italic bit 7 + ForceBold bit 19 and `/ItalicAngle=-12` (forced via pikepdf). Generated by `make_descriptor_style_font.py`. | descriptor-based style detection — italic/bold detected here can ONLY come from the `/FontDescriptor`, not the name. Proves `TextChar`/`TextSpan.isItalic/isBold` are descriptor-sourced and that `buildOverlayFont` renders italic+bold for a keyword-free name (gtest `FontDescriptorStyleTest`). |

### ActualText prefix crash fixture (bugfix-3e91a7c4 #6, extraction crash)

Generated deterministically by `make_actualtext_fixture.py` (public-domain synthetic
content, no user data). Drives the crash regression `tests/test_actualtext_extraction_crash.cpp`.

| File | Scenario | Defect exercised |
|---|---|---|
| `actualtext_prefix_crash.pdf` | One page whose single span displays `ABC` inside a `/Span <</ActualText(A)>> BDC … EMC` marked-content block — the `/ActualText` string (`A`) is a proper **prefix** of the displayed glyphs (`ABC`). | Text-extraction crash. MuPDF 1.26.10 `do_extract_within_actualtext` (`stext-device.c:1142`) underflows its unsigned running length `z` to 0 while the span still has items, hitting `assert(z != 0)` (Debug abort) then `rune_index(actualtext, (size_t)-1)` OOB read (Release "Access Violation: read address 0x0"). PDFE extracts via every text path (search / Replace All / edit), so opening a real document with such a span and running Replace All crashed. Fix: PDFE sets `FZ_STEXT_IGNORE_ACTUALTEXT` (extract displayed glyphs, not the accessibility override) — extraction returns `ABC` and never enters the crashing path. |

### Small-text / offset-origin edit fixtures (bugfix-3e91a7c4 #4/#5, render/edit no-op)

Generated by `make_smalltext_edit.py` / `make_multipage_smalltext_edit.py` (public-domain
synthetic content). Drive `tests/test_smalltext_edit_render.cpp`.

| File | Scenario | Defect exercised |
|---|---|---|
| `smalltext_edit.pdf` | One page, **MediaBox `[0 7.9 594 745.9]`** (nonzero origin, like the real SpecimenA doc), with four single-word lines at 18 / 11 / 9 / 7 pt. | Content-stream text-edit NO-OP for small fonts. ContentStreamEditor's glyph matcher built the glyph box via `pageHeight - y`, discarding the page origin, while span boxes are CropBox-normalized — so on a nonzero-origin page the two diverge by Δ = origin.y0 and short (small-font) spans reject every glyph, leaving `/Contents` unchanged while the overlay shows the edit. RED: 11/9/7pt edits don't land; 18pt (control) does. Fix: map the glyph box with the page's `pdf_page_transform`. |
| `multipage_smalltext_edit.pdf` | THREE pages, same nonzero origin; each page has an 18pt title (`PAGEBIG0..2`) + a repeated 9pt body token (`REPLNINE`). | Same origin-offset no-op, exercised across pages: (1) editing the 9pt span on page 2 (index 1) must land; (2) Replace All (`MainWindow::onReplaceAll`) must update EVERY matched page and its reported count must equal the actual landed replacements. RED pre-fix: 9pt spans no-op (0 land) while the count reports 3. |

### Cumulative single-session journey corpus (pipeline f4a82c1e)

Generated by `make_journey_corpus.py` (dependency-free raw-PDF writer;
regeneration is deterministic — no timestamps/RNG). Drives the long-lived
journey soak `tests/test_ui_journey_soak.py` and its manifest
`journey_corpus_manifest.json`.

| File | Scenario | Defect class exercised |
|---|---|---|
| `journey_corpus.pdf` | 64-page CI-portable corpus cycling five page types `[body, actualtext, blank, image, origin]` by `i % 5`: body pages carry a repeated 8pt whole-word `the` (192 total across the doc) for content-edit / Replace-All, `/Span <</ActualText(A)>> BDC (ABC) Tj EMC` prefix spans, blank + raw-RGB image pages, and nonzero-origin MediaBox pages. | **Cumulative single-session state** — the corpus is driven through one long-lived session (annotate / move / delete / redact / rotate / delete-page / small-text edit / deep undo-redo / Replace-All / whole-doc scroll / save-reopen) to surface armed-crash, lost-changes, corruption, resource-leak, and undo-divergence defects that only manifest after a *sequence* of operations. See `tests/docs/testing-methodology-cumulative-single-session.md`. |

### Content-stream state-continuity corpus (pipeline e5b9c231 Phase 2)

Adversarial-ambient fixtures for the 6 content-stream injectors + adjacent
fixes (Register H1 / `docs/content-stream-state-continuity-audit.md`). Each has
a dependency-free deterministic generator (`gen_*.py`).

| File | Adversarial trait | Injector / fix exercised |
|---|---|---|
| `state_leak_ts.pdf` | Ambient `50 Ts` (text rise) set inside the BT and left active; edited span not last (inline-injection path). | ContentStreamEditor **inbound** leak — the edited replacement inherits the ambient rise (shifted ~50 units) unless the injection resets `0 Tc/0 Tw/0 Ts`. |
| `outbound_neighbor.pdf` | Two independent text objects; object 2 inherits object 1's ambient `-100 Tz`. | **Refutation** of the audit's predicted ContentStreamEditor OUTBOUND leak — proves MuPDF's filter wraps the injection in `q…Q` and re-materialises text state, so editing object 1 does NOT mirror the later object 2. |
| `ambient_white_fill.pdf` | Top-level `1 1 1 rg` (white) ambient fill, never reset; a black `NEIGHBOR` text line. | AlignmentWriter + HeaderFooterEngine **inbound** fill leak — appended text renders invisible (white) unless a known-basis `rg` is emitted. |
| `ambient_neg_tz_page.pdf` | Top-level `-100 Tz` (negative horizontal scale) left active. | BatchEngine text watermark — mirrored unless the injection resets `100 Tz`. |
| `ambient_half_scale_page.pdf` | Top-level unbalanced `0.5 0 0 0.5 0 0 cm` (half-scale ambient CTM), the N-up/imposed pattern. | Image insert/resize/rotate/crop + image watermark **CTM** leak — image drawn half-size/shifted unless the fragment is bracketed to identity CTM (`PageContentGuard`). |
| `redaction_partial_overlap.pdf` | A single word `SECRET` whose glyphs a redaction region can intersect without fully containing. | RedactionVerifier full-containment→intersection security fix — partial-overlap leftover text must be detected. |
| `hf_adversarial.pdf` | Non-zero MediaBox origin `[100 100 712 892]` and `/F1` bound to Courier (not Helvetica). | HeaderFooter/Watermark MediaBox-origin offset + HeaderFooter `/F1` collision (fresh Helvetica key). |

### Phase-3 adversarial corpus + oracle-repair fixtures (pipeline e5b9c231)

| File | Adversarial trait | Test / oracle |
|---|---|---|
| `js_sandbox_probe.pdf` | AcroForm text field with an `/AA/F` Format JavaScript action that reports `typeof` for 12 dangerous Acrobat APIs. | `JsSandboxRealOracle.*` — asserts leak/traversal/exfiltration APIs (System, Net, app.openDoc, submitForm, getURL, …) are `undefined` (real sandbox oracle, replacing EXPECT_NO_THROW). |
| `adv_cmyk_fill.pdf` | Ambient CMYK (`k`) cyan text fill. | `AdversarialAmbientCorpus.CmykFill_EditSurvives` — edit lands upright + stays cyan (not black). |
| `adv_dangling_q.pdf` | Dangling/unbalanced `q` leaking a grey graphics state. | `AdversarialAmbientCorpus.DanglingQ_EditSurvives`. |
| `adv_clip_path.pdf` | Active clip path around the edit region. | `AdversarialAmbientCorpus.ClipPath_EditNotClippedAway` — edit not clipped away. |
| `redaction_multi_revision.pdf` | Multi-revision PDF: a base object saved, then an incremental-update edit so a prior generation survives (object 3 at generation 1, verified maxgen=1). | Remediation P1 — `RedactionVerifierFailPathTest.CheckSingleRevision_MultiRevisionPdf_ReturnsFalse`: `checkSingleRevision` must return **false** when recoverable prior-revision data survives a redaction rewrite. Kills mutant M11 ("checkSingleRevision always reports clean"). F31 note: M11 was re-anchored on 2026-08-06 after commit `ed75a5a` (F1/PDF-68) replaced the generation-only heuristic it used to pin (`maxGeneration==0`→`true`) with a three-signal structural check; the mutant's effect is unchanged and this fixture still kills it — it trips all three signals (generation 1, `/Prev`, two `startxref`/`%%EOF` markers). |
| `dedup_duplicate_objects.pdf` | A PDF containing duplicate objects that full-GC (`do_garbage=3`) would merge. | Remediation P1 — `CommandPageRebindTest.FullGcEscalationDedupsDuplicateObjects`: a differential oracle — saving with `markNeedsFullGc()` (gc=3) yields strictly fewer objects (xref 6) than without (gc=1, xref 8). Kills mutant M09 (full-GC downgrade `3`→`1`). |
| `recovery_verbatim_streams.pdf` | An uncompressed content-stream ASCII marker plus a raw-image `0xAB` sentinel run, both stored verbatim. | Remediation P1 — `AutosaveRecoveryTest.RecoveryCopyPreservesVerbatimStreams`: the fast no-recompress recovery-copy write must keep both streams byte-verbatim. Kills mutants M16 (`do_compress=0`→`1`) and M08 (`do_compress_images=0`→`1`). |

See `docs/audit/phase3-adversarial-corpus.md` for the full hostile-class matrix.

### Adversarial-ambient CTM corpus (harness-remediation phase-2, R-33/R-34)

Lives in `tests/fixtures/adversarial_ambient/`, generated deterministically by
`gen_aa_corpus.py` (dependency-free raw-PDF writer). One minimal PDF per hostile
ambient graphics/text-state operator: a safe `KEEPME` neighbour in its own
`q…Q`, then ONE hostile operator left LIVE at the top level, plus (text family)
an `EDITME` span drawn UNDER the live state. Two families per op: `aa_<op>.pdf`
(text injectors) and `aa_img_<op>.pdf` (image injectors). Drives the
SUCCEED()-based discovery grid `test_content_stream_adversarial_ambient.cpp` and
the HARD glyph-quad oracles in `test_content_stream_ctm_reset.cpp`.

| File | Adversarial trait | Test / oracle |
|---|---|---|
| `aa_shear.pdf` | Live `1 0 0.5 1 0 0 cm` horizontal SKEW (determinant-1: height+width preserved, only the glyph left edge tilts). | `CtmReset.*UprightUnshearedUnderAmbientShear` (Alignment/HeaderFooter/TRE) — left-edge-slope oracle proves the guarded injection lands un-skewed. |
| `aa_rotate.pdf` | Live `+15°` rotate CTM (determinant-1, tilts BOTH edges). `EDITME` drawn under the rotate is the built-in tilted positive control. | `CtmReset.*UprightUnrotatedUnderAmbientRotate` (Alignment/HeaderFooter/TRE) — top-edge-slope oracle proves the guarded injection lands level (un-rotated). |
| `aa_mediabox.pdf` | Non-zero MediaBox origin `[100 100 712 892]` + live half-scale `0.5 0 0 0.5 0 0 cm` (imposition / N-up). Base content offset onto the visible box. | `CtmReset.HeaderFooterEngineLandsOnPageUnderMediaboxOrigin` — on-page-placement oracle proves the injected footer's left edge is a small positive margin (origin added), not shifted off by `-origin`. |
| `aa_img_shear.pdf` | Image family of `aa_shear.pdf` (solid-blue `Im0` neighbour). | Image-injector columns of the adversarial-ambient discovery grid. |
| `aa_img_rotate.pdf` | Image family of `aa_rotate.pdf`. | Image-injector columns of the discovery grid. |
| `aa_img_mediabox.pdf` | Image family of `aa_mediabox.pdf` (offset MediaBox + `Im0`). | Image-injector MediaBox-origin corpus completeness. |

The remaining `aa_<op>` / `aa_img_<op>` fixtures (`neg_tz`, `tc`, `ts`, `tr`,
`white`, `cm`, `dangling_q`, `cmyk`) cover the other hostile ambient operators
for the discovery grid; regenerate the whole set with `python gen_aa_corpus.py`.

### R-29 no-self-seed golden corpus (harness-remediation phase-6)

Committed, trusted-reference golden backing the golden-oracle no-self-seed
proof. NOT a screenshot: produced by a deterministic PIL generator from a fixed
spec so any machine regenerates byte-identical output and a reviewer can verify
correctness by reading the spec. Drives `tests/test_golden_no_selfseed.py`.

| File | Purpose |
|---|---|
| `ui_screenshots/baseline/reference/save_icon.png` | The committed known-correct golden (128×128, asymmetric red/blue halves + amber right-arrow + upper-left disk + baseline bar) so a MIRROR, HALF-SIZE, or WRONG-ICON render is detectably different. Regenerated by `ui_screenshots/generate_reference_goldens.py`. Proves `golden.compare_to_golden` REJECTS each defect class and never self-seeds (missing golden → FAIL, not auto-pass; `regen_golden` refuses without `PDFE_ALLOW_GOLDEN_SEED=1`). |

Regenerate (deliberate maintenance only):

```bash
python tests/fixtures/ui_screenshots/generate_reference_goldens.py
```

### Coverage-expansion phase-2 corpora (C-03 i18n render + C-04 format-compat)

Two deterministic corpora backing the phase-2 coverage-expansion test classes
(`docs/coverage-expansion/register.json`). Both regenerate reproducibly.

**C-03 i18n render** — generated by `gen_i18n_render_corpus.py` (PyMuPDF
`insert_htmlbox` → HarfBuzz bidi/joining/mark-attachment baked into the content
stream). Drives `tests/test_i18n_render.cpp`.

| File | Content | Oracle |
|---|---|---|
| `i18n_rtl.pdf` | Hebrew "shalom" + Arabic "marhaba" (`direction:rtl`) + Latin "LTR" control. | Hebrew logical-order x-centres strictly DECREASE (RTL); Latin control strictly INCREASES (non-vacuous); Arabic carries Presentation-Form-B glyphs (U+FB50..U+FEFF → contextual joining). |
| `i18n_cjk.pdf` | Chinese "中文测试" (SimSun) + a blank control band. | Exact CJK code points preserved, each glyph full-width square with real interior ink; blank band reads ~0 ink (meter non-vacuity). |
| `i18n_combining.pdf` | "x"+U+0301 COMBINING ACUTE vs "x"+U+00B4 SPACING ACUTE, each + "N". | Combining mark attaches (zero-advance, over the base); spacing accent advances the pen — differential proves attachment. |

Font note: Latin/Hebrew/Arabic/combining use `arial.ttf`; CJK uses `simsun.ttc`
(falls back through `msgothic.ttc`/`msyh.ttc`/`YuGothM.ttc`). A target without a
CJK font cannot build `i18n_cjk.pdf`; the generator fails loudly (SKIP=FAIL)
rather than emitting a Latin-fallback fixture.

**C-04 format-compat / migration** — generated by `gen_format_compat_corpus.py`
(PyMuPDF for the body/objstm/incremental/damaged-xref shapes, pikepdf/libqpdf for
linearization since PyMuPDF 1.27 dropped it). Each carries the shared tokens
`COMPATMARK`/`ONE`/`TWO` on two pages. Drives `tests/test_format_compat.cpp`.

| File | On-disk shape |
|---|---|
| `compat_objstm_xref.pdf` | Object streams (`/ObjStm`) + xref streams (PDF 1.5+ compact layout). |
| `compat_incremental.pdf` | Chained incremental updates — base body + two appended revisions (multiple `startxref` / `/Prev`). |
| `compat_linearized.pdf` | Linearized ("fast web view"): leading `/Linearized` dict + hint stream. |
| `compat_rebuilt_xref.pdf` | Classic xref with the startxref offset + every in-use xref offset zeroed, forcing reader reconstruction (object bodies intact → recoverable). |
| `compat_intact_xref.pdf` | The **intact twin** of the row above: the same document, written by the same `doc.save()` call, before the damage transform. Not a fifth on-disk shape — it is the independent reference `FormatCompat.RebuiltXrefMatchesIntactTwin` (issue #33 / PDF-86) compares the reconstruction against. Producing the reference by re-saving the damaged file would compare the repair with itself. |

The tests assert each shape opens (2 pages + known text), accepts an edit (a
tokened Text annotation), and survives save→reopen with the edit persisted and the
original content intact — a guard that a save-format change can't silently break
opening older/other-producer files. Structural distinctness is asserted separately
(`FormatCompatShape.*` reads the committed bytes) so the roundtrip can't pass
trivially on four identical files.

Regenerate:

```bash
cd tests/fixtures
python gen_i18n_render_corpus.py
python gen_format_compat_corpus.py
```

## Meta-audit follow-up F15 fixtures (forms & annotations)

| File | Purpose |
|---|---|
| `f15_annot_rotation.pdf` | PDF-52 annotation placement under page rotation: four 400×600 pages differing ONLY in `/Rotate` (0 / 90 / 180 / 270), each painted plain white so an annotation drawn by the test is the only ink. Generated by `gen_f15_annot_rotation.py`. |
| `f15_form_needappearances.pdf` | PDF-41 / PDF-42 appearance regeneration: AcroForm with authored `/NeedAppearances true`, a `/DR` Helvetica so an appearance *can* be built, and one text field `acct` deliberately carrying **no** `/AP`. Generated by `gen_f15_form_needappearances.py`. |
| `f15_unsupported_annots.pdf` | PDF-56 / PDF-59 subtype preservation: twelve annotations PDFE does not model — Polygon, PolyLine, Caret, Sound, Movie, Screen, Watermark, 3D, PrinterMark, TrapNet, RichMedia, Projection — each with authored `/Contents` (`F15-<Subtype>-marker`), `/NM` (`f15-annot-<i>`) and `/Rect`. Generated by `gen_f15_unsupported_annots.py`. |
| `f15_form_data.fdf` | PDF-40 FDF import (not a `.pdf`): Acrobat-shaped FDF targeting `test_form.pdf`'s fields with authored values, a hierarchical `/Kids` field, plus `/JavaScript` and `/F` entries that the importer must ignore. Generated by `gen_f15_form_data_fdf.py`. |
| `f15_form_hierarchy.pdf` | PDF-45 nested-field oracle: an AcroForm whose fields are only reachable through `/Parent` chains — two sibling groups (`billing`, `shipping`) each holding a child named `name`, so a PARTIAL field name is ambiguous and only the fully-qualified `billing.name` / `shipping.name` identifies one. Also carries a non-terminal field that owns `/FT` **and** `/Kids`, so the "has `/FT` ⇒ terminal" shortcut hides its children. This fixture is what made two real defects visible (partial-vs-qualified naming, and the terminal-field rule). Generated by `gen_f15_form_hierarchy.py`. |

Regenerate:

```bash
cd tests/fixtures
python gen_f15_annot_rotation.py
python gen_f15_form_needappearances.py
python gen_f15_unsupported_annots.py
python gen_f15_form_data_fdf.py
python gen_f15_form_hierarchy.py
```

## Meta-audit follow-up WP3 fixtures (UI / forms oracle repair)

Both fixtures exist because the oracles they serve previously had to
*discover* their preconditions from whatever an older fixture happened to
contain — which is what let those tests skip themselves, call themselves
"degenerate", or discard the verdict entirely. Every value the tests assert
is authored here and documented in the generator docstring.

| File | Purpose |
|---|---|
| `form_radio_group.pdf` | A1-61 radio-group exclusivity: ONE `/Btn` radio field `Choice` (radio + NoToggleToOff flags) with EXACTLY TWO kid widgets, so they are unambiguously in the same group. Authored export values `/OptionA` (left widget) and `/OptionB` (right widget), each kid's `/AP /N` carrying exactly `/Off` plus its own on-state so MuPDF resolves the on-state deterministically. Starts fully deselected (`/V = /Off`, both `/AS = /Off`). Generated by `gen_form_radio_group.py`. |
| `form_validate_js.pdf` | A1-60 JavaScript Validate verdicts: four text fields whose `/AA /V` outcomes are knowable without running the product — `ShortOnly` (`event.value.length <= 5`, boundary pair 5/6), `AlwaysReject` (`event.rc = false`), `InRange` (integers in `[10, 20]`, boundary pairs 9/10 and 20/21) and `NoValidateScript` (no `/AA` at all, so accepted by contract). Generated by `gen_form_validate_js.py`. |
| `form_importdata_action.pdf` | PDF-49 `/ImportData` inertness. Two text fields (`ImportTarget`, `SecondTarget`) with EMPTY `/V`, and one `/ImportData` action referenced from THREE places so "ignored" means ignored everywhere a viewer might look: the document `/OpenAction` (fires on open), a Link annotation's `/A` (fires on click), and each widget's `/AA /Fo` (fires on tab-in). Ships with `form_importdata_payload.fdf`, a real and valid FDF carrying the sentinel `PDFE_IMPORTDATA_EXECUTED` — **the payload has to be genuinely reachable, or a test that observes no import would pass on a viewer that implements `/ImportData` perfectly.** Generated by `gen_form_importdata_action.py`. |

| `form_comb_maxlen.pdf` | PDF-44 / GitHub #16 comb cell layout. TWO text fields that are geometrically identical — same 120x20pt rect, same `/MaxLen 6`, same `/DA /Helv 10 Tf`, same visible border — and differ in exactly one bit: `CombSix` sets **bit 25** of `/Ff` (`1 << 24`, comb) and `PlainSix` does not. Six 20pt cells means character *i* must be centred at `(i + 0.5) * 20`; `PlainSix` is the control that must be drawn as one ordinary left-aligned run, without which "the characters are present" would satisfy both. The border is required because MuPDF only draws the cell DIVIDERS on a bordered widget, and the font size is pinned (never `0 Tf` auto-size) so the cell arithmetic does not depend on the string. Generated by `gen_form_comb_maxlen.py`. |
| `form_aa_events.pdf` | PDF-46 / GitHub #17 authored `/AA` Format and Keystroke actions, chosen so **their output is necessarily different from their input** — that is what separates a working MuJS bridge from a no-op. `Amount` (`/AA /F`) turns `1234567.5` into `$1,234,567.50` using string operations only (no `parseFloat`, no locale, no Acrobat `AF*` host functions MuJS does not have); `NoFormatAction` has no `/AA` at all and is the negative control; `DigitsOnly` (`/AA /K`) sets `event.rc` false for any non-digit in `event.change` and true for the empty commit-pass change; `UpperCase` (`/AA /K`) REWRITES `event.change` to upper case, so committing `abc` must store `ABC` — a value no unexecuted bridge can produce. Generated by `gen_form_aa_events.py`. |
| `annot_measure.pdf` | PDF-57 / GitHub #21 page `/VP` measurement properties (ISO 32000-1 §12.9). THREE viewports whose scale is arithmetically knowable, so the oracle never asks the product what the document says. `SitePlan` covers the whole page at `/R "1 in = 10 ft"` with `/C 0.138889` (= 10/72 as pikepdf writes it — the raw bytes were checked) and `/D 100`, so a 72pt segment must display as exactly 10.00 ft and 144pt as 20.00 ft. `DetailCallout` is a SMALLER box INSIDE `SitePlan` at a different scale (`"1 in = 2 m"`) and appears LATER in `/VP`, because §12.9 says the last matching viewport wins and scanning the array forwards gives the opposite answer — a single-viewport fixture cannot tell the two apart. `NoMeasure` carries no `/Measure` at all and is the negative control against a reader that fabricates a default. Every number format also sets `/F`, `/RD`, `/RT`, `/PS`, `/SS` and `/O` to NON-default values, so the round-trip assertion covers the whole dictionary and not just the three entries the arithmetic reads. Generated by `gen_annot_measure.py`. |

Regenerate:

```bash
cd tests/fixtures
python gen_form_radio_group.py
python gen_form_validate_js.py
python gen_form_comb_maxlen.py
python gen_form_aa_events.py
python gen_annot_measure.py
```

## Meta-audit follow-up F18 fixtures (phase-2 synthesis backlog corpus)

Every class below was recorded by the meta-audit as having **zero fixtures**
(gap ids A3-33, A3-48, A3-49, A3-51, A3-52, A3-53). All six files are assembled
from raw bytes by one generator so byte offsets are exact and the result is
deterministic; the authored ground truth is documented in that generator's
docstring and is what `tests/test_f18_backlog_corpus.cpp` asserts against.

| File | Purpose |
|---|---|
| `f18_search_ground_truth.pdf` | A3-16 search oracle ground truth. 3 pages of `/Helvetica` with authored token counts: `NEEDLE` x6 (2/3/1), `needle` x2 (1/0/1), `HAYSTACK` x6 (3/2/1), so case-insensitive "needle" = 8 exactly. Replaces the `EXPECT_GE(totalMatches(), 0)` tautology. |
| `f18_no_tounicode.pdf` | A3-48 redaction-by-search false-negative class. **TWO Type 3 fonts sharing the same six glyph procedures and the same `/Widths`** — identical ink — and differing only in their `/Encoding /Differences` glyph names; neither has a `/ToUnicode`. `/T3` uses the Adobe Expert small-caps names `/Ssmall /Esmall /Csmall /Rsmall /Esmall /Tsmall`, which are in the AGL but map to the Corporate Use Subarea (`U+F773`…), so an unaided reader extracts Private Use codepoints that match nothing; PDFE's glyph-name recovery (`src/core/ToUnicodeRecovery.cpp`) strips the Expert suffix and recovers `SECRET`. `/T4` uses opaque `/gx1../gx6` names carrying no Unicode information at all, so that run stays unsearchable and keeps the residual half of the A3-48 class under test. The page also draws `VISIBLEMARK` in `/Helvetica` as a searchable control. Reworked for GitHub #23. |
| `f18_zero_page.pdf` | A3-49 / A3-33 0-page document: structurally valid, `/Type /Pages /Count 0 /Kids []`. |
| `f18_incremental_shadow.pdf` | A3-51 latest-object-wins `/Prev` chain. Revision 1 defines object 4 drawing `ORIGINALSECRET`; revision 2 redefines it to `UPDATEDPUBLIC`. Both byte runs are in the file; only revision 2 is reachable. |
| `f18_deeply_nested_array.pdf` | A3-52 deep-recursion parse boundary: a 200-level nested array hung off the page dict as `/F18Nest`, with the page still drawing `NESTHOST`. |
| `f18_10000page.pdf` | A3-53 / A3-33 scale boundary: exactly 10000 pages sharing one resources dict and three content streams (`FIRSTPAGE`, `FILLERPAGE`, `LASTPAGE`). |

`phase3_cjk_text.pdf` was an **orphan** (committed and inventoried, consumed by
no `.cpp`) until F18 — it is now consumed by
`F18CjkContentPreservation.CjkTextSurvivesSaveAndReopen` (gap A3-50).

Regenerate:

```bash
cd tests/fixtures
python gen_f18_backlog_corpus.py
```

## p84-remaining-open phase-7 fixtures (character/encoding extraction)

Gaps `PDF-71` / `PDF-73` / `PDF-75` — GitHub #22, #24, #25. Each closes a
text-extraction contract that had no on-disk oracle at all. All three are
assembled from RAW PDF BYTES: nothing PDFE writes is involved, because a fixture
produced by PDFE's own writer and read back would be a round-trip against
itself and would pass even if writer and reader shared the same wrong mapping.
Consumed by `tests/test_fidelity_r3_07_text_extraction.cpp`, which hard-ASSERTs
on their presence (they are pinned in the `_pdfe_required_fixtures` floor).

| File | Purpose |
|---|---|
| `i18n_vertical_cjk.pdf` | PDF-71 / GitHub #22 vertical writing (tategaki). The SAME three CJK characters (上 above / 中 middle / 下 below) drawn TWICE by two Type0 fonts that share ONE descendant CIDFont, ONE `/FontDescriptor` and ONE embedded font program — the only difference between the two font dicts is `/Encoding /Identity-V` versus `/Identity-H`. Any measured difference is therefore attributable to the writing mode and nothing else, and the horizontal run gives the OPPOSITE answer to the same two measurements so neither can pass by accident. The vertical run is ONE `Tj` holding three CIDs: the stacking comes from the vertical encoding plus `/DW2 [880 -1000]`, not from hand-placed coordinates, so the geometry oracle is a real oracle. The font is FULLY SYNTHETIC (fontTools `FontBuilder`, three bar glyphs whose position in the em box matches the character's meaning) — no third-party font bytes are committed and the generator needs no particular system font. Generated by `gen_vertical_cjk.py`. |
| `text_soft_hyphen.pdf` | PDF-73 / GitHub #24 the U+00AD SOFT (discretionary) HYPHEN, which is a different character from the U+002D hyphen-minus that `HyphenationPreservesAuthoredHyphen` already pins. The soft hyphen is authored unambiguously — `/Encoding /Differences` maps code `0xAD` to `/uni00AD` AND an explicit `/ToUnicode` states `<AD> -> <00AD>` for every code the page draws — so "the file contains U+00AD" is a fact about the bytes, not an inference about a reader. Four lines: `hyphen`+U+00AD / `ation` (a word broken at a discretionary hyphen), `well-being` (the U+002D CONTROL, without which the soft-hyphen count could not be told apart from a fold), and `un`+U+00AD+`breakable` (mid-line). The explicit `/ToUnicode` also keeps the fixture out of the way of `src/core/ToUnicodeRecovery.cpp`, which only ever touches a font with NO `/ToUnicode`. Generated by `gen_soft_hyphen.py`. |
| `encoding_edge_cases.pdf` | PDF-75 / GitHub #25 simple-font encoding on the READ side. Three `/Helvetica` fonts, none with a `/ToUnicode`, so the reader must go through the ENCODING. `/FW` draws codes `80 85 91 92 93 94 96 97 99` under `/WinAnsiEncoding` — exactly the band where WinAnsi diverges from Latin-1, which has C1 CONTROLS there — so they must extract as U+20AC, U+2026, U+2018, U+2019, U+201C, U+201D, U+2013, U+2014, U+2122 and a raw-byte reader is caught. `/FD` remaps the ASCII bytes for `ABC` via `/Differences [65 /bullet /Euro /trademark]`, so they must extract as U+2022, U+20AC, U+2122 — a reader that ignores `/Differences` produces the literal `ABC`. `/FC` draws the same three bytes with no `/Differences` and must give `ABC`, which is what stops the `/FD` assertion passing on a reader that always says bullet/euro/trademark. Expected codepoints come from PDF 32000-1 Annex D and the AGL, not from observing PDFE. Generated by `gen_encoding_edge_cases.py`. |

Regenerate:

```bash
cd tests/fixtures
python gen_vertical_cjk.py
python gen_soft_hyphen.py
python gen_encoding_edge_cases.py
```

## p84-remaining-open phase-7 fixture (layout / word boundaries)

Gap `PDF-76` (GitHub #26): a word boundary in a real PDF is frequently pure
GEOMETRY rather than a character. Assembled from RAW PDF BYTES with an
UNCOMPRESSED content stream and no whitespace inside the `TJ` arrays, so a
consumer can prove from the file bytes — with no PDF parsing at all — that the
space it sees in the extracted text was synthesized by the extractor and not
read off disk. Consumed by
`tests/test_fidelity_r3_07_text_extraction.cpp::KernedAndTjOffsetGapsReconstructWordBoundaries`,
which hard-ASSERTs on its presence (pinned in the `_pdfe_required_fixtures`
floor).

| File | Purpose |
|---|---|
| `text_word_boundary_tj.pdf` | PDF-76 / GitHub #26 word breaks from `TJ` numeric offsets, **with the control that makes the pair meaningful**. `[(Hello)-400(World)]TJ` and `[(Left)-700(Right)]TJ` contain no `0x20` byte anywhere, yet must extract with a word boundary — the offsets are 9.6pt and 16.8pt at Helvetica 24pt, against MuPDF's measured `~0.15 * fontsize` = 3.6pt space threshold (and −700 is kept below the ~−900 point at which MuPDF starts a NEW LINE instead of inserting a space). The controls use the SAME mechanism at a magnitude that must NOT break a word: `[(W)-20(a)-12(v)-18(e)-10(f)-14(r)-12(o)-16(n)-14(t)]TJ` — largest offset 0.48pt, ~7.5x BELOW the threshold — must extract as the single contiguous token `Wavefront`, and `[(Over)+15(lapping)]TJ` uses a POSITIVE offset, which pulls the next element LEFT and can therefore never be a word gap, so it must give `Overlapping`. Without those controls the fixture would be satisfied by an extractor that inserted a space at EVERY numeric offset, which would shred every kerned word in every real document. Expected page text is exactly `Hello World\nWavefront\nLeft Right\nOverlapping\n`. Generated by `gen_word_boundary_corpus.py`. |

Regenerate:

```bash
cd tests/fixtures
python gen_word_boundary_corpus.py
```

## Meta-audit follow-up F21 fixtures (adversarial signature corpus)

Gaps `PDF-33` (FieldMDP), `PDF-36` (shadow attacks / signature wrapping) and
`PDF-37` (`/ByteRange` file coverage). Consumed by
`tests/test_signature_shadow_attacks.cpp` and the `FieldMdpEnforcement.*`
tests in `tests/test_docmdp_enforcement.cpp`.

**Every file here is synthetic.** Each is hand-authored by the generator and
signed with the repository's own committed test key
(`test_signer.pfx`, password `test123`). **None is a captured or adapted
exploit against third-party software** — the attacks modelled are structural
properties of ISO 32000-1 §12.8 and every fixture targets PDFE's own parsing.

Authored ground truth (byte counts, expected verdicts, locked field names)
lives in `sig_shadow_corpus_manifest.json`; the tests read it from there rather
than anchoring to whatever PDFE reports.

| fixture | what it is |
|---|---|
| `sig_clean_signed.pdf` | The over-refusal **CONTROL**: really signed, untampered. `/ByteRange` covers all 9490 bytes except the 8192-byte `/Contents` hex string. |
| `sig_orphan_widget_signed.pdf` | The **F26 under-warn regression fixture**. A one-property change from the control: the document catalog carries no `/AcroForm`, so the applied signature is unreachable from `/Root /AcroForm /Fields` and visible only as a widget in the page's `/Annots`. Really signed, whole file covered (9474 bytes). Any component that answers "where are the signatures?" by walking the AcroForm field tree alone reports **zero** here — which is how `removeEncryption` came to destroy such a signature in silence. |
| `sig_shadow_hide.pdf` | Incremental-update **shadow attack (hide)**. The first 9490 bytes are byte-identical to the control, so the PKCS#7 digest still verifies; the 318 appended bytes redefine the page content stream to conceal the signed wording behind an opaque box. |
| `sig_shadow_replace.pdf` | Incremental-update **shadow attack (replace)**. Same shape, 303 appended bytes, and the page now reads "Pay EVIL Holdings the sum of 100000 USD." where the signed revision reads "Pay ACME Corporation the sum of 100 USD.". |
| `sig_wrapping_two_byteranges.pdf` | **Signature wrapping**. 2048 injected bytes are spliced into the middle of the file and `/ByteRange` is widened so the second range starts after them, leaving 2059 bytes inside the file outside every signed range. The signature is **re-signed over the manipulated ranges**, so the digest is genuinely valid — the only thing wrong with the file is its coverage. The injected block carries a decoy `/ByteRange` array, hence the name. |
| `sig_iso_docmdp_p1.pdf` / `sig_iso_docmdp_p3.pdf` | **ISO-layout certification**: `/Perms /DocMDP` is the signature dictionary and `/P` lives in `/Reference [ << /TransformMethod /DocMDP /TransformParams << /P n >> >> ]`, the layout Acrobat and pyHanko emit. The P1/P3 pair discriminates — a reader that ignores `/Reference` reports the `/P` default of 2 for both. |
| `sig_docmdp_multirev.pdf` | **Attacker-added revision chain** against the P1 certification: three appended incremental updates, each rewriting the document catalog (the object `DocMdpEnforcer`'s anchor advance keys off) plus one object no later revision touches. Exercises the one-section anchor cap. |
| `sig_fieldmdp_locked.pdf` / `sig_fieldmdp_unlocked.pdf` | The **FieldMDP discriminating pair**. Both certified at `/P 2` (form filling permitted), both carrying the same `/Lock /Action /Include /Fields [(locked_field)]`. The first's update changes `locked_field`; the second's changes `free_field`. A detector that cannot read `/Lock` returns the same answer for both. |

Regenerate:

```bash
cd tests/fixtures
python gen_sig_shadow_corpus.py
```

## Pre-existing `/DSS` fixtures (meta-audit follow-up F57, phase-52)

Every /DSS fault-injection case before F57 started from a document with **no**
`/DSS`, so `rollbackDss()` could only ever reach its "delete what we added"
branch and the "restore what was already there" branch was untested — in either
`/Root/DSS` shape. These two exist to reach it, and the tests assert
`pdf_is_indirect()` on each rather than assuming the shape survived signing.

| fixture | what it is |
|---|---|
| `dss_pre_direct.pdf` | Already carries a `/DSS` whose `/Root/DSS` slot is a **direct dictionary** — the shape PDFE itself writes (`pdf_dict_puts_dict`). Restored on rollback via `pdf_dict_puts()`. |
| `dss_pre_indirect.pdf` | Already carries a `/DSS` whose `/Root/DSS` slot is an **indirect reference** — the shape Acrobat writes. Restored on rollback via `pdf_update_object()`, a route no test executed before F57. Hand-built because the direct generator will not emit it. |

Regenerate:

```bash
cd tests/fixtures
python make_dss_pre_fixtures.py
```

## Licensing

All generated content is placed in the public domain. The test CA key
material (`test_signer.pfx`) is generated fresh by
`generate_signed_pdf.py` and exists only for the purpose of exercising
PKCS#7 signing paths in tests. It should never be trusted by anything
outside this repository.
