#pragma once

#include "tre/TreTypes.h"
#include "tre/VisualFontRanker.h"
#include "fonts/FontMetricProfile.h"
#include "fonts/SubstituteFontSelector.h"  // SubstituteResult (by-value in vector params)
#include <QImage>
#include <optional>
#include <string>

namespace pdfe::fonts {
class SubstituteFontSelector;
class EmbeddedFontAnalyzer;
}

namespace pdfe::tre {
class BitmapCompositor;
class CNNFontClassifier;
}

namespace pdfe::tre {

/// Bridges OCR-detected font properties with the font substitution system.
/// Given OCR text data and page image, detects visual font properties (size,
/// weight, italic, color) and finds the best matching system font.
class FontMatcher {
public:
    FontMatcher(pdfe::fonts::SubstituteFontSelector* selector,
                pdfe::fonts::EmbeddedFontAnalyzer* analyzer);
    ~FontMatcher();

    // Non-copyable
    FontMatcher(const FontMatcher&) = delete;
    FontMatcher& operator=(const FontMatcher&) = delete;

    /// Detect visual properties of scanned text from OCR data and page image.
    /// @param regionData OCR text data containing lines, words, and metrics.
    /// @param pageImage Rasterized page image used for color detection.
    /// @param scanDpi DPI resolution of the page image.
    /// @return Detected font properties (size, weight, italic, color, etc).
    ScannedTextProperties detectProperties(
        const RegionTextData& regionData,
        const QImage& pageImage,
        int scanDpi) const;

    /// Find the best matching system font for the given properties.
    /// @param properties Visual font properties detected from scanned text.
    /// @param textNeeded Text content that needs to be rendered (for glyph coverage check).
    /// @return Font match result with file path, size, color, and confidence.
    FontMatch findMatch(
        const ScannedTextProperties& properties,
        const std::u32string& textNeeded) const;

    /// A maximal contiguous run of same-script codepoints within a text string.
    /// @ref start / @ref end are UTF-32 indices (half-open [start, end)).
    struct ScriptRun {
        size_t start = 0;
        size_t end = 0;
        pdfe::fonts::FontScript script = pdfe::fonts::FontScript::Unknown;
    };

    /// Partition @p text into maximal consecutive same-script runs (defect #7).
    /// Unlike detectScript() — which collapses a whole region to ONE dominant
    /// script and thereby mis-matches minority-script runs (e.g. an Arabic
    /// numeral group inside Latin body text) — this preserves every run so each
    /// can be matched against fonts appropriate for ITS actual script.
    /// Script-neutral codepoints (spaces, digits, punctuation) attach to the
    /// surrounding run rather than splitting it.
    static std::vector<ScriptRun> segmentScriptRuns(const std::u32string& text);

    /// Script of the specific run @p text[start, end) (defect #7). Thin wrapper
    /// over detectScript() applied to just that run's codepoints — the per-run
    /// analogue of the region-wide detectScript().
    static pdfe::fonts::FontScript scriptForRun(
        const std::u32string& text, size_t start, size_t end);

    /// Find the best matching font for a single text RUN (defect #7).
    /// Builds a query whose script is the run's OWN script (via scriptForRun)
    /// rather than the region-dominant script, so a minority-script run is
    /// matched against — and not penalized for using — a font that covers that
    /// run's script. @p fullText is the region text; @p runStart / @p runEnd are
    /// UTF-32 indices of the run within it.
    FontMatch findMatchForRun(
        const ScannedTextProperties& properties,
        const std::u32string& fullText,
        size_t runStart,
        size_t runEnd) const;

    /// Set optional BitmapCompositor for NCC-based visual ranking.
    /// Used as fallback when CNN classifier is not available.
    void setBitmapCompositor(BitmapCompositor* compositor);

    /// Set CNN font classifier for primary visual ranking.
    /// When set and model is available, matchRegion() uses CNN embedding
    /// similarity as the primary font identification method. Metric scoring
    /// is used only for structural pre-filtering (serif/sans, italic, weight).
    /// Falls back to NCC (if compositor set) or metric-only otherwise.
    void setCNNFontClassifier(CNNFontClassifier* classifier);

