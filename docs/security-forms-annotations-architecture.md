# Security, Forms, and Annotations Architecture for PDFE

**Date:** 2026-02-05
**Status:** v1 -- Partially Superseded (see note below)
**Author:** Security Architect

> **v2 Note:** This document was written before the AGPL licensing decision. It references
> PDFium as the renderer and QuickJS as the JS engine. In the v2 architecture:
> - **MuPDF replaces PDFium** -- MuPDF provides built-in redaction (`pdf_redact_page()`),
>   annotation CRUD with auto-generated appearance streams, and form widget APIs
> - **MuJS replaces QuickJS** -- MuJS is bundled with MuPDF and already implements a
>   subset of the Acrobat JavaScript API
> - **Encryption, digital signatures, and QPDF structural operations** remain as described here
>
> See `docs/project-analysis-consolidated.md` (v2) and `docs/agpl-library-architecture-reevaluation.md`
> for the current architecture.

---

## Table of Contents

1. [Executive Summary](#executive-summary)
2. [Part 1: Security Features](#part-1-security-features)
   - [Digital Signatures](#1-digital-signatures)
   - [Real Redaction](#2-real-redaction)
   - [Password Protection and Encryption](#3-password-protection-and-encryption)
   - [Permission Controls](#4-permission-controls-print--copy-prevention)
   - [Metadata Stripping](#5-metadata-stripping)
3. [Part 2: Form Features](#part-2-form-features)
   - [Form Creation and Editing](#1-form-creation-and-editing)
   - [Form Automation and JavaScript](#2-form-automation-and-javascript-sandboxing)
   - [Form Data Handling](#3-form-data-handling-fdfxfdf)
4. [Part 3: Annotation Features](#part-3-annotation-features)
   - [Annotation Types and Operations](#annotation-types-and-operations)
   - [Comment Threads and Replies](#comment-threads-and-replies)
5. [Library Evaluation Matrix](#library-evaluation-matrix)
6. [Recommended Architecture](#recommended-architecture)
7. [Implementation Roadmap](#implementation-roadmap)

---

## Executive Summary

This document evaluates open-source libraries and defines architecture for three critical capability areas of the PDFE editor: security (signatures, encryption, redaction), interactive forms (creation, automation, data exchange), and annotations (markup, comments, threads). The evaluation covers completeness, security robustness, cross-platform support, licensing, maintenance status, and performance.

**Primary Recommendation:** A layered architecture using PDFium as the rendering and interaction engine, QPDF for low-level PDF manipulation and encryption, OpenSSL for cryptographic primitives, BouncyCastle (C#) for high-level signature operations, and QuickJS for sandboxed form JavaScript execution.

**Key Findings:**

- No single open-source library covers all requirements. A composable architecture is necessary.
- "Real" redaction is poorly supported across the ecosystem and requires custom implementation atop low-level PDF object manipulation.
- Digital signature support (especially PAdES B-LTA) is the most complex area; iText (AGPL) is the most complete but the license is restrictive.
- PDFium provides the best cross-platform form and annotation rendering, but creating/modifying annotations programmatically requires careful API usage.
- JavaScript in PDF forms is a significant security surface; QuickJS with strict sandboxing is the recommended approach.

---

## Part 1: Security Features

### 1. Digital Signatures

#### 1.1 Standards to Support

| Standard | Description | Priority |
|----------|-------------|----------|
| PKCS#7 / CMS | Base signature container format (RFC 5652) | Required |
| PAdES B-B | Basic signature with signing certificate | Required |
| PAdES B-T | Adds trusted timestamp | Required |
| PAdES B-LT | Adds long-term validation data (OCSP, CRLs, cert chains) | Required |
| PAdES B-LTA | Adds archival timestamps for permanent validity | Recommended |
| PDF 2.0 Signatures | ISO 32000-2 compliant signature handling | Recommended |

**PAdES Levels Explained:**

- **B-B (Basic):** Contains the electronic signature and signing certificate. Valid only as long as the certificate is valid.
- **B-T (Timestamped):** Adds a trusted timestamp from a TSA (Timestamp Authority), proving the signature existed at a specific time.
- **B-LT (Long-Term):** Embeds validation data (OCSP responses, CRLs, full certificate chain) so the signature can be validated even after the signing CA becomes unavailable.
- **B-LTA (Archival):** Adds periodic document timestamps with current algorithms, ensuring the signature remains verifiable indefinitely even as algorithms age.

#### 1.2 Library Evaluation for Digital Signatures

##### OpenSSL 3.x

- **Version:** 3.5.x (latest stable as of early 2026)
- **License:** Apache License 2.0
- **Role:** Low-level cryptographic primitives (RSA, ECDSA, hash functions, AES)
- **Strengths:**
  - Industry standard cryptographic library
  - Excellent cross-platform support (Windows, macOS, Linux)
  - Provider architecture in OpenSSL 3 allows pluggable crypto backends
  - Supports PKCS#11 for hardware security module integration
  - Post-quantum algorithm research ongoing (via OQS provider)
  - Apache 2.0 license is permissive and compatible with most open-source licenses
- **Weaknesses:**
  - No PDF-specific signature logic; you must implement the PDF signature container, byte-range handling, and PAdES profile yourself
  - Complex API surface; easy to misuse
  - Does not handle certificate chain building or OCSP/CRL fetching natively (requires manual orchestration)
- **Verdict:** Use as the cryptographic foundation. Not sufficient alone for PDF signatures.

##### BoringSSL

- **License:** ISC-style (permissive)
- **Role:** Alternative to OpenSSL
- **Strengths:**
  - Security-first design (used by Google in Chrome, Android)
  - Smaller binary footprint than OpenSSL
  - Faster to adopt new TLS features
- **Weaknesses:**
  - No API/ABI stability guarantees -- Google explicitly states this
  - Poorly documented for external use
  - Missing features that OpenSSL has (PKCS#11 support, some legacy algorithms)
  - Not designed for third-party consumption
- **Verdict:** Not recommended for this project. The lack of API stability is a disqualifier for a desktop application with release cycles.

##### LibreSSL

- **Version:** 4.1.x
- **License:** ISC, OpenSSL, SSLeay (permissive)
- **Role:** Alternative to OpenSSL
- **Strengths:**
  - Security-focused fork of OpenSSL (born from Heartbleed response)
  - Removed insecure/legacy code
  - Maintained by OpenBSD team
- **Weaknesses:**
  - Slower feature adoption than OpenSSL (late QUIC support, no post-quantum yet)
  - Primarily targets BSD; Linux/Windows packaging is sparse
  - Smaller community and fewer integrations
- **Verdict:** Not recommended. OpenSSL 3.x with its provider architecture and Apache 2.0 license is a better fit for cross-platform desktop software.

##### BouncyCastle (C#)

- **Version:** 2.5.x (Portable.BouncyCastle NuGet)
- **License:** MIT
- **Role:** High-level CMS/PKCS#7 signature creation and verification, certificate handling
- **Strengths:**
  - Pure managed code -- no native dependencies
  - Complete CMS/PKCS#7 implementation
  - Certificate chain building, OCSP, CRL handling
  - Well-documented for PDF signing workflows (commonly used with iText)
  - MIT license is fully permissive
  - Works identically on Windows, macOS (via .NET), Linux (via .NET)
- **Weaknesses:**
  - No PDF-specific logic (byte-range digesting, PAdES profile, signature field embedding)
  - Performance is slower than native OpenSSL for bulk crypto operations
  - Must be combined with a PDF library for the actual signing workflow
- **Verdict:** Strongly recommended for CMS container creation and certificate operations. Pair with QPDF or custom code for the PDF-level integration.

##### iText (AGPL)

- **Version:** 9.5.x (iText Core)
- **License:** AGPL-3.0 (commercial license available)
- **Strengths:**
  - Most complete open-source PDF signature implementation available
  - Full PAdES B-B through B-LTA support
  - High-level API for both creation and validation
  - Post-quantum signature proof-of-concept (ML-DSA, SLH-DSA) in v9.5
  - Two-step signing support (for HSM/cloud signing)
  - Active development with frequent releases
  - Comprehensive documentation and examples
- **Weaknesses:**
  - **AGPL license is the primary concern.** Any application that uses iText and is distributed must also be AGPL, or must purchase a commercial license. For an open-source editor under a permissive or even GPL license, AGPL is viral and incompatible unless the entire project is also AGPL.
  - Java/.NET only (no C/C++ native)
  - Heavy dependency footprint
- **Verdict:** The gold standard for PDF signatures, but the AGPL license makes it unsuitable unless PDFE itself is AGPL or a commercial license is purchased. Use as a reference implementation for understanding correct PAdES behavior.

##### Apache PDFBox

- **Version:** 3.0.5 / 2.0.35
- **License:** Apache License 2.0
- **Strengths:**
  - Full digital signature creation and verification
  - Visible and invisible signature support
  - Timestamp validation
  - Apache 2.0 license is permissive
  - Active development on both 2.x and 3.x branches
  - Good example code for signature workflows
- **Weaknesses:**
  - Java only -- cannot be used natively in a C/C++/C# desktop application without JNI or a subprocess
  - PAdES support is less mature than iText
  - No built-in PAdES B-LTA
- **Verdict:** Excellent reference implementation. Could be used via a Java subprocess or JNI bridge, but adds significant complexity. Better to implement the same logic natively using BouncyCastle + QPDF.

#### 1.3 Recommended Signature Architecture

```
+----------------------------------------------------------+
|                    PDFE Application                       |
+----------------------------------------------------------+
|           Signature Manager (Custom C# Code)             |
|  - PDF byte-range calculation                             |
|  - Signature field creation (via QPDF object model)      |
|  - PAdES profile construction                             |
|  - Signature appearance rendering                         |
+---------------------------+------------------------------+
                            |
              +-------------+-------------+
              |                           |
   +----------v----------+    +----------v----------+
   |   BouncyCastle C#   |    |      QPDF (C++)     |
   |                     |    |                      |
   | - CMS/PKCS#7 create |    | - PDF object model   |
   | - Certificate chain |    | - Byte-range mgmt    |
   | - OCSP/CRL fetch    |    | - Incremental save   |
   | - Timestamp request |    | - Linearization      |
   +----------+----------+    +----------------------+
              |
   +----------v----------+
   |    OpenSSL 3.x       |
   | (via BouncyCastle    |
   |  or native interop)  |
   | - RSA/ECDSA ops      |
   | - Hash computation   |
   | - PKCS#11 (HSM)      |
   +----------------------+
```

**Signing Workflow:**

1. Create or locate the signature form field in the PDF (QPDF object manipulation)
2. Calculate the byte range to be signed (everything except the signature value placeholder)
3. Compute the digest of the byte range (SHA-256 or SHA-384)
4. Build the CMS signed-data structure using BouncyCastle:
   - Embed signing certificate
   - Include timestamp token from TSA (for PAdES B-T+)
   - Include OCSP responses and CRLs (for PAdES B-LT+)
5. Encode the CMS container as hex and write it into the signature placeholder
6. For "save as read-only after signing": set the document permissions via a DocMDP transform

**Verification Workflow:**

1. Parse the signature field and extract the CMS container (QPDF)
2. Extract the byte range and compute the digest
3. Verify the CMS signature using BouncyCastle:
   - Validate the certificate chain
   - Check revocation status (OCSP/CRL)
   - Verify the timestamp
4. Check document integrity (ensure no unauthorized modifications since signing)

**Making Signed Documents Read-Only:**

PDF supports a "DocMDP" (Modification Detection and Prevention) transform dictionary that can be attached to a signature. This specifies what modifications are allowed after signing:

- **Level 1 (No changes):** No modifications at all
- **Level 2 (Form fill + sign):** Only form filling and additional signatures
- **Level 3 (Form fill + sign + annotate):** Form filling, signing, and annotations

For "read-only once signed," use DocMDP Level 1 or Level 2 depending on whether additional signatures are expected.

---

### 2. Real Redaction

#### 2.1 The Problem with Naive Redaction

A 2023 academic study found that **65% of supposedly redacted PDFs from security agencies still exposed hidden information**. Common failures include:

- **Black rectangle overlay:** The text remains in the content stream; the rectangle is just a drawing on top. Anyone can remove the rectangle or copy-paste the text.
- **Text color set to white/background:** The text is still there, selectable, searchable.
- **Image overlay without content removal:** The underlying image data remains in the file.
- **OCR text layer retained:** Scanned documents often have an invisible text layer for searchability. Redacting the visible scan without removing the OCR text defeats the purpose.
- **Metadata not stripped:** Author, revision history, comments, and embedded file attachments can leak information.

#### 2.2 Correct Redaction Process

Real redaction must **remove content from the PDF object graph**, not just obscure it visually. The process has five stages:

**Stage 1: Mark**
- User marks regions for redaction (rectangular areas on pages)
- Redaction marks are stored as PDF annotation objects (Redact annotation type, PDF 2.0)
- At this stage, the content is still present -- marks are just proposals

**Stage 2: Apply**
- Parse the page content stream(s)
- Identify all text operators (Tj, TJ, ', ", etc.) whose glyphs fall within the redaction rectangle
- Remove those operators from the content stream
- Identify all image XObjects whose bounding boxes intersect the redaction rectangle:
  - If fully contained: remove the image XObject entirely
  - If partially overlapping: re-render the image with the redacted region zeroed out (filled with the redaction color), then replace the original
- Remove any vector drawing operators (path construction/painting) within the redaction area
- Replace the redacted area with a filled rectangle (typically black) in the content stream
- Remove the redaction annotations themselves

**Stage 3: Clean Object Graph**
- Remove any orphaned objects (images, fonts, etc.) that were only referenced by redacted content
- Remove any associated form fields or annotations in the redacted area
- Rebuild the cross-reference table

**Stage 4: Sanitize Metadata**
- Strip document-level metadata (see Section 5 below)
- Remove revision history if using incremental saves (re-save as a single-revision PDF)
- Remove any embedded files, JavaScript, or other potentially sensitive objects

**Stage 5: Verify**
- Re-parse the output PDF
- Search for any remnants of the redacted text (full-text search of all strings in the object graph)
- Verify the redacted image regions are truly blank
- Validate the PDF structure

#### 2.3 Library Evaluation for Redaction

| Library | Content Stream Parsing | Object Removal | Image Manipulation | Cross-Ref Rebuild | Verdict |
|---------|----------------------|----------------|-------------------|-------------------|---------|
| **QPDF** | Partial (token-level, not semantic) | Yes (excellent) | No | Yes (excellent) | Foundation layer |
| **PDFium** | Yes (rendering-level) | No (read-focused) | No | No | Rendering verification only |
| **MuPDF** | Yes (has redaction API) | Yes | Partial | Yes | Best built-in redaction |
| **Poppler** | Partial | Partial | No | Partial | Not recommended |
| **pikepdf** | Yes (via QPDF) | Yes (via QPDF) | No | Yes (via QPDF) | Python wrapper; good for prototyping |
| **Apache PDFBox** | Yes | Yes | Yes | Yes | Java only |

##### MuPDF Redaction Capability

MuPDF (v1.26.x) includes a built-in redaction API (`pdf_redact_page`) that:
- Processes redaction annotations
- Removes text from content streams within the redaction area
- Handles image intersection
- Removes the redaction annotations after application

**However:** MuPDF is licensed under **AGPL-3.0**. This has the same license incompatibility issue as iText. A commercial license from Artifex is required for non-AGPL use.

##### Recommended Redaction Architecture

Given that MuPDF's AGPL license may be prohibitive, the recommended approach is to build custom redaction logic on top of QPDF:

```
+----------------------------------------------------------+
|                   Redaction Engine                         |
+----------------------------------------------------------+
|  1. Parse content streams (QPDF QPDFObjectHandle)        |
|  2. Build glyph position map (font metrics + text ops)   |
|  3. Identify intersecting content                         |
|  4. Rewrite content streams without redacted operators    |
|  5. Handle image intersection (render, mask, replace)     |
|  6. Remove orphaned objects                               |
|  7. Sanitize metadata                                     |
|  8. Re-save as single-revision PDF                        |
+----------------------------------------------------------+
|                      QPDF (C++)                           |
|  - Content stream access and rewriting                    |
|  - Object graph traversal and manipulation                |
|  - Cross-reference table rebuild                          |
|  - Single-revision output (--linearize or --qdf)          |
+----------------------------------------------------------+
```

**Complexity Warning:** Building correct content stream parsing that accurately maps text operators to glyph positions is one of the hardest problems in PDF processing. It requires:
- Font metric resolution (Type1, TrueType, CIDFont)
- Text matrix and CTM (Current Transformation Matrix) tracking
- Handling of all text operators including array-form (TJ with kerning adjustments)
- Character encoding resolution (especially for CIDFonts with CMaps)

This is a multi-month engineering effort. Consider MuPDF's commercial license as an alternative if budget allows.

**Alternative Approach: Rasterize-and-Reconstruct**

A simpler but lossy approach:
1. Render the page to a high-resolution raster image (PDFium)
2. Paint over the redaction regions in the raster image
3. Replace the page with the rasterized version
4. This guarantees no text/vector data survives, but loses editability and increases file size

This can be offered as a "secure redaction" option alongside content-stream-level redaction.

---

### 3. Password Protection and Encryption

#### 3.1 PDF Encryption Standards

| Version | Algorithm | Key Length | PDF Version | Status |
|---------|-----------|------------|-------------|--------|
| V1 R2 | RC4 | 40-bit | PDF 1.1+ | **Insecure. Do not use.** |
| V2 R3 | RC4 | 128-bit | PDF 1.4+ | **Deprecated in PDF 2.0.** Known weaknesses. |
| V4 R4 | AES-128 | 128-bit | PDF 1.6+ | **Deprecated in PDF 2.0.** Still widely used. |
| V5 R5 | AES-256 | 256-bit | PDF 1.7 ext3 (Adobe) | Adobe extension, not ISO standard. |
| V5 R6 | AES-256 | 256-bit | PDF 2.0 | **Current standard. Use this.** |

**Recommendation:** Support reading all formats (for compatibility) but only create V5 R6 (AES-256) encrypted files. Display a warning when opening RC4-encrypted files about their insecurity.

#### 3.2 User vs. Owner Passwords

The PDF specification defines two password types:

- **User Password:** Required to open and view the document. Without it, the PDF cannot be decrypted.
- **Owner Password:** Required to change permissions (print, copy, modify). The document can still be viewed with just the user password, but restricted by permissions.

**Critical Security Note for AES-256 (V5 R6):** Unlike earlier schemes, the user password and owner password independently encrypt the main file encryption key. There is no way to derive the user password from the owner password. An empty owner password with AES-256 means anyone can change permissions, which is why QPDF requires `--allow-insecure` for this case.

#### 3.3 Library Evaluation for Encryption

| Library | RC4 Read | AES-128 Read | AES-256 Read | AES-256 Write | Permission Setting | Verdict |
|---------|----------|-------------|-------------|--------------|-------------------|---------|
| **QPDF** | Yes | Yes | Yes | Yes | Yes (full) | **Primary choice** |
| **PDFium** | Yes | Yes | Yes | No (read-only lib) | No | Read only |
| **pikepdf** | Yes | Yes | Yes | Yes (via QPDF) | Yes | Python; prototyping |
| **MuPDF** | Yes | Yes | Yes | Yes | Yes | AGPL |
| **OpenSSL** | N/A (raw crypto only) | N/A | N/A | N/A | N/A | Underlying crypto |

**QPDF is the clear winner** for encryption handling:
- Supports all PDF encryption versions
- Handles the full password-to-key derivation (including the complex V5 R6 scheme)
- Can set granular permissions
- Apache 2.0 license
- C++ with excellent cross-platform support
- Version 12.3.x is current and actively maintained

#### 3.4 Recommended Encryption Architecture

```
+----------------------------------------------------------+
|              Encryption Manager (C# / C++)                |
+----------------------------------------------------------+
|  Public API:                                              |
|  - SetUserPassword(string password)                       |
|  - SetOwnerPassword(string password)                      |
|  - SetPermissions(PdfPermissions flags)                   |
|  - Encrypt(EncryptionLevel.AES256)                        |
|  - Decrypt(string password)                               |
|  - GetEncryptionInfo() -> EncryptionMetadata              |
+----------------------------------------------------------+
|                      QPDF (C++ / P/Invoke)                |
|  - QPDFWriter::setR6EncryptionParameters(...)             |
|  - Handles key derivation, stream encryption              |
|  - Permission flag encoding                               |
+----------------------------------------------------------+
```

---

### 4. Permission Controls (Print / Copy Prevention)

#### 4.1 PDF Permission Flags

The PDF specification defines permission flags as a 32-bit integer in the encryption dictionary. Key flags:

| Bit | Permission | Description |
|-----|-----------|-------------|
| 3 | Print (low quality) | Allow printing at low resolution |
| 4 | Modify contents | Allow document modification |
| 5 | Copy/extract | Allow text and graphics extraction |
| 6 | Add/modify annotations | Allow annotation and form field changes |
| 9 | Fill forms | Allow form field filling |
| 10 | Extract for accessibility | Allow text extraction for accessibility |
| 11 | Assemble | Allow page insertion, rotation, deletion |
| 12 | Print (high quality) | Allow high-resolution printing |

#### 4.2 Implementation Notes

**Print Prevention:** Clear bits 3 and 12. Note that this is enforced by the PDF viewer, not cryptographically. A non-compliant viewer can ignore this. The owner password is needed to change these flags.

**Copy Prevention:** Clear bit 5. Same caveat -- this is a viewer-enforced restriction. The actual content is still decryptable with the user password; the viewer simply refuses to expose copy functionality.

**Security Reality:** PDF permissions are a "polite request" to conforming viewers. They do not provide real DRM. Any tool that can decrypt the PDF (using the user password) has access to all content. However, they are standard practice and expected by users.

**Implementation via QPDF:**

```cpp
// QPDF permission setting (pseudocode)
QPDFWriter w(qpdf, output_filename);
w.setR6EncryptionParameters(
    user_password,
    owner_password,
    /* allow_accessibility */ true,
    /* allow_extract */ false,       // Copy prevention
    /* allow_assemble */ false,
    /* allow_annotate_and_form */ true,
    /* allow_form_filling */ true,
    /* allow_modify_other */ false,
    /* allow_print */ qpdf_r3p_none, // Print prevention
    /* encrypt_metadata */ true
);
```

---

### 5. Metadata Stripping

#### 5.1 Metadata Locations in PDF

PDFs store metadata in multiple locations:

1. **Document Information Dictionary** (`/Info`): Title, Author, Subject, Keywords, Creator, Producer, CreationDate, ModDate
2. **XMP Metadata Stream** (`/Metadata`): XML-based metadata (Dublin Core, XMP, PDF/A schemas). Can contain extensive author info, edit history, software versions.
3. **Incremental Save History:** Each incremental save appends to the file, preserving all previous versions. Rolling back reveals prior content.
4. **Embedded Files / Attachments:** Can contain any file type
5. **JavaScript:** Can contain identifying information
6. **Comments / Annotations:** May contain author names, timestamps
7. **Form Field Values:** May contain data from previous fill sessions
8. **Named Destinations / Bookmarks:** Can contain internal names
9. **Structure Tree (Tagged PDF):** Can contain alt text, language, author info

#### 5.2 Sanitization Strategy

```
+----------------------------------------------------------+
|              Metadata Sanitizer                           |
+----------------------------------------------------------+
|  Level 1 (Basic):                                        |
|  - Clear /Info dictionary (Author, Creator, Producer)    |
|  - Remove /Metadata XMP stream                           |
|  - Set generic Producer ("PDFE")                         |
|                                                          |
|  Level 2 (Standard - recommended for redaction):         |
|  - All of Level 1                                        |
|  - Remove incremental save history (re-linearize)        |
|  - Strip embedded files                                  |
|  - Remove JavaScript                                     |
|  - Strip annotation author fields                        |
|  - Remove form field default values                      |
|                                                          |
|  Level 3 (Paranoid):                                     |
|  - All of Level 2                                        |
|  - Remove all bookmarks/named destinations               |
|  - Strip structure tree author info                      |
|  - Remove thumbnail images                               |
|  - Remove document ID                                    |
|  - Re-number all objects sequentially                    |
|  - Regenerate document ID                                |
+----------------------------------------------------------+
|                      QPDF (C++)                           |
|  - Object traversal and removal                          |
|  - Linearization (removes incremental history)           |
|  - Object renumbering                                    |
+----------------------------------------------------------+
```

**Implementation via QPDF:** QPDF's object model allows direct manipulation of the document catalog, info dictionary, and any stream object. Linearization (`--linearize`) forces a complete rewrite that eliminates incremental save history.

---

## Part 2: Form Features

### 1. Form Creation and Editing

#### 1.1 PDF Form Types

| Form Type | Description | Support Priority |
|-----------|-------------|-----------------|
| **AcroForm** | Classic PDF form fields (text, checkbox, radio, dropdown, signature) | Required |
| **XFA** | XML Forms Architecture (deprecated in PDF 2.0, used in legacy Adobe forms) | Read-only support |
| **Static XFA** | XFA with fixed layout | Read-only rendering |
| **Dynamic XFA** | XFA with dynamic layout (data-driven) | Best-effort rendering |

**Recommendation:** Focus on full AcroForm support. XFA is deprecated in PDF 2.0 and Adobe has been moving away from it. Provide read-only rendering of XFA forms for compatibility, but do not implement XFA form creation.

#### 1.2 AcroForm Field Types

| Field Type | Widget Annotation | Description | Complexity |
|-----------|-------------------|-------------|------------|
| Text Field | `/Tx` | Single-line or multi-line text input | Medium |
| Checkbox | `/Btn` (non-pushbutton, non-radio) | Boolean toggle | Low |
| Radio Button | `/Btn` (radio flag set) | Mutual exclusion group | Medium |
| Dropdown (Combo Box) | `/Ch` (combo flag set) | Select from list or type custom | Medium |
| List Box | `/Ch` (no combo flag) | Select from scrollable list | Medium |
| Push Button | `/Btn` (pushbutton flag) | Clickable button (triggers action) | Low |
| Signature Field | `/Sig` | Digital signature placeholder | High |

#### 1.3 Library Evaluation for Forms

##### PDFium

- **Version:** Chromium HEAD (no independent versioning)
- **License:** BSD 3-Clause
- **Form Capabilities:**
  - Full AcroForm rendering (text fields, checkboxes, radio buttons, dropdowns)
  - Form field interaction (focus, editing, selection)
  - JavaScript execution for form calculations (has its own JS interpreter)
  - XFA rendering (static and dynamic) -- this is unique among open-source libraries
  - Form field value get/set via `FORM_GetFocusedText`, `FORM_SetIndexSelected`, etc.
  - Annotation creation via `FPDFPage_CreateAnnot()`
- **Weaknesses:**
  - Creating new form fields programmatically is limited; the API is focused on rendering and interaction, not authoring
  - Build system is complex (GN/Ninja, Chromium-style)
  - No stable release versioning
  - Documentation is sparse; you read the header files
- **Verdict:** Best for form rendering and interaction. Needs supplementation for form field creation.

##### QPDF

- **Version:** 12.3.x
- **License:** Apache 2.0
- **Form Capabilities:**
  - `QPDFAcroFormDocumentHelper`: Enumerate form fields, map fields to annotations
  - `QPDFFormFieldObjectHelper`: Read/write field values, properties
  - `QPDFAnnotationObjectHelper`: Annotation manipulation
  - Can create form fields by constructing the appropriate PDF objects
  - Example code: `pdf-set-form-values.cc` in the examples directory
- **Weaknesses:**
  - Low-level API: you must understand the PDF specification to construct form fields correctly
  - No widget appearance stream generation (you must create the visual representation yourself)
  - No JavaScript support
- **Verdict:** Excellent for form field creation and manipulation at the object level. Requires custom appearance stream generation.

##### pdf-lib (JavaScript)

- **Version:** 1.17.1
- **License:** MIT
- **Form Capabilities:**
  - Create text fields, checkboxes, radio buttons, dropdowns, option lists
  - Fill existing form fields
  - Flatten form fields (convert to static content)
  - Read form field values
- **Weaknesses:**
  - JavaScript only (browser/Node.js) -- not suitable as a native library
  - Limited annotation support
  - No signature field creation
  - No XFA support
  - Project maintenance has slowed (last release was 2021, though still functional)
- **Verdict:** Good reference for API design. Not suitable as a native dependency. Could be used in an Electron-based variant.

##### Apache PDFBox

- **Version:** 3.0.5
- **License:** Apache 2.0
- **Form Capabilities:**
  - Full AcroForm support (all field types)
  - Form field creation with appearance generation
  - Form flattening
  - Digital signature field handling
  - FDF/XFDF import/export
- **Weaknesses:**
  - Java only
  - No XFA support (by design, since XFA is deprecated)
- **Verdict:** Most complete form implementation among Apache-licensed libraries, but Java-only limits direct use.

##### MuPDF

- **Version:** 1.26.x
- **License:** AGPL-3.0 (commercial available)
- **Form Capabilities:**
  - AcroForm rendering and interaction
  - Form field value manipulation via `pdf_set_text_field_value`, etc.
  - JavaScript support (limited, via MuJS)
  - Annotation creation and editing
- **Weaknesses:**
  - AGPL license
  - Form field creation API is limited
- **Verdict:** Capable but AGPL-encumbered.

#### 1.4 Recommended Form Architecture

```
+----------------------------------------------------------+
|                    Form Manager                           |
+----------------------------------------------------------+
|  Creation Layer (QPDF-based):                            |
|  - Create field dictionaries (/FT, /T, /V, /DV, etc.)   |
|  - Create widget annotations with appearance streams     |
|  - Register fields in the AcroForm dictionary            |
|  - Handle field hierarchy (parent/child grouping)        |
|                                                          |
|  Interaction Layer (PDFium-based):                       |
|  - Render form fields with correct appearance            |
|  - Handle user input (click, type, select)               |
|  - Execute form JavaScript (calculations, validation)    |
|  - Manage focus and tab order                            |
|                                                          |
|  Data Layer:                                              |
|  - FDF/XFDF import/export                                |
|  - Form flattening                                       |
|  - Value extraction and population                       |
+----------------------------------------------------------+
```

**Appearance Stream Generation:**

One of the hardest parts of form creation is generating the appearance stream (`/AP`) for each widget annotation. The appearance stream is a content stream (like a page content stream) that defines how the field looks. This requires:

- Text rendering with proper font embedding (at minimum, standard 14 fonts)
- Border and background drawing
- Checkbox/radio button glyphs (ZapfDingbats or custom)
- Dropdown arrow rendering

Recommendation: Build a small appearance stream generator that handles the common cases. Use PDFium's rendering as the reference for what the output should look like.

---

### 2. Form Automation and JavaScript Sandboxing

#### 2.1 PDF JavaScript Capabilities

The PDF specification defines a JavaScript API (based on Acrobat's implementation) that includes:

- **Field calculations:** `AFSimple_Calculate("SUM", ["field1", "field2"])`
- **Field formatting:** `AFNumber_Format(2, 0, 0, 0, "$", true)`
- **Field validation:** `AFRange_Validate(true, 1, true, 100)`
- **Date handling:** `AFDate_FormatEx("mm/dd/yyyy")`
- **Document-level scripts:** Run on document open, print, save
- **Page-level scripts:** Run on page open/close
- **Field-level scripts:** Run on focus, blur, keystroke, calculate, validate, format

#### 2.2 Security Threat: JavaScript in PDFs

PDF JavaScript is a significant attack surface. Historical vulnerabilities include:

- Heap buffer overflows in JavaScript engines
- Arbitrary code execution via crafted JavaScript
- Information disclosure (local file paths, system info)
- Cross-site scripting in web-based viewers

**The PDF JavaScript API in Acrobat includes dangerous functions like:**
- `app.launchURL()` -- open arbitrary URLs
- `this.submitForm()` -- send form data to arbitrary endpoints
- `app.openDoc()` -- open other files
- `util.printf()` -- historically exploited for format string attacks

#### 2.3 JavaScript Engine Evaluation

##### QuickJS

- **Version:** 2025-09-13 (Bellard's release) / quickjs-ng latest
- **License:** MIT
- **Strengths:**
  - Tiny footprint (~210KB binary)
  - ES2023 compliant
  - No external dependencies
  - Easy to embed in C/C++ applications
  - **Mozilla uses QuickJS specifically for PDF.js JavaScript sandboxing** (pdf.js.quickjs project)
  - Built-in memory and execution time limits
  - No filesystem, network, or OS access by default
  - Pure C (compiles everywhere)
- **Weaknesses:**
  - Single-threaded
  - Slower than V8 for complex computations (but PDF form scripts are simple)
  - Must implement the Acrobat JavaScript API yourself (field objects, document object, app object)
- **Verdict:** **Strongly recommended.** The security properties are exactly right for PDF form JavaScript. Mozilla's use for the same purpose validates this choice.

##### Duktape

- **Version:** 2.7.x
- **License:** MIT
- **Strengths:**
  - Very small footprint (~140KB)
  - Easy C API
  - Good sandboxing
  - Extensively tested
- **Weaknesses:**
  - ES5.1 only (no ES6+)
  - Less actively maintained than QuickJS
  - Missing modern JavaScript features that some PDF scripts may use
- **Verdict:** Acceptable fallback, but QuickJS is superior due to modern JS support.

##### V8

- **Version:** Continuously updated (Chrome release cycle)
- **License:** BSD 3-Clause
- **Strengths:**
  - Fastest JavaScript engine
  - Full ES2023+ support
  - Isolates provide memory isolation
  - Used by PDFium internally
- **Weaknesses:**
  - Enormous binary size (~30MB+)
  - Complex build system (GN/Ninja)
  - Complex embedding API
  - Overkill for PDF form scripts
  - Difficult to limit resource consumption precisely
- **Verdict:** Not recommended for form JavaScript. The complexity and size are not justified for the simple scripts used in PDF forms.

#### 2.4 Sandboxing Architecture

```
+----------------------------------------------------------+
|              PDF JavaScript Sandbox                       |
+----------------------------------------------------------+
|                                                          |
|  +-----------+    +--------------------------------+     |
|  | QuickJS   |    | Acrobat JS API (Subset)        |     |
|  | Engine    |<-->| - Field object (get/set value)  |     |
|  |           |    | - Doc object (getField, etc.)   |     |
|  | Memory    |    | - AFSimple_Calculate            |     |
|  | limit:    |    | - AFNumber_Format               |     |
|  | 8MB       |    | - AFDate_FormatEx               |     |
|  |           |    | - AFRange_Validate              |     |
|  | Time      |    | - console.println (debug only)  |     |
|  | limit:    |    |                                  |     |
|  | 1 second  |    | BLOCKED:                         |     |
|  |           |    | - app.launchURL                  |     |
|  +-----------+    | - this.submitForm                |     |
|                   | - app.openDoc                    |     |
|                   | - util.printf (format strings)   |     |
|                   | - Any file/network access        |     |
|                   +--------------------------------+     |
|                                                          |
+----------------------------------------------------------+
|  Security Controls:                                      |
|  - No access to host filesystem                          |
|  - No network access                                     |
|  - Memory capped at 8MB per document                     |
|  - Execution time capped at 1 second per script          |
|  - Script errors caught and logged, never crash host     |
|  - User prompt before executing document-level scripts   |
+----------------------------------------------------------+
```

**Implementation of Auto-Date and Calculations:**

For the common cases (auto-date fields, sum/average/min/max calculations), these can be implemented without JavaScript at all:

```
Auto-Date:
  - On document open or form fill, set field value to current date
  - Format according to field's format string
  - No JavaScript needed; handle in the Form Manager

Calculations:
  - Parse the calculation order from the AcroForm /CO array
  - For AFSimple_Calculate: directly compute SUM/AVG/MIN/MAX/PRODUCT
  - Update dependent fields in order
  - This covers 90%+ of real-world form calculations
  - Only fall back to QuickJS for custom scripts
```

---

### 3. Form Data Handling (FDF/XFDF)

#### 3.1 Format Support

| Format | Description | Use Case |
|--------|-------------|----------|
| **FDF** (Forms Data Format) | Binary format, PDF-like structure | Legacy, compact |
| **XFDF** (XML Forms Data Format) | XML-based, human-readable | Modern, interoperable |
| **JSON** | Custom export format | API integration, web forms |
| **CSV** | Tabular export | Spreadsheet import |

#### 3.2 Implementation

FDF and XFDF handling is straightforward:

**XFDF (recommended primary format):**
```xml
<?xml version="1.0" encoding="UTF-8"?>
<xfdf xmlns="http://ns.adobe.com/xfdf/" xml:space="preserve">
  <fields>
    <field name="FirstName">
      <value>John</value>
    </field>
    <field name="LastName">
      <value>Doe</value>
    </field>
    <field name="Date">
      <value>2026-02-05</value>
    </field>
  </fields>
</xfdf>
```

**Implementation via QPDF:**
- Export: Enumerate form fields via `QPDFAcroFormDocumentHelper`, serialize to XFDF XML
- Import: Parse XFDF, map field names to form field objects, set values, regenerate appearance streams

**No additional library is needed for FDF/XFDF.** QPDF's object model plus standard XML parsing (e.g., pugixml for C++ or System.Xml for C#) is sufficient.

---

## Part 3: Annotation Features

### Annotation Types and Operations

#### Required Annotation Types

| PDF Annotation Type | User-Facing Feature | Complexity |
|---------------------|--------------------|-----------|
| `/Text` | Sticky notes / comments | Medium |
| `/Highlight` | Text highlighting | Medium |
| `/Underline` | Text underline markup | Medium |
| `/StrikeOut` | Text strikethrough | Medium |
| `/Ink` | Freehand drawing | Medium |
| `/FreeText` | Text box (typewriter) | Medium |
| `/Square`, `/Circle` | Shape markup | Low |
| `/Line` | Line with optional arrows | Low |
| `/Stamp` | Rubber stamp annotations | Medium |
| `/Popup` | Popup note (associated with other annotations) | Medium |

#### Library Evaluation for Annotations

##### PDFium

- **License:** BSD 3-Clause
- **Annotation Capabilities:**
  - `FPDFPage_CreateAnnot()`: Create annotations of any type
  - `FPDFAnnot_SetRect()`: Set position/size
  - `FPDFAnnot_SetColor()`: Set color
  - `FPDFAnnot_SetStringValue()`: Set properties (contents, author, date)
  - `FPDFAnnot_AppendAttachmentPoints()`: Set text markup positions
  - `FPDFAnnot_GetSubtype()`: Read annotation type
  - Full rendering of all standard annotation types
  - Annotation object enumeration per page
- **Strengths:**
  - BSD license
  - Rendering quality matches Chrome's PDF viewer
  - Create, read, update, delete operations all supported
  - Active development (Chromium project)
- **Weaknesses:**
  - No high-level "add highlight to selected text" API; you must compute the quad points yourself
  - Appearance stream generation is automatic for some types but not all
  - Build complexity
- **Verdict:** **Primary choice for annotation rendering and basic CRUD.**

##### Poppler

- **Version:** 26.02.0 (February 2026)
- **License:** GPL-2.0 (or GPL-3.0 depending on version)
- **Annotation Capabilities:**
  - Supports notes, highlights, ink, stamps, and custom annotations
  - Annotation creation and modification via C++ API
  - Recent versions improved digital signature annotation rendering
- **Weaknesses:**
  - GPL license restricts use in non-GPL projects
  - Limited annotation creation compared to PDFium
  - No XFA form support
  - No JavaScript
- **Verdict:** GPL license is a concern. PDFium is preferred.

##### MuPDF

- **Version:** 1.26.x
- **License:** AGPL-3.0
- **Annotation Capabilities:**
  - Full annotation creation and editing API
  - `pdf_create_annot()`, `pdf_set_annot_contents()`, etc.
  - Automatic appearance stream generation
  - Good rendering quality
- **Weaknesses:**
  - AGPL license
- **Verdict:** Technically excellent but license is restrictive.

##### QPDF

- **Annotation Capabilities:**
  - Low-level object manipulation for annotations
  - `QPDFAnnotationObjectHelper` for reading annotations
  - Can create annotations by building the object dictionary manually
- **Weaknesses:**
  - No rendering
  - No appearance stream generation
  - Very low-level; requires deep PDF spec knowledge
- **Verdict:** Supplement to PDFium for annotation data manipulation.

#### Recommended Annotation Architecture

```
+----------------------------------------------------------+
|                 Annotation Manager                        |
+----------------------------------------------------------+
|                                                          |
|  +-------------------+   +---------------------------+  |
|  | UI Layer           |   | Annotation Model          |  |
|  | - Selection tools  |   | - Type (highlight, note)  |  |
|  | - Property panels  |   | - Position (rect/quads)   |  |
|  | - Context menus    |   | - Content (text)          |  |
|  | - Color pickers    |   | - Author                  |  |
|  +--------+-----------+   | - Timestamp               |  |
|           |               | - Color, opacity           |  |
|           v               | - Reply chain (IRT)        |  |
|  +-------------------+   | - Visibility state         |  |
|  | PDFium Renderer    |   +---------------------------+  |
|  | - Render annots    |               |                  |
|  | - Hit testing      |               v                  |
|  | - Create/update    |   +---------------------------+  |
|  |   via C API        |   | QPDF Persistence          |  |
|  +-------------------+   | - Save to PDF objects      |  |
|                           | - Load from PDF objects    |  |
|                           | - Incremental save         |  |
|                           +---------------------------+  |
|                                                          |
+----------------------------------------------------------+
```

### Comment Threads and Replies

PDF supports threaded comments via the `/IRT` (In Reply To) key in annotation dictionaries:

```
Annotation A (Sticky Note):
  /Type /Annot
  /Subtype /Text
  /Contents (This paragraph needs review.)
  /T (Alice)                         % Author
  /M (D:20260205120000)              % Timestamp

Annotation B (Reply to A):
  /Type /Annot
  /Subtype /Text
  /Contents (I agree, the wording is unclear.)
  /T (Bob)
  /M (D:20260205130000)
  /IRT <reference to Annotation A>   % This is a reply to A
  /RT /R                             % Reply type: Reply

Annotation C (Reply to B):
  /Type /Annot
  /Subtype /Text
  /Contents (I will revise it today.)
  /T (Alice)
  /M (D:20260205140000)
  /IRT <reference to Annotation B>   % This is a reply to B
  /RT /R
```

**Implementation:**
- Build a tree structure from annotations using the `/IRT` references
- Display as a threaded list in the comments panel
- Creating a reply: create a new `/Text` annotation with `/IRT` pointing to the parent
- All standard PDF viewers (Acrobat, Foxit, etc.) support this format

### Hide/Show Annotations

PDF annotations have a `/F` (Flags) field with the following relevant bits:

| Bit | Name | Description |
|-----|------|-------------|
| 1 | Invisible | Do not render if no handler |
| 2 | Hidden | Do not render or allow interaction |
| 3 | Print | Include when printing |
| 4 | NoZoom | Do not scale with page zoom |
| 5 | NoRotate | Do not rotate with page |
| 6 | NoView | Do not render on screen (but may print) |

**Toggle visibility:** Set/clear the Hidden flag (bit 2). This is the standard PDF mechanism.

**Implementation in UI:** Provide a toggle per annotation and a global "show/hide all annotations" control. Modify the `/F` flags via QPDF and re-render via PDFium.

---

## Library Evaluation Matrix

### Comprehensive Comparison

| Criterion | PDFium | QPDF | MuPDF | Poppler | pdf-lib | PDFBox | iText | BouncyCastle |
|-----------|--------|------|-------|---------|---------|--------|-------|--------------|
| **License** | BSD 3-Clause | Apache 2.0 | AGPL-3.0 | GPL-2.0 | MIT | Apache 2.0 | AGPL-3.0 | MIT |
| **Language** | C/C++ | C++ | C | C++ | JavaScript | Java | Java/.NET | C#/Java |
| **Cross-Platform** | Win/Mac/Linux | Win/Mac/Linux | Win/Mac/Linux | Linux-primary | Any (JS) | Any (JVM) | Any (JVM/.NET) | Any (.NET/JVM) |
| **PDF Rendering** | Excellent | None | Excellent | Good | None | Partial | Partial | None |
| **Form Rendering** | Excellent | None | Good | Partial | None | N/A | N/A | None |
| **Form Creation** | Limited | Yes (low-level) | Limited | No | Yes | Yes | Yes | None |
| **XFA Support** | Yes | No | No | No | No | No | Yes | None |
| **Annotations** | Full CRUD | Low-level | Full CRUD | Partial | Limited | Yes | Yes | None |
| **Digital Signatures** | No | No | Limited | Verify only | No | Yes | Excellent | CMS only |
| **Encryption** | Read only | Full (all versions) | Full | Read + some write | No | Full | Full | N/A |
| **Redaction** | No | Partial (manual) | Yes (built-in) | No | No | Yes | Yes | None |
| **JavaScript** | Yes (V8-based) | No | Yes (MuJS) | No | N/A | No | No | None |
| **Active Maintenance** | Yes (Google) | Yes | Yes (Artifex) | Yes | Slow | Yes (Apache) | Yes | Yes |
| **Binary Size** | ~15MB | ~3MB | ~5MB | ~8MB | N/A (JS) | N/A (JVM) | N/A (JVM) | N/A (.NET) |
| **Performance** | Excellent | Excellent | Excellent | Good | N/A | Good | Good | Good |

### License Compatibility Summary

For an open-source PDF editor, license compatibility is critical:

| License | Compatible With | Implication |
|---------|----------------|-------------|
| BSD 3-Clause (PDFium) | Any license | No restrictions |
| Apache 2.0 (QPDF, PDFBox) | Most licenses (not GPLv2 without exception) | Patent grant, attribution required |
| MIT (BouncyCastle, pdf-lib, QuickJS) | Any license | Minimal restrictions |
| AGPL-3.0 (MuPDF, iText) | Only AGPL projects | Must release all source, including server-side. Very restrictive. |
| GPL-2.0 (Poppler) | GPL-2.0+ projects only | Must release source under GPL |

**For maximum flexibility (MIT/Apache/BSD project):** PDFium + QPDF + BouncyCastle + QuickJS

**For GPL project:** All of the above + Poppler (but still not MuPDF/iText without AGPL)

---

## Recommended Architecture

### Primary Library Stack

| Role | Library | Version | License | Justification |
|------|---------|---------|---------|---------------|
| **PDF Rendering** | PDFium | Chromium HEAD | BSD 3-Clause | Best rendering quality, form interaction, annotation support |
| **PDF Manipulation** | QPDF | 12.3.x | Apache 2.0 | Best encryption, object model, linearization, content-preserving transforms |
| **Cryptographic Primitives** | OpenSSL | 3.5.x | Apache 2.0 | Industry standard, cross-platform, provider architecture |
| **CMS/Signature Operations** | BouncyCastle C# | 2.5.x | MIT | Complete CMS/PKCS#7, certificate chain, OCSP/CRL |
| **Form JavaScript** | QuickJS | 2025-09-13+ | MIT | Secure sandboxing, small footprint, proven in PDF.js |
| **XML Parsing (XFDF)** | pugixml (C++) or System.Xml (C#) | Latest | MIT / .NET | Lightweight, standards-compliant |

### Architecture Diagram

```
+=====================================================================+
|                        PDFE Application                              |
+=====================================================================+
|                                                                      |
|  +-------------------+  +--------------------+  +----------------+  |
|  | Security Module    |  | Forms Module        |  | Annotations    |  |
|  |                   |  |                    |  | Module          |  |
|  | - Signature Mgr   |  | - Field Creator    |  | - CRUD Mgr     |  |
|  | - Encryption Mgr  |  | - Interaction Mgr  |  | - Thread Mgr   |  |
|  | - Redaction Eng   |  | - JS Sandbox       |  | - Visibility   |  |
|  | - Metadata Strip  |  | - FDF/XFDF I/O     |  | - Rendering    |  |
|  | - Permission Mgr  |  | - Calc Engine      |  |                |  |
|  +--------+----------+  +--------+-----------+  +-------+--------+  |
|           |                      |                       |           |
|  +--------v----------+  +-------v------------+  +-------v--------+  |
|  | BouncyCastle C#    |  | QuickJS            |  |                |  |
|  | (CMS/PKCS#7)       |  | (Sandboxed JS)     |  |                |  |
|  +--------+-----------+  +--------------------+  |                |  |
|           |                                       |                |  |
|  +--------v------------------------------------------v--------+     |
|  |                     PDFium (BSD)                            |     |
|  |  - Rendering    - Form Interaction    - Annotation CRUD    |     |
|  +------------------------------------------------------------+     |
|                                                                      |
|  +------------------------------------------------------------+     |
|  |                     QPDF (Apache 2.0)                       |     |
|  |  - Encryption/Decryption    - Object Manipulation           |     |
|  |  - Linearization            - Content Stream Access         |     |
|  |  - Permission Setting       - Form Field Objects            |     |
|  |  - Incremental Save         - Annotation Objects            |     |
|  +------------------------------------------------------------+     |
|                                                                      |
|  +------------------------------------------------------------+     |
|  |                   OpenSSL 3.x (Apache 2.0)                  |     |
|  |  - Hash computation    - RSA/ECDSA    - AES    - PKCS#11   |     |
|  +------------------------------------------------------------+     |
|                                                                      |
+=====================================================================+
```

### Integration Pattern

The two PDF libraries (PDFium and QPDF) serve complementary roles:

- **PDFium** handles all rendering, form interaction, and annotation display. It operates on an in-memory document loaded from bytes.
- **QPDF** handles all structural manipulation: encryption, decryption, object creation, form field creation, metadata stripping, linearization, and saving. It operates on the PDF object graph.

**Workflow for a typical edit-save cycle:**

1. Open file: QPDF decrypts if needed, passes raw bytes to PDFium for rendering
2. User edits (annotations, forms, etc.): PDFium handles interaction, changes tracked in the application model
3. Save: Application model changes are applied to the QPDF object graph, QPDF writes the output (with encryption, linearization, etc. as configured)

This avoids conflicts between the two libraries and keeps each in its area of strength.

---

## Implementation Roadmap

### Phase 1: Foundation (Weeks 1-4)

| Task | Library | Priority |
|------|---------|----------|
| Integrate PDFium rendering pipeline | PDFium | P0 |
| Integrate QPDF for open/save | QPDF | P0 |
| Password-protected PDF open/save (AES-256) | QPDF | P0 |
| User/Owner password management UI | QPDF | P0 |
| Basic annotation rendering (display existing) | PDFium | P0 |
| Basic form rendering (display existing) | PDFium | P0 |

### Phase 2: Annotation System (Weeks 5-8)

| Task | Library | Priority |
|------|---------|----------|
| Sticky note creation/editing/deletion | PDFium + QPDF | P0 |
| Text highlighting | PDFium + QPDF | P0 |
| Underline and strikethrough markup | PDFium + QPDF | P0 |
| Freehand drawing (ink annotations) | PDFium + QPDF | P1 |
| Comment threads and replies (IRT) | QPDF | P1 |
| Hide/show annotations toggle | PDFium + QPDF | P1 |
| Annotation property editing (color, author) | PDFium + QPDF | P1 |

### Phase 3: Form System (Weeks 9-14)

| Task | Library | Priority |
|------|---------|----------|
| Form field filling (existing forms) | PDFium | P0 |
| Form field creation (text, checkbox, radio, dropdown) | QPDF | P0 |
| Signature field creation | QPDF | P0 |
| Appearance stream generator | Custom + QPDF | P1 |
| FDF/XFDF export | QPDF + XML parser | P1 |
| FDF/XFDF import | QPDF + XML parser | P1 |
| Auto-date fields | Custom | P1 |
| Basic calculations (SUM, AVG, etc.) | Custom | P1 |
| QuickJS integration for custom scripts | QuickJS | P2 |
| Acrobat JS API subset implementation | QuickJS + Custom | P2 |
| Form flattening | QPDF | P2 |

### Phase 4: Security (Weeks 15-22)

| Task | Library | Priority |
|------|---------|----------|
| Digital signature creation (PAdES B-B) | BouncyCastle + QPDF | P0 |
| Digital signature verification | BouncyCastle + QPDF | P0 |
| Timestamped signatures (PAdES B-T) | BouncyCastle + TSA | P1 |
| Long-term validation (PAdES B-LT) | BouncyCastle + OCSP/CRL | P1 |
| DocMDP (read-only after signing) | QPDF | P1 |
| Visible signature appearance | Custom + QPDF | P1 |
| Permission management (print/copy prevent) | QPDF | P1 |
| Metadata stripping (Level 1-3) | QPDF | P1 |
| Redaction marking UI | PDFium + Custom | P1 |
| Redaction application (content removal) | QPDF + Custom | P1 |
| Redaction verification | Custom | P2 |
| Archival signatures (PAdES B-LTA) | BouncyCastle | P2 |

### Phase 5: Hardening (Weeks 23-26)

| Task | Library | Priority |
|------|---------|----------|
| JavaScript sandbox security audit | QuickJS | P0 |
| Redaction completeness verification tool | Custom | P0 |
| Encryption downgrade warnings (RC4 files) | QPDF | P1 |
| Certificate management UI (import/export) | BouncyCastle | P1 |
| Signature validation result display | Custom | P1 |
| Penetration testing of JS sandbox | QuickJS | P2 |

---

## Security Best Practices

### 1. Signature Implementation

- Always compute digests over the exact byte ranges specified in the `/ByteRange` array
- Never trust self-signed certificates without explicit user confirmation
- Display clear visual indicators for signature validity states: Valid (green), Invalid (red), Unknown/Partial (yellow)
- Support incremental saves to preserve existing signatures when adding new ones
- Implement DocMDP checking: reject modifications that violate the signature's modification permissions

### 2. Encryption Implementation

- Default to AES-256 (V5 R6) for new encryption
- Warn users when opening RC4-encrypted files
- Never store decrypted content in temporary files on disk; keep decrypted data in memory only
- Clear decryption keys from memory when the document is closed (use SecureString or equivalent)
- Enforce minimum password strength for user-created passwords (recommend but do not require)

### 3. Redaction Implementation

- Always re-save as a single-revision file after redaction (eliminates undo history)
- Strip all metadata after redaction (Level 2 sanitization minimum)
- Provide a verification step that searches the entire object graph for remnants of redacted text
- Consider offering the rasterize-and-reconstruct option for maximum security
- Never leave redaction annotations in the file after application -- they must be removed
- Log redaction operations for audit purposes

### 4. JavaScript Security

- Sandbox all JavaScript execution in QuickJS with strict memory (8MB) and time (1 second) limits
- Implement only the subset of the Acrobat JavaScript API needed for form calculations and validation
- Block all APIs that access the filesystem, network, or system information
- Prompt the user before executing document-level scripts (scripts that run on open)
- Provide a global setting to disable JavaScript entirely
- Never pass unsanitized user input from JavaScript into the host application

### 5. General PDF Security

- Validate PDF structure on open (QPDF's `check` mode)
- Limit resource consumption when parsing (max page count, max object count, max stream size)
- Handle malformed PDFs gracefully without crashes
- Never execute embedded files or launch external applications without explicit user consent

---

## Sources

- [QPDF Encryption Documentation](https://qpdf.readthedocs.io/en/stable/encryption.html)
- [QPDF 12.3.0 Release](https://qpdf.readthedocs.io/_/downloads/en/latest/pdf/)
- [pikepdf Digital Signatures Issue](https://github.com/pikepdf/pikepdf/issues/131)
- [pikepdf Security Documentation](https://pikepdf.readthedocs.io/en/latest/topics/security.html)
- [PDFium Form and JavaScript APIs](https://deepwiki.com/chromium/pdfium/6.2-form-and-javascript-apis)
- [PDFium Annotation Header](https://github.com/chromium/pdfium/blob/master/public/fpdf_annot.h)
- [PDFium in 2025](https://www.nutrient.io/blog/why-pdfium-is-a-trusted-platform-for-pdf-rendering/)
- [PDFium License](https://github.com/chromium/pdfium/blob/main/LICENSE)
- [MuPDF Official Site](https://mupdf.com/)
- [MuPDF Wikipedia](https://en.wikipedia.org/wiki/MuPDF)
- [MuPDF Form Source](https://github.com/ArtifexSoftware/mupdf/blob/master/source/pdf/pdf-form.c)
- [Poppler Official Site](https://poppler.freedesktop.org/)
- [QuickJS Official Site](https://bellard.org/quickjs/)
- [Mozilla pdf.js.quickjs Sandbox](https://github.com/mozilla/pdf.js.quickjs)
- [QuickJS-NG](https://github.com/quickjs-ng/quickjs)
- [MicroQuickJS](https://simonwillison.net/2025/Dec/23/microquickjs/)
- [iText PAdES Signing API](https://itextpdf.com/blog/itext-news-technical-notes/itext-suite-803-advanced-pades-api-two-step-signing)
- [iText Post-Quantum Signatures (Suite 9.5)](https://pdfa.org/itext-suite-95-supporting-both-brotli-compression-and-quantum-safe-signatures/)
- [pdf-lib on npm](https://www.npmjs.com/package/pdf-lib)
- [pdf-lib Form API](https://pdf-lib.js.org/docs/api/classes/pdfform)
- [BouncyCastle PKCS#7 Example](https://gist.github.com/markscottwright/331bde6028352098883e07b16110732f)
- [BouncyCastle CMS Source](https://github.com/bcgit/bc-csharp/blob/master/crypto/src/cms/CMSSignedData.cs)
- [Apache PDFBox Digital Signatures](https://deepwiki.com/apache/pdfbox/4.3-digital-signatures)
- [Apache PDFBox Official Site](https://pdfbox.apache.org/)
- [PAdES Signature Levels Explained](https://idura.eu/blog/pades-signature-levels-explained)
- [PAdES Wikipedia](https://en.wikipedia.org/wiki/PAdES)
- [PAdES and Long-Term Archival](https://www.cryptomathic.com/blog/pades-and-long-term-archival-lta)
- [PDF Redaction Failures Research](https://www.argeliuslabs.com/deep-research-on-pdf-redaction-failures-and-security-risks-exploits-and-best-practices/)
- [Adobe Redacting and Sanitizing](https://helpx.adobe.com/acrobat/desktop/protect-documents/redact-pdfs/redacting-sanitizing.html)
- [PDF Encryption Standards Comparison (PDFlib)](https://www.pdflib.com/pdf-knowledge-base/pdf-password-security/encryption/)
- [OpenSSL 3.5 Release Notes](https://openssl-library.org/news/openssl-3.5-notes/)
- [LibreSSL vs OpenSSL vs BoringSSL](https://www.techtarget.com/searchsecurity/answer/Are-LibreSSL-and-BoringSSL-safe-OpenSSL-alternatives)
- [OpenSSL vs BoringSSL Comparison](https://www.e2encrypted.com/posts/openssl-vs-boringssl-a-comparison-of-security-and-performance/)
- [QPDF Form Fields Example](https://github.com/qpdf/qpdf/blob/main/examples/pdf-set-form-values.cc)
- [QPDF Annotation Issue](https://github.com/qpdf/qpdf/issues/437)
