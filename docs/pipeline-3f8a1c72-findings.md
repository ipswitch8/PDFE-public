# Pipeline 3f8a1c72 — Phase 1 Investigation Findings

**Pipeline:** 3f8a1c72-d94b-4e6f-b203-9a5c0e817d24
**Phase:** 1 (investigation, read-only)
**Date:** 2026-06-22
**Purpose:** Pin the *evidence* needed to decide the Phase 2 (save recompression) and
Phase 3 (TRE per-line font properties) fixes — both of which were flagged as
*candidate* residual "coarse verdict → drives per-element behavior" anti-patterns
in the post-e7d3c2a1 sweep, explicitly pending confirmation. This document gates
both downstream phases.

---

## Track A — Save-path recompression (`do_compress_images` / `do_compress_fonts`)

### MuPDF 1.26.10 source behavior (CONCLUSIVE)

Source root: `vcpkg/buildtrees/libmupdf/src/1.26.10-*/source/pdf/pdf-write.c`.

**The flags never invoke an image/font re-encoder.** In `writeobject()` they only set
`do_deflate` and force `do_expand = 0`, routing the stream to `copystream()` (the
verbatim raw-bytes path), never to `expandstream()` (the decode-then-recompress path):

```c
// pdf-write.c:1129-1136 (writeobject)
if (opts->do_compress_images && pdf_is_image_stream(ctx, obj))
    do_deflate = opts->do_compress ? opts->do_compress : 1, do_expand = 0;
if (opts->do_compress_fonts && is_font_stream(ctx, obj))
    do_deflate = opts->do_compress ? opts->do_compress : 1, do_expand = 0;
if (is_jpx_stream(ctx, obj))
    do_deflate = 0, do_expand = 0;          // JPEG2000 explicitly never touched
```

**The decisive guard** is in `copystream()` — compression acts *only* on streams that
have **no existing `/Filter`**:

```c
// pdf-write.c:798,809  (copystream)
buf = pdf_load_raw_stream_number(ctx, doc, num);          // RAW (still-encoded) bytes
...
if (do_deflate && !pdf_dict_get(ctx, obj, PDF_NAME(Filter)))   // <-- only filterless streams
{
    if (is_bitmap_stream(...)) { /* CCITT G4 */ }
    else if (do_deflate == 1) { /* FlateDecode via deflatebuf() */ }
    else { /* BrotliDecode */ }
}
```

Any image already carrying `/Filter /DCTDecode` (JPEG), `/JPXDecode`, `/FlateDecode`,
`/CCITTFaxDecode`, or `/JBIG2Decode` keeps its filter → the `if` is skipped → the raw
bytes are copied **byte-for-byte**. There is **no libjpeg / DCT encoder anywhere in
`pdf-write.c`** — "compress images" can only mean Flate (or CCITT G4 for 1-bpc), and
only for *filterless* image data.

### Per-flag verdict

| Flag | Behavior on already-optimized objects | Verdict |
|---|---|---|
| `do_compress_images = 1` | Already-DCT/JPX/Flate/CCITT/JBIG2 images detected via `/Filter` presence → copied raw, untouched. Never re-encodes to JPEG (no JPEG encoder exists in write path). Only Flate-deflates *filterless* raw image data. | **SAFE — not wasteful, not lossy** |
| `do_compress_fonts = 1` | No font-subsetting code exists in the write path. Already-`FlateDecode` font programs skipped via the same `/Filter` guard. The `XXXXXX+` subset prefix is never inspected here and never re-subset. | **SAFE — not wasteful, not lossy** |

### PDFE call-site enumeration (10 sites)

| # | File:line | Function | gc | compress | img | font | notes |
|---|---|---|---|---|---|---|---|
| 1 | core/Document.cpp:277 | createBlank | default | 1 | – | – | blank doc |
| 2 | **core/Document.cpp:808** | **save** | gcLevel | 1 | 1 | 1 | preserve_metadata |
| 3 | **core/Document.cpp:957** | **saveAs** | gcLevel | 1 | 1 | 1 | preserve_metadata |
| 4 | core/Document.cpp:998 | saveIncremental | – | – | – | – | incremental, no compress (correct) |
| 5 | core/PageOperations.cpp:195 | extractPagesToFile | gcLevel | 1 | 1 | 1 | page graft → new doc |
| 6 | **encryption/EncryptionManager.cpp:260** | saveEncrypted | gcLevel | 1 | 1 | 1 | encrypt-then-save |
| 7 | **encryption/EncryptionManager.cpp:434** | removeEncryption | gcLevel | 1 | 1 | 1 | strip encryption |
| 8 | compliance/PdfAConverter.cpp:428 | convertAndSave | effGc | 1 | 1 | 1 | PDF/A |
| 9 | compliance/PdfUAConverter.cpp:107 | convertAndSave | effGc | 1 | 1 | 1 | PDF/UA |
| 10 | optimize/OptimizeEngine.cpp:36-38 | fillWriteOptions | opts | opts | opts | opts | **already user-toggleable** |

