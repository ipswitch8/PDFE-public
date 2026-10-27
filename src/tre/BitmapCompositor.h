#pragma once
#include "tre/TreTypes.h"
#include <QImage>
#include <memory>
#include <string>
#include <vector>

namespace pdfe::tre {

/// C5: controls whether a text render uses LCD subpixel rendering
/// for sharper on-screen glyphs, or grayscale coverage (the
/// historical default). WYSIWYG is the default — the overlay and
/// save paths produce visually identical bitmaps, so there is no
/// layout jump on commit. HighQuality forces LCD on the overlay
/// path; the save path still runs whatever post-processing the
/// caller configures (typically ScanStyleTransfer), so the user
/// sees crisp text while editing but accepts a visual shift at
/// commit time.
///
/// See docs/image-pdf-risks.md (C5 / C1/C4/C5 polish bundle).
enum class DisplayQuality : uint8_t {
    WYSIWYG = 0,    ///< Default: grayscale rendering, matches save path.
    HighQuality,    ///< Force LCD subpixel on the overlay path.
};

/// A character range with associated style flags for per-word rendering.
struct StyledRun {
    int charStart = 0;            ///< UTF-32 character index
    int charEnd = 0;              ///< UTF-32 character index (exclusive)
    WordStyleFlags style;         ///< Bold/italic/underline/strikethrough
};

/// A character range with an explicit font for multi-font rendering.
struct FontRun {
    int charStart = 0;            ///< UTF-32 character index
    int charEnd = 0;              ///< UTF-32 character index (exclusive)
    std::string fontFilePath;     ///< Path to .ttf/.otf file
    float fontSizePt = 0.0f;      ///< Font size in points (0 = use default)
    WordStyleFlags style;         ///< Bold/italic/underline/strikethrough
};

/// Renders text to a transparent RGBA bitmap using FreeType for glyph
/// rasterization and HarfBuzz (via TextShaper) for text shaping.
/// Used for Approach C (image-level text replacement) in the TRE pipeline.
class BitmapCompositor {
public:
    BitmapCompositor();
    ~BitmapCompositor();

    BitmapCompositor(const BitmapCompositor&) = delete;
    BitmapCompositor& operator=(const BitmapCompositor&) = delete;
    BitmapCompositor(BitmapCompositor&&) noexcept;
    BitmapCompositor& operator=(BitmapCompositor&&) noexcept;

    /// Render text to a transparent RGBA bitmap.
    QImage renderText(const std::u32string& text,
                      const std::string& fontFilePath,
                      float fontSizePt,
                      const RgbColor& textColor,
                      int targetWidth,
                      int targetHeight,
                      int dpi = 300);

    /// Convenience overload using FontMatch struct.
    QImage renderText(const std::u32string& text,
                      const FontMatch& fontMatch,
                      int targetWidth,
                      int targetHeight,
                      int dpi = 300);

    /// Render text with per-word styled runs (bold/italic/underline/strikethrough).
    /// Uses FT_GlyphSlot_Embolden/Oblique for per-glyph synthetic effects,
    /// and QPainter for underline/strikethrough line decoration.
    QImage renderStyledText(const std::u32string& text,
                            const FontMatch& fontMatch,
                            const std::vector<StyledRun>& styledRuns,
                            int targetWidth,
                            int targetHeight,
                            int dpi = 300);

    /// Render text with per-run font overrides (different font files per run).
    /// Falls back to fontMatch for characters not covered by any FontRun.
    QImage renderMultiFontText(const std::u32string& text,
                               const FontMatch& fontMatch,
                               const std::vector<FontRun>& fontRuns,
                               int targetWidth,
                               int targetHeight,
                               int dpi = 300);

    /// Measure the pixel advance width of shaped text at the given DPI
    /// without producing a bitmap. Used by B2 (computeExpandedWidth)
    /// to avoid character-count heuristics on kerning-sensitive or
    /// ligature-heavy text. Returns 0 when the font cannot be loaded
    /// or the text is empty. See docs/image-pdf-risks.md (B2).
    int measureTextWidth(const std::u32string& text,
                         const std::string& fontFilePath,
                         float fontSizePt,
                         int dpi = 300);

    /// R2-21: select vertical (top-to-bottom) writing mode for subsequent
    /// render / measure calls. Mirrors MuPDF wmode and
    /// core::TextLine::writingMode (0 = horizontal, 1 = vertical). Default is
    /// false (horizontal); Latin and all existing horizontal callers are
    /// unaffected and byte-identical. When true, shaped glyphs advance along Y
    /// (TTB) via ShapeParams and the caret model, so e.g. CJK stacks
    /// vertically. See docs/audit/round-2/ (R2-20/R2-21).
    void setVerticalWritingMode(bool vertical);

    [[nodiscard]] bool verticalWritingMode() const;

    /// R2-21: logical -> visual reordering (UAX#9-lite bidi) for caret /
    /// selection hit-testing on mixed-direction lines. The text-edit overlay
    /// maps a logical caret index to its on-screen column through this. Exposed
    /// on the compositor so the render path and the hit-test path share one
    /// bidi implementation.
    [[nodiscard]] std::u32string visualOrder(const std::u32string& logical) const;

    /// C5: whether to enable LCD subpixel rendering in subsequent
    /// renderText calls. Default is false (grayscale, matches the
    /// historical behavior and keeps WYSIWYG by default). The
    /// caller sets this before calling renderText based on the
    /// decision logic in shouldUseLcdSubpixel().
    void setLcdSubpixelEnabled(bool enabled);

    [[nodiscard]] bool lcdSubpixelEnabled() const;

    /// C5: decide whether LCD subpixel rendering should be used,
    /// given (a) the caller's DisplayQuality preference and
    /// (b) whether a post-render degradation pass (e.g.
    /// ScanStyleTransfer) would wipe out the subpixel advantage
    /// anyway.
    ///
    /// Truth table:
    ///   quality=WYSIWYG, willDegrade=true  → false (match save)
    ///   quality=WYSIWYG, willDegrade=false → true  (no degradation, free win)
    ///   quality=HighQuality, *             → true  (user override)
    ///
    /// Exposed for testing. See docs/image-pdf-risks.md (C5).
    [[nodiscard]] static bool shouldUseLcdSubpixel(DisplayQuality quality,
                                                   bool willDegrade);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl_;
};

} // namespace pdfe::tre
