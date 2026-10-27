# Phase-2 Fixture Catalog — Adversarial Test-Harness Audit

**Scope:** every PDF under `tests/fixtures/` classified benign-vs-adversarial, with the
product path each exercises and its consuming test(s). FIND-AND-DOCUMENT only — no
product/test code was modified.

**Machine-readable companion:** `fixture-catalog.json` (same directory).

---

## Totals

| Metric | Count |
|---|---|
| Fixtures on disk (`**/*.pdf`) | **200** |
| Benign | **98** |
| Adversarial | **102** |
| Orphan (consumed by NO test) | **32** |
| Missing-referenced (test names a fixture that is absent) | **1** |

"Consumed by" counts only real test files (`.cpp` / `.py` under `tests/`, excluding the
fixture's own generator/downloader script). A generator is not a consumer.

---

## Reconciliation vs prior inventory

The prior `coverage_inventory.json` / `coverage_matrix.md` enumerate **81 fixtures**
(root + `image_pdf/` + `realworld/` structural variants) and were generated 2026-05-08.

- **Accuracy:** every one of the 81 prior entries is still present on disk with matching
  attributes. The prior inventory is *accurate for what it covers*.
- **Drift:** disk now holds **200** PDFs — **+119 added, −0 removed** since the prior
  inventory. The prior inventory is ~60% stale by count and predates the whole adversarial
  push. Newly-added, uncatalogued families:
  - `adversarial_ambient/` — 16 hostile-ambient-state fixtures (`gen_aa_corpus.py`)
  - `flaw_corpus/` — 47 malformed / corrupt / edge fixtures (external curated corpus)
  - `realworld/` downloaded real documents — 20 (IRS forms/pubs, arXiv papers, USCIS I-9, SSA SF86)
  - content-stream state-leak, render-oracle, embedded-font, OCR-variant, journey, and
    misc edge fixtures — ~36
- **Axis gap:** the prior inventory carries no benign-vs-adversarial dimension at all, which
  is exactly the blind spot this phase fills.

---

## Benign vs adversarial by product path

Each row = a product code-path; columns = how many benign vs adversarial fixtures reach it.
**Danger zones** (only benign inputs ever exercise the path) are flagged.

| Product path | Benign | Adversarial | Danger? |
|---|---:|---:|---|
| render/source-dpi (scanned `image_pdf/`) | 20 | 0 | **BENIGN-ONLY** |
| OCR / TRE | 8 | 0 | **BENIGN-ONLY** |
| open/parse (real-world docs: IRS/arXiv/etc) | 12 | 0 | benign-only (but real docs) |
| render-oracle | 5 | 0 | **BENIGN-ONLY** |
| render/text-edit (embedded font) | 3 | 0 | **BENIGN-ONLY** |
| compliance/pdfa | 6 | 0 | **BENIGN-ONLY** |
| compliance/tagging | 2 | 0 | **BENIGN-ONLY** |
| compliance/icc | 1 | 0 | benign-only |
| annotations | 1 | 0 | **BENIGN-ONLY** |
| save/rewrite (batch copy) | 1 | 0 | benign-only |
| form-fill (real gov forms) | 5 | 0 | benign-only |
| open/parse (misc: linearized, objstm, links, attachments, large, soak) | 7 | 0 | benign-only |
| render | 0 | 12 | adv-only |
| render/extract | 1 | 21 | mixed |
| open/parse | 15 | 16 | mixed |
| open/parse (decrypt) | 0 | 12 | adv-only |
| content-stream-edit | 2 | 28 | mixed |
| form-js | 0 | 3 | adv-only |
| metadata | 0 | 2 | adv-only |
| open/parse (permissions) | 0 | 2 | adv-only |
| page-ops | 1 | 2 | mixed |
| image-edit | 2 | 1 | mixed |
| redact | 1 | 1 | mixed |
| sign/verify | 3 | 1 | mixed |
| form-fill | 3 | 1 | mixed |

### Danger-zone summary (paths fed ONLY benign inputs)

1. **Scanned-image render / source-DPI** (`image_pdf/`, 20 fixtures, all benign). Every
   scanned-render and DPI-detection assertion runs on clean synthetic scans — no truncated
   image stream, no corrupt DCT/JBIG2/CCITT, no absurd DPI.
2. **OCR / TRE** (8 benign, 0 adversarial). Text-reconstruction is only ever fed clean
   OCR-able scans; no skew-past-threshold, garbled, or empty-glyph adversarial scan drives it.
3. **Annotations** (1 benign fixture, `phase3_annotations_all_types.pdf`). No malformed
   appearance-stream / recursive-AP / oversized-rect annotation fixture exists.
4. **Render-oracle & embedded-font text-edit** (8 benign, 0 adversarial). Visual-fidelity
   oracles never see a hostile-ambient or malformed-font page (those live in a *separate*
   corpus that the render-oracle tests do not consume — see orphans below).
5. **PDF/A & tagging compliance** (8 benign, 0 adversarial). Compliance conversion/validation
   is only exercised on already-clean fixtures, never on a spec-violating input.
6. **Save/rewrite** has no dedicated adversarial fixture; the rewrite path is exercised only
   as a side effect of benign `test_3page/test_single` documents.

> **Structural caveat (concentration):** the 47 `flaw_corpus/` adversarial fixtures are all
> consumed by a **single** test — `test_external_flaw_corpus.cpp` — as an open/parse smoke.
> So most "adversarial coverage" is one open-only test with (per the prior state-continuity
> audit) weak neighbour/mutation oracles, not per-product-path adversarial exercise.

---

## Orphan fixtures (32) — exist on disk, consumed by NO test

### Hard orphans — zero references anywhere (15)

The entire **image family** and 7 of 8 **text-family** hostile-ambient fixtures from
`gen_aa_corpus.py`. The generator emits 8 operators × {text, image} = 16 fixtures, but only
`aa_cm.pdf` is wired into a test (`test_content_stream_ctm_reset.cpp`,
`test_content_stream_multi_injector.cpp`). The other 15 hostile-state PDFs are generated and
then never asserted:

```
adversarial_ambient/aa_cmyk.pdf      adversarial_ambient/aa_img_cm.pdf
adversarial_ambient/aa_dangling_q.pdf adversarial_ambient/aa_img_cmyk.pdf
adversarial_ambient/aa_neg_tz.pdf     adversarial_ambient/aa_img_dangling_q.pdf
adversarial_ambient/aa_tc.pdf         adversarial_ambient/aa_img_neg_tz.pdf
adversarial_ambient/aa_tr.pdf         adversarial_ambient/aa_img_tc.pdf
adversarial_ambient/aa_ts.pdf         adversarial_ambient/aa_img_tr.pdf
adversarial_ambient/aa_white.pdf      adversarial_ambient/aa_img_ts.pdf
                                      adversarial_ambient/aa_img_white.pdf
```

### Generator/downloader-only orphans — no consuming test (17)

Referenced only by their generator or the corpus downloader:

- **Adversarial, uncovered:** `phase3_cjk_text.pdf`, `phase3_utf16_metadata.pdf`,
  `phase3_v20_minimal.pdf`, `realworld/cid_mixed/cid_mixed_encoding.pdf`,
  `realworld/mixed_page_sizes/mixed_page_sizes.pdf`,
  `realworld/subsetted_fonts/subset_prefix_conflict.pdf` — exotic-script / CID / version-edge
  / subset-conflict PDFs that are *generated as adversarial* but no test asserts anything.
- **Benign, uncovered:** `realworld/object_streams/object_streams.pdf`,
  `image_pdf/b3_blue_ledger_300dpi.pdf`, `b3_gradient_shaded_300dpi.pdf`,
  `b3_highlighted_yellow_300dpi.pdf`, `b3_jpeg_blocky_200dpi.pdf`,
  `c2_color_ledger_10page.pdf`, `c2_large_12page_600dpi.pdf`, `c2_mixed_8page.pdf`,
  `c2_small_6page_300dpi.pdf`, `c2_tiny_20page_72dpi.pdf`, `test_ocr_multicol.pdf`.

(`batch_output/test_single.pdf` is a duplicate-basename build artifact, not a source fixture.)

---

## Missing-referenced fixtures (1)

- **`specimen_a_green.pdf`** — expected at `tests/fixtures/specimen_a_green.pdf`, referenced by
  `tests/test_ui_specimen_a_green_subset.py` (`FIXTURES_DIR / "specimen_a_green.pdf"`) and
  `tests/diagnose_live_toolbar_font.py`. The file is **absent**; the test falls back / is
  marked **xfail (P-G-INFRA-1)** → a **silent skip** masking a subsetted-font round-trip check.

### Not missing (verified generated at runtime)

The ~600 other `.pdf` tokens in tests that have no on-disk file are **not** silent skips:
they are output artifacts (`*_out.pdf`, `working.pdf`, `saved.pdf`, `pdfe_*.pdf`) or
in-test-synthesized inputs. Notably the entire **CVE corpus** (`cve_2017_*`…`cve_2024_*`,
~24), **bomb/malicious** (`decompression_bomb.pdf`, `embedded_files_bomb.pdf`,
`hostile_js.pdf`, `deeply_nested_dict.pdf`, `circular_objref.pdf`), and **malformed-filter**
(`ascii85_malformed.pdf`, `lzw_malformed.pdf`, etc.) fixtures are **written at test runtime**
via `std::ofstream` in `test_malicious_pdf_advanced.cpp` / `test_malicious_pdf_security.cpp`
— they are real adversarial coverage that simply lives in code, not on disk, so they do NOT
appear in this on-disk catalog.

---

## Method notes

- Enumeration: `find tests/fixtures -name '*.pdf'` (200 files).
- Consumption: substring scan of every `.cpp/.h/.py` under `tests/` for each fixture basename;
  generator/downloader references filtered out of the consumer list.
- Classification: filename + generator-script intent (e.g. `gen_aa_corpus.py` header
  documents each fixture as "one hostile ambient operator left LIVE") + consuming-test purpose.
- Missing-fixture detection scoped to references containing a `fixtures/` path segment to
  exclude runtime-output filenames.