    /// Detect properties and find best matching font in one call.
    /// Uses metric-only scoring for the initial result. CNN visual ranking
    /// is deferred to visualRankForLine() which runs when the user clicks
    /// a specific text span, so the correct line/words are used.
    /// @param regionData OCR text data containing lines, words, and metrics.
    /// @param pageImage Rasterized page image used for color detection.
    /// @param scanDpi DPI resolution of the page image.
    /// @return Font match result with file path, size, color, and confidence.
    FontMatch matchRegion(
        const RegionTextData& regionData,
        const QImage& pageImage,
        int scanDpi) const;

    /// Run CNN (or NCC fallback) visual ranking on a specific clicked line.
    /// Crops 2-3 words from the center of the line, renders each candidate
    /// font with that text, and ranks by visual similarity.
    /// @param clickedLine The HOCR line the user clicked on.
    /// @param pageImage Page image at scanDpi resolution.
    /// @param scanDpi DPI of the page image.
    /// @param properties Detected font properties (size, color, weight).
    /// @param candidates Font candidates to rank (from collectCandidates).
    /// @return Updated FontMatch from the visual winner, or empty if ranking fails.
    std::optional<FontMatch> visualRankForLine(
        const HocrLine& clickedLine,
        const QImage& pageImage,
        int scanDpi,
        const ScannedTextProperties& properties,
        const std::vector<pdfe::fonts::SubstituteResult>& candidates) const;

    /// Result of visual ranking including the full ranked candidate list.
    struct VisualRankResult {
        std::optional<FontMatch> bestMatch;
        std::vector<VisualFontRanker::RankedCandidate> rankedCandidates;
    };

    /// Same as visualRankForLine but also returns the full ranked candidates list.
    VisualRankResult visualRankForLineWithCandidates(
        const HocrLine& clickedLine,
        const QImage& pageImage,
        int scanDpi,
        const ScannedTextProperties& properties,
        const std::vector<pdfe::fonts::SubstituteResult>& candidates) const;

    /// Collect font candidates for visual ranking (CNN or NCC).
    /// When CNN is available, returns all non-decorative system fonts.
    /// When only NCC, returns metric-filtered top candidates.
    /// Call at tool activation time and store for later use by visualRankForLine().
    std::vector<pdfe::fonts::SubstituteResult> collectCandidates(
        const ScannedTextProperties& properties,
        const std::u32string& textNeeded) const;

    /// Detect text color by sampling foreground pixels in word bounding boxes.
    /// @param pageImage Rasterized page image.
    /// @param words Vector of OCR words with bounding box coordinates.
    /// @return Median RGB color of foreground (dark) pixels.
    static RgbColor detectTextColor(
        const QImage& pageImage,
        const std::vector<HocrWord>& words);

    /// Estimate font weight from x-height/total-height ratio.
    /// Bold fonts have relatively larger x-heights compared to ascenders/descenders.
    /// @param xHeightRatio Ratio of x-height to total height (xHeight / (xHeight + ascenders + descenders)).
    /// @return Estimated font weight classification.
    static FontWeight estimateWeight(float xHeightRatio);

    /// Detect the dominant script of a Unicode text string.
    /// Counts characters per script range and returns the most frequent.
    /// Used to set primaryScript on the query profile for font matching.
    static pdfe::fonts::FontScript detectScript(const std::u32string& text);

    /// Per-script ascender-to-em ratio used by the baseline-to-em size estimate
    /// (returns 0 for scripts with no meaningful baseline, e.g. CJK/Unknown).
    static float ascenderRatioForScript(pdfe::fonts::FontScript script);

    /// Font size (PDF points) for ONE HOCR line from its own x_size + baseline
    /// (pipeline 3f8a1c72 Phase 3 / c4a9e271 Phase 1). Combines Estimate A
    /// (x_size, or line-bbox-height fallback) and Estimate B (baseline-to-ascender,
    /// gated to ascender 30-95% of line height). Returns {sizePt, valid}; valid is
    /// false when the line yields no usable measurement. This is the single shared
    /// implementation: detectProperties() fills ScannedTextProperties::perLineFontSizePt
    /// with it over the word-level region lines, and the text-edit overlay computes a
    /// per-line size aligned to the char-level atlas lines the click resolves against.
    static std::pair<float, bool> lineFontSizePt(
        const HocrLine& line,
        pdfe::fonts::FontScript script,
        int scanDpi);

