# Phase 11: PDF/A, PDF/UA, and Tagged PDF Support

> **NOT A THREAT-MODEL SOURCE.** This is an implementation plan for the
> phase-11 PDF/A + PDF/UA + Tagged-PDF track. It was previously counted as one
> of PDFE's threat-model documents, which overstated the documented security
> posture: a `grep` for threat / supply-chain / SBOM / resource / DoS terms
> across its 412 lines returns four incidental hits, none of them a threat
> statement (meta-audit gap `SEC-107`). Follow-up phase **F9 chose to stop
> citing it** rather than to invent threat content for a compliance plan.
>
> The single canonical threat model is **`docs/security-testing.md` §(a)**.
> User-facing controls live in `docs/security-policy.md`; the CI-lane inventory
> lives in `tests/docs/security_workflows.md`. `ThreatModelSourceDocs` in
> `tests/test_doc_drift_guards.cpp` fails if any non-audit document under
> `docs/` starts citing this file as a threat-model source again.

## Context

All 10 original phases are complete (1411 tests, 0 failures). The project has no PDF standards compliance support -- it can open/edit any PDF but cannot validate, create, or preserve PDF/A (archival), PDF/UA (accessibility), or Tagged PDF documents. This phase fills that gap.

**User decisions:**
- **PDF/A levels**: Full coverage -- 1b, 2b, 3b, 2u, 2a
- **Validation**: Custom C++ validator (no Java/veraPDF dependency)
- **Tagging**: Auto-tag only (no manual structure tree editing UI)

## Key Technical Constraints

- **MuPDF** can READ structure trees (`FZ_STEXT_COLLECT_STRUCTURE`) but has NO APIs for CREATING them
- **QPDF** has low-level object manipulation but no structure tree helpers
- Structure tree creation requires manual `pdf_obj` construction (dicts, arrays, streams)
- **pugixml** is already in vcpkg.json but unused -- perfect for XMP metadata
- No ICC profile library needed -- bundle sRGB profile as constexpr byte array (~3KB)
- Two-stage save pipeline (MuPDF -> temp -> QPDF -> output) proven in EncryptionManager

## New Module: `src/compliance/`

All new code lives in `src/compliance/` with `pdfe_compliance` CMake target linking `pdfe_core`, `pdfe_fonts`, `pugixml::pugixml`.

## Implementation Status

**Partially started (previous session, not yet built/tested):**
- 9 source files created in `src/compliance/` for sub-phase 11a
- Known issue: enum naming mismatches between ComplianceTypes.h and implementation files need fixing before compilation
- Friend declarations and CMake wiring already added to Document.h and root CMakeLists.txt

---

## Sub-Phase 11a: Foundation (XMP, OutputIntent, Font Validation)

Every PDF/A level needs these three pieces. Build them first.

### New Files (6 header+source pairs)

| File | Purpose |
|------|---------|
| `src/compliance/ComplianceTypes.h` | `PdfALevel` enum, `ComplianceIssue`/`ComplianceResult` structs, helper functions |
| `src/compliance/XmpMetadataWriter.h/.cpp` | Build XMP XML via pugixml, write as `/Metadata` stream on catalog |
| `src/compliance/IccProfileManager.h/.cpp` | Add sRGB `/OutputIntent` with embedded ICC profile to catalog |
| `src/compliance/FontComplianceChecker.h/.cpp` | Validate font embedding, ToUnicode CMaps, glyph widths |
| `src/compliance/srgb_icc_profile.h` | Bundled sRGB ICC profile as `constexpr std::array<uint8_t, N>` |
| `src/compliance/CMakeLists.txt` | Build target |

### Key APIs

```cpp
namespace pdfe::compliance {

// --- ComplianceTypes.h ---
enum class PdfALevel { None, PdfA_1b, PdfA_2b, PdfA_2u, PdfA_2a, PdfA_3b };
enum class PdfUALevel { None, PdfUA_1 };
struct ComplianceIssue { IssueSeverity severity; std::string rule, description; int pageIndex; };
struct ComplianceResult { bool success; PdfALevel targetLevel; std::vector<ComplianceIssue> issues; };

// --- XmpMetadataWriter ---
void writePdfAIdentification(Document* doc, PdfALevel level);
void writePdfUAIdentification(Document* doc, PdfUALevel level);
std::string readXmpPacket(Document* doc);
PdfALevel detectPdfALevel(Document* doc);

// --- IccProfileManager ---
bool hasOutputIntent(Document* doc) const;
void addSrgbOutputIntent(Document* doc);

// --- FontComplianceChecker ---
std::vector<FontComplianceInfo> checkFonts(Document* doc, bool requireUnicode = false);
bool allFontsCompliant(Document* doc, bool requireUnicode = false);
```

