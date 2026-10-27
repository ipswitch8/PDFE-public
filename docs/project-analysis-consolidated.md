# PDFE - Cross-Platform PDF Editor: Technical Analysis & Library Recommendations

**Date:** 2026-02-05
**Status:** Revised Technical Analysis (v2 -- AGPL licensing, MuPDF core, expanded font/OCR editing)

---

## Executive Summary

Building an Adobe Acrobat Pro-class PDF editor is a major engineering undertaking. With PDFE licensed under AGPL-3.0, **MuPDF** becomes the primary library -- replacing PDFium and absorbing rendering, annotations, forms, redaction, JavaScript, and content stream editing into a single coherent API. **QPDF** retains a narrowed but critical role for incremental saves (digital signature preservation), linearization, and low-level object graph manipulation. The foundation is **C++20 / Qt 6.8 LTS** with specialized libraries for OCR, fonts, search, encryption, and image processing.

This revised architecture is substantially simpler than the previous PDFium + QPDF dual-model design. The previous architecture required synchronizing two separate document representations; now **MuPDF owns the document model** and QPDF is invoked only for specific structural operations. MuPDF's built-in redaction API (`pdf_redact_page()`), annotation CRUD with automatic appearance stream generation, form widget creation, and integrated JavaScript engine (MuJS) eliminate several months of custom development work that the PDFium architecture would have required.

Two new subsystems are added in this revision: a **Text Reconstruction Engine** for editing text on scanned/image-only PDFs, and a **Metric-Compatible Font Substitution** system for handling custom embedded fonts with missing glyphs. Both are detailed in dedicated architecture documents.

The biggest technical risks remain text reflow (PDF text is glyph-by-glyph, not semantic) and OCG/layer editing (poorly supported even in MuPDF). Real redaction drops from "high risk" to "medium" thanks to MuPDF's built-in support, and the dual-model synchronization risk is eliminated entirely.

---

## Technology Stack Overview

| Layer | Technology | License | Role |
|-------|-----------|---------|------|
| **Project License** | **AGPL-3.0** | -- | Prevents proprietary forks, enables copyleft library use |
| **Language** | C++20 | -- | Primary language, zero FFI overhead with PDF libs |
| **GUI Framework** | Qt 6.8 LTS | LGPL 3.0 | Cross-platform UI with GPU canvas |
| **PDF Core (rendering, editing, annotations, forms, redaction)** | **MuPDF 1.25+** | **AGPL-3.0** | Primary PDF library -- rendering, content editing, annotations, forms, JS, redaction |
| **PDF Structure (incremental saves, linearization, encryption)** | QPDF 12.x | Apache 2.0 | Incremental saves, linearization, signature byte-range, object-level manipulation |
| **Form JavaScript** | **MuJS** (bundled with MuPDF) | ISC | PDF form JS with partial Acrobat API -- replaces QuickJS |
| **Font Rendering** | FreeType 2 + HarfBuzz | BSD-like + MIT | Glyph rasterization + text shaping |
| **Font Discovery** | Fontconfig (Linux/macOS) / DirectWrite (Windows) | MIT / Proprietary | System font enumeration and matching |
| **Font Substitution** | **Custom Metric-Compatible Substitution Engine** | -- | Glyph-level analysis, substitute scoring, dual-font embedding |
| **OCR** | Tesseract 5.x | Apache 2.0 | Text recognition with HOCR output |
| **Scanned PDF Editing** | **Custom Text Reconstruction Engine** | -- | OCR-to-editable pipeline, image masking, region reconstruction |
| **Text Search** | Tantivy (via C FFI) or ICU + Custom Index | MIT / Unicode License | Boolean/regex/stemming search |
| **Encryption/Signatures** | OpenSSL 3.x | Apache 2.0 | AES-256, PKCS#7/CMS signatures |
| **Image Processing** | libvips + libjpeg-turbo + libpng + OpenJPEG | LGPL-2.1 + BSD + libpng + BSD-2 | Image decode/encode/manipulation |
| **Metadata** | ExifTool (subprocess) or libexif | Artistic / LGPL-2.1 | EXIF/XMP stripping |
| **Build System** | CMake 3.25+ | BSD-3 | Cross-platform builds |
| **Package Manager** | vcpkg | MIT | C++ dependency management |
| **Stemming** | Snowball | BSD | Multi-language stemming for search |
| **Hyphenation** | libhyphen | LGPL | Text reflow hyphenation |

### Changes from v1

