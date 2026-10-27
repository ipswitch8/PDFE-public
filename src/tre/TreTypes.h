#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pdfe::tre {

// ---------------------------------------------------------------------------
// Geometry Types - Pixel coordinate space (top-left origin, Y-down)
// ---------------------------------------------------------------------------

struct PixelPoint {
    double x = 0;
    double y = 0;
};

struct PixelRect {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;

    [[nodiscard]] double width() const;
    [[nodiscard]] double height() const;
    [[nodiscard]] double area() const;
    [[nodiscard]] bool contains(PixelPoint pt) const;
    [[nodiscard]] bool intersects(const PixelRect& other) const;
    [[nodiscard]] PixelRect intersection(const PixelRect& other) const;
    [[nodiscard]] PixelPoint center() const;
};

// ---------------------------------------------------------------------------
// Geometry Types - PDF/fitz coordinate space (top-left origin, Y-down, points)
// NOTE: These use MuPDF fitz-space (Y-down), NOT standard PDF space (Y-up).
// ---------------------------------------------------------------------------

struct PdfPoint {
    double x = 0;
    double y = 0;
};

struct PdfRect {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;

    [[nodiscard]] double width() const;
    [[nodiscard]] double height() const;
    [[nodiscard]] double area() const;
    [[nodiscard]] bool contains(PdfPoint pt) const;
    [[nodiscard]] bool intersects(const PdfRect& other) const;
    [[nodiscard]] PdfRect intersection(const PdfRect& other) const;
    [[nodiscard]] PdfPoint center() const;
};

// ---------------------------------------------------------------------------
// Color Types
// ---------------------------------------------------------------------------

struct RgbColor {
    uint8_t r = 0, g = 0, b = 0;
    bool operator==(const RgbColor&) const = default;
};

struct RgbaColor {
    uint8_t r = 0, g = 0, b = 0, a = 255;
};

// ---------------------------------------------------------------------------
// ID Types
// ---------------------------------------------------------------------------

enum class RegionId : uint32_t {};

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

/// The reconstruction approach used for a text region.
enum class Approach {
    B_HybridReconstruction,  ///< Reconstruct text with font matching over cleaned background
    C_ImageReplacement       ///< Fall back to image patch from the scanned page
};

/// CSS-compatible font weight values.
enum class FontWeight {
    Thin = 100,
    ExtraLight = 200,
    Light = 300,
    Regular = 400,
    Medium = 500,
    SemiBold = 600,
    Bold = 700,
    ExtraBold = 800,
    Black = 900
};

/// Types of quality warnings emitted during region assessment.
enum class QualityWarningType {
    LowConfidence,
    ComplexBackground,
    PoorFontMatch,
    NonTextOverlap,
    GradientBackground,
    MinConfidenceTooLow
};

// ---------------------------------------------------------------------------
// Quality Assessment
// ---------------------------------------------------------------------------

struct QualityWarning {
    QualityWarningType type;
    std::string message;
};

struct QualityThresholds {
    float approachBMinScore = 0.65f;    ///< Minimum overall score for Approach B
    int minMeanConfidence = 80;          ///< Minimum mean OCR word confidence
    int minWordConfidence = 50;          ///< Minimum individual word confidence
    float minBgUniformity = 0.7f;        ///< Minimum background uniformity score
    float minFontMatchConf = 0.5f;       ///< Minimum font match confidence
};

struct QualityVerdict {
    Approach recommendedApproach = Approach::C_ImageReplacement;
    float overallScore = 0.0f;
    std::vector<QualityWarning> warnings;
};

// ---------------------------------------------------------------------------
// Background Analysis
// ---------------------------------------------------------------------------

struct GradientInfo {
    RgbColor topLeft, topRight, bottomLeft, bottomRight;
};

/// Background classification. Used by B3 to distinguish uniform /
/// gradient backgrounds (safe to fill with a solid color) from
/// halftone-dithered or textured backgrounds (would show a visible
/// flat rectangle after a solid fill — callers should prefer pattern
/// sampling from a nearby clean area instead).
///
/// See docs/image-pdf-risks.md (B3).
enum class BgClassification : uint8_t {
    Unknown = 0,    ///< Not yet classified or insufficient samples
    Uniform,        ///< Single dominant color, low local variance
    Gradient,       ///< Smooth gradient, low high-frequency content
    Halftone,       ///< Periodic dot pattern (newspaper / offset print)
    Textured,       ///< Random high-frequency noise (photo of paper)
    Complex         ///< Something else — solid fill is the safest fallback
};