### MuPDF Implementation Details

**XMP metadata** -- write as stream on catalog `/Metadata`:
```cpp
fz_buffer* buf = fz_new_buffer_from_copied_data(ctx, xmpBytes.data(), xmpBytes.size());
pdf_obj* stmObj = pdf_add_stream(ctx, pdfDoc, buf, nullptr, 0);
pdf_dict_put(ctx, stmObj, PDF_NAME(Type), PDF_NAME(Metadata));
pdf_dict_put_name(ctx, stmObj, PDF_NAME(Subtype), "XML");
pdf_dict_put(ctx, catalog, PDF_NAME(Metadata), stmObj);
```

**ICC OutputIntent** -- create dict + stream on catalog `/OutputIntents`:
```cpp
pdf_obj* iccStream = pdf_add_stream(ctx, pdfDoc, iccBuf, nullptr, 0);
pdf_dict_put_int(ctx, iccStream, PDF_NAME(N), 3); // RGB
pdf_obj* intent = pdf_new_dict(ctx, pdfDoc, 6);
pdf_dict_put(ctx, intent, PDF_NAME(Type), PDF_NAME(OutputIntent));
pdf_dict_put_name(ctx, intent, PDF_NAME(S), "GTS_PDFA1");
pdf_dict_put_text_string(ctx, intent, PDF_NAME(OutputConditionIdentifier), "sRGB IEC61966-2.1");
pdf_dict_puts(ctx, intent, "DestOutputProfile", iccStream);
// Add to /OutputIntents array on catalog
```

**Font validation** -- iterate page `/Resources/Font` dicts:
- Check `/FontDescriptor/FontFile|FontFile2|FontFile3` for embedding
- Check `/ToUnicode` CMap presence (required for PDF/A-2u+)
- Check `/Widths` or CIDFont `/DW`/`/W` for glyph widths

### Tests: ~30 (3 test files)
- `test_xmp_metadata_writer.cpp` (~12): build packet per level, write/read roundtrip, detect level, sync Info dict
- `test_icc_profile_manager.cpp` (~8): add sRGB intent, verify stream, idempotent add, has/no intent
- `test_font_compliance_checker.cpp` (~10): embedded pass, non-embedded detected, ToUnicode missing, CIDFont widths

### Files Modified
- `CMakeLists.txt` (root) -- add `add_subdirectory(src/compliance)`
- `tests/CMakeLists.txt` -- add test sources, link `pdfe_compliance`
- `src/core/Document.h` -- add friend declarations for compliance classes

---

## Sub-Phase 11b: PDF/A Conversion + Validation (1b, 2b, 2u, 3b)

### New Files (3 header+source pairs)

| File | Purpose |
|------|---------|
| `src/compliance/PdfAValidator.h/.cpp` | Composable validation engine (follows RedactionVerifier pattern) |
| `src/compliance/PdfAConverter.h/.cpp` | Two-stage conversion pipeline (MuPDF -> QPDF) |
| `src/compliance/ProhibitedFeatureScanner.h/.cpp` | Detect JS, encryption, external refs, transparency, object streams |

### Key APIs

```cpp
// --- PdfAValidator ---
ComplianceResult validate(Document* doc, PdfALevel level);
bool isCompliant(Document* doc, PdfALevel level);

// --- PdfAConverter ---
ComplianceResult convertAndSave(Document* doc, PdfALevel level, const fs::path& outputPath);
ComplianceResult analyzeForConversion(Document* doc, PdfALevel level);

// --- ProhibitedFeatureScanner ---
bool hasEncryption(Document* doc) const;
bool hasJavaScript(Document* doc) const;
bool hasTransparency(Document* doc) const;  // prohibited in PDF/A-1b only
bool hasObjectStreams(Document* doc) const;  // prohibited in PDF/A-1b only
std::vector<ComplianceIssue> scanAll(Document* doc, PdfALevel level) const;
```

