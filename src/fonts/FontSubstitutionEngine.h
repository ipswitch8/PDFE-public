#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pdfe::core {
class MuPdfContext;
class Page;
class Document;
}

namespace pdfe::textedit {
struct TextSpan;
}

namespace pdfe::fonts {

class EmbeddedFontAnalyzer;
class FontAnalysisCache;
class SubstituteFontSelector;
class GlyphScaleAdjuster;
class TextShaper;
class PdfFontEmbedder;

/// One per-run slice of a multi-formatting-run substitution.
/// Used by SubstitutionInfo.runSegments to drive UI-TE-SAVE-08's
/// per-run Tf/Tj emission. A SubstitutionInfo with empty runSegments
/// falls back to the legacy single-font path; non-empty runSegments
/// trigger the per-run emission branch in the content-stream filter
/// callback.
///
/// Indices are into the UTF-32 form of the replacement text. Empty
/// segments (charStart == charEnd) are stored at the model layer
/// without validation; the emission layer skips them.
struct RunSegment {
    int charStart = 0;            // Inclusive UTF-32 index into newText
    int charEnd = 0;              // Exclusive UTF-32 index into newText
    std::string fontResName;      // Per-run resource name (e.g., "Fsub2")
    float fontSize = 0.0f;        // Per-run font size in points
};

struct SubstitutionInfo {
    int spanIndex = -1;                 // Which span this applies to
    std::string substituteResName;      // Resource name (e.g., "Fsub1")
    float horizontalScale = 1.0f;       // Width correction factor
    float originalFontSize = 0.0f;
    std::string originalFontName;       // Original font resource name

    /// Per-run segments for multi-formatting-run spans (UI-TE-SAVE-08).
    /// Empty → legacy single-font emission path runs unchanged.
    /// Non-empty → emission walks segments and emits per-run Tf/Tj
    /// pairs. Phase-1 model only; emission lands in phase-2.
    std::vector<RunSegment> runSegments;
};

class FontSubstitutionEngine {
public:
    explicit FontSubstitutionEngine(
        pdfe::core::MuPdfContext* context,
        const std::filesystem::path& cachePath = {}
    );
    ~FontSubstitutionEngine();

    // Process a set of text edits and determine which spans need font substitution.
    // Embeds substitute fonts as needed. Returns substitution info for each span
    // that requires a font switch.
    std::vector<SubstitutionInfo> processEdits(
        pdfe::core::Page* page,
        pdfe::core::Document* document,
        const std::vector<pdfe::textedit::TextSpan>& allSpans,
        const std::vector<std::pair<int, std::u32string>>& modifications
    );

    FontSubstitutionEngine(const FontSubstitutionEngine&) = delete;
    FontSubstitutionEngine& operator=(const FontSubstitutionEngine&) = delete;

private:
    pdfe::core::MuPdfContext* context_;
    std::unique_ptr<EmbeddedFontAnalyzer> analyzer_;
    std::unique_ptr<FontAnalysisCache> cache_;
    std::unique_ptr<SubstituteFontSelector> selector_;
    std::unique_ptr<GlyphScaleAdjuster> scaleAdjuster_;
    std::unique_ptr<TextShaper> shaper_;
    std::unique_ptr<PdfFontEmbedder> embedder_;
};

} // namespace pdfe::fonts