struct BackgroundInfo {
    RgbColor dominantColor;
    bool isUniform = false;
    bool hasGradient = false;
    GradientInfo gradient;
    bool isComplex = false;
    float confidence = 0.0f;

    // B3 classification fields
    BgClassification classification = BgClassification::Unknown;
    /// Mean local variance (Laplacian-like) in the sampled region,
    /// normalized to [0.0, 1.0]. Low = smooth, high = noisy/patterned.
    float localVariance = 0.0f;
    /// When classification==Halftone, approximate dot spacing in pixels
    /// at the source image's native DPI. 0 when not applicable.
    float halftoneSpacingPx = 0.0f;
};

// ---------------------------------------------------------------------------
// Font Matching
// ---------------------------------------------------------------------------

/// Per-word detected formatting flags (from image pixel analysis).
struct WordStyleFlags {
    bool isBold = false;
    bool isItalic = false;
    bool isUnderlined = false;
    bool isStrikethrough = false;
};

/// Properties detected from the scanned text image via OCR and analysis.
struct ScannedTextProperties {
    float fontSizePt = 0.0f;         ///< Region-representative size (also the
                                     ///< per-line fallback, see perLineFontSizePt).
    /// Per-line font size in points, indexed parallel to RegionTextData::lines
    /// (pipeline 3f8a1c72 Phase 3). Real regions mix sizes — e.g. a heading line
    /// at 30pt above 10-12pt body (measured per-line CV up to 0.24) — so the
    /// single region-average fontSizePt is wrong for BOTH. Each entry is that
    /// line's own size when it has a valid x_size/baseline measurement, or the
    /// region-average fontSizePt as a fallback for lines lacking valid data
    /// (never the average broadcast over a line that has its own measurement).
    /// Callers that render a specific line (e.g. the clicked-line overlay) use
    /// this instead of fontSizePt; empty only when the region has no lines.
    std::vector<float> perLineFontSizePt;
    FontWeight weight = FontWeight::Regular;
    bool isItalic = false;
    bool isSerif = false;
    /// True when isSerif above is a MEASUREMENT rather than a default.
    ///
    /// `FontMatcher::detectSerif()` returns false both for "measured as sans"
    /// and for "could not measure" -- it collects stroke-width ratios and
    /// returns false outright when none are valid. Those two cases must not be
    /// treated alike by anything that acts categorically on serif-ness:
    /// GitHub #249 partitions font candidates so serif mismatches rank last,
    /// and doing that on an unmeasured value would silently force sans-only
    /// results for every region whose serif-ness could not be determined.
    ///
    /// Read it as "isSerif is worth acting on", not as a confidence score.
    bool serifDetermined = false;
    RgbColor textColor;
    float lineSpacingRatio = 1.2f;
    float xHeightRatio = 0.0f;       ///< x-height / font size
    float ascenderRatio = 0.0f;      ///< ascender / font size
    float descenderRatio = 0.0f;     ///< descender / font size
    // Page-level summary of per-word style detection
    bool hasAnyBold = false;
    bool hasAnyItalic = false;
    bool hasAnyUnderline = false;
    bool hasAnyStrikethrough = false;
};

/// Result of matching a scanned text region to a system/embedded font.
struct FontMatch {
    std::string fontFamily;
    std::string fontStyle;
    std::string fontFilePath;
    int fontFaceIndex = 0;
    float sizePt = 0.0f;
    RgbColor color;
    float lineSpacingRatio = 1.2f;
    float confidence = 0.0f;         ///< 0.0-1.0 match confidence
    ScannedTextProperties detectedProperties;
};

// ---------------------------------------------------------------------------
// HOCR Structures (Tesseract OCR output representation)
// ---------------------------------------------------------------------------