### Conversion Pipeline (two-stage, follows EncryptionManager pattern)

```
Stage 1 (MuPDF in-memory):
  1. Write XMP metadata with pdfaid identification
  2. Add sRGB OutputIntent if missing
  3. Set /MarkInfo /Marked true
  4. Remove JavaScript (catalog /Names/JavaScript, page /AA, annotation /A actions)
  5. Save to temp with pdf_write_options:
     - do_encrypt = PDF_ENCRYPT_NONE
     - do_use_objstms = 0 (for PDF/A-1b)
     - do_garbage = 3, do_compress = 1

Stage 2 (QPDF structural):
  1. Remove /Encrypt from trailer
  2. For PDF/A-1b: strip transparency groups from pages
  3. Set object stream mode (disable for 1b, generate for 2b+)
  4. Write final output

Post-save:
  - Open output with fresh context
  - Run PdfAValidator to verify
  - Return ComplianceResult
```

### Validation Rules Implemented

| Rule | ISO Clause | Applies To |
|------|-----------|------------|
| All fonts embedded | 6.3.3 | All levels |
| ToUnicode CMap | 6.3.8 | 2u, 2a |
| XMP pdfaid metadata | 6.7.11 | All levels |
| OutputIntent present | 6.2.2 | All levels |
| No encryption | 6.1.3 | All levels |
| No JavaScript | 6.6.1 | All levels |
| No external references | 6.6.3 | All levels |
| No transparency | 6.4 | 1b only |
| No object streams | 6.1.7 | 1b only |
| Tagged PDF | 6.8 | 2a only (deferred to 11d) |

### Tests: ~45 (3 test files + 1 fixture script)
- `test_pdfa_validator.cpp` (~20): validate each level, missing XMP/OutputIntent/fonts, JS/encryption detected, transparency on 1b
- `test_pdfa_converter.cpp` (~15): convert to each level, removes encryption/JS, adds XMP/OutputIntent, verify output
- `test_prohibited_feature_scanner.cpp` (~10): detect each prohibited feature, scan all per level

### Files Modified
- `src/compliance/CMakeLists.txt` -- add sources
- `tests/CMakeLists.txt` -- add tests
- `src/core/Document.h` -- add friend declarations

---

## Sub-Phase 11c: Tagged PDF / Auto-Tagging Infrastructure

### New Files (4 header+source pairs)

| File | Purpose |
|------|---------|
| `src/compliance/StructureTypes.h` | `fz_structure` <-> PDF tag name mapping constants |
| `src/compliance/StructureTreeBuilder.h/.cpp` | Build `/StructTreeRoot`, `/StructElem` dicts, `/ParentTree` via MuPDF pdf_obj API |
| `src/compliance/AutoTagger.h/.cpp` | Analyze content via `FZ_STEXT_COLLECT_STRUCTURE`, generate semantic tags |
| `src/compliance/ContentStreamTagger.h/.cpp` | Inject BDC/EMC marked content operators into page content streams |

### Key APIs

```cpp
// --- StructureTreeBuilder ---
void buildTree(Document* doc, const StructureNode& root);
bool hasStructureTree(Document* doc) const;
void removeStructureTree(Document* doc);
StructureNode readStructureTree(Document* doc);
void setDocumentLanguage(Document* doc, const std::string& lang);

// --- AutoTagger ---
StructureNode tagDocument(Document* doc);  // returns complete tree
std::vector<StructureNode> tagPage(Document* doc, int pageIndex, int& nextMcid);

// --- ContentStreamTagger ---
void tagPageContent(Document* doc, int pageIndex, const std::vector<MarkedContentRegion>& regions);
bool pageHasMarkedContent(Document* doc, int pageIndex) const;
```

### Structure Tree Construction (MuPDF low-level pdf_obj API)

The structure tree requires these PDF objects:

