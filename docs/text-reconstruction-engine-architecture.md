# Text Reconstruction Engine -- Architecture Design

**Date:** 2026-02-05
**Status:** Proposed
**Author:** Backend Architect
**Component:** Text Reconstruction Engine (TRE)
**Dependencies:** Tesseract 5.x, MuPDF, QPDF, FreeType, HarfBuzz, libvips

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Component Architecture](#2-component-architecture)
3. [OCR-to-Editable Pipeline](#3-ocr-to-editable-pipeline)
4. [Image Masking Strategy](#4-image-masking-strategy)
5. [Font Matching for Scanned Text](#5-font-matching-for-scanned-text)
6. [Coordinate System](#6-coordinate-system)
7. [Mixed Content Page Structure](#7-mixed-content-page-structure)
8. [Background Color Detection](#8-background-color-detection)
9. [Multi-Region Editing](#9-multi-region-editing)
10. [Undo/Redo Integration](#10-undoredo-integration)
11. [Quality Thresholds and Fallback](#11-quality-thresholds-and-fallback)
12. [C++ Interface Definitions](#12-c-interface-definitions)
13. [Implementation Roadmap](#13-implementation-roadmap)
14. [ADR: Key Architectural Decisions](#14-adr-key-architectural-decisions)

---

## 1. Executive Summary

The Text Reconstruction Engine (TRE) enables editing of text on scanned/image-only PDF pages. A scanned PDF page contains a single large raster image with no text objects. The TRE bridges the gap between "pixels that look like text" and "real PDF text objects the user can edit."

**Architecture Pattern:** Pipeline with Strategy (dual-approach with quality-gated fallback)

**Primary Approach (B -- Hybrid Region-Based Reconstruction):**
The page image remains intact except where the user edits. Each edited region is surgically replaced: the image is masked (background-filled) in that rectangle, and real PDF text objects are layered on top. The result is a mixed-content page where most of the visual content is still the original scan, but edited regions contain real, searchable, reflowable text.

**Fallback Approach (C -- Image-Level Text Replacement):**
When Approach B cannot guarantee visual quality (low OCR confidence, complex background, overlapping graphics), the replacement text is rendered to a bitmap using the matched font and composited directly onto the source image. The result is still an image-only page, but the visual text has changed.

**Key Technologies:**
- Tesseract 5.x: OCR with HOCR output (bounding boxes, confidence, baseline, x-height)
- MuPDF: PDF content stream construction, page object manipulation, font embedding
- QPDF: Structural PDF operations, page tree manipulation, incremental save
- FreeType + HarfBuzz: Font metrics extraction, text shaping, glyph rasterization
- libvips: Image masking, compositing, background color sampling

**Critical Design Decisions:**
1. Region-level granularity (paragraph/block, not word-level, not full-page) for reconstruction boundaries
2. PDF Form XObject layering to keep original image, mask fills, and text overlays as separable content stream segments
3. Dual-coordinate affine transform cached per page-image pair, converting between pixel space and PDF user space

**Estimated Complexity:** High

---

## 2. Component Architecture

### 2.1 System Diagram

```
                          +----------------------------------+
                          |        PDF Editor Shell          |
                          | (Document Model, Page View,     |
                          |  Selection Tools, Undo Manager) |
                          +---------------+------------------+
                                          |
                               ITextReconstructionEngine
                                          |
+=========================================================================+
|                    TextReconstructionEngine                              |
|                                                                         |
|  +------------------+     +---------------------+    +-----------------+|
|  | OcrPipeline      |     | RegionReconstructor |    | ImageReplacer   ||
|  | (Tesseract HOCR) |     | (Approach B)        |    | (Approach C)    ||
|  +--------+---------+     +---+--------+--------+    +-------+---------+|
|           |                   |        |                     |          |
|  +--------v---------+  +-----v--+ +---v----------+  +-------v-------+ |
|  | HocrParser       |  |ImageMasker| |FontMatcher |  | BitmapCompositor| |
|  | (XML -> struct)  |  |(libvips) | |(FreeType+HB)|  | (libvips)     | |
|  +------------------+  +-----+---+ +---+----------+  +---------------+ |
|                              |         |                                |
|  +---------------------------+---------+----+                          |
|  |            Shared Infrastructure          |                          |
|  |  +----------------+  +----------------+   |                          |
|  |  |CoordMapper     |  |BgColorDetector |   |                          |
|  |  |(pixel<->PDF)   |  |(libvips sample)|   |                          |
|  |  +----------------+  +----------------+   |                          |
|  |  +----------------+  +----------------+   |                          |
|  |  |QualityAssessor |  |PdfContentWriter|   |                          |
|  |  |(confidence eval)|  |(MuPDF + QPDF)  |   |                          |
|  |  +----------------+  +----------------+   |                          |
|  +-------------------------------------------+                          |
+=========================================================================+
```

### 2.2 Component Responsibilities

| Component | Responsibility | Primary Library |
|-----------|---------------|-----------------|
| **OcrPipeline** | Invoke Tesseract, cache HOCR results per page, manage OCR lifecycle | Tesseract 5.x |
| **HocrParser** | Parse HOCR XML into structured C++ types; extract region subsets | pugixml |
| **QualityAssessor** | Evaluate OCR confidence and background complexity to choose approach | Custom analysis |
| **RegionReconstructor** | Orchestrate Approach B: mask image, place text objects, build content stream | All |
| **ImageReplacer** | Orchestrate Approach C: render text to bitmap, composite over source image | FreeType, libvips |
| **ImageMasker** | Cut/fill regions in the source image (background fill, feathered edges) | libvips |
| **FontMatcher** | Determine visual text properties from OCR+image, select best font | FreeType, HarfBuzz |
| **BgColorDetector** | Sample and analyze the background color behind text in a region | libvips |
| **CoordMapper** | Bidirectional transform between pixel coordinates and PDF user space | Custom math |
| **PdfContentWriter** | Emit PDF content stream operators and page structure for the mixed page | MuPDF, QPDF |
| **BitmapCompositor** | Render shaped text to a bitmap at correct DPI using matched font | FreeType, HarfBuzz, libvips |

### 2.3 Data Flow -- Approach B (Primary)

```
User selects text region on scanned page view
    |
    v
[1] OcrPipeline::getPageOcr(pageIndex)
    - Returns cached HocrDocument or runs Tesseract
    - HOCR contains full page: blocks, paragraphs, lines, words
    |
    v
[2] HocrParser::extractRegion(hocrDoc, selectionBboxPdf)
    - CoordMapper converts PDF selection rect to pixel rect
    - Filters HOCR words/lines that intersect the pixel rect
    - Returns RegionTextData: ordered lines of {text, bbox, confidence, baseline, x_size}
    |
    v
[3] QualityAssessor::evaluate(regionData, sourceImageRegion)
    - Checks mean/min word confidence
    - Analyzes background complexity (gradient? texture? image behind text?)
    - Checks for overlapping non-text content
    - Returns QualityVerdict: {approach, confidence, warnings}
    |
    +--- verdict.approach == Approach::B (quality acceptable) ---+
    |                                                             |
    v                                                             |
[4] FontMatcher::matchRegion(regionData, sourceImageRegion)       |
    - Estimates: font size (pt), weight, italic, serif/sans,     |
      text color (RGB), line spacing                              |
    - Searches system + bundled fonts via PANOSE + metrics        |
    - Returns FontMatch: {font, sizePt, color, lineSpacing,      |
      confidence}                                                 |
    |                                                             |
    v                                                             |
[5] BgColorDetector::detect(sourceImage, regionPixelBbox)         |
    - Samples edges and inter-line gaps                           |
    - Returns BgColor: {r, g, b, isUniform, gradientInfo}        |
    |                                                             |
    v                                                             |
[6] ImageMasker::maskRegion(sourceImage, regionPixelBbox,         |
                            bgColor, featherRadiusPx)             |
    - Fills region with detected background color                 |
    - Applies feathered (anti-aliased) edges at boundary          |
    - Returns MaskedImage (modified copy of the page image)       |
    |                                                             |
    v                                                             |
[7] User edits text in an overlay text editor widget              |
    - Initial content = OCR'd text from RegionTextData            |
    - User modifies the text                                      |
    - Returns editedText: UTF-8 string                            |
    |                                                             |
    v                                                             |
[8] PdfContentWriter::writeReconstructedPage(                     |
        pageIndex, maskedImage, editedText, fontMatch,            |
        regionPdfBbox, bgColor)                                   |
    - Replaces the page image XObject with the masked image       |
    - Adds font resource to page Resources dictionary             |
    - Appends text-drawing operators to the content stream:       |
        BT /F1 12 Tf <color> rg <x> <y> Td (text) Tj ET         |
    - Wraps operations in q/Q save/restore pairs                  |
    - Returns ReconstructionResult                                |
    |                                                             |
    v                                                             |
[9] Page is re-rendered; user sees their edited text in place     |
```

### 2.4 Data Flow -- Approach C (Fallback)

```
[3] QualityAssessor returns verdict.approach == Approach::C
    |
    v
[4] FontMatcher::matchRegion(...) -- same as Approach B
    |
    v
[5] BitmapCompositor::renderText(editedText, fontMatch,
        regionPixelBbox, dpi)
    - Shapes text with HarfBuzz
    - Rasterizes glyphs with FreeType at the target DPI
    - Renders to an RGBA bitmap with transparent background
    - Returns renderedBitmap
    |
    v
[6] ImageMasker::compositeOver(sourceImage, renderedBitmap,
        regionPixelBbox, bgColor)
    - Fills region with background color first
    - Alpha-composites the rendered text bitmap on top
    - Returns compositedImage (full page image with text replaced)
    |
    v
[7] PdfContentWriter::replacePageImage(pageIndex, compositedImage)
    - Replaces the page's image XObject with the composited image
    - No text objects added (page remains image-only)
    - Returns ReconstructionResult
```

---

## 3. OCR-to-Editable Pipeline

### 3.1 HOCR Output Structure

Tesseract's HOCR output is hierarchical HTML/XML. The relevant levels:

```xml
<div class='ocr_page' title='image "page.png"; bbox 0 0 2550 3300; ppageno 0; scan_res 300 300'>
  <div class='ocr_carea' title="bbox 150 200 2400 600">           <!-- content area / block -->
    <p class='ocr_par' title="bbox 150 200 2400 400">              <!-- paragraph -->
      <span class='ocr_line' title="bbox 150 200 2400 240;
            baseline 0.002 -7; x_size 28.5; x_descenders 5.5; x_ascenders 7">
        <span class='ocrx_word' title='bbox 150 208 280 236; x_wconf 95;
              x_fsize 14'>The</span>
        <span class='ocrx_word' title='bbox 295 208 440 240; x_wconf 92;
              x_fsize 14'>quick</span>
        <span class='ocrx_word' title='bbox 455 208 560 236; x_wconf 97;
              x_fsize 14'>brown</span>
        <!-- ... more words ... -->
      </span>
      <span class='ocr_line' title="bbox 150 260 2400 300; ...">
        <!-- second line of paragraph -->
      </span>
    </p>
  </div>
</div>
```

Key HOCR properties extracted per element:

| Property | Source | Meaning |
|----------|--------|---------|
| `bbox x0 y0 x1 y1` | All elements | Bounding box in pixel coordinates (origin = top-left of image) |
| `x_wconf N` | `ocrx_word` | Word confidence 0-100 |
| `x_fsize N` | `ocrx_word` | Estimated font size in pixels at scan DPI |
| `x_size N` | `ocr_line` | Line x-height in pixels |
| `x_descenders N` | `ocr_line` | Descender depth in pixels |
| `x_ascenders N` | `ocr_line` | Ascender height in pixels |
| `baseline a b` | `ocr_line` | Baseline: y = a*x + b (slope and offset from bbox bottom) |
| `scan_res X Y` | `ocr_page` | DPI of the scanned image |

### 3.2 Parsed Data Structures

```cpp
struct HocrWord {
    std::u8string text;                // UTF-8 text content
    PixelRect bbox;                    // {x0, y0, x1, y1} in image pixels
    int confidence;                    // 0-100
    float fontSizePx;                  // estimated font size in pixels
};

struct HocrLine {
    PixelRect bbox;
    float baselineSlope;               // 'a' in y = ax + b
    float baselineOffset;              // 'b' in y = ax + b
    float xSizePx;                     // x-height in pixels
    float descendersPx;
    float ascendersPx;
    std::vector<HocrWord> words;
};

struct HocrParagraph {
    PixelRect bbox;
    std::string lang;                  // detected language (e.g., "eng")
    std::vector<HocrLine> lines;
};

struct HocrBlock {
    PixelRect bbox;
    std::vector<HocrParagraph> paragraphs;
};

struct HocrDocument {
    int pageWidth;                     // image width in pixels
    int pageHeight;                    // image height in pixels
    int scanResX;                      // horizontal DPI
    int scanResY;                      // vertical DPI
    std::vector<HocrBlock> blocks;
};
```

### 3.3 Transformation: HOCR to Editable Text Objects

The pipeline from HOCR data to PDF text operators:

```
HocrLine (pixel coords, pixel font size)
    |
    +-- CoordMapper::pixelToPdf(bbox) --> PdfRect (PDF user space coords)
    |
    +-- CoordMapper::pixelSizeToPdfPt(fontSizePx, scanDpi)
    |       --> fontSizePt (e.g., 14.0 for 14px at 72 dpi,
    |                        or 3.36pt for 14px at 300 dpi)
    |
    +-- FontMatcher::selectFont(fontSizePt, detectedWeight,
    |       detectedSerif, detectedItalic)
    |       --> selectedFont, adjustedSizePt
    |
    +-- HarfBuzz shape(selectedFont, editedText)
    |       --> positioned glyph sequence with advances
    |
    +-- PDF text operators:
            BT
            /F1 <sizePt> Tf
            <r> <g> <b> rg
            <baselineX> <baselineY> Td
            <glyph sequence via TJ operator with kerning>
            ET
```

The critical transformation here: Tesseract reports font size in pixels at the scan DPI. To get PDF points:

```
fontSizePt = fontSizePx * 72.0 / scanDpi
```

For a 14px font at 300 DPI scan, that yields 3.36pt -- which is the correct size when the PDF page is set up to display the image at its physical size.

### 3.4 Handling the Invisible Text Layer

Before any editing, the TRE can embed an invisible text layer for the entire page (for search/select). This follows the standard OCRmyPDF approach:

```
% Invisible text rendering mode (mode 3 = invisible)
BT
3 Tr                           % Set text rendering mode to invisible
/F1 3.36 Tf                   % Font at correct mapped size
150.0 3052.0 Td               % Position (PDF coords, origin bottom-left)
(The quick brown) Tj           % Text string
ET
```

This text layer is placed in the content stream ABOVE the image (drawn after the image operator). It is invisible but selectable and searchable. The coordinate mapping (Section 6) ensures each word aligns visually with its image counterpart.

---

## 4. Image Masking Strategy

### 4.1 The Core Problem

When the user edits text in a region of a scanned page, we need to:
1. Remove the old text appearance from the image (it would show through behind our new text objects)
2. Fill the gap with the background color so the area looks clean
3. Handle the boundary carefully so the mask does not create visible artifacts

### 4.2 Masking Pipeline

```
Source Page Image (e.g., 2550x3300 px at 300 DPI)
    |
    v
[A] Determine mask region
    - Start with the HOCR block/paragraph bounding box
    - Expand by padding (default: 4px each side) to cover anti-aliased glyph edges
    - Snap to the line boundaries from HOCR (top of first line's ascender
      to bottom of last line's descender)
    - Clip to page bounds
    |
    v
[B] Detect background color (see Section 8)
    - Sample the detected background from the region's periphery
    - Determine if background is uniform or has gradient/texture
    |
    v
[C] Create mask fill
    - If background is uniform:
        Fill the entire mask region with the solid background color
    - If background has a gradient:
        Sample a column/row of pixels along the edges, interpolate a gradient
        fill across the mask region
    - If background is textured/complex (e.g., watermark behind text):
        This triggers Approach C fallback (see Section 11)
    |
    v
[D] Apply feathered edge blending
    - At the boundary of the mask region, blend between the original image
      and the fill using a gaussian falloff
    - Feather radius: 2-3 pixels (at 300 DPI this is barely visible)
    - Implementation via libvips:
        1. Create a binary mask (1 inside region, 0 outside)
        2. Apply gaussian blur to the mask (sigma = featherRadius)
        3. Use the blurred mask as alpha for compositing:
           result = fill * blurredMask + original * (1 - blurredMask)
    |
    v
[E] Output: MaskedImage
    - The original page image with the text region filled with background color
    - Feathered edges blend seamlessly into the surrounding image
```

### 4.3 libvips Implementation Sketch

```cpp
// Pseudocode using libvips C API
VipsImage* maskRegion(VipsImage* source, PixelRect region,
                      RgbColor bgColor, int featherRadius) {
    // 1. Create the background fill (same size as source)
    VipsImage* bgFill = vips_image_new_from_memory(
        /* solid color buffer */, source->Xsize, source->Ysize, 3, VIPS_FORMAT_UCHAR);

    // 2. Create binary mask: 255 inside region, 0 outside
    VipsImage* binaryMask = createRectMask(source->Xsize, source->Ysize, region);

    // 3. Blur the mask for feathered edges
    VipsImage* blurredMask;
    vips_gaussblur(binaryMask, &blurredMask, featherRadius, NULL);

    // 4. Composite: source * (1 - mask) + bgFill * mask
    //    Using vips_ifthenelse with the blurred mask as the condition
    VipsImage* result;
    vips_ifthenelse(blurredMask, bgFill, source, &result,
                    "blend", TRUE, NULL);

    return result;
}
```

### 4.4 Handling Text That Overlaps Non-Text Content

Scanned pages sometimes have text printed over logos, images, watermarks, or ruled lines. The QualityAssessor detects this by analyzing pixel variance within the HOCR word bounding boxes:

```
For each HocrWord in the region:
    Extract the word's bounding box from the source image
    Compute color histogram of pixels outside the glyph strokes
    If histogram has multiple distinct peaks (bimodal/multimodal):
        The text overlaps non-text content
        Set region.hasComplexBackground = true
```

When `hasComplexBackground == true`:

- **If the non-text content is below the text (e.g., horizontal rule, watermark):** Approach B can still work. The masking fills with interpolated background, and the non-text content is lost in the masked region. This is acceptable if the non-text content is minor. The user is warned via the quality assessment.

- **If the non-text content is significant (e.g., text over a photograph):** Fall back to Approach C. Image compositing can handle this because the rendered text bitmap is alpha-blended, preserving the underlying content outside of glyph strokes.

### 4.5 Anti-Aliasing at Region Boundaries

The feathered-edge approach (Section 4.2, step D) handles the macro boundary. But there is also a micro-boundary issue: at the edge of the mask region, partially visible characters from adjacent text may be clipped. To handle this:

1. Expand the mask region to the nearest full-line boundary (using HOCR line bboxes). This prevents cutting through a line of text horizontally.
2. Vertically, expand to the nearest inter-paragraph gap or block boundary. This prevents cutting through a column of text vertically.
3. If a word straddles the mask boundary (its bbox partially inside and partially outside), include it fully. The mask region is expanded to encompass it.

This "snap to natural boundaries" approach means the mask is always a clean rectangle containing complete lines of text, never slicing through individual characters.

---

## 5. Font Matching for Scanned Text

### 5.1 The Challenge

Scanned text has no font metadata. We must infer the font's visual characteristics entirely from the rasterized image and OCR metadata, then find the best matching installed font.

### 5.2 Property Detection Pipeline

```
+------------------------------------------------------------------+
|                   Font Property Detection                         |
+------------------------------------------------------------------+
|                                                                  |
|  [From HOCR]                    [From Image Analysis]            |
|  +---------------------------+  +-----------------------------+  |
|  | x_fsize -> font size (px) |  | Stroke width analysis       |  |
|  | x_size  -> x-height (px)  |  |   -> weight estimate        |  |
|  | baseline slope/offset     |  |   (thin/regular/bold/black) |  |
|  | x_ascenders, x_descenders |  |                             |  |
|  +---------------------------+  | Stroke angle analysis       |  |
|                                 |   -> italic detection        |  |
|                                 |   (upright vs ~12deg slant) |  |
|                                 |                             |  |
|                                 | Serif detection             |  |
|                                 |   (terminal stroke analysis)|  |
|                                 |                             |  |
|                                 | Foreground color sampling   |  |
|                                 |   -> text color (RGB)       |  |
|                                 |                             |  |
|                                 | Inter-line gap measurement  |  |
|                                 |   -> line spacing ratio     |  |
|                                 +-----------------------------+  |
|                                                                  |
+------------------------------+-----------------------------------+
                               |
                               v
              +--------------------------------+
              | ScannedTextProperties           |
              | {                               |
              |   fontSizePt: 12.0              |
              |   weight: FontWeight::Regular   |
              |   isItalic: false               |
              |   isSerif: true                 |
              |   textColor: {0, 0, 0}          |
              |   lineSpacingRatio: 1.2         |
              |   xHeightRatio: 0.48            |
              |   ascenderRatio: 0.72           |
              |   descenderRatio: 0.28          |
              | }                               |
              +--------------------------------+
```

### 5.3 Detailed Detection Methods

**Font Size:**
```
fontSizePt = hocrLine.xSizePx * 72.0 / scanDpi

// Cross-validate with x_fsize from word level:
// If Tesseract provides x_fsize for words, average across the line.
// If not, derive from line x_size + ascenders + descenders:
//   totalHeightPx = ascendersPx + xSizePx + descendersPx
//   fontSizePt = totalHeightPx * 72.0 / scanDpi
```

**Weight Detection (stroke width analysis):**
```
For each representative glyph in the region (sample 'e', 'o', 'n', 'a'):
    1. Extract glyph bounding box from image
    2. Binarize (Otsu threshold)
    3. Compute horizontal stroke width:
       - Scan horizontal lines through vertical center of glyph
       - Measure average run-length of foreground pixels
    4. Compute stroke-width-to-height ratio:
       strokeRatio = avgStrokeWidth / glyphHeight
    5. Classify:
       strokeRatio < 0.10 -> Thin/Light (weight ~300)
       strokeRatio 0.10-0.14 -> Regular (weight 400)
       strokeRatio 0.14-0.19 -> Medium/SemiBold (weight 500-600)
       strokeRatio >= 0.19 -> Bold/Black (weight 700+)
```

**Italic Detection (stroke angle):**
```
For vertical strokes in sample glyphs ('l', 'h', 'd', 'b'):
    1. Extract glyph, binarize
    2. Find the principal axis of vertical strokes via Hough transform
       or moment analysis
    3. Measure angle from vertical:
       angle < 3deg -> Upright
       angle 3-8deg -> Oblique (treat as italic)
       angle >= 8deg -> Italic
```

**Serif Detection:**
```
For sample glyphs with clear terminals ('I', 'T', 'i', 'l'):
    1. Extract glyph, binarize
    2. Examine top and bottom terminal regions:
       - If horizontal extent at terminal > stroke width * 1.3: serif present
       - If terminals taper to stroke width: sans-serif
    3. Majority vote across sample glyphs
```

**Text Color:**
```
1. For all word bounding boxes in the region:
    - Extract pixels
    - Binarize to identify foreground (ink) pixels
    - Compute median RGB of foreground pixels
2. Result is the dominant text color
   (median is robust to anti-aliasing artifacts at glyph edges)
```

**Line Spacing:**
```
lineSpacingRatio = (line[i+1].baselineY - line[i].baselineY) / fontSizePx
// Typical values: 1.0 (single), 1.15 (default), 1.5 (1.5x), 2.0 (double)
```

### 5.4 Font Selection Algorithm

Once we have ScannedTextProperties, the font matching pipeline (from the existing architecture) is invoked with an emphasis on visual similarity:

```
[1] Serif/Sans Classification
    - Divides the candidate pool immediately (eliminates ~50% of fonts)

[2] Weight + Italic Filtering
    - Filter to fonts within +/- 100 of detected weight
    - Filter to italic/upright matching detected style

[3] PANOSE Distance Scoring
    - For remaining candidates, compute PANOSE weighted distance
    - Weight the PANOSE digits most relevant to visual similarity at text size:
        - Weight (digit 3): weight 3.0
        - Proportion (digit 4): weight 2.0
        - Contrast (digit 5): weight 1.5
        - Serif style (digit 2): weight 2.5
        - x-height (digit 10): weight 2.0

[4] Metric Matching (tiebreaker)
    - Compare x-height ratio, ascender ratio, cap-height ratio
    - Fonts whose metrics most closely match the detected ratios score higher

[5] Unicode Coverage Check
    - Verify the selected font covers all codepoints in the OCR'd text
    - If not, try the next best candidate

[6] Fallback Chain
    - If no good match found:
        Serif text -> Noto Serif -> Liberation Serif -> DejaVu Serif
        Sans text  -> Noto Sans  -> Liberation Sans  -> DejaVu Sans
```

### 5.5 Font Match Confidence

The FontMatcher returns a confidence score (0.0-1.0) indicating how well the selected font matches the detected properties:

```
fontMatchConfidence =
    0.30 * weightMatchScore +       // How close the weight is
    0.20 * serifMatchScore +        // Serif classification correctness
    0.15 * xHeightMatchScore +      // x-height ratio similarity
    0.15 * proportionMatchScore +   // Width proportion similarity
    0.10 * italicMatchScore +       // Italic detection correctness
    0.10 * panoseDistanceScore      // Overall PANOSE distance

// If fontMatchConfidence < 0.5, warn the user that the
// replacement font may look noticeably different from the original.
```

---

## 6. Coordinate System

### 6.1 The Two Coordinate Spaces

**Pixel Space (HOCR / Image):**
- Origin: top-left corner of the rasterized page image
- Units: pixels
- Y-axis: increases downward
- Range: (0,0) to (imageWidth-1, imageHeight-1)
- Example: a word bbox from HOCR = `(150, 208, 280, 236)`

**PDF User Space:**
- Origin: bottom-left corner of the page
- Units: points (1 point = 1/72 inch)
- Y-axis: increases upward
- Range: (0,0) to (pageWidthPt, pageHeightPt)
- Defined by the page's MediaBox (typically `[0 0 612 792]` for US Letter)

### 6.2 The Transform

For a scanned page, there is a direct relationship between the image's pixel coordinates and the PDF page's user space, because the scanned image is placed to fill the entire page.

```
Given:
    imageWidth, imageHeight     (pixels, from HOCR / image file)
    scanDpiX, scanDpiY          (DPI, from HOCR or image metadata)
    pageMediaBox                (PDF rect, e.g., [0, 0, 612, 792] points)

The page dimensions in points should match the physical dimensions:
    expectedPageWidthPt  = imageWidth  * 72.0 / scanDpiX
    expectedPageHeightPt = imageHeight * 72.0 / scanDpiY

However, the actual MediaBox may differ (the image may be scaled to fit).
So we compute scale factors:

    scaleX = pageMediaBox.width  / imageWidth
    scaleY = pageMediaBox.height / imageHeight

Pixel-to-PDF transform:
    pdfX = pixelX * scaleX + pageMediaBox.x0
    pdfY = (imageHeight - pixelY) * scaleY + pageMediaBox.y0
    //     ^^^^^^^^^^^^^^^^^^^^^^
    //     Y-axis flip (pixel Y increases down, PDF Y increases up)

PDF-to-Pixel transform (inverse):
    pixelX = (pdfX - pageMediaBox.x0) / scaleX
    pixelY = imageHeight - (pdfY - pageMediaBox.y0) / scaleY
```

### 6.3 Handling Rotation and CropBox

Scanned pages may have a `/Rotate` attribute (90, 180, 270) and/or a `/CropBox` that differs from the `/MediaBox`. The CoordMapper must account for both:

```
[1] Read the effective page box (CropBox if present, else MediaBox)
[2] Read the Rotate value
[3] Apply rotation to the coordinate transform:
    - Rotate 0: standard transform (above)
    - Rotate 90: swap X/Y, adjust origin
    - Rotate 180: invert both axes
    - Rotate 270: swap X/Y, adjust origin (opposite direction)
[4] If the image is placed via a content stream matrix (cm operator)
    that includes scaling or translation, extract that matrix and
    incorporate it into the transform.
```

### 6.4 Extracting the Image Placement Matrix

In practice, the scanned page's content stream looks like:

```
q
<width> 0 0 <height> <tx> <ty> cm    % Image placement matrix
/Im0 Do                               % Draw image XObject
Q
```

The `cm` operator provides the exact transformation matrix. The CoordMapper should parse this from the content stream (via MuPDF's content stream interpretation) to get the precise image-to-page mapping rather than assuming the image fills the entire MediaBox.

```cpp
struct ImagePlacement {
    PdfRect bbox;           // Where the image appears on the page (in PDF points)
    double scaleX, scaleY;  // Pixels-to-points scale
    double tx, ty;          // Translation offset
    int rotation;           // Degrees (0, 90, 180, 270)
};

// Extract from content stream:
// The cm operator gives us [a b c d e f] which is:
//   a = scaleX (or involves rotation)
//   d = scaleY (or involves rotation)
//   e = tx (translation X)
//   f = ty (translation Y)
// For a simple scaled placement: a=widthPt, d=heightPt, e=0, f=0
```

### 6.5 CoordMapper Caching

The CoordMapper computes and caches the affine transform per page-image pair:

```cpp
class CoordMapper {
    // Cache key: (pageIndex, imageObjectId)
    // Value: the precomputed AffineTransform
    std::unordered_map<PageImageKey, AffineTransform> cache_;

public:
    // Compute or retrieve cached transform
    AffineTransform const& getTransform(int pageIndex, pdf_obj* imageObj);

    // Convert coordinates
    PdfPoint pixelToPdf(PixelPoint px, AffineTransform const& tf);
    PixelPoint pdfToPixel(PdfPoint pt, AffineTransform const& tf);
    PdfRect pixelRectToPdf(PixelRect px, AffineTransform const& tf);
    PixelRect pdfRectToPixel(PdfRect pt, AffineTransform const& tf);

    // Convert sizes (not positions -- no translation, just scaling)
    double pixelSizeToPdfPt(double sizePx, double dpi);
    double pdfPtToPixelSize(double sizePt, double dpi);
};
```

---

## 7. Mixed Content Page Structure

### 7.1 Before Editing: Image-Only Page

A typical scanned page in PDF:

```
Page Dictionary:
  /Type /Page
  /MediaBox [0 0 612 792]
  /Contents <stream ref>
  /Resources <<
    /XObject << /Im0 <image ref> >>
  >>

Content Stream:
  q
  612 0 0 792 0 0 cm       % Scale image to fill page
  /Im0 Do                   % Draw the scanned page image
  Q
```

### 7.2 After Adding Invisible OCR Layer (Search Only)

```
Content Stream:
  % --- Layer 1: Original scanned image ---
  q
  612 0 0 792 0 0 cm
  /Im0 Do
  Q

  % --- Layer 2: Invisible OCR text (for search/select) ---
  q
  BT
  3 Tr                              % Rendering mode 3 = invisible
  /OcrFont 3.36 Tf                  % Tiny font matching OCR pixel size
  1 0 0 1 36.0 756.0 Tm            % Position first word
  (The quick brown fox) Tj
  1 0 0 1 36.0 748.0 Tm            % Next line
  (jumps over the lazy dog) Tj
  ET
  Q
```

### 7.3 After Editing One Region (Approach B)

This is the critical transformation. The page now has three layers:

```
Page Dictionary:
  /Type /Page
  /MediaBox [0 0 612 792]
  /Contents <stream ref>
  /Resources <<
    /XObject <<
      /Im0 <MASKED image ref>       % <-- Original image WITH the edited
                                     %     region filled with background color
    >>
    /Font <<
      /F1 <embedded font ref>        % Font for the reconstructed text
      /OcrFont <OCR invisible font>  % Font for invisible OCR layer
    >>
  >>

Content Stream:
  % --- Layer 1: Masked scanned image ---
  % (The image now has the edited region filled with background color.
  %  Where text used to be, there is now a clean rectangle of bg color.)
  q
  612 0 0 792 0 0 cm
  /Im0 Do                            % Draw the MASKED image
  Q

  % --- Layer 2: Reconstructed visible text ---
  % (Real PDF text objects placed precisely over the masked region)
  q
  BT
  0 Tr                               % Rendering mode 0 = normal (visible)
  /F1 12 Tf                          % Matched font at detected size
  0 0 0 rg                           % Matched text color (black)
  1 0 0 1 108.0 584.0 Tm            % Position on baseline (PDF coords)
  [(The quick br) -15 (own f) 10 (ox)] TJ   % Text with kerning via TJ
  1 0 0 1 108.0 570.0 Tm            % Next line
  [(jumps o) 20 (ver the lazy dog)] TJ
  ET
  Q

  % --- Layer 3: Invisible OCR text for unedited regions ---
  % (Covers the rest of the page for search/select)
  q
  BT
  3 Tr
  /OcrFont 3.36 Tf
  % ... invisible text for all OTHER regions on the page ...
  ET
  Q
```

### 7.4 Why Replace the Image Rather Than Overlay

An alternative approach would be to leave the original image untouched and simply draw a white (or background-colored) rectangle over the text region, then draw the new text on top of that. This is simpler but has problems:

1. **Transparency:** If the page ever has transparency interactions (blend modes, soft masks), the original text pixels still participate in compositing. The overlaid rectangle may not perfectly occlude them.

2. **Printing:** Some print workflows flatten differently, and the order of operations between the image and the overlay rectangle may produce artifacts.

3. **File size:** Every edit adds a new rectangle drawing operation. Over many edits, the content stream grows. Replacing the image once (with a mask applied) is a one-time cost.

4. **PDF/A compliance:** Some PDF/A validators may flag overlay-based approaches as problematic if the original content is still technically present.

Therefore, the architecture modifies the actual image XObject data. The image is re-encoded with the masked regions filled. This is a clean, unambiguous representation.

### 7.5 Image Re-encoding Strategy

When the image is modified (masked regions filled), it must be re-encoded and written back as a PDF image XObject:

```
Original image XObject:
  /Type /XObject
  /Subtype /Image
  /Width 2550
  /Height 3300
  /BitsPerComponent 8
  /ColorSpace /DeviceRGB
  /Filter /DCTDecode          % JPEG compressed
  /Length <original size>

After masking:
  - Decode the original image stream (JPEG decompress via libjpeg-turbo)
  - Apply the mask fills (libvips operations)
  - Re-encode:
    If original was JPEG: re-encode as JPEG at same quality
    If original was lossless (Flate/PNG): re-encode as Flate
  - Replace the stream data in the image XObject
  - Update /Length
```

**Quality preservation:** For JPEG images, re-encoding introduces generational quality loss. To mitigate:
- Detect the original JPEG quality (via quantization table analysis) and re-encode at the same or slightly higher quality setting.
- If the masked region is small relative to the full image, the perceptible quality loss is negligible (most of the image data is unchanged, but JPEG re-encoding does not allow partial updates).
- For lossless source images, use Flate (zlib) compression to avoid any quality loss.

---

## 8. Background Color Detection

### 8.1 The Problem

To cleanly mask out text from the scanned image, we need to know what color to fill the masked region with. The "background" behind text in a scanned document is typically:
- Uniform white paper
- Off-white/cream/aged paper
- Colored paper (forms, sticky notes)
- Subtle gradient (scanner lighting variation)
- Textured (watermark, security pattern, ruled lines)

### 8.2 Sampling Strategy

```
Given: regionPixelBbox (the text region to mask)

[1] Edge Sampling
    - Sample 4 strips along the region edges (top, bottom, left, right)
    - Each strip is 3-5 pixels wide, running the length of the edge
    - These strips are OUTSIDE the text glyphs (at the region boundary)

    +------TTTTTTTTTTTTTTTTTTTTT------+
    L      text text text text text    R
    L      text text text text text    R
    L      text text text text text    R
    +------BBBBBBBBBBBBBBBBBBBBB------+

[2] Inter-Line Gap Sampling
    - Between each pair of HOCR lines, there is typically a gap
    - Sample horizontal strips in these gaps (they contain pure background)
    - These are the most reliable samples because they are within the region
      but between text lines

    +--------------------------------------+
    | text text text text text text text   |
    | ~~~~ inter-line gap (sample here) ~~~|
    | text text text text text text text   |
    | ~~~~ inter-line gap (sample here) ~~~|
    | text text text text text text text   |
    +--------------------------------------+

[3] Histogram Analysis
    - Combine all sampled pixels
    - Build an RGB histogram
    - Find the dominant peak (mode)
    - If one peak dominates (>80% of samples within dE < 5):
        Background is UNIFORM -> use the peak color
    - If two peaks exist (e.g., paper color + rule line color):
        Background has MINOR FEATURES -> use the larger peak
    - If histogram is broadly distributed:
        Background is COMPLEX -> flag for Approach C fallback
```

### 8.3 Gradient Detection

If the edge samples show a systematic color shift (e.g., left edge is slightly darker than right edge due to scanner light falloff):

```
[1] Compute mean color for each of the 4 edge strips
[2] Measure color difference (CIE dE76) between opposite edges:
    horizontalGradient = dE(leftMean, rightMean)
    verticalGradient = dE(topMean, bottomMean)
[3] If gradient > 3.0 dE:
    - Background has a detectable gradient
    - Compute a bilinear interpolation of the 4 corner colors
    - Use this interpolated gradient as the fill
[4] If gradient <= 3.0 dE:
    - Gradient is imperceptible
    - Use the overall mean color as a uniform fill
```

### 8.4 Output

```cpp
struct BackgroundInfo {
    RgbColor dominantColor;     // The single best background color
    bool isUniform;             // True if variance is low
    bool hasGradient;           // True if systematic color shift detected
    GradientInfo gradient;      // Corner colors for bilinear interpolation
    bool isComplex;             // True if textured/patterned/multi-feature
    float confidence;           // 0.0-1.0, how confident we are in the detection
};
```

---

## 9. Multi-Region Editing

### 9.1 The Problem

When the user edits multiple text regions on the same scanned page, each edit produces a masked area and a set of text objects. These must coexist without conflicting.

### 9.2 Architecture: Region Registry

Each page maintains a `ReconstructedRegionSet`:

```cpp
struct ReconstructedRegion {
    int regionId;                       // Unique ID within the page
    PdfRect pdfBbox;                    // Region bounds in PDF space
    PixelRect pixelBbox;                // Region bounds in pixel space
    Approach approach;                  // B (text objects) or C (bitmap composite)
    std::u8string originalText;         // OCR'd text before editing
    std::u8string editedText;           // User's edited text
    FontMatch fontMatch;                // Selected font + properties
    BackgroundInfo bgInfo;              // Detected background
    std::vector<PdfTextOp> textOps;     // Generated PDF text operators (Approach B)
    // For Approach C: the composited bitmap data is folded into the image
};

class ReconstructedRegionSet {
    int pageIndex_;
    std::vector<ReconstructedRegion> regions_;

public:
    // Add a new edited region. Validates no overlap with existing regions.
    RegionId addRegion(ReconstructedRegion region);

    // Update an existing region's text.
    void updateRegion(RegionId id, std::u8string_view newText);

    // Remove a region (revert to original image content).
    void removeRegion(RegionId id);

    // Get all regions for page assembly.
    std::span<ReconstructedRegion const> regions() const;

    // Check if a point/rect overlaps any existing region.
    std::optional<RegionId> hitTest(PdfPoint pt) const;
    bool overlaps(PdfRect rect) const;
};
```

### 9.3 Image Masking with Multiple Regions

When multiple regions are edited, all masks are applied to a single copy of the image:

```
Original Image
    |
    +-- Apply mask for Region 1 (fill with bg color 1)
    +-- Apply mask for Region 2 (fill with bg color 2)
    +-- Apply mask for Region 3 (fill with bg color 3)
    |
    v
Single Masked Image (all regions filled)
```

This is done in one pass to avoid re-encoding the JPEG multiple times:

```cpp
VipsImage* applyAllMasks(VipsImage* source,
                         std::span<ReconstructedRegion const> regions) {
    // Decode image once
    VipsImage* working = vips_copy(source);

    // Apply each mask region
    for (auto const& region : regions) {
        if (region.approach == Approach::B) {
            // Fill with detected background color (feathered edges)
            working = maskRegion(working, region.pixelBbox,
                                region.bgInfo.dominantColor, /*feather=*/3);
        } else if (region.approach == Approach::C) {
            // Composite the rendered text bitmap over the region
            working = compositeOver(working, region.renderedBitmap,
                                   region.pixelBbox, region.bgInfo.dominantColor);
        }
    }

    // Encode once: JPEG or Flate depending on original encoding
    return working;
}
```

### 9.4 Content Stream Assembly with Multiple Regions

```
Content Stream (multi-region page):

  % --- Layer 1: Masked image (all regions filled) ---
  q
  612 0 0 792 0 0 cm
  /Im0 Do
  Q

  % --- Layer 2: Reconstructed text for Region 1 ---
  q
  BT
  /F1 12 Tf  0 0 0 rg
  1 0 0 1 108.0 584.0 Tm
  [(Edited text for region 1)] TJ
  ET
  Q

  % --- Layer 3: Reconstructed text for Region 2 ---
  q
  BT
  /F2 10 Tf  0.2 0.2 0.2 rg
  1 0 0 1 72.0 400.0 Tm
  [(Edited text for region 2)] TJ
  ET
  Q

  % --- Layer 4: Invisible OCR for unedited regions ---
  q
  BT
  3 Tr  /OcrFont 3.36 Tf
  % ... invisible text for non-edited areas ...
  ET
  Q
```

Each reconstructed region gets its own `q`/`Q` graphics state block, isolating its font, color, and transform from other regions.

### 9.5 Overlap Prevention

The system prevents overlapping edits:

```
When user selects a new region to edit:
    1. Check ReconstructedRegionSet::overlaps(selectionRect)
    2. If overlap detected:
       - Option A: Merge the new selection with the existing region
                   (expand the region to encompass both)
       - Option B: Reject the selection and inform the user
                   "This area overlaps with an existing edit.
                    Please edit the existing region or revert it first."
    3. The default behavior is Option B (explicit is better than implicit)
```

### 9.6 Region Fonts

Different regions may require different fonts (e.g., a heading in bold sans-serif vs. body text in regular serif). Each region's FontMatch is independent:

```
Page Resources:
  /Font <<
    /F1 <NotoSerif-Regular>      % Region 1's font
    /F2 <NotoSans-Bold>          % Region 2's font
    /OcrFont <hidden-ocr-font>   % Invisible OCR layer font
  >>
```

Font names (/F1, /F2, etc.) are allocated sequentially by the PdfContentWriter as regions are added.

---

## 10. Undo/Redo Integration

### 10.1 Command Pattern Design

Each text reconstruction operation is encapsulated as a Command that can be executed and undone:

```cpp
// Base command interface (from existing undo system)
class ICommand {
public:
    virtual ~ICommand() = default;
    virtual void execute() = 0;
    virtual void undo() = 0;
    virtual std::u8string description() const = 0;
};

// Reconstructs a single region (Approach B or C)
class ReconstructRegionCommand : public ICommand {
    int pageIndex_;
    ReconstructedRegion region_;

    // Snapshot data for undo:
    std::vector<uint8_t> originalImageData_;   // Original image XObject stream
    std::vector<uint8_t> originalContentStream_; // Original page content stream
    std::optional<ReconstructedRegion> previousRegion_; // If updating existing

public:
    ReconstructRegionCommand(int pageIndex, ReconstructedRegion region);

    void execute() override {
        // 1. Snapshot current state (image data + content stream) for undo
        // 2. Apply image mask
        // 3. Write text objects to content stream
        // 4. Register region in ReconstructedRegionSet
        // 5. Invalidate page render cache
    }

    void undo() override {
        // 1. Restore original image XObject stream data
        // 2. Restore original content stream
        // 3. Unregister region from ReconstructedRegionSet
        // 4. Invalidate page render cache
    }

    std::u8string description() const override {
        return u8"Edit scanned text in region";
    }
};

// Edits the text within an already-reconstructed region
class EditReconstructedTextCommand : public ICommand {
    int pageIndex_;
    RegionId regionId_;
    std::u8string newText_;
    std::u8string previousText_;

    std::vector<uint8_t> previousImageData_;
    std::vector<uint8_t> previousContentStream_;

public:
    void execute() override {
        // 1. Snapshot current state
        // 2. Re-render the region with new text:
        //    a. Re-mask the image (region may change size if text is longer/shorter)
        //    b. Regenerate text operators
        //    c. Rewrite content stream
        // 3. Invalidate page render cache
    }

    void undo() override {
        // 1. Restore previous image data
        // 2. Restore previous content stream
        // 3. Restore previous text in region
        // 4. Invalidate page render cache
    }
};

// Reverts a region back to original (removes reconstruction)
class RevertRegionCommand : public ICommand {
    int pageIndex_;
    RegionId regionId_;
    ReconstructedRegion removedRegion_;  // For redo

    std::vector<uint8_t> previousImageData_;
    std::vector<uint8_t> previousContentStream_;

public:
    void execute() override {
        // 1. Remove region from ReconstructedRegionSet
        // 2. Regenerate image (re-mask all remaining regions from original)
        // 3. Regenerate content stream (without this region's text ops)
        // 4. Invalidate page render cache
    }

    void undo() override {
        // 1. Re-add the region
        // 2. Restore the image and content stream as they were
        // 3. Invalidate page render cache
    }
};
```

### 10.2 State Snapshot Strategy

The main concern with undo for image operations is memory. A full page image at 300 DPI in RGB can be ~24 MB uncompressed (2550 * 3300 * 3 bytes). Strategies to manage this:

**Strategy 1: Store compressed image diffs (preferred)**
```
Rather than storing the entire image for every undo step:
1. XOR the new image with the previous image
2. The XOR result is mostly zeros (only the masked region differs)
3. Compress the XOR result with zlib (compresses very well due to sparsity)
4. Store the compressed diff (~50-200 KB typically)

To undo:
1. Decompress the diff
2. XOR the current image with the diff to recover the previous image
```

**Strategy 2: Store the original image once + regenerate**
```
1. Keep the original (unmasked) image data stored once when the first
   region is reconstructed on a page
2. For each undo step, store only the region metadata (text, bbox, font)
3. To undo: regenerate the image by re-applying all remaining region
   masks to the original image

Trade-off: Undo is slower (requires re-masking) but uses minimal memory.
```

**Recommended:** Use Strategy 2 (store original + regenerate) for the initial implementation. It is simpler, uses less memory, and the regeneration time (~50-200ms for a typical page) is acceptable for undo operations.

### 10.3 Compound Commands

Some user actions produce compound operations:

```
"Edit region text" might involve:
    1. If region size changes (text is longer):
       - Expand the mask region
       - Re-detect background for expanded area
       - Re-mask the image
    2. Re-shape text with HarfBuzz
    3. Regenerate text operators
    4. Update content stream

All of these are grouped into a single CompoundCommand so that
Ctrl+Z reverts the entire operation atomically.
```

### 10.4 Undo Stack Integration

```cpp
class UndoManager {
    std::vector<std::unique_ptr<ICommand>> undoStack_;
    std::vector<std::unique_ptr<ICommand>> redoStack_;
    int maxDepth_ = 500;  // From existing architecture

public:
    void execute(std::unique_ptr<ICommand> cmd) {
        cmd->execute();
        undoStack_.push_back(std::move(cmd));
        redoStack_.clear();  // New action invalidates redo history
        if (undoStack_.size() > maxDepth_) {
            undoStack_.erase(undoStack_.begin());  // Drop oldest
        }
    }

    void undo() {
        if (undoStack_.empty()) return;
        auto cmd = std::move(undoStack_.back());
        undoStack_.pop_back();
        cmd->undo();
        redoStack_.push_back(std::move(cmd));
    }

    void redo() {
        if (redoStack_.empty()) return;
        auto cmd = std::move(redoStack_.back());
        redoStack_.pop_back();
        cmd->execute();
        undoStack_.push_back(std::move(cmd));
    }
};
```

---

## 11. Quality Thresholds and Fallback

### 11.1 Decision Matrix

The QualityAssessor evaluates multiple signals to decide between Approach B (real text) and Approach C (bitmap composite):

```
+-------------------------------+----------+--------+-----------------------------------+
| Signal                        | Threshold| Weight | Rationale                          |
+-------------------------------+----------+--------+-----------------------------------+
| Mean word confidence          | >= 80    | 0.30   | Below 80, OCR text is likely wrong |
| Min word confidence           | >= 50    | 0.15   | Any word below 50 is suspect       |
| Background uniformity         | >= 0.7   | 0.20   | Complex bg = bad masking result    |
| Font match confidence         | >= 0.5   | 0.15   | Poor font match = ugly result      |
| Text overlap with non-text    | == false | 0.10   | Overlapping content = masking fail |
| Region contains only text     | == true  | 0.10   | Tables, diagrams should not be     |
|   (no images, lines, etc.)   |          |        | reconstructed as text              |
+-------------------------------+----------+--------+-----------------------------------+
```

### 11.2 Scoring Algorithm

```cpp
struct QualityVerdict {
    Approach recommendedApproach;  // B or C
    float overallScore;            // 0.0 - 1.0
    std::vector<QualityWarning> warnings;
};

QualityVerdict QualityAssessor::evaluate(
    RegionTextData const& regionData,
    VipsImage* sourceImageRegion,
    BackgroundInfo const& bgInfo,
    FontMatch const& fontMatch)
{
    float score = 0.0f;

    // Mean confidence
    float meanConf = regionData.meanWordConfidence();
    float confScore = std::clamp((meanConf - 50.0f) / 50.0f, 0.0f, 1.0f);
    score += 0.30f * confScore;

    // Min confidence
    float minConf = regionData.minWordConfidence();
    float minConfScore = (minConf >= 50) ? 1.0f : minConf / 50.0f;
    score += 0.15f * minConfScore;

    // Background uniformity
    float bgScore = bgInfo.isComplex ? 0.0f :
                    bgInfo.hasGradient ? 0.6f : 1.0f;
    score += 0.20f * bgScore;

    // Font match confidence
    score += 0.15f * fontMatch.confidence;

    // Text overlap with non-text content
    bool hasOverlap = detectNonTextOverlap(regionData, sourceImageRegion);
    score += 0.10f * (hasOverlap ? 0.0f : 1.0f);

    // Region purity (text only, no embedded images/diagrams)
    bool isPureText = regionData.containsOnlyText();
    score += 0.10f * (isPureText ? 1.0f : 0.0f);

    QualityVerdict verdict;
    verdict.overallScore = score;

    // Decision threshold
    if (score >= 0.65f) {
        verdict.recommendedApproach = Approach::B;
    } else {
        verdict.recommendedApproach = Approach::C;
    }

    // Generate warnings
    if (meanConf < 80) {
        verdict.warnings.push_back({
            QualityWarning::LowConfidence,
            "OCR confidence is low; some words may be incorrect."
        });
    }
    if (bgInfo.isComplex) {
        verdict.warnings.push_back({
            QualityWarning::ComplexBackground,
            "The background behind this text is complex. "
            "Editing as an image overlay instead."
        });
    }
    if (fontMatch.confidence < 0.5f) {
        verdict.warnings.push_back({
            QualityWarning::PoorFontMatch,
            "Could not find a close font match. "
            "The replacement text may look different from the original."
        });
    }

    return verdict;
}
```

### 11.3 User Override

The user can override the automatic decision:

```
Quality assessment says Approach C, but user wants editable text:
    -> Show warning dialog:
       "The OCR quality for this region is low (65% average confidence).
        The result may have incorrect text or visual artifacts.
        [Use editable text anyway] [Use image replacement] [Cancel]"

If user chooses "Use editable text anyway":
    -> Proceed with Approach B, but:
       - Present the OCR text in the editor with low-confidence words highlighted
       - Let the user correct them before committing
```

### 11.4 Per-Word Fallback (Hybrid within a Region)

In some cases, most words in a region have high confidence but a few are low. Rather than falling back the entire region to Approach C, the system can use a per-word hybrid:

```
For each word in the region:
    If word.confidence >= 70:
        Include in the editable text (Approach B word)
    Else:
        Extract the word's pixel region from the original image
        Include as a small inline image in the PDF content stream
        (The word appears as its original scan; surrounding words are real text)

This is an ADVANCED optimization, not required for the initial implementation.
Flag it as a future enhancement.
```

---

## 12. C++ Interface Definitions

### 12.1 Core Engine Interface

```cpp
// file: include/pdfe/tre/ITextReconstructionEngine.h

#pragma once

#include <pdfe/tre/Types.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pdfe::tre {

/// The primary interface for the Text Reconstruction Engine.
/// This is the facade used by the PDF Editor shell.
class ITextReconstructionEngine {
public:
    virtual ~ITextReconstructionEngine() = default;

    /// Run OCR on a page (or return cached results).
    /// The invisible text layer is optionally embedded for search/select.
    /// @param pageIndex  Zero-based page index in the document.
    /// @param embedInvisibleLayer  If true, write invisible text into the PDF.
    /// @return The structured OCR result for the page.
    virtual HocrDocument ocrPage(int pageIndex,
                                 bool embedInvisibleLayer = true) = 0;

    /// Assess quality of a region for text reconstruction.
    /// Determines whether Approach B or C should be used.
    /// @param pageIndex   Page containing the region.
    /// @param regionPdf   Region bounds in PDF user space coordinates.
    /// @return Quality assessment with recommended approach and warnings.
    virtual QualityVerdict assessRegion(int pageIndex,
                                        PdfRect regionPdf) = 0;

    /// Extract the OCR'd text for a region (for display in the editor).
    /// @param pageIndex   Page containing the region.
    /// @param regionPdf   Region bounds in PDF user space coordinates.
    /// @return Structured text data: lines, words, confidence, font info.
    virtual RegionTextData extractRegionText(int pageIndex,
                                             PdfRect regionPdf) = 0;

    /// Match fonts for a region based on OCR and image analysis.
    /// @param pageIndex   Page containing the region.
    /// @param regionPdf   Region bounds in PDF user space coordinates.
    /// @return Best font match with properties and confidence.
    virtual FontMatch matchFont(int pageIndex,
                                PdfRect regionPdf) = 0;

    /// Reconstruct a region with edited text (Approach B).
    /// Masks the image, places real text objects, updates the page.
    /// @param pageIndex   Page to modify.
    /// @param regionPdf   Region bounds in PDF user space coordinates.
    /// @param editedText  The user's edited text (UTF-8).
    /// @param fontMatch   Font selection (from matchFont or user override).
    /// @return The command to execute (for undo/redo integration).
    virtual std::unique_ptr<ICommand> createReconstructCommand(
        int pageIndex,
        PdfRect regionPdf,
        std::u8string_view editedText,
        FontMatch const& fontMatch) = 0;

    /// Replace text as a bitmap composite (Approach C fallback).
    /// Renders text to bitmap and composites over the source image.
    /// @param pageIndex   Page to modify.
    /// @param regionPdf   Region bounds in PDF user space coordinates.
    /// @param editedText  The user's edited text (UTF-8).
    /// @param fontMatch   Font selection.
    /// @return The command to execute (for undo/redo integration).
    virtual std::unique_ptr<ICommand> createImageReplaceCommand(
        int pageIndex,
        PdfRect regionPdf,
        std::u8string_view editedText,
        FontMatch const& fontMatch) = 0;

    /// Revert a previously reconstructed region back to original.
    /// @param pageIndex   Page containing the region.
    /// @param regionId    ID of the region to revert.
    /// @return The command to execute (for undo/redo integration).
    virtual std::unique_ptr<ICommand> createRevertCommand(
        int pageIndex,
        RegionId regionId) = 0;

    /// Query all reconstructed regions on a page.
    /// @param pageIndex   Page to query.
    /// @return Span of active reconstructed regions.
    virtual std::vector<ReconstructedRegion> getRegions(
        int pageIndex) const = 0;

    /// Hit-test a point against reconstructed regions.
    /// @param pageIndex   Page to test.
    /// @param pointPdf    Point in PDF user space.
    /// @return Region ID if point is inside a reconstructed region.
    virtual std::optional<RegionId> hitTestRegion(
        int pageIndex,
        PdfPoint pointPdf) const = 0;
};

} // namespace pdfe::tre
```

### 12.2 Type Definitions

```cpp
// file: include/pdfe/tre/Types.h

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pdfe::tre {

// --- Geometry ---

struct PixelPoint { double x, y; };
struct PixelRect  { double x0, y0, x1, y1;
                    double width() const { return x1 - x0; }
                    double height() const { return y1 - y0; } };

struct PdfPoint   { double x, y; };
struct PdfRect    { double x0, y0, x1, y1;
                    double width() const { return x1 - x0; }
                    double height() const { return y1 - y0; } };

// --- Color ---

struct RgbColor   { uint8_t r, g, b; };
struct RgbaColor  { uint8_t r, g, b, a; };

// --- IDs ---

enum class RegionId : uint32_t {};

// --- Enums ---

enum class Approach {
    B_HybridReconstruction,   // Real text objects over masked image
    C_ImageReplacement        // Text rendered as bitmap, composited
};

enum class FontWeight {
    Thin       = 100,
    ExtraLight = 200,
    Light      = 300,
    Regular    = 400,
    Medium     = 500,
    SemiBold   = 600,
    Bold       = 700,
    ExtraBold  = 800,
    Black      = 900
};

// --- Quality Assessment ---

enum class QualityWarningType {
    LowConfidence,
    ComplexBackground,
    PoorFontMatch,
    NonTextOverlap,
    GradientBackground,
    MinConfidenceTooLow
};

struct QualityWarning {
    QualityWarningType type;
    std::string message;
};

struct QualityVerdict {
    Approach recommendedApproach;
    float overallScore;          // 0.0 - 1.0
    std::vector<QualityWarning> warnings;
};

// --- Background ---

struct GradientInfo {
    RgbColor topLeft, topRight, bottomLeft, bottomRight;
};

struct BackgroundInfo {
    RgbColor dominantColor;
    bool isUniform;
    bool hasGradient;
    GradientInfo gradient;
    bool isComplex;
    float confidence;            // 0.0 - 1.0
};

// --- Font Matching ---

struct ScannedTextProperties {
    float fontSizePt;
    FontWeight weight;
    bool isItalic;
    bool isSerif;
    RgbColor textColor;
    float lineSpacingRatio;      // e.g., 1.2 for 120% line spacing
    float xHeightRatio;          // x-height / total height
    float ascenderRatio;
    float descenderRatio;
};

struct FontMatch {
    std::string fontFamily;      // e.g., "Noto Serif"
    std::string fontStyle;       // e.g., "Regular", "Bold Italic"
    std::string fontFilePath;    // Absolute path to the font file
    int fontFaceIndex;           // Face index within the font file (TTC)
    float sizePt;                // Font size in points
    RgbColor color;              // Text color
    float lineSpacingRatio;      // Line spacing
    float confidence;            // 0.0-1.0: how well this matches the original
    ScannedTextProperties detectedProperties; // What we detected from the scan
};

// --- HOCR Structures ---

struct HocrWord {
    std::u8string text;
    PixelRect bbox;
    int confidence;              // 0-100
    float fontSizePx;
};

struct HocrLine {
    PixelRect bbox;
    float baselineSlope;
    float baselineOffset;
    float xSizePx;
    float descendersPx;
    float ascendersPx;
    std::vector<HocrWord> words;
};

struct HocrParagraph {
    PixelRect bbox;
    std::string lang;
    std::vector<HocrLine> lines;
};

struct HocrBlock {
    PixelRect bbox;
    std::vector<HocrParagraph> paragraphs;
};

struct HocrDocument {
    int pageWidth;
    int pageHeight;
    int scanResX;
    int scanResY;
    std::vector<HocrBlock> blocks;
};

// --- Region Editing ---

struct RegionTextData {
    PdfRect pdfBbox;
    PixelRect pixelBbox;
    std::vector<HocrLine> lines;
    std::u8string plainText;     // All text concatenated with newlines

    float meanWordConfidence() const;
    float minWordConfidence() const;
    bool containsOnlyText() const;
};

struct ReconstructedRegion {
    RegionId id;
    PdfRect pdfBbox;
    PixelRect pixelBbox;
    Approach approach;
    std::u8string originalText;
    std::u8string editedText;
    FontMatch fontMatch;
    BackgroundInfo bgInfo;
};

// --- Commands ---

class ICommand {
public:
    virtual ~ICommand() = default;
    virtual void execute() = 0;
    virtual void undo() = 0;
    virtual std::u8string description() const = 0;
};

} // namespace pdfe::tre
```

### 12.3 OCR Pipeline Interface

```cpp
// file: include/pdfe/tre/OcrPipeline.h

#pragma once

#include <pdfe/tre/Types.h>
#include <functional>
#include <memory>

namespace pdfe::tre {

/// Configuration for the OCR pipeline.
struct OcrConfig {
    std::string language = "eng";     // Tesseract language code
    std::string tessdataPath;         // Path to tessdata directory
    int ocrDpi = 300;                 // DPI for rasterization (if needed)
    bool useGpu = false;              // GPU acceleration (if available)
    int pageSegMode = 3;              // Tesseract PSM (3 = fully automatic)
};

/// Progress callback: (current step, total steps, status message)
using OcrProgressCallback = std::function<void(int, int, std::string_view)>;

/// The OCR pipeline: manages Tesseract lifecycle and caches results.
class IOcrPipeline {
public:
    virtual ~IOcrPipeline() = default;

    /// Initialize the OCR engine with the given configuration.
    virtual void initialize(OcrConfig const& config) = 0;

    /// Run OCR on a page, returning the parsed HOCR document.
    /// Results are cached; subsequent calls return the cached result.
    /// @param pageIndex     Zero-based page index.
    /// @param pageImageData Raw pixel data (RGB, row-major) for the page.
    /// @param imageWidth    Width in pixels.
    /// @param imageHeight   Height in pixels.
    /// @param dpi           DPI of the provided image.
    /// @param progress      Optional progress callback.
    virtual HocrDocument ocrPage(
        int pageIndex,
        std::span<uint8_t const> pageImageData,
        int imageWidth, int imageHeight, int dpi,
        OcrProgressCallback progress = nullptr) = 0;

    /// Check if a page has cached OCR results.
    virtual bool hasCachedResult(int pageIndex) const = 0;

    /// Get cached OCR result (returns nullopt if not cached).
    virtual std::optional<HocrDocument> getCachedResult(
        int pageIndex) const = 0;

    /// Clear cached results for a page (e.g., after image modification).
    virtual void invalidateCache(int pageIndex) = 0;

    /// Clear all cached results.
    virtual void invalidateAllCaches() = 0;
};

} // namespace pdfe::tre
```

### 12.4 Coordinate Mapper Interface

```cpp
// file: include/pdfe/tre/CoordMapper.h

#pragma once

#include <pdfe/tre/Types.h>

namespace pdfe::tre {

/// Affine transform for pixel <-> PDF coordinate conversion.
struct AffineTransform {
    double scaleX, scaleY;   // Scale factors
    double translateX, translateY; // Translation
    bool flipY;              // Whether Y axis is flipped (almost always true)
    int rotation;            // Page rotation in degrees (0, 90, 180, 270)
    double imageWidth, imageHeight; // Source image dimensions in pixels
    double pageWidth, pageHeight;   // PDF page dimensions in points
};

/// Bidirectional coordinate mapping between pixel space and PDF user space.
class ICoordMapper {
public:
    virtual ~ICoordMapper() = default;

    /// Compute (or retrieve cached) the affine transform for a page.
    /// Parses the content stream to find the image placement matrix.
    /// @param pageIndex   Zero-based page index.
    /// @return The computed affine transform.
    virtual AffineTransform const& getTransform(int pageIndex) = 0;

    /// Convert a pixel point to PDF user space.
    virtual PdfPoint pixelToPdf(PixelPoint px,
                                AffineTransform const& tf) const = 0;

    /// Convert a PDF point to pixel space.
    virtual PixelPoint pdfToPixel(PdfPoint pt,
                                  AffineTransform const& tf) const = 0;

    /// Convert a pixel rectangle to PDF rectangle.
    /// Handles Y-axis flip and coordinate reordering.
    virtual PdfRect pixelRectToPdf(PixelRect px,
                                   AffineTransform const& tf) const = 0;

    /// Convert a PDF rectangle to pixel rectangle.
    virtual PixelRect pdfRectToPixel(PdfRect pt,
                                     AffineTransform const& tf) const = 0;

    /// Convert a size in pixels to PDF points (no translation, just scale).
    /// @param sizePx  Size in pixels.
    /// @param dpi     The DPI of the source image.
    /// @return Size in PDF points.
    virtual double pixelSizeToPdfPt(double sizePx, double dpi) const = 0;

    /// Invalidate cached transform for a page (e.g., after page rotation).
    virtual void invalidateCache(int pageIndex) = 0;
};

} // namespace pdfe::tre
```

### 12.5 Image Masker Interface

```cpp
// file: include/pdfe/tre/ImageMasker.h

#pragma once

#include <pdfe/tre/Types.h>
#include <vips/vips8>
#include <memory>
#include <span>

namespace pdfe::tre {

/// Handles image masking and compositing for the text reconstruction engine.
class IImageMasker {
public:
    virtual ~IImageMasker() = default;

    /// Mask a rectangular region of the source image by filling it
    /// with the detected background color, using feathered edges.
    /// @param source         The source page image.
    /// @param region         Region to mask (pixel coordinates).
    /// @param bgColor        Background color to fill with.
    /// @param featherRadius  Gaussian blur radius for edge feathering (pixels).
    /// @return New image with the region masked.
    virtual vips::VImage maskRegion(
        vips::VImage const& source,
        PixelRect region,
        RgbColor bgColor,
        int featherRadius = 3) = 0;

    /// Mask a region with a gradient fill (for non-uniform backgrounds).
    /// @param source    The source page image.
    /// @param region    Region to mask (pixel coordinates).
    /// @param gradient  Corner colors for bilinear interpolation.
    /// @param featherRadius  Edge feathering radius.
    /// @return New image with the region gradient-filled.
    virtual vips::VImage maskRegionGradient(
        vips::VImage const& source,
        PixelRect region,
        GradientInfo const& gradient,
        int featherRadius = 3) = 0;

    /// Apply multiple region masks in a single pass (avoids repeated
    /// JPEG re-encoding for multi-region edits).
    /// @param source   The original (unmasked) page image.
    /// @param regions  All regions to mask with their background info.
    /// @return New image with all regions masked.
    virtual vips::VImage maskAllRegions(
        vips::VImage const& source,
        std::span<std::pair<PixelRect, BackgroundInfo> const> regions) = 0;

    /// Composite a rendered text bitmap over the source image
    /// (for Approach C: image-level replacement).
    /// @param source       The source page image.
    /// @param textBitmap   RGBA bitmap of rendered text (transparent bg).
    /// @param region       Where to place the bitmap (pixel coordinates).
    /// @param bgColor      Background color to fill before compositing.
    /// @return New image with the text bitmap composited.
    virtual vips::VImage compositeTextBitmap(
        vips::VImage const& source,
        vips::VImage const& textBitmap,
        PixelRect region,
        RgbColor bgColor) = 0;
};

} // namespace pdfe::tre
```

### 12.6 Font Matcher Interface

```cpp
// file: include/pdfe/tre/FontMatcher.h

#pragma once

#include <pdfe/tre/Types.h>
#include <vips/vips8>

namespace pdfe::tre {

/// Analyzes scanned text to determine visual properties and finds
/// the best matching installed font.
class IFontMatcher {
public:
    virtual ~IFontMatcher() = default;

    /// Detect text properties from OCR data and the source image.
    /// @param regionData    Structured OCR data for the region.
    /// @param regionImage   Cropped source image of the region.
    /// @param scanDpi       DPI of the source scan.
    /// @return Detected visual properties of the scanned text.
    virtual ScannedTextProperties detectProperties(
        RegionTextData const& regionData,
        vips::VImage const& regionImage,
        int scanDpi) = 0;

    /// Find the best matching font for the detected properties.
    /// Uses the font matching pipeline: exact name -> family+style ->
    /// PANOSE -> metrics -> Unicode coverage -> fallback.
    /// @param properties  Detected text properties.
    /// @param requiredCodepoints  Set of Unicode codepoints that the
    ///                            font must support.
    /// @return Best font match with confidence score.
    virtual FontMatch findMatch(
        ScannedTextProperties const& properties,
        std::vector<char32_t> const& requiredCodepoints) = 0;

    /// Convenience: detect properties and find match in one call.
    virtual FontMatch matchRegion(
        RegionTextData const& regionData,
        vips::VImage const& regionImage,
        int scanDpi) = 0;
};

} // namespace pdfe::tre
```

### 12.7 Background Color Detector Interface

```cpp
// file: include/pdfe/tre/BgColorDetector.h

#pragma once

#include <pdfe/tre/Types.h>
#include <vips/vips8>
#include <span>

namespace pdfe::tre {

/// Detects the background color behind text in a scanned image region.
class IBgColorDetector {
public:
    virtual ~IBgColorDetector() = default;

    /// Detect background color for a text region.
    /// Samples edges and inter-line gaps to determine the background.
    /// @param sourceImage   Full page image.
    /// @param regionBbox    Region bounds in pixel coordinates.
    /// @param lines         HOCR line data (for inter-line gap sampling).
    /// @return Background analysis result.
    virtual BackgroundInfo detect(
        vips::VImage const& sourceImage,
        PixelRect regionBbox,
        std::span<HocrLine const> lines) = 0;
};

} // namespace pdfe::tre
```

### 12.8 Quality Assessor Interface

```cpp
// file: include/pdfe/tre/QualityAssessor.h

#pragma once

#include <pdfe/tre/Types.h>
#include <vips/vips8>

namespace pdfe::tre {

/// Configuration for quality thresholds (tunable).
struct QualityThresholds {
    float approachBMinScore    = 0.65f;  // Minimum score for Approach B
    int   minMeanConfidence    = 80;     // Minimum mean word confidence
    int   minWordConfidence    = 50;     // Minimum per-word confidence
    float minBgUniformity      = 0.7f;   // Background uniformity threshold
    float minFontMatchConf     = 0.5f;   // Font match confidence threshold
};

/// Evaluates whether a region is suitable for Approach B reconstruction
/// or should fall back to Approach C.
class IQualityAssessor {
public:
    virtual ~IQualityAssessor() = default;

    /// Set quality thresholds (defaults are reasonable starting points).
    virtual void setThresholds(QualityThresholds const& thresholds) = 0;

    /// Evaluate a region for reconstruction quality.
    /// @param regionData      OCR text data for the region.
    /// @param regionImage     Cropped source image of the region.
    /// @param bgInfo          Detected background info.
    /// @param fontMatch       Best font match result.
    /// @return Verdict with recommended approach and quality warnings.
    virtual QualityVerdict evaluate(
        RegionTextData const& regionData,
        vips::VImage const& regionImage,
        BackgroundInfo const& bgInfo,
        FontMatch const& fontMatch) = 0;
};

} // namespace pdfe::tre
```

### 12.9 PDF Content Writer Interface

```cpp
// file: include/pdfe/tre/PdfContentWriter.h

#pragma once

#include <pdfe/tre/Types.h>
#include <span>
#include <string>
#include <vector>

// Forward declarations for MuPDF/QPDF types
struct fz_context;
struct pdf_document;
struct pdf_page;

namespace pdfe::tre {

/// Writes PDF content stream operators and manages page structure
/// for reconstructed pages.
class IPdfContentWriter {
public:
    virtual ~IPdfContentWriter() = default;

    /// Embed an invisible text layer for the entire page (for search/select).
    /// Uses text rendering mode 3 (invisible).
    /// @param pageIndex  Page to add the invisible layer to.
    /// @param hocrDoc    Full-page OCR result.
    virtual void embedInvisibleTextLayer(
        int pageIndex,
        HocrDocument const& hocrDoc) = 0;

    /// Replace the page's image XObject with a masked version.
    /// @param pageIndex       Page to modify.
    /// @param maskedImageData JPEG or Flate-compressed image data.
    /// @param imageWidth      Image width in pixels.
    /// @param imageHeight     Image height in pixels.
    /// @param encoding        "DCTDecode" for JPEG, "FlateDecode" for PNG/zlib.
    virtual void replacePageImage(
        int pageIndex,
        std::span<uint8_t const> maskedImageData,
        int imageWidth, int imageHeight,
        std::string_view encoding) = 0;

    /// Add visible text objects for a reconstructed region.
    /// Generates BT/ET text operators with proper font, size, color,
    /// and positioning.
    /// @param pageIndex   Page to modify.
    /// @param editedText  The user's edited text (UTF-8).
    /// @param fontMatch   Font selection and properties.
    /// @param regionPdf   Region bounds in PDF user space.
    /// @param lineSpacing Line spacing in points.
    /// @return Font resource name allocated (e.g., "/F1").
    virtual std::string addReconstructedText(
        int pageIndex,
        std::u8string_view editedText,
        FontMatch const& fontMatch,
        PdfRect regionPdf,
        float lineSpacing) = 0;

    /// Write the complete content stream for a page with multiple regions.
    /// Assembles: image draw + per-region text + invisible OCR layer.
    /// @param pageIndex  Page to write.
    /// @param regions    All reconstructed regions on this page.
    /// @param hocrDoc    Full page OCR (for invisible layer on unedited areas).
    virtual void writeCompletePage(
        int pageIndex,
        std::span<ReconstructedRegion const> regions,
        HocrDocument const& hocrDoc) = 0;

    /// Embed a font into the PDF document and return its resource name.
    /// Handles TrueType/OpenType embedding, subsetting via HarfBuzz,
    /// and encoding (WinAnsi or Identity-H for Unicode).
    /// @param fontFilePath  Path to the font file.
    /// @param faceIndex     Face index within the file (for TTC fonts).
    /// @param codepoints    Unicode codepoints to include in the subset.
    /// @return Font resource dictionary reference for use in content streams.
    virtual std::string embedFont(
        std::string_view fontFilePath,
        int faceIndex,
        std::span<char32_t const> codepoints) = 0;
};

} // namespace pdfe::tre
```

### 12.10 Bitmap Compositor Interface (Approach C)

```cpp
// file: include/pdfe/tre/BitmapCompositor.h

#pragma once

#include <pdfe/tre/Types.h>
#include <vips/vips8>
#include <string>

namespace pdfe::tre {

/// Renders text to a bitmap using FreeType + HarfBuzz,
/// for Approach C (image-level text replacement).
class IBitmapCompositor {
public:
    virtual ~IBitmapCompositor() = default;

    /// Render the given text to an RGBA bitmap.
    /// Text is shaped with HarfBuzz and rasterized with FreeType.
    /// The background is transparent (alpha = 0).
    /// @param text       The text to render (UTF-8).
    /// @param fontMatch  Font, size, and color to use.
    /// @param targetWidth   Width of the target region in pixels.
    /// @param targetHeight  Height of the target region in pixels.
    /// @param dpi        Target DPI for rendering.
    /// @return RGBA bitmap of the rendered text.
    virtual vips::VImage renderText(
        std::u8string_view text,
        FontMatch const& fontMatch,
        int targetWidth, int targetHeight,
        int dpi) = 0;
};

} // namespace pdfe::tre
```

---

## 13. Implementation Roadmap

### Phase 1: Foundation (Weeks 1-2)

| Task | Component | Priority |
|------|-----------|----------|
| Define and implement `Types.h` (all shared types) | Core | P0 |
| Implement `CoordMapper` (pixel <-> PDF transform) | CoordMapper | P0 |
| Implement `HocrParser` (XML parsing with pugixml) | HocrParser | P0 |
| Implement `OcrPipeline` (Tesseract integration, caching) | OcrPipeline | P0 |
| Implement invisible text layer embedding | PdfContentWriter | P0 |
| Unit tests for coordinate mapping (known page geometries) | Tests | P0 |
| Unit tests for HOCR parsing (sample Tesseract output) | Tests | P0 |

### Phase 2: Analysis & Detection (Weeks 3-4)

| Task | Component | Priority |
|------|-----------|----------|
| Implement `BgColorDetector` (edge/gap sampling, histogram) | BgColorDetector | P0 |
| Implement `FontMatcher::detectProperties` (stroke width, serif, italic) | FontMatcher | P0 |
| Implement `FontMatcher::findMatch` (PANOSE + metric matching) | FontMatcher | P0 |
| Implement `QualityAssessor` (scoring algorithm) | QualityAssessor | P0 |
| Implement `ImageMasker::maskRegion` (single region, solid bg) | ImageMasker | P0 |
| Unit tests for background detection (synthetic test images) | Tests | P0 |
| Unit tests for font property detection (known font samples) | Tests | P0 |

### Phase 3: Approach B -- Region Reconstruction (Weeks 5-7)

| Task | Component | Priority |
|------|-----------|----------|
| Implement font embedding (TrueType/OpenType, subsetting) | PdfContentWriter | P0 |
| Implement text operator generation (BT/ET, Tf, Td, TJ) | PdfContentWriter | P0 |
| Implement `ImageMasker::maskRegionGradient` | ImageMasker | P1 |
| Implement feathered edge blending | ImageMasker | P0 |
| Implement image re-encoding (JPEG quality preservation) | ImageMasker | P0 |
| Implement `ReconstructRegionCommand` (undo/redo) | Commands | P0 |
| Implement `ReconstructedRegionSet` (multi-region tracking) | RegionRegistry | P0 |
| Integration test: single region edit on real scanned PDF | Tests | P0 |
| Integration test: multi-region edit on same page | Tests | P1 |

### Phase 4: Approach C -- Image Replacement (Weeks 8-9)

| Task | Component | Priority |
|------|-----------|----------|
| Implement `BitmapCompositor::renderText` (HarfBuzz + FreeType) | BitmapCompositor | P0 |
| Implement `ImageMasker::compositeTextBitmap` (alpha blend) | ImageMasker | P0 |
| Implement `ImageReplaceCommand` (undo/redo) | Commands | P0 |
| Implement automatic fallback (quality-gated B -> C) | QualityAssessor | P0 |
| Integration test: Approach C on complex-background scans | Tests | P0 |

### Phase 5: Polish & Edge Cases (Weeks 10-12)

| Task | Component | Priority |
|------|-----------|----------|
| Handle rotated pages (90/180/270 degree scans) | CoordMapper | P1 |
| Handle non-standard MediaBox/CropBox combinations | CoordMapper | P1 |
| Handle multi-image pages (some scanned pages use tiled images) | PdfContentWriter | P1 |
| Image diff-based undo optimization (Strategy 1) | Commands | P2 |
| User override UI for approach selection | UI integration | P1 |
| Low-confidence word highlighting in editor | UI integration | P1 |
| Performance optimization: parallel OCR for multi-page | OcrPipeline | P2 |
| Performance optimization: lazy image re-encoding | ImageMasker | P2 |
| Comprehensive test suite with scanned PDF corpus | Tests | P0 |

### Phase 6: Advanced Features (Future)

| Task | Component | Priority |
|------|-----------|----------|
| Per-word hybrid fallback (Approach B words + image words) | Advanced | P3 |
| Multi-column layout detection and handling | HocrParser | P3 |
| Table region detection (defer to Approach C) | QualityAssessor | P3 |
| RTL/BiDi text reconstruction (Arabic, Hebrew) | PdfContentWriter | P2 |
| CJK text reconstruction (vertical layout support) | PdfContentWriter | P2 |

---

## 14. ADR: Key Architectural Decisions

### ADR-TRE-001: Region-Level Granularity for Reconstruction

**Context:** The reconstruction boundary could be at word level (each word independently reconstructed), line level, paragraph/block level, or full-page level.

**Decision:** Use paragraph/block level (HOCR `ocr_par` or `ocr_carea`) as the reconstruction unit.

**Rationale:**
- **Word-level** is too fine-grained: each word would need its own mask, creating visible seams. Line spacing and word spacing would not be coherent.
- **Line-level** is slightly better but still produces seams between lines within a paragraph. If the user edits text that reflows across lines, line-level granularity cannot handle it.
- **Paragraph/block level** captures a coherent unit of text. Masking a paragraph rectangle produces a clean region. Text reflow within the paragraph (due to edits changing text length) is naturally contained.
- **Full-page level** converts the entire page to editable text, which is unnecessary complexity and risk when the user only wants to change one paragraph.

**Consequences:**
- (+) Clean mask boundaries at natural paragraph gaps
- (+) Text reflow works within the paragraph
- (+) Minimal image modification (most of the page is untouched)
- (-) If a paragraph spans the entire page width with no margin, the mask is wide and more visible
- (-) Cannot edit individual words in isolation (must edit the whole paragraph)

---

### ADR-TRE-002: Modify Image XObject Rather Than Overlay Rectangles

**Context:** To hide the original scanned text before placing new text objects, we could either (A) modify the image to fill the region with background color, or (B) draw an opaque rectangle over the image in the content stream.

**Decision:** Modify the image XObject (option A).

**Rationale:**
- Overlay rectangles can fail with transparency, blend modes, and complex compositing
- Modified image is a clean, unambiguous representation
- No risk of the original text "showing through" in any rendering scenario
- Better PDF/A compliance
- Better print fidelity

**Consequences:**
- (+) Guaranteed visual correctness
- (+) No residual original text data in the masked region
- (-) Requires image re-encoding (potential JPEG generational loss)
- (-) Slightly more complex implementation
- (-) Larger file delta on first edit (entire image is rewritten)

---

### ADR-TRE-003: Store Original Image + Regenerate for Undo

**Context:** Undo for image modifications could store complete image snapshots (expensive memory), compressed diffs (moderate complexity), or the original image with regeneration from region metadata (lowest memory).

**Decision:** Store the original unmasked image once per page (when the first region is reconstructed), and regenerate the masked image from region metadata on undo.

**Rationale:**
- A 300 DPI full-page scan is ~24 MB uncompressed. Storing a snapshot per undo step is prohibitive (500 steps = 12 GB).
- Compressed diffs (~100-200 KB each) are viable but add complexity.
- Storing the original once (~24 MB) and regenerating from metadata is simple and memory-efficient.
- Regeneration time is ~50-200ms (acceptable for undo latency).

**Consequences:**
- (+) Memory usage bounded at ~1 original image per edited page
- (+) Simple implementation
- (+) No generational image quality loss from undo/redo cycles
- (-) Undo is not instant (~50-200ms per page)
- (-) Must regenerate all region masks on every undo (O(n) in region count)

---

### ADR-TRE-004: Quality-Gated Fallback from Approach B to C

**Context:** Some scanned regions cannot be cleanly reconstructed as real text (poor OCR, complex background, no matching font). The system needs a way to still provide editing capability.

**Decision:** Implement a scoring algorithm that evaluates 6 signals (OCR confidence, background complexity, font match quality, text overlap, region purity) and automatically falls back to Approach C (image-level replacement) when the score is below 0.65.

**Rationale:**
- Binary thresholds on a single signal (e.g., "confidence < 80 = fallback") are too brittle. A region with 75% confidence but perfect background and font match should still use Approach B.
- A weighted multi-signal score captures the interplay between factors.
- 0.65 threshold was chosen as a conservative starting point. It can be tuned based on real-world testing.
- The user can override the automatic decision.

**Consequences:**
- (+) Graceful degradation: low-quality regions still get editing, just as bitmap composites
- (+) High-quality regions get real text objects (searchable, selectable, reflowable)
- (+) User override prevents the system from being too conservative
- (-) The threshold requires empirical tuning with real scanned documents
- (-) Users may not understand why some regions get "better" editing than others (needs clear UI communication)

---

## Sources

### MuPDF Documentation and API
- [MuPDF Documentation Release 1.26.1](https://mupdf.readthedocs.io/_/downloads/en/1.26.1/pdf/)
- [Using MuPDF with C](https://mupdf.readthedocs.io/en/1.26.1/using-mupdf.html)
- [MuPDF C++ API Blog Post](https://artifex.com/blog/an-improved-mupdf-api-using-c-plus-plus)
- [MuPDF PDF Font Source](https://github.com/ArtifexSoftware/mupdf/blob/master/source/pdf/pdf-font.c)
- [MuPDF Core Features](https://mupdf.com/core)

### HOCR Specification and Tesseract
- [hOCR Specification 1.2](https://github.com/kba/hocr-spec/blob/master/1.2/spec.md)
- [hOCR Wikipedia](https://en.wikipedia.org/wiki/HOCR)
- [Tesseract + hOCR Structure Guide](https://gaurav-neema.medium.com/ocring-identifying-page-structure-tesseract-hocr-760f3031f3ac)
- [Tesseract Character-Level Bounding Boxes](https://github.com/tesseract-ocr/tesseract/issues/135)
- [Reconstruct PDF from hOCR (UniDoc reference)](https://docs.unidoc.io/docs/unipdf/guides/ocr/reconstruct_pdf_from_hocr/)

### libvips
- [libvips Dominant Color Discussion](https://github.com/libvips/libvips/issues/259)
- [libvips Colour API](https://www.libvips.org/API/current/libvips-colour.html)
- [libvips VImage Class Reference](https://www.libvips.org/API/8.17/class.Image.html)

### Font Matching
- [PANOSE 2.0 Specification (W3C)](https://www.w3.org/Fonts/Panose/pan2.html)
- [PANOSE Metrics Guide (Monotype)](https://monotype.github.io/panose/pan1.htm)
- [font-kit GitHub Repository](https://github.com/servo/font-kit)