> Note: the EncryptionManager paths are at `src/encryption/EncryptionManager.cpp`
> (the sweep's "src/security/…" was the wrong directory — corrected here).

### Existing already-compressed detection (for reference)
`src/image/ImageManager.cpp` (`enumerateViaFilter`, ~lines 119-141) already detects
`DCTDecode` via `pdf_dict_get(ctx, xobj, PDF_NAME(Filter))` + name/array scan — the same
idiom MuPDF uses internally (`is_image_filter`, pdf-write.c:964-972).

### → Phase 2 recommendation
**No functional code change is warranted.** The "uniform recompression decision" is not
actually applied uniformly to already-optimized objects — MuPDF discriminates per-object
internally via the `/Filter`-presence guard. Adding a PDFE-side per-object guard would be
**redundant** (and would only duplicate logic MuPDF already performs correctly).

Phase 2 deliverable (per the criteria's "MuPDF already skips" branch):
1. A clarifying **code comment** at the warranted save call sites (Document.cpp save/saveAs,
   EncryptionManager.cpp ×2) recording *why* `do_compress_images/fonts = 1` is safe (cite the
   `/Filter` guard), so a future reader does not "fix" a non-bug or flip the flags off.
2. A **gtest round-trip fidelity test** (`tests/test_save_recompression.cpp`) proving the
   claim empirically: a PDF with a DCTDecode (JPEG) image, saved through `Document::save`,
   has its image stream **bytes preserved** (no re-encode); and an already-`FlateDecode`
   font program is not altered. This converts the source-reading conclusion into a
   regression-guarded invariant.
3. Compliance converters (PdfA/PdfUA) and PageOperations are **out of scope** — same safe
   behavior, and converters legitimately rewrite for compliance; a comment is sufficient.

---

## Track B — TRE region-averaged font properties (`FontMatcher::detectProperties`)

### Measurement methodology (honest note)
The TRE path consumes `HocrLine.xSizePx` from OCR of *scanned* pages. No `.hocr` fixtures
exist in the repo, and no `tesseract.exe`/`pytesseract` is available on this host to
generate OCR `x_size` directly. So per-line size heterogeneity was measured from the
**ground-truth font sizes** of the *vector* fixtures (PyMuPDF `get_text("dict")` span
`size`, in pt) — which is precisely the physical quantity OCR `x_size` proxies
(`x_size ≈ (x-height + ascenders) ∝ font size`). Ground truth is a *more* accurate
measure of "do real regions contain lines of different sizes," which is the question that
decides whether the per-line fix is warranted. (The scanned `multilingual` fixture yields
0 vector lines, as expected.)

### Per-line FONT SIZE variance (real data)

| Fixture | per-line sizes (pt) | n | CV (σ/μ) | actionable (>0.10) |
|---|---|---|---|---|
| invoice_vector_heading.pdf | 9.0 … 12.0 … **30.0** (heading) | 86 | **0.243** | **YES** |
| mixed_content_page.pdf | 10, 12, 12, 14 | 4 | **0.118** | **YES** |

A region/selection spanning a heading + body (the invoice case) yields a region-average
font size that is wrong for *both* the 30 pt heading and the 10–12 pt body. **Per-line
`fontSizePt` heterogeneity is real and actionable.**

### Per-line SERIF variance (real data)

| Fixture | fonts across lines | serif/sans mix within region? |
|---|---|---|
| invoice_vector_heading.pdf | Calibri, Arial, DejaVuSans (all sans) | **No — uniform sans** |
| mixed_content_page.pdf | TimesNewRomanPSMT (all serif) | **No — uniform serif** |

Serif/sans is **uniform within a region** in both fixtures. A user selection is normally a
single text block of one family. Moreover `detectSerif()` already does *per-character*
median analysis internally and is **more robust region-wide** than per-line (per-line has
fewer diagnostic glyphs → noisier). **Per-line serif is NOT warranted by the evidence**
and would likely *reduce* robustness on the common uniform case.

### → Phase 3 recommendations per property

| Property | Finding | Recommendation |
|---|---|---|
| **fontSizePt** | CV 0.118–0.243 across real fixtures — genuinely heterogeneous | **FIX: per-line `fontSizePt`.** Compute size per `HocrLine` from that line's `xSizePx` (+ its own baseline estimate); keep the region average only as a fallback for lines lacking valid data. |
| **isSerif** | Uniform within regions; per-line less robust | **KEEP region-wide**, add a code comment recording this finding. |
| **isItalic** | Documented deliberate tradeoff (per-word centroid slant → serif false positives) | **PRESERVE verbatim** — no per-line/per-word override. |
| **textColor** | Text within a region is one color in real documents; per-word color already flows through the style/save (`word.style`) path where it matters | **KEEP median**, add a code comment recording the explicit decision. |

---

## Summary — what Phases 2 & 3 will actually do

The investigation narrowed four *candidate* fixes to **one genuine functional change**:

- **Phase 2 (save):** documentation comment + round-trip fidelity **test** (no functional
  change — MuPDF already discriminates per-object; a redundant guard would be dishonest busywork).
- **Phase 3 (TRE):** **per-line `fontSizePt`** (the one measured, genuine heterogeneity);
  `isSerif`/`textColor` kept with documented rationale; `isItalic` tradeoff preserved.

This is the intended payoff of an investigation gate: it prevents shipping a redundant
save-path guard and a robustness-reducing per-line serif change, while confirming the one
fix (per-line font size) that real data supports.
