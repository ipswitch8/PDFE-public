# Research Report: OCR, Text Search, and Font Libraries for Cross-Platform PDF Editor

**Date:** 2026-02-05
**Scope:** Library evaluation for Adobe Acrobat Pro-level PDF editor
**Platforms:** Windows, Linux, macOS

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [OCR Libraries](#2-ocr-libraries)
3. [Text Search / Indexing Libraries](#3-text-search--indexing-libraries)
4. [Font Matching / Management Libraries](#4-font-matching--management-libraries)
5. [Open-Source Font Collections](#5-open-source-font-collections)
6. [Recommended Architecture](#6-recommended-architecture)
7. [Integration Challenges](#7-integration-challenges)
8. [Sources](#8-sources)

---

## 1. Executive Summary

After evaluating over 20 libraries and font collections across the four categories, the recommended stack is:

| Category | Primary Recommendation | Secondary / Supplement |
|---|---|---|
| OCR Engine | **Tesseract 5.x** (C++ API with HOCR) | PaddleOCR (optional high-accuracy backend) |
| OCR Orchestration | **OCRmyPDF** (reference architecture) | Custom pipeline modeled on OCRmyPDF |
| Text Search | **Tantivy** (via tantivy-py or C FFI) | SQLite FTS5 (lightweight fallback) |
| Font Matching | **FreeType + HarfBuzz + custom PANOSE matcher** | font-kit (Rust, for discovery layer) |
| Font Collection | **Google Noto** (core + optional CJK packs) | Liberation + DejaVu (metric-compatible fallbacks) |

The rationale for each selection follows in the detailed analysis below.

---

## 2. OCR Libraries

### 2.1 Tesseract 5.x

| Attribute | Detail |
|---|---|
| **License** | Apache 2.0 |
| **Language** | C++ (with C API, Python/Java/etc. wrappers) |
| **Languages Supported** | 100+ languages, 35+ scripts via traineddata packs |
| **HOCR Support** | Native -- `GetHOCRText()` API and CLI `--hocr` flag |
| **Accuracy** | Average confidence ~0.89; LSTM engine significantly improved over legacy |
| **Cross-Platform** | Windows, Linux, macOS -- well-established build system |
| **Maintenance** | Active; v5.x stable branch, maintained by Stefan Weil and community |
| **Integration** | libtesseract can be linked statically or dynamically; requires Leptonica |

**Strengths:**
- Native HOCR output with character-level bounding boxes and confidence scores.
- The `lstm_choice_mode` parameter enables alternative character choices in HOCR output, which is valuable for post-processing and correction workflows.
- Mature C++ API (`TessBaseAPI`) with well-documented integration patterns.
- Smallest dependency footprint of the evaluated engines.
- Traineddata packs available in three tiers: `tessdata_best` (highest accuracy), `tessdata` (balanced), `tessdata_fast` (speed-optimized).
- Can be embedded directly into a C++ application via `#include <tesseract/baseapi.h>`.

**Weaknesses:**
- Lower accuracy than PaddleOCR on complex layouts (tables, multi-column).
- Accuracy is heavily dependent on image preprocessing (deskewing, binarization, noise removal).
- No built-in layout analysis as sophisticated as PaddleOCR's or Kraken's.
- Scene text / curved text recognition is weak compared to deep-learning approaches.

**Verdict:** **PRIMARY RECOMMENDATION.** Tesseract's native HOCR support, C++ embeddability, Apache 2.0 license, and proven cross-platform maturity make it the best fit for this project. Its accuracy limitations can be mitigated with preprocessing and optional PaddleOCR fallback for difficult documents.

---

### 2.2 PaddleOCR

| Attribute | Detail |
|---|---|
| **License** | Apache 2.0 |
| **Language** | Python primary; C++/Java/Go/C#/Node.js deployment supported |
| **Languages Supported** | 100+ languages |
| **HOCR Support** | Limited -- outputs JSON with bounding boxes; no native HOCR renderer |
| **Accuracy** | Average confidence ~0.93; highest among evaluated open-source engines |
| **Cross-Platform** | Windows, Linux, macOS (GPU acceleration requires CUDA on Win/Linux) |
| **Maintenance** | Very active; v3.0.3 (June 2025), PP-OCRv5 architecture |
| **Integration** | Requires PaddlePaddle framework; heavier dependency chain |

**Strengths:**
- Highest accuracy among traditional (non-LLM) OCR engines.
- Superior layout analysis including table detection, multi-column recognition.
- PP-OCRv5 provides character-level coordinate output.
- C++ deployment path available for production integration.
- Active development with frequent releases.

**Weaknesses:**
- No native HOCR output; would require a custom conversion layer from PaddleOCR's JSON coordinate format to HOCR XML.
- PaddlePaddle framework is a substantial dependency (~500MB+ with models).
- GPU strongly recommended for competitive performance; CPU-only mode is significantly slower.
- Cross-platform build complexity is higher than Tesseract.
- Community and ecosystem are primarily China-centric; documentation quality in English is uneven.

**Verdict:** **SECONDARY / OPTIONAL.** Excellent as a high-accuracy fallback engine for documents where Tesseract struggles. Not recommended as the sole engine due to HOCR limitations and dependency weight. A custom HOCR adapter would need to be written to convert PaddleOCR's JSON coordinate output to compliant HOCR XML.

---

### 2.3 EasyOCR

| Attribute | Detail |
|---|---|
| **License** | Apache 2.0 |
| **Language** | Python (PyTorch-based) |
| **Languages Supported** | 80+ languages |
| **HOCR Support** | None -- outputs list of [coordinates, text, confidence] |
| **Accuracy** | Average confidence ~0.85; weakest of the three main engines |
| **Cross-Platform** | Windows, Linux, macOS (CPU or GPU) |
| **Maintenance** | Active but slower release cadence than PaddleOCR |

**Verdict:** **NOT RECOMMENDED.** Lower accuracy than both Tesseract and PaddleOCR. No HOCR output. PyTorch dependency is heavy. The only advantage (simpler Python API) is irrelevant for a C++-based PDF editor.

---

### 2.4 Kraken

| Attribute | Detail |
|---|---|
| **License** | Apache 2.0 |
| **Language** | Python |
| **Languages Supported** | Arbitrary (via trainable models); pre-trained for many historical scripts |
| **HOCR Support** | Yes -- native ALTO and hOCR output |
| **Accuracy** | >95% mean character accuracy on trained scripts; 96.9% on Dead Sea Scrolls |
| **Cross-Platform** | Linux primary; macOS supported; Windows support is limited |
| **Maintenance** | Active in digital humanities community |

**Strengths:**
- Trainable layout analysis and recognition -- excellent for specialized document types.
- Native HOCR and ALTO output.
- Superior on historical, non-Latin, and handwritten text.

**Weaknesses:**
- Designed for academic/humanities use cases, not general-purpose PDF editing.
- Windows support is not first-class.
- Requires model training for best results on new document types.
- Python-only with no C/C++ API.

**Verdict:** **NOT RECOMMENDED for this project.** Excellent technology for specialized use cases but poor fit for a general-purpose cross-platform PDF editor. Windows support gaps and Python-only architecture are disqualifying.

---

### 2.5 OCRmyPDF

| Attribute | Detail |
|---|---|
| **License** | MPL-2.0 |
| **Language** | Python |
| **Current Version** | 16.13.x / 17.x |
| **Role** | OCR orchestration tool (uses Tesseract/PaddleOCR/EasyOCR as backends) |

**Relevance:** OCRmyPDF is not an OCR engine itself but an orchestration layer that demonstrates best practices for:
- PDF page rasterization (Ghostscript or pypdfium2)
- Image preprocessing (deskew, rotation detection)
- OCR engine invocation and HOCR collection
- Text layer embedding back into PDF
- Multi-core parallel processing

**Verdict:** **REFERENCE ARCHITECTURE.** Study OCRmyPDF's pipeline design (plugin system, rasterizer selection, HOCR integration) as a model for the PDF editor's OCR subsystem. The MPL-2.0 license allows studying the code; any reimplementation in C++ for the PDF editor would be original work.

---

### 2.6 OCR Accuracy Summary (Benchmarks)

| Engine | Avg. Confidence | Clean Documents | Complex Layouts | Scene Text | CJK |
|---|---|---|---|---|---|
| Tesseract 5 | 0.89 | Good | Fair | Poor | Good (with packs) |
| PaddleOCR v5 | 0.93 | Excellent | Excellent | Good | Excellent |
| EasyOCR | 0.85 | Fair | Fair | Good | Good |
| Kraken (trained) | >0.95 | Excellent | Good | N/A | Good (trainable) |

---

## 3. Text Search / Indexing Libraries

### 3.1 Tantivy

| Attribute | Detail |
|---|---|
| **License** | MIT |
| **Language** | Rust (with Python bindings via tantivy-py, C FFI possible) |
| **Boolean Support** | Full: AND, OR, NOT, +/- operators |
| **Regex Support** | Yes -- `RegexQuery` and `RegexPhraseQuery` |
| **Phrase Matching** | Yes -- quoted phrases with slop operator (~N) |
| **Stemming** | 17 Latin languages built-in; CJK via third-party tokenizers |
| **Performance** | ~2x faster than Lucene in latency benchmarks |
| **Cross-Platform** | Windows, Linux, macOS (stable Rust) |
| **Maintenance** | Very active; backed by Quickwit; v0.25.x (Dec 2025) |

**Strengths:**
- All required search features are natively supported: boolean queries, regex, quoted phrases, stemming.
- `RegexPhraseQuery` allows multi-term regex matching (e.g., `"b.* b.* wolf"` matches "big bad wolf").
- BM25 relevance scoring out of the box.
- Extremely fast indexing and search -- can index an entire document in milliseconds.
- Memory-efficient via FST (Finite State Transducer) term dictionaries and integer compression.
- MIT license is maximally permissive.
- tantivy-py (v0.25.1, Dec 2025) provides Python bindings if needed for scripting/testing.
- Incremental indexing supports updating the index as OCR results arrive.

**Weaknesses:**
- Rust library; integration into a non-Rust application requires C FFI wrapper or Python bindings.
- No built-in CJK tokenizer (requires third-party `lindera` or `jieba` tokenizer integration).
- Relatively young ecosystem compared to Lucene/Xapian.

**Verdict:** **PRIMARY RECOMMENDATION.** Tantivy provides every required search feature with exceptional performance and a permissive license. The Rust-to-C FFI boundary is well-established (Tantivy provides `tantivy-ffi` examples). For a C++-based PDF editor, a thin C wrapper around Tantivy would provide the search backend.

---

### 3.2 Xapian

| Attribute | Detail |
|---|---|
| **License** | GPL-2.0 (core) |
| **Language** | C++ (with Python, Java, C#, Ruby, Lua, Node.js bindings) |
| **Boolean Support** | Full: AND, OR, NOT, XOR, NEAR, ADJ |
| **Regex Support** | Wildcard only (prefix `xap*`); no full regex |
| **Phrase Matching** | Yes -- quoted phrases, NEAR operator |
| **Stemming** | 24 languages via Snowball stemmers |
| **Performance** | 5,408 searches/second (benchmark vs Whoosh at 26.3/sec) |
| **Cross-Platform** | Windows, Linux, macOS |
| **Maintenance** | Active but slow release cadence |

**Strengths:**
- Written in C++ -- natural fit for a C++ application.
- Mature, battle-tested codebase (used by Debian apt-xapian-index, Omega, Notmuch).
- Broader stemming language support than Tantivy (24 vs 17).
- NEAR and ADJ proximity operators are powerful for document search.

**Weaknesses:**
- GPL-2.0 license is a significant concern for a commercial PDF editor. Any application linking to Xapian must be GPL-compatible or use it as a separate process (complicating architecture).
- No full regex support -- only prefix wildcards.
- Older API design; less modern than Tantivy.
- Slower than Tantivy in head-to-head benchmarks.

**Verdict:** **NOT RECOMMENDED due to GPL-2.0 license.** If the PDF editor is proprietary or uses a permissive license, Xapian's GPL-2.0 creates legal complications. The lack of full regex support is also a gap. Would be a strong choice if license were compatible.

---

### 3.3 SQLite FTS5

| Attribute | Detail |
|---|---|
| **License** | Public Domain (SQLite) |
| **Language** | C (embedded in SQLite) |
| **Boolean Support** | AND, OR, NOT |
| **Regex Support** | No native regex in FTS5 (SQLite has REGEXP but requires extension) |
| **Phrase Matching** | Yes -- quoted phrases |
| **Stemming** | Porter stemmer built-in; additional stemmers via extensions |
| **Performance** | Good for small-medium datasets; degrades on very large corpora |
| **Cross-Platform** | Anywhere SQLite runs (effectively everywhere) |
| **Maintenance** | Extremely active (SQLite is one of the most maintained projects in existence) |

**Strengths:**
- Zero external dependencies -- SQLite is often already embedded in applications.
- Public domain license -- no restrictions whatsoever.
- Simple API -- just SQL queries.
- Excellent for per-document indexing where each PDF gets its own search index.
- Tiny memory footprint.

**Weaknesses:**
- No full regex support in FTS5.
- Limited stemming (Porter only without extensions).
- No BM25-quality relevance scoring (has `rank` function but less sophisticated).
- Performance drops on very large documents (thousands of pages).
- No incremental index updates -- must rebuild.

**Verdict:** **VIABLE FALLBACK.** SQLite FTS5 is an excellent choice for lightweight, per-document search where the PDF editor already uses SQLite for metadata storage. It covers boolean queries and phrase matching but falls short on regex and advanced stemming. Could serve as the default search for simple queries, with Tantivy engaged for advanced search features.

---

### 3.4 Whoosh

| Attribute | Detail |
|---|---|
| **License** | BSD 2-Clause |
| **Language** | Pure Python |
| **Maintenance** | **Effectively abandoned.** Last official release 2019. Community fork at whoosh-reloaded. |

**Verdict:** **NOT RECOMMENDED.** Abandoned project. Pure Python performance is inadequate for a desktop application. No path to C++ integration.

---

### 3.5 Apache Lucene

| Attribute | Detail |
|---|---|
| **License** | Apache 2.0 |
| **Language** | Java |
| **Features** | Industry-standard full-text search; all required features supported |

**Verdict:** **NOT RECOMMENDED for this project.** Requires JVM, which is a heavyweight dependency for a desktop PDF editor. Tantivy provides equivalent or better performance without the JVM requirement. If the application were Java-based, Lucene would be the obvious choice.

---

### 3.6 Text Search Feature Matrix

| Feature | Tantivy | Xapian | SQLite FTS5 | Whoosh | Lucene |
|---|---|---|---|---|---|
| Boolean (AND/OR/NOT) | Yes | Yes | Yes | Yes | Yes |
| Full Regex | Yes | No (wildcard only) | No | Yes | Yes |
| Quoted Phrases | Yes | Yes | Yes | Yes | Yes |
| Stemming | 17 languages | 24 languages | Porter only | Yes | Yes |
| Relevance Scoring | BM25 | BM25 | Basic rank | BM25 | BM25/TF-IDF |
| Cross-Platform | Yes | Yes | Yes | Yes | Yes (JVM) |
| License | MIT | GPL-2.0 | Public Domain | BSD | Apache 2.0 |
| Large Doc Performance | Excellent | Good | Fair | Poor | Excellent |
| C/C++ Integration | Via FFI | Native | Native | None | Via JNI |
| Maintenance | Active | Active | Active | Dead | Active |

---

## 4. Font Matching / Management Libraries

### 4.1 FreeType

| Attribute | Detail |
|---|---|
| **License** | FreeType License (BSD-like) or GPL-2.0 (dual-licensed) |
| **Language** | C |
| **Purpose** | Font file parsing, glyph rasterization |
| **Cross-Platform** | Windows, Linux, macOS, and many others |
| **Maintenance** | Extremely active; foundational library |

**Capabilities for this project:**
- Parse TrueType, OpenType, Type 1, CFF, WOFF, and other font formats.
- Extract glyph outlines, metrics, kerning tables, and PANOSE data from font files.
- Rasterize individual glyphs at any size and resolution.
- Access the `OS/2` table which contains PANOSE classification numbers.

**Verdict:** **ESSENTIAL COMPONENT.** FreeType is the de facto standard for font file operations. It must be part of the stack regardless of other choices. The BSD-like license option is suitable for any project type.

---

### 4.2 HarfBuzz

| Attribute | Detail |
|---|---|
| **License** | MIT (Old MIT license) |
| **Language** | C++ |
| **Purpose** | Text shaping (Unicode text to positioned glyphs) |
| **Cross-Platform** | Windows, Linux, macOS |
| **Maintenance** | Extremely active; used by Chrome, Firefox, Android, iOS |

**Capabilities for this project:**
- Complex text layout for Arabic, Hebrew, Indic, and other bidirectional/complex scripts.
- OpenType feature processing (ligatures, contextual alternates, etc.).
- Integrates directly with FreeType for glyph access.
- Essential for accurate text rendering and glyph positioning in PDF output.

**Verdict:** **ESSENTIAL COMPONENT.** Required for correct text shaping, especially for CJK and complex scripts. Without HarfBuzz, text rendering would be incorrect for a significant portion of the world's writing systems.

---

### 4.3 Fontconfig

| Attribute | Detail |
|---|---|
| **License** | MIT-like (fontconfig license) |
| **Language** | C |
| **Purpose** | Font discovery, configuration, and pattern-based matching |
| **Cross-Platform** | Linux/Unix native; Windows support via community forks but not first-class |
| **Maintenance** | Active on freedesktop.org |

**Capabilities for this project:**
- Enumerate all fonts installed on the system.
- Match font requests (family, style, weight) to available fonts.
- Pattern-based matching with configurable substitution rules.
- Reads font metadata including PANOSE data.

**Weaknesses:**
- Windows support is not official -- requires community fork patches.
- Configuration-file-driven matching is powerful but complex.
- No visual similarity matching beyond basic attribute matching.

**Verdict:** **USE ON LINUX/macOS ONLY.** Fontconfig is the standard font discovery mechanism on Linux and can be used on macOS. For Windows, the application should use the native DirectWrite API for font enumeration, or use font-kit as a cross-platform abstraction.

---

### 4.4 font-kit (Rust)

| Attribute | Detail |
|---|---|
| **License** | Apache 2.0 / MIT (dual-licensed) |
| **Language** | Rust |
| **Purpose** | Cross-platform font discovery, matching, and loading |
| **Cross-Platform** | Windows (DirectWrite), Linux (Fontconfig), macOS (Core Text) |
| **Maintenance** | Active; maintained by Servo project |

**Capabilities for this project:**
- Unified API across all three platforms for font discovery.
- CSS Fonts Level 3 matching algorithm.
- Uses native platform font APIs under the hood (DirectWrite, Core Text, Fontconfig).
- Font loading from files, memory, or system databases.

**Verdict:** **RECOMMENDED for cross-platform font discovery.** Provides the abstraction layer needed to find fonts on all three platforms through a single API. Rust-to-C FFI is straightforward. The CSS matching algorithm is a good baseline for "find the best available font matching these attributes."

---

### 4.5 PANOSE-Based Visual Similarity Matching

PANOSE is not a library but a classification system embedded in font metadata (the OS/2 table in OpenType fonts). A custom matching algorithm should be built on top of FreeType's ability to read PANOSE data.

**Recommended approach:**

1. **Extract PANOSE numbers** from all available fonts using FreeType's `TT_OS2` table access.
2. **Build a PANOSE distance function** that computes weighted visual distance between two fonts based on the 10 PANOSE digits (weight, serif style, proportion, contrast, stroke variation, arm style, letterform, midline, x-height, etc.).
3. **Create a font similarity index** at application startup that maps each font's PANOSE classification to enable nearest-neighbor lookups.
4. **Supplement with metric matching** using font metrics (x-height, cap-height, ascender, descender ratios) for cases where PANOSE data is missing or insufficient.

The `panosifier` open-source tool (Python, by source-foundry) provides reference code for reading and writing PANOSE data. The W3C PANOSE 2.0 specification provides the distance calculation algorithm.

**Verdict:** **CUSTOM IMPLEMENTATION REQUIRED.** No off-the-shelf library provides production-ready visual font similarity matching. The recommended approach is to build a custom matcher using FreeType for data extraction and the PANOSE distance algorithm for similarity scoring.

---

### 4.6 Font Creation from Available Glyphs

For the requirement to create fonts from available glyphs when no match is found:

| Library | Purpose | License |
|---|---|---|
| **FontForge** (libfontforge) | Programmatic font creation and editing | GPL-3.0 |
| **fonttools** (Python) | Font file manipulation, subsetting, merging | MIT |
| **HarfBuzz subset** | Font subsetting (extracting used glyphs) | MIT |

**Recommended approach:**

1. Use **fonttools** (Python, MIT license) for runtime font generation. It can create new font files by:
   - Extracting glyphs from multiple source fonts.
   - Building a new font with a custom glyph set.
   - Setting proper metrics and encoding tables.
2. Use **HarfBuzz's subsetting API** (`hb-subset`) for efficiently extracting only the needed glyphs from large fonts (critical for CJK fonts that can be 15MB+).
3. The generated font would be embedded in the PDF as a subset, following PDF specification requirements.

**Note:** FontForge is GPL-3.0 and should be avoided for direct linking in a non-GPL application. fonttools (MIT) is the preferred alternative for programmatic font manipulation.

---

### 4.7 Font Library Feature Matrix

| Feature | FreeType | HarfBuzz | Fontconfig | font-kit |
|---|---|---|---|---|
| Font File Parsing | Yes | No | No | Yes (via backends) |
| Glyph Rasterization | Yes | No | No | Yes (via FreeType) |
| Text Shaping | No | Yes | No | No |
| Font Discovery | No | No | Yes (Linux) | Yes (all platforms) |
| Font Matching | No | No | Yes (pattern-based) | Yes (CSS L3 algo) |
| PANOSE Access | Yes (OS/2 table) | Yes (via font access) | Yes (reads from fonts) | Limited |
| Cross-Platform | Yes | Yes | Linux/macOS | Yes |
| License | BSD/GPL dual | MIT | MIT-like | Apache 2.0/MIT |

---

## 5. Open-Source Font Collections

### 5.1 Google Noto Fonts

| Attribute | Detail |
|---|---|
| **License** | SIL Open Font License 1.1 |
| **Families** | 1,000+ covering 150+ writing systems |
| **CJK Coverage** | 43,027+ characters; Simplified Chinese, Traditional Chinese, Japanese, Korean |
| **CJK Weights** | 7 weights per language variant (ExtraLight through Black) |
| **CJK Size** | ~1.5GB for full CJK package (Sans + Serif, all regions, all weights) |
| **Non-CJK Size** | ~500MB for all non-CJK scripts |
| **Maintenance** | Active; updated through Dec 2025 |

**Strengths:**
- Goal of "no tofu" (no missing glyphs) for any Unicode character.
- Consistent visual design across all scripts.
- Most comprehensive open-source font collection in existence.
- CJK fonts handle region-specific glyph variants correctly (JP vs CN vs KR vs TW).
- Variable font versions available for some families, reducing file size.

**Weaknesses:**
- Total collection is very large (~2GB+ with CJK).
- CJK fonts alone are ~1.5GB, which is substantial for a desktop application download.

**Verdict:** **PRIMARY RECOMMENDATION.** Noto fonts should be the default fallback font family. For the PDF editor:
- **Bundle Noto Sans + Noto Serif (Latin/Cyrillic/Greek)** with the application (~50MB).
- **Offer CJK fonts as optional downloads** per-language (~200-400MB each for Sans+Serif per region).
- **Offer other script packs as optional downloads** grouped by region.

---

### 5.2 Liberation Fonts

| Attribute | Detail |
|---|---|
| **License** | SIL Open Font License 1.1 (v2.0+) |
| **Families** | Liberation Sans, Serif, Mono |
| **Purpose** | Metric-compatible replacements for Arial, Times New Roman, Courier New |
| **Coverage** | 666 glyphs; Latin, Greek, Cyrillic |
| **Size** | ~3MB total |

**Strengths:**
- Metrically compatible with the most commonly used Microsoft fonts (Arial, Times New Roman, Courier New).
- Documents created with Microsoft fonts will render with identical line breaks and page layouts.
- Tiny footprint.

**Weaknesses:**
- Very limited Unicode coverage (666 glyphs vs DejaVu's 3,289).
- Only three families -- no decorative, display, or handwriting options.
- No CJK support.

**Verdict:** **BUNDLE AS METRIC-COMPATIBLE FALLBACKS.** Liberation fonts should be included specifically for metric-compatible rendering of documents that reference Arial, Times New Roman, or Courier New. They are essential for maintaining document fidelity.

---

### 5.3 DejaVu Fonts

| Attribute | Detail |
|---|---|
| **License** | Bitstream Vera license (MIT-like) + public domain changes |
| **Families** | DejaVu Sans, Serif, Sans Mono (with condensed variants) |
| **Coverage** | 3,289 glyphs; Latin, Greek, Cyrillic, Arabic, Hebrew, Georgian, and more |
| **Size** | ~10MB total |
| **MES Coverage** | MES-1, MES-2, partial MES-3 |

**Strengths:**
- Much broader Unicode coverage than Liberation.
- Familiar, readable design based on Bitstream Vera.
- Good coverage of mathematical symbols and box-drawing characters.
- Permissive license.

**Weaknesses:**
- No CJK support.
- Not metric-compatible with any Microsoft fonts.
- Development has slowed in recent years.

**Verdict:** **BUNDLE AS GENERAL-PURPOSE FALLBACK.** DejaVu provides broader Unicode coverage than Liberation for scripts not covered by the Noto core bundle. Useful as a secondary fallback before reaching for Noto's script-specific fonts.

---

### 5.4 Google Fonts Collection (beyond Noto)

| Attribute | Detail |
|---|---|
| **License** | Various open-source (mostly SIL OFL and Apache 2.0) |
| **Families** | ~1,900 font families (as of Jan 2026) |
| **Styles** | 525 variable font families |
| **CJK** | Noto CJK variants + additional CJK families |

**Relevance:** The full Google Fonts collection is too large to bundle but represents the largest pool of freely-licensed fonts for the font matching system to search. The PDF editor could:
- Index all Google Fonts by PANOSE classification and visual metrics.
- Offer a "download matching font" feature that fetches from Google Fonts API.
- Pre-compute PANOSE similarity tables for offline font matching.

**Verdict:** **USE AS FONT MATCHING DATABASE.** Pre-compute PANOSE data and visual metrics for the entire Google Fonts collection. Store as a lightweight lookup table (~5MB) that the font matcher uses to suggest or download matching fonts.

---

### 5.5 Font Collection Summary

| Collection | Bundle Strategy | Size Impact | License |
|---|---|---|---|
| **Noto (core Latin/Cyrillic/Greek)** | Bundle with app | ~50MB | SIL OFL 1.1 |
| **Noto CJK** | Optional download per region | ~200-400MB each | SIL OFL 1.1 |
| **Noto (other scripts)** | Optional download packs | Varies | SIL OFL 1.1 |
| **Liberation** | Bundle with app | ~3MB | SIL OFL 1.1 |
| **DejaVu** | Bundle with app | ~10MB | Bitstream Vera + PD |
| **Google Fonts (metadata)** | PANOSE lookup table only | ~5MB | N/A (metadata only) |
| **Total bundled** | -- | **~68MB** | -- |

---

## 6. Recommended Architecture

### 6.1 OCR Pipeline

```
PDF Page Image
      |
      v
[Image Preprocessing]
  - Deskew (Leptonica)
  - Rotation detection
  - Noise removal
  - Resolution normalization
      |
      v
[OCR Engine Selection]
  - Default: Tesseract 5.x
  - Optional: PaddleOCR (user-selectable or auto-fallback)
      |
      v
[HOCR Generation]
  - Tesseract: native GetHOCRText()
  - PaddleOCR: custom JSON-to-HOCR converter
      |
      v
[HOCR Post-Processing]
  - Merge character boxes into words/lines
  - Confidence thresholding
  - Language detection verification
      |
      v
[Text Layer Embedding]
  - Invisible text layer in PDF
  - Preserves original page appearance
  - Searchable and selectable text
      |
      v
[Search Index Update]
  - Feed OCR'd text to Tantivy index
```

### 6.2 Search Architecture

```
[User Query]
      |
      v
[Query Parser]
  - Boolean operators (AND, OR, NOT)
  - Quoted phrase detection
  - Regex pattern detection
  - Stemming expansion
      |
      v
[Tantivy Index]
  - Per-document index
  - Fields: page_number, text_content, text_type (native/ocr)
  - Tokenizer: language-aware with stemming
      |
      v
[Result Mapping]
  - Map Tantivy results to PDF page/position
  - Use HOCR bounding boxes for highlight coordinates
  - Return ranked results with snippets
```

### 6.3 Font Matching Architecture

```
[Missing Font Request]
  - Family name, weight, style from PDF metadata
      |
      v
[Stage 1: Exact Name Match]
  - font-kit system font query
  - Check bundled fonts (Noto, Liberation, DejaVu)
      |
      v (no match)
[Stage 2: Metric-Compatible Match]
  - Liberation for Arial/Times/Courier
  - Known substitution table
      |
      v (no match)
[Stage 3: PANOSE Visual Similarity]
  - Extract PANOSE from PDF's embedded font descriptor
  - Compare against PANOSE index of available fonts
  - Return closest visual match
      |
      v (no match or poor match)
[Stage 4: Google Fonts Lookup]
  - Search pre-computed PANOSE table of ~1,900 families
  - Offer download of best match
      |
      v (no downloadable match)
[Stage 5: Glyph-Level Font Construction]
  - Identify needed glyphs from document
  - Extract from best available fonts using fonttools
  - Construct synthetic font with correct metrics
  - Embed as subset in PDF
```

---

## 7. Integration Challenges

### 7.1 Tesseract C++ Integration

**Challenge:** Tesseract requires Leptonica for image I/O and has its own memory management patterns.

**Mitigation:**
- Use `TessBaseAPI` RAII wrappers to manage lifecycle.
- Pre-process images to Leptonica `PIX` format before passing to Tesseract.
- Use the Renderer API for document-level HOCR generation.
- Tesseract traineddata files must be distributed with the application (~15MB per language for `tessdata_fast`, ~30MB for `tessdata_best`).

### 7.2 Tantivy-to-C++ Bridge

**Challenge:** Tantivy is a Rust library; the PDF editor is likely C++ or C-based.

**Mitigation:**
- Create a thin C API wrapper (`extern "C"` functions) around Tantivy's core operations: create index, add document, search, delete.
- Build as a shared library (`.dll`/`.so`/`.dylib`) that the C++ application loads.
- Alternative: Use tantivy-py if parts of the application use Python scripting.
- The Rust-C FFI is well-established; Tantivy's own `tantivy-ffi` examples demonstrate the pattern.
- Expected wrapper surface: ~20 functions covering index CRUD, query building, and result iteration.

### 7.3 PaddleOCR HOCR Conversion

**Challenge:** PaddleOCR outputs JSON with bounding box coordinates but not HOCR XML.

**Mitigation:**
- Write a converter that maps PaddleOCR's JSON output format:
  ```json
  {"text": "Hello", "confidence": 0.95, "box": [[x1,y1],[x2,y2],[x3,y3],[x4,y4]]}
  ```
  to HOCR XML:
  ```xml
  <span class="ocrx_word" title="bbox x1 y1 x3 y3; x_wconf 95">Hello</span>
  ```
- Handle coordinate system differences (PaddleOCR uses quadrilateral boxes; HOCR uses axis-aligned bounding boxes).
- This is a moderate engineering effort but well-defined.

### 7.4 Cross-Platform Font Discovery

**Challenge:** Each platform has a different font discovery mechanism (DirectWrite, Core Text, Fontconfig).

**Mitigation:**
- Use font-kit as the abstraction layer; it already handles all three backends.
- Build the PANOSE similarity index at application startup by scanning discovered fonts.
- Cache the PANOSE index to disk to avoid re-scanning on every launch.
- Typical system font scan takes 1-3 seconds on modern hardware.

### 7.5 CJK Font Size Management

**Challenge:** CJK fonts are 200-400MB per language variant, too large to bundle.

**Mitigation:**
- Implement a font download manager within the application.
- Offer CJK font packs as optional downloads (Noto Sans CJK JP, Noto Sans CJK SC, etc.).
- Use HarfBuzz subsetting to extract only needed glyphs for individual documents.
- Store downloaded fonts in a user-specific font cache directory.
- Display clear UI indicating CJK support status and offering one-click download.

### 7.6 Search Index Size and Performance

**Challenge:** Large PDFs (1000+ pages) may produce substantial search indices.

**Mitigation:**
- Tantivy's compressed index format is typically 20-30% of raw text size.
- A 1000-page PDF with ~500 words/page produces ~2.5MB of raw text, resulting in a ~500KB-750KB index.
- Index creation is fast (~50ms for this volume on modern hardware).
- Store indices alongside the PDF file or in a central cache.
- Implement lazy indexing: index on first search, not on file open.

---

## 8. Sources

### OCR Libraries
- [Tesseract OCR GitHub Repository](https://github.com/tesseract-ocr/tesseract)
- [Tesseract C++ API Examples](https://tesseract-ocr.github.io/tessdoc/Examples_C++.html)
- [Tesseract API Documentation](https://tesseract-ocr.github.io/tessdoc/APIExample.html)
- [Tesseract Release Notes](https://tesseract-ocr.github.io/tessdoc/ReleaseNotes.html)
- [PaddleOCR GitHub Repository](https://github.com/PaddlePaddle/PaddleOCR)
- [PaddleOCR vs Tesseract Comparison (Koncile)](https://www.koncile.ai/en/ressources/paddleocr-analyse-avantages-alternatives-open-source)
- [OCR Comparison: Tesseract vs EasyOCR vs PaddleOCR vs MMOCR](https://toon-beerten.medium.com/ocr-comparison-tesseract-versus-easyocr-vs-paddleocr-vs-mmocr-a362d9c79e66)
- [Technical Analysis of Modern Non-LLM OCR Engines (IntuitionLabs)](https://intuitionlabs.ai/articles/non-llm-ocr-technologies)
- [7 Best Open-Source OCR Models 2025 (E2E Networks)](https://www.e2enetworks.com/blog/complete-guide-open-source-ocr-models-2025)
- [OCR Benchmark: Text Extraction Accuracy 2026 (AIMultiple)](https://research.aimultiple.com/ocr-accuracy/)
- [EasyOCR GitHub Repository](https://github.com/JaidedAI/EasyOCR)
- [Kraken OCR GitHub Repository](https://github.com/mittagessen/kraken)
- [OCRmyPDF GitHub Repository](https://github.com/ocrmypdf/OCRmyPDF)
- [OCRmyPDF Documentation](https://ocrmypdf.readthedocs.io/)
- [hOCR Wikipedia](https://en.wikipedia.org/wiki/HOCR)

### Text Search Libraries
- [Tantivy GitHub Repository](https://github.com/quickwit-oss/tantivy)
- [Tantivy API Documentation](https://docs.rs/tantivy/)
- [Tantivy Query Types](https://docs.rs/tantivy/latest/tantivy/query/index.html)
- [Tantivy RegexQuery Documentation](https://docs.rs/tantivy/latest/tantivy/query/struct.RegexQuery.html)
- [Tantivy-py Python Bindings](https://github.com/quickwit-oss/tantivy-py)
- [Introduction to Tantivy (ParadeDB)](https://www.paradedb.com/learn/tantivy/introduction)
- [Xapian Project Features](https://xapian.org/features)
- [Xapian QueryParser Reference](https://xapian.org/docs/queryparser.html)
- [SQLite FTS5 Extension](https://www.sqlite.org/fts5.html)
- [Whoosh GitHub (Community Fork)](https://github.com/whoosh-community/whoosh)

### Font Libraries
- [FreeType Project](https://freetype.org/)
- [HarfBuzz Manual](https://harfbuzz.github.io/what-does-harfbuzz-do.html)
- [HarfBuzz FreeType Integration](https://harfbuzz.github.io/integration-freetype.html)
- [font-kit GitHub Repository](https://github.com/servo/font-kit)
- [font-kit API Documentation](https://docs.rs/font-kit)
- [Fontconfig Documentation](https://www.freedesktop.org/software/fontconfig/fontconfig-user.html)
- [PANOSE Classification System (W3C)](https://www.w3.org/Printing/stevahn.html)
- [PANOSE 2.0 White Paper (W3C)](https://www.w3.org/Fonts/Panose/pan2.html)
- [PANOSE Metrics Guide (Monotype)](https://monotype.github.io/panose/pan1.htm)
- [Panosifier GitHub Repository](https://github.com/source-foundry/panosifier)

### Font Collections
- [Noto CJK Fonts GitHub Repository](https://github.com/notofonts/noto-cjk)
- [Noto Fonts Home (Google Fonts)](https://fonts.google.com/noto)
- [Noto Fonts Wikipedia](https://en.wikipedia.org/wiki/Noto_fonts)
- [Liberation Fonts Wikipedia](https://en.wikipedia.org/wiki/Liberation_fonts)
- [DejaVu Fonts](https://dejavu-fonts.github.io/)
- [DejaVu Fonts Wikipedia](https://en.wikipedia.org/wiki/DejaVu_fonts)
- [Google Fonts](https://fonts.google.com/)
- [Google Fonts Wikipedia](https://en.wikipedia.org/wiki/Google_Fonts)
