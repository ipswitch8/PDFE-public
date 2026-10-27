#pragma once

#include "fonts/FontMetricProfile.h"
#include <optional>
#include <vector>
#include <memory>

struct FT_LibraryRec_;
typedef struct FT_LibraryRec_* FT_Library;
struct FT_FaceRec_;
typedef struct FT_FaceRec_* FT_Face;

namespace pdfe::core {
class MuPdfContext;
class Page;
class Document;
}

namespace pdfe::fonts {

class EmbeddedFontAnalyzer {
public:
    explicit EmbeddedFontAnalyzer(pdfe::core::MuPdfContext* context);
    ~EmbeddedFontAnalyzer();

    // Analyze a single font via its fz_font* handle.
    // Returns a basic profile from struct fields only (no FT function calls).
    std::optional<FontMetricProfile> analyzeFont(void* fzFont);

    // Analyze all fonts used on a page.
    // Extracts font data from PDF streams and creates own FT_Face for deep analysis.
    std::vector<FontMetricProfile> analyzePageFonts(
        pdfe::core::Page* page,
        const pdfe::core::Document* document
    );

    // Analyze a system font file from disk.
    // Creates own FT_Face from file data for full analysis.
    std::optional<FontMetricProfile> analyzeFromFile(const std::string& filePath);

    // Full analysis of a caller-owned FT_Face (the caller retains ownership and
    // is responsible for FT_Done_Face). Runs the same deep path as the page /
    // file analyzers (PANOSE serif classification, metrics, coverage), so the
    // resulting FontMetricProfile.isSerif is the authoritative serif flag.
    // Used by AutoMissingGlyphResolver, which already holds the source font's
    // FT_Face in memory and needs the profile serif classification without
    // re-extracting from disk. Returns nullopt for a null face.
    // (pipeline d4e7f921 phase 3)
    std::optional<FontMetricProfile> analyzeFace(FT_Face face,
                                                 const std::string& psName = {});

    // Check which codepoints in text are NOT covered by a font file.
    // Loads own FT_Face from fontFilePath, tests each codepoint.
    // Returns vector of missing codepoints (empty = all covered).
    std::vector<char32_t> findMissingGlyphs(const std::string& fontFilePath, const std::u32string& text);


    // Non-copyable
    EmbeddedFontAnalyzer(const EmbeddedFontAnalyzer&) = delete;
    EmbeddedFontAnalyzer& operator=(const EmbeddedFontAnalyzer&) = delete;

private:
    pdfe::core::MuPdfContext* context_;
    FT_Library ftLibrary_ = nullptr;

    // Full analysis using our own FT_Face (created from extracted font data)
    FontMetricProfile analyzeOwnedFace(FT_Face face, const std::string& psName);

    FontMetricProfile extractTableMetrics(FT_Face face);
    float measureStemWeightV(FT_Face face);
    float measureStemWeightH(FT_Face face);
    float measureSerifExtent(FT_Face face);
    void classifyCoverage(FT_Face face, FontMetricProfile& profile);
    void measureMonospace(FT_Face face, FontMetricProfile& profile);
    float measureAvgCharWidth(FT_Face face);
    /// Renders glyphs at a sample size and returns the maximum
    /// cap-top-to-descender-bottom extent / pixelSize. Bypasses OS/2.
    /// Works on subsetted fonts without Unicode charmaps by iterating
    /// glyph indices directly. Also populates @c profile.renderedMaxTop
    /// and @c profile.renderedMinBottom so callers can position the
    /// substitute font's baseline at the same vertical offset the
    /// source font uses.
    float measureRenderedExtent(FT_Face face, FontMetricProfile& profile);
};

} // namespace pdfe::fonts
