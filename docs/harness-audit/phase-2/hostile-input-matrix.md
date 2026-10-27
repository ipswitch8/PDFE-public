# Hostile-Input Coverage Matrix — Harness Adversarial Audit, Phase-2

**Generated:** 2026-07-28 · **Method:** Equivalence-Partitioning / Boundary-Value-Analysis / FMEA
**Scope:** FIND-AND-DOCUMENT only — no product/test code changed.
**Rule for "nonzero":** a test must actually **feed** the hostile input to a real product
path (open/render/extract/edit/save/sign/redact/permissions/js-eval) **and assert
something**. Fixture-without-consumer, or one-path-covered-while-siblings-blind, = **partial**.
No fixture and no consumer = **zero**.

**Totals: 38 classes — 28 nonzero · 6 partial · 4 zero.**

Machine-readable companion: `hostile-input-matrix.json`.

---

## Coverage matrix (class × coverage × product paths)

### Graphics / content

| Class | Coverage | Product paths covered | Uncovered / blind | Key evidence |
|---|---|---|---|---|
| Ambient graphics state (neg Tz / Tr3 / white / non-black / dangling q / Tc / Ts) | **nonzero** | textedit-inject, render, save | image-edit + TRE injectors (SUCCEED-only probe, never asserted) | `test_adversarial_ambient_corpus.cpp`, `test_content_stream_neg_tz/_state_leak/_multi_injector.cpp`, `test_hf_watermark_adversarial.cpp`; measurement probe `test_content_stream_adversarial_ambient.cpp` |
| Imposed / half-scale CTM (`cm`) | **nonzero** | textedit-inject, render, save | image-edit under imposed CTM (measured only) | `test_content_stream_ctm_reset.cpp`, `test_content_stream_multi_injector.cpp` (fix a03f6b6) |
| **Sheared / skewed CTM** (off-diagonal `cm`) | **ZERO** | — | textedit-inject, image-edit, render, save | none — corpus has half-*scale* cm but no shear |
| MediaBox non-zero origin | **nonzero** | crop, HF/watermark centering, render | inline text-edit + image-edit + redact under offset origin | `test_mediabox_origin_crop.cpp`, `test_hf_watermark_adversarial.cpp` |
| Rotated pages (/Rotate 90/180/270) | **partial** | open, render | edit / redact / crop on a rotated page | `test_external_flaw_corpus.cpp` (render-no-throw), `test_coord_mapper_rotation.cpp` (math) |
| Clip paths (W/W*) | **nonzero** | textedit-inject, render, extract | — | `test_adversarial_ambient_corpus.cpp` (ClipPath_EditNotClippedAway), `test_external_flaw_corpus.cpp` |

### Structure

| Class | Coverage | Product paths covered | Uncovered / blind | Key evidence |
|---|---|---|---|---|
| Encrypted RC4 (40/128) ±password | **nonzero** | open, permissions | RC4 re-encrypt roundtrip | `test_external_flaw_corpus.cpp` (Standard40/128) |
| Encrypted AES (128/256/R5/R6) ±password | **nonzero** | open, permissions, save | — | `test_external_flaw_corpus.cpp`, `test_malicious_pdf_advanced.cpp`, `test_encryption_roundtrip.cpp` |
| Certificate (public-key) encryption | **nonzero** | open (graceful-fail) | — | `test_external_flaw_corpus.cpp` (CertificateEncryption_FailsGracefully) |
| Linearized | **nonzero** | open, save | — | `test_linearization.cpp` (QPDF isLinearized oracle), `test_malicious_pdf_advanced.cpp` |
| XFA forms | **partial** | open, save | form-fill / render / extract of XFA | `test_realworld_external_corpus.cpp` (SaveRoundTrip), `test_xfa_save_benchmark.cpp` |
| Incremental-update chains | **partial** | save, open | latest-object-wins correctness on adversarial /Prev chain | `test_incremental_save_routing.cpp`, CVE_2024_25088 (/Prev cycle) |
| Object streams / xref streams | **nonzero** | open, render | — | `test_realworld_corpus.cpp`, `test_malicious_pdf_advanced.cpp` (ObjectStreamCorruption) |
| Damaged / rebuilt xref | **nonzero** | open, render | — | `test_external_flaw_corpus.cpp` (XrefCommandMissing + 5 more), CVE_2019_19797 |

### Malformed

