# PDFE Architecture Re-evaluation: AGPL Library Stack

**Date:** 2026-02-05
**Status:** Proposed
**Author:** Backend Architect
**Supersedes:** Portions of `project-analysis-consolidated.md` (library selection only)

---

## Executive Summary

With PDFE now licensed under AGPL-3.0, MuPDF (AGPL-3.0) becomes fully license-compatible and represents a substantial upgrade over PDFium as the primary PDF library. This document evaluates replacing PDFium with MuPDF, reassesses QPDF's role, evaluates iText and other GPL/AGPL libraries, and provides a revised architecture.

**Key Recommendation:** Adopt a **MuPDF (primary) + QPDF (structural complement)** dual-library architecture, replacing PDFium entirely. This combination gains significant editing capabilities (annotations, redaction, forms, content streams) while retaining QPDF's irreplaceable strengths in incremental saves, linearization, and content-preserving transformations. iText is not recommended. MuPDF's built-in MuJS engine replaces QuickJS for form JavaScript.

**Net Assessment:** The switch from PDFium to MuPDF is a clear win. It simplifies the architecture (MuPDF covers rendering AND editing), improves rendering quality marginally, adds substantial built-in editing capabilities, and simplifies the build system. The one meaningful capability loss is XFA form rendering, which is deprecated in PDF 2.0 and a declining concern.

---

## Table of Contents