| What Changed | v1 (Permissive) | v2 (AGPL) | Impact |
|---|---|---|---|
| PDF rendering & editing | PDFium (BSD) | **MuPDF (AGPL)** | Better rendering, built-in editing/annotations/redaction/forms |
| Form JavaScript | QuickJS (MIT) | **MuJS** (ISC, bundled with MuPDF) | Eliminated a dependency; partial Acrobat JS API already implemented |
| Document model ownership | QPDF (canonical) + PDFium (render-only) | **MuPDF (canonical)** + QPDF (structural ops only) | Eliminated dual-model synchronization problem |
| Redaction | Custom content stream parser | **MuPDF `pdf_redact_page()`** + verification | ~2 months of custom dev eliminated |
| Annotation appearance streams | Custom generation required | **MuPDF auto-generates** | ~1 month of custom dev eliminated |
| New: Scanned PDF editing | Not addressed | **Text Reconstruction Engine** | Approach B (hybrid region) + Approach C (image replacement) |
| New: Font glyph substitution | Basic pipeline only | **Metric-Compatible Substitution Engine** | Measured-metric scoring, dual-font embedding, user choice UI |
| Build complexity | PDFium GN/Ninja (30-60 min) | **MuPDF Makefile (2-5 min)** | Dramatically simpler builds |
| Estimated dev time saved | -- | **~5-9 months** | From MuPDF built-ins replacing custom code |

---

## Functional Categories & Library Recommendations

### 1. PDF Rendering & Core Editing