| Class | Coverage | Product paths covered | Uncovered / blind | Key evidence |
|---|---|---|---|---|
| Truncated file | **nonzero** | open, render | — | `test_external_flaw_corpus.cpp` (OneByteMissing) |
| Malformed/truncated content stream + hostile filters | **nonzero** | open, render | output-correctness (only no-crash) | `test_malicious_pdf_advanced.cpp` (all 5 filters), CVE replays |
| Circular / recursive refs | **nonzero** | open, render | — | `test_malicious_pdf_advanced.cpp` (RecursivePopup / CircularObjRef), CVE_2024_25088/2020_9543 |
| Missing required keys | **nonzero** | open, render, extract | — | CVE_2017_9408 (no Catalog), CVE_2019_9543, MalformedPageLabels |
| Oversized / negative numbers | **nonzero** | open, render | — | CVE_2019_7654 / 2018_17000 / 2022_3878, HugeMediaBox |
| Deeply nested arrays / dicts | **partial** | open | nested **arrays**; nesting deeper than 100 | `test_malicious_pdf_advanced.cpp` (100-level dict only) |

### Scale / boundary

| Class | Coverage | Product paths covered | Uncovered / blind | Key evidence |
|---|---|---|---|---|
| **0-page document** | **ZERO** | — | open, render, thumbnail, autosave, save | no /Count-0 fixture |
| 1-page | **nonzero** | open, render, extract, edit, save | — | `test_single.pdf` (ubiquitous) |
| **10k-page** | **ZERO** | — | open, nav, thumbnail, save | max fixture = 100 pages |
| Huge single page (>14400) | **nonzero** | open, render | — | HugeMediaBoxOver14400, CVE_2022_3878 |
| Tiny page (ArtBox<3) | **nonzero** | open, render | — | TinyArtBoxUnder3 |
| Zero-length streams | **nonzero** | open, render | — | MultipleFiltersLengthZero |

### Text / i18n

| Class | Coverage | Product paths covered | Uncovered / blind | Key evidence |
|---|---|---|---|---|
| CID fonts (Type0 / Identity-H / CMap overflow) | **nonzero** | open, render, extract | edit into CID-only page | `test_external_flaw_corpus.cpp` (3 CID tests), `test_cid_font_encoding.cpp` |
| RTL (Arabic / Hebrew) | **nonzero** | open, render, extract | RTL editing (bidi caret) — Low residual | `test_fidelity_r3_07_text_extraction.cpp` (Unicode-block oracle) |
| CJK (Chinese/Japanese/Korean) | **partial** | open, render | content-preservation extraction; CJK edit | fixture `phase3_cjk_text.pdf` has no asserting consumer |
| **Combining marks / ZWJ / grapheme clusters** | **ZERO** | — | extract, textedit caret/delete, render | no fixture |
| ToUnicode-absent + malformed ToUnicode | **partial** | open, render, extract(no-throw) | absent-ToUnicode extraction **correctness** + redaction-by-search | `test_malicious_pdf_advanced.cpp` (malformed only), CVE_2017_7438 |
| ActualText | **nonzero** | open, extract | — | `test_actualtext_extraction_crash.cpp` (fix 5873dd9) |

### Security

| Class | Coverage | Product paths covered | Uncovered / blind | Key evidence |
|---|---|---|---|---|
| JavaScript / OpenAction | **nonzero** | open, js-eval | — | `test_document_js_events.cpp`, CVE_2018_4993, JsApiSurfaceTest |
| Launch actions (/Launch) | **nonzero** | open, annotate, save | — | `test_launch_link_follow.cpp`, `test_link_actions.cpp` |
| Embedded files / portfolios | **nonzero** | open, extract | — | ManyEmbeddedFilesBounded, `test_attachments.cpp` |
| /AA additional-actions | **nonzero** | open, js-eval, form-fill | — | JsApiSurfaceTest (fires /AA/K), CVE_2021_30560 (/AA/WC) |
| form-JS-XSS | **nonzero** | js-eval, form-fill | — | `test_form_javascript_xss.cpp` |
| Decompression bomb | **nonzero** (weak) | open, render | memory-cap enforcement; giant-nested bomb excluded by design | `test_malicious_pdf_security.cpp` (time-bound only) |

---

## Prioritized adversarial-fixture backlog (zero + partial classes)