1. [MuPDF vs PDFium: Detailed Comparison](#1-mupdf-vs-pdfium-detailed-comparison)
2. [QPDF's Remaining Role](#2-qpdfs-remaining-role)
3. [iText (AGPL) Evaluation](#3-itext-agpl-evaluation)
4. [Other GPL/AGPL Libraries](#4-other-gplagpl-libraries)
5. [Revised Architecture](#5-revised-architecture)
6. [Gains vs Losses Analysis](#6-gains-vs-losses-analysis)
7. [Build System Implications](#7-build-system-implications)
8. [Migration Impact on Existing Architecture](#8-migration-impact-on-existing-architecture)
9. [Revised Technology Stack](#9-revised-technology-stack)
10. [ADR: Switch from PDFium to MuPDF](#10-adr-switch-from-pdfium-to-mupdf)

---

## 1. MuPDF vs PDFium: Detailed Comparison

### 1.1 Rendering Quality

| Aspect | MuPDF | PDFium | Assessment |
|--------|-------|--------|------------|
| **Anti-aliasing** | High-fidelity, designed for quality | Chrome-grade, designed for speed | MuPDF marginally better |
| **Text metrics** | Pixel-accurate, sub-pixel positioning | Excellent | Comparable, slight MuPDF edge |
| **Transparency/blending** | Full PDF 2.0 compliance | Full compliance | Equivalent |
| **Color management** | ICC profile support | ICC profile support | Equivalent |
| **Complex vector graphics** | Excellent | Excellent | Equivalent |
| **Embedded font handling** | Excellent, FreeType-based internally | Excellent | Equivalent |
| **Overall rendering** | Slightly higher fidelity | Battle-tested at Chrome scale | MuPDF wins by a small margin |

Independent benchmarks (PDF Association 2021 survey) found MuPDF and PDFium "neck and neck in rendering results." MuPDF has historically been slightly favored for rendering quality in the PDF community, particularly for anti-aliased text at small sizes, while PDFium is slightly faster for bulk rasterization. For a desktop PDF editor where quality matters more than milliseconds-per-page, MuPDF has the edge.

### 1.2 Editing Capabilities

This is where MuPDF dramatically outperforms PDFium:

| Capability | MuPDF | PDFium | Impact |
|-----------|-------|--------|--------|
| **Annotation CRUD** | Full API: `pdf_create_annot()`, `pdf_set_annot_*()`, `pdf_delete_annot()`. Automatic appearance stream generation. All standard annotation types. | `FPDFPage_CreateAnnot()`, `FPDFAnnot_Set*()`. Partial appearance stream generation. | MuPDF is more complete, especially for appearance stream generation |
| **Redaction** | Built-in `pdf_redact_page()` API. Character-level text removal. Image blanking/removal. Content stream rewriting. Handles shared content streams. | None. Redaction must be built entirely from scratch. | **Massive advantage for MuPDF.** Eliminates months of custom development. |
| **Form field creation** | `pdf_create_signature_widget()`, `pdf_set_text_field_value()`, choice widget APIs. Widget creation and property manipulation. | Limited creation APIs. Primarily focused on rendering and interaction, not authoring. | MuPDF moderately better for form authoring |
| **Form interaction** | AcroForm rendering/filling. Widget iteration. Event handling. | Full AcroForm rendering/filling. XFA support (static + dynamic). Richer interaction API. | PDFium wins on XFA. Comparable on AcroForm. |
| **Content stream editing** | Story/DOM API for creating new content with HTML/CSS-like layout. `mutool clean` for content stream rewriting. | None beyond page object manipulation. | **Significant MuPDF advantage** for text layout and content creation |
| **Text extraction** | Structured text API (`fz_stext_page`). Character-level bounding boxes. Font metadata. Block/line/character hierarchy. JSON/HTML/XML output. | Basic text extraction. Character positions available. | MuPDF's structured text is more capable |
| **Search** | Built-in text search with highlight support. | Basic text search. | Comparable; both need supplementation for advanced search |
| **Digital signatures** | `mutool sign`. Signature field creation/verification. Multiple signature field support. | None. | MuPDF has basic support; still needs supplementation |
| **Encryption read/write** | Full AES-256 (V5 R6) read and write. All legacy formats. | Read only. | MuPDF significantly better |
| **Multi-format support** | PDF, XPS, EPUB, SVG, CBZ | PDF only | MuPDF has broader format coverage |
| **Document conversion** | PDF to HTML, SVG, CBZ, text. Bi-directional. | PDF to bitmap only. | MuPDF much more capable |

### 1.3 What PDFium Can Do That MuPDF Cannot

This is the critical counter-analysis:

| PDFium Capability | MuPDF Status | Severity |
|-------------------|-------------|----------|
| **XFA forms (static + dynamic)** | Cannot render dynamic XFA. Can partially handle static XFA with AcroForm fallback. | **Medium.** XFA is deprecated in PDF 2.0 (ISO 32000-2). Adobe stopped creating new XFA forms. Legacy XFA files are declining. Static XFA with AcroForm duplicates (the most common type) will still work. Dynamic-only XFA is rare outside government/enterprise legacy forms. |
| **V8 JavaScript engine** | MuJS (ES5 only). No ES6+ features (no `let`, arrow functions, `class`). | **Low-Medium.** PDF form JavaScript is almost exclusively ES5. Real-world PDF forms use `AFSimple_Calculate`, `AFNumber_Format`, etc. -- all ES5. The ES5 limitation is rarely encountered in practice with actual PDF forms. |
| **Chrome-scale battle testing** | MuPDF is battle-tested in SumatraPDF, many embedded devices, Artifex commercial products. | **Low.** MuPDF is mature and widely deployed, just not at Chrome's scale. |
| **Stable pre-built binaries** | No equivalent to `pdfium-binaries`. Must build from source or use community CMake. | **Low-Medium.** MuPDF builds are simpler than PDFium from source. See Section 7. |

### 1.4 MuPDF's Built-in JavaScript: MuJS vs QuickJS

| Aspect | MuJS (MuPDF built-in) | QuickJS (current plan) |
|--------|----------------------|----------------------|
| **ES version** | ES5.1 | ES2023 |
| **Size** | ~100KB | ~210KB |
| **PDF form JS compatibility** | Excellent -- ES5 covers 99%+ of real PDF forms | Excellent |
| **Sandboxing** | Built-in via MuPDF's document model | Must be configured manually |
| **Integration effort** | Zero -- already embedded in MuPDF | Requires custom integration |
| **Acrobat JS API** | Partial implementation already in MuPDF | Must be implemented from scratch |
| **Performance** | Adequate for form scripts | Faster for complex scripts |

**Recommendation:** Use MuPDF's built-in MuJS engine for PDF form JavaScript. It already implements a subset of the Acrobat JavaScript API and is integrated with MuPDF's document model. This eliminates the need for QuickJS and the entire custom Acrobat JS API implementation effort. If edge cases arise where ES6+ features are needed (unlikely for PDF forms), QuickJS can be added as a fallback later, but this is an optimization that should not be in the initial architecture.

### 1.5 Verdict: MuPDF Replaces PDFium

MuPDF is the clear winner for PDFE under AGPL licensing:

- **Rendering quality:** Equal or marginally better
- **Editing capabilities:** Dramatically superior (annotations, redaction, forms, content streams)
- **Built-in features:** Text extraction, search, JS engine, multi-format, encryption write
- **What we lose:** XFA dynamic forms (deprecated), V8 JS engine (unnecessary)
- **Net complexity:** Significantly reduced -- MuPDF covers both rendering AND editing

---

## 2. QPDF's Remaining Role

### 2.1 Can MuPDF Replace QPDF Entirely?

No. While MuPDF takes over many capabilities that previously required QPDF, there are several areas where QPDF remains essential or significantly superior:

| Capability | MuPDF | QPDF | Who Wins |
|-----------|-------|------|----------|
| **Incremental saves** | Supported but with caveats. File repair triggers full rewrite. Some operations break incremental save capability. | Rock-solid. Designed specifically for this. Content-preserving by design. | **QPDF** |
| **Digital signature preservation** | Incremental save sometimes breaks signatures. Byte-range handling is less mature. | Precise byte-range control. Signature-preserving incremental saves are a core use case. | **QPDF** |
| **Linearization** | No longer supported (feature was removed). Cannot produce linearized/"Fast Web View" PDFs. | Best-in-class linearization. Full RFC compliance. Linearization checking/verification. | **QPDF** (MuPDF cannot do this at all) |
| **PDF object graph manipulation** | Has object access but oriented toward rendering/editing workflows. | Purpose-built for arbitrary PDF object manipulation. JSON representation. Full object graph traversal. | **QPDF** |
| **Content-preserving transforms** | Rewrites content streams during operations. Not designed for minimal-change transformations. | Explicitly designed for content-preserving transforms. Minimal changes to file structure. | **QPDF** |
| **Encryption handler quality** | Supports all encryption versions. Write support confirmed. | Supports all encryption versions. More granular permission control. Better documented encryption API. `--allow-insecure` for edge cases. | **QPDF slightly** |
| **Page tree manipulation** | Page insert/delete/move/copy. Adequate for most operations. | More robust page tree handling. Better handling of shared resources, broken page trees, and edge cases. | **QPDF** for robustness |
| **PDF repair/recovery** | Can repair files during open. | More sophisticated repair. JSON inspection mode. QDF (pretty-printed) output for debugging. | **QPDF** |
| **Object stream compression** | Supported | Supported with fine-grained control | **QPDF slightly** |
| **Cross-reference stream handling** | Supported | Supported with more control | **QPDF slightly** |

### 2.2 Critical QPDF-Only Capabilities

These capabilities have no equivalent in MuPDF and are essential for a professional PDF editor:

1. **Linearization (Fast Web View):** MuPDF dropped this feature. QPDF is the only library in our stack that can produce linearized PDFs. While linearization is declining in importance, it remains a feature users expect in an Acrobat-class editor.

2. **Signature-preserving incremental saves:** When a user signs a PDF and then makes further changes (adds annotations, fills forms), those changes must be saved incrementally to preserve the existing signature. QPDF's content-preserving incremental save is significantly more reliable than MuPDF's for this purpose.

3. **Precise byte-range control for signatures:** The digital signature workflow requires calculating exact byte ranges and inserting signature containers at precise offsets. QPDF's object-level control is essential for this.

4. **PDF forensics and repair:** QPDF's JSON mode, QDF output, and repair capabilities are invaluable for debugging PDF issues that a professional editor must handle.

### 2.3 QPDF's New Role: Structural Integrity Layer

QPDF transitions from "structural manipulation engine" (its previous role as the canonical document representation) to a "structural integrity layer" that handles specific operations where its capabilities are irreplaceable:

**Primary responsibilities:**
- Incremental saves (especially signature-preserving)
- Linearization
- PDF encryption write (granular permission control)
- Page tree operations for complex merges and splits
- File repair and integrity checking
- Content-preserving transformations (metadata stripping, object renumbering)
- Byte-range management for digital signatures

**Transferred to MuPDF:**
- Annotation creation and manipulation
- Form field creation and manipulation
- Content stream parsing and rewriting (redaction, text editing)
- Encryption read/write for standard use cases
- Text extraction
- Page rendering (already PDFium's role, now MuPDF's)

### 2.4 Revised Dual-Library Strategy

The canonical document representation shifts to **MuPDF's document model** (previously QPDF's object graph). This is a significant architectural change:

**Previous architecture (PDFium + QPDF):**
- QPDF = source of truth (document model owner)
- PDFium = rendering service (stateless, re-renders from QPDF's output)

**Revised architecture (MuPDF + QPDF):**
- MuPDF = source of truth AND renderer (document model owner, editing engine, rendering engine)
- QPDF = structural operations service (called for specific operations: save, linearize, encrypt, sign)

This is a cleaner separation because MuPDF natively understands the document it is rendering and editing. There is no synchronization problem between two document representations -- MuPDF owns the document and QPDF operates on the serialized output when needed.

---

## 3. iText (AGPL) Evaluation

### 3.1 iText's Strengths

iText is the gold standard for PDF digital signatures:
- Full PAdES B-B through B-LTA support
- Two-step signing for HSM/cloud integration
- Post-quantum signature proof-of-concept (ML-DSA, SLH-DSA)
- Comprehensive validation with OCSP/CRL handling
- PDF/A compliance checking
- Advanced form operations
- Redaction with sanitization

### 3.2 Integration Cost Analysis

iText is Java/.NET only. Integration with a C++/Qt application requires one of:

| Approach | Complexity | Latency | Maintenance |
|----------|-----------|---------|-------------|
| **Java subprocess** | High. Must bundle JRE (~150MB). Process communication overhead. Serialization/deserialization of PDF data. | 500ms+ startup per operation. | Must maintain Java wrapper code alongside C++ codebase. JRE version management. |
| **.NET subprocess** | High. Must bundle .NET runtime (~80MB). Same communication overhead. | 300ms+ startup per operation. | Must maintain C# wrapper code. .NET version management. |
| **JNI bridge** | Very High. Complex memory management across boundaries. Crash propagation risks. | Low per-call, high for setup. | Extremely fragile. JNI bugs are notoriously difficult to debug. |
| **gRPC/REST service** | High. Must run a persistent service process. Service lifecycle management. | 10-50ms per call (local). | Must maintain service + client code. Service health monitoring. |

### 3.3 What iText Provides That We Cannot Get Elsewhere

| Capability | iText | Alternative in MuPDF + QPDF + BouncyCastle/OpenSSL |
|-----------|-------|-----------------------------------------------------|
| **PAdES B-B** | Built-in, one-call API | Achievable with BouncyCastle + QPDF (custom integration, ~2-3 weeks) |
| **PAdES B-T** | Built-in | Achievable with BouncyCastle + TSA client (~1 week additional) |
| **PAdES B-LT** | Built-in | Achievable with BouncyCastle + OCSP/CRL client (~2 weeks additional) |
| **PAdES B-LTA** | Built-in | Achievable but complex (~3-4 weeks additional) |
| **Two-step/cloud signing** | Built-in | Custom implementation (~2 weeks) |
| **Post-quantum signatures** | Proof-of-concept | Not available in BouncyCastle yet |
| **PDF/A validation** | Built-in | No open-source C/C++ alternative. veraPDF (Java) exists. |
| **XFA flattening** | Built-in (pdfXFA add-on) | Not available |

### 3.4 Recommendation: Do Not Integrate iText

**iText is not worth the integration complexity.** The analysis:

1. **Digital signatures (the main attraction):** PAdES B-B through B-LT is achievable with BouncyCastle (C#) + QPDF byte-range management. This is already in the architecture. The additional development time (6-8 weeks) is less than the ongoing maintenance burden of a Java/.NET subprocess integration.

2. **PAdES B-LTA:** This is the only capability that is genuinely difficult to replicate. However, B-LTA is a "Recommended" priority, not "Required." It can be deferred and implemented natively when BouncyCastle adds the necessary primitives, or added as an optional Java-based tool later.

3. **Post-quantum signatures:** Research-stage feature. Not production-relevant yet. Can be revisited in 2-3 years.

4. **PDF/A validation:** If needed, veraPDF (Apache 2.0, Java) can be invoked as a standalone subprocess without the full iText dependency.

5. **XFA flattening:** Declining relevance (XFA deprecated in PDF 2.0).

6. **Architectural cost:** Adding a JVM or .NET runtime dependency to a lean C++/Qt application fundamentally changes the deployment model, increases installer size by 80-150MB, adds startup latency, and creates a class of bugs (inter-process communication, serialization, runtime version conflicts) that do not exist in a pure native application.

**Bottom line:** Keep the BouncyCastle (C#) + QPDF + OpenSSL approach for digital signatures. It covers PAdES B-B through B-LT natively without the weight of iText. Revisit iText only if PAdES B-LTA becomes a hard requirement and cannot be implemented with BouncyCastle.

---

## 4. Other GPL/AGPL Libraries

### 4.1 Poppler (GPL-2.0+)

| Aspect | Assessment |
|--------|-----------|
| **License** | GPL-2.0-or-later. Compatible with AGPL-3.0 (AGPL-3.0 is a superset of GPL-3.0, and GPL-2+-licensed code can be used under GPL-3.0 terms per the "or later" clause). |
| **Rendering** | Good but inferior to both MuPDF and PDFium. Uses Cairo or Splash backends. |
| **Text extraction** | Good. Powers most Linux PDF text extraction (pdftotext). |
| **Annotations** | Partial support. Qt6 frontend available. |
| **Forms** | Limited. No XFA. |
| **Editing** | Minimal. Primarily a rendering/extraction library. |
| **Unique capabilities over MuPDF** | None identified. MuPDF matches or exceeds Poppler in every category. |

**Recommendation: Do not add Poppler.** It provides no capability that MuPDF does not already provide, and its rendering quality is inferior. Adding it would increase complexity without benefit.

### 4.2 Ghostscript (AGPL-3.0)

| Aspect | Assessment |
|--------|-----------|
| **License** | AGPL-3.0. Same as MuPDF (both from Artifex). |
| **Primary strength** | PostScript interpretation. PDF to printer output (PCL, PostScript). Most comprehensive format conversion engine. |
| **PDF capabilities** | Full rendering. PDF creation. PDF/A conversion. |
| **Unique capabilities** | PostScript rendering (no other library handles this well). PCL output. PDF/A conversion. PDF optimization (image downsampling, font subsetting during conversion). |
| **Size** | Large (~30MB+). Heavy dependency chain. |
| **Overlap with MuPDF** | Significant overlap in rendering. Ghostscript and MuPDF are siblings from Artifex. |

**Recommendation: Do not add Ghostscript to the core architecture.** However, consider it as an **optional subprocess tool** for two specific use cases:

1. **PostScript file import:** If PDFE needs to open .ps/.eps files, Ghostscript is the only viable converter. This can be handled as a subprocess (`gs -dNOPAUSE -dBATCH -sDEVICE=pdfwrite -sOutputFile=output.pdf input.ps`) without linking the library.

2. **PDF/A conversion:** Ghostscript can convert PDFs to PDF/A-1b, PDF/A-2b, and PDF/A-3b profiles. Again, this can be a subprocess operation.

Neither justifies adding Ghostscript as a linked library. Subprocess invocation keeps the architecture clean.

### 4.3 LibreOffice/LibreOffice Draw (MPL-2.0)

Not PDF-specific. Occasionally mentioned for PDF editing but uses its own internal PDF import that loses fidelity. Not relevant.

### 4.4 Xapian (GPL-2.0)

Previously rejected for its GPL-2.0 license. Now compatible with AGPL-3.0. However, Tantivy (MIT) remains the better choice:
- Tantivy has full regex support; Xapian has only wildcards
- Tantivy is faster in benchmarks
- Tantivy's MIT license avoids adding another copyleft dependency
- The Rust FFI overhead is minimal

**Recommendation: Keep Tantivy.** Xapian's GPL compatibility does not change the technical evaluation.

### 4.5 FontForge (GPL-3.0)

Previously rejected for GPL licensing. Now compatible. Provides programmatic font creation and editing. However, the current architecture uses fonttools (Python, MIT) for font manipulation and HarfBuzz subsetting for glyph extraction. FontForge's C library (libfontforge) could replace the Python fonttools dependency for runtime font generation, avoiding the Python runtime requirement.

**Recommendation: Consider FontForge as a future optimization** to replace the fonttools Python dependency for font generation. Not a priority for initial architecture.

### 4.6 Summary: GPL/AGPL Library Decisions

| Library | License | Decision | Rationale |
|---------|---------|----------|-----------|
| **MuPDF** | AGPL-3.0 | **ADOPT** as primary library | Replaces PDFium with significant net gains |
| **Poppler** | GPL-2.0+ | **REJECT** | No capabilities beyond MuPDF |
| **Ghostscript** | AGPL-3.0 | **Optional subprocess** | PostScript import and PDF/A conversion only |
| **iText** | AGPL-3.0 | **REJECT** | Integration cost exceeds benefit |
| **Xapian** | GPL-2.0 | **REJECT** | Tantivy remains technically superior |
| **FontForge** | GPL-3.0 | **DEFER** | Potential future optimization |

---

## 5. Revised Architecture

### 5.1 System Architecture Diagram

```
+====================================================================+
|                         PDFE Application                            |
+====================================================================+
|                                                                     |
|  +--------------------+    +-------------------------------+        |
|  |    Qt 6.8 LTS GUI  |    |      Plugin System            |        |
|  |  (Widgets + QML)   |    |  (OCR, Search, Signatures)    |        |
|  +--------+-----------+    +-------------+----------------+        |
|           |                              |                          |
|  +--------v------------------------------v----------------+        |
|  |              Application Controller Layer               |        |
|  |  +-------------+  +-----------+  +------------------+  |        |
|  |  | Command Bus  |  | Undo/Redo |  | Event Dispatcher |  |        |
|  |  | (CQRS-lite)  |  | Manager   |  | (Observer)       |  |        |
|  |  +-------------+  +-----------+  +------------------+  |        |
|  +-----+------------------------------------------+-------+        |
|        |                                          |                 |
|  +-----v------------------------------------------v--------+       |
|  |              MuPDF Engine (AGPL-3.0)                     |       |
|  |              [Primary Library - Document Owner]           |       |
|  |                                                          |       |
|  |  +------------------+  +------------------+              |       |
|  |  |  Rendering       |  |  Editing Engine  |              |       |
|  |  |  - Page render   |  |  - Annotations   |              |       |
|  |  |  - Tile cache    |  |  - Redaction     |              |       |
|  |  |  - Text extract  |  |  - Forms         |              |       |
|  |  |  - Search        |  |  - Content edit  |              |       |
|  |  +------------------+  +------------------+              |       |
|  |  +------------------+  +------------------+              |       |
|  |  |  MuJS Engine     |  |  Encryption      |              |       |
|  |  |  (Form JS)       |  |  (Read/Write)    |              |       |
|  |  +------------------+  +------------------+              |       |
|  +----------------------------------------------------------+       |
|                                                                     |
|  +----------------------------------------------------------+       |
|  |              QPDF (Apache 2.0)                            |       |
|  |              [Structural Integrity Layer]                  |       |
|  |                                                           |       |
|  |  +------------------+  +------------------+               |       |
|  |  |  Incremental Save|  |  Linearization   |               |       |
|  |  |  (Sig-preserving)|  |  (Fast Web View) |               |       |
|  |  +------------------+  +------------------+               |       |
|  |  +------------------+  +------------------+               |       |
|  |  |  Signature Byte  |  |  PDF Repair &    |               |       |
|  |  |  Range Mgmt      |  |  Integrity Check |               |       |
|  |  +------------------+  +------------------+               |       |
|  |  +------------------+  +------------------+               |       |
|  |  |  Complex Merges  |  |  Metadata Strip  |               |       |
|  |  |  & Splits        |  |  & Sanitization  |               |       |
|  |  +------------------+  +------------------+               |       |
|  +----------------------------------------------------------+       |
|                                                                     |
|  +----------------------------------------------------------+       |
|  |              Supporting Libraries                         |       |
|  |                                                           |       |
|  |  +------------------+  +------------------+               |       |
|  |  |  OpenSSL 3.x     |  |  BouncyCastle C# |               |       |
|  |  |  (Crypto)        |  |  (CMS/Signatures)|               |       |
|  |  +------------------+  +------------------+               |       |
|  |  +------------------+  +------------------+               |       |
|  |  |  Tesseract 5.x   |  |  Tantivy (Rust)  |               |       |
|  |  |  (OCR)           |  |  (Search Index)  |               |       |
|  |  +------------------+  +------------------+               |       |
|  |  +------------------+  +------------------+               |       |
|  |  |  FreeType +      |  |  libvips         |               |       |
|  |  |  HarfBuzz (Fonts)|  |  (Image Process) |               |       |
|  |  +------------------+  +------------------+               |       |
|  +----------------------------------------------------------+       |
|                                                                     |
|  +----------------------------------------------------------+       |
|  |              Platform Abstraction Layer                    |       |
|  |  (File I/O, Clipboard, Printing, System Fonts, Dialogs)  |       |
|  +----------------------------------------------------------+       |
+=====================================================================+
```

### 5.2 Data Flow: Document Lifecycle

```
[File Open]
     |
     v
  MuPDF: fz_open_document()
  - Parses PDF structure
  - Handles decryption (if encrypted)
  - MuPDF now OWNS the document model
     |
     v
[Rendering]
     |
     v
  MuPDF: fz_run_page() -> fz_device (pixmap)
  - Tile-based rendering (512x512, LRU cache)
  - Direct rendering from MuPDF's document model
  - No synchronization needed (single source of truth)
     |
     v
[Editing Operations]
     |
     +---> Annotations: MuPDF pdf_create_annot() / pdf_set_annot_*()
     +---> Forms: MuPDF pdf_set_text_field_value() / widget APIs
     +---> Redaction: MuPDF pdf_redact_page()
     +---> Text/Content: MuPDF Story API / content stream access
     +---> Encryption: MuPDF or QPDF (depending on operation)
     |
     v
[Save Operations]
     |
     +---> Simple save: MuPDF pdf_save_document()
     |
     +---> Incremental save (sig-preserving):
     |     MuPDF exports -> QPDF incremental write
     |
     +---> Linearized save:
     |     MuPDF exports -> QPDF linearize
     |
     +---> Save with encryption:
     |     MuPDF or QPDF (QPDF for granular permissions)
     |
     +---> Save after redaction:
           MuPDF redact -> QPDF full rewrite (no incremental)
           + QPDF metadata sanitization
```

### 5.3 MuPDF-QPDF Interaction Pattern

The interaction between MuPDF and QPDF is now **unidirectional** for most operations:

```
[Normal editing workflow]
MuPDF owns document -> MuPDF saves document -> Done

[Signature-preserving workflow]
MuPDF owns document -> MuPDF writes to temp file
-> QPDF opens temp file -> QPDF performs incremental save -> Done

[Linearization workflow]
MuPDF owns document -> MuPDF writes to temp file
-> QPDF opens temp file -> QPDF linearizes -> Done

[Digital signature creation workflow]
MuPDF owns document -> QPDF opens in parallel for byte-range calculation
-> BouncyCastle creates CMS container -> QPDF inserts signature -> Done

[Post-redaction workflow]
MuPDF applies redaction -> MuPDF writes to temp file
-> QPDF opens -> QPDF sanitizes metadata -> QPDF rewrites (non-incremental)
-> Done
```

This is cleaner than the previous architecture where every edit required synchronization between PDFium and QPDF. MuPDF edits the document directly, and QPDF is invoked only for specific structural operations.

### 5.4 Eliminated Complexity: The Synchronization Problem

The previous architecture's biggest risk (ranked "MEDIUM RISK" in the consolidated analysis) was:

> **PDFium + QPDF Synchronization:** Two separate PDF document representations must stay synchronized. Edits through QPDF must invalidate the correct parts of PDFium's rendering state.

This problem **no longer exists.** MuPDF is both the document model and the renderer. When an annotation is created via MuPDF's API, MuPDF already knows about it and will render it correctly. There is no second representation to synchronize.

This eliminates an entire category of bugs and simplifies the invalidation/caching logic significantly.

---

## 6. Gains vs Losses Analysis

### 6.1 Capabilities GAINED by Switching from PDFium to MuPDF

| Capability | Previous Status (PDFium) | New Status (MuPDF) | Development Time Saved |
|-----------|-------------------------|-------------------|----------------------|
| **Built-in redaction** | Not available. Required building an entire custom content stream parser on QPDF. Estimated 3-6 months of engineering. | `pdf_redact_page()` -- handles character-level text removal, image blanking, shared content streams. | **3-6 months** |
| **Annotation appearance streams** | Partially automatic. Many types required custom appearance stream generation. | Fully automatic for all standard annotation types. | **2-4 weeks** |
| **Content stream editing** | Not available. Required custom parser. | Story/DOM API for layout. `mutool clean` for rewriting. | **2-4 weeks** |
| **Structured text extraction** | Basic text extraction with positions. | Full hierarchical extraction: blocks -> lines -> characters. Font metadata, colors, bounding boxes. JSON/HTML/XML output. | **1-2 weeks** |
| **Built-in form JavaScript** | Required QuickJS integration + custom Acrobat JS API implementation (~4-6 weeks). | MuJS already integrated with partial Acrobat JS API. | **4-6 weeks** |
| **Encryption write support** | Required QPDF for all encryption writes. | MuPDF handles standard encryption natively. QPDF only needed for granular permissions. | **1 week** |
| **Multi-format support** | PDF only. | PDF, XPS, EPUB, SVG, CBZ. Document conversion between formats. | **Nice to have** |
| **Digital signature basics** | Not available. | `mutool sign` for basic signing/verification. Signature field creation. | **1-2 weeks** |
| **Document model unity** | Two models (PDFium + QPDF) required synchronization. | Single model. No synchronization needed. | **Ongoing maintenance savings** |

**Total estimated development time saved: 5-9 months.**

### 6.2 Capabilities LOST by Switching from PDFium to MuPDF

| Capability | Impact | Mitigation |
|-----------|--------|------------|
| **XFA dynamic form rendering** | Medium. Affects legacy enterprise/government forms. Declining in prevalence. | Static XFA with AcroForm fallback still works. For dynamic-only XFA, display a "This form requires Adobe Acrobat" message (same as most non-Adobe PDF viewers). Long-term: build an XFA-to-AcroForm converter using the XFA XML spec. |
| **V8 JavaScript engine** | Low. ES5 covers effectively all PDF form JavaScript. | MuJS (ES5.1) is sufficient. Monitor for real-world failures and add QuickJS fallback if needed. |
| **Pre-built binaries (pdfium-binaries)** | Low-Medium. Requires building MuPDF from source. | MuPDF's build system is simpler than PDFium's from source. Community CMake wrappers exist. See Section 7. |
| **Chrome-scale validation** | Low. MuPDF is well-tested across SumatraPDF and Artifex commercial products. | Augment with our own PDF test corpus and fuzzing. |

### 6.3 Net Complexity Change

| Area | Previous (PDFium + QPDF) | Revised (MuPDF + QPDF) | Change |
|------|-------------------------|------------------------|--------|
| **Core libraries** | 2 libraries with document model sync | 2 libraries, unidirectional flow | Simpler |
| **Document model** | Dual model (sync required) | Single model (MuPDF owns) | **Much simpler** |
| **Rendering pipeline** | PDFium as stateless render service | MuPDF renders its own model | Simpler |
| **Annotation system** | PDFium render + QPDF persist + custom appearance streams | MuPDF handles all three | **Much simpler** |
| **Redaction engine** | Custom build on QPDF (massive effort) | MuPDF built-in + QPDF sanitization | **Much simpler** |
| **Form system** | PDFium render/interact + QPDF create + QuickJS for JS + custom Acrobat API | MuPDF handles render/interact/create/JS | **Much simpler** |
| **Encryption** | PDFium read + QPDF write | MuPDF read/write + QPDF for granular cases | Slightly simpler |
| **Save system** | QPDF always | MuPDF default + QPDF for special cases | Equivalent |
| **Build system** | PDFium (GN/Ninja or pdfium-binaries) + QPDF (CMake) | MuPDF (Make/VS + community CMake) + QPDF (CMake) | Slightly simpler |
| **JavaScript sandbox** | QuickJS + custom Acrobat API | MuJS (built-in) + extend existing API | **Much simpler** |
| **Total external dependencies** | PDFium + QPDF + QuickJS + ... | MuPDF + QPDF + ... | One fewer library |

**Net complexity: Significantly reduced.** The architecture goes from "two libraries that must be carefully synchronized with several custom bridges" to "one primary library that handles most operations natively, with a second library for specific structural operations."

---

## 7. Build System Implications

### 7.1 PDFium Build (Previous)

- **From source:** Requires Chromium's `depot_tools`, GN build system, Ninja. ~10GB of Chromium checkout. Build takes 30-60 minutes. Complex and fragile.
- **Via pdfium-binaries:** Pre-built shared libraries. Easy to integrate. But: no V8/XFA (those require the full Chromium build), no control over compile options, version lag vs Chromium HEAD.
- **Integration:** Drop-in headers + .dll/.so/.dylib. CMake `find_package` or manual path configuration.

### 7.2 MuPDF Build (Proposed)

- **From source:** Standard Makefile (Linux/macOS) or Visual Studio solution (Windows). Self-contained -- all third-party dependencies (FreeType, HarfBuzz, libjpeg, OpenJPEG, zlib) are bundled in the `thirdparty/` directory. Build takes 2-5 minutes. Much simpler than PDFium.
- **Community CMake:** [mupdf-cmake](https://github.com/FabriceSalvaire/mupdf-cmake) provides a CMake-based build. Tested on Android, Windows, CentOS.
- **vcpkg:** MuPDF is available as a vcpkg port (`mupdf`).
- **Integration:** C API headers in `include/mupdf/`. Also provides an auto-generated C++ wrapper with RAII and exceptions.

### 7.3 Build System Comparison

| Aspect | PDFium (from source) | PDFium (pdfium-binaries) | MuPDF (from source) |
|--------|---------------------|------------------------|---------------------|
| **Build time** | 30-60 min | N/A (pre-built) | 2-5 min |
| **Disk space** | ~10GB (Chromium) | ~15MB | ~100MB (with thirdparty) |
| **Build system** | GN + Ninja | N/A | Make / VS Solution |
| **CMake integration** | Difficult | Easy | Community CMake or vcpkg |
| **Cross-platform** | Complex | Pre-built per platform | Simple (Make or VS) |
| **Dependency management** | Chromium's system | None needed | Bundled in thirdparty/ |
| **Reproducibility** | Tied to Chromium version | Tied to release version | Standard Makefile |
| **CI complexity** | High | Low | Low |

**MuPDF from source is simpler than PDFium from source by a wide margin.** The loss of pdfium-binaries' convenience is offset by MuPDF's straightforward build system. For CI/CD, building MuPDF from source on all three platforms is routine.

### 7.4 Recommended Build Integration

```
PDFE CMake Build System
    |
    +-- MuPDF (via vcpkg port OR vendored source with community CMake)
    |     +-- FreeType (bundled in MuPDF thirdparty/)
    |     +-- HarfBuzz (bundled in MuPDF thirdparty/)
    |     +-- libjpeg (bundled in MuPDF thirdparty/)
    |     +-- OpenJPEG (bundled in MuPDF thirdparty/)
    |     +-- zlib (bundled in MuPDF thirdparty/)
    |     +-- MuJS (bundled in MuPDF thirdparty/)
    |
    +-- QPDF (via vcpkg)
    +-- Qt 6.8 LTS (via Qt installer or vcpkg)
    +-- OpenSSL 3.x (via vcpkg)
    +-- Tesseract 5.x (via vcpkg)
    +-- Tantivy (Rust, built as shared library)
    +-- libvips (via vcpkg)
    +-- BouncyCastle (NuGet, for .NET signature module)
```

**Note on MuPDF's bundled dependencies:** MuPDF bundles its own copies of FreeType, HarfBuzz, libjpeg, OpenJPEG, and zlib in its `thirdparty/` directory. Since PDFE also uses FreeType and HarfBuzz directly (for the font matching system), there are two options:

1. **Use MuPDF's bundled versions:** Simpler build. MuPDF's FreeType/HarfBuzz are tested together.
2. **Use system/vcpkg versions for both:** Avoids duplicate libraries. Use `HAVE_SYSTEM_FREETYPE=yes` and similar flags when building MuPDF to use external versions.

**Recommendation:** Option 2 (shared system libraries) for release builds to reduce binary size. Option 1 (bundled) for initial development to avoid version conflicts.

---

## 8. Migration Impact on Existing Architecture

### 8.1 Components That Change

| Component | Previous | Revised | Migration Effort |
|-----------|----------|---------|-----------------|
| **Rendering Engine** | PDFium-based tile renderer | MuPDF-based tile renderer | Medium. API surface is different but concepts are the same. `fz_run_page()` replaces `FPDF_RenderPageBitmap()`. |
| **Document Model** | QPDF object graph | MuPDF `pdf_document` | Medium. All code that manipulates PDF objects must use MuPDF's API instead of QPDF's. |
| **Annotation Manager** | PDFium CRUD + QPDF persistence + custom appearance | MuPDF CRUD + automatic appearance | Low-Medium. Simpler API, less custom code. |
| **Form Manager** | PDFium render + QPDF create + QuickJS JS | MuPDF render + create + MuJS | Medium. Eliminates QuickJS dependency but requires learning MuPDF's form API. |
| **Redaction Engine** | Custom build on QPDF (not yet built) | MuPDF `pdf_redact_page()` + QPDF sanitization | **Low.** (The custom engine was not yet built, so this is net new code using MuPDF's API.) |
| **Save Manager** | QPDF always | MuPDF default + QPDF for special cases | Medium. New save path routing logic. |
| **Encryption Manager** | QPDF | MuPDF + QPDF fallback | Low. |
| **Text Extraction** | PDFium | MuPDF structured text API | Low. Better output. |
| **Search Integration** | PDFium text + Tantivy index | MuPDF text + Tantivy index | Trivial. Only the text source changes. |
| **OCR Pipeline** | Render via PDFium -> Tesseract | Render via MuPDF -> Tesseract | Trivial. Only the renderer call changes. |
| **Digital Signatures** | BouncyCastle + QPDF | BouncyCastle + MuPDF basics + QPDF byte-range | Low-Medium. MuPDF adds some capability. |

### 8.2 Components That Do NOT Change

- Qt 6.8 LTS GUI layer
- Application Controller layer (Command Bus, Undo/Redo, Event Dispatcher)
- Tile cache system (concept unchanged, just backed by MuPDF rendering)
- Plugin architecture
- OCR pipeline (Tesseract integration)
- Search indexing (Tantivy)
- Font matching system (FreeType + HarfBuzz + PANOSE)
- Image processing (libvips)
- Platform abstraction layer
- Build system (CMake)
- Package management (vcpkg)

### 8.3 Risk Assessment of Migration

| Risk | Severity | Probability | Mitigation |
|------|----------|-------------|------------|
| **MuPDF rendering regression** (some PDF renders differently) | Medium | Low | Build comprehensive visual regression test suite from PDF test corpus. Run against both PDFium and MuPDF during transition. |
| **MuPDF API stability** (breaking changes between versions) | Medium | Low-Medium | Pin to specific MuPDF release. C++ wrapper provides some insulation. MuPDF has improved API stability in recent years. |
| **MuJS insufficiency** (some PDF form JS fails) | Low | Low | Monitor with real-world PDF corpus. QuickJS available as fallback. |
| **XFA form complaints** (users with legacy XFA forms) | Medium | Medium | *Proposed:* document limitation clearly; provide "XFA not supported" dialog; prioritize XFA-to-AcroForm converter if demand materializes. **STATUS 2026-08-03 (F19, gap A5-69): NOT IMPLEMENTED — see the note at the end of this document.** |
| **Build system issues** (MuPDF build fails on some platform) | Low | Low | vcpkg port available. Community CMake available. MuPDF's VS solution handles Windows. |

---

## 9. Revised Technology Stack

| Layer | Previous | Revised | License | Change Rationale |
|-------|----------|---------|---------|-----------------|
| **Language** | C++20 | C++20 | -- | No change |
| **GUI Framework** | Qt 6.8 LTS | Qt 6.8 LTS | LGPL 3.0 | No change |
| **PDF Rendering + Editing** | PDFium (BSD) | **MuPDF (AGPL-3.0)** | AGPL-3.0 | Gains editing capabilities, rendering quality, simplifies architecture |
| **PDF Structure** | QPDF 12.x (Apache 2.0) | QPDF 12.x (Apache 2.0) | Apache 2.0 | Role narrowed to structural operations |
| **Font Rendering** | FreeType 2 + HarfBuzz | FreeType 2 + HarfBuzz | BSD-like + MIT | No change (MuPDF bundles these too) |
| **Font Discovery** | Fontconfig / DirectWrite | Fontconfig / DirectWrite | MIT / Proprietary | No change |
| **OCR** | Tesseract 5.x | Tesseract 5.x | Apache 2.0 | No change |
| **Text Search** | Tantivy (via C FFI) | Tantivy (via C FFI) | MIT | No change |
| **Encryption/Signatures** | OpenSSL 3.x + BouncyCastle C# | OpenSSL 3.x + BouncyCastle C# | Apache 2.0 + MIT | No change |
| **Form JS Sandbox** | QuickJS (MIT) | **MuJS (ISC, via MuPDF)** | ISC | Built into MuPDF, eliminates separate dependency |
| **Image Processing** | libvips + codecs | libvips + codecs | LGPL-2.1 + various | No change |
| **Build System** | CMake 3.25+ | CMake 3.25+ | BSD-3 | No change |
| **Package Manager** | vcpkg | vcpkg | MIT | No change |

### Libraries Removed

| Library | Reason |
|---------|--------|
| **PDFium** | Replaced by MuPDF |
| **QuickJS** | Replaced by MuPDF's built-in MuJS |

### Libraries Added

| Library | Reason |
|---------|--------|
| **MuPDF** | Primary rendering + editing engine |

### Net Library Count Change: -1 (removed 2, added 1)

---

## 10. ADR: Switch from PDFium to MuPDF

### ADR-002: Replace PDFium with MuPDF as Primary PDF Library

**Status:** Proposed
**Date:** 2026-02-05
**Decision Makers:** Architecture team

#### Context

PDFE is now licensed under AGPL-3.0, making AGPL-licensed libraries available for direct integration. The previous architecture used PDFium (BSD) for rendering and QPDF (Apache 2.0) for structural manipulation, with a synchronization layer between them. MuPDF (AGPL-3.0) offers rendering quality equal to PDFium plus significant editing capabilities that neither PDFium nor QPDF provide individually.

#### Decision

Replace PDFium with MuPDF as the primary PDF library. MuPDF becomes the document model owner, rendering engine, and editing engine. QPDF's role narrows to structural operations (incremental saves, linearization, signature byte-range management, metadata sanitization). QuickJS is removed in favor of MuPDF's built-in MuJS engine.

#### Consequences

**Positive:**
- Built-in redaction eliminates 3-6 months of custom development
- Built-in annotation appearance stream generation eliminates 2-4 weeks
- Built-in form JS engine eliminates QuickJS integration (4-6 weeks)
- Document model synchronization problem eliminated entirely
- Net reduction of one external dependency
- Simpler build system (MuPDF source builds are straightforward)
- Marginally better rendering quality

**Negative:**
- Loss of XFA dynamic form rendering (mitigated: XFA is deprecated in PDF 2.0)
- MuJS is ES5 only vs QuickJS ES2023 (mitigated: PDF form JS is ES5 in practice)
- No pre-built binaries equivalent to pdfium-binaries (mitigated: MuPDF builds are simpler)
- MuPDF's API may have breaking changes between major versions (mitigated: pin version, use C++ wrapper)

**Neutral:**
- AGPL-3.0 license applies to both MuPDF and PDFE (already the case for PDFE)
- Rendering performance is comparable between MuPDF and PDFium

#### Alternatives Considered

1. **Keep PDFium + QPDF:** Status quo. Loses the editing capabilities MuPDF provides. Retains XFA support. Requires building custom redaction, annotation, and form engines from scratch.

2. **MuPDF only (drop QPDF):** Tempting for simplicity, but MuPDF cannot linearize, has less reliable incremental saves for signature preservation, and lacks QPDF's content-preserving transformation guarantees.

3. **PDFium + MuPDF + QPDF (triple library):** Maximum capability coverage. But the complexity of managing three PDF libraries, each with its own document model, is not justified when MuPDF subsumes PDFium's role entirely.

---

## Appendix A: MuPDF C API Key Functions for PDFE

### Document Management
```c
fz_context *fz_new_context(...)           // Create runtime context
fz_document *fz_open_document(ctx, path)  // Open any supported format
pdf_document *pdf_open_document(ctx, path) // Open PDF specifically
pdf_save_document(ctx, doc, path, opts)   // Save PDF
```

### Rendering
```c
fz_page *fz_load_page(ctx, doc, page_num) // Load page
fz_run_page(ctx, page, dev, transform)    // Render to device
fz_new_draw_device(ctx, transform, pixmap) // Create pixel device
fz_stext_page *fz_new_stext_page_from_page(ctx, page, opts) // Structured text
```

### Annotations
```c
pdf_annot *pdf_create_annot(ctx, page, type) // Create annotation
pdf_set_annot_rect(ctx, annot, rect)         // Set position
pdf_set_annot_contents(ctx, annot, text)     // Set text content
pdf_set_annot_color(ctx, annot, n, color)    // Set color
pdf_set_annot_author(ctx, annot, author)     // Set author
pdf_delete_annot(ctx, page, annot)           // Delete
pdf_update_annot(ctx, annot)                 // Update appearance
```

### Redaction
```c
pdf_annot *pdf_create_annot(ctx, page, PDF_ANNOT_REDACT) // Create redact mark
pdf_set_annot_rect(ctx, annot, rect)                      // Set redaction area
pdf_redact_page(ctx, doc, page, opts)                     // Apply all redactions
// Options: PDF_REDACT_IMAGE_PIXELS, PDF_REDACT_IMAGE_REMOVE, etc.
```

### Forms
```c
pdf_widget *pdf_first_widget(ctx, page)     // Iterate widgets
pdf_widget *pdf_next_widget(ctx, widget)    // Next widget
pdf_widget_type(ctx, widget)                // Get type
pdf_set_text_field_value(ctx, widget, val)  // Set text value
pdf_choice_widget_set_value(ctx, widget, v) // Set choice value
pdf_create_signature_widget(ctx, page, ...)  // Create signature field
```

### Encryption
```c
pdf_authenticate_password(ctx, doc, password)  // Decrypt
// Encryption on save via pdf_write_options
```

---

## Appendix B: Feature Parity Checklist

This checklist maps every capability in the previous PDFium-based architecture to its equivalent in the MuPDF-based architecture:

| Previous PDFium API | MuPDF Equivalent | Status |
|--------------------|-----------------|--------|
| `FPDF_LoadDocument()` | `pdf_open_document()` | Direct replacement |
| `FPDF_RenderPageBitmap()` | `fz_run_page()` + `fz_new_draw_device()` | Direct replacement |
| `FPDFText_LoadPage()` | `fz_new_stext_page_from_page()` | Direct replacement, richer output |
| `FPDFText_GetText()` | `fz_stext_page` traversal | Direct replacement |
| `FPDFText_FindStart()` / `FPDFText_FindNext()` | `fz_search_page()` | Direct replacement |
| `FPDFPage_CreateAnnot()` | `pdf_create_annot()` | Direct replacement |
| `FPDFAnnot_SetRect()` | `pdf_set_annot_rect()` | Direct replacement |
| `FPDFAnnot_SetColor()` | `pdf_set_annot_color()` | Direct replacement |
| `FPDFAnnot_SetStringValue()` | `pdf_set_annot_contents()` | Direct replacement |
| `FPDF_SetOCGVisibility()` | `pdf_set_layer_config_as_default()` | Equivalent |
| `FORM_OnLButtonDown()` etc. | MuPDF widget event handling | Conceptually similar |
| `FORM_GetFocusedText()` | `pdf_text_widget_value()` | Direct replacement |
| `FPDFPage_GenerateContent()` | `pdf_update_page()` | Equivalent |
| XFA rendering | **Not available** | Gap -- see mitigation. **The mitigation was never implemented** (F19, gap A5-69) -- see the note below the table. |
| V8 JavaScript | MuJS (ES5.1) | Functional replacement |

---

## Sources

### MuPDF
- [MuPDF Official Site](https://mupdf.com/)
- [MuPDF Core Features](https://mupdf.com/core)
- [MuPDF Documentation](https://mupdf.com/docs)
- [MuPDF Wikipedia](https://en.wikipedia.org/wiki/MuPDF)
- [MuPDF C++ API Blog](https://artifex.com/blog/an-improved-mupdf-api-using-c-plus-plus)
- [MuPDF Story API (PyMuPDF)](https://pymupdf.readthedocs.io/en/latest/story-class.html)
- [MuPDF Redaction (PyMuPDF)](https://pymupdf.readthedocs.io/en/latest/page.html)
- [MuPDF Form API Header](https://github.com/ArtifexSoftware/mupdf/blob/master/include/mupdf/pdf/form.h)
- [MuPDF Annotation Code](https://github.com/ArtifexSoftware/mupdf/blob/master/platform/gl/gl-annotate.c)
- [MuPDF Release History](https://mupdf.com/releases/history)
- [MuPDF CMake Community Build](https://github.com/FabriceSalvaire/mupdf-cmake)
- [MuPDF Install Guide](https://mupdf.readthedocs.io/en/latest/guide/install.html)
- [Choosing Between Ghostscript and MuPDF](https://artifex.com/blog/choosing-between-ghostscript-and-mupdf)
- [PyMuPDF Incremental Save Discussion](https://github.com/pymupdf/PyMuPDF/discussions/3640)
- [PyMuPDF Signature Discussion](https://github.com/pymupdf/PyMuPDF/discussions/2544)
- [MuPDF Encryption Source](https://github.com/ArtifexSoftware/mupdf/blob/master/source/pdf/pdf-crypt.c)

### MuJS
- [MuJS Official Site](https://mujs.com/)
- [MuPDF JavaScript Guide](https://mupdf.readthedocs.io/en/latest/guide/using-with-javascript.html)

### PDFium
- [PDFium Getting Started](https://pdfium.googlesource.com/pdfium/+/HEAD/docs/getting-started.md)
- [pdfium-binaries](https://github.com/bblanchon/pdfium-binaries)
- [PDFium in 2025 (Nutrient)](https://www.nutrient.io/blog/why-pdfium-is-a-trusted-platform-for-pdf-rendering/)

### Benchmarks & Comparisons
- [PDF Rasterization Benchmark](https://github.com/nathanstitt/dc-pdf-raster-test)
- [PDF Association: Survey of Open Source Solutions](https://pdfa.org/wp-content/uploads/2021/06/Survey-of-OpenSource-Solutions.pdf)
- [Hyland: PDF Rendering Engine Comparison](https://connect.hyland.com/t5/alfresco-blog/pdf-rendering-engine-performance-and-fidelity-comparison/ba-p/125428)

### QPDF
- [QPDF Linearization Docs](https://qpdf.readthedocs.io/en/stable/linearization.html)
- [QPDF Encryption Docs](https://qpdf.readthedocs.io/en/stable/encryption.html)

### Licensing
- [Artifex Licensing](https://artifex.com/licensing)
- [AGPL-3.0 License Text](https://www.gnu.org/licenses/agpl-3.0.html)
- [AGPL Wikipedia](https://en.wikipedia.org/wiki/GNU_Affero_General_Public_License)
- [PyMuPDF License Clarification](https://github.com/pymupdf/pymupdf/issues/4504)

### Other Libraries
- [Poppler Official Site](https://poppler.freedesktop.org/)
- [Ghostscript Official Site](https://www.ghostscript.com/)
- [iText Official Site](https://itextpdf.com/)
- [iText PAdES API](https://itextpdf.com/blog/itext-news-technical-notes/itext-suite-803-advanced-pades-api-two-step-signing)

---

## Correction — the XFA mitigation was never implemented (F19, gap A5-69)

*Re-verified against the working tree 2026-08-03, meta-audit follow-up phase-19.*

This document records "XFA rendering — **Not available** — Gap -- see
mitigation", and the mitigation it points at is stated twice:

- *"For dynamic-only XFA, display a 'This form requires Adobe Acrobat' message
  (same as most non-Adobe PDF viewers)."*
- *"Document limitation clearly. Provide 'XFA not supported' dialog."*

**Neither exists.** `grep -rn -i "xfa" src/ui/ src/forms/` returns **nothing**,
and every XFA hit anywhere in `src/` is a comment about garbage-collection
cost during save (`src/core/Document.cpp:101-103,1617,1865`,
`src/core/Document.h:428,455,465-466`). There is no user-facing XFA string, no
dialog, no banner, no status-bar warning, no log line.

The observable behaviour today is that
`tests/test_compat_advanced.cpp:49-73`
(`CompatAdvancedTest.XfaOnlyPdfRendersWithoutCrash`) builds a PDF with
`/XFA 4 0 R` and no `/Fields`, and asserts only that it renders **without
crashing**. A user opening a dynamic-XFA form gets a blank or wrong render and
**no notification of any kind**.

The mitigation column of a risk table reads as the standing answer to the
risk. Here it was a recommendation that was accepted and never landed, and
nothing re-checked it for the intervening period. It is now labelled at both
sites. **The gap is open**: F19 is a documentation-truth phase and did not
build the dialog. `F19DocClaims.XfaMitigationIsLabelledUnimplementedWhileItIs`
re-derives the absence from `src/` and fails if a real XFA user-facing string
appears without this note being updated — so the note cannot outlive the gap
either.
