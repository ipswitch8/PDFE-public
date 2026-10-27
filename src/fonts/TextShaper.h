#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pdfe::fonts {

// Forward-declared with its fixed underlying type so ShapeParams can hold a
// FontScript by value without pulling in the whole FontMetricProfile.h. The
// definition lives in fonts/FontMetricProfile.h (must match this exactly).
enum class FontScript : uint8_t;

struct ShapedGlyph {
    uint32_t glyphId = 0;
    uint32_t cluster = 0;     // Logical (pre-shaping) codepoint index this glyph maps to
    float xAdvance = 0.0f;    // In points at requested fontSize
    float yAdvance = 0.0f;
    float xOffset = 0.0f;
    float yOffset = 0.0f;
};

// Shaping direction. TTB = top-to-bottom (vertical writing mode / MuPDF wmode 1).
enum class ShapeDirection { LTR, RTL, TTB };

// Per-run shaping parameters threaded into HarfBuzz (R2-20/R2-21). Maps a
// detected FontScript to the correct HarfBuzz script/direction/language so that
// Arabic joins, Hebrew runs RTL, and CJK can be laid out vertically.
struct ShapeParams {
    FontScript script{};               // value-inits to Unknown; set via forScript()
    ShapeDirection direction = ShapeDirection::LTR;
    std::string language = "en";       // BCP-47 tag for HarfBuzz
    bool vertical = false;             // writing mode; forces TTB when true

    // Build params from a detected script:
    //   Arabic / Hebrew        -> RTL
    //   any script + vertical  -> TTB (overrides RTL/LTR)
    //   everything else        -> LTR
    // Also picks a sensible default language tag per script.
    static ShapeParams forScript(FontScript s, bool verticalMode = false);
};

// A maximal run of one resolved bidi direction within a logical string.
// [start, end) are UTF-32 indices; rtl indicates the run reads right-to-left.
struct DirectionalRun {
    size_t start = 0;
    size_t end = 0;
    bool rtl = false;
};

// One caret step in layout coordinates. Horizontal text advances in X;
// vertical (TTB) text advances in Y with zero X.
struct CaretAdvance {
    float dx = 0.0f;
    float dy = 0.0f;
};

class TextShaper {
public:
    TextShaper();
    ~TextShaper();

    // Backward-compatible: shapes as LTR/Latin/en (existing callers unchanged).
    std::vector<ShapedGlyph> shape(
        void* ftFace,
        const std::u32string& text,
        float fontSize
    );

    // Script/direction/language-aware shaping (R2-20/R2-21). For RTL params
    // HarfBuzz returns glyphs in visual order (leftmost glyph first, whose
    // cluster is the LAST logical char); for TTB the glyphs carry yAdvance.
    std::vector<ShapedGlyph> shape(
        void* ftFace,
        const std::u32string& text,
        float fontSize,
        const ShapeParams& params
    );

    // Measure total advance width of shaped text (LTR/Latin).
    float measureWidth(
        void* ftFace,
        const std::u32string& text,
        float fontSize
    );

    // Measure total advance along the writing axis for the given params:
    // sum of xAdvance for horizontal, sum of |yAdvance| for vertical.
    float measureWidth(
        void* ftFace,
        const std::u32string& text,
        float fontSize,
        const ShapeParams& params
    );

    // Check if font has glyphs for all characters in text.
    bool canRender(void* ftFace, const std::u32string& text);

    // === Bidi (UAX#9-lite) ===================================================

    // Partition logical text into directional runs (resolved bidi level 0/1).
    // Neutral/weak characters attach to the preceding strong context (or the
    // paragraph base direction at the start).
    static std::vector<DirectionalRun> bidiRuns(const std::u32string& logical);

    // Reorder logical text into visual (display) order: each RTL run is
    // reversed. Latin+Hebrew ("abcאבג") -> Latin kept, Hebrew reversed; a pure
    // Arabic/Hebrew string -> fully reversed. Correctly orders Arabic/Hebrew
    // RTL relative to Latin for an LTR base and pure-RTL paragraphs.
    static std::u32string reorderForDisplay(const std::u32string& logical);

    // === Vertical writing-mode caret model ===================================

    // Caret step for one shaped glyph. vertical=true -> advance in Y (dx=0);
    // horizontal -> advance in X (dy=0). Honours MuPDF wmode threaded via
    // TextLine::writingMode / ShapeParams::vertical.
    static CaretAdvance caretAdvance(const ShapedGlyph& glyph, bool vertical);

    // Non-copyable
    TextShaper(const TextShaper&) = delete;
    TextShaper& operator=(const TextShaper&) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pdfe::fonts