| # | Class | Fixture to add | Product path | Oracle | Failure it catches | Priority |
|---|---|---|---|---|---|---|
| 1 | **Sheared/skewed CTM** | `adversarial_ambient/aa_shear.pdf` + `aa_img_shear.pdf` (live off-diagonal `1 0.4 0 1 0 0 cm`) | All content injectors (add `"shear"` to the `ops[]` vector in `test_content_stream_adversarial_ambient.cpp`) | Glyph-quad parallelogram check (ul.y≈ur.y, not slanted) + upright-ROI ink | Injector that fails to reset a sheared ambient CTM → slanted/parallelogram output — the direct sibling of the -100 Tz mirror bug this audit exists for | **High** |
| 2 | **Rotated-page edits** | reuse `realworld/rotated_pages/rotated_mixed.pdf` or `rotated_edit_page.pdf` (EDITME + KEEPME on /Rotate 90 & 270) | ContentStreamEditor / ImageContentEditor / RedactionEngine → saveAs → reopen | Render reopened page (applies /Rotate); assert edit lands upright at expected on-screen position | Misplaced/rotated edits & redactions on rotated pages (common workflow, no landing-position oracle) | **High** |
| 3 | MediaBox non-zero origin — inline edit + redact | reuse `mediabox_offset.pdf`; add EDITME span + redact target at known offset coords | ContentStreamEditor + RedactionEngine under offset MediaBox | Colour-at-known-coordinate (as in the crop test) | Off-by-origin edit/redaction placement (ambient corpus uses a 0-origin box) | Med |
| 4 | ToUnicode-absent extraction + redaction-by-search | `fonts/no_tounicode_simple.pdf` (known string, no /ToUnicode) | `extractPlainText` + redaction-by-text-search | Known string recovered legibly; redaction actually covers glyphs (pixel) | SECURITY false-negative: redaction-by-search misses text because extraction returned wrong codepoints | Med |
| 5 | Combining/ZWJ grapheme clusters | `i18n/grapheme_clusters.pdf` (combining diacritics, ZWJ emoji, Devanagari/Thai cluster) | TextEditTool caret/backspace/selection + extraction normalization | Caret/backspace operate on whole clusters; extraction round-trips codepoints | Delete-half-a-cluster corruption, caret desync, or crash on multi-codepoint clusters | Med |
| 6 | 0-page document | `corrupt/zero_page.pdf` (valid catalog, /Count 0, empty /Kids) | open → first-page render, thumbnail strip, autosave, saveAs | pageCount()==0 and no downstream path crashes/UB | Null/OOB when the app assumes ≥1 page exists — crash on a legally-empty doc | Med |
| 7 | Image-edit / TRE injectors under ambient state (hard oracle) | none — fixtures exist (`aa_img_*.pdf`); promote SUCCEED cells to asserts | ImageManager insert / ImageContentEditor resize·rotate·crop / TRE | Existing `evalImageCell`/`evalTextCell` booleans → EXPECT_TRUE | Image/TRE injector leaks ambient state (halved/mirrored/invisible image) recorded only as data today | Med |
| 8 | CJK content-preservation extraction | none — `phase3_cjk_text.pdf` exists; add consumer | `extractPlainText` on `phase3_cjk_text.pdf` | Unicode-block oracle (Hiragana/Katakana/CJK-Unified/Hangul survive, FFFD not dominant) | Dropped/mangled CJK regression passes today (RTL has an oracle, CJK doesn't) | Med |
| 9 | Incremental-update latest-object-wins | `structure/incremental_shadow.pdf` (object redefined in rev 2 via /Prev) | open → extract/render | Newest definition is the one used | Reading a stale earlier object revision (wrong content) | Low |
| 10 | Deeply nested arrays | `corrupt/deeply_nested_array.pdf` (1000-level array) | open | bounded-time + no-crash | Stack overflow on deep array recursion (only dict nesting guarded) | Low |
| 11 | 10k-page scale boundary | `generate_10kpage.py` → `test_10000page.pdf` (git-ignored, generated) | open + thumbnail/nav virtualization + saveAs | perf budget on open/nav; lazy thumbnails; bounded GC | O(n²) page-list build, eager thumbnails, save-GC blowup at upper boundary | Low |

---

## Notes on method / caveats

- **Ambient-graphics-state confirmed NONZERO** (the class the recent content-stream work
  targeted). The **text** injectors — ContentStreamEditor, AlignmentInjector,
  HeaderFooterEngine, Watermark — have real pixel + glyph-quad oracles. The **four
  image-edit injectors and the TRE writer** are exercised against every hostile op in
  `test_content_stream_adversarial_ambient.cpp`, but that file is a `SUCCEED()`-only
  **measurement probe** (0 EXPECT/ASSERT — flagged in census `low_assertion_files`): a
  CORRUPT cell becomes gap-register data, not a red test. Backlog item #7 closes this.
- Many malformed/CVE classes are **nonzero with a no-crash / bounded-time oracle**, not an
  output-correctness oracle. That is appropriate for DoS/robustness classes but means a
  *wrong-but-not-crashing* output would still pass — noted per-class where it matters
  (filters, decompression bomb).
- `gen_aa_corpus.py` generates the **adversarial-ambient** corpus (aa_<op>.pdf), not
  "/AA additional-actions" — do not confuse the two. /AA is covered separately via the
  JsApiSurfaceTest keystroke path and CVE_2021_30560.