**Recommended: MuPDF 1.25+**
- **License:** AGPL-3.0 (compatible with PDFE's AGPL license)
- **Capabilities:**
  - Highest-quality open-source PDF rendering (equal or marginally better than PDFium)
  - Full annotation CRUD with automatic appearance stream generation
  - Form widget creation and interaction (AcroForm)
  - Built-in redaction via `pdf_redact_page()` -- actually removes content, not just overlays
  - Content stream editing via Story/DOM API
  - Structured text extraction with position information
  - Integrated JavaScript engine (MuJS) with partial Acrobat JS API
  - Encryption/decryption read support
  - Multi-format support (also handles EPUB, XPS, CBZ)
  - Clean C API, builds from source in 2-5 minutes with standard Makefile
- **Rationale:** With AGPL licensing, MuPDF is the clear winner. It replaces PDFium for rendering and absorbs substantial custom development for annotations, forms, redaction, and JS.
- **Limitations:** No XFA dynamic form rendering (but XFA was deprecated in PDF 2.0 and static XFA with AcroForm duplicates still works). Incremental save support is less robust than QPDF for digital signature preservation. No linearization support (dropped the feature).

**Why not PDFium?** PDFium's rendering is roughly equivalent to MuPDF, but PDFium has no content stream editing, no built-in redaction, requires custom annotation appearance stream generation, and has no integrated JS engine. Under AGPL licensing, MuPDF is strictly superior.

### 2. PDF Structural Operations

**Recommended: QPDF 12.x (narrowed role)**
- **License:** Apache 2.0
- **Retained responsibilities:**
  - **Incremental saves** -- critical for preserving digital signatures. MuPDF's incremental save is less reliable for this specific use case. QPDF's precise object-level control is essential for signature byte-range management.
  - **Linearization** -- MuPDF dropped this feature entirely. QPDF handles web-optimized PDF output.
  - **Content-preserving transformations** -- metadata sanitization, object renumbering after redaction verification.
  - **PDF repair and forensics** -- JSON inspection, QDF output for debugging.
  - **Page manipulation** -- QPDF's page tree manipulation is extremely robust for merge/split/reorder operations, though MuPDF can also handle basic page operations.
- **Architectural relationship:** Previously QPDF was the document model owner. Now **MuPDF owns the document model** and QPDF is invoked for specific structural operations via a unidirectional flow. This eliminates the previous dual-model synchronization risk.

### 3. OCR, Text Embedding & Scanned PDF Editing

**OCR Engine: Tesseract 5.x**
- **License:** Apache 2.0
- **Capabilities:** LSTM neural network OCR, native HOCR output, 100+ languages, CJK as optional downloads.
- **Pipeline:**
  1. Render page to bitmap at 300 DPI via MuPDF
  2. Pass bitmap to Tesseract via C API
  3. Parse HOCR output for text + positions + confidence + font size estimates
  4. Create invisible text layer in PDF (text rendering mode 3) positioned over recognized text
  5. Write modified page via MuPDF's content editing API

**NEW: Text Reconstruction Engine (TRE)**

Handles the case where a scanned/image-only PDF needs actual text *editing*, not just search.

**Primary approach (Hybrid Region Reconstruction):**
1. OCR the page, embed invisible text layer for search
2. When user wants to edit text in a specific region, *only that region* is reconstructed as real text using OCR results + font matching
3. The surrounding page remains as the original scanned image
4. The edited text block replaces the corresponding area: the image is masked (background color fill + feathered edges), and real text objects are placed on top
5. Multiple edited regions per page are supported, each independently reconstructed

**Fallback approach (Image-Level Text Replacement):**
- Triggered automatically when OCR confidence is below threshold (0.65), background is complex/textured, or font matching fails
- Replacement text is rendered to bitmap using matched font, composited over the original scanned image
- Result is still an image, but visual content is updated

**Key TRE components:**
- `TextReconstructionEngine` -- facade for the subsystem
- `RegionReconstructor` -- orchestrates Approach B (hybrid region editing)
- `ImageReplacer` -- orchestrates Approach C (image-level replacement)
- `CoordMapper` -- maps Tesseract pixel coordinates to PDF user space (handles `/Rotate`, `/CropBox`, image scaling via `cm` operator)
- `BgColorDetector` -- determines background color behind text for clean masking (edge sampling, inter-line gap analysis, gradient detection)
- `QualityAssessor` -- weighted multi-signal score (OCR confidence 0.30, background uniformity 0.20, font match quality 0.15, etc.) with 0.65 threshold for Approach B vs C fallback
- `ImageMasker` -- feathered-edge image masking via gaussian-blurred alpha compositing (libvips)
- `PdfContentWriter` -- generates PDF content streams with mixed image + text objects

Full architecture: `docs/text-reconstruction-engine-architecture.md`

### 4. Text Search

**Recommended: Tantivy (via C FFI bridge)**
- **License:** MIT
- **Capabilities:** Boolean operators (AND/OR/NOT), full regex, quoted phrase matching with slop, stemming for 17 Latin languages. ~2x faster than Lucene. FST-based term dictionaries.
- **Integration:** ~20 C FFI wrapper functions for index CRUD, query building, result iteration.

**Alternative: ICU + custom inverted index** if Rust FFI is unacceptable. Xapian (GPL-2.0) is now license-compatible under AGPL but Tantivy remains technically superior (full regex, better performance).

### 5. Font System

**Core stack: FreeType 2 + HarfBuzz + Fontconfig/DirectWrite**

| Component | License | Role |
|-----------|---------|------|
| FreeType 2 | BSD-like (FTL) | Glyph rasterization, font file parsing, metric extraction |
| HarfBuzz | MIT | Text shaping (ligatures, kerning, complex scripts) |
| Fontconfig | MIT | System font enumeration (Linux/macOS) |
| DirectWrite | Windows API | System font enumeration (Windows) |

**Font matching pipeline (for missing fonts):**
1. Check embedded fonts in PDF
2. Match by PostScript name against system fonts
3. Match by family + style (with alias table: "Helvetica" → "Arial", etc.)
4. **Measured-metric matching** (x-height, cap height, stem weight, contrast, serif detection) -- more reliable than PANOSE metadata, which is often missing or incorrect
5. Unicode coverage analysis
6. Fallback to bundled DejaVu fonts

**NEW: Metric-Compatible Font Substitution Engine**

Handles the case where a PDF embeds a custom font with only a partial glyph subset, and the user needs to add text requiring glyphs not in that subset.

**Pipeline:**

**Step 1 -- Find the full font:**
Search system fonts by PostScript name, family name, and embedded metadata. If found, extract needed glyphs and extend the embedded subset in the PDF using TrueType/CFF table manipulation via FreeType.

**Step 2 -- Analyze embedded font (when full font unavailable):**
Direct geometric analysis of available glyph outlines, not PANOSE metadata:
- Stem weight via ray-casting across vertical strokes
- x-height, cap height, ascender, descender from glyph bounding boxes
- Serif detection via horizontal extent ratios at baseline vs. stem midpoint
- Contrast ratio (thick-to-thin stroke variation)
- Roundness and counter openness from curve analysis
- Even a handful of glyphs (the letter "I" alone gives stem weight + serif style) yields highly discriminating metrics

**Step 3a -- Metric-compatible substitution (primary):**
Score candidate fonts using weighted distance across 9 measured metrics:
- Vertical stem weight: 25% (most visually impactful)
- x-to-cap ratio: 20%
- Measured x-height: 15%
- Cap height: 10%, Width-to-height: 10%
- Contrast ratio: 8%
- Roundness: 4%, Counter openness: 3%
- Hard disqualifiers: serif/sans-serif mismatch, italic/upright mismatch

For missing glyphs only, use the best-scoring substitute font, scaled and adjusted to match the embedded font's metrics. Embed as a **secondary PDF font object** with automatic `Tf` operator switching in content streams:

```
BT
  /F1 12 Tf              % embedded font (original glyphs)
  100 700 Td
  (H) Tj
  /F2 11.76 Tf           % substitute font (scaled to match)
  (e) Tj
  /F1 12 Tf
  (llo Wo) Tj
  /F2 11.76 Tf
  (r) Tj
  /F1 12 Tf
  (ld!) Tj
ET
```

Consecutive same-font characters are batched into single `Tj` operators.

**Step 3c -- User choice (UX layer):**
- Show available glyphs from embedded font
- Show top substitute matches with visual comparison (rendered sample text)
- Let user choose substitute or provide their own font file
- Option to flag text as "edited with substitute glyphs"

**Step 3b -- Glyph synthesis (future enhancement):**
Style transfer at the glyph level -- analyze design characteristics from available outlines, apply to modify substitute glyphs. Deferred to post-1.0 as an ML/algorithmic R&D effort.

**Key design decisions:**
- **Dual-font embedding over glyph merging** -- preserves original font program untouched (critical for signed documents), simplifies undo/redo, avoids fragile TrueType/CFF table merging
- **Commit-time substitution, not per-keystroke** -- user types into Qt overlay with preview font; full subsetting + PDF embedding runs only on commit
- **SQLite-backed analysis cache** -- font metric analysis is expensive; results are cached persistently across sessions

Full architecture: `docs/font-substitution-architecture.md`

**Bundled font collection (~68MB):**
- Noto Sans/Serif (Latin/Cyrillic/Greek): ~50MB
- Liberation Sans/Serif/Mono: ~3MB (metric-compatible with Arial/Times/Courier)
- DejaVu Sans/Serif/Mono: ~10MB (broad Unicode coverage)
- PANOSE index database: ~5MB
- CJK fonts: optional per-language downloads (~200-400MB each)

### 6. Forms & Automation

**Rendering/filling/creation:** MuPDF (form widget API with annotation-level access)
**JavaScript execution:** MuJS (bundled with MuPDF, ISC license)
- ES5.1 compliant, already implements a subset of the Acrobat JavaScript API
- Integrated with MuPDF's document model -- no custom bridge needed
- PDF form JavaScript is almost exclusively ES5, so QuickJS's ES2023 support was unnecessary overhead
- MuJS is sandboxed by default (no filesystem/network access)

**Form automation (auto-date, basic math):**
- Custom FormAutomation engine: `SUM`, `PRODUCT`, `AVERAGE`, `MIN`, `MAX`, simple expressions (`field1 + field2 * 0.1`)
- Topological sort for calculation chain dependencies
- Auto-date fields with configurable format strings

### 7. Security & Encryption

| Feature | Library | License | Standard |
|---------|---------|---------|----------|
| Password protection | QPDF + OpenSSL 3.x | Apache 2.0 + Apache 2.0 | AES-256, AES-128, RC4 (legacy) |
| Digital signatures | OpenSSL 3.x (+ BouncyCastle C++ port or C subprocess for PAdES) | Apache 2.0 (+ MIT) | PKCS#7, CMS, PAdES B-B/B-T |
| Permissions (print/copy prevention) | QPDF | Apache 2.0 | PDF permission flags |
| Certificate handling | OpenSSL 3.x | Apache 2.0 | X.509, PKCS#12 |

**Real redaction (simplified by MuPDF):**
1. Mark regions for redaction (visual overlay in MuPDF annotation layer)
2. On "Apply Redactions": call MuPDF's `pdf_redact_page()` which handles content stream parsing and removal
3. **Verification pass** (custom): search entire object graph for remnants of redacted content -- MuPDF's built-in redaction is good but verification is still our responsibility
4. Strip metadata that might reference redacted content
5. Full rewrite via QPDF to eliminate any trace in file structure

**iText (AGPL) was evaluated and rejected.** The integration cost (Java subprocess, 80-150MB runtime, IPC overhead) exceeds the benefit. PAdES B-B through B-LT is achievable with OpenSSL 3.x for core crypto (RSA/ECDSA, AES, hashing), QPDF for signature byte-range management and incremental saves, and either BouncyCastle's C# edition via a thin .NET subprocess for CMS/PAdES container construction, or a pure-C++ CMS implementation built on OpenSSL's CMS API (`CMS_sign()`, `CMS_verify()`). The OpenSSL-native path avoids any .NET dependency at the cost of more custom code; the BouncyCastle path provides a battle-tested CMS implementation at the cost of a subprocess call during signing. Both approaches are viable; the choice can be deferred to Phase 8 implementation. PAdES B-LTA (long-term archival with re-timestamping) is deferred as a future enhancement.

### 8. Image Processing

**Primary: libvips (C API)**
- **License:** LGPL-2.1
- **Capabilities:** Streaming pipeline architecture (3-10x less memory than ImageMagick), resize/rotate/crop/color adjustment
- **Performance:** Best-in-class, 2-5x faster than ImageMagick

**Image codecs (for PDF-embedded images):**
- libjpeg-turbo (BSD): JPEG decode/encode
- libpng (libpng license): PNG decode/encode
- OpenJPEG (BSD-2): JPEG2000 decode/encode

**Metadata stripping:** ExifTool via subprocess (`-all=` flag) or libexif for JPEG-specific EXIF removal

**Image editing workflow within PDFs:**
1. Use MuPDF's API to locate image objects on the page
2. Extract raw image data
3. Process with libvips (resize, rotate, crop)
4. Re-embed via MuPDF's content editing API
5. Invalidate tile cache for affected pages

### 9. Layers (Optional Content Groups)

**Still the weakest area in the ecosystem**, though MuPDF's OCG support is better than PDFium's.

| Operation | Approach | Effort |
|-----------|----------|--------|
| View/toggle existing layers | MuPDF OCG visibility API | 1-2 weeks |
| Create new layers | QPDF raw object manipulation (more reliable than MuPDF for structural changes) | 2-3 weeks |
| Move content between layers | Custom content stream parser + QPDF | 3-4 weeks |
| Layer properties editing | QPDF raw object manipulation | 1 week |

### 10. Document Layout Editing

**Handled by MuPDF + QPDF:**
- Page rotation: MuPDF API or QPDF `/Rotate` modification
- Page reordering: QPDF page tree manipulation (more robust for complex operations)
- Page insertion: QPDF `copyForeignObject()` for cross-document operations
- Page deletion: MuPDF or QPDF page tree removal
- PDF merging: QPDF (superior handling of shared resources and cross-references)
- PDF splitting: QPDF page extraction
- Crop box modification: MuPDF or QPDF `/CropBox`, `/MediaBox`, `/TrimBox`

---

## Integration Architecture

### System Architecture

```
+----------------------------------------------------------------------+
|                          PDFE Application                            |
|                          License: AGPL-3.0                           |
+----------------------------------------------------------------------+
|                                                                      |
|  +---------------------+    +----------------------------------+     |
|  |   Qt 6.8 LTS GUI    |    |       Plugin System              |     |
|  |  (Widgets + QML +   |    |  (OCR, Advanced Search,          |     |
|  |   QOpenGLWidget)     |    |   Digital Signatures)            |     |
|  +--------+------------+    +-------------+-------------------+     |
|           |                               |                          |
|  +--------v-------------------------------v-------------------+     |
|  |              Application Controller Layer                   |     |
|  |  +-------------+  +-----------+  +---------------------+   |     |
|  |  | Command Bus  |  | Undo/Redo |  | Event Dispatcher    |   |     |
|  |  | (CQRS-lite)  |  | Manager   |  | (Observer)          |   |     |
|  |  +-------------+  +-----------+  +---------------------+   |     |
|  +---------+------------------------------------------+--------+     |
|            |                                          |              |
|  +---------v-----------------+  +---------------------v--------+    |
|  |   Rendering Engine         |  |   Document Model Layer       |    |
|  |                            |  |                              |    |
|  |  +----------------------+  |  |  +------------------------+  |    |
|  |  | MuPDF Renderer       |  |  |  | MuPDF Document Object  |  |    |
|  |  | (primary: rendering, |  |  |  | (CANONICAL MODEL)      |  |    |
|  |  |  text extraction,    |  |  |  +------------------------+  |    |
|  |  |  annotations, forms, |  |  |  +------------------------+  |    |
|  |  |  redaction, JS)      |  |  |  | QPDF Structural Ops    |  |    |
|  |  +----------------------+  |  |  | (incremental save,     |  |    |
|  |  +----------------------+  |  |  |  linearization,        |  |    |
|  |  | Tile Cache           |  |  |  |  signature byte-range, |  |    |
|  |  | (512x512, LRU)       |  |  |  |  merge/split)         |  |    |
|  |  +----------------------+  |  |  +------------------------+  |    |
|  |  +----------------------+  |  |  +------------------------+  |    |
|  |  | Font Subsystem       |  |  |  | Text Reconstruction    |  |    |
|  |  | (FreeType + HarfBuzz |  |  |  | Engine (TRE)           |  |    |
|  |  |  + Metric Subst.)    |  |  |  | (scanned PDF editing)  |  |    |
|  |  +----------------------+  |  |  +------------------------+  |    |
|  +----------------------------+  +------------------------------+    |
|                                                                      |
|  +----------------------------------------------------------------+ |
|  |                   Platform Abstraction Layer                     | |
|  |  (File I/O, Clipboard, Printing, System Fonts, Dialogs)        | |
|  +----------------------------------------------------------------+ |
+----------------------------------------------------------------------+
```

### Key Architectural Patterns

1. **MuPDF as Document Model Owner:** MuPDF's `fz_document` / `pdf_document` is the canonical in-memory representation. All rendering, annotation editing, form interaction, and content editing go through MuPDF's API. QPDF is invoked only for specific structural operations (incremental save, linearization, merge/split) by exporting the document from MuPDF, processing with QPDF, and re-importing if needed. This is a **unidirectional flow** that eliminates the previous dual-model synchronization problem.

2. **Command Pattern for Undo/Redo:** All document mutations are Command objects with `execute()` and `undo()` methods. The UndoManager maintains undo/redo stacks (default depth: 500). Composite commands group related operations. Typing coalescing merges adjacent text insertions within 2-second windows.

3. **Tile-Based GPU Rendering:** Pages rendered as 512x512 tiles via MuPDF, uploaded to GPU textures, cached in configurable LRU cache (default: 512MB). Background threads pre-render adjacent tiles. During zoom, existing tiles are GPU-scaled for instant feedback while high-res tiles render in background.

4. **Hybrid Overlay Editing:** The PDF canvas is a rendered bitmap. Editing happens in overlay layers composited on top:
   - QPainter overlays for annotations, highlights, selection
   - Real QWidget instances for form fields and text editing
   - Coordinate transform system maps PDF space to screen space

5. **Text Reconstruction Engine:** Bridge between OCR output and editable PDF content for scanned documents. Region-level granularity (not page-level) with automatic quality assessment for Approach B/C fallback decision.

6. **Metric-Compatible Font Substitution:** When embedded fonts have missing glyphs, the system performs geometric analysis of available outlines, scores substitute candidates by measured visual similarity, and embeds substitutes as secondary font objects with automatic content stream font switching.

7. **Plugin Architecture:** Optional features (OCR, advanced search, digital signatures) loaded as shared libraries through a stable C++ API.

---

## UI Framework Decision

**Recommended: Qt 6.8 LTS (C++ Widgets + QOpenGLWidget)**

Unchanged from v1. Qt was selected after evaluating Qt, wxWidgets, GTK 4, Electron, Tauri, Rust GUIs (egui, Slint, Iced), Avalonia UI, JavaFX, and Flutter Desktop. The deciding factors:

1. **GPU canvas integration:** QOpenGLWidget / QRhi for direct GPU-accelerated tile rendering
2. **Zero-overhead C++ library integration:** MuPDF, QPDF, FreeType, HarfBuzz are all C/C++
3. **Widget maturity:** 30+ years, used in Autodesk Maya, VirtualBox, VLC, KDE

**UI Layout:**
```
+------------------------------------------------------------------+
| [File] [Edit] [View] [Insert] [Tools] [Forms] [Help]            |
+------------------------------------------------------------------+
| Context Toolbar (changes based on active tool)                    |
+----------+-------------------------------+-----------------------+
| Pages    |                               | Properties / Layers / |
| (thumb-  |   PDF Canvas (QOpenGLWidget)  | Comments / Search /   |
|  nails)  |                               | Fields / Metadata /   |
|          |   Tile-based GPU rendering    | Signatures / Redact   |
| Book-    |   + Annotation overlays       |                       |
| marks    |   + Form field overlays       | (Tabbed right panel,  |
|          |   + Tool overlays             |  QDockWidget)         |
+----------+-------------------------------+-----------------------+
| Status: [Page 3/47] [Zoom: 125%] [Tool: Select]                 |
+------------------------------------------------------------------+
```

---

## Critical Challenges & Mitigations

### 1. Text Reflow (HIGHEST RISK)

**Challenge:** PDF text is positioned glyph-by-glyph, not semantically structured. Reflow requires reconstructing paragraph structure from glyph positions.

**Mitigation:**
- Structural analysis: cluster characters → words → lines → paragraphs using spacing heuristics
- Editing via Logical Text Model, re-layout using HarfBuzz (shaping) + FreeType (metrics) + Knuth-Plass line-breaking
- Reflow only within edited paragraph's bounding box; surrounding content is not reflowed
- **Scope limitation:** Reliable only for single-column, standard-reading-order text initially
- MuPDF's Story/DOM API provides a starting point for content stream editing that PDFium lacked

### 2. Scanned PDF Text Editing (HIGH RISK -- NEW)

**Challenge:** Converting OCR output into visually consistent editable text with proper font matching, background color detection, and clean image masking.

**Mitigation:**
- Region-level granularity (not full-page) limits reconstruction complexity
- Quality assessment with weighted multi-signal scoring automatically falls back to image-level replacement when confidence is low
- Feathered-edge masking via gaussian-blurred alpha compositing prevents hard visual boundaries
- Background color detection uses inter-line gap sampling (most reliable source)
- Full architecture with 10 interface definitions in `docs/text-reconstruction-engine-architecture.md`

### 3. Font Glyph Substitution (MEDIUM-HIGH RISK -- NEW)

**Challenge:** Matching the visual appearance of a partially-embedded custom font well enough that substitute glyphs are not jarring next to original glyphs.

**Mitigation:**
- Measured geometric metrics (stem weight, x-height, contrast) are more reliable than PANOSE metadata
- Even a few available glyphs yield discriminating characteristics
- Dual-font embedding preserves original font program (no fragile table merging)
- User choice dialog provides escape hatch when automatic matching isn't good enough
- SQLite-backed cache avoids redundant expensive analysis
- Full architecture in `docs/font-substitution-architecture.md`

### 4. Real Redaction (MEDIUM RISK -- reduced from HIGH)

**Challenge:** Must actually remove content, not just cover it.

**Mitigation:**
- MuPDF's `pdf_redact_page()` handles the hard part (content stream parsing and removal)
- Custom verification pass searches entire object graph for remnants
- Full rewrite via QPDF eliminates traces in file structure
- Risk reduced from v1 because MuPDF's built-in redaction is well-tested

### 5. OCG/Layer Editing (MEDIUM RISK)

**Challenge:** No library provides complete high-level OCG editing APIs. Moving content between layers requires custom content stream parsing.

**Mitigation:**
- MuPDF's OCG visibility API for view/toggle (simpler than PDFium's)
- QPDF's raw object access for structural OCG creation/modification
- Start with view/toggle, add creation, defer content movement