```
Catalog
  /StructTreeRoot -> dict { /Type /StructTreeRoot, /K [children], /ParentTree, /ParentTreeNextKey }
  /MarkInfo -> dict { /Marked true }
  /Lang (en-US)

Each /StructElem:
  /Type /StructElem, /S /<type>, /P <parent ref>
  /K <MCID int> (leaf) or <array of child StructElem refs> (internal)
  /Pg <page ref> (leaf only)
  /Alt (text) (Figure elements)

/ParentTree -> number tree { /Nums [idx, elemRef, ...] }
```

Build via `pdf_new_dict`, `pdf_add_object`, `pdf_dict_put`, `pdf_array_push`, `pdf_dict_put_int`, `pdf_dict_put_name`.

### Auto-Tagging Heuristics

Uses MuPDF `fz_new_stext_page_from_page()` with `FZ_STEXT_COLLECT_STRUCTURE`:

| Content | Detection | Tag |
|---------|-----------|-----|
| Large bold text (>1.8x avg) | Font size + bold flag | H1 |
| Medium bold text (>1.4x avg) | Font size + bold flag | H2 |
| Slightly bold text (>1.2x avg) | Font size + bold flag | H3 |
| Normal text blocks | Default | P |
| Image blocks | `FZ_STEXT_BLOCK_IMAGE` | Figure (with alt="Image" placeholder) |
| Existing structure | `FZ_STEXT_BLOCK_STRUCT` | Preserve original type |

### Content Stream Marking

Inject BDC/EMC operators around content:
```
/P <</MCID 0>> BDC
  ... existing text operators ...
EMC
/Figure <</MCID 1>> BDC
  ... existing image operators ...
EMC
```

Initial approach: one MCID per detected block per page. Wrap contiguous operator sequences.

### Tests: ~37 (3 test files)
- `test_structure_tree_builder.cpp` (~15): build minimal/multi-level tree, parent tree, StructParent, MarkInfo, read/remove
- `test_auto_tagger.cpp` (~12): heading detection, paragraph detection, image as Figure, MCID sequencing, multi-page
- `test_content_stream_tagger.cpp` (~10): inject BDC/EMC, artifact marking, content integrity, roundtrip verify

### Files Modified
- `src/compliance/CMakeLists.txt` -- add sources
- `tests/CMakeLists.txt` -- add tests
- `src/core/Document.h` + `src/core/Page.h` -- add friend declarations

---

## Sub-Phase 11d: PDF/A-2a + PDF/UA + UI Integration

### New Files (4 header+source pairs)

| File | Purpose |
|------|---------|
| `src/compliance/PdfUAConverter.h/.cpp` | PDF/UA-1 conversion (wraps AutoTagger + StructureTreeBuilder) |
| `src/compliance/ComplianceManager.h/.cpp` | Facade class for UI layer (validate, convert, analyze) |
| `src/ui/ComplianceDialog.h/.cpp` | Qt dialog: level selection, validation results, convert button |

### Key APIs

```cpp
// --- ComplianceManager (facade) ---
ComplianceResult validatePdfA(Document* doc, PdfALevel level);
ComplianceResult validatePdfUA(Document* doc);
PdfALevel detectPdfALevel(Document* doc);
ComplianceResult convertToPdfA(Document* doc, PdfALevel level, const fs::path& outputPath);
ComplianceResult convertToPdfUA(Document* doc, const fs::path& outputPath, const std::string& lang);
ComplianceResult convertToPdfA2aAndUA(Document* doc, const fs::path& outputPath, const std::string& lang);

// --- ComplianceDialog (Qt) ---
// Level combo, Validate/Analyze/Convert buttons, issue table
// All widgets have setAccessibleName() for Selenium testing
```

### PDF/A-2a Conversion

Extends PdfAConverter from 11b. When `level == PdfA_2a`:
1. Run AutoTagger to generate structure tree
2. Build structure tree via StructureTreeBuilder
3. Inject BDC/EMC via ContentStreamTagger
4. Set document language
5. Continue with standard PDF/A conversion pipeline

### PDF/UA-1 Conversion

1. Set `/Lang` on catalog
2. Auto-tag document (AutoTagger)
3. Build structure tree (StructureTreeBuilder)
4. Inject marked content (ContentStreamTagger)
5. Write XMP with `pdfuaid:part=1`
6. Remove restrictive encryption
7. Save via two-stage pipeline
8. Validate output

### PDF/UA Validation Rules