/// Per-character data from Tesseract HOCR output (ocrx_cinfo elements).
/// Populated only when OcrConfig::enableCharBoxes is true.
struct HocrChar {
    char32_t codepoint = 0;          ///< Unicode codepoint
    PixelRect bbox;                  ///< Bounding box in page pixel coordinates
    int confidence = 0;              ///< 0-100 character confidence
};

struct HocrWord {
    std::string text;
    PixelRect bbox;
    int confidence = 0;              ///< 0-100 word confidence from Tesseract
    float fontSizePx = 0.0f;
    WordStyleFlags style;            ///< Per-word formatting detected from image pixels
    std::vector<HocrChar> chars;     ///< Per-character data (populated when hocr_char_boxes=1)
};

struct HocrLine {
    PixelRect bbox;
    float baselineSlope = 0.0f;
    float baselineOffset = 0.0f;
    float xSizePx = 0.0f;           ///< x-height in pixels
    float descendersPx = 0.0f;
    float ascendersPx = 0.0f;
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
    int pageWidth = 0;
    int pageHeight = 0;
    int scanResX = 300;              ///< Horizontal scan resolution (DPI)
    int scanResY = 300;              ///< Vertical scan resolution (DPI)
    std::vector<HocrBlock> blocks;
};

// ---------------------------------------------------------------------------
// Region Types
// ---------------------------------------------------------------------------

/// Text data extracted from a region via OCR.
struct RegionTextData {
    PdfRect pdfBbox;
    PixelRect pixelBbox;
    std::vector<HocrLine> lines;
    std::string plainText;

    /// Average word confidence across all lines (0-100).
    [[nodiscard]] float meanWordConfidence() const;

    /// Minimum word confidence across all lines (returns 100 if no words).
    [[nodiscard]] float minWordConfidence() const;

    /// Heuristic: true if all words have confidence > 0.
    [[nodiscard]] bool containsOnlyText() const;
};

/// A fully processed region ready for PDF output.
struct ReconstructedRegion {
    RegionId id = RegionId{0};
    PdfRect pdfBbox;
    PixelRect pixelBbox;
    Approach approach = Approach::B_HybridReconstruction;
    std::string originalText;
    std::string editedText;
    FontMatch fontMatch;
    BackgroundInfo bgInfo;
};

// ---------------------------------------------------------------------------
// Coordinate Transform (pixel <-> PDF mapping metadata)
// ---------------------------------------------------------------------------

struct AffineTransform {
    double scaleX = 1.0, scaleY = 1.0;
    double translateX = 0.0, translateY = 0.0;
    bool flipY = false;              ///< Both pixel and fitz-space are Y-down; no flip needed
    int rotation = 0;                 ///< Page rotation in degrees (0, 90, 180, 270)
    double imageWidth = 0.0, imageHeight = 0.0;
    double pageWidth = 0.0, pageHeight = 0.0;
};

// ---------------------------------------------------------------------------
// OCR Configuration
// ---------------------------------------------------------------------------

struct OcrConfig {
    std::string language = "eng";
    std::string tessdataPath;
    int ocrDpi = 300;
    bool useGpu = false;
    int pageSegMode = 3;             ///< PSM_AUTO
    bool enableCharBoxes = false;    ///< Enable per-character bounding boxes in HOCR output
};

/// Progress callback: (current step, total steps, status message).
using OcrProgressCallback = std::function<void(int current, int total, std::string_view message)>;

// ---------------------------------------------------------------------------
// Image Placement (for Approach C fallback)
// ---------------------------------------------------------------------------

struct ImagePlacement {
    PdfRect bbox;
    double scaleX = 1.0, scaleY = 1.0;
    double tx = 0.0, ty = 0.0;
    int rotation = 0;
};

// ---------------------------------------------------------------------------
// UTF-8 Utilities
// ---------------------------------------------------------------------------

/// Convert a UTF-8 string to UTF-32, handling 1-4 byte sequences.
/// Invalid bytes are skipped.
std::u32string utf8ToU32(const std::string& utf8);

/// Convert a UTF-8 string to a vector of uint32_t codepoints.
std::vector<uint32_t> utf8ToCodepoints(const std::string& utf8);

} // namespace pdfe::tre