    /// Select the per-line size for a clicked line: perLine[idx] when idx is in
    /// range and positive, else fallbackPt. Centralizes the fallback contract so
    /// callers (and tests) agree on empty-vector / out-of-range / invalid-line
    /// handling. (pipeline c4a9e271 Phase 1.)
    static float clickedLineSizePt(
        const std::vector<float>& perLineSizePt,
        int lineIndex,
        float fallbackPt);

    /// Detect serif vs sans-serif from scanned image pixels.
    /// Measures horizontal pixel extent at baseline vs mid-stem of tall characters.
    /// Serif fonts have wider strokes at the baseline due to decorative terminals.
    /// @param pageImage Rasterized page image at scan DPI.
    /// @param lines HOCR lines with baseline equations and word bounding boxes.
    /// @return true if serif, false if sans-serif or indeterminate.
    /// @param determined Optional out-param, set true only when the result is
    ///        a MEASUREMENT. The return value alone cannot distinguish
    ///        "measured as sans" from "could not measure" -- both are false --
    ///        and callers that act categorically on serif-ness must not treat
    ///        those alike. See ScannedTextProperties::serifDetermined and
    ///        GitHub #249.
    static bool detectSerif(const QImage& pageImage,
                            const std::vector<HocrLine>& lines,
                            bool* determined = nullptr);

    /// Detect per-word bold/italic/underline/strikethrough from image pixels.
    /// Populates each word's style flags by analyzing stem thickness, character
    /// slant, and horizontal line presence at baseline/mid-text positions.
    /// @param pageImage Rasterized page image at scan DPI.
    /// @param lines HOCR lines (modified in-place: word.style flags set).
    static void detectWordStyles(const QImage& pageImage,
                                  std::vector<HocrLine>& lines);

    /// Detect bold for a single word by measuring vertical stem thickness.
    static bool detectWordBold(const QImage& pageImage,
                               const HocrWord& word,
                               const HocrLine& line);

    /// Detect italic for a single word by measuring character slant angle.
    static bool detectWordItalic(const QImage& pageImage,
                                  const HocrWord& word,
                                  const HocrLine& line);

    /// Detect underline for words in a line (presence of horizontal line below baseline).
    static void detectUnderlinedWords(const QImage& pageImage,
                                       HocrLine& line);

    /// Detect strikethrough for words in a line (horizontal line through text center).
    static void detectStrikethroughWords(const QImage& pageImage,
                                          HocrLine& line);

    /// Build a FontMetricProfile query from detected properties and text.
    /// Shared by findMatch() and matchRegion() to avoid divergence.
    /// @param scriptOverride When non-Unknown, the query's primaryScript is set
    ///        to this per-run script (defect #7) instead of the region-dominant
    ///        detectScript(textNeeded). The region-dominant value is retained
    ///        only as a fallback hint when no override is given.
    static pdfe::fonts::FontMetricProfile buildQueryProfile(
        const ScannedTextProperties& properties,
        const std::u32string& textNeeded,
        pdfe::fonts::FontScript scriptOverride = pdfe::fonts::FontScript::Unknown);

private:
    /// Shared implementation for findMatch() (region) and findMatchForRun()
    /// (per-run, defect #7). @p scriptOverride is forwarded to buildQueryProfile:
    /// Unknown → region-dominant script; otherwise the run's own script.
    FontMatch findMatchWithScript(
        const ScannedTextProperties& properties,
        const std::u32string& textNeeded,
        pdfe::fonts::FontScript scriptOverride) const;

    pdfe::fonts::SubstituteFontSelector* selector_;
    pdfe::fonts::EmbeddedFontAnalyzer* analyzer_;
    BitmapCompositor* compositor_ = nullptr;
    CNNFontClassifier* cnnClassifier_ = nullptr;
};

} // namespace pdfe::tre