| Rule | Description |
|------|-------------|
| Structure tree exists | `/StructTreeRoot` in catalog |
| All content tagged | No untagged content (check MCIDs cover all operators) |
| Images have alt text | All Figure elements have `/Alt` |
| Language set | `/Lang` in catalog |
| No restrictive encryption | Accessibility extraction not restricted |
| Logical reading order | Structure tree order matches visual layout |

### UI: ComplianceDialog

```
+------------------------------------------+
| PDF Standards Compliance                  |
|                                           |
| Target: [PDF/A-2b (Archival)        v]   |
|                                           |
| [Analyze] [Validate] [Convert...]        |
|                                           |
| Issues:                                   |
| +------+--------+-----------------------+ |
| | Sev  | Rule   | Description           | |
| +------+--------+-----------------------+ |
| | ERR  | 6.3.3  | Font X not embedded   | |
| | WARN | 6.7.11 | XMP missing pdfaid    | |
| +------+--------+-----------------------+ |
|                                           |
| Status: 2 errors, 1 warning              |
+------------------------------------------+
```

### MainWindow Integration

- Add "Compliance..." menu item under Tools menu
- Opens ComplianceDialog with current document
- `conversionCompleted` signal triggers file open or refresh

### Tests: ~34 (4 test files)
- `test_pdfa_2a_conversion.cpp` (~10): convert to 2a, structure tree created, content tagged, headings detected
- `test_pdfua_converter.cpp` (~10): convert to PDF/UA-1, structure tree, language, alt text, validation
- `test_compliance_manager.cpp` (~8): facade validate/convert/detect for all levels
- `test_compliance_dialog.cpp` (~6): construct, level selection, button triggers, issue table, accessible names

### Files Modified
- `src/compliance/CMakeLists.txt` -- add sources
- `src/compliance/PdfAConverter.cpp` -- add tagging branch for PdfA_2a
- `src/compliance/PdfAValidator.cpp` -- implement PDF/UA validation + PDF/A-2a tagging checks
- `src/ui/CMakeLists.txt` -- add ComplianceDialog
- `src/ui/MainWindow.cpp` -- add "Compliance..." menu item, wire dialog
- `tests/CMakeLists.txt` -- add tests

---

## Summary

| Sub-phase | Focus | New Files | Tests | Depends On |
|-----------|-------|-----------|-------|------------|
| **11a** | XMP + OutputIntent + Font validation | 6 h/cpp + 1 CMake | ~30 | pugixml, MuPDF pdf_obj |
| **11b** | PDF/A conversion + validation (1b/2b/2u/3b) | 3 h/cpp | ~45 | 11a, QpdfWrapper |
| **11c** | Tagged PDF / auto-tagging | 4 h/cpp | ~37 | 11a, MuPDF stext |
| **11d** | PDF/A-2a + PDF/UA + UI | 3 h/cpp + 1 dialog | ~34 | 11b + 11c |
| **Total** | | **16 h/cpp + 3 support** | **~146** | |

## Known Issues (Pre-Implementation)

- **Enum naming mismatch**: `ComplianceTypes.h` defines `PdfA_1b` (with underscore) but XmpMetadataWriter.cpp uses `PdfA1b` (no underscore). Also references non-existent values `PdfA1a`, `PdfA3u`, `PdfA3a`, `PdfUA2`. Must reconcile before building.
- The 9 source files in `src/compliance/` have never been compiled or tested.

## Verification

1. **Build**: `cmake --build --preset debug-vs2022` -- clean compilation, no warnings
2. **All existing 1411+ tests pass** plus ~146 new tests
3. **PDF/A-1b**: Open any PDF -> Tools -> Compliance -> PDF/A-1b -> Convert -> open output in veraPDF online validator
4. **PDF/A-2b**: Same flow, verify transparency allowed
5. **PDF/A-2u**: Verify ToUnicode CMaps present in output
6. **PDF/A-2a**: Verify structure tree exists in output (inspect with qpdf --show-object)
7. **PDF/A-3b**: Same as 2b
8. **PDF/UA-1**: Verify structure tree + alt text + /Lang in output
9. **Roundtrip**: Convert to PDF/A -> reopen in PDFE -> verify rendering matches original