### 6. Cross-Platform Build (LOW RISK -- reduced from MEDIUM)

**Challenge:** Build system complexity for multiple C++ dependencies.

**Mitigation:**
- MuPDF builds from source in 2-5 minutes with standard Makefile (vs PDFium's 30-60 minute GN/Ninja build)
- vcpkg for other dependencies
- CMake 3.25+ as unified build system
- CI matrix testing on all three platforms

---

## Performance Targets

| Metric | Target | Strategy |
|--------|--------|----------|
| Open 100-page PDF | < 500ms | Lazy loading, MuPDF incremental parsing |
| Open 1000-page PDF | < 2s | Memory-mapped I/O, deferred content stream parsing |
| Render standard A4 page (150 DPI) | < 100ms | MuPDF rendering + tile cache |
| Render A0 engineering drawing | < 500ms | Tile-based rendering, background pre-render |
| Scroll to next page | < 16ms (60 FPS) | Pre-rendered tile cache, predictive loading |
| Text search (100-page doc) | < 500ms | In-memory inverted index (Tantivy) |
| Incremental save | < 200ms | QPDF append-only write |
| Full save (100 pages) | < 5s | QPDF with object stream compression |
| OCR per page (300 DPI) | 2-5s | Background thread, Tesseract LSTM |
| Undo/redo | < 10ms | Command pattern, selective cache invalidation |
| Text region reconstruction (scanned page) | < 500ms | Pre-cached OCR results, single-region scope |

---

## License Summary

| License | Libraries | Compatibility with AGPL-3.0 |
|---------|-----------|----------------------------|
| **AGPL-3.0** | MuPDF | Same license -- fully compatible |
| **ISC** | MuJS (bundled with MuPDF) | Permissive -- compatible |
| **Apache 2.0** | QPDF, Tesseract, OpenSSL 3.x | Compatible with AGPL-3.0 (one-way) |
| **MIT** | HarfBuzz, Tantivy, Fontconfig, BouncyCastle C# | Compatible |
| **BSD-like (FTL)** | FreeType 2 | Compatible |
| **LGPL-2.1** | libvips, libexif | Compatible |
| **LGPL-3.0** | Qt 6 | Compatible with AGPL-3.0 |
| **BSD-2** | OpenJPEG | Compatible |
| **BSD** | Snowball stemmer, libjpeg-turbo | Compatible |
| **Unicode License** | ICU | Compatible |

All licenses in the stack are compatible with AGPL-3.0. The combined work is distributed under AGPL-3.0.

---

## Estimated Distribution Size

| Platform | Size (without OCR language packs) |
|----------|-----------------------------------|
| Windows (MSI/MSIX) | 35-55 MB |
| macOS (DMG) | 30-50 MB |
| Linux (AppImage) | 45-65 MB |
| + Bundled fonts | +68 MB |
| + CJK fonts (optional, per language) | +200-400 MB each |

Slightly smaller than v1 because MuPDF's binary is smaller than PDFium's, and MuJS replaces QuickJS.

---

## Implementation Roadmap

| Phase | Weeks | Focus |
|-------|-------|-------|
| 1. Foundation | 1-6 | CMake/vcpkg, MuPDF + QPDF integration, Document Model, tile cache, Qt main window, basic navigation |
| 2. Core Viewing | 7-10 | Text extraction, basic search, thumbnails, bookmarks, printing, font substitution pipeline, HiDPI |
| 3. Annotations & Markup | 11-14 | Sticky notes, highlights, markup, drawing (via MuPDF annotation API), comment panel, undo/redo |
| 4. Page Manipulation | 15-17 | Rotation, reordering, insertion, deletion, merging, splitting |
| 5. Forms | 18-21 | AcroForm rendering/filling (MuPDF), field creation, MuJS automation, tab order, FDF/XFDF |
| 6. Text Editing | 22-29 | Content stream parsing, text block detection, editing, reflow, font matching, **metric-compatible substitution engine** |
| 7. Scanned PDF Editing | 30-33 | **Text Reconstruction Engine**: OCR-to-editable pipeline, region reconstruction, image masking, quality assessment |
| 8. Advanced Features | 34-40 | Advanced search (Tantivy), digital signatures, redaction (MuPDF + verification), layers, encryption, permissions |
| 9. Image & Metadata | 41-44 | Image insert/remove/resize/rotate/crop, EXIF stripping, metadata editing |
| 10. Polish | 45-50 | Performance optimization, cross-platform testing, accessibility, plugin system, installers |

Note: Phases 6-7 (text editing + scanned PDF editing + font substitution) are the critical path and represent the bulk of novel engineering. MuPDF's built-in capabilities accelerate Phases 3, 5, and 8 significantly compared to the v1 roadmap.

---

## Reference Architectures

- **PDF4QT** (MIT) -- Qt-based C++ PDF editor, useful reference for content stream parsing and form implementation
- **Okular** (GPL-2.0) -- KDE document viewer using Poppler, useful reference for Qt PDF UI patterns
- **MuPDF's own `mupdf-gl` viewer** -- reference for MuPDF API usage patterns

---

## Appendix: Detailed Architecture Documents

| Document | Location | Contents |
|----------|----------|----------|
| AGPL library re-evaluation | `docs/agpl-library-architecture-reevaluation.md` | MuPDF vs PDFium comparison, QPDF role analysis, iText evaluation |
| Text Reconstruction Engine | `docs/text-reconstruction-engine-architecture.md` | Full TRE architecture: 10 C++ interfaces, HOCR pipeline, image masking, quality assessment, ADRs |
| Font Substitution System | `docs/font-substitution-architecture.md` | Metric analysis, scoring algorithm, dual-font embedding, content stream font switching, edge cases |
| Security & Forms (v1) | `docs/security-forms-annotations-architecture.md` | Encryption, signatures, forms, annotations (some superseded by MuPDF built-ins) |
| OCR & Search (v1) | `docs/research-ocr-search-fonts.md` | Tesseract evaluation, Tantivy analysis, font collection recommendations |
